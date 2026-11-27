// The metronome's click and the count-in's (CyDrums S6, docs/CONTROLS.md "CLICK and COUNT-IN").
//
// Enaga has no click of its own (checked at v0.3.0: nothing in include/enaga or runtime/), so
// this is STRATA's: a short decaying sine, higher and louder on a bar's first beat. The engine
// fires it from the clock's ticks (Engine::metronome_tick), so it lands on the beat's exact frame
// as a step does.
//
// ## Where it goes
//
// Straight to the output, after the master effects and after the RESAMPLE take's tap, as the
// sample audition does: a click is for the player, so it is never recorded, looped, repeated or
// ducked. Both channels, at a fixed level under full scale (kGainBeat, kGainBar), and the output
// sum is clamped to full scale in the loop (clickloops.ti.c), as the audition's is.
//
// ## Cost
//
// One recursive sine and one envelope multiply a sample, for kFrames (about 30 ms) a beat:
// nothing while it is silent. The loop is clickloops.ti.c.
//
// Audio context only, apart from init(). Fixed size, no allocation.
#pragma once

#include <cstdint>

namespace strata {

class Click {
public:
    static constexpr float kFreqBeat = 1000.0f;     // Hz
    static constexpr float kFreqBar = 1600.0f;      // a bar's first beat
    static constexpr float kGainBeat = 0.22f;       // about -13 dB
    static constexpr float kGainBar = 0.32f;        // about -10 dB
    static constexpr float kDecaySeconds = 0.006f;  // the envelope's time constant
    static constexpr int kFrames = 1440;            // 30 ms at 48 kHz: past this it is under -40 dB

    // Coefficients for both pitches and the decay, once.
    void init(float sample_rate);

    // A click starting `frame` frames into the next render_add (0 when that is past the block).
    // A click still ringing is replaced: beats are far further apart than kFrames.
    void trigger(uint32_t frame, bool bar);

    // Add this block's click into interleaved stereo `out` (L R L R), `frames` frames.
    void render_add(float *out, int frames);

    bool active() const { return remaining_ > 0 || pending_; }

    // Silence at once (a stop, a project load).
    void stop() {
        remaining_ = 0;
        pending_ = false;
    }

private:
    float coef_beat_ = 0.0f, coef_bar_ = 0.0f;      // 2 cos(w)
    float sin_beat_ = 0.0f, sin_bar_ = 0.0f;        // sin(w): the first sample
    float decay_ = 0.0f;                            // the envelope's per-sample factor

    // The sounding click.
    float coef_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f;                   // the oscillator's last two outputs
    float env_ = 0.0f;
    int remaining_ = 0;

    // A click waiting for its frame in the next block.
    bool pending_ = false;
    bool pending_bar_ = false;
    uint32_t pending_frame_ = 0;
};

}  // namespace strata
