// Tests for tap tempo and the tempo nudge (core/taptempo).
#include "check.hpp"

#include "taptempo.hpp"

using namespace strata;

namespace {

// Taps every `interval` ms from `start`, `count` of them; returns the last result.
uint32_t taps(TapTempo &tap, uint32_t start, uint32_t interval, int count) {
    uint32_t result = 0;
    for (int i = 0; i < count; i++) result = tap_tempo(tap, start + uint32_t(i) * interval);
    return result;
}

}  // namespace

TEST(taptempo_the_first_tap_only_starts_the_count) {
    TapTempo tap;
    CHECK_EQ(tap_tempo(tap, 1000), 0u);
    CHECK(tap.started);
    CHECK_EQ(tap.count, 0);
}

TEST(taptempo_two_taps_set_nothing_and_a_third_that_agrees_sets_120_bpm) {
    // The review's M5: two stray taps used to set and save 60000 / gap BPM.
    TapTempo tap;
    CHECK_EQ(taps(tap, 5000, 500, 2), 0u);
    CHECK_EQ(tap_tempo(tap, 6000), 1200u);
}

TEST(taptempo_a_third_tap_that_disagrees_sets_nothing_and_pairs_with_the_next) {
    TapTempo tap;
    // 1.2 s then 0.5 s: off by more than a quarter, nothing set (it would have been 50 BPM).
    CHECK_EQ(taps(tap, 0, 1200, 2), 0u);
    CHECK_EQ(tap_tempo(tap, 1700), 0u);
    CHECK_EQ(tap.count, 1);
    // The next tap 500 ms on agrees with the kept 500: 120.
    CHECK_EQ(tap_tempo(tap, 2200), 1200u);
}

TEST(taptempo_two_intervals_a_quarter_apart_still_agree) {
    TapTempo tap;
    taps(tap, 0, 400, 2);
    CHECK(tap_tempo(tap, 400 + 500) != 0u);    // 500 against 400: exactly 25%
    TapTempo off;
    taps(off, 0, 400, 2);
    CHECK_EQ(tap_tempo(off, 400 + 501), 0u);   // just over
}

TEST(taptempo_steady_taps_give_the_tempo_to_a_tenth) {
    TapTempo tap;
    // 130 BPM is 461.54 ms a beat: alternate 461 and 462 and the mean lands on 130.0.
    uint32_t now = 100;
    tap_tempo(tap, now);
    uint32_t result = 0;
    for (int i = 0; i < 8; i++) {
        now += (i & 1) ? 462u : 461u;
        result = tap_tempo(tap, now);
    }
    CHECK_EQ(result, 1300u);
    CHECK_EQ(tap.count, kTapIntervals);
}

TEST(taptempo_one_late_tap_moves_the_tempo_a_little_not_all_the_way) {
    TapTempo tap;
    taps(tap, 0, 500, 5);                      // four intervals of 500 ms: 120.0
    const uint32_t result = tap_tempo(tap, 2000 + 600);   // one tap 100 ms late
    CHECK(result < 1200u);
    CHECK(result > 1100u);                     // the mean of 500 500 500 600 is 525 ms: 114.3
    CHECK_EQ(result, 1143u);
}

TEST(taptempo_the_average_is_over_the_last_four_intervals_only) {
    TapTempo tap;
    taps(tap, 0, 600, 6);                      // 100 BPM
    uint32_t now = 3000;
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {              // four intervals at 120: 600 ms is fully gone
        now += 500;
        result = tap_tempo(tap, now);
    }
    CHECK_EQ(result, 1200u);
}

TEST(taptempo_a_long_gap_starts_a_new_count) {
    TapTempo tap;
    taps(tap, 0, 500, 4);
    CHECK_EQ(tap_tempo(tap, 1500 + kTapTimeoutMs + 1), 0u);
    CHECK_EQ(tap.count, 0);
    // And the next tap counts from the one after the gap, not from the old taps.
    CHECK_EQ(tap_tempo(tap, 1500 + kTapTimeoutMs + 1 + 400), 0u);
    CHECK_EQ(tap_tempo(tap, 1500 + kTapTimeoutMs + 1 + 800), 1500u);
}

TEST(taptempo_a_gap_of_exactly_the_timeout_still_counts) {
    TapTempo tap;
    taps(tap, 0, kTapTimeoutMs, 2);
    CHECK_EQ(tap_tempo(tap, 2 * kTapTimeoutMs), 300u);   // 2 s a beat: 30 BPM
}

TEST(taptempo_a_tap_at_twice_the_tempo_starts_again_rather_than_averaging) {
    TapTempo tap;
    taps(tap, 0, 600, 4);                      // 100 BPM
    // Then the hand taps eighths: 250 ms is under half of 600, a new count from it.
    CHECK_EQ(tap_tempo(tap, 1800 + 250), 0u);
    CHECK_EQ(tap.count, 1);
    CHECK_EQ(tap_tempo(tap, 1800 + 500), 2400u);   // and the eighths, once two agree
}

TEST(taptempo_two_taps_in_one_millisecond_restart_rather_than_divide_by_nothing) {
    TapTempo tap;
    tap_tempo(tap, 700);
    CHECK_EQ(tap_tempo(tap, 700), 0u);
    CHECK_EQ(tap.count, 0);
}

TEST(taptempo_the_tempo_is_clamped_to_the_clocks_range) {
    TapTempo fast;
    CHECK_EQ(taps(fast, 0, 150, 3), kTempoTenthsMax);     // 400 BPM asked, 300 given
    TapTempo slow;
    CHECK_EQ(taps(slow, 0, 1999, 3), 300u);              // 30.0: inside, not clamped
}

TEST(taptempo_it_keeps_counting_across_the_millisecond_counters_wrap) {
    TapTempo tap;
    taps(tap, 0xFFFFFE00u, 500, 2);
    CHECK_EQ(tap_tempo(tap, 0xFFFFFE00u + 1000u), 1200u);  // wraps past 0
}

TEST(taptempo_reset_forgets_the_count) {
    TapTempo tap;
    taps(tap, 0, 500, 3);
    tap_reset(tap);
    CHECK(!tap.started);
    CHECK_EQ(tap_tempo(tap, 2000), 0u);
}

TEST(taptempo_a_nudge_holds_for_its_time_from_the_last_detent) {
    Nudge nudge;
    nudge_turn(nudge, 1, 1000);
    CHECK_EQ(nudge.direction, 1);
    CHECK(!nudge_due(nudge, 1000 + kNudgeMs - 1));
    nudge_turn(nudge, 2, 1100);                // another detent: held longer
    CHECK(!nudge_due(nudge, 1000 + kNudgeMs));
    CHECK(nudge_due(nudge, 1100 + kNudgeMs));
    CHECK_EQ(nudge.direction, 0);
    CHECK(!nudge_due(nudge, 5000));            // once only
}

TEST(taptempo_a_turn_the_other_way_swaps_the_nudge) {
    Nudge nudge;
    nudge_turn(nudge, 3, 0);
    nudge_turn(nudge, -1, 50);
    CHECK_EQ(nudge.direction, -1);
    nudge_turn(nudge, 0, 100);                 // no detent: nothing changes
    CHECK_EQ(nudge.direction, -1);
    CHECK_EQ(nudge.until_ms, 50u + kNudgeMs);
}

TEST(taptempo_a_nudge_runs_out_across_the_counters_wrap) {
    Nudge nudge;
    nudge_turn(nudge, -1, 0xFFFFFFF0u);
    CHECK(!nudge_due(nudge, 0xFFFFFFFFu));
    CHECK(nudge_due(nudge, 0xFFFFFFF0u + kNudgeMs));
}

TEST(taptempo_the_nudged_tempo_is_four_percent_either_way_and_clamped) {
    CHECK_NEAR(nudged_bpm(1300, 1), 135.2f, 0.01f);
    CHECK_NEAR(nudged_bpm(1300, -1), 124.8f, 0.01f);
    CHECK_NEAR(nudged_bpm(1300, 0), 130.0f, 0.01f);
    CHECK_NEAR(nudged_bpm(3000, 1), 300.0f, 0.001f);
    CHECK_NEAR(nudged_bpm(200, -1), 20.0f, 0.001f);
    CHECK_NEAR(nudged_bpm(1300, 7), 135.2f, 0.01f);      // only the sign counts
}
