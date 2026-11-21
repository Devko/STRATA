// Live recording's arithmetic. core/record.hpp holds the contract; this is how it is met.
//
// ## Why this file writes out floor, round and abs
//
// core/ has no libm (CLAUDE.md), so they are here. They are not boilerplate: micro-timing is
// signed, a hit lands before the step it belongs to as often as after it, and the familiar
// `int(x + 0.5f)` rounds -2.5 to -2 while rounding +2.5 to +3. An asymmetric rounding records
// a hit half a tick early differently from one half a tick late -- a bias of one tick on every
// early hit, which on hardware reads as "my fills drag" and as nothing more findable than that.
//
// ## Why swing is hoisted out of the candidate scan
//
// The contract puts step s at `s * step_ticks + swing_offset(s)`, and swing_offset() divides by
// 50. The C674x has no divide instruction, so every division is a libgcc call; calling it per
// candidate would put up to 66 of them in the scan. It is zero for every even step and the SAME
// value for every odd one, so one call before the loop is the whole of it. The two readings
// could drift apart if core/timing's swing ever became index-dependent, so
// `the_swing_hoist_matches_core_timing` in tests/test_record.cpp holds them together.
#include "record.hpp"

namespace strata {

namespace {

// Floats beyond this are folded in rather than cast: `int(x)` is undefined when x does not fit,
// and a wild tempo or latency must not be able to turn a pad hit into undefined behaviour.
// 1e9 is comfortably inside int32_t.
constexpr float kCastLimit = 1.0e9f;

// Written as negated comparisons so that a NaN -- which answers false to every comparison --
// lands on a defined value instead of reaching the cast.
float fold_for_cast(float value) {
    if (!(value >= -kCastLimit)) return -kCastLimit;
    if (!(value <= kCastLimit)) return kCastLimit;
    return value;
}

// floor(), as an int. The cast truncates toward zero, so a negative value that is not already
// whole has been rounded the wrong way and needs one step down.
int32_t floor_to_int(float value) {
    const float v = fold_for_cast(value);
    const int32_t truncated = int32_t(v);
    return float(truncated) > v ? truncated - 1 : truncated;
}

// Round to nearest, halves away from zero, so that -0.5 and +0.5 end up one tick apart instead
// of both landing on 0. See the file comment: the symmetry is the point.
int32_t round_to_int(float value) {
    const float v = fold_for_cast(value);
    return v < 0.0f ? -floor_to_int(0.5f - v) : floor_to_int(v + 0.5f);
}

float absolute(float value) { return value < 0.0f ? -value : value; }

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

}  // namespace

// --- stamps ---------------------------------------------------------------

float ticks_per_ms(float bpm) {
    // bpm * kPpqn / 60000, with the constant ratio folded by the compiler so there is no
    // division here at all -- this is called from the same place the hit is stamped.
    return bpm * (float(kPpqn) / 60000.0f);
}

Stamp stamp_normalise(Stamp stamp) {
    const float value = fold_for_cast(stamp.fraction);
    int32_t whole = floor_to_int(value);
    float fraction = value - float(whole);

    // The two folds below are unconditional rather than defensive-looking: everything
    // downstream indexes a tick grid with `fraction`, so 0 <= fraction < 1 has to be true for
    // every input, including a NaN that arrived from a division by a zero tempo.
    if (!(fraction >= 0.0f)) fraction = 0.0f;
    while (fraction >= 1.0f) {
        fraction -= 1.0f;
        whole++;
    }

    Stamp out;
    if (whole >= 0) {
        out.tick = stamp.tick + uint32_t(whole);
        out.fraction = fraction;
        return out;
    }

    // `tick` is unsigned, so the borrow is tested before it can wrap. Moving back past the
    // start of the transport gives tick 0, fraction 0: there is nothing there to borrow from.
    const uint32_t back = uint32_t(-whole);
    if (back > stamp.tick) {
        out.tick = 0;
        out.fraction = 0.0f;
        return out;
    }
    out.tick = stamp.tick - back;
    out.fraction = fraction;
    return out;
}

Stamp stamp_back(Stamp now, float latency_ms, float tempo_bpm) {
    // Subtracting into `fraction` and normalising leaves the borrow and the clamp at zero in
    // one place instead of two.
    Stamp moved = now;
    moved.fraction -= latency_ms * ticks_per_ms(tempo_bpm);
    return stamp_normalise(moved);
}

// --- quantizing -----------------------------------------------------------

Landing quantize_hit(Stamp hit, const TrackTiming &timing, int quantize_percent) {
    const int length = clamp_int(int(timing.length), 1, kMaxSteps);
    const int step_ticks = timing.step_ticks();
    const int loop_ticks = length * step_ticks;

    // 1. Reduce the hit into the track's loop. Normalising first makes the function total: app/
    //    may hand over a stamp it has adjusted itself, and this costs one control-rate call.
    //    The division is 32-bit (a libgcc helper on the C674x) and happens once, before the
    //    scan rather than inside it.
    const Stamp at = stamp_normalise(hit);
    const uint32_t loop = at.tick / uint32_t(loop_ticks);
    const float within = float(at.tick - loop * uint32_t(loop_ticks)) + at.fraction;

    // 2. See the file comment: one swing_offset() call covers every candidate.
    const int odd_offset = swing_offset(1, int(timing.swing), step_ticks);

    // 3. The nearest candidate wins. Candidates are walked in increasing order of effective
    //    tick and only a strictly smaller distance replaces the best, which is what gives a tie
    //    to the earlier one.
    //
    //    The list is the track's own steps plus one on each side. The one on the right -- step 0
    //    of the next pass -- is what makes a hit a few ticks before the end of the loop record
    //    forward instead of being dragged back to the last step of this pass. The one on the
    //    left cannot win as long as `within` is reduced to be non-negative, because swing never
    //    moves an even step and so step 0 sits at exactly 0; it is in the contract and costs
    //    one comparison, so it stays here rather than becoming an unwritten assumption about
    //    how step 1 reduced the stamp.
    int best_step = 0;
    uint32_t best_loop = loop;
    int best_tick = 0;                  // the winner's effective tick, in this pass's frame
    float best_distance = -1.0f;        // < 0 means "nothing chosen yet"

    for (int candidate = -1; candidate <= length; candidate++) {
        int step = candidate;
        uint32_t step_loop = loop;
        int frame = 0;                  // the offset of another pass, in ticks
        if (candidate < 0) {
            if (loop == 0) continue;    // there is no pass before the first one
            step = length - 1;
            step_loop = loop - 1;
            frame = -loop_ticks;
        } else if (candidate >= length) {
            step = 0;
            step_loop = loop + 1;
            frame = loop_ticks;
        }

        const int tick = frame + step * step_ticks + ((step & 1) ? odd_offset : 0);
        const float distance = absolute(within - float(tick));
        if (best_distance < 0.0f || distance < best_distance) {
            best_distance = distance;
            best_step = step;
            best_loop = step_loop;
            best_tick = tick;
        }
    }

    // 4. micro is what is left over, scaled, rounded and clamped. The offset is measured from
    //    the winner's EFFECTIVE tick, swing included, which is the reason this module exists: a
    //    hit played dead on a swung odd step stores micro 0, not micro +swing.
    const float scale = float(100 - clamp_int(quantize_percent, 0, 100)) / 100.0f;
    const int32_t wanted = round_to_int((within - float(best_tick)) * scale);
    const int stored = clamp_int(int(wanted), -kMicroRange, kMicroRange);

    Landing landing;
    landing.step = uint8_t(best_step);
    landing.loop = best_loop;
    landing.micro = int8_t(stored);
    // Rounding to the tick grid always happens and the UI has nothing to do with it. The clamp
    // is different: it means the hit could not be stored where it was played.
    landing.clamped = stored != wanted;
    return landing;
}

}  // namespace strata
