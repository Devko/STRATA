// Tests for live recording's arithmetic in core/record.
//
// The invariant that matters most here is the inversion: the micro-timing a hit records is an
// offset from where its step ALREADY sits, swing included. A straight pattern records perfectly
// either way, so the only thing that catches the mistake is a swung one -- which is why every
// test below that can carry a swing value does, and why the first one walks 50, 58, 66 and 75.
//
// The second theme is that a hit near the end of a loop belongs to the NEXT pass. Recording it
// backwards puts it on a step the user is about to hear again, a bar late.
#include "check.hpp"

#include "record.hpp"
#include "timing.hpp"

using namespace strata;

namespace {

TrackTiming track(int length, Speed speed, int swing) {
    TrackTiming timing;
    timing.length = uint8_t(length);
    timing.speed = speed;
    timing.swing = uint8_t(swing);
    return timing;
}

Stamp stamp_at(uint32_t tick, float fraction) {
    Stamp stamp;
    stamp.tick = tick;
    stamp.fraction = fraction;
    return stamp;
}

// A hit played exactly on a step, as the sequencer itself would place it.
Stamp on_step(int step, int loop, const TrackTiming &timing) {
    return stamp_at(uint32_t(loop * timing.loop_ticks() + effective_tick(step, timing, nullptr)),
                    0.0f);
}

// Where the sequencer will fire a landing, in ticks from the start of the transport.
float landing_tick(const Landing &landing, const TrackTiming &timing, const int8_t *micro) {
    return float(int(landing.loop) * timing.loop_ticks()
                 + effective_tick(int(landing.step), timing, micro));
}

float absolute(float value) { return value < 0.0f ? -value : value; }

// A whole-pattern sweep. Like the census in tests/test_timing.cpp it accumulates and is checked
// once per configuration, so a single off-by-one does not print ten thousand failures.
struct SweepResult {
    int hits = 0;
    bool step_in_range = true;
    bool loop_monotonic = true;
    bool landing_monotonic = true;
    float worst_distance = 0.0f;
    float worst_playback_error = 0.0f;
};

SweepResult sweep(const TrackTiming &timing, int passes, int quantize) {
    SweepResult result;
    const int loop_ticks = timing.loop_ticks();
    const float fractions[] = {0.0f, 0.5f};

    uint32_t previous_loop = 0;
    float previous_landing = -1.0f;

    for (int tick = 0; tick < loop_ticks * passes; tick++) {
        for (float fraction : fractions) {
            const float position = float(tick) + fraction;
            const Landing landing = quantize_hit(stamp_at(uint32_t(tick), fraction), timing,
                                                 quantize);
            result.hits++;

            if (int(landing.step) >= int(timing.length)) result.step_in_range = false;
            if (landing.loop < previous_loop) result.loop_monotonic = false;
            previous_loop = landing.loop;

            // The grid position the hit was snapped to, before its own micro is applied. It
            // must never go backwards as the sweep moves forward, or two hits played in order
            // would record out of order.
            const float grid = landing_tick(landing, timing, nullptr);
            if (grid < previous_landing) result.landing_monotonic = false;
            previous_landing = grid;

            const float distance = absolute(position - grid);
            if (distance > result.worst_distance) result.worst_distance = distance;

            // At quantize 0 the micro carries the whole offset, so playing the step back lands
            // within half a tick of where it was hit -- unless the offset did not fit, which is
            // exactly what `clamped` is for.
            if (quantize == 0 && !landing.clamped) {
                int8_t micro[kMaxSteps] = {};
                micro[landing.step] = landing.micro;
                const float error = absolute(position - landing_tick(landing, timing, micro));
                if (error > result.worst_playback_error) result.worst_playback_error = error;
            }
        }
    }
    return result;
}

void check_sweep(const TrackTiming &timing, int passes, int quantize) {
    const SweepResult result = sweep(timing, passes, quantize);
    const int step_ticks = timing.step_ticks();

    // Swing makes the grid uneven: from an even step to the next odd one is step_ticks plus the
    // swing offset, and from that odd step to the following even one is step_ticks minus it. A
    // nearest-neighbour landing is at most half of the WIDER gap away -- which at swing 50 is
    // the textbook half a step.
    const float bound = 0.5f * float(step_ticks + swing_offset(1, int(timing.swing), step_ticks));

    CHECK(result.hits > 0);
    CHECK(result.step_in_range);
    CHECK(result.loop_monotonic);
    CHECK(result.landing_monotonic);
    CHECK(result.worst_distance <= bound + 0.001f);
    if (quantize == 0) CHECK(result.worst_playback_error <= 0.5f + 0.001f);
}

}  // namespace

// --- the inversion --------------------------------------------------------

TEST(a_hit_on_a_swung_step_records_micro_zero) {
    // THE test. A hit played dead on a step must store micro 0 whatever the swing, because the
    // stored micro is an offset from where the step already sits. Measuring from the nominal
    // tick instead would store the swing amount here, and a swung pattern would record every
    // odd step late by it.
    const int swings[] = {50, 58, 66, 75};
    const Speed speeds[] = {Speed::x1, Speed::x2, Speed::x3_4, Speed::x1_2};
    const int quantizes[] = {0, 100};

    for (int swing : swings) {
        for (Speed speed : speeds) {
            const TrackTiming timing = track(16, speed, swing);
            bool all_zero = true;
            for (int loop = 0; loop < 3; loop++) {
                for (int step = 0; step < 16; step++) {
                    const Stamp hit = on_step(step, loop, timing);
                    for (int quantize : quantizes) {
                        const Landing landing = quantize_hit(hit, timing, quantize);
                        if (int(landing.step) != step) all_zero = false;
                        if (int(landing.loop) != loop) all_zero = false;
                        if (landing.micro != 0) all_zero = false;
                        if (landing.clamped) all_zero = false;
                    }
                }
            }
            CHECK(all_zero);
        }
    }

    // Spelled out once without the loops, so a failure above has something to read against: at
    // swing 75 and 1x, odd step 1 sits at 24 + 12 = 36 ticks.
    const TrackTiming swung = track(16, Speed::x1, 75);
    CHECK_EQ(effective_tick(1, swung, nullptr), 36);
    const Landing landing = quantize_hit(stamp_at(36, 0.0f), swung, 0);
    CHECK_EQ(int(landing.step), 1);
    CHECK_EQ(int(landing.micro), 0);
}

TEST(an_offset_hit_measures_from_the_swung_tick) {
    // The same inversion with the hit slightly off. Measuring from the nominal tick would make
    // this +3 become +15 -- clamped to +11 -- on every odd step of a swung track.
    const TrackTiming timing = track(16, Speed::x1, 75);
    bool all_match = true;
    for (int offset = -5; offset <= 5; offset++) {
        const Stamp hit = stamp_at(uint32_t(36 + offset), 0.0f);
        const Landing landing = quantize_hit(hit, timing, 0);
        if (int(landing.step) != 1) all_match = false;
        if (int(landing.micro) != offset) all_match = false;
        if (landing.clamped) all_match = false;
    }
    CHECK(all_match);
}

TEST(a_recorded_hit_fires_where_it_was_played) {
    // The round trip, through the mapping that reads the result: record a hit, store the micro
    // it asks for, and ask core/timing which step fires on the tick that produces. Both halves
    // of the swing inversion have to agree for this to hold.
    const TrackTiming timing = track(16, Speed::x1, 66);
    bool all_fired = true;
    bool all_exact = true;

    for (int offset = -6; offset <= 6; offset++) {
        for (int step = 0; step < 16; step++) {
            const int played = effective_tick(step, timing, nullptr) + offset;
            if (played < 0) continue;      // a hit before the transport started has nowhere to go

            const Landing landing = quantize_hit(stamp_at(uint32_t(played), 0.0f), timing, 0);
            if (int(landing.step) != step || landing.clamped) {
                all_fired = false;
                continue;
            }

            int8_t micro[kMaxSteps] = {};
            micro[landing.step] = landing.micro;
            const int fires_at = int(landing_tick(landing, timing, micro));
            if (fires_at != played) all_exact = false;

            Firing firings[kMaxFiringsPerTick];
            const int n = steps_firing_at(uint32_t(fires_at), timing, micro, firings,
                                          kMaxFiringsPerTick);
            bool found = false;
            for (int i = 0; i < n; i++) {
                if (int(firings[i].step) == step && int(firings[i].loop) == int(landing.loop)) {
                    found = true;
                }
            }
            if (!found) all_fired = false;
        }
    }
    CHECK(all_fired);
    CHECK(all_exact);
}

TEST(the_swing_hoist_matches_core_timing) {
    // quantize_hit() calls swing_offset() once and applies it to every odd step, because the
    // division inside it is a libgcc call on the C674x. That is only valid while the offset
    // depends on the step's parity and nothing else, so this is the test that notices if
    // core/timing's swing ever becomes index-dependent.
    for (int speed = 0; speed < int(Speed::count); speed++) {
        const int step_ticks = ticks_per_step(Speed(speed));
        for (int swing = kSwingMin; swing <= kSwingMax; swing++) {
            const int odd = swing_offset(1, swing, step_ticks);
            bool matches = true;
            for (int step = 0; step < kMaxSteps; step++) {
                if (swing_offset(step, swing, step_ticks) != ((step & 1) ? odd : 0)) {
                    matches = false;
                }
            }
            CHECK(matches);
        }
    }
}

// --- the quantize amount --------------------------------------------------

TEST(quantize_100_always_snaps) {
    // Whatever the offset, a hard quantize stores no micro at all -- and never reports a clamp,
    // because nothing was lost that the user could have heard.
    const TrackTiming timing = track(16, Speed::x1, 66);
    const float fractions[] = {0.0f, 0.5f, 0.75f};
    bool all_snapped = true;

    for (int tick = 0; tick < timing.loop_ticks(); tick++) {
        for (float fraction : fractions) {
            const Landing landing = quantize_hit(stamp_at(uint32_t(tick), fraction), timing, 100);
            if (landing.micro != 0 || landing.clamped) all_snapped = false;
        }
    }
    CHECK(all_snapped);
}

TEST(quantize_0_preserves_the_offset_up_to_the_clamp) {
    // 1/4 speed: a 96-tick step, so there is room either side for offsets well past the micro
    // range without the nearest step changing.
    const TrackTiming timing = track(8, Speed::x1_4, kSwingMin);
    const int base = effective_tick(2, timing, nullptr);
    CHECK_EQ(base, 192);

    bool all_preserved = true;
    for (int offset = -kMicroRange; offset <= kMicroRange; offset++) {
        const Landing landing = quantize_hit(stamp_at(uint32_t(base + offset), 0.0f), timing, 0);
        if (int(landing.step) != 2) all_preserved = false;
        if (int(landing.micro) != offset) all_preserved = false;
        if (landing.clamped) all_preserved = false;
    }
    CHECK(all_preserved);

    // Past the range the step is still right and the micro saturates, with `clamped` set.
    const int outside[] = {-40, -kMicroRange - 1, kMicroRange + 1, 40};
    bool all_clamped = true;
    for (int offset : outside) {
        const Landing landing = quantize_hit(stamp_at(uint32_t(base + offset), 0.0f), timing, 0);
        if (int(landing.step) != 2) all_clamped = false;
        if (int(landing.micro) != (offset < 0 ? -kMicroRange : kMicroRange)) all_clamped = false;
        if (!landing.clamped) all_clamped = false;
    }
    CHECK(all_clamped);
}

TEST(quantize_50_halves_the_offset) {
    const TrackTiming timing = track(8, Speed::x1_4, kSwingMin);
    const int base = effective_tick(2, timing, nullptr);
    const int offsets[] = {-10, -8, -4, -2, 0, 2, 4, 8, 10};

    bool all_halved = true;
    for (int offset : offsets) {
        const Landing landing = quantize_hit(stamp_at(uint32_t(base + offset), 0.0f), timing, 50);
        if (int(landing.micro) != offset / 2) all_halved = false;
        if (landing.clamped) all_halved = false;
    }
    CHECK(all_halved);

    // A quarter of the way: 20% quantize leaves 80% of the offset.
    const Landing eighty = quantize_hit(stamp_at(uint32_t(base + 10), 0.0f), timing, 20);
    CHECK_EQ(int(eighty.micro), 8);
}

TEST(micro_shrinks_as_quantize_rises) {
    // The pull toward the grid is monotonic in the percentage and never changes the hit's sign,
    // so turning the encoder up always feels like tightening.
    const TrackTiming timing = track(8, Speed::x1_4, 58);
    const int base = effective_tick(3, timing, nullptr);
    const int offsets[] = {-9, -5, 5, 9};

    for (int offset : offsets) {
        bool monotonic = true;
        bool sign_kept = true;
        int previous = kMicroRange + 1;
        for (int quantize = 0; quantize <= 100; quantize++) {
            const Landing landing =
                quantize_hit(stamp_at(uint32_t(base + offset), 0.0f), timing, quantize);
            const int magnitude = landing.micro < 0 ? -int(landing.micro) : int(landing.micro);
            if (magnitude > previous) monotonic = false;
            if (landing.micro != 0 && (landing.micro < 0) != (offset < 0)) sign_kept = false;
            previous = magnitude;
        }
        CHECK(monotonic);
        CHECK(sign_kept);
        CHECK_EQ(previous, 0);          // and it really reaches zero at 100
    }
}

TEST(rounding_is_symmetric_about_zero) {
    // Half a tick early and half a tick late must record the same distance from the step. The
    // familiar int(x + 0.5f) rounds +0.5 to 1 and -0.5 to 0, which drags every early hit.
    const TrackTiming timing = track(8, Speed::x1_4, kSwingMin);
    const int base = effective_tick(2, timing, nullptr);

    const Landing late = quantize_hit(stamp_at(uint32_t(base + 4), 0.5f), timing, 0);
    const Landing early = quantize_hit(stamp_at(uint32_t(base - 5), 0.5f), timing, 0);
    CHECK_EQ(int(late.micro), 5);        // +4.5 rounds away from zero
    CHECK_EQ(int(early.micro), -5);      // -4.5 by the same amount, the other way

    // And through the scaling: +/-11 at 50% is +/-5.5, which must also be symmetric.
    const Landing up = quantize_hit(stamp_at(uint32_t(base + kMicroRange), 0.0f), timing, 50);
    const Landing down = quantize_hit(stamp_at(uint32_t(base - kMicroRange), 0.0f), timing, 50);
    CHECK_EQ(int(up.micro), 6);
    CHECK_EQ(int(down.micro), -6);
}

TEST(clamped_reports_the_clamp_and_not_rounding) {
    const TrackTiming timing = track(4, Speed::x1_4, kSwingMin);
    const int base = effective_tick(1, timing, nullptr);

    // A fractional offset loses something to rounding. That always happens and the UI has
    // nothing to say about it, so `clamped` stays false.
    const Landing rounded = quantize_hit(stamp_at(uint32_t(base + 4), 0.37f), timing, 0);
    CHECK_EQ(int(rounded.micro), 4);
    CHECK(!rounded.clamped);

    // Exactly on the edge of the range: representable, so still not clamped.
    const Landing edge = quantize_hit(stamp_at(uint32_t(base + kMicroRange), 0.0f), timing, 0);
    CHECK_EQ(int(edge.micro), kMicroRange);
    CHECK(!edge.clamped);

    // Half a tick further rounds to 12, which does not fit.
    const Landing over = quantize_hit(stamp_at(uint32_t(base + kMicroRange), 0.5f), timing, 0);
    CHECK_EQ(int(over.micro), kMicroRange);
    CHECK(over.clamped);

    const Landing under = quantize_hit(stamp_at(uint32_t(base - kMicroRange - 1), 0.5f), timing, 0);
    CHECK_EQ(int(under.micro), -kMicroRange);
    CHECK(under.clamped);

    // Scaling a far-out hit back inside the range is not a clamp either: a 90% quantize pulls
    // 40 ticks down to 4.
    const Landing scaled = quantize_hit(stamp_at(uint32_t(base + 40), 0.0f), timing, 90);
    CHECK_EQ(int(scaled.micro), 4);
    CHECK(!scaled.clamped);
}

// --- the wrap -------------------------------------------------------------

TEST(a_hit_just_before_the_loop_ends_wraps_forward) {
    // A hit a few ticks early belongs to step 0 of the NEXT pass. Dragging it back to the last
    // step of this one would place it nearly a bar late, on a step the user is about to hear.
    const TrackTiming timing = track(16, Speed::x1, kSwingMin);
    const int loop_ticks = timing.loop_ticks();
    CHECK_EQ(loop_ticks, 384);

    bool all_forward = true;
    for (int before = 1; before <= 3; before++) {
        const Landing landing =
            quantize_hit(stamp_at(uint32_t(loop_ticks - before), 0.0f), timing, 0);
        if (int(landing.step) != 0) all_forward = false;
        if (int(landing.loop) != 1) all_forward = false;
        if (int(landing.micro) != -before) all_forward = false;
        if (landing.clamped) all_forward = false;
    }
    CHECK(all_forward);

    // The symmetric case, just after step 0: the same step and pass, micro the other way.
    bool all_back = true;
    for (int after = 1; after <= 3; after++) {
        const Landing landing =
            quantize_hit(stamp_at(uint32_t(loop_ticks + after), 0.0f), timing, 0);
        if (int(landing.step) != 0) all_back = false;
        if (int(landing.loop) != 1) all_back = false;
        if (int(landing.micro) != after) all_back = false;
    }
    CHECK(all_back);

    // At the very start of the transport there is no earlier pass to fall into, so the loop
    // number must not underflow into four billion.
    const Landing first = quantize_hit(stamp_at(0, 0.0f), timing, 0);
    CHECK_EQ(int(first.step), 0);
    CHECK_EQ(int(first.loop), 0);
    CHECK_EQ(int(first.micro), 0);

    // Nor when the hit is inside the first step but before its own grid position is reachable.
    const Landing nudged = quantize_hit(stamp_at(0, 0.5f), timing, 0);
    CHECK_EQ(int(nudged.loop), 0);
    CHECK_EQ(int(nudged.step), 0);
    CHECK_EQ(int(nudged.micro), 1);
}

TEST(the_wrap_follows_swing_at_the_loop_boundary) {
    // With swing the last step of an even-length track sits late, which moves the point where
    // a hit starts belonging to the next pass. At swing 75 and 1x, step 15 is at 372 and the
    // next pass's step 0 at 384, so the crossover is 378.
    const TrackTiming timing = track(16, Speed::x1, 75);
    CHECK_EQ(effective_tick(15, timing, nullptr), 372);

    const Landing stays = quantize_hit(stamp_at(377, 0.0f), timing, 0);
    CHECK_EQ(int(stays.step), 15);
    CHECK_EQ(int(stays.loop), 0);
    CHECK_EQ(int(stays.micro), 5);

    const Landing crosses = quantize_hit(stamp_at(379, 0.0f), timing, 0);
    CHECK_EQ(int(crosses.step), 0);
    CHECK_EQ(int(crosses.loop), 1);
    CHECK_EQ(int(crosses.micro), -5);
}

TEST(ties_go_to_the_earlier_candidate) {
    // Dead between two steps. Which one wins matters less than that the same hit always picks
    // the same one: a unit recording and a host test have to agree.
    const TrackTiming fast = track(16, Speed::x2, kSwingMin);      // 12-tick steps
    const Landing chosen = quantize_hit(stamp_at(6, 0.0f), fast, 0);
    CHECK_EQ(int(chosen.step), 0);
    CHECK_EQ(int(chosen.micro), 6);
    CHECK(!chosen.clamped);

    // The same rule at the loop boundary, where it decides whether a hit wraps at all: exactly
    // halfway between the last step and the next pass's step 0, the earlier one wins.
    const TrackTiming timing = track(16, Speed::x1, kSwingMin);
    const Landing edge = quantize_hit(stamp_at(372, 0.0f), timing, 0);
    CHECK_EQ(int(edge.step), 15);
    CHECK_EQ(int(edge.loop), 0);
    CHECK_EQ(int(edge.micro), kMicroRange);      // +12 does not fit in micro
    CHECK(edge.clamped);

    // One tick later it is no longer a tie and the hit goes forward.
    const Landing past = quantize_hit(stamp_at(373, 0.0f), timing, 0);
    CHECK_EQ(int(past.step), 0);
    CHECK_EQ(int(past.loop), 1);
}

// --- the sweep ------------------------------------------------------------

TEST(the_sweep_holds_at_every_speed) {
    for (int speed = 0; speed < int(Speed::count); speed++) {
        check_sweep(track(16, Speed(speed), kSwingMin), 3, 0);
        check_sweep(track(16, Speed(speed), 66), 3, 0);
        check_sweep(track(16, Speed(speed), kSwingMax), 3, 0);
    }
}

TEST(the_sweep_holds_for_polymeter_and_odd_speeds) {
    // A 7-step track at 3/4 speed: the loop is 224 ticks, lines up with no bar, and the swing
    // pattern restarts on an even step mid-phrase. A 64-step track at 2x is the other extreme.
    check_sweep(track(7, Speed::x3_4, 66), 4, 0);
    check_sweep(track(7, Speed::x3_4, kSwingMax), 4, 50);
    check_sweep(track(64, Speed::x2, 58), 2, 0);
    check_sweep(track(64, Speed::x2, kSwingMax), 2, 100);
    check_sweep(track(1, Speed::x1, kSwingMin), 4, 0);       // a one-step loop
    check_sweep(track(3, Speed::x1_8, kSwingMax), 2, 0);     // 192-tick steps, the coarsest grid
}

TEST(straight_time_never_quantizes_further_than_half_a_step) {
    // check_sweep()'s bound widens with swing, because swing makes the grid uneven. With no
    // swing it is the textbook half a step, at every length.
    const int lengths[] = {1, 7, 16, 64};
    for (int length : lengths) {
        const TrackTiming timing = track(length, Speed::x1, kSwingMin);
        const SweepResult result = sweep(timing, 3, 0);
        CHECK(result.worst_distance <= 0.5f * float(timing.step_ticks()) + 0.001f);
    }
}

TEST(quantize_hit_is_deterministic) {
    const TrackTiming timing = track(13, Speed::x3_4, 71);

    bool repeatable = true;
    for (int tick = 0; tick < timing.loop_ticks() * 2; tick++) {
        const Stamp hit = stamp_at(uint32_t(tick), 0.3f);
        const Landing a = quantize_hit(hit, timing, 40);
        const Landing b = quantize_hit(hit, timing, 40);
        if (a.step != b.step || a.loop != b.loop || a.micro != b.micro ||
            a.clamped != b.clamped) {
            repeatable = false;
        }
    }
    CHECK(repeatable);

    // And across other work in between, so nothing is carried between calls.
    const Stamp hit = stamp_at(100, 0.25f);
    const Landing before = quantize_hit(hit, timing, 37);
    sweep(timing, 2, 0);
    const Landing after = quantize_hit(hit, timing, 37);
    CHECK_EQ(int(before.step), int(after.step));
    CHECK_EQ(int(before.loop), int(after.loop));
    CHECK_EQ(int(before.micro), int(after.micro));
    CHECK_EQ(before.clamped, after.clamped);
}

// --- stamps ---------------------------------------------------------------

TEST(ticks_per_ms_matches_the_grid) {
    // 130 BPM is 0.208 ticks per ms, so the spec's 6 ms pad latency is 1.248 ticks -- more than
    // a tenth of a step at 2x, which is why it is compensated rather than ignored.
    CHECK_NEAR(ticks_per_ms(130.0f), 0.208f, 1e-5);
    CHECK_NEAR(ticks_per_ms(60.0f), 0.096f, 1e-5);           // at 60 BPM a tick is 1/96 s
    CHECK_EQ(ticks_per_ms(0.0f), 0.0f);
    CHECK_NEAR(ticks_per_ms(300.0f), 5.0f * ticks_per_ms(60.0f), 1e-5);
    CHECK_NEAR(kPadLatencyMs * ticks_per_ms(130.0f), 1.248f, 1e-4);
}

TEST(stamp_normalise_folds_the_fraction_into_ticks) {
    const Stamp whole = stamp_normalise(stamp_at(5, 1.0f));
    CHECK_EQ(whole.tick, 6u);
    CHECK_EQ(whole.fraction, 0.0f);

    const Stamp over = stamp_normalise(stamp_at(5, 1.9f));
    CHECK_EQ(over.tick, 6u);
    CHECK_NEAR(over.fraction, 0.9f, 1e-6);

    // A negative fraction borrows a whole tick: this is the case int(x) gets wrong, and the one
    // latency compensation produces every time.
    const Stamp borrowed = stamp_normalise(stamp_at(5, -0.3f));
    CHECK_EQ(borrowed.tick, 4u);
    CHECK_NEAR(borrowed.fraction, 0.7f, 1e-6);

    const Stamp borrowed_more = stamp_normalise(stamp_at(5, -2.5f));
    CHECK_EQ(borrowed_more.tick, 2u);
    CHECK_NEAR(borrowed_more.fraction, 0.5f, 1e-6);

    // Already normal, so nothing moves.
    const Stamp untouched = stamp_normalise(stamp_at(9, 0.25f));
    CHECK_EQ(untouched.tick, 9u);
    CHECK_EQ(untouched.fraction, 0.25f);

    // Never below tick 0, fraction 0.
    const Stamp floored = stamp_normalise(stamp_at(0, -0.3f));
    CHECK_EQ(floored.tick, 0u);
    CHECK_EQ(floored.fraction, 0.0f);

    const Stamp way_under = stamp_normalise(stamp_at(3, -100.0f));
    CHECK_EQ(way_under.tick, 0u);
    CHECK_EQ(way_under.fraction, 0.0f);
}

TEST(stamp_normalise_is_idempotent) {
    // app/ may normalise a stamp it has adjusted and hand it on, and quantize_hit() normalises
    // again, so a second pass must not move anything.
    const float fractions[] = {-2.5f, -0.3f, 0.0f, 0.25f, 0.999f, 1.0f, 1.9f, 7.5f};
    bool stable = true;
    for (float fraction : fractions) {
        const Stamp once = stamp_normalise(stamp_at(20, fraction));
        const Stamp twice = stamp_normalise(once);
        if (once.tick != twice.tick || once.fraction != twice.fraction) stable = false;
        if (!(once.fraction >= 0.0f && once.fraction < 1.0f)) stable = false;
    }
    CHECK(stable);
}

TEST(stamp_back_moves_the_hit_to_when_the_pad_was_struck) {
    const float bpm = 130.0f;

    // Across a tick boundary: 0.5 - 1.248 is negative, so the borrow has to come out of `tick`.
    const Stamp crossed = stamp_back(stamp_at(10, 0.5f), kPadLatencyMs, bpm);
    CHECK_EQ(crossed.tick, 9u);
    CHECK_NEAR(crossed.fraction, 0.252f, 1e-3);

    // Across several: 50 ms is 10.4 ticks, so tick 20 becomes 9.6.
    const Stamp far = stamp_back(stamp_at(20, 0.0f), 50.0f, bpm);
    CHECK_EQ(far.tick, 9u);
    CHECK_NEAR(far.fraction, 0.6f, 1e-3);

    // Zero latency is the identity, so a unit with no measured latency records untouched.
    const Stamp same = stamp_back(stamp_at(7, 0.25f), 0.0f, bpm);
    CHECK_EQ(same.tick, 7u);
    CHECK_EQ(same.fraction, 0.25f);

    // A hit 10 ms into a transport that just started cannot move back before tick 0.
    const Stamp just_started = stamp_at(2, 0.08f);          // 10 ms at 130 BPM is 2.08 ticks
    const Stamp clamped = stamp_back(just_started, 100.0f, bpm);
    CHECK_EQ(clamped.tick, 0u);
    CHECK_EQ(clamped.fraction, 0.0f);

    // The clamp is a floor, not a snap: a hit that lands just after tick 0 keeps its fraction.
    const Stamp onto_zero = stamp_back(just_started, 9.0f, bpm);
    CHECK_EQ(onto_zero.tick, 0u);
    CHECK_NEAR(onto_zero.fraction, 0.208f, 1e-3);

    // Slower tempo, more ticks of the same latency -- the compensation is in musical time.
    const Stamp at_300 = stamp_back(stamp_at(10, 0.0f), 10.0f, 300.0f);
    const Stamp at_60 = stamp_back(stamp_at(10, 0.0f), 10.0f, 60.0f);
    CHECK_EQ(at_300.tick, 5u);        // 10 ms is 4.8 ticks at 300 BPM
    CHECK_EQ(at_60.tick, 9u);         // and 0.96 of one at 60
}

TEST(a_latency_compensated_hit_records_on_the_step_it_was_played) {
    // The whole chain, which is what app/ calls: the pad was struck dead on swung step 3, the
    // event reached the main context 6 ms later, and the recording has to land back on step 3
    // with no micro at all.
    const TrackTiming timing = track(16, Speed::x1, 66);
    const float bpm = 130.0f;
    const int struck = effective_tick(3, timing, nullptr);
    CHECK_EQ(struck, 80);                                    // 72 nominal, 8 of swing

    Stamp dispatched = stamp_at(uint32_t(struck), 0.0f);
    dispatched.fraction += kPadLatencyMs * ticks_per_ms(bpm);
    dispatched = stamp_normalise(dispatched);
    CHECK_EQ(dispatched.tick, uint32_t(struck + 1));         // 1.248 ticks later

    const Landing landing = quantize_hit(stamp_back(dispatched, kPadLatencyMs, bpm), timing, 0);
    CHECK_EQ(int(landing.step), 3);
    CHECK_EQ(int(landing.micro), 0);
    CHECK(!landing.clamped);

    // Without the compensation the same hit records a tick late, which is what the latency
    // figure in the spec buys.
    const Landing uncompensated = quantize_hit(dispatched, timing, 0);
    CHECK_EQ(int(uncompensated.step), 3);
    CHECK_EQ(int(uncompensated.micro), 1);
}

// --- robustness -----------------------------------------------------------

TEST(recording_clamps_degenerate_input) {
    // A zeroed or corrupt length must not divide by zero or index outside the pattern. The
    // clamp is core/timing's, and recording follows it rather than inventing its own.
    const TrackTiming zero_length = track(0, Speed::x1, kSwingMin);
    CHECK_EQ(zero_length.loop_ticks(), 24);
    const Landing from_zero = quantize_hit(stamp_at(97, 0.5f), zero_length, 0);
    CHECK_EQ(int(from_zero.step), 0);
    CHECK_EQ(int(from_zero.loop), 4);           // a one-step loop is 24 ticks, so 97.5 is pass 4

    const TrackTiming over_length = track(200, Speed::x1, kSwingMin);
    const Landing from_over = quantize_hit(stamp_at(50, 0.0f), over_length, 0);
    CHECK(int(from_over.step) < kMaxSteps);
    CHECK_EQ(int(from_over.loop), 0);

    // A percentage outside 0-100 is clamped, not wrapped: a negative one must not amplify the
    // offset and 500 must not invert it.
    const TrackTiming timing = track(8, Speed::x1_4, kSwingMin);
    const Stamp hit = stamp_at(uint32_t(effective_tick(1, timing, nullptr) + 9), 0.0f);
    CHECK_EQ(int(quantize_hit(hit, timing, -50).micro), int(quantize_hit(hit, timing, 0).micro));
    CHECK_EQ(int(quantize_hit(hit, timing, 500).micro), 0);

    // An out-of-range swing is clamped the same way, so a hit on the clamped position is still
    // micro 0 rather than a few ticks out.
    const TrackTiming wild = track(16, Speed::x1, 200);
    const Landing on_wild =
        quantize_hit(stamp_at(uint32_t(effective_tick(1, wild, nullptr)), 0.0f), wild, 0);
    CHECK_EQ(int(on_wild.step), 1);
    CHECK_EQ(int(on_wild.micro), 0);

    // A fraction that arrives out of range is folded rather than trusted: tick 24 with a
    // fraction of -0.5 is 23.5 ticks past step 0 of this 96-tick grid, which is too far to
    // store.
    const Landing folded = quantize_hit(stamp_at(24, -0.5f), timing, 0);
    CHECK_EQ(int(folded.step), 0);
    CHECK_EQ(int(folded.loop), 0);
    CHECK_EQ(int(folded.micro), kMicroRange);
    CHECK(folded.clamped);
}

TEST(a_long_recording_stays_consistent) {
    // Four minutes or so at 130 BPM, to be sure the tick reduction does not drift or overflow
    // as the loop number grows.
    const TrackTiming timing = track(16, Speed::x1, 62);
    const int loop_ticks = timing.loop_ticks();
    bool all_on_the_grid = true;

    for (int loop = 0; loop < 200; loop++) {
        for (int step = 0; step < 16; step++) {
            const Landing landing = quantize_hit(on_step(step, loop, timing), timing, 0);
            if (int(landing.step) != step || int(landing.loop) != loop || landing.micro != 0) {
                all_on_the_grid = false;
            }
        }
    }
    CHECK(all_on_the_grid);

    // And far out, where a 16-bit tick would have wrapped long ago.
    const Landing distant = quantize_hit(stamp_at(uint32_t(100000 * loop_ticks), 0.0f), timing, 0);
    CHECK_EQ(int(distant.step), 0);
    CHECK_EQ(int(distant.loop), 100000);
    CHECK_EQ(int(distant.micro), 0);
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(stamp_normalise_carries_a_fraction_that_rounds_up_to_a_whole_tick) {
    // -1e-9 floors to -1, and 1 - 1e-9 is 1.0f in a float: the fold has to carry it, or the
    // stamp would land a whole tick early with fraction 0.
    const Stamp tiny = stamp_normalise(stamp_at(5, -1.0e-9f));
    CHECK_EQ(tiny.tick, 5u);
    CHECK_EQ(tiny.fraction, 0.0f);
}
