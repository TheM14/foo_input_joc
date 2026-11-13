#include "pcm_source.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "joc_stream.h"
#include "log.h"
#include "win_path.h"

namespace joc_pcm {

namespace {

constexpr std::size_t kFrameSamples = 1536;
constexpr std::uint32_t kRate = 48000;
std::atomic<std::uint64_t> g_temp_serial{0};

std::uint16_t u16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t u32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

bool read_exact(std::FILE* file, void* data, std::size_t bytes) {
    return std::fread(data, 1, bytes, file) == bytes;
}

std::uint64_t file_size(std::FILE* file) {
    const long long current = _ftelli64(file);
    _fseeki64(file, 0, SEEK_END);
    const long long end = _ftelli64(file);
    _fseeki64(file, current, SEEK_SET);
    return end < 0 ? 0 : static_cast<std::uint64_t>(end);
}

}  // namespace

Source::~Source() { close(); }

bool Source::decode_temp(const std::string& input_path, const std::string& ffmpeg,
                         unsigned audio_index, std::string* error) {
    wchar_t directory[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, directory) == 0) {
        if (error != nullptr) *error = "cannot locate the temp directory";
        return false;
    }
    std::wstring temp = directory;
    if (!temp.empty() && temp.back() != L'\\') temp.push_back(L'\\');
    temp += L"foo_input_joc_pcm_";
    temp += std::to_wstring(GetCurrentProcessId());
    temp += L"_";
    temp += std::to_wstring(g_temp_serial.fetch_add(1, std::memory_order_relaxed));
    temp += L"_";
    temp += std::to_wstring(GetTickCount64());
    temp += L".wav";
    temp_path_ = joc_path::to_utf8(temp.c_str());
    DeleteFileW(joc_path::to_wide_extended(temp_path_).c_str());

    std::wstring command = L"\"" + joc_path::to_wide_extended(ffmpeg) + L"\"";
    command += L" -hide_banner -loglevel error -nostdin -y -i \"";
    command += joc_path::to_wide_extended(input_path);
    command += L"\" -map 0:a:";
    command += std::to_wstring(audio_index);
    command += L" -af \"aresample=resampler=soxr:precision=28:osr=48000\" "
               L"-c:a pcm_f32le \"";
    command += joc_path::to_wide_extended(temp_path_);
    command += L"\"";
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &inheritable, OPEN_EXISTING, 0, nullptr);
    const std::wstring log_path = joc_path::to_wide_extended(temp_path_ + ".log");
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = log;
    startup.hStdError = (log != INVALID_HANDLE_VALUE) ? log : null_input;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &process);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (created == FALSE) {
        if (error != nullptr) *error = "cannot start ffmpeg";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool wrote_output =
            GetFileAttributesExW(temp.c_str(), GetFileExInfoStandard, &attributes) != FALSE &&
            (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32 |
             attributes.nFileSizeLow) > 44u;
        if (!wrote_output) {
            std::FILE* log_file = _wfopen(log_path.c_str(), L"rb");
            if (log_file != nullptr) {
                char line[512];
                while (std::fgets(line, sizeof(line), log_file) != nullptr) {
                    std::printf("ffmpeg: %s", line);
                }
                std::fclose(log_file);
            }
            if (error != nullptr) {
                *error = "ffmpeg could not decode the PCM source (exit " +
                         std::to_string(exit_code) + ")";
            }
            return false;
        }
        joc_log::line("pcm: ffmpeg exited with %lu but produced the WAV", exit_code);
    }
    return true;
}

bool Source::parse_wav(const std::string& path, Header* header, std::string* error) {
    std::FILE* file = _wfopen(joc_path::to_wide_extended(path).c_str(), L"rb");
    if (file == nullptr) {
        if (error != nullptr) *error = "cannot open the decoded WAV";
        return false;
    }
    unsigned char riff[12] = {};
    if (!read_exact(file, riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(file);
        if (error != nullptr) *error = "ffmpeg did not produce a RIFF/WAVE file";
        return false;
    }
    Header parsed{};
    bool have_format = false;
    for (;;) {
        unsigned char chunk[8] = {};
        if (!read_exact(file, chunk, sizeof(chunk))) break;
        const std::uint32_t size = u32(chunk + 4);
        const std::uint64_t payload = static_cast<std::uint64_t>(_ftelli64(file));
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16u) {
            unsigned char fmt[40] = {};
            const std::size_t fmt_bytes = std::min<std::size_t>(size, sizeof(fmt));
            if (!read_exact(file, fmt, fmt_bytes)) break;
            std::uint16_t format = u16(fmt);
            parsed.channels = u16(fmt + 2);
            parsed.sample_rate = u32(fmt + 4);
            parsed.bits = u16(fmt + 14);
            if (format == 0xFFFEu && fmt_bytes >= 40u) {
                format = u16(fmt + 24);  // WAVEFORMATEXTENSIBLE sub-format
            }
            if (format != 3u || parsed.bits != 32u) {
                std::fclose(file);
                if (error != nullptr) *error = "decoded WAV is not 32-bit float";
                return false;
            }
            have_format = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            parsed.data_offset = payload;
            parsed.data_bytes = size;
        }
        _fseeki64(file, static_cast<long long>(payload + size + (size & 1u)), SEEK_SET);
    }
    const std::uint64_t file_bytes = file_size(file);
    std::fclose(file);
    if (!have_format || parsed.channels < 2 || parsed.channels > 8 ||
        parsed.data_offset == 0) {
        if (error != nullptr) *error = "decoded WAV has no usable audio data";
        return false;
    }
    if (parsed.data_bytes == 0u || parsed.data_bytes == 0xFFFFFFFFu ||
        parsed.data_offset + parsed.data_bytes > file_bytes) {
        parsed.data_bytes = file_bytes - parsed.data_offset;
    }
    *header = parsed;
    return true;
}

bool Source::open(const std::string& input_path, const joc_decode::Settings& settings,
                  std::string* error) {
    close();
    if (!decode_temp(input_path, settings.ffmpeg_path, settings.audio_index, error)) return false;
    Header header{};
    if (!parse_wav(temp_path_, &header, error)) {
        close();
        return false;
    }
    if (header.sample_rate != kRate) {
        if (error != nullptr) *error = "ffmpeg did not produce 48 kHz audio";
        close();
        return false;
    }
    header_ = header;
    channels_ = header.channels;
    file_ = _wfopen(joc_path::to_wide_extended(temp_path_).c_str(), L"rb");
    if (file_ == nullptr) {
        if (error != nullptr) *error = "cannot reopen the decoded WAV";
        close();
        return false;
    }
    total_frames_ = header.data_bytes / (static_cast<std::uint64_t>(channels_) * 4u);
    length_seconds_ = static_cast<double>(total_frames_) / kRate;
    feed_.assign(kFrameSamples * static_cast<std::size_t>(channels_), 0.0f);

    joc_stream_config config{};
    config.struct_size = sizeof(config);
    config.struct_version = 1;
    config.input = JOC_STREAM_IN_PCM_LAYOUT;
    config.output = JOC_STREAM_OUT_BINAURAL;
    config.pcm_layout = "auto";
    const std::string hrtf = joc_decode::resolve_hrtf_file(settings);
    config.personalized_headphone_path = hrtf.c_str();
    config.binaural_mode = settings.binaural_mode;
    config.binaural_tail_seconds = settings.tail_seconds;
    config.object_delay_samples = settings.object_delay_samples;
    config.gain_db = settings.gain_db;
    config.rosella_filterbank = 1u;
    const joc_error created = joc_stream_create(&config, &stream_);
    if (created != JOC_OK || stream_ == nullptr) {
        if (error != nullptr) *error = "kernel PCM layout stream creation failed";
        close();
        return false;
    }
    _fseeki64(file_, static_cast<long long>(header_.data_offset), SEEK_SET);
    frames_read_ = 0;
    emitted_frames_ = 0;
    eof_ = false;
    flushed_ = false;
    return true;
}

bool Source::push_block(std::string* error) {
    if (file_ == nullptr || stream_ == nullptr || eof_) return false;
    std::size_t frames = kFrameSamples;
    const std::uint64_t remaining = total_frames_ - frames_read_;
    if (remaining < frames) frames = static_cast<std::size_t>(remaining);
    std::fill(feed_.begin(), feed_.end(), 0.0f);
    if (frames != 0u &&
        std::fread(feed_.data(), sizeof(float), frames * static_cast<std::size_t>(channels_),
                   file_) != frames * static_cast<std::size_t>(channels_)) {
        if (error != nullptr) *error = "cannot read the decoded WAV";
        return false;
    }
    if (frames != 0u) {
        joc_stream_buffer input{};
        input.struct_size = sizeof(input);
        input.struct_version = 1;
        input.kind = JOC_STREAM_IN_PCM_LAYOUT;
        input.channels = static_cast<std::uint32_t>(channels_);
        input.sample_rate = kRate;
        input.sample_count = static_cast<std::uint32_t>(frames);
        input.pcm = feed_.data();
        std::uint32_t consumed = 0;
        std::uint32_t consumed_bytes = 0;
        if (joc_stream_push(stream_, &input, &consumed, &consumed_bytes) != JOC_OK) {
            if (error != nullptr) *error = "kernel PCM layout push failed";
            return false;
        }
    }
    frames_read_ += frames;
    if (frames_read_ >= total_frames_ || frames == 0u) eof_ = true;
    return true;
}

bool Source::pull(std::size_t frames, float* destination, std::size_t* produced,
                  std::string* error) {
    *produced = 0;
    if (stream_ == nullptr || frames == 0u) return true;
    joc_stream_buffer output{};
    output.struct_size = sizeof(output);
    output.struct_version = 1;
    output.out_pcm = destination;
    output.sample_count = static_cast<std::uint32_t>(frames);
    std::uint32_t rendered = 0;
    if (joc_stream_pull(stream_, &output, &rendered) != JOC_OK) {
        if (error != nullptr) *error = "kernel binaural pull failed";
        return false;
    }
    *produced = rendered;
    return true;
}

std::size_t Source::read(float* destination, std::size_t frames, std::string* error) {
    if (stream_ == nullptr || destination == nullptr || frames == 0u) return 0;
    if (emitted_frames_ >= total_frames_) return 0;
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, total_frames_ - emitted_frames_));
    std::size_t written = 0;
    while (written < wanted) {
        std::size_t produced = 0;
        if (!pull(wanted - written, destination + written * 2u, &produced, error)) {
            return written;
        }
        written += produced;
        if (written == wanted) break;
        if (!eof_) {
            if (!push_block(error)) return written;
            continue;
        }
        if (!flushed_) {
            if (joc_stream_flush(stream_) != JOC_OK) {
                if (error != nullptr) *error = "kernel flush failed";
                return written;
            }
            flushed_ = true;
            continue;
        }
        break;
    }
    emitted_frames_ += written;
    return written;
}

bool Source::seek(double seconds, std::string* error) {
    if (stream_ == nullptr || file_ == nullptr) return false;
    if (seconds < 0.0) seconds = 0.0;
    std::uint64_t frame = static_cast<std::uint64_t>(seconds * kRate + 0.5);
    frame = std::min(frame, total_frames_);
    if (joc_stream_reset(stream_) != JOC_OK) {
        if (error != nullptr) *error = "kernel reset failed";
        return false;
    }
    _fseeki64(file_, static_cast<long long>(header_.data_offset +
                                            frame * static_cast<std::uint64_t>(channels_) * 4u),
              SEEK_SET);
    frames_read_ = frame;
    emitted_frames_ = frame;
    eof_ = frames_read_ >= total_frames_;
    flushed_ = false;
    return true;
}

void Source::close() {
    if (stream_ != nullptr) {
        joc_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!temp_path_.empty()) {
        DeleteFileW(joc_path::to_wide_extended(temp_path_).c_str());
        DeleteFileW(joc_path::to_wide_extended(temp_path_ + ".log").c_str());
        temp_path_.clear();
    }
    feed_.clear();
    frames_read_ = 0;
    emitted_frames_ = 0;
    total_frames_ = 0;
    length_seconds_ = 0.0;
    eof_ = false;
    flushed_ = false;
    channels_ = 0;
}

}  // namespace joc_pcm
