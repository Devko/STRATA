#include "timing.hpp"

namespace strata {

namespace {

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

int micro_of(const int8_t *micro, int step_index) {
    if (micro == nullptr) return 0;
    return clamp_int(int(micro[step_index]), -kMicroRange, kMicroRange);
}

// The same avalanche as core/scheduler's probability roll ("lowbias32"): neighbouring blocks and
// passes must not give neighbouring picks.
uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Where slot `slot` sits in its pass, with the swing of the SLOT and the micro of the step it
// plays (timing.hpp, steps_firing_at).
int slot_tick(int slot, int step, const TrackTiming &timing, const int8_t *micro) {
    const int step_ticks = timing.step_ticks();
    return slot * step_ticks + swing_offset(slot, timing.swing, step_ticks) + micro_of(micro, step);
}

}  // namespace

uint32_t play_seed(uint32_t pattern_seed, int track) {
    return mix32(pattern_seed ^ (uint32_t(track) * 0x85EBCA77u + 0x7F4A7C15u));
}

uint8_t play_step(int slot, uint32_t loop, const TrackTiming &timing) {
    const int length = clamp_int(int(timing.length), 1, kMaxSteps);
    const int s = clamp_int(slot, 0, length - 1);
    switch (timing.play) {
        case PlayMode::rev:
            return uint8_t(length - 1 - s);
        case PlayMode::ping:
            return uint8_t((loop & 1u) != 0 ? length - 1 - s : s);
        case PlayMode::rnd1:
        case PlayMode::rnd2:
        case PlayMode::rnd4:
        case PlayMode::rnd8:
        case PlayMode::rnd16: {
            // Blocks of 1 << shift slots: a shift and a mask, never a divide.
            const int shift = int(timing.play) - int(PlayMode::rnd1);
            const uint32_t blocks = uint32_t(length) >> shift;
            const uint32_t block = uint32_t(s) >> shift;
            // One whole block is nothing to shuffle, and the slots past the last whole block
            // play in place.
            if (blocks < 2u || block >= blocks) return uint8_t(s);
            uint32_t h = mix32(timing.play_seed ^ (block * 0xC2B2AE3Du + 0x27D4EB2Fu));
            h = mix32(h ^ (loop * 0x9E3779B1u + 0x85EBCA6Bu));
            // Multiply-shift into 0..blocks-1: the high bits, and no modulo.
            const uint32_t pick = uint32_t((uint64_t(h) * blocks) >> 32);
            return uint8_t((pick << shift) | (uint32_t(s) & ((1u << shift) - 1u)));
        }
        case PlayMode::fwd:
        default:
            return uint8_t(s);
    }
}

int effective_tick(int step_index, const TrackTiming &timing, const int8_t *micro) {
    return slot_tick(step_index, step_index, timing, micro);
}

int steps_firing_at(uint32_t tick, const TrackTiming &timing, const int8_t *micro,
                    Firing *out, int max) {
    if (out == nullptr || max <= 0) return 0;

    const int length = clamp_int(int(timing.length), 1, kMaxSteps);
    const int step_ticks = timing.step_ticks();
    const int loop_ticks = length * step_ticks;
    const int window = max_shift(step_ticks);

    int found = 0;

    // The candidate steps are those whose nominal tick lies within +/- window of `tick`. Nominal
    // ticks are step_ticks apart, so the candidates are a short contiguous run of step indices.
    // A step pushed past the end of its loop belongs to the next pass, and one pulled before the
    // start to the previous one, so the run is allowed to cross a loop boundary.
    //
    // ## Why this is 32-bit, and why it divides as little as it does
    //
    // The C674x has NO integer divide instruction: every division is a libgcc call. A 64-bit one
    // (__c6xabi_divlli) is both the most expensive and, on this toolchain, drags libgcc's
    // unwinder in and fails to link. So the arithmetic is int32_t throughout and the three
    // divisions are hoisted out of the candidate loop -- the loop body itself has none.
    //
    // int32_t is ample: ticks come from Enaga's clock at 96 PPQN, so 2^31 ticks is about 119 days
    // of continuous play at 130 BPM. Signed is deliberate, so that `tick - window` underflowing
    // near tick 0 produces a negative number that the loop skips rather than a huge unsigned one.
    const int32_t absolute = int32_t(tick);
    const int32_t first_candidate = (absolute - window) / step_ticks - 1;
    const int32_t last_candidate = (absolute + window) / step_ticks + 1;

    // Walk the candidates, tracking the loop pass and step index by addition instead of dividing
    // per candidate. Start from the first candidate's pass, then step forward.
    int32_t candidate = first_candidate < 0 ? 0 : first_candidate;
    int32_t loop = candidate / length;            // the only division in the setup
    int32_t step_index = candidate - loop * length;
    int32_t loop_start = loop * loop_ticks;
    const bool forward = timing.play == PlayMode::fwd;

    for (; candidate <= last_candidate; candidate++) {
        // A step pulled before tick 0 -- step 1 with negative micro-timing on the very first pass
        // after PLAY or a pattern start -- has no tick to land on, so it lands on tick 0. It used
        // to be skipped, and the first downbeat of a pattern with a pushed kick was silent.
        //
        // `step_index` is the time slot; the play mode says which step sounds in it. FWD, every
        // track of the default beat, skips the call: it measured 0.7 points on the emulator.
        const uint8_t plays = forward ? uint8_t(step_index)
                                      : play_step(int(step_index), uint32_t(loop), timing);
        const int32_t nominal = loop_start + slot_tick(int(step_index), plays, timing, micro);
        const int32_t effective = nominal < 0 ? 0 : nominal;
        if (effective == absolute) {
            out[found].step = plays;
            out[found].loop = uint32_t(loop);
            found++;
            if (found >= max) break;
        }

        // Advance without dividing.
        step_index++;
        if (step_index >= length) {
            step_index = 0;
            loop++;
            loop_start += loop_ticks;
        }
    }

    return found;
}

}  // namespace strata
