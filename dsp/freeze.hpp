// The delay freeze: while FUNC + MUTATE is held, whatever is echoing in the delay loops forever,
// and nothing new goes in.
//
// Enaga's StereoDelay caps its feedback at 0.95 and darkens every pass through its tone filter,
// so a freeze cannot be "feedback to 1". Instead, on the press the delay's feed is cut and the
// next echo period of its WET output is recorded (one delay time, two with ping-pong, which is
// when the pattern of echoes repeats); from then on that recording loops in place of the delay's
// output. Recording forward rather than keeping a history means an idle freeze costs nothing.
// The delay itself keeps running underneath, unfed, so on release it crossfades (5 ms) back to
// the delay's own tail, which has decayed naturally while the loop played.
//
// Hot loops in stoploops.ti.c.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kFreezeFadeFrames = 256;

class DelayFreeze {
public:
    // App::init only: a ring of `max_seconds` of stereo. False when it cannot.
    bool init(float sample_rate, float max_seconds);

    // Held, with the loop's length in frames for a press (clamped to the ring); a release of a
    // loop fades back, a release while still recording simply stops (the output was live).
    void hold(bool held, int loop_frames);

    // The delay's wet output for this block, as two planes: recorded, replaced or crossfaded.
    void process(float *wet_left, float *wet_right, int n);

    bool held() const { return held_; }
    // Anything to do this block: the mixer then routes the delay through the wet planes.
    bool active() const { return state_ != State::idle; }
    bool looping() const { return state_ == State::looping; }
    int loop_frames() const { return length_; }

private:
    enum class State : uint8_t { idle, recording, looping, releasing };

    float *ring_ = nullptr;
    int ring_frames_ = 0;
    State state_ = State::idle;
    bool held_ = false;
    int length_ = 0;
    int recorded_ = 0;
    int read_ = 0;
    float fade_ = 0.0f;
};

}  // namespace strata
