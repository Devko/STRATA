// Copy, paste and clear for steps, ranges and tracks.
//
// ## A paste decides before it writes
//
// editops.hpp promises that a failed paste leaves the pattern exactly as it was, and the lock
// pool is what makes that hard: the clip's locks have to go somewhere, the destination steps'
// own locks are room the paste gets back, and compaction can free more still. So a paste works
// out arithmetically what the pattern would hold afterwards -- reachable locks, minus what the
// destination gives back, plus what the clip needs, against kLockPool -- and writes only once
// that answer is yes. Nothing here rolls a half-finished paste back, because nothing here ever
// starts one.
//
// Compaction is the last resort rather than the first for the same reason. It keeps every step's
// locks, but it rewrites every `lock_first` in the pattern and blanks the slots above the packed
// runs, so compacting and *then* refusing would leave a pattern that is observably identical and
// byte-for-byte different. core/undo.hpp snapshots whole patterns, so that difference is not
// academic: it would make a refused paste an undoable edit.
//
// ## Ranges are clipped, not refused
//
// A track stores its own length (polymeter) while the grid always shows four pages of 16, so a
// two-key selection can perfectly well name steps the track does not have. Those are dropped
// rather than refused: the user selected what was on the panel. `track_erase` is the one
// exception and clears all 64 steps, because a step past the end still holds whatever a longer
// length left there, and "clear the track" means the track.
#include "editops.hpp"

namespace strata {

namespace {

bool track_ok(int track) { return track >= 0 && track < kTracks; }

int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// How many steps of `track` a request for `count` steps from `first` actually names, after
// clipping to the track's stored length. Zero for anything out of range, which every caller
// here treats as "nothing to do".
int span_of(const TrackPattern &track, int first, int count) {
    if (first < 0 || count <= 0) return 0;
    const int length = clamp_int(int(track.length), 0, kStepsPerTrack);
    if (first >= length) return 0;
    const int room = length - first;
    return count < room ? count : room;
}

// A clip's own fields are caller-owned memory like everything else in core/, so they are read
// through these two rather than trusted: clamping the counts is what keeps the run packing
// inside `clip.locks`, whose kStepsPerTrack * kLocksPerStep entries are exactly the most that
// kStepsPerTrack clamped counts can address.
int clip_span(const RangeClip &clip) { return clamp_int(int(clip.count), 0, kStepsPerTrack); }

int clip_locks_at(const RangeClip &clip, int step) {
    return clamp_int(int(clip.lock_count[step]), 0, kLocksPerStep);
}

// Release the locks of a span, highest step first. `pattern_clear_step_locks` hands slots back
// only when the run it releases ends at the top of the pool, and a user filling a track in
// locks it up in step order, so going backwards unwinds the whole span instead of leaving it as
// garbage for the next compaction.
void release_span_locks(Pattern &pattern, int track, int first, int span) {
    for (int i = span - 1; i >= 0; i--) pattern_clear_step_locks(pattern, track, first + i);
}

}  // namespace

uint16_t range_clip_locks(const RangeClip &clip) {
    const int span = clip_span(clip);
    int total = 0;
    for (int i = 0; i < span; i++) total += clip_locks_at(clip, i);
    return uint16_t(total);
}

// --- copying --------------------------------------------------------------

void range_copy(const Pattern &pattern, int track, int first, int count, RangeClip &clip) {
    clip.count = 0;
    if (!track_ok(track)) return;

    const int span = span_of(pattern.tracks[track], first, count);
    uint16_t write = 0;
    for (int i = 0; i < span; i++) {
        const LockView view = pattern_step_locks(pattern, track, first + i);
        clip.steps[i] = pattern.tracks[track].steps[first + i];
        // A Step in a clip is content only. `lock_first` indexes the pattern's pool, which the
        // clip is not part of, and the run it names can be relocated or compacted away while
        // the clip sits in the app waiting to be pasted -- so the index does not travel. The
        // clip's own copy of the locks is the only thing a paste reads.
        clip.steps[i].lock_first = kNoLock;
        clip.steps[i].lock_count = 0;
        clip.lock_count[i] = view.count;
        for (uint8_t k = 0; k < view.count; k++) clip.locks[write++] = view.locks[k];
    }
    clip.count = uint8_t(span);
}

// --- pasting --------------------------------------------------------------

bool range_paste(Pattern &pattern, int track, int first, const RangeClip &clip) {
    if (!track_ok(track) || first < 0 || first >= kStepsPerTrack) return false;

    TrackPattern &dst = pattern.tracks[track];
    const int span = span_of(dst, first, clip_span(clip));
    if (span == 0) return true;   // an empty clip, or a destination this track has grown past

    int need = 0;
    for (int i = 0; i < span; i++) need += clip_locks_at(clip, i);

    // Room at the top is the common case and needs no compaction: releasing the destination's
    // locks can only make `pattern_locks_free` bigger, so a pool that has `need` slots free now
    // still has them when the runs are written.
    const bool compact = int(pattern_locks_free(pattern)) < need;
    if (compact) {
        // Otherwise the question is not "is there room now" but "is there room for what the
        // pattern will hold afterwards": overwriting a step releases its locks, and compaction
        // reclaims those along with whatever relocation has abandoned. If the answer is no, it
        // is no for good -- the pool genuinely cannot hold the result -- and the pattern has not
        // been touched yet.
        int reclaim = 0;
        for (int i = 0; i < span; i++) {
            reclaim += pattern_step_locks(pattern, track, first + i).count;
        }
        if (int(pattern_locks_reachable(pattern)) - reclaim + need > kLockPool) return false;
    }

    release_span_locks(pattern, track, first, span);
    // Now that the destination's runs are unreachable, compaction drops them as well, which is
    // what turns the arithmetic above into actual free slots at the top.
    if (compact) pattern_compact_locks(pattern);

    uint16_t read = 0;
    for (int i = 0; i < span; i++) {
        Step &step = dst.steps[first + i];
        step = clip.steps[i];
        const int locks = clip_locks_at(clip, i);
        // The test above guarantees the room. The bound is checked anyway because a pattern
        // built by hand rather than by this API can hold overlapping runs, which makes
        // `pattern_locks_reachable` report fewer locks than the pool really holds; a step that
        // loses its locks is recoverable where a write past the pool is not.
        if (locks != 0 && uint32_t(pattern.locks_used) + uint32_t(locks) <= uint32_t(kLockPool)) {
            const uint16_t at = pattern.locks_used;
            for (int k = 0; k < locks; k++) pattern.locks[at + k] = clip.locks[read + k];
            pattern.locks_used = uint16_t(at + locks);
            step.lock_first = at;
            step.lock_count = uint8_t(locks);
        }
        read = uint16_t(read + locks);
    }
    return true;
}

// --- clearing -------------------------------------------------------------

void range_erase(Pattern &pattern, int track, int first, int count) {
    if (!track_ok(track)) return;
    const int span = span_of(pattern.tracks[track], first, count);
    release_span_locks(pattern, track, first, span);
    for (int i = 0; i < span; i++) step_clear(pattern.tracks[track].steps[first + i]);
}

void track_erase(Pattern &pattern, int track) {
    if (!track_ok(track)) return;
    // All 64 steps rather than the track's length: see the header comment. Length, speed and
    // swing are the track's layout and are deliberately left alone.
    release_span_locks(pattern, track, 0, kStepsPerTrack);
    for (int i = 0; i < kStepsPerTrack; i++) step_clear(pattern.tracks[track].steps[i]);
}

void pattern_erase_steps(Pattern &pattern) {
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) step_clear(pattern.tracks[t].steps[i]);
    }
    // Nothing reaches the pool now, so the bump pointer goes straight back to the bottom
    // instead of waiting for a compaction. Blanking the slots costs 2 KiB of stores once and
    // leaves a cleared pattern byte-identical to a fresh one, which the undo snapshots and any
    // comparison of two patterns can then rely on.
    for (int i = 0; i < kLockPool; i++) pattern.locks[i] = Lock{};
    pattern.locks_used = 0;
}

bool pattern_double(Pattern &pattern, RangeClip &scratch) {
    for (int t = 0; t < kTracks; t++) {
        TrackPattern &track = pattern.tracks[t];
        const int length = int(track.length);
        if (length >= kStepsPerTrack || length < 1) continue;
        const int doubled = length * 2 > kStepsPerTrack ? kStepsPerTrack : length * 2;
        range_copy(pattern, t, 0, length, scratch);
        track.length = uint8_t(doubled);
        if (!range_paste(pattern, t, length, scratch)) {
            track.length = uint8_t(length);
            return false;
        }
    }
    const int master = int(pattern.master_length) * 2;
    pattern.master_length = uint8_t(master > kStepsPerTrack ? kStepsPerTrack : master);
    return true;
}

// --- small edits the UI needs ---------------------------------------------

void range_transpose(Pattern &pattern, int track, int first, int count, int semitones) {
    if (!track_ok(track)) return;
    const int span = span_of(pattern.tracks[track], first, count);
    // Clamping the amount first keeps the sum below in range whatever an encoder accumulator
    // hands over; +/-127 already saturates every note, so this changes no result.
    const int by = clamp_int(semitones, -127, 127);
    for (int i = 0; i < span; i++) {
        Step &step = pattern.tracks[track].steps[first + i];
        // Every step in the span, trig or not. A trigless step's note is what its locks play
        // against, and an untrigged step keeps the note it was left with, so transposing only
        // the trigs would make half the range come back wrong when those steps are turned on.
        step.note = uint8_t(clamp_int(int(step.note) + by, 0, 127));
    }
}

void range_nudge(Pattern &pattern, int track, int first, int count, int ticks) {
    if (!track_ok(track)) return;
    const int span = span_of(pattern.tracks[track], first, count);
    const int by = clamp_int(ticks, -127, 127);
    for (int i = 0; i < span; i++) {
        Step &step = pattern.tracks[track].steps[first + i];
        // Clamped per step, not wrapped: a step already at the edge of the window stays there
        // while the rest of the range keeps moving, which is what nudging a selection on
        // hardware does. Wrapping would jump a step a whole 16th the other way.
        step.micro = int8_t(clamp_int(int(step.micro) + by, -kMicroRange, kMicroRange));
    }
}

}  // namespace strata
