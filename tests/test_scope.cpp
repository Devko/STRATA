// core/scope: the master output for the screen's scopes.
#include "scope.hpp"

#include "check.hpp"

using namespace strata;

namespace {

// A block of interleaved stereo whose frame f is (f / 10000, -f / 10000), from `first`.
void ramp_block(float *out, uint32_t frames, uint32_t first) {
    for (uint32_t f = 0; f < frames; f++) {
        out[2 * f] = float(first + f) / 10000.0f;
        out[2 * f + 1] = -float(first + f) / 10000.0f;
    }
}

int16_t expect(uint32_t frame) { return int16_t(float(frame) / 10000.0f * 32767.0f); }

ScopeRing g_ring;            // 8 KiB each: file scope, not the stack
ScopeHistory g_history;

}  // namespace

TEST(scope_the_ring_keeps_every_second_frame_in_order) {
    static ScopeRing ring;
    float block[128];
    ramp_block(block, 64, 0);
    ring.push(block, 64);
    int16_t l[64], r[64];
    const uint32_t n = ring.take(l, r, 64);
    CHECK_EQ(n, 32u);
    for (uint32_t i = 0; i < n; i++) {
        CHECK_EQ(l[i], expect(2 * i));
        CHECK_EQ(r[i], int16_t(-expect(2 * i)));
    }
    CHECK_EQ(ring.take(l, r, 64), 0u);      // taken is gone
}

TEST(scope_the_ring_clamps_past_full_scale) {
    static ScopeRing ring;
    float block[4] = {3.0f, -3.0f, 0.0f, 0.0f};
    ring.push(block, 2);
    int16_t l[1], r[1];
    CHECK_EQ(ring.take(l, r, 1), 1u);
    CHECK_EQ(l[0], 32767);
    CHECK_EQ(r[0], -32768);
}

TEST(scope_the_ring_wraps_and_drops_what_does_not_fit_without_waiting) {
    static ScopeRing ring;
    float block[128];
    int16_t l[ScopeRing::kFrames], r[ScopeRing::kFrames];
    // Fill most of it and take it, so the write position sits near the wrap.
    for (uint32_t b = 0; b < 60; b++) {
        ramp_block(block, 64, b * 64);
        ring.push(block, 64);
    }
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), 60u * 32u);
    // Across the wrap, in order.
    for (uint32_t b = 0; b < 8; b++) {
        ramp_block(block, 64, 3840 + b * 64);
        ring.push(block, 64);
    }
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), 8u * 32u);
    for (uint32_t i = 0; i < 256; i++) CHECK_EQ(l[i], expect(3840 + 2 * i));
    // Nobody taking: the ring fills and the rest is counted, not waited for.
    for (uint32_t b = 0; b < 100; b++) {
        ramp_block(block, 64, 0);
        ring.push(block, 64);
    }
    CHECK_EQ(ring.dropped(), 100u * 32u - ScopeRing::kFrames);
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), ScopeRing::kFrames);
}

TEST(scope_the_history_holds_the_newest_frames_newest_first) {
    float block[128];
    for (uint32_t b = 0; b < 50; b++) {           // 1,600 frames: more than the history holds
        ramp_block(block, 64, b * 64);
        g_ring.push(block, 64);
        if (b % 7 == 6) g_history.drain(g_ring);   // drained now and then, as draw() does
    }
    g_history.drain(g_ring);
    CHECK_EQ(g_history.arrived(), 1600u);
    CHECK_EQ(g_history.left(0), expect(3198));     // the newest kept frame, 2 x 1599
    CHECK_EQ(g_history.left(1), expect(3196));
    CHECK_EQ(g_history.right(0), int16_t(-expect(3198)));
    CHECK_EQ(g_history.left(ScopeHistory::kHistory - 1), expect(3198 - 2 * (ScopeHistory::kHistory - 1)));
    CHECK_EQ(g_history.left(ScopeHistory::kHistory), 0);   // past the history
}

// A push that straddles the ring's end splits in the right place, a push that half fits counts
// only the half it drops, and one drain takes everything waiting even past the history's wrap.
TEST(scope_a_push_across_the_end_and_a_drain_past_the_wrap_lose_nothing) {
    static ScopeRing ring;
    float block[2 * 64];
    int16_t l[ScopeRing::kFrames], r[ScopeRing::kFrames];
    ramp_block(block, 2, 0);
    ring.push(block, 2);                                 // one frame: the blocks no longer align
    for (uint32_t b = 0; b < 63; b++) {
        ramp_block(block, 64, b * 64);
        ring.push(block, 64);
    }
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), 1u + 63u * 32u);
    // The write position is 2017: this block's 32 frames go 31 before the end and 1 after.
    ramp_block(block, 64, 5000);
    ring.push(block, 64);
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), 32u);
    for (uint32_t i = 0; i < 32; i++) {
        CHECK_EQ(l[i], expect(5000 + 2 * i));
        CHECK_EQ(r[i], int16_t(-expect(5000 + 2 * i)));
    }
    // Room for 16 frames, a block of 32: 16 kept, 16 dropped.
    for (uint32_t b = 0; b < 63; b++) ring.push(block, 64);
    ring.push(block, 32);
    CHECK_EQ(ring.dropped(), 0u);
    ring.push(block, 64);
    CHECK_EQ(ring.dropped(), 16u);
    CHECK_EQ(ring.take(l, r, ScopeRing::kFrames), ScopeRing::kFrames);

    static ScopeRing full;
    static ScopeHistory history;
    for (uint32_t b = 0; b < 47; b++) {                  // 1,504 frames, more than the history
        ramp_block(block, 64, b * 64);
        full.push(block, 64);
    }
    history.drain(full);
    CHECK_EQ(history.arrived(), 1504u);
    CHECK_EQ(history.left(0), expect(2 * 1503));
    for (uint32_t b = 0; b < 20; b++) {                  // 640 more, from a head past the wrap
        ramp_block(block, 64, 4000 + b * 64);
        full.push(block, 64);
    }
    history.drain(full);
    CHECK_EQ(history.arrived(), 2144u);
    CHECK_EQ(history.left(0), expect(4000 + 2 * 639));
    CHECK_EQ(history.left(639), expect(4000));
    CHECK_EQ(history.left(640), expect(2 * 1503));
}
