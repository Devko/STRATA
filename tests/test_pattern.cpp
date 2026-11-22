// The pattern data model: layout, trig conditions and the shared lock pool.
//
// The layout tests are the important ones. docs/SPEC.md fixes Step at 12 bytes with
// `lock_first` on offset 8, and both halves of that matter for different reasons: the size is
// what makes 128 patterns fit in about 1.1 MiB, and the offset is what keeps a 16-bit load
// aligned on the C674x, which does not fault on a misaligned access but reads the wrong
// address. A field reordering that looks like tidying would pass every behavioural test in
// this file, so the asserts come first.
//
// The condition numbers are checked one by one on purpose. They are written into every pattern
// file, so renumbering them is a silent data migration; this test is what makes it loud.
#include "check.hpp"

#include "pattern.hpp"
#include "scheduler.hpp"   // StepEvent, whose length_index default this file pins against Step's
#include "timing.hpp"      // length_valid and length_sixteenths

#include <cstddef>
#include <initializer_list>

namespace {

using namespace strata;

// Fills a pattern with distinct, in-range values so that a round trip or a compaction has
// something to get wrong. Used here and, deliberately, by test_serialize.cpp's own copy.
void fill_steps(Pattern &pattern) {
    pattern_init(pattern);
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &step = pattern.tracks[t].steps[i];
            step.flags = uint8_t((t + i) & kStepFlagMask);
            step.note = uint8_t((36 + t * 7 + i) & 0x7F);
            step.velocity = uint8_t(1 + (t * 13 + i) % 127);
            step.probability = uint8_t((t * 11 + i) % 101);
            step.micro = int8_t((i % 23) - 11);
        }
    }
}

}  // namespace

// --- layout ---------------------------------------------------------------

TEST(step_is_twelve_bytes) {
    // Also asserted at compile time in pattern.hpp; checked again here so the failure says
    // what the number actually is. The casts to int are what the framework can print.
    CHECK_EQ(int(sizeof(Step)), 12);
    CHECK_EQ(int(sizeof(Lock)), 2);
    CHECK_EQ(int(alignof(Step)), 2);
    CHECK_EQ(int(alignof(Lock)), 1);
}

TEST(lock_first_stays_naturally_aligned) {
    // Eight single bytes, then the uint16_t. Offset 8 is even, so a 16-bit load of it is
    // aligned however the pattern itself is placed -- Pattern holds Steps in arrays of 12
    // bytes, which keeps every step on an even address.
    CHECK_EQ(int(offsetof(Step, lock_first)), 8);
    CHECK_EQ(int(offsetof(Step, lock_first) % sizeof(uint16_t)), 0);
    CHECK_EQ(int(offsetof(Step, lock_count)), 10);
    CHECK_EQ(int(offsetof(Step, chord)), 11);
}

TEST(pattern_memory_matches_the_budget) {
    // docs/SPEC.md: about 6 KiB of steps plus 2 KiB of locks, so 128 patterns in about
    // 1.1 MiB. If a field is added, this is where the budget notices.
    CHECK_EQ(int(sizeof(TrackPattern)), 4 + 12 * 64);
    CHECK_EQ(int(sizeof(Step) * kStepsPerTrack * kTracks), 6144);
    CHECK_EQ(int(sizeof(Lock) * kLockPool), 2048);
    CHECK(sizeof(Pattern) * 128 < 1100u * 1024u);
}

TEST(an_initialised_pattern_is_empty_and_plays_straight) {
    Pattern pattern;
    pattern_init(pattern);

    CHECK_EQ(int(pattern.locks_used), 0);
    CHECK_EQ(int(pattern.master_length), kStepsPerTrack);
    CHECK_EQ(int(pattern.swing), int(kSwingMin));
    CHECK_EQ(int(pattern.seed), 1);

    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(pattern.tracks[t].length), kStepsPerTrack);
        CHECK_EQ(int(pattern.tracks[t].speed), int(Speed::x1));
        CHECK_EQ(int(pattern.tracks[t].swing), int(kSwingInherit));
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &step = pattern.tracks[t].steps[i];
            CHECK_EQ(int(step.flags), 0);
            CHECK_EQ(int(step.lock_count), 0);
            CHECK_EQ(int(step.lock_first), int(kNoLock));
            CHECK_EQ(int(step.condition), int(Condition::NONE));
            CHECK_EQ(int(ratchet_count(step.ratchet)), 1);
        }
    }
}

TEST(an_untouched_step_asks_for_a_note_one_step_long) {
    // The defaults a blank pattern plays with, and `length` is the one with teeth.
    //
    // Until `Trig::gate_frames` existed nothing read it, and it defaulted to 1 because 1 is the
    // table's lowest valid index. 1 is a SIXTEENTH OF A STEP -- about 7 ms at 130 BPM -- so the
    // moment the field became audible that default turned every untouched note into a blip. The
    // emulator saw it as a 2.4-point drop in load before the sound was even listened to.
    //
    // 8 is "1 step". It is pinned here, and against `StepEvent`'s own default, because the two
    // disagreed for three milestones without anything noticing: core/pattern.hpp is what a
    // pattern plays with and core/scheduler.hpp is what a step with no pattern behind it plays
    // with, and an untouched note should not depend on which of them it came through.
    const Step fresh{};
    CHECK_EQ(int(fresh.length), 8);
    CHECK_EQ(int(length_sixteenths(fresh.length)), 16);   // one step, exactly
    CHECK(length_valid(fresh.length));

    const StepEvent event{};
    CHECK_EQ(int(event.length_index), int(fresh.length));

    Pattern pattern;
    pattern_init(pattern);
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            CHECK_EQ(int(pattern.tracks[t].steps[i].length), 8);
        }
    }

    // And the thing that made 1 the old default is still true: index 0 is docs/SPEC.md's
    // invalid sentinel, so there is no "let the envelope decide" to default to instead.
    CHECK(!length_valid(0));
    CHECK_EQ(int(length_sixteenths(0)), 0);
}

// --- trig conditions ------------------------------------------------------

TEST(condition_numbers_are_on_disk_and_must_not_move) {
    CHECK_EQ(int(Condition::NONE), 0);
    CHECK_EQ(int(Condition::A_B), 1);
    CHECK_EQ(int(Condition::FILL), 2);
    CHECK_EQ(int(Condition::NOT_FILL), 3);
    CHECK_EQ(int(Condition::PRE), 4);
    CHECK_EQ(int(Condition::NOT_PRE), 5);
    CHECK_EQ(int(Condition::NEI), 6);
    CHECK_EQ(int(Condition::NOT_NEI), 7);
    CHECK_EQ(int(Condition::FIRST), 8);
    CHECK_EQ(int(Condition::NOT_FIRST), 9);
    CHECK_EQ(kConditionKinds, 10);
    CHECK_EQ(int(kConditionAbBase), 16);
    CHECK_EQ(int(kConditionAbPairs), 35);
}

TEST(every_condition_kind_round_trips_through_its_byte) {
    const Condition kinds[] = {Condition::NONE,    Condition::FILL,  Condition::NOT_FILL,
                               Condition::PRE,     Condition::NOT_PRE, Condition::NEI,
                               Condition::NOT_NEI, Condition::FIRST, Condition::NOT_FIRST};
    for (const Condition kind : kinds) {
        const uint8_t packed = condition_pack(kind);
        CHECK_EQ(int(packed), int(kind));
        CHECK(condition_valid(packed));
        CHECK_EQ(int(condition_kind(packed)), int(kind));
        CHECK_EQ(int(condition_a(packed)), 0);
        CHECK_EQ(int(condition_b(packed)), 0);
    }
}

TEST(every_a_b_pair_round_trips_through_its_byte) {
    // 1:2 2:2 1:3 .. 8:8 is 35 pairs, and each must come back as itself.
    int pairs = 0;
    for (uint8_t b = kConditionBMin; b <= kConditionBMax; b++) {
        for (uint8_t a = 1; a <= b; a++) {
            const uint8_t packed = condition_pack_a_b(a, b);
            CHECK(condition_valid(packed));
            CHECK_EQ(int(condition_kind(packed)), int(Condition::A_B));
            CHECK_EQ(int(condition_a(packed)), int(a));
            CHECK_EQ(int(condition_b(packed)), int(b));
            CHECK(packed >= kConditionAbBase);
            CHECK(packed < kConditionAbBase + kConditionAbPairs);
            pairs++;
        }
    }
    CHECK_EQ(pairs, int(kConditionAbPairs));

    // Distinct bytes, all of them: a collision would make two conditions the same trig.
    bool seen[256] = {};
    int distinct = 0;
    for (uint8_t b = kConditionBMin; b <= kConditionBMax; b++) {
        for (uint8_t a = 1; a <= b; a++) {
            const uint8_t packed = condition_pack_a_b(a, b);
            if (!seen[packed]) distinct++;
            seen[packed] = true;
        }
    }
    CHECK_EQ(distinct, int(kConditionAbPairs));
}

TEST(a_b_is_clamped_and_the_bare_kind_means_one_of_two) {
    CHECK_EQ(int(condition_pack(Condition::A_B)), int(condition_pack_a_b(1, 2)));
    CHECK_EQ(int(condition_pack_a_b(0, 0)), int(condition_pack_a_b(1, 2)));    // b below 2
    CHECK_EQ(int(condition_pack_a_b(9, 9)), int(condition_pack_a_b(8, 8)));    // b above 8
    CHECK_EQ(int(condition_pack_a_b(5, 3)), int(condition_pack_a_b(3, 3)));    // a above b

    // A hand-made file may hold the bare A_B value; it reads back as 1:2 rather than nonsense.
    CHECK_EQ(int(condition_kind(uint8_t(Condition::A_B))), int(Condition::A_B));
    CHECK_EQ(int(condition_a(uint8_t(Condition::A_B))), 1);
    CHECK_EQ(int(condition_b(uint8_t(Condition::A_B))), 2);
}

TEST(unknown_condition_bytes_read_back_as_none) {
    const uint8_t invalid[] = {10, 11, 15, 51, 100, 200, 255};
    for (const uint8_t packed : invalid) {
        CHECK(!condition_valid(packed));
        CHECK_EQ(int(condition_kind(packed)), int(Condition::NONE));
        CHECK_EQ(int(condition_a(packed)), 0);
        CHECK_EQ(int(condition_b(packed)), 0);
    }
    // And no byte at all makes the helpers misbehave.
    for (int packed = 0; packed < 256; packed++) {
        const uint8_t b = condition_b(uint8_t(packed));
        CHECK(b == 0 || (b >= kConditionBMin && b <= kConditionBMax));
        const uint8_t a = condition_a(uint8_t(packed));
        CHECK(a <= b);
    }
}

// --- ratchets -------------------------------------------------------------

TEST(ratchet_packs_count_and_ramp_into_one_byte) {
    for (uint8_t count = kRatchetMin; count <= kRatchetMax; count++) {
        for (const RatchetRamp ramp : {RatchetRamp::none, RatchetRamp::up, RatchetRamp::down}) {
            const uint8_t packed = ratchet_pack(count, ramp);
            CHECK_EQ(int(ratchet_count(packed)), int(count));
            CHECK_EQ(int(ratchet_ramp(packed)), int(ramp));
        }
    }
}

// --- locks ----------------------------------------------------------------

TEST(a_lock_can_be_added_read_and_updated_without_duplicating) {
    Pattern pattern;
    pattern_init(pattern);

    CHECK(pattern_set_lock(pattern, 0, 0, 4, 100));
    CHECK_EQ(int(pattern.locks_used), 1);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), 1);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_first), 0);

    uint8_t value = 0;
    CHECK(pattern_get_lock(pattern, 0, 0, 4, value));
    CHECK_EQ(int(value), 100);

    // Setting the same parameter again is an edit, not a second lock.
    CHECK(pattern_set_lock(pattern, 0, 0, 4, 7));
    CHECK_EQ(int(pattern.locks_used), 1);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), 1);
    CHECK(pattern_get_lock(pattern, 0, 0, 4, value));
    CHECK_EQ(int(value), 7);

    // A parameter that was never locked is absent, and the out value is left alone.
    value = 0xAB;
    CHECK(!pattern_get_lock(pattern, 0, 0, 5, value));
    CHECK_EQ(int(value), 0xAB);
    CHECK(!pattern_get_lock(pattern, 1, 0, 4, value));
}

TEST(a_steps_locks_read_back_in_the_order_they_were_added) {
    Pattern pattern;
    pattern_init(pattern);

    for (uint8_t i = 0; i < kLocksPerStep; i++) {
        CHECK(pattern_set_lock(pattern, 3, 17, uint8_t(i * 2), uint8_t(10 + i)));
    }
    const LockView view = pattern_step_locks(pattern, 3, 17);
    CHECK_EQ(int(view.count), kLocksPerStep);
    int seen = 0;
    for (const Lock &lock : view) {
        CHECK_EQ(int(lock.param), seen * 2);
        CHECK_EQ(int(lock.value), 10 + seen);
        seen++;
    }
    CHECK_EQ(seen, kLocksPerStep);
}

TEST(a_step_takes_no_more_than_eight_locks) {
    Pattern pattern;
    pattern_init(pattern);

    for (uint8_t i = 0; i < kLocksPerStep; i++) {
        CHECK(pattern_set_lock(pattern, 0, 0, i, i));
    }
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), kLocksPerStep);

    // The ninth parameter is refused, and nothing is disturbed.
    const uint16_t used = pattern.locks_used;
    CHECK(!pattern_set_lock(pattern, 0, 0, 99, 99));
    CHECK_EQ(int(pattern.locks_used), int(used));
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), kLocksPerStep);

    // But updating one of the eight it already has still works.
    CHECK(pattern_set_lock(pattern, 0, 0, 3, 123));
    uint8_t value = 0;
    CHECK(pattern_get_lock(pattern, 0, 0, 3, value));
    CHECK_EQ(int(value), 123);
}

TEST(indices_outside_the_pattern_are_refused) {
    Pattern pattern;
    pattern_init(pattern);
    uint8_t value = 0;

    CHECK(!pattern_set_lock(pattern, kTracks, 0, 0, 0));
    CHECK(!pattern_set_lock(pattern, 0, kStepsPerTrack, 0, 0));
    CHECK(!pattern_set_lock(pattern, -1, 0, 0, 0));
    CHECK(!pattern_set_lock(pattern, 0, -1, 0, 0));
    CHECK(!pattern_get_lock(pattern, kTracks, 0, 0, value));
    CHECK(!pattern_clear_lock(pattern, 0, kStepsPerTrack, 0));
    CHECK_EQ(int(pattern_step_locks(pattern, kTracks, 0).count), 0);
    CHECK(pattern_step_locks(pattern, 0, kStepsPerTrack).locks == nullptr);
    pattern_clear_step_locks(pattern, 99, 99);   // must simply do nothing
    CHECK_EQ(int(pattern.locks_used), 0);
}

TEST(removing_a_lock_keeps_the_others_in_order) {
    Pattern pattern;
    pattern_init(pattern);

    for (uint8_t i = 0; i < 4; i++) CHECK(pattern_set_lock(pattern, 2, 5, i, uint8_t(50 + i)));

    CHECK(pattern_clear_lock(pattern, 2, 5, 1));
    CHECK_EQ(int(pattern.tracks[2].steps[5].lock_count), 3);

    const LockView view = pattern_step_locks(pattern, 2, 5);
    CHECK_EQ(int(view.count), 3);
    CHECK_EQ(int(view.locks[0].param), 0);
    CHECK_EQ(int(view.locks[1].param), 2);
    CHECK_EQ(int(view.locks[2].param), 3);
    CHECK_EQ(int(view.locks[0].value), 50);
    CHECK_EQ(int(view.locks[1].value), 52);
    CHECK_EQ(int(view.locks[2].value), 53);

    // Removing one that is not there changes nothing.
    CHECK(!pattern_clear_lock(pattern, 2, 5, 1));
    CHECK_EQ(int(pattern.tracks[2].steps[5].lock_count), 3);

    // Emptying the step puts lock_first back to the sentinel.
    pattern_clear_step_locks(pattern, 2, 5);
    CHECK_EQ(int(pattern.tracks[2].steps[5].lock_count), 0);
    CHECK_EQ(int(pattern.tracks[2].steps[5].lock_first), int(kNoLock));
    CHECK_EQ(int(pattern_locks_reachable(pattern)), 0);
}

TEST(removing_the_top_of_the_pool_hands_the_slots_straight_back) {
    Pattern pattern;
    pattern_init(pattern);

    CHECK(pattern_set_lock(pattern, 0, 0, 1, 1));
    CHECK(pattern_set_lock(pattern, 0, 1, 2, 2));
    CHECK_EQ(int(pattern.locks_used), 2);

    // The last step's run is at the top, so its slot is reclaimed at once.
    CHECK(pattern_clear_lock(pattern, 0, 1, 2));
    CHECK_EQ(int(pattern.locks_used), 1);

    // The first step's is now at the top in turn.
    CHECK(pattern_clear_lock(pattern, 0, 0, 1));
    CHECK_EQ(int(pattern.locks_used), 0);
}

TEST(growing_a_buried_run_relocates_it_and_leaks_until_compaction) {
    Pattern pattern;
    pattern_init(pattern);

    // Step A takes one slot, then step B takes one, burying A's run.
    CHECK(pattern_set_lock(pattern, 0, 0, 1, 11));
    CHECK(pattern_set_lock(pattern, 0, 1, 2, 22));
    CHECK_EQ(int(pattern.locks_used), 2);

    // A has to move to the top to grow, abandoning its old slot.
    CHECK(pattern_set_lock(pattern, 0, 0, 3, 33));
    CHECK_EQ(int(pattern.locks_used), 4);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_first), 2);
    CHECK_EQ(int(pattern_locks_reachable(pattern)), 3);   // one slot is now garbage

    uint8_t value = 0;
    CHECK(pattern_get_lock(pattern, 0, 0, 1, value));
    CHECK_EQ(int(value), 11);
    CHECK(pattern_get_lock(pattern, 0, 0, 3, value));
    CHECK_EQ(int(value), 33);
    CHECK(pattern_get_lock(pattern, 0, 1, 2, value));
    CHECK_EQ(int(value), 22);

    // Compaction is what gets the slot back.
    CHECK_EQ(int(pattern_compact_locks(pattern)), 3);
    CHECK_EQ(int(pattern.locks_used), 3);
    CHECK_EQ(int(pattern_locks_reachable(pattern)), 3);
}

TEST(the_pool_runs_out_gracefully) {
    Pattern pattern;
    pattern_init(pattern);

    // 128 steps of 8 locks is exactly the 1,024-slot pool, with no relocation anywhere: each
    // step's run is at the top when it grows.
    int added = 0;
    for (int step = 0; step < 128; step++) {
        for (uint8_t param = 0; param < kLocksPerStep; param++) {
            if (!pattern_set_lock(pattern, step / kStepsPerTrack, step % kStepsPerTrack, param,
                                  uint8_t(param + step))) {
                break;
            }
            added++;
        }
    }
    CHECK_EQ(added, kLockPool);
    CHECK_EQ(int(pattern.locks_used), kLockPool);
    CHECK_EQ(int(pattern_locks_free(pattern)), 0);
    CHECK_EQ(int(pattern_locks_reachable(pattern)), kLockPool);

    // The pool is full: a lock on a fresh step is refused, and nothing changes.
    CHECK(!pattern_set_lock(pattern, 7, 63, 0, 0));
    CHECK_EQ(int(pattern.locks_used), kLockPool);
    CHECK_EQ(int(pattern.tracks[7].steps[63].lock_count), 0);

    // Updating a lock that already exists needs no slot, so it still works.
    CHECK(pattern_set_lock(pattern, 0, 0, 0, 42));
    uint8_t value = 0;
    CHECK(pattern_get_lock(pattern, 0, 0, 0, value));
    CHECK_EQ(int(value), 42);

    // Freeing one slot elsewhere is not enough for a run of 2 that has to relocate...
    CHECK(pattern_clear_lock(pattern, 1, 0, 0));
    CHECK_EQ(int(pattern_locks_free(pattern)), 0);   // the hole is in the middle, not the top
    // ...but compacting turns the holes back into room at the top.
    CHECK_EQ(int(pattern_compact_locks(pattern)), kLockPool - 1);
    CHECK_EQ(int(pattern_locks_free(pattern)), 1);
    CHECK(pattern_set_lock(pattern, 7, 63, 0, 9));
}

TEST(a_buried_run_cannot_grow_into_a_full_pool) {
    Pattern pattern;
    pattern_init(pattern);

    // Two locks on one step, then fill the pool with other steps so that this run is buried
    // and growing it would need three free slots rather than one.
    CHECK(pattern_set_lock(pattern, 0, 0, 1, 11));
    CHECK(pattern_set_lock(pattern, 0, 0, 2, 22));

    for (int t = 0; t < kTracks && pattern_locks_free(pattern) > 0; t++) {
        for (int i = 0; i < kStepsPerTrack && pattern_locks_free(pattern) > 0; i++) {
            if (t == 0 && i == 0) continue;
            for (uint8_t p = 0; p < kLocksPerStep && pattern_locks_free(pattern) > 0; p++) {
                CHECK(pattern_set_lock(pattern, t, i, p, uint8_t(p)));
            }
        }
    }
    CHECK_EQ(int(pattern_locks_free(pattern)), 0);

    CHECK(!pattern_set_lock(pattern, 0, 0, 3, 33));

    // Refused, and the two locks it already had are untouched.
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), 2);
    uint8_t value = 0;
    CHECK(pattern_get_lock(pattern, 0, 0, 1, value));
    CHECK_EQ(int(value), 11);
    CHECK(pattern_get_lock(pattern, 0, 0, 2, value));
    CHECK_EQ(int(value), 22);
    CHECK(!pattern_get_lock(pattern, 0, 0, 3, value));
}

TEST(compaction_preserves_every_steps_locks) {
    Pattern pattern;
    fill_steps(pattern);

    // Build a fragmented pool on purpose: locks go on in an interleaved order so that nearly
    // every step has to relocate at some point, and a few are removed afterwards.
    for (uint8_t round = 0; round < 5; round++) {
        for (int t = 0; t < kTracks; t++) {
            for (int i = 0; i < 6; i++) {
                pattern_set_lock(pattern, t, i * 5, uint8_t(round), uint8_t(t * 16 + i + round));
            }
        }
    }
    for (int t = 0; t < kTracks; t++) pattern_clear_lock(pattern, t, 10, 2);

    CHECK(pattern.locks_used > pattern_locks_reachable(pattern));   // fragmented, as intended

    // Photograph every step's locks, compact, then compare.
    struct Shot {
        uint8_t count;
        Lock locks[kLocksPerStep];
    };
    static Shot before[kTracks][kStepsPerTrack];
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const LockView view = pattern_step_locks(pattern, t, i);
            before[t][i].count = view.count;
            for (uint8_t k = 0; k < view.count; k++) before[t][i].locks[k] = view.locks[k];
        }
    }
    const uint16_t reachable = pattern_locks_reachable(pattern);

    CHECK_EQ(int(pattern_compact_locks(pattern)), int(reachable));
    CHECK_EQ(int(pattern.locks_used), int(reachable));

    int checked = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const LockView view = pattern_step_locks(pattern, t, i);
            CHECK_EQ(int(view.count), int(before[t][i].count));
            for (uint8_t k = 0; k < before[t][i].count; k++) {
                CHECK_EQ(int(view.locks[k].param), int(before[t][i].locks[k].param));
                CHECK_EQ(int(view.locks[k].value), int(before[t][i].locks[k].value));
                checked++;
            }
        }
    }
    CHECK_EQ(checked, int(reachable));

    // Compacting again is a no-op, and the runs are contiguous from 0 in track-step order.
    CHECK_EQ(int(pattern_compact_locks(pattern)), int(reachable));
    uint32_t expect_first = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &step = pattern.tracks[t].steps[i];
            if (step.lock_count == 0) {
                CHECK_EQ(int(step.lock_first), int(kNoLock));
                continue;
            }
            CHECK_EQ(int(step.lock_first), int(expect_first));
            expect_first += step.lock_count;
        }
    }
    CHECK_EQ(int(expect_first), int(reachable));
}

TEST(compaction_of_an_empty_pool_is_harmless) {
    Pattern pattern;
    pattern_init(pattern);
    CHECK_EQ(int(pattern_compact_locks(pattern)), 0);
    CHECK_EQ(int(pattern.locks_used), 0);
    CHECK_EQ(int(pattern_locks_free(pattern)), kLockPool);
}

TEST(a_run_pointing_outside_the_pool_is_treated_as_no_locks) {
    // Nothing in core/ produces this -- the serializer refuses such a file -- but a command
    // applier with a bug could, and the lock helpers must not read outside Pattern::locks.
    Pattern pattern;
    pattern_init(pattern);
    CHECK(pattern_set_lock(pattern, 0, 0, 1, 1));

    Step &step = pattern.tracks[0].steps[0];
    step.lock_first = 1000;
    step.lock_count = 8;     // runs past locks_used

    CHECK_EQ(int(pattern_step_locks(pattern, 0, 0).count), 0);
    CHECK(pattern_step_locks(pattern, 0, 0).locks == nullptr);
    uint8_t value = 0;
    CHECK(!pattern_get_lock(pattern, 0, 0, 1, value));
    CHECK(!pattern_clear_lock(pattern, 0, 0, 1));
    CHECK_EQ(int(pattern_locks_reachable(pattern)), 0);

    // Setting a lock drops the bad run and starts a fresh one.
    CHECK(pattern_set_lock(pattern, 0, 0, 2, 2));
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), 1);
    CHECK(pattern_get_lock(pattern, 0, 0, 2, value));
    CHECK_EQ(int(value), 2);

    // And compaction clears such a run rather than copying it.
    step.lock_first = 60000;
    step.lock_count = 3;
    pattern_compact_locks(pattern);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_count), 0);
    CHECK_EQ(int(pattern.tracks[0].steps[0].lock_first), int(kNoLock));
}

// --- sanitising -----------------------------------------------------------

TEST(sanitize_forces_every_field_into_range) {
    Step step;
    step.flags = 0xFF;
    step.note = 200;
    step.velocity = 0;
    step.length = 0;
    step.micro = 100;
    step.probability = 250;
    step.condition = 200;
    step.ratchet = 0xF0;    // 0 hits, ramp 15
    step.lock_count = 30;
    step.lock_first = 5;

    step_sanitize(step);

    CHECK_EQ(int(step.flags), int(kStepFlagMask));
    CHECK_EQ(int(step.note), 200 & 0x7F);
    CHECK_EQ(int(step.velocity), 1);
    CHECK_EQ(int(step.length), 1);
    CHECK_EQ(int(step.micro), 11);
    CHECK_EQ(int(step.probability), 100);
    CHECK_EQ(int(step.condition), int(Condition::NONE));
    CHECK_EQ(int(ratchet_count(step.ratchet)), 1);
    CHECK_EQ(int(ratchet_ramp(step.ratchet)), int(RatchetRamp::none));
    CHECK_EQ(int(step.lock_count), 0);
    CHECK_EQ(int(step.lock_first), int(kNoLock));

    step.micro = -100;
    step_sanitize(step);
    CHECK_EQ(int(step.micro), -11);
}

TEST(sanitize_leaves_a_legal_step_alone) {
    Step step;
    step.flags = kStepTrig | kStepAccent;
    step.note = 48;
    step.velocity = 127;
    step.length = 4;
    step.micro = -11;
    step.probability = 100;
    step.condition = condition_pack_a_b(3, 4);
    step.ratchet = ratchet_pack(4, RatchetRamp::down);
    step.chord = 7;

    const Step copy = step;
    step_sanitize(step);

    CHECK_EQ(int(step.flags), int(copy.flags));
    CHECK_EQ(int(step.note), int(copy.note));
    CHECK_EQ(int(step.velocity), int(copy.velocity));
    CHECK_EQ(int(step.length), int(copy.length));
    CHECK_EQ(int(step.micro), int(copy.micro));
    CHECK_EQ(int(step.probability), int(copy.probability));
    CHECK_EQ(int(step.condition), int(copy.condition));
    CHECK_EQ(int(step.ratchet), int(copy.ratchet));
    CHECK_EQ(int(step.chord), int(copy.chord));
}

// The length index has a reserved invalid 0 AND a top of kLengthMax, and a corrupt byte can be
// either side. Only the low end used to be clamped, which let a length of 21..255 through a
// load; length_sixteenths() then returns 0 and the step plays a note of no duration. The whole
// point of reserving index 0 was that an unusable length should be refusable rather than silent,
// so the high end has to be clamped too.
TEST(sanitize_clamps_a_length_at_both_ends) {
    for (int raw = 0; raw <= 255; raw++) {
        Step step;
        step_clear(step);
        step.length = uint8_t(raw);
        step_sanitize(step);

        CHECK(length_valid(step.length));
        CHECK(length_sixteenths(step.length) > 0);

        // A legal value must survive untouched: clamping must not quietly rewrite good data.
        if (raw >= 1 && raw <= kLengthMax) CHECK_EQ(int(step.length), raw);
    }
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

namespace {
Pattern g_gap_pattern;
}

TEST(a_damaged_run_reads_as_no_locks_and_is_dropped_before_it_grows) {
    Pattern &p = g_gap_pattern;
    pattern_init(p);
    CHECK(pattern_set_lock(p, 0, 0, 1, 11));
    CHECK(pattern_set_lock(p, 0, 0, 2, 22));
    Step &s = p.tracks[0].steps[1];
    s.lock_first = 0;
    s.lock_count = uint8_t(kLocksPerStep + 1);          // longer than a step may hold
    CHECK_EQ(int(pattern_step_locks(p, 0, 1).count), 0);
    s.lock_first = kNoLock;                              // a count with nowhere to point
    s.lock_count = 1;
    CHECK_EQ(int(pattern_step_locks(p, 0, 1).count), 0);
    // Locking that step starts a fresh run of one, rather than copying what kNoLock pointed at.
    CHECK(pattern_set_lock(p, 0, 1, 7, 70));
    const LockView view = pattern_step_locks(p, 0, 1);
    CHECK_EQ(int(view.count), 1);
    CHECK_EQ(int(view.locks[0].param), 7);
    CHECK_EQ(int(view.locks[0].value), 70);
}

TEST(a_run_moved_to_the_top_of_the_pool_keeps_every_lock_in_order) {
    Pattern &p = g_gap_pattern;
    pattern_init(p);
    CHECK(pattern_set_lock(p, 2, 4, 1, 11));
    CHECK(pattern_set_lock(p, 2, 4, 2, 22));
    CHECK(pattern_set_lock(p, 2, 4, 3, 33));
    CHECK(pattern_set_lock(p, 2, 5, 9, 99));            // buries step 4's run
    CHECK(pattern_set_lock(p, 2, 4, 4, 44));            // so this relocates it
    const LockView view = pattern_step_locks(p, 2, 4);
    CHECK_EQ(int(view.count), 4);
    for (int k = 0; k < 4; k++) {
        CHECK_EQ(int(view.locks[k].param), k + 1);
        CHECK_EQ(int(view.locks[k].value), 11 * (k + 1));
    }
}

TEST(clearing_a_lock_from_the_run_at_the_top_gives_its_slot_back) {
    Pattern &p = g_gap_pattern;
    pattern_init(p);
    CHECK(pattern_set_lock(p, 0, 0, 1, 10));
    CHECK(pattern_set_lock(p, 0, 1, 1, 10));
    CHECK(pattern_set_lock(p, 0, 1, 2, 20));
    CHECK(pattern_set_lock(p, 0, 1, 3, 30));
    CHECK_EQ(int(p.locks_used), 4);
    CHECK(pattern_clear_lock(p, 0, 1, 2));
    CHECK_EQ(int(p.locks_used), 3);
}

TEST(a_full_pool_refuses_to_grow_the_run_at_its_top_and_compacts_without_loss) {
    // 127 steps of 8, one of 1 and one of 7 on top: exactly kLockPool, with room in the top
    // step's run but none in the pool.
    Pattern &p = g_gap_pattern;
    pattern_init(p);
    int step = 0;
    for (; step < 127; step++) {
        for (uint8_t k = 0; k < kLocksPerStep; k++) {
            CHECK(pattern_set_lock(p, step / kStepsPerTrack, step % kStepsPerTrack, k, k));
        }
    }
    CHECK(pattern_set_lock(p, 1, 63, 0, 5));
    for (uint8_t k = 0; k < 7; k++) CHECK(pattern_set_lock(p, 2, 0, k, uint8_t(k + 1)));
    CHECK_EQ(int(p.locks_used), kLockPool);
    CHECK(!pattern_set_lock(p, 2, 0, 7, 9));
    CHECK_EQ(int(p.locks_used), kLockPool);
    CHECK_EQ(int(pattern_step_locks(p, 2, 0).count), 7);

    // Compacting a pool that is exactly full of reachable locks keeps every one of them.
    CHECK_EQ(int(pattern_compact_locks(p)), kLockPool);
    CHECK_EQ(int(pattern_locks_reachable(p)), kLockPool);
    CHECK_EQ(int(pattern_step_locks(p, 2, 0).count), 7);
}

TEST(packing_a_b_with_no_numbers_gives_one_of_the_shortest_cycle) {
    const uint8_t ab = condition_pack(Condition::A_B);
    CHECK_EQ(int(condition_a(ab)), 1);
    CHECK_EQ(int(condition_b(ab)), int(kConditionBMin));
    CHECK_EQ(int(condition_pack(Condition(kConditionKinds))), int(Condition::NONE));
}
