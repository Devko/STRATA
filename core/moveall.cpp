#include "moveall.hpp"

#include "project.hpp"   // MachineId, machine_reads_asr

namespace strata {

namespace {

// 2^(k / 32) in Q16, k = 0 .. 31.
constexpr uint32_t kOctaveQ16[32] = {
    65536,  66971,  68438,  69936,  71468,  73032,  74632,  76266,
    77936,  79642,  81386,  83169,  84990,  86851,  88752,  90696,
    92682,  94711,  96785,  98905,  101070, 103283, 105545, 107856,
    110218, 112631, 115098, 117618, 120194, 122825, 125515, 128263,
};

// Where the parameter pages start, as dsp/track.hpp numbers them. core/ cannot include it, so
// the two numbers are stated here; tests/test_moveall.cpp holds them to dsp's.
constexpr uint8_t kAmpFirst = 8;
constexpr uint8_t kAsrRel = 26;

}  // namespace

int8_t all_turn(int8_t current, int clicks) {
    const int from = current < -kAllRange ? -kAllRange : (current > kAllRange ? kAllRange : current);
    int to = from + clicks * kAllStep;
    // The detent: crossing 0 in either direction lands on it.
    if ((from > 0 && to < 0) || (from < 0 && to > 0)) to = 0;
    if (to < -kAllRange) to = -kAllRange;
    if (to > kAllRange) to = kAllRange;
    return int8_t(to);
}

uint8_t rel_all_byte(uint8_t value, int8_t amount) {
    if (amount == 0) return value;
    const int moved = int(value) + int(amount);
    return uint8_t(moved < 0 ? 0 : (moved > 127 ? 127 : moved));
}

int machine_release_params(uint8_t machine, uint8_t out[2]) {
    int count = 0;
    switch (MachineId(machine)) {
        case MachineId::kick:
        case MachineId::snare:
        case MachineId::hat:
        case MachineId::perc:
        case MachineId::va_mono:
        case MachineId::va_poly:
        case MachineId::wave_mono:
        case MachineId::wave_poly:
        case MachineId::fm:
            out[count++] = kAmpFirst;            // DEC is AMP's first knob
            break;
        case MachineId::sample:
            out[count++] = kAmpFirst + 1;        // ATK DEC PLY LVL
            break;
        case MachineId::grain:
            out[count++] = kAmpFirst + 2;        // PIT SHP DEC LVL
            break;
        case MachineId::midi:
        default:
            return 0;
    }
    if (machine_reads_asr(machine)) out[count++] = kAsrRel;
    return count;
}

uint32_t len_all_frames(uint32_t frames, int8_t amount) {
    if (amount == 0 || frames == 0) return frames;
    const int a = amount < -kAllRange ? -kAllRange : (amount > kAllRange ? kAllRange : amount);
    // a = 32 octaves + k, k in 0..31 (octaves -2 .. 2).
    const int octaves = (a + 2 * 32) / 32 - 2;    // a constant divisor: a shift
    const int k = a - octaves * 32;
    uint64_t scaled = uint64_t(frames) * kOctaveQ16[k];
    scaled = octaves >= 0 ? scaled << octaves : scaled >> -octaves;
    scaled = (scaled + 32768u) >> 16;
    if (scaled < 1u) scaled = 1u;
    if (scaled > 0xFFFFFFFFu) scaled = 0xFFFFFFFFu;
    return uint32_t(scaled);
}

int len_all_percent(int8_t amount) {
    return int(len_all_frames(100u * 65536u, amount) + 32768u) >> 16;
}

}  // namespace strata
