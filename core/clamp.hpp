// The integer clamps core/ uses everywhere: one definition instead of a private copy per file.
//
// Each of these was written out again, identically, in nine .cpp files as features landed in
// parallel. Plain functions rather than a template so a call with mixed argument types (a
// uint8_t value against int limits, say) converts exactly as it did when each file had its own.
#pragma once

#include <cstdint>

namespace strata {

constexpr int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

constexpr uint8_t clamp_u8(uint8_t value, uint8_t low, uint8_t high) {
    return value < low ? low : (value > high ? high : value);
}

constexpr uint16_t clamp_u16(uint16_t value, uint16_t low, uint16_t high) {
    return value < low ? low : (value > high ? high : value);
}

}  // namespace strata
