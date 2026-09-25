// Exercises the container probe directly, without going through foobar2000.
//
//   container_scan_test <file> [more files...]
//
// It exists because container dispatch is decided by the core's decoder priority
// table: when a built-in demuxer is offered the file first, this component never
// sees it, so the probe would otherwise never run during a test.  Running it here
// shows what the probe decides for each file on its own.

#include <cstdio>
#include <string>

#include "../src/container_scan.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: container_scan_test <file> [more files...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        const joc_container::Result result = joc_container::scan(path);
        std::printf("%-28s kind=%-9s eac3=%d audio#%u codec=%-6s duration=%8.3f s  %s\n",
                    path.c_str(), joc_container::kind_name(result.kind), result.eac3 ? 1 : 0,
                    result.audio_index, result.codec.empty() ? "-" : result.codec.c_str(),
                    result.duration_seconds, result.detail.c_str());
        if (result.kind == joc_container::Kind::kNone) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
