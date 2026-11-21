// Tick-to-step mapping: swing, per-track speed and length, and micro-timing.
//
// Enaga owns the clock (docs/REVIEW.md 1.3), so STRATA never counts time. What it does own is
// deciding, for a tick that has arrived, which steps of which tracks fire on it. That decision
// is pure integer arithmetic on the 96 PPQN grid, which is why it lives here in core/: no Enaga
// headers, no floats, fully host-testable.
//
// ## Why the mapping is inverted
//
// A step does not fire on its nominal tick. Swing pushes odd steps later, and micro-timing
// moves a step up to 11 ticks either way -- including EARLIER than its nominal tick. Walking
// forward from a step to its tick therefore cannot work: when tick t arrives we would already
// have missed a step whose nominal tick is t + 11.
//
// So the mapping runs backwards. For each arriving tick t, look at the handful of steps whose
// NOMINAL tick lies within the maximum possible shift of t, and ask which of them land exactly
// on t. The shift is bounded by swing (at most half a step) plus micro (11 ticks), so the window
// is small and the scan is a fixed, short loop with no division -- which is what the audio
// context needs.
#pragma once

#include <cstdint>

namespace strata {

// --- the grid -------------------------------------------------------------

constexpr int kPpqn = 96;                  // ENAGA_CLOCK_PPQN, repeated here so core/ stays
constexpr int kTicksPerSixteenth = kPpqn / 4;   // 24: the 1x step length
constexpr int kMaxSteps = 64;
constexpr int kMicroRange = 11;            // micro-timing is -11..+11 ticks

// Per-track speed. The values are serialized in a pattern, so they must never be renumbered
// once a release exists.
//
// x1 is deliberately 0 so that zeroed memory means normal speed. "Clear pattern" is a
// user-facing feature and the obvious implementation is a memset; with 2x at 0 that would
// silently double the tempo of every track. Zeroed memory should always mean the neutral
// setting, which is also why kSwingInherit is 0 in core/pattern.hpp.
//
// The numbering is therefore NOT the order the UI shows these in -- see kSpeedDisplayOrder.
enum class Speed : uint8_t {
    x1 = 0,       // 24 ticks: one 16th. The neutral default, so zeroed memory is sane
    x2 = 1,       // twice as fast: 12 ticks a step
    x3_4 = 2,     // 32 ticks
    x1_2 = 3,     // 48 ticks
    x1_4 = 4,     // 96 ticks: one quarter note
    x1_8 = 5,     // 192 ticks
    count = 6
};

// The order the spec lists for the UI: fastest to slowest. The encoder walks this, not the
// enum's own numbering.
constexpr Speed kSpeedDisplayOrder[int(Speed::count)] = {
    Speed::x2, Speed::x1, Speed::x3_4, Speed::x1_2, Speed::x1_4, Speed::x1_8
};

// Three-letter labels for the screen, indexed by the enum value (the font is upper case only).
constexpr const char *speed_name(Speed speed) {
    switch (speed) {
        case Speed::x1:   return "1";
        case Speed::x2:   return "2";
        case Speed::x3_4: return "3/4";
        case Speed::x1_2: return "1/2";
        case Speed::x1_4: return "1/4";
        case Speed::x1_8: return "1/8";
        default:          return "?";
    }
}

// Ticks between consecutive steps at a given speed. Exact integers at 96 PPQN, including 3/4.
constexpr int ticks_per_step(Speed speed) {
    switch (speed) {
        case Speed::x2:   return kTicksPerSixteenth / 2;        // 12
        case Speed::x1:   return kTicksPerSixteenth;            // 24
        case Speed::x3_4: return kTicksPerSixteenth * 4 / 3;    // 32
        case Speed::x1_2: return kTicksPerSixteenth * 2;        // 48
        case Speed::x1_4: return kTicksPerSixteenth * 4;        // 96
        case Speed::x1_8: return kTicksPerSixteenth * 8;        // 192
        default:          return kTicksPerSixteenth;
    }
}

// --- swing ----------------------------------------------------------------

constexpr int kSwingMin = 50;    // straight
constexpr int kSwingMax = 75;    // the spec's limit: odd steps half a step late

// How far swing pushes a step later, in ticks. Even steps never move; odd steps move by
// (swing - 50) / 50 of a step, so 75% is half a step. Integer arithmetic throughout, rounded
// to nearest, and it is monotonic in `swing`.
//
// At 1x this is the familiar 16th-note swing. At other speeds it shifts odd steps of that
// track's own grid, which is the only reading that generalises.
constexpr int swing_offset(int step_index, int swing, int step_ticks) {
    if ((step_index & 1) == 0) return 0;
    const int clamped = swing < kSwingMin ? kSwingMin : (swing > kSwingMax ? kSwingMax : swing);
    const int numerator = (clamped - kSwingMin) * step_ticks;
    return (numerator + 25) / 50;          // round to nearest; 50 is kSwingMax - kSwingMin x 2
}

// The largest distance any step can sit from its nominal tick, which sets the scan window.
constexpr int max_shift(int step_ticks) {
    return swing_offset(1, kSwingMax, step_ticks) + kMicroRange;
}

// --- the note-length table ------------------------------------------------
//
// docs/SPEC.md gives Step::length as an "index into a table: 1/16 step .. 64 steps" without
// publishing the table, so this is it. Lengths are held in SIXTEENTHS OF A STEP, which keeps
// every entry an exact integer and makes the table independent of the track's speed: a length
// of "one step" is one step whether the track runs at 1x or 1/8x.
//
// Index 0 is reserved as invalid so that a zeroed or corrupt byte is refusable rather than
// meaning the shortest possible note. Valid indices are 1..kLengthMax.
//
// Note on resolution: a sixteenth of a 24-tick step is 1.5 ticks, which the 96 PPQN grid cannot
// express. Gate length does not have to land on the tick grid though -- it only decides when a
// voice is released -- so the caller converts to SAMPLES at control rate, where the tempo is
// known. That is why this table deliberately does not offer a ticks conversion.

constexpr int kLengthMax = 20;                      // highest valid index
constexpr int kLengthTableSize = kLengthMax + 1;    // entry 0 is the invalid sentinel

// Sixteenths of a step. Musically useful values rather than a uniform ramp: the short end has
// the triplet-ish and dotted subdivisions a techno part wants, the long end doubles.
constexpr uint16_t kLengthSixteenthsOfStep[kLengthTableSize] = {
    0,       // 0: invalid
    1,       // 1/16 step
    2,       // 1/8 step
    3,       // 3/16 step
    4,       // 1/4 step
    6,       // 3/8 step
    8,       // 1/2 step
    12,      // 3/4 step
    16,      // 1 step
    24,      // 1.5 steps
    32,      // 2 steps
    48,      // 3 steps
    64,      // 4 steps
    96,      // 6 steps
    128,     // 8 steps
    192,     // 12 steps
    256,     // 16 steps: one bar at 1x
    384,     // 24 steps
    512,     // 32 steps
    768,     // 48 steps
    1024     // 64 steps: the whole of a full-length track
};

constexpr bool length_valid(uint8_t index) {
    return index >= 1 && index <= kLengthMax;
}

// Sixteenths of a step for a length index; 0 for an invalid index, so a caller that ignores
// length_valid() gets a silent note rather than an out-of-bounds read.
constexpr uint16_t length_sixteenths(uint8_t index) {
    return length_valid(index) ? kLengthSixteenthsOfStep[index] : uint16_t(0);
}

// The gate length in samples. Done at control rate, in float, where the tempo is known:
// sixteenths / 16 x step_ticks x samples_per_tick.
inline float length_samples(uint8_t index, int step_ticks, float samples_per_tick) {
    return float(length_sixteenths(index)) * (1.0f / 16.0f) * float(step_ticks) * samples_per_tick;
}

// --- a track's timing -----------------------------------------------------

struct TrackTiming {
    uint8_t length = 16;            // 1..64 steps, for polymeter
    Speed speed = Speed::x1;
    uint8_t swing = kSwingMin;      // 50..75

    int step_ticks() const { return ticks_per_step(speed); }

    // The track's loop length in ticks. Tracks of different lengths drift against each other,
    // which is the point of polymeter.
    int loop_ticks() const {
        const int steps = length < 1 ? 1 : (length > kMaxSteps ? kMaxSteps : length);
        return steps * step_ticks();
    }
};

// A step that fires on the tick being examined.
struct Firing {
    uint8_t step = 0;      // which step index of the track
    uint32_t loop = 0;     // which pass of the loop, for A:B conditions
};

constexpr int kMaxFiringsPerTick = 4;   // at 2x, a 23-tick window can hold at most 4 steps

// Which steps of one track fire exactly on `tick`.
//
// `micro` is the per-step micro-timing in ticks, indexed by step, or nullptr for none. Only
// entries below `timing.length` are read.
//
// Returns how many firings were written to `out` (at most `max`). The scan is a bounded loop
// over the few candidate steps near the tick, with no division in the inner path.
int steps_firing_at(uint32_t tick, const TrackTiming &timing, const int8_t *micro,
                    Firing *out, int max);

// The effective tick of one step within its loop pass, after swing and micro-timing. Exposed
// for tests and for the UI, which draws where a step actually sits.
int effective_tick(int step_index, const TrackTiming &timing, const int8_t *micro);

}  // namespace strata
