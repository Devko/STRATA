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
// ## Where the new pattern starts
//
// docs/SPEC.md: the next pattern "switches at the end of the current one, or after 16 or 32
// steps, or at once keeping the position". Only the last of those keeps the position. The other
// three START the new pattern: every track begins at its step 1 on the boundary, whatever its
// length. The queue says so by handing back `start_tick`, the boundary the switch belongs to, and
// the engine measures the new pattern's ticks from there (app/engine.cpp, `origin_`).
//
// An earlier version kept the position for every mode, on the theory that a pattern of unequal
// lengths "stays in time". It did not: the new pattern's tracks were resolved from the absolute
// tick, so a 64-step track came in on step 16 at the first 16-step boundary and a 12-step track
// on step 4. Only `immediate` keeps the position now, because that is the one mode that asks to.
//
// The grace window below still has a cost: a switch that lands a few ticks after its boundary
// starts the pattern at that boundary, so the new pattern's step 1 has already gone by. See
// `queue_switch_tick`.
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
    // Whether the new pattern starts from its step 1 (every mode but `immediate`), and the tick
    // its step 1 belongs to: the boundary, which is `switch_tick` itself unless the press was
    // forgiven by the grace window, when it is the boundary just missed.
    bool restart = false;
    uint32_t start_tick = 0;
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
// **The price: a forgiven press means the new pattern's step 0 has already gone by.** The new
// pattern is started at the boundary the press missed (`Queue::start_tick`), so it stays on the
// grid, but its step 0 fired from the OLD pattern a few ticks before the switch landed, and the
// new pattern is first heard on step 1. That is the right trade for a performance instrument: the change
// happens when the player meant it, and one trig at the seam is a smaller surprise than a bar and
// a half of the pattern they were trying to leave. But it is emphatically not free, and a reader
// who saw only "late presses are forgiven" would be caught out by it on the unit.
uint32_t queue_switch_tick(QueueMode mode, uint32_t armed_tick, int pattern_ticks);

// Arm a change. False, with the queue untouched, for an index no pattern exists at.
//
// `origin` is the tick the current pattern started on, so that "the end of the current one" is
// counted from its own start rather than from tick 0: after a 15-step pattern has started on a
// 16-step boundary, its passes end 15 steps apart from THERE. The 16- and 32-step modes are bars
// of the transport and ignore it. An `origin` after `armed_tick` (the transport relocated) reads
// as 0.
bool queue_arm(Queue &queue, int bank, int slot, QueueMode mode, uint32_t armed_tick,
               int pattern_ticks, uint32_t origin = 0);

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
    // The entry that was playing has been removed from the chain (`chain_player_removed`).
    // The pattern goes on to the end of its pass, and `entry` is then the one that comes next:
    // it starts on the following boundary, from its first pass.
    bool orphaned = false;
};

// How many entries of `project.chain` are usable: entries are read in order and the first one
// with `repeats == 0` ends the chain, since that is what zeroed memory means (core/project.hpp).
// Also bounded by `settings.chain_length` and by the entries naming patterns that exist.
int chain_length(const Project &project);

// Start a chain at its first entry. False when the chain is empty, leaving `player` inactive.
bool chain_start(const Project &project, ChainPlayer &player);

// Follow the chain again without moving off the live pattern: a project reopened with its chain
// on. The live pattern plays out its pass and the chain then continues from its first entry --
// at once, if the live pattern IS the first entry. False when the chain is empty.
bool chain_resume(const Project &project, ChainPlayer &player);

// One pass of the current entry has finished. Advances `played`, and when the entry's repeats are
// used up moves to the next entry, wrapping at the end -- a chain loops, it does not stop.
//
// Returns true when the pattern should change, with `bank` and `slot` set to the new entry's.
// Returns false when the same pattern simply repeats again -- including when the player has
// moved to the NEXT ENTRY and that entry names the pattern already playing, as a one-entry chain
// does on every pass. `bank` and `slot` are then untouched, and `chain_current` says where the
// chain is. The test is the pattern, not the entry, because a pattern change is not free: it
// clears the undo history and restarts the `1ST` conditions. "Already playing" means the LIVE
// pattern, `project.bank` / `project.slot`, not the entry being left: the two differ once the
// playing entry has been edited, and comparing with the entry then kept the old pattern playing
// under the next entry's name.
bool chain_advance(const Project &project, ChainPlayer &player, uint8_t &bank, uint8_t &slot);

// Song position in: where a chain started at tick 0 and played without a break has got to at
// `tick`. An outside sequencer's MIDI song position pointer locates Enaga's clock (it sends
// ENAGA_TRANSPORT_LOCATE under MIDI clock), and STRATA puts the chain where that position falls
// rather than carrying on from wherever it stopped.
//
// Sets `player` to the entry and its finished passes, and `origin` to the tick that entry's
// pattern started on: the first of a run of consecutive entries naming the same pattern, which
// is where `chain_advance` leaves it (it does not restart a pattern that simply repeats). A
// looping chain wraps; in song mode a tick at or past the song's end is `over`, with the player
// back on entry 1 and `origin` at the end, as a song that played out leaves it. False, with the
// player inactive, for an empty chain.
bool chain_locate(const Project &project, uint32_t tick, ChainPlayer &player, uint32_t &origin,
                  bool &over);

// Song mode: is the pass now ending the chain's last? True on the last pass of the last entry,
// which is where a song stops instead of wrapping. False for an inactive player, an empty chain
// and a player whose entry was removed (it has a next entry to go to).
bool chain_at_end(const Project &project, const ChainPlayer &player);

// The mute state the chain's current entry recalls, 0-7, or -1 for none. Read when an entry
// starts: `chain_start`, and a `chain_advance` that left `played` at 0.
int chain_entry_mutes(const Project &project, const ChainPlayer &player);

// --- editing a chain (PROJECT's CHAIN page) --------------------------------
//
// Each edit keeps the chain what `chain_length` reads: a prefix of usable entries with every
// entry after it zeroed, and `settings.chain_length` equal to the prefix. A chain whose stored
// length and entries disagreed (a damaged file) is first cut back to the usable prefix. All
// three refuse an index outside the chain, a pattern that cannot exist and a repeat count of
// 0, leaving the project as it was.

// Replace entry `at` (0 .. length-1). `mutes` is the entry's mute recall, 0-8
// (`ChainEntry::mutes`); past 8 is refused. The entry's section fields stay, except that a mute
// recall drops a CAP: MUT is one field holding `--`, M1-M8 or CAP.
bool chain_set(Project &project, int at, uint8_t bank, uint8_t slot, uint8_t repeats,
               uint8_t mutes = 0);

// Insert a new entry so that it becomes entry `at` (0 .. length); the entries from `at` on move
// up one. False when the chain already holds kChainEntries.
//
// `extra` is the new entry's section fields (a copy of an entry carries its own); none gives
// every field `--`. A mute recall drops a CAP, as `chain_set` does; fields `section_valid`
// refuses refuse the insert.
bool chain_insert(Project &project, int at, uint8_t bank, uint8_t slot, uint8_t repeats,
                  uint8_t mutes = 0, const SectionExtra *extra = nullptr);

// Remove entry `at` (0 .. length-1); the entries after it move down one.
bool chain_remove(Project &project, int at);

// --- song sections (core/project.hpp, SectionExtra) ------------------------

// True when every field is in its range: CAP 0 or 1 with no mask unless set, SCN 0-16, MRP
// 0-3, NRG 0-129, BPM 0 or kTempoCentiMin .. kTempoCentiMax.
bool section_valid(const SectionExtra &extra);

// Replace entry `at`'s section fields. A CAP drops the entry's mute recall. False, with the
// project as it was, for an index outside the chain or fields `section_valid` refuses.
bool chain_set_section(Project &project, int at, const SectionExtra &extra);

// The fields of the entry the chain is playing, or null when it plays none (inactive, or the
// playing entry was removed). Read when an entry starts, as `chain_entry_mutes` is.
const SectionExtra *chain_entry_section(const Project &project, const ChainPlayer &player);

// The section fields a knob edits, in the order the detail view lays them out after PAT and REP.
enum class SectionField : uint8_t { mut = 0, scn, mrp, nrg, bpm, trn, count };

// One knob turn of `field` on entry `at`, `delta` clicks:
//   MUT walks --, M1-M8, CAP; arriving on CAP stores `live_mutes` as its mask.
//   SCN walks --, 1-16 and MRP --, SNAP, RIDE, BACK, SNAP A, stopping at the ends.
//   NRG and BPM leave `--` for a value on any turn (ENERGY 0; `tempo_centi`, the tempo playing),
//   then walk -64..+64 and 20-300 BPM a whole BPM a click. TRN likewise leaves `--` for 0, then
//   walks -12..+12 a step a click. Back to `--` is `section_clear`.
// False for an index outside the chain or a field past the six.
bool section_step(Project &project, int at, SectionField field, int delta, uint8_t live_mutes,
                  uint16_t tempo_centi);

// `field` of entry `at` back to `--` (push the knob). MUT `--` clears a recall and a CAP alike.
bool section_clear(Project &project, int at, SectionField field);

// ENERGY from NRG's byte (1-129), and back. Byte 0 is `--` and has no ENERGY.
constexpr int section_energy(uint8_t byte) { return int(byte) - int(kSectionEnergyZero); }
constexpr uint8_t section_energy_byte(int energy) {
    return uint8_t((energy < -64 ? -64 : (energy > 64 ? 64 : energy)) + int(kSectionEnergyZero));
}

// TRANSPOSE from TRN's byte (1-25), and back. Byte 0 is `--` and has no TRANSPOSE.
constexpr int section_transpose(uint8_t byte) { return int(byte) - int(kSectionTransposeZero); }
constexpr uint8_t section_transpose_byte(int steps) {
    return uint8_t((steps < -12 ? -12 : (steps > 12 ? 12 : steps)) + int(kSectionTransposeZero));
}

// Keep a running player on the entry it is actually playing across an edit. `chain_insert` and
// `chain_remove` renumber the entries, and `ChainPlayer::entry` is an index: without these, an
// entry added above the playing one made the chain carry on from the wrong entry, and removing
// the playing entry made it skip the one after it. Call each after the matching edit succeeded.
void chain_player_inserted(ChainPlayer &player, int at);
void chain_player_removed(ChainPlayer &player, int at);

// The pattern the chain is currently on, for the screen. False when the chain is not active.
bool chain_current(const Project &project, const ChainPlayer &player, uint8_t &bank,
                   uint8_t &slot);

}  // namespace strata
