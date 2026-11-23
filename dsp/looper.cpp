// The master looper. dsp/looper.hpp has the design.
#include "looper.hpp"

#include "looperloops.h"
#include "loopsync.hpp"

namespace strata {
namespace {

constexpr int kMaxBlock = 64;
constexpr float kInvBlock = 1.0f / float(kMaxBlock);

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
    // A loop playing (or already outgoing under an earlier arm) keeps playing until the bar.
    outgoing_ = state_ == State::playing || (state_ == State::armed && outgoing_);
    if (!outgoing_) {
        frames_ = 0;
        pos_ = 0;
        mix_ = 0.0f;
    }
    state_ = State::armed;
    take_frames_ = frames;
    take_bars_mask_ = bars - 1u;
    recorded_ = 0;
    return true;
}

void Looper::clear() {
    state_ = State::idle;
    frames_ = 0;
    recorded_ = 0;
    pos_ = 0;
    mix_ = 0.0f;
    outgoing_ = false;
}

void Looper::on_bar(uint32_t bar, int32_t frame) {
    if (state_ == State::armed) {
        // The outgoing loop's place, for its fade over the take's first block.
        out_frames_ = frames_;
        out_pos_ = pos_;
        // The take records this whole block from frame 0, so the bar line is loop frame
        // `frame` (dsp/frozen.hpp's `lead`; negative after a start that landed mid-bar).
        state_ = State::recording;
        frames_ = take_frames_;
        bars_mask_ = take_bars_mask_;
        origin_ = bar;
        lead_ = loop_wrap(frame, frames_);
        recorded_ = 0;
        pos_ = 0;
        return;
    }
    if (state_ != State::playing) return;
    pos_ = loop_resync(pos_, frames_, bars_mask_, origin_, lead_, bar, frame);
}

void Looper::advance(uint32_t frames) {
    if (frames_ == 0u) return;
    pos_ += frames;
    while (pos_ >= frames_) pos_ -= frames_;
}

void Looper::play(float *interleaved, int n, float target) {
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

void Looper::fade_out_outgoing(float *interleaved, int n) {
    outgoing_ = false;
    if (mix_ == 0.0f || out_frames_ == 0u) {
        mix_ = 0.0f;
        return;
    }
    // Copied out first: the take is about to write this block over the front of the buffer,
    // which is where the outgoing loop may be reading.
    int16_t old[2 * kMaxBlock];
    uint32_t at = out_pos_;
    for (int i = 0; i < n; i++) {
        old[2 * i] = buffer_[2u * at];
        old[2 * i + 1] = buffer_[2u * at + 1u];
        at = at + 1u < out_frames_ ? at + 1u : 0u;
    }
    const float step = -mix_ * (n == kMaxBlock ? kInvBlock : 1.0f / float(n));
    strata_loop_mix(interleaved, old, n, mix_, step);
    mix_ = 0.0f;
}

void Looper::process(float *interleaved, int frames, float mix, bool running) {
    if (frames <= 0) return;
    const int n = frames > kMaxBlock ? kMaxBlock : frames;
    const float wanted = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    const float target = running ? wanted : 0.0f;

    if (state_ == State::recording) {
        // The block is recorded as the kit made it, before the outgoing loop's fade goes over it.
        int16_t *const into = buffer_ + 2u * recorded_;
        const uint32_t room = frames_ - recorded_;
        const int run = uint32_t(n) < room ? n : int(room);
        if (outgoing_) {
            float dry[2 * kMaxBlock];
            for (int i = 0; i < 2 * n; i++) dry[i] = interleaved[i];
            fade_out_outgoing(interleaved, n);
            strata_loop_record(into, dry, 2 * run);
        } else {
            strata_loop_record(into, interleaved, 2 * run);
        }
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
    // Armed over a loop that is still playing: it plays on until the bar line.
    if (state_ == State::playing || (state_ == State::armed && outgoing_)) play(interleaved, n, target);
}

}  // namespace strata
