// Sample audition: hearing a file in the sample browser before it is loaded into a track.
//
// The backlog's "sample audition in the browser" (2026-11-25). Push A in SAMPLES plays the file
// under the cursor, whole and from its start, straight to the output; nothing about the track
// changes, and nothing is loaded into the sample pool. The file is STREAMED from the card, the
// same way a track's file too big for the pool is (app/storage.hpp, "streaming"), so a 50 MB stem
// starts as quickly as a hi-hat, costs no pool memory and needs no conversion pass first: the
// card side opens an Enaga `files::Stream` at the WAV's data, the engine reads its bytes into
// `fifo()` each block, converted to int16 on the way (core/pcm.hpp, Engine::top_up_audition),
// and this plays them.
//
// ## Rate
//
// A file is played at its own rate, as a streamed track's is: a 44.1 kHz file steps 0.91875
// frames an output frame, a 96 kHz one 2. The step arrives as a 32-bit Q8.24 number the card
// side worked out (it may divide; this side may not), and the read is the sampler's 4-point
// Catmull-Rom (dsp/sample.cpp). Load-time conversion uses the windowed sinc (core/pcm.hpp) because
// what it makes is kept; an audition is heard once and decides nothing but "this one?".
//
// ## Stereo, level, and where it goes
//
// Unlike a track, which sums its file to mono for its mono strip, an audition keeps the file's
// channels: it goes straight to the output, after the master effects, beat repeat and the tape
// stop, and after the RESAMPLE take's tap, so it is never recorded, looped or repeated. Its level
// is fixed, about -6 dB under full scale, so a normalised file sits under a beat rather than on
// top of it; the sum is clamped to full scale.
//
// ## When the card is late
//
// Before each block the player asks whether the fifo holds every tap the block will read. If not,
// the block is silent and the position HOLDS, so a slow card delays the audition rather than
// skipping a piece of it -- the opposite of a streamed track, which has to keep time with the beat.
// `late()` counts such blocks.
//
// ## Ending
//
// Past the file's last frame the player stops by itself; `stop()` instead fades out over one block
// (1.3 ms), so a cut is not a click. Either way `finished()` goes true once, and the engine then
// stops reading the stream and tells the card side it may close it (Status::audition_epoch).
//
// Audio context only, apart from construction. Fixed size, no allocation (the fifo is 16 KiB).
#pragma once

#include "streamfifo.hpp"

#include <cstdint>

namespace strata {

class Audition {
public:
    // Q8.24: the step between the file frames read for two output frames.
    static constexpr uint32_t kStepOne = 1u << 24;
    // The rates a step is made for, as the loader's (app/storage.hpp, kMaxSampleSourceRate).
    static constexpr uint32_t kMinRate = 8000;
    static constexpr uint32_t kMaxRate = 96000;
    // About -6 dB. A stereo file's L and R each go out at this; a mono one's to both.
    static constexpr float kGain = 0.5f;

    // The step for a file at `rate` played at `output_rate`: rate / output_rate in Q8.24, or 0 for
    // a rate outside kMinRate..kMaxRate (or a zero output rate). Main context: it divides.
    static uint32_t step_for(uint32_t rate, uint32_t output_rate);

    // Begin `total` frames of `channels` (1 or 2) at `step` (Q8.24) from frame 0; the fifo is
    // emptied and expects frame 0 next. False, leaving it stopped, for 0 frames, a channel count
    // other than 1 or 2, or a step of 0 or more than 4.
    bool start(uint32_t total, int channels, uint32_t step);
    // Fade out over the next block, then finish. Nothing when it is not playing.
    void stop();

    bool playing() const { return on_; }
    // True once after the audition ended (its last frame, or a stop's fade); `take_finished()`
    // reads and clears it.
    bool take_finished() {
        const bool was = finished_;
        finished_ = false;
        return was;
    }

    // Where the reading is, in file frames, and how long the file is: for the screen.
    uint32_t position() const { return pos_; }
    uint32_t total() const { return total_; }
    // Blocks held back because the card had not delivered yet. Only ever grows.
    uint32_t late() const { return late_; }

    // The engine fills this from the stream until next_frame() reaches total().
    StreamFifo &fifo() { return fifo_; }
    const StreamFifo &fifo() const { return fifo_; }
    // Still frames of the file to come from the card?
    bool wants_frames() const { return on_ && fifo_.next_frame() < total_; }

    // Add `frames` (at most 64) interleaved stereo frames into `out` and advance.
    void render_add(float *out, int frames);

private:
    StreamFifo fifo_;
    bool on_ = false;
    bool finished_ = false;
    bool fading_ = false;
    int channels_ = 1;
    uint32_t total_ = 0;
    uint32_t pos_ = 0;       // the integer frame
    uint32_t frac_ = 0;      // and the fraction, Q24
    uint32_t istep_ = 0;
    uint32_t fstep_ = 0;
    uint32_t late_ = 0;
};

}  // namespace strata
