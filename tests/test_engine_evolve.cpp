// app/engine.cpp's GEN and EVOLVE: the commands, the pass boundary and the resets.
//
// core/generate.hpp and core/evolve.hpp are tested on their own (test_generate.cpp,
// test_evolve.cpp); these check the engine's half: that a turn lands on the pass boundary and
// says so, that HOME and KEEP reach the track, and that a pattern change re-homes.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include "evolve.hpp"
#include "generate.hpp"
#include "pattern.hpp"
#include "queue.hpp"

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

namespace {

Engine &eng() { return strata::test::engine(); }

uint32_t refused() { return eng().status().refused.load(std::memory_order_relaxed); }
uint8_t turns() { return eng().status().evolve_turns.load(std::memory_order_relaxed); }

const Step &step_at(int track, int step) { return eng().pattern().tracks[track].steps[step]; }

void post_full(Cmd cmd, uint8_t track, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint32_t u) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.a = a;
    command.b = b;
    command.c = c;
    command.d = d;
    command.u = u;
    eng().commands().post(command);
}

int velocity_sum(int track) {
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += step_at(track, i).velocity;
    return sum;
}

// Four trigs on track 0, a one-beat pass, EVOLVE every pass on velocity at full amount.
void evolving(EvolveMode mode) {
    fresh();
    for (int s = 0; s < 4; s++) put_trig(0, s);
    post(Cmd::set_master_length, 0, 0, 4);              // a pass is 96 ticks
    post(Cmd::evolve_config, 0, 0, uint8_t(mode), uint8_t(Mutation::velocity), 100);
    post(Cmd::evolve_set, 0, 0, 1);
    pump();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
}

}  // namespace

TEST(engine_gen_writes_a_part_and_counts_one_that_cannot) {
    fresh();
    const uint32_t start = refused();
    post_full(Cmd::generate, 0, 100, 1, 0, uint8_t(GenWhat::both), 7);
    pump();
    CHECK_EQ(refused(), start);
    int hits = 0;
    for (int i = 0; i < 16; i++) hits += (step_at(0, i).flags & kStepTrig) != 0;
    CHECK(hits >= 8);
    // NOTES on the kick: a drum part has no notes to write.
    post_full(Cmd::generate, 0, 100, 1, 0, uint8_t(GenWhat::notes), 8);
    pump();
    CHECK_EQ(refused(), start + 1);
}

TEST(engine_evolve_turns_on_the_pass_boundary) {
    evolving(EvolveMode::orbit);
    CHECK_EQ(int(eng().evolve_period(0)), 1);
    const uint8_t before = turns();
    const int home = velocity_sum(0);
    // The turn lands in the pass's last kEvolveLeadTicks (8) ticks, not before them.
    FakeHost::the().deliver_tick(87, 0);
    pump();
    CHECK_EQ(turns(), before);
    CHECK_EQ(velocity_sum(0), home);
    FakeHost::the().deliver_tick(88, 0);
    pump();
    CHECK_EQ(int(turns()), int(uint8_t(before + 1)));
    CHECK(velocity_sum(0) != home);

    // HOME puts it back and stops.
    post(Cmd::evolve_home, 0);
    pump();
    CHECK_EQ(velocity_sum(0), home);
    CHECK_EQ(int(eng().evolve_period(0)), 0);
    FakeHost::the().deliver_tick(192, 0);
    pump();
    CHECK_EQ(velocity_sum(0), home);
}

TEST(engine_evolve_keep_stops_with_what_plays) {
    evolving(EvolveMode::drift);
    FakeHost::the().deliver_tick(96, 0);
    pump();
    const int kept = velocity_sum(0);
    post(Cmd::evolve_keep, 0);
    pump();
    CHECK_EQ(int(eng().evolve_period(0)), 0);
    FakeHost::the().deliver_tick(192, 0);
    pump();
    CHECK_EQ(velocity_sum(0), kept);
}

TEST(engine_evolve_config_clamps_a_damaged_command) {
    fresh();
    post(Cmd::evolve_config, 0, 0, 9, 200, 250);
    pump();
    CHECK(eng().evolve_mode() == EvolveMode::orbit);
}

TEST(engine_evolve_follows_the_file_on_a_load) {
    // The settings are the project's (core/project.hpp): a change writes them, a load reads them.
    evolving(EvolveMode::drift);
    CHECK_EQ(int(eng().project().settings.evolve_period[0]), 1);
    CHECK_EQ(int(eng().project().settings.evolve_mode), int(EvolveMode::drift));
    CHECK_EQ(int(eng().project().settings.evolve_amount), 100);
    FakeHost::the().set_running(false);
    ProjectSettings &file = eng().project_for_load().settings;
    file.evolve_period[0] = 0;
    file.evolve_period[3] = 3;
    file.evolve_mode = uint8_t(EvolveMode::orbit);
    file.evolve_kind = uint8_t(Mutation::mix);
    file.evolve_amount = 40;
    const uint8_t loaded = eng().status().evolve_loaded.load(std::memory_order_relaxed);
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(int(eng().evolve_period(0)), 0);
    CHECK_EQ(int(eng().evolve_period(3)), 3);
    CHECK(eng().evolve_mode() == EvolveMode::orbit);
    CHECK(eng().evolve_kind() == Mutation::mix);
    CHECK_EQ(eng().evolve_amount(), 40);
    CHECK_EQ(int(eng().status().evolve_loaded.load(std::memory_order_relaxed)),
             int(uint8_t(loaded + 1)));
}

TEST(engine_evolve_kind_byte_is_the_mutation) {
    // core/project.hpp keeps the saved kind as a number, since it cannot include core/mutate.
    CHECK_EQ(int(kEvolveKindDefault), int(Mutation::velocity));
    CHECK_EQ(int(kEvolveKindMax), int(Mutation::mix));
    CHECK_EQ(int(kEvolveKindMax) + 1, int(Mutation::count));
    CHECK_EQ(int(kEvolvePeriodMax) + 1, kEvolvePeriods);
}

TEST(engine_evolve_orbit_leaves_a_pattern_home_and_follows_to_the_next) {
    evolving(EvolveMode::orbit);
    const int home = velocity_sum(0);
    FakeHost::the().deliver_tick(96, 0);
    pump();
    CHECK(velocity_sum(0) != home);
    // Pattern 2 now: pattern 1 goes back to HOME, and track 0 evolves on pattern 2.
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::immediate));
    pump();
    CHECK_EQ(int(eng().project().slot), 1);
    CHECK_EQ(int(eng().evolve_period(0)), 1);
    const Pattern &first = eng().project().patterns[0][0];
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += first.tracks[0].steps[i].velocity;
    CHECK_EQ(sum, home);
}

TEST(engine_evolve_turns_eight_tracks_one_a_tick) {
    // Eight tracks due on the same pass turn on eight different ticks before the boundary, so no
    // audio block carries more than one turn; the boundary then counts nothing twice.
    fresh();
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < 4; s++) put_trig(t, s);
    }
    post(Cmd::set_master_length, 0, 0, 4);
    post(Cmd::evolve_config, 0, 0, uint8_t(EvolveMode::orbit), uint8_t(Mutation::velocity), 100);
    for (int t = 0; t < kTracks; t++) post(Cmd::evolve_set, uint8_t(t), 0, 1);
    pump();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    uint8_t last = turns();
    for (uint32_t tick = 88; tick < 96; tick++) {
        FakeHost::the().deliver_tick(tick, 0);
        pump();
        CHECK_EQ(int(turns()), int(uint8_t(last + 1)));   // one track this tick
        last = turns();
    }
    FakeHost::the().deliver_tick(96, 0);
    pump();
    CHECK_EQ(turns(), last);                               // all turned already
    // And the next pass turns them all again.
    for (uint32_t tick = 97; tick < 192; tick++) {
        FakeHost::the().deliver_tick(tick, 0);
        pump();
    }
    CHECK_EQ(int(turns()), int(uint8_t(last + 8)));
}
