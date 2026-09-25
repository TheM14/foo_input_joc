// The foobar2000 input component: E-AC-3 JOC files.
//
// One deliberate behaviour dominates this file: open() decides from the bitstream
// whether the file is E-AC-3 JOC, and throws exception_io_unsupported_format when
// it is not.  A plain E-AC-3 file must be handed back to the decoder the user
// already had, so that installing this component cannot change how ordinary
// Dolby Digital Plus files sound.

#include <SDK/foobar2000-lite.h>

#include <SDK/audio_chunk.h>
#include <SDK/exception_io.h>
#include <SDK/file_info.h>
#include <SDK/input.h>
#include <SDK/input_file_type.h>
#include <SDK/input_impl.h>

#include <cstring>
#include <string>

#include "eac3_scan.h"
#include "joc_decode.h"
#include "log.h"
#include "prefs.h"
#include "settings.h"

namespace {

constexpr std::size_t kSniffBytes = 256u * 1024u;
constexpr std::size_t kRunFrames = 4096u;
constexpr unsigned kSampleRate = 48000;

// Identity in the decoder priority table.

const GUID g_decoder_guid = {0x9c3f1d58, 0x27ab, 0x4e64, {0xb0, 0x93, 0x5e, 0x1c, 0xd7, 0x48, 0x2f, 0xa6}};

const char* output_name(const joc_decode::Settings& settings) {
    return (settings.output == joc_decode::Output::kBinaural) ? "binaural (HRTF)" : "speaker";
}

const char* binaural_mode_name(std::uint32_t mode) {
    switch (mode) {
        case 1: return "near";
        case 2: return "far";
        default: return "mid";
    }
}

std::string file_name_of(const std::string& path) {
    const std::string::size_type slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

class input_joc : public input_stubs {
public:
    void open(service_ptr_t<file> hint, const char* path, t_input_open_reason reason,
              abort_callback& abort) {
        if (reason == input_open_info_write) throw exception_tagging_unsupported();
        m_path = (path != nullptr) ? path : "";

        service_ptr_t<file> source = hint;
        input_open_file_helper(source, path, reason, abort);
        m_file = source;

        pfc::array_t<t_uint8> buffer;
        buffer.set_size(kSniffBytes);
        const std::size_t got = m_file->read(buffer.get_ptr(), kSniffBytes, abort);
        const joc_eac3::ScanResult scan = joc_eac3::scan(buffer.get_ptr(), got, 8);

        joc_log::line("decoder: open \"%s\" reason=%d bytes=%llu frames=%llu with_joc=%llu",
                        m_path.c_str(), static_cast<int>(reason),
                        static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(scan.frames_examined),
                        static_cast<unsigned long long>(scan.frames_with_joc));

        if (scan.joc != joc_eac3::JocState::kYes) {
            // Hand the file to the next entry in the priority table.
            joc_log::line("decoder: yielding to the built-in decoder (%s)", scan.detail);
            throw exception_io_unsupported_format();
        }
        joc_log::line("decoder: claiming this file as E-AC-3 JOC");

        // The core passes URLs ("file://C:\...").  ffmpeg and the file APIs need a
        // native path, and a URL we cannot map to one is a file we cannot decode.
        try {
            m_native_path = filesystem::g_get_native_path(m_path.c_str());
        } catch (const pfc::exception& error) {
            joc_log::line("decoder: not a native file path (%s): %s", m_path.c_str(),
                            error.what());
            throw exception_io_unsupported_format();
        }
        joc_log::line("decoder: native path \"%s\"", m_native_path.get_ptr());
    }

    void get_info(file_info& info, abort_callback& abort) {
        (void)abort;
        const joc_decode::FileProbe probe = joc_decode::probe_file(m_native_path.get_ptr());
        const joc_decode::Settings settings = joc_settings::current();

        unsigned channels = 2;
        std::string render;
        if (settings.output == joc_decode::Output::kBinaural) {
            channels = 2;
            render = std::string("binaural (") + binaural_mode_name(settings.binaural_mode) + ")";
        } else {
            const unsigned layout_channels = joc_decode::speaker_channels(settings.speaker_layout);
            channels = layout_channels != 0 ? layout_channels : 6;
            render = "speaker " + settings.speaker_layout;
        }

        info.info_set("codec", "E-AC-3 JOC");
        info.info_set("codec_long", "E-AC-3 JOC (Dolby Atmos)");
        info.info_set("encoding", "lossy");
        info.info_set_int("samplerate", kSampleRate);
        info.info_set_int("channels", channels);
        info.info_set_int("bitspersample", 32);
        info.info_set("bitspersample_extra", "floating-point");
        info.set_length(probe.duration_seconds);
        if (probe.duration_seconds > 0.0) {
            const t_filesize bytes = m_file.is_valid() ? m_file->get_size(abort) : filesize_invalid;
            if (bytes != filesize_invalid && bytes > 0) {
                info.info_set_bitrate(static_cast<t_int64>(
                    static_cast<double>(bytes) * 8.0 / probe.duration_seconds / 1000.0));
            }
        }
        // Custom fields: visible in Properties and usable as %joc_*% in title
        // formatting, which is how a user can tell a JOC file from a plain one.
        info.info_set("joc_render", render.c_str());
        info.info_set("joc_hrtf", settings.hrtf_file.empty()
                                      ? "(未设置)"
                                      : file_name_of(settings.hrtf_file).c_str());
        info.info_set_int("joc_frames", static_cast<t_int64>(probe.frames));
        info.info_set("joc_scan", probe.detail.c_str());
        joc_log::line("decoder: get_info duration=%.3f s frames=%llu channels=%u render=%s",
                        probe.duration_seconds, static_cast<unsigned long long>(probe.frames),
                        channels, render.c_str());
    }

    t_filestats2 get_stats2(uint32_t flags, abort_callback& abort) {
        if (m_file.is_valid()) return m_file->get_stats2_(flags, abort);
        throw exception_io_unsupported_format();
    }

    void decode_initialize(unsigned flags, abort_callback& abort) {
        (void)abort;
        m_settings = joc_settings::current();
        joc_log::line("decoder: initialize flags=0x%X settings: %s", flags,
                        joc_settings::describe(m_settings).c_str());

        std::string error;
        if (!m_engine.start(m_native_path.get_ptr(), m_settings, &error)) {
            joc_log::line("decoder: engine start failed: %s", error.c_str());
            // The most common cause by far, and the one a user can act on.
            if (m_settings.output == joc_decode::Output::kBinaural &&
                m_settings.hrtf_file.empty()) {
                throw exception_io_data(
                    "JOC：双耳渲染需要先指定 HRTF 文件（Preferences -> Tools -> JOC 解码器），"
                    "或把输出改为扬声器布局。");
            }
            throw exception_io_data(error.c_str());
        }
        m_channels = m_engine.channels();
        if (m_channels == 0) m_channels = 2;
        m_buffer.resize(kRunFrames * m_channels);
        m_frames_delivered = 0;
        m_reported = false;
        joc_log::line("decoder: engine ready, %u output channel(s), %u frames per read",
                        m_channels, static_cast<unsigned>(kRunFrames));
    }

    bool decode_run(audio_chunk& chunk, abort_callback& abort) {
        (void)abort;
        std::string error;
        const std::size_t frames = m_engine.read(m_buffer.data(), kRunFrames, &error);
        if (frames == 0) {
            if (!error.empty()) {
                joc_log::line("decoder: read failed: %s", error.c_str());
                throw exception_io_data(error.c_str());
            }
            joc_log::line("decoder: end of stream after %llu frames",
                            static_cast<unsigned long long>(m_frames_delivered));
            return false;
        }

        chunk.set_data_size(frames * m_channels);
        chunk.set_channels(m_channels, audio_chunk::g_guess_channel_config(m_channels));
        chunk.set_sample_rate(kSampleRate);
        chunk.set_sample_count(frames);
        std::memcpy(chunk.get_data(), m_buffer.data(),
                    frames * m_channels * sizeof(audio_sample));

        m_frames_delivered += frames;
        if (!m_reported) {
            m_reported = true;
            float peak = 0.0f;
            for (std::size_t i = 0; i < frames * m_channels; ++i) {
                const float value = m_buffer[i] < 0.0f ? -m_buffer[i] : m_buffer[i];
                if (value > peak) peak = value;
            }
            joc_log::line("decoder: first %llu frames delivered (%u ch), peak %.6f",
                            static_cast<unsigned long long>(frames), m_channels,
                            static_cast<double>(peak));
        }
        return true;
    }

    void decode_seek(double, abort_callback&) {
        // The renderer is stateful and has no seek; can_seek() says so.
        throw exception_io_unsupported_format();
    }

    bool decode_can_seek() { return false; }

    size_t extended_param(const GUID& type, size_t arg1, void* arg2, size_t arg2size) {
        (void)arg1;
        (void)arg2;
        (void)arg2size;
        if (type == input_params::seeking_expensive) return 1;
        return 0;
    }

    void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

    static bool g_is_our_path(const char* path, const char* extension) {
        (void)path;
        // Claim by extension, then decide from the bitstream in open(): a file that
        // turns out not to carry JOC is handed back with
        // exception_io_unsupported_format, and the core moves on to the next
        // decoder in its priority table.
        return (extension != nullptr) &&
               ((stricmp_utf8(extension, "eac3") == 0) || (stricmp_utf8(extension, "ec3") == 0));
    }

    static bool g_is_our_content_type(const char* content_type) {
        (void)content_type;
        // Container dispatch (MP4 ec-3, Matroska A_EAC3) is not claimed: only bare
        // E-AC-3 streams are handled, so let the container readers have them.
        return false;
    }

    static GUID g_get_guid() { return g_decoder_guid; }
    static const char* g_get_name() { return "JOC decoder (E-AC-3 JOC / Dolby Atmos)"; }
    static GUID g_get_preferences_guid() { return joc_prefs::page_guid(); }
    static bool g_is_low_merit() { return false; }

private:
    service_ptr_t<file> m_file;
    std::string m_path;
    pfc::string8 m_native_path;
    joc_decode::Settings m_settings;
    joc_decode::Engine m_engine;
    std::vector<float> m_buffer;
    unsigned m_channels = 2;
    std::uint64_t m_frames_delivered = 0;
    bool m_reported = false;
};

static input_singletrack_factory_t<input_joc> g_input_joc_factory;

}  // namespace

DECLARE_FILE_TYPE_EX("eac3;ec3", "E-AC-3 JOC file", "E-AC-3 JOC files");
