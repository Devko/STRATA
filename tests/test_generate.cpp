// GEN: the weights, determinism, the key, the range, and what it leaves alone.
//
// Patterns are file-scope statics: one is about 8 KiB and CLAUDE.md caps a stack frame at 16 KiB.
#include "check.hpp"

#include "generate.hpp"
#include "pattern.hpp"
#include "scale.hpp"
#include "stepnotes.hpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

using namespace strata;

Pattern g_a;
Pattern g_b;

int hits(const Pattern &p, int track) {
    int n = 0;
    const TrackPattern &t = p.tracks[track];
    for (int i = 0; i < int(t.length); i++) n += (t.steps[i].flags & kStepTrig) != 0;
    return n;
}

bool same_track(const Pattern &a, const Pattern &b, int track) {
    for (int i = 0; i < kStepsPerTrack; i++) {
        const Step &x = a.tracks[track].steps[i];
        const Step &y = b.tracks[track].steps[i];
        if (x.flags != y.flags || x.note != y.note || x.velocity != y.velocity ||
            x.length != y.length) {
            return false;
        }
    }
    return true;
}

void fresh(Pattern &p, int length, int root, Scale scale) {
    pattern_init(p);
    p.root = uint8_t(root);
    p.scale = uint8_t(scale);
    p.seed = 0x1234567u;
    for (int t = 0; t < kTracks; t++) p.tracks[t].length = uint8_t(length);
}

}  // namespace

TEST(gen_chance_follows_the_bar) {
    // The one beats a beat beats an 8th beats a 16th, at every density that is not 0.
    for (int d = 5; d <= 100; d += 5) {
        CHECK(gen_hit_chance(0, d) >= gen_hit_chance(4, d));
        CHECK(gen_hit_chance(4, d) >= gen_hit_chance(2, d));
        CHECK(gen_hit_chance(2, d) >= gen_hit_chance(1, d));
        CHECK(gen_hit_chance(1, d) > 0);
    }
    CHECK_EQ(gen_hit_chance(0, 0), 0);
    CHECK_EQ(gen_hit_chance(7, 0), 0);
    CHECK_EQ(gen_hit_chance(0, 100), 100);
    // Every bar the same: step 16 is a one again.
    CHECK_EQ(gen_hit_chance(16, 50), gen_hit_chance(0, 50));
    CHECK_EQ(gen_hit_chance(20, 50), gen_hit_chance(4, 50));
}

TEST(gen_settings_clamp_what_an_encoder_sends) {
    const GenSettings s = GenSettings::make(300, 9, -4, 7);
    CHECK_EQ(int(s.density), 100);
    CHECK_EQ(int(s.range), kGenRangeMax);
    CHECK_EQ(int(s.leap), 0);
    CHECK(s.what == GenWhat::both);
    CHECK_EQ(int(GenSettings::make(50, 0, 50, 2).range), kGenRangeMin);
    CHECK(GenSettings::make(50, 1, 50, 2).what == GenWhat::notes);
}

TEST(gen_is_deterministic_and_each_variant_differs) {
    fresh(g_a, 16, 0, Scale::minor);
    fresh(g_b, 16, 0, Scale::minor);
    const GenSettings s = GenSettings::make(60, 2, 40, 0);
    CHECK(generate_track(g_a, 2, true, s, 7));
    CHECK(generate_track(g_b, 2, true, s, 7));
    CHECK(same_track(g_a, g_b, 2));

    fresh(g_b, 16, 0, Scale::minor);
    generate_track(g_b, 2, true, s, 8);
    CHECK(!same_track(g_a, g_b, 2));
}

TEST(gen_density_sets_how_busy_a_part_is) {
    // Averaged over many variants, so one lucky draw cannot pass or fail it.
    int sparse = 0, busy = 0;
    for (uint32_t v = 1; v <= 40; v++) {
        fresh(g_a, 64, 0, Scale::minor);
        generate_track(g_a, 0, false, GenSettings::make(20, 1, 0, 0), v);
        sparse += hits(g_a, 0);
        fresh(g_a, 64, 0, Scale::minor);
        generate_track(g_a, 0, false, GenSettings::make(90, 1, 0, 0), v);
        busy += hits(g_a, 0);
    }
    // 64 steps are four bars: about 2.5 a bar at 20, about 10 at 90.
    CHECK(sparse > 40 * 4 * 1);
    CHECK(sparse < 40 * 4 * 4);
    CHECK(busy > 40 * 4 * 8);
    CHECK(busy > 3 * sparse);
}

TEST(gen_drum_part_starts_on_the_one) {
    for (uint32_t v = 1; v <= 30; v++) {
        fresh(g_a, 16, 0, Scale::minor);
        generate_track(g_a, 0, false, GenSettings::make(kGenDownbeatDensity, 1, 0, 0), v);
        CHECK((g_a.tracks[0].steps[0].flags & kStepTrig) != 0);
    }
    fresh(g_a, 16, 0, Scale::minor);
    CHECK(!generate_track(g_a, 0, false, GenSettings::make(0, 1, 0, 0), 1));
    CHECK_EQ(hits(g_a, 0), 0);
}

TEST(gen_drum_part_keeps_its_note_and_accents_the_beat) {
    fresh(g_a, 16, 0, Scale::minor);
    g_a.tracks[0].steps[3].flags = kStepTrig;
    g_a.tracks[0].steps[3].note = 41;
    for (int i = 0; i < 16; i++) g_a.tracks[0].steps[i].note = i == 3 ? 41 : 60;
    generate_track(g_a, 0, false, GenSettings::make(100, 1, 0, 0), 3);
    const TrackPattern &t = g_a.tracks[0];
    for (int i = 0; i < 16; i++) {
        if ((t.steps[i].flags & kStepTrig) == 0) continue;
        // A drum part's notes are not pitches in a key: a new hit takes the part's note, an old
        // one keeps its own.
        CHECK_EQ(int(t.steps[i].note), 41);
        if (i == 0) CHECK(t.steps[i].velocity >= 110);
        if (i % 2 == 1) CHECK(t.steps[i].velocity <= 70);
    }
}

TEST(gen_melodic_notes_stay_in_the_key_and_the_range) {
    for (int range = 1; range <= 3; range++) {
        for (uint32_t v = 1; v <= 20; v++) {
            fresh(g_a, 64, 9, Scale::phrygian);               // A phrygian
            g_a.tracks[4].steps[0].flags = kStepTrig;
            g_a.tracks[4].steps[0].note = 50;                   // D3: the walk starts at A2 (45)
            generate_track(g_a, 4, true, GenSettings::make(80, range, 60, 0), v);
            const TrackPattern &t = g_a.tracks[4];
            for (int i = 0; i < 64; i++) {
                if ((t.steps[i].flags & kStepTrig) == 0) continue;
                CHECK(scale_contains(t.steps[i].note, 9, int(Scale::phrygian)));
                CHECK(t.steps[i].note >= 45);
                CHECK(t.steps[i].note <= 45 + 12 * range);
            }
        }
    }
}

TEST(gen_melodic_part_starts_on_the_root) {
    fresh(g_a, 16, 2, Scale::minor);
    generate_track(g_a, 1, true, GenSettings::make(70, 2, 50, 0), 5);
    const TrackPattern &t = g_a.tracks[1];
    for (int i = 0; i < 16; i++) {
        if ((t.steps[i].flags & kStepTrig) == 0) continue;
        CHECK_EQ(int(t.steps[i].note), 38);                    // D2: C3's octave, rooted down
        break;
    }
}

TEST(gen_leap_makes_wider_lines) {
    // The mean jump between hits, over many variants.
    auto spread = [](int leap) {
        int total = 0, moves = 0;
        for (uint32_t v = 1; v <= 40; v++) {
            fresh(g_a, 64, 0, Scale::minor);
            generate_track(g_a, 0, true, GenSettings::make(70, 3, leap, 0), v);
            int last = -1;
            for (int i = 0; i < 64; i++) {
                const Step &s = g_a.tracks[0].steps[i];
                if ((s.flags & kStepTrig) == 0) continue;
                if (last >= 0) {
                    total += s.note > last ? s.note - last : last - s.note;
                    moves++;
                }
                last = s.note;
            }
        }
        return moves > 0 ? total * 10 / moves : 0;
    };
    CHECK(spread(90) > spread(0) + 10);
}

TEST(gen_notes_keeps_the_rhythm) {
    fresh(g_a, 16, 0, Scale::minor);
    for (int i : {0, 3, 6, 10, 12}) g_a.tracks[5].steps[i].flags = kStepTrig;
    g_b = g_a;
    CHECK(generate_track(g_a, 5, true, GenSettings::make(100, 2, 50, int(GenWhat::notes)), 2));
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(int(g_a.tracks[5].steps[i].flags & kStepTrig),
                 int(g_b.tracks[5].steps[i].flags & kStepTrig));
    }
    // And on a drum track NOTES has nothing to do.
    CHECK(!generate_track(g_a, 5, false, GenSettings::make(100, 2, 50, int(GenWhat::notes)), 2));
}

TEST(gen_rhythm_keeps_the_notes_of_the_steps_it_keeps) {
    fresh(g_a, 16, 0, Scale::minor);
    for (int i = 0; i < 16; i++) {
        g_a.tracks[1].steps[i].flags = kStepTrig;
        g_a.tracks[1].steps[i].note = uint8_t(40 + i);
    }
    generate_track(g_a, 1, true, GenSettings::make(40, 2, 50, int(GenWhat::rhythm)), 4);
    for (int i = 0; i < 16; i++) CHECK_EQ(int(g_a.tracks[1].steps[i].note), 40 + i);
    CHECK(hits(g_a, 1) < 16);
}

TEST(gen_leaves_locks_conditions_and_other_tracks_alone) {
    fresh(g_a, 16, 0, Scale::minor);
    Step &s = g_a.tracks[3].steps[4];
    s.flags = kStepTrig;
    s.micro = 5;
    s.probability = 60;
    s.ratchet = 3;
    CHECK(pattern_set_lock(g_a, 3, 4, 2, 99));
    g_a.tracks[6].steps[2].flags = kStepTrig;
    g_b = g_a;
    generate_track(g_a, 3, true, GenSettings::make(100, 2, 50, 0), 9);
    CHECK_EQ(int(g_a.tracks[3].steps[4].micro), 5);
    CHECK_EQ(int(g_a.tracks[3].steps[4].probability), 60);
    CHECK_EQ(int(g_a.tracks[3].steps[4].ratchet), 3);
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_a, 3, 4, 2, value));
    CHECK_EQ(int(value), 99);
    CHECK(same_track(g_a, g_b, 6));
}

TEST(gen_moves_a_chord_with_its_note) {
    fresh(g_a, 16, 0, Scale::chromatic);
    g_a.tracks[2].steps[0].flags = kStepTrig;
    g_a.tracks[2].steps[0].note = 60;
    for (int i = 1; i < 16; i++) g_a.tracks[2].steps[i].flags = kStepTrig;
    CHECK(step_add_note(g_a, 2, 5, 70) == NoteAdd::added);
    const int was = g_a.tracks[2].steps[5].note;
    generate_track(g_a, 2, true, GenSettings::make(100, 2, 90, int(GenWhat::notes)), 11);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(g_a, 2, 5, extras), 1);
    CHECK_EQ(int(extras[0]) - 70, int(g_a.tracks[2].steps[5].note) - was);
}

TEST(gen_refuses_a_track_that_does_not_exist) {
    fresh(g_a, 16, 0, Scale::minor);
    CHECK(!generate_track(g_a, -1, true, GenSettings{}, 1));
    CHECK(!generate_track(g_a, kTracks, true, GenSettings{}, 1));
}

namespace {

uint32_t track_sum(const Pattern &p, int track) {
    uint32_t h = 2166136261u;
    const TrackPattern &t = p.tracks[track];
    for (int i = 0; i < kStepsPerTrack; i++) {
        const Step &s = t.steps[i];
        for (uint8_t b : {s.flags, s.note, s.velocity, s.length}) h = (h ^ b) * 16777619u;
    }
    return h;
}

struct GenCase {
    bool melodic;
    int length, root;
    Scale scale;
    int density, range, leap, what;
    uint32_t variant;
    int seed_step;  // a trig to inherit from before generating, or -1
    int seed_note;
};

constexpr GenCase kGenCases[] = {
    {false, 16, 0, Scale::minor, 60, 1, 0, 0, 1, -1, 0},
    {false, 32, 0, Scale::minor, 95, 1, 0, 1, 2, 4, 70},
    {false, 64, 0, Scale::minor, 20, 1, 0, 0, 3, -1, 0},
    {true, 16, 0, Scale::minor, 60, 2, 40, 0, 7, -1, 0},
    {true, 64, 5, Scale::major, 80, 1, 90, 0, 9, 4, 70},
    {true, 64, 9, Scale::minor, 70, 3, 100, 2, 11, 4, 70},
    {true, 32, 2, Scale::chromatic, 100, 2, 10, 0, 13, -1, 0},
    {true, 48, 11, Scale::major, 50, 2, 60, 0, 0xFFFFFFFFu, 4, 70},
    {true, 64, 3, Scale::minor, 90, 3, 50, 0, 17, 0, 2},        // the lowest root, from step 0
    {true, 64, 10, Scale::major, 90, 3, 50, 0, 19, 4, 125},     // the walk held under 127
    {false, 1, 0, Scale::minor, 100, 1, 0, 0, 23, -1, 0},       // a one-step part
};

void run_case(Pattern &p, const GenCase &c) {
    fresh(p, c.length, c.root, c.scale);
    if (c.seed_step >= 0) {
        Step &s = p.tracks[3].steps[c.seed_step];
        s.flags = kStepTrig;
        s.note = uint8_t(c.seed_note);
        s.length = 9;
        s.velocity = 33;
        // A trigless lock step keeps its own note and gate if it gains a hit.
        for (int i = 1; i < c.length; i += 3) {
            if (i == c.seed_step) continue;
            Step &t = p.tracks[3].steps[i];
            t.flags = kStepTrigless;
            t.note = 40;
            t.length = 3;
        }
    }
    generate_track(p, 3, c.melodic, GenSettings::make(c.density, c.range, c.leap, c.what), c.variant);
}

}  // namespace

// GEN's draws are pinned, step by step: the same seed, settings and press give the same part in
// every build. A deliberate change to GENERATE updates these sums (STRATA_GOLDEN=print lists them).
TEST(gen_parts_are_the_same_in_every_build) {
    static const uint32_t kSums[] = {0xA4114282u, 0x0BF2DA14u, 0xFE2D9C63u, 0xEF86882Au,
                                     0xF60E4FEAu, 0x9EC2DEAAu, 0x0BFB6D7Fu, 0x9224E267u,
                                     0x0CE1DB01u, 0x550485FEu, 0xBABBD51Du};
    int k = 0;
    for (const GenCase &c : kGenCases) {
        run_case(g_a, c);
        const uint32_t sum = track_sum(g_a, 3);
        if (std::getenv("STRATA_GOLDEN") != nullptr) std::printf("    gen case %d: 0x%08Xu\n", k, unsigned(sum));
        CHECK_EQ(sum, kSums[k]);
        k++;
    }
}

// The answer says whether anything changed: the same press again changes nothing, and a press
// that only puts back a trig, a gate or a slide says it did.
TEST(gen_reports_exactly_whether_it_changed_the_part) {
    const GenSettings drums = GenSettings::make(60, 1, 0, 1);    // RHYTHM
    fresh(g_a, 16, 0, Scale::minor);
    CHECK(generate_track(g_a, 1, false, drums, 5));
    CHECK(!generate_track(g_a, 1, false, drums, 5));
    int hit = 0;
    while ((g_a.tracks[1].steps[hit].flags & kStepTrig) == 0) hit++;
    g_a.tracks[1].steps[hit].flags = 0;
    CHECK(generate_track(g_a, 1, false, drums, 5));
    CHECK((g_a.tracks[1].steps[hit].flags & kStepTrig) != 0);

    const GenSettings line = GenSettings::make(70, 2, 100, 0);
    fresh(g_a, 64, 0, Scale::minor);
    CHECK(generate_track(g_a, 2, true, line, 3));
    CHECK(!generate_track(g_a, 2, true, line, 3));
    hit = 0;
    while ((g_a.tracks[2].steps[hit].flags & kStepTrig) == 0) hit++;
    // Only a velocity, or only a note, put back: still a change.
    g_a.tracks[2].steps[hit].velocity = uint8_t(g_a.tracks[2].steps[hit].velocity ^ 1);
    CHECK(generate_track(g_a, 2, true, line, 3));
    CHECK(!generate_track(g_a, 2, true, line, 3));
    g_a.tracks[2].steps[hit].note = uint8_t(g_a.tracks[2].steps[hit].note + 1);
    CHECK(generate_track(g_a, 2, true, line, 3));
    CHECK(!generate_track(g_a, 2, true, line, 3));
    const uint8_t gate = g_a.tracks[2].steps[hit].length;
    g_a.tracks[2].steps[hit].length = uint8_t(gate + 1);
    CHECK(generate_track(g_a, 2, true, line, 3));
    CHECK_EQ(int(g_a.tracks[2].steps[hit].length), int(gate));
    int slid = 0;
    while (slid < 64 && (g_a.tracks[2].steps[slid].flags & kStepSlide) == 0) slid++;
    CHECK(slid < 64);
    g_a.tracks[2].steps[slid].flags = uint8_t(g_a.tracks[2].steps[slid].flags & ~kStepSlide);
    CHECK(generate_track(g_a, 2, true, line, 3));
    CHECK((g_a.tracks[2].steps[slid].flags & kStepSlide) != 0);
}

TEST(gen_settings_edges) {
    CHECK_EQ(int(GenSettings::make(0, 1, 0, 0).density), 0);
    CHECK(GenSettings::make(50, 1, 0, int(GenWhat::count)).what == GenWhat::both);
    CHECK(GenSettings::make(50, 1, 0, int(GenWhat::count) - 1).what == GenWhat(int(GenWhat::count) - 1));
}
