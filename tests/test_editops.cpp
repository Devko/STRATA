// Copy, paste and clear for steps, ranges and tracks.
//
// Almost every test here is really a test about the lock pool. Pasting a step means putting
// locks back into a bump-allocated pool that may be full of slots relocation abandoned, so the
// cases worth writing down are the ones where that goes wrong quietly: a paste that disturbs a
// step it was not asked to touch, a paste that half-succeeds, a paste refused although
// compaction would have made room. The pattern-wide comparisons (`same_bytes`,
// `same_lock_views`) are what catch the first two, because a step whose run moved under it
// looks perfectly healthy until you read the run.
//
// Every Pattern and RangeClip in this file is at file scope. A Pattern is about 8 KiB and a clip
// about 1.9 KiB against a 16 KiB stack frame limit, and on the host a stack overflow would
// simply not show -- it would show on the unit, as corrupted audio.
#include "check.hpp"

#include "editops.hpp"

#include <cstring>

namespace {

using namespace strata;

Pattern g_pattern;
Pattern g_before;
Pattern g_reference;
RangeClip g_clip;

// --- helpers --------------------------------------------------------------

// The whole struct, padding included. Both operands are file-scope and so zero-initialised, and
// the only writes either sees are field assignments and trivial copies from the other, so the
// padding Pattern carries in front of `seed` compares equal in every snapshot taken here. This
// is the strict comparison: a compaction leaves two patterns observably identical and byte-wise
// different, and a refused paste must not even do that.
bool same_bytes(const Pattern &a, const Pattern &b) {
    return std::memcmp(&a, &b, sizeof(Pattern)) == 0;
}

// Every step's locks as the sequencer would read them -- a different question from the bytes,
// and the one that matters after an operation that was allowed to repack the pool.
bool same_lock_views(const Pattern &a, const Pattern &b) {
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const LockView va = pattern_step_locks(a, t, i);
            const LockView vb = pattern_step_locks(b, t, i);
            if (va.count != vb.count) return false;
            for (uint8_t k = 0; k < va.count; k++) {
                if (va.locks[k].param != vb.locks[k].param) return false;
                if (va.locks[k].value != vb.locks[k].value) return false;
            }
        }
    }
    return true;
}

// Step fields without the pool indices: the same step pasted somewhere else holds the same
// content with its run in a different place.
bool same_step_content(const Step &a, const Step &b) {
    return a.flags == b.flags && a.note == b.note && a.velocity == b.velocity &&
           a.length == b.length && a.micro == b.micro && a.probability == b.probability &&
           a.condition == b.condition && a.ratchet == b.ratchet && a.chord == b.chord;
}

bool step_is_empty(const Step &step) {
    const Step fresh;
    return same_step_content(step, fresh) && step.lock_count == 0 && step.lock_first == kNoLock;
}

// Distinct, in-range content on every step of a track, so that a copy, an erase or a paste has
// something to get wrong. One step in three has no trig: its stored note and micro-timing are
// as much part of the pattern as a trig's.
void fill_track(Pattern &pattern, int track) {
    for (int i = 0; i < kStepsPerTrack; i++) {
        Step &step = pattern.tracks[track].steps[i];
        step_clear(step);
        step.flags = uint8_t(i % 3 == 0 ? 0 : (i % 3 == 1 ? kStepTrig : kStepTrigless));
        step.note = uint8_t((24 + track * 5 + i) & 0x7F);
        step.velocity = uint8_t(1 + (track * 9 + i) % 127);
        step.length = uint8_t(1 + i % 7);
        step.micro = int8_t(i % 23 - 11);
        step.probability = uint8_t(i * 7 % 101);
        step.condition = condition_pack_a_b(uint8_t(1 + i % 4), 4);
        step.ratchet = ratchet_pack(uint8_t(1 + i % 8), RatchetRamp(i % 3));
        step.chord = uint8_t(i % 5);
    }
}

// Lock values are derived from where the lock was made, so a run that ends up on the wrong step
// shows as wrong values and not merely as a wrong count.
int lock_seed(int track, int step) { return track * kStepsPerTrack + step; }
uint8_t lock_value(int seed, int k) { return uint8_t(1 + (seed * 5 + k * 3) % 127); }

void add_locks(Pattern &pattern, int track, int step, uint8_t first_param, int count) {
    for (int k = 0; k < count; k++) {
        pattern_set_lock(pattern, track, step, uint8_t(first_param + k),
                         lock_value(lock_seed(track, step), k));
    }
}

// `count` locks on a step, with the params and values `add_locks` would have made for
// `value_seed` (by default this step itself).
bool locks_are(const Pattern &pattern, int track, int step, uint8_t first_param, int count,
               int value_seed = -1) {
    if (value_seed < 0) value_seed = lock_seed(track, step);
    const LockView view = pattern_step_locks(pattern, track, step);
    if (int(view.count) != count) return false;
    for (int k = 0; k < count; k++) {
        if (view.locks[k].param != uint8_t(first_param + k)) return false;
        if (view.locks[k].value != lock_value(value_seed, k)) return false;
    }
    return true;
}

// Fill the pool with abandoned slots and no reachable locks at all, down to at most
// `leave_free` + 8 free. Growing a buried run is what relocates it, and a run that is buried
// when it is released cannot hand its slots back -- which is the state a long editing session
// leaves the pool in, and the one a paste has to compact its way out of. Uses two steps of
// track 7 and leaves both empty.
void waste_pool(Pattern &pattern, int leave_free) {
    while (int(pattern_locks_free(pattern)) > leave_free + kLocksPerStep) {
        // Values no blank slot could have, so that a comparison of two pools can tell an
        // abandoned slot from a compacted one.
        for (uint8_t p = 0; p < kLocksPerStep; p++) {
            pattern_set_lock(pattern, 7, 62, p, uint8_t(0x5A + p));
        }
        pattern_set_lock(pattern, 7, 63, 0, 0x5A);  // buries the run below it
        pattern_clear_step_locks(pattern, 7, 62);   // ...so those 8 slots are abandoned
        pattern_clear_step_locks(pattern, 7, 63);   // this one is at the top: reclaimed at once
    }
}

}  // namespace

// --- copying --------------------------------------------------------------

TEST(copying_a_step_takes_its_locks_out_of_the_pool) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    add_locks(g_pattern, 0, 7, 20, 5);

    range_copy(g_pattern, 0, 7, 1, g_clip);

    CHECK_EQ(int(g_clip.count), 1);
    CHECK_EQ(int(g_clip.lock_count[0]), 5);
    CHECK_EQ(int(range_clip_locks(g_clip)), 5);
    CHECK(same_step_content(g_clip.steps[0], g_pattern.tracks[0].steps[7]));
    for (int k = 0; k < 5; k++) {
        CHECK_EQ(int(g_clip.locks[k].param), 20 + k);
        CHECK_EQ(int(g_clip.locks[k].value), int(lock_value(lock_seed(0, 7), k)));
    }

    // A Step in a clip carries no pool index: the run it was copied from can be relocated or
    // compacted away while the clip waits in the app, so a stale `lock_first` would be a
    // pointer into whatever took its place.
    CHECK_EQ(int(g_clip.steps[0].lock_first), int(kNoLock));
    CHECK_EQ(int(g_clip.steps[0].lock_count), 0);
}

TEST(a_clip_reports_the_locks_it_holds) {
    pattern_init(g_pattern);
    add_locks(g_pattern, 1, 0, 0, 8);
    add_locks(g_pattern, 1, 1, 0, 0);
    add_locks(g_pattern, 1, 2, 3, 3);

    range_copy(g_pattern, 1, 0, 3, g_clip);
    CHECK_EQ(int(g_clip.count), 3);
    CHECK_EQ(int(range_clip_locks(g_clip)), 11);

    // An out-of-range request is not an error, it is an empty clip -- and an empty clip holds
    // no locks rather than whatever the last copy left in the arrays.
    range_copy(g_pattern, 1, 70, 4, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    CHECK_EQ(int(range_clip_locks(g_clip)), 0);
}

// --- pasting --------------------------------------------------------------

TEST(pasting_over_a_step_replaces_its_locks_and_touches_no_other_step) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);

    // Three runs, allocated in this order, so that the destination's sits in the middle of the
    // pool: the step at 20 is the one a paste that shifts or truncates the pool would break.
    add_locks(g_pattern, 0, 0, 0, kLocksPerStep);   // the source: 8 locks
    add_locks(g_pattern, 0, 10, 20, 3);             // the destination: 3 locks
    add_locks(g_pattern, 0, 20, 30, 4);             // above the destination in the pool
    CHECK(g_pattern.tracks[0].steps[10].lock_first < g_pattern.tracks[0].steps[20].lock_first);

    range_copy(g_pattern, 0, 0, 1, g_clip);
    CHECK(range_paste(g_pattern, 0, 10, g_clip));

    // The destination holds the source's content and all 8 of its locks.
    CHECK(same_step_content(g_pattern.tracks[0].steps[10], g_pattern.tracks[0].steps[0]));
    CHECK_EQ(int(pattern_step_locks(g_pattern, 0, 10).count), kLocksPerStep);
    CHECK(locks_are(g_pattern, 0, 10, 0, kLocksPerStep, lock_seed(0, 0)));

    // The step whose run sat above the destination's keeps its own locks, and so does the
    // source. Everything else in the pattern still has none.
    CHECK(locks_are(g_pattern, 0, 20, 30, 4));
    CHECK(locks_are(g_pattern, 0, 0, 0, kLocksPerStep));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 8 + 8 + 4);

    int with_locks = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            if (pattern_step_locks(g_pattern, t, i).count != 0) with_locks++;
        }
    }
    CHECK_EQ(with_locks, 3);

    // And the steps either side of the destination are untouched.
    CHECK_EQ(int(g_pattern.tracks[0].steps[9].note), int(24 + 9));
    CHECK_EQ(int(g_pattern.tracks[0].steps[11].note), int(24 + 11));
}

TEST(pasting_over_the_top_of_the_pool_wastes_no_slots) {
    pattern_init(g_pattern);
    add_locks(g_pattern, 1, 0, 40, 4);   // the source
    add_locks(g_pattern, 0, 5, 0, 2);
    add_locks(g_pattern, 0, 9, 10, 3);   // allocated last, so this run is at the top
    CHECK_EQ(int(g_pattern.locks_used), 9);

    range_copy(g_pattern, 1, 0, 1, g_clip);
    CHECK(range_paste(g_pattern, 0, 9, g_clip));

    // Releasing the destination's run handed its three slots straight back, so the four new
    // ones start where it did and the pool holds no garbage at all.
    CHECK_EQ(int(g_pattern.tracks[0].steps[9].lock_first), 6);
    CHECK_EQ(int(g_pattern.locks_used), 10);
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 10);
    CHECK(locks_are(g_pattern, 0, 9, 40, 4, lock_seed(1, 0)));
    CHECK(locks_are(g_pattern, 0, 5, 0, 2));
    CHECK(locks_are(g_pattern, 1, 0, 40, 4));
}

TEST(a_paste_that_cannot_fit_leaves_the_pattern_byte_for_byte_identical) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    fill_track(g_pattern, 1);
    // 126 steps of 8 locks, each run growing at the top so none of them is abandoned, and then
    // one relocation's worth of garbage on top. The garbage is the point: it leaves the pool
    // with 8 free slots and 8 abandoned ones, so this refusal has to be decided by arithmetic.
    // A paste that compacted the pool and only then found it had to refuse would leave the
    // pattern playing identically and stored differently -- and core/undo.hpp snapshots whole
    // patterns, which would make that an undoable edit that changed nothing.
    for (int s = 0; s < 126; s++) {
        add_locks(g_pattern, s / kStepsPerTrack, s % kStepsPerTrack, 0, kLocksPerStep);
    }
    waste_pool(g_pattern, 0);
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 1008);
    CHECK_EQ(int(g_pattern.locks_used), 1016);
    CHECK(int(g_pattern.locks_used) > int(pattern_locks_reachable(g_pattern)));

    // 24 locks against 8 free and 8 reclaimable by compaction: no arrangement of the pool fits
    // this, which is the only thing a refusal is allowed to mean.
    range_copy(g_pattern, 0, 0, 3, g_clip);
    CHECK_EQ(int(range_clip_locks(g_clip)), 24);

    g_before = g_pattern;
    // Track 4's steps hold no locks, so the paste gets nothing back to pay for the clip's 24.
    CHECK(!range_paste(g_pattern, 4, 0, g_clip));

    CHECK(same_bytes(g_pattern, g_before));
    CHECK(same_lock_views(g_pattern, g_before));
    CHECK(step_is_empty(g_pattern.tracks[4].steps[0]));
    CHECK(step_is_empty(g_pattern.tracks[4].steps[1]));
    CHECK(step_is_empty(g_pattern.tracks[4].steps[2]));
}

TEST(a_paste_that_exactly_fills_the_pool_is_not_refused) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    fill_track(g_pattern, 1);
    for (int s = 0; s < 128; s++) {
        add_locks(g_pattern, s / kStepsPerTrack, s % kStepsPerTrack, 0, kLocksPerStep);
    }
    range_copy(g_pattern, 0, 0, 2, g_clip);

    // The destination's own 16 locks are room the paste gets back, so this fits exactly -- a
    // refusal here would mean the user could never paste over a step on a full pattern.
    CHECK(range_paste(g_pattern, 0, 2, g_clip));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), kLockPool);
    CHECK_EQ(int(g_pattern.locks_used), kLockPool);

    CHECK(same_step_content(g_pattern.tracks[0].steps[2], g_pattern.tracks[0].steps[0]));
    CHECK(same_step_content(g_pattern.tracks[0].steps[3], g_pattern.tracks[0].steps[1]));
    CHECK(locks_are(g_pattern, 0, 2, 0, kLocksPerStep, lock_seed(0, 0)));
    CHECK(locks_are(g_pattern, 0, 3, 0, kLocksPerStep, lock_seed(0, 1)));

    // Every other locked step kept its own run through the compaction this needed.
    int wrong = 0;
    for (int s = 0; s < 128; s++) {
        const int track = s / kStepsPerTrack;
        const int step = s % kStepsPerTrack;
        if (track == 0 && (step == 2 || step == 3)) continue;
        if (!locks_are(g_pattern, track, step, 0, kLocksPerStep)) wrong++;
    }
    CHECK_EQ(wrong, 0);
}

TEST(a_paste_that_only_fits_after_compaction_succeeds) {
    pattern_init(g_pattern);

    // Locks that must survive: 8 steps of track 1 with 2 each, and the two source steps.
    for (int i = 0; i < 8; i++) add_locks(g_pattern, 1, i, 10, 2);
    add_locks(g_pattern, 0, 0, 0, kLocksPerStep);
    add_locks(g_pattern, 0, 1, 0, kLocksPerStep);
    range_copy(g_pattern, 0, 0, 2, g_clip);
    CHECK_EQ(int(range_clip_locks(g_clip)), 16);

    waste_pool(g_pattern, 0);
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 32);   // the garbage reaches nothing
    CHECK(int(pattern_locks_free(g_pattern)) < 16);          // and leaves no room for the clip

    CHECK(range_paste(g_pattern, 2, 4, g_clip));

    // Compaction dropped the abandoned slots, so the pool now holds exactly what is reachable.
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 48);
    CHECK_EQ(int(g_pattern.locks_used), 48);

    CHECK(locks_are(g_pattern, 2, 4, 0, kLocksPerStep, lock_seed(0, 0)));
    CHECK(locks_are(g_pattern, 2, 5, 0, kLocksPerStep, lock_seed(0, 1)));
    CHECK(locks_are(g_pattern, 0, 0, 0, kLocksPerStep));
    CHECK(locks_are(g_pattern, 0, 1, 0, kLocksPerStep));
    for (int i = 0; i < 8; i++) CHECK(locks_are(g_pattern, 1, i, 10, 2));
    CHECK(locks_are(g_pattern, 7, 62, 0, 0));
    CHECK(locks_are(g_pattern, 7, 63, 0, 0));
}

TEST(a_track_round_trips_through_copy_erase_and_paste) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 3);
    g_pattern.tracks[3].speed = uint8_t(Speed::x3_4);
    g_pattern.tracks[3].swing = 62;

    int total = 0;
    for (int i = 0; i < kStepsPerTrack; i++) {
        const int count = i % 5;
        add_locks(g_pattern, 3, i, uint8_t(i & 7), count);
        total += count;
    }
    add_locks(g_pattern, 6, 1, 0, 3);   // another track's locks, to be left alone throughout
    g_before = g_pattern;

    range_copy(g_pattern, 3, 0, kStepsPerTrack, g_clip);
    CHECK_EQ(int(g_clip.count), kStepsPerTrack);
    CHECK_EQ(int(range_clip_locks(g_clip)), total);

    track_erase(g_pattern, 3);
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 3);
    CHECK_EQ(int(g_pattern.tracks[3].length), kStepsPerTrack);
    CHECK_EQ(int(g_pattern.tracks[3].speed), int(Speed::x3_4));
    CHECK_EQ(int(g_pattern.tracks[3].swing), 62);

    CHECK(range_paste(g_pattern, 3, 0, g_clip));

    CHECK_EQ(int(pattern_locks_reachable(g_pattern)),
             int(pattern_locks_reachable(g_before)));
    CHECK(same_lock_views(g_pattern, g_before));
    int wrong = 0;
    for (int i = 0; i < kStepsPerTrack; i++) {
        if (!same_step_content(g_pattern.tracks[3].steps[i], g_before.tracks[3].steps[i])) {
            wrong++;
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK(locks_are(g_pattern, 6, 1, 0, 3));
}

TEST(a_range_is_clipped_to_the_tracks_length_not_to_sixtyfour) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 5);
    g_pattern.tracks[5].length = 16;
    add_locks(g_pattern, 5, 12, 0, 2);
    add_locks(g_pattern, 5, 20, 0, 3);   // past the length: no range may ever see it
    g_before = g_pattern;

    // The grid shows four pages of 16 whatever the length, so a selection can name steps 12-19.
    range_copy(g_pattern, 5, 12, 8, g_clip);
    CHECK_EQ(int(g_clip.count), 4);
    CHECK_EQ(int(range_clip_locks(g_clip)), 2);

    range_copy(g_pattern, 5, 16, 4, g_clip);
    CHECK_EQ(int(g_clip.count), 0);

    range_copy(g_pattern, 5, 0, 200, g_clip);
    CHECK_EQ(int(g_clip.count), 16);
    CHECK_EQ(int(range_clip_locks(g_clip)), 2);

    // An erase stops at the length too: step 20's content and locks are still there.
    range_erase(g_pattern, 5, 8, 40);
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 3);
    CHECK(locks_are(g_pattern, 5, 20, 0, 3));
    CHECK(same_step_content(g_pattern.tracks[5].steps[20], g_before.tracks[5].steps[20]));
    for (int i = 8; i < 16; i++) CHECK(step_is_empty(g_pattern.tracks[5].steps[i]));
    CHECK(same_step_content(g_pattern.tracks[5].steps[7], g_before.tracks[5].steps[7]));
}

TEST(a_clip_pasted_near_the_end_of_a_track_is_truncated_and_succeeds) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    for (int i = 0; i < 4; i++) add_locks(g_pattern, 0, i, 0, 2);

    range_copy(g_pattern, 0, 0, 4, g_clip);
    CHECK_EQ(int(g_clip.count), 4);

    // Two of the four steps fit. Truncating at the end of a track is what the user asked for,
    // so this succeeds rather than refusing a selection they could see.
    CHECK(range_paste(g_pattern, 2, 62, g_clip));
    CHECK(same_step_content(g_pattern.tracks[2].steps[62], g_pattern.tracks[0].steps[0]));
    CHECK(same_step_content(g_pattern.tracks[2].steps[63], g_pattern.tracks[0].steps[1]));
    CHECK(locks_are(g_pattern, 2, 62, 0, 2, lock_seed(0, 0)));
    CHECK(locks_are(g_pattern, 2, 63, 0, 2, lock_seed(0, 1)));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 8 + 4);

    // Nothing of the clip's tail went anywhere, and the source is intact.
    CHECK(step_is_empty(g_pattern.tracks[2].steps[61]));
    for (int i = 0; i < 4; i++) CHECK(locks_are(g_pattern, 0, i, 0, 2));
}

TEST(pasting_an_empty_clip_changes_nothing_and_succeeds) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    add_locks(g_pattern, 0, 3, 0, 4);

    range_copy(g_pattern, 0, 70, 4, g_clip);   // out of range: an empty clip, not an error
    CHECK_EQ(int(g_clip.count), 0);

    g_before = g_pattern;
    CHECK(range_paste(g_pattern, 0, 0, g_clip));
    CHECK(same_bytes(g_pattern, g_before));

    // A destination the track's own length has grown past is the same case: nothing to paste
    // onto, nothing written, and no refusal for the UI to explain.
    g_pattern.tracks[0].length = 8;
    g_before = g_pattern;
    range_copy(g_pattern, 0, 3, 1, g_clip);
    CHECK_EQ(int(g_clip.count), 1);
    CHECK(range_paste(g_pattern, 0, 40, g_clip));
    CHECK(same_bytes(g_pattern, g_before));
}

// --- clearing -------------------------------------------------------------

TEST(range_erase_releases_only_its_own_steps_locks) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    for (int i = 2; i <= 5; i++) add_locks(g_pattern, 0, i, 0, 2);
    g_before = g_pattern;

    range_erase(g_pattern, 0, 3, 2);

    CHECK(step_is_empty(g_pattern.tracks[0].steps[3]));
    CHECK(step_is_empty(g_pattern.tracks[0].steps[4]));
    CHECK(same_step_content(g_pattern.tracks[0].steps[2], g_before.tracks[0].steps[2]));
    CHECK(same_step_content(g_pattern.tracks[0].steps[5], g_before.tracks[0].steps[5]));
    CHECK(locks_are(g_pattern, 0, 2, 0, 2));
    CHECK(locks_are(g_pattern, 0, 5, 0, 2));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 4);
}

TEST(track_erase_clears_every_step_and_keeps_the_layout) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 1);
    g_pattern.tracks[1].length = 8;
    g_pattern.tracks[1].speed = uint8_t(Speed::x1_2);
    g_pattern.tracks[1].swing = 70;
    add_locks(g_pattern, 1, 3, 0, 2);
    add_locks(g_pattern, 1, 40, 0, 3);   // past the length, and still cleared

    track_erase(g_pattern, 1);

    for (int i = 0; i < kStepsPerTrack; i++) CHECK(step_is_empty(g_pattern.tracks[1].steps[i]));
    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 0);
    // Releasing the range backwards unwound the whole chain of runs from the top of the pool,
    // so clearing a track gives its lock budget straight back instead of waiting for a save.
    CHECK_EQ(int(g_pattern.locks_used), 0);

    // Length, speed and swing are the track's layout, not its content: clearing a polymeter
    // track must not silently make it 64 steps again.
    CHECK_EQ(int(g_pattern.tracks[1].length), 8);
    CHECK_EQ(int(g_pattern.tracks[1].speed), int(Speed::x1_2));
    CHECK_EQ(int(g_pattern.tracks[1].swing), 70);
}

TEST(pattern_erase_steps_keeps_the_settings_and_blanks_the_pool) {
    pattern_init(g_pattern);
    g_pattern.seed = 0xC0FFEEu;
    g_pattern.root = 7;
    g_pattern.scale = 3;
    g_pattern.swing = 66;
    g_pattern.master_length = 48;
    g_pattern.tracks[0].length = 16;
    g_pattern.tracks[0].speed = uint8_t(Speed::x2);
    g_pattern.tracks[0].swing = 70;
    g_pattern.tracks[4].length = 7;
    g_pattern.tracks[4].speed = uint8_t(Speed::x1_8);
    g_pattern.tracks[4].swing = 51;

    // The same settings on an otherwise fresh pattern: what an erase should leave behind.
    g_reference = g_pattern;

    fill_track(g_pattern, 0);
    fill_track(g_pattern, 4);
    fill_track(g_pattern, 7);
    add_locks(g_pattern, 0, 1, 0, 4);
    add_locks(g_pattern, 4, 2, 5, 6);
    add_locks(g_pattern, 7, 63, 0, 8);

    pattern_erase_steps(g_pattern);

    CHECK_EQ(int(pattern_locks_reachable(g_pattern)), 0);
    CHECK_EQ(int(g_pattern.locks_used), 0);
    CHECK_EQ(int(g_pattern.seed), 0xC0FFEE);
    CHECK_EQ(int(g_pattern.root), 7);
    CHECK_EQ(int(g_pattern.scale), 3);
    CHECK_EQ(int(g_pattern.swing), 66);
    CHECK_EQ(int(g_pattern.master_length), 48);
    CHECK_EQ(int(g_pattern.tracks[4].length), 7);
    CHECK_EQ(int(g_pattern.tracks[4].speed), int(Speed::x1_8));
    CHECK_EQ(int(g_pattern.tracks[4].swing), 51);

    // Blanking the released slots as well is what makes this hold: a cleared pattern is
    // byte-identical to a fresh one with the same settings, so the undo snapshots and any
    // comparison of two patterns can be a memcmp.
    CHECK(same_bytes(g_pattern, g_reference));
}

// --- small edits ----------------------------------------------------------

TEST(transpose_clamps_per_step_and_moves_untrigged_steps_too) {
    pattern_init(g_pattern);
    g_pattern.tracks[0].length = 20;
    Step *steps = g_pattern.tracks[0].steps;
    steps[0].flags = 0;                 // no trig at all
    steps[0].note = 0;
    steps[1].flags = kStepTrig;
    steps[1].note = 60;
    steps[2].flags = kStepTrigless;     // its locks play against this note
    steps[2].note = 127;
    steps[3].flags = kStepTrig;
    steps[3].note = 1;
    steps[4].flags = 0;
    steps[4].note = 126;
    steps[5].note = 64;                 // outside the span
    steps[19].note = 70;
    steps[20].note = 100;               // outside the track's length

    range_transpose(g_pattern, 0, 0, 5, 3);
    CHECK_EQ(int(steps[0].note), 3);
    CHECK_EQ(int(steps[1].note), 63);
    CHECK_EQ(int(steps[2].note), 127);   // clamped, not wrapped round to 2
    CHECK_EQ(int(steps[3].note), 4);
    CHECK_EQ(int(steps[4].note), 127);
    CHECK_EQ(int(steps[5].note), 64);

    // Clamped per step: the step at the top stays there while the rest keep moving.
    range_transpose(g_pattern, 0, 0, 5, -70);
    CHECK_EQ(int(steps[0].note), 0);
    CHECK_EQ(int(steps[1].note), 0);
    CHECK_EQ(int(steps[2].note), 57);
    CHECK_EQ(int(steps[4].note), 57);

    // Clipped to the length, like every other range.
    range_transpose(g_pattern, 0, 10, 40, 1);
    CHECK_EQ(int(steps[19].note), 71);
    CHECK_EQ(int(steps[20].note), 100);
}

TEST(nudge_clamps_to_the_micro_range) {
    pattern_init(g_pattern);
    Step *steps = g_pattern.tracks[0].steps;
    steps[0].micro = int8_t(-kMicroRange);
    steps[1].micro = -5;
    steps[2].micro = 0;
    steps[3].micro = 10;
    steps[4].micro = int8_t(kMicroRange);
    steps[5].micro = 7;                  // outside the span

    range_nudge(g_pattern, 0, 0, 5, 4);
    CHECK_EQ(int(steps[0].micro), -7);
    CHECK_EQ(int(steps[1].micro), -1);
    CHECK_EQ(int(steps[2].micro), 4);
    CHECK_EQ(int(steps[3].micro), kMicroRange);   // 14 would be a tick the scheduler never sees
    CHECK_EQ(int(steps[4].micro), kMicroRange);
    CHECK_EQ(int(steps[5].micro), 7);

    range_nudge(g_pattern, 0, 0, 5, -1000);
    for (int i = 0; i < 5; i++) CHECK_EQ(int(steps[i].micro), -kMicroRange);
    CHECK_EQ(int(steps[5].micro), 7);
}

// --- bad arguments --------------------------------------------------------

TEST(out_of_range_arguments_change_nothing) {
    pattern_init(g_pattern);
    fill_track(g_pattern, 0);
    fill_track(g_pattern, 7);
    add_locks(g_pattern, 0, 2, 0, 3);
    add_locks(g_pattern, 7, 63, 0, 8);

    range_copy(g_pattern, 0, 0, 2, g_clip);
    CHECK_EQ(int(g_clip.count), 2);
    g_before = g_pattern;

    // A copy that names nothing is an empty clip rather than an error, so the UI has one case
    // to handle and not two.
    range_copy(g_pattern, kTracks, 0, 1, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    range_copy(g_pattern, -1, 0, 1, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    range_copy(g_pattern, 0, -3, 4, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    range_copy(g_pattern, 0, kStepsPerTrack, 4, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    range_copy(g_pattern, 0, 0, 0, g_clip);
    CHECK_EQ(int(g_clip.count), 0);
    range_copy(g_pattern, 0, 0, -5, g_clip);
    CHECK_EQ(int(g_clip.count), 0);

    // A count past the end of the track is clipped, not refused.
    range_copy(g_pattern, 0, 60, 99, g_clip);
    CHECK_EQ(int(g_clip.count), 4);

    range_copy(g_pattern, 0, 0, 2, g_clip);
    CHECK(!range_paste(g_pattern, kTracks + 1, 0, g_clip));
    CHECK(!range_paste(g_pattern, -1, 0, g_clip));
    CHECK(!range_paste(g_pattern, 0, -1, g_clip));
    CHECK(!range_paste(g_pattern, 0, kStepsPerTrack, g_clip));

    range_erase(g_pattern, kTracks, 0, kStepsPerTrack);
    range_erase(g_pattern, -2, 0, 4);
    range_erase(g_pattern, 0, -5, 3);
    range_erase(g_pattern, 0, 0, -1);
    range_erase(g_pattern, 0, 100, 8);
    track_erase(g_pattern, kTracks);
    track_erase(g_pattern, -1);
    range_transpose(g_pattern, kTracks, 0, kStepsPerTrack, 1);
    range_transpose(g_pattern, 0, kStepsPerTrack, 8, 1);
    range_transpose(g_pattern, 0, 0, 0, 1);
    range_nudge(g_pattern, kTracks, 0, kStepsPerTrack, 1);
    range_nudge(g_pattern, 0, -1, 8, 1);
    range_nudge(g_pattern, 0, 0, -8, 1);

    CHECK(same_bytes(g_pattern, g_before));
    CHECK(same_lock_views(g_pattern, g_before));
}
