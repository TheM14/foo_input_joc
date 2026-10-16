// E-AC-3 syncframe walking and JOC detection.
//
// The plugin must decide "is this file E-AC-3 JOC" from the bitstream alone,
// because container metadata (MP4 dec3, Matroska A_EAC3) says only "E-AC-3" and
// the JOC flag inside it is frequently missing.
//
// A syncframe carries JOC when it holds exactly one top-level EMDF container
// (syncword 0x5838, any of the eight bit alignments) whose payload list contains
// both ID11 (OAMD) and ID14 (JOC) — the same per-frame test the rendering core
// applies.  The stream counts as JOC when at least one examined audio frame
// carries one: in a 7-channel program the JOC payload lives in the dependent
// substream while the independent substream is a plain 5.1 core, and an AC-3
// core may precede it, so "every syncframe carries JOC" is not a valid rule.
// The first audio frame may also carry no metadata at all.
#pragma once

#include <cstddef>
#include <cstdint>

namespace joc_eac3 {

// Syncframe header fields the plugin needs.  codec/strmtyp/substreamid identify
// the substream: within one audio frame each combination appears once, which is
// what turns a syncframe stream into audio frames.
struct FrameHeader {
    std::size_t frame_bytes = 0;
    std::size_t offset = 0;
    std::uint8_t codec = 0;  // 0 = AC-3, 1 = E-AC-3
    std::uint8_t strmtyp = 0;
    std::uint8_t substreamid = 0;

    bool same_stream(const FrameHeader& other) const {
        return codec == other.codec && strmtyp == other.strmtyp &&
               substreamid == other.substreamid;
    }
};

enum class JocState {
    kYes,      // at least one examined syncframe carried a JOC EMDF container
    kNo,       // no examined syncframe carried one
    kUnknown,  // not enough data, or the stream is not walkable ac3/eac3
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
// header there.  AC-3 and E-AC-3 are told apart by bsid exactly as the core does:
// bsid <= 8 sizes the frame from fscod/frmsizecod, anything above uses frmsiz.
std::size_t frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset);

// Same, and fills the substream identity.  Returns 0 when no header is there.
std::size_t frame_header_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                            FrameHeader* out);

// Length of the whole audio frame starting at data[offset]: every syncframe up to
// (and excluding) the first repeated (codec, strmtyp, substreamid) combination.
// A 7-channel program interleaves an independent and a dependent substream, so one
// audio frame is two syncframes and one bed frame.
//
// Returns 0 when the group is still open inside the buffer -- the caller has to ask
// again with more data -- unless `end_of_stream` says no more bytes will arrive, in
// which case the trailing group is complete by definition.
std::size_t audio_frame_bytes_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                                 bool end_of_stream);

// Reads joc_dmx_config_idx (the first 3 bits of the JOC payload) out of one
// syncframe.  False when the frame carries no JOC payload.
bool joc_config_idx(const std::uint8_t* frame, std::size_t frame_bytes, unsigned* out);

// True when this single syncframe carries the JOC EMDF container.
bool frame_has_joc(const std::uint8_t* frame, std::size_t frame_bytes);

}  // namespace joc_eac3
