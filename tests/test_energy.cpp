// ENERGY (core/energy.hpp): its arithmetic, then what the scheduler plays under it. The gesture
// is in tests/test_ui.cpp, the engine and LOCK in tests/test_lockstore.cpp.
#include "check.hpp"

#include "apprig.hpp"
#include "energy.hpp"
#include "fakehost.hpp"
#include "pattern.hpp"
#include "project.hpp"
#include "scheduler.hpp"
#include "timing.hpp"

using namespace strata;

namespace {

constexpr uint32_t kStepTicks = uint32_t(kTicksPerSixteenth);

constexpr EnergyTier kAllTiers[] = {EnergyTier::hats, EnergyTier::perc, EnergyTier::mid,
                                    EnergyTier::line, EnergyTier::pad,  EnergyTier::kick,
                                    EnergyTier::midi};

void blank(Pattern &pattern, uint8_t length = 16) {
    pattern_init(pattern);
    for (int t = 0; t < kTracks; t++) {
        pattern.tracks[t].length = length;
        pattern.tracks[t].speed = uint8_t(Speed::x1);
        pattern.tracks[t].swing = kSwingInherit;
    }
    pattern.master_length = length;
    pattern.swing = uint8_t(kSwingMin);
    pattern.seed = 0xE4E2u;
}

Step &put_trig(Pattern &pattern, int track, int step, uint8_t note = 0) {
    Step &s = pattern.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = note != 0 ? note : uint8_t(36 + track);
    s.velocity = 100;
    s.length = 8;
    s.micro = 0;
    s.probability = 100;
    s.condition = condition_pack(Condition::NONE);
    s.ratchet = ratchet_pack(1, RatchetRamp::none);
    return s;
}

// Every step of every track a trig: what thinning has to work on.
void full(Pattern &pattern) {
    blank(pattern);
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < 16; s++) put_trig(pattern, t, s);
    }
}

void tiers(PerformState &perform, EnergyTier tier) {
    for (int t = 0; t < kTracks; t++) perform.energy_tier[t] = uint8_t(tier);
}

struct Log {
    static constexpr int kMax = 4096;
    StepEvent events[kMax];
    uint32_t ticks[kMax];
    int count = 0;
    void clear() { count = 0; }
    int on_track(int track) const {
        int n = 0;
        for (int i = 0; i < count; i++) n += events[i].track == uint8_t(track) ? 1 : 0;
        return n;
    }
    // Bit s: track `track` sounded on a tick of step s (in one pass of 16).
    uint32_t steps_sounded(int track) const {
        uint32_t mask = 0;
        for (int i = 0; i < count; i++) {
            if (events[i].track != uint8_t(track)) continue;
            mask |= 1u << ((ticks[i] / kStepTicks) & 15u);
        }
        return mask;
    }
};

void run(const Pattern &pattern, const PerformState &perform, int loops, Log &log,
         Scheduler *keep = nullptr) {
    Scheduler local;
    Scheduler &sched = keep != nullptr ? *keep : local;
    sched.configure(pattern);
    sched.reset();
    log.clear();
    const uint32_t end = uint32_t(loops) * 16u * kStepTicks;
    for (uint32_t tick = 0; tick < end; tick++) {
        StepEvent events[kMaxEventsPerTick];
        const int n = sched.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
        for (int i = 0; i < n && log.count < Log::kMax; i++) {
            log.events[log.count] = events[i];
            log.ticks[log.count] = tick;
            log.count++;
        }
    }
}

bool same(const Log &a, const Log &b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count; i++) {
        const StepEvent &x = a.events[i];
        const StepEvent &y = b.events[i];
        if (a.ticks[i] != b.ticks[i] || x.track != y.track || x.step != y.step ||
            x.note != y.note || x.velocity != y.velocity || x.trigless != y.trigless ||
            x.ratchet_index != y.ratchet_index || x.frame != y.frame) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- the arithmetic --------------------------------------------------------

TEST(energy_turn_moves_two_a_click_stops_at_rest_and_at_both_ends) {
    CHECK_EQ(int(energy_turn(0, 1)), 2);
    CHECK_EQ(int(energy_turn(0, -3)), -6);
    CHECK_EQ(int(energy_turn(4, -5)), 0);          // the detent
    CHECK_EQ(int(energy_turn(-2, 1)), 0);
    CHECK_EQ(int(energy_turn(0, -1)), -2);         // and on past it from rest
    CHECK_EQ(int(energy_turn(60, 9)), kEnergyRange);
    CHECK_EQ(int(energy_turn(-60, -9)), -kEnergyRange);
    CHECK_EQ(int(energy_turn(127, 0)), kEnergyRange);    // a damaged byte held to the range
    CHECK_EQ(int(energy_turn(-128, 0)), -kEnergyRange);
}

TEST(energy_tiers_follow_the_machines) {
    CHECK(energy_tier(uint8_t(MachineId::hat)) == EnergyTier::hats);
    CHECK(energy_tier(uint8_t(MachineId::perc)) == EnergyTier::perc);
    CHECK(energy_tier(uint8_t(MachineId::snare)) == EnergyTier::mid);
    CHECK(energy_tier(uint8_t(MachineId::sample)) == EnergyTier::mid);
    CHECK(energy_tier(uint8_t(MachineId::va_mono)) == EnergyTier::line);
    CHECK(energy_tier(uint8_t(MachineId::wave_mono)) == EnergyTier::line);
    CHECK(energy_tier(uint8_t(MachineId::va_poly)) == EnergyTier::pad);
    CHECK(energy_tier(uint8_t(MachineId::wave_poly)) == EnergyTier::pad);
    CHECK(energy_tier(uint8_t(MachineId::fm)) == EnergyTier::pad);
    CHECK(energy_tier(uint8_t(MachineId::grain)) == EnergyTier::pad);
    CHECK(energy_tier(uint8_t(MachineId::kick)) == EnergyTier::kick);
    CHECK(energy_tier(uint8_t(MachineId::midi)) == EnergyTier::midi);
    CHECK(energy_tier(200) == EnergyTier::mid);
}

TEST(energy_cut_is_zero_at_and_above_rest_and_grows_on_the_way_down) {
    for (EnergyTier tier : kAllTiers) {
        for (int e = 0; e <= kEnergyRange; e++) CHECK_EQ(energy_cut(int8_t(e), tier), 0);
        int previous = 0;
        for (int e = 0; e >= -kEnergyRange; e--) {
            const int cut = energy_cut(int8_t(e), tier);
            CHECK(cut >= previous);
            CHECK(cut >= 0 && cut <= 100);
            previous = cut;
        }
    }
    // At the bottom: every tier gone except the pads, which keep their strongest hits.
    CHECK_EQ(energy_cut(-64, EnergyTier::hats), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::perc), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::mid), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::line), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::kick), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::midi), 100);
    CHECK_EQ(energy_cut(-64, EnergyTier::pad), 60);
    // A damaged tier byte thins nothing.
    CHECK_EQ(energy_cut(-64, EnergyTier(99)), 0);
}

TEST(energy_thins_the_hats_first_and_the_kick_last) {
    CHECK(energy_cut(-2, EnergyTier::hats) > 0);
    for (int e = -1; e > -40; e--) CHECK_EQ(energy_cut(int8_t(e), EnergyTier::kick), 0);
    CHECK(energy_cut(-42, EnergyTier::kick) > 0);
    // At every point on the way down a tier earlier in the order is thinned at least as far.
    for (int e = 0; e >= -kEnergyRange; e--) {
        const int8_t x = int8_t(e);
        CHECK(energy_cut(x, EnergyTier::hats) >= energy_cut(x, EnergyTier::perc));
        CHECK(energy_cut(x, EnergyTier::perc) >= energy_cut(x, EnergyTier::mid));
        CHECK(energy_cut(x, EnergyTier::mid) >= energy_cut(x, EnergyTier::line));
        CHECK(energy_cut(x, EnergyTier::line) >= energy_cut(x, EnergyTier::pad));
        CHECK(energy_cut(x, EnergyTier::line) >= energy_cut(x, EnergyTier::kick));
    }
}

TEST(energy_strength_follows_the_bar_and_an_accent_lifts_it) {
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 64; step++) {
            const int s = energy_strength(0x1234u, track, step, false);
            const int in_bar = step & 15;
            const int low = in_bar == 0 ? 75 : (in_bar & 3) == 0 ? 50 : (in_bar & 1) == 0 ? 25 : 0;
            CHECK(s >= low && s <= low + 24);
            const int a = energy_strength(0x1234u, track, step, true);
            CHECK_EQ(a, s + 15 > 99 ? 99 : s + 15);
            // Fixed: the same arguments, the same strength.
            CHECK_EQ(energy_strength(0x1234u, track, step, false), s);
        }
    }
}

TEST(energy_probability_lifts_only_above_rest_and_leaves_zero_off) {
    for (int p = 0; p <= 100; p++) {
        for (int e = -kEnergyRange; e <= 0; e++) {
            CHECK_EQ(int(energy_probability(uint8_t(p), int8_t(e))), p);
        }
        CHECK_EQ(int(energy_probability(uint8_t(p), kEnergyRange)), p == 0 ? 0 : 100);
        int previous = p;
        for (int e = 1; e <= kEnergyRange; e++) {
            const int lifted = energy_probability(uint8_t(p), int8_t(e));
            CHECK(lifted >= previous && lifted <= 100);
            previous = lifted;
        }
    }
    CHECK_EQ(int(energy_probability(50, 32)), 75);
}

TEST(energy_ghost_chance_is_zero_at_rest_and_for_pads_and_midi_and_grows_upwards) {
    for (EnergyTier tier : kAllTiers) {
        for (int step = 0; step < 16; step++) {
            for (int e = -kEnergyRange; e <= 0; e++) {
                CHECK_EQ(energy_ghost_chance(int8_t(e), tier, step), 0);
            }
            int previous = 0;
            for (int e = 1; e <= kEnergyRange; e++) {
                const int c = energy_ghost_chance(int8_t(e), tier, step);
                CHECK(c >= previous && c <= 100);
                previous = c;
            }
        }
    }
    for (int step = 0; step < 16; step++) {
        CHECK_EQ(energy_ghost_chance(64, EnergyTier::pad, step), 0);
        CHECK_EQ(energy_ghost_chance(64, EnergyTier::midi, step), 0);
        for (int e = 1; e <= 32; e++) CHECK_EQ(energy_ghost_chance(int8_t(e), EnergyTier::kick, step), 0);
    }
    // The hats fill their off-beat 8ths completely at the top; the kick only rarely.
    CHECK_EQ(energy_ghost_chance(64, EnergyTier::hats, 2), 100);
    CHECK(energy_ghost_chance(64, EnergyTier::kick, 2) < 20);
    CHECK(energy_ghost_chance(64, EnergyTier::kick, 2) > 0);
}

TEST(energy_ghost_velocities_stay_quiet) {
    int ghosts = 0;
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 64; step++) {
            uint8_t v = 0;
            if (energy_ghost(64, EnergyTier::hats, 0xABCDu, track, step, v)) {
                ghosts++;
                CHECK(v >= 40 && v <= 86);
            }
            if (energy_ghost(64, EnergyTier::line, 0xABCDu, track, step, v)) {
                CHECK(v >= 70 && v <= 108);
            }
        }
    }
    CHECK(ghosts > 200);   // most of 512 at the top
}

TEST(energy_source_step_is_the_trig_before_wrapping_and_none_on_an_empty_track) {
    static Pattern pattern;
    blank(pattern);
    const TrackPattern &track = pattern.tracks[0];
    CHECK_EQ(energy_source_step(track, 16, 5), -1);
    put_trig(pattern, 0, 4);
    put_trig(pattern, 0, 12);
    CHECK_EQ(energy_source_step(track, 16, 5), 4);
    CHECK_EQ(energy_source_step(track, 16, 11), 4);
    CHECK_EQ(energy_source_step(track, 16, 13), 12);
    CHECK_EQ(energy_source_step(track, 16, 2), 12);    // wraps
    CHECK_EQ(energy_source_step(track, 8, 6), 4);      // step 12 is past an 8-step track
    CHECK_EQ(energy_source_step(track, 8, 1), 4);
    // A trigless step lends nothing: it has no note to play.
    pattern.tracks[1].steps[3].flags = kStepTrigless;
    CHECK_EQ(energy_source_step(pattern.tracks[1], 16, 5), -1);
}

// A trig that does not play every pass is not copied: the walk goes on back past it to a plain
// one, and a track with only such trigs gets no ghost.
TEST(energy_source_step_skips_conditional_and_zero_percent_trigs) {
    static Pattern pattern;
    blank(pattern);
    TrackPattern &track = pattern.tracks[0];
    put_trig(pattern, 0, 2);
    put_trig(pattern, 0, 6);
    put_trig(pattern, 0, 9);
    put_trig(pattern, 0, 12);
    track.steps[12].condition = condition_pack(Condition::FILL);       // a FILL-only crash
    track.steps[9].condition = condition_pack_a_b(1, 4);              // a 1:4 hit
    track.steps[6].probability = 0;                                   // a step set to 0%
    CHECK_EQ(energy_source_step(track, 16, 14), 2);
    CHECK_EQ(energy_source_step(track, 16, 1), 2);                    // round the end too
    track.steps[6].probability = 40;                                  // sometimes still lends
    CHECK_EQ(energy_source_step(track, 16, 14), 6);
    track.steps[2].condition = condition_pack(Condition::FIRST);
    track.steps[6].condition = condition_pack(Condition::NOT_FILL);
    CHECK_EQ(energy_source_step(track, 16, 14), -1);                  // nothing plain: no ghost
}

// --- in the scheduler ------------------------------------------------------

TEST(energy_at_rest_plays_the_programmed_part_to_the_bit) {
    static Pattern pattern;
    blank(pattern);
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < 16; s += 1 + (t & 1)) {
            Step &step = put_trig(pattern, t, s);
            step.probability = uint8_t(30 + 7 * s);
            if (s == 6) step.condition = condition_pack(Condition::PRE);
            if (s == 9) step.ratchet = ratchet_pack(3, RatchetRamp::up);
        }
    }
    PerformState plain;
    PerformState rest;
    tiers(rest, EnergyTier::hats);
    rest.energy = 0;
    static Log a;
    static Log b;
    run(pattern, plain, 4, a);
    run(pattern, rest, 4, b);
    CHECK(a.count > 100);
    CHECK(same(a, b));
}

TEST(energy_down_thins_the_hats_before_the_kick_and_empties_both_at_the_bottom) {
    static Pattern pattern;
    full(pattern);
    PerformState perform;
    perform.energy_tier[0] = uint8_t(EnergyTier::kick);
    perform.energy_tier[1] = uint8_t(EnergyTier::hats);
    perform.energy_tier[2] = uint8_t(EnergyTier::pad);
    static Log log;

    perform.energy = -20;
    run(pattern, perform, 1, log);
    CHECK_EQ(log.on_track(0), 16);           // the kick has not started
    CHECK(log.on_track(1) < 16);             // the hats have
    CHECK(log.on_track(1) > 0);
    CHECK_EQ(log.on_track(2), 16);           // nor the pads

    perform.energy = -64;
    run(pattern, perform, 1, log);
    CHECK_EQ(log.on_track(0), 0);
    CHECK_EQ(log.on_track(1), 0);
    // The pads keep their strongest hits, the one among them.
    CHECK(log.on_track(2) > 0);
    CHECK(log.on_track(2) < 16);
    CHECK((log.steps_sounded(2) & 1u) != 0);
}

TEST(energy_down_drops_hits_in_one_fixed_order_and_brings_each_back) {
    static Pattern pattern;
    full(pattern);
    for (int t = 0; t < kTracks; t++) {
        pattern.tracks[t].steps[(3 * t) & 15].flags |= kStepAccent;
    }
    static Log log;
    for (EnergyTier tier : kAllTiers) {
        PerformState perform;
        tiers(perform, tier);
        uint32_t previous[kTracks];
        for (int t = 0; t < kTracks; t++) previous[t] = 0xFFFFu;
        for (int e = 0; e >= -kEnergyRange; e -= 2) {
            perform.energy = int8_t(e);
            run(pattern, perform, 2, log);
            for (int t = 0; t < kTracks; t++) {
                const uint32_t now = log.steps_sounded(t);
                CHECK_EQ(now & ~previous[t], 0u);   // nothing comes back on the way down
                previous[t] = now;
            }
        }
    }
}

TEST(energy_never_thins_a_trigless_step_and_keeps_the_chain) {
    static Pattern pattern;
    blank(pattern);
    // Track 0, hats: a trigless sweep on step 1 and a trig on step 0. Track 1, a pad, NEI on
    // step 0: the hat's trig is thinned at -64, but its condition passed, so NEI still fires.
    put_trig(pattern, 0, 1);
    pattern.tracks[0].steps[1].flags = kStepTrigless;
    put_trig(pattern, 0, 0);
    put_trig(pattern, 1, 0).condition = condition_pack(Condition::NEI);
    PerformState perform;
    perform.energy_tier[0] = uint8_t(EnergyTier::hats);
    perform.energy_tier[1] = uint8_t(EnergyTier::pad);
    perform.energy = -64;
    static Log log;
    Scheduler sched;
    run(pattern, perform, 1, log, &sched);
    int trigless = 0;
    int notes0 = 0;
    for (int i = 0; i < log.count; i++) {
        if (log.events[i].track == 0) {
            if (log.events[i].trigless) trigless++;
            else notes0++;
        }
    }
    CHECK_EQ(trigless, 1);
    CHECK_EQ(notes0, 0);
    CHECK_EQ(log.on_track(1), 1);
    CHECK(sched.last_condition(0));
}

TEST(energy_up_adds_ghosts_with_the_sound_of_the_trig_before) {
    static Pattern pattern;
    blank(pattern);
    Step &kick = put_trig(pattern, 0, 0, 40);
    kick.length = 5;
    put_trig(pattern, 0, 8, 43);
    PerformState perform;
    perform.energy_tier[0] = uint8_t(EnergyTier::hats);   // the most appetite, to see many
    perform.energy = 64;
    static Log log;
    run(pattern, perform, 1, log);
    CHECK(log.on_track(0) > 8);
    for (int i = 0; i < log.count; i++) {
        const StepEvent &e = log.events[i];
        if (e.track != 0) continue;
        const int slot = int(log.ticks[i] / kStepTicks);
        if (slot == 0 || slot == 8) {
            CHECK_EQ(int(e.velocity), 100);                 // the programmed hits, untouched
            continue;
        }
        // A ghost: the note, length and step (for its locks) of the trig before it.
        const int from = slot < 8 ? 0 : 8;
        CHECK_EQ(int(e.step), from);
        CHECK_EQ(int(e.note), from == 0 ? 40 : 43);
        CHECK_EQ(int(e.length_index), from == 0 ? 5 : 8);
        CHECK(e.velocity >= 40 && e.velocity <= 86);
        CHECK(!e.trigless);
        CHECK(!e.accent);
        CHECK(e.ghost);
    }
    // And the programmed hits are not ghosts.
    for (int i = 0; i < log.count; i++) {
        const int slot = int(log.ticks[i] / kStepTicks);
        if (log.events[i].track == 0 && (slot == 0 || slot == 8)) CHECK(!log.events[i].ghost);
    }
}

TEST(energy_up_never_wakes_an_empty_track_or_fills_a_pad) {
    static Pattern pattern;
    blank(pattern);
    put_trig(pattern, 1, 0);
    PerformState perform;
    tiers(perform, EnergyTier::hats);
    perform.energy_tier[1] = uint8_t(EnergyTier::pad);
    perform.energy = 64;
    static Log log;
    run(pattern, perform, 2, log);
    CHECK_EQ(log.count, 2);           // the pad's own hit, twice; nothing else anywhere
    CHECK_EQ(log.on_track(1), 2);
}

TEST(energy_up_adds_hits_in_one_fixed_order) {
    static Pattern pattern;
    blank(pattern);
    for (int t = 0; t < kTracks; t++) put_trig(pattern, t, 0);
    static Log log;
    for (EnergyTier tier : kAllTiers) {
        PerformState perform;
        tiers(perform, tier);
        uint32_t previous[kTracks] = {};
        for (int e = 0; e <= kEnergyRange; e += 2) {
            perform.energy = int8_t(e);
            run(pattern, perform, 2, log);
            for (int t = 0; t < kTracks; t++) {
                const uint32_t now = log.steps_sounded(t);
                CHECK_EQ(previous[t] & ~now, 0u);   // nothing goes away on the way up
                previous[t] = now;
            }
        }
    }
}

TEST(energy_up_lifts_a_half_chance_step_to_every_pass) {
    static Pattern pattern;
    blank(pattern);
    put_trig(pattern, 0, 0).probability = 50;
    PerformState perform;
    perform.energy_tier[0] = uint8_t(EnergyTier::pad);   // no ghosts in the way
    static Log log;
    run(pattern, perform, 64, log);
    const int at_rest = log.on_track(0);
    CHECK(at_rest > 10 && at_rest < 54);
    perform.energy = 64;
    run(pattern, perform, 64, log);
    CHECK_EQ(log.on_track(0), 64);
}

// --- in the engine ---------------------------------------------------------

// The engine hands the scheduler each track's tier from its machine: at -20 the hats have begun
// to thin and the kick has not, so a weak 16th on both plays on the kick only.
TEST(energy_engine_takes_the_tiers_from_the_machines) {
    using strata::test::FakeHost;
    Engine &engine = strata::test::fresh();
    int kick = -1;
    int hat = -1;
    for (int t = 0; t < kTracks; t++) {
        if (kick < 0 && engine.machine_id(t) == uint8_t(MachineId::kick)) kick = t;
        if (hat < 0 && engine.machine_id(t) == uint8_t(MachineId::hat)) hat = t;
    }
    CHECK(kick >= 0 && hat >= 0);
    if (kick < 0 || hat < 0) return;
    strata::test::put_trig(kick, 1);
    strata::test::put_trig(hat, 1);
    strata::test::pump();
    const Machine *k = engine.machine(kick);
    const Machine *h = engine.machine(hat);
    CHECK(k != nullptr && h != nullptr);
    if (k == nullptr || h == nullptr) return;

    // At rest both play.
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(kStepTicks, 0);
    strata::test::pump();
    CHECK(k->active());
    CHECK(h->active());
    FakeHost::the().set_running(false);
    strata::test::post(Cmd::stop, 0, 0, 1);
    strata::test::pump(2);
    CHECK(strata::test::settle());

    strata::test::post(Cmd::set_energy, 0, 0, uint8_t(int8_t(-20)));
    strata::test::pump();
    CHECK_EQ(int(engine.energy()), -20);
    // Step 1's strength on the hat track: a 16th, so under the hats' cut at -20.
    CHECK(energy_strength(engine.pattern().seed, hat, 1, false) <
          energy_cut(-20, EnergyTier::hats));
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(kStepTicks, 0);
    strata::test::pump();
    CHECK(k->active());
    CHECK(!h->active());

    FakeHost::the().set_running(false);
    strata::test::post(Cmd::stop, 0, 0, 1);
    strata::test::post(Cmd::set_energy, 0, 0, 0);
    strata::test::pump(2);
    CHECK(strata::test::settle());
}

// A ghost lights nothing (its `step` is the trig it borrows from, not a step that played), and on
// a track whose arp is on it is one note, not a run. MIDI OUT counts what sounds,
// Status::triggered what lights. Only one-note machines fill (pads and MID never do), so a line
// is the case to hold.
TEST(energy_ghost_lights_nothing_and_plays_one_note_with_the_arp_on) {
    using strata::test::FakeHost;
    {
        const uint8_t want = uint8_t(MachineId::va_mono);
        Engine &engine = strata::test::fresh();
        int track = -1;
        for (int t = 0; t < kTracks && track < 0; t++) {
            if (engine.machine_id(t) == want) track = t;
        }
        CHECK(track >= 0);
        if (track < 0) return;
        const uint8_t tr = uint8_t(track);
        Pattern &live = engine.project_for_load().live();
        for (int s = 0; s < kStepsPerTrack; s++) live.tracks[track].steps[s].flags = 0;
        strata::test::put_trig(track, 0);
        Step &first = live.tracks[track].steps[0];
        first.length = 8;                                    // one step
        first.probability = 100;
        first.condition = condition_pack(Condition::NONE);
        first.ratchet = ratchet_pack(1, RatchetRamp::none);
        // ARP: MOD UP, RAT 1/32 (two hits a step), one octave; MIDI OUT on channel 3.
        auto smallest = [](int zone, int zones) { return uint8_t((zone * 128 + zones - 1) / zones); };
        strata::test::post(Cmd::set_param, tr, 0, uint8_t(kArpFirst + 0),
                           smallest(1, int(ArpMode::count)));
        strata::test::post(Cmd::set_param, tr, 0, uint8_t(kArpFirst + 1), smallest(5, kArpRates));
        strata::test::post(Cmd::set_param, tr, 0, uint8_t(kArpFirst + 2), smallest(0, kArpMaxOctaves));
        strata::test::post(Cmd::set_param, tr, 0, uint8_t(kArpFirst + 3), 0);
        strata::test::post(Cmd::set_midi_out, tr, 0, 3);
        strata::test::post(Cmd::set_energy, 0, 0, uint8_t(int8_t(64)));
        strata::test::pump(2);

        // The first step a ghost lands on at +64.
        const EnergyTier tier = energy_tier(want);
        CHECK(tier == EnergyTier::line);
        int ghost_step = -1;
        for (int s = 1; s < 16 && ghost_step < 0; s++) {
            uint8_t v = 0;
            if (energy_ghost(64, tier, engine.pattern().seed, track, s, v)) ghost_step = s;
        }
        CHECK(ghost_step >= 1);
        if (ghost_step < 1) return;

        FakeHost &host = FakeHost::the();
        host.clear_midi();
        host.set_running(true);
        auto window = [&](uint32_t from, uint32_t to, int &ons, int &lit) {
            const int ons_before = host.midi_messages(0x90, 3);
            const uint8_t lit_before =
                engine.status().triggered[track].load(std::memory_order_relaxed);
            for (uint32_t tick = from; tick < to; tick++) {
                host.deliver_tick(tick, 0);
                strata::test::pump();
            }
            ons = host.midi_messages(0x90, 3) - ons_before;
            lit = int(uint8_t(engine.status().triggered[track].load(std::memory_order_relaxed) -
                              lit_before));
        };
        int ons = 0, lit = 0;
        // The programmed step: the arp's run, two hits, each lit.
        window(0, kStepTicks, ons, lit);
        CHECK_EQ(ons, 2);
        CHECK_EQ(lit, 2);
        window(kStepTicks, uint32_t(ghost_step) * kStepTicks, ons, lit);   // up to the ghost
        // The ghost: one note, nothing lit.
        window(uint32_t(ghost_step) * kStepTicks, uint32_t(ghost_step + 1) * kStepTicks, ons, lit);
        CHECK_EQ(ons, 1);
        CHECK_EQ(lit, 0);

        host.set_running(false);
        strata::test::post(Cmd::stop, 0, 0, 1);
        strata::test::post(Cmd::set_energy, 0, 0, 0);
        strata::test::post(Cmd::set_midi_out, tr, 0, 0);
        strata::test::pump(2);
        CHECK(strata::test::settle());
    }
}

// Track Launch (core/launch.hpp): a launched lane plays its own pattern, and ENERGY's draws are
// that pattern's, as the probability roll's are. The lane's events with a different master are
// the events of its own pattern played whole, thinned and filled alike.
TEST(energy_draws_from_a_launched_lanes_own_seed) {
    static Pattern master;
    static Pattern part;
    full(master);
    master.seed = 0x1111u;
    full(part);
    part.seed = 0x7A5Eu;
    for (int s = 1; s < 16; s += 2) part.tracks[2].steps[s].flags = 0;   // room for ghosts
    const Pattern *lanes[kTracks] = {};
    lanes[2] = &part;
    uint32_t ticks[kTracks] = {};

    constexpr int kAmounts[] = {-30, 40};
    for (int e : kAmounts) {
        PerformState perform;
        tiers(perform, EnergyTier::hats);
        perform.energy = int8_t(e);
        static Log alone;
        run(part, perform, 2, alone);   // the part as the whole pattern
        static Log launched;
        launched.clear();
        Scheduler sched;
        sched.configure(master, lanes, 100.0f);
        sched.reset();
        for (uint32_t tick = 0; tick < 2u * 16u * kStepTicks; tick++) {
            for (int t = 0; t < kTracks; t++) ticks[t] = tick;
            StepEvent events[kMaxEventsPerTick];
            const int n = sched.tick(ticks, 0, master, lanes, perform, events, kMaxEventsPerTick);
            for (int i = 0; i < n && launched.count < Log::kMax; i++) {
                launched.events[launched.count] = events[i];
                launched.ticks[launched.count] = tick;
                launched.count++;
            }
        }
        CHECK_EQ(launched.on_track(2), alone.on_track(2));
        CHECK_EQ(launched.steps_sounded(2), alone.steps_sounded(2));
        int ghosts_alone = 0, ghosts_launched = 0;
        for (int i = 0; i < alone.count; i++) {
            if (alone.events[i].track == 2 && alone.events[i].ghost) ghosts_alone++;
        }
        for (int i = 0; i < launched.count; i++) {
            if (launched.events[i].track == 2 && launched.events[i].ghost) ghosts_launched++;
        }
        CHECK_EQ(ghosts_launched, ghosts_alone);
        if (e > 0) CHECK(ghosts_alone > 0);
        // And the master's seed would have drawn differently: the test can tell them apart.
        static Log other;
        static Pattern reseeded;
        reseeded = part;
        reseeded.seed = master.seed;
        run(reseeded, perform, 2, other);
        CHECK(other.steps_sounded(2) != alone.steps_sounded(2) ||
              other.on_track(2) != alone.on_track(2));
    }
}
