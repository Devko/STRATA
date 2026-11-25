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

    // Counted by PASS, not by tick window: passes 0 to loops-1, scanned one pass past their end
    // so a step pushed over the last boundary is still found. A tick window would count the
    // next pass's step 1 when micro-timing pulls it in early, and miss the first pass's when it
    // is pulled before tick 0 (which now lands on tick 0).
    for (int tick = 0; tick < loop_ticks * (loops + 1); tick++) {
        Firing firings[kMaxFiringsPerTick];
        const int n = steps_firing_at(uint32_t(tick), timing, micro, firings, kMaxFiringsPerTick);

        if (n > census.max_per_tick) census.max_per_tick = n;
        for (int i = 0; i < n; i++) {
            if (int(firings[i].loop) >= loops) continue;
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

TEST(step_1_pulled_before_tick_0_fires_on_tick_0_of_the_first_pass) {
    // Step 0 at negative micro-timing belongs to the tick before the pass starts, which on the
    // first pass after PLAY does not exist. It was skipped there and first heard a whole loop
    // later. It now lands on tick 0, once, and every later pass is unchanged.
    int8_t micro[kMaxSteps] = {};
    micro[0] = -5;

    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;

    Firing firings[kMaxFiringsPerTick];
    int zero_hits = 0;
    for (uint32_t t = 0; t < 384u * 3u; t++) {
        const int n = steps_firing_at(t, timing, micro, firings, kMaxFiringsPerTick);
        for (int i = 0; i < n; i++) {
            if (firings[i].step != 0 || firings[i].loop >= 3u) continue;
            zero_hits++;
            if (firings[i].loop == 0) CHECK_EQ(t, 0u);
            else CHECK_EQ(t, firings[i].loop * 384u - 5u);
        }
    }
    CHECK_EQ(zero_hits, 3);      // passes 0, 1 and 2
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

TEST(timing_clamps_degenerate_input) {
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

TEST(a_long_timing_run_stays_consistent) {
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
    for (int tick = 0; tick < loop_ticks * (loops + 1); tick++) {
        Firing firings[kMaxFiringsPerTick];
        const int n = steps_firing_at(uint32_t(tick), timing, micro, firings, kMaxFiringsPerTick);
        for (int i = 0; i < n; i++) total += int(firings[i].loop) < loops ? 1 : 0;
    }
    CHECK_EQ(total, 16 * loops);
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(two_steps_landing_on_one_tick_fill_a_one_slot_buffer_and_no_more) {
    // At double speed a step is 12 ticks: step 1 pushed +6 and step 2 pulled -6 both land on 18.
    TrackTiming timing;
    timing.length = 16;
    timing.speed = Speed::x2;
    timing.swing = kSwingMin;
    int8_t micro[kMaxSteps] = {};
    micro[1] = 6;
    micro[2] = -6;
    Firing firings[3];
    CHECK_EQ(steps_firing_at(18, timing, micro, firings, 3), 2);
    firings[1].step = 99;
    CHECK_EQ(steps_firing_at(18, timing, micro, firings, 1), 1);
    CHECK_EQ(int(firings[1].step), 99);
}

TEST(a_stored_length_of_zero_plays_as_one_step) {
    TrackTiming timing;
    timing.length = 0;
    timing.speed = Speed::x1;
    timing.swing = kSwingMin;
    Firing firings[kMaxFiringsPerTick];
    for (uint32_t t = 0; t < 4 * 24; t += 24) {
        CHECK_EQ(steps_firing_at(t, timing, nullptr, firings, kMaxFiringsPerTick), 1);
        CHECK_EQ(int(firings[0].step), 0);
        CHECK_EQ(firings[0].loop, t / 24);
    }
}

TEST(timing_length_within_steps_never_runs_past_the_step_pressed) {
    CHECK_EQ(int(length_index_within_steps(1)), 8);            // 1 step
    CHECK_EQ(int(length_sixteenths(length_index_within_steps(2))), 32);
    CHECK_EQ(int(length_sixteenths(length_index_within_steps(4))), 64);
    CHECK_EQ(int(length_sixteenths(length_index_within_steps(5))), 64);    // 4, not 6
    CHECK_EQ(int(length_sixteenths(length_index_within_steps(16))), 256);
    CHECK_EQ(int(length_index_within_steps(64)), kLengthMax);
    CHECK_EQ(int(length_index_within_steps(200)), kLengthMax);
    CHECK_EQ(int(length_index_within_steps(0)), 8);
    for (int n = 1; n <= 64; n++) {
        const int got = int(length_sixteenths(length_index_within_steps(n)));
        CHECK(got <= n * 16);
    }
}

// --- play modes -------------------------------------------------------------

TEST(play_mode_values_are_stable_and_forward_is_zero) {
    // Serialized in a pattern (core/serialize.hpp, section 4): never renumbered.
    CHECK_EQ(int(PlayMode::fwd), 0);
    CHECK_EQ(int(PlayMode::rev), 1);
    CHECK_EQ(int(PlayMode::ping), 2);
    CHECK_EQ(int(PlayMode::rnd1), 3);
    CHECK_EQ(int(PlayMode::rnd16), 7);
    CHECK_EQ(int(PlayMode::count), 8);
    CHECK(PlayMode(0) == PlayMode::fwd);       // a zeroed byte plays forward
    CHECK(TrackTiming{}.play == PlayMode::fwd);
    for (int m = 0; m < int(PlayMode::count); m++) {
        const char *name = play_mode_name(PlayMode(m));
        CHECK(std::strlen(name) >= 3 && std::strlen(name) <= 5);
    }
}

TEST(forward_reverse_and_ping_pong_map_slots_to_steps) {
    TrackTiming timing;
    timing.length = 12;
    for (int s = 0; s < 12; s++) {
        timing.play = PlayMode::fwd;
        CHECK_EQ(int(play_step(s, 0, timing)), s);
        CHECK_EQ(int(play_step(s, 5, timing)), s);
        timing.play = PlayMode::rev;
        CHECK_EQ(int(play_step(s, 0, timing)), 11 - s);
        CHECK_EQ(int(play_step(s, 5, timing)), 11 - s);
        timing.play = PlayMode::ping;
        CHECK_EQ(int(play_step(s, 0, timing)), s);         // even passes forward
        CHECK_EQ(int(play_step(s, 1, timing)), 11 - s);    // odd passes back
        CHECK_EQ(int(play_step(s, 6, timing)), s);
    }
}

TEST(every_play_mode_stays_inside_the_track_for_every_length) {
    TrackTiming timing;
    timing.play_seed = 0xDEADBEEFu;
    for (int m = 0; m < int(PlayMode::count); m++) {
        timing.play = PlayMode(m);
        for (int length = 1; length <= kMaxSteps; length++) {
            timing.length = uint8_t(length);
            for (uint32_t loop = 0; loop < 9; loop++) {
                for (int s = 0; s < length; s++) {
                    CHECK(int(play_step(s, loop, timing)) < length);
                }
            }
        }
    }
    // Degenerate input is clamped, not trusted: a slot past the end and a length of 0.
    timing.play = PlayMode::rev;
    timing.length = 16;
    CHECK_EQ(int(play_step(40, 0, timing)), 0);
    CHECK_EQ(int(play_step(-3, 0, timing)), 15);
    timing.length = 0;
    CHECK_EQ(int(play_step(0, 0, timing)), 0);
}

TEST(random_blocks_keep_their_steps_in_order_and_the_tail_in_place) {
    TrackTiming timing;
    timing.play_seed = play_seed(1, 2);
    timing.length = 14;     // three whole blocks of 4, then slots 12 and 13
    timing.play = PlayMode::rnd4;
    for (uint32_t loop = 0; loop < 64; loop++) {
        for (int block = 0; block < 3; block++) {
            const int first = int(play_step(block * 4, loop, timing));
            CHECK_EQ(first % 4, 0);                        // a whole block, from its start
            CHECK(first < 12);                             // never the partial tail
            for (int o = 1; o < 4; o++) CHECK_EQ(int(play_step(block * 4 + o, loop, timing)), first + o);
        }
        CHECK_EQ(int(play_step(12, loop, timing)), 12);
        CHECK_EQ(int(play_step(13, loop, timing)), 13);
    }
    // A block as long as the track, or longer, leaves it playing forward.
    timing.length = 16;
    timing.play = PlayMode::rnd16;
    for (int s = 0; s < 16; s++) CHECK_EQ(int(play_step(s, 3, timing)), s);
    timing.length = 12;
    for (int s = 0; s < 12; s++) CHECK_EQ(int(play_step(s, 3, timing)), s);
}

TEST(random_play_is_deterministic_varies_by_pass_and_seed_and_reaches_every_block) {
    TrackTiming a;
    a.length = 16;
    a.play = PlayMode::rnd1;
    a.play_seed = play_seed(1, 0);
    TrackTiming b = a;
    b.play_seed = play_seed(1, 1);       // another track of the same pattern

    int hits[16] = {};
    int differs_by_pass = 0, differs_by_track = 0;
    for (uint32_t loop = 0; loop < 400; loop++) {
        for (int s = 0; s < 16; s++) {
            const int step = int(play_step(s, loop, a));
            CHECK_EQ(int(play_step(s, loop, a)), step);    // the same arguments, the same step
            hits[step]++;
            if (step != int(play_step(s, loop + 1, a))) differs_by_pass++;
            if (step != int(play_step(s, loop, b))) differs_by_track++;
        }
    }
    // 6,400 draws over 16 steps: 400 each on average. Every step is reached and none dominates.
    for (int i = 0; i < 16; i++) CHECK(hits[i] > 300 && hits[i] < 500);
    CHECK(differs_by_pass > 5000);
    CHECK(differs_by_track > 5000);
}

// Under a play mode each pass still fires exactly `length` times, once per slot, and the step a
// slot fires is play_step's. Swing stays on the slot; micro-timing comes with the step.
TEST(a_play_mode_fires_once_per_slot_with_swing_on_the_slot_and_micro_on_the_step) {
    for (int m = 0; m < int(PlayMode::count); m++) {
        TrackTiming timing;
        timing.length = 16;
        timing.swing = 66;
        timing.play = PlayMode(m);
        timing.play_seed = play_seed(7, 3);
        int8_t micro[kMaxSteps] = {};
        micro[0] = 5;
        micro[5] = -4;

        const int loops = 6;
        int per_loop[loops] = {};
        for (int tick = 0; tick < timing.loop_ticks() * (loops + 1); tick++) {
            Firing firings[kMaxFiringsPerTick];
            const int n = steps_firing_at(uint32_t(tick), timing, micro, firings, kMaxFiringsPerTick);
            for (int i = 0; i < n; i++) {
                const int loop = int(firings[i].loop);
                if (loop >= loops) continue;
                per_loop[loop]++;
                // Which slot of this pass fires this tick? The one whose step and position agree.
                bool found = false;
                for (int s = 0; s < 16; s++) {
                    const int step = int(play_step(s, uint32_t(loop), timing));
                    const int at = loop * timing.loop_ticks() + s * 24 +
                                   swing_offset(s, 66, 24) + int(micro[step]);
                    if (step == int(firings[i].step) && (at < 0 ? 0 : at) == tick) found = true;
                }
                CHECK(found);
            }
        }
        for (int loop = 0; loop < loops; loop++) CHECK_EQ(per_loop[loop], 16);
    }
}
