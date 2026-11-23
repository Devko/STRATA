// Keeping a bar-aligned loop in time: the frozen tracks (dsp/frozen.hpp) and the master looper
// (dsp/looper.hpp) share this.
//
// A loop of `bars` bars (1, 2, 4 or 8) began on bar `origin`, at loop frame `lead`. Each bar
// line says where the read should be. On the loop's own first bar it is put back exactly there,
// as it always was: at a steady tempo that is where the read already is, so it is seamless.
//
// On the bars between, the read used to run free until the loop came round again. That left a
// loop out of time for up to seven bars after anything that moved the clock under it: a stop
// and start (the clock goes back to bar 0, and a loop that began on bar 5 has its next own bar
// at bar 5 again, or bar 1, depending on its length), a relocate, a tempo change. So on those
// bars the read is moved too, but only when it is clearly lost: further than kLoopLostFrames
// from where it belongs. Small differences, a frame or two from rounding at a steady tempo or
// the jitter of an external MIDI clock, are left to the loop's own bar as before, so a playing
// loop does not take a tiny jump on every bar.
//
// Control rate only (an integer remainder is a library call on the C674x), 32-bit (a 64-bit
// divide pulls __c6xabi_divull into the link, which fails it). A loop is at most 30 s, 1.44 M
// frames, so everything here fits an int32 with room to spare.
#pragma once

#include <cstdint>

namespace strata {

// 10 ms at 48 kHz: half a sixteenth at 140 BPM is 54 ms, so a read this far off is audibly out
// of time, and a clock's rounding or jitter never comes near it.
constexpr uint32_t kLoopLostFrames = 480;

// `x` modulo `n`, for any sign of x.
inline uint32_t loop_wrap(int32_t x, uint32_t n) {
    if (x >= 0) return uint32_t(x) % n;
    const uint32_t r = uint32_t(-x) % n;
    return r == 0u ? 0u : n - r;
}

// Where the read belongs at the start of a block whose bar line `bar` falls `frame` frames into
// it (negative: before it). `bars_mask` is bars - 1, a power of two less one, so a bar's share
// of the loop is a shift rather than a divide.
inline uint32_t loop_position(uint32_t frames, uint32_t bars_mask, uint32_t origin, uint32_t lead,
                              uint32_t bar, int32_t frame) {
    const uint32_t into = (bar - origin) & bars_mask;
    const uint32_t shift = bars_mask >= 7u ? 3u : (bars_mask >= 3u ? 2u : bars_mask);
    const uint32_t at = lead + ((frames * into) >> shift);
    return loop_wrap(int32_t(at) - frame, frames);
}

// The read after bar line `bar`: exact on the loop's own first bar, and elsewhere moved only
// when it is lost (the header above).
inline uint32_t loop_resync(uint32_t pos, uint32_t frames, uint32_t bars_mask, uint32_t origin,
                            uint32_t lead, uint32_t bar, int32_t frame) {
    const uint32_t want = loop_position(frames, bars_mask, origin, lead, bar, frame);
    if (((bar - origin) & bars_mask) == 0u) return want;
    const uint32_t d = want >= pos ? want - pos : pos - want;
    const uint32_t apart = d < frames - d ? d : frames - d;     // round the loop either way
    return apart > kLoopLostFrames ? want : pos;
}

}  // namespace strata
