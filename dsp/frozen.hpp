// Freeze: a track that plays a loop recorded from itself instead of running its machine.
//
// Roland, 2026-11-23: CPU is the tight resource on the SmplTrek and RAM is not. A six-voice VA
// POLY chord is about 13 points of the block with cl6x (docs/BUDGET.md); played back from RAM it
// is one int16 read and a multiply-add a sample. So FREEZE, a row in the FUNC menu, records 1, 2,
// 4 or 8 bars of the selected track alone (the bounce tap, RESAMPLE's TRK source) and then plays
// that loop in place of the machine. UNFREEZE gives the machine back at once.
//
// ## What stays live
//
// The loop is the MACHINE's output: the bounce tap is after the insert chain, so a frozen loop
// would bake in the drive, filter and lo-fi -- and then run through them a second time. The
// engine therefore records a freeze BEFORE the chain (Engine::process), and the chain, fader,
// pan, sends and duck all stay live on a frozen track. Its notes, locks and machine knobs do
// nothing until it is unfrozen; that is what frozen means.
//
// ## Staying in time
//
// The take began on a bar line, so loop frame `lead` (the bar tick's frame in the block the take
// began in) is that bar. On every bar line where (bar - origin) is a multiple of the loop's
// length the read position is set back to exactly that frame, and between those bars it runs
// free a frame a sample, wrapping at the loop's end. At a steady tempo the reset lands where the
// read already was, so it is seamless. A read that is lost -- after a stop and start, a relocate
// or a tempo change -- jumps back into time on the next bar line of any kind, never stretched
// (dsp/loopsync.hpp): a stop and start puts the clock on bar 0, which may be a few bars from the
// loop's own. Stopped, the track fades out over one
// block and stays silent; the first tick after a start resyncs it (Engine::on_bar).
//
// Nothing here allocates: the loop lives in the sample pool's freeze slot for the track
// (core/samplepool.hpp, freeze_slot), filled by the main context before `set` is applied.
#pragma once

#include <cstdint>

namespace strata {

class FrozenLoop {
public:
    // Start playing `frames` of mono int16 at `data`, a loop of `bars` bars (1, 2, 4 or 8) whose
    // first bar line is `origin` (the engine's bar count) at loop frame `lead`. Takes effect at
    // once: the read position is put where the clock says it is now (`bar`, `frame` into the
    // current block). Refuses (false) a null buffer, an empty loop or a bar count that is not a
    // power of two up to 8.
    //
    // `lead` may be negative or past the end (a take that began mid-bar after a start): it is
    // taken modulo the loop.
    bool set(const int16_t *data, uint32_t frames, uint32_t bars, uint32_t origin, int32_t lead,
             uint32_t bar_now);
    void clear();
    bool on() const { return data_ != nullptr; }

    // A bar line `frame` frames into the coming block (negative: that many before it, the first
    // tick after a start landing mid-bar): the loop resyncs on its own bars.
    void on_bar(uint32_t bar, int32_t frame);

    // Add this block into `mono` (`frames` floats) and advance. `running` false fades to silence
    // over the block and holds there; true fades back in from silence over one block.
    void render_add(float *mono, int frames, bool running);
    // Advance without sounding, for a muted track, so it is still in time when unmuted.
    void skip(int frames);

    uint32_t position() const { return pos_; }
    uint32_t frames() const { return frames_; }

private:
    void advance(uint32_t frames);

    const int16_t *data_ = nullptr;
    uint32_t frames_ = 0;
    uint32_t bars_mask_ = 0;     // bars - 1
    uint32_t origin_ = 0;
    uint32_t lead_ = 0;
    uint32_t pos_ = 0;
    float gain_ = 0.0f;          // 0 or 1, ramped across a block when the transport moves
};

}  // namespace strata
