// Tests for the step page's encoder table in core/stepedit.
//
// This module exists because its bugs are silent. Every field here is a packed byte whose
// off-by-one produces another legal byte with a different meaning: a condition that becomes
// 2:3 instead of 1:3, a length index one short of the entry the screen names, a ratchet edit
// that wipes the ramp sharing its byte. Nothing crashes and nothing looks wrong -- the step
// just does something else, on hardware, days later. So the tests here are mostly exhaustive
// sweeps rather than examples: every one of the 44 condition positions, every byte 0..255 of
// every field, every text with a guard byte behind the buffer.
//
// Sweeps record their first counterexample instead of checking inside the loop, so a failure
// names the byte that broke it and the check count stays a count of invariants.
#include "check.hpp"

#include "scale.hpp"
#include "stepedit.hpp"
#include "chord.hpp"
#include "timing.hpp"

#include <cstring>

using namespace strata;

namespace {

constexpr StepField kFields[] = {
    StepField::note,     StepField::velocity,      StepField::length,
    StepField::micro,    StepField::probability,   StepField::condition,
    StepField::ratchet_count, StepField::ratchet_ramp, StepField::accent,
    StepField::slide,    StepField::chord,
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == kStepFields, "a field is missing here");

// The documented layout of the condition walk: NONE, then the eight named kinds, then the 35
// A:B pairs. These are written out rather than derived from the header's constants, because
// pinning the actual numbers is the whole point -- a walk that quietly puts the pairs back in
// front would still satisfy any arithmetic relation between them.
constexpr int kFirstNamedKind = 1;
constexpr int kNamedKindCount = 8;
constexpr int kFirstPair = 9;

// The encoder deltas worth hammering with: one click either way, a fast turn, and a delta far
// past the end of every field (which is also where `value + delta` would overflow if the walk
// added before it clamped).
constexpr int kDeltas[] = {1, -1, 7, -7, 1000, -1000};
constexpr int kDeltaCount = int(sizeof(kDeltas) / sizeof(kDeltas[0]));

// The first counterexample a sweep found. Checking it afterwards costs one check and still
// prints the offending byte.
struct Witness {
    int subject = -1;
    int detail = 0;

    void saw(bool bad, int subject_value, int detail_value = 0) {
        if (bad && subject < 0) {
            subject = subject_value;
            detail = detail_value;
        }
    }
};

// Point a field's underlying byte at `raw` without going through step_field_apply, so the
// sweeps start from bytes a hand-edited file could hold and not only from legal ones.
void set_raw(Step &step, StepField field, uint8_t raw) {
    switch (field) {
        case StepField::note:          step.note = raw; break;
        case StepField::velocity:      step.velocity = raw; break;
        case StepField::length:        step.length = raw; break;
        case StepField::micro:         step.micro = int8_t(raw); break;
        case StepField::probability:   step.probability = raw; break;
        case StepField::condition:     step.condition = raw; break;
        case StepField::ratchet_count:
        case StepField::ratchet_ramp:  step.ratchet = raw; break;
        case StepField::accent:
        case StepField::slide:         step.flags = raw; break;
        case StepField::chord:         step.chord = raw; break;
        case StepField::count:
        default:                       break;
    }
}

// What the sequencer is allowed to assume about the field that was just edited. The other
// fields are deliberately not checked: a sweep sets one byte raw and leaves the rest alone.
bool field_in_range(const Step &step, StepField field) {
    switch (field) {
        case StepField::note:        return step.note <= 127;
        case StepField::velocity:    return step.velocity >= 1 && step.velocity <= 127;
        case StepField::length:      return length_valid(step.length);
        case StepField::micro:       return step.micro >= -kMicroRange && step.micro <= kMicroRange;
        case StepField::probability: return step.probability <= 100;
        case StepField::condition:   return condition_valid(step.condition);
        case StepField::ratchet_count: {
            const uint8_t hits = ratchet_count(step.ratchet);
            return hits >= kRatchetMin && hits <= kRatchetMax;
        }
        case StepField::ratchet_ramp:
            return uint8_t(ratchet_ramp(step.ratchet)) <= uint8_t(RatchetRamp::down);
        case StepField::accent:
        case StepField::slide:       return true;    // one bit is always in range
        case StepField::chord:       return chord_valid(step.chord);
        case StepField::count:
        default:                     return true;
    }
}

// True when `after` differs from `before` only in the byte `field` owns. A table of per-field
// cases is exactly where a copy-paste writes velocity into the length slot, and on screen the
// two fields sit next to each other looking fine.
bool only_its_own_byte_changed(const Step &before, const Step &after, StepField field) {
    Step patched = before;
    switch (field) {
        case StepField::note:          patched.note = after.note; break;
        case StepField::velocity:      patched.velocity = after.velocity; break;
        case StepField::length:        patched.length = after.length; break;
        case StepField::micro:         patched.micro = after.micro; break;
        case StepField::probability:   patched.probability = after.probability; break;
        case StepField::condition:     patched.condition = after.condition; break;
        case StepField::ratchet_count:
        case StepField::ratchet_ramp:  patched.ratchet = after.ratchet; break;
        case StepField::accent:
        case StepField::slide:         patched.flags = after.flags; break;
        case StepField::chord:         patched.chord = after.chord; break;
        case StepField::count:
        default:                       break;
    }
    return std::memcmp(&patched, &after, sizeof(Step)) == 0;
}

// Walk a field from one end to the other, one click at a time, and count the distinct values
// it passes through.
int distinct_values_in_a_full_walk(StepField field) {
    Step step{};
    step_field_apply(step, field, -1000);          // park at the bottom (the ramp cycles)

    int values[256];
    int found = 0;
    const int clicks = step_field_positions(field) + 8;   // a few clicks past the top
    for (int i = 0; i < clicks; i++) {
        const int value = step_field_value(step, field);
        bool known = false;
        for (int k = 0; k < found; k++) {
            if (values[k] == value) known = true;
        }
        if (!known && found < int(sizeof(values) / sizeof(values[0]))) values[found++] = value;
        step_field_apply(step, field, 1);
    }
    return found;
}

// A buffer with a guard pattern behind it, so "never writes past max" is a fact and not a hope.
struct Guarded {
    static constexpr char kGuard = '\x7F';
    static constexpr int kSize = 40;
    char buf[kSize];

    void arm() {
        for (int i = 0; i < kSize; i++) buf[i] = kGuard;
    }
    bool guard_intact(int max) const {
        for (int i = max < 0 ? 0 : max; i < kSize; i++) {
            if (buf[i] != kGuard) return false;
        }
        return true;
    }
    // The terminator has to be inside the first `max` bytes for this to be a string at all.
    bool terminated(int max) const {
        for (int i = 0; i < max && i < kSize; i++) {
            if (buf[i] == '\0') return true;
        }
        return false;
    }
};

// Every character the labels are allowed to use. The font is upper case and digits, so a lower
// case letter would print as something else entirely, and a stray control byte as rubbish.
bool printable_label_char(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == ' ' || c == '-' || c == '+' || c == '#' || c == ':' || c == '%' || c == '/' ||
           c == '.' || c == '?';
}

bool label_is_clean(const char *text) {
    for (int i = 0; text[i] != '\0'; i++) {
        if (!printable_label_char(text[i])) return false;
    }
    return true;
}

// "1/4 STP", "1.5 STP" and "64 STP" must mean the same thing as the entry they name in
// kLengthSixteenthsOfStep, or the screen names one note length while the sequencer plays
// another. One parser covers all three spellings: digits, then either '/' and a divisor or '.'
// and a decimal. Returns -1 if the text is not a length at all.
int sixteenths_from_text(const char *text) {
    int numerator = 0;
    int denominator = 1;
    int i = 0;
    if (text[i] < '0' || text[i] > '9') return -1;
    while (text[i] >= '0' && text[i] <= '9') numerator = numerator * 10 + (text[i++] - '0');
    if (text[i] == '/') {
        i++;
        denominator = 0;
        while (text[i] >= '0' && text[i] <= '9') denominator = denominator * 10 + (text[i++] - '0');
    } else if (text[i] == '.') {
        i++;
        while (text[i] >= '0' && text[i] <= '9') {
            numerator = numerator * 10 + (text[i++] - '0');
            denominator *= 10;
        }
    }
    if (denominator == 0) return -1;
    if (std::strcmp(text + i, " STP") != 0) return -1;
    const int sixteenths = 16 * numerator;
    return (sixteenths % denominator) == 0 ? sixteenths / denominator : -1;
}

}  // namespace

// --- the field table ------------------------------------------------------

TEST(every_field_has_a_short_upper_case_label) {
    Witness bad_length;
    Witness unclean;
    Witness duplicate;

    for (int i = 0; i < kStepFields; i++) {
        const char *name = step_field_name(kFields[i]);
        CHECK(name != nullptr);
        const int length = int(std::strlen(name));
        bad_length.saw(length < 3 || length > 4, i, length);
        unclean.saw(!label_is_clean(name), i);
        for (int k = 0; k < i; k++) {
            duplicate.saw(std::strcmp(name, step_field_name(kFields[k])) == 0, i, k);
        }
    }
    // "A three- or four-character name", upper case, and one per slot: two fields sharing a
    // label would leave the user unable to tell which encoder they are turning.
    CHECK_EQ(bad_length.subject, -1);
    CHECK_EQ(unclean.subject, -1);
    CHECK_EQ(duplicate.subject, -1);

    // An id this build does not know must still return a string the screen can draw.
    CHECK(step_field_name(StepField::count) != nullptr);
    CHECK(step_field_name(StepField(99)) != nullptr);
}

TEST(step_field_positions_matches_a_full_walk_of_the_field) {
    // If the encoder can only reach 19 lengths, a bar drawn in 20 segments lies, and the last
    // segment is one the user can never select.
    Witness mismatch;
    for (int i = 0; i < kStepFields; i++) {
        const int claimed = step_field_positions(kFields[i]);
        const int reached = distinct_values_in_a_full_walk(kFields[i]);
        mismatch.saw(claimed != reached, i, reached);
        CHECK_EQ(claimed, reached);
    }
    CHECK_EQ(mismatch.subject, -1);

    CHECK_EQ(step_field_positions(StepField::condition), kConditionPositions);
    CHECK_EQ(step_field_positions(StepField::accent), 2);     // "2 for a flag"
    CHECK_EQ(step_field_positions(StepField::slide), 2);
    CHECK_EQ(step_field_positions(StepField::count), 0);
    CHECK_EQ(step_field_positions(StepField(99)), 0);
}

TEST(the_documented_ranges_are_the_ends_of_the_walk) {
    // The header's "Ranges:" paragraph, pinned: a field that clamps one short at either end is
    // a value the user can see in the spec and never reach.
    struct Ends {
        StepField field;
        int low;
        int high;
    };
    constexpr Ends kEnds[] = {
        {StepField::note, 0, 127},
        {StepField::velocity, 1, 127},
        {StepField::length, 1, kLengthMax},
        {StepField::micro, -kMicroRange, kMicroRange},
        {StepField::probability, 0, 100},
        {StepField::condition, 0, kConditionPositions - 1},
        {StepField::ratchet_count, kRatchetMin, kRatchetMax},
        {StepField::accent, 0, 1},
        {StepField::slide, 0, 1},
        {StepField::chord, 0, kChordWalkPositions - 1},
    };

    for (const Ends &ends : kEnds) {
        Step step{};
        step_field_apply(step, ends.field, -1000);
        CHECK_EQ(step_field_value(step, ends.field), ends.low);
        step_field_apply(step, ends.field, 1000);
        CHECK_EQ(step_field_value(step, ends.field), ends.high);
        CHECK_EQ(step_field_positions(ends.field), ends.high - ends.low + 1);
    }
}

// --- the condition walk ---------------------------------------------------

TEST(every_condition_position_round_trips) {
    Witness not_valid;
    Witness not_round_trip;
    Witness duplicate;
    uint8_t bytes[kConditionPositions];

    for (int p = 0; p < kConditionPositions; p++) {
        const uint8_t packed = condition_at_position(p);
        bytes[p] = packed;
        not_valid.saw(!condition_valid(packed), p, packed);
        not_round_trip.saw(condition_position(packed) != p, p, condition_position(packed));
        for (int k = 0; k < p; k++) {
            duplicate.saw(bytes[k] == packed, p, k);
        }
    }
    CHECK_EQ(not_valid.subject, -1);
    CHECK_EQ(not_round_trip.subject, -1);
    CHECK_EQ(duplicate.subject, -1);      // 44 positions must be 44 different conditions

    // Out of range clamps, so the byte is always one the sequencer can read.
    CHECK_EQ(condition_at_position(-1), condition_pack(Condition::NONE));
    CHECK_EQ(condition_at_position(-10000), condition_pack(Condition::NONE));
    CHECK_EQ(condition_at_position(kConditionPositions), bytes[kConditionPositions - 1]);
    CHECK_EQ(condition_at_position(10000), bytes[kConditionPositions - 1]);
}

TEST(the_bare_a_b_byte_reads_as_the_1_2_position) {
    // pattern.hpp: "condition_pack(Condition::A_B) produces 1:2, so a writer never emits the
    // bare 1; readers accept it because a hand-made file might." The walk has no position of
    // its own for A_B, so both spellings have to land on the position that holds 1:2.
    CHECK_EQ(condition_position(condition_pack(Condition::A_B)), kFirstPair);
    CHECK_EQ(condition_position(uint8_t(Condition::A_B)), kFirstPair);

    const uint8_t at_first_pair = condition_at_position(kFirstPair);
    CHECK_EQ(at_first_pair, condition_pack_a_b(1, 2));
    CHECK_EQ(at_first_pair, condition_pack(Condition::A_B));
    CHECK_EQ(condition_a(at_first_pair), 1);
    CHECK_EQ(condition_b(at_first_pair), 2);

    // And the walk itself never writes the bare byte, so a file written here is the packed form.
    Witness bare;
    for (int p = 0; p < kConditionPositions; p++) {
        bare.saw(condition_at_position(p) == uint8_t(Condition::A_B), p);
    }
    CHECK_EQ(bare.subject, -1);
}

TEST(condition_pairs_are_the_order_pattern_hpp_documents) {
    // "1:2 2:2 1:3 2:3 3:3 1:4 .. 8:8", at positions 9..43, behind the named kinds. A shift
    // here silently renames every A:B condition the user can select.
    int position = kFirstPair;
    Witness wrong_a;
    Witness wrong_b;
    for (int b = kConditionBMin; b <= kConditionBMax; b++) {
        for (int a = 1; a <= b; a++) {
            const uint8_t packed = condition_at_position(position);
            wrong_a.saw(int(condition_a(packed)) != a, position, int(condition_a(packed)));
            wrong_b.saw(int(condition_b(packed)) != b, position, int(condition_b(packed)));
            // The same pair, reached the other way round, must be the same byte.
            wrong_a.saw(packed != condition_pack_a_b(uint8_t(a), uint8_t(b)), position, packed);
            position++;
        }
    }
    CHECK_EQ(position, kConditionPositions);        // the pairs run to the end of the walk
    CHECK_EQ(wrong_a.subject, -1);
    CHECK_EQ(wrong_b.subject, -1);

    // 1:2 first, then the header's "1:4" a few clicks on, and 8:8 last.
    CHECK_EQ(condition_a(condition_at_position(kFirstPair)), 1);
    CHECK_EQ(condition_b(condition_at_position(kFirstPair)), 2);
    CHECK_EQ(condition_a(condition_at_position(kFirstPair + 5)), 1);
    CHECK_EQ(condition_b(condition_at_position(kFirstPair + 5)), 4);
    CHECK_EQ(condition_a(condition_at_position(kConditionPositions - 1)), 8);
    CHECK_EQ(condition_b(condition_at_position(kConditionPositions - 1)), 8);
}

TEST(the_named_conditions_come_first_so_fill_is_one_click_away) {
    // The reason this order exists: with the 35 pairs in front, FILL was 36 clicks from NONE
    // and NOT 1ST was 43, which a clamping encoder makes unusable. These eight are the ones a
    // fill key and a PRE chain are for, so they sit at the near end.
    constexpr Condition kExpected[] = {
        Condition::FILL, Condition::NOT_FILL, Condition::PRE,   Condition::NOT_PRE,
        Condition::NEI,  Condition::NOT_NEI,  Condition::FIRST, Condition::NOT_FIRST,
    };
    constexpr int kCount = int(sizeof(kExpected) / sizeof(kExpected[0]));
    CHECK_EQ(kCount, kNamedKindCount);
    CHECK_EQ(1 + kNamedKindCount + int(kConditionAbPairs), kConditionPositions);
    CHECK_EQ(kFirstNamedKind + kNamedKindCount, kFirstPair);

    for (int i = 0; i < kCount; i++) {
        const uint8_t packed = condition_at_position(kFirstNamedKind + i);
        CHECK_EQ(int(condition_kind(packed)), int(kExpected[i]));
        CHECK_EQ(packed, condition_pack(kExpected[i]));
    }
    CHECK_EQ(int(condition_kind(condition_at_position(0))), int(Condition::NONE));

    // One click from a cleared step is FILL, not 1:2.
    Step step{};
    step_field_apply(step, StepField::condition, 1);
    CHECK_EQ(int(condition_kind(step.condition)), int(Condition::FILL));
}

TEST(every_byte_maps_to_a_position_and_corruption_reads_as_none) {
    Witness out_of_range;
    Witness not_identity;
    Witness not_none;

    for (int b = 0; b < 256; b++) {
        const uint8_t packed = uint8_t(b);
        const int p = condition_position(packed);
        out_of_range.saw(p < 0 || p >= kConditionPositions, b, p);

        // The bytes a position produces: NONE, the eight kinds past A_B, and the 35 pairs.
        const bool produced = packed == 0 || (packed >= 2 && packed <= 9) ||
                              (packed >= kConditionAbBase &&
                               packed < kConditionAbBase + int(kConditionAbPairs));
        if (produced) {
            not_identity.saw(condition_at_position(p) != packed, b, condition_at_position(p));
        } else if (packed == uint8_t(Condition::A_B)) {
            not_identity.saw(p != kFirstPair, b, p);
        } else {
            // 10..15, 51..255: an unknown kind or an out-of-range pair. 255 among them.
            not_none.saw(p != 0, b, p);
        }
    }
    CHECK_EQ(out_of_range.subject, -1);
    CHECK_EQ(not_identity.subject, -1);
    CHECK_EQ(not_none.subject, -1);

    CHECK_EQ(condition_position(255), 0);
    CHECK_EQ(condition_position(10), 0);
    CHECK_EQ(condition_position(15), 0);
    CHECK_EQ(condition_position(51), 0);
    CHECK_EQ(int(condition_kind(condition_at_position(condition_position(255)))),
             int(Condition::NONE));
}

TEST(the_condition_field_clamps_at_both_ends_rather_than_wrapping) {
    // An encoder that wraps from NOT 1ST round to NONE makes both ends unreachable in practice.
    Step step{};
    CHECK_EQ(condition_position(step.condition), 0);

    Witness invalid;
    Witness went_backwards;
    int last = 0;
    int reached[kConditionPositions] = {};
    for (int i = 0; i < 200; i++) {
        step_field_apply(step, StepField::condition, 1);
        const int p = condition_position(step.condition);
        invalid.saw(!condition_valid(step.condition), i, step.condition);
        went_backwards.saw(p < last, i, p);         // a wrap shows up as a step backwards
        if (p >= 0 && p < kConditionPositions) reached[p]++;
        last = p;
    }
    CHECK_EQ(invalid.subject, -1);
    CHECK_EQ(went_backwards.subject, -1);
    CHECK_EQ(last, kConditionPositions - 1);
    CHECK_EQ(int(condition_kind(step.condition)), int(Condition::A_B));   // the top is 8:8
    CHECK_EQ(condition_a(step.condition), 8);
    CHECK_EQ(condition_b(step.condition), 8);

    // Going up one click at a time visited every position on the way.
    Witness missed;
    for (int p = 1; p < kConditionPositions; p++) missed.saw(reached[p] == 0, p);
    CHECK_EQ(missed.subject, -1);

    for (int i = 0; i < 200; i++) {
        step_field_apply(step, StepField::condition, -1);
        const int p = condition_position(step.condition);
        invalid.saw(!condition_valid(step.condition), 1000 + i, step.condition);
        went_backwards.saw(p > last, 1000 + i, p);
        last = p;
    }
    CHECK_EQ(invalid.subject, -1);
    CHECK_EQ(went_backwards.subject, -1);
    CHECK_EQ(last, 0);
    CHECK_EQ(step.condition, condition_pack(Condition::NONE));
    CHECK_EQ(int(condition_kind(step.condition)), int(Condition::NONE));
}

TEST(a_corrupt_condition_byte_is_repaired_by_one_click) {
    // The encoder is the user's only way out of a bad byte, so it has to lead somewhere legal.
    Witness invalid;
    for (int b = 0; b < 256; b++) {
        for (int d = 0; d < kDeltaCount; d++) {
            Step step{};
            step.condition = uint8_t(b);
            step_field_apply(step, StepField::condition, kDeltas[d]);
            invalid.saw(!condition_valid(step.condition), b, kDeltas[d]);
        }
    }
    CHECK_EQ(invalid.subject, -1);

    Step step{};
    step.condition = 200;                                   // nothing this build knows
    step_field_apply(step, StepField::condition, -1);       // reads as NONE, so it stays there
    CHECK_EQ(step.condition, condition_pack(Condition::NONE));
    step.condition = 200;
    step_field_apply(step, StepField::condition, 1);        // and one click up is FILL
    CHECK_EQ(step.condition, condition_pack(Condition::FILL));
    step.condition = 200;
    step_field_apply(step, StepField::condition, kFirstPair);
    CHECK_EQ(step.condition, condition_pack_a_b(1, 2));
}

// --- step_field_apply -----------------------------------------------------

TEST(step_field_apply_clamps_from_every_starting_byte) {
    Witness out_of_range;
    Witness leaked;

    for (int i = 0; i < kStepFields; i++) {
        const StepField field = kFields[i];
        for (int raw = 0; raw < 256; raw++) {
            for (int d = 0; d < kDeltaCount; d++) {
                Step step{};
                set_raw(step, field, uint8_t(raw));
                const Step before = step;
                step_field_apply(step, field, kDeltas[d]);
                out_of_range.saw(!field_in_range(step, field), i * 1000 + raw, kDeltas[d]);
                leaked.saw(!only_its_own_byte_changed(before, step, field), i * 1000 + raw,
                           kDeltas[d]);
            }
        }
    }
    // The subject is field * 1000 + the starting byte, so a failure names both.
    CHECK_EQ(out_of_range.subject, -1);
    CHECK_EQ(leaked.subject, -1);
}

TEST(a_flag_edit_leaves_the_other_flags_alone) {
    // flags carries the trig and trigless bits too (pattern.hpp): an accent edit that cleared
    // the trig would silence the step, which on hardware looks like a broken pad.
    struct FlagField {
        StepField field;
        uint8_t bit;
    };
    constexpr FlagField kFlagFields[] = {{StepField::accent, kStepAccent},
                                         {StepField::slide, kStepSlide}};

    Witness disturbed;
    for (int raw = 0; raw < 256; raw++) {
        for (int d = 0; d < kDeltaCount; d++) {
            for (const FlagField &flag : kFlagFields) {
                Step step{};
                step.flags = uint8_t(raw);
                const uint8_t before = step.flags;
                step_field_apply(step, flag.field, kDeltas[d]);
                const uint8_t others = uint8_t(~flag.bit);
                disturbed.saw((step.flags & others) != (before & others), raw, kDeltas[d]);
            }
        }
    }
    CHECK_EQ(disturbed.subject, -1);

    Step step{};
    step.flags = kStepTrig;
    step_field_apply(step, StepField::accent, 1);
    CHECK_EQ(step.flags, uint8_t(kStepTrig | kStepAccent));
    step_field_apply(step, StepField::accent, 1);        // already on: clamps, does not toggle
    CHECK_EQ(step.flags, uint8_t(kStepTrig | kStepAccent));
    step_field_apply(step, StepField::accent, -1);
    CHECK_EQ(step.flags, uint8_t(kStepTrig));
    step_field_apply(step, StepField::accent, -1);
    CHECK_EQ(step.flags, uint8_t(kStepTrig));
    step_field_apply(step, StepField::slide, 1000);
    CHECK_EQ(step.flags, uint8_t(kStepTrig | kStepSlide));
}

TEST(the_ratchet_count_and_ramp_share_a_byte_without_disturbing_each_other) {
    Witness ramp_moved;
    Witness count_moved;

    for (int count = kRatchetMin; count <= kRatchetMax; count++) {
        for (int ramp = 0; ramp <= int(RatchetRamp::down); ramp++) {
            for (int d = 0; d < kDeltaCount; d++) {
                Step step{};
                step.ratchet = ratchet_pack(uint8_t(count), RatchetRamp(ramp));

                step_field_apply(step, StepField::ratchet_count, kDeltas[d]);
                ramp_moved.saw(int(uint8_t(ratchet_ramp(step.ratchet))) != ramp, count * 100 + ramp,
                               kDeltas[d]);

                const int hits = int(ratchet_count(step.ratchet));
                step_field_apply(step, StepField::ratchet_ramp, kDeltas[d]);
                count_moved.saw(int(ratchet_count(step.ratchet)) != hits, count * 100 + ramp,
                                kDeltas[d]);
            }
        }
    }
    CHECK_EQ(ramp_moved.subject, -1);
    CHECK_EQ(count_moved.subject, -1);

    // A corrupt high nibble is preserved too: an edit must not quietly move a field the user is
    // not touching, even to repair it. step_sanitize() is where repairs belong.
    Step step{};
    step.ratchet = 0xF9u;                                    // ramp nibble 15, count 9
    step_field_apply(step, StepField::ratchet_count, -1);
    CHECK_EQ(ratchet_count(step.ratchet), 8);                // 9 clamps down into range
    CHECK_EQ(uint8_t(step.ratchet >> 4), 15);                // the ramp nibble is untouched
}

TEST(the_ratchet_count_clamps_and_the_ramp_cycles) {
    Step step{};
    step.ratchet = ratchet_pack(kRatchetMin, RatchetRamp::none);
    step_field_apply(step, StepField::ratchet_count, -1);
    CHECK_EQ(ratchet_count(step.ratchet), kRatchetMin);      // 1 hit is the unratcheted step
    step_field_apply(step, StepField::ratchet_count, 1000);
    CHECK_EQ(ratchet_count(step.ratchet), kRatchetMax);
    step_field_apply(step, StepField::ratchet_count, 1);
    CHECK_EQ(ratchet_count(step.ratchet), kRatchetMax);

    // Three settings with no natural ends, so this one really does wrap.
    step.ratchet = ratchet_pack(4, RatchetRamp::none);
    step_field_apply(step, StepField::ratchet_ramp, 1);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::up));
    step_field_apply(step, StepField::ratchet_ramp, 1);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::down));
    step_field_apply(step, StepField::ratchet_ramp, 1);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::none));
    step_field_apply(step, StepField::ratchet_ramp, -1);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::down));

    // A big delta still lands where the cycle says, not at an end.
    step.ratchet = ratchet_pack(4, RatchetRamp::none);
    step_field_apply(step, StepField::ratchet_ramp, 1000);   // 1000 % 3 == 1
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::up));
    step.ratchet = ratchet_pack(4, RatchetRamp::none);
    step_field_apply(step, StepField::ratchet_ramp, -1000);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::down));
    CHECK_EQ(ratchet_count(step.ratchet), 4);
}

TEST(a_zero_delta_leaves_a_legal_step_exactly_as_it_was) {
    // The UI calls this on every encoder frame; a no-op has to be a no-op or an undo entry
    // appears for a field nobody turned.
    Step step{};
    step.flags = uint8_t(kStepTrig | kStepAccent | kStepSlide);
    step.note = 48;
    step.velocity = 90;
    step.length = 8;
    step.micro = -4;
    step.probability = 75;
    step.condition = condition_pack_a_b(2, 5);
    step.ratchet = ratchet_pack(3, RatchetRamp::down);
    step.chord = 7;

    const Step before = step;
    for (int i = 0; i < kStepFields; i++) step_field_apply(step, kFields[i], 0);
    CHECK(std::memcmp(&before, &step, sizeof(Step)) == 0);

    // And an unknown field id is inert, not a write through the end of the range table.
    step_field_apply(step, StepField::count, 5);
    step_field_apply(step, StepField(99), -5);
    CHECK(std::memcmp(&before, &step, sizeof(Step)) == 0);
}

TEST(step_field_value_reads_the_byte_and_the_condition_reads_as_a_position) {
    Step step{};
    step.note = 42;
    step.velocity = 11;
    step.length = 13;
    step.micro = -9;
    step.probability = 37;
    step.condition = condition_pack_a_b(3, 6);
    step.ratchet = ratchet_pack(5, RatchetRamp::up);
    step.chord = 21;
    step.flags = kStepAccent;

    CHECK_EQ(step_field_value(step, StepField::note), 42);
    CHECK_EQ(step_field_value(step, StepField::velocity), 11);
    CHECK_EQ(step_field_value(step, StepField::length), 13);
    CHECK_EQ(step_field_value(step, StepField::micro), -9);
    CHECK_EQ(step_field_value(step, StepField::probability), 37);
    CHECK_EQ(step_field_value(step, StepField::condition), condition_position(step.condition));
    CHECK_EQ(step_field_value(step, StepField::ratchet_count), 5);
    CHECK_EQ(step_field_value(step, StepField::ratchet_ramp), int(RatchetRamp::up));
    CHECK_EQ(step_field_value(step, StepField::accent), 1);
    CHECK_EQ(step_field_value(step, StepField::slide), 0);
    CHECK_EQ(step_field_value(step, StepField::chord), chord_walk_position(21));
    CHECK_EQ(step_field_value(step, StepField::count), 0);

    // The condition's value is a walk position, which is why it is not what gets printed.
    CHECK(step_field_value(step, StepField::condition) != int(step.condition));
}

// --- text -----------------------------------------------------------------

TEST(step_field_text_never_writes_past_max_for_any_byte_of_any_field) {
    // The caller passes a small fixed buffer. An overrun here is a stack smash on the unit
    // that the host build would not even notice, so this sweeps every field, every byte and
    // every buffer size down to the degenerate ones.
    // Every max stays well short of Guarded::kSize so there are always guard bytes left to
    // overwrite: an overrun then fails a check and names the field, instead of running off the
    // end of the struct and taking the runner down with it.
    constexpr int kMaxes[] = {0, 1, 2, 3, 4, 5, 8, 9, 16, 24};
    static_assert(kMaxes[sizeof(kMaxes) / sizeof(kMaxes[0]) - 1] + 8 < Guarded::kSize,
                  "leave room for a guard");
    Witness overrun;
    Witness unterminated;
    Witness empty;
    Witness unclean;

    for (int i = 0; i < kStepFields; i++) {
        for (int raw = 0; raw < 256; raw++) {
            for (const int max : kMaxes) {
                Guarded guarded;
                guarded.arm();
                Step step{};
                set_raw(step, kFields[i], uint8_t(raw));
                step_field_text(step, kFields[i], guarded.buf, max);

                overrun.saw(!guarded.guard_intact(max), i * 1000 + raw, max);
                if (max <= 0) continue;    // nowhere to put a terminator: nothing is written
                unterminated.saw(!guarded.terminated(max), i * 1000 + raw, max);
                // A one-byte buffer can only hold the empty string; anything larger must say
                // something, or the encoder slot draws blank.
                empty.saw(max >= 2 && guarded.buf[0] == '\0', i * 1000 + raw, max);
                unclean.saw(!label_is_clean(guarded.buf), i * 1000 + raw, max);
            }
        }
    }
    CHECK_EQ(overrun.subject, -1);
    CHECK_EQ(unterminated.subject, -1);
    CHECK_EQ(empty.subject, -1);
    CHECK_EQ(unclean.subject, -1);
}

TEST(the_chord_field_shows_names_and_leaves_a_byte_it_cannot_name_alone) {
    char buf[kStepFieldTextMax];
    Step step{};
    step_field_text(step, StepField::chord, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "1 NOTE");

    step.chord = chord_pack(ChordType::min7, 2);
    step_field_text(step, StepField::chord, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "MIN7/2");

    step.chord = chord_pack(ChordType::fifth_octave, 2);
    step_field_text(step, StepField::chord, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "5THOCT/2");          // squeezed to fit

    // One click from a single note is MIN, not byte 1's neighbour.
    step.chord = 0;
    step_field_apply(step, StepField::chord, +1);
    CHECK_EQ(int(step.chord), int(chord_pack(ChordType::minor, 0)));

    // A byte a later build wrote: shown as '?', untouched by a zero delta.
    step.chord = 0x0E;                                  // type id 14, free in this build
    step_field_text(step, StepField::chord, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "?");
    step_field_apply(step, StepField::chord, 0);
    CHECK_EQ(int(step.chord), 0x0E);
}

TEST(step_field_text_fits_in_kStepFieldTextMax) {
    // So app/ can size one buffer from the header and never truncate.
    Witness too_long;
    for (int i = 0; i < kStepFields; i++) {
        for (int raw = 0; raw < 256; raw++) {
            char buf[64];
            Step step{};
            set_raw(step, kFields[i], uint8_t(raw));
            step_field_text(step, kFields[i], buf, int(sizeof(buf)));
            too_long.saw(int(std::strlen(buf)) + 1 > kStepFieldTextMax, i * 1000 + raw,
                         int(std::strlen(buf)));
        }
    }
    CHECK_EQ(too_long.subject, -1);

    // An unknown field id still draws something.
    char buf[kStepFieldTextMax];
    Step step{};
    step_field_text(step, StepField::count, buf, int(sizeof(buf)));
    CHECK(buf[0] != '\0');
}

TEST(step_field_text_truncates_instead_of_lying_about_the_buffer) {
    Step step{};
    step.condition = condition_pack(Condition::NOT_FILL);     // "NOT FILL", the longest text

    char buf[Guarded::kSize];
    step_field_text(step, StepField::condition, buf, int(sizeof(buf)));
    CHECK(std::strcmp(buf, "NOT FILL") == 0);

    constexpr int kShortMaxes[] = {1, 2, 3, 4, 5};
    for (const int max : kShortMaxes) {
        Guarded guarded;
        guarded.arm();
        step_field_text(step, StepField::condition, guarded.buf, max);
        CHECK_EQ(int(std::strlen(guarded.buf)), max - 1);
        CHECK(std::strncmp(guarded.buf, "NOT FILL", size_t(max - 1)) == 0);
        CHECK(guarded.guard_intact(max));
    }
}

TEST(step_field_text_reads_the_values_the_header_documents) {
    Step step{};
    char buf[kStepFieldTextMax];
    const int max = int(sizeof(buf));

    step.note = 60;
    step_field_text(step, StepField::note, buf, max);
    CHECK(std::strcmp(buf, "C-4") == 0);

    step.velocity = 127;
    step_field_text(step, StepField::velocity, buf, max);
    CHECK(std::strcmp(buf, "127") == 0);
    step.velocity = 1;
    step_field_text(step, StepField::velocity, buf, max);
    CHECK(std::strcmp(buf, "1") == 0);

    step.length = 8;                                   // 16 sixteenths of a step = one step
    step_field_text(step, StepField::length, buf, max);
    CHECK(std::strcmp(buf, "1 STP") == 0);
    step.length = 4;
    step_field_text(step, StepField::length, buf, max);
    CHECK(std::strcmp(buf, "1/4 STP") == 0);
    step.length = 0;                                   // timing.hpp's invalid sentinel
    step_field_text(step, StepField::length, buf, max);
    CHECK(std::strcmp(buf, "?") == 0);

    step.micro = 3;
    step_field_text(step, StepField::micro, buf, max);
    CHECK(std::strcmp(buf, "+3") == 0);
    step.micro = -11;
    step_field_text(step, StepField::micro, buf, max);
    CHECK(std::strcmp(buf, "-11") == 0);
    step.micro = 0;                                    // no sign: zero is not an offset
    step_field_text(step, StepField::micro, buf, max);
    CHECK(std::strcmp(buf, "0") == 0);

    step.probability = 100;
    step_field_text(step, StepField::probability, buf, max);
    CHECK(std::strcmp(buf, "100%") == 0);
    step.probability = 0;
    step_field_text(step, StepField::probability, buf, max);
    CHECK(std::strcmp(buf, "0%") == 0);

    step.condition = condition_pack(Condition::NONE);
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "NONE") == 0);
    step.condition = condition_pack_a_b(1, 4);
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "1:4") == 0);
    step.condition = condition_pack(Condition::NOT_PRE);
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "NOT PRE") == 0);
    step.condition = condition_pack(Condition::FIRST);
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "1ST") == 0);
    step.condition = uint8_t(Condition::A_B);          // the bare byte reads as 1:2
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "1:2") == 0);
    step.condition = 200;                              // corruption reads as NONE
    step_field_text(step, StepField::condition, buf, max);
    CHECK(std::strcmp(buf, "NONE") == 0);

    step.ratchet = ratchet_pack(6, RatchetRamp::up);
    step_field_text(step, StepField::ratchet_count, buf, max);
    CHECK(std::strcmp(buf, "6") == 0);
    step_field_text(step, StepField::ratchet_ramp, buf, max);
    CHECK(std::strcmp(buf, "UP") == 0);
    step.ratchet = ratchet_pack(1, RatchetRamp::down);
    step_field_text(step, StepField::ratchet_ramp, buf, max);
    CHECK(std::strcmp(buf, "DOWN") == 0);
    step.ratchet = ratchet_pack(1, RatchetRamp::none);
    step_field_text(step, StepField::ratchet_ramp, buf, max);
    CHECK(std::strcmp(buf, "FLAT") == 0);
    step.ratchet = 0x00;                               // count 0 is not a ratchet
    step_field_text(step, StepField::ratchet_count, buf, max);
    CHECK(std::strcmp(buf, "?") == 0);
    step.ratchet = 0x91;                               // nor is ramp nibble 9 a ramp
    step_field_text(step, StepField::ratchet_ramp, buf, max);
    CHECK(std::strcmp(buf, "?") == 0);

    step.flags = kStepTrig;
    step_field_text(step, StepField::accent, buf, max);
    CHECK(std::strcmp(buf, "--") == 0);
    step_field_text(step, StepField::slide, buf, max);
    CHECK(std::strcmp(buf, "--") == 0);
    step.flags = uint8_t(kStepTrig | kStepAccent | kStepSlide);
    step_field_text(step, StepField::accent, buf, max);
    CHECK(std::strcmp(buf, "ON") == 0);
    step_field_text(step, StepField::slide, buf, max);
    CHECK(std::strcmp(buf, "ON") == 0);
}

TEST(the_length_labels_agree_with_the_timing_table) {
    // The label has to name the same duration the sequencer will play, and the two live in
    // different files.
    char buf[kStepFieldTextMax];
    Witness disagreed;
    for (int index = 1; index <= kLengthMax; index++) {
        Step step{};
        step.length = uint8_t(index);
        step_field_text(step, StepField::length, buf, int(sizeof(buf)));
        const int sixteenths = sixteenths_from_text(buf);
        disagreed.saw(sixteenths != int(kLengthSixteenthsOfStep[index]), index, sixteenths);
    }
    CHECK_EQ(disagreed.subject, -1);

    // The parser itself has to be able to fail, or the sweep above proves nothing.
    CHECK_EQ(sixteenths_from_text("NONE"), -1);
    CHECK_EQ(sixteenths_from_text("1 STEP"), -1);
    CHECK_EQ(sixteenths_from_text("1/16 STP"), 1);
    CHECK_EQ(sixteenths_from_text("1.5 STP"), 24);
    CHECK_EQ(sixteenths_from_text("64 STP"), 1024);

    // Every reachable length reads differently, so the encoder's 20 positions are 20 labels.
    Witness duplicate;
    char seen[kLengthTableSize][kStepFieldTextMax];
    for (int index = 1; index <= kLengthMax; index++) {
        Step step{};
        step.length = uint8_t(index);
        step_field_text(step, StepField::length, seen[index], kStepFieldTextMax);
        for (int k = 1; k < index; k++) {
            duplicate.saw(std::strcmp(seen[index], seen[k]) == 0, index, k);
        }
    }
    CHECK_EQ(duplicate.subject, -1);
}

TEST(note_name_is_the_tracker_spelling_with_middle_c_at_60) {
    char buf[kStepFieldTextMax];
    const int max = int(sizeof(buf));

    note_name(60, buf, max);
    CHECK(std::strcmp(buf, "C-4") == 0);

    // The octave boundary sits between 59 and 60, not between 60 and 61.
    note_name(59, buf, max);
    CHECK(std::strcmp(buf, "B-3") == 0);
    note_name(61, buf, max);
    CHECK(std::strcmp(buf, "C#4") == 0);
    note_name(12, buf, max);
    CHECK(std::strcmp(buf, "C-0") == 0);
    note_name(69, buf, max);                 // A440
    CHECK(std::strcmp(buf, "A-4") == 0);

    // The ends. Notes 0..11 are octave -1, the one octave needing two characters for its
    // number: "C--1" is ugly but it cannot be confused with "C-1", which is note 24.
    note_name(0, buf, max);
    CHECK(std::strcmp(buf, "C--1") == 0);
    note_name(127, buf, max);
    CHECK(std::strcmp(buf, "G-9") == 0);
    note_name(24, buf, max);
    CHECK(std::strcmp(buf, "C-1") == 0);
}

// The step page and the PROJECT screen spell a pitch the same way: core/scale.cpp's pitch-class
// name is exactly this one's letter and accidental, with the tracker's '-' standing for "none".
// They disagreed (flats there, sharps here) until 2026-11-22.
TEST(note_name_agrees_with_the_scale_pages_spelling) {
    char buf[kStepFieldTextMax];
    for (int note = 0; note <= 127; note++) {
        note_name(uint8_t(note), buf, int(sizeof(buf)));
        const char *pitch = note_name(uint8_t(note));
        CHECK_EQ(buf[0], pitch[0]);
        CHECK_EQ(buf[1], pitch[1] == '#' ? '#' : '-');
    }
}

TEST(every_note_has_its_own_name_and_fits_a_small_buffer) {
    char names[128][kStepFieldTextMax];
    Witness bad_shape;
    Witness duplicate;

    for (int note = 0; note < 128; note++) {
        note_name(uint8_t(note), names[note], kStepFieldTextMax);
        const int length = int(std::strlen(names[note]));
        bad_shape.saw(length < 3 || length > 4, note, length);
        bad_shape.saw(std::strchr("CDEFGAB", names[note][0]) == nullptr, note, names[note][0]);
        bad_shape.saw(names[note][1] != '-' && names[note][1] != '#', note, names[note][1]);
        bad_shape.saw(!label_is_clean(names[note]), note);
        for (int k = 0; k < note; k++) {
            duplicate.saw(std::strcmp(names[note], names[k]) == 0, note, k);
        }
    }
    CHECK_EQ(bad_shape.subject, -1);
    CHECK_EQ(duplicate.subject, -1);        // 128 notes must be 128 names

    // Notes above 127 fold into the 7-bit range rather than growing the string, matching what
    // step_sanitize() does with the same byte.
    char high[kStepFieldTextMax];
    char folded[kStepFieldTextMax];
    Witness unfolded;
    for (int note = 128; note < 256; note++) {
        note_name(uint8_t(note), high, kStepFieldTextMax);
        note_name(uint8_t(note & 0x7F), folded, kStepFieldTextMax);
        unfolded.saw(std::strcmp(high, folded) != 0, note);
    }
    CHECK_EQ(unfolded.subject, -1);
}

TEST(note_name_respects_a_short_buffer) {
    constexpr int kMaxes[] = {0, 1, 2, 3, 4, 5};
    Guarded guarded;
    for (const int max : kMaxes) {
        guarded.arm();
        note_name(0, guarded.buf, max);          // "C--1", the longest name
        CHECK(guarded.guard_intact(max));
        if (max > 0) CHECK_EQ(int(std::strlen(guarded.buf)), max - 1 < 4 ? max - 1 : 4);
    }

    // A null buffer, or a negative size, must not be written to at all.
    note_name(60, nullptr, 8);
    guarded.arm();
    note_name(60, guarded.buf, -3);
    CHECK(guarded.guard_intact(0));
}

TEST(step_field_text_agrees_with_note_name_for_the_note_field) {
    // The grid draws note names through note_name and the step page through step_field_text;
    // the same note must read the same way in both.
    char from_field[kStepFieldTextMax];
    char direct[kStepFieldTextMax];
    Witness disagreed;
    for (int note = 0; note < 128; note++) {
        Step step{};
        step.note = uint8_t(note);
        step_field_text(step, StepField::note, from_field, kStepFieldTextMax);
        note_name(uint8_t(note), direct, kStepFieldTextMax);
        disagreed.saw(std::strcmp(from_field, direct) != 0, note);
    }
    CHECK_EQ(disagreed.subject, -1);
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(field_text_signs_a_micro_of_one_and_shows_the_ratchet_range_ends) {
    char buf[kStepFieldTextMax];
    Step step{};
    step.micro = 1;
    step_field_text(step, StepField::micro, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "+1");
    step.micro = -1;
    step_field_text(step, StepField::micro, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "-1");
    step.micro = 0;
    step_field_text(step, StepField::micro, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "0");

    step.ratchet = ratchet_pack(uint8_t(kRatchetMin), RatchetRamp::none);
    step_field_text(step, StepField::ratchet_count, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "1");
    step.ratchet = ratchet_pack(uint8_t(kRatchetMax), RatchetRamp::none);
    step_field_text(step, StepField::ratchet_count, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "8");
}

TEST(a_chord_name_keeps_its_space_when_there_is_no_inversion_to_make_room_for) {
    char buf[kStepFieldTextMax];
    Step step{};
    step.chord = chord_pack(ChordType::fifth_octave, 0);
    step_field_text(step, StepField::chord, buf, int(sizeof(buf)));
    CHECK_EQ((const char *)buf, "5TH OCT");
}

TEST(the_chord_walk_clamps_both_ends_and_an_invalid_byte_sits_at_its_start) {
    const uint8_t last = chord_at_walk_position(kChordWalkPositions - 1);
    CHECK(last != 0);
    CHECK_EQ(int(chord_at_walk_position(-3)), int(chord_at_walk_position(0)));
    CHECK_EQ(int(chord_at_walk_position(kChordWalkPositions)), int(last));
    CHECK_EQ(int(chord_at_walk_position(kChordWalkPositions + 9)), int(last));
    // A byte that is not a chord counts as the walk's start, so one click up is position 1.
    Step step{};
    step.chord = 0xFF;
    step_field_apply(step, StepField::chord, +1);
    CHECK_EQ(int(step.chord), int(chord_at_walk_position(1)));
}

TEST(a_field_index_past_the_table_changes_nothing) {
    Step step{};
    step.velocity = 90;
    step.note = 60;
    const Step before = step;
    step_field_apply(step, StepField(kStepFields), 5);
    step_field_apply(step, StepField(-1), 5);
    CHECK(std::memcmp(&step, &before, sizeof(Step)) == 0);
}
