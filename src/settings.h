// Component settings: what the decoder renders and which HRTF it renders with.
//
// The HRTF source mirrors joc_task_config v2, which is the interface the
// reference CLI uses: a SOFA file, or a Rosella personalized model, are the
// user-facing inputs; a compiled .jochrtf is the advanced override; the filter
// bank tables are compiled into the library and are not a setting at all.
//
// Every value also has an environment override (JOC_*), which exists so the
// decoder can be exercised headlessly; an override is logged when used, so a test
// run cannot silently disagree with what the page shows.
#pragma once

#include <string>

#include "joc_decode.h"

namespace joc_settings {

// Which HRTF the binaural renderer uses.  Only these two inputs exist: everything
// else (compiling a SOFA into the renderer's field, caching it) is internal.
enum class HrtfSource {
    kSofa = 0,     // SOFA SimpleFreeFieldHRIR, compiled by the renderer
    kRosella = 1,  // Rosella .personalized_headphone model
};

const char* hrtf_source_name(HrtfSource source);
const char* hrtf_source_extension(HrtfSource source);
// The file looked up in the default folder (next to the component) when no path
// is configured; same names the reference CLI uses.
const char* hrtf_default_file_name(HrtfSource source);

// The stored values, exactly as the preferences page shows them.
struct Values {
    int output = 0;                   // 0 = binaural, 1 = speaker layout
    std::string speaker_layout = "7.1";
    HrtfSource hrtf_source = HrtfSource::kSofa;
    // Empty means "the default file in the default folder".
    std::string hrtf_file;
    unsigned binaural_mode = 3;       // JOC_BINAURAL_NEAR/FAR/MID = 1/2/3
    bool gain_enabled = true;
    double gain_db = 0.0;
    double tail_seconds = 5.0;
    unsigned object_delay_samples = 1473;
    unsigned native_threads = 0;
    std::string ffmpeg_path = "ffmpeg";
};

Values defaults();
Values read();
void write(const Values& values);

// Stored values plus environment overrides, as the decoder should use them.
joc_decode::Settings current();

std::string describe(const joc_decode::Settings& settings);
std::string describe(const Values& values);

}  // namespace joc_settings
