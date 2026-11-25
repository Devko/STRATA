// Beat repeat: hold to loop the last fraction of the master output.
//
// docs/SPEC.md's performance table: *"Hold to loop the last 1/4, 1/8, 1/16 or 1/32 (or triplets)
// of the master output from a 2-bar ring buffer, with optional pitch-down."* Not committed to
// the pattern -- it is an effect on the output and nothing else.
//
// ## Where it sits
//
// At the very end of the chain, on audio()'s interleaved stereo buffer, AFTER the master. That
// is what "the master output" means, and it is why this takes the finished buffer rather than
// living inside dsp/buses.cpp: a repeat should capture the limiter and the master filter too,
// because that is what the room heard.
//
// ## Capture stops while it is held
//
// While the loop plays, nothing is written into the ring. If it were, the repeat would record
// itself and the history it is looping would be gone by the time the key came up -- the loop
// would decay into a copy of its own first pass. So the ring holds what played BEFORE the
// press, and the moment the key is released the output is the live master again, continuous.
//
// The consequence to know is that the audio under a long hold is frozen: coming out of a four
// second repeat, the mix resumes where the music actually is, not where the repeat left off.
// That is what every box with this feature does, and it is the point.
//
// ## The ring is two bars at 60 BPM, and why that is enough
//
// docs/archive/REVIEW.md sized it: "Beat-repeat ring 2 bars @ 60 BPM stereo float = 2.93 MiB". Enaga's
// clock goes down to 20 BPM, where two bars would be 24 s and 8.8 MiB, so below 60 BPM the ring
// holds less than two bars. That costs nothing: the LONGEST loop this offers is a quarter note,
// which even at 20 BPM is 3 s against the ring's 8. Two bars was always headroom rather than a
// requirement.
//
// ## Allocation
//
// `init()` takes the ring from the heap, once, as every buffer in the signal path does
// (CLAUDE.md). 2.93 MiB of it, which is a figure docs/BUDGET.md should carry next to the sample
// region's 64 MiB and the staging buffers' 18 MiB.
#pragma once

#include <cstdint>

namespace strata {

// The loop lengths, as note values. Serialized nowhere -- a repeat is live state -- so free to
// renumber. The order is the order the UI walks them in: longest first, because that is the one
// a performer reaches for and VALUE picks the length while the pad is held.
enum class RepeatLength : uint8_t {
    quarter = 0,
    eighth,
    sixteenth,
    thirty_second,
    quarter_triplet,
    eighth_triplet,
    sixteenth_triplet,
    count
};

// "1/4", "1/8T" and so on. Upper case and digits only, for the device font.
const char *repeat_length_name(RepeatLength length);

// The length in clock ticks at 96 PPQN, which is what makes these note values rather than
// milliseconds: a repeat has to stay in time when the tempo moves. The caller turns ticks into
// frames, because dsp/ does not know the tempo -- the same split core/scheduler.hpp makes when
// it takes `samples_per_tick` from outside.
//
// A triplet is two thirds of the plain value: a quarter-note triplet is 64 ticks of the 96 in a
// quarter note.
constexpr int repeat_length_ticks(RepeatLength length) {
    switch (length) {
        case RepeatLength::quarter:           return 96;
        case RepeatLength::eighth:            return 48;
        case RepeatLength::sixteenth:         return 24;
        case RepeatLength::thirty_second:     return 12;
        case RepeatLength::quarter_triplet:   return 64;
        case RepeatLength::eighth_triplet:    return 32;
        case RepeatLength::sixteenth_triplet: return 16;
        default:                              return 96;
    }
}

// One step faster (`direction` > 0) or slower (< 0) along the lengths sorted by their ticks:
// 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32. This is the walk REPEAT + LEFT/RIGHT makes (the
// roll rate on the arrows, CyDrums' P3), where the enum's own order -- the plain values, then
// the triplets -- would jump from 1/32 back to 1/4T. Stops at both ends.
RepeatLength repeat_length_step(RepeatLength length, int direction);

// How long the ring is, in seconds. Two bars of 4/4 at 60 BPM.
constexpr float kRepeatSeconds = 8.0f;

class BeatRepeat {
public:
    // App::init only: takes the ring from the heap. False when it cannot.
    bool init(float sample_rate);

    // Control rate. `loop_frames` is how much of the past to loop, which the caller derives from
    // `repeat_length_ticks` and the tempo; it is clamped to the ring and to how much audio has
    // actually been captured, so a repeat pressed a moment after the transport starts loops what
    // exists rather than reading silence it was never given.
    //
    // `pitch_down` halves the read rate, which drops an octave and makes the loop take twice as
    // long to play through. docs/SPEC.md's "optional pitch-down".
    void set(uint32_t loop_frames, bool pitch_down);

    // Hold the pad, or let it go. Taking it up is immediate: the next frame is the live master
    // again.
    void hold(bool held);

    // The whole of it, on audio()'s interleaved stereo buffer. Captures while idle, replaces the
    // buffer while held. Safe to call with `held` never set, which is the normal case and costs
    // one copy of the block into the ring.
    void process(float *interleaved, int frames);

    bool active() const { return held_ && length_ > 0; }
    bool pitch_down() const { return pitch_down_; }

    // For the screen and for tests.
    uint32_t loop_frames() const { return uint32_t(length_); }
    uint32_t ring_frames() const { return uint32_t(ring_frames_); }
    uint32_t captured() const { return uint32_t(captured_); }

private:
    float *ring_ = nullptr;
    int ring_frames_ = 0;

    // How much has ever been written. Caps the loop length until the ring has filled once, so a
    // repeat cannot loop frames that were never captured.
    int captured_ = 0;
    int write_ = 0;

    bool held_ = false;
    bool pitch_down_ = false;

    // The loop, decided on the press and then fixed: `set` while held changes the length for the
    // NEXT press, not this one. Changing it under a running loop would move the start and jump
    // the read position, which is a click rather than a musical change.
    int start_ = 0;
    int length_ = 0;
    int offset_ = 0;
    uint32_t frac_ = 0;

    // What `set` asked for, waiting for the next press.
    int wanted_ = 0;
};

}  // namespace strata
