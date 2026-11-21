// The undo history described in core/undo.hpp: whole-pattern snapshots in a ring.
//
// There is deliberately very little here beyond index arithmetic. A snapshot cannot be wrong
// about what it restores, so the only bugs this file can have are off-by-ones in the ring --
// which is why the ring is the thing the tests hammer.
//
// The ring is kUndoLevels + 1 slots wide because `mark`, `undo` and `redo` all store the live
// state on the way past: the slot the cursor sits on is scratch for the state the engine is
// holding, not a level of history. Sixteen levels therefore need seventeen slots, and the
// seventeenth is exactly the one freed when the oldest state is dropped.
#include "undo.hpp"

namespace strata {

// --- gestures -------------------------------------------------------------

const char *gesture_name(GestureKind kind) {
    // Upper case only: docs/SPEC.md's built-in font is A-Z, digits and eleven symbols, so a
    // lower-case letter would draw as nothing. The caller prefixes "UNDO ", and the screen is
    // 21 characters wide, so these stay short.
    switch (kind) {
        case GestureKind::trig:   return "TRIG";
        case GestureKind::lock:   return "LOCK";
        case GestureKind::field:  return "STEP";
        case GestureKind::erase:  return "CLEAR";
        case GestureKind::paste:  return "PASTE";
        case GestureKind::record: return "RECORD";
        case GestureKind::motion: return "MOTION";
        case GestureKind::length: return "LENGTH";
        case GestureKind::none:
        case GestureKind::count:
            return "";
        default:
            // Gesture::kind is a byte on the wire between the UI and here, so an out-of-range
            // value is possible; an empty name draws "UNDO" on its own rather than garbage.
            return "";
    }
}

bool gesture_same(const Gesture &a, const Gesture &b) {
    // "No gesture" is not a gesture that can match. If two `none` marks coalesced, a caller
    // that forgot to set a kind would quietly collapse every edit it ever made into one undo
    // level -- the exact failure the gesture mechanism exists to prevent. Checking `a` alone is
    // enough: if only `b` is `none` the kind comparison below already fails.
    if (a.kind == uint8_t(GestureKind::none)) return false;
    return a.kind == b.kind && a.track == b.track && a.step == b.step && a.seq == b.seq;
}

// --- the stack ------------------------------------------------------------

int UndoStack::slot(int index) const {
    // `base_` is below kSlots and `index` is at most kUndoLevels, so the sum is below 2 * kSlots
    // and a single conditional subtraction is exact. See the header on why this is not a `%`.
    const int raw = base_ + index;
    return raw >= kSlots ? raw - kSlots : raw;
}

void UndoStack::clear() {
    base_ = 0;
    cursor_ = 0;
    newest_ = 0;
    last_ = Gesture{};
    has_last_ = false;
    for (int i = 0; i < kSlots; i++) kinds_[i] = uint8_t(GestureKind::none);

    // `states_` is left as it is. With the cursor and newest at 0 no index reaches any of it,
    // and zeroing seventeen patterns is ~140 KiB of memset in audio context for no observable
    // difference. What makes the old history unrestorable is depth() == redo_depth() == 0.
}

bool UndoStack::mark(const Pattern &live, const Gesture &gesture) {
    if (has_last_ && gesture_same(last_, gesture)) return false;

    states_[slot(cursor_)] = live;
    cursor_++;

    if (cursor_ > kUndoLevels) {
        // Out of levels: forget state 0. Every surviving state keeps its contents and loses one
        // from its index, which is precisely what moving the ring offset on by one does -- no
        // pattern is copied, and the slot that state 0 vacated is the one the cursor now sits
        // on. `slot(1)` is still read against the old offset, which is the point.
        base_ = slot(1);
        cursor_ = kUndoLevels;
    }

    // Everything ahead of the cursor is abandoned here: the patterns are still in the ring but
    // no index reaches them, so redo_depth() drops to 0. Dropping the redo branch on a new edit
    // is what every editor does, and the header commits to it.
    newest_ = cursor_;

    kinds_[slot(cursor_)] = gesture.kind;
    last_ = gesture;
    has_last_ = true;
    return true;
}

bool UndoStack::undo(Pattern &live) {
    if (cursor_ <= 0) return false;

    states_[slot(cursor_)] = live;   // the redo entry: this is where redo comes back to
    cursor_--;
    live = states_[slot(cursor_)];

    // The gesture is over. Without this, an undo during a sweep -- or any mark whose gesture
    // happens to equal the one before the undo -- would coalesce into the edit just taken back
    // and store nothing, so the next undo would skip a state the user can see.
    has_last_ = false;
    return true;
}

bool UndoStack::redo(Pattern &live) {
    if (cursor_ >= newest_) return false;

    states_[slot(cursor_)] = live;
    cursor_++;
    live = states_[slot(cursor_)];

    // Belt and braces, and the header promises it. It cannot actually matter as the code stands:
    // a successful mark leaves redo_depth() at 0, so the only way to reach a redo is through an
    // undo, which has already cleared this. Removing it would make the behaviour depend on that
    // argument staying true, and a redo is not a hot path.
    has_last_ = false;
    return true;
}

int UndoStack::depth() const { return cursor_; }

int UndoStack::redo_depth() const { return newest_ - cursor_; }

GestureKind UndoStack::pending() const {
    if (cursor_ <= 0) return GestureKind::none;
    // kinds_[i] is the gesture of the edit that produced state i, and the live state is state
    // `cursor_` -- so this is the edit the next undo takes back, which is what the screen
    // names. It is not `last_`: after an undo the cursor has moved to an older mark's edit.
    return GestureKind(kinds_[slot(cursor_)]);
}

}  // namespace strata
