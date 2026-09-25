#include "container_scan.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

namespace joc_container {
namespace {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

// A bounded, read-only view of the file.  Every accessor returns false instead of
// throwing, so a truncated or hostile file simply fails the probe.
class Window {
public:
    bool open(const std::string& path) {
        const std::wstring wide = utf8_to_wide(path);
        handle_ = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            return false;
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(handle_, &size) == FALSE) return false;
        size_ = static_cast<std::uint64_t>(size.QuadPart);
        return true;
    }

    ~Window() {
        if (handle_ != nullptr) CloseHandle(handle_);
    }

    std::uint64_t size() const { return size_; }

    // Reads at an absolute offset; false when the range is not fully available.
    bool read(std::uint64_t offset, void* destination, std::size_t bytes) const {
        if (offset + bytes > size_) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == FALSE) return false;
        std::size_t done = 0;
        auto* target = static_cast<std::uint8_t*>(destination);
        while (done < bytes) {
            const DWORD chunk = static_cast<DWORD>(
                (bytes - done) > 0x10000u ? 0x10000u : (bytes - done));
            DWORD got = 0;
            if (ReadFile(handle_, target + done, chunk, &got, nullptr) == FALSE || got == 0) {
                return false;
            }
            done += got;
        }
        return true;
    }

    bool load(std::uint64_t offset, std::size_t bytes, std::vector<std::uint8_t>* out) const {
        out->assign(bytes, 0);
        return bytes == 0 || read(offset, out->data(), bytes);
    }

private:
    HANDLE handle_ = nullptr;
    std::uint64_t size_ = 0;
};

// Set JOC_SCAN_TRACE=1 to have the walk printed: the practical way to see where a
// container's structure stops matching what the probe expects.
bool trace_enabled() {
    static const bool enabled = [] {
        return GetEnvironmentVariableA("JOC_SCAN_TRACE", nullptr, 0) != 0;
    }();
    return enabled;
}

void trace(const char* format, ...) {
    if (!trace_enabled()) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    std::fflush(stdout);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::string fourcc(const std::uint8_t* p) { return std::string(reinterpret_cast<const char*>(p), 4); }

bool is_audio_sample_entry(const std::string& format) {
    // E-AC-3 is "ec-3"; "ac-3" is plain Dolby Digital and is *not* ours.
    return format == "ec-3" || format == "ec3 " || format == "EAC3";
}

bool is_ac3_sample_entry(const std::string& format) { return format == "ac-3" || format == "ac3 "; }

// --- MP4 / ISO base media -------------------------------------------------

// Walks the boxes inside [begin, end) and hands each to the visitor.
template <typename Fn>
bool walk_boxes(const Window& file, std::uint64_t begin, std::uint64_t end,
                std::size_t depth, Fn visitor) {
    std::uint64_t offset = begin;
    std::uint8_t header[8];
    while (offset + 8 <= end) {
        if (!file.read(offset, header, sizeof(header))) return false;
        std::uint64_t size = be32(header);
        const std::string type = fourcc(header + 4);
        std::uint64_t payload = offset + 8;
        if (size == 1) {
            std::uint8_t extended[8];
            if (!file.read(offset + 8, extended, sizeof(extended))) return false;
            size = (static_cast<std::uint64_t>(be32(extended)) << 32) | be32(extended + 4);
            payload = offset + 16;
        } else if (size == 0) {
            size = end - offset;  // "to the end of the file"
        }
        trace("%*sbox %s size=%llu\n", static_cast<int>(depth) * 2, "", type.c_str(), static_cast<unsigned long long>(size));
        if (size < 8 || offset + size > end) return false;
        if (!visitor(type, payload, offset + size, depth)) return false;
        offset += size;
    }
    return true;
}

// One trak: is it an audio track, and which sample entry does it use?
struct TrackInfo {
    bool audio = false;
    std::string format;
};

bool scan_stbl(const Window& file, std::uint64_t begin, std::uint64_t end, TrackInfo* info) {
    return walk_boxes(file, begin, end, 0, [&](const std::string& type, std::uint64_t payload,
                                                std::uint64_t box_end, std::size_t) {
        (void)box_end;
        if (type == "stsd") {
            std::uint8_t head[16];
            if (file.read(payload, head, sizeof(head))) {
                // version+flags(4) entry_count(4), then the first sample entry:
                // size(4) format(4).
                info->format = fourcc(head + 12);
                trace("%*sstsd format=%s\n", 8, "", info->format.c_str());
            }
            return false;  // one sample entry is all the probe needs
        }
        return true;
    });
}

// minf -> stbl -> stsd
bool scan_minf(const Window& file, std::uint64_t begin, std::uint64_t end, TrackInfo* info) {
    return walk_boxes(file, begin, end, 0, [&](const std::string& type, std::uint64_t payload,
                                                std::uint64_t box_end, std::size_t) {
        if (type == "stbl") scan_stbl(file, payload, box_end, info);
        return true;
    });
}

// mdia: the track handler says whether this is audio, minf leads to the sample entry.
bool scan_mdia(const Window& file, std::uint64_t begin, std::uint64_t end, TrackInfo* info) {
    return walk_boxes(file, begin, end, 0, [&](const std::string& type, std::uint64_t payload,
                                                std::uint64_t box_end, std::size_t) {
        if (type == "hdlr") {
            std::uint8_t head[12];
            if (file.read(payload, head, sizeof(head))) {
                info->audio = fourcc(head + 8) == "soun";
                trace("%*shdlr handler=%s audio=%d\n", 6, "", fourcc(head + 8).c_str(),
                      info->audio ? 1 : 0);
            }
        } else if (type == "minf") {
            scan_minf(file, payload, box_end, info);
        }
        return true;
    });
}

// trak -> mdia
bool scan_trak(const Window& file, std::uint64_t begin, std::uint64_t end, TrackInfo* info) {
    return walk_boxes(file, begin, end, 0, [&](const std::string& type, std::uint64_t payload,
                                                std::uint64_t box_end, std::size_t) {
        if (type == "mdia") scan_mdia(file, payload, box_end, info);
        return true;
    });
}
Result scan_mp4(const Window& file, std::size_t max_bytes) {
    Result result;
    result.kind = Kind::kMp4;

    // moov is usually at the start for streamed files and at the end otherwise.
    struct Range {
        std::uint64_t begin;
        std::uint64_t end;
    };
    std::vector<Range> ranges{{0, (std::min<std::uint64_t>)(file.size(), max_bytes)}};
    if (file.size() > max_bytes) {
        ranges.push_back({file.size() - max_bytes, file.size()});
    }

    unsigned audio_seen = 0;
    bool found_any_audio = false;
    std::string first_audio_format;
    for (const Range& range : ranges) {
        walk_boxes(file, range.begin, range.end, 0,
                   [&](const std::string& type, std::uint64_t payload, std::uint64_t box_end,
                       std::size_t) {
                       if (type != "moov") return true;
                       walk_boxes(file, payload, box_end, 1,
                                  [&](const std::string& inner, std::uint64_t inner_payload,
                                      std::uint64_t inner_end, std::size_t) {
                                      if (inner == "mvhd") {
                                          // version(1) flags(3) then either
                                          // creation/modification/timescale/duration (32-bit)
                                          // or the 64-bit variant.
                                          std::uint8_t header[32];
                                          if (file.read(inner_payload, header, sizeof(header))) {
                                              const bool wide = header[0] == 1;
                                              const std::uint32_t timescale =
                                                  wide ? be32(header + 20) : be32(header + 12);
                                              const std::uint64_t duration =
                                                  wide ? ((static_cast<std::uint64_t>(
                                                               be32(header + 24))
                                                           << 32) |
                                                          be32(header + 28))
                                                       : be32(header + 16);
                                              if (timescale != 0) {
                                                  result.duration_seconds =
                                                      static_cast<double>(duration) /
                                                      static_cast<double>(timescale);
                                              }
                                          }
                                          return true;
                                      }
                                      if (inner != "trak") return true;
                                      TrackInfo info;
                                      scan_trak(file, inner_payload, inner_end, &info);
                                      if (!info.audio) return true;
                                      if (!found_any_audio) {
                                          found_any_audio = true;
                                          first_audio_format = info.format;
                                      }
                                      if (is_audio_sample_entry(info.format)) {
                                          result.eac3 = true;
                                          result.audio_index = audio_seen;
                                          result.codec = info.format;
                                      } else if (is_ac3_sample_entry(info.format)) {
                                          result.codec = info.format;
                                      }
                                      ++audio_seen;
                                      return !result.eac3;  // stop once found
                                  });
                       return !result.eac3;
                   });
        if (result.eac3) break;
    }

    if (result.eac3) {
        result.detail = "mp4: E-AC-3 audio track " + std::to_string(result.audio_index);
    } else if (found_any_audio) {
        result.detail = "mp4: first audio track is " +
                        (first_audio_format.empty() ? std::string("unknown")
                                                    : first_audio_format);
    } else {
        result.detail = "mp4: no audio track found in the scanned window";
    }
    return result;
}

// --- Matroska / WebM ------------------------------------------------------

struct EbmlReader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t position = 0;

    bool at_end() const { return position >= size; }

    // Element IDs keep their leading bits; sizes drop the marker bit.
    bool read_id(std::uint64_t* id) {
        if (at_end()) return false;
        const std::uint8_t first = data[position];
        int length = 1;
        for (int bit = 0x80; bit != 0; bit >>= 1, ++length) {
            if ((first & bit) != 0) break;
        }
        if (length > 4 || position + static_cast<std::size_t>(length) > size) return false;
        std::uint64_t value = 0;
        for (int i = 0; i < length; ++i) value = (value << 8) | data[position + i];
        position += static_cast<std::size_t>(length);
        *id = value;
        return true;
    }

    bool read_size(std::uint64_t* value, bool* unknown) {
        *unknown = false;
        if (at_end()) return false;
        const std::uint8_t first = data[position];
        int length = 1;
        for (int bit = 0x80; bit != 0; bit >>= 1, ++length) {
            if ((first & bit) != 0) break;
        }
        if (length > 8 || position + static_cast<std::size_t>(length) > size) return false;
        std::uint64_t result = first & (0xFFu >> length);
        bool all_ones = (first & (0xFFu >> length)) == (0xFFu >> length);
        for (int i = 1; i < length; ++i) {
            const std::uint8_t byte = data[position + i];
            all_ones = all_ones && byte == 0xFFu;
            result = (result << 8) | byte;
        }
        position += static_cast<std::size_t>(length);
        *unknown = all_ones;
        *value = result;
        return true;
    }
};

Result scan_matroska(const Window& file, std::size_t max_bytes) {
    Result result;
    result.kind = Kind::kMatroska;

    std::vector<std::uint8_t> data;
    if (!file.load(0, static_cast<std::size_t>(
                         (std::min<std::uint64_t>)(file.size(), max_bytes)),
                   &data)) {
        result.detail = "matroska: cannot read the header";
        return result;
    }

    unsigned audio_seen = 0;
    std::string first_audio_codec;
    bool saw_tracks = false;
    bool found = false;
    double timestamp_scale_ns = 1000000.0;  // Matroska default: 1 ms per tick
    double duration_ticks = 0.0;

    // Only the two levels that matter are walked: Tracks, then TrackEntry.
    auto walk = [&](std::size_t begin, std::size_t end, int depth,
                    auto&& self) -> void {
        EbmlReader reader{data.data(), end, begin};
        while (!reader.at_end() && !found) {
            const std::size_t element_start = reader.position;
            std::uint64_t id = 0;
            std::uint64_t size = 0;
            bool unknown = false;
            if (!reader.read_id(&id) || !reader.read_size(&size, &unknown)) break;
            std::size_t payload = reader.position;
            std::size_t payload_end =
                unknown ? end : (std::min<std::size_t>)(end, payload + static_cast<std::size_t>(size));
            if (payload_end < payload) break;

            switch (id) {
                case 0x1654AE6Bu:  // Tracks
                    saw_tracks = true;
                    self(payload, payload_end, depth + 1, self);
                    break;
                case 0x1549A966u: {  // Info: carries the segment duration
                    EbmlReader inner{data.data(), payload_end, payload};
                    while (!inner.at_end()) {
                        std::uint64_t child_id = 0;
                        std::uint64_t child_size = 0;
                        bool child_unknown = false;
                        if (!inner.read_id(&child_id) ||
                            !inner.read_size(&child_size, &child_unknown)) {
                            break;
                        }
                        const std::size_t child_payload = inner.position;
                        const std::size_t child_end =
                            child_unknown ? payload_end
                                          : (std::min<std::size_t>)(
                                                payload_end,
                                                child_payload + static_cast<std::size_t>(child_size));
                        // TimestampScale (ns per tick, default 1 ms) and Duration (ticks).
                        if (child_id == 0x2AD7B1u && child_size >= 1 && child_size <= 8) {
                            std::uint64_t scale = 0;
                            for (std::size_t i = 0; i < child_size; ++i) {
                                scale = (scale << 8) | data[child_payload + i];
                            }
                            if (scale != 0) timestamp_scale_ns = scale;
                        } else if (child_id == 0x4489u && (child_size == 4 || child_size == 8)) {
                            // Duration is a big-endian float: 4 or 8 bytes, both occur.
                            const std::uint8_t* field = data.data() + child_payload;
                            if (child_size == 4) {
                                const std::uint32_t bits = be32(field);
                                float value = 0.0f;
                                std::memcpy(&value, &bits, sizeof(value));
                                duration_ticks = static_cast<double>(value);
                            } else {
                                const std::uint64_t bits =
                                    (static_cast<std::uint64_t>(be32(field)) << 32) | be32(field + 4);
                                double value = 0.0;
                                std::memcpy(&value, &bits, sizeof(value));
                                duration_ticks = value;
                            }
                        }
                        inner.position = child_end;
                    }
                    break;
                }
                case 0xAEu: {  // TrackEntry
                    // Read this track's type and codec.
                    std::uint64_t track_type = 0;
                    std::string codec;
                    EbmlReader inner{data.data(), payload_end, payload};
                    while (!inner.at_end()) {
                        std::uint64_t child_id = 0;
                        std::uint64_t child_size = 0;
                        bool child_unknown = false;
                        if (!inner.read_id(&child_id) ||
                            !inner.read_size(&child_size, &child_unknown)) {
                            break;
                        }
                        const std::size_t child_payload = inner.position;
                        const std::size_t child_end =
                            child_unknown ? payload_end
                                          : (std::min<std::size_t>)(
                                                payload_end,
                                                child_payload + static_cast<std::size_t>(child_size));
                        if (child_id == 0x83u && child_size >= 1) {  // TrackType
                            track_type = data[child_payload];
                        } else if (child_id == 0x86u) {  // CodecID
                            codec.assign(reinterpret_cast<const char*>(data.data() + child_payload),
                                         child_end - child_payload);
                        }
                        inner.position = child_end;
                    }
                    if (track_type == 2u) {  // audio
                        if (first_audio_codec.empty()) first_audio_codec = codec;
                        if (codec == "A_EAC3") {
                            result.eac3 = true;
                            result.audio_index = audio_seen;
                            result.codec = codec;
                            found = true;
                        }
                        ++audio_seen;
                    }
                    break;
                }
                case 0x18538067u:  // Segment: descend, the size may be unknown
                    self(payload, payload_end, depth + 1, self);
                    break;
                default:
                    break;
            }
            if (found) break;
            reader.position = payload_end;
            if (payload_end == element_start) break;  // no progress: stop
        }
    };
    walk(0, data.size(), 0, walk);

    if (duration_ticks > 0.0 && timestamp_scale_ns > 0.0) {
        result.duration_seconds = duration_ticks * timestamp_scale_ns / 1.0e9;
    }
    if (result.eac3) {
        result.detail = "matroska: E-AC-3 audio track " + std::to_string(result.audio_index);
    } else if (saw_tracks) {
        result.detail = "matroska: first audio track is " +
                        (first_audio_codec.empty() ? std::string("unknown") : first_audio_codec);
    } else {
        result.detail = "matroska: no track list in the scanned window";
    }
    return result;
}

}  // namespace

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::kMp4: return "mp4";
        case Kind::kMatroska: return "matroska";
        default: return "none";
    }
}

bool is_container_extension(const char* extension) {
    if (extension == nullptr) return false;
    static const char* const kExtensions[] = {"mp4", "m4a",  "m4b", "m4p", "m4r",
                                              "mov", "mkv",  "mka", "webm"};
    for (const char* candidate : kExtensions) {
        if (_stricmp(extension, candidate) == 0) return true;
    }
    return false;
}

Result scan(const std::string& path, std::size_t max_bytes) {
    Result result;
    Window file;
    if (!file.open(path)) {
        result.detail = "cannot open the file";
        return result;
    }
    if (file.size() < 16) {
        result.detail = "file too small to be a container";
        return result;
    }

    std::uint8_t head[16];
    if (!file.read(0, head, sizeof(head))) {
        result.detail = "cannot read the file header";
        return result;
    }
    // Matroska starts with the EBML header element (0x1A45DFA3); ISO base media
    // starts with a box whose type is at offset 4 ("ftyp" for a normal file).
    if (head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) {
        result = scan_matroska(file, max_bytes);
    } else {
        result = scan_mp4(file, max_bytes);
    }
    joc_log::line("container: %s -> %s (eac3=%d audio#%u codec=%s)", path.c_str(),
                  result.detail.c_str(), result.eac3 ? 1 : 0, result.audio_index,
                  result.codec.empty() ? "-" : result.codec.c_str());
    return result;
}

}  // namespace joc_container