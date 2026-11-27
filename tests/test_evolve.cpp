// EVOLVE: periods, ORBIT against DRIFT, KEEP and HOME, edits made while it runs, and chords.
//
// Patterns are file-scope statics: one is about 8 KiB and CLAUDE.md caps a stack frame at 16 KiB.
#include "check.hpp"

#include "evolve.hpp"
#include "pattern.hpp"
#include "stepnotes.hpp"

namespace {

using namespace strata;

Pattern g_p;
Pattern g_home;
Evolve g_e;

void setup(int track) {
    pattern_init(g_p);
    g_p.seed = 0xBEEFu;
    g_p.tracks[track].length = 16;
    for (int i = 0; i < 16; i += 2) {
        Step &s = g_p.tracks[track].steps[i];
        s.flags = kStepTrig;
        s.note = uint8_t(48 + i);
        s.velocity = 100;
    }
    g_e = Evolve{};
}

bool steps_equal(const Pattern &a, const Pattern &b, int track) {
    for (int i = 0; i < kStepsPerTrack; i++) {
        const Step &x = a.tracks[track].steps[i];
        const Step &y = b.tracks[track].steps[i];
        if (x.flags != y.flags || x.note != y.note || x.velocity != y.velocity ||
            x.micro != y.micro || x.probability != y.probability) {
            return false;
        }
    }
    return true;
}

int velocity_distance(const Pattern &a, const Pattern &b, int track) {
    int d = 0;
    for (int i = 0; i < 16; i++) {
        const int x = a.tracks[track].steps[i].velocity;
        const int y = b.tracks[track].steps[i].velocity;
        d += x > y ? x - y : y - x;
    }
    return d;
}

}  // namespace

TEST(evolve_names) {
    CHECK(evolve_period_name(0)[0] == 'O');
    CHECK(evolve_period_name(3)[0] == '4');
    CHECK(evolve_mode_name(EvolveMode::orbit)[0] == 'O');
    CHECK(evolve_mode_name(EvolveMode::drift)[0] == 'D');
}

TEST(evolve_off_does_nothing) {
    setup(1);
    g_home = g_p;
    for (int pass = 0; pass < 10; pass++) {
        CHECK(!evolve_pass(g_e, g_p, 1, EvolveMode::orbit, Mutation::velocity, 100, pass));
    }
    CHECK(steps_equal(g_p, g_home, 1));
    CHECK(!evolve_home(g_e, g_p, 1));
}

TEST(evolve_turns_once_a_period) {
    setup(1);
    evolve_set(g_e, g_p, 1, 3);                             // every 4 passes
    int turns = 0;
    for (int pass = 1; pass <= 16; pass++) {
        g_home = g_p;
        const bool turned =
            evolve_pass(g_e, g_p, 1, EvolveMode::drift, Mutation::velocity, 100, uint32_t(pass));
        turns += turned;
        CHECK_EQ(turned, pass % 4 == 0);
        CHECK_EQ(!steps_equal(g_p, g_home, 1), pass % 4 == 0);
    }
    CHECK_EQ(turns, 4);
}

TEST(evolve_set_again_changes_the_period_and_keeps_home) {
    setup(1);
    g_home = g_p;
    evolve_set(g_e, g_p, 1, 1);
    evolve_pass(g_e, g_p, 1, EvolveMode::drift, Mutation::velocity, 100, 1);
    evolve_set(g_e, g_p, 1, 2);                             // HOME is still the first one
    CHECK_EQ(int(g_e.period), 2);
    CHECK(evolve_home(g_e, g_p, 1));
    CHECK(steps_equal(g_p, g_home, 1));
    CHECK(!g_e.on());
}

TEST(evolve_orbit_stays_near_home_and_drift_wanders) {
    // ORBIT: every turn is one mutation away from HOME, so the distance never grows past what
    // one mutation can do (8 trigs, 48 each at amount 100).
    setup(2);
    g_home = g_p;
    evolve_set(g_e, g_p, 2, 1);
    for (int pass = 1; pass <= 40; pass++) {
        evolve_pass(g_e, g_p, 2, EvolveMode::orbit, Mutation::velocity, 100, uint32_t(pass));
        CHECK(velocity_distance(g_p, g_home, 2) <= 8 * 48);
    }
    // ORBIT on a rotation stays a single rotation of HOME: step 0 holds HOME's step 0 or 1.
    setup(2);
    g_home = g_p;
    evolve_set(g_e, g_p, 2, 1);
    for (int pass = 1; pass <= 5; pass++) {
        evolve_pass(g_e, g_p, 2, EvolveMode::orbit, Mutation::rotate_left, 100, uint32_t(pass));
    }
    CHECK_EQ(int(g_p.tracks[2].steps[15].note), int(g_home.tracks[2].steps[0].note));
    CHECK_EQ(int(g_p.tracks[2].steps[0].flags), int(g_home.tracks[2].steps[1].flags));

    // DRIFT on a rotation adds up: five turns, five places.
    setup(2);
    g_home = g_p;
    evolve_set(g_e, g_p, 2, 1);
    for (int pass = 1; pass <= 5; pass++) {
        evolve_pass(g_e, g_p, 2, EvolveMode::drift, Mutation::rotate_left, 100, uint32_t(pass));
    }
    CHECK_EQ(int(g_p.tracks[2].steps[11].note), int(g_home.tracks[2].steps[0].note));
}

TEST(evolve_keep_stops_and_leaves_what_plays) {
    setup(0);
    evolve_set(g_e, g_p, 0, 1);
    evolve_pass(g_e, g_p, 0, EvolveMode::orbit, Mutation::velocity, 100, 3);
    g_home = g_p;
    evolve_keep(g_e);
    CHECK(!g_e.on());
    CHECK(!evolve_pass(g_e, g_p, 0, EvolveMode::orbit, Mutation::velocity, 100, 4));
    CHECK(steps_equal(g_p, g_home, 0));
    // And set to OFF is a KEEP.
    evolve_set(g_e, g_p, 0, 1);
    evolve_pass(g_e, g_p, 0, EvolveMode::orbit, Mutation::velocity, 100, 5);
    g_home = g_p;
    evolve_set(g_e, g_p, 0, 0);
    CHECK(!g_e.on());
    CHECK(steps_equal(g_p, g_home, 0));
}

TEST(evolve_orbit_keeps_an_edit_made_while_it_runs) {
    setup(3);
    evolve_set(g_e, g_p, 3, 1);
    evolve_pass(g_e, g_p, 3, EvolveMode::orbit, Mutation::velocity, 100, 1);
    // A trig typed in between turns, on a step that was off.
    g_p.tracks[3].steps[5].flags = kStepTrig;
    g_p.tracks[3].steps[5].note = 77;
    for (int pass = 2; pass <= 6; pass++) {
        evolve_pass(g_e, g_p, 3, EvolveMode::orbit, Mutation::velocity, 100, uint32_t(pass));
        CHECK((g_p.tracks[3].steps[5].flags & kStepTrig) != 0);
        CHECK_EQ(int(g_p.tracks[3].steps[5].note), 77);
    }
    // HOME keeps it too.
    evolve_home(g_e, g_p, 3);
    CHECK((g_p.tracks[3].steps[5].flags & kStepTrig) != 0);
    CHECK_EQ(int(g_p.tracks[3].steps[5].note), 77);
}

TEST(evolve_home_brings_a_chord_back) {
    setup(4);
    g_p.scale = 0;                                          // chromatic, so every shift lands
    CHECK(step_add_note(g_p, 4, 2, 70) == NoteAdd::added);
    evolve_set(g_e, g_p, 4, 1);
    bool moved = false;
    for (int pass = 1; pass <= 8; pass++) {
        evolve_pass(g_e, g_p, 4, EvolveMode::drift, Mutation::notes, 100, uint32_t(pass));
        uint8_t extras[kStepExtraNotes];
        step_extra_notes(g_p, 4, 2, extras);
        moved = moved || extras[0] != 70;
    }
    CHECK(moved);
    evolve_home(g_e, g_p, 4);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(g_p, 4, 2, extras), 1);
    CHECK_EQ(int(extras[0]), 70);
    CHECK_EQ(int(g_p.tracks[4].steps[2].note), 50);
}

TEST(evolve_orbit_chord_never_drifts) {
    setup(4);
    g_p.scale = 0;
    CHECK(step_add_note(g_p, 4, 2, 70) == NoteAdd::added);
    evolve_set(g_e, g_p, 4, 1);
    for (int pass = 1; pass <= 30; pass++) {
        evolve_pass(g_e, g_p, 4, EvolveMode::orbit, Mutation::notes, 100, uint32_t(pass));
        uint8_t extras[kStepExtraNotes];
        step_extra_notes(g_p, 4, 2, extras);
        // The chord moves with its note, one mutation from HOME: 70 + (note - 50).
        CHECK_EQ(int(extras[0]) - 70, int(g_p.tracks[4].steps[2].note) - 50);
    }
}
