// The performance mixer's own master effects: pump, crush, noise riser and gate.
// (The flanger was a fifth until slot 13 became the morph, 2026-11-25.)
//
// The other slots of the mixer (core/perffx.hpp) are overrides on things the engine already has
// -- the DJ filter, the sends, the kill EQ, the faders, the tape stop. These four had nothing
// to borrow, so they are here, on audio()'s interleaved stereo buffer after the mixer and the
// looper and before beat repeat: a stutter or a tape stop then takes the effect with it, and an
// effect goes over a playing loop, as on the Octatrack template the mixer copies.
//
// ## What idle costs
//
// Nothing. Each depth is smoothed a block at a time towards its target, and an effect whose
// depth has settled at 0 is not called at all, so a mixer that is never turned leaves the
// output bit-exact (the goldens stay untouched) and a turned one pays for its one effect.
// Loops in masterfxloops.ti.c, so all of it goes through cl6x.
//
// ## Staying under the limiter
//
// The master's soft limiter is before this, so nothing here may add level: the pump and gate
// only take away, the crush holds samples it was given, and the noise scales the program down by as much as it adds.
//
// No allocation, and no buffer: every effect here is a gain or a held sample.
#pragma once

#include <cstdint>

namespace strata {

// The four depths, 0 off ... 1 the whole effect. What core/perffx.hpp's plan carries.
struct MasterFxSettings {
    float pump = 0.0f;
    float crush = 0.0f;
    float noise = 0.0f;
    float gate = 0.0f;
};

class MasterFx {
public:
    // How far a depth moves towards its target each block: about 10 ms to settle.
    static constexpr float kSmooth = 0.12f;

    void init(float sample_rate);

    // Control rate: the targets, from the plan.
    void set(const MasterFxSettings &targets) { target_ = targets; }

    // In place on `frames` interleaved stereo frames. `beat_phase` is where in the beat the
    // block starts (0-1) and `phase_step` how far a frame moves it (1 / frames a beat): the pump
    // and the gate are in time with the clock.
    void process(float *interleaved, int frames, float beat_phase, float phase_step);

    // Anything up, or still settling.
    bool active() const;
    const MasterFxSettings &depth() const { return depth_; }

private:
    float rate_ = 48000.0f;
    MasterFxSettings target_{};
    MasterFxSettings depth_{};

    // The crush.
    float crush_phase_ = 0.0f;
    float crush_held_[2] = {};
    // The gain moves.
    float gate_state_ = 1.0f;
    float gate_smooth_ = 0.0f;
    // The noise.
    uint32_t seed_ = 0x2545F491u;
    float noise_lp_ = 0.0f;
};

}  // namespace strata
