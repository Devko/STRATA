// 16 levels of undo for pattern edits, as whole-pattern snapshots.
//
// docs/SPEC.md, "Pattern changes and chains": "16 levels of undo for edits". The edits it means
// are pattern edits -- trigs, locks, step fields, lengths, clears and pastes. Kit parameters are
// not in a Pattern and are not covered; turning a knob is not an undoable edit here.
//
// ## Why snapshots and not deltas
//
// A Pattern is about 8 KiB, so 16 levels is ~140 KiB of the app's ~124 MiB. A delta scheme would
// save almost nothing and would have to describe every kind of edit correctly -- including lock
// pool relocation, which moves memory a delta did not mention. A snapshot cannot be wrong about
// what it restores. The copies are memcpy of one struct, which on the C674x is a few thousand
// cycles: a one-off spike on an edit, nowhere near an audio block's 496,000.
//
// ## Where this runs
//
// Audio context, from app/engine.cpp, because the live pattern is the audio side's. Snapshots
// taken from the main context while the sequencer reads the pattern would tear. So every undo
// operation arrives as a command and happens at a block boundary, which is also the only point
// where the pattern is in a consistent state.
//
// ## The timeline
//
// Think of the edit history as states s_0, s_1, ... s_n. The CURRENT state is the live pattern
// in the engine, not a copy in here; `states_` holds every other one. `cursor_` is the index of
// the live state, `newest_` the highest index that exists.
//
//   mark(live)  stores live at cursor_, advances cursor_, and sets newest_ = cursor_
//               (so the redo branch is dropped, which is what every editor does)
//   undo(live)  stores live at cursor_, steps cursor_ back, and loads live from it
//   redo(live)  stores live at cursor_, steps cursor_ forward, and loads live from it
//
// Storing the live state on the way past is what makes redo work without a second ring.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// docs/SPEC.md's figure.
constexpr int kUndoLevels = 16;

// --- gestures -------------------------------------------------------------

// What kind of edit a mark belongs to. Only used to decide whether two marks are the same
// gesture, and to name the pending undo on screen, so these are free to renumber.
enum class GestureKind : uint8_t {
    none = 0,
    trig,        // tapping a step
    lock,        // holding a step and turning an encoder
    field,       // the step page: note, velocity, condition and the rest
    erase,       // CLR on a step, a range or a track
    paste,
    record,      // one live-recording pass
    motion,      // one motion-recording pass
    length,      // track length, speed or swing
    count
};

const char *gesture_name(GestureKind kind);

// Which gesture a mark belongs to.
//
// `seq` is the UI's gesture counter. It is the whole mechanism for deciding where one undoable
// edit ends and the next begins: the UI bumps it when a step key is released, when recording
// stops, when the selection moves -- whenever the thing the user is doing has finished. Without
// it, a single encoder sweep would fill all 16 levels with one step's worth of history and throw
// away everything that came before.
struct Gesture {
    uint8_t kind = uint8_t(GestureKind::none);
    uint8_t track = 0;
    uint8_t step = 0;
    uint8_t seq = 0;
};

// Field-for-field equality. A `none` gesture never matches, not even another `none`: it means
// "no gesture", so two of them are two separate edits.
bool gesture_same(const Gesture &a, const Gesture &b);

// --- the stack ------------------------------------------------------------

class UndoStack {
public:
    // Forget everything. Call when a pattern is loaded or a project opened: history from
    // another pattern must not be restorable over this one.
    void clear();

    // Remember `live` as a state to come back to, before the edit that follows is applied.
    //
    // Returns false, having stored nothing, when `gesture` is the same gesture as the last mark
    // (see Gesture). That is the normal case during a sweep and is not an error.
    //
    // An undo or a redo ends the current gesture, so the mark after one is never coalesced.
    bool mark(const Pattern &live, const Gesture &gesture);

    // Step back one state. `live` is both read (to make the redo entry) and written.
    // False, with `live` untouched, when there is nothing to undo.
    bool undo(Pattern &live);

    // Step forward again. False, with `live` untouched, when there is nothing to redo.
    bool redo(Pattern &live);

    int depth() const;        // how many undos are available, 0..kUndoLevels
    int redo_depth() const;   // how many redos are available

    // The gesture of the edit the next undo would take back, for the screen ("UNDO LOCK").
    // `none` when there is nothing to undo.
    GestureKind pending() const;

private:
    // One slot per state, and one more because the live state is stored on the way past.
    static constexpr int kSlots = kUndoLevels + 1;

    // Ring index. Deliberately NOT `%`: a 32-bit modulo on the C674x is a call to a division
    // helper, and this header's whole point is that an undo is cheap. Every index here moves by
    // one at a time, so a conditional subtraction is exact.
    int slot(int index) const;

    Pattern states_[kSlots];

    // The kind of the edit that produced each state, indexed by slot exactly like `states_`,
    // so dropping the oldest state shifts these with it for free. It has to be per level and
    // not just `last_`: after an undo, `pending()` names the edit the NEXT undo would take
    // back, which belongs to an older mark.
    uint8_t kinds_[kSlots] = {};

    int base_ = 0;         // ring offset of state 0
    int cursor_ = 0;       // index of the live state, 0..newest_
    int newest_ = 0;       // highest index that exists
    Gesture last_{};       // the gesture of the last mark, for coalescing
    bool has_last_ = false;
};

// A Pattern is about 8 KiB and a stack frame is capped at 16 KiB (CLAUDE.md), so an UndoStack is
// a member of something long-lived -- never a local, and never passed by value.
static_assert(sizeof(Pattern) > 4096, "if a Pattern got small, revisit the note above");

}  // namespace strata
