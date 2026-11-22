// The sample memory budget: who owns which part of the region, and what each track is doing.
//
// One invariant dominates this file: **no two live ranges may ever share a byte.** Everything
// else here is a convenience compared with that, because getting it wrong does not look like a
// bug in core/samplepool.cpp -- it looks like a bug in the voice, or in the loader, or in the
// file that happens to be on the second track, and it only shows up once two samples of the
// wrong sizes happen to land next to each other. So `sane()` below walks all eight slots and
// checks every pair, and it is called after every step of every test, including after the
// operations that are *supposed* to fail: a refused reserve that quietly carved a range anyway
// is exactly the shape of the bug this module exists not to have.
//
// The other reason these tests exist is the trade the header records. A bump pointer like
// core/pattern.cpp's lock pool cannot reclaim a track's space, so auditioning samples on one
// track would exhaust 32 MiB after a handful of changes; `samplepool_reload_one_slot_reuses_its_space`
// is that scenario written down, and it is what would fail if someone ever simplified this
// file into a high-water mark.
#include "check.hpp"

#include "samplepool.hpp"

#include <cstring>

namespace {

using namespace strata;

constexpr uint32_t kMib = 1024u * 1024u;

bool live(SampleState state) {
    return state == SampleState::loading || state == SampleState::ready;
}

// Every structural invariant of the pool, checked from the outside: no two live ranges overlap,
// every live range is non-empty and inside the budget, a slot that owns no bytes says so,
// `used()` agrees with the slots, and `largest_free()` is no larger than the free total (which
// catches a hole that was double-counted or that overlaps a live range -- the free list is
// private, but a phantom hole shows up here before it shows up as an overlapping reservation).
//
// Returns a bool rather than checking, so that `CHECK(sane(pool))` at the call site reports the
// line of the step that broke it; the detail goes to stdout.
bool sane(const SamplePool &pool) {
    bool ok = true;
    uint32_t total = 0;

    for (int a = 0; a < kTracks; a++) {
        const SampleSlot &sa = pool.slot(a);
        if (!live(sa.state)) {
            if (sa.bytes != 0) {
                std::printf("    slot %d is %s but still owns %u bytes\n", a,
                            sample_state_name(sa.state), sa.bytes);
                ok = false;
            }
            continue;
        }
        total += sa.bytes;
        if (sa.bytes == 0) {
            std::printf("    slot %d is live with a zero-length range\n", a);
            ok = false;
        }
        if (sa.offset > pool.budget() || sa.bytes > pool.budget() - sa.offset) {
            std::printf("    slot %d range [%u, %u) leaves the %u-byte region\n", a, sa.offset,
                        sa.offset + sa.bytes, pool.budget());
            ok = false;
        }
        for (int b = a + 1; b < kTracks; b++) {
            const SampleSlot &sb = pool.slot(b);
            if (!live(sb.state)) continue;
            if (sa.offset < sb.offset + sb.bytes && sb.offset < sa.offset + sa.bytes) {
                std::printf("    slots %d [%u, %u) and %d [%u, %u) overlap\n", a, sa.offset,
                            sa.offset + sa.bytes, b, sb.offset, sb.offset + sb.bytes);
                ok = false;
            }
        }
    }

    if (total != pool.used()) {
        std::printf("    used() says %u, the slots say %u\n", pool.used(), total);
        ok = false;
    }
    if (pool.used() > pool.budget()) {
        std::printf("    used() %u exceeds the budget %u\n", pool.used(), pool.budget());
        ok = false;
    }
    if (pool.largest_free() > pool.free_bytes()) {
        std::printf("    largest_free() %u exceeds free_bytes() %u\n", pool.largest_free(),
                    pool.free_bytes());
        ok = false;
    }
    return ok;
}

// By content, not by address: two identical literals are not required to be the same pointer,
// and `CHECK_EQ` on `const char *` would compare the addresses.
bool name_is(SampleState state, const char *expect) {
    return std::strcmp(sample_state_name(state), expect) == 0;
}

bool same(const SampleSlot &a, const SampleSlot &b) {
    return a.state == b.state && a.offset == b.offset && a.bytes == b.bytes &&
           a.frames == b.frames && a.rate == b.rate && a.channels == b.channels;
}

// Reserves `bytes` on every track. Returns false if any of them was refused, so a test that
// expects a full pool can say so in one check.
bool fill_all(SamplePool &pool, uint32_t bytes) {
    bool ok = true;
    for (int t = 0; t < kTracks; t++) {
        if (!pool.reserve(t, bytes)) ok = false;
    }
    return ok;
}

}  // namespace

// --- the empty pool -------------------------------------------------------

TEST(samplepool_starts_with_the_whole_region_free) {
    SamplePool pool;
    pool.init(32 * kMib);

    CHECK_EQ(pool.budget(), 32u * kMib);
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.free_bytes(), 32u * kMib);
    CHECK_EQ(pool.largest_free(), 32u * kMib);     // one hole, not eight
    CHECK_EQ(pool.count_in(SampleState::empty), kTracks);
    CHECK_EQ(pool.count_in(SampleState::ready), 0);
    CHECK(sane(pool));
}

TEST(samplepool_init_clamps_the_budget_to_the_maximum) {
    // docs/SPEC.md allows 64 MiB; the budget arrives from a project file, so a damaged byte
    // asking for more must not become a region the app never allocated. Clamping here is also
    // what keeps every offset below 2^26 and `offset + bytes` from wrapping.
    SamplePool pool;
    pool.init(100 * kMib);
    CHECK_EQ(pool.budget(), kSampleBudgetMaxMib * kMib);
    CHECK_EQ(pool.largest_free(), kSampleBudgetMaxMib * kMib);
    CHECK(sane(pool));

    pool.init(0xFFFFFFFFu);
    CHECK_EQ(pool.budget(), kSampleBudgetMaxMib * kMib);
    CHECK(sane(pool));

    // And the default fits inside it, which is the only relation between the two constants
    // that matters.
    CHECK(kSampleBudgetDefaultMib <= kSampleBudgetMaxMib);
}

TEST(samplepool_with_no_budget_refuses_everything) {
    SamplePool pool;
    pool.init(0);
    CHECK_EQ(pool.budget(), 0u);
    CHECK_EQ(pool.largest_free(), 0u);
    CHECK(!pool.reserve(0, 1));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 0u);
    CHECK(sane(pool));
}

// --- filling and releasing ------------------------------------------------

TEST(samplepool_fills_all_eight_slots_end_to_end) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    // First fit is by address, so eight equal reservations tile the region in slot order. The
    // offsets are asserted because they are what a voice reads: a correct `used()` over the
    // wrong offsets is the failure this file is about.
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(pool.slot(t).offset, uint32_t(t) * 4u * kMib);
        CHECK_EQ(pool.slot(t).bytes, 4u * kMib);
        CHECK_EQ(int(pool.slot(t).state), int(SampleState::loading));
    }
    CHECK_EQ(pool.used(), 32u * kMib);
    CHECK_EQ(pool.free_bytes(), 0u);
    CHECK_EQ(pool.largest_free(), 0u);
    CHECK(!pool.reserve(0, 32u * kMib + 1));       // and nothing more fits
    CHECK(sane(pool));
}

TEST(samplepool_release_order_never_produces_an_overlap) {
    // The orders that have historically broken free lists: forward, reverse, every other one,
    // and middle-out. Each one leaves the holes in a different arrangement for `coalesce()` to
    // put back together, and the test is the same every time -- exact accounting at each step,
    // no overlap at any step, and the whole region available again at the end.
    static const int orders[4][kTracks] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {7, 6, 5, 4, 3, 2, 1, 0},
        {0, 2, 4, 6, 1, 3, 5, 7},
        {3, 4, 2, 5, 1, 6, 0, 7},
    };

    for (const auto &order : orders) {
        SamplePool pool;
        pool.init(32 * kMib);
        CHECK(fill_all(pool, 4 * kMib));

        for (int i = 0; i < kTracks; i++) {
            pool.release(order[i]);
            CHECK_EQ(pool.used(), uint32_t(kTracks - 1 - i) * 4u * kMib);
            CHECK(sane(pool));
        }

        CHECK_EQ(pool.count_in(SampleState::empty), kTracks);
        CHECK_EQ(pool.largest_free(), 32u * kMib);   // fully coalesced, not eight 4 MiB holes
        CHECK(pool.reserve(0, 32u * kMib));          // so the whole budget is allocatable again
        pool.release(0);

        // Re-reserving after the churn lands in exactly the same places as a fresh pool.
        CHECK(fill_all(pool, 4 * kMib));
        for (int t = 0; t < kTracks; t++) {
            CHECK_EQ(pool.slot(t).offset, uint32_t(t) * 4u * kMib);
        }
        CHECK(sane(pool));
    }
}

TEST(samplepool_reload_one_slot_reuses_its_space) {
    // Auditioning samples on one track. This is the case the header says a bump allocator
    // cannot serve: with one, twenty reserves of up to 5 MiB would raise the high-water mark
    // past the budget and the last ones would be refused.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 2 * kMib));
    const uint32_t others = 7u * 2u * kMib;

    for (int i = 0; i < 20; i++) {
        const uint32_t bytes = (1u + uint32_t(i % 5)) * kMib;
        CHECK(pool.reserve(3, bytes));
        CHECK(pool.finish(3, bytes / 2u, 1, 48000));
        CHECK_EQ(pool.used(), others + bytes);       // exact, every time: nothing leaks
        CHECK(sane(pool));
    }

    // And on an otherwise empty pool the space is reused to the byte and to the address.
    pool.reset();
    for (int i = 0; i < 20; i++) {
        const uint32_t bytes = (1u + uint32_t(i % 7)) * 4u * kMib;
        CHECK(pool.reserve(0, bytes));
        CHECK_EQ(pool.slot(0).offset, 0u);
        CHECK_EQ(pool.used(), bytes);
        CHECK_EQ(pool.largest_free(), 32u * kMib - bytes);
        CHECK(sane(pool));
    }
}

// --- coalescing -----------------------------------------------------------

TEST(samplepool_coalesces_two_adjacent_releases) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    pool.release(2);
    CHECK_EQ(pool.largest_free(), 4u * kMib);
    pool.release(3);
    // The point of coalescing: a sample needing both halves fits, where a free list that kept
    // two 4 MiB holes would refuse it and the track would stay silent for no reason.
    CHECK_EQ(pool.largest_free(), 8u * kMib);
    CHECK_EQ(pool.free_bytes(), 8u * kMib);
    CHECK(pool.reserve(2, 8 * kMib));
    CHECK_EQ(pool.slot(2).offset, 8u * kMib);
    CHECK_EQ(pool.largest_free(), 0u);
    CHECK(sane(pool));
}

TEST(samplepool_coalesces_three_adjacent_releases) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    pool.release(4);
    pool.release(2);
    pool.release(3);          // the middle one last: it has to merge on both sides at once
    CHECK_EQ(pool.largest_free(), 12u * kMib);
    CHECK_EQ(pool.free_bytes(), 12u * kMib);
    CHECK(pool.reserve(2, 12 * kMib));
    CHECK_EQ(pool.slot(2).offset, 8u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_coalesces_at_both_ends_of_the_region) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    // The bottom edge: offset 0 has no hole below it to merge with.
    pool.release(1);
    pool.release(0);
    CHECK_EQ(pool.largest_free(), 8u * kMib);
    CHECK(pool.reserve(0, 8 * kMib));
    CHECK_EQ(pool.slot(0).offset, 0u);

    // The top edge: the same at the other end of the array.
    pool.release(6);
    pool.release(7);
    CHECK_EQ(pool.largest_free(), 8u * kMib);
    CHECK(pool.reserve(6, 8 * kMib));
    CHECK_EQ(pool.slot(6).offset, 24u * kMib);
    CHECK_EQ(pool.used(), 32u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_merges_a_release_into_the_unused_tail) {
    // A partly used region: 24 MiB reserved, 8 MiB never touched. Releasing the topmost slot
    // has to join its range to that tail, or the biggest sample the pool can take afterwards
    // is understated by 3 MiB for the rest of the session.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 3 * kMib));
    CHECK_EQ(pool.largest_free(), 8u * kMib);

    pool.release(7);
    CHECK_EQ(pool.largest_free(), 11u * kMib);
    CHECK_EQ(pool.free_bytes(), 11u * kMib);

    pool.release(0);                                  // a hole at the bottom end as well
    CHECK_EQ(pool.free_bytes(), 14u * kMib);
    CHECK_EQ(pool.largest_free(), 11u * kMib);        // still the tail: honest, not the sum
    CHECK(pool.reserve(7, 11 * kMib));
    CHECK_EQ(pool.slot(7).offset, 21u * kMib);
    CHECK(sane(pool));
}

// --- what actually fits ---------------------------------------------------

TEST(samplepool_largest_free_is_honest_under_fragmentation) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    for (int t = 0; t < kTracks; t += 2) pool.release(t);

    // Four 4 MiB holes separated by four live ranges. A caller that trusted `free_bytes()`
    // would start a 16 MiB load that cannot land -- 4 seconds of loading for a track that then
    // stays silent -- which is why `largest_free()` exists.
    CHECK_EQ(pool.free_bytes(), 16u * kMib);
    CHECK_EQ(pool.largest_free(), 4u * kMib);
    CHECK(pool.largest_free() < pool.free_bytes());
    CHECK(!pool.reserve(0, 16 * kMib));
    CHECK(sane(pool));

    // Exactly what it reports fits; one byte more does not.
    const uint32_t fits = pool.largest_free();
    CHECK(!pool.reserve(0, fits + 1));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK(pool.reserve(0, fits));
    CHECK_EQ(pool.slot(0).bytes, fits);
    CHECK(sane(pool));
}

TEST(samplepool_holds_the_worst_case_fragmentation) {
    // The arrangement the free list has to be able to represent at all: eight live ranges with
    // a hole after each one. Built by shrinking each slot from the top down, because first fit
    // takes the lowest hole and so leaves the gap above the range it just carved.
    //
    // Eight is also the peak the `kMaxHoles` argument in add_hole has to cover -- nine entries
    // exist for eight holes plus the one being returned, and a ninth maximal hole would need
    // every allocation to sit above offset 0, which first fit by address never produces. If
    // this test ever reports fewer than eight holes' worth of fragmentation, that argument has
    // stopped being tested.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));

    for (int t = kTracks - 1; t >= 0; t--) {
        CHECK(pool.reserve(t, 2 * kMib));
        CHECK(sane(pool));
    }

    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(pool.slot(t).offset, uint32_t(t) * 4u * kMib);
        CHECK_EQ(pool.slot(t).bytes, 2u * kMib);
    }
    CHECK_EQ(pool.used(), 16u * kMib);
    CHECK_EQ(pool.free_bytes(), 16u * kMib);
    CHECK_EQ(pool.largest_free(), 2u * kMib);      // eight 2 MiB holes, none of them joined
    CHECK(pool.largest_free() < 3u * kMib);        // so half the free space is unusable

    // Reloading a track at maximum fragmentation still gets its own range plus the hole above
    // it -- the one case where fragmentation costs the user nothing, and the reason `reserve`
    // releases before it searches.
    CHECK(pool.reserve(0, 4 * kMib));
    CHECK_EQ(pool.slot(0).offset, 0u);
    CHECK(sane(pool));

    // More than that is refused, and the slot is left empty with its old range given back.
    CHECK(!pool.reserve(1, 5 * kMib));
    CHECK_EQ(int(pool.slot(1).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 16u * kMib);             // slot 0 grew by 2 MiB, slot 1 gave back 2
    CHECK(sane(pool));

    for (int t = 0; t < kTracks; t++) pool.release(t);
    CHECK_EQ(pool.largest_free(), 32u * kMib);     // nothing was dropped at the array bound
    CHECK(sane(pool));
}

TEST(samplepool_reserves_the_whole_budget_in_one_range) {
    SamplePool pool;
    pool.init(32 * kMib);

    CHECK(pool.reserve(0, 32 * kMib));
    CHECK_EQ(pool.slot(0).offset, 0u);
    CHECK_EQ(pool.used(), 32u * kMib);
    CHECK_EQ(pool.largest_free(), 0u);
    CHECK(!pool.reserve(1, 1));                    // not one byte left for another track
    CHECK_EQ(pool.count_in(SampleState::loading), 1);
    CHECK(sane(pool));

    pool.release(0);
    CHECK_EQ(pool.largest_free(), 32u * kMib);     // and the region comes back whole
    CHECK(sane(pool));
}

TEST(samplepool_refuses_a_reserve_larger_than_the_region) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(!pool.reserve(0, 32u * kMib + 1));
    CHECK(!pool.reserve(0, 0xFFFFFFFFu));          // the wrap case, if there were one
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.largest_free(), 32u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_refuses_a_zero_byte_reserve) {
    // Nothing can be loaded into no bytes, and a zero-length range would be a `ready` slot that
    // no voice can read. The slot stays `empty`, which is what the UI already knows how to show.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(!pool.reserve(0, 0));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.slot(0).bytes, 0u);
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.largest_free(), 32u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_refused_reserve_leaves_the_other_slots_alone) {
    SamplePool pool;
    pool.init(8 * kMib);
    CHECK(pool.reserve(0, 3 * kMib));
    CHECK(pool.finish(0, 1000, 2, 44100));
    CHECK(pool.reserve(1, 3 * kMib));

    const SampleSlot before0 = pool.slot(0);
    const SampleSlot before1 = pool.slot(1);
    const uint32_t used = pool.used();
    const uint32_t largest = pool.largest_free();

    CHECK(!pool.reserve(2, 3 * kMib));             // 2 MiB left, 3 MiB asked for
    CHECK_EQ(int(pool.slot(2).state), int(SampleState::empty));
    CHECK_EQ(pool.slot(2).bytes, 0u);
    CHECK_EQ(pool.slot(2).offset, 0u);
    CHECK(same(pool.slot(0), before0));
    CHECK(same(pool.slot(1), before1));
    CHECK_EQ(pool.used(), used);
    CHECK_EQ(pool.largest_free(), largest);        // and no hole was carved for the attempt
    CHECK(sane(pool));
}

TEST(samplepool_refused_reserve_still_frees_the_slots_own_range) {
    // The one consequence of releasing before reserving, written down so that nobody "fixes" it
    // by accident. core/samplepool.hpp promises the slot is `empty` on failure, not that the
    // previous sample survives -- the space has to be given back before the new size can be
    // tried, or re-loading a track could never reuse its own range. app/ therefore checks
    // `largest_free()` before it starts a load, rather than finding out here.
    SamplePool pool;
    pool.init(8 * kMib);
    CHECK(pool.reserve(0, 4 * kMib));
    CHECK(pool.finish(0, 100, 1, 48000));
    CHECK(pool.reserve(1, 2 * kMib));
    CHECK_EQ(pool.used(), 6u * kMib);

    CHECK(!pool.reserve(1, 7 * kMib));             // more than the 6 MiB that would be free
    CHECK_EQ(int(pool.slot(1).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 4u * kMib);              // slot 1's 2 MiB went back
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::ready));
    CHECK(sane(pool));
}

// --- the state machine ----------------------------------------------------

TEST(samplepool_runs_reserve_loading_finish_ready) {
    SamplePool pool;
    pool.init(1 * kMib);
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));

    CHECK(pool.reserve(0, 4096));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::loading));
    CHECK_EQ(pool.slot(0).bytes, 4096u);
    CHECK_EQ(pool.slot(0).frames, 0u);             // a loading slot describes no audio yet
    CHECK_EQ(pool.used(), 4096u);
    CHECK(sane(pool));

    CHECK(pool.finish(0, 400, 2, 44100));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::ready));
    CHECK_EQ(pool.slot(0).frames, 400u);
    CHECK_EQ(int(pool.slot(0).channels), 2);
    CHECK_EQ(pool.slot(0).rate, 44100u);
    CHECK_EQ(pool.slot(0).bytes, 4096u);           // finishing owns the same range
    CHECK_EQ(pool.used(), 4096u);
    CHECK_EQ(pool.count_in(SampleState::ready), 1);
    CHECK(sane(pool));

    pool.release(0);
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.largest_free(), 1u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_reserve_clears_a_finished_description) {
    // Re-loading a track must not leave the old sample's frame count and rate visible while the
    // new one is still loading: a voice reading those would play the new bytes at the old
    // length. The spec's "stays silent rather than glitching" depends on `loading` describing
    // nothing.
    SamplePool pool;
    pool.init(1 * kMib);
    CHECK(pool.reserve(0, 4096));
    CHECK(pool.finish(0, 400, 2, 44100));

    CHECK(pool.reserve(0, 2048));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::loading));
    CHECK_EQ(pool.slot(0).frames, 0u);
    CHECK_EQ(int(pool.slot(0).channels), 1);
    CHECK_EQ(pool.slot(0).rate, 48000u);
    CHECK(sane(pool));
}

TEST(samplepool_fail_gives_the_bytes_back_and_says_why) {
    SamplePool pool;
    pool.init(1 * kMib);
    CHECK(pool.reserve(0, 4096));

    pool.fail(0);
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::failed));
    CHECK_EQ(pool.slot(0).bytes, 0u);              // a track that will never play holds nothing
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.largest_free(), 1u * kMib);
    CHECK_EQ(pool.count_in(SampleState::failed), 1);
    CHECK_EQ(pool.count_in(SampleState::empty), kTracks - 1);
    CHECK(sane(pool));

    // `failed` is distinct from `empty` for the screen only; the slot is reusable.
    CHECK(pool.reserve(0, 1 * kMib));
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::loading));
    CHECK(sane(pool));
}

TEST(samplepool_finish_refuses_a_slot_that_is_not_loading) {
    SamplePool pool;
    pool.init(1 * kMib);

    CHECK(!pool.finish(0, 100, 1, 48000));         // empty
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));

    CHECK(pool.reserve(0, 4096));
    CHECK(pool.finish(0, 400, 1, 44100));
    const SampleSlot ready = pool.slot(0);
    CHECK(!pool.finish(0, 7, 2, 8000));            // already ready: nothing moves
    CHECK(same(pool.slot(0), ready));

    pool.fail(1);
    CHECK(!pool.finish(1, 100, 1, 48000));         // failed
    CHECK_EQ(int(pool.slot(1).state), int(SampleState::failed));
    CHECK_EQ(pool.used(), 4096u);
    CHECK(sane(pool));
}

TEST(samplepool_finish_clamps_frames_to_the_reserved_range) {
    // A loader reporting more frames than it reserved would send voices reading into the next
    // track's sample. Clamping keeps the range the authority; fewer frames than reserved is
    // normal (a truncated file) and is kept as given.
    SamplePool pool;
    pool.init(1 * kMib);

    CHECK(pool.reserve(0, 1000));
    CHECK(pool.finish(0, 100000, 1, 48000));
    CHECK_EQ(pool.slot(0).frames, 500u);           // 2 bytes per mono frame

    CHECK(pool.reserve(1, 1000));
    CHECK(pool.finish(1, 100000, 2, 48000));
    CHECK_EQ(pool.slot(1).frames, 250u);           // 4 bytes per stereo frame

    CHECK(pool.reserve(2, 1000));
    CHECK(pool.finish(2, 7, 1, 48000));
    CHECK_EQ(pool.slot(2).frames, 7u);             // a truncated file keeps its short length
    CHECK(sane(pool));
}

TEST(samplepool_finish_normalises_channels_and_rate) {
    SamplePool pool;
    pool.init(1 * kMib);

    CHECK(pool.reserve(0, 4096));
    CHECK(pool.finish(0, 10, 0, 0));
    CHECK_EQ(int(pool.slot(0).channels), 1);       // 0 or negative channels is still a mono read
    CHECK_EQ(pool.slot(0).rate, 48000u);           // a zero rate is a voice stepping nowhere

    CHECK(pool.reserve(1, 4096));
    CHECK(pool.finish(1, 10, 9, 96000));
    CHECK_EQ(int(pool.slot(1).channels), 2);       // nothing here reads more than stereo
    CHECK_EQ(pool.slot(1).rate, 96000u);
    CHECK(sane(pool));
}

TEST(samplepool_release_from_any_state_leaves_the_slot_empty) {
    SamplePool pool;
    pool.init(1 * kMib);

    pool.release(0);                               // from empty: a no-op, not a stray hole
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.largest_free(), 1u * kMib);
    CHECK(sane(pool));

    CHECK(pool.reserve(0, 4096));
    pool.release(0);                               // from loading
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.used(), 0u);
    CHECK(sane(pool));

    CHECK(pool.reserve(0, 4096));
    CHECK(pool.finish(0, 10, 1, 48000));
    pool.release(0);                               // from ready
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.slot(0).frames, 0u);
    CHECK_EQ(pool.used(), 0u);
    CHECK(sane(pool));

    pool.fail(0);
    pool.release(0);                               // from failed
    CHECK_EQ(int(pool.slot(0).state), int(SampleState::empty));
    CHECK_EQ(pool.count_in(SampleState::empty), kTracks);
    CHECK_EQ(pool.largest_free(), 1u * kMib);      // and exactly one hole, every time
    CHECK(sane(pool));
}

TEST(samplepool_reset_gives_the_whole_region_back) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 3 * kMib));
    CHECK(pool.finish(0, 100, 1, 48000));
    pool.fail(1);

    pool.reset();
    CHECK_EQ(pool.count_in(SampleState::empty), kTracks);
    CHECK_EQ(pool.used(), 0u);
    CHECK_EQ(pool.budget(), 32u * kMib);           // reset is not init: the budget stays
    CHECK_EQ(pool.largest_free(), 32u * kMib);
    CHECK(pool.reserve(0, 32 * kMib));
    CHECK(sane(pool));
}

TEST(samplepool_state_names_are_the_four_the_screen_shows) {
    CHECK(name_is(SampleState::empty, ""));
    CHECK(name_is(SampleState::loading, "LOAD"));
    CHECK(name_is(SampleState::ready, "OK"));
    CHECK(name_is(SampleState::failed, "FAIL"));
    CHECK(name_is(SampleState::count, ""));        // not a state, and not a crash either
}

// --- the budget -----------------------------------------------------------

TEST(samplepool_set_budget_refuses_to_strand_a_loaded_sample) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 1 * kMib));               // 8 MiB, ending at offset 8 MiB

    const SampleSlot before = pool.slot(7);
    CHECK(!pool.set_budget(4 * kMib));
    CHECK_EQ(pool.budget(), 32u * kMib);           // refused means nothing changed
    CHECK(same(pool.slot(7), before));
    CHECK_EQ(pool.used(), 8u * kMib);
    CHECK_EQ(pool.largest_free(), 24u * kMib);
    CHECK(sane(pool));

    CHECK(pool.set_budget(8 * kMib));              // exactly the high-water mark is allowed
    CHECK_EQ(pool.budget(), 8u * kMib);
    CHECK_EQ(pool.free_bytes(), 0u);
    CHECK_EQ(pool.largest_free(), 0u);             // the tail hole went with the budget
    CHECK(sane(pool));
}

TEST(samplepool_set_budget_is_bounded_by_the_top_range_not_by_the_total) {
    // The nuance worth a test of its own: nothing here may move an allocation, so a single
    // sample sitting high in the region pins the budget however little is used in total.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));
    for (int t = 0; t < kTracks - 1; t++) pool.release(t);

    CHECK_EQ(pool.used(), 4u * kMib);              // one sample left, at offset 28 MiB
    CHECK(!pool.set_budget(8 * kMib));             // which is far more than `used()`
    CHECK_EQ(pool.budget(), 32u * kMib);
    CHECK(sane(pool));

    pool.release(kTracks - 1);
    CHECK(pool.set_budget(8 * kMib));              // now there is nothing to strand
    CHECK_EQ(pool.budget(), 8u * kMib);
    CHECK_EQ(pool.largest_free(), 8u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_raising_the_budget_makes_the_new_space_usable) {
    SamplePool pool;
    pool.init(16 * kMib);
    CHECK(pool.reserve(0, 10 * kMib));
    CHECK_EQ(pool.largest_free(), 6u * kMib);

    // The new tail has to merge with the hole that ended at the old budget, or the pool would
    // hold 6 MiB and 16 MiB where it should hold 22 MiB and refuse a sample that fits.
    CHECK(pool.set_budget(32 * kMib));
    CHECK_EQ(pool.budget(), 32u * kMib);
    CHECK_EQ(pool.free_bytes(), 22u * kMib);
    CHECK_EQ(pool.largest_free(), 22u * kMib);
    CHECK(pool.reserve(1, 22 * kMib));
    CHECK_EQ(pool.slot(1).offset, 10u * kMib);
    CHECK_EQ(pool.used(), 32u * kMib);
    CHECK(sane(pool));

    // Raising it past the maximum clamps rather than refuses: the figure comes from a project
    // file, and docs/SPEC.md's ceiling is what the memory budget allows.
    CHECK(pool.set_budget(0xFFFFFFFFu));
    CHECK_EQ(pool.budget(), kSampleBudgetMaxMib * kMib);
    CHECK_EQ(pool.largest_free(), kSampleBudgetMaxMib * kMib - 32u * kMib);
    CHECK(sane(pool));
}

TEST(samplepool_set_budget_to_the_same_size_is_a_no_op) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(fill_all(pool, 4 * kMib));
    for (int t = 0; t < kTracks; t += 2) pool.release(t);

    const uint32_t largest = pool.largest_free();
    CHECK(pool.set_budget(32 * kMib));
    CHECK_EQ(pool.budget(), 32u * kMib);
    CHECK_EQ(pool.largest_free(), largest);        // the holes were not re-cut
    CHECK_EQ(pool.used(), 16u * kMib);
    CHECK(sane(pool));
}

// --- bad input ------------------------------------------------------------

TEST(samplepool_refuses_slot_indices_outside_the_tracks) {
    // -1 and kTracks are the two a loop or a UI off-by-one produces. Reading or writing either
    // one is a sample offset in another struct, so every entry point checks.
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(pool.reserve(0, 4 * kMib));
    const uint32_t used = pool.used();
    const uint32_t largest = pool.largest_free();

    static const int bad[] = {-1, kTracks, kTracks + 1, 1000, -1000};
    for (const int index : bad) {
        CHECK(!pool.reserve(index, 4 * kMib));
        CHECK(!pool.finish(index, 10, 1, 48000));
        pool.fail(index);
        pool.release(index);

        const SampleSlot &none = pool.slot(index);
        CHECK_EQ(int(none.state), int(SampleState::empty));
        CHECK_EQ(none.offset, 0u);
        CHECK_EQ(none.bytes, 0u);
    }

    CHECK_EQ(pool.used(), used);
    CHECK_EQ(pool.largest_free(), largest);
    CHECK_EQ(pool.count_in(SampleState::loading), 1);
    CHECK(sane(pool));
}

TEST(samplepool_count_in_handles_a_state_that_is_not_one) {
    SamplePool pool;
    pool.init(32 * kMib);
    CHECK(pool.reserve(0, 1 * kMib));
    CHECK(pool.finish(0, 10, 1, 48000));
    pool.fail(1);

    CHECK_EQ(pool.count_in(SampleState::ready), 1);
    CHECK_EQ(pool.count_in(SampleState::failed), 1);
    CHECK_EQ(pool.count_in(SampleState::loading), 0);
    CHECK_EQ(pool.count_in(SampleState::empty), kTracks - 2);
    CHECK_EQ(pool.count_in(SampleState::count), 0);          // the sentinel counts nothing
}

// --- churn ----------------------------------------------------------------

TEST(samplepool_survives_churn_without_handing_out_the_same_bytes_twice) {
    // Two thousand operations in an order no hand-written test would think of: reserves of
    // sizes that fit and sizes that cannot, finishes, failures, releases and budget changes,
    // all interleaved. `sane()` runs after every single one, including after the refusals,
    // because a refused operation that carved a hole anyway is the bug that shows up a hundred
    // steps later as two tracks reading the same memory.
    SamplePool pool;
    pool.init(32 * kMib);

    uint32_t seed = 0x5EEDu;
    int bad = 0;
    for (int step = 0; step < 2000; step++) {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t roll = seed >> 8;
        const int slot = int(roll % uint32_t(kTracks));

        switch ((roll >> 4) % 6u) {
            case 0:
            case 1:
            case 2:
                // Sizes from zero to 40 MiB: some fit, some cannot fit anywhere, some are
                // larger than the whole region.
                pool.reserve(slot, (roll >> 8) % (40u * kMib));
                break;
            case 3:
                pool.finish(slot, (roll >> 8) % (4u * kMib), int((roll >> 6) % 3u),
                            48000);
                break;
            case 4:
                if (((roll >> 7) & 1u) != 0) {
                    pool.fail(slot);
                } else {
                    pool.release(slot);
                }
                break;
            default:
                pool.set_budget((16u + ((roll >> 8) % 4u) * 16u) * kMib);
                break;
        }

        if (!sane(pool)) {
            if (bad == 0) std::printf("    first failure at churn step %d\n", step);
            bad++;
        }
    }
    CHECK_EQ(bad, 0);

    // Whatever that left behind, releasing everything has to recover the region whole: a free
    // list that dropped or split a hole along the way would report less than the budget here,
    // and would keep refusing samples that fit for the rest of the session.
    for (int t = 0; t < kTracks; t++) pool.release(t);
    CHECK_EQ(pool.used(), 0u);
    CHECK(pool.set_budget(32 * kMib));
    CHECK_EQ(pool.largest_free(), 32u * kMib);
    CHECK(pool.reserve(0, 32 * kMib));
    CHECK(sane(pool));
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(an_empty_pool_can_shrink_to_nothing_and_grow_again) {
    SamplePool pool;
    pool.init(8 * kMib);
    CHECK(pool.set_budget(0));
    CHECK_EQ(pool.largest_free(), 0u);
    CHECK(!pool.reserve(0, 2));
    CHECK(pool.set_budget(8 * kMib));
    CHECK_EQ(pool.largest_free(), 8u * kMib);
}

TEST(a_one_byte_range_is_given_back_like_any_other) {
    // Nothing in app/ asks for an odd size, but the free list must not lose a byte when one is.
    SamplePool pool;
    pool.init(1 * kMib);
    CHECK(pool.reserve(0, 1));
    CHECK(pool.reserve(1, 1000));
    pool.release(0);
    pool.release(1);
    CHECK_EQ(pool.largest_free(), 1u * kMib);
    CHECK(pool.reserve(2, 1 * kMib));
}
