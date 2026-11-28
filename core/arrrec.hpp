// ARR REC: a jam written into the chain as song sections, one pass of the pattern at a time.
//
// The arrangement concept (Roland, 2026-11-28; docs/CONTROLS.md "ARR REC"). While ARR REC is on,
// every pass of the playing pattern is compared with the chain's last entry: the same pattern
// with the same mutes, scene, morph side, ENERGY and tempo adds a repeat to that entry, anything
// else starts a new entry holding what plays now (MUT CAP, SCN, MRP SNAP or SNAP A, NRG, BPM).
// An entry is capped at X16, so a seventeenth pass of the same thing starts a new entry of it.
// The chain holds 64 entries; a take that needs a 65th ends there (ARR FULL).
//
// ## What a pass plays
//
// A pass is recorded as what played at its START: a mute or an ENERGY move made inside a pass is
// recorded at the next boundary, so the song is quantized to whole passes. The engine keeps
// taking the state for the first kQueueGraceTicks of a pass, the same forgiveness a queued
// pattern change gets, so a mute pressed a hair after the downbeat belongs to the pass it was
// meant for rather than the next one.
//
// ## Passes that are cut short
//
// A pass can end before its boundary: a pattern change at once (NOW) or on a 16 / 32 step grid,
// a LOCK jump, STOP, ARR REC turned off, or a take started in the middle of a pass. The chain
// has no half passes, so the rule is one rule for all of them: a pass is written when at least
// half of it was recorded, and dropped otherwise. A whole pass is always written.
//
// ## Track Launch
//
// The chain holds no launch states (Roland, 2026-11-27), so a pass with a borrowed lane is
// written as a pattern of its own: what every lane played is CAPTUREd into the first free slot of
// the playing pattern's bank (core/launch.hpp's rules: no steps, not playing and not a lane's
// source), WITHOUT switching to it, and the new entry names that slot. The arrangement concept,
// choice 4. Passes that play the same mix again in a row reuse the slot, as repeats or, past
// X16, as a new entry on it; the same mix after something else is captured again, because a part
// may have been edited in between and the chain must replay what was heard. With no free slot,
// or with more locks than one pattern holds, the pass is written as the playing pattern alone
// and the take says so (`ArrLaunch::lost`, `LAUNCH NOT KEPT`), once for that run of passes.
//
// Every lane replays from the captured pattern's start. A lane launched on the pattern's own
// boundary (the default) is in step with it; one landed NOW or on a 16 / 32 step grid replays
// shifted to the pattern's start.
//
// ## Undo
//
// Starting a take keeps the chain it replaces in an `ArrBackup` (one level). ARR UNDO swaps the
// two, so a second ARR UNDO brings the take back. A take that wrote nothing puts the old chain
// back at once: turning ARR REC on and off by mistake costs nothing. The backup is dropped by the
// next take and by a project load; the main context never touches it (it lives in the engine).
//
// Everything here is plain arithmetic on a `Project`, driven by ticks the caller passes in, so
// the host tests run whole synthetic jams through it.
#pragma once

#include "launch.hpp"
#include "project.hpp"

#include <cstdint>

namespace strata {

// What plays now, as a chain entry would hold it.
struct ArrState {
    uint8_t bank = 0;
    uint8_t slot = 0;
    SectionExtra fields;     // always MUT CAP; SCN, MRP, NRG set; BPM `--` under an outside clock
    uint8_t lanes[kTracks] = {};   // each borrowed lane's source, kLaneOwn for its own part
};

// The live state as an ArrState. `scene_b` is 0-15, `morph` 0..kMorphScale (snapped to its
// nearer side, as the SECTION view's quick fill does), `energy` -64..+64, `tempo_centi` the
// project's tempo. `external`: the clock is someone else's, so BPM is `--` (a tempo that follows
// an outside clock is not the song's, and would split a section on every drift).
ArrState arr_state(uint8_t bank, uint8_t slot, uint8_t mutes, uint8_t scene_b, uint16_t morph,
                   int energy, uint16_t tempo_centi, bool external);

// Fill `state.lanes` from the project's launch state: a borrowed lane's source, kLaneOwn for every
// lane on its own part (so two states playing the same mix always compare equal).
void arr_state_lanes(ArrState &state, const Project &project);

// The chain as it was before a take.
struct ArrBackup {
    ChainEntry chain[kChainEntries];
    SectionExtra extra[kChainEntries];
    uint8_t length = 0;
    bool valid = false;
};

// What the last pass written did with Track Launch.
enum class ArrLaunch : uint8_t {
    none = 0,    // nothing to say: no borrowed lane, or the slot of the passes before it reused
    kept,        // captured into `launch_bank` / `launch_slot`
    lost,        // no free slot or too many locks: written as the playing pattern alone
};

struct ArrRec {
    bool on = false;            // armed (waiting for the transport) or recording
    bool pass_open = false;     // a pass is being recorded
    bool extending = false;     // the chain's last entry is this take's and may take more passes
    uint8_t written = 0;        // entries this take has added
    uint32_t pass_from = 0;     // the tick its recording began: the boundary, or where it was cut in
    uint32_t pass_loop = 1;     // its pattern's master loop in ticks
    ArrState pass;              // what it plays
    // The last pass written, as played (`last`, of which bank, slot and lanes are read) and as
    // the pattern it went into the chain as (`target_*`: the captured slot, or the live one).
    // A pass that plays the same goes into the same pattern without another capture.
    bool last_known = false;
    ArrState last;
    uint8_t target_bank = 0;
    uint8_t target_slot = 0;
    // What the last pass written did with Track Launch, for the engine to say. Kept by arr_finish.
    ArrLaunch launch = ArrLaunch::none;
    uint8_t launch_bank = 0;
    uint8_t launch_slot = 0;
};

// What ending a pass did with it.
enum class ArrPass : uint8_t {
    none = 0,    // no pass was open
    dropped,     // less than half of it was recorded
    extended,    // a repeat on the chain's last entry
    added,       // a new entry
    full,        // it needed a 65th entry: the take is over
};

// Start a take: the chain goes into `backup` and the chain is emptied. False, changing nothing,
// when a take is already on. The first pass begins with `arr_pass_begin`.
bool arr_start(Project &project, ArrRec &rec, ArrBackup &backup);

// A pass begins at `tick`, `loop` ticks long (0 is read as 1), playing `state`.
void arr_pass_begin(ArrRec &rec, const ArrState &state, uint32_t tick, uint32_t loop);

// The pass in progress ends at `tick` and is written (or not) as the rules above say. On `full`
// the take is over: the caller ends it with `arr_finish`. A pass with a borrowed lane may write a
// pattern (Track Launch, above); `rec.launch` says what happened, and is reset at every call.
ArrPass arr_pass_end(Project &project, ArrRec &rec, uint32_t tick);

// The chain was edited by hand during a take: the next pass starts a new entry rather than
// adding a repeat to one that may no longer be this take's.
void arr_chain_edited(ArrRec &rec);

// End a take at `tick`: the pass in progress is ended (a cut pass, by the half rule), and ARR
// REC is off. Returns how many entries the take wrote. A take that wrote none puts the chain
// from `backup` back and drops the backup. Does nothing (and returns 0) when no take is on.
int arr_finish(Project &project, ArrRec &rec, ArrBackup &backup, uint32_t tick);

// ARR UNDO: swap the chain and the backup. False when there is no backup or a take is on.
bool arr_undo(Project &project, const ArrRec &rec, ArrBackup &backup);

}  // namespace strata
