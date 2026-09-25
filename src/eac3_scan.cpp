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

std::size_t frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (data == nullptr || offset + 4u > size) return 0;
    const std::uint16_t syncword =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) | data[offset + 1]);
    if (syncword != kEac3Syncword) return 0;
    const std::size_t words =
        (static_cast<std::size_t>(data[offset + 2] & 0x07u) << 8) | data[offset + 3];
    return (words + 1u) * 2u;
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
    } else if (with_joc == frames) {
        result.joc = JocState::kYes;
        result.detail = "every examined syncframe carries the JOC EMDF container";
    } else {
        result.joc = JocState::kNo;
        result.detail = "at least one examined syncframe has no JOC EMDF container";
    }
    return result;
}

}  // namespace joc_eac3
