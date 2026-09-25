// Cheap structural probe of container files, to answer one question: does this
// file hold an E-AC-3 audio track, and if so which one?
//
// It exists so that a Media Library scan does not pay for an ffmpeg launch on
// every MP4 in the collection.  The probe reads a bounded window of the file and
// walks only the headers it needs:
//
//   MP4 / MOV  top-level boxes -> moov -> trak -> hdlr(mdia) + stsd sample entry.
//              moov may sit at the end (no faststart), so the tail is checked too.
//   Matroska   EBML header -> Segment -> Tracks -> TrackEntry -> CodecID.
//
// The E-AC-3 bitstream itself is never demuxed here: ffmpeg extracts it when the
// file is actually decoded, which keeps this file small and lets every container
// ffmpeg understands work without a demuxer of our own.
#pragma once

#include <cstdint>
#include <string>

namespace joc_container {

enum class Kind {
    kNone,
    kMp4,
    kMatroska,
};

const char* kind_name(Kind kind);

struct Result {
    Kind kind = Kind::kNone;
    bool eac3 = false;         // an E-AC-3 audio track is present
    unsigned audio_index = 0;  // which audio track it is, 0-based
    double duration_seconds = 0.0;  // from the container header, 0 when not stated
    std::string codec;         // codec identifier found, for the log
    std::string detail;
};

// True when the extension belongs to a container this component looks inside.
bool is_container_extension(const char* extension);

// Reads at most max_bytes of header (plus the same amount at the end for MP4) and
// reports what it found.  Missing files, empty files and unknown containers come
// back as Kind::kNone with a detail string.
Result scan(const std::string& path, std::size_t max_bytes = 4u * 1024u * 1024u);

}  // namespace joc_container
