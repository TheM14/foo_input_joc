// Drives the diagnostic log past its byte budget, without foobar2000.
//
//   log_rotation_test
//
// The component's log is bounded by rolling: a file that reaches the budget is
// moved to a .1 sibling and a fresh one is started.  What pushes it there is a
// Media Library scan, which a test cannot arrange, so the budget is spent here
// with lines of a known size instead.  What is checked is what the bound
// promises: neither file passes the budget, there is no chain of rolls, the
// newest lines are the ones in the live file, and the session banner is written
// again after a roll -- which is the only reason a rolled log still says which
// build produced it.

#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/log.h"

namespace {

std::wstring temp_directory() {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    return std::wstring(buffer, length) + L"joc_log_rotation_test";
}

bool read_file(const std::wstring& path, std::string* out) {
    std::FILE* file = _wfopen(path.c_str(), L"rb");
    if (file == nullptr) return false;
    char buffer[64 * 1024];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) != 0) out->append(buffer, got);
    std::fclose(file);
    return true;
}

unsigned long long file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) return 0;
    return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    const std::wstring dir = temp_directory();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring live = dir + L"\\rotation.log";
    const std::wstring rolled = live + L".1";
    const std::wstring chained = live + L".2";
    DeleteFileW(live.c_str());
    DeleteFileW(rolled.c_str());
    DeleteFileW(chained.c_str());
    SetEnvironmentVariableW(L"JOC_LOG", live.c_str());

    const unsigned long long budget = joc_log::budget_bytes();
    std::printf("budget %llu byte(s) per file\n", budget);

    joc_log::open();
    joc_log::header_begin();
    joc_log::line("=== header marker ===");
    joc_log::header_end();

    // Enough lines to spend the budget twice over, so the roll has to happen and
    // the file it produces has to be replaced rather than added to.
    const std::string filler(160, 'x');
    const unsigned lines = static_cast<unsigned>(budget / 190ull * 3ull);
    for (unsigned i = 1; i <= lines; ++i) joc_log::line("filler %u %s", i, filler.c_str());

    const unsigned long long live_size = file_size(live);
    const unsigned long long rolled_size = file_size(rolled);
    std::printf("wrote %u line(s): live %llu byte(s), rolled %llu byte(s)\n", lines, live_size,
                rolled_size);

    check(live_size > 0 && live_size <= budget + 4096, "live file is inside the budget");
    check(rolled_size > 0 && rolled_size <= budget + 4096, "rolled file is inside the budget");
    check(live_size + rolled_size <= 2 * budget + 8192, "the two together are inside twice it");
    check(file_size(chained) == 0, "no second roll is kept");

    std::string live_text;
    std::string rolled_text;
    check(read_file(live, &live_text), "live file reads back");
    check(read_file(rolled, &rolled_text), "rolled file reads back");
    check(contains(live_text, "[log rolled"), "the live file says it rolled");
    check(contains(live_text, "=== header marker ==="), "the banner was written again");

    char newest[64] = {};
    std::snprintf(newest, sizeof(newest), "filler %u ", lines);
    check(contains(live_text, newest), "the newest line is in the live file");
    check(!contains(rolled_text, newest), "and not in the rolled one");

    if (failures == 0) {
        std::printf("log rotation holds\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
