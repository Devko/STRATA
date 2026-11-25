// The performance mixer: sixteen effects on VALUE (push VALUE + STEP 1-16).
//
// Roland, 2026-11-23, after modbang's Octatrack Performance Mixer: a template whose crossfader
// blends the dry mix into one of sixteen effect "scenes", the sixteenth being a master looper.
// STRATA's VALUE becomes that crossfader. Push VALUE + a STEP picks an effect, VALUE turns its
// AMOUNT (0 dry ... 127 the whole effect), and only one effect plays at a time. Slot 16 is the
// looper (dsp/looper.hpp), which keeps its mix when an effect is picked over it.
//
// This file is the pure half: which effect a slot is, its name, and what an amount means as a
// set of overrides on things the engine already has (the DJ filter, the sends, the kill EQ,
// the track faders, the tape stop, the delay freeze) plus the depths of the master effects
// dsp/masterfx.hpp adds. Slot 13, the scene morph, is no effect at all: the app turns the morph
// with it, the one PERFORM's VALUE turns (backlog D2), so it plans nothing here. The engine applies the plan at control rate, over the kit and under
// nothing: none of it touches the project, so letting go needs no memory of what was there.
//
// ## At rest it lets go
//
// Amount 0 is a plan that overrides nothing, the same rule as the scene morph and the macros
// (docs/CONTROLS.md): turning VALUE back to 0 is the dry mix, and the knobs own the sound.
//
// core/ includes no Enaga header; the units here are the project's bytes and plain floats.
#pragma once

#include <cstdint>

namespace strata {

// The sixteen slots, as STEP 1-16. Serialized nowhere (the mixer is live state, and no project
// file holds a slot or an amount), so free to renumber and an old project loads the same; but
// docs/CONTROLS.md and the help list them in this order. Slot 7 was STUTTER (the REPEAT pad
// keeps it) and 13 FLANGER until 2026-11-25 (backlog D2, CHANGELOG "Gestures that moved").
enum class PerfFx : uint8_t {
    none = 0,
    lp_sweep,       //  1 the DJ filter's low-pass side
    hp_sweep,       //  2 its high-pass side
    hp_riser,       //  3 a resonant high-pass into the reverb
    echo_riser,     //  4 every track into the delay, feedback climbing
    echo_freeze,    //  5 into the delay, which freezes near the top
    reverb_wash,    //  6 every track into a longer reverb
    mute_bus,       //  7 every track faded out, the delay and reverb ringing on
    tape_stop,      //  8 the tape stop, past a quarter of the way
    bass_kill,      //  9 the kill EQ's LOW band
    pump,           // 10 a sidechain-style dip on every beat
    crush,          // 11 bit and rate reduction
    noise_riser,    // 12 white noise, rising in level and pitch
    morph,          // 13 the scene morph (the app's, not a plan)
    gate,           // 14 a sixteenth-note chopper
    build,          // 15 high-pass, echo and noise together
    looper,         // 16 the master looper (dsp/looper.hpp)
    count
};

constexpr int kPerfSlots = 16;
constexpr uint8_t kPerfAmountMax = 127;
// Which slot is the looper, 1-based like the STEP keys.
constexpr int kLooperSlot = int(PerfFx::looper);

// The slot a STEP key (0-15) picks, and back.
constexpr PerfFx perf_fx_for_step(int step) {
    return step >= 0 && step < kPerfSlots ? PerfFx(step + 1) : PerfFx::none;
}

// Up to 12 characters for the screen, in Enaga's font ("ECHO RISER"). "OFF" for none.
const char *perf_fx_name(PerfFx fx);
// Five characters, for the help and the STEP-key legend ("ECHO").
const char *perf_fx_short(PerfFx fx);

// What one effect at one amount does. Every field's "leave it" value is the default, so a
// default-constructed plan is the dry mix and the engine applies a plan without asking which
// effect made it.
struct PerfPlan {
    // The DJ filter, -1 full low-pass ... +1 full high-pass. Used when `dj_on`.
    bool dj_on = false;
    float dj = 0.0f;
    // Every track's send A (delay) and send B (reverb) raised to at least this byte (0-127).
    uint8_t send_delay = 0;
    uint8_t send_reverb = 0;
    // The delay's feedback raised to at least this (0-0.95); 0 leaves it.
    float delay_feedback = 0.0f;
    // The reverb's decay raised to at least this, in seconds; 0 leaves it.
    float reverb_decay_s = 0.0f;
    // The delay freeze held (dsp/freeze.hpp).
    bool delay_freeze = false;
    // The tape stop held (dsp/tapestop.hpp).
    bool tape_stop = false;
    // Every track's fader scaled by this (1 as set ... 0 silent). The sends are post-fader, so
    // their feeds fade with it and the delay and reverb ring out what they already hold: the
    // MUTE BUS (CyDrums' P5).
    float tracks = 1.0f;
    // The kill EQ's LOW band lowered to at most this gain (1 flat ... 0 a kill).
    float eq_low = 1.0f;
    // The master effects (dsp/masterfx.hpp), each 0 off ... 1 the whole effect.
    float pump = 0.0f;
    float crush = 0.0f;
    float noise = 0.0f;
    float gate = 0.0f;

    bool operator==(const PerfPlan &) const = default;
};

// The plan for `fx` at `amount` (0-127). The looper, the morph and none give the dry plan: the
// looper's mix and the morph's position are not overrides, they are their own.
PerfPlan perf_plan(PerfFx fx, uint8_t amount);

// A slot VALUE turns that is not an effect: the looper (its mix) and the morph (its position).
// Picking one leaves the effect that was up where it is.
constexpr bool perf_fx_is_control(PerfFx fx) {
    return fx == PerfFx::looper || fx == PerfFx::morph;
}

// The amount the tape stop starts at: under it it is not held, so a VALUE turned back down lets
// go before it reaches 0.
constexpr uint8_t kPerfHoldThreshold = 16;
// The echo freeze holds from here up.
constexpr uint8_t kPerfFreezeThreshold = 112;

}  // namespace strata
