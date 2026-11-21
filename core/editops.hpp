// Copy, paste and clear for steps, ranges and tracks.
//
// docs/SPEC.md, "Pattern changes and chains": "Copy, paste and clear for steps, tracks and
// patterns", and "Panel mapping": "Hold two STEP keys: set the selection range for copy, clear
// and mutation." So the unit of all three is a RANGE of steps on one track -- a single step is a
// range of one and a whole track is a range of 64, which is why there is one clipboard type
// here and not three.
//
// ## The locks are the hard part
//
// A step's locks live in the pattern's shared pool, not in the step, so copying a step means
// copying its locks out of the pool and pasting means putting them back. The pool is
// bump-allocated and can be full, and pasting over a step has to release what was there first.
// Every function here therefore reports failure, and a failed paste leaves the pattern exactly
// as it was -- a half-pasted range would be worse than a refused one, because the user would
// have to work out which steps made it.
//
// ## Where this runs
//
// Audio context, from app/engine.cpp, like every other pattern edit (the UI never writes
// pattern memory -- docs/SPEC.md, "Software architecture"). Nothing here allocates; the
// clipboard's storage belongs to its owner. A RangeClip is about 1.9 KiB, so it lives in the
// app, not on a stack frame.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// A copied span of steps with their locks. One step, a selection or a whole track.
struct RangeClip {
    uint8_t count = 0;                                  // 0 = empty, up to kStepsPerTrack
    Step steps[kStepsPerTrack];
    uint8_t lock_count[kStepsPerTrack] = {};            // per copied step
    Lock locks[kStepsPerTrack * kLocksPerStep];         // run-packed, in step order
};

// How many locks `clip` holds in total -- what a paste needs room for.
uint16_t range_clip_locks(const RangeClip &clip);

// --- copying --------------------------------------------------------------

// Copy `count` steps starting at `first` of `track` into `clip`, with their locks. The span is
// clipped to the track's stored length, so a range selected on a 64-step page of a 16-step
// track copies only the steps that exist. `clip.count` says how many were taken; it is 0 for an
// out-of-range request, which is not an error.
void range_copy(const Pattern &pattern, int track, int first, int count, RangeClip &clip);

// --- pasting --------------------------------------------------------------

// Write `clip` onto `track` starting at `first`, locks included, replacing whatever was there.
//
// All or nothing. False, with the pattern unchanged, when the clip's locks cannot be made to
// fit: the destination steps' own locks are counted as reclaimable and the pool is compacted
// if that would be enough, so a refusal means the pattern genuinely has no room. The span is
// clipped to the track's length; a clip longer than the space left is pasted as far as it fits
// and still succeeds, because truncating at the end of a track is what the user asked for.
//
// An impossible destination is false as well -- a track outside 0..7, or a `first` off the
// 64-step grid. A `first` that is on the grid but past this track's own length, like an empty
// clip, writes nothing and returns true: that is the truncation rule again, which leaves the
// only refusal the UI can actually reach meaning one thing, that the pool is full.
bool range_paste(Pattern &pattern, int track, int first, const RangeClip &clip);

// --- clearing -------------------------------------------------------------

// Reset `count` steps from `first` to the empty step and release their locks. Clipped to the
// track's length.
void range_erase(Pattern &pattern, int track, int first, int count);

// Every step of a track. Length, speed and swing are the track's layout rather than its
// content, and are deliberately kept -- clearing a polymeter track should not silently make it
// 64 steps again.
void track_erase(Pattern &pattern, int track);

// Every step of every track, and every lock. Keeps the pattern's seed, root, scale, swing and
// per-track layout, for the same reason.
void pattern_erase_steps(Pattern &pattern);

// --- small edits the UI needs ---------------------------------------------

// Transpose a range by `semitones`, clamped to 0..127 per step rather than wrapping: a note
// that would leave the range sticks at the end, which is what a hardware sequencer does.
void range_transpose(Pattern &pattern, int track, int first, int count, int semitones);

// Nudge a range's micro-timing by `ticks`, clamped to +/- kMicroRange per step.
void range_nudge(Pattern &pattern, int track, int first, int count, int ticks);

}  // namespace strata
