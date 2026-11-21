// Beat repeat. dsp/repeat.hpp has the design; the per-sample work is in repeatloops.ti.c.
#include "repeat.hpp"

#include "repeatloops.h"

#include <enaga/abi.h>

#include <cstddef>

namespace strata {
namespace {

// The tick figures in repeat_length_ticks are note values at 96 PPQN, so they are only note
// values if that is what the clock runs at.
static_assert(ENAGA_CLOCK_PPQN == 96,
              "repeat_length_ticks spells its note values in 96ths of a quarter note");

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

}  // namespace

const char *repeat_length_name(RepeatLength length) {
    switch (length) {
        case RepeatLength::quarter:           return "1/4";
        case RepeatLength::eighth:            return "1/8";
        case RepeatLength::sixteenth:         return "1/16";
        case RepeatLength::thirty_second:     return "1/32";
        case RepeatLength::quarter_triplet:   return "1/4T";
        case RepeatLength::eighth_triplet:    return "1/8T";
        case RepeatLength::sixteenth_triplet: return "1/16T";
        default:                              return "?";
    }
}

bool BeatRepeat::init(float sample_rate) {
    const float rate = sample_rate > 1.0f ? sample_rate : 48000.0f;
    ring_frames_ = int(rate * kRepeatSeconds);
    if (ring_frames_ < 2) ring_frames_ = 2;

    // Interleaved stereo, so two floats a frame. 2.93 MiB at 48 kHz.
    ring_ = new float[2 * std::size_t(ring_frames_)];
    if (ring_ == nullptr) return false;
    for (int i = 0; i < 2 * ring_frames_; i++) ring_[i] = 0.0f;

    captured_ = 0;
    write_ = 0;
    held_ = false;
    pitch_down_ = false;
    start_ = 0;
    length_ = 0;
    offset_ = 0;
    frac_ = 0;
    wanted_ = 0;
    return true;
}

void BeatRepeat::set(uint32_t loop_frames, bool pitch_down) {
    // Clamped to the ring. Clamping to what has been CAPTURED happens on the press instead,
    // because the ring fills as the transport runs and the answer changes between now and then.
    wanted_ = clamp_int(int(loop_frames), 1, ring_frames_);
    pitch_down_ = pitch_down;
}

void BeatRepeat::hold(bool held) {
    if (held == held_) return;
    held_ = held;
    if (!held_) {
        length_ = 0;
        return;
    }

    // The press. The loop is the last `wanted_` frames of what has been captured, which is where
    // the write cursor is now.
    const int available = captured_ < ring_frames_ ? captured_ : ring_frames_;
    length_ = clamp_int(wanted_, 0, available);
    if (length_ <= 0) {
        // Nothing has been captured yet -- the transport has only just started. Holding loops
        // nothing rather than a ring of silence, and `active()` says so, so the caller leaves
        // the live master alone.
        return;
    }
    start_ = write_ - length_;
    start_ = start_ < 0 ? start_ + ring_frames_ : start_;
    offset_ = 0;
    frac_ = 0;
}

void BeatRepeat::process(float *interleaved, int frames) {
    if (ring_ == nullptr || frames <= 0) return;

    if (!active()) {
        // Idle: the ring follows the master, and the output is untouched.
        write_ = strata_repeat_capture(ring_, ring_frames_, write_, interleaved, frames);
        captured_ += frames;
        if (captured_ > ring_frames_) captured_ = ring_frames_;
        return;
    }

    // Held: the output IS the loop, and nothing is captured -- see dsp/repeat.hpp on why the
    // repeat must not record itself.
    //
    // Half rate is an exact (0, 2^31) step, so an octave down is not an approximation.
    const int step_int = pitch_down_ ? 0 : 1;
    const uint32_t step_frac = pitch_down_ ? 0x80000000u : 0u;
    strata_repeat_play(ring_, ring_frames_, start_, length_, &offset_, &frac_, step_int,
                       step_frac, interleaved, frames);
}

}  // namespace strata
