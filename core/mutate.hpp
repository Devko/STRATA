// Mutation: bounded, seeded variations of one track.
//
// docs/SPEC.md's performance table: *"Variations of the selected track: rotate, Euclidean
// density up or down, bounded randomizing of velocity, micro-timing, probability and notes in
// scale. Seeded and undoable. OK keeps it, CLR reverts."*
//
// ## Undo is the existing undo
//
// "OK keeps it, CLR reverts" needs no mechanism of its own. A mutation is one edit, so `app/`
// marks an undo level before applying it and CLR is the undo that already exists
// (core/undo.hpp). OK does nothing at all, which is the point: there is no provisional state to
// commit, and therefore no way for the provisional state and the real pattern to disagree. The
// cost is that a mutation spends one of the sixteen undo levels, which is what they are for.
//
// ## Seeded, and what that means here
//
// Every draw comes from the pattern's seed mixed with the track, the step, the kind of mutation
// and a `variant` the caller supplies -- never from a running RNG. core/scheduler.hpp takes the
// same approach and says why: the same inputs give the same pattern on every machine and after
// a reload, so a mutation can be recalled exactly and the tests are reproducible.
//
// `variant` is what makes pressing MUTATE twice give two different results. `app/` passes a
// counter; passing the same one again reproduces the same mutation on the same input, which is
// the property that makes "recalled exactly" true.
//
// ## Bounded means bounded
//
// `amount` is 0..100 and scales how far a value may move, and every result is clamped into the
// range the sequencer is allowed to assume -- the same ranges `step_sanitize` enforces. A
// mutation can make a pattern worse, which is the idea; it cannot make one the scheduler would
// read out of range.
//
// Nothing here allocates and nothing here touches the lock pool: a rotation moves whole `Step`
// values, so each step's `lock_first` travels with it and the pool is untouched. That is also
// why rotation cannot fail for want of pool space.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// What a mutation does. Live state, never serialized, so free to renumber.
enum class Mutation : uint8_t {
    rotate_left = 0,     // the range's steps move one place earlier, wrapping inside the range
    rotate_right,
    density_up,          // one more Euclidean hit across the range
    density_down,        // one fewer
    velocity,            // bounded randomizing of the trigged steps' velocity
    micro,               // ... of their micro-timing
    probability,         // ... of their probability
    notes,               // ... of their notes, by scale degree, staying in the key
    count
};

// A short upper-case name for the screen; the device font has no lower case.
const char *mutation_name(Mutation kind);

// How far each kind may move a value at `amount` 100. Stated here rather than buried in the
// implementation because they are a musical judgement, not a derivation, and they are the first
// thing anyone will want to change:
//
//   velocity     +/-48 of 1..127, so a mutation is clearly audible without flattening a part
//   micro        +/-11 ticks, the whole of kMicroRange: the most the grid allows
//   probability  +/-40 points of 0..100
//   notes        +/-3 scale degrees, which is a third or a fifth depending on where it lands
//
// At `amount` 0 every one of them is 0 and a mutation changes nothing, which makes the knob
// usable from "off" rather than from "already ruined".
constexpr int kMutateVelocitySpan = 48;
constexpr int kMutateMicroSpan = kMicroRange;
constexpr int kMutateProbabilitySpan = 40;
constexpr int kMutateNoteDegrees = 3;

// --- Euclidean ------------------------------------------------------------

// Is step `index` of `steps` a hit, for `hits` hits distributed as evenly as possible?
//
// Bjorklund's algorithm produces the canonical Euclidean rhythm, and it needs a loop and a
// scratch array. This is the standard closed form instead -- a hit where the running fraction
// crosses an integer -- which gives the same SET of hits for every (hits, steps) pair, rotated
// so that step 0 is always a hit. Rotation is a separate mutation anyway, and this way the
// function is pure arithmetic with no state and no allocation.
//
// Degenerate inputs are answered rather than refused: no hits means no hits, and hits >= steps
// means every step.
constexpr bool euclidean_hit(int index, int hits, int steps) {
    if (steps <= 0 || hits <= 0 || index < 0 || index >= steps) return false;
    if (hits >= steps) return true;
    // ((index * hits) mod steps) < hits is the crossing test. Both products fit easily: steps
    // is at most 64 and hits at most steps.
    return ((index * hits) % steps) < hits;
}

// How many of `steps` are hits -- always `hits`, clamped into 0..steps. For tests and for the
// UI's readout.
constexpr int euclidean_count(int hits, int steps) {
    if (steps <= 0 || hits <= 0) return 0;
    return hits >= steps ? steps : hits;
}

// --- the generator --------------------------------------------------------
//
// docs/SPEC.md, "Musical defaults that help creativity": *"Euclidean generator per track (hits,
// steps, rotation) as a starting point that you then edit by hand."* M6's, and the one part of
// it with no audio-path cost at all.
//
// It is the same arithmetic the density mutation uses, which is why it lives here rather than in
// a module of its own: `density_up` is this generator with `hits` one higher and `steps` fixed
// to the range. What the generator adds is the other two numbers.
//
// ## `steps` is a CYCLE, not the track's length
//
// Three hits in eight over a sixteen-step track is the eight-step figure played twice, which is
// the point of having both numbers: a 3-in-8 against a 4-in-16 is a polyrhythm, and typing the
// track length into both boxes would make that unreachable. A cycle longer than the track simply
// gets cut off at the end, which is what a performer would expect from asking for sixteen steps
// on an eight-step track.
//
// ## Where it runs, and what it costs
//
// `app/` applies this through the command ring like any other edit, so it runs in the AUDIO
// context -- once per press. It is not free: `euclidean_hit` takes a modulo per step and this
// chip has no integer divide instruction, so a 64-step track costs 64 software divides, on the
// order of a few thousand cycles. That is under 1% of one block, once, and it is the reason the
// rotation is normalised outside the loop rather than taking two more modulos per step.

// Write the Euclidean rhythm for `hits` in `steps`, rotated by `rotation`, over `track`'s trigs.
// Reports whether anything changed.
//
// `hits` is clamped into 0..steps and `steps` into 1..kStepsPerTrack; `rotation` may be any
// value and is reduced into the cycle, positive moving the figure LATER. A step that gains a
// trig inherits its note, velocity and length from the first trig already on the track, for the
// same reason the density mutation does: a new hit should sound like the part it joins rather
// than like a bare middle C.
//
// Only the trig bit is written. Locks, conditions, ratchets, micro-timing and probability are
// all left exactly as they were, so generating over a part you have already shaped replaces the
// RHYTHM and keeps the sound -- which is what "a starting point that you then edit by hand"
// means in the other direction too.
bool euclid_generate(Pattern &pattern, int track, int hits, int steps, int rotation);

// How many trigs `euclid_generate` will leave on a track of `length`, for the screen. A cycle
// shorter than the track repeats; one longer is cut off.
int euclid_trigs(int hits, int steps, int length);

// --- applying -------------------------------------------------------------

// Apply `kind` to `track`, steps `[first, first + count)`, and report whether anything changed.
//
// False, with the pattern untouched, for an index or a count that does not name steps on that
// track, and for a mutation that had nothing to work on -- a velocity mutation over a range with
// no trigs, say. "Nothing changed" is worth distinguishing from "done": `app/` should not spend
// an undo level on an edit that did nothing, and should say so rather than looking broken.
//
// `amount` is clamped into 0..100. `variant` separates one press from the next.
//
// The range is clamped to the track's own length, not to kStepsPerTrack: a 16-step track's
// steps 16..63 are not part of the pattern, and rotating them in would bring in steps the
// sequencer never plays.
bool mutate_apply(Pattern &pattern, int track, int first, int count, Mutation kind,
                  uint32_t variant, int amount);

// The draw a mutation uses, exposed because the tests check determinism directly and because
// `app/` draws the same numbers to preview a mutation on screen.
//
// Returns a value in `[-span, span]`, from the pattern seed mixed with everything that
// identifies this draw. Pure: the same arguments always give the same answer.
int mutate_draw(uint32_t seed, int track, int step, Mutation kind, uint32_t variant, int span);

}  // namespace strata
