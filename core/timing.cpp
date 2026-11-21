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

}  // namespace

int effective_tick(int step_index, const TrackTiming &timing, const int8_t *micro) {
    const int step_ticks = timing.step_ticks();
    const int nominal = step_index * step_ticks;
    return nominal + swing_offset(step_index, timing.swing, step_ticks) + micro_of(micro, step_index);
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

    for (; candidate <= last_candidate; candidate++) {
        const int32_t effective = loop_start + effective_tick(int(step_index), timing, micro);
        if (effective == absolute) {
            out[found].step = uint8_t(step_index);
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
