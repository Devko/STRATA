// The sample memory budget: who owns which part of one big region, and what is still loading.
//
// docs/SPEC.md, "Samples": samples are "loaded whole into RAM as int16 at project open", with a
// budget of "32 MiB by default, raisable per project to the 64 MiB the memory budget allows", and
// "a track whose sample is still loading stays silent rather than glitching".
//
// Two jobs, both small and both easy to get quietly wrong:
//
//   1. Hand out ranges of one caller-owned region to at most kTracks slots, and take them back.
//   2. Remember what state each slot is in, so the engine can tell "no sample" from "not yet".
//
// ## Why a free list and not a bump pointer
//
// A bump pointer is what core/pattern.cpp's lock pool uses, and it works there because locks are
// two bytes and compaction can move them. Samples are megabytes and a voice may be reading one,
// so nothing here may ever move an allocation. Loading a different sample into one track of eight
// must therefore reuse that track's space rather than growing the high-water mark -- otherwise
// auditioning samples on one track exhausts 32 MiB after a handful of changes.
//
// With at most eight live ranges, a first-fit free list with coalescing is a few dozen lines and
// has no pathological case worth worrying about. Fragmentation is real but bounded: the worst
// case is eight holes, and `largest_free()` tells the caller what will actually fit before it
// starts a 4-second load that cannot land.
//
// ## Releasing is the caller's promise, not this file's problem
//
// `release` makes a range available again immediately. It cannot check whether a voice is still
// reading it -- core/ knows nothing about voices -- so the caller must silence the track first.
// app/ does that by clearing the machine's sample before releasing, and the state machine here is
// what makes that ordering expressible: a slot goes `ready` -> `empty` before its bytes are
// handed out again.
//
// Nothing here allocates or touches a file. The region's memory comes from the app, in init().
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// docs/SPEC.md's figures. Stored in MiB in the project file so a damaged byte cannot ask for 4
// GiB (core/project.hpp).
constexpr uint32_t kSampleBudgetDefaultMib = 32;
constexpr uint32_t kSampleBudgetMaxMib = 64;

// What a track's sample is doing. `loading` is the state that makes the spec's "stays silent
// rather than glitching" expressible; `failed` is kept distinct from `empty` so the UI can say
// why a track is silent instead of pretending nothing was asked for.
enum class SampleState : uint8_t { empty = 0, loading, ready, failed, count };

const char *sample_state_name(SampleState state);   // "", "LOAD", "OK", "FAIL"

struct SampleSlot {
    SampleState state = SampleState::empty;
    uint32_t offset = 0;        // into the region, in BYTES
    uint32_t bytes = 0;         // reserved, which may exceed what finally loaded
    uint32_t frames = 0;        // int16 frames actually present once `ready`
    uint32_t rate = 48000;      // after any resampling: the engine's rate
    uint8_t channels = 1;
};

class SamplePool {
public:
    // The region belongs to the caller and must outlive the pool. `bytes` is the budget, clamped
    // to kSampleBudgetMaxMib. App::init() only.
    void init(uint32_t bytes);

    // Change the budget (a project may raise it). Shrinking below what is already reserved is
    // refused rather than stranding a loaded sample: returns false and changes nothing.
    bool set_budget(uint32_t bytes);

    // Forget every reservation. The caller must have silenced every track first.
    void reset();

    // Reserve `bytes` for `slot` and mark it `loading`. Any range the slot already held is
    // released first, so re-loading a track reuses its own space instead of growing the pool.
    //
    // False when nothing contiguous is large enough, leaving the slot `empty` -- which is the
    // honest outcome, because a partially reserved sample is a sample that cannot play.
    bool reserve(int slot, uint32_t bytes);

    // A load finished. `frames`, `channels` and `rate` describe what is actually in the range,
    // which may be fewer frames than were reserved (a truncated file). Marks the slot `ready`.
    // False for a slot that was not `loading`.
    bool finish(int slot, uint32_t frames, int channels, uint32_t rate);

    // A load failed. The range is released and the slot becomes `failed`, so the UI can say so.
    void fail(int slot);

    // Give a slot's range back. The caller must have silenced the track first.
    void release(int slot);

    const SampleSlot &slot(int index) const;

    uint32_t budget() const { return budget_; }
    uint32_t used() const;
    uint32_t free_bytes() const { return budget_ - used(); }

    // The largest single range still available -- what will actually fit. Smaller than
    // `free_bytes()` once the region is fragmented, and the number a caller should check before
    // starting a load it cannot finish.
    uint32_t largest_free() const;

    // How many slots are in each state, for the screen and for tests.
    int count_in(SampleState state) const;

private:
    // A free range. At most kTracks + 1 can exist: eight allocations make at most nine holes.
    struct Hole {
        uint32_t offset = 0;
        uint32_t bytes = 0;
    };
    static constexpr int kMaxHoles = kTracks + 1;

    void add_hole(uint32_t offset, uint32_t bytes);
    void coalesce();

    SampleSlot slots_[kTracks];
    Hole holes_[kMaxHoles];
    int hole_count_ = 0;
    uint32_t budget_ = 0;
};

}  // namespace strata
