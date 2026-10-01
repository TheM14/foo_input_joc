// Checks that the component works with a path past MAX_PATH.
//
//   long_path_test
//
// Windows refuses a path of MAX_PATH characters or more to any Win32 call that does
// not carry the \\?\ prefix.  The prefix is a property of the path rather than of the
// process, so it has to be added by whoever calls the API -- which is why a file that
// plays from the desktop stops being found once its folder tree is deep enough, the
// shape an Atmos download under a long album title has.
//
// The path is built here rather than taken from the command line, so the test needs
// no fixture and cannot be defeated by the ANSI argv a console tool is handed.

#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/container_scan.h"
#include "../src/joc_decode.h"
#include "../src/win_path.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

// %TEMP%\joc_long_path
std::string temp_root() {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0) return {};
    return joc_path::to_utf8(std::wstring(buffer, length) + L"joc_long_path");
}

// One 45-character directory name.  Five of them put the file comfortably past the
// limit whatever %TEMP% is.
std::string level_name(int index) {
    return "level-" + std::string(39, static_cast<char>('a' + index));
}

bool make_directory(const std::string& path) {
    if (CreateDirectoryW(joc_path::to_wide_extended(path).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Builds the deep tree one level at a time, so every parent already exists by the
// time its child is asked for.
std::string build_tree() {
    std::string path = temp_root();
    if (path.empty() || !make_directory(path)) return {};
    for (int i = 0; i < 5; ++i) {
        path += "\\" + level_name(i);
        if (!make_directory(path)) return {};
    }
    return path;
}

// A file that is a container but holds no audio track: enough for the probe to walk
// its boxes and report something of its own, rather than "cannot open the file".
bool write_container(const std::string& path) {
    unsigned char bytes[32] = {};
    bytes[3] = sizeof(bytes);  // box size, big-endian
    const char type[] = "ftypisom";
    for (int i = 0; i < 8; ++i) bytes[4 + i] = static_cast<unsigned char>(type[i]);
    std::FILE* file = _wfopen(joc_path::to_wide_extended(path).c_str(), L"wb");
    if (file == nullptr) return false;
    const std::size_t written = std::fwrite(bytes, 1, sizeof(bytes), file);
    std::fclose(file);
    return written == sizeof(bytes);
}

void remove_tree(const std::string& root, const std::string& leaf) {
    // Files before the directories that hold them, deepest directory first, and
    // never above root.  Worth doing at all because a tree past MAX_PATH is one
    // Explorer cannot delete: leaving it behind would be worse than the disk it
    // occupies.
    DeleteFileW(joc_path::to_wide_extended(leaf + "\\probe.m4a").c_str());
    DeleteFileW(joc_path::to_wide_extended(root + "\\short.m4a").c_str());
    std::string current = leaf;
    while (current.size() > root.size()) {
        RemoveDirectoryW(joc_path::to_wide_extended(current).c_str());
        const std::string::size_type slash = current.find_last_of('\\');
        if (slash == std::string::npos) break;
        current.resize(slash);
    }
    RemoveDirectoryW(joc_path::to_wide_extended(root).c_str());
}

// The rules that keep the prefix from being applied where it would break a path.
void check_helper_rules() {
    check(joc_path::to_wide_extended(std::string("ffmpeg")) == L"ffmpeg",
          "a bare executable name is left for PATH to resolve");
    check(joc_path::to_wide_extended(std::string("sub\\file.m4a")) == L"sub\\file.m4a",
          "a relative path is left alone");
    check(joc_path::to_wide_extended(std::string("C:\\a\\b.m4a")) == L"\\\\?\\C:\\a\\b.m4a",
          "an absolute path gains the prefix");
    check(joc_path::to_wide_extended(std::string("\\\\?\\C:\\a\\b.m4a")) ==
              L"\\\\?\\C:\\a\\b.m4a",
          "an already-prefixed path is not prefixed twice");
    check(joc_path::to_wide_extended(std::string("\\\\server\\share\\b.m4a")) ==
              L"\\\\?\\UNC\\server\\share\\b.m4a",
          "a UNC path takes the UNC form");
    check(joc_path::to_wide_extended(std::string("//server/share/b.m4a")) ==
              L"\\\\?\\UNC\\server\\share\\b.m4a",
          "and the same one in forward slashes, without doubling a separator");
    check(joc_path::to_wide_extended(std::string("C:/a/b.m4a")) == L"\\\\?\\C:\\a\\b.m4a",
          "forward slashes become backslashes, which the prefix stops normalizing");
    check(joc_path::to_wide_extended(std::string("C:\\a\\..\\b.m4a")) == L"C:\\a\\..\\b.m4a",
          "a .. segment is left alone, since the prefix would stop resolving it");
    check(joc_path::to_wide_extended(std::string("C:/a/../b.m4a")) == L"C:/a/../b.m4a",
          "and one in forward slashes is recognized as well");
}

}  // namespace

int main() {
    check_helper_rules();

    const std::string tree = build_tree();
    if (tree.empty()) {
        std::printf("FAIL cannot build the tree under %s\n", temp_root().c_str());
        return 1;
    }
    const std::string long_file = tree + "\\probe.m4a";
    const std::string short_file = temp_root() + "\\short.m4a";
    std::printf("long path  : %zu chars\n", long_file.size());
    std::printf("short path : %zu chars\n", short_file.size());

    check(long_file.size() > MAX_PATH, "the path is past MAX_PATH, so the test is on it");
    check(write_container(long_file), "the file was written at the long path");
    check(write_container(short_file), "the control file was written at the short path");

    // The premise: without the prefix the very same path is refused.  If this ever
    // starts passing, the machine lifts MAX_PATH for ordinary processes and the rest
    // of the test no longer proves anything.
    const HANDLE raw = CreateFileW(joc_path::to_wide(long_file).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    check(raw == INVALID_HANDLE_VALUE, "the raw path is still refused without the prefix");
    if (raw != INVALID_HANDLE_VALUE) CloseHandle(raw);

    const joc_container::Result long_result = joc_container::scan(long_file);
    std::printf("container probe: kind=%s detail=%s\n",
                joc_container::kind_name(long_result.kind), long_result.detail.c_str());
    check(long_result.kind == joc_container::Kind::kMp4,
          "the container probe reads the long path");
    check(long_result.detail != "cannot open the file",
          "and did not fall back to the unreadable-file answer");

    const joc_decode::FileProbe probe = joc_decode::probe_file(long_file);
    std::printf("decode reader  : readable=%d detail=%s\n", probe.readable ? 1 : 0,
                probe.detail.c_str());
    check(probe.readable, "the decode-side reader opens the long path too");

    const joc_container::Result short_result = joc_container::scan(short_file);
    check(short_result.kind == joc_container::Kind::kMp4 && short_result.detail != "cannot open the file",
          "a short path in the same tree still probes, prefix and all");

    remove_tree(temp_root(), tree);

    if (failures == 0) {
        std::printf("long paths are handled\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
