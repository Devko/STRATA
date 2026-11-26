#include "click.hpp"

#include "clickloops.h"

#include <enaga/dsp.hpp>

namespace strata {

void Click::init(float sample_rate) {
    const float rate = sample_rate > 1.0f ? sample_rate : 48000.0f;
    const float inv_rate = enaga::dsp::reciprocal(rate);
    coef_beat_ = 2.0f * enaga_cos_turns(kFreqBeat * inv_rate);
    coef_bar_ = 2.0f * enaga_cos_turns(kFreqBar * inv_rate);
    sin_beat_ = enaga_sin_turns(kFreqBeat * inv_rate);
    sin_bar_ = enaga_sin_turns(kFreqBar * inv_rate);
    decay_ = enaga_exp(-inv_rate * enaga::dsp::reciprocal(kDecaySeconds));
    stop();
}

void Click::trigger(uint32_t frame, bool bar) {
    pending_ = true;
    pending_bar_ = bar;
    pending_frame_ = frame;
}

void Click::render_add(float *out, int frames) {
    if (frames <= 0) return;
    if (remaining_ > 0) {
        // The click already ringing, up to where a new one starts (or the block's end).
        const int until = pending_ && pending_frame_ < uint32_t(frames) ? int(pending_frame_) : frames;
        const int n = until < remaining_ ? until : remaining_;
        env_ = strata_click_add(out, n, &s1_, &s2_, coef_, env_, decay_);
        remaining_ -= n;
    }
    if (!pending_) return;
    // A new click: on its frame when that is in this block, else on the first.
    const int start = pending_frame_ < uint32_t(frames) ? int(pending_frame_) : 0;
    pending_ = false;
    coef_ = pending_bar_ ? coef_bar_ : coef_beat_;
    // y[n] = c y[n-1] - y[n-2] with y[-1] = 0, y[-2] = -sin(w): the first sample is sin(w).
    s1_ = 0.0f;
    s2_ = -(pending_bar_ ? sin_bar_ : sin_beat_);
    env_ = pending_bar_ ? kGainBar : kGainBeat;
    remaining_ = kFrames;
    const int n = frames - start < remaining_ ? frames - start : remaining_;
    env_ = strata_click_add(out + 2 * start, n, &s1_, &s2_, coef_, env_, decay_);
    remaining_ -= n;
}

}  // namespace strata
