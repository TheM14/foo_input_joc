#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace joc_log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_opened = false;
std::string g_path;          // UTF-8, for reporting
std::wstring g_wide_path;    // for _wfopen
std::wstring g_wide_rolled;  // g_wide_path with the .1 suffix
unsigned long long g_bytes = 0;

// A Media Library scan calls into us for every file it walks, so the cap has to
// be on bytes: the lines a scan produces run from 40 to 2048 bytes each, and a
// cap on their number leaves the file they build unpredictable by a factor of
// fifty.  Sixteen mebibytes per file, one previous file kept, so the component
// never holds more than 32 MiB of log.
constexpr unsigned long long kMaxBytes = 16ull << 20;

// The lines recorded between header_begin() and header_end(): the version
// banner, a handful of lines written once per process.
bool g_recording = false;
std::string g_header;

std::wstring module_directory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_directory), &module)) {
        return {};
    }
    wchar_t buffer[4096] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(4096));
    if (length == 0) return {};
    std::wstring dir(buffer, length);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::string::size_type>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

void open_locked() {
    g_opened = true;

    wchar_t from_env[4096] = {};
    const DWORD env_length = GetEnvironmentVariableW(L"JOC_LOG", from_env,
                                                     static_cast<DWORD>(4096));
    if (env_length > 0 && env_length < 4096) {
        g_wide_path.assign(from_env, env_length);
    } else {
        const std::wstring dir = module_directory();
        if (dir.empty()) return;
        g_wide_path = dir + L"\\joc_decoder.log";
    }

    g_wide_rolled = g_wide_path + L".1";
    g_path = to_utf8(g_wide_path);
    g_file = _wfopen(g_wide_path.c_str(), L"wb");
    if (g_file == nullptr) {
        g_path.clear();
        return;
    }
    // Unbuffered: the log stays readable while the process is running.
    std::setvbuf(g_file, nullptr, _IONBF, 0);
}

// Composes one line with its timestamp, writes it, and accounts for it.  Returns
// the bytes written, 0 when there was nowhere to write them.
unsigned long long write_locked(const char* text) {
    if (g_file == nullptr) return 0;

    SYSTEMTIME now;
    GetLocalTime(&now);
    char composed[2100];
    const int length =
        std::snprintf(composed, sizeof(composed), "%02u:%02u:%02u.%03u [t%05lu] %s\n", now.wHour,
                      now.wMinute, now.wSecond, now.wMilliseconds,
                      static_cast<unsigned long>(GetCurrentThreadId()), text);
    if (length <= 0) return 0;
    // snprintf reports the length it would have needed, which for a truncated
    // line is past the end of the buffer.
    const std::size_t size = (static_cast<std::size_t>(length) < sizeof(composed))
                                 ? static_cast<std::size_t>(length)
                                 : sizeof(composed) - 1;
    std::fwrite(composed, 1, size, g_file);
    g_bytes += size;
    if (g_recording) g_header.append(composed, size);
    return size;
}

// Moves the full file aside and starts an empty one.  Called with the mutex held,
// once the budget is spent.  The banner is written again afterwards, so a log
// that has rolled still opens with the versions it was produced by.
void roll_locked() {
    std::fclose(g_file);
    g_file = nullptr;
    // One previous window, replaced rather than chained: a chain of rolls would
    // be the same unbounded log under another name.  A move that fails -- the .1
    // file held open by a reader -- leaves the truncation below to bound the file
    // anyway, at the cost of the window that would have been kept.
    MoveFileExW(g_wide_path.c_str(), g_wide_rolled.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(g_wide_path.c_str(), L"wb");
    if (g_file == nullptr) return;
    std::setvbuf(g_file, nullptr, _IONBF, 0);
    g_bytes = 0;
    write_locked("[log rolled: the previous window is in the .1 file beside this one]");
    if (!g_header.empty()) {
        std::fwrite(g_header.c_str(), 1, g_header.size(), g_file);
        g_bytes += g_header.size();
    }
}

}  // namespace

void open() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_opened) open_locked();
}

const char* path() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_path.c_str();
}

unsigned long long budget_bytes() { return kMaxBytes; }

void line_v(const char* fmt, va_list args) {
    char text[2048];
    std::vsnprintf(text, sizeof(text), fmt, args);

    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_opened) open_locked();
    if (g_file == nullptr) return;
    if (write_locked(text) != 0 && g_bytes >= kMaxBytes) roll_locked();
}

void line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    line_v(fmt, args);
    va_end(args);
}

void header_begin() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_header.clear();
    g_recording = true;
}

void header_end() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_recording = false;
}

}  // namespace joc_log
