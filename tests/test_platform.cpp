// Guards the platform constants STRATA's design arithmetic rests on.
//
// Enaga's ABI is "provisional until 1.0" and this repo pins a commit, so the top-ranked risk in
// docs/SPEC.md is the platform moving under us. These assertions are the tripwire: if a submodule
// bump changes a constant the budget or the sequencer depends on, the host tests fail immediately
// instead of the change surfacing as a timing bug on the unit.
#include "check.hpp"

#include <enaga/abi.h>

namespace {

// --- platform constants, checked at compile time --------------------------

static_assert(ENAGA_BLOCK_FRAMES == 64u,
              "the whole DSP design assumes 64-frame blocks (1.33 ms)");
static_assert(ENAGA_SAMPLE_RATE == 48000u,
              "48 kHz is assumed by every tempo, delay and envelope calculation");
static_assert(ENAGA_CLOCK_PPQN == 96u,
              "96 PPQN is the step grid; micro-timing is +/-11 of these ticks");

// The C674x runs at 372 MHz, so one 64-frame block is 496,000 cycles. Every figure in
// docs/BUDGET.md is a share of this, so it is written down once, here.
constexpr unsigned kCpuHz = 372'000'000u;
constexpr unsigned kCyclesPerBlock = kCpuHz / (ENAGA_SAMPLE_RATE / ENAGA_BLOCK_FRAMES);
static_assert(kCyclesPerBlock == 496'000u, "the cycle budget per block changed");

// 24 ticks to a 16th note, which is what bounds micro-timing at +/-11.
static_assert(ENAGA_CLOCK_PPQN / 4u == 24u, "a 16th note is no longer 24 ticks");

// --- measured per-voice costs (docs/REVIEW.md 1.5) ------------------------
// Measured on a unit with caches on. Kept here so the budget maths is testable rather than
// living only in prose, and so the 8-track worst case is recomputed on every test run.

constexpr unsigned kCyclesComputedVoice = 1868;   // SYNTH-class full VA voice
constexpr unsigned kCyclesGrainVoice = 2690;      // reads DDR

unsigned permille_of_block(unsigned cycles) {
    return (cycles * 1000u + kCyclesPerBlock / 2u) / kCyclesPerBlock;
}

}  // namespace

TEST(framework_reports_passing_checks) {
    CHECK(true);
    CHECK_EQ(2 + 2, 4);
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
}

TEST(voice_budget_matches_the_spec) {
    // 17 computed voices (5 drum/perc tracks of 2, BASS 1, CHORD 6) plus 8 grains.
    const unsigned computed = (5u * 2u + 1u + 6u) * kCyclesComputedVoice;
    const unsigned grains = 8u * kCyclesGrainVoice;
    CHECK_EQ(computed, 31756u);
    CHECK_EQ(grains, 21520u);
    CHECK_EQ(computed + grains, 53276u);

    // About 10.7% of the block: the correction that reversed the spec's cut-voices-first advice.
    CHECK_EQ(permille_of_block(computed + grains), 107u);
}

TEST(cutting_chord_voices_is_not_a_lever) {
    // The original spec claimed 6 -> 4 chord voices saves about 5%. It saves under 1%.
    const unsigned saved = 2u * kCyclesComputedVoice;
    CHECK_EQ(saved, 3736u);
    CHECK(permille_of_block(saved) < 10u);   // i.e. under 1.0%
}

TEST(a_grain_voice_costs_more_than_a_computed_one) {
    // Reading samples from DDR costs about 40% more than computing, which is why the LOOP
    // track rather than the CHORD track dominates the voice budget.
    CHECK(kCyclesGrainVoice > kCyclesComputedVoice);
}
