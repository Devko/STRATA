// Tap tempo and the tempo nudge: the arithmetic behind two VALUE gestures, kept here so the host
// suite can drive it from synthetic millisecond stamps.
//
// ## Tap tempo
//
// A tap of VALUE while VALUE is on the tempo (nothing picked, not in PERFORM, not on PROJECT's
// CHAIN or PATTERN page) is a beat. Nothing is set until three taps agree: the first starts the
// count, the second gives one interval, and the third sets the tempo only when its interval is
// within kTapAgreePercent of the second's. Two stray taps (a hand letting go of VALUE twice) set
// and save nothing. A third tap that does not agree keeps only its own interval, so the next tap
// can agree with it. From then on the tempo is the average of the last few intervals, so one
// early or late tap moves it a little rather than throwing it. A gap longer than kTapTimeoutMs starts a new count, as does a tap
// that is less than half or more than twice the running average: that is a hand starting again
// at another tempo, and averaging it in would land between the two.
//
// The result is in tenths of a BPM, the unit the VALUE knob already turns
// (app/ui_gestures.hpp, `tempo_tenths_`), clamped to Enaga's 20-300.
//
// ## The nudge
//
// FUNC + push VALUE + turn bends the tempo for a moment and lets it go back, as a DJ pushes a record
// to bring it in line with another: each detent holds the tempo kNudgePermille above or below
// the set tempo for kNudgeMs from the last detent. Nothing is saved and the set tempo does not
// move, so the beat ends up a little earlier or later and the tempo is where it was. It is the
// only way to line STRATA up by ear with a source that sends no clock.
#pragma once

#include <cstdint>

namespace strata {

constexpr uint32_t kTapTimeoutMs = 2000;      // a longer gap starts a new count (30 BPM)
constexpr int kTapIntervals = 4;              // the average is over at most this many
constexpr uint32_t kTapAgreePercent = 25;     // the first two intervals agree within this
constexpr uint32_t kTempoTenthsMin = 200;     // Enaga's clock: 20-300 BPM
constexpr uint32_t kTempoTenthsMax = 3000;

struct TapTempo {
    uint32_t last_ms = 0;
    uint32_t intervals[kTapIntervals] = {};
    int count = 0;          // intervals held, 0 .. kTapIntervals
    int next = 0;           // where the next interval goes
    bool started = false;   // a first tap has been seen
};

// A tap at `now_ms`. Returns the tempo in tenths of a BPM once three taps in this count agree
// (above), 0 for a tap that only starts or builds one.
uint32_t tap_tempo(TapTempo &tap, uint32_t now_ms);

// Forget the count: the next tap is a first tap again.
void tap_reset(TapTempo &tap);

constexpr int kNudgePermille = 40;            // 4%: 130 BPM plays at 135.2 while held
constexpr uint32_t kNudgeMs = 250;            // from the last detent

struct Nudge {
    int direction = 0;      // -1 slower, +1 faster, 0 at rest
    uint32_t until_ms = 0;  // when it lets go
};

// A turn of `delta` detents at `now_ms`. The direction is the latest turn's; a turn the other
// way while one is held swaps it. A delta of 0 changes nothing.
void nudge_turn(Nudge &nudge, int delta, uint32_t now_ms);

// Has the nudge run out at `now_ms`? True exactly once per nudge: it is then at rest and the
// caller puts the set tempo back. Wrap-safe over the 49-day millisecond counter.
bool nudge_due(Nudge &nudge, uint32_t now_ms);

// The tempo to play while `direction` is held, in BPM, from the set tempo in tenths. Clamped to
// Enaga's range, so a nudge at 300 BPM plays 300 rather than a tempo the clock refuses.
float nudged_bpm(uint32_t tempo_tenths, int direction);

}  // namespace strata
