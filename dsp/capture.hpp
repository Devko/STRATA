// Resampling's capture buffer: the finished master, recorded for a fixed number of frames.
//
// docs/SPEC.md's eighth performance tool: "Record the master or one track for 1, 2, 4 or 8 bars
// to `/RECORD` as WAV, then load it into a SAMPLE or GRAIN track in one action." This file is
// only the recording. The file goes to the card from the main context (app/storage.cpp) and the
// loading back is the sample loader that already exists.
//
// ## What it records, and from where
//
// The FINISHED master: `Engine::process` taps it after the mixer, after the limiter and after
// beat repeat, which is what comes out of the box. dsp/repeat.hpp makes the same argument for
// where it sits, and a resample that did not include the repeat would surprise anyone who had
// just played one into the take.
//
// ## int16, in the audio path
//
// The buffer holds interleaved stereo int16, converted as it is written, rather than float
// converted later in the main context. Two reasons, and the second is the real one:
//
//   * **Half the memory.** 30 s of stereo float is 11.5 MiB; int16 is 5.76 MiB. That matters
//     against docs/SPEC.md's budget, where the sample region is already 64 MiB.
//   * **The main context does no pass at all.** <enaga/wav.hpp> writes a 16-bit header and these
//     bytes are already the payload, so a save is `files::save` over the buffer -- no conversion
//     pass over 5.76 MiB in the UI thread, and no second buffer to hold the result of one.
//
// The cost is a multiply, a clamp and a convert per sample in the audio path, which is the same
// arithmetic dsp/sample.cpp's reader already does per tap and far less than one voice.
//
// ## Why the length arrives in FRAMES
//
// `start(frames)` takes a frame count, not a number of bars and a tempo. The bar maths needs the
// tempo and the pattern's master length, which live in app/engine.cpp where the clock is -- the
// same division of labour as the rest of dsp/ (CLAUDE.md: a hot path takes `dt`, and the caller
// converts at control rate where the tempo is known). It also makes every test here exact.
//
// ## Bar alignment is the CALLER's job
//
// A take that is not bar-aligned does not loop, which is the whole point of recording a round
// number of bars. So the sequence is `arm(frames)` from the command, then `begin()` from the
// engine on the next bar line, then `process()` every block. This file deliberately does not
// know what a bar is; it only knows how to be armed and then to run for exactly as long as it
// was told.
#pragma once

#include <cstdint>

namespace strata {

// How long a take can be, in seconds.
//
// 30 s, and not an hour, because of the OTHER end of the round trip: app/storage.hpp's loader
// sizes `raw_` and `src_` from `kMaxSampleSeconds`, which is 30, and a take longer than that
// could be written to the card and then not loaded back into a track. A resample that cannot be
// resampled would be a strange feature. docs/SPEC.md's 8 bars fits in it above 64 BPM; below
// that the take is clamped and `clamped()` says so, rather than being silently short.
constexpr float kCaptureSeconds = 30.0f;

class Capture {
public:
    // App::init only: takes the buffer. False if the allocation failed, which the app treats as
    // a failed init rather than carrying on without resampling.
    bool init(float sample_rate);

    // How many frames the buffer can hold, so the engine can report a clamp before it starts.
    uint32_t capacity() const { return capacity_; }

    // Ask for a take of `frames`, to begin at the next bar. Replaces any take that was armed but
    // not yet begun; refuses -- returning false and changing nothing -- while one is RUNNING, so
    // a second press cannot silently restart a take halfway through.
    //
    // `frames` is clamped to `capacity()`, and `clamped()` then reports it.
    bool arm(uint32_t frames);

    // Begin the armed take. The engine calls this on a bar line. No-op unless armed.
    void begin();

    // Throw away whatever is armed or running. A stop, a pattern change or a card removal.
    void cancel();

    // Append this block of the finished master. `master` is interleaved stereo, 2 * frames
    // floats, and is never modified -- a capture must not change what the user hears.
    //
    // The take ends by itself at the frame it was asked for, mid-block if that is where it
    // falls: the length is exact, because a loop that is a block too long clicks.
    void process(const float *master, int frames);

    // --- what the engine publishes and the card reads ---------------------

    bool armed() const { return state_ == State::armed; }
    bool recording() const { return state_ == State::recording; }

    // A finished take, waiting to be written. Stays true until `release()`.
    bool done() const { return state_ == State::done; }

    // True when the last `arm` asked for more than the buffer holds. Sticky until the next arm,
    // because the screen has to be able to say so after the take rather than during it.
    bool clamped() const { return clamped_; }

    uint32_t frames() const { return written_; }       // frames captured so far, or in the take
    uint32_t wanted() const { return wanted_; }        // frames the take will be, once begun

    // The take, as interleaved stereo int16: `2 * frames()` samples. Valid from the main context
    // only once `done()`, which is the same discipline a project save uses -- the audio side has
    // finished with it by then and nothing else writes it.
    const int16_t *data() const { return buffer_; }

    // The main context has finished with the take; the buffer is free to be armed again.
    void release();

private:
    enum class State : uint8_t { idle, armed, recording, done };

    int16_t *buffer_ = nullptr;
    uint32_t capacity_ = 0;       // frames
    uint32_t wanted_ = 0;         // frames this take should be
    uint32_t written_ = 0;        // frames written so far
    State state_ = State::idle;
    bool clamped_ = false;
};

}  // namespace strata
