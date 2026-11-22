// The delay freeze. dsp/freeze.hpp has the design.
#include "freeze.hpp"

#include "stoploops.h"

#include <cstddef>

namespace strata {

bool DelayFreeze::init(float sample_rate, float max_seconds) {
    const float rate = sample_rate > 1.0f ? sample_rate : 48000.0f;
    ring_frames_ = int(rate * max_seconds);
    if (ring_frames_ < 2) ring_frames_ = 2;
    ring_ = new float[2 * std::size_t(ring_frames_)];
    if (ring_ == nullptr) return false;
    for (int i = 0; i < 2 * ring_frames_; i++) ring_[i] = 0.0f;
    state_ = State::idle;
    held_ = false;
    return true;
}

void DelayFreeze::hold(bool held, int loop_frames) {
    if (held == held_) return;
    held_ = held;
    if (held) {
        if (ring_ == nullptr) return;
        length_ = loop_frames < 2 ? 2 : (loop_frames > ring_frames_ ? ring_frames_ : loop_frames);
        recorded_ = 0;
        read_ = 0;
        state_ = State::recording;
        return;
    }
    if (state_ == State::looping) {
        state_ = State::releasing;
        fade_ = 1.0f;
    } else {
        state_ = State::idle;
    }
}

void DelayFreeze::process(float *wet_left, float *wet_right, int n) {
    if (n <= 0) return;
    int done = 0;
    if (state_ == State::recording) {
        const int room = length_ - recorded_;
        const int m = n < room ? n : room;
        strata_freeze_capture(ring_, recorded_, wet_left, wet_right, m);
        recorded_ += m;
        done = m;
        if (recorded_ < length_) return;
        state_ = State::looping;
        read_ = 0;
    }
    if (state_ == State::looping) {
        strata_freeze_play(ring_, length_, &read_, wet_left + done, wet_right + done, n - done,
                           1.0f, 0.0f);
        return;
    }
    if (state_ == State::releasing) {
        const float step = -1.0f / float(kFreezeFadeFrames);
        strata_freeze_play(ring_, length_, &read_, wet_left, wet_right, n, fade_, step);
        fade_ += step * float(n);
        if (fade_ <= 0.0f) state_ = State::idle;
    }
}

}  // namespace strata
