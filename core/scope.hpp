// The master output for the screen's scopes and spectra (live visuals, part 2).
//
// The audio side pushes every block's finished output, every second frame kept (24 kHz at a
// 48 kHz rate, plenty for a 128-pixel picture), as int16 left and right into a single-producer
// single-consumer ring. The main context takes everything new once a frame and keeps the most
// recent kHistory frames in a ScopeHistory of its own, which is what a scope, a vectorscope or a
// spectrum draws from. So the two sides share only the ring, with an acquire/release index each
// way (CLAUDE.md: cross-context state through single-writer atomics or SPSC rings), and a draw
// never reads memory the audio side is writing.
//
// When the main context is not taking (Enaga's own menu is up, so draw() is not called), the
// ring fills and the audio side drops what does not fit, counting it: a picture missing a moment
// is harmless, and the audio side never waits.
//
// Cost on the audio side: 32 frames a block through strata_scope_decimate (core/scopeloops.ti.c),
// a few cycles each, about 0.05% of a block. Memory: 8 KiB in the ring, 4 KiB in the history.
#pragma once

#include <atomic>
#include <cstdint>

namespace strata {

class ScopeRing {
public:
    static constexpr uint32_t kFrames = 2048;          // a power of two: the wrap is a mask
    static constexpr uint32_t kDecimate = 2;
    static_assert((kFrames & (kFrames - 1)) == 0, "the ring's size is a power of two");

    // Audio context only. `frames` of interleaved stereo; frame 0, 2, 4 ... are kept.
    void push(const float *interleaved, uint32_t frames);

    // Main context only. Up to `max` frames, oldest first; returns how many.
    uint32_t take(int16_t *left, int16_t *right, uint32_t max);

    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kMask = kFrames - 1;
    int16_t left_[kFrames] = {};
    int16_t right_[kFrames] = {};
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    std::atomic<uint32_t> dropped_{0};
};

// The main context's own copy of the most recent output: what the screens draw from.
class ScopeHistory {
public:
    static constexpr uint32_t kHistory = 1024;         // 42.7 ms at 24 kHz
    static_assert((kHistory & (kHistory - 1)) == 0, "the history's size is a power of two");

    // Everything new from the ring. Main context, once a frame.
    void drain(ScopeRing &ring);

    // `ago` frames back from the newest (0 the newest), as -32768..32767. Past kHistory, 0.
    int16_t left(uint32_t ago) const { return ago < kHistory ? left_[(head_ - 1u - ago) & kMask] : 0; }
    int16_t right(uint32_t ago) const { return ago < kHistory ? right_[(head_ - 1u - ago) & kMask] : 0; }
    // How many frames have ever arrived (wraps; for "has anything new come").
    uint32_t arrived() const { return head_; }

private:
    static constexpr uint32_t kMask = kHistory - 1;
    int16_t left_[kHistory] = {};
    int16_t right_[kHistory] = {};
    uint32_t head_ = 0;
};

}  // namespace strata
