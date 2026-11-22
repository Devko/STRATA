// A streamed sample's decoded frames, between the card and the SAMPLE machine.
//
// docs/SPEC.md: "loops over 30 s stream from the card". A file too big for the sample pool
// (app/storage.hpp, "Streaming") is played as a HEAD, its first seconds loaded into the pool like
// any sample, followed by the rest read through an Enaga `files::Stream`. The audio side reads
// that stream's bytes each block, converts them to int16 (core/pcm.hpp) and keeps them here, and
// dsp/sample.cpp's stream voice reads its taps from here once it is past the head.
//
// Frames are numbered as in the FILE: frame 0 is the file's first, and the first frame this
// holds after a restart is the head's length. That is what lets the voice ask for "frame n"
// without caring whether it comes from the head or from here, and what lets an underrun be
// absorbed: the voice keeps time, asks for frames that have not arrived, and `drop_before`
// moves the window past them so that what arrives late is thrown away rather than played late.
//
// One context only: the audio side writes and reads it. There is nothing atomic here because
// nothing else touches it. Fixed size, no allocation. core/, so it is host-tested
// (tests/test_streamfifo.cpp).
#pragma once

#include <cstdint>

namespace strata {

class StreamFifo {
public:
    // 4096 frames: 85 ms at 48 kHz, which is many blocks of even a voice pitched up five
    // octaves (32 frames a sample, 2048 a block). A power of two, so a frame's slot is a mask.
    static constexpr uint32_t kFrames = 4096;
    static constexpr uint32_t kMask = kFrames - 1u;

    // Empty, holding frames of `channels` (1 or 2), the next one written being `first`.
    void reset(int channels, uint32_t first);

    // --- writing (the audio side, from the stream) ---

    // The file frame the next write is.
    uint32_t next_frame() const { return end_; }
    // Room for this many frames, contiguous in the buffer, at `next_frame()`. Never more than the
    // frames that are free, so a write cannot overwrite one still wanted.
    int16_t *write_span(uint32_t *frames);
    // `frames` were written into the span.
    void commit(uint32_t frames) { end_ += frames; }

    // --- reading (the voice) ---

    bool has(uint32_t frame) const { return frame >= base_ && frame < end_; }
    // The frame's samples (one, or L and R). Only meaningful when `has(frame)`.
    const int16_t *frame(uint32_t frame) const { return data_ + (frame & kMask) * uint32_t(channels_); }
    // Frames before `frame` are no longer wanted. Past what has arrived, too: frames that come in
    // later than that are then written and never read, which is how an underrun catches up.
    void drop_before(uint32_t frame) {
        if (frame > base_) base_ = frame;
    }

    int channels() const { return channels_; }
    uint32_t held() const { return end_ > base_ ? end_ - base_ : 0u; }

private:
    int16_t data_[kFrames * 2] = {};
    int channels_ = 1;
    uint32_t base_ = 0;     // the oldest frame still wanted
    uint32_t end_ = 0;      // one past the newest frame written
};

// How many whole frames `bytes` holds, for frame sizes 1-8 (u8 mono to f32 stereo). The audio
// side needs this every block, and the C674x has no integer divide: a reciprocal estimate, then
// an exact correction.
uint32_t stream_frames_in(uint32_t bytes, uint32_t frame_bytes);

}  // namespace strata
