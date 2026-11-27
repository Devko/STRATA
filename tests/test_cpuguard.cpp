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
    // From silence the 85 ms average needs a while to reach kHigh: no rise while it is under,
    // and the rise comes on the block it gets there (the settle time has long passed by then).
    int blocks = 0;
    while (g.level() == 0 && blocks < 1000) {
        g.update(800);
        blocks++;
        if (g.level() == 0) CHECK(g.average() < CpuGuard::kHigh);
    }
    CHECK_EQ(g.level(), 1);
    CHECK(g.average() >= CpuGuard::kHigh);
    CHECK(blocks > int(CpuGuard::kSettleBlocks) && blocks < 200);
    // The next step waits a whole settle time: what level 1 shed has to go quiet first.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks - 1), 1);
    CHECK_EQ(feed(g, 800, 1), 2);
    // And it tops out.
    CHECK_EQ(feed(g, 800, CpuGuard::kSettleBlocks * 10), CpuGuard::kMaxLevel);
}

TEST(cpuguard_one_heavy_block_under_panic_does_not_rise) {
    CpuGuard g;
    feed(g, 300, 1000);
    // A single block at 89.9%: the average moves a sixty-fourth of the way, nowhere near kHigh.
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
    CHECK(blocks > 1 && blocks < 100);
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks - 2), CpuGuard::kMaxLevel);
    CHECK_EQ(feed(g, 100, 1), CpuGuard::kMaxLevel - 1);
    // Each later step is another whole second.
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks - 1), CpuGuard::kMaxLevel - 1);
    CHECK_EQ(feed(g, 100, 1), CpuGuard::kMaxLevel - 2);
    CHECK_EQ(feed(g, 100, CpuGuard::kFallBlocks * 10), 0);
}

TEST(cpuguard_a_load_between_low_and_high_holds_the_level) {
    CpuGuard g;
    feed(g, 800, CpuGuard::kSettleBlocks * 2);   // up once, not yet twice
    CHECK_EQ(g.level(), 1);
    // 60%: shedding brought it under kHigh but not under kLow. Neither up nor down.
    CHECK_EQ(feed(g, 600, 750 * 20), 1);
}

TEST(cpuguard_a_hot_burst_takes_blocks_off_the_cool_count_rather_than_restarting_it) {
    CpuGuard g;
    while (g.level() == 0) g.update(800);
    while (g.average() >= CpuGuard::kLow) g.update(100);
    feed(g, 100, CpuGuard::kFallBlocks - 200);
    // A burst that lifts the average over kLow: each block of it over kLow takes one block back.
    int hot = 0;
    for (int i = 0; i < 80; i++) {
        g.update(CpuGuard::kPanic - 1);
        if (g.average() >= CpuGuard::kLow) hot++;
    }
    CHECK(hot > 0);
    int cooled = 0;
    while (g.average() >= CpuGuard::kLow) {
        g.update(100);
        hot++;
    }
    // The second needs about the 200 blocks it was short, give or take what the burst took
    // back and the blocks under kLow at either end of it -- not a whole new second.
    CHECK_EQ(g.level(), 1);
    while (g.level() == 1 && cooled < int(CpuGuard::kFallBlocks)) {
        g.update(100);
        cooled++;
    }
    CHECK_EQ(g.level(), 0);
    CHECK(cooled < 200 + hot + 10);
    CHECK(cooled < int(CpuGuard::kFallBlocks) / 2);
}

// One block's load, in percent at the UNIT's speed, for 2 s of the emulator's default kit after
// GENERATE at DENSITY 100, RANGE 3, LEAP 100 on all eight tracks (cl6x; the emulator's
// audio_cycles_last read every block, times 2.4: Roland's unit showed 40-60% on that kind of
// playing where the emulator reads a mean of 20%). The 1.5 s with the most load in an 8 s run.
const uint8_t kGenTrace[] = {
    69, 72, 68, 68, 73, 68, 67, 68, 73, 67, 67, 72, 68, 68, 67, 72, 68, 67, 67, 72, 68, 67, 72, 67, 68,
    67, 72, 67, 68, 72, 68, 68, 69, 72, 68, 68, 66, 70, 65, 65, 69, 64, 64, 56, 62, 56, 53, 53, 59, 53,
    53, 58, 54, 53, 53, 58, 54, 53, 58, 52, 53, 52, 56, 52, 53, 52, 56, 52, 53, 56, 52, 52, 53, 56, 52,
    52, 57, 52, 52, 52, 57, 52, 52, 52, 59, 51, 51, 56, 52, 51, 51, 63, 56, 55, 59, 55, 56, 55, 59, 54,
    55, 54, 58, 54, 55, 58, 54, 54, 55, 58, 54, 54, 59, 54, 54, 54, 59, 54, 53, 49, 55, 49, 49, 54, 50,
    49, 47, 45, 42, 38, 43, 38, 39, 38, 43, 38, 39, 38, 43, 38, 39, 43, 38, 38, 39, 43, 38, 38, 44, 38,
    38, 38, 44, 38, 38, 38, 44, 38, 38, 43, 39, 38, 38, 43, 39, 38, 38, 42, 39, 38, 64, 60, 62, 60, 64,
    60, 61, 64, 60, 60, 61, 64, 60, 60, 61, 64, 60, 60, 65, 58, 56, 57, 62, 56, 56, 61, 57, 56, 56, 61,
    57, 56, 57, 61, 58, 57, 62, 57, 58, 55, 59, 55, 56, 60, 54, 54, 48, 52, 47, 44, 45, 49, 44, 42, 48,
    42, 42, 43, 48, 42, 42, 47, 43, 42, 42, 47, 43, 42, 42, 47, 43, 42, 47, 42, 43, 42, 47, 42, 43, 47,
    42, 42, 43, 46, 42, 42, 43, 46, 42, 42, 48, 42, 42, 42, 59, 51, 50, 55, 50, 47, 47, 52, 49, 47, 47,
    52, 48, 47, 52, 48, 48, 47, 52, 47, 48, 52, 47, 47, 48, 52, 47, 47, 45, 49, 44, 44, 51, 45, 45, 45,
    51, 45, 43, 43, 48, 43, 43, 47, 44, 43, 43, 47, 44, 40, 40, 36, 37, 36, 40, 36, 37, 36, 40, 36, 37,
    40, 36, 36, 37, 40, 36, 36, 41, 36, 36, 36, 41, 36, 36, 36, 41, 36, 36, 60, 62, 60, 60, 65, 62, 61,
    66, 61, 61, 60, 65, 60, 61, 60, 65, 61, 61, 65, 60, 60, 61, 65, 60, 60, 65, 59, 60, 60, 65, 60, 60,
    60, 66, 60, 60, 65, 61, 60, 58, 62, 59, 58, 63, 58, 59, 50, 55, 50, 48, 46, 49, 44, 43, 47, 42, 42,
    43, 47, 42, 42, 48, 42, 42, 42, 48, 42, 42, 42, 48, 42, 42, 47, 43, 42, 42, 47, 43, 42, 42, 47, 43,
    42, 46, 42, 43, 42, 46, 42, 43, 47, 42, 42, 43, 57, 52, 52, 53, 56, 48, 48, 53, 48, 48, 48, 53, 48,
    48, 52, 49, 48, 48, 52, 49, 48, 48, 52, 46, 43, 48, 43, 44, 39, 44, 39, 40, 44, 39, 39, 40, 44, 39,
    37, 38, 41, 37, 37, 42, 37, 29, 29, 35, 29, 29, 34, 30, 29, 29, 34, 30, 29, 29, 34, 30, 29, 34, 29,
    30, 29, 34, 29, 30, 34, 29, 29, 30, 34, 29, 29, 30, 34, 29, 29, 55, 58, 58, 57, 62, 57, 57, 61, 59,
    57, 56, 61, 57, 57, 56, 61, 58, 56, 61, 56, 57, 56, 61, 56, 58, 56, 61, 56, 58, 61, 56, 56, 57, 62,
    57, 57, 63, 57, 57, 55, 60, 55, 55, 55, 60, 55, 47, 52, 48, 44, 44, 49, 45, 43, 47, 43, 44, 43, 47,
    43, 44, 43, 47, 43, 44, 47, 43, 43, 44, 47, 43, 43, 48, 43, 43, 43, 48, 43, 43, 43, 48, 43, 43, 47,
    44, 43, 43, 47, 43, 42, 47, 42, 43, 42, 51, 43, 44, 43, 48, 43, 44, 48, 43, 43, 44, 48, 43, 43, 49,
    43, 43, 43, 49, 43, 43, 43, 49, 43, 43, 48, 44, 43, 43, 44, 39, 38, 43, 38, 39, 38, 43, 38, 39, 38,
    43, 38, 39, 44, 38, 38, 39, 43, 38, 38, 41, 34, 34, 34, 39, 34, 34, 34, 39, 34, 34, 38, 35, 34, 34,
    38, 35, 34, 34, 38, 35, 34, 38, 34, 34, 33, 38, 33, 34, 55, 61, 61, 61, 64, 60, 60, 61, 65, 60, 59,
    65, 59, 59, 59, 65, 60, 60, 64, 60, 56, 56, 61, 57, 57, 56, 61, 57, 57, 61, 56, 57, 56, 61, 57, 58,
    62, 57, 57, 56, 59, 55, 55, 56, 59, 55, 47, 53, 47, 44, 44, 50, 44, 43, 47, 44, 43, 43, 47, 44, 43,
    43, 47, 44, 43, 47, 43, 44, 43, 47, 43, 44, 47, 43, 43, 44, 47, 43, 43, 44, 47, 43, 43, 48, 43, 43,
    43, 48, 43, 43, 47, 44, 43, 43, 60, 59, 58, 58, 62, 58, 58, 62, 58, 58, 58, 62, 58, 58, 57, 62, 58,
    58, 62, 57, 58, 59, 62, 56, 53, 59, 53, 53, 53, 56, 50, 51, 51, 57, 51, 51, 56, 52, 51, 51, 56, 52,
    51, 56, 51, 52, 51, 56, 51, 49, 48, 53, 48, 49, 53, 48, 48, 49, 53, 48, 48, 54, 48, 48, 48, 54, 48,
    48, 48, 54, 48, 48, 53, 49, 48, 48, 53, 49, 48, 69, 73, 73, 73, 77, 72, 73, 72, 79, 72, 73, 76, 72,
    72, 73, 76, 72, 72, 78, 72, 69, 69, 74, 68, 68, 68, 74, 68, 68, 73, 69, 68, 68, 73, 69, 69, 73, 68,
    70, 66, 71, 66, 67, 66, 71, 66, 59, 63, 58, 56, 54, 49, 44, 42, 43, 47, 42, 43, 48, 42, 42, 42, 48,
    42, 42, 47, 43, 42, 42, 47, 43, 42, 42, 47, 43, 42, 47, 42, 43, 42, 47, 42, 43, 46, 42, 42, 43, 46,
    42, 42, 43, 47, 42, 42, 51, 47, 46, 46, 52, 42, 42, 46, 43, 42, 42, 46, 43, 42, 42, 46, 43, 42, 46,
    42, 43, 42, 46, 42, 43, 46, 42, 42, 43, 45, 40, 40, 41, 45, 41, 41, 46, 41, 41, 41, 46, 41, 41, 45,
    42, 41, 41, 45, 42, 38, 35, 38, 35, 34, 38, 34, 35, 34, 38, 34, 35, 35, 31, 31, 32, 35, 31, 31, 32,
    35, 31, 31, 36, 31, 31, 31, 36, 31, 31, 49, 56, 54, 54, 59, 55, 54, 54, 59, 55, 54, 58, 54, 55, 54,
    58, 54, 55, 54, 58, 54, 55, 58, 54, 54, 55, 58, 54, 54, 59, 54, 54, 54, 60, 54, 54, 54, 60, 54, 52,
    56, 53, 52, 50, 55, 51, 43, 47, 43, 41, 40, 44, 40, 41, 40, 44, 40, 41, 44, 40, 40, 41, 44, 40, 40,
    45, 40, 40, 40, 45, 40, 40, 40, 45, 40, 40, 44, 41, 40, 40, 44, 41, 40, 44, 40, 41, 40, 45, 40, 41,
    40, 44, 40, 41, 53, 50, 50, 51, 55, 50, 50, 56, 50, 49, 49, 55, 49, 49, 49, 55, 50, 49, 54, 50, 49,
    49, 54, 50, 49, 54, 49, 50, 49, 50, 45, 46, 45, 50, 45, 46, 46, 41, 41, 40, 43, 39, 39, 40, 43, 39,
    31, 37, 31, 31, 29, 32, 27, 27, 31, 28, 27, 27, 31, 28, 27, 27, 31, 28, 27, 31, 27, 28, 27, 31, 27,
    28, 31, 27, 27, 28, 31, 27, 27, 28, 69, 56, 56, 61, 55, 56, 55, 61, 56, 55, 59, 56, 55, 55, 59, 57,
    55, 55, 59, 56, 55, 59, 55, 56, 55, 59, 55, 56, 59, 55, 55, 56, 60, 55, 55, 56, 60, 56, 53, 59, 53,
    53, 53, 59, 53, 45, 50, 46, 43, 43, 47, 44, 43, 43, 47, 44, 43, 47, 43, 44, 43, 47, 43, 44, 47, 43,
    43, 44, 47, 43, 43, 44, 47, 43, 43, 48, 43, 43, 43, 48, 43, 43, 43, 48, 43, 43, 48, 44, 43, 43, 47,
    44, 43, 52, 49, 49, 48, 53, 48, 49, 48, 53, 48, 49, 52, 48, 48, 49, 52, 48, 48, 53, 48, 48, 48, 53,
    48, 48, 48, 53, 48, 48, 51, 47, 46, 46, 51, 47, 46, 49, 42, 43, 42, 47, 42, 43, 42, 47, 42, 43, 47,
    42, 40, 39, 40, 35, 35, 41, 35, 35, 35, 41, 35, 35, 34, 39, 34, 34, 38, 35, 34, 34, 38, 35, 34, 38,
    34, 35, 34, 38, 34, 35, 34, 80, 68, 69, 72, 68, 68, 69, 72, 68, 67, 73, 67, 67, 67, 73, 68, 67, 67,
    73, 67, 67, 72, 68, 67, 67, 72, 68, 67, 72, 67, 68, 68, 72, 68, 69, 68, 72, 68, 66, 70, 65, 65, 66,
    68, 64, 56, 57, 61, 56, 53, 59, 53, 53, 53, 59, 53, 53, 58, 54, 53, 53, 58, 53, 52, 52, 56, 53, 52,
    56, 52, 53, 52, 56, 52, 53, 56, 52, 52, 53, 56, 52, 52, 53, 56, 52, 52, 59, 51, 51, 51, 57, 51, 51,
    62, 56, 55, 55, 59, 56, 55, 55, 59, 55, 54, 58, 54, 55, 54, 58, 54, 55, 58, 54, 54, 55, 58, 54, 54,
};
constexpr int kGenTraceBlocks = int(sizeof(kGenTrace));

TEST(cpuguard_a_generated_beat_at_the_units_speed_does_not_raise_the_guard) {
    // Mean 49%, plateaus to 80% over some 40 blocks a step, no block near an overrun: there is
    // nothing to shed, and for 6 s of it the guard sheds nothing. (Averaged over 8 blocks it
    // rose to GUARD4 on this trace: the bug.)
    static_assert(kGenTraceBlocks == 1500, "the trace above");
    CpuGuard g;
    uint32_t peak = 0;
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 0; i < kGenTraceBlocks; i++) {
            const uint32_t load = uint32_t(kGenTrace[i]) * 10u;
            peak = load > peak ? load : peak;
            CHECK_EQ(g.update(load), 0);
        }
    }
    CHECK(peak >= CpuGuard::kHigh);       // the plateaus do reach where the old average tripped
    CHECK(peak < CpuGuard::kPanic);
}

TEST(cpuguard_the_same_beat_with_blocks_near_an_overrun_still_rises) {
    // The same trace a sixth heavier reaches 93%: those blocks are a chord from an overrun, and
    // the guard answers them. Then the beat goes back to the lighter figures and the guard comes
    // back down to 0.
    CpuGuard g;
    for (int i = 0; i < kGenTraceBlocks; i++) g.update(uint32_t(kGenTrace[i]) * 70u / 6u);
    CHECK(g.level() >= 1);
    for (int pass = 0; pass < 8 && g.level() > 0; pass++) {
        for (int i = 0; i < kGenTraceBlocks; i++) g.update(uint32_t(kGenTrace[i]) * 10u);
    }
    CHECK_EQ(g.level(), 0);
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
