// Component entry point: identity, and the one line of start-up diagnostics.
//
// foobar2000-lite.h is the SDK's recommended entry header; the rest are pulled in
// on a need-to-use basis.

#include <SDK/foobar2000-lite.h>

#include <SDK/componentversion.h>
#include <SDK/coreversion.h>
#include <SDK/initquit.h>
#include "log.h"

// Kept in one place: the string reported to foobar2000 and written to the log
// must not drift apart.
#define JOC_VERSION "0.2.0"

DECLARE_COMPONENT_VERSION("JOC decoder (E-AC-3 JOC)", JOC_VERSION,
                          "Plays E-AC-3 JOC (Dolby Atmos) files: the JOC objects are "
                          "rendered to binaural (SOFA or Rosella HRTF) or to a speaker "
                          "layout up to 7.1.");

// Refuses to run when the file has been renamed; the troubleshooter relies on it.
VALIDATE_COMPONENT_FILENAME("foo_input_joc.dll");

namespace {

// Writes one banner per session so a support log always starts with the versions
// it was produced by.
class joc_initquit : public initquit {
public:
    void on_init() override {
        joc_log::open();
        joc_log::line("=== foo_input_joc %s ===", JOC_VERSION);
        joc_log::line("foobar2000 core : %s", core_version_info::g_get_version_string());
        joc_log::line("component file  : %s", core_api::get_my_file_name());
        joc_log::line("profile path    : %s", core_api::get_profile_path());
        joc_log::line("portable mode   : %s",
                      core_api::is_portable_mode_enabled() ? "yes" : "no");
        joc_log::line("log file        : %s", joc_log::path());
        joc_log::line("compiled as     : %s",
#if defined(_M_IX86)
                      "x86 (32-bit)"
#elif defined(_M_X64)
                      "x64"
#else
                      "other"
#endif
        );
    }

    void on_quit() override { joc_log::line("=== foo_input_joc shutdown ==="); }
};

FB2K_SERVICE_FACTORY(joc_initquit);

}  // namespace
