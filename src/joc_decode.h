// Decode engine: E-AC-3 JOC file -> rendered PCM, without any foobar2000 types.
//
// Kept free of fb2k headers on purpose: the same code is what a headless harness
// drives when the output has to be compared byte for byte against joc_cli.
//
// Two inputs are needed to render, and they come from different places:
//   * the bare E-AC-3 syncframes -- the metadata stream -- read straight from the
//     file, since the plugin only claims bare .eac3/.ec3 today;
//   * the 5.1 core PCM of those same frames -- the rendering core does not decode
//     the core (it has no AC-3 decoder at all), so ffmpeg produces it exactly the
//     way the reference CLI does.
//
// The core is loaded from joc_core.dll through the public C ABI and called via
// function pointers.  Static linking is not an option: the core's static library
// carries the internals but not the API layer in src/api, and the plugin is not
// allowed to compile core sources.  Loading explicitly also lets the component
// ship its own copy of the DLL and check its ABI version at start-up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace joc_decode {

// Delay of the rendering pipeline itself, in output samples: in a run that began
// at source sample 0, output sample k carries the rendering of source sample
// k - this value.  A seek starts the inputs this much earlier and drops the
// samples before the target.  Measured as 0: the renderer's own filter-bank
// latency and its metadata delay are inside the renderer, not delays of the
// delivered stream.  It stays a named, overridable constant so the measurement can
// be repeated.
constexpr std::uint32_t kJocSeekPipelineDelaySamples = 0;

// Syncframes fed before the frame the target sits in.  The renderer's state is
// rebuilt from the frames it is given, so a restart needs a moment to converge and
// the delivered part has to be past that.  Two things converge at different rates:
// the metadata state (object positions, matrix interpolation, gain ramps), which
// the measured 3008-sample window covers, and the binaural room tail, which is
// recursive and can only be approached -- two syncframes are enough for the speaker
// path, and the tail keeps improving with more, which is why this is 64.
constexpr std::uint32_t kJocSeekPrerollFrames = 64;

enum class Output {
    kBinaural = 0,  // 2 channels, HRTF rendering
    kSpeaker = 1,   // N channels, named layout
};

// Which HRTF to render with.  Both inputs are user-facing model/data files; how
// the renderer gets from them to its internal filter field is its own business.
enum class HrtfSource {
    kSofa = 0,
    kRosella = 1,
};

// Where the E-AC-3 syncframes come from.  A bare stream is read straight from the
// file; inside a container the track has to be extracted first (ffmpeg, stream copy).
enum class InputKind {
    kBare = 0,
    kContainer = 1,
};

struct Settings {
    Output output = Output::kBinaural;
    InputKind input_kind = InputKind::kBare;
    // Which of the container's audio tracks holds the E-AC-3 stream, 0-based.
    unsigned audio_index = 0;
    std::string speaker_layout = "7.1";
    HrtfSource hrtf_source = HrtfSource::kSofa;
    // Empty means the default file in the default folder, see resolve_hrtf_file().
    std::string hrtf_file;
    std::string hrtf_cache_dir;       // SOFA compile cache (disk policy)
    unsigned hrtf_cache_policy = 1;   // 0 none, 1 memory, 2 disk
    double hrtf_radius_m = 1.0;       // SOFA measurement-radius shell
    std::uint32_t binaural_mode = 3;  // JOC_BINAURAL_MID
    double gain_db = 0.0;
    double tail_seconds = 5.0;
    // Duration of the file, in seconds.  The renderer ends a binaural stream with a
    // room tail, which would be played as time the file does not have; the delivered
    // stream is cut at exactly this much audio instead.  Zero means "no limit".
    double length_seconds = 0.0;
    std::uint32_t object_delay_samples = 1473;
    std::uint32_t native_threads = 0;
    std::string ffmpeg_path = "ffmpeg";
    // Bytes in front of the first syncframe -- a leading ID3v2 tag.  The renderer
    // rejects a stream that does not begin on a syncword, so both the walk and the
    // feed start here.
    std::uint64_t stream_start_bytes = 0;
    // Samples of pipeline delay a seek compensates for, and syncframes it pre-rolls
    // before the target: the calibrated constants above, unless the comparison
    // harness overrides them to measure those constants.
    std::uint32_t pipeline_delay_samples = kJocSeekPipelineDelaySamples;
    std::uint32_t seek_preroll_frames = kJocSeekPrerollFrames;
    // Stop feeding the renderer after this many input syncframes and flush, which
    // is what the reference CLI's --duration does.  Zero means "the whole file".
    // Only the comparison harness sets it; playback leaves it at zero.
    std::uint64_t input_frame_limit = 0;
};

// Channel count of a named layout, or 0 when the layout is not one of the
// layouts the core accepts.
unsigned speaker_channels(const std::string& layout);

// Directory this component's DLL was loaded from.  The default HRTF folder is
// <that directory>\HRTF, the same place the reference CLI looks next to its exe.
std::string component_directory();

// The HRTF file a configuration will actually use: the configured path, or the
// default file in the default folder when none is configured.  Empty when the
// component directory cannot be determined.
std::string resolve_hrtf_file(const Settings& settings);

// Every layout the core accepts, in the order a settings page should list them.
const char* const* speaker_layouts(std::size_t* count);

// Result of examining a file without rendering anything.
struct FileProbe {
    bool readable = false;
    bool joc = false;              // every examined syncframe carries JOC
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;    // core 5.1 layout, i.e. 6 (including LFE)
    std::uint64_t frames = 0;      // E-AC-3 syncframes in the file
    double duration_seconds = 0.0;
    std::string detail;
};

// Walks the file's syncframes.  Cheap enough for a Media Library scan: it only
// does pointer arithmetic over the stream, no decoding, no HRTF work.  The walk
// starts at `start_offset`, which is where the stream begins behind a tag area.
FileProbe probe_file(const std::string& path, std::size_t max_scan_bytes = 0,
                     std::uint64_t start_offset = 0);

// Decides whether the E-AC-3 track of a container file carries JOC, without
// rendering anything: ffmpeg copies a short prefix of that track out (stream copy,
// so the syncframes are the stored ones) and the same bitstream test is applied.
// The verdict is cached per file, because the information and the decode pass would
// otherwise each launch ffmpeg for it.
bool probe_container_joc(const std::string& ffmpeg_path, const std::string& path,
                         unsigned audio_index, bool* joc, std::string* detail);

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(const std::string& input_path, const Settings& settings, std::string* error);
    // Interleaved float32; returns frames produced per channel, 0 means end of stream.
    std::size_t read(float* destination, std::size_t frames, std::string* error);
    unsigned channels() const;
    void stop();

    // Duration of the file, in seconds, for a caller that learns it after start()
    // (reported duration of the track); zero means "no limit".
    void set_length(double seconds);

    // Repositions so that the next sample read() returns is the rendering of source
    // sample round(seconds * 48000): the inputs restart at the syncframe holding
    // that sample and the output before it is dropped.  Seeking at or past
    // `total_seconds` (0 when the duration is unknown) succeeds and makes the next
    // read() return 0, as the decoder contract requires.  start() must have run.
    bool seek(double seconds, double total_seconds, std::string* error);

    // Polled while the syncframe index is being walked, so a long seek stays
    // interruptible; returning false aborts the seek.
    void set_abort_check(std::function<bool()> check);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace joc_decode
