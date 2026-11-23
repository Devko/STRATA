// The CPU guard: sheds the costliest sustaining work when the audio block nears its budget.
//
// docs/BUDGET.md puts the default beat under 30% on the unit, but long free play with big effects
// held a MAX of 84.3% -- past the 60% target and with nothing to stop the next chord taking it
// past 100%, which is an overrun and a click. Two things can grow without bound by playing:
// VA POLY voices (2.16 points each with cl6x, twelve allowed) and GRAIN clouds (+9.2 points a
// dense track, GCC-only). This is the safety net for both. It does not make the budget: the
// static rules in CLAUDE.md still do. It only decides HOW HARD to shed, as a level 0-4; the
// engine turns a level into a smaller POLY voice budget and a smaller grain pool.
//
// ## What it reads
//
// enaga::audio_load(), called at the top of the audio callback, is the PREVIOUS block's cost in
// per mille of its real-time budget (Enaga's own unit check app reads it the same way, from
// audio()). Blocks are 64 frames at 48 kHz, so 750 a second. core/ never includes Enaga: the
// engine reads the figure and hands it here.
//
// ## How it decides
//
// One block's cost jumps with where the step trigs land, so a level on one block's figure would
// flap. The guard keeps a short running average (a one-pole over about 8 blocks, 11 ms) and:
//
//  - RISES one level when the average reaches kHigh, at most once per kSettleBlocks (100 ms),
//    because what it sheds takes time to go quiet -- a choke is a 64-frame fade but a grain
//    lives out its window -- and rising again before the first step has landed overshoots.
//  - RISES at once on a single block at kPanic or more (at most once per kPanicSettle blocks):
//    one block that close to 100% is one chord from an overrun.
//  - FALLS one level after kFallBlocks (1 s) in a row with the average under kLow. Slow on
//    purpose: a level that drops the moment the load does would let the same chord straight
//    back in, and the user would hear the voice count pump.
//
// Integer arithmetic only, no division (the C674x has no integer divide): the average is held
// in Q8 and moved by a shift.
#pragma once

#include <cstdint>

namespace strata {

class CpuGuard {
public:
    static constexpr int kMaxLevel = 4;

    // Per mille of the block. Provisional, like Engine::kPolyVoiceBudget: set from the unit's
    // MAX readings (docs/UNIT_CHECKLIST.md, "The CPU guard"). The unit has held 84.3% with no
    // dropout, so kHigh leaves room above the 60% target and kPanic below an overrun.
    static constexpr uint32_t kHigh = 700;
    static constexpr uint32_t kLow = 550;
    static constexpr uint32_t kPanic = 900;

    static constexpr uint32_t kSettleBlocks = 75;    // 100 ms between ordinary rises
    static constexpr uint32_t kPanicSettle = 8;      // 11 ms between rises on a panic block
    static constexpr uint32_t kFallBlocks = 750;     // 1 s cool before each step down
    static constexpr int kAverageShift = 3;          // one-pole over ~8 blocks

    // One block's load, per mille; returns the level to apply to the next one.
    int update(uint32_t load_per_mille);

    int level() const { return level_; }
    // The running average, per mille. For tests and for the screen.
    uint32_t average() const { return average_q8_ >> 8; }

    // Back to level 0 with no history: a hard stop silences everything, so what was measured
    // before it says nothing about what comes after.
    void reset();

    // What a level means. Both tables are indexed by level and clamp it; level 0 is the full
    // allowance, so a guard that never rises changes nothing.
    //
    // POLY: the shared voice budget, two voices a level (each ~2.2 points with cl6x, ~3 on the
    // unit if it runs 1.35x the emulator). GRAIN: the grains one track may hold in the air.
    // GRAIN keeps at least two so a cloud stays a cloud rather than a stutter.
    static int poly_budget(int level, int full);
    static int grain_limit(int level, int full);

private:
    uint32_t average_q8_ = 0;   // per mille, x256
    uint32_t since_change_ = 0; // blocks since the level last moved, saturating
    uint32_t cool_ = 0;         // blocks in a row with the average under kLow
    int level_ = 0;
};

}  // namespace strata
