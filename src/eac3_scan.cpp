#include "eac3_scan.h"

#include <cstring>

namespace joc_eac3 {
namespace {

constexpr std::uint16_t kEac3Syncword = 0x0B77;
constexpr std::uint16_t kEmdfSyncword = 0x5838;
constexpr std::uint8_t kIdOamd = 11;
constexpr std::uint8_t kIdJoc = 14;
constexpr std::size_t kMaxPayloads = 16;

// MSB-first bit reader with an explicit limit, matching the core's reader
// semantics (reads past the limit fail rather than returning zeros).
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t limit_bits, std::size_t start_bit)
        : data_(data), limit_(limit_bits), position_(start_bit) {}

    std::uint32_t read(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (position_ >= limit_) {
                failed_ = true;
                return 0;
            }
            const unsigned byte = data_[position_ >> 3];
            value = (value << 1) | ((byte >> (7u - (position_ & 7u))) & 1u);
            ++position_;
        }
        return value;
    }

    bool skip(std::size_t count) {
        if (position_ + count > limit_) {
            failed_ = true;
            return false;
        }
        position_ += count;
        return true;
    }

    // value, then a continuation bit; an unterminated chain is a syntax error.
    bool variable_bits(unsigned width, unsigned max_groups, std::uint32_t* out) {
        std::uint32_t value = 0;
        for (unsigned group = 0; group < max_groups; ++group) {
            value += read(width);
            if (failed_) return false;
            const std::uint32_t more = read(1);
            if (failed_) return false;
            if (more == 0u) {
                if (out != nullptr) *out = value;
                return true;
            }
            value = (value + 1u) << width;
        }
        failed_ = true;
        return false;
    }

    std::size_t position() const { return position_; }
    bool failed() const { return failed_; }
    void set_failed() { failed_ = true; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t limit_ = 0;
    std::size_t position_ = 0;
    bool failed_ = false;
};

struct Payload {
    std::uint8_t id = 0;
    std::size_t bit_offset = 0;
    std::size_t size = 0;
};

struct Container {
    std::size_t start_bit = 0;
    std::size_t raw_size = 0;
    std::size_t payload_count = 0;
    Payload payloads[kMaxPayloads] = {};

    bool has(std::uint8_t id) const {
        for (std::size_t i = 0; i < payload_count; ++i) {
            if (payloads[i].id == id) return true;
        }
        return false;
    }
};

bool parse_container(const std::uint8_t* data, std::size_t size_bits, std::size_t start_bit,
                     Container* out) {
    if (start_bit + 16u > size_bits) return false;
    BitReader reader(data, size_bits, start_bit);
    if (reader.read(16) != kEmdfSyncword) return false;

    const std::uint32_t length = reader.read(16);
    const std::size_t body_start = reader.position();
    const std::size_t body_end = body_start + static_cast<std::size_t>(length) * 8u;
    if (reader.failed() || body_end > size_bits) return false;

    BitReader body(data, body_end, body_start);
    std::uint32_t version = body.read(2);
    if (body.failed()) return false;
    if (version == 3u) {
        std::uint32_t extra = 0;
        if (!body.variable_bits(2, 8, &extra)) return false;
        version += extra;
    }
    std::uint32_t key_id = body.read(3);
    if (body.failed()) return false;
    if (key_id == 7u) {
        std::uint32_t extra = 0;
        if (!body.variable_bits(3, 8, &extra)) return false;
        key_id += extra;
    }
    // TS 103 420 JOC is version 0 / key_id 0; the strict check also rejects
    // false 0x5838 markers that happen to sit inside audio data.
    if (version != 0u || key_id != 0u) return false;

    Container container;
    container.start_bit = start_bit;
    bool terminated = false;
    while (body.position() + 5u <= body_end) {
        std::uint32_t payload_id = body.read(5);
        if (body.failed()) return false;
        if (payload_id == 0u) {
            terminated = true;
            break;
        }
        if (payload_id == 0x1Fu) {
            std::uint32_t extra = 0;
            if (!body.variable_bits(5, 8, &extra)) return false;
            payload_id += extra;
        }
        for (std::size_t i = 0; i < container.payload_count; ++i) {
            if (container.payloads[i].id == static_cast<std::uint8_t>(payload_id)) return false;
        }
        if (container.payload_count >= kMaxPayloads) return false;

        const std::uint32_t has_sample_offset = body.read(1);
        if (body.failed()) return false;
        if (has_sample_offset != 0u) {
            (void)body.read(12);
        }
        if (body.read(1) != 0u) {
            std::uint32_t ignored = 0;
            if (!body.variable_bits(11, 8, &ignored)) return false;
        }
        if (body.read(1) != 0u) {
            std::uint32_t ignored = 0;
            if (!body.variable_bits(2, 8, &ignored)) return false;
        }
        if (body.read(1) != 0u) {
            if (!body.skip(8)) return false;
        }
        if (body.failed()) return false;
        if (body.read(1) == 0u) {
            bool frame_aligned = false;
            if (has_sample_offset == 0u) {
                frame_aligned = body.read(1) != 0u;
                if (frame_aligned && !body.skip(2)) return false;
            }
            if ((has_sample_offset != 0u || frame_aligned) && !body.skip(7)) return false;
        }
        if (body.failed()) return false;

        std::uint32_t payload_size = 0;
        if (!body.variable_bits(8, 8, &payload_size)) return false;
        const std::size_t payload_bits = static_cast<std::size_t>(payload_size) * 8u;
        if (body.position() + payload_bits > body_end) return false;

        Payload& entry = container.payloads[container.payload_count++];
        entry.id = static_cast<std::uint8_t>(payload_id);
        entry.bit_offset = body.position();
        entry.size = payload_size;
        if (!body.skip(payload_bits)) return false;
    }
    if (!terminated) return false;
    container.raw_size = 4u + static_cast<std::size_t>(length);
    *out = container;
    return true;
}

// Candidate 0x5838 positions over the eight bit alignments, ascending.
void marker_offsets(const std::uint8_t* data, std::size_t size, std::size_t* out,
                    std::size_t capacity, std::size_t* count) {
    *count = 0;
    if (size < 4u) return;
    for (std::size_t shift = 0; shift < 8u; ++shift) {
        const std::size_t aligned_len = (shift == 0u) ? size : (size - 1u);
        for (std::size_t i = 0; i + 1u < aligned_len; ++i) {
            std::uint8_t first = 0;
            std::uint8_t second = 0;
            if (shift == 0u) {
                first = data[i];
                second = data[i + 1u];
            } else {
                first = static_cast<std::uint8_t>(
                    ((static_cast<std::uint16_t>(data[i]) << shift) |
                     (static_cast<std::uint16_t>(data[i + 1u]) >> (8u - shift))) &
                    0xFFu);
                second = static_cast<std::uint8_t>(
                    ((static_cast<std::uint16_t>(data[i + 1u]) << shift) |
                     (static_cast<std::uint16_t>(data[i + 2u]) >> (8u - shift))) &
                    0xFFu);
            }
            if (first == 0x58u && second == 0x38u) {
                if (*count < capacity) out[(*count)++] = i * 8u + shift;
            }
        }
    }
}

bool contains_payload(const std::uint8_t* frame, std::size_t size, std::uint8_t wanted,
                      std::size_t* first_bit) {
    for (std::size_t at = 0; at + 1u < size; ++at) {
        if (frame[at] != 0x58u || frame[at + 1u] != 0x38u) continue;
        Container container;
        if (parse_container(frame, size * 8u, at * 8u, &container) && container.has(wanted)) {
            if (first_bit != nullptr) *first_bit = at * 8u;
            return true;
        }
    }
    for (std::size_t shift = 1; shift < 8u; ++shift) {
        const std::size_t aligned_len = size - 1u;
        for (std::size_t i = 0; i + 1u < aligned_len; ++i) {
            const std::uint8_t first = static_cast<std::uint8_t>(
                ((static_cast<std::uint16_t>(frame[i]) << shift) |
                 (static_cast<std::uint16_t>(frame[i + 1u]) >> (8u - shift))) &
                0xFFu);
            const std::uint8_t second = static_cast<std::uint8_t>(
                ((static_cast<std::uint16_t>(frame[i + 1u]) << shift) |
                 (static_cast<std::uint16_t>(frame[i + 2u]) >> (8u - shift))) &
                0xFFu);
            if (first != 0x58u || second != 0x38u) continue;
            Container container;
            if (parse_container(frame, size * 8u, i * 8u + shift, &container) &&
                container.has(wanted)) {
                if (first_bit != nullptr) *first_bit = i * 8u + shift;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

namespace {

std::uint32_t read_bits(const std::uint8_t* data, std::size_t start, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t index = start; index < start + count; ++index) {
        value = (value << 1) | ((data[index >> 3] >> (7u - (index & 7u))) & 1u);
    }
    return value;
}

// AC-3 syncframe length in 16-bit words, indexed by fscod then frmsizecod.
const std::uint16_t kAc3FrameWords[3][38] = {
    {64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224,
     256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768, 768,
     896, 896, 1024, 1024, 1152, 1152, 1280, 1280},
    {69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243, 244,
     278, 278, 348, 348, 417, 418, 487, 487, 557, 557, 696, 696, 835, 835,
     975, 975, 1114, 1114, 1253, 1253, 1393, 1393},
    {96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336, 336,
     384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960, 1152, 1152,
     1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920},
};

}  // namespace

std::size_t frame_header_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                            FrameHeader* out) {
    if (data == nullptr || offset + 6u > size) return 0;
    const std::uint16_t syncword =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) | data[offset + 1]);
    if (syncword != kEac3Syncword) return 0;
    const std::size_t base = offset * 8u;
    const std::uint32_t bsid = read_bits(data, base + 40u, 5u);
    if (bsid <= 8u) {
        const std::uint32_t fscod = read_bits(data, base + 32u, 2u);
        const std::uint32_t frmsizecod = read_bits(data, base + 34u, 6u);
        if (fscod > 2u || frmsizecod >= 38u) return 0;
        if (out != nullptr) {
            out->frame_bytes = static_cast<std::size_t>(kAc3FrameWords[fscod][frmsizecod]) * 2u;
            out->offset = offset;
            out->codec = 0;
            out->strmtyp = 0;
            out->substreamid = 0;
        }
        return static_cast<std::size_t>(kAc3FrameWords[fscod][frmsizecod]) * 2u;
    }
    const std::size_t words =
        (static_cast<std::size_t>(data[offset + 2] & 0x07u) << 8) | data[offset + 3];
    if (out != nullptr) {
        out->frame_bytes = (words + 1u) * 2u;
        out->offset = offset;
        out->codec = 1;
        out->strmtyp = static_cast<std::uint8_t>(read_bits(data, base + 16u, 2u));
        out->substreamid = static_cast<std::uint8_t>(read_bits(data, base + 18u, 3u));
    }
    return (words + 1u) * 2u;
}

std::size_t frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    return frame_header_at(data, size, offset, nullptr);
}

std::size_t audio_frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                                 bool end_of_stream) {
    constexpr std::size_t kMaxSubstreams = 8;
    FrameHeader seen[kMaxSubstreams];
    std::size_t count = 0;
    std::size_t consumed = 0;
    while (count < kMaxSubstreams) {
        FrameHeader header;
        const std::size_t bytes = frame_header_at(data, size, offset + consumed, &header);
        if (bytes == 0 || offset + consumed + bytes > size) break;
        bool repeats = false;
        for (std::size_t index = 0; index < count; ++index) {
            if (seen[index].same_stream(header)) {
                repeats = true;
                break;
            }
        }
        if (repeats) return consumed;
        seen[count++] = header;
        consumed += bytes;
    }
    if (count != 0u && consumed != 0u && end_of_stream) return consumed;
    return 0;
}

bool joc_config_idx(const std::uint8_t* frame, std::size_t frame_bytes, unsigned* out) {
    if (out == nullptr || frame == nullptr || frame_bytes < 8u) return false;
    std::size_t offsets[4096];
    std::size_t count = 0;
    marker_offsets(frame, frame_bytes, offsets, 4096, &count);
    for (std::size_t i = 0; i < count; ++i) {
        Container candidate;
        if (!parse_container(frame, frame_bytes * 8u, offsets[i], &candidate)) continue;
        if (!candidate.has(kIdOamd) || !candidate.has(kIdJoc)) continue;
        for (std::size_t p = 0; p < candidate.payload_count; ++p) {
            if (candidate.payloads[p].id != kIdJoc) continue;
            // joc_dmx_config_idx is the first 3 bits of the JOC payload, followed by
            // joc_num_objects_bits (6) and joc_ext_config_idx (3).
            const std::size_t bit = candidate.payloads[p].bit_offset;
            std::uint32_t value = 0;
            for (unsigned index = 0; index < 3u; ++index) {
                const std::size_t position = bit + index;
                if (position / 8u >= frame_bytes) return false;
                value = (value << 1) | ((frame[position / 8u] >> (7u - (position % 8u))) & 1u);
            }
            *out = value;
            return true;
        }
    }
    return false;
}

bool frame_has_joc(const std::uint8_t* frame, std::size_t frame_bytes) {
    if (frame == nullptr || frame_bytes < 8u) return false;

    std::size_t offsets[4096];
    std::size_t count = 0;
    marker_offsets(frame, frame_bytes, offsets, 4096, &count);

    Container matches[16];
    std::size_t match_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Container candidate;
        if (!parse_container(frame, frame_bytes * 8u, offsets[i], &candidate)) continue;
        if (candidate.has(kIdOamd) && candidate.has(kIdJoc)) {
            if (match_count < 16) matches[match_count++] = candidate;
        }
    }
    if (match_count == 0) return false;

    // A payload may contain bytes that look like another container; only
    // top-level ones count.
    Container top_level[16];
    std::size_t top_count = 0;
    for (std::size_t i = 0; i < match_count; ++i) {
        bool nested = false;
        for (std::size_t j = 0; j < top_count; ++j) {
            if (top_level[j].start_bit < matches[i].start_bit &&
                matches[i].start_bit < top_level[j].start_bit + top_level[j].raw_size * 8u) {
                nested = true;
                break;
            }
        }
        if (!nested && top_count < 16) top_level[top_count++] = matches[i];
    }
    // Exactly one is what the core requires; more than one is ambiguous there.
    return top_count == 1u;
}

ScanResult scan(const std::uint8_t* data, std::size_t size, std::size_t max_frames) {
    ScanResult result;
    if (data == nullptr || size < 8u) {
        result.detail = "buffer too small";
        return result;
    }

    std::size_t offset = 0;
    std::size_t frames = 0;
    std::size_t with_joc = 0;
    bool first = true;
    while (frames < max_frames) {
        const std::size_t frame_bytes = frame_bytes_at(data, size, offset);
        if (frame_bytes == 0) {
            result.detail = frames == 0 ? "no E-AC-3 syncword at the start" : "stream walk ended";
            break;
        }
        if (offset + frame_bytes > size) {
            result.detail = "buffer ends inside a syncframe";
            break;
        }
        if (first) {
            result.first_frame_bytes = frame_bytes;
            first = false;
        } else if (frame_bytes != result.first_frame_bytes) {
            result.all_frames_same_size = false;
        }
        if (frame_has_joc(data + offset, frame_bytes)) ++with_joc;
        ++frames;
        offset += frame_bytes;
    }

    result.frames_examined = frames;
    result.frames_with_joc = with_joc;
    if (frames == 0) {
        result.joc = JocState::kUnknown;
        if (result.detail[0] == '\0') result.detail = "no syncframes";
    } else if (with_joc != 0) {
        result.joc = JocState::kYes;
        result.detail = (with_joc == frames)
                            ? "every examined syncframe carries the JOC EMDF container"
                            : "the JOC EMDF container is present in part of the examined "
                              "audio frames (dependent substream or a late first frame)";
    } else {
        result.joc = JocState::kNo;
        result.detail = "no examined syncframe carries a JOC EMDF container";
    }
    return result;
}

}  // namespace joc_eac3
