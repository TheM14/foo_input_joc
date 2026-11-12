// Whole-file PCM takeover for foo_input_joc.
//
// ffmpeg is the only decoder here: it produces a 48 kHz float32 WAV, and the
// kernel's PCM_LAYOUT stream maps 2.0/5.1/7.1 to fixed objects and renders
// binaural.  The temp WAV exists so the component can seek without a second
// decoder; it is deleted when the input closes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "joc_decode.h"

struct joc_stream;

namespace joc_pcm {

class Source {
public:
    Source() = default;
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    bool open(const std::string& input_path, const joc_decode::Settings& settings,
              std::string* error);
    // Fills interleaved stereo float32 frames; returns frames written.
    std::size_t read(float* destination, std::size_t frames, std::string* error);
    bool seek(double seconds, std::string* error);
    void close();

    double length_seconds() const { return length_seconds_; }
    unsigned channels() const { return 2; }
    unsigned sample_rate() const { return 48000; }
    bool is_open() const { return stream_ != nullptr; }

private:
    struct Header {
        std::uint32_t sample_rate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits = 0;
        std::uint64_t data_offset = 0;
        std::uint64_t data_bytes = 0;
    };

    bool decode_temp(const std::string& input_path, const std::string& ffmpeg,
                     unsigned audio_index, std::string* error);
    bool parse_wav(const std::string& path, Header* header, std::string* error);
    bool push_block(std::string* error);
    bool pull(std::size_t frames, float* destination, std::size_t* produced,
              std::string* error);

    std::FILE* file_ = nullptr;
    joc_stream* stream_ = nullptr;
    Header header_{};
    std::string temp_path_;
    std::vector<float> feed_;
    std::uint64_t frames_read_ = 0;
    std::uint64_t total_frames_ = 0;
    double length_seconds_ = 0.0;
    bool eof_ = false;
    bool flushed_ = false;
    int channels_ = 0;
};

}  // namespace joc_pcm
