// The ambient modes of the two sends: send A's LONG delay and send B's HALL reverb.
//
// Roland chose (2026-11-22) to give the existing sends a second mode each rather than add a third
// send: no new per-track knob, and throws, the delay freeze and the delay feeding the reverb all
// keep working. The classic modes (Enaga's StereoDelay and Reverb) stay the default, and a project
// that never switches mode renders bit for bit as before (tests/test_golden.cpp).
//
// LONG delay
//   Up to 12 s of stereo, so four bars of 4/4 down to 80 BPM: the division times a multiplier
//   (X1 to X8). In the feedback path a low cut, the tone low-pass, four all-passes a side
//   (DIF: each pass smears a little more, so echoes become a wash) and a soft ceiling. WOW
//   sweeps the read heads, a quarter turn apart left and right, like a tape loop. A time change
//   glides at up to half speed, as a tape would, rather than jumping. The freeze is in place:
//   the feed fades out, the feedback goes to exactly 1 and the filters and wow open, so the loop
//   holds unchanged for as long as it is held -- no copy, and any length up to the full 12 s.
//
// HALL reverb
//   Four input all-passes into eight long lines (about 60 ms to 280 ms, scaled by SIZ) mixed by a
//   Hadamard matrix, each line damped and read at a slowly swept length (MOD), which keeps a long
//   tail from ringing metallic. SHM feeds the tail back in an octave up, through a two-grain pitch
//   shifter, for the rising shimmer of ambient pads.
//
// Hot loops in ambientloops.ti.c. Both allocate in init() only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ambientloops.h"

namespace strata {

class LongDelay {
public:
    static constexpr float kMaxSeconds = 12.0f;
    // The all-passes' delays, a side: part of the loop, so the read delay is shortened by them.
    static constexpr int kDiffuserFrames = 2016;
    static constexpr float kMaxWowSeconds = 0.008f;

    LongDelay() = default;
    LongDelay(const LongDelay &) = delete;
    LongDelay &operator=(const LongDelay &) = delete;

    // App::init only. False when the memory is not there.
    bool init(float sample_rate);

    // Control rate. `seconds` is the whole echo period; wow and diffusion 0 to 1. `snap` jumps
    // to the new time instead of gliding (when nothing is sounding).
    void set(float seconds, float feedback, float tone, float low_cut_hz, float wow,
             float diffusion, bool ping_pong, bool snap);

    // The freeze: held, what is in the loop repeats unchanged and nothing new goes in.
    void hold(bool held) { held_ = held; }
    bool held() const { return held_; }

    // Feeds mono `in` and ADDS level x the echoes to out_l / out_r.
    void process(const float *in, float *out_l, float *out_r, int n, float level);

    // The current read delay in frames, all of the loop included, for the tests.
    float loop_frames() const {
        return float(state_.delay_whole + kDiffuserFrames) + state_.delay_frac;
    }
    std::size_t bytes() const { return bytes_; }

private:
    float rate_ = 48000.0f;
    strata_long_delay state_{};
    strata_long_delay_block block_{};
    float feedback_target_ = 0.0f;
    float wow_target_ = 0.0f;
    float low_pass_ = 1.0f, high_pass_ = 0.0f;
    bool held_ = false;
    std::size_t bytes_ = 0;
};

class HallReverb {
public:
    HallReverb() = default;
    HallReverb(const HallReverb &) = delete;
    HallReverb &operator=(const HallReverb &) = delete;

    bool init(float sample_rate);

    // Control rate. decay is the RT60 in seconds; damping 0 bright to 1 dark; size 0.35 to 1.8
    // (times the line lengths); mod and shimmer 0 to 1. `snap` sets the lengths at once.
    void set(float decay_s, float damping, float size, float mod, float shimmer, bool snap);

    void process(const float *in, float *out_l, float *out_r, int n, float level);

    std::size_t bytes() const { return bytes_; }

    static constexpr float kMinSize = 0.35f;
    static constexpr float kMaxSize = 1.8f;

private:
    float rate_ = 48000.0f;
    strata_hall state_{};
    strata_hall_block block_{};
    float target_[STRATA_HALL_LINES] = {};
    std::size_t bytes_ = 0;
};

}  // namespace strata
