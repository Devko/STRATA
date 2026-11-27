// Ratchets that span the step's LEN (CyDrums review, Roland's choice 2026-11-26: RATE + span
// flag). core/pattern.hpp's kRatchetSpan, the scheduler's first hit, Engine::span_tick for the
// rest, and the step editor's walk past 8 hits into the rates.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"
#include "pattern.hpp"
#include "scheduler.hpp"
#include "stepedit.hpp"

#include <cstring>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;

namespace {

constexpr int kKick = 0;
// The rate indices, as kRatchetRateTicks orders them.
constexpr uint8_t k64 = 0, k32 = 2, k16 = 4, k8 = 6;
// Length indices (core/timing.hpp): 8 = one step, 12 = four steps, 16 = sixteen steps.
constexpr uint8_t kOneStep = 8, kFourSteps = 12, kBarLen = 16;

uint8_t triggers(int track) {
    return strata::test::engine().status().triggered[track].load(std::memory_order_relaxed);
}

int fired(uint32_t tick) {
    const uint8_t before = triggers(kKick);
    FakeHost::the().deliver_tick(tick, 0);
    pump();
    return int(uint8_t(triggers(kKick) - before));
}

// Which of ticks [from, to) the kick hit on, as a bit per tick from `from` (at most 64 ticks).
uint64_t hit_map(uint32_t from, uint32_t to) {
    uint64_t map = 0;
    for (uint32_t t = from; t < to; t++) {
        if (fired(t) > 0) map |= uint64_t(1) << (t - from);
    }
    return map;
}

Step &kick_step(Engine &engine, int step) {
    return engine.project_for_load().live().tracks[kKick].steps[step];
}

// The blank project, the clock stopped, and the kick's step `step` a trig rolling at `rate` for
// `length`.
Engine &rolling(uint8_t rate, uint8_t length, RatchetRamp ramp = RatchetRamp::none, int step = 0) {
    Engine &engine = fresh();
    FakeHost::the().set_running(false);
    pump();
    Step &s = kick_step(engine, step);
    s.flags = kStepTrig;
    s.length = length;
    s.velocity = 100;
    s.ratchet = ratchet_pack_span(rate, ramp);
    pump();
    FakeHost::the().set_running(true);
    return engine;
}

}  // namespace

// --- core -----------------------------------------------------------------

TEST(span_the_byte_packs_and_reads_back) {
    for (uint8_t rate = 0; rate < kRatchetRates; rate++) {
        for (uint8_t r = 0; r <= uint8_t(RatchetRamp::down); r++) {
            const uint8_t b = ratchet_pack_span(rate, RatchetRamp(r));
            CHECK(ratchet_spans(b));
            CHECK_EQ(ratchet_rate(b), rate);
            CHECK_EQ(int(ratchet_ramp(b)), int(r));
            CHECK_EQ(b & 0x08u, 0u);
        }
    }
    // A count byte is never a roll.
    for (uint8_t n = kRatchetMin; n <= kRatchetMax; n++) {
        CHECK(!ratchet_spans(ratchet_pack(n, RatchetRamp::down)));
    }
    CHECK_EQ(std::strcmp(ratchet_rate_name(k32), "1/32"), 0);
    CHECK_EQ(std::strcmp(ratchet_rate_name(3), "1/16T"), 0);
    CHECK_EQ(std::strcmp(ratchet_rate_name(7), "1/4"), 0);
}

TEST(span_sanitize_keeps_a_roll_and_clears_what_it_cannot_hold) {
    Step step{};
    step.ratchet = 0xDF;            // span, ramp 5, rate nibble 15
    step_sanitize(step);
    CHECK(ratchet_spans(step.ratchet));
    CHECK_EQ(ratchet_rate(step.ratchet), 7);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::none));
    CHECK_EQ(step.ratchet & 0x08u, 0u);
    step.ratchet = ratchet_pack_span(k32, RatchetRamp::down);
    step_sanitize(step);
    CHECK_EQ(step.ratchet, ratchet_pack_span(k32, RatchetRamp::down));
}

TEST(span_hit_count_is_len_over_the_rate) {
    // A 1x step is 24 ticks.
    CHECK_EQ(int(span_hit_count(kBarLen, 24, k32)), 32);      // the review's 1/32 over a bar
    CHECK_EQ(int(span_hit_count(kBarLen, 24, k16)), 16);
    CHECK_EQ(int(span_hit_count(kFourSteps, 24, k64)), 16);
    CHECK_EQ(int(span_hit_count(kFourSteps, 24, 3)), 6);      // 1/16T: 96 / 16
    CHECK_EQ(int(span_hit_count(kOneStep, 24, k8)), 1);       // slower than LEN: one hit
    // A 2x track's steps are 12 ticks: the same note value, half the hits over its LEN.
    CHECK_EQ(int(span_hit_count(kBarLen, 12, k32)), 16);
    // The longest LEN at the fastest rate does not overflow.
    CHECK_EQ(int(span_hit_count(uint8_t(kLengthMax), 192, k64)), 2048);
}

TEST(span_velocity_ramps_across_the_whole_roll) {
    const float inv = 1.0f / 4.0f;
    CHECK_EQ(int(span_velocity(100, 0, 4, inv, RatchetRamp::none)), 100);
    CHECK_EQ(int(span_velocity(100, 3, 4, inv, RatchetRamp::none)), 100);
    CHECK_EQ(int(span_velocity(100, 0, 4, inv, RatchetRamp::up)), 25);
    CHECK_EQ(int(span_velocity(100, 3, 4, inv, RatchetRamp::up)), 100);
    CHECK_EQ(int(span_velocity(100, 0, 4, inv, RatchetRamp::down)), 100);
    CHECK_EQ(int(span_velocity(100, 3, 4, inv, RatchetRamp::down)), 25);
    // Never 0, and an index past the end reads as the last hit.
    CHECK_EQ(int(span_velocity(1, 0, 32, 1.0f / 32.0f, RatchetRamp::up)), 1);
    CHECK_EQ(int(span_velocity(100, 99, 4, inv, RatchetRamp::up)), 100);
}

TEST(span_the_step_editor_walks_past_eight_into_the_rates) {
    Step step{};
    step.ratchet = ratchet_pack(kRatchetMax, RatchetRamp::none);
    char buf[kStepFieldTextMax];
    step_field_apply(step, StepField::ratchet_count, 3);      // 8 -> 1/64, 1/48, 1/32
    CHECK(ratchet_spans(step.ratchet));
    CHECK_EQ(ratchet_rate(step.ratchet), k32);
    step_field_text(step, StepField::ratchet_count, buf, int(sizeof buf));
    CHECK_EQ(std::strcmp(buf, "ROLL/32"), 0);
    CHECK_EQ(step_field_value(step, StepField::ratchet_count), int(kRatchetMax) + 1 + k32);
    // The ramp edit leaves the roll a roll.
    step_field_apply(step, StepField::ratchet_ramp, 1);
    CHECK(ratchet_spans(step.ratchet));
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::up));
}

// --- the engine -----------------------------------------------------------

TEST(span_a_roll_hits_at_its_rate_for_its_len) {
    // 1/32 (12 ticks) for four steps (96 ticks): 8 hits, on 0, 12 .. 84, and none after.
    rolling(k32, kFourSteps);
    uint64_t first = 0;
    for (uint32_t t = 0; t < 64; t += 12) first |= uint64_t(1) << t;   // 0 .. 60
    CHECK(hit_map(0, 64) == first);
    CHECK(hit_map(64, 128) == ((uint64_t(1) << 8) | (uint64_t(1) << 20)));  // 72, 84
    FakeHost::the().set_running(false);
    pump();
}

TEST(span_a_roll_counts_its_hits_and_the_ramp_climbs) {
    Engine &engine = rolling(k16, kBarLen, RatchetRamp::up);
    int hits = 0;
    for (uint32_t t = 0; t < 16 * 24; t++) hits += fired(t);
    CHECK_EQ(hits, 16);                                         // a 1/16 roll over a bar
    // The step plays again on the next pass, and its roll with it.
    hits = 0;
    const uint32_t pass = 64 * 24;
    for (uint32_t t = 16 * 24; t < pass; t++) hits += fired(t);
    CHECK_EQ(hits, 0);
    CHECK_EQ(fired(pass), 1);
    (void)engine;
    FakeHost::the().set_running(false);
    pump();
}

TEST(span_a_new_trig_on_the_track_ends_the_roll) {
    // 1/16 for a bar from step 1, and a plain trig on step 5: the roll stops there, and the
    // track plays step 5's one note.
    Engine &engine = rolling(k16, kBarLen);
    Step &five = kick_step(engine, 4);
    five.flags = kStepTrig;
    five.length = kOneStep;
    pump();
    int hits = 0;
    for (uint32_t t = 0; t < 4 * 24; t++) hits += fired(t);
    CHECK_EQ(hits, 4);                                          // 0, 24, 48, 72
    CHECK_EQ(fired(4 * 24), 1);                                 // step 5 itself, once
    hits = 0;
    for (uint32_t t = 4 * 24 + 1; t < 16 * 24; t++) hits += fired(t);
    CHECK_EQ(hits, 0);                                          // the roll is gone
    FakeHost::the().set_running(false);
    pump();
}

TEST(span_a_stop_or_a_restart_ends_the_roll) {
    rolling(k16, kBarLen);
    CHECK_EQ(fired(0), 1);
    CHECK_EQ(fired(24), 1);
    post(Cmd::stop);
    pump();
    // Ticks past the stop (as if the clock ran on): no hit of the old roll.
    int hits = 0;
    for (uint32_t t = 25; t < 200; t++) hits += fired(t);
    CHECK_EQ(hits, 0);
    FakeHost::the().set_running(false);
    pump();

    rolling(k16, kBarLen);
    CHECK_EQ(fired(0), 1);
    post(Cmd::restart);
    pump();
    // The restart starts the pattern over: the step and its roll again from tick 0.
    CHECK_EQ(fired(0), 1);
    CHECK_EQ(fired(24), 1);
    FakeHost::the().set_running(false);
    pump();
}

TEST(span_a_slower_rate_than_len_is_one_hit) {
    rolling(k8, kOneStep);
    int hits = 0;
    for (uint32_t t = 0; t < 200; t++) hits += fired(t);
    CHECK_EQ(hits, 1);
    FakeHost::the().set_running(false);
    pump();
}
