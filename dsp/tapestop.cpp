// The tape stop. dsp/tapestop.hpp has the design.
#include "tapestop.hpp"

#include "stoploops.h"

#include <enaga/dsp.hpp>

#include <cstddef>

namespace strata {

bool TapeStop::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    ring_frames_ = int(rate_ * kTapeStopRingSeconds);
    if (ring_frames_ < 4) ring_frames_ = 4;
    ring_ = new float[2 * std::size_t(ring_frames_)];
    if (ring_ == nullptr) return false;
    for (int i = 0; i < 2 * ring_frames_; i++) ring_[i] = 0.0f;
    write_ = 0;
    lag_ = 0.0f;
    speed_ = 1.0f;
    wet_ = 0.0f;
    held_ = false;
    running_ = false;
    set(rate_);
    return true;
}

void TapeStop::set(float stop_frames) {
    const float low = kTapeStopMinSeconds * rate_;
    const float high = kTapeStopMaxSeconds * rate_;
    stop_frames_ = stop_frames < low ? low : (stop_frames > high ? high : stop_frames);
}

void TapeStop::hold(bool held) {
    if (held == held_) return;
    held_ = held;
    if (held) {
        // From full speed, reading the frame just written: the press itself changes nothing.
        lag_ = 0.0f;
        speed_ = 1.0f;
        speed_step_ = enaga::dsp::reciprocal(stop_frames_);
        wet_ = 1.0f;
        running_ = ring_ != nullptr;
    }
    // A release lets `process` fade the wet side out, and then go idle.
}

void TapeStop::process(float *interleaved, int frames) {
    if (!running_ || frames <= 0) return;
    const float wet_step = held_ ? 0.0f : -1.0f / float(kTapeStopFadeFrames);
    strata_tape_stop(interleaved, frames, ring_, ring_frames_, &write_, &lag_, &speed_,
                     speed_step_, &wet_, wet_step);
    if (!held_ && wet_ <= 0.0f) running_ = false;
}

}  // namespace strata
