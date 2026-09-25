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
unsigned long long g_lines = 0;
bool g_capped = false;

// A Media Library scan calls into us for every file; the cap keeps a long
// session from filling the disk, and says so once when it is reached.
constexpr unsigned long long kMaxLines = 500000;

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

    g_path = to_utf8(g_wide_path);
    g_file = _wfopen(g_wide_path.c_str(), L"wb");
    if (g_file == nullptr) {
        g_path.clear();
        return;
    }
    // Unbuffered: the log stays readable while the process is running.
    std::setvbuf(g_file, nullptr, _IONBF, 0);
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

void line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char text[2048];
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_opened) open_locked();
    if (g_file == nullptr) return;
    if (g_lines >= kMaxLines) {
        if (!g_capped) {
            g_capped = true;
            std::fputs("[log capped]\n", g_file);
        }
        return;
    }
    ++g_lines;

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [t%05lu] %s\n", now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds,
                 static_cast<unsigned long>(GetCurrentThreadId()), text);
}

}  // namespace joc_log
