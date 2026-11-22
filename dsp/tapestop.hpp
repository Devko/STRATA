// The tape stop: the finished master slows to a standstill while FUNC + push D is held, its pitch
// and level falling together like a deck losing power, and comes straight back on release.
//
// A short ring of the master is written while the effect runs, and read back from behind the
// write at a speed that falls from 1 to 0 over the stop's length (two beats, so it lands with the
// music). Read at speed s, the read falls behind by 1 - s a frame, so the furthest it ever lags
// is half the stop's length -- which is what sizes the ring. The press is seamless (speed 1, lag
// 0 reads the frame just written), the release is a 5 ms crossfade back to the live master, and
// in between nothing is skipped: the music carried on underneath and the release lands on it.
//
// Idle, it costs nothing: no capture runs, because the read never needs a frame from before the
// press. Hot loop in stoploops.ti.c.
#pragma once

#include <cstdint>

namespace strata {

// The longest stop, and the ring that follows from it (a lag of at most half of that, plus room).
constexpr float kTapeStopMaxSeconds = 2.0f;
constexpr float kTapeStopMinSeconds = 0.05f;
constexpr float kTapeStopRingSeconds = 0.5f * kTapeStopMaxSeconds + 0.25f;
// The release's crossfade back to the live master.
constexpr int kTapeStopFadeFrames = 256;

class TapeStop {
public:
    // App::init only: takes the ring from the heap. False when it cannot.
    bool init(float sample_rate);

    // Control rate: the stop's length in frames, clamped to kTapeStopMin/MaxSeconds. Applied from
    // the next press; a stop under way keeps the length it started with.
    void set(float stop_frames);

    // FUNC + push D down, or up.
    void hold(bool held);

    // On audio()'s interleaved stereo buffer, in place. Does nothing while idle.
    void process(float *interleaved, int frames);

    bool held() const { return held_; }
    bool running() const { return running_; }   // held, or fading back to the live master
    float speed() const { return speed_; }
    float stop_frames() const { return stop_frames_; }
    uint32_t ring_frames() const { return uint32_t(ring_frames_); }

private:
    float *ring_ = nullptr;
    int ring_frames_ = 0;
    float rate_ = 48000.0f;

    int write_ = 0;
    float lag_ = 0.0f;
    float speed_ = 1.0f;
    float speed_step_ = 0.0f;
    float wet_ = 0.0f;
    float stop_frames_ = 0.0f;
    bool held_ = false;
    bool running_ = false;
};

}  // namespace strata
