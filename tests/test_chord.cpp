// Chords: the packing of Step::chord, the table, and the expansion to notes.
//
// Three things here are worth more than the rest.
//
// The first is that **zeroed memory is a single note**. core/pattern.hpp has serialized this
// byte since M3, so there are already files full of zeroes in this field, and a packing that
// read 0 as a chord would make every cleared step on a POLY track sprout one. It is checked
// from both ends: the byte a writer produces for "no chord", and a `pattern_init`ed pattern.
//
// The second is that the expansion is TOTAL. It runs in audio context against a fixed pool of
// voices, so a seventh note or a note at 131 is not something the caller can refuse when it
// arrives: `chord_expansion_is_total_over_every_root_byte_key_and_scale` sweeps every chord
// byte against every scale, including ids this build does not have, and counts violations
// instead of checking inside the loop.
//
// The third is the decision core/chord.hpp argues for -- semitone stacks folded into the key
// rather than stacks of scale degrees -- which stands on a property rather than on taste:
// `chord_a_major_stack_is_the_diatonic_triad_on_every_degree` shows that folding a major stack
// onto each degree of each diatonic mode gives exactly that degree's diatonic triad. If that
// ever stops being true, the header's fourth reason has gone and the table should be rethought,
// not the test adjusted.
//
// Patterns are file-scope statics -- one is about 8 KiB and CLAUDE.md caps a stack frame at
// 16 KiB.
#include "check.hpp"

#include "chord.hpp"
#include "pattern.hpp"
#include "scale.hpp"
#include "stepedit.hpp"

#include <initializer_list>

namespace {

using namespace strata;

Pattern g_pattern;

// Is this chord exactly these semitones above its root, and nothing else?
bool intervals_are(ChordType type, std::initializer_list<int> want) {
    const ChordIntervals iv = chord_intervals(type);
    if (int(iv.count) != int(want.size())) return false;
    int i = 0;
    for (int semitone : want) {
        if (int(iv.semitones[i]) != semitone) return false;
        i++;
    }
    return true;
}

bool ascending(const uint8_t *notes, int count) {
    for (int i = 1; i < count; i++) {
        if (notes[i] <= notes[i - 1]) return false;
    }
    return true;
}

bool all_in_scale(const uint8_t *notes, int count, uint8_t key, int scale) {
    for (int i = 0; i < count; i++) {
        if (!scale_contains(notes[i], key, scale)) return false;
    }
    return true;
}

bool same_notes(const uint8_t *a, const uint8_t *b, int count) {
    for (int i = 0; i < count; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// The twelve pitch classes of a voicing, counted. An inversion must not change this.
void pitch_classes(const uint8_t *notes, int count, int *into) {
    for (int i = 0; i < 12; i++) into[i] = 0;
    for (int i = 0; i < count; i++) into[notes[i] % 12]++;
}

bool is_diatonic_mode(int scale) {
    switch (Scale(scale)) {
        case Scale::major:
        case Scale::dorian:
        case Scale::phrygian:
        case Scale::lydian:
        case Scale::mixolydian:
        case Scale::minor:
        case Scale::locrian:
            return true;
        default:
            return false;
    }
}

}  // namespace

// --- zero is a single note ------------------------------------------------

TEST(chord_zero_is_a_single_note) {
    // The one that matters most: a zeroed Step, a cleared pattern and a zeroed file all hold 0
    // here, and 0 has to mean one note. It is why the type and not the inversion owns the low
    // bits of the byte.
    const Step fresh;
    CHECK_EQ(int(fresh.chord), 0);
    CHECK_EQ(int(chord_type(0)), int(ChordType::none));
    CHECK_EQ(int(chord_inversion(0)), 0);
    CHECK(chord_valid(0));
    CHECK(intervals_are(ChordType::none, {0}));

    // And one encoding of it, not four: an inversion of nothing is nothing.
    for (uint8_t inversion = 0; inversion <= 3; inversion++) {
        CHECK_EQ(int(chord_pack(ChordType::none, inversion)), 0);
    }

    uint8_t notes[kChordMaxNotes] = {};
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (uint8_t key = 0; key < 12; key++) {
            for (int root = 0; root <= 127; root += 5) {
                const int n = chord_expand(uint8_t(root), 0, key, scale, notes);
                CHECK_EQ(n, 1);
                // The step's own note, folded into the key -- which is the single thing a POLY
                // track with no chord has to do.
                CHECK_EQ(int(notes[0]), int(scale_fold(uint8_t(root), key, scale)));
            }
        }
    }
}

TEST(chord_a_cleared_pattern_does_not_sprout_chords) {
    pattern_init(g_pattern);
    g_pattern.root = 0;
    g_pattern.scale = uint8_t(Scale::minor);

    uint8_t notes[kChordMaxNotes] = {};
    int chorded = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &step = g_pattern.tracks[t].steps[i];
            if (step.chord != 0) chorded++;
            if (chord_expand(step.note, step.chord, g_pattern.root, int(g_pattern.scale), notes,
                             kChordMaxNotes) != 1) {
                chorded++;
            }
        }
    }
    CHECK_EQ(chorded, 0);

    // And a step that HAD a chord gives it up when it is cleared, like every other field.
    Step step;
    step.chord = chord_pack(ChordType::min9, 2);
    CHECK(step.chord != 0);
    step_clear(step);
    CHECK_EQ(int(step.chord), 0);
}

// --- the table ------------------------------------------------------------

TEST(chord_the_table_is_the_chords_its_names_promise) {
    // Checked by value, for the reason core/scale.hpp gives about its masks: a wrong interval
    // here is a chord that is a different chord, which is musical rather than loud, and nothing
    // checks a comment.
    CHECK(intervals_are(ChordType::minor, {0, 3, 7}));
    CHECK(intervals_are(ChordType::major, {0, 4, 7}));
    CHECK(intervals_are(ChordType::sus2, {0, 2, 7}));
    CHECK(intervals_are(ChordType::sus4, {0, 5, 7}));
    CHECK(intervals_are(ChordType::dim, {0, 3, 6}));
    CHECK(intervals_are(ChordType::min7, {0, 3, 7, 10}));
    CHECK(intervals_are(ChordType::dom7, {0, 4, 7, 10}));
    CHECK(intervals_are(ChordType::maj7, {0, 4, 7, 11}));
    CHECK(intervals_are(ChordType::min9, {0, 3, 7, 10, 14}));
    CHECK(intervals_are(ChordType::min11, {0, 3, 7, 10, 14, 17}));
    CHECK(intervals_are(ChordType::fifth, {0, 7}));
    CHECK(intervals_are(ChordType::octave, {0, 12}));
    CHECK(intervals_are(ChordType::fifth_octave, {0, 7, 12}));
    CHECK(intervals_are(ChordType::none, {0}));
}

TEST(chord_no_chord_asks_for_more_voices_than_va_poly_has) {
    // docs/SPEC.md gives VA POLY six voices, so six is the bound on the table as well as on the
    // expansion. Every stack also starts on its root, rises, and leaves nothing readable past
    // its count.
    CHECK_EQ(kChordMaxNotes, 6);
    for (int id = 0; id < kChordTypeCount; id++) {
        const ChordIntervals iv = chord_intervals(ChordType(id));
        CHECK(iv.count >= 1);
        CHECK(int(iv.count) <= kChordMaxNotes);
        CHECK_EQ(int(iv.semitones[0]), 0);
        for (int i = 1; i < int(iv.count); i++) {
            CHECK(iv.semitones[i] > iv.semitones[i - 1]);
        }
        for (int i = int(iv.count); i < kChordMaxNotes; i++) {
            CHECK_EQ(int(iv.semitones[i]), 0);
        }
        // The deepest inversion that moves the voicing, and never deeper than two bits hold.
        const int deepest = chord_max_inversion(ChordType(id));
        CHECK_EQ(deepest, int(iv.count) - 1 < int(kChordInversionMax) ? int(iv.count) - 1
                                                                      : int(kChordInversionMax));
        CHECK(deepest <= int(kChordInversionMax));
    }
}

TEST(chord_names_are_upper_case_digits_and_fit_the_screen) {
    // The device font has upper case, digits and a few symbols only. "1 NOTE" rather than "OFF"
    // because a zero in this field does not switch anything off.
    CHECK_EQ(chord_name(ChordType::none), "1 NOTE");
    for (int id = 0; id < kChordTypeCount; id++) {
        const char *name = chord_name(ChordType(id));
        int length = 0;
        for (const char *p = name; *p != '\0'; p++) {
            const char c = *p;
            const bool allowed = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
            CHECK(allowed);
            length++;
        }
        CHECK(length > 0);
        CHECK(length <= 16);
    }
    // An id this build does not have reads as unknown rather than as a chord it is not.
    CHECK_EQ(chord_name(ChordType(kChordTypeCount)), "?");
    CHECK_EQ(chord_name(ChordType(15)), "?");
    CHECK_EQ(chord_name(ChordType(200)), "?");
}

// --- the packing ----------------------------------------------------------

TEST(chord_the_packing_round_trips_through_the_byte) {
    for (int id = 1; id < kChordTypeCount; id++) {
        const ChordType type = ChordType(id);
        const int deepest = chord_max_inversion(type);
        for (uint8_t inversion = 0; inversion <= kChordInversionMax; inversion++) {
            const uint8_t packed = chord_pack(type, inversion);
            CHECK_EQ(int(chord_type(packed)), id);
            // Clamped, not wrapped: a writer never produces two bytes that sound the same.
            const int want = int(inversion) < deepest ? int(inversion) : deepest;
            CHECK_EQ(int(chord_inversion(packed)), want);
            CHECK(chord_valid(packed));
            // The two halves live in their own bits and nothing else is set.
            CHECK_EQ(int(packed & kChordTypeMask), id);
            CHECK_EQ(int((packed >> kChordInversionShift) & kChordInversionMask), want);
            CHECK_EQ(int(packed & kChordReservedMask), 0);
        }
    }
}

TEST(chord_the_step_editor_walks_every_valid_chord_and_nothing_else) {
    // The encoder used to walk the raw byte 0..63, two thirds of which is not a chord. It walks
    // the valid packings now, in type order and then by inversion, and each one exactly once.
    const int positions = step_field_positions(StepField::chord);
    CHECK_EQ(positions, kChordWalkPositions);
    int valid = 0;
    for (int byte = 0; byte < 256; byte++) valid += chord_valid(uint8_t(byte)) ? 1 : 0;
    CHECK_EQ(valid, positions);

    uint8_t previous = 0;
    for (int p = 0; p < positions; p++) {
        const uint8_t packed = chord_at_walk_position(p);
        CHECK(chord_valid(packed));
        CHECK_EQ(chord_walk_position(packed), p);
        if (p > 0) {
            const bool later_type = int(chord_type(packed)) > int(chord_type(previous));
            const bool next_inversion = chord_type(packed) == chord_type(previous) &&
                                        chord_inversion(packed) == chord_inversion(previous) + 1;
            CHECK(later_type || next_inversion);
        }
        previous = packed;
    }
    // The packing itself is unchanged: six bits, reserved bits on top.
    CHECK_EQ(int(kChordTypeMask) + 1, kChordTypeIds);
    CHECK_EQ(int(kChordInversionMask) << kChordInversionShift, 0x30);
    CHECK_EQ(int(kChordReservedMask), 0xC0);
}

TEST(chord_an_id_this_build_does_not_have_is_a_single_note) {
    // Ids 14 and 15 are the growth room, and core/project.cpp's rule for scales applies here
    // too: a byte a later build wrote must survive a load and a save, so it has to be playable.
    // Quietly, as one note -- not as whatever chord the low bits happen to name.
    uint8_t notes[kChordMaxNotes] = {};
    for (int id = kChordTypeCount; id < kChordTypeIds; id++) {
        for (uint8_t inversion = 0; inversion <= kChordInversionMax; inversion++) {
            const uint8_t packed = uint8_t(id | (inversion << kChordInversionShift));
            CHECK_EQ(int(chord_type(packed)), int(ChordType::none));
            CHECK_EQ(int(chord_inversion(packed)), 0);
            CHECK(!chord_valid(packed));
            CHECK_EQ(chord_expand(60, packed, 0, int(Scale::minor), notes), 1);
            CHECK_EQ(int(notes[0]), 60);
        }
    }

    // A reserved bit means the byte is not a chord at all, whatever the low bits say. Masking
    // them away instead would play a guess: MIN for 0xC1, which a build that understood 0xC1
    // might mean as something else entirely.
    for (uint8_t packed : {uint8_t(0x40), uint8_t(0x80), uint8_t(0xC1), uint8_t(0xFF)}) {
        CHECK_EQ(int(chord_type(packed)), int(ChordType::none));
        CHECK_EQ(int(chord_inversion(packed)), 0);
        CHECK(!chord_valid(packed));
        CHECK_EQ(chord_expand(60, packed, 0, int(Scale::minor), notes), 1);
        CHECK_EQ(int(notes[0]), 60);
    }

    // An inversion with no chord under it is not a chord either, and must not report one: the
    // UI would draw a first inversion for a step that plays one note.
    CHECK_EQ(int(chord_inversion(0x10)), 0);
    CHECK(!chord_valid(0x10));
    CHECK_EQ(chord_expand(60, 0x10, 0, int(Scale::minor), notes), 1);
    CHECK_EQ(int(notes[0]), 60);
}

TEST(chord_valid_accepts_exactly_the_bytes_a_writer_produces) {
    // The property, rather than a list: a byte is valid exactly when packing what it reads back
    // as reproduces it. That is what "canonical" has to mean for a field that goes on disk.
    int disagreed = 0;
    for (int byte = 0; byte <= 255; byte++) {
        const uint8_t packed = uint8_t(byte);
        const uint8_t inversion =
            uint8_t((packed >> kChordInversionShift) & kChordInversionMask);
        const bool canonical = chord_pack(chord_type(packed), inversion) == packed;
        if (chord_valid(packed) != canonical) disagreed++;
    }
    CHECK_EQ(disagreed, 0);

    // Deeper than the chord can use is not canonical: MIN has three notes, so inversion 3 is a
    // byte no writer here emits -- and it still plays, as MIN's second inversion.
    CHECK(!chord_valid(uint8_t(int(ChordType::minor) | 0x30)));
    CHECK(chord_valid(chord_pack(ChordType::minor, 3)));
    CHECK_EQ(int(chord_inversion(chord_pack(ChordType::minor, 3))), 2);
}

// --- expansion: totality --------------------------------------------------

TEST(chord_expansion_is_total_over_every_root_byte_key_and_scale) {
    // Nothing a pattern can hold -- including a chord byte and a scale id from a later build --
    // may produce a note outside 0..127, more notes than the caller has voices, two voices on
    // one pitch, or notes out of order. The audio side cannot refuse any of those once they
    // arrive, so this is counted rather than spot-checked.
    int out_of_range = 0, out_of_scale = 0, not_ascending = 0, too_many = 0, empty = 0;
    uint8_t notes[kChordMaxNotes] = {};

    for (int scale : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, kScaleCount, 99, 255}) {
        for (uint8_t key : {uint8_t(0), uint8_t(1), uint8_t(5), uint8_t(8), uint8_t(11)}) {
            for (int root = 0; root <= 127; root += 7) {
                for (int byte = 0; byte < 64; byte++) {
                    const int n =
                        chord_expand(uint8_t(root), uint8_t(byte), key, scale, notes);
                    if (n < 1) empty++;
                    if (n > kChordMaxNotes) too_many++;
                    if (n > int(chord_intervals(chord_type(uint8_t(byte))).count)) too_many++;
                    if (!ascending(notes, n)) not_ascending++;
                    if (!all_in_scale(notes, n, key, scale)) out_of_scale++;
                    for (int i = 0; i < n; i++) {
                        if (notes[i] > 127) out_of_range++;
                    }
                }
            }
        }
    }
    CHECK_EQ(out_of_range, 0);
    CHECK_EQ(out_of_scale, 0);
    CHECK_EQ(not_ascending, 0);
    CHECK_EQ(too_many, 0);
    CHECK_EQ(empty, 0);
}

TEST(chord_voices_bound_what_comes_out) {
    uint8_t notes[kChordMaxNotes] = {};

    // A caller with fewer voices than the chord has notes gets the bottom of the stack, which
    // is what makes it that chord: three voices of MIN11 are a minor triad.
    uint8_t few[kChordMaxNotes] = {};
    uint8_t triad[kChordMaxNotes] = {};
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min11, 0), 0, int(Scale::minor), few, 3), 3);
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::minor, 0), 0, int(Scale::minor), triad), 3);
    CHECK(same_notes(few, triad, 3));

    for (int id = 0; id < kChordTypeCount; id++) {
        const int have = int(chord_intervals(ChordType(id)).count);
        for (int max = 1; max <= 8; max++) {
            const int want = have < max ? have : max;
            CHECK_EQ(chord_expand(60, chord_pack(ChordType(id), 0), 0, int(Scale::minor), notes,
                                  max),
                     want <= kChordMaxNotes ? want : kChordMaxNotes);
        }
    }

    // No voices is nothing to play, and it is the only empty answer. A null buffer likewise
    // writes nothing rather than one note somewhere.
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min7, 0), 0, int(Scale::minor), notes, 0), 0);
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min7, 0), 0, int(Scale::minor), notes, -4),
             0);
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min7, 0), 0, int(Scale::minor), nullptr), 0);
}

TEST(chord_at_the_top_of_the_midi_range_a_chord_thins_rather_than_piling_up) {
    // A note above 127 is dropped, not clamped: clamping would spend several of six voices on
    // note 127 at once, which is louder and less like the chord than not having that note.
    uint8_t notes[kChordMaxNotes] = {};
    const int n = chord_expand(127, chord_pack(ChordType::min11, 0), 0, int(Scale::minor), notes);
    CHECK_EQ(n, 1);
    CHECK_EQ(int(notes[0]), 127);

    const int wide = chord_expand(120, chord_pack(ChordType::octave, 0), 0, int(Scale::major),
                                  notes);
    CHECK_EQ(wide, 1);                 // 120 + 12 is 132, so OCT has one note up here
    CHECK_EQ(int(notes[0]), 120);

    // Every type at the very top: at least the root, never more than its stack, never above 127.
    for (int id = 0; id < kChordTypeCount; id++) {
        for (int root = 120; root <= 127; root++) {
            const int count = chord_expand(uint8_t(root), chord_pack(ChordType(id), 0), 0,
                                           int(Scale::minor), notes);
            CHECK(count >= 1);
            CHECK(ascending(notes, count));
            for (int i = 0; i < count; i++) CHECK(notes[i] <= 127);
        }
    }
}

TEST(chord_sus4_at_the_top_of_a_narrow_key_drops_the_note_it_cannot_separate) {
    // The one place in the whole table where two intervals fold onto one pitch through
    // chord_expand: the fourth and the fifth both land on the highest note these keys have, and
    // there is nothing above it to lift the second one to, so it is dropped rather than doubled.
    // Found by sweeping, not by thinking, and pinned here so that a change in core/scale.hpp's
    // table or in the fold shows up as a test rather than as a doubled voice.
    uint8_t notes[kChordMaxNotes] = {};
    const uint8_t sus4 = chord_pack(ChordType::sus4, 0);

    struct Case {
        int scale;
        uint8_t key;
    };
    const Case cases[4] = {{int(Scale::harmonic_minor), 9},
                           {int(Scale::minor_pentatonic), 5},
                           {int(Scale::major_pentatonic), 8},
                           {int(Scale::blues), 5}};
    for (const Case &c : cases) {
        for (int root = 120; root <= 121; root++) {
            const int n = chord_expand(uint8_t(root), sus4, c.key, c.scale, notes);
            CHECK_EQ(n, 2);
            CHECK_EQ(int(notes[0]), 120);
            CHECK_EQ(int(notes[1]), 125);
            CHECK(all_in_scale(notes, n, c.key, c.scale));
        }
    }
}

// --- expansion: the key ---------------------------------------------------

TEST(chord_only_the_keys_pitch_class_matters) {
    // core/scale.hpp reads only the root's pitch class, and a chord must not change when the
    // project's key moves an octave.
    uint8_t at_0[kChordMaxNotes] = {};
    uint8_t elsewhere[kChordMaxNotes] = {};
    int differed = 0;
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (int byte = 0; byte < 64; byte++) {
            for (int root = 24; root <= 108; root += 11) {
                const int n = chord_expand(uint8_t(root), uint8_t(byte), 0, scale, at_0);
                for (uint8_t key : {uint8_t(12), uint8_t(60), uint8_t(120)}) {
                    if (chord_expand(uint8_t(root), uint8_t(byte), key, scale, elsewhere) != n) {
                        differed++;
                    } else if (!same_notes(at_0, elsewhere, n)) {
                        differed++;
                    }
                }
            }
        }
    }
    CHECK_EQ(differed, 0);
}

TEST(chord_the_root_is_folded_into_the_key_before_the_stack_goes_on_it) {
    // A chord is one harmonic object: folding its third and fifth into the key while leaving its
    // root outside would put the one note that defines it -- and the loudest -- out of the key.
    uint8_t notes[kChordMaxNotes] = {};

    // 61 is not in C minor; it folds to 60, and the chord is built on 60.
    CHECK(!scale_contains(61, 0, int(Scale::minor)));
    CHECK_EQ(chord_expand(61, chord_pack(ChordType::minor, 0), 0, int(Scale::minor), notes), 3);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 63);
    CHECK_EQ(int(notes[2]), 67);

    // In root position the lowest note is always the folded root, for every chord and key.
    int wrong = 0;
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (uint8_t key = 0; key < 12; key++) {
            for (int id = 0; id < kChordTypeCount; id++) {
                for (int root = 12; root <= 108; root += 5) {
                    const int n = chord_expand(uint8_t(root), chord_pack(ChordType(id), 0), key,
                                               scale, notes);
                    if (n < 1 || notes[0] != scale_fold(uint8_t(root), key, scale)) wrong++;
                }
            }
        }
    }
    CHECK_EQ(wrong, 0);
}

TEST(chord_under_chromatic_the_table_comes_out_exactly) {
    // CHROMATIC is "no key", folding is the identity under it, and that is the way out of the
    // trade the header documents: a stab moved around without a key keeps its quality.
    uint8_t notes[kChordMaxNotes] = {};
    for (int id = 0; id < kChordTypeCount; id++) {
        const ChordIntervals iv = chord_intervals(ChordType(id));
        const int n = chord_expand(60, chord_pack(ChordType(id), 0), 0, int(Scale::chromatic),
                                   notes);
        CHECK_EQ(n, int(iv.count));
        for (int i = 0; i < n; i++) CHECK_EQ(int(notes[i]), 60 + int(iv.semitones[i]));
    }
    // A minor and a major third are two different chords here, where in a minor key they are
    // not (see chord_a_quality_the_key_does_not_have_folds_to_the_nearest_voicing).
    uint8_t minor[kChordMaxNotes] = {};
    uint8_t major[kChordMaxNotes] = {};
    chord_expand(60, chord_pack(ChordType::minor, 0), 0, int(Scale::chromatic), minor);
    chord_expand(60, chord_pack(ChordType::major, 0), 0, int(Scale::chromatic), major);
    CHECK(!same_notes(minor, major, 3));
    CHECK_EQ(int(minor[1]), 63);
    CHECK_EQ(int(major[1]), 64);
}

TEST(chord_a_major_stack_is_the_diatonic_triad_on_every_degree) {
    // The property the whole semitones-then-fold decision rests on, and the answer to "is a
    // chord on the sixth of a minor scale different from one on the root": a major stack folded
    // onto each degree of a diatonic mode comes out as exactly that degree's diatonic triad --
    // the scale's own first, third and fifth from that degree -- in all seven modes. So folding
    // gives the diatonic behaviour without a second mechanism for it.
    uint8_t notes[kChordMaxNotes] = {};
    const uint8_t major = chord_pack(ChordType::major, 0);
    int modes = 0, wrong = 0;

    for (int scale = 0; scale < kScaleCount; scale++) {
        if (!is_diatonic_mode(scale)) continue;
        modes++;
        for (uint8_t key = 0; key < 12; key++) {
            for (int degree = 0; degree < 7; degree++) {
                const uint8_t root = scale_step(60, key, scale, degree);
                const int n = chord_expand(root, major, key, scale, notes);
                if (n != 3) {
                    wrong++;
                    continue;
                }
                if (notes[0] != root) wrong++;
                if (notes[1] != scale_step(root, key, scale, 2)) wrong++;
                if (notes[2] != scale_step(root, key, scale, 4)) wrong++;
            }
        }
    }
    CHECK_EQ(modes, 7);
    CHECK_EQ(wrong, 0);

    // What that means in C minor, spelled out: the chord on the root is minor, the chord on the
    // sixth is major, from the same chord byte.
    const int on_root = chord_expand(60, major, 0, int(Scale::minor), notes);
    CHECK_EQ(on_root, 3);
    CHECK_EQ(int(notes[1]) - int(notes[0]), 3);          // C Eb G: a minor third
    CHECK_EQ(int(notes[2]) - int(notes[0]), 7);
    const int on_sixth = chord_expand(68, major, 0, int(Scale::minor), notes);
    CHECK_EQ(on_sixth, 3);
    CHECK_EQ(int(notes[1]) - int(notes[0]), 4);          // Ab C Eb: a major third
    CHECK_EQ(int(notes[2]) - int(notes[0]), 7);
}

TEST(chord_a_quality_the_key_does_not_have_folds_to_the_nearest_voicing) {
    // The cost of folding, pinned so that it is a decision and not a surprise: C minor has no
    // major third, so MAJ on its root is the nearest voicing the key allows, which is MIN.
    // CHROMATIC is how you ask for the other thing.
    uint8_t minor[kChordMaxNotes] = {};
    uint8_t major[kChordMaxNotes] = {};
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::minor, 0), 0, int(Scale::minor), minor), 3);
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::major, 0), 0, int(Scale::minor), major), 3);
    CHECK(same_notes(minor, major, 3));
    CHECK_EQ(int(minor[0]), 60);
    CHECK_EQ(int(minor[1]), 63);
    CHECK_EQ(int(minor[2]), 67);

    // MIN7 in C minor is the stab it is named after, untouched: C Eb G Bb.
    uint8_t stab[kChordMaxNotes] = {};
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min7, 0), 0, int(Scale::minor), stab), 4);
    CHECK_EQ(int(stab[0]), 60);
    CHECK_EQ(int(stab[1]), 63);
    CHECK_EQ(int(stab[2]), 67);
    CHECK_EQ(int(stab[3]), 70);
}

// --- inversions -----------------------------------------------------------

TEST(chord_inversion_moves_the_voicing) {
    uint8_t root_position[kChordMaxNotes] = {};
    uint8_t inverted[kChordMaxNotes] = {};

    // MIN7 in C minor, all four voicings, by value: each one lifts the bottom note an octave.
    const int expect[4][4] = {{60, 63, 67, 70},
                              {63, 67, 70, 72},
                              {67, 70, 72, 75},
                              {70, 72, 75, 79}};
    for (uint8_t inversion = 0; inversion <= 3; inversion++) {
        const int n = chord_expand(60, chord_pack(ChordType::min7, inversion), 0,
                                   int(Scale::minor), inverted);
        CHECK_EQ(n, 4);
        for (int i = 0; i < 4; i++) CHECK_EQ(int(inverted[i]), expect[inversion][i]);
    }

    // And in general: an inversion a chord can use moves its lowest note up, away from the ends
    // of the range where the header says it saturates instead. OCT is the exception it names --
    // its octave up is already in the chord -- so it is excluded by `chord_invert`'s own answer.
    int unmoved = 0;
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (int id = 1; id < kChordTypeCount; id++) {
            const int deepest = chord_max_inversion(ChordType(id));
            for (int inversion = 1; inversion <= deepest; inversion++) {
                const int n = chord_expand(48, chord_pack(ChordType(id), 0), 0, scale,
                                           root_position);
                uint8_t scratch[kChordMaxNotes] = {};
                for (int i = 0; i < n; i++) scratch[i] = root_position[i];
                if (chord_invert(scratch, n, inversion) != inversion) continue;   // blocked
                chord_expand(48, chord_pack(ChordType(id), uint8_t(inversion)), 0, scale,
                             inverted);
                if (inverted[0] <= root_position[0]) unmoved++;
                if (same_notes(root_position, inverted, n)) unmoved++;
            }
        }
    }
    CHECK_EQ(unmoved, 0);
}

TEST(chord_inversion_keeps_the_notes_and_their_pitch_classes) {
    // What separates an inversion from everything else in this module: the same notes, the same
    // count, one or more of them an octave up. If an inversion ever changed the note count, the
    // voice pool would be allocated for one chord and handed another.
    int changed_count = 0, changed_classes = 0, not_ascending = 0, out_of_scale = 0;
    uint8_t base[kChordMaxNotes] = {};
    uint8_t inverted[kChordMaxNotes] = {};
    int classes_base[12] = {};
    int classes_inverted[12] = {};

    for (int scale = 0; scale < kScaleCount; scale++) {
        for (uint8_t key = 0; key < 12; key += 4) {
            for (int id = 0; id < kChordTypeCount; id++) {
                for (int root = 24; root <= 108; root += 5) {
                    const int n =
                        chord_expand(uint8_t(root), chord_pack(ChordType(id), 0), key, scale,
                                     base);
                    pitch_classes(base, n, classes_base);
                    for (uint8_t inversion = 1; inversion <= kChordInversionMax; inversion++) {
                        const int m = chord_expand(uint8_t(root),
                                                   chord_pack(ChordType(id), inversion), key,
                                                   scale, inverted);
                        if (m != n) changed_count++;
                        pitch_classes(inverted, m, classes_inverted);
                        for (int c = 0; c < 12; c++) {
                            if (classes_base[c] != classes_inverted[c]) changed_classes++;
                        }
                        if (!ascending(inverted, m)) not_ascending++;
                        if (!all_in_scale(inverted, m, key, scale)) out_of_scale++;
                    }
                }
            }
        }
    }
    CHECK_EQ(changed_count, 0);
    CHECK_EQ(changed_classes, 0);
    CHECK_EQ(not_ascending, 0);
    CHECK_EQ(out_of_scale, 0);
}

TEST(chord_inversion_keeps_the_notes_ascending_when_the_chord_is_wider_than_an_octave) {
    // MIN9 spans fourteen semitones, so its bottom note lifted an octave is NOT the new top
    // note: it belongs in the middle. Appending it instead would hand the voice pool a
    // descending pair, which nothing downstream checks.
    uint8_t notes[kChordMaxNotes] = {};
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min9, 0), 0, int(Scale::minor), notes), 5);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[4]), 74);

    CHECK_EQ(chord_expand(60, chord_pack(ChordType::min9, 1), 0, int(Scale::minor), notes), 5);
    CHECK_EQ(int(notes[0]), 63);
    CHECK_EQ(int(notes[1]), 67);
    CHECK_EQ(int(notes[2]), 70);
    CHECK_EQ(int(notes[3]), 72);          // 60 + 12, inserted, not appended
    CHECK_EQ(int(notes[4]), 74);
    CHECK(ascending(notes, 5));
}

TEST(chord_an_inversion_deeper_than_the_chord_is_the_deepest_one_it_has) {
    // Lifting as many notes as the chord has would transpose the whole thing an octave, which
    // the step's note field already does. A byte that asks for it plays the deepest inversion
    // that is still an inversion.
    uint8_t deep[kChordMaxNotes] = {};
    uint8_t deepest[kChordMaxNotes] = {};
    for (int id = 1; id < kChordTypeCount; id++) {
        const int limit = chord_max_inversion(ChordType(id));
        const int n = chord_expand(60, uint8_t(id | (uint8_t(limit) << kChordInversionShift)), 0,
                                   int(Scale::minor), deepest);
        for (int asked = limit; asked <= int(kChordInversionMax); asked++) {
            // The raw byte, not chord_pack's -- chord_pack clamps, and this is the hand-made
            // file the reader has to accept.
            const uint8_t raw = uint8_t(id | (uint8_t(asked) << kChordInversionShift));
            CHECK_EQ(chord_expand(60, raw, 0, int(Scale::minor), deep), n);
            CHECK(same_notes(deep, deepest, n));
        }
    }

    // A single note has no voicing at all, so nothing moves however deep the byte asks.
    uint8_t one[1] = {60};
    CHECK_EQ(chord_invert(one, 1, 3), 0);
    CHECK_EQ(int(one[0]), 60);
}

TEST(chord_inverting_an_octave_would_double_a_note_so_it_does_not) {
    // OCT's own octave up is its top note. Rotating would spend two of six voices on one pitch
    // at twice the level, which is not what an inversion is for -- so the voicing stays and the
    // return value says it did not move.
    uint8_t notes[2] = {60, 72};
    CHECK_EQ(chord_invert(notes, 2, 1), 0);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 72);

    uint8_t root_position[kChordMaxNotes] = {};
    uint8_t inverted[kChordMaxNotes] = {};
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::octave, 0), 0, int(Scale::minor),
                          root_position),
             2);
    CHECK_EQ(chord_expand(60, chord_pack(ChordType::octave, 1), 0, int(Scale::minor), inverted),
             2);
    CHECK(same_notes(root_position, inverted, 2));

    // 5TH OCT holds an octave too, so its first inversion is blocked for the same reason, and
    // its second therefore cannot happen either.
    uint8_t stack[3] = {60, 67, 72};
    CHECK_EQ(chord_invert(stack, 3, 2), 0);
    CHECK_EQ(int(stack[0]), 60);
}

TEST(chord_inversion_stops_at_the_top_of_the_range) {
    // Part of the way up is allowed: what cannot happen is a note above 127 or a voicing that
    // has stopped ascending.
    uint8_t two_of_two[3] = {100, 104, 107};
    CHECK_EQ(chord_invert(two_of_two, 3, 2), 2);
    CHECK_EQ(int(two_of_two[0]), 107);
    CHECK_EQ(int(two_of_two[1]), 112);
    CHECK_EQ(int(two_of_two[2]), 116);

    uint8_t one_of_two[3] = {113, 120, 127};
    CHECK_EQ(chord_invert(one_of_two, 3, 2), 1);     // 113 + 12 fits, 120 + 12 does not
    CHECK_EQ(int(one_of_two[0]), 120);
    CHECK_EQ(int(one_of_two[1]), 125);
    CHECK_EQ(int(one_of_two[2]), 127);
    CHECK(ascending(one_of_two, 3));

    uint8_t none_at_all[3] = {120, 123, 127};
    CHECK_EQ(chord_invert(none_at_all, 3, 2), 0);
    CHECK_EQ(int(none_at_all[0]), 120);
    CHECK_EQ(int(none_at_all[2]), 127);

    // Degenerate asks are answered, not refused.
    CHECK_EQ(chord_invert(nullptr, 3, 1), 0);
    CHECK_EQ(chord_invert(none_at_all, 0, 1), 0);
    CHECK_EQ(chord_invert(none_at_all, 3, 0), 0);
    CHECK_EQ(chord_invert(none_at_all, 3, -2), 0);
}

// --- the fold, driven directly --------------------------------------------

TEST(chord_fold_lifts_a_note_the_key_folded_onto_its_neighbour) {
    // The branch no chord in the table can reach through chord_expand: a stack whose intervals
    // fold onto one another. A cluster is the shortest way to force it, and the lift has to be
    // right before a sparser scale in core/scale.hpp makes the real chords reach it.
    const int8_t cluster[3] = {0, 1, 2};
    uint8_t notes[kChordMaxNotes] = {};

    // C major: 61 folds down onto 60, so it is lifted to 62; 62 is then on 62, so it goes to 64.
    CHECK_EQ(chord_fold(cluster, 3, 60, 0, int(Scale::major), notes, kChordMaxNotes), 3);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 62);
    CHECK_EQ(int(notes[2]), 64);

    // C minor, where the scale steps are not the same sizes: 60, then 62, then 63.
    CHECK_EQ(chord_fold(cluster, 3, 60, 0, int(Scale::minor), notes, kChordMaxNotes), 3);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 62);
    CHECK_EQ(int(notes[2]), 63);

    // Six intervals inside two semitones still come out as six separate notes of the key.
    const int8_t pile[6] = {0, 0, 1, 1, 2, 2};
    CHECK_EQ(chord_fold(pile, 6, 60, 0, int(Scale::minor), notes, kChordMaxNotes), 6);
    CHECK(ascending(notes, 6));
    CHECK(all_in_scale(notes, 6, 0, int(Scale::minor)));
}

TEST(chord_fold_drops_a_note_it_cannot_lift_rather_than_doubling_it) {
    uint8_t notes[kChordMaxNotes] = {};

    // At the top of the range there is nothing to lift to, so the second note goes.
    const int8_t twice[2] = {0, 0};
    CHECK(scale_contains(127, 0, int(Scale::major)));
    CHECK_EQ(chord_fold(twice, 2, 127, 0, int(Scale::major), notes, kChordMaxNotes), 1);
    CHECK_EQ(int(notes[0]), 127);

    // Above the range it is dropped rather than clamped onto 127.
    const int8_t octave[2] = {0, 12};
    CHECK_EQ(chord_fold(octave, 2, 120, 0, int(Scale::major), notes, kChordMaxNotes), 1);
    CHECK_EQ(int(notes[0]), 120);

    // Below it too: this function takes a caller's stack, which may reach down.
    const int8_t under[2] = {0, -12};
    CHECK_EQ(chord_fold(under, 2, 5, 0, int(Scale::chromatic), notes, kChordMaxNotes), 1);
    CHECK_EQ(int(notes[0]), 5);
}

TEST(chord_fold_never_writes_more_than_the_caller_asked_for) {
    const int8_t eight[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    uint8_t notes[kChordMaxNotes] = {};

    // Six voices is the bound whatever the caller or the stack says.
    CHECK_EQ(chord_fold(eight, 8, 60, 0, int(Scale::major), notes, 99), kChordMaxNotes);
    CHECK_EQ(chord_fold(eight, 8, 60, 0, int(Scale::major), notes, 2), 2);
    CHECK_EQ(chord_fold(eight, 3, 60, 0, int(Scale::major), notes, 6), 3);
    CHECK_EQ(chord_fold(eight, 0, 60, 0, int(Scale::major), notes, 6), 0);
    CHECK_EQ(chord_fold(eight, -1, 60, 0, int(Scale::major), notes, 6), 0);
    CHECK_EQ(chord_fold(eight, 8, 60, 0, int(Scale::major), notes, 0), 0);
    CHECK_EQ(chord_fold(nullptr, 3, 60, 0, int(Scale::major), notes, 6), 0);
    CHECK_EQ(chord_fold(eight, 3, 60, 0, int(Scale::major), nullptr, 6), 0);
}
