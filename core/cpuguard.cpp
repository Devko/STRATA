#include "cpuguard.hpp"

namespace strata {

int CpuGuard::update(uint32_t load) {
    // A figure past the whole block is an overrun already; clamp so the average cannot wrap.
    if (load > 2000u) load = 2000u;

    // average += (load - average) / 64, in Q8 and without a divide. Unsigned, so the two
    // directions are written out rather than letting a negative difference wrap.
    const uint32_t target = load << 8;
    if (target >= average_q8_) {
        average_q8_ += (target - average_q8_) >> kAverageShift;
    } else {
        average_q8_ -= (average_q8_ - target) >> kAverageShift;
    }
    if (since_change_ < 0xFFFFu) since_change_++;

    const uint32_t avg = average();
    const bool panic = load >= kPanic && since_change_ >= kPanicSettle;
    const bool hot = avg >= kHigh && since_change_ >= kSettleBlocks;
    if ((panic || hot) && level_ < kMaxLevel) {
        level_++;
        since_change_ = 0;
        cool_ = 0;
        return level_;
    }

    if (avg < kLow) {
        cool_++;
        if (cool_ >= kFallBlocks && level_ > 0) {
            level_--;
            since_change_ = 0;
            cool_ = 0;
        }
    } else if (cool_ > 0) {
        cool_--;            // a hot block takes one back; it does not start the second again
    }
    return level_;
}

void CpuGuard::reset() {
    average_q8_ = 0;
    since_change_ = 0;
    cool_ = 0;
    level_ = 0;
}

int CpuGuard::poly_budget(int level, int full) {
    if (level <= 0) return full;
    if (level > kMaxLevel) level = kMaxLevel;
    const int budget = full - 2 * level;
    return budget < 4 ? 4 : budget;
}

int CpuGuard::grain_limit(int level, int full) {
    static constexpr int kLimit[kMaxLevel + 1] = {8, 6, 4, 3, 2};
    if (level <= 0) return full;
    if (level > kMaxLevel) level = kMaxLevel;
    return kLimit[level] < full ? kLimit[level] : full;
}

}  // namespace strata
