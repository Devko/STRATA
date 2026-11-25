// Editing one field of a step with an encoder, and naming it for the screen.
//
// docs/SPEC.md, "Step editing": "Hold STEP + OK: open the step page (note, velocity, length,
// micro-timing / probability, condition, ratchet, accent and slide) on A-D." This is the table
// that turns an encoder delta into a legal value of a packed byte, and the value back into text.
//
// ## Why this is in core/ rather than in the UI
//
// Because it is where the silent bugs live. Step::condition is a PARTITIONED byte -- 0-9 are
// condition kinds and 16-50 are the 35 A:B pairs (core/pattern.hpp) -- so walking it with an
// encoder is not an increment. Step::ratchet packs a count and a ramp into one byte.
// Step::length is an index into a table whose entry 0 is deliberately invalid. Step::micro is
// signed and bounded. Every one of those is a place where an off-by-one produces a legal-looking
// byte with the wrong meaning: nothing crashes, the step just does something else. app/ cannot
// be host-tested (it is the only part that includes Enaga headers), so this logic belongs on
// the testable side of that line.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// The fields of the step page, in the order the UI shows them: A-D on page one, A-D on page two,
// and the flags on the third.
enum class StepField : uint8_t {
    note = 0,
    velocity,
    length,
    micro,
    probability,
    condition,
    ratchet_count,
    ratchet_ramp,
    accent,
    slide,
    chord,
    count
};

constexpr int kStepFields = int(StepField::count);

// A three- or four-character name for the encoder slot. Upper case: the font has no lower case.
const char *step_field_name(StepField field);

// The field's current value as a plain number, for a bar or a comparison. Not what to print --
// a condition's value is a position in the walk below, which means nothing on screen.
int step_field_value(const Step &step, StepField field);

// How many positions the field has, so the UI can draw a bar. 2 for a flag.
int step_field_positions(StepField field);

// Move a field by `delta` encoder clicks and store the result in `step`.
//
// Every field CLAMPS at its ends rather than wrapping, including the condition walk: an encoder
// that wraps round from NOT 1ST to NONE makes it impossible to stop at either end, and on
// hardware that is the difference between an editable field and a fight. The one exception is
// the ramp, which has three positions and is a genuine cycle.
//
// Ranges: note 0-127, velocity 1-127, length 1..kLengthMax (never 0, which is the invalid
// sentinel), micro -kMicroRange..+kMicroRange, probability 0-100, ratchet count
// kRatchetMin..kRatchetMax, accent and slide 0 or 1, chord a position in the walk of named chords (below).
void step_field_apply(Step &step, StepField field, int delta);

// --- the condition walk ---------------------------------------------------
//
// The encoder sees ONE ordered list of 44 positions, not a kind and a pair:
//
//   0            NONE
//   1 .. 8       FILL, NOT FILL, PRE, NOT PRE, NEI, NOT NEI, 1ST, NOT 1ST
//   9 .. 43      the 35 A:B pairs, in the order core/pattern.hpp documents:
//                1:2 2:2 1:3 2:3 3:3 1:4 .. 8:8
//
// The named conditions come FIRST because the walk clamps and 35 of the 44 positions are A:B
// pairs: with the pairs in front, FILL -- the condition a fill key exists for -- would be 36
// clicks away and NOT 1ST would be 43, which is not a field anyone can use on hardware. The
// eight named kinds are now one to eight clicks, 1:2 is nine, and the long tail of 5:7-ish
// pairs is the part that is rarely visited. This order is the ENCODER's alone: Condition's own
// numbering and the packed byte are unchanged, so no saved pattern depends on it.
//
// Condition::A_B does not get a position of its own: a bare A_B byte means 1:2, which is
// position 9. Reading a byte that no position produces (an unknown kind, an out-of-range pair)
// gives position 0, so a corrupt step reads as NONE -- the same rule condition_kind() uses.

constexpr int kConditionPositions = 44;

int condition_position(uint8_t packed);     // byte -> position
uint8_t condition_at_position(int position);   // position -> byte

// --- the chord walk -------------------------------------------------------
//
// The chord field's encoder positions: every valid packing of core/chord.hpp's table, in type
// order and then by inversion -- 0 is a single note, 1-3 MIN and its inversions, and so on.
// `chord_walk_position` reads a byte that is not a valid chord as position 0.
constexpr int kChordWalkPositions = 43;
int chord_walk_position(uint8_t packed);
uint8_t chord_at_walk_position(int position);

// --- hold STEP + upper pad: the cycles (D6, "STEP first edits") ---------
//
// An upper pad pressed while a STEP key is held steps one field of that step through a short
// cycle, so a condition, a ratchet or a probability is one press away (docs/CONTROLS.md):
//
//     FILL     the condition   FILL, NOT FILL, none
//     REPEAT   the ratchet     2, 3, 4, off (1)
//     MUTE     the condition   1:2, 2:2, 1:4, 4:4, none
//     MUTATE   probability     75, 50, 25, 100 %
//
// A value outside the cycle (a 1:3 set on the step page, a 60 %) goes to the cycle's first.
enum class StepCycle : uint8_t { fill, ratchet, a_b, probability };

StepField step_cycle_field(StepCycle cycle);

// The field's value after one press, in `step_field_value`'s terms (the condition as a walk
// position), from the step as it is.
int step_cycle_next(const Step &step, StepCycle cycle);

// --- text -----------------------------------------------------------------

// The buffer size `step_field_text` and `note_name` never need more than, terminator included:
// the longest text any field produces is eight characters ("NOT FILL", "3/16 STP"). A smaller
// buffer is safe -- the text is truncated, never overrun -- but it will show half a word, so a
// caller with no reason to be tighter should size its buffer with this.
constexpr int kStepFieldTextMax = 9;

// The field's value as text, written to `out` with a terminating 0 and never more than `max`
// bytes including it. Notes read as names ("C-3", "F#4", middle C = C-4 at note 60), conditions
// as "NONE", "1:4", "NOT PRE", lengths as "1 STP" or "1/4 STP", micro as "+3" or "-11",
// probability as "100%", the ramp as "UP", "DOWN" or "FLAT", flags as "ON" or "--".
void step_field_text(const Step &step, StepField field, char *out, int max);

// A note name on its own, for the grid and the live-record display.
void note_name(uint8_t note, char *out, int max);

}  // namespace strata
