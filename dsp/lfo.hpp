// The per-track LFO: docs/SPEC.md, "One LFO per track (sine, triangle, saw, square, sample and
// hold), free or tempo-synced, routable to any of the 16 parameters."
//
// ## Four bytes, like every other parameter
//
// The LFO's settings are parameters 16-19 of the track (dsp/track.hpp, `Page::lfo`): the kit
// stores them, a step can lock them, a scene stores and morphs them and a macro can drive them,
// all through the code that already does that for the 16 sound parameters. What each byte means:
//
//     16  WAV  the shape: SIN TRI RMP SAW SQR S+H, six bands of the 0-127 byte. RMP rises and
//              SAW falls, which is what stands in for a signed depth: an inverted sine or
//              triangle is the same wave half a cycle on, so only the ramp needs both.
//     17  RAT  the rate. 0-63 run FREE, 0.0625 Hz to 32 Hz, an octave every 7 counts; 64-127
//              are SYNCED to the clock, sixteen note lengths from 8 bars to a 1/32, four counts
//              each. One knob rather than a switch and a knob because a page has four encoders.
//     18  DEP  how far: 127 swings the destination about 64 counts either side of its value.
//     19  DST  which parameter: OFF, then the 16 sound parameters (1-16), seventeen bands.
//
// **Zeroed bytes are OFF** (DST 0, DEP 0), so a kit or a scene written before the LFO existed
// sounds exactly as it did, and a track with its LFO off costs one compare a block. That is what
// keeps every render test bit-exact.
//
// ## Block rate
//
// The LFO is stepped once per audio block, not per sample, and moves its destination's BYTE.
// That is the same path a knob turn or a lock takes (the machine re-prepares from the byte), so it
// adds no per-sample work at all; the cost is one `prepare` of the modulated track per block in
// which the byte actually changed, which docs/BUDGET.md measures. The price is resolution: the
// destination moves in 0-127 steps at about 750 updates a second, which is fine for a filter
// sweep or a pan and audible as steps on a slow, deep pitch sweep.
//
// ## Phase
//
// A free LFO runs on its own and never resets. A synced one takes its phase from the clock's
// tick count (`Engine` passes it), so it lines up with the bar wherever the song is, follows an
// external clock, and starts on its zero crossing when the transport starts at tick 0. While the
// transport is stopped the engine keeps counting at the tempo, so a synced LFO keeps moving.
#pragma once

#include "track.hpp"

#include <enaga/dsp.hpp>

#include <cstdint>

namespace strata {

// The four LFO bytes, as indices into TrackParams.
constexpr int kLfoWave = kParams + 0;
constexpr int kLfoRate = kParams + 1;
constexpr int kLfoDepth = kParams + 2;
constexpr int kLfoDest = kParams + 3;

// Shapes, in the order the WAV byte walks them.
enum class LfoWave : uint8_t { sine = 0, triangle, ramp, saw, square, sample_hold, count };
constexpr int kLfoWaves = int(LfoWave::count);

// The RAT byte below this is free running; at or above it, synced.
constexpr uint8_t kLfoSyncFirst = 64;
constexpr int kLfoSyncDivisions = 16;

// The bands, as integer arithmetic (no divide): WAV in six, DST in seventeen.
constexpr LfoWave lfo_wave(uint8_t byte) {
    return LfoWave((int(byte > 127 ? 127 : byte) * kLfoWaves) >> 7);
}
// -1 for OFF, else the sound parameter 0-15.
constexpr int lfo_dest(uint8_t byte) {
    return ((int(byte > 127 ? 127 : byte) * (kParams + 1)) >> 7) - 1;
}
// The smallest byte in a band, which is what a test or the factory writes to pick it.
constexpr uint8_t lfo_wave_byte(LfoWave wave) {
    return uint8_t((int(wave) * 128 + kLfoWaves - 1) / kLfoWaves);
}
constexpr uint8_t lfo_dest_byte(int param) {
    return param < 0 ? uint8_t(0) : uint8_t(((param + 1) * 128 + kParams) / (kParams + 1));
}

// How many encoder clicks one band is worth, so one click on WAV or DST is one shape or one
// destination rather than a seventh of one. app/ scales a click by this.
constexpr int lfo_click(int index) {
    return index == kLfoWave ? 22 : (index == kLfoDest ? 8 : 1);
}

// One synced cycle in clock ticks (96 PPQN), longest first; 0 for a free-running byte.
uint32_t lfo_sync_ticks(uint8_t rate);

// A free-running byte's frequency, in Hz. Meaningless for a synced byte.
float lfo_free_hz(uint8_t rate);

// For the screen, at most five characters each: "SIN", "1/4T", "2B", "0.06", "OFF".
const char *lfo_wave_name(LfoWave wave);
const char *lfo_sync_name(uint8_t rate);
const char *lfo_param_name(int index);          // "WAV" "RAT" "DEP" "DST" for 16-19
// The rate in hundredths of a hertz, rounded, for the screen to print.
uint32_t lfo_free_centi_hz(uint8_t rate);

// The destination byte moved by the LFO: `base` plus `value` (-1..1) times DEP's share of 64
// counts, rounded to nearest and clamped into 0-127.
uint8_t lfo_apply(uint8_t base, float value, uint8_t depth);

// The four bytes resolved, at control rate, only when they change.
struct LfoSettings {
    LfoWave wave = LfoWave::sine;
    uint32_t sync_ticks = 0;        // 0: free
    float inv_sync_ticks = 0.0f;    // 1 / sync_ticks, so the block never divides
    float hz = 0.0f;
    uint8_t depth = 0;
    int dest = -1;                  // -1: off

    bool on() const { return dest >= 0 && depth != 0; }

    static LfoSettings make(const uint8_t bytes[kLfoParams]);
};

// One track's oscillator. Wraps Enaga's `Lfo` (sine, triangle, saw, square, S&H) and adds the
// falling ramp.
class TrackLfo {
public:
    // A distinct seed per track keeps eight S&H tracks from moving together.
    void init(uint32_t seed);

    // Free running: advance by `cycles` (Hz x seconds) and return the value, -1..1.
    float advance(LfoWave wave, float cycles);

    // Synced: move to `phase` (0..1) and return the value. A move backwards counts as a wrap,
    // so S&H draws a new value each time a synced cycle starts.
    float seek(LfoWave wave, float phase);

    float phase() const { return lfo_.phase(); }

private:
    float shaped(LfoWave wave, float raw) const;
    static enaga::dsp::LfoShape enaga_shape(LfoWave wave);

    enaga::dsp::Lfo lfo_;
    enaga::dsp::Random random_;
};

}  // namespace strata
