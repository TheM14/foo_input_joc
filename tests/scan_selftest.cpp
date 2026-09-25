// Standalone self-test for the JOC bitstream gate.
//
//   scan_selftest <file.eac3> [more files...]
//
// Prints, per file, how many of the first syncframes carry the JOC EMDF container.
// Run it against a known JOC stream and against one whose extension has been
// stripped (ffmpeg -c copy -bsf:a eac3_core): the first must report every frame
// with JOC, the second none.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/eac3_scan.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: scan_selftest <file> [more files...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        std::FILE* file = std::fopen(argv[i], "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "%s: cannot open\n", argv[i]);
            ++failures;
            continue;
        }
        std::vector<unsigned char> buffer(256u * 1024u);
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), file);
        std::fclose(file);

        const joc_eac3::ScanResult result = joc_eac3::scan(buffer.data(), got, 64);
        const char* state = "unknown";
        if (result.joc == joc_eac3::JocState::kYes) state = "JOC";
        else if (result.joc == joc_eac3::JocState::kNo) state = "not-JOC";

        std::printf("%-28s %-8s frames=%llu with_joc=%llu first_frame=%llu same_size=%s\n",
                    argv[i], state, static_cast<unsigned long long>(result.frames_examined),
                    static_cast<unsigned long long>(result.frames_with_joc),
                    static_cast<unsigned long long>(result.first_frame_bytes),
                    result.all_frames_same_size ? "yes" : "no");
        std::printf("%-28s   %s\n", "", result.detail);
    }
    return failures == 0 ? 0 : 1;
}
