// dsp/frozen: a frozen track's loop player (Freeze, 2026-11-23).
//
// The things that can go wrong are the ones that put a loop out of time or click: reading past
// the end, a resync that lands a frame off, a stop that cuts instead of fading. Each is checked
// against the loop's own samples, which are a ramp so every value says which frame it was.
#include "check.hpp"

#include <cstdint>

#include "frozen.hpp"
#include "frozenloops.h"

using namespace strata;

namespace {

constexpr int kBlk = 64;
constexpr uint32_t kFrames = 1000;      // a short loop: wraps every 15.6 blocks
int16_t g_loop[kFrames];

void fill_loop() {
    for (uint32_t i = 0; i < kFrames; i++) g_loop[i] = int16_t(i * 16u);
}

float frame_value(uint32_t i) { return float(int16_t((i % kFrames) * 16u)) * (1.0f / 32768.0f); }

}  // namespace

TEST(frozen_refuses_what_it_cannot_play) {
    fill_loop();
    FrozenLoop loop;
    CHECK(!loop.set(nullptr, kFrames, 1, 0, 0, 0));
    CHECK(!loop.set(g_loop, 0, 1, 0, 0, 0));
    CHECK(!loop.set(g_loop, kFrames, 3, 0, 0, 0));
    CHECK(!loop.set(g_loop, kFrames, 16, 0, 0, 0));
    CHECK(!loop.on());
    CHECK(loop.set(g_loop, kFrames, 8, 0, 0, 0));
    CHECK(loop.on());
    loop.clear();
    CHECK(!loop.on());
}

TEST(frozen_plays_the_loop_in_order_and_wraps_without_a_gap) {
    fill_loop();
    FrozenLoop loop;
    CHECK(loop.set(g_loop, kFrames, 1, 0, 0, 0));
    float out[kBlk];
    // The first block fades in from silence; after it, every frame is the loop's own value.
    for (float &x : out) x = 0.0f;
    loop.render_add(out, kBlk, true);
    CHECK_EQ(out[0], 0.0f);
    CHECK(out[kBlk - 1] > 0.0f);
    uint32_t at = kBlk;
    for (int b = 0; b < 40; b++) {               // well past two wraps
        for (float &x : out) x = 0.0f;
        loop.render_add(out, kBlk, true);
        for (int i = 0; i < kBlk; i++) CHECK_EQ(out[i], frame_value(at + uint32_t(i)));
        at += kBlk;
    }
    CHECK_EQ(loop.position(), at % kFrames);
}

TEST(frozen_adds_into_the_block_rather_than_replacing_it) {
    fill_loop();
    FrozenLoop loop;
    CHECK(loop.set(g_loop, kFrames, 1, 0, 0, 0));
    float out[kBlk];
    for (float &x : out) x = 0.0f;
    loop.render_add(out, kBlk, true);            // past the fade-in
    for (float &x : out) x = 0.25f;
    loop.render_add(out, kBlk, true);
    CHECK_EQ(out[3], 0.25f + frame_value(kBlk + 3));
}

TEST(frozen_resyncs_on_its_own_bars_to_the_bar_lines_frame) {
    fill_loop();
    FrozenLoop loop;
    // A two-bar loop whose first bar line (bar 10) was loop frame 40.
    CHECK(loop.set(g_loop, kFrames, 2, 10, 40, 10));
    CHECK_EQ(loop.position(), 40u);
    loop.skip(300);
    // Bar 11 is the loop's second bar: no resync.
    loop.on_bar(11, 5);
    CHECK_EQ(loop.position(), 340u);
    // Bar 12 is its first again: the bar line, 5 frames into the block, is frame 40, so the
    // block starts at 35.
    loop.on_bar(12, 5);
    CHECK_EQ(loop.position(), 35u);
    // A bar line 30 frames into the block when the loop's bar is at frame 20: back past 0.
    CHECK(loop.set(g_loop, kFrames, 1, 0, 20, 0));
    loop.on_bar(7, 30);
    CHECK_EQ(loop.position(), kFrames - 10u);
    // And one that was BEFORE the block (the first tick after a start, mid-bar).
    loop.on_bar(8, -100);
    CHECK_EQ(loop.position(), 120u);
}

TEST(frozen_a_lost_read_comes_back_on_the_first_bar_after_a_start) {
    // A four-bar loop of 10,000-frame bars that began on bar 5 at loop frame 100. Only the read
    // position is checked, so nothing renders and the short test buffer stands in for it.
    fill_loop();
    FrozenLoop loop;
    constexpr uint32_t kBig = 40000;
    CHECK(loop.set(g_loop, kBig, 4, 5, 100, 5));
    loop.skip(12345);                             // played a while, then stopped and started
    CHECK_EQ(loop.position(), 12445u);
    // The start puts the clock back on bar 0, the loop's fourth bar ((0 - 5) mod 4 = 3), not
    // one of its own: before, it ran on out of time until bar 1. Now the read goes to where
    // bar 0 belongs, 3 x 10,000 past the lead.
    loop.on_bar(0, 0);
    CHECK_EQ(loop.position(), 30100u);
    // Bar 1 is the loop's own: exact, as always (the bar line 3 frames into the block).
    loop.skip(10000);
    loop.on_bar(1, 3);
    CHECK_EQ(loop.position(), 97u);
    // A few frames of rounding or clock jitter on a bar between is left alone: no jump.
    loop.skip(10000);
    loop.on_bar(2, 0);
    CHECK_EQ(loop.position(), 10097u);
    // A relocate mid-loop (bar 7 is its third bar) that leaves the read far off moves it.
    loop.skip(2000);
    loop.on_bar(7, -40);
    CHECK_EQ(loop.position(), 100u + 20000u + 40u);
}

TEST(frozen_a_lead_outside_the_loop_wraps_and_set_starts_where_the_clock_is) {
    fill_loop();
    FrozenLoop loop;
    CHECK(loop.set(g_loop, kFrames, 4, 0, -30, 0));
    CHECK_EQ(loop.position(), kFrames - 30u);
    // Set during the third bar of a four-bar loop (bar 6 from origin 4): two bars of 250 in.
    CHECK(loop.set(g_loop, kFrames, 4, 4, 10, 6));
    CHECK_EQ(loop.position(), 510u);
    CHECK(loop.set(g_loop, kFrames, 1, 0, int32_t(kFrames) + 7, 0));
    CHECK_EQ(loop.position(), 7u);
}

TEST(frozen_a_stop_fades_out_over_one_block_and_stays_silent) {
    fill_loop();
    FrozenLoop loop;
    CHECK(loop.set(g_loop, kFrames, 1, 0, 500, 0));
    float out[kBlk];
    for (float &x : out) x = 0.0f;
    loop.render_add(out, kBlk, true);
    for (float &x : out) x = 0.0f;
    loop.render_add(out, kBlk, false);            // the fade: full at the start, 0 by the end
    CHECK_EQ(out[0], frame_value(500 + kBlk));
    CHECK(out[kBlk - 1] < out[0] * 0.05f);
    for (int b = 0; b < 3; b++) {
        for (float &x : out) x = 0.0f;
        loop.render_add(out, kBlk, false);
        for (int i = 0; i < kBlk; i++) CHECK_EQ(out[i], 0.0f);
    }
    // Still advancing while silent, so it comes back in time.
    CHECK_EQ(loop.position(), (500u + 5u * kBlk) % kFrames);
}

TEST(frozen_the_read_loop_ramps_its_gain_across_the_run) {
    static const int16_t src[4] = {16384, 16384, -16384, 0};
    float dst[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    strata_frozen_add(dst, src, 4, 0.5f / 16384.0f, 0.25f / 16384.0f);
    CHECK_EQ(dst[0], 1.5f);
    CHECK_EQ(dst[1], 1.75f);
    CHECK_EQ(dst[2], 0.0f);
    CHECK_EQ(dst[3], 1.0f);
}
