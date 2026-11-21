// Mutation: Euclidean density, rotation, and the four bounded randomizers.
//
// Two things here are worth more than the rest. The first is that `euclidean_hit` is checked by
// its defining PROPERTY -- the gaps between hits take at most two values and they differ by one
// -- for every (hits, steps) pair a track can have, rather than against a handful of rhythms
// copied from a reference. The closed form in core/mutate.hpp is not Bjorklund's algorithm and
// produces the same rhythms rotated, so a test against a reference's exact positions would fail
// for a correct implementation; the evenness property is what both share.
//
// The second is determinism. docs/SPEC.md makes "seeded, so a mutation can be recalled exactly"
// a design principle, and a mutation that is almost deterministic is worse than one that is
// obviously random: it would come back subtly different after a reload.
//
// Patterns are file-scope statics -- one is about 8 KiB and CLAUDE.md caps a stack frame at
// 16 KiB.
#include "check.hpp"

#include "mutate.hpp"
#include "pattern.hpp"
#include "scale.hpp"

#include <initializer_list>

namespace {

using namespace strata;

Pattern g_pattern;
Pattern g_before;

// A track with trigs on the steps named, each with a note and velocity of its own so a rotation
// can be followed.
void make_track(Pattern &pattern, int track, int length, std::initializer_list<int> trigs) {
    pattern_init(pattern);
    pattern.tracks[track].length = uint8_t(length);
    for (int step : trigs) {
        Step &s = pattern.tracks[track].steps[step];
        s.flags = uint8_t(s.flags | kStepTrig);
        s.note = uint8_t(36 + step);
        s.velocity = uint8_t(40 + step);
        s.probability = uint8_t(50 + step % 20);
        s.micro = int8_t(step % 7 - 3);
    }
}

void snapshot(const Pattern &from) { g_before = from; }

// How many fields differ across the whole pattern. 0 means nothing moved.
int differences(const Pattern &a, const Pattern &b) {
    int n = 0;
    for (int t = 0; t < kTracks; t++) {
        if (a.tracks[t].length != b.tracks[t].length) n++;
        if (a.tracks[t].speed != b.tracks[t].speed) n++;
        if (a.tracks[t].swing != b.tracks[t].swing) n++;
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &x = a.tracks[t].steps[i];
            const Step &y = b.tracks[t].steps[i];
            if (x.flags != y.flags) n++;
            if (x.note != y.note) n++;
            if (x.velocity != y.velocity) n++;
            if (x.length != y.length) n++;
            if (x.micro != y.micro) n++;
            if (x.probability != y.probability) n++;
            if (x.condition != y.condition) n++;
            if (x.ratchet != y.ratchet) n++;
            if (x.chord != y.chord) n++;
            if (x.lock_first != y.lock_first) n++;
            if (x.lock_count != y.lock_count) n++;
        }
    }
    return n;
}

int trig_count(const Pattern &pattern, int track, int first, int count) {
    int n = 0;
    for (int i = first; i < first + count; i++) {
        if ((pattern.tracks[track].steps[i].flags & kStepTrig) != 0) n++;
    }
    return n;
}

// The gaps between consecutive hits of a Euclidean rhythm, cyclically. Returns how many there
// are, which is the hit count.
int euclidean_gaps(int hits, int steps, int *gaps, int max) {
    int positions[kStepsPerTrack] = {};
    int found = 0;
    for (int i = 0; i < steps && found < kStepsPerTrack; i++) {
        if (euclidean_hit(i, hits, steps)) positions[found++] = i;
    }
    for (int i = 0; i < found && i < max; i++) {
        const int next = i + 1 < found ? positions[i + 1] : positions[0] + steps;
        gaps[i] = next - positions[i];
    }
    return found;
}

}  // namespace

// --- Euclidean ------------------------------------------------------------

TEST(mutate_euclidean_places_exactly_the_hits_asked_for) {
    for (int steps = 1; steps <= kStepsPerTrack; steps++) {
        for (int hits = 0; hits <= steps; hits++) {
            int found = 0;
            for (int i = 0; i < steps; i++) {
                if (euclidean_hit(i, hits, steps)) found++;
            }
            if (found != hits) CHECK_EQ(found, hits);
            CHECK_EQ(euclidean_count(hits, steps), hits);
        }
    }
}

TEST(mutate_euclidean_is_maximally_even) {
    // The defining property: the gaps between consecutive hits take at most two distinct values
    // and those differ by exactly one. That is what makes a rhythm Euclidean, and it is what
    // this closed form shares with Bjorklund's algorithm -- which produces the same rhythms in
    // a different rotation, so comparing exact positions against a reference would fail for a
    // correct implementation.
    int uneven = 0, too_many_values = 0;
    for (int steps = 2; steps <= kStepsPerTrack; steps++) {
        for (int hits = 1; hits <= steps; hits++) {
            int gaps[kStepsPerTrack] = {};
            const int found = euclidean_gaps(hits, steps, gaps, kStepsPerTrack);
            if (found < 1) continue;
            int low = gaps[0], high = gaps[0];
            for (int i = 1; i < found; i++) {
                if (gaps[i] < low) low = gaps[i];
                if (gaps[i] > high) high = gaps[i];
            }
            if (high - low > 1) uneven++;
            int distinct = 0;
            for (int value = low; value <= high; value++) {
                for (int i = 0; i < found; i++) {
                    if (gaps[i] == value) { distinct++; break; }
                }
            }
            if (distinct > 2) too_many_values++;
        }
    }
    CHECK_EQ(uneven, 0);
    CHECK_EQ(too_many_values, 0);
}

TEST(mutate_euclidean_starts_on_the_first_step) {
    // core/mutate.hpp promises this: the rhythm is rotated so step 0 is always a hit, because
    // rotation is a separate mutation and a density change that silently moved the downbeat
    // would be two edits in one.
    for (int steps = 1; steps <= kStepsPerTrack; steps++) {
        for (int hits = 1; hits <= steps; hits++) {
            CHECK(euclidean_hit(0, hits, steps));
        }
    }
}

TEST(mutate_euclidean_matches_the_well_known_rhythms) {
    // Up to rotation, which the property test above covers. These are the sets this form
    // produces, pinned so a change to the formula is visible rather than merely still even.
    const bool e38[8] = {true, false, false, true, false, false, true, false};
    for (int i = 0; i < 8; i++) CHECK_EQ(euclidean_hit(i, 3, 8), e38[i]);

    const bool e48[8] = {true, false, true, false, true, false, true, false};
    for (int i = 0; i < 8; i++) CHECK_EQ(euclidean_hit(i, 4, 8), e48[i]);

    const bool e4_16[16] = {true,  false, false, false, true,  false, false, false,
                            true,  false, false, false, true,  false, false, false};
    for (int i = 0; i < 16; i++) CHECK_EQ(euclidean_hit(i, 4, 16), e4_16[i]);
}

TEST(mutate_euclidean_answers_degenerate_input_rather_than_refusing) {
    CHECK(!euclidean_hit(0, 0, 8));            // no hits
    CHECK(!euclidean_hit(0, 3, 0));            // no steps
    CHECK(!euclidean_hit(-1, 3, 8));           // before the start
    CHECK(!euclidean_hit(8, 3, 8));            // past the end
    CHECK(!euclidean_hit(0, 3, -4));
    CHECK(euclidean_hit(3, 8, 8));             // every step
    CHECK(euclidean_hit(3, 99, 8));            // more hits than steps
    CHECK_EQ(euclidean_count(0, 8), 0);
    CHECK_EQ(euclidean_count(99, 8), 8);
    CHECK_EQ(euclidean_count(3, 0), 0);
    CHECK_EQ(euclidean_count(-1, 8), 0);
}

// --- the draw -------------------------------------------------------------

TEST(mutate_the_draw_is_deterministic) {
    // docs/SPEC.md's principle. The same identity must give the same number on every machine
    // and after a reload, or a mutation cannot be recalled exactly.
    for (int step = 0; step < kStepsPerTrack; step++) {
        const int a = mutate_draw(0xDEADBEEFu, 3, step, Mutation::velocity, 7, 48);
        const int b = mutate_draw(0xDEADBEEFu, 3, step, Mutation::velocity, 7, 48);
        CHECK_EQ(a, b);
    }
}

TEST(mutate_the_draw_stays_inside_its_span) {
    int outside = 0;
    for (int span : {1, 3, 11, 40, 48}) {
        for (int track = 0; track < kTracks; track++) {
            for (int step = 0; step < kStepsPerTrack; step++) {
                for (uint32_t variant = 0; variant < 4; variant++) {
                    const int draw =
                        mutate_draw(0x12345678u, track, step, Mutation::micro, variant, span);
                    if (draw < -span || draw > span) outside++;
                }
            }
        }
    }
    CHECK_EQ(outside, 0);
}

TEST(mutate_a_span_of_zero_draws_zero) {
    // What `amount` 0 reduces to: the knob is off rather than broken.
    for (int step = 0; step < 16; step++) {
        CHECK_EQ(mutate_draw(1u, 0, step, Mutation::velocity, 0, 0), 0);
        CHECK_EQ(mutate_draw(1u, 0, step, Mutation::velocity, 0, -5), 0);
    }
}

TEST(mutate_the_draw_uses_its_whole_span_in_both_directions) {
    // A draw that never went negative, or never reached its edges, would make a mutation a
    // one-way drift. Checked over the steps of one track, which is the population a real
    // mutation draws from.
    int negative = 0, positive = 0, at_edge = 0;
    for (int step = 0; step < kStepsPerTrack; step++) {
        const int draw = mutate_draw(0xC0FFEEu, 2, step, Mutation::velocity, 1, 10);
        if (draw < 0) negative++;
        if (draw > 0) positive++;
        if (draw == -10 || draw == 10) at_edge++;
    }
    CHECK(negative > 8);
    CHECK(positive > 8);
    CHECK(at_edge > 0);
}

TEST(mutate_the_draw_separates_everything_that_identifies_it) {
    // The seed, the track, the step, the kind and the variant must each change the answer. The
    // kind matters most: without it a velocity mutation and a micro mutation of the same step on
    // the same press would move by the same amount, which looks like a correlation the user
    // cannot explain.
    const int base = mutate_draw(100u, 1, 5, Mutation::velocity, 2, 20);
    int differs = 0;
    if (mutate_draw(101u, 1, 5, Mutation::velocity, 2, 20) != base) differs++;
    if (mutate_draw(100u, 2, 5, Mutation::velocity, 2, 20) != base) differs++;
    if (mutate_draw(100u, 1, 6, Mutation::velocity, 2, 20) != base) differs++;
    if (mutate_draw(100u, 1, 5, Mutation::micro, 2, 20) != base) differs++;
    if (mutate_draw(100u, 1, 5, Mutation::velocity, 3, 20) != base) differs++;
    CHECK_EQ(differs, 5);

    // And across a whole track, two kinds must not agree step for step.
    int same = 0;
    for (int step = 0; step < kStepsPerTrack; step++) {
        if (mutate_draw(7u, 0, step, Mutation::velocity, 0, 30) ==
            mutate_draw(7u, 0, step, Mutation::probability, 0, 30)) {
            same++;
        }
    }
    CHECK(same < kStepsPerTrack / 2);
}

// --- what gets refused ----------------------------------------------------

TEST(mutate_refuses_what_it_cannot_do_and_changes_nothing) {
    make_track(g_pattern, 0, 16, {0, 4, 8, 12});
    snapshot(g_pattern);

    // Not a track.
    CHECK(!mutate_apply(g_pattern, -1, 0, 16, Mutation::velocity, 0, 100));
    CHECK(!mutate_apply(g_pattern, kTracks, 0, 16, Mutation::velocity, 0, 100));
    // An empty or backwards range.
    CHECK(!mutate_apply(g_pattern, 0, 0, 0, Mutation::velocity, 0, 100));
    CHECK(!mutate_apply(g_pattern, 0, 8, -4, Mutation::velocity, 0, 100));
    // Entirely past the track's length.
    CHECK(!mutate_apply(g_pattern, 0, 16, 16, Mutation::velocity, 0, 100));
    // Amount 0.
    CHECK(!mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 0, 0));
    CHECK(!mutate_apply(g_pattern, 0, 0, 16, Mutation::notes, 0, 0));
    // A kind that does not exist.
    CHECK(!mutate_apply(g_pattern, 0, 0, 16, Mutation::count, 0, 100));

    CHECK_EQ(differences(g_pattern, g_before), 0);
}

TEST(mutate_leaves_other_tracks_alone) {
    make_track(g_pattern, 3, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    for (int t = 0; t < kTracks; t++) {
        if (t == 3) continue;
        g_pattern.tracks[t].steps[1].flags = kStepTrig;
        g_pattern.tracks[t].steps[1].velocity = 99;
    }
    snapshot(g_pattern);

    CHECK(mutate_apply(g_pattern, 3, 0, 16, Mutation::velocity, 1, 100));

    for (int t = 0; t < kTracks; t++) {
        if (t == 3) continue;
        CHECK_EQ(int(g_pattern.tracks[t].steps[1].velocity), 99);
    }
    CHECK_EQ(int(g_pattern.tracks[3].length), int(g_before.tracks[3].length));
}

TEST(mutate_never_reaches_past_the_tracks_own_length) {
    // A 16-step track's steps 16..63 are not part of the pattern. A mutation asked for the whole
    // 64 must leave them exactly as they were, or rotating would bring in steps the sequencer
    // never plays and the user has never seen.
    make_track(g_pattern, 0, 16, {0, 3, 7, 11});
    for (int i = 16; i < kStepsPerTrack; i++) {
        g_pattern.tracks[0].steps[i].flags = kStepTrig;
        g_pattern.tracks[0].steps[i].note = uint8_t(100 + i % 20);
        g_pattern.tracks[0].steps[i].velocity = 77;
    }
    snapshot(g_pattern);

    CHECK(mutate_apply(g_pattern, 0, 0, kStepsPerTrack, Mutation::rotate_left, 0, 100));
    for (int i = 16; i < kStepsPerTrack; i++) {
        CHECK_EQ(int(g_pattern.tracks[0].steps[i].note),
                 int(g_before.tracks[0].steps[i].note));
        CHECK_EQ(int(g_pattern.tracks[0].steps[i].velocity), 77);
    }
}

// --- rotation -------------------------------------------------------------

TEST(mutate_rotation_moves_the_steps_one_place) {
    make_track(g_pattern, 0, 8, {0, 1, 2, 3, 4, 5, 6, 7});
    snapshot(g_pattern);

    CHECK(mutate_apply(g_pattern, 0, 0, 8, Mutation::rotate_left, 0, 100));
    for (int i = 0; i < 7; i++) {
        CHECK_EQ(int(g_pattern.tracks[0].steps[i].note),
                 int(g_before.tracks[0].steps[i + 1].note));
    }
    CHECK_EQ(int(g_pattern.tracks[0].steps[7].note), int(g_before.tracks[0].steps[0].note));

    CHECK(mutate_apply(g_pattern, 0, 0, 8, Mutation::rotate_right, 0, 100));
    CHECK_EQ(differences(g_pattern, g_before), 0);     // and back to where it started
}

TEST(mutate_rotation_stays_inside_the_range_it_was_given) {
    make_track(g_pattern, 0, 16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    snapshot(g_pattern);

    // Steps 4..7 only. Everything outside must be untouched, and the four inside must cycle
    // among themselves.
    CHECK(mutate_apply(g_pattern, 0, 4, 4, Mutation::rotate_left, 0, 100));
    for (int i = 0; i < 16; i++) {
        const int want = (i >= 4 && i < 8) ? (i == 7 ? 4 : i + 1) : i;
        CHECK_EQ(int(g_pattern.tracks[0].steps[i].note),
                 int(g_before.tracks[0].steps[want].note));
    }
}

TEST(mutate_rotation_carries_each_steps_locks_with_it) {
    // A Step holds its locks by index into the pattern's pool, so moving whole Step values moves
    // the reference too and the pool is untouched. That is what makes rotation unable to fail
    // for want of pool space -- and it is worth checking, because a rotation that renumbered
    // would silently give a step someone else's locks.
    make_track(g_pattern, 0, 8, {0, 2, 4, 6});
    CHECK(pattern_set_lock(g_pattern, 0, 2, 5, 111));
    CHECK(pattern_set_lock(g_pattern, 0, 2, 9, 222));
    CHECK(pattern_set_lock(g_pattern, 0, 6, 5, 33));
    const uint16_t used = g_pattern.locks_used;

    CHECK(mutate_apply(g_pattern, 0, 0, 8, Mutation::rotate_left, 0, 100));

    // The locks that were on step 2 are now on step 1, with their values intact.
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_pattern, 0, 1, 5, value));
    CHECK_EQ(int(value), 111);
    CHECK(pattern_get_lock(g_pattern, 0, 1, 9, value));
    CHECK_EQ(int(value), 222);
    CHECK(pattern_get_lock(g_pattern, 0, 5, 5, value));
    CHECK_EQ(int(value), 33);
    CHECK(!pattern_get_lock(g_pattern, 0, 2, 5, value));

    // And the pool itself did not grow.
    CHECK_EQ(int(g_pattern.locks_used), int(used));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 3);
}

TEST(mutate_rotating_identical_steps_is_refused) {
    // Not a mutation, and spending one of sixteen undo levels on it would be a press that
    // looks like it did nothing because it did nothing.
    pattern_init(g_pattern);
    g_pattern.tracks[0].length = 8;
    snapshot(g_pattern);
    CHECK(!mutate_apply(g_pattern, 0, 0, 8, Mutation::rotate_left, 0, 100));
    CHECK(!mutate_apply(g_pattern, 0, 0, 8, Mutation::rotate_right, 0, 100));
    CHECK_EQ(differences(g_pattern, g_before), 0);

    // A single step cannot rotate either.
    make_track(g_pattern, 0, 8, {0, 4});
    snapshot(g_pattern);
    CHECK(!mutate_apply(g_pattern, 0, 3, 1, Mutation::rotate_left, 0, 100));
    CHECK_EQ(differences(g_pattern, g_before), 0);
}

// --- density --------------------------------------------------------------

TEST(mutate_density_changes_the_hit_count_by_one) {
    make_track(g_pattern, 0, 16, {0, 4, 8, 12});
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 4);

    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::density_up, 0, 100));
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 5);
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::density_up, 0, 100));
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 6);
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::density_down, 0, 100));
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 5);
}

TEST(mutate_density_leaves_a_euclidean_rhythm_behind) {
    // core/mutate.hpp's decision: the hits are REDISTRIBUTED evenly rather than nudged, so the
    // result is exactly the Euclidean rhythm for the new count. Checked against euclidean_hit
    // itself, which the property tests above pin independently.
    for (int start = 0; start <= 4; start += 4) {
        make_track(g_pattern, 0, 16, {1, 2, 3, 9});
        const int before = trig_count(g_pattern, 0, start, 8);
        CHECK(mutate_apply(g_pattern, 0, start, 8, Mutation::density_up, 0, 100));
        const int after = trig_count(g_pattern, 0, start, 8);
        CHECK_EQ(after, before + 1);
        for (int i = 0; i < 8; i++) {
            const bool have =
                (g_pattern.tracks[0].steps[start + i].flags & kStepTrig) != 0;
            CHECK_EQ(have, euclidean_hit(i, after, 8));
        }
    }
}

TEST(mutate_density_refuses_when_there_is_nowhere_to_go) {
    // Already empty, or already full.
    pattern_init(g_pattern);
    g_pattern.tracks[0].length = 8;
    snapshot(g_pattern);
    CHECK(!mutate_apply(g_pattern, 0, 0, 8, Mutation::density_down, 0, 100));
    CHECK_EQ(differences(g_pattern, g_before), 0);

    make_track(g_pattern, 0, 8, {0, 1, 2, 3, 4, 5, 6, 7});
    snapshot(g_pattern);
    CHECK(!mutate_apply(g_pattern, 0, 0, 8, Mutation::density_up, 0, 100));
    CHECK_EQ(differences(g_pattern, g_before), 0);
}

TEST(mutate_density_up_from_empty_puts_one_hit_on_the_first_step) {
    pattern_init(g_pattern);
    g_pattern.tracks[0].length = 16;
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::density_up, 0, 100));
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 1);
    CHECK((g_pattern.tracks[0].steps[0].flags & kStepTrig) != 0);
}

TEST(mutate_a_gained_hit_sounds_like_the_part_it_joins) {
    // A new hit copies note, velocity and length from the first existing hit in the range. On a
    // melodic track the alternative is a middle C nobody asked for.
    make_track(g_pattern, 0, 16, {0});
    g_pattern.tracks[0].steps[0].note = 41;
    g_pattern.tracks[0].steps[0].velocity = 93;
    g_pattern.tracks[0].steps[0].length = 6;

    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::density_up, 0, 100));
    CHECK_EQ(trig_count(g_pattern, 0, 0, 16), 2);
    for (int i = 0; i < 16; i++) {
        const Step &s = g_pattern.tracks[0].steps[i];
        if ((s.flags & kStepTrig) == 0) continue;
        CHECK_EQ(int(s.note), 41);
        CHECK_EQ(int(s.velocity), 93);
        CHECK_EQ(int(s.length), 6);
    }
}

TEST(mutate_a_lost_hit_keeps_its_sound_for_the_undo) {
    // Only the trig bit is cleared, so an undo -- or the next density up -- gets the step's own
    // note and velocity back rather than a default.
    make_track(g_pattern, 0, 8, {0, 1, 2, 3, 4, 5, 6});
    const uint8_t note_was = g_pattern.tracks[0].steps[1].note;
    const uint8_t velocity_was = g_pattern.tracks[0].steps[1].velocity;

    CHECK(mutate_apply(g_pattern, 0, 0, 8, Mutation::density_down, 0, 100));
    // Whichever steps lost their trig kept everything else.
    int cleared = 0;
    for (int i = 0; i < 8; i++) {
        if ((g_pattern.tracks[0].steps[i].flags & kStepTrig) == 0) cleared++;
    }
    CHECK_EQ(cleared, 2);
    CHECK_EQ(int(g_pattern.tracks[0].steps[1].note), int(note_was));
    CHECK_EQ(int(g_pattern.tracks[0].steps[1].velocity), int(velocity_was));
}

// --- the bounded randomizers ---------------------------------------------

TEST(mutate_velocity_stays_in_range_and_only_touches_live_steps) {
    make_track(g_pattern, 0, 16, {0, 4, 8, 12});
    g_pattern.tracks[0].steps[2].flags = kStepTrigless;   // locks without a note: still live
    g_pattern.tracks[0].steps[2].velocity = 70;
    snapshot(g_pattern);

    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 5, 100));

    for (int i = 0; i < 16; i++) {
        const Step &now = g_pattern.tracks[0].steps[i];
        const Step &was = g_before.tracks[0].steps[i];
        CHECK(now.velocity >= 1);
        CHECK(now.velocity <= 127);
        const bool live = (was.flags & (kStepTrig | kStepTrigless)) != 0;
        if (!live) CHECK_EQ(int(now.velocity), int(was.velocity));
        // Nothing but the velocity moved.
        CHECK_EQ(int(now.note), int(was.note));
        CHECK_EQ(int(now.micro), int(was.micro));
        CHECK_EQ(int(now.probability), int(was.probability));
        CHECK_EQ(int(now.flags), int(was.flags));
    }
}

TEST(mutate_micro_and_probability_stay_in_their_ranges) {
    make_track(g_pattern, 0, 16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    // Start at the extremes, where an unclamped mutation would leave the range.
    for (int i = 0; i < 16; i++) {
        g_pattern.tracks[0].steps[i].micro = int8_t(i % 2 == 0 ? kMicroRange : -kMicroRange);
        g_pattern.tracks[0].steps[i].probability = uint8_t(i % 2 == 0 ? 100 : 0);
    }

    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::micro, 1, 100));
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::probability, 1, 100));

    for (int i = 0; i < 16; i++) {
        const Step &s = g_pattern.tracks[0].steps[i];
        CHECK(s.micro >= -kMicroRange);
        CHECK(s.micro <= kMicroRange);
        CHECK(s.probability <= 100);
    }
}

TEST(mutate_velocity_never_reaches_zero) {
    // A velocity of 0 is a note that does not sound, which is a step the user would have turned
    // off instead. Driven from 1, where the clamp is the only thing stopping it.
    make_track(g_pattern, 0, 16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    for (int i = 0; i < 16; i++) g_pattern.tracks[0].steps[i].velocity = 1;
    for (uint32_t variant = 0; variant < 8; variant++) {
        mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, variant, 100);
        for (int i = 0; i < 16; i++) CHECK(g_pattern.tracks[0].steps[i].velocity >= 1);
    }
}

TEST(mutate_notes_stay_in_the_patterns_key) {
    // By DEGREES, so a line stays in the scale rather than being transposed out of it.
    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.root = 2;                                  // D
    g_pattern.scale = uint8_t(Scale::minor_pentatonic);
    for (int i = 0; i < 16; i++) {
        g_pattern.tracks[0].steps[i].note =
            scale_fold(uint8_t(48 + i), g_pattern.root, int(g_pattern.scale));
    }

    for (uint32_t variant = 0; variant < 6; variant++) {
        CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::notes, variant, 100));
        for (int i = 0; i < 16; i++) {
            const Step &s = g_pattern.tracks[0].steps[i];
            if ((s.flags & (kStepTrig | kStepTrigless)) == 0) continue;
            CHECK(scale_contains(s.note, g_pattern.root, int(g_pattern.scale)));
            CHECK(s.note <= 127);
        }
    }
}

TEST(mutate_notes_move_by_no_more_than_the_span_of_degrees) {
    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.root = 0;
    g_pattern.scale = uint8_t(Scale::minor);
    for (int i = 0; i < 16; i++) {
        g_pattern.tracks[0].steps[i].note = scale_fold(uint8_t(60 + i), 0, int(Scale::minor));
    }
    snapshot(g_pattern);

    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::notes, 3, 100));
    for (int i = 0; i < 16; i++) {
        const Step &now = g_pattern.tracks[0].steps[i];
        const Step &was = g_before.tracks[0].steps[i];
        if ((was.flags & (kStepTrig | kStepTrigless)) == 0) continue;
        // kMutateNoteDegrees degrees of the minor scale is at most 6 semitones.
        const int moved = int(now.note) - int(was.note);
        CHECK(moved <= 6);
        CHECK(moved >= -6);
    }
}

TEST(mutate_a_smaller_amount_moves_less) {
    // The knob has to be monotonic in spirit: at amount 100 the spread must be wider than at a
    // low amount. Measured as the total movement across the track, which is stable where any one
    // step's draw is not.
    int spread[3] = {};
    const int amounts[3] = {10, 50, 100};
    for (int a = 0; a < 3; a++) {
        make_track(g_pattern, 0, 16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
        for (int i = 0; i < 16; i++) g_pattern.tracks[0].steps[i].velocity = 64;
        mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 2, amounts[a]);
        for (int i = 0; i < 16; i++) {
            const int moved = int(g_pattern.tracks[0].steps[i].velocity) - 64;
            spread[a] += moved < 0 ? -moved : moved;
        }
    }
    CHECK(spread[0] < spread[1]);
    CHECK(spread[1] < spread[2]);
}

TEST(mutate_a_low_amount_still_does_something) {
    // The span is rounded to nearest, not truncated. With truncation, 3 degrees times 20 over
    // 100 is 0, and the note mutation would do nothing at all below amount 34 -- a knob that is
    // dead for its first third is a knob that looks broken. Checked on `notes`, whose span is
    // the smallest and therefore the only one where it shows.
    int did_something = 0;
    for (uint32_t variant = 0; variant < 8; variant++) {
        make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
        g_pattern.root = 0;
        g_pattern.scale = uint8_t(Scale::minor);
        for (int i = 0; i < 16; i++) {
            g_pattern.tracks[0].steps[i].note = scale_fold(uint8_t(60 + i), 0, int(Scale::minor));
        }
        if (mutate_apply(g_pattern, 0, 0, 16, Mutation::notes, variant, 20)) did_something++;
    }
    CHECK(did_something > 0);

    // And amount 0 really is off, for every kind that takes one.
    for (Mutation kind : {Mutation::velocity, Mutation::micro, Mutation::probability,
                          Mutation::notes}) {
        make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
        snapshot(g_pattern);
        CHECK(!mutate_apply(g_pattern, 0, 0, 16, kind, 1, 0));
        CHECK_EQ(differences(g_pattern, g_before), 0);
    }
}

TEST(mutate_a_trigless_step_is_mutated_too) {
    // A trigless step applies its locks without a note, and docs/SPEC.md makes that the point of
    // it: its locks are what the step is for and its timing still decides when they land. So a
    // velocity or micro mutation has to reach it. Measured over several variants because any one
    // step's draw may legitimately be zero.
    int moved = 0;
    for (uint32_t variant = 0; variant < 8; variant++) {
        pattern_init(g_pattern);
        g_pattern.tracks[0].length = 16;
        for (int i = 0; i < 16; i++) {
            Step &s = g_pattern.tracks[0].steps[i];
            s.flags = kStepTrigless;          // no trigs on this track at all
            s.micro = 0;
        }
        if (!mutate_apply(g_pattern, 0, 0, 16, Mutation::micro, variant, 100)) continue;
        for (int i = 0; i < 16; i++) {
            if (g_pattern.tracks[0].steps[i].micro != 0) moved++;
        }
    }
    CHECK(moved > 0);
}

TEST(mutate_the_same_variant_reproduces_the_same_mutation) {
    // "Seeded and undoable" means a mutation can be recalled exactly: the same pattern, the same
    // variant and the same amount must give the same result, every time.
    for (Mutation kind : {Mutation::velocity, Mutation::micro, Mutation::probability,
                          Mutation::notes, Mutation::density_up, Mutation::rotate_left}) {
        make_track(g_pattern, 0, 16, {0, 3, 6, 9, 12, 15});
        g_pattern.seed = 0xABCD1234u;
        CHECK(mutate_apply(g_pattern, 0, 0, 16, kind, 9, 100));
        snapshot(g_pattern);

        make_track(g_pattern, 0, 16, {0, 3, 6, 9, 12, 15});
        g_pattern.seed = 0xABCD1234u;
        CHECK(mutate_apply(g_pattern, 0, 0, 16, kind, 9, 100));
        CHECK_EQ(differences(g_pattern, g_before), 0);
    }
}

TEST(mutate_a_different_variant_gives_a_different_mutation) {
    // What makes pressing MUTATE twice do something new.
    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.seed = 55u;
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 1, 100));
    snapshot(g_pattern);

    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.seed = 55u;
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 2, 100));
    CHECK(differences(g_pattern, g_before) > 0);
}

TEST(mutate_a_different_pattern_seed_gives_a_different_mutation) {
    // The seed is the pattern's, so two patterns mutate differently and a reload reproduces each.
    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.seed = 1u;
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 0, 100));
    snapshot(g_pattern);

    make_track(g_pattern, 0, 16, {0, 2, 4, 6, 8, 10, 12, 14});
    g_pattern.seed = 2u;
    CHECK(mutate_apply(g_pattern, 0, 0, 16, Mutation::velocity, 0, 100));
    CHECK(differences(g_pattern, g_before) > 0);
}

TEST(mutate_every_kind_leaves_a_pattern_the_sequencer_can_read) {
    // step_sanitize is what the loader uses against a damaged file. Running it over a mutated
    // pattern must change nothing: a mutation is allowed to make a part worse, never to make one
    // the scheduler would read out of range.
    for (int kind = 0; kind < int(Mutation::count); kind++) {
        make_track(g_pattern, 0, 32, {0, 1, 5, 9, 13, 17, 21, 25, 31});
        g_pattern.root = 7;
        g_pattern.scale = uint8_t(Scale::phrygian);
        for (uint32_t variant = 0; variant < 4; variant++) {
            mutate_apply(g_pattern, 0, 0, 32, Mutation(kind), variant, 100);
        }
        snapshot(g_pattern);
        for (int t = 0; t < kTracks; t++) {
            for (int i = 0; i < kStepsPerTrack; i++) step_sanitize(g_pattern.tracks[t].steps[i]);
        }
        CHECK_EQ(differences(g_pattern, g_before), 0);
    }
}
