// The performance mixer's master effects. dsp/masterfx.hpp has the design.
#include "masterfx.hpp"

#include "masterfxloops.h"

#include <enaga/dsp.hpp>

namespace strata {
namespace {

namespace ed = enaga::dsp;

constexpr float kSettled = 0.001f;
constexpr float kTwoPi = 6.2831853f;

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
           depth_.gate > 0.0f || target_.pump > 0.0f || target_.crush > 0.0f ||
           target_.noise > 0.0f || target_.gate > 0.0f;
}

void MasterFx::process(float *interleaved, int frames, float beat_phase, float phase_step) {
    if (!active() || frames <= 0) return;
    depth_.pump = approach(depth_.pump, target_.pump);
    depth_.crush = approach(depth_.crush, target_.crush);
    depth_.noise = approach(depth_.noise, target_.noise);
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
