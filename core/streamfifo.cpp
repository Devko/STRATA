#include "streamfifo.hpp"

namespace strata {

void StreamFifo::reset(int channels, uint32_t first) {
    channels_ = channels == 2 ? 2 : 1;
    base_ = first;
    end_ = first;
}

int16_t *StreamFifo::write_span(uint32_t *frames) {
    // Free room: the whole buffer when nothing wanted is held (including after an underrun moved
    // base_ past end_), otherwise what the wanted frames leave.
    const uint32_t used = held();
    const uint32_t room = kFrames - used;
    const uint32_t start = end_ & kMask;
    const uint32_t contiguous = kFrames - start;
    *frames = room < contiguous ? room : contiguous;
    return data_ + start * uint32_t(channels_);
}

uint32_t stream_frames_in(uint32_t bytes, uint32_t frame_bytes) {
    if (frame_bytes == 0u || frame_bytes > 8u) return 0u;
    static constexpr float kInverse[9] = {0.0f,        1.0f,        1.0f / 2.0f,
                                          1.0f / 3.0f, 1.0f / 4.0f, 1.0f / 5.0f,
                                          1.0f / 6.0f, 1.0f / 7.0f, 1.0f / 8.0f};
    uint32_t frames = uint32_t(float(bytes) * kInverse[frame_bytes]);
    while (frames > 0u && frames * frame_bytes > bytes) frames--;
    while ((frames + 1u) * frame_bytes <= bytes) frames++;
    return frames;
}

}  // namespace strata
