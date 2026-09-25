#include "joc_decode.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
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
    joc_error(JOC_CALL* reset)(joc_stream*) = joc_stream_reset;
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
class FfmpegPipe {
public:
    ~FfmpegPipe() { stop(); }

    // ffmpeg_path, the input file and whatever should follow "-i <input>" are
    // separate: the 5.1 bed and the E-AC-3 metadata stream of a container file are
    // both ffmpeg output, they only differ in those arguments.  input_arguments come
    // before -i and carry the decoder options the bed needs.
    bool start(const std::string& ffmpeg_path, const std::string& input_path,
               const std::wstring& input_arguments, const std::wstring& output_arguments,
               const char* label, const std::wstring& stderr_path, std::string* error,
               std::size_t pipe_bytes = kBedPipeBytes) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (CreatePipe(&read_end, &write_end, &attributes, static_cast<DWORD>(pipe_bytes)) == FALSE) {
            if (error != nullptr) *error = std::string("cannot create the ") + label + " pipe";
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
        command += L" -hide_banner -loglevel error -nostdin -y ";
        command += input_arguments;  // input options must precede -i
        command += L" -i \"";
        command += utf8_to_wide(input_path);
        command += L"\" ";
        command += output_arguments;

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
            if (error != nullptr) *error = std::string("cannot start ffmpeg for the ") + label;
            return false;
        }
        CloseHandle(process.hThread);
        pipe_ = read_end;
        process_ = process.hProcess;
        joc_log::line("core %s: ffmpeg started (pid %lu)", label, process.dwProcessId);
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
    bool is_open() const { return handle_ != INVALID_HANDLE_VALUE; }
    std::size_t read(void* destination, std::size_t bytes) {
        if (handle_ == INVALID_HANDLE_VALUE) return 0;
        DWORD got = 0;
        if (ReadFile(handle_, destination, static_cast<DWORD>(bytes), &got, nullptr) == FALSE) {
            return 0;
        }
        return got;
    }
    bool seek(std::uint64_t offset) {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER value{};
        value.QuadPart = static_cast<LONGLONG>(offset);
        return SetFilePointerEx(handle_, value, nullptr, FILE_BEGIN) != FALSE;
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

// Every ffmpeg child writes its diagnostics next to the component rather than into
// whatever working directory the host process happens to have; separate files so
// no child can truncate another's.
std::wstring stderr_path_for(const wchar_t* name) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&speaker_channels), &self);
    wchar_t path[4096] = {};
    const DWORD length = GetModuleFileNameW(self, path, 4096);
    if (length == 0) return std::wstring(name);
    const std::wstring text(path, length);
    const std::wstring::size_type slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(name)
                                      : text.substr(0, slash) + L"\\" + name;
}

// The time ffmpeg's -ss takes for a given source sample.  A seek always starts on
// a syncframe boundary, so the sample is exactly representable in microseconds.
std::wstring seek_time(std::uint64_t source_sample) {
    if (source_sample == 0) return {};
    wchar_t text[64] = {};
    std::swprintf(text, 64, L"%.6f", static_cast<double>(source_sample) / 48000.0);
    return text;
}

// Source sample a time offset names, rounded the way the core rounds a position
// (pfc::rint64, i.e. llrint: to nearest, ties to even).
std::int64_t sample_of_seconds(double seconds) {
    if (!(seconds > 0.0)) return 0;  // also catches NaN
    const double value = seconds * 48000.0;
    if (value >= 9.0e18) return 9223372036854775807LL;
    return static_cast<std::int64_t>(std::llrint(value));
}

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

FileProbe probe_file(const std::string& path, std::size_t max_scan_bytes,
                     std::uint64_t start_offset) {
    FileProbe probe;
    InputFile file;
    if (!file.open(path)) {
        probe.detail = "cannot open";
        return probe;
    }
    const std::uint64_t size = file.size();
    if (start_offset >= size) {
        probe.detail = "file too small to be E-AC-3";
        return probe;
    }
    if (start_offset != 0 && !file.seek(start_offset)) {
        probe.detail = "cannot skip the leading tag area";
        return probe;
    }
    const std::uint64_t stream_bytes = size - start_offset;

    // A Media Library scan calls this for every file, so the whole stream is
    // walked only when that is cheap; otherwise the first window is enough,
    // because E-AC-3 syncframes in a stream like this are all the same size.
    const std::uint64_t kFullWalkLimit = 16u * 1024u * 1024u;
    const std::size_t window =
        (max_scan_bytes != 0) ? max_scan_bytes : 256u * 1024u;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(
        (stream_bytes < kFullWalkLimit && stream_bytes > 0) ? stream_bytes : window));
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
    if (buffer.size() == stream_bytes) {
        std::size_t offset = 0;
        while (true) {
            const std::size_t bytes = joc_eac3::frame_bytes_at(buffer.data(), got, offset);
            if (bytes == 0 || offset + bytes > got) break;
            offset += bytes;
            ++frames;
        }
        probe.detail = "frame count walked over the whole file";
    } else if (scan.all_frames_same_size) {
        frames = stream_bytes / scan.first_frame_bytes;
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
                                     (static_cast<double>(stream_bytes) /
                                      static_cast<double>(offset)) *
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
// JOC verdict for a container file, without rendering anything.
// ---------------------------------------------------------------------------
bool probe_container_joc(const std::string& ffmpeg_path, const std::string& path,
                         unsigned audio_index, bool* joc, std::string* detail) {
    struct Entry {
        std::string path;
        unsigned audio_index = 0;
        std::uint64_t size = 0;
        std::uint64_t modified = 0;
        bool joc = false;
        std::string detail;
    };
    static std::vector<Entry> cache;

    std::uint64_t size = 0;
    std::uint64_t modified = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(utf8_to_wide(path).c_str(), GetFileExInfoStandard, &data) !=
            FALSE) {
            size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            modified = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                       data.ftLastWriteTime.dwLowDateTime;
        }
    }
    for (const Entry& entry : cache) {
        if (entry.path == path && entry.audio_index == audio_index && entry.size == size &&
            entry.modified == modified) {
            if (joc != nullptr) *joc = entry.joc;
            if (detail != nullptr) *detail = entry.detail;
            return true;
        }
    }

    if (ffmpeg_path.empty()) {
        if (detail != nullptr) *detail = "no ffmpeg configured";
        return false;
    }

    FfmpegPipe pipe;
    std::string error;
    std::wstring arguments = L"-map 0:a:";
    arguments += std::to_wstring(audio_index);
    arguments += L" -vn -t 3 -c:a copy -f eac3 -";
    const std::wstring stderr_path = [] {
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        return length == 0 ? std::wstring(L"NUL")
                           : std::wstring(temp) + L"joc_container_probe.log";
    }();
    if (!pipe.start(ffmpeg_path, path, L"", arguments, "probe", stderr_path, &error, 1u << 20)) {
        if (detail != nullptr) *detail = error;
        return false;
    }

    std::vector<std::uint8_t> prefix(512u * 1024u);
    std::size_t filled = 0;
    while (filled < prefix.size()) {
        const std::size_t got = pipe.read(prefix.data() + filled, prefix.size() - filled);
        if (got == 0) break;
        filled += got;
    }
    pipe.stop();

    const joc_eac3::ScanResult scan = joc_eac3::scan(prefix.data(), filled, 8);
    const bool carries_joc = (scan.joc == joc_eac3::JocState::kYes);

    Entry entry;
    entry.path = path;
    entry.audio_index = audio_index;
    entry.size = size;
    entry.modified = modified;
    entry.joc = carries_joc;
    entry.detail = scan.detail;
    if (cache.size() >= 8) cache.erase(cache.begin());
    cache.push_back(entry);

    joc_log::line("container: %s track %u -> %s (%s)", path.c_str(), audio_index,
                  carries_joc ? "JOC" : "not JOC", scan.detail);
    if (joc != nullptr) *joc = carries_joc;
    if (detail != nullptr) *detail = scan.detail;
    return true;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
struct Engine::Impl {
    CoreApi api;
    joc_stream* stream = nullptr;
    InputFile eac3;        // bare E-AC-3 input: read straight from the file
    FfmpegPipe eac3_pipe;  // E-AC-3 inside a container: ffmpeg extracts the stream
    bool eac3_from_pipe = false;
    FfmpegPipe bed;
    Settings settings;
    std::string input_path;

    // The metadata stream comes either from the file itself or from ffmpeg.
    std::size_t read_eac3(void* destination, std::size_t bytes) {
        return eac3_from_pipe ? eac3_pipe.read(destination, bytes)
                              : eac3.read(destination, bytes);
    }

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

    // Seek state.  seek_skip counts output frames still to be discarded: a seek
    // restarts the inputs on the syncframe before the target, and the samples the
    // renderer produces for the part already behind the target are dropped here.
    std::uint64_t seek_skip = 0;
    // Delivered-stream accounting: where the stream currently being delivered starts
    // on the source timeline, how much of it has been handed over, and where it has
    // to stop.  end_sample is the file's own end, so a binaural room tail cannot
    // turn into playback time the file does not have.
    std::uint64_t start_sample = 0;
    std::uint64_t delivered = 0;
    std::uint64_t end_sample = 0;   // 0 = no limit
    // Syncframes of a container's metadata stream still to be read and thrown away.
    // ffmpeg's input seek on a copy stream lands on the frame whose timestamp is
    // at or after the target, which is not reliably the frame the bed restarts on,
    // so a container seek re-reads the stream and drops the frames here instead.
    std::uint64_t metadata_skip = 0;
    bool at_end = false;                      // seek landed at or past the end
    std::function<bool()> abort_check;
    // Bare-stream syncframe index: offset of every syncframe within the stream
    // (so index_base has to be added to get a file offset).  Filled by walking the
    // file, and only as far as a seek asks for.
    std::vector<std::uint64_t> frame_offsets;
    std::uint64_t index_base = 0;              // file offset the stream starts at
    std::uint64_t index_file_offset = 0;       // file offset the walk continues from
    std::uint64_t index_stream_offset = 0;     // stream offset the walk continues from
    bool index_complete = false;

    void reset_feed_state() {
        frames_queued = 0;
        bed_frames_pushed = 0;
        eac3_eof = false;
        bed_eof = false;
        flushed = false;
        trace_count = 0;
        read_calls = 0;
        eac3_carry = 0;
        bed_bytes_read = 0;
        bed_staged_bytes = 0;
        seek_skip = 0;
        metadata_skip = 0;
        start_sample = 0;
        delivered = 0;
        at_end = false;
    }

    void stop_inputs() {
        bed.stop();
        eac3_pipe.stop();
        eac3.close();
    }

    void reset_index() {
        frame_offsets.clear();
        index_base = settings.stream_start_bytes;
        index_file_offset = index_base;
        index_stream_offset = 0;
        index_complete = false;
    }

    // Grows the syncframe index until it holds `wanted` entries or the stream ends.
    bool grow_frame_index(std::uint64_t wanted, std::string* error);
    // Points the file at the syncframe `frame`, or reports at_end when the stream
    // has fewer frames than that.
    bool position_bare(std::uint64_t frame, std::string* error);
    bool start_bed(std::uint64_t source_sample, std::string* error);
    bool start_metadata(std::string* error);
};

bool Engine::Impl::grow_frame_index(std::uint64_t wanted, std::string* error) {
    constexpr std::size_t kWindow = 256u * 1024u;
    std::vector<std::uint8_t> buffer(kWindow);
    while (frame_offsets.size() < wanted && !index_complete) {
        if (abort_check != nullptr && !abort_check()) {
            if (error != nullptr) *error = "the seek was aborted";
            return false;
        }
        if (!eac3.is_open() && !eac3.open(input_path)) {
            if (error != nullptr) *error = "cannot reopen the input file";
            return false;
        }
        const std::uint64_t size = eac3.size();
        if (index_file_offset + 4u > size) {
            index_complete = true;
            break;
        }
        if (!eac3.seek(index_file_offset)) {
            if (error != nullptr) *error = "cannot position the input file";
            return false;
        }
        const std::uint64_t remaining = size - index_file_offset;
        const std::size_t want = static_cast<std::size_t>(
            remaining < kWindow ? remaining : kWindow);
        const std::size_t got = eac3.read(buffer.data(), want);
        if (got < 4u) {
            index_complete = true;
            break;
        }
        std::size_t consumed = 0;
        while (frame_offsets.size() < wanted) {
            const std::size_t bytes = joc_eac3::frame_bytes_at(buffer.data(), got, consumed);
            if (bytes == 0 || consumed + bytes > got) break;
            frame_offsets.push_back(index_stream_offset + consumed);
            consumed += bytes;
        }
        if (consumed == 0) {
            // Not even one whole syncframe in a full window: the stream stops here.
            index_complete = true;
            break;
        }
        index_file_offset += consumed;
        index_stream_offset += consumed;
    }
    return true;
}

bool Engine::Impl::position_bare(std::uint64_t frame, std::string* error) {
    if (!grow_frame_index(frame + 1u, error)) return false;
    if (frame >= frame_offsets.size()) {
        // Past the last syncframe: the decoder contract asks for a successful seek
        // that the next read() answers with end of stream.
        at_end = true;
        stop_inputs();
        return true;
    }
    // Position check first: frame_offsets.size() is what bounds the index.
    const std::uint64_t file_offset =
        index_base + frame_offsets[static_cast<std::size_t>(frame)];
    if (!eac3.is_open() && !eac3.open(input_path)) {
        if (error != nullptr) *error = "cannot reopen the input file";
        return false;
    }
    // The renderer rejects a stream that does not begin on a syncword and never
    // resynchronises, so a mispositioned start has to fail loudly here rather than
    // turn into silence at the end of the file.
    std::uint8_t header[8] = {};
    if (!eac3.seek(file_offset) || eac3.read(header, sizeof(header)) < 4u ||
        joc_eac3::frame_bytes_at(header, sizeof(header), 0) == 0) {
        if (error != nullptr) {
            *error = "the syncframe index does not point at a syncframe (offset " +
                     std::to_string(file_offset) + ")";
        }
        return false;
    }
    if (!eac3.seek(file_offset)) {
        if (error != nullptr) *error = "cannot position the input file";
        return false;
    }
    return true;
}

bool Engine::Impl::start_bed(std::uint64_t source_sample, std::string* error) {
    // The 5.1 core PCM, exactly as the reference renderer's own core decode does
    // it: 5.1 interleaved float32 at 48 kHz, the layout the renderer expects
    // (L R C LFE Ls Rs).
    // -drc_scale 0 -target_level 0: the bed is taken as stored, without the
    // stream's dynrng or target-level metadata being applied by the decoder.
    //
    // -ss is an input option: the demuxer is positioned on the frame boundary and
    // decoding starts there, so a seek costs the same wherever it lands.  The price is
    // that the bed is carried by a decoder that started at the seek point rather than
    // at the start of the file, which is a low-level, noise-like difference from a
    // play-through rather than a misalignment: the position stays exact.
    std::wstring input_arguments = L"-drc_scale 0 -target_level 0";
    const std::wstring offset = seek_time(source_sample);
    if (!offset.empty()) input_arguments += L" -ss " + offset;
    std::wstring bed_arguments = L"-map 0:a:";
    bed_arguments += std::to_wstring(settings.audio_index);
    bed_arguments += L" -vn -ac 6 -ar 48000 -c:a pcm_f32le -f f32le -";
    return bed.start(settings.ffmpeg_path, input_path, input_arguments, bed_arguments, "bed",
                     stderr_path_for(L"joc_ffmpeg_bed.log"), error);
}

bool Engine::Impl::start_metadata(std::string* error) {
    // Stream copy from the start of the track: the syncframes arrive byte for byte
    // as they are stored, which is what the JOC metadata needs, and a seek then
    // discards whole syncframes from the front (see metadata_skip) rather than
    // asking ffmpeg to position the stream.
    std::wstring stream_arguments = L"-map 0:a:";
    stream_arguments += std::to_wstring(settings.audio_index);
    stream_arguments += L" -vn -c:a copy -f eac3 -";
    return eac3_pipe.start(settings.ffmpeg_path, input_path, std::wstring(), stream_arguments,
                           "metadata", stderr_path_for(L"joc_ffmpeg_stream.log"), error,
                           1u << 20);
}

Engine::Engine() : impl_(new Impl()) {}

Engine::~Engine() {
    stop();
    delete impl_;
}

unsigned Engine::channels() const { return impl_->channels; }

void Engine::set_abort_check(std::function<bool()> check) { impl_->abort_check = std::move(check); }

void Engine::set_length(double seconds) { impl_->end_sample = sample_of_seconds(seconds); }

void Engine::stop() {
    Impl& impl = *impl_;
    if (impl.stream != nullptr && impl.api.destroy != nullptr) {
        impl.api.destroy(impl.stream);
        impl.stream = nullptr;
    }
    impl.stop_inputs();
    impl.reset_feed_state();
}

bool Engine::start(const std::string& input_path, const Settings& settings, std::string* error) {
    Impl& impl = *impl_;
    // initialize() may be called more than once on the same instance, and each call
    // has the renderer and its inputs start from scratch.
    stop();
    impl.settings = settings;
    impl.input_path = input_path;
    impl.reset_index();
    impl.reset_feed_state();
    impl.end_sample = sample_of_seconds(settings.length_seconds);
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

    // Metadata stream: a bare E-AC-3 file is read directly, while the E-AC-3 track
    // of a container is extracted by ffmpeg (stream copy, so the syncframes reach
    // the renderer exactly as stored).
    impl.eac3_from_pipe = (settings.input_kind == InputKind::kContainer);
    if (!impl.eac3_from_pipe) {
        if (!impl.eac3.open(input_path)) {
            if (error != nullptr) *error = "cannot open the input file";
            return false;
        }
        // A tag area in front of the stream is skipped here rather than by the
        // renderer, which rejects a stream that does not begin on a syncword.
        if (impl.index_base != 0 && !impl.eac3.seek(impl.index_base)) {
            if (error != nullptr) *error = "cannot skip the leading tag area";
            return false;
        }
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

    // ffmpeg's stderr lands next to the component rather than in whatever working
    // directory the host process happens to have.  The two children write separate
    // files so neither can truncate the other's diagnostics.
    if (!impl.start_bed(0, error)) return false;
    if (impl.eac3_from_pipe && !impl.start_metadata(error)) return false;
    return true;
}

bool Engine::seek(double seconds, double total_seconds, std::string* error) {
    Impl& impl = *impl_;
    if (impl.stream == nullptr || impl.api.reset == nullptr) {
        if (error != nullptr) *error = "the renderer is not running";
        return false;
    }

    // Where the stream that is about to be delivered starts:
    //
    //   * the frame the target sits in, minus a pre-roll, so the renderer's own state
    //     -- object timeline, matrix interpolation, room tail -- has converged by the
    //     time the target itself is delivered.  Both inputs restart there, and the bed
    //     is positioned with an input seek, which is what keeps the cost of a seek
    //     independent of where it lands;
    //   * the samples in front of the target are then dropped from the output, which is
    //     what makes the delivery start exactly on the requested sample.
    const std::int64_t target = sample_of_seconds(seconds);
    std::int64_t wanted = target - static_cast<std::int64_t>(impl.settings.pipeline_delay_samples);
    if (wanted < 0) wanted = 0;
    const std::uint64_t target_frame = static_cast<std::uint64_t>(wanted) / kFrameSamples;
    const std::uint64_t preroll = impl.settings.seek_preroll_frames;
    const std::uint64_t frame = (target_frame > preroll) ? (target_frame - preroll) : 0;
    const std::uint64_t first_sample = frame * kFrameSamples;
    const std::uint64_t skip = static_cast<std::uint64_t>(wanted) - first_sample;

    impl.stop_inputs();
    impl.reset_feed_state();

    if (impl.eac3_from_pipe) {
        // A container's frame count is not known before it is decoded, so the
        // caller's duration is what says whether this lands past the end.
        if (total_seconds > 0.0 && seconds >= total_seconds) {
            impl.at_end = true;
            joc_log::line("engine: seek %.6f s is at or past the end (%.6f s)", seconds,
                          total_seconds);
        }
    } else if (!impl.position_bare(frame, error)) {
        return false;
    }

    if (!impl.at_end) {
        if (!impl.start_bed(first_sample, error)) return false;
        if (impl.eac3_from_pipe) {
            if (!impl.start_metadata(error)) return false;
            impl.metadata_skip = frame;
        }
        impl.seek_skip = skip;
        impl.start_sample = static_cast<std::uint64_t>(wanted);
        impl.delivered = 0;
    }

    // joc_stream_reset keeps the renderer and its HRTF field: it drops the whole
    // timeline, gain ramps, room tail and filter-bank history, which is exactly
    // what a restart at another position needs.
    const joc_error reset = impl.api.reset(impl.stream);
    if (reset != JOC_OK) {
        if (error != nullptr) {
            *error = std::string("cannot reset the render stream: ") +
                     error_text(impl.api, reset);
        }
        return false;
    }
    joc_log::line("engine: seek %.6f s -> source sample %lld, frames %llu.. from sample %llu, "
                  "drop %llu sample(s)%s",
                  seconds, static_cast<long long>(target),
                  static_cast<unsigned long long>(frame),
                  static_cast<unsigned long long>(first_sample),
                  static_cast<unsigned long long>(impl.seek_skip),
                  impl.at_end ? " (at end)" : "");
    return true;
}

std::size_t Engine::read(float* destination, std::size_t frames, std::string* error) {
    Impl& impl = *impl_;
    if (impl.stream == nullptr || frames == 0) return 0;
    // A seek at or past the end of the file succeeds and leaves the next read to
    // report end of stream.
    if (impl.at_end) return 0;
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
            const std::size_t got = impl.read_eac3(impl.eac3_buffer.data() + impl.eac3_carry,
                                                   impl.eac3_buffer.size() - impl.eac3_carry);
            std::size_t total = impl.eac3_carry + got;
            if (total == 0) {
                impl.eac3_eof = true;
            } else {
                // Syncframes in front of a seek target are thrown away before
                // anything is interpreted.  They are dropped a whole window at a
                // time, so only as much of the stream is read as the skip needs.
                if (impl.metadata_skip != 0) {
                    std::size_t dropped = 0;
                    while (impl.metadata_skip != 0) {
                        const std::size_t bytes =
                            joc_eac3::frame_bytes_at(impl.eac3_buffer.data(), total, dropped);
                        if (bytes == 0 || dropped + bytes > total) break;
                        dropped += bytes;
                        --impl.metadata_skip;
                    }
                    if (dropped != 0) {
                        total -= dropped;
                        std::memmove(impl.eac3_buffer.data(),
                                     impl.eac3_buffer.data() + dropped, total);
                    }
                    if (impl.metadata_skip != 0) {
                        // The window ended inside the part to be skipped: keep what
                        // is left and come back with the next read.
                        impl.eac3_carry = total;
                        if (got == 0) impl.eac3_eof = true;
                        continue;
                    }
                }

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
            std::size_t count = produced;
            if (impl.seek_skip != 0) {
                // The renderer had to be fed from before the seek target, so the
                // samples it produces for that part are dropped before anything
                // reaches the caller: the first sample handed over is the target.
                const std::size_t drop = (impl.seek_skip < count)
                                             ? static_cast<std::size_t>(impl.seek_skip)
                                             : count;
                impl.seek_skip -= drop;
                count -= drop;
                if (count != 0) {
                    std::memmove(destination, destination + drop * impl.channels,
                                 count * impl.channels * sizeof(float));
                }
                if (count == 0) continue;  // the whole pull was pre-roll
            }
            // The renderer's binaural room tail is not part of the file: the stream
            // ends where the file ends, not later.
            if (impl.end_sample != 0) {
                const std::uint64_t from = impl.start_sample + impl.delivered;
                if (from >= impl.end_sample) return 0;
                const std::uint64_t left = impl.end_sample - from;
                if (left < count) count = static_cast<std::size_t>(left);
            }
            impl.delivered += count;
            if (trace) {
                ++impl.trace_count;
                joc_log::line("engine: pulled %u frame(s) on the %u%s attempt", count,
                                impl.trace_count, impl.trace_count == 1 ? "st" : "th");
            }
            return count;
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
