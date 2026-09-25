// Cross-validation of the plugin's bitstream gate against the rendering core.
//
//   scan_crosscheck <file.eac3> [more files...]
//
// For every syncframe in the first part of each file this compares
//   * the frame length the plugin derives from frmsiz, against joc_eac3_frame_bytes()
//   * the plugin's JOC verdict, against whether joc_parse_eac3_frame() succeeds
//
// joc_parse_eac3_frame() is the core's [T2] verification tier.  A media host must
// not build on that tier, which is exactly why the plugin has its own gate -- but
// it is the right reference for proving the gate agrees with the renderer, and
// that is what this tool is for.

#include <cstdio>
#include <cstring>
#include <vector>

#include "joc_core.h"

#include "../src/eac3_scan.h"

namespace {

bool kernel_accepts_frame(const unsigned char* frame, std::size_t frame_bytes,
                          const char** reason) {
    joc_frame_params params{};
    params.struct_size = sizeof(params);
    params.struct_version = JOC_FRAME_PARAMS_VERSION;
    joc_emdf_info emdf{};
    emdf.struct_size = sizeof(emdf);
    emdf.struct_version = JOC_EMDF_INFO_VERSION;

    const joc_error error = joc_parse_eac3_frame(frame, frame_bytes, &params, &emdf);
    if (error == JOC_OK) return true;
    if (reason != nullptr) *reason = joc_error_name(error);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: scan_crosscheck <file> [more files...]\n");
        return 2;
    }
    int mismatches = 0;
    int files = 0;

    for (int i = 1; i < argc; ++i) {
        std::FILE* handle = std::fopen(argv[i], "rb");
        if (handle == nullptr) {
            std::fprintf(stderr, "%s: cannot open\n", argv[i]);
            ++mismatches;
            continue;
        }
        std::vector<unsigned char> buffer(256u * 1024u);
        const std::size_t size = std::fread(buffer.data(), 1, buffer.size(), handle);
        std::fclose(handle);
        ++files;

        std::size_t offset = 0;
        std::size_t frames = 0;
        std::size_t length_mismatch = 0;
        std::size_t verdict_mismatch = 0;
        std::size_t plugin_yes = 0;
        std::size_t kernel_yes = 0;
        const char* first_reason = "";

        while (frames < 64) {
            const std::size_t mine = joc_eac3::frame_bytes_at(buffer.data(), size, offset);
            if (mine == 0) break;
            if (offset + mine > size) break;

            std::size_t theirs = 0;
            const joc_error length_error = joc_eac3_frame_bytes(buffer.data(), size, offset, &theirs);
            if (length_error != JOC_OK || theirs != mine) ++length_mismatch;

            const bool plugin = joc_eac3::frame_has_joc(buffer.data() + offset, mine);
            const char* reason = "";
            const bool kernel = kernel_accepts_frame(buffer.data() + offset, mine, &reason);
            if (plugin != kernel) {
                if (verdict_mismatch == 0) first_reason = reason;
                ++verdict_mismatch;
            }
            plugin_yes += plugin ? 1u : 0u;
            kernel_yes += kernel ? 1u : 0u;

            ++frames;
            offset += mine;
        }

        std::printf("%-34s frames=%-3llu plugin_joc=%-3llu kernel_joc=%-3llu len_mismatch=%llu verdict_mismatch=%llu\n",
                    argv[i], static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(plugin_yes),
                    static_cast<unsigned long long>(kernel_yes),
                    static_cast<unsigned long long>(length_mismatch),
                    static_cast<unsigned long long>(verdict_mismatch));
        if (verdict_mismatch != 0) {
            std::printf("%-34s   first kernel error on a disagreeing frame: %s\n", "", first_reason);
        }
        mismatches += static_cast<int>(length_mismatch + verdict_mismatch);
    }

    std::printf("crosscheck: %d file(s), %s\n", files, mismatches == 0 ? "AGREES WITH CORE" : "DISAGREES");
    return mismatches == 0 ? 0 : 1;
}
