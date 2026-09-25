#include "joc_decode.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

// The kernel copy that is compiled into this component; see kernel/.
#include "../kernel/include/joc_core.h"
#include "../kernel/include/joc_stream.h"

#include "eac3_scan.h"
#include "log.h"

namespace joc_decode {
namespace {

constexpr std::size_t kEac3Chunk = 96u * 1024u;      // bytes read per push
constexpr std::size_t kBedFramesChunk = 8192u;       // staging capacity, in frames
constexpr std::size_t kBedChannels = 6;              // ffmpeg -ac 6
// A read on an anonymous pipe only completes once the whole request is available,
// so each read asks for a slice ffmpeg can always fill, and the pipe buffer is
// sized well above that slice.  Requesting the whole staging buffer deadlocks:
// ffmpeg fills the pipe and blocks, while the reader waits for more than the pipe
// can ever hold.
constexpr std::size_t kBedReadBytes = 48u * 1024u;
constexpr std::size_t kBedPipeBytes = 1u << 20;
constexpr std::size_t kFrameSamples = JOC_FRAME_SAMPLES;

// ---------------------------------------------------------------------------
// Kernel entry points.
//
// The kernel's own C++ sources are part of this component (see kernel/, a copy of
// the project's source tree), so the public C ABI is linked in directly.  There is
// nothing to load at run time, and no way for the component and the renderer to
// disagree about which ABI they were built against.
// ---------------------------------------------------------------------------
struct CoreApi {
    joc_error(JOC_CALL* create)(const joc_stream_config*, joc_stream**) = joc_stream_create;
    joc_error(JOC_CALL* push)(joc_stream*, const joc_stream_buffer*, std::uint32_t*,
                              std::uint32_t*) = joc_stream_push;
    joc_error(JOC_CALL* pull)(joc_stream*, joc_stream_buffer*, std::uint32_t*) = joc_stream_pull;
    joc_error(JOC_CALL* flush)(joc_stream*) = joc_stream_flush;
    joc_error(JOC_CALL* status)(const joc_stream*, joc_stream_status_info*) = joc_stream_status;
    joc_error(JOC_CALL* destroy)(joc_stream*) = joc_stream_destroy;
    std::uint32_t(JOC_CALL* abi_version)() = joc_abi_version;
    const char*(JOC_CALL* version_string)() = joc_version_string;
    const char*(JOC_CALL* error_name)(joc_error) = joc_error_name;
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

bool file_exists(const std::string& path) {
    const std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// The renderer is inside this binary, so "loading" it is just an ABI check.
bool load_api(const std::string& explicit_path, CoreApi* api, std::string* error) {
    (void)explicit_path;
    if (api->abi_version() != JOC_ABI_VERSION) {
        if (error != nullptr) *error = "the bundled renderer has an unexpected ABI";
        return false;
    }
    return true;
}

const char* error_text(const CoreApi& api, joc_error code) {
    if (api.error_name != nullptr) {
        const char* name = api.error_name(code);
        if (name != nullptr) return name;
    }
    return "unknown core error";
}

// ---------------------------------------------------------------------------
// ffmpeg child process producing the 5.1 core PCM on a pipe.
// ---------------------------------------------------------------------------
class BedProcess {
public:
    ~BedProcess() { stop(); }

    bool start(const std::string& ffmpeg_path, const std::string& input_path,
               const std::wstring& stderr_path, std::string* error) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (CreatePipe(&read_end, &write_end, &attributes, static_cast<DWORD>(kBedPipeBytes)) == FALSE) {
            if (error != nullptr) *error = "cannot create the core PCM pipe";
            return false;
        }
        // Only the child's end is inheritable.
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

        HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &attributes, OPEN_EXISTING, 0, nullptr);
        HANDLE error_file = CreateFileW(stderr_path.c_str(), GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                        CREATE_ALWAYS, 0, nullptr);

        std::wstring command = L"\"" + utf8_to_wide(ffmpeg_path) + L"\"";
        command += L" -hide_banner -loglevel error -nostdin -y -i \"";
        command += utf8_to_wide(input_path);
        // Identical to the reference CLI's core decode: 5.1 interleaved float32
        // at 48 kHz, which is the layout the rendering core expects
        // (L R C LFE Ls Rs).
        command += L"\" -map 0:a:0 -vn -ac 6 -ar 48000 -c:a pcm_f32le -f f32le -";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = null_input;
        startup.hStdOutput = write_end;
        startup.hStdError = (error_file != INVALID_HANDLE_VALUE) ? error_file : null_input;

        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                            &process);
        CloseHandle(write_end);
        if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
        if (error_file != INVALID_HANDLE_VALUE) CloseHandle(error_file);
        if (created == FALSE) {
            CloseHandle(read_end);
            if (error != nullptr) *error = "cannot start ffmpeg for the 5.1 core PCM";
            return false;
        }
        CloseHandle(process.hThread);
        pipe_ = read_end;
        process_ = process.hProcess;
        joc_log::line("core bed: ffmpeg started (pid %lu)", process.dwProcessId);
        return true;
    }

    // Returns bytes read; 0 means end of stream.
    std::size_t read(void* destination, std::size_t bytes) {
        if (pipe_ == nullptr) return 0;
        DWORD got = 0;
        if (ReadFile(pipe_, destination, static_cast<DWORD>(bytes), &got, nullptr) == FALSE) {
            return 0;
        }
        return got;
    }

    void stop() {
        if (pipe_ != nullptr) {
            CloseHandle(pipe_);
            pipe_ = nullptr;
        }
        if (process_ != nullptr) {
            // ffmpeg is normally gone by now (its stdout was drained); ask it to
            // finish rather than killing it, and only then let go.
            if (WaitForSingleObject(process_, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    bool finished() const { return pipe_ == nullptr; }

private:
    HANDLE pipe_ = nullptr;
    HANDLE process_ = nullptr;
};

// ---------------------------------------------------------------------------
// Small file reader (wide paths, no CRT locale involved).
// ---------------------------------------------------------------------------
class InputFile {
public:
    ~InputFile() { close(); }
    bool open(const std::string& path) {
        handle_ = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    std::size_t read(void* destination, std::size_t bytes) {
        if (handle_ == INVALID_HANDLE_VALUE) return 0;
        DWORD got = 0;
        if (ReadFile(handle_, destination, static_cast<DWORD>(bytes), &got, nullptr) == FALSE) {
            return 0;
        }
        return got;
    }
    std::uint64_t size() const {
        LARGE_INTEGER value{};
        if (handle_ == INVALID_HANDLE_VALUE || GetFileSizeEx(handle_, &value) == FALSE) return 0;
        return static_cast<std::uint64_t>(value.QuadPart);
    }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

const char* const kLayouts[] = {"2.0",   "3.0",   "3.1",   "4.0",   "5.0",   "5.1",  "5.1.2",
                                "5.1.4", "6.1",   "7.0",   "7.1",   "7.1.2", "7.1.4", "9.1.4",
                                "9.1.6", "22.2"};
const unsigned kLayoutChannels[] = {2, 3, 4, 4, 5, 6, 8, 10, 7, 7, 8, 10, 12, 14, 16, 24};

}  // namespace

const char* const* speaker_layouts(std::size_t* count) {
    if (count != nullptr) *count = sizeof(kLayouts) / sizeof(kLayouts[0]);
    return kLayouts;
}

unsigned speaker_channels(const std::string& layout) {
    for (std::size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); ++i) {
        if (layout == kLayouts[i]) return kLayoutChannels[i];
    }
    return 0;
}

std::string component_directory() {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&component_directory), &self) == FALSE) {
        return {};
    }
    wchar_t path[4096] = {};
    const DWORD length = GetModuleFileNameW(self, path, 4096);
    if (length == 0) return {};
    const std::wstring text(path, length);
    const std::wstring::size_type slash = text.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    const std::wstring directory = text.substr(0, slash);
    const int needed = WideCharToMultiByte(CP_UTF8, 0, directory.c_str(),
                                           static_cast<int>(directory.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::string resolve_hrtf_file(const Settings& settings) {
    if (!settings.hrtf_file.empty()) return settings.hrtf_file;
    const std::string directory = component_directory();
    if (directory.empty()) return {};
    // Same convention as the reference CLI next to its executable:
    // <base>\HRTF\binaural.sofa or <base>\HRTF\binaural.personalized_headphone.
    const char* name = (settings.hrtf_source == HrtfSource::kRosella)
                           ? "binaural.personalized_headphone"
                           : "binaural.sofa";
    return directory + "\\HRTF\\" + name;
}

FileProbe probe_file(const std::string& path, std::size_t max_scan_bytes) {
    FileProbe probe;
    InputFile file;
    if (!file.open(path)) {
        probe.detail = "cannot open";
        return probe;
    }
    const std::uint64_t size = file.size();

    // A Media Library scan calls this for every file, so the whole stream is
    // walked only when that is cheap; otherwise the first window is enough,
    // because E-AC-3 syncframes in a stream like this are all the same size.
    const std::uint64_t kFullWalkLimit = 16u * 1024u * 1024u;
    const std::size_t window =
        (max_scan_bytes != 0) ? max_scan_bytes : 256u * 1024u;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(
        (size < kFullWalkLimit && size > 0) ? size : window));
    const std::size_t got = file.read(buffer.data(), buffer.size());
    if (got < 8) {
        probe.detail = "file too small to be E-AC-3";
        return probe;
    }

    const joc_eac3::ScanResult scan = joc_eac3::scan(buffer.data(), got, 8);
    probe.readable = true;
    probe.joc = (scan.joc == joc_eac3::JocState::kYes);
    probe.detail = scan.detail;
    if (scan.frames_examined == 0 || scan.first_frame_bytes == 0) {
        probe.detail = "not a bare E-AC-3 stream";
        return probe;
    }

    std::uint64_t frames = 0;
    if (buffer.size() == size) {
        std::size_t offset = 0;
        while (true) {
            const std::size_t bytes = joc_eac3::frame_bytes_at(buffer.data(), got, offset);
            if (bytes == 0 || offset + bytes > got) break;
            offset += bytes;
            ++frames;
        }
        probe.detail = "frame count walked over the whole file";
    } else if (scan.all_frames_same_size) {
        frames = size / scan.first_frame_bytes;
        probe.detail = "frame count extrapolated from a constant frame size";
    } else {
        // Variable frame size: count in the window and scale by the byte ratio.
        std::size_t offset = 0;
        std::uint64_t seen = 0;
        while (true) {
            const std::size_t bytes = joc_eac3::frame_bytes_at(buffer.data(), got, offset);
            if (bytes == 0 || offset + bytes > got) break;
            offset += bytes;
            ++seen;
        }
        frames = (offset != 0) ? static_cast<std::uint64_t>(
                                     (static_cast<double>(size) / static_cast<double>(offset)) *
                                     static_cast<double>(seen))
                               : 0;
        probe.detail = "frame count estimated from a variable frame size";
    }

    probe.frames = frames;
    probe.sample_rate = 48000;
    probe.channels = 6;  // the core audio of an E-AC-3 JOC stream
    probe.duration_seconds =
        static_cast<double>(frames) * static_cast<double>(kFrameSamples) / 48000.0;
    return probe;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
struct Engine::Impl {
    CoreApi api;
    joc_stream* stream = nullptr;
    InputFile eac3;
    BedProcess bed;
    Settings settings;
    unsigned channels = 0;
    std::uint64_t frames_queued = 0;    // E-AC-3 frames handed to the core
    std::uint64_t bed_frames_pushed = 0;
    bool eac3_eof = false;
    bool bed_eof = false;
    bool flushed = false;
    unsigned trace_count = 0;
    unsigned read_calls = 0;
    std::size_t eac3_carry = 0;   // trailing partial syncframe, for exact counting
    std::uint64_t bed_bytes_read = 0;
    std::vector<std::uint8_t> eac3_buffer;
    std::vector<float> bed_buffer;
    std::size_t bed_staged_bytes = 0;   // bytes staged at the front of bed_buffer
    std::vector<float> pull_buffer;
};

Engine::Engine() : impl_(new Impl()) {}

Engine::~Engine() {
    stop();
    delete impl_;
}

unsigned Engine::channels() const { return impl_->channels; }

void Engine::stop() {
    Impl& impl = *impl_;
    if (impl.stream != nullptr && impl.api.destroy != nullptr) {
        impl.api.destroy(impl.stream);
        impl.stream = nullptr;
    }
    impl.bed.stop();
    impl.eac3.close();
}

bool Engine::start(const std::string& input_path, const Settings& settings, std::string* error) {
    Impl& impl = *impl_;
    impl.settings = settings;
    impl.eac3_buffer.resize(kEac3Chunk);
    impl.bed_buffer.resize(kBedFramesChunk * kBedChannels);

    if (!load_api(std::string(), &impl.api, error)) return false;
    const std::uint32_t abi = impl.api.abi_version();
    const char* version = impl.api.version_string != nullptr ? impl.api.version_string() : "?";
    joc_log::line("core: in-process renderer %s (abi %u), component built against abi %u",
                    version, abi, static_cast<unsigned>(JOC_ABI_VERSION));
    if (abi != JOC_ABI_VERSION) {
        joc_log::line("core: ABI mismatch; refusing to continue");
        if (error != nullptr) *error = "the bundled renderer ABI does not match the component";
        return false;
    }

    if (!impl.eac3.open(input_path)) {
        if (error != nullptr) *error = "cannot open the input file";
        return false;
    }

    joc_stream_config config{};
    config.struct_size = sizeof(config);
    config.struct_version = 1;
    config.input = JOC_STREAM_IN_EAC3;
    config.output = (settings.output == Output::kBinaural) ? JOC_STREAM_OUT_BINAURAL
                                                           : JOC_STREAM_OUT_SPEAKER;
    config.speaker_layout_name = settings.speaker_layout.c_str();
    config.speaker_metadata_offset = 0;
    config.binaural_mode = settings.binaural_mode;
    // The filter bank tables live in the library (hrtf::builtin_kernels in
    // kernel_tables.cpp), so there is no table path to configure and none is
    // passed: the field stays null.  The same goes for the compiled-HRTF cache:
    // it is the renderer's internal business, not a user choice.
    config.kernels_path = nullptr;
    config.hrtf_path = nullptr;
    // Only binaural output reads an HRTF at all: a speaker layout must not be
    // blocked by a missing HRTF file, and must not have to name one.
    std::string hrtf_in_use;
    if (settings.output == Output::kBinaural) {
        hrtf_in_use = resolve_hrtf_file(settings);
        const std::string& hrtf_file = hrtf_in_use;
        if (hrtf_file.empty()) {
            if (error != nullptr) {
                *error = "双耳渲染需要 HRTF 文件，但组件目录无法确定；请在设置页里显式指定路径";
            }
            return false;
        }
        if (!file_exists(hrtf_file)) {
            if (error != nullptr) {
                *error = std::string("找不到 HRTF 文件：") + hrtf_file +
                         (settings.hrtf_file.empty() ? "（默认位置，可在设置页里指定其它路径）"
                                                     : "");
            }
            return false;
        }
        if (settings.hrtf_source == HrtfSource::kRosella) {
            config.personalized_headphone_path = hrtf_file.c_str();
        } else {
            config.hrtf_sofa_path = hrtf_file.c_str();
        }
    }
    config.hrtf_cache_policy = settings.hrtf_cache_policy;
    config.hrtf_cache_dir =
        settings.hrtf_cache_dir.empty() ? nullptr : settings.hrtf_cache_dir.c_str();
    config.hrtf_radius_m = settings.hrtf_radius_m;
    config.binaural_tail_seconds = settings.tail_seconds;
    config.object_delay_samples = settings.object_delay_samples;
    config.gain_db = settings.gain_db;
    config.native_threads = settings.native_threads;

    const joc_error created = impl.api.create(&config, &impl.stream);
    if (created != JOC_OK) {
        if (error != nullptr) {
            *error = std::string("cannot create the render stream: ") +
                     error_text(impl.api, created);
        }
        return false;
    }
    joc_stream_status_info status{};
    status.struct_size = sizeof(status);
    status.struct_version = 1;
    if (impl.api.status(impl.stream, &status) == JOC_OK && status.output_channels != 0u) {
        impl.channels = status.output_channels;
    } else {
        impl.channels = (settings.output == Output::kBinaural)
                            ? 2u
                            : speaker_channels(settings.speaker_layout);
    }
    joc_log::line("core: stream created, %u output channel(s), layout=%s, hrtf=%s", impl.channels,
                    settings.speaker_layout.c_str(),
                    hrtf_in_use.empty() ? "(none)" : hrtf_in_use.c_str());

    const std::wstring stderr_path = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&speaker_channels), &self);
        wchar_t path[4096] = {};
        const DWORD length = GetModuleFileNameW(self, path, 4096);
        if (length == 0) return std::wstring(L"joc_ffmpeg.log");
        const std::wstring text(path, length);
        const std::wstring::size_type slash = text.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L"joc_ffmpeg.log")
                                          : text.substr(0, slash) + L"\\joc_ffmpeg.log";
    }();
    if (!impl.bed.start(settings.ffmpeg_path, input_path, stderr_path, error)) return false;
    return true;
}

std::size_t Engine::read(float* destination, std::size_t frames, std::string* error) {
    Impl& impl = *impl_;
    if (impl.stream == nullptr || frames == 0) return 0;
    const bool trace = impl.trace_count < 6;
    ++impl.read_calls;
    if ((impl.read_calls % 50u) == 0u) {
        joc_log::line("engine: call %u queued=%llu bed=%llu staged_bytes=%llu eac3_eof=%d bed_eof=%d flushed=%d",
                        impl.read_calls, static_cast<unsigned long long>(impl.frames_queued),
                        static_cast<unsigned long long>(impl.bed_frames_pushed),
                        static_cast<unsigned long long>(impl.bed_staged_bytes), impl.eac3_eof ? 1 : 0,
                        impl.bed_eof ? 1 : 0, impl.flushed ? 1 : 0);
    }

    for (;;) {
        if (trace) {
            joc_log::line("engine: loop eac3_eof=%d bed_eof=%d queued=%llu bed=%llu staged_bytes=%llu",
                            impl.eac3_eof ? 1 : 0, impl.bed_eof ? 1 : 0,
                            static_cast<unsigned long long>(impl.frames_queued),
                            static_cast<unsigned long long>(impl.bed_frames_pushed),
                            static_cast<unsigned long long>(impl.bed_staged_bytes));
        }
        // Feed the metadata stream, keeping it only a little ahead of the bed so
        // the core's pairing queue stays small.
        // A test-side input limit has to look like end of file, or the loop below
        // never reaches its terminal state.
        if (impl.settings.input_frame_limit != 0 &&
            impl.frames_queued >= impl.settings.input_frame_limit) {
            impl.eac3_eof = true;
        }

        if (!impl.eac3_eof && impl.frames_queued <= impl.bed_frames_pushed + 2u &&
            (impl.settings.input_frame_limit == 0 ||
             impl.frames_queued < impl.settings.input_frame_limit)) {
            // The tail of a chunk is usually the head of the next syncframe.  It
            // stays at the front of the buffer so the frame count stays exact
            // across chunk boundaries: counting each chunk on its own loses one
            // frame at the end, which leaves the bed a frame short and the last
            // frame of the file unrendered.
            const std::size_t got = impl.eac3.read(impl.eac3_buffer.data() + impl.eac3_carry,
                                                   impl.eac3_buffer.size() - impl.eac3_carry);
            const std::size_t total = impl.eac3_carry + got;
            if (total == 0) {
                impl.eac3_eof = true;
            } else {
                std::size_t offset = 0;
                std::uint64_t complete = 0;
                while (true) {
                    const std::size_t bytes =
                        joc_eac3::frame_bytes_at(impl.eac3_buffer.data(), total, offset);
                    if (bytes == 0 || offset + bytes > total) break;
                    offset += bytes;
                    ++complete;
                }
                // With an input limit the chunk is cut at a frame boundary: the
                // renderer's output depends on how many frames it was given, so a
                // limit that overshoots to the end of the read buffer would not
                // reproduce a run that stopped earlier.
                std::size_t push_bytes = offset;
                std::uint64_t pushed_frames = complete;
                if (impl.settings.input_frame_limit != 0) {
                    const std::uint64_t room =
                        impl.settings.input_frame_limit - impl.frames_queued;
                    if (complete > room) {
                        pushed_frames = room;
                        std::size_t walk = 0;
                        for (std::uint64_t index = 0; index < pushed_frames; ++index) {
                            const std::size_t bytes = joc_eac3::frame_bytes_at(
                                impl.eac3_buffer.data(), total, walk);
                            if (bytes == 0 || walk + bytes > total) break;
                            walk += bytes;
                        }
                        push_bytes = walk;
                    }
                }
                joc_stream_buffer input{};
                input.struct_size = sizeof(input);
                input.struct_version = 1;
                input.kind = JOC_STREAM_IN_EAC3;
                input.bytes = impl.eac3_buffer.data();
                input.byte_count = static_cast<std::uint32_t>(push_bytes);
                const joc_error pushed = impl.api.push(impl.stream, &input, nullptr, nullptr);
                if (pushed != JOC_OK) {
                    if (error != nullptr) {
                        *error = std::string("E-AC-3 push failed: ") +
                                 error_text(impl.api, pushed);
                    }
                    return 0;
                }
                impl.frames_queued += pushed_frames;
                impl.eac3_carry = total - push_bytes;
                if (impl.eac3_carry != 0 && push_bytes != 0) {
                    std::memmove(impl.eac3_buffer.data(), impl.eac3_buffer.data() + push_bytes,
                                 impl.eac3_carry);
                }
                if (got == 0) impl.eac3_eof = true;
            }
        }

        // Feed the core PCM until the bed has caught up with the metadata.  The
        // core pairs one syncframe with exactly 1536 bed samples, so only whole
        // frames are pushed; whatever is left over stays at the front of the
        // staging buffer.  Staging counted in bytes, not samples: a pipe read may
        // return any number of bytes, and rounding a read down to whole samples
        // quietly drops the rest of the stream's alignment.
        while (!impl.bed_eof && impl.bed_frames_pushed < impl.frames_queued) {
            constexpr std::size_t kFrameBytes = kFrameSamples * kBedChannels * sizeof(float);
            const std::size_t whole_frames = impl.bed_staged_bytes / kFrameBytes;
            if (whole_frames != 0) {
                joc_stream_buffer input{};
                input.struct_size = sizeof(input);
                input.struct_version = 1;
                input.kind = JOC_STREAM_IN_CORE_PCM;
                input.pcm = impl.bed_buffer.data();
                input.channels = static_cast<std::uint32_t>(kBedChannels);
                input.sample_rate = 48000;
                input.sample_count = static_cast<std::uint32_t>(whole_frames * kFrameSamples);
                const joc_error pushed = impl.api.push(impl.stream, &input, nullptr, nullptr);
                if (pushed != JOC_OK) {
                    if (error != nullptr) {
                        *error = std::string("core PCM push failed: ") +
                                 error_text(impl.api, pushed);
                    }
                    return 0;
                }
                impl.bed_frames_pushed += whole_frames;
                const std::size_t pushed_bytes = whole_frames * kFrameBytes;
                impl.bed_staged_bytes -= pushed_bytes;
                if (impl.bed_staged_bytes != 0) {
                    std::memmove(impl.bed_buffer.data(),
                                 reinterpret_cast<const std::uint8_t*>(impl.bed_buffer.data()) +
                                     pushed_bytes,
                                 impl.bed_staged_bytes);
                }
                continue;
            }

            // Less than one whole frame staged: read more.
            const std::size_t capacity_bytes = impl.bed_buffer.size() * sizeof(float);
            const std::size_t room = capacity_bytes - impl.bed_staged_bytes;
            const std::size_t want =
                (room < kBedReadBytes) ? room : kBedReadBytes;
            if (want == 0) break;  // cannot happen while capacity exceeds a frame
            const std::size_t bytes = impl.bed.read(
                reinterpret_cast<std::uint8_t*>(impl.bed_buffer.data()) + impl.bed_staged_bytes,
                want);
            if (bytes == 0) {
                impl.bed_eof = true;
                break;
            }
            impl.bed_bytes_read += bytes;
            impl.bed_staged_bytes += bytes;
        }

        // Take whatever the renderer has.
        joc_stream_buffer output{};
        output.struct_size = sizeof(output);
        output.struct_version = 1;
        output.kind = (impl.settings.output == Output::kBinaural) ? JOC_STREAM_OUT_BINAURAL
                                                                  : JOC_STREAM_OUT_SPEAKER;
        output.channels = impl.channels;
        output.sample_rate = 48000;
        output.sample_count = static_cast<std::uint32_t>(frames);
        output.out_pcm = destination;
        std::uint32_t produced = 0;
        const joc_error pulled = impl.api.pull(impl.stream, &output, &produced);
        if (pulled != JOC_OK) {
            if (error != nullptr) {
                *error = std::string("pull failed: ") + error_text(impl.api, pulled);
            }
            return 0;
        }
        if (produced != 0u) {
            if (trace) {
                ++impl.trace_count;
                joc_log::line("engine: pulled %u frame(s) on the %u%s attempt", produced,
                                impl.trace_count, impl.trace_count == 1 ? "st" : "th");
            }
            return produced;
        }

        // Nothing more can arrive once the metadata stream is drained and the bed
        // has caught up with it.  The bed process is normally stopped as a
        // consequence of that catch-up rather than by reaching its own end, so
        // waiting for bed_eof here would spin forever.
        const bool no_more_input =
            impl.eac3_eof &&
            (impl.bed_eof || impl.bed_frames_pushed >= impl.frames_queued);
        if (no_more_input) {
            if (!impl.flushed) {
                const joc_error flushed = impl.api.flush(impl.stream);
                if (flushed != JOC_OK) {
                    if (error != nullptr) {
                        *error = std::string("flush failed: ") + error_text(impl.api, flushed);
                    }
                    return 0;
                }
                impl.flushed = true;
                joc_stream_status_info status{};
                status.struct_size = sizeof(status);
                status.struct_version = 1;
                if (impl.api.status(impl.stream, &status) == JOC_OK) {
                    joc_log::line(
                        "core: exhausted (eac3 frames queued=%llu, bed frames pushed=%llu); "
                        "core counters frames_in=%llu frames_out=%llu samples_out=%llu "
                        "buffered=%llu",
                        static_cast<unsigned long long>(impl.frames_queued),
                        static_cast<unsigned long long>(impl.bed_frames_pushed),
                        static_cast<unsigned long long>(status.frames_in),
                        static_cast<unsigned long long>(status.frames_out),
                        static_cast<unsigned long long>(status.samples_out),
                        static_cast<unsigned long long>(status.buffered_samples));
                    joc_log::line("core: bed bytes read=%llu, staged samples left=%llu",
                                    static_cast<unsigned long long>(impl.bed_bytes_read),
                                    static_cast<unsigned long long>(impl.bed_staged_bytes));
                }
                continue;  // drain the tail
            }
            return 0;  // genuinely done
        }
    }
}

}  // namespace joc_decode
