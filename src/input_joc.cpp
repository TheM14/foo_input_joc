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
#include <SDK/file_info_impl.h>
#include <SDK/input.h>
#include <SDK/input_file_type.h>
#include <SDK/input_impl.h>
#include <SDK/tag_processor.h>

#include <cstring>
#include <string>

#include "container_scan.h"
#include "eac3_scan.h"
#include "joc_decode.h"
#include "log.h"
#include "prefs.h"
#include "settings.h"

namespace {

constexpr std::size_t kSniffBytes = 256u * 1024u;
constexpr std::size_t kRunFrames = 4096u;
constexpr unsigned kSampleRate = 48000;

// Largest magnitude in a block, for the delivery check in decode_run().
template <typename Sample>
double peak_of(const Sample* values, std::size_t count) {
    double peak = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value =
            values[i] < Sample(0) ? -static_cast<double>(values[i]) : static_cast<double>(values[i]);
        if (value > peak) peak = value;
    }
    return peak;
}

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

// ---------------------------------------------------------------------------
// Tags of a file this component has taken over.
//
// An MP4/M4A keeps its tags in its own metadata box, and the component that knows
// how to read and write them is the container reader the core already ships.
// Claiming a file for decoding must not take it away from that reader, and the SDK
// has no "decode with me, ask someone else for tags" arrangement -- whichever
// entry answers open() answers for everything.  So the information read and write
// paths are forwarded to whichever other entry claims the file, and only the tags
// of its answer are merged into ours: the technical information stays this
// component's own, which is what tells a user the file is JOC rather than plain
// E-AC-3.
// ---------------------------------------------------------------------------

// Entries other than this one that claim the path, in the user's own decoding
// order.  Ourselves is never in the list: an open forwarded back here would enter
// open() again, for ever.
void forwarding_candidates(const char* url, pfc::list_t<input_entry::ptr>& out) {
    out.remove_all();
    input_manager_v3::ptr manager;
    if (input_manager_v3::tryGet(manager)) {
        manager->get_enabled_inputs(out);
    } else {
        input_entry::g_find_inputs_by_path_ex(out, url,
                                              [](input_entry::ptr) { return true; });
    }
    const char* dot = std::strrchr(url, '.');
    const char* extension = (dot != nullptr) ? dot + 1 : "";
    const GUID self = g_decoder_guid;
    for (t_size index = out.get_count(); index-- > 0;) {
        input_entry::ptr entry = out[index];
        if (entry->get_guid_() == self || !entry->is_our_path(url, extension)) {
            out.remove_by_idx(index);
        }
    }
}

// Opens the file again through another entry, for information reading or writing.
// The file is left unopened on our side, so the other entry can have it to itself.
template <typename t_interface>
bool open_forwarded(service_ptr_t<t_interface>& out, const GUID& what_for, const char* url,
                    abort_callback& abort, pfc::string8* name) {
    out.release();
    pfc::list_t<input_entry::ptr> candidates;
    forwarding_candidates(url, candidates);
    if (candidates.get_count() == 0) return false;
    try {
        GUID used = pfc::guid_null;
        service_ptr opened = input_entry::g_open_from_list(candidates, what_for, nullptr, url,
                                                           nullptr, abort, &used);
        if (!opened.is_valid() || !opened->service_query_t(out)) return false;
        if (name != nullptr) {
            input_entry::ptr entry = input_entry::g_find_by_guid(used);
            *name = entry.is_valid() ? entry->get_name_() : "another component";
        }
        return true;
    } catch (const pfc::exception& error) {
        joc_log::line("decoder: no other component answers for this file's tags: %s",
                      error.what());
        return false;
    }
}

// Bytes in front of the E-AC-3 stream, which is where a tagging tool puts an
// ID3v2 tag.  The renderer refuses a stream that does not begin on a syncword and
// never resynchronises, so the walk and the feed both have to start after it.
t_filesize leading_tag_bytes(file::ptr const& source, abort_callback& abort) {
    if (!source.is_valid()) return 0;
    try {
        if (source->get_position(abort) != 0) source->seek(0, abort);
        return tag_processor::skip_id3v2(source, abort);
    } catch (const pfc::exception& error) {
        joc_log::line("decoder: cannot inspect the area in front of the stream: %s",
                      error.what());
        return 0;
    }
}

// Tags read straight from the file, for a bare stream that no other component
// claims: an ID3v2 tag in front of the syncframes, or an APEv2/ID3v1 tag behind
// them.  Neither is part of E-AC-3, so a tag that is there was written by a
// tagging tool and is worth showing.
void read_local_tags(file::ptr const& source, file_info& info, abort_callback& abort) {
    if (!source.is_valid()) return;
    bool found = false;
    try {
        source->seek(0, abort);
        tag_processor::read_id3v2(source, info, abort);
        found = true;
    } catch (const pfc::exception&) {
        // No leading tag; the trailing one is still worth a look.
    }
    try {
        tag_processor::read_trailing(source, info, abort);
        found = true;
    } catch (const pfc::exception&) {
    }
    if (found) {
        joc_log::line("decoder: %u tag field(s) read from the file itself",
                      static_cast<unsigned>(info.meta_get_count()));
    }
}

class input_joc : public input_stubs {
public:
    void open(service_ptr_t<file> hint, const char* path, t_input_open_reason reason,
              abort_callback& abort) {
        m_path = (path != nullptr) ? path : "";

        if (reason == input_open_info_write) {
            // Writing tags belongs to whoever owns the file's format, and that is
            // not this component: its inputs are two ffmpeg children and the JOC
            // renderer, none of which writes anything.  The file is deliberately
            // left unopened here, because a write-mode handle of ours would make
            // the writer that replaces it fail on a sharing violation.
            m_write_only = true;
            if (!open_forwarded(m_forward_writer, input_info_writer::class_guid, m_path.c_str(),
                                abort, &m_forward_name)) {
                throw exception_tagging_unsupported();
            }
            joc_log::line("decoder: open \"%s\" reason=2 tags: written by %s", m_path.c_str(),
                          m_forward_name.c_str());
            return;
        }

        service_ptr_t<file> source = hint;
        input_open_file_helper(source, path, reason, abort);
        m_file = source;

        // The core passes URLs ("file://C:\...").  ffmpeg and the file APIs need a
        // native path, and a URL we cannot map to one is a file we cannot decode.
        try {
            m_native_path = filesystem::g_get_native_path(m_path.c_str());
        } catch (const pfc::exception& error) {
            joc_log::line("decoder: not a native file path (%s): %s", m_path.c_str(),
                          error.what());
            throw exception_io_unsupported_format();
        }

        const char* extension = std::strrchr(m_native_path.get_ptr(), '.');
        extension = (extension != nullptr) ? extension + 1 : "";

        if (joc_container::is_container_extension(extension)) {
            open_container(extension);
        } else {
            open_bare(abort);
        }

        // Whatever else happens, the tags of this file are read by the component
        // that owns its format; failing to find one is not fatal, the technical
        // information below is still worth showing.
        if (!open_forwarded(m_forward_reader, input_info_reader::class_guid, m_path.c_str(), abort,
                            &m_forward_name)) {
            joc_log::line("decoder: no other component reads this file's tags");
        } else {
            joc_log::line("decoder: tags for \"%s\" are read by %s", m_path.c_str(),
                          m_forward_name.c_str());
        }
    }

    // A bare stream is either read from the file or handed back.  One thing has to
    // happen first: a tag area in front of the syncframes is not part of the
    // stream, and treating it as one would hand the file to the built-in decoder,
    // which plays it without the Atmos objects.
    void open_bare(abort_callback& abort) {
        m_stream_start = leading_tag_bytes(m_file, abort);
        if (m_stream_start != 0) {
            joc_log::line("decoder: %llu byte(s) of tags in front of the stream are skipped",
                          static_cast<unsigned long long>(m_stream_start));
        }

        pfc::array_t<t_uint8> buffer;
        buffer.set_size(kSniffBytes);
        const std::size_t got = m_file->read(buffer.get_ptr(), kSniffBytes, abort);
        const joc_eac3::ScanResult scan = joc_eac3::scan(buffer.get_ptr(), got, 8);

        joc_log::line("decoder: open \"%s\" reason=1 bytes=%llu frames=%llu with_joc=%llu",
                        m_path.c_str(), static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(scan.frames_examined),
                        static_cast<unsigned long long>(scan.frames_with_joc));

        if (scan.joc != joc_eac3::JocState::kYes) {
            // Hand the file to the next entry in the priority table.
            joc_log::line("decoder: yielding to the built-in decoder (%s)", scan.detail);
            throw exception_io_unsupported_format();
        }
        joc_log::line("decoder: claiming this file as E-AC-3 JOC (bare stream)");
        m_input_kind = joc_decode::InputKind::kBare;
    }

    // Two questions, in this order: is there an E-AC-3 track inside (answered from
    // the container's own headers, so a library scan pays nothing for the MP4s that
    // hold AAC), and does that track carry JOC (answered from a copied prefix of
    // the track).  Either "no" hands the file to the next decoder in the table.
    void open_container(const char* extension) {
        m_container = joc_container::scan(m_native_path.get_ptr());
        if (!m_container.eac3) {
            joc_log::line("decoder: yielding to the built-in decoder (%s)",
                          m_container.detail.c_str());
            throw exception_io_unsupported_format();
        }

        const joc_decode::Settings settings = joc_settings::current();
        bool joc = false;
        std::string detail;
        if (!joc_decode::probe_container_joc(settings.ffmpeg_path, m_native_path.get_ptr(),
                                            m_container.audio_index, &joc, &detail)) {
            joc_log::line("decoder: cannot examine the E-AC-3 track (%s); yielding",
                          detail.c_str());
            throw exception_io_unsupported_format();
        }
        if (!joc) {
            joc_log::line("decoder: yielding to the built-in decoder (E-AC-3 track %u carries "
                          "no JOC: %s)",
                          m_container.audio_index, detail.c_str());
            throw exception_io_unsupported_format();
        }

        joc_log::line("decoder: claiming this file as E-AC-3 JOC (%s, audio track %u, .%s)",
                      joc_container::kind_name(m_container.kind), m_container.audio_index,
                      extension);
        m_input_kind = joc_decode::InputKind::kContainer;
        m_audio_index = m_container.audio_index;
    }

    void get_info(file_info& info, abort_callback& abort) {
        if (m_write_only) {
            // An instance opened to write tags is the writer's reader: what it
            // reports is exactly what the caller has just written.
            if (m_forward_writer.is_valid()) {
                m_forward_writer->get_info(0, info, abort);
                return;
            }
            throw exception_tagging_unsupported();
        }

        const joc_decode::FileProbe probe =
            joc_decode::probe_file(m_native_path.get_ptr(), 0, m_stream_start);
        const joc_decode::Settings settings = joc_settings::current();

        // A container knows its own duration even though the E-AC-3 syncframes are
        // not directly addressable in the file.
        const bool container = (m_input_kind == joc_decode::InputKind::kContainer);
        const double duration = (container && m_container.duration_seconds > 0.0)
                                    ? m_container.duration_seconds
                                    : probe.duration_seconds;
        const std::uint64_t frames =
            probe.frames != 0
                ? probe.frames
                : ((duration > 0.0) ? static_cast<std::uint64_t>(duration * 48000.0 / 1536.0) : 0);

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
        info.set_length(duration);
        m_length = duration;
        // The renderer keeps its room tail, but the stream this component hands over
        // ends where the file ends: the tail is rendering, not playback time.
        m_engine.set_length(duration);
        if (duration > 0.0) {
            const t_filesize bytes = m_file.is_valid() ? m_file->get_size(abort) : filesize_invalid;
            if (bytes != filesize_invalid && bytes > 0) {
                info.info_set_bitrate(static_cast<t_int64>(
                    static_cast<double>(bytes) * 8.0 / duration / 1000.0));
            }
        }
        // Custom fields: visible in Properties and usable as %joc_*% in title
        // formatting, which is how a user can tell a JOC file from a plain one.
        info.info_set("joc_render", render.c_str());
        info.info_set("joc_hrtf", settings.hrtf_file.empty()
                                      ? "(未设置)"
                                      : file_name_of(settings.hrtf_file).c_str());
        info.info_set_int("joc_frames", static_cast<t_int64>(frames));
        info.info_set("joc_scan", container ? m_container.detail.c_str() : probe.detail.c_str());
        if (container) {
            char text[128] = {};
            std::snprintf(text, sizeof(text), "%s (%s, audio track %u)",
                          joc_container::kind_name(m_container.kind),
                          m_container.codec.empty() ? "E-AC-3" : m_container.codec.c_str(),
                          m_container.audio_index);
            info.info_set("joc_container", text);
        }

        // The file's own reader supplies the tags; nothing above this line is one.
        // Only the metadata is taken over -- its technical information (E-AC-3,
        // 6 channels, the stream's own bitrate) would replace this component's,
        // which is the part that says whether the file is JOC.
        bool have_tags = false;
        if (m_forward_reader.is_valid()) {
            try {
                file_info_impl tags;
                m_forward_reader->get_info(0, tags, abort);
                info.copy_meta(tags);
                have_tags = tags.meta_get_count() != 0;
                joc_log::line("decoder: %u tag field(s) from %s",
                              static_cast<unsigned>(tags.meta_get_count()),
                              m_forward_name.c_str());
            } catch (const pfc::exception& error) {
                joc_log::line("decoder: reading this file's own tags failed: %s", error.what());
            }
        }
        // A reader that answers for the format but has nothing to say about a bare
        // stream is common -- ffmpeg's AC-3 decoder reads no tags at all -- while
        // the file may still carry an ID3v2 or APEv2 tag a tagging tool wrote.
        if (!have_tags && m_input_kind == joc_decode::InputKind::kBare) {
            read_local_tags(m_file, info, abort);
        }

        joc_log::line("decoder: get_info duration=%.3f s frames=%llu channels=%u render=%s%s",
                      duration, static_cast<unsigned long long>(frames), channels, render.c_str(),
                      container ? " (container)" : "");
    }

    t_filestats2 get_stats2(uint32_t flags, abort_callback& abort) {
        if (m_file.is_valid()) return m_file->get_stats2_(flags, abort);
        if (m_forward_writer.is_valid()) return m_forward_writer->get_stats2_(nullptr, flags, abort);
        throw exception_io_unsupported_format();
    }

    void decode_initialize(unsigned flags, abort_callback& abort) {
        (void)abort;
        m_settings = joc_settings::current();
        m_settings.input_kind = m_input_kind;
        m_settings.audio_index = m_audio_index;
        m_settings.stream_start_bytes = m_stream_start;
        m_settings.length_seconds = m_length;
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
        m_delivery_mismatches = 0;
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
            joc_log::line("decoder: end of stream after %llu frames%s",
                            static_cast<unsigned long long>(m_frames_delivered),
                            m_delivery_mismatches == 0 ? ""
                                                       : " (the delivery changed samples)");
            return false;
        }

        // The renderer produces float32 and a chunk holds audio_sample, which is float on
        // 32-bit builds and double on 64-bit ones (SDK audio_math.h): the samples are
        // converted, not copied.  set_data_32() is the SDK's conversion for a float32
        // source, and it sets the channel count, the sample rate and the sample count.
        chunk.set_data_32(m_buffer.data(), frames, m_channels, kSampleRate);

        m_frames_delivered += frames;
        // A delivery that mangled the samples would be heard as noise rather than reported
        // as a failure, so every chunk is checked: the conversion is exact, and the peak of
        // what the chunk holds has to equal the peak of what the renderer produced.
        const double produced = peak_of(m_buffer.data(), frames * m_channels);
        const double delivered =
            peak_of(chunk.get_data(), chunk.get_sample_count() * chunk.get_channels());
        if (delivered > produced + 1e-6 + produced * 1e-6 ||
            delivered < produced - 1e-6 - produced * 1e-6) {
            ++m_delivery_mismatches;
            if (m_delivery_mismatches == 1) {
                joc_log::line("decoder: delivery changed the samples: peak %.9f produced, "
                                "%.9f delivered",
                                produced, delivered);
            }
        }
        if (!m_reported) {
            m_reported = true;
            joc_log::line("decoder: first %llu frames delivered (%u ch), peak %.6f, "
                            "delivered peak %.6f",
                            static_cast<unsigned long long>(frames), m_channels, produced,
                            delivered);
        }
        return true;
    }

    void decode_seek(double seconds, abort_callback& abort) {
        // Walking the syncframe index of a long file is the only part of a seek
        // that can take a while, and it polls this.
        m_engine.set_abort_check([&abort] { return !abort.is_aborting(); });
        std::string error;
        const bool ok = m_engine.seek(seconds, m_length, &error);
        m_engine.set_abort_check(nullptr);
        if (!ok) {
            // An aborted seek reports itself as an abort, not as a decode failure.
            abort.check();
            joc_log::line("decoder: seek to %.6f s failed: %s", seconds, error.c_str());
            throw exception_io_data(error.c_str());
        }
        // The position reporting and the first-read statistics belong to the run
        // that starts here, not to the one that was interrupted.
        m_frames_delivered = 0;
        m_reported = false;
        m_delivery_mismatches = 0;
        joc_log::line("decoder: seek to %.6f s, the next read starts at the target", seconds);
    }

    bool decode_can_seek() { return true; }

    size_t extended_param(const GUID& type, size_t arg1, void* arg2, size_t arg2size) {
        (void)arg1;
        (void)arg2;
        (void)arg2size;
        // A seek restarts both ffmpeg children and replays the renderer's warm-up,
        // so it is worth avoiding the ones the core would only make speculatively.
        if (type == input_params::seeking_expensive) return 1;
        return 0;
    }

    void retag(const file_info& info, abort_callback& abort) {
        if (!m_forward_writer.is_valid()) throw exception_tagging_unsupported();
        // A single-track input has no commit() of its own -- the SDK wrapper
        // implements it as a no-op -- so the writer's commit has to happen here or
        // nothing reaches the file.
        m_forward_writer->set_info(0, info, abort);
        m_forward_writer->commit(abort);
        joc_log::line("decoder: %u tag field(s) written through %s",
                      static_cast<unsigned>(info.meta_get_count()), m_forward_name.c_str());
    }

    void remove_tags(abort_callback& abort) {
        if (!m_forward_writer.is_valid()) throw exception_tagging_unsupported();
        input_info_writer_v2::ptr v2;
        if (m_forward_writer->service_query_t(v2)) {
            v2->remove_tags(abort);
            return;
        }
        m_forward_writer->remove_tags_fallback(abort);
    }

    static bool g_is_our_path(const char* path, const char* extension) {
        (void)path;
        // Claim by extension, then decide from the contents in open(): a file whose
        // audio turns out not to be E-AC-3 JOC is handed back with
        // exception_io_unsupported_format, and the core moves on to the next decoder
        // in its priority table.  Containers are included because an E-AC-3 JOC
        // track is commonly wrapped in MP4 or Matroska; the container walk in
        // open() is what keeps the other files cheap to decline.
        if (joc_container::is_container_extension(extension)) return true;
        return (extension != nullptr) &&
               ((stricmp_utf8(extension, "eac3") == 0) || (stricmp_utf8(extension, "ec3") == 0));
    }

    static bool g_is_our_content_type(const char* content_type) {
        if (content_type == nullptr) return false;
        // E-AC-3 content types, and only the E-AC-3 ones: the generic Dolby aliases
        // are deliberately left alone, because a track the core can already decode
        // must not end up claimed by an entry that will not decode it.
        static const char* const kTypes[] = {"audio/eac3", "audio/eac3joc", "audio/ec3",
                                            "audio/x-eac3", "E-AC-3",      "eac3",
                                            "ec3"};
        for (const char* candidate : kTypes) {
            if (stricmp_utf8(content_type, candidate) == 0) return true;
        }
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
    // Set by open(): a bare stream is read directly, an E-AC-3 track inside a
    // container is extracted by ffmpeg before it reaches the renderer.
    joc_decode::InputKind m_input_kind = joc_decode::InputKind::kBare;
    unsigned m_audio_index = 0;
    joc_container::Result m_container;
    std::vector<float> m_buffer;
    unsigned m_channels = 2;
    std::uint64_t m_frames_delivered = 0;
    bool m_reported = false;
    // Chunks whose delivered samples did not match what the renderer produced.
    std::uint64_t m_delivery_mismatches = 0;
    // Duration get_info() last reported; a seek needs it to tell "past the end"
    // from "inside the file" without decoding anything.
    double m_length = 0.0;
    // Bytes in front of a bare stream, which is where an ID3v2 tag sits.
    std::uint64_t m_stream_start = 0;
    // The other component that answers for this file's tags, and the one that
    // writes them.  Only the writer exists on an instance opened to retag.
    service_ptr_t<input_info_reader> m_forward_reader;
    service_ptr_t<input_info_writer> m_forward_writer;
    pfc::string8 m_forward_name;
    bool m_write_only = false;
};

static input_singletrack_factory_t<input_joc> g_input_joc_factory;

}  // namespace

DECLARE_FILE_TYPE_EX("eac3;ec3", "E-AC-3 JOC file", "E-AC-3 JOC files");
