// The step page's encoder table: a delta into a legal packed byte, and the byte back into text.
//
// Two decisions shape this file.
//
// **Every field is a walk over positions, and the walk is the only thing that writes a step.**
// The ranges live once, in kRanges, so `step_field_positions` counts exactly the values
// `step_field_apply` can reach -- a bar drawn in 20 segments for a field the encoder can only
// move through 19 of is the kind of disagreement nobody notices until the hardware feels wrong.
// Fields whose byte is not its own value (the condition's partitioned byte, the ratchet's two
// nibbles) convert to a position first and walk there, so the clamping is written once rather
// than once per packing.
//
// **A starting byte is never trusted.** These entry points are reachable from a loaded pattern,
// and serialize.cpp's CRC does not stop a hand-edited file, so every walk clamps its RESULT
// into range rather than assuming its input was in range. An edit of a corrupt field therefore
// repairs it, which is the behaviour a user can actually recover from: the encoder moves and the
// value becomes legal, instead of the encoder moving and nothing changing.
//
// There is no printf and no libm here (CLAUDE.md), so the text is written a character at a time
// through a bounded writer. The callers pass small stack buffers, where one byte past the end is
// a stack smash that the host build will not notice and the unit will.
#include "stepedit.hpp"

#include "chord.hpp"

#include "timing.hpp"

namespace strata {

namespace {

// --- the two kinds of walk ------------------------------------------------

// Move `value` by `delta` within [lo, hi], clamping at both ends.
//
// The delta is compared with the span BEFORE it is added: a field spans at most 128 positions
// while `delta` is an int, so `value + delta` can overflow where `delta >= hi - lo` cannot, and
// a delta that big has reached the end anyway. The result is clamped unconditionally, which is
// what makes a corrupt starting value legal again after one click.
int walk(int value, int delta, int lo, int hi) {
    if (delta >= hi - lo) return hi;
    if (delta <= lo - hi) return lo;
    const int moved = value + delta;
    return moved < lo ? lo : (moved > hi ? hi : moved);
}

// Move `value` by `delta` over `count` positions that genuinely cycle. Only the ratchet ramp
// uses this: three settings with no natural ends, where clamping would cost a user a wasted
// click at each end for nothing. `value` may be a corrupt nibble, so it is folded first.
int cycle(int value, int delta, int count) {
    const int step = delta % count;      // a 32-bit divide, at control rate, outside any loop
    const int base = value % count;
    return (base + step + count) % count;
}

// The RTCH field's position: the count 1-8, then 9-16 for the rolls over LEN at each rate. A
// corrupt count reads as itself, so the walk clamps it back on the first click.
int ratchet_position(uint8_t packed) {
    return ratchet_spans(packed) ? int(kRatchetMax) + 1 + int(ratchet_rate(packed))
                                 : int(ratchet_count(packed));
}

// RatchetRamp has no `count` member, so the length of its walk is written here.
constexpr int kRatchetRampPositions = 3;
static_assert(int(RatchetRamp::down) == kRatchetRampPositions - 1,
              "a ramp shape was added: extend the ramp walk and ramp_text()");

// --- the ranges -----------------------------------------------------------

// The header's "Ranges:" paragraph, in the one place both the encoder and the bar read it from.
// `lo`/`hi` are POSITIONS, which for most fields is also the stored value; for the condition it
// is an index into the walk below, and for the ramp it is the RatchetRamp value.
struct Range {
    int lo;
    int hi;
};

constexpr Range kRanges[] = {
    {0, 127},                        // note
    {1, 127},                        // velocity: 0 is not a quiet note, it is a missing one
    {1, kLengthMax},                 // length: index 0 is timing.hpp's invalid sentinel
    {-kMicroRange, kMicroRange},     // micro
    {0, 100},                        // probability, per cent
    {0, kConditionPositions - 1},    // condition: a position in the walk, not a byte
    // ratchet: counts 1-8 (one hit is the unratcheted step), then the eight roll rates over LEN
    {kRatchetMin, kRatchetMax + kRatchetRates},
    {0, kRatchetRampPositions - 1},  // ramp: cycles rather than clamps
    {0, 1},                          // accent
    {0, 1},                          // slide
    {0, kChordWalkPositions - 1},    // chord: a position in the walk of named chords
};
static_assert(sizeof(kRanges) / sizeof(kRanges[0]) == kStepFields,
              "kRanges must have one entry per StepField, in the enum's order");

// Three and four letter labels. The font is upper case and digits only.
constexpr const char *kFieldNames[] = {
    "NOTE", "VEL", "LEN", "MICR", "PROB", "COND", "RTCH", "RAMP", "ACNT", "SLID", "CHRD",
};
static_assert(sizeof(kFieldNames) / sizeof(kFieldNames[0]) == kStepFields,
              "every StepField needs a label");

// --- the chord walk -------------------------------------------------------
//
// The chord byte packs a type and an inversion (core/chord.hpp), and most of 0..63 is not a
// chord: a type id this build does not have, or an inversion deeper than the chord has notes.
// The encoder used to walk the raw byte, so two clicks in three landed on bytes that play a
// single note and showed as a bare number. It walks the valid encodings instead, in type order
// and then by inversion, and the screen shows the chord's name. A byte a later build wrote is
// left exactly as it is until the knob is turned.

namespace {
constexpr int chord_walk_length() {
    int n = 0;
    for (int t = 0; t < kChordTypeCount; t++) n += chord_max_inversion(ChordType(t)) + 1;
    return n;
}
static_assert(chord_walk_length() == kChordWalkPositions,
              "stepedit.hpp's kChordWalkPositions must count core/chord.hpp's table");
}  // namespace

uint8_t chord_at(int position) {
    int left = position < 0 ? 0 : (position >= kChordWalkPositions ? kChordWalkPositions - 1
                                                                    : position);
    for (int t = 0; t < kChordTypeCount; t++) {
        const int inversions = chord_max_inversion(ChordType(t)) + 1;
        if (left < inversions) return chord_pack(ChordType(t), uint8_t(left));
        left -= inversions;
    }
    return 0;
}

int chord_position_of(uint8_t packed) {
    if (!chord_valid(packed)) return 0;
    const int type = int(chord_type(packed));
    int position = 0;
    for (int t = 0; t < type; t++) position += chord_max_inversion(ChordType(t)) + 1;
    return position + int(chord_inversion(packed));
}

// --- the condition walk ---------------------------------------------------

// Positions 1..8: the kinds that are neither NONE nor A:B, in the order the header lists, which
// is the order of the spec's own "Trig conditions" list. Both directions of the walk read this
// table, so the order exists once. These happen to be bytes 2..9 in sequence today, but that is
// Condition's numbering and not this walk's promise.
constexpr Condition kConditionKindWalk[] = {
    Condition::FILL, Condition::NOT_FILL, Condition::PRE,   Condition::NOT_PRE,
    Condition::NEI,  Condition::NOT_NEI,  Condition::FIRST, Condition::NOT_FIRST,
};
constexpr int kConditionKindWalkCount =
    int(sizeof(kConditionKindWalk) / sizeof(kConditionKindWalk[0]));

// The 35 A:B pairs run to the end of the walk, behind NONE and the eight named kinds.
constexpr int kConditionAbFirst = 1 + kConditionKindWalkCount;   // 9
static_assert(kConditionAbFirst + int(kConditionAbPairs) == kConditionPositions,
              "the condition walk must reach every kind and every pair exactly once");

// --- bounded text ---------------------------------------------------------

// `max` counts the terminator, so the last byte of the caller's buffer is reserved from the
// start and a text that does not fit is truncated instead of written past the end. A max of 0
// or a null buffer writes nothing at all, including the terminator -- there is nowhere to put
// it -- which is the only case where the result is not a usable string.
class Writer {
public:
    Writer(char *out, int max) : out_((out != nullptr && max > 0) ? out : nullptr),
                                 room_(out_ != nullptr ? max - 1 : 0) {}

    void put(char c) {
        if (used_ < room_) out_[used_++] = c;
    }

    void text(const char *s) {
        while (*s != '\0') put(*s++);
    }

    void number(uint32_t value) {
        char digits[10];
        int n = 0;
        do {
            digits[n++] = char('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (n > 0) put(digits[--n]);
    }

    // The byte this writes was reserved by the constructor, so it is always inside the buffer.
    void finish() {
        if (out_ != nullptr) out_[used_] = '\0';
    }

private:
    char *out_;
    int room_;
    int used_ = 0;
};

// The tracker spelling the header specifies: two characters for the note, so a natural pads
// with '-' ("C-4") where a sharp uses its own mark ("F#4"), then the octave. Middle C is note
// 60 = C-4, which puts notes 0..11 in octave -1 -- the one octave whose number needs two
// characters, "C--1". It reads oddly but it is unambiguous, and a one-character spelling would
// collide with octave 1.
constexpr const char *kNoteLetters[] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-",
};
static_assert(sizeof(kNoteLetters) / sizeof(kNoteLetters[0]) == 12, "twelve semitones");

void write_note(Writer &w, uint8_t note) {
    const int n = int(note & 0x7Fu);   // notes are 7 bits; fold a stray high bit as sanitize does
    w.text(kNoteLetters[n % 12]);
    const int octave = n / 12 - 1;
    if (octave < 0) {
        w.put('-');
        w.number(uint32_t(-octave));
    } else {
        w.number(uint32_t(octave));
    }
}

// The note-length table of core/timing.hpp, spelled out. The fractions are of a STEP, which is
// what keeps the table independent of the track's speed, so "1/4 STP" is a quarter of this
// track's step and not a quarter note. Entry 0 is the invalid sentinel and is never reached
// through length_valid(); it is kept so the table lines up with kLengthSixteenthsOfStep index
// for index.
constexpr const char *kLengthText[] = {
    "?",                                                                 // 0: invalid
    "1/16 STP", "1/8 STP", "3/16 STP", "1/4 STP", "3/8 STP", "1/2 STP",  // 1..6
    "3/4 STP",  "1 STP",   "1.5 STP",  "2 STP",   "3 STP",   "4 STP",    // 7..12
    "6 STP",    "8 STP",   "12 STP",   "16 STP",  "24 STP",  "32 STP",   // 13..18
    "48 STP",   "64 STP",                                                // 19..20
};
static_assert(sizeof(kLengthText) / sizeof(kLengthText[0]) == kLengthTableSize,
              "the length labels must match kLengthSixteenthsOfStep entry for entry");

constexpr int text_length(const char *s) {
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

constexpr bool every_label_fits() {
    for (const char *s : kLengthText) {
        if (text_length(s) + 1 > kStepFieldTextMax) return false;
    }
    for (const char *s : kFieldNames) {
        if (text_length(s) + 1 > 5) return false;   // "a three- or four-character name"
    }
    // The condition names are in a switch rather than a table; these are the long ones.
    return text_length("NOT FILL") + 1 <= kStepFieldTextMax &&
           text_length("NOT 1ST") + 1 <= kStepFieldTextMax;
}
static_assert(every_label_fits(), "a label outgrew kStepFieldTextMax: raise it in the header");

const char *length_text(uint8_t index) {
    return length_valid(index) ? kLengthText[index] : "?";
}

// A ramp nibble holds sixteen values and only three of them are a ramp, so a corrupt step says
// so on screen rather than claiming to be flat.
const char *ramp_text(uint8_t packed) {
    switch (ratchet_ramp(packed)) {
        case RatchetRamp::none: return "FLAT";
        case RatchetRamp::up:   return "UP";
        case RatchetRamp::down: return "DOWN";
    }
    return "?";
}

const char *condition_kind_text(Condition kind) {
    switch (kind) {
        case Condition::NONE:      return "NONE";
        case Condition::A_B:       return "A:B";   // never printed: the pair path handles it
        case Condition::FILL:      return "FILL";
        case Condition::NOT_FILL:  return "NOT FILL";
        case Condition::PRE:       return "PRE";
        case Condition::NOT_PRE:   return "NOT PRE";
        case Condition::NEI:       return "NEI";
        case Condition::NOT_NEI:   return "NOT NEI";
        case Condition::FIRST:     return "1ST";
        case Condition::NOT_FIRST: return "NOT 1ST";
    }
    return "NONE";
}

void write_condition(Writer &w, uint8_t packed) {
    const Condition kind = condition_kind(packed);
    if (kind == Condition::A_B) {
        // A and B are 1..8, one digit each, and condition_a/condition_b already read the bare
        // A_B byte as 1:2.
        w.put(char('0' + condition_a(packed)));
        w.put(':');
        w.put(char('0' + condition_b(packed)));
        return;
    }
    w.text(condition_kind_text(kind));
}

// Micro-timing is the only signed field and the sign is the whole point -- "+3" and "-3" are
// opposite edits -- so everything but zero carries one.
void write_signed(Writer &w, int value) {
    if (value > 0) w.put('+');
    if (value < 0) w.put('-');
    w.number(uint32_t(value < 0 ? -value : value));
}

// --- flags ----------------------------------------------------------------

int flag_value(const Step &step, uint8_t bit) {
    return (step.flags & bit) != 0 ? 1 : 0;
}

// Only the one bit moves: the trig, trigless and the other flag share this byte, and an accent
// edit that cleared the trig would silence the step.
void set_flag(Step &step, uint8_t bit, bool on) {
    step.flags = uint8_t(on ? uint8_t(step.flags | bit) : uint8_t(step.flags & uint8_t(~bit)));
}

}  // namespace

// --- names, values and sizes ----------------------------------------------

const char *step_field_name(StepField field) {
    const int index = int(field);
    return (index >= 0 && index < kStepFields) ? kFieldNames[index] : "?";
}

int step_field_value(const Step &step, StepField field) {
    switch (field) {
        case StepField::note:          return int(step.note);
        case StepField::velocity:      return int(step.velocity);
        case StepField::length:        return int(step.length);
        case StepField::micro:         return int(step.micro);
        case StepField::probability:   return int(step.probability);
        case StepField::condition:     return condition_position(step.condition);
        case StepField::ratchet_count: return ratchet_position(step.ratchet);
        case StepField::ratchet_ramp:  return int(uint8_t(ratchet_ramp(step.ratchet)));
        case StepField::accent:        return flag_value(step, kStepAccent);
        case StepField::slide:         return flag_value(step, kStepSlide);
        case StepField::chord:         return chord_position_of(step.chord);
        case StepField::count:
        default:                       return 0;
    }
}

int step_field_positions(StepField field) {
    const int index = int(field);
    if (index < 0 || index >= kStepFields) return 0;
    return kRanges[index].hi - kRanges[index].lo + 1;
}

// --- the edit -------------------------------------------------------------

void step_field_apply(Step &step, StepField field, int delta) {
    const int index = int(field);
    if (index < 0 || index >= kStepFields) return;
    const Range range = kRanges[index];

    switch (field) {
        case StepField::note:
            step.note = uint8_t(walk(int(step.note), delta, range.lo, range.hi));
            break;
        case StepField::velocity:
            step.velocity = uint8_t(walk(int(step.velocity), delta, range.lo, range.hi));
            break;
        case StepField::length:
            step.length = uint8_t(walk(int(step.length), delta, range.lo, range.hi));
            break;
        case StepField::micro:
            step.micro = int8_t(walk(int(step.micro), delta, range.lo, range.hi));
            break;
        case StepField::probability:
            step.probability = uint8_t(walk(int(step.probability), delta, range.lo, range.hi));
            break;
        case StepField::condition:
            step.condition = condition_at_position(
                walk(condition_position(step.condition), delta, range.lo, range.hi));
            break;
        case StepField::ratchet_count: {
            const int at = walk(ratchet_position(step.ratchet), delta, range.lo, range.hi);
            // Write the count or the rate and the span bit alone. The ramp lives in bits 4-6 and
            // an edit of either must leave the other byte-for-byte alone, a corrupt value
            // included: repairing it behind the user's back would move a field they are not
            // touching. Past 8 the walk goes on into the rolls over LEN (core/pattern.hpp).
            const uint8_t ramp_bits = uint8_t(step.ratchet & 0x70u);
            step.ratchet = at <= int(kRatchetMax)
                               ? uint8_t(ramp_bits | uint8_t(at & 0x0F))
                               : uint8_t(ramp_bits | kRatchetSpan |
                                         uint8_t((at - int(kRatchetMax) - 1) & 0x07));
            break;
        }
        case StepField::ratchet_ramp: {
            const int ramp = cycle(int(uint8_t(ratchet_ramp(step.ratchet))), delta,
                                   kRatchetRampPositions);
            step.ratchet = uint8_t(uint8_t(step.ratchet & 0x8Fu) | uint8_t(ramp << 4));
            break;
        }
        case StepField::accent:
            set_flag(step, kStepAccent,
                     walk(flag_value(step, kStepAccent), delta, range.lo, range.hi) != 0);
            break;
        case StepField::slide:
            set_flag(step, kStepSlide,
                     walk(flag_value(step, kStepSlide), delta, range.lo, range.hi) != 0);
            break;
        case StepField::chord:
            // A zero delta writes nothing, so a byte this build cannot name survives a glance.
            if (delta != 0) {
                step.chord =
                    chord_at(walk(chord_position_of(step.chord), delta, range.lo, range.hi));
            }
            break;
        case StepField::count:
        default:
            break;
    }
}

// --- the condition walk ---------------------------------------------------

int condition_position(uint8_t packed) {
    if (packed == uint8_t(Condition::NONE)) return 0;
    // The bare A_B byte is 1:2, which is the first pair's position. A writer never emits it
    // (condition_pack turns A_B into the 1:2 pair) but a hand-made file can hold it, so it is
    // read.
    if (packed == uint8_t(Condition::A_B)) return kConditionAbFirst;
    if (packed >= kConditionAbBase && packed < kConditionAbBase + int(kConditionAbPairs)) {
        return kConditionAbFirst + int(packed - kConditionAbBase);
    }
    for (int i = 0; i < kConditionKindWalkCount; i++) {
        if (packed == uint8_t(kConditionKindWalk[i])) return 1 + i;
    }
    return 0;   // an unknown kind or an out-of-range pair reads as NONE, like condition_kind()
}

uint8_t condition_at_position(int position) {
    // Clamping rather than refusing keeps the promise that this never returns an invalid byte,
    // which is what lets step_field_apply write the result straight into the step.
    const int p = position < 0 ? 0
                              : (position >= kConditionPositions ? kConditionPositions - 1
                                                                 : position);
    if (p == 0) return condition_pack(Condition::NONE);
    if (p < kConditionAbFirst) return condition_pack(kConditionKindWalk[p - 1]);
    return uint8_t(kConditionAbBase + (p - kConditionAbFirst));
}

// --- text -----------------------------------------------------------------

void step_field_text(const Step &step, StepField field, char *out, int max) {
    Writer w(out, max);
    switch (field) {
        case StepField::note:
            write_note(w, step.note);
            break;
        case StepField::velocity:
            w.number(uint32_t(step.velocity));
            break;
        case StepField::length:
            w.text(length_text(step.length));
            break;
        case StepField::micro:
            write_signed(w, int(step.micro));
            break;
        case StepField::probability:
            w.number(uint32_t(step.probability));
            w.put('%');
            break;
        case StepField::condition:
            write_condition(w, step.condition);
            break;
        case StepField::ratchet_count: {
            const uint8_t hits = ratchet_count(step.ratchet);
            if (ratchet_spans(step.ratchet)) {
                // ROLL/32 .. ROLL/16T: the rate's name without its "1".
                w.text("ROLL");
                w.text(ratchet_rate_name(ratchet_rate(step.ratchet)) + 1);
            } else if (hits < kRatchetMin || hits > kRatchetMax) {
                w.put('?');
            } else {
                w.number(uint32_t(hits));
            }
            break;
        }
        case StepField::ratchet_ramp:
            w.text(ramp_text(step.ratchet));
            break;
        case StepField::accent:
            w.text(flag_value(step, kStepAccent) != 0 ? "ON" : "--");
            break;
        case StepField::slide:
            w.text(flag_value(step, kStepSlide) != 0 ? "ON" : "--");
            break;
        case StepField::chord: {
            if (!chord_valid(step.chord)) {
                w.put('?');
                break;
            }
            // The name, and "/n" for an inversion. "5TH OCT/2" would be nine characters, so a
            // name that would not fit with its inversion drops its spaces ("5THOCT/2").
            const char *name = chord_name(chord_type(step.chord));
            const uint8_t inversion = chord_inversion(step.chord);
            int length = 0;
            while (name[length] != '\0') length++;
            const bool squeeze = inversion != 0 && length + 2 > kStepFieldTextMax - 1;
            for (const char *c = name; *c != '\0'; c++) {
                if (squeeze && *c == ' ') continue;
                w.put(*c);
            }
            if (inversion != 0) {
                w.put('/');
                w.number(uint32_t(inversion));
            }
            break;
        }
        case StepField::count:
        default:
            // A field id this build does not know. Say so rather than draw an empty slot.
            w.put('?');
            break;
    }
    w.finish();
}

void note_name(uint8_t note, char *out, int max) {
    Writer w(out, max);
    write_note(w, note);
    w.finish();
}

uint8_t chord_at_walk_position(int position) { return chord_at(position); }

// --- hold STEP + upper pad -----------------------------------------------------

StepField step_cycle_field(StepCycle cycle) {
    switch (cycle) {
        case StepCycle::ratchet:     return StepField::ratchet_count;
        case StepCycle::probability: return StepField::probability;
        case StepCycle::fill:
        case StepCycle::a_b:
        default:                     return StepField::condition;
    }
}

namespace {

// The value after `now` in `cycle`, or the cycle's first when `now` is not in it.
int next_in(const int *cycle, int count, int now) {
    for (int i = 0; i < count; i++) {
        if (cycle[i] == now) return cycle[(i + 1) % count];
    }
    return cycle[0];
}

}  // namespace

int step_cycle_next(const Step &step, StepCycle cycle) {
    switch (cycle) {
        case StepCycle::fill: {
            const int cycle_positions[] = {
                condition_position(condition_pack(Condition::FILL)),
                condition_position(condition_pack(Condition::NOT_FILL)),
                condition_position(condition_pack(Condition::NONE)),
            };
            return next_in(cycle_positions, 3, condition_position(step.condition));
        }
        case StepCycle::a_b: {
            const int cycle_positions[] = {
                condition_position(condition_pack_a_b(1, 2)),
                condition_position(condition_pack_a_b(2, 2)),
                condition_position(condition_pack_a_b(1, 4)),
                condition_position(condition_pack_a_b(4, 4)),
                condition_position(condition_pack(Condition::NONE)),
            };
            return next_in(cycle_positions, 5, condition_position(step.condition));
        }
        case StepCycle::ratchet: {
            const int counts[] = {2, 3, 4, 1};
            return next_in(counts, 4, ratchet_position(step.ratchet));
        }
        case StepCycle::probability:
        default: {
            const int percents[] = {75, 50, 25, 100};
            return next_in(percents, 4, int(step.probability));
        }
    }
}
int chord_walk_position(uint8_t packed) { return chord_position_of(packed); }

}  // namespace strata
