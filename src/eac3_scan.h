// E-AC-3 syncframe walking and JOC detection.
//
// The plugin must decide "is this file E-AC-3 JOC" from the bitstream alone,
// because container metadata (MP4 dec3, Matroska A_EAC3) says only "E-AC-3" and
// the JOC flag inside it is frequently missing.
//
// The criterion implemented here is deliberately the same one the rendering core
// applies before it will render a frame: a syncframe carries JOC when it holds
// exactly one top-level EMDF container (syncword 0x5838, any of the eight bit
// alignments) whose payload list contains both ID11 (OAMD) and ID14 (JOC).
// Agreeing with the core matters: a looser test would claim files the core then
// refuses, which is worse than declining them.
#pragma once

#include <cstddef>
#include <cstdint>

namespace joc_eac3 {

// E-AC-3 syncframe header fields the plugin needs.
struct FrameHeader {
    std::size_t frame_bytes = 0;
    std::size_t offset = 0;
};

enum class JocState {
    kYes,      // every frame examined carried a JOC EMDF container
    kNo,       // at least one frame examined did not
    kUnknown,  // not enough data, or the stream is not walkable E-AC-3
};

struct ScanResult {
    JocState joc = JocState::kUnknown;
    std::size_t frames_examined = 0;
    std::size_t frames_with_joc = 0;
    std::size_t first_frame_bytes = 0;
    bool all_frames_same_size = true;
    const char* detail = "";
};

// Walks up to `max_frames` syncframes of `data` and classifies the stream.
ScanResult scan(const std::uint8_t* data, std::size_t size, std::size_t max_frames);

// Length of the syncframe starting at data[offset], or 0 when there is no valid
// header there.  Mirrors the core's frmsiz handling exactly.
std::size_t frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset);

// True when this single syncframe carries the JOC EMDF container.
bool frame_has_joc(const std::uint8_t* frame, std::size_t frame_bytes);

}  // namespace joc_eac3
