// Keys and scales.
//
// The table in core/scale.hpp is a decision this project made rather than one docs/SPEC.md
// recorded, and its ids go on disk permanently, so the first tests here pin the intervals
// against the names a musician would check them by. The first draft of that table had six of
// fourteen masks disagreeing with their own comments, which is why they are checked by value
// and not by eye.
//
// The rest are the properties the rest of M5 and M6 lean on: folding is total over every note,
// root and scale; stepping by degrees stays in the key; and nothing ever leaves 0..127.
#include "check.hpp"

#include "scale.hpp"

#include <initializer_list>

namespace {

using namespace strata;

// Does the scale contain exactly these semitones above the root, and nothing else?
bool intervals_are(int scale, std::initializer_list<int> want) {
    uint16_t expect = 0;
    for (int i : want) expect = uint16_t(expect | uint16_t(1u << i));
    return scale_mask(scale) == expect;
}

}  // namespace

// --- the table ------------------------------------------------------------

TEST(scale_intervals_match_their_names) {
    // Checked against the usual definitions. A wrong bit here is a pattern that comes back in a
    // different key, which is silent and musical rather than loud.
    CHECK(intervals_are(int(Scale::chromatic), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));
    CHECK(intervals_are(int(Scale::minor), {0, 2, 3, 5, 7, 8, 10}));
    CHECK(intervals_are(int(Scale::major), {0, 2, 4, 5, 7, 9, 11}));
    CHECK(intervals_are(int(Scale::dorian), {0, 2, 3, 5, 7, 9, 10}));
    CHECK(intervals_are(int(Scale::phrygian), {0, 1, 3, 5, 7, 8, 10}));
    CHECK(intervals_are(int(Scale::lydian), {0, 2, 4, 6, 7, 9, 11}));
    CHECK(intervals_are(int(Scale::mixolydian), {0, 2, 4, 5, 7, 9, 10}));
    CHECK(intervals_are(int(Scale::locrian), {0, 1, 3, 5, 6, 8, 10}));
    CHECK(intervals_are(int(Scale::harmonic_minor), {0, 2, 3, 5, 7, 8, 11}));
    CHECK(intervals_are(int(Scale::melodic_minor), {0, 2, 3, 5, 7, 9, 11}));
    CHECK(intervals_are(int(Scale::minor_pentatonic), {0, 3, 5, 7, 10}));
    CHECK(intervals_are(int(Scale::major_pentatonic), {0, 2, 4, 7, 9}));
    CHECK(intervals_are(int(Scale::blues), {0, 3, 5, 6, 7, 10}));
    CHECK(intervals_are(int(Scale::whole_tone), {0, 2, 4, 6, 8, 10}));
    CHECK(intervals_are(int(Scale::diminished), {0, 2, 3, 5, 6, 8, 9, 11}));
}

TEST(scale_the_modes_are_rotations_of_each_other) {
    // A property rather than a restatement of the table: the seven diatonic modes are the same
    // seven notes started in different places, so rotating major's mask by the mode's own first
    // interval must give that mode's mask. A single wrong bit in any of the seven fails this
    // even when it happens to match its comment.
    const int modes[7] = {int(Scale::major),      int(Scale::dorian),     int(Scale::phrygian),
                          int(Scale::lydian),     int(Scale::mixolydian), int(Scale::minor),
                          int(Scale::locrian)};
    const int starts[7] = {0, 2, 4, 5, 7, 9, 11};   // the degrees of the major scale
    const uint16_t major = scale_mask(int(Scale::major));

    for (int m = 0; m < 7; m++) {
        const int shift = starts[m];
        uint16_t rotated = 0;
        for (int i = 0; i < 12; i++) {
            if ((major >> ((i + shift) % 12) & 1u) != 0) rotated = uint16_t(rotated | (1u << i));
        }
        CHECK_EQ(int(scale_mask(modes[m])), int(rotated));
    }
}

TEST(scale_sizes_are_what_the_names_promise) {
    CHECK_EQ(scale_size(int(Scale::chromatic)), 12);
    CHECK_EQ(scale_size(int(Scale::minor)), 7);
    CHECK_EQ(scale_size(int(Scale::major)), 7);
    CHECK_EQ(scale_size(int(Scale::minor_pentatonic)), 5);
    CHECK_EQ(scale_size(int(Scale::major_pentatonic)), 5);
    CHECK_EQ(scale_size(int(Scale::blues)), 6);
    CHECK_EQ(scale_size(int(Scale::whole_tone)), 6);
    CHECK_EQ(scale_size(int(Scale::diminished)), 8);

    // Every scale has a root and at most twelve notes.
    for (int s = 0; s < kScaleCount; s++) {
        CHECK((scale_mask(s) & 1u) != 0);
        CHECK(scale_size(s) >= 1);
        CHECK(scale_size(s) <= 12);
    }
}

TEST(scale_an_unknown_id_plays_chromatically_but_is_named_unknown) {
    // core/project.cpp deliberately does not clamp the scale byte: an id a later build wrote
    // must survive a load and a save here. So it has to be playable, and it has to be drawn as
    // something other than a scale this build pretends to know.
    for (int id : {kScaleCount, kScaleCount + 1, 99, 255}) {
        CHECK_EQ(int(scale_mask(id)), 0x0FFF);
        CHECK_EQ(scale_size(id), 12);
        CHECK_EQ(scale_name(id), "?");
        CHECK(scale_contains(61, 60, id));
        CHECK_EQ(int(scale_fold(61, 60, id)), 61);
    }
    CHECK(scale_name(int(Scale::minor))[0] != '?');
}

TEST(scale_names_are_upper_case_and_fit_the_screen) {
    // The device font has upper case, digits and a few symbols only, and a 128-pixel line of the
    // 5x7 font holds 25 characters.
    for (int s = 0; s < kScaleCount; s++) {
        const char *name = scale_name(s);
        int length = 0;
        for (const char *p = name; *p != '\0'; p++) {
            const char c = *p;
            const bool allowed = (c >= 'A' && c <= 'Z') || c == ' ';
            if (!allowed) CHECK_EQ(int(c), int('?'));
            length++;
        }
        CHECK(length > 0);
        CHECK(length <= 16);
    }
}

TEST(scale_note_names_are_the_twelve_pitch_classes_in_flats) {
    const char *want[12] = {"C", "DB", "D",  "EB", "E",  "F",
                            "GB", "G",  "AB", "A",  "BB", "B"};
    for (int i = 0; i < 12; i++) CHECK_EQ(note_name(uint8_t(i)), want[i]);

    // Only the pitch class is read, so every octave names the same note.
    for (int note = 0; note <= 127; note++) {
        CHECK_EQ(note_name(uint8_t(note)), want[note % 12]);
    }

    // Upper case and nothing the font might not have: no '#', no lower case.
    for (int i = 0; i < 12; i++) {
        for (const char *p = note_name(uint8_t(i)); *p != '\0'; p++) {
            CHECK(*p >= 'A' && *p <= 'G');
        }
    }
}

TEST(scale_octave_numbering_puts_middle_c_at_four) {
    static_assert(note_octave(60) == 4, "middle C is C4 in this convention");
    CHECK_EQ(note_octave(60), 4);
    CHECK_EQ(note_octave(0), -1);
    CHECK_EQ(note_octave(127), 9);
    CHECK_EQ(note_octave(72), 5);
}

// --- the key --------------------------------------------------------------

TEST(scale_only_the_roots_pitch_class_matters) {
    // docs/SPEC.md's "global key" is a MIDI note 0..127, but a key is a pitch class: C2 and C5
    // are the same key, and a project whose root moved an octave must not change which notes
    // are in the scale.
    for (int s = 0; s < kScaleCount; s++) {
        for (int note = 0; note <= 127; note++) {
            const bool at_0 = scale_contains(uint8_t(note), 0, s);
            CHECK_EQ(scale_contains(uint8_t(note), 12, s), at_0);
            CHECK_EQ(scale_contains(uint8_t(note), 60, s), at_0);
            CHECK_EQ(scale_contains(uint8_t(note), 120, s), at_0);
        }
    }
}

TEST(scale_contains_the_root_in_every_octave) {
    for (int s = 0; s < kScaleCount; s++) {
        for (int octave = 0; octave < 10; octave++) {
            const int note = 60 % 12 + octave * 12;
            if (note > 127) break;
            CHECK(scale_contains(uint8_t(note), 60, s));
        }
    }
}

TEST(scale_a_minor_scale_in_a_holds_the_white_notes) {
    // A natural minor is the white notes: A B C D E F G. Root 69 is A4, so 69..80 should admit
    // exactly 69, 71, 72, 74, 76, 77, 79.
    const int want[7] = {69, 71, 72, 74, 76, 77, 79};
    int found = 0;
    for (int note = 69; note <= 80; note++) {
        if (!scale_contains(uint8_t(note), 69, int(Scale::minor))) continue;
        CHECK_EQ(note, want[found]);
        found++;
    }
    CHECK_EQ(found, 7);
}

// --- folding --------------------------------------------------------------

TEST(scale_folding_leaves_a_note_already_in_the_scale_alone) {
    for (int s = 0; s < kScaleCount; s++) {
        for (int note = 0; note <= 127; note++) {
            if (!scale_contains(uint8_t(note), 60, s)) continue;
            CHECK_EQ(int(scale_fold(uint8_t(note), 60, s)), note);
        }
    }
}

TEST(scale_folding_always_lands_in_the_scale_and_in_range) {
    // Total over every note, every root and every scale. This is the property the mutation and
    // KEYS mode rely on, and the one that has to hold at the ends of the MIDI range where one
    // search direction runs out.
    int out_of_scale = 0, out_of_range = 0, far = 0;
    for (int s = 0; s < kScaleCount; s++) {
        for (int root = 0; root <= 127; root++) {
            for (int note = 0; note <= 127; note++) {
                const uint8_t got = scale_fold(uint8_t(note), uint8_t(root), s);
                if (!scale_contains(got, uint8_t(root), s)) out_of_scale++;
                if (got > 127) out_of_range++;
                const int distance = int(got) - note;
                if (distance > 6 || distance < -6) far++;
            }
        }
    }
    CHECK_EQ(out_of_scale, 0);
    CHECK_EQ(out_of_range, 0);
    // Nothing is ever more than six semitones away: the sparsest scale has one note in twelve.
    CHECK_EQ(far, 0);
}

TEST(scale_folding_is_never_further_than_half_a_gap) {
    // The real bound on folding, rather than the loop's defensive six. For each scale, the
    // widest gap between consecutive notes decides how far a fold can possibly need to go: a
    // note in the middle of a gap of `g` is at most `g / 2` from one end of it. A search bound
    // too small for some future sparse scale fails here, where the blanket "within six" of
    // `scale_folding_always_lands_in_the_scale_and_in_range` would not.
    for (int s = 0; s < kScaleCount; s++) {
        // The widest gap, counting the wrap from the last note back to the root an octave up.
        int widest = 0, previous = -1, first = -1;
        for (int i = 0; i < 12; i++) {
            if ((scale_mask(s) >> i & 1u) == 0) continue;
            if (first < 0) first = i;
            if (previous >= 0 && i - previous > widest) widest = i - previous;
            previous = i;
        }
        const int wrap = first + 12 - previous;
        if (wrap > widest) widest = wrap;
        const int bound = widest / 2;

        int too_far = 0;
        for (int root = 0; root < 12; root++) {
            for (int note = 12; note <= 115; note++) {   // clear of the saturating ends
                const int moved =
                    int(scale_fold(uint8_t(note), uint8_t(root), s)) - note;
                if (moved > bound || moved < -bound) too_far++;
            }
        }
        CHECK_EQ(too_far, 0);
    }
}

TEST(scale_folding_prefers_the_lower_note_on_a_tie) {
    // A tie only happens in the symmetric scales. Whole tone from the root has every other
    // semitone, so an odd note is one away in both directions, and down is the one that keeps a
    // bass line from drifting upwards every time a mutation folds it.
    CHECK_EQ(int(scale_fold(61, 60, int(Scale::whole_tone))), 60);
    CHECK_EQ(int(scale_fold(63, 60, int(Scale::whole_tone))), 62);
    CHECK_EQ(int(scale_fold(71, 60, int(Scale::whole_tone))), 70);
}

TEST(scale_folding_at_the_ends_of_the_range_turns_round) {
    // 127 is G9. In C major 127 is a G, so it stays; make a scale where it does not and the fold
    // has to come down rather than leave the range.
    CHECK_EQ(int(scale_fold(127, 60, int(Scale::major))), 127);       // G is in C major

    // Root C, whole tone: C D E F# G# A#, so 127 (G) is not in it and 126 (F#) is.
    CHECK(!scale_contains(127, 60, int(Scale::whole_tone)));
    CHECK_EQ(int(scale_fold(127, 60, int(Scale::whole_tone))), 126);

    // And at the bottom: 0 is C, which is in C major; 1 is C# and must fold to 0, not to -1.
    CHECK_EQ(int(scale_fold(0, 60, int(Scale::major))), 0);
    CHECK_EQ(int(scale_fold(1, 60, int(Scale::major))), 0);
}

// --- degrees --------------------------------------------------------------

TEST(scale_degree_numbering_starts_at_the_root) {
    CHECK_EQ(scale_degree_of(60, 60, int(Scale::major)), 0);
    CHECK_EQ(scale_degree_of(62, 60, int(Scale::major)), 1);
    CHECK_EQ(scale_degree_of(64, 60, int(Scale::major)), 2);
    CHECK_EQ(scale_degree_of(71, 60, int(Scale::major)), 6);
    CHECK_EQ(scale_degree_of(72, 60, int(Scale::major)), 0);   // the octave is the root again
    CHECK_EQ(scale_degree_of(61, 60, int(Scale::major)), -1);  // not in the scale

    // The pentatonic's degrees are three semitones apart in one place and two in another, which
    // is the whole reason stepping walks the mask instead of doing arithmetic.
    CHECK_EQ(scale_degree_of(60, 60, int(Scale::minor_pentatonic)), 0);
    CHECK_EQ(scale_degree_of(63, 60, int(Scale::minor_pentatonic)), 1);
    CHECK_EQ(scale_degree_of(65, 60, int(Scale::minor_pentatonic)), 2);
    CHECK_EQ(scale_degree_of(67, 60, int(Scale::minor_pentatonic)), 3);
    CHECK_EQ(scale_degree_of(70, 60, int(Scale::minor_pentatonic)), 4);
}

TEST(scale_stepping_moves_by_degrees_not_semitones) {
    // One degree up the minor pentatonic from the root is three semitones, and the next is two.
    // A transpose by semitones cannot do this, which is what "notes in scale" means.
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), 1)), 63);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), 2)), 65);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), 3)), 67);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), 4)), 70);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), 5)), 72);   // the octave

    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), -1)), 58);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::minor_pentatonic), -5)), 48);

    CHECK_EQ(int(scale_step(60, 60, int(Scale::major), 7)), 72);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::chromatic), 7)), 67);
    CHECK_EQ(int(scale_step(60, 60, int(Scale::major), 0)), 60);
}

TEST(scale_stepping_folds_first_if_the_note_is_off_the_scale) {
    // 61 is not in C major; it folds to 60, and one degree up from there is 62. Stepping must
    // not transpose an off-scale note and leave it off the scale.
    CHECK_EQ(int(scale_step(61, 60, int(Scale::major), 1)), 62);
    CHECK_EQ(int(scale_step(61, 60, int(Scale::major), 0)), 60);
}

TEST(scale_stepping_stays_in_the_scale_and_in_range_everywhere) {
    int out_of_scale = 0, out_of_range = 0, wrong_direction = 0;
    for (int s = 0; s < kScaleCount; s++) {
        for (int note = 0; note <= 127; note += 3) {
            for (int degrees = -14; degrees <= 14; degrees++) {
                const uint8_t got = scale_step(uint8_t(note), 60, s, degrees);
                if (!scale_contains(got, 60, s)) out_of_scale++;
                if (got > 127) out_of_range++;
                // It never moves the wrong way. Equality is allowed: the range saturates.
                const uint8_t base = scale_fold(uint8_t(note), 60, s);
                if (degrees > 0 && got < base) wrong_direction++;
                if (degrees < 0 && got > base) wrong_direction++;
            }
        }
    }
    CHECK_EQ(out_of_scale, 0);
    CHECK_EQ(out_of_range, 0);
    CHECK_EQ(wrong_direction, 0);
}

TEST(scale_stepping_saturates_rather_than_wrapping) {
    // Walked off the bottom, a line sits on the lowest note it can play. Wrapping would be a
    // ten-octave jump in the middle of a bar.
    const uint8_t low = scale_step(0, 60, int(Scale::minor), -40);
    CHECK(low <= 11);
    CHECK(scale_contains(low, 60, int(Scale::minor)));
    CHECK_EQ(int(scale_step(low, 60, int(Scale::minor), -1)), int(low));

    const uint8_t high = scale_step(127, 60, int(Scale::minor), 40);
    CHECK(high >= 116);
    CHECK(scale_contains(high, 60, int(Scale::minor)));
    CHECK_EQ(int(scale_step(high, 60, int(Scale::minor), 1)), int(high));
}

TEST(scale_stepping_up_then_down_returns_to_where_it_started) {
    // Only away from the ends of the range, where saturation is the documented behaviour.
    for (int s = 0; s < kScaleCount; s++) {
        for (int note = 36; note <= 84; note++) {
            const uint8_t base = scale_fold(uint8_t(note), 60, s);
            for (int degrees = 1; degrees <= 5; degrees++) {
                const uint8_t up = scale_step(base, 60, s, degrees);
                CHECK_EQ(int(scale_step(up, 60, s, -degrees)), int(base));
            }
        }
    }
}
