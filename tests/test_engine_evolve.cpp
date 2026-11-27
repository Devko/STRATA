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
    FakeHost::the().deliver_tick(95, 0);
    pump();
    CHECK_EQ(turns(), before);
    CHECK_EQ(velocity_sum(0), home);
    FakeHost::the().deliver_tick(96, 0);
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

TEST(engine_evolve_stops_on_a_load) {
    evolving(EvolveMode::orbit);
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(int(eng().evolve_period(0)), 0);
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
