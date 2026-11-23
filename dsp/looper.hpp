// The master looper: slot 16 of the performance mixer (core/perffx.hpp).
//
// Roland, 2026-11-23, after modbang's Octatrack Performance Mixer, whose sixteenth scene is a
// master looper: record a few bars of the whole mix from the bar line, crossfade to the loop,
// change things underneath, and fade back. Here:
//
//   push VALUE + STEP 16   records the finished master from the next bar line, 1, 2, 4 or 8
//                          bars (the FUNC menu's LOOP LEN), then plays it. Again: a new take.
//   VALUE                  with the looper picked, the mix: 0 live ... 127 only the loop.
//
// Picking another effect leaves the loop playing at its mix, and the effect goes over it
// (dsp/masterfx.hpp sits after this), so a loop can be filtered or stuttered as on the template.
//
// ## Where it sits, and what it records
//
// On audio()'s interleaved stereo buffer, after the mixer and BEFORE the performance effects,
// beat repeat and the tape stop: it records the mix as the kit makes it, not a stutter that
// happened to be held, and it plays back under whatever is turned on next.
//
// ## Its buffer is RESAMPLE's
//
// The take is int16 stereo in dsp/capture.hpp's buffer (30 s: 8 bars down to 64 BPM), lent by
// the engine, which keeps the two apart: no RESAMPLE while a loop is held, and no loop while a
// take is armed, running or waiting for the card. So the looper costs no RAM of its own: the
// heap is 109 of 120 MiB with an 8 MiB margin (docs/BUDGET.md), and a second 5.76 MiB buffer
// would not fit beside it. Not saved with the project, like freeze.
//
// ## Staying in time
//
// As dsp/frozen.hpp: the take began on a bar line, so loop frame `lead` is that bar, and on every
// bar where (bar - origin) is a multiple of the loop's bars the read is put back exactly there,
// and on any other bar a read that is lost is moved to where it belongs (dsp/loopsync.hpp).
// Stopped, the loop fades out over a block and stays silent; the first bar after a start
// brings it back in time.
#pragma once

#include <cstdint>

namespace strata {

class Looper {
public:
    enum class State : uint8_t { idle, armed, recording, playing };

    // App::init: the buffer it records into, `capacity` frames of interleaved int16 stereo.
    void attach(int16_t *buffer, uint32_t capacity);

    // Record `frames` (exactly `bars` bars, 1, 2, 4 or 8) from the next bar line. Replaces a loop
    // that is playing; refuses (false, nothing changes) no buffer, an empty or oversized take, a
    // bar count that is not a power of two up to 8, or a take already recording.
    //
    // A loop that is playing goes on playing, at its mix and in time, until the new take begins,
    // and fades out over that first block: the buffer is not written before the bar line, so a
    // new take neither drops the loop a bar early nor cuts it with a click.
    bool arm(uint32_t frames, uint32_t bars);

    // Throw the loop away (or the take armed or running). The buffer is free again.
    void clear();

    // A bar line `frame` frames into the coming block (negative: before it). Starts an armed
    // take on this block, and keeps a playing loop in time.
    void on_bar(uint32_t bar, int32_t frame);

    // On audio()'s interleaved stereo buffer, in place. Recording: the block is written to the
    // take and the output left alone. Playing: crossfaded to the loop by `mix` (0-1), ramped from
    // the last block's, and faded out while `running` is false.
    void process(float *interleaved, int frames, float mix, bool running);

    State state() const { return state_; }
    bool holding() const { return state_ != State::idle; }   // the buffer is the looper's
    bool playing() const { return state_ == State::playing; }
    uint32_t capacity() const { return capacity_; }
    uint32_t frames() const { return frames_; }
    uint32_t recorded() const { return recorded_; }
    uint32_t position() const { return pos_; }
    // The take's bars while one is armed, else the loop's.
    uint32_t bars() const { return (state_ == State::armed ? take_bars_mask_ : bars_mask_) + 1u; }
    float mix() const { return mix_; }

private:
    void advance(uint32_t frames);
    // The playing loop crossfaded in by `mix_`, ramped to `target` over the block.
    void play(float *interleaved, int n, float target);
    // The loop a new take replaces, faded out over the take's first block: read before the
    // take writes over it.
    void fade_out_outgoing(float *interleaved, int n);

    int16_t *buffer_ = nullptr;
    uint32_t capacity_ = 0;
    State state_ = State::idle;
    uint32_t frames_ = 0;        // the take's length
    uint32_t recorded_ = 0;
    uint32_t bars_mask_ = 0;
    uint32_t origin_ = 0;        // the bar the take began on
    uint32_t lead_ = 0;          // the loop frame that is that bar line
    uint32_t pos_ = 0;
    float mix_ = 0.0f;           // where the crossfade is, after the transport fade
    // The take armed: its length and bars, which become the loop's at the bar line.
    uint32_t take_frames_ = 0;
    uint32_t take_bars_mask_ = 0;
    // A loop is still playing under the armed take (and, for the take's first block, fading).
    bool outgoing_ = false;
    uint32_t out_frames_ = 0;
    uint32_t out_pos_ = 0;
};

}  // namespace strata
