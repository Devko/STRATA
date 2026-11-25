// core/scope.hpp: the master output for the screen.
#include "scope.hpp"

#include "scopeloops.h"

namespace strata {

void ScopeRing::push(const float *interleaved, uint32_t frames) {
    const uint32_t write = write_.load(std::memory_order_relaxed);
    const uint32_t read = read_.load(std::memory_order_acquire);
    const uint32_t room = kFrames - (write - read);
    uint32_t n = frames / kDecimate;
    if (n > room) {
        // Single writer: a load and a store, no read-modify-write (app/commands.hpp, Status).
        dropped_.store(dropped_.load(std::memory_order_relaxed) + (n - room),
                       std::memory_order_relaxed);
        n = room;
    }
    if (n == 0) return;
    const uint32_t at = write & kMask;
    const uint32_t first = n < kFrames - at ? n : kFrames - at;
    strata_scope_decimate(interleaved, int(first), left_ + at, right_ + at);
    if (first < n) {
        strata_scope_decimate(interleaved + 2u * kDecimate * first, int(n - first), left_, right_);
    }
    write_.store(write + n, std::memory_order_release);   // publishes the frames with it
}

uint32_t ScopeRing::take(int16_t *left, int16_t *right, uint32_t max) {
    const uint32_t write = write_.load(std::memory_order_acquire);
    const uint32_t read = read_.load(std::memory_order_relaxed);
    uint32_t n = write - read;
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) {
        left[i] = left_[(read + i) & kMask];
        right[i] = right_[(read + i) & kMask];
    }
    read_.store(read + n, std::memory_order_release);
    return n;
}

void ScopeHistory::drain(ScopeRing &ring) {
    // Straight into the history, a run at a time up to its wrap: no buffer in between.
    for (;;) {
        const uint32_t at = head_ & kMask;
        const uint32_t got = ring.take(left_ + at, right_ + at, kHistory - at);
        head_ += got;
        if (got < kHistory - at) return;
    }
}

}  // namespace strata
