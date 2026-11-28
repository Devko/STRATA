// Track Launch: one track's part from another pattern slot while the rest keep playing.
//
// Concept agreed with Roland on 2026-11-27 (the "Track Launch" page; docs/CONTROLS.md
// "LAUNCH"). Each of the 8 tracks is a LANE with a SOURCE. By default the source is the playing
// pattern, which is how STRATA always played. Launching sets one lane's source to another slot,
// and from the next boundary that lane plays the same track of that slot while the other lanes
// keep playing the pattern. Nothing is copied: the 128 patterns already are the parts, and a
// borrowed pattern is never changed by being borrowed.
//
// ## The decisions this module encodes
//
// - **Same lane only.** Track 3 of B07 plays on track 3: the kit is per project, so the part
//   arrives with its own sound and its locks still mean the same parameters.
// - **A pattern change sends every lane back to its own part, except pinned lanes.** A pinned
//   lane keeps what it plays; a pinned lane on its own part keeps the pattern being LEFT (pin
//   track 3 on A01, change to A02, and track 3 keeps A01's bass).
// - **One way to say "own".** A source that names the playing pattern is stored as 0: after a
//   pattern change onto the slot a lane borrows from, that lane plays its own part again.
// - **Read-only while borrowed.** The engine refuses edits on a borrowed lane (CAPTURE first);
//   this module only answers `lane_borrowed`.
//
// Where a launch lands is core/queue.hpp's arithmetic, reused as it is, one Queue per lane.
//
// core/ never includes Enaga headers. Nothing here allocates; CAPTURE copies about 10 KiB.
#pragma once

#include "pattern.hpp"
#include "project.hpp"
#include "queue.hpp"

#include <cstdint>

namespace strata {

// --- sources ----------------------------------------------------------------

// A lane's source byte, as PROJECT.BIN's section 8 stores it: 0 its own part, 1-128 the same
// track of pattern A01-H16.
constexpr uint8_t kLaneOwn = 0;

constexpr bool lane_source_valid(uint8_t source) { return source <= uint8_t(kProjectPatterns); }

// The source for pattern `bank`/`slot`, or kLaneOwn for an index no pattern exists at.
constexpr uint8_t lane_source(int bank, int slot) {
    return project_index_valid(bank, slot) ? uint8_t(bank * kPatternsPerBank + slot + 1)
                                           : kLaneOwn;
}

// The pattern a non-own source names. Undefined for kLaneOwn: ask lane_source_valid and != 0.
// A shift and a mask, not a divide: the engine asks once per lane per tick.
static_assert(kPatternsPerBank == 16, "a source splits into bank and slot by four bits");
constexpr int lane_source_bank(uint8_t source) { return (int(source) - 1) >> 4; }
constexpr int lane_source_slot(uint8_t source) { return (int(source) - 1) & 15; }

// The playing pattern as a source.
uint8_t lane_live_source(const Project &project);

// Whether lane `track` plays another pattern's part.
bool lane_borrowed(const Project &project, int track);

// Every borrowed lane, bit t = lane t.
uint8_t lane_borrowed_mask(const Project &project);

// The pattern lane `track` plays: its source, or the playing pattern. Safe for any byte: a damaged
// source reads as the playing pattern.
const Pattern &lane_pattern(const Project &project, int track);

// The source lane `track` plays, never kLaneOwn: the playing pattern's when it plays its own part.
uint8_t lane_playing_source(const Project &project, int track);

// --- launching --------------------------------------------------------------

// Lane `track` plays `source` from now on. A source naming the playing pattern is stored as its
// own part. False, changing nothing, for a lane or a source that does not exist.
bool launch_set(Project &project, int track, uint8_t source);

// Pin or unpin lane `track` (bit t of `lane_pinned`). False for a lane that does not exist.
bool launch_toggle_pin(Project &project, int track);

// A pattern change has happened: `project.bank`/`slot` are already the new pattern, and
// `old_bank`/`old_slot` the one left. Unpinned lanes go back to their own part. A pinned lane on
// its own part keeps the pattern left; a pinned borrowed lane keeps its source. A source that is
// now the playing pattern becomes the lane's own part.
//
// Returns the lanes that, after the change, play the new pattern's part AND did not before: the
// engine starts them on the new pattern's start. A pinned lane that keeps playing what it played
// keeps its own start, even when its source happens to become the playing pattern.
uint8_t launch_pattern_changed(Project &project, int old_bank, int old_slot);

// Every lane back on its own part; pins kept. CAPTURE ends with this.
void launch_all_own(Project &project);

// --- the per-lane queue -----------------------------------------------------

// One armed launch. `queue` carries the timing (core/queue.hpp: switch tick, restart, start
// tick); its bank and slot are not read. `source` is what the lane plays when it lands.
struct LaneQueue {
    Queue queue;
    uint8_t source = kLaneOwn;
};

// Arm a launch of `source` on a lane, with the playing pattern's queue arithmetic: `pattern_ticks`
// its master loop, `origin` its start. False, leaving the queue as it was, for a source that does
// not exist.
bool lane_queue_arm(LaneQueue &lane, uint8_t source, QueueMode mode, uint32_t armed_tick,
                    int pattern_ticks, uint32_t origin);

bool lane_queue_due(const LaneQueue &lane, uint32_t tick);
void lane_queue_clear(LaneQueue &lane);

// --- CAPTURE ------------------------------------------------------------------

// Whether a pattern has any step that plays (a trig or a trigless) on any track.
bool pattern_has_steps(const Pattern &pattern);

// The first slot of `bank` that CAPTURE may write: no steps, not the playing pattern and not any
// lane's source. -1 when there is none.
int launch_capture_slot(const Project &project, int bank);

// The same, and not any of the `count` sources in `avoid` either (kLaneOwn entries are skipped):
// ARR REC writes what a pass played after the pass, when what plays may already be something
// else, and neither may be written over.
int launch_capture_slot_avoiding(const Project &project, int bank, const uint8_t *avoid,
                                 int count);

enum class CaptureResult : uint8_t {
    ok,
    no_slot,       // `bank`/`slot` is not a slot CAPTURE may write (launch_capture_slot)
    locks_full,    // the parts' locks together do not fit in one pattern's pool
};

// Write what every lane plays into `out`, which must be none of the project's patterns that a
// lane reads (the playing pattern or a source): each lane's part with its locks compacted into
// `out`'s pool, and the playing pattern's master length, swing, FEEL, key, scale and seed. False,
// writing nothing, when the parts' locks together do not fit in one pool. CAPTURE and LOCK both
// take what you hear through this.
bool launch_compose(const Project &project, Pattern &out);

// launch_compose for a mix that is not (or no longer) the project's own: `live` is the playing
// pattern's source and `lanes[t]` lane t's (kLaneOwn: its own part of `live`). ARR REC writes a
// pass this way once the pass is over. False, writing nothing, for an invalid `live` or a pool
// that does not fit; an invalid lane source reads as the lane's own part.
bool launch_compose_lanes(const Project &project, uint8_t live, const uint8_t lanes[kTracks],
                          Pattern &out);

// Write what every lane plays into pattern `bank`/`slot`: each lane's part from its source, with
// its locks moved into the new pattern's pool (compacted), and the playing pattern's master
// length, swing, FEEL, key, scale and seed. Nothing is written unless all of it fits. The
// sources and the launch state are left as they are: the caller switches to the new slot.
//
// What does not carry over: a borrowed part drew its probability and random ranges from its
// own pattern's seed, and in the new pattern it draws from the playing pattern's.
CaptureResult launch_capture(Project &project, int bank, int slot);

}  // namespace strata
