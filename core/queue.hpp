// Queued pattern changes and chains: when does the next pattern take over.
//
// docs/SPEC.md, "Pattern changes and chains": "The next pattern is queued and switches at the end
// of the current one, or after 16 or 32 steps, or at once keeping the position", and "Chains: up
// to 64 entries per project, each a pattern plus a repeat count. No song timeline in v1."
//
// ## Why this is arithmetic and not a timer
//
// The switch point is a tick number, decided when the change is armed, and the audio side only
// ever asks "is it this tick yet". Nothing here counts time, holds a deadline or runs a clock:
// Enaga owns time (CLAUDE.md) and `core/timing` owns the grid. That also makes the whole thing
// testable from a synthetic tick stream, like the scheduler.
//
// The one real subtlety is that a switch has to land on a tick the sequencer will actually see.
// Enaga's clock delivers every tick in order, so "the first tick at or after X" is reachable --
// but the test is `tick >= switch_tick`, never `tick == switch_tick`, because a block can hold
// several ticks and a transport that was stopped and started lands wherever it lands. An
// equality test would hang a queued change forever the first time a tick was skipped.
//
// ## Position is kept across a switch, deliberately
//
// STRATA does not restart the sequencer on a pattern change: the tick keeps running and the new
// pattern's steps are resolved from it, so a 16-step pattern followed by a 15-step one stays in
// time with the bar instead of starting over. That is what makes a chain of unequal patterns
// musical rather than a stutter, and it falls out of core/timing deriving everything from the
// absolute tick.
//
// It is also what makes the grace window below cost something: a switch that lands a few ticks
// into a pass arrives after that pass's step 0 has already played. See `queue_switch_tick`.
#pragma once

#include "pattern.hpp"
#include "project.hpp"
#include "timing.hpp"

#include <cstdint>

namespace strata {

// When a queued change takes effect. The numbers are not serialized -- a queue is live state,
// never saved -- so they are free to renumber.
enum class QueueMode : uint8_t {
    end_of_pattern = 0,   // the default: at the end of the current pass
    after_16,             // at the next 16-step boundary
    after_32,
    immediate,            // on the very next tick, keeping the position
    count
};

const char *queue_mode_name(QueueMode mode);   // "END", "16", "32", "NOW"

// One armed pattern change.
struct Queue {
    bool pending = false;
    uint8_t bank = 0;
    uint8_t slot = 0;
    QueueMode mode = QueueMode::end_of_pattern;
    uint32_t switch_tick = 0;    // the first tick at which it should happen
};

// The master loop in ticks: `master_length` steps at 1x, which is what `queue_switch_tick`'s
// `pattern_ticks` means and what "the end of the current one" means for a pattern whose tracks
// may each have their own length.
//
// Here rather than hand-written at each call site, because every caller needs the same clamp and
// two of them would eventually disagree about it: a `master_length` of 0 is zeroed memory or a
// damaged file, and it has to mean one step rather than a loop of no ticks at all.
constexpr int master_loop_ticks(const Pattern &pattern) {
    const int steps = int(pattern.master_length);
    const int clamped = steps < 1 ? 1 : (steps > kMaxSteps ? kMaxSteps : steps);
    return clamped * kTicksPerSixteenth;
}

// How late a press may be and still count as the boundary it has just missed: half a sixteenth,
// about 58 ms at 130 BPM. Wide enough for an ordinary late press, and narrower than a step at 1x
// (24 ticks), so a forgiven press never reaches into the next step.
constexpr int kQueueGraceTicks = kTicksPerSixteenth / 2;    // 12

// The tick a change armed at `armed_tick` should take effect on.
//
// `pattern_ticks` is the current pattern's master loop in ticks -- `master_loop_ticks` above. A
// `pattern_ticks` of 0 or less is treated as one 16th, so a damaged value cannot divide by zero
// or arm a change that never comes.
//
// end_of_pattern rounds up to the next multiple of `pattern_ticks`; after_16 and after_32 round
// up to the next 16- or 32-step boundary of the 1x grid. Those two are boundaries of the grid
// rather than "16 steps from where you pressed": docs/SPEC.md says only "after 16 or 32 steps",
// and the grid is a reading of it, chosen because "at the next bar" is what a performer means.
// Written down because it was a choice and not the only one.
//
// A change can never take effect on the boundary tick itself -- by the time it is armed, that
// tick has been played -- so outside the grace window below, a change armed on or after a
// boundary waits for the NEXT one.
//
// ## The grace window, and what it costs
//
// Within `kQueueGraceTicks` AFTER a boundary, the change takes effect on the very next tick
// instead of waiting a whole period. A press a few milliseconds late is a human being on time,
// and charging it a whole pattern -- 7.4 s on a 64-step pattern at 130 BPM -- would make the
// feature unusable in performance, which is the only place it is used.
//
// **The price is the other half of this header: position is kept across a switch, so a forgiven
// press means the new pattern's step 0 has already gone by.** It fired from the OLD pattern, a
// few ticks before the switch landed, and the new pattern comes in partway through its own first
// step -- first heard on step 1. That is the right trade for a performance instrument: the change
// happens when the player meant it, and one trig at the seam is a smaller surprise than a bar and
// a half of the pattern they were trying to leave. But it is emphatically not free, and a reader
// who saw only "late presses are forgiven" would be caught out by it on the unit.
uint32_t queue_switch_tick(QueueMode mode, uint32_t armed_tick, int pattern_ticks);

// Arm a change. False, with the queue untouched, for an index no pattern exists at.
bool queue_arm(Queue &queue, int bank, int slot, QueueMode mode, uint32_t armed_tick,
               int pattern_ticks);

// Is the queued change due on this tick? True exactly once per armed change: the caller then
// performs the switch and calls `queue_clear`.
//
// `>=`, not `==`, for the reason in the header comment.
bool queue_due(const Queue &queue, uint32_t tick);

void queue_clear(Queue &queue);

// --- chains ---------------------------------------------------------------

// Where a chain has got to. Live state, not saved: a project reopens at the start of its chain.
struct ChainPlayer {
    bool active = false;
    uint8_t entry = 0;       // which chain entry is playing
    uint8_t played = 0;      // how many passes of it have finished
};

// How many entries of `project.chain` are usable: entries are read in order and the first one
// with `repeats == 0` ends the chain, since that is what zeroed memory means (core/project.hpp).
// Also bounded by `settings.chain_length` and by the entries naming patterns that exist.
int chain_length(const Project &project);

// Start a chain at its first entry. False when the chain is empty, leaving `player` inactive.
bool chain_start(const Project &project, ChainPlayer &player);

// One pass of the current entry has finished. Advances `played`, and when the entry's repeats are
// used up moves to the next entry, wrapping at the end -- a chain loops, it does not stop.
//
// Returns true when the pattern should change, with `bank` and `slot` set to the new entry's.
// Returns false when the same pattern simply repeats again -- including when the player has
// moved to the NEXT ENTRY and that entry names the pattern already playing, as a one-entry chain
// does on every pass. `bank` and `slot` are then untouched, and `chain_current` says where the
// chain is. The test is the pattern, not the entry, because a pattern change is not free: it
// clears the undo history and restarts the `1ST` conditions.
bool chain_advance(const Project &project, ChainPlayer &player, uint8_t &bank, uint8_t &slot);

// The pattern the chain is currently on, for the screen. False when the chain is not active.
bool chain_current(const Project &project, const ChainPlayer &player, uint8_t &bank,
                   uint8_t &slot);

}  // namespace strata
