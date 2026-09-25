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
#include <string>

namespace joc_decode {

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

struct Settings {
    Output output = Output::kBinaural;
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
    std::uint32_t object_delay_samples = 1473;
    std::uint32_t native_threads = 0;
    std::string ffmpeg_path = "ffmpeg";
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
// does pointer arithmetic over the stream, no decoding, no HRTF work.
FileProbe probe_file(const std::string& path, std::size_t max_scan_bytes = 0);

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

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace joc_decode
