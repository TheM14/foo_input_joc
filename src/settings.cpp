// The umbrella must come first: cfg_var.h pulls in cfg_var_legacy.h, whose whole
// body is behind FOOBAR2000_HAVE_CFG_VAR_LEGACY, a macro foobar2000-winver.h sets.
#include <SDK/foobar2000-lite.h>

#include <SDK/cfg_var.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "settings.h"

namespace {

// One GUID per stored value.  These are the on-disk identity of a setting and
// must never be reused for a different meaning.
constexpr GUID kGuidOutput = {0x2f1c4a90, 0x6d3e, 0x4b51, {0x9a, 0x77, 0x0c, 0x31, 0x8d, 0x54, 0x2e, 0x10}};
constexpr GUID kGuidLayout = {0x7b62e5d1, 0x14a8, 0x4c07, {0x83, 0x2f, 0x6b, 0x90, 0x5a, 0x1d, 0xc4, 0x72}};
// Reused from the previous build, where it stored the .jochrtf path: it still
// holds "the HRTF file", so the meaning is unchanged for anyone upgrading.
constexpr GUID kGuidHrtfFile = {0x4e8a1f36, 0x2c95, 0x47d0, {0xb1, 0x68, 0x3e, 0xa2, 0x77, 0x09, 0xf5, 0x8c}};
constexpr GUID kGuidHrtfSource = {0xa3d51b72, 0x6f48, 0x4c19, {0x9e, 0x04, 0x2b, 0x87, 0xd1, 0x53, 0x6a, 0x20}};
constexpr GUID kGuidBinauralMode = {0x1a57c9e4, 0x83b6, 0x42f1, {0x9c, 0x50, 0x24, 0xfd, 0x6a, 0x18, 0x73, 0x0e}};
constexpr GUID kGuidGainEnabled = {0x74b1e5c8, 0x2a39, 0x4d16, {0x8f, 0x62, 0x0d, 0x51, 0xb7, 0x3c, 0x9a, 0x28}};
constexpr GUID kGuidGain = {0x6c04d8b2, 0xe75f, 0x4a39, {0x8d, 0x26, 0x51, 0xb3, 0xc8, 0x40, 0x9e, 0x27}};
constexpr GUID kGuidTail = {0x38f1b6a7, 0x0d24, 0x4e88, {0xb7, 0x95, 0x2c, 0x61, 0x4f, 0xd2, 0x08, 0xa3}};
constexpr GUID kGuidObjectDelay = {0x5b2e9c18, 0xa640, 0x4d7b, {0x92, 0x0e, 0x37, 0xc5, 0x81, 0x6b, 0x2a, 0x54}};
constexpr GUID kGuidThreads = {0xcb7a4015, 0x3f98, 0x4c62, {0xa0, 0x8d, 0x1b, 0x74, 0x29, 0xe6, 0x53, 0x0f}};
constexpr GUID kGuidFfmpeg = {0x0e9d37c2, 0x5a18, 0x4b40, {0xb6, 0x3f, 0x88, 0x2d, 0x9a, 0x70, 0x1c, 0x65}};

cfg_uint g_output(kGuidOutput, 0);
cfg_string g_layout(kGuidLayout, "7.1");
// 0 = SOFA, 1 = Rosella model, 2 = compiled .jochrtf (advanced)
cfg_uint g_hrtf_source(kGuidHrtfSource, 0);
cfg_string g_hrtf_file(kGuidHrtfFile, "");
cfg_uint g_binaural_mode(kGuidBinauralMode, 3);
cfg_bool g_gain_enabled(kGuidGainEnabled, true);
// milli-decibels, so the value stays an integer
cfg_int g_gain_mdb(kGuidGain, 0);
// milli-seconds
cfg_uint g_tail_ms(kGuidTail, 5000);
cfg_uint g_object_delay(kGuidObjectDelay, 1473);
cfg_uint g_threads(kGuidThreads, 0);
cfg_string g_ffmpeg(kGuidFfmpeg, "ffmpeg");

std::string environment(const char* name) {
    char buffer[4096] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return {};
    return std::string(buffer, length);
}

bool env_double(const char* name, double* out) {
    const std::string text = environment(name);
    if (text.empty()) return false;
    *out = std::atof(text.c_str());
    joc_log::line("settings: %s overrides the stored value (%.3f)", name, *out);
    return true;
}

bool env_uint(const char* name, unsigned* out) {
    const std::string text = environment(name);
    if (text.empty()) return false;
    *out = static_cast<unsigned>(std::strtoul(text.c_str(), nullptr, 10));
    joc_log::line("settings: %s overrides the stored value (%u)", name, *out);
    return true;
}

bool env_string(const char* name, std::string* out) {
    const std::string text = environment(name);
    if (text.empty()) return false;
    *out = text;
    joc_log::line("settings: %s overrides the stored value (%s)", name, text.c_str());
    return true;
}

}  // namespace

namespace joc_settings {

const char* hrtf_source_name(HrtfSource source) {
    return source == HrtfSource::kRosella ? "Rosella 个性化模型" : "SOFA";
}

const char* hrtf_source_extension(HrtfSource source) {
    return source == HrtfSource::kRosella ? "personalized_headphone" : "sofa";
}

const char* hrtf_default_file_name(HrtfSource source) {
    return source == HrtfSource::kRosella ? "binaural.personalized_headphone"
                                         : "binaural.sofa";
}

Values defaults() { return Values(); }

Values read() {
    Values values;
    values.output = static_cast<int>(static_cast<unsigned>(g_output));
    values.speaker_layout = g_layout.get_ptr();
    if (values.speaker_layout.empty()) values.speaker_layout = "7.1";
    const unsigned source = static_cast<unsigned>(g_hrtf_source);
    values.hrtf_source = (source == 1u) ? HrtfSource::kRosella : HrtfSource::kSofa;
    values.hrtf_file = g_hrtf_file.get_ptr();
    values.binaural_mode = static_cast<unsigned>(g_binaural_mode);
    values.gain_enabled = static_cast<bool>(g_gain_enabled);
    values.gain_db = static_cast<double>(static_cast<long long>(g_gain_mdb)) / 1000.0;
    values.tail_seconds = static_cast<double>(static_cast<unsigned>(g_tail_ms)) / 1000.0;
    values.object_delay_samples = static_cast<unsigned>(g_object_delay);
    values.native_threads = static_cast<unsigned>(g_threads);
    values.ffmpeg_path = g_ffmpeg.get_ptr();
    if (values.ffmpeg_path.empty()) values.ffmpeg_path = "ffmpeg";
    return values;
}

void write(const Values& values) {
    g_output = static_cast<unsigned>(values.output);
    g_layout = values.speaker_layout.c_str();
    g_hrtf_source = static_cast<unsigned>(values.hrtf_source);
    g_hrtf_file = values.hrtf_file.c_str();
    g_binaural_mode = values.binaural_mode;
    g_gain_enabled = values.gain_enabled;
    g_gain_mdb = static_cast<long long>(values.gain_db * 1000.0 + (values.gain_db >= 0 ? 0.5 : -0.5));
    g_tail_ms = static_cast<unsigned>(values.tail_seconds * 1000.0 + 0.5);
    g_object_delay = values.object_delay_samples;
    g_threads = values.native_threads;
    g_ffmpeg = values.ffmpeg_path.c_str();
}

joc_decode::Settings current() {
    Values values = read();

    std::string text;
    if (env_string("JOC_OUTPUT", &text)) values.output = (text == "speaker") ? 1 : 0;
    (void)env_string("JOC_LAYOUT", &values.speaker_layout);
    (void)env_string("JOC_HRTF", &values.hrtf_file);
    if (env_string("JOC_HRTF_SOURCE", &text)) {
        values.hrtf_source = (text == "rosella") ? HrtfSource::kRosella : HrtfSource::kSofa;
    }
    (void)env_string("JOC_FFMPEG", &values.ffmpeg_path);
    if (env_string("JOC_BINAURAL_MODE", &text)) {
        values.binaural_mode = (text == "near") ? 1u : (text == "far") ? 2u : 3u;
    }
    (void)env_double("JOC_GAIN_DB", &values.gain_db);
    if (env_string("JOC_GAIN_ENABLED", &text)) values.gain_enabled = (text != "0");
    (void)env_double("JOC_TAIL_SECONDS", &values.tail_seconds);
    (void)env_uint("JOC_OBJECT_DELAY", &values.object_delay_samples);
    (void)env_uint("JOC_THREADS", &values.native_threads);

    joc_decode::Settings settings;
    settings.output =
        (values.output == 0) ? joc_decode::Output::kBinaural : joc_decode::Output::kSpeaker;
    settings.speaker_layout = values.speaker_layout;
    settings.hrtf_source = static_cast<joc_decode::HrtfSource>(values.hrtf_source);
    settings.hrtf_file = values.hrtf_file;
    settings.binaural_mode = values.binaural_mode;
    // The switch is explicit rather than implied by the value: binaural rendering
    // can exceed full scale on material that does not clip in the core mix, so
    // attenuation has to be visible and switchable.
    settings.gain_db = values.gain_enabled ? values.gain_db : 0.0;
    settings.tail_seconds = values.tail_seconds;
    settings.object_delay_samples = values.object_delay_samples;
    settings.native_threads = values.native_threads;
    settings.ffmpeg_path = values.ffmpeg_path.empty() ? "ffmpeg" : values.ffmpeg_path;
    return settings;
}

std::string describe(const joc_decode::Settings& settings) {
    char text[640] = {};
    if (settings.output == joc_decode::Output::kBinaural) {
        std::snprintf(text, sizeof(text),
                      "binaural, hrtf=%s(%s), mode=%u, gain=%.2f dB, tail=%.2f s, delay=%u, "
                      "threads=%u, ffmpeg=%s",
                      hrtf_source_name(static_cast<HrtfSource>(settings.hrtf_source)),
                      settings.hrtf_file.empty() ? "(默认位置)" : settings.hrtf_file.c_str(),
                      settings.binaural_mode, settings.gain_db, settings.tail_seconds,
                      settings.object_delay_samples, settings.native_threads,
                      settings.ffmpeg_path.c_str());
    } else {
        // The HRTF fields mean nothing here, and printing them only invites the
        // reader to wonder why one is missing.
        std::snprintf(text, sizeof(text),
                      "speaker %s, gain=%.2f dB, delay=%u, threads=%u, ffmpeg=%s",
                      settings.speaker_layout.c_str(), settings.gain_db,
                      settings.object_delay_samples, settings.native_threads,
                      settings.ffmpeg_path.c_str());
    }
    return text;
}

std::string describe(const Values& values) {
    char text[640] = {};
    std::snprintf(text, sizeof(text),
                  "output=%d layout=%s hrtf=%s(%s) mode=%u gain=%s%.2f dB tail=%.2f s delay=%u "
                  "threads=%u ffmpeg=\"%s\"",
                  values.output, values.speaker_layout.c_str(),
                  hrtf_source_name(values.hrtf_source), values.hrtf_file.c_str(),
                  values.binaural_mode, values.gain_enabled ? "" : "(off) ", values.gain_db,
                  values.tail_seconds, values.object_delay_samples, values.native_threads,
                  values.ffmpeg_path.c_str());
    return text;
}

}  // namespace joc_settings
