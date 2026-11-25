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
