// The sequencer: turns arriving clock ticks into note events.
//
// This is where a Pattern meets the clock. Enaga owns time and core/timing owns which steps sit
// on which tick; the scheduler decides whether a step that lands actually sounds, and with what.
// It knows nothing about machines or audio -- it emits StepEvent, and app/ turns those into the
// triggers dsp/ understands. That keeps core/ portable and the whole thing host-testable.
//
// ## Order of resolution, per step that lands on a tick
//
//   1. trig state   -- off: nothing. trigless: locks apply, no note. on: carry on.
//   2. condition    -- A:B, FILL, PRE, NEI, 1ST and their negations (see below).
//   3. probability  -- 0-100%, drawn deterministically (see below).
//   4. ratchet      -- 1-8 hits spread across the step, with a velocity ramp.
//
// Condition is resolved BEFORE probability because PRE and NEI chain off the *condition*
// result: a step whose condition passed but whose probability roll failed still counts as a
// pass for the next step's PRE. That is how Elektron-style boxes behave and it is what makes
// PRE musically useful.
//
// ## Determinism
//
// Every random draw comes from the pattern's seed mixed with the track, step and loop number,
// never from a running RNG. So the same pattern plays the same way on every pass and after a
// reload, a mutation can be recalled exactly, and host tests are reproducible. docs/SPEC.md
// calls this out as a design principle: "Every random operation takes its seed from the
// pattern, so a mutation can be recalled exactly."
//
// ## What the caller owns
//
// The scheduler holds only per-track chaining state (the previous condition result, the loop a
// track is on). Pattern memory stays read-only to it. Nothing here allocates, so it is safe to
// call from audio().
#pragma once

#include "pattern.hpp"
#include "timing.hpp"

#include <cstdint>

namespace strata {

// --- what comes out -------------------------------------------------------

// One note to start. app/ converts this into dsp/'s Trig; core/ must not include dsp/.
struct StepEvent {
    uint8_t track = 0;
    uint8_t step = 0;            // which step produced it, for the UI and for locks
    uint8_t note = 60;
    uint8_t velocity = 100;      // 1-127, after any ratchet ramp
    uint8_t length_index = 8;    // index into core/timing's note-length table
    bool accent = false;
    bool slide = false;          // MONO tracks only
    bool trigless = false;       // apply locks, do not start a note
    uint32_t frame = 0;          // frame within the audio block: sample accuracy
    uint8_t ratchet_index = 0;   // 0 for the first hit of a step, then 1, 2, ...
    uint8_t ratchet_count = 1;   // how many hits this step produces in total
};

// How many events one tick can produce across all 8 tracks.
//
// The obvious bound -- one step per track times the ratchet maximum -- is WRONG, and the
// scheduler's tests caught it. core/timing can land up to kMaxFiringsPerTick steps of a single
// track on one tick: at 2x a step is 12 ticks and micro-timing reaches +/-11, so step 0 at
// micro +1 and step 1 at micro -11 both fall on the same tick. With a full ratchet on each,
// one tick can want 8 x 4 x 8 events.
//
// That only happens at 2x with extreme micro-timing, so the realistic figure is far lower, but
// a buffer sized for the easy case would truncate exactly when someone is doing something
// deliberately wild. 4 KiB of events is cheap insurance; the engine keeps it off the stack.
constexpr int kMaxEventsPerTick = kTracks * kMaxFiringsPerTick * kRatchetMax;

// --- what goes in ---------------------------------------------------------

// Live performance state the sequencer reads but does not own. The app fills this each block.
struct PerformState {
    bool fill = false;           // the FILL pad is held (or auto-fill is in its bar)
    // LIVE RND (core/liverand.hpp): the unit, 0 OFF .. 5 RND16, and the armed tracks. While the
    // unit is on, an armed track plays that RND mode over its own; the pattern is not touched.
    uint8_t rnd_unit = 0;
    uint8_t rnd_armed = 0xFF;
};

// --- the scheduler --------------------------------------------------------

class Scheduler {
public:
    // Forget all chaining state. Call on a pattern change, on stop, and at init.
    //
    // `first_loop` makes the next pass count as the first after a pattern change, which is what
    // the 1ST and NOT 1ST conditions test.
    void reset(bool first_loop = true);

    // Per-track timing, derived from the pattern. Call when the pattern or its lengths change,
    // not per tick: it resolves each track's swing against the pattern's and clamps lengths.
    //
    // `samples_per_tick` is how long one clock tick lasts at the current tempo
    // (sample_rate * 60 / (bpm * kPpqn)). A ratchet's hits are spread across the step in
    // SAMPLES, so that number has to come in from outside -- core/ does not know the tempo,
    // and tick() is handed a frame rather than a tempo. It is hoisted here into a per-track
    // step length, so call configure() again when the tempo changes: control-rate work, never
    // per tick. Left at zero, a ratchet still emits its full count with the right velocities,
    // but every hit lands on the step's own frame.
    void configure(const Pattern &pattern, float samples_per_tick = 0.0f);

    // Resolve one tick. Appends to `out`, returns how many events were written.
    //
    // `frame` is the frame within the current audio block that this tick falls on -- straight
    // from enaga::clock::ticks(), so events inherit sample accuracy. Micro-timing has already
    // been applied by core/timing in deciding that the step lands on this tick; `frame` is the
    // tick's own position in the block. A ratchet's later hits sit at `frame` plus an offset
    // inside the step, which can be past the end of the block: spreading them is the caller's
    // to defer, not the scheduler's to withhold.
    //
    // `max` is a hard limit: nothing is ever written past it. Truncation drops events from the
    // tail and nothing else -- every track is still resolved in full, so conditions, the
    // PRE/NEI chain and the playheads are the same whatever buffer size the caller passed. A
    // return value equal to `max` therefore means events MAY have been dropped, which is the
    // only signal a caller gets; a buffer of kMaxEventsPerTick makes it impossible for any
    // pattern that lands at most one step per track per tick. A null `out`, or a `max` of 0,
    // resolves the tick and writes nothing, which runs the sequencer silently.
    //
    // Safe to call from audio(): no allocation, no division in the inner path.
    int tick(uint32_t tick_number, uint32_t frame, const Pattern &pattern,
             const PerformState &perform, StepEvent *out, int max);

    // The playhead, for the UI: which step each track is currently on. Updated by tick().
    uint8_t playhead(int track) const;

    // Whether the last resolved condition on a track passed, for the UI to show PRE chains.
    bool last_condition(int track) const;

private:
    struct TrackState {
        TrackTiming timing;
        float step_samples = 0.0f;    // one step in samples, for spreading a ratchet
        uint8_t playhead = 0;
        bool last_condition = true;   // PRE on the first step of a fresh pattern reads as true
        // The loop pass this track was in when reset() ran: the window 1ST tests against. Not
        // the current pass -- swing and micro-timing let a late step of one pass fire after an
        // early step of the next, so the loop numbers that arrive are not monotonic and
        // "changed since last time" would end the 1ST window a pass too soon.
        uint32_t first_pass_loop = 0;
        bool seen_a_loop = false;     // whether first_pass_loop has been anchored yet
    };

    TrackState tracks_[kTracks];
    bool first_loop_ = true;
    uint32_t pattern_seed_ = 0;
};

// --- step stutter ---------------------------------------------------------

// Step stutter (hold REPEAT, then a STEP: docs/CONTROLS.md): the one hit track `track` makes
// each time the stutter retriggers step `step`. The engine calls it on the roll's grid while
// the scheduler runs on silently underneath, so on release every track is exactly where it
// would have been.
//
// A step past a short track's length wraps into it (step 13 of an 8-step track is its step 5),
// so the whole beat stutters at the same place in its loop. The step plays as written -- trig,
// note, velocity, length, accent, slide, and its locks, which a trigless step applies alone --
// but its condition, probability and ratchet are not drawn: the stutter IS the ratchet now, and
// a loop that skipped its own step would not loop. False, writing nothing, when that step has
// neither a trig nor a trigless.
bool stutter_event(const Pattern &pattern, int track, int step, uint32_t frame, StepEvent &out);

// --- the pieces, exposed for testing --------------------------------------

// A deterministic draw in 0..99 from the pattern seed and a step's identity. Pure: the same
// arguments always give the same number, which is what makes probability reproducible.
uint32_t probability_roll(uint32_t seed, int track, int step, uint32_t loop);

// Does a packed condition byte pass? `previous` is this track's last condition result (PRE),
// `neighbour` is the track to the left's (NEI), `loop` is the pass number for A:B, and
// `first_loop` is whether this is the first pass after a pattern change (1ST).
//
// An invalid condition byte reads as NONE, which always passes.
bool condition_passes(uint8_t packed, uint32_t loop, bool fill, bool previous, bool neighbour,
                      bool first_loop);

// The velocity of hit `index` of `count`, given the step's velocity and ramp direction.
// Always returns 1-127.
uint8_t ratchet_velocity(uint8_t velocity, uint8_t index, uint8_t count, RatchetRamp ramp);

// The frame offset of ratchet hit `index` of `count` within a step of `step_samples` samples.
// Hits are spread evenly, the first on the step itself.
uint32_t ratchet_frame(uint32_t index, uint32_t count, float step_samples);

}  // namespace strata
