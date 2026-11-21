// Tests for the tick-to-step mapping in core/timing.
//
// The invariant that matters most is "every step fires exactly once per loop pass". Swing and
// micro-timing move steps around, and an off-by-one in the scan window would silently drop a
// step or fire one twice -- a bug that is nearly impossible to find by ear on hardware and
// trivial to catch here. So most of these tests sweep every tick across several loops and count.
#include "check.hpp"

#include "timing.hpp"

#include <cstring>

using namespace strata;

namespace {

// Sweep every tick of `loops` passes and count how often each step fires.
struct Census {
    int fired[kMaxSteps] = {};
    int total = 0;
    int max_per_tick = 0;
    bool loop_numbers_ok = true;
};

Census take_census(const TrackTiming &timing, const int8_t *micro, int loops) {
    Census census;
    const int loop_ticks = timing.loop_ticks();

    for (int tick = 0; tick < loop_ticks * loops; tick++) {
        Firing firings[kMaxFiringsPerTick];
        const int n = steps_firing_at(uint32_t(tick), timing, micro, firings, kMaxFiringsPerTick);

        if (n > census.max_per_tick) census.max_per_tick = n;
        for (int i = 0; i < n; i++) {
            census.fired[firings[i].step]++;
            census.total++;
            // The loop number must match the pass the tick falls in, give or take the shift
            // that can push a step over a loop boundary.
            const int expected_loop = tick / loop_ticks;
            const int delta = int(firings[i].loop) - expected_loop;
            if (delta < -1 || delta > 1) census.loop_numbers_ok = false;
        }
    }
    return census;
}

// Every step of the track fired exactly `loops` times, and nothing outside its length fired.
void check_every_step_once_per_loop(const TrackTiming &timing, const int8_t *micro, int loops) {
    const Census census = take_census(timing, micro, loops);
    const int length = int(timing.length);

    for (int step = 0; step < length; step++) {
        CHECK_EQ(census.fired[step], loops);
    }
    for (int step = length; step < kMaxSteps; step++) {
        CHECK_EQ(census.fired[step], 0);
    }
    CHECK_EQ(census.total, length * loops);
    CHECK(census.loop_numbers_ok);
    CHECK(census.max_per_tick <= kMaxFiringsPerTick);
}

}  // namespace

// --- the grid -------------------------------------------------------------

TEST(the_grid_is_exact_at_96_ppqn) {
    CHECK_EQ(kPpqn, 96);
    CHECK_EQ(kTicksPerSixteenth, 24);

    // Every speed divides the grid exactly -- including 3/4, which is why 96 PPQN works.
    CHECK_EQ(ticks_per_step(Speed::x2), 12);
    CHECK_EQ(ticks_per_step(Speed::x1), 24);
    CHECK_EQ(ticks_per_step(Speed::x3_4), 32);
    CHECK_EQ(ticks_per_step(Speed::x1_2), 48);
    CHECK_EQ(ticks_per_step(Speed::x1_4), 96);
    CHECK_EQ(ticks_per_step(Speed::x1_8), 192);
}

TEST(speed_values_are_stable) {
    // These are serialized in a pattern, so a renumber must fail here.
    CHECK_EQ(int(Speed::x1), 0);
    CHECK_EQ(int(Speed::x2), 1);
    CHECK_EQ(int(Speed::x3_4), 2);
    CHECK_EQ(int(Speed::x1_2), 3);
    CHECK_EQ(int(Speed::x1_4), 4);
    CHECK_EQ(int(Speed::x1_8), 5);
}

TEST(zeroed_memory_means_normal_speed) {
    // "Clear pattern" is a user-facing feature and memset is the obvious implementation, so
    // zero must mean 1x. With 2x at zero, clearing a pattern would double its tempo.
    CHECK_EQ(int(Speed(0)), int(Speed::x1));
    CHECK_EQ(ticks_per_step(Speed(0)), kTicksPerSixteenth);

    // A default-constructed TrackTiming agrees, and so does a zeroed one.
    TrackTiming fresh;
    CHECK_EQ(int(fresh.speed), int(Speed::x1));
}

TEST(speed_display_order_is_the_spec_order) {
    // The UI walks fastest to slowest, which is not the enum's numbering.
    CHECK_EQ(int(kSpeedDisplayOrder[0]), int(Speed::x2));
    CHECK_EQ(int(kSpeedDisplayOrder[1]), int(Speed::x1));
    CHECK_EQ(int(kSpeedDisplayOrder[5]), int(Speed::x1_8));

    // Every speed appears exactly once, so the encoder can reach them all.
    int seen[int(Speed::count)] = {};
    for (int i = 0; i < int(Speed::count); i++) seen[int(kSpeedDisplayOrder[i])]++;
    for (int i = 0; i < int(Speed::count); i++) CHECK_EQ(seen[i], 1);

    // And the display order really is slowest-last.
    for (int i = 1; i < int(Speed::count); i++) {
        CHECK(ticks_per_step(kSpeedDisplayOrder[i]) > ticks_per_step(kSpeedDisplayOrder[i - 1]));
    }
}

// --- the note-length table ------------------------------------------------

TEST(length_index_zero_is_invalid) {
    // A zeroed or corrupt byte must be refusable, not silently the shortest note.
    CHECK(!length_valid(0));
    CHECK_EQ(int(length_sixteenths(0)), 0);
    CHECK(!length_valid(uint8_t(kLengthMax + 1)));
    CHECK(!length_valid(255));
    CHECK_EQ(int(length_sixteenths(255)), 0);      // no out-of-bounds read
}

TEST(length_table_spans_the_spec_range) {
    CHECK(length_valid(1));
    CHECK(length_valid(kLengthMax));

    CHECK_EQ(int(length_sixteenths(1)), 1);                 // 1/16 step, the spec's minimum
    CHECK_EQ(int(length_sixteenths(kLengthMax)), 1024);     // 64 steps, the spec's maximum
    CHECK_EQ(1024 / 16, 64);                                // which really is 64 steps
}

TEST(length_table_is_strictly_increasing) {
    // A non-monotonic table would make the encoder jump around.
    for (int i = 2; i <= kLengthMax; i++) {
        CHECK(kLengthSixteenthsOfStep[i] > kLengthSixteenthsOfStep[i - 1]);
    }
}

TEST(length_in_samples_scales_with_tempo_and_speed) {
    // One step at 1x, 130 BPM: a tick is 60/130/96 s = 4.8077 ms = 230.77 samples at 48 kHz.
    const float samples_per_tick = 48000.0f * 60.0f / (130.0f * float(kPpqn));
    CHECK_NEAR(samples_per_tick, 230.769f, 0.01);

    const uint8_t one_step = 8;    // table entry 8 is 16 sixteenths = one step
    CHECK_EQ(int(length_sixteenths(one_step)), 16);

    const float at_x1 = length_samples(one_step, ticks_per_step(Speed::x1), samples_per_tick);
    CHECK_NEAR(at_x1, 24.0f * samples_per_tick, 0.1);      // 24 ticks

    // "One step" follows the track's speed: at 1/2x a step is twice as long.
    const float at_half = length_samples(one_step, ticks_per_step(Speed::x1_2), samples_per_tick);
    CHECK_NEAR(at_half, 2.0f * at_x1, 0.1);

    // A sub-tick length is representable in samples even though 96 PPQN cannot express it.
    const float sixteenth_of_a_step =
        length_samples(1, ticks_per_step(Speed::x1), samples_per_tick);
    CHECK_NEAR(sixteenth_of_a_step, 1.5f * samples_per_tick, 0.1);
    CHECK(sixteenth_of_a_step > 0.0f);

    // An invalid index gives a zero gate, not garbage.
    CHECK_EQ(length_samples(0, ticks_per_step(Speed::x1), samples_per_tick), 0.0f);
}

// --- swing ----------------------------------------------------------------

TEST(swing_moves_only_odd_steps) {
    const int step_ticks = kTicksPerSixteenth;

    // Even steps never move, whatever the swing.
    CHECK_EQ(swing_offset(0, 50, step_ticks), 0);
    CHECK_EQ(swing_offset(0, 75, step_ticks), 0);
    CHECK_EQ(swing_offset(2, 75, step_ticks), 0);
    CHECK_EQ(swing_offset(62, 75, step_ticks), 0);

    // At 50% nothing moves at all: that is straight time.
    CHECK_EQ(swing_offset(1, 50, step_ticks), 0);
    CHECK_EQ(swing_offset(3, 50, step_ticks), 0);

    // At 75% an odd step sits half a step late: 12 of 24 ticks.
    CHECK_EQ(swing_offset(1, 75, step_ticks), 12);
    CHECK_EQ(swing_offset(5, 75, step_ticks), 12);
}

TEST(swing_is_monotonic_and_clamped) {
    const int step_ticks = kTicksPerSixteenth;
    int previous = -1;
    for (int swing = kSwingMin; swing <= kSwingMax; swing++) {
        const int offset = swing_offset(1, swing, step_ticks);
        CHECK(offset >= previous);            // never goes backwards
        CHECK(offset <= step_ticks / 2);      // never past half a step
        previous = offset;
    }
    // Out-of-range input is clamped, not wrapped.
    CHECK_EQ(swing_offset(1, 0, step_ticks), swing_offset(1, kSwingMin, step_ticks));
    CHECK_EQ(swing_offset(1, 200, step_ticks), swing_offset(1, kSwingMax, step_ticks));
}

TEST(swing_scales_with_speed) {
    // Swing is half a step at 75%, whatever the step length.
    CHECK_EQ(swing_offset(1, 75, 12), 6);
    CHECK_EQ(swing_offset(1, 75, 48), 24);
    CHECK_EQ(swing_offset(1, 75, 192), 96);
}

// --- the firing invariant -------------------------------------------------

TEST(straight_16ths_fire_on_their_nominal_ticks) {
    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;

    for (int step = 0; step < 16; step++) {
        Firing firings[kMaxFiringsPerTick];
        const int n = steps_firing_at(uint32_t(step * 24), timing, nullptr, firings,
                                      kMaxFiringsPerTick);
        CHECK_EQ(n, 1);
        if (n == 1) CHECK_EQ(int(firings[0].step), step);
    }
    // And nothing fires in between.
    Firing firings[kMaxFiringsPerTick];
    CHECK_EQ(steps_firing_at(1, timing, nullptr, firings, kMaxFiringsPerTick), 0);
    CHECK_EQ(steps_firing_at(23, timing, nullptr, firings, kMaxFiringsPerTick), 0);
}

TEST(every_step_fires_once_per_loop_straight) {
    for (int speed = 0; speed < int(Speed::count); speed++) {
        TrackTiming timing;
        timing.length = 16;
        timing.speed = Speed(speed);
        timing.swing = kSwingMin;
        check_every_step_once_per_loop(timing, nullptr, 4);
    }
}

TEST(every_step_fires_once_per_loop_with_swing) {
    for (int swing = kSwingMin; swing <= kSwingMax; swing++) {
        TrackTiming timing;
        timing.length = 16;
        timing.speed = Speed::x1;
        timing.swing = uint8_t(swing);
        check_every_step_once_per_loop(timing, nullptr, 4);
    }
}

TEST(every_step_fires_once_per_loop_with_micro_timing) {
    // A hostile micro pattern: alternating extremes, which is where an off-by-one in the scan
    // window shows up.
    int8_t micro[kMaxSteps];
    for (int i = 0; i < kMaxSteps; i++) {
        micro[i] = int8_t((i % 2) ? kMicroRange : -kMicroRange);
    }

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;
    check_every_step_once_per_loop(timing, micro, 4);
}

TEST(every_step_fires_once_per_loop_with_swing_and_micro_together) {
    int8_t micro[kMaxSteps];
    for (int i = 0; i < kMaxSteps; i++) {
        micro[i] = int8_t(((i * 7) % 23) - kMicroRange);   // spread across the whole range
    }

    for (int swing = kSwingMin; swing <= kSwingMax; swing += 5) {
        for (int speed = 0; speed < int(Speed::count); speed++) {
            TrackTiming timing;
            timing.length = 16;
            timing.speed = Speed(speed);
            timing.swing = uint8_t(swing);
            check_every_step_once_per_loop(timing, micro, 3);
        }
    }
}

TEST(polymeter_lengths_all_hold_the_invariant) {
    int8_t micro[kMaxSteps];
    for (int i = 0; i < kMaxSteps; i++) micro[i] = int8_t((i % 5) - 2);

    for (int length = 1; length <= kMaxSteps; length++) {
        TrackTiming timing;
        timing.length = uint8_t(length);
        timing.speed = Speed::x1;
        timing.swing = 66;
        check_every_step_once_per_loop(timing, micro, 2);
    }
}

// --- micro-timing ---------------------------------------------------------

TEST(negative_micro_timing_fires_before_the_nominal_tick) {
    // This is the case a forward walk cannot handle, and the reason the mapping is inverted.
    int8_t micro[kMaxSteps] = {};
    micro[4] = -kMicroRange;        // step 4 nominally at tick 96, pulled to 85

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;

    Firing firings[kMaxFiringsPerTick];
    CHECK_EQ(steps_firing_at(96, timing, micro, firings, kMaxFiringsPerTick), 0);

    const int n = steps_firing_at(85, timing, micro, firings, kMaxFiringsPerTick);
    CHECK_EQ(n, 1);
    if (n == 1) CHECK_EQ(int(firings[0].step), 4);
}

TEST(micro_timing_is_clamped_not_wrapped) {
    int8_t micro[kMaxSteps] = {};
    micro[2] = 100;        // far outside the legal range

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;

    // Clamped to +11, so step 2 lands at 48 + 11 = 59, not somewhere unpredictable.
    CHECK_EQ(effective_tick(2, timing, micro), 59);
    check_every_step_once_per_loop(timing, micro, 2);
}

TEST(effective_tick_combines_swing_and_micro) {
    int8_t micro[kMaxSteps] = {};
    micro[1] = -4;

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = 75;

    // Step 1: nominal 24, swing +12, micro -4.
    CHECK_EQ(effective_tick(1, timing, micro), 32);
    // Step 0 never moves.
    CHECK_EQ(effective_tick(0, timing, micro), 0);
}

// --- loop numbering -------------------------------------------------------

TEST(loop_numbers_advance_for_conditions) {
    // A:B conditions need to know which pass of the loop a firing belongs to.
    TrackTiming timing;
    timing.length = 4;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;

    const int loop_ticks = timing.loop_ticks();
    CHECK_EQ(loop_ticks, 96);

    for (int loop = 0; loop < 5; loop++) {
        Firing firings[kMaxFiringsPerTick];
        const int n = steps_firing_at(uint32_t(loop * loop_ticks), timing, nullptr, firings,
                                      kMaxFiringsPerTick);
        CHECK_EQ(n, 1);
        if (n == 1) {
            CHECK_EQ(int(firings[0].step), 0);
            CHECK_EQ(int(firings[0].loop), loop);
        }
    }
}

// --- robustness -----------------------------------------------------------

TEST(degenerate_input_is_handled) {
    TrackTiming timing;
    timing.length = 0;              // illegal: clamped to 1
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;
    CHECK_EQ(timing.loop_ticks(), 24);

    Firing firings[kMaxFiringsPerTick];
    CHECK_EQ(steps_firing_at(0, timing, nullptr, nullptr, 0), 0);        // max 0
    CHECK_EQ(steps_firing_at(0, timing, nullptr, nullptr, 4), 0);        // null out

    timing.length = 200;            // illegal: clamped to 64
    CHECK_EQ(timing.loop_ticks(), kMaxSteps * 24);
    CHECK(steps_firing_at(0, timing, nullptr, firings, kMaxFiringsPerTick) >= 0);
}

TEST(a_long_run_stays_consistent) {
    // Roughly four minutes at 130 BPM, to be sure nothing drifts or overflows.
    int8_t micro[kMaxSteps];
    for (int i = 0; i < kMaxSteps; i++) micro[i] = int8_t((i % 23) - kMicroRange);

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = 62;

    const int loop_ticks = timing.loop_ticks();
    const int loops = 500;
    int total = 0;
    for (int tick = 0; tick < loop_ticks * loops; tick++) {
        Firing firings[kMaxFiringsPerTick];
        total += steps_firing_at(uint32_t(tick), timing, micro, firings, kMaxFiringsPerTick);
    }
    CHECK_EQ(total, 16 * loops);
}
