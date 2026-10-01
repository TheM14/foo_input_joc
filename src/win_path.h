// Paths handed to the Win32 file APIs.
//
// Windows refuses a path of MAX_PATH characters or more to any call that does not
// carry the \\?\ prefix.  The prefix is a property of the path, not of the process:
// a component cannot inherit it from the host's manifest, so whoever calls the API
// has to add it.  A file that plays from the desktop therefore stops being found
// once its folder tree is deep enough, which is what this header exists to prevent.
//
// Everything in this component that hands a path to Win32 -- or puts one on an
// ffmpeg command line, which is the same call made by the child process -- goes
// through to_wide_extended().  What is deliberately not routed through it is the
// path the component keeps for itself: the one it logs, and the one it uses as the
// container-probe cache key.  Those stay in their plain form, so a log line reads
// like a path a user can paste into Explorer.
#pragma once

#include <windows.h>

#include <string>

namespace joc_path {

// UTF-8 -> UTF-16, for the W APIs.  Every path crosses into the OS through here:
// the narrow CRT would convert it through the ANSI code page, which is what breaks
// names like "Les Fêtes d'Hébé".
inline std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(),
                        needed);
    return out;
}

// The same path in the form the Win32 file APIs need.  Three kinds of path are left
// exactly as they came in, and each of them would be broken by a prefix:
//
//   * one that already carries \\?\, which makes this idempotent;
//   * one that is not fully qualified -- a relative path, or the ffmpeg setting
//     when it is a bare executable name that PATH resolves.  \\?\ is undefined for
//     those;
//   * one with a "." or ".." segment, because \\?\ switches off the normalization
//     that would resolve it.
//
// The separators are settled first for the same reason: once the prefix is on,
// nothing turns a forward slash into a backslash any more, and both of the checks
// above have to see the path the way the file system will.
inline std::wstring to_wide_extended(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path;

    std::wstring plain = path;
    for (wchar_t& character : plain) {
        if (character == L'/') character = L'\\';
    }

    const bool unc = plain.size() >= 2 && plain[0] == L'\\' && plain[1] == L'\\';
    const bool drive = plain.size() >= 3 && plain[1] == L':' && plain[2] == L'\\';
    if (!unc && !drive) return path;
    if (plain.find(L"\\.\\") != std::wstring::npos ||
        plain.find(L"\\..\\") != std::wstring::npos) {
        return path;
    }

    // A UNC path drops its two leading separators; everything else is copied whole.
    std::wstring out = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    out.append(plain, unc ? 2 : 0, std::wstring::npos);
    return out;
}

inline std::wstring to_wide_extended(const std::string& utf8) {
    return to_wide_extended(to_wide(utf8));
}

// UTF-16 -> UTF-8, the direction the component reports paths in.
inline std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::string::size_type>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

}  // namespace joc_path
