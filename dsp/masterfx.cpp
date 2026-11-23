// The performance mixer's master effects. dsp/masterfx.hpp has the design.
#include "masterfx.hpp"

#include "masterfxloops.h"

#include <enaga/dsp.hpp>

namespace strata {
namespace {

namespace ed = enaga::dsp;

constexpr float kSettled = 0.001f;
constexpr float kTwoPi = 6.2831853f;
// The flanger's feedback at full depth. The wet side is scaled by 1 - feedback, the most the
// fed-back ring can grow by, so dry + wet never passes the input's peak.
constexpr float kFeedback = 0.6f;

// One depth a block nearer its target; snapped to exactly 0 once it is close, so the effect
// stops being called and the output is bit-exact again.
float approach(float depth, float target) {
    const float next = depth + MasterFx::kSmooth * (target - depth);
    return target <= 0.0f && next < kSettled ? 0.0f : next;
}

}  // namespace

void MasterFx::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    target_ = MasterFxSettings{};
    depth_ = MasterFxSettings{};
    for (float &x : ring_) x = 0.0f;
    write_ = 0;
    lfo_ = 0.0f;
    // A fifth of a hertz: one sweep every five seconds, the classic jet.
    lfo_step_ = 0.2f * ed::reciprocal(rate_);
    ring_dirty_ = false;
    crush_phase_ = 0.0f;
    crush_held_[0] = crush_held_[1] = 0.0f;
    gate_state_ = 1.0f;
    // About 2 ms of edge on the gate: a chop rather than a click.
    gate_smooth_ = 1.0f - ed::exp(-1.0f * ed::reciprocal(0.002f * rate_));
    seed_ = 0x2545F491u;
    noise_lp_ = 0.0f;
}

bool MasterFx::active() const {
    return depth_.pump > 0.0f || depth_.crush > 0.0f || depth_.noise > 0.0f ||
           depth_.flanger > 0.0f || depth_.gate > 0.0f || target_.pump > 0.0f ||
           target_.crush > 0.0f || target_.noise > 0.0f || target_.flanger > 0.0f ||
           target_.gate > 0.0f;
}

void MasterFx::process(float *interleaved, int frames, float beat_phase, float phase_step) {
    if (!active() || frames <= 0) return;
    depth_.pump = approach(depth_.pump, target_.pump);
    depth_.crush = approach(depth_.crush, target_.crush);
    depth_.noise = approach(depth_.noise, target_.noise);
    depth_.flanger = approach(depth_.flanger, target_.flanger);
    depth_.gate = approach(depth_.gate, target_.gate);

    if (depth_.crush > 0.0f) {
        const float c = depth_.crush;
        // From every frame down to one in sixteen, and from 16 bits down to 4.
        const float rate = ed::reciprocal(1.0f + 15.0f * c);
        const float step = ed::exp2(-(15.0f - 12.0f * c));
        const float wet = c * 3.0f > 1.0f ? 1.0f : c * 3.0f;
        strata_fx_crush(interleaved, frames, rate, step, ed::reciprocal(step), wet,
                        &crush_phase_, crush_held_);
    }

    if (depth_.flanger > 0.0f) {
        if (ring_dirty_) {
            // Coming back in: what the ring held is from the last time, seconds ago.
            for (float &x : ring_) x = 0.0f;
            ring_dirty_ = false;
        }
        const float f = depth_.flanger;
        const float base = 0.0005f * rate_;           // 0.5 ms
        const float span = 0.004f * rate_ * f;        // up to 4 ms more
        strata_fx_flanger(interleaved, frames, ring_, kRingFrames - 1, &write_, &lfo_,
                          lfo_step_, base, span, kFeedback * f,
                          1.0f - 0.5f * f, 0.5f * f * (1.0f - kFeedback * f));
    } else {
        ring_dirty_ = true;
    }

    if (depth_.pump > 0.0f || depth_.gate > 0.0f) {
        float phase = beat_phase;
        strata_fx_gain(interleaved, frames, &phase, phase_step, depth_.pump, depth_.gate,
                       gate_smooth_, &gate_state_);
    } else {
        gate_state_ = 1.0f;
    }

    if (depth_.noise > 0.0f) {
        const float n = depth_.noise;
        // From 150 Hz to about 6 kHz: the riser gets thinner and brighter as it climbs.
        const float cutoff = 150.0f * ed::exp2(5.3f * n);
        const float k = 1.0f - ed::exp(-kTwoPi * cutoff * ed::reciprocal(rate_));
        strata_fx_noise(interleaved, frames, &seed_, k, &noise_lp_, 0.075f * n, 1.0f - 0.15f * n);
    }
}

}  // namespace strata
