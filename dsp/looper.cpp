// The master looper. dsp/looper.hpp has the design.
#include "looper.hpp"

#include "looperloops.h"

namespace strata {
namespace {

constexpr int kMaxBlock = 64;
constexpr float kInvBlock = 1.0f / float(kMaxBlock);

// `x` modulo `n`, for any sign of x. Control rate only (an integer remainder is a library call
// on this chip), and 32-bit, as dsp/frozen.cpp explains.
uint32_t wrap(int32_t x, uint32_t n) {
    if (x >= 0) return uint32_t(x) % n;
    const uint32_t r = uint32_t(-x) % n;
    return r == 0u ? 0u : n - r;
}

}  // namespace

void Looper::attach(int16_t *buffer, uint32_t capacity) {
    buffer_ = buffer;
    capacity_ = buffer != nullptr ? capacity : 0u;
    clear();
}

bool Looper::arm(uint32_t frames, uint32_t bars) {
    const bool bars_ok = bars == 1u || bars == 2u || bars == 4u || bars == 8u;
    if (buffer_ == nullptr || frames == 0u || frames > capacity_ || !bars_ok) return false;
    if (state_ == State::recording) return false;
    state_ = State::armed;
    frames_ = frames;
    bars_mask_ = bars - 1u;
    recorded_ = 0;
    pos_ = 0;
    mix_ = 0.0f;
    return true;
}

void Looper::clear() {
    state_ = State::idle;
    frames_ = 0;
    recorded_ = 0;
    pos_ = 0;
    mix_ = 0.0f;
}

void Looper::on_bar(uint32_t bar, int32_t frame) {
    if (state_ == State::armed) {
        // The take records this whole block from frame 0, so the bar line is loop frame
        // `frame` (dsp/frozen.hpp's `lead`; negative after a start that landed mid-bar).
        state_ = State::recording;
        origin_ = bar;
        lead_ = wrap(frame, frames_);
        recorded_ = 0;
        return;
    }
    if (state_ != State::playing || ((bar - origin_) & bars_mask_) != 0u) return;
    pos_ = wrap(int32_t(lead_) - frame, frames_);
}

void Looper::advance(uint32_t frames) {
    if (frames_ == 0u) return;
    pos_ += frames;
    while (pos_ >= frames_) pos_ -= frames_;
}

void Looper::process(float *interleaved, int frames, float mix, bool running) {
    if (frames <= 0) return;
    const int n = frames > kMaxBlock ? kMaxBlock : frames;

    if (state_ == State::recording) {
        const uint32_t room = frames_ - recorded_;
        const int run = uint32_t(n) < room ? n : int(room);
        strata_loop_record(buffer_ + 2u * recorded_, interleaved, 2 * run);
        recorded_ += uint32_t(run);
        if (recorded_ >= frames_) {
            // The next frame is loop frame 0 again: the take is a whole number of bars.
            state_ = State::playing;
            pos_ = 0;
            mix_ = 0.0f;
            advance(uint32_t(n - run));
        }
        return;
    }
    if (state_ != State::playing) return;

    const float wanted = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    const float target = running ? wanted : 0.0f;
    if (mix_ == 0.0f && target == 0.0f) {
        advance(uint32_t(n));
        return;
    }
    const float step = (target - mix_) * (n == kMaxBlock ? kInvBlock : 1.0f / float(n));
    int done = 0;
    while (done < n) {
        const uint32_t room = frames_ - pos_;
        const int run = uint32_t(n - done) < room ? n - done : int(room);
        strata_loop_mix(interleaved + 2 * done, buffer_ + 2u * pos_, run,
                        mix_ + step * float(done), step);
        done += run;
        advance(uint32_t(run));
    }
    mix_ = target;
}

}  // namespace strata
