// A frozen track's loop player. frozen.hpp has the design.
#include "frozen.hpp"

#include "frozenloops.h"
#include "loopsync.hpp"

namespace strata {
namespace {

constexpr float kToFloat = 1.0f / 32768.0f;
constexpr int kMaxBlock = 64;
constexpr float kInvBlock = 1.0f / float(kMaxBlock);

}  // namespace

bool FrozenLoop::set(const int16_t *data, uint32_t frames, uint32_t bars, uint32_t origin,
                     int32_t lead, uint32_t bar_now) {
    const bool bars_ok = bars == 1u || bars == 2u || bars == 4u || bars == 8u;
    if (data == nullptr || frames == 0u || !bars_ok) return false;
    data_ = data;
    frames_ = frames;
    bars_mask_ = bars - 1u;
    origin_ = origin;
    lead_ = loop_wrap(lead, frames);
    gain_ = 0.0f;                       // fades in over the first block
    // Where the clock is now: at the start of bar (bar_now - origin) mod bars of the loop, which
    // is as close as a control-rate set can get. The next bar line puts it exactly in time.
    pos_ = loop_position(frames_, bars_mask_, origin_, lead_, bar_now, 0);
    return true;
}

void FrozenLoop::clear() {
    data_ = nullptr;
    frames_ = 0;
    pos_ = 0;
    gain_ = 0.0f;
}

void FrozenLoop::on_bar(uint32_t bar, int32_t frame) {
    if (data_ == nullptr) return;
    // On the loop's own bar the bar line is loop frame lead_, `frame` frames into the coming
    // block, so the block starts `frame` frames before it. On the others, only a read that is
    // lost moves: after a stop and start, a relocate or a tempo change (dsp/loopsync.hpp).
    pos_ = loop_resync(pos_, frames_, bars_mask_, origin_, lead_, bar, frame);
}

void FrozenLoop::advance(uint32_t frames) {
    if (frames_ == 0u) return;
    pos_ += frames;
    while (pos_ >= frames_) pos_ -= frames_;
}

void FrozenLoop::skip(int frames) {
    if (data_ == nullptr || frames <= 0) return;
    advance(uint32_t(frames));
}

void FrozenLoop::render_add(float *mono, int frames, bool running) {
    if (data_ == nullptr || frames <= 0) return;
    const int n = frames > kMaxBlock ? kMaxBlock : frames;
    const float target = running ? 1.0f : 0.0f;
    if (gain_ == 0.0f && target == 0.0f) {
        advance(uint32_t(n));
        return;
    }
    // One ramp across the block (a 64-frame fade on a start or a stop), in int16 units.
    const float g0 = gain_ * kToFloat;
    const float step = (target - gain_) * (n == kMaxBlock ? kInvBlock : 1.0f / float(n)) * kToFloat;
    // At most two runs: to the loop's end, then from its start. A loop shorter than a block
    // (it cannot be: a bar is thousands of frames) would only repeat the second run.
    int done = 0;
    while (done < n) {
        const uint32_t room = frames_ - pos_;
        const int run = uint32_t(n - done) < room ? n - done : int(room);
        strata_frozen_add(mono + done, data_ + pos_, run, g0 + step * float(done), step);
        done += run;
        advance(uint32_t(run));
    }
    gain_ = target;
}

}  // namespace strata
