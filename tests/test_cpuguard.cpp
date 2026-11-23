// Tests for the CPU guard's decision (core/cpuguard).
//
// The guard is arithmetic on a stream of per-mille figures, so these feed it streams: steady,
// alternating, a single spike, and the edges of every threshold and every wait. What matters on
// the unit is that it does NOT move on an ordinary beat (a guard that sheds on the default kit is
// a bug that sounds like a thin chord), that it does move before a sustained overload becomes an
// overrun, and that it comes down slowly enough not to pump.
#include "check.hpp"

#include "cpuguard.hpp"

using namespace strata;

namespace {

// Feed `blocks` blocks of `load`; returns the level after the last.
int feed(CpuGuard &g, uint32_t load, uint32_t blocks) {
    int level = g.level();
    for (uint32_t i = 0; i < blocks; i++) level = g.update(load);
    return level;
}

}  // namespace

TEST(cpuguard_starts_at_level_zero_and_stays_there_under_an_ordinary_load) {
    CpuGuard g;
    CHECK_EQ(g.level(), 0);
    // The unit's default beat reads under 30%; a busy one well under kHigh never rises, for a
    // minute of blocks.
    CHECK_EQ(feed(g, 300, 750 * 60), 0);
    CHECK_EQ(feed(g, CpuGuard::kHigh - 1, 750 * 10), 0);
}

TEST(cpuguard_a_steady_overload_rises_one_level_per_settle_time) {
    CpuGuard g;
    // At kHigh exactly the average reaches it (from below, a few blocks late) and the guard rises
    // once the settle time since the start has passed -- and not before.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks - 1), 0);
    CHECK_EQ(feed(g, 800, 1), 1);
    // The next step waits a whole settle time again: what level 1 shed has to go quiet first.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks - 1), 1);
    CHECK_EQ(feed(g, 800, 1), 2);
    // And it tops out.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks * 10), CpuGuard::kMaxLevel);
}

TEST(cpuguard_one_heavy_block_under_panic_does_not_rise) {
    CpuGuard g;
    feed(g, 300, 1000);
    // A single block at 89.9%: the average moves an eighth of the way, nowhere near kHigh.
    CHECK_EQ(g.update(CpuGuard::kPanic - 1), 0);
    CHECK_EQ(feed(g, 300, 1000), 0);
}

TEST(cpuguard_a_panic_block_rises_at_once_after_a_short_settle) {
    CpuGuard g;
    feed(g, 300, 1000);
    CHECK_EQ(g.update(CpuGuard::kPanic), 1);         // one block at 90%: up at once
    // Another panic block straight after waits out kPanicSettle, so a burst of them cannot take
    // the guard to the top before the first step has had any effect.
    CHECK_EQ(feed(g, CpuGuard::kPanic, CpuGuard::kPanicSettle - 1), 1);
    CHECK_EQ(g.update(CpuGuard::kPanic), 2);
}

TEST(cpuguard_the_average_rides_out_a_beat_that_alternates_heavy_and_light_blocks) {
    // Trig blocks are expensive and the blocks between them cheap. 95% / 30% alternating averages
    // ~62% -- under kHigh -- but every other block is a panic block, so it rises on those: a
    // block that close to an overrun is a real risk whatever the average says.
    CpuGuard g;
    int level = 0;
    for (int i = 0; i < 100; i++) level = g.update(i % 2 ? 950u : 300u);
    CHECK(level >= 1);

    // 85% / 30% never panics, averages under kHigh, and so never rises.
    CpuGuard h;
    for (int i = 0; i < 750 * 10; i++) level = h.update(i % 2 ? 850u : 300u);
    CHECK_EQ(level, 0);
    CHECK(h.average() < CpuGuard::kHigh);
    CHECK(h.average() > 500u);
}

TEST(cpuguard_falls_one_level_per_second_of_cool_and_no_sooner) {
    CpuGuard g;
    feed(g, 800, CpuGuard::kSettleBlocks * 10);
    CHECK_EQ(g.level(), CpuGuard::kMaxLevel);

    // The average needs a few blocks to fall under kLow; the block it does is the first of the
    // second that has to pass.
    int blocks = 0;
    while (g.average() >= CpuGuard::kLow) {
        g.update(100);
        blocks++;
    }
    CHECK(blocks > 1 && blocks < 10);
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks - 2), CpuGuard::kMaxLevel);
    CHECK_EQ(feed(g, 100, 1), CpuGuard::kMaxLevel - 1);
    // Each later step is another whole second.
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks - 1), CpuGuard::kMaxLevel - 1);
    CHECK_EQ(feed(g, 100, 1), CpuGuard::kMaxLevel - 2);
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks * 10), 0);
}

TEST(cpuguard_a_load_between_low_and_high_holds_the_level) {
    CpuGuard g;
    feed(g, 800, CpuGuard::kSettleBlocks);
    CHECK_EQ(g.level(), 1);
    // 60%: shedding brought it under kHigh but not under kLow. Neither up nor down.
    CHECK_EQ(feed(g, 600, 750 * 20), 1);
}

TEST(cpuguard_a_hot_block_restarts_the_cool_count) {
    CpuGuard g;
    feed(g, 800, CpuGuard::kSettleBlocks);
    CHECK_EQ(g.level(), 1);
    feed(g, 100, CpuGuard::kFallBlocks - 10);
    // A burst that lifts the average over kLow resets the second; it does not just pause it.
    feed(g, CpuGuard::kPanic - 1, 7);
    CHECK(g.average() >= CpuGuard::kLow);
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks - 1), 1);
    CHECK_EQ(feed(g, 100, 40), 0);
}

TEST(cpuguard_reset_forgets_the_level_and_the_history) {
    CpuGuard g;
    feed(g, 800, CpuGuard::kSettleBlocks * 3);
    CHECK(g.level() > 0);
    g.reset();
    CHECK_EQ(g.level(), 0);
    CHECK_EQ(g.average(), 0u);
    // And the settle time starts again from the reset.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks - 1), 0);
}

TEST(cpuguard_an_absurd_figure_is_clamped_not_wrapped) {
    CpuGuard g;
    for (int i = 0; i < 2000; i++) g.update(0xFFFFFFFFu);
    CHECK(g.average() <= 2000u);
    CHECK_EQ(g.level(), CpuGuard::kMaxLevel);
    // Back to zero from there, in the same time as from any other level.
    CHECK_EQ(feed(g, 0, CpuGuard::kFallBlocks * 5), 0);
}

TEST(cpuguard_the_level_tables) {
    // Level 0 is the full allowance, whatever the full allowance is.
    CHECK_EQ(CpuGuard::poly_budget(0, 12), 12);
    CHECK_EQ(CpuGuard::grain_limit(0, 8), 8);
    // Two POLY voices a level, never under four.
    CHECK_EQ(CpuGuard::poly_budget(1, 12), 10);
    CHECK_EQ(CpuGuard::poly_budget(2, 12), 8);
    CHECK_EQ(CpuGuard::poly_budget(3, 12), 6);
    CHECK_EQ(CpuGuard::poly_budget(4, 12), 4);
    CHECK_EQ(CpuGuard::poly_budget(9, 12), 4);
    CHECK_EQ(CpuGuard::poly_budget(4, 6), 4);
    CHECK_EQ(CpuGuard::poly_budget(-1, 12), 12);
    // GRAIN's pool: 8, 6, 4, 3, 2, and never more than the pool it was given.
    CHECK_EQ(CpuGuard::grain_limit(1, 8), 6);
    CHECK_EQ(CpuGuard::grain_limit(2, 8), 4);
    CHECK_EQ(CpuGuard::grain_limit(3, 8), 3);
    CHECK_EQ(CpuGuard::grain_limit(4, 8), 2);
    CHECK_EQ(CpuGuard::grain_limit(9, 8), 2);
    CHECK_EQ(CpuGuard::grain_limit(1, 5), 5);
    // Monotonic: a higher level never allows more.
    for (int l = 1; l <= CpuGuard::kMaxLevel; l++) {
        CHECK(CpuGuard::poly_budget(l, 12) <= CpuGuard::poly_budget(l - 1, 12));
        CHECK(CpuGuard::grain_limit(l, 8) <= CpuGuard::grain_limit(l - 1, 8));
    }
}
