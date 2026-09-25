// Headless render harness: drives the plugin's own decode engine and writes a WAV
// in the exact header format the reference CLI writes, so the two outputs can be
// compared byte for byte.
//
//   render_harness --input <file.eac3> --output <out.wav> [options]
//     --mode binaural|speaker     default binaural
//     --layout NAME               speaker layout (default 7.1)
//     --hrtf-source sofa|rosella  kind of model --hrtf points at (default sofa)
//     --hrtf PATH                 HRTF to render with; supplied by the caller and
//                                 never part of this repository.  Without it the
//                                 default location beside the executable is used.
//                                 Speaker layouts need no HRTF.
//     --gain-db X                 default 0
//     --tail S                    binaural tail seconds (default 5)
//     --ffmpeg PATH               default ffmpeg
//     --max-frames N              stop after N output frames (0 = all)
//     --max-input-frames N        stop feeding the renderer after N syncframes
//     --seek-to S                 seek to S seconds before reading (repeatable;
//                                 only the last one before reading takes effect)
//     --seek-at N:S               seek to S seconds once N output frames have been
//                                 written (repeatable; N = 0 seeks before reading)
//     --total-seconds S           the duration seek() is told about (0 = unknown)
//     --length S                  cut the delivered stream at S seconds (the file's
//                                 own duration; the renderer still renders its tail)
//     --pipeline-delay N          delay a seek compensates for, in samples
//     --preroll-frames N          syncframes fed before the target frame
//     --cycles N                  render the file N times in this process, in sequence
//     --parallel N                render it N times at once, in separate threads
//
// The two constants a seek uses are overridable so they can be checked by measurement.
//
// --cycles and --parallel cover what one render per process cannot: the renderer and the
// compiled-HRTF cache live as long as the process does, and foobar2000 starts the next
// file while the current one is still being read.
//
// It exists because the plugin's engine (src/joc_decode.*) has no foobar2000
// dependency: the very code that plays in foobar2000 can be run here, against the
// reference renderer or against the same engine's continuous render.  The WAV header
// mirrors the renderer's own writer: a simple IEEE-float header up to two channels,
// WAVE_FORMAT_EXTENSIBLE above that with a channel mask of zero.

#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/joc_decode.h"

namespace {

struct Options {
    std::string input;
    std::string output;
    std::string mode = "binaural";
    std::string layout = "7.1";
    std::string hrtf;                  // SOFA file, or Rosella model file
    std::string hrtf_source = "sofa";  // sofa | rosella
    std::string ffmpeg = "ffmpeg";
    double gain_db = 0.0;
    double tail_seconds = 5.0;
    std::uint64_t max_frames = 0;
    std::uint64_t max_input_frames = 0;
    std::vector<double> seeks_before;                       // --seek-to
    std::vector<std::pair<std::uint64_t, double>> seeks_at;  // --seek-at
    double total_seconds = 0.0;
    double length_seconds = 0.0;   // --length: cut the delivered stream here
    std::uint32_t pipeline_delay = joc_decode::kJocSeekPipelineDelaySamples;
    std::uint32_t preroll_frames = joc_decode::kJocSeekPrerollFrames;
    bool container = false;
    unsigned audio_index = 0;
    unsigned cycles = 0;      // --cycles N: N renders in one process, in sequence
    unsigned parallel = 0;    // --parallel N: N renders at once, in one process
};

void put_u16(std::string* out, unsigned value) {
    out->push_back(static_cast<char>(value & 0xFFu));
    out->push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32(std::string* out, unsigned long long value) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

// Same bytes the core's wav_writer produces.
std::string wav_header(unsigned channels, unsigned rate, std::uint64_t frames) {
    const unsigned bytes_per_sample = 4;
    const unsigned block_align = channels * bytes_per_sample;
    const std::uint64_t data_bytes = frames * block_align;

    std::string fmt;
    if (channels <= 2) {
        put_u16(&fmt, 3);  // WAVE_FORMAT_IEEE_FLOAT
        put_u16(&fmt, channels);
        put_u32(&fmt, rate);
        put_u32(&fmt, rate * block_align);
        put_u16(&fmt, block_align);
        put_u16(&fmt, 8 * bytes_per_sample);
    } else {
        put_u16(&fmt, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
        put_u16(&fmt, channels);
        put_u32(&fmt, rate);
        put_u32(&fmt, rate * block_align);
        put_u16(&fmt, block_align);
        put_u16(&fmt, 8 * bytes_per_sample);
        put_u16(&fmt, 22);      // cbSize
        put_u16(&fmt, 8 * bytes_per_sample);
        put_u32(&fmt, 0);       // channel mask, as the core writes it
        static const unsigned char kFloatGuid[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                     0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.append(reinterpret_cast<const char*>(kFloatGuid), 16);
    }

    std::string header;
    header.append("RIFF", 4);
    put_u32(&header, 4u + 8u + fmt.size() + 8u + data_bytes);
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    put_u32(&header, fmt.size());
    header.append(fmt);
    header.append("data", 4);
    put_u32(&header, data_bytes);
    return header;
}

bool parse(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--input") options->input = next();
        else if (arg == "--output") options->output = next();
        else if (arg == "--mode") options->mode = next();
        else if (arg == "--layout") options->layout = next();
        else if (arg == "--hrtf") options->hrtf = next();
        else if (arg == "--hrtf-source") options->hrtf_source = next();
        else if (arg == "--ffmpeg") options->ffmpeg = next();
        else if (arg == "--gain-db") options->gain_db = std::atof(next().c_str());
        else if (arg == "--tail") options->tail_seconds = std::atof(next().c_str());
        else if (arg == "--max-frames") options->max_frames = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--max-input-frames") options->max_input_frames = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--seek-to") options->seeks_before.push_back(std::atof(next().c_str()));
        else if (arg == "--total-seconds") options->total_seconds = std::atof(next().c_str());
        else if (arg == "--length") options->length_seconds = std::atof(next().c_str());
        else if (arg == "--pipeline-delay") options->pipeline_delay = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (arg == "--preroll-frames") options->preroll_frames = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (arg == "--container") options->container = true;
        else if (arg == "--cycles") options->cycles = static_cast<unsigned>(std::strtoul(next().c_str(), nullptr, 10));
        else if (arg == "--parallel") options->parallel = static_cast<unsigned>(std::strtoul(next().c_str(), nullptr, 10));
        else if (arg == "--audio-index") options->audio_index = static_cast<unsigned>(std::strtoul(next().c_str(), nullptr, 10));
        else if (arg == "--seek-at") {
            const std::string value = next();
            const std::string::size_type colon = value.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "render_harness: --seek-at wants <frames>:<seconds>\n");
                return false;
            }
            options->seeks_at.emplace_back(
                std::strtoull(value.substr(0, colon).c_str(), nullptr, 10),
                std::atof(value.substr(colon + 1).c_str()));
        }
        else if (arg == "--help" || arg == "-h") return false;
        else {
            std::fprintf(stderr, "render_harness: unknown argument %s\n", arg.c_str());
            return false;
        }
    }
    return !options->input.empty() && !options->output.empty();
}


// One render: everything the component does for one file from start to stop.
// Returns false with *error set when the engine reports a failure.
bool render_once(const joc_decode::Settings& settings, const Options& options,
                 const std::string& output, std::string* error) {
    joc_decode::Engine engine;
    if (!engine.start(options.input, settings, error)) return false;
    const unsigned channels = engine.channels();
    if (channels == 0) {
        *error = "engine reported zero channels";
        return false;
    }
    for (const double seconds : options.seeks_before) {
        if (!engine.seek(seconds, options.total_seconds, error)) return false;
    }

    std::FILE* file = std::fopen(output.c_str(), "wb");
    if (file == nullptr) {
        *error = "cannot write " + output;
        return false;
    }
    // The header carries the length, so write a placeholder and come back to it.
    const std::string header = wav_header(channels, 48000, 0);
    std::fwrite(header.data(), 1, header.size(), file);

    constexpr std::size_t kChunk = 4096;
    std::vector<float> buffer(kChunk * channels);
    std::uint64_t frames_written = 0;
    double peak = 0.0;
    std::vector<char> applied(options.seeks_at.size(), 0);
    for (;;) {
        for (std::size_t i = 0; i < options.seeks_at.size(); ++i) {
            if (applied[i] != 0 || options.seeks_at[i].first > frames_written) continue;
            applied[i] = 1;
            if (!engine.seek(options.seeks_at[i].second, options.total_seconds, error)) {
                std::fclose(file);
                return false;
            }
        }
        std::size_t want = kChunk;
        if (options.max_frames != 0) {
            if (frames_written >= options.max_frames) break;
            const std::uint64_t left = options.max_frames - frames_written;
            if (left < want) want = static_cast<std::size_t>(left);
        }
        const std::size_t frames = engine.read(buffer.data(), want, error);
        if (frames == 0) {
            if (!error->empty()) {
                std::fclose(file);
                return false;
            }
            break;
        }
        for (std::size_t i = 0; i < frames * channels; ++i) {
            const double value = buffer[i] < 0.0f ? -static_cast<double>(buffer[i])
                                                  : static_cast<double>(buffer[i]);
            if (value > peak) peak = value;
        }
        std::fwrite(buffer.data(), sizeof(float), frames * channels, file);
        frames_written += frames;
    }
    engine.stop();

    const std::string final_header = wav_header(channels, 48000, frames_written);
    std::fseek(file, 0, SEEK_SET);
    std::fwrite(final_header.data(), 1, final_header.size(), file);
    std::fclose(file);

    std::printf("render_harness: %s -> %s\n", options.input.c_str(), output.c_str());
    std::printf("  channels=%u frames=%llu peak=%.9f\n", channels,
                static_cast<unsigned long long>(frames_written), peak);
    return true;
}

std::string numbered(const std::string& output, unsigned index) {
    return output + "." + std::to_string(index) + ".wav";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, &options)) {
        std::fprintf(stderr,
                     "usage: render_harness --input <eac3> --output <wav> [--mode binaural|speaker]\n"
                     "       [--layout NAME] [--hrtf-source sofa|rosella] [--hrtf PATH]\n"
                     "       [--gain-db X] [--tail S] [--ffmpeg path] [--max-frames N]\n"
                     "       [--max-input-frames N] [--seek-to S] [--seek-at N:S]\n"
                     "       [--total-seconds S] [--pipeline-delay N] [--preroll-frames N]\n"
                     "       [--container] [--audio-index N] [--length S]\n"
                     "       [--cycles N] [--parallel N]\n");
        return 2;
    }

    joc_decode::Settings settings;
    settings.output = (options.mode == "speaker") ? joc_decode::Output::kSpeaker
                                                  : joc_decode::Output::kBinaural;
    settings.speaker_layout = options.layout;
    settings.hrtf_source = (options.hrtf_source == "rosella") ? joc_decode::HrtfSource::kRosella
                                                             : joc_decode::HrtfSource::kSofa;
    settings.hrtf_file = options.hrtf;
    settings.gain_db = options.gain_db;
    settings.tail_seconds = options.tail_seconds;
    settings.ffmpeg_path = options.ffmpeg;
    settings.input_frame_limit = options.max_input_frames;
    settings.pipeline_delay_samples = options.pipeline_delay;
    settings.seek_preroll_frames = options.preroll_frames;
    settings.length_seconds = options.length_seconds;
    settings.input_kind = options.container ? joc_decode::InputKind::kContainer
                                           : joc_decode::InputKind::kBare;
    settings.audio_index = options.audio_index;

    std::string error;
    if (options.parallel != 0) {
        // The same file rendered by several engines at once, which is what a track
        // change looks like from the engine's side: foobar2000 starts the next file
        // while the current one is still being read.
        std::vector<std::thread> threads;
        std::vector<std::string> errors(options.parallel);
        for (unsigned i = 0; i < options.parallel; ++i) {
            threads.emplace_back([&, i] {
                if (!render_once(settings, options, numbered(options.output, i), &errors[i])) {
                    std::fprintf(stderr, "render_harness: parallel %u: %s\n", i,
                                 errors[i].c_str());
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        for (const std::string& text : errors) {
            if (!text.empty()) return 1;
        }
        return 0;
    }

    const unsigned cycles = (options.cycles == 0) ? 1 : options.cycles;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        // Every cycle is a fresh engine in the same process: the compiled-HRTF cache
        // and everything else the renderer keeps per process is reused, exactly as it
        // is when a second file is played in the same foobar2000 session.
        error.clear();
        const std::string output = (cycles == 1) ? options.output : numbered(options.output, cycle);
        if (!render_once(settings, options, output, &error)) {
            std::fprintf(stderr, "render_harness: cycle %u: %s\n", cycle, error.c_str());
            return 1;
        }
    }
    return 0;
}