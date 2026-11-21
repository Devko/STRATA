// Tests for the pattern undo history in core/undo.
//
// Nothing here can go wrong in the snapshot itself -- it is a struct copy -- so every test is
// really about the ring: which state a cursor movement lands on, which state the seventeenth
// mark throws away, and which marks coalesce into one level. Those are off-by-one bugs, and on
// hardware they look like "undo sometimes skips an edit", which is unfindable by hand.
//
// So the states are made identifiable rather than merely different: `fill_pattern(p, id)` builds
// a whole pattern from `id` deterministically, and every restore is checked against a freshly
// built copy of the id the timeline says should be live. A test that only checked depth() would
// pass with the ring wired to the wrong slots.
//
// The patterns and the stack are at file scope on purpose. A Pattern is ~8 KiB and an UndoStack
// ~140 KiB against CLAUDE.md's 16 KiB frame cap, so neither may ever be a local -- on the host
// a local would merely work, and on the unit it would overflow.
#include "check.hpp"

#include "pattern.hpp"
#include "undo.hpp"

using namespace strata;

namespace {

// --- scratch --------------------------------------------------------------

UndoStack g_stack;
Pattern g_live;
Pattern g_expected;
Pattern g_other;

// --- building distinguishable patterns ------------------------------------

// xorshift32: a handful of instructions, no libm, and the same sequence on every host, which is
// what makes `fill_pattern` reproducible.
uint32_t next_random(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// A whole pattern derived from `id`: every step field, every track header, the pattern fields
// and a scattering of locks. The locks go through the real allocator so that the pool ends up
// in a layout a live pattern could actually have -- including runs abandoned by relocation,
// which a snapshot has to carry too.
void fill_pattern(Pattern &pattern, uint32_t id) {
    pattern_init(pattern);

    uint32_t rng = (id * 2654435761u) | 1u;   // never 0: xorshift sticks there forever

    for (int t = 0; t < kTracks; t++) {
        TrackPattern &track = pattern.tracks[t];
        const uint32_t h = next_random(rng);
        track.length = uint8_t(1 + (h & 63u));
        track.speed = uint8_t(h % uint32_t(Speed::count));
        track.swing = uint8_t(kSwingMin + ((h >> 8) % 26u));

        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &s = track.steps[i];
            const uint32_t r = next_random(rng);
            const uint32_t q = next_random(rng);
            s.flags = uint8_t(r & kStepFlagMask);
            s.note = uint8_t((r >> 4) & 0x7Fu);
            s.velocity = uint8_t(1 + ((r >> 11) & 0x7Eu));
            s.length = uint8_t(1 + ((r >> 18) % uint32_t(kLengthMax)));
            s.micro = int8_t(int((r >> 24) % uint32_t(2 * kMicroRange + 1)) - kMicroRange);
            s.probability = uint8_t(q % 101u);
            s.condition =
                condition_pack_a_b(uint8_t(1 + ((q >> 8) & 7u)), uint8_t(2 + ((q >> 12) & 6u)));
            s.ratchet = ratchet_pack(uint8_t(1 + ((q >> 16) & 7u)), RatchetRamp((q >> 20) % 3u));
            s.chord = uint8_t((q >> 24) & 0x3Fu);
        }
    }

    for (int n = 0; n < 48; n++) {
        const uint32_t r = next_random(rng);
        pattern_set_lock(pattern, int(r & 7u), int((r >> 3) & 63u), uint8_t((r >> 9) & 15u),
                         uint8_t((r >> 13) & 0x7Fu));
    }

    pattern.master_length = uint8_t(1 + (next_random(rng) & 63u));
    pattern.swing = uint8_t(kSwingMin + (next_random(rng) % 26u));
    pattern.root = uint8_t(next_random(rng) % 12u);
    pattern.scale = uint8_t(next_random(rng) & 7u);
    pattern.seed = next_random(rng) | 1u;
}

// --- comparing patterns ---------------------------------------------------

int g_reported = 0;

// "The patterns differ" on its own, from the middle of a sixteen-level ladder, is nearly
// useless; the first few locations are worth printing. Capped so a wholly broken ring does not
// bury the failure list.
bool differ(const char *what, int a, int b) {
    if (g_reported < 8) {
        g_reported++;
        std::printf("    patterns differ: %s (%d, %d)\n", what, a, b);
    }
    return false;
}

bool same_text(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// Field for field, including the whole lock pool. Deliberately not memcmp: Pattern carries
// padding before `seed`, and whether a struct copy carries padding bytes with it is not
// something the language promises, so a memcmp here could fail on a perfectly good snapshot.
//
// The pool is compared in full, above `locks_used` as well. Those slots hold the garbage that
// relocation abandoned, and restoring them is what "restores the pattern exactly" means; if a
// later snapshot ever copies only the used part, that should be a decision someone makes on
// purpose rather than something this test lets through.
bool patterns_equal(const Pattern &a, const Pattern &b) {
    for (int t = 0; t < kTracks; t++) {
        const TrackPattern &x = a.tracks[t];
        const TrackPattern &y = b.tracks[t];
        if (x.length != y.length) return differ("track length", t, -1);
        if (x.speed != y.speed) return differ("track speed", t, -1);
        if (x.swing != y.swing) return differ("track swing", t, -1);

        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &p = x.steps[i];
            const Step &q = y.steps[i];
            if (p.flags != q.flags) return differ("flags", t, i);
            if (p.note != q.note) return differ("note", t, i);
            if (p.velocity != q.velocity) return differ("velocity", t, i);
            if (p.length != q.length) return differ("length", t, i);
            if (p.micro != q.micro) return differ("micro", t, i);
            if (p.probability != q.probability) return differ("probability", t, i);
            if (p.condition != q.condition) return differ("condition", t, i);
            if (p.ratchet != q.ratchet) return differ("ratchet", t, i);
            if (p.lock_first != q.lock_first) return differ("lock_first", t, i);
            if (p.lock_count != q.lock_count) return differ("lock_count", t, i);
            if (p.chord != q.chord) return differ("chord", t, i);
        }
    }

    for (int i = 0; i < kLockPool; i++) {
        if (a.locks[i].param != b.locks[i].param) return differ("pool param", -1, i);
        if (a.locks[i].value != b.locks[i].value) return differ("pool value", -1, i);
    }

    if (a.locks_used != b.locks_used) return differ("locks_used", -1, -1);
    if (a.master_length != b.master_length) return differ("master_length", -1, -1);
    if (a.swing != b.swing) return differ("pattern swing", -1, -1);
    if (a.root != b.root) return differ("root", -1, -1);
    if (a.scale != b.scale) return differ("scale", -1, -1);
    if (a.seed != b.seed) return differ("seed", -1, -1);
    return true;
}

// Every step's locks as the sequencer would read them. `locks_used` matching proves nothing on
// its own: a run restored to the wrong slots still has a plausible count and a readable view.
bool lock_views_equal(const Pattern &a, const Pattern &b) {
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const LockView x = pattern_step_locks(a, t, i);
            const LockView y = pattern_step_locks(b, t, i);
            if (x.count != y.count) return differ("view count", t, i);
            for (uint8_t k = 0; k < x.count; k++) {
                if (x.locks[k].param != y.locks[k].param) return differ("view param", t, i);
                if (x.locks[k].value != y.locks[k].value) return differ("view value", t, i);
            }
        }
    }
    return true;
}

// The live pattern is exactly the state `id` builds.
bool live_is(uint32_t id) {
    fill_pattern(g_expected, id);
    return patterns_equal(g_live, g_expected);
}

// A gesture whose fields all follow `seq`, so distinct seqs never coalesce and a repeated seq
// always does.
Gesture edit(GestureKind kind, int seq) {
    Gesture gesture;
    gesture.kind = uint8_t(kind);
    gesture.track = uint8_t(seq & 7);
    gesture.step = uint8_t(seq & 63);
    gesture.seq = uint8_t(seq);
    return gesture;
}

// `count` marks with distinct gestures, each followed by the "edit" that makes state k.
void mark_ladder(int count) {
    fill_pattern(g_live, 0);
    for (int k = 1; k <= count; k++) {
        CHECK(g_stack.mark(g_live, edit(GestureKind::trig, k)));
        fill_pattern(g_live, uint32_t(k));
    }
}

}  // namespace

// --- gestures -------------------------------------------------------------

TEST(gesture_names_are_drawable_and_distinct) {
    // The screen font is upper case, digits and eleven symbols only (docs/SPEC.md), so a name
    // with anything else in it would draw as holes. Every real kind needs its own name, or two
    // different pending undos would read identically.
    const char *names[int(GestureKind::count)] = {};
    for (int k = 1; k < int(GestureKind::count); k++) {
        const char *name = gesture_name(GestureKind(k));
        names[k] = name;
        CHECK(name[0] != '\0');
        for (const char *c = name; *c != '\0'; c++) {
            const bool drawable = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                                  *c == ' ' || *c == '/' || *c == '-';
            CHECK(drawable);
        }
        for (int j = 1; j < k; j++) CHECK(!same_text(names[j], name));
    }

    // `none` has no name: pending() returns it when there is nothing to undo, and the screen
    // then draws "UNDO" on its own. An out-of-range byte from the UI gets the same treatment
    // rather than an index into nothing.
    CHECK(same_text(gesture_name(GestureKind::none), ""));
    CHECK(same_text(gesture_name(GestureKind(200)), ""));
}

TEST(gesture_same_needs_every_field) {
    const Gesture base = edit(GestureKind::lock, 5);
    CHECK(gesture_same(base, base));

    // Each field on its own ends a gesture. Track and step matter because holding a different
    // step is a different edit even with the same encoder; seq matters because that is the UI
    // telling us the user let go.
    Gesture other = base;
    other.kind = uint8_t(GestureKind::field);
    CHECK(!gesture_same(base, other));
    other = base;
    other.track = uint8_t(base.track + 1);
    CHECK(!gesture_same(base, other));
    other = base;
    other.step = uint8_t(base.step + 1);
    CHECK(!gesture_same(base, other));
    other = base;
    other.seq = uint8_t(base.seq + 1);
    CHECK(!gesture_same(base, other));
}

TEST(a_none_gesture_never_matches) {
    // Two "no gesture" marks are two separate edits. If they matched, a caller that forgot to
    // set a kind would collapse its whole history into one level and lose everything older.
    const Gesture nothing{};
    CHECK(!gesture_same(nothing, nothing));

    Gesture dressed = nothing;
    dressed.track = 3;
    dressed.step = 9;
    dressed.seq = 4;
    Gesture twin = dressed;
    CHECK(!gesture_same(dressed, twin));

    twin.kind = uint8_t(GestureKind::trig);
    CHECK(!gesture_same(dressed, twin));
    CHECK(!gesture_same(twin, dressed));
}

// --- an empty stack -------------------------------------------------------

TEST(a_fresh_stack_has_no_history) {
    g_stack.clear();
    fill_pattern(g_live, 7);

    CHECK_EQ(g_stack.depth(), 0);
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::none));

    // A refusal must leave the live pattern alone -- the engine hands us the real thing.
    CHECK(!g_stack.undo(g_live));
    CHECK(live_is(7));
    CHECK(!g_stack.redo(g_live));
    CHECK(live_is(7));
}

TEST(clear_forgets_both_directions) {
    g_stack.clear();
    mark_ladder(5);
    CHECK(g_stack.undo(g_live));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(g_stack.depth(), 3);
    CHECK_EQ(g_stack.redo_depth(), 2);

    g_stack.clear();
    CHECK_EQ(g_stack.depth(), 0);
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::none));
    CHECK(!g_stack.undo(g_live));
    CHECK(!g_stack.redo(g_live));
    CHECK(live_is(3));   // whatever was live stays live; only the history goes

    // The gesture goes too, so a mark that repeats the one from before the clear is stored.
    // Otherwise loading a pattern mid-sweep would swallow the first edit made to it.
    const Gesture gesture = edit(GestureKind::lock, 9);
    CHECK(g_stack.mark(g_live, gesture));
    g_stack.clear();
    CHECK(g_stack.mark(g_live, gesture));
    CHECK_EQ(g_stack.depth(), 1);
}

// --- the ladder -----------------------------------------------------------

TEST(sixteen_marks_then_sixteen_undos_restore_the_original) {
    g_stack.clear();
    mark_ladder(kUndoLevels);
    CHECK_EQ(g_stack.depth(), kUndoLevels);
    CHECK_EQ(g_stack.redo_depth(), 0);

    for (int k = kUndoLevels - 1; k >= 0; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
        CHECK_EQ(g_stack.depth(), k);
        CHECK_EQ(g_stack.redo_depth(), kUndoLevels - k);
    }

    // Exactly sixteen levels: the seventeenth undo is refused and the pattern is untouched.
    CHECK(!g_stack.undo(g_live));
    CHECK(live_is(0));
    CHECK_EQ(g_stack.depth(), 0);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::none));
}

TEST(the_seventeenth_mark_drops_the_oldest_state) {
    g_stack.clear();
    mark_ladder(kUndoLevels + 1);

    // Seventeen marks, still sixteen levels.
    CHECK_EQ(g_stack.depth(), kUndoLevels);
    CHECK_EQ(g_stack.redo_depth(), 0);

    // And the sixteen that survived are the newest ones, in order: state 0 is what went.
    for (int k = kUndoLevels; k >= 1; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
    CHECK_EQ(g_stack.depth(), 0);
    CHECK(!g_stack.undo(g_live));
    CHECK(live_is(1));
}

TEST(many_marks_keep_only_the_newest_sixteen) {
    // Forty marks walks the ring offset right round it (24 drops against 17 slots), so this
    // catches a wrap that is only correct the first time.
    g_stack.clear();
    mark_ladder(40);
    CHECK_EQ(g_stack.depth(), kUndoLevels);

    for (int k = 39; k >= 40 - kUndoLevels; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
    CHECK_EQ(g_stack.depth(), 0);
    CHECK(!g_stack.undo(g_live));
}

// --- redo -----------------------------------------------------------------

TEST(undo_and_redo_are_each_others_inverse) {
    g_stack.clear();
    mark_ladder(6);

    // Down the whole timeline and back up, twice. A redo has to land on the state the undo
    // left, which only works because each cursor move stores the live state on the way past.
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 5; k >= 0; k--) {
            CHECK(g_stack.undo(g_live));
            CHECK(live_is(uint32_t(k)));
        }
        CHECK_EQ(g_stack.redo_depth(), 6);
        for (int k = 1; k <= 6; k++) {
            CHECK(g_stack.redo(g_live));
            CHECK(live_is(uint32_t(k)));
        }
        CHECK_EQ(g_stack.redo_depth(), 0);
        CHECK(!g_stack.redo(g_live));
        CHECK(live_is(6));
    }

    // Mixed with a further mark: the new edit becomes the thing redo comes back to.
    CHECK(g_stack.undo(g_live));
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(4));
    CHECK(g_stack.mark(g_live, edit(GestureKind::paste, 99)));
    fill_pattern(g_live, 77);
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(4));
    CHECK(g_stack.redo(g_live));
    CHECK(live_is(77));
}

TEST(redo_survives_the_ring_wrapping) {
    // Twenty marks, so four states have been dropped and the ring offset has moved. Undoing to
    // the bottom and redoing to the top must still visit every surviving state in order.
    g_stack.clear();
    mark_ladder(20);

    for (int k = 19; k >= 20 - kUndoLevels; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
    CHECK_EQ(g_stack.depth(), 0);
    CHECK_EQ(g_stack.redo_depth(), kUndoLevels);

    for (int k = 20 - kUndoLevels + 1; k <= 20; k++) {
        CHECK(g_stack.redo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK(!g_stack.redo(g_live));
}

TEST(a_mark_after_an_undo_drops_the_redo_branch) {
    g_stack.clear();
    mark_ladder(4);
    CHECK(g_stack.undo(g_live));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(g_stack.redo_depth(), 2);

    CHECK(g_stack.mark(g_live, edit(GestureKind::erase, 50)));
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK_EQ(g_stack.depth(), 3);

    // A refused redo must not touch the pattern either.
    CHECK(!g_stack.redo(g_live));
    CHECK(live_is(2));
}

// --- coalescing -----------------------------------------------------------

TEST(one_gesture_consumes_one_level) {
    g_stack.clear();
    fill_pattern(g_live, 1);
    const Gesture sweep = edit(GestureKind::lock, 7);

    CHECK(g_stack.mark(g_live, sweep));
    for (int i = 0; i < 49; i++) {
        fill_pattern(g_live, uint32_t(100 + i));   // the encoder still turning
        CHECK(!g_stack.mark(g_live, sweep));
    }
    CHECK_EQ(g_stack.depth(), 1);
    CHECK_EQ(g_stack.redo_depth(), 0);

    // The level holds the state from before the sweep began, not one from the middle of it --
    // an undo takes back the whole sweep, which is the point of coalescing.
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(1));
}

TEST(bumping_seq_starts_a_new_level) {
    g_stack.clear();
    fill_pattern(g_live, 0);

    // Three sweeps of ten marks each on the same step with the same encoder: only the seq tells
    // them apart, and without it the user would get one undo for all three.
    for (int seq = 1; seq <= 3; seq++) {
        Gesture gesture = edit(GestureKind::lock, 0);
        gesture.seq = uint8_t(seq);
        for (int i = 0; i < 10; i++) {
            CHECK_EQ(g_stack.mark(g_live, gesture), i == 0);
            if (i == 0) fill_pattern(g_live, uint32_t(seq));
        }
        CHECK_EQ(g_stack.depth(), seq);
    }

    for (int k = 2; k >= 0; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
}

TEST(a_none_gesture_never_coalesces) {
    g_stack.clear();
    fill_pattern(g_live, 0);
    const Gesture nothing{};

    for (int k = 1; k <= 5; k++) {
        CHECK(g_stack.mark(g_live, nothing));
        fill_pattern(g_live, uint32_t(k));
    }
    CHECK_EQ(g_stack.depth(), 5);

    // Five levels, each its own state.
    for (int k = 4; k >= 0; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK(live_is(uint32_t(k)));
    }
}

TEST(a_refused_mark_changes_nothing) {
    g_stack.clear();
    fill_pattern(g_live, 0);
    CHECK(g_stack.mark(g_live, edit(GestureKind::field, 5)));
    fill_pattern(g_live, 1);
    CHECK(g_stack.mark(g_live, edit(GestureKind::field, 6)));
    fill_pattern(g_live, 2);
    CHECK(g_stack.undo(g_live));
    CHECK(g_stack.mark(g_live, edit(GestureKind::field, 7)));
    fill_pattern(g_live, 3);

    const int depth = g_stack.depth();
    const int redo = g_stack.redo_depth();
    const GestureKind pending = g_stack.pending();

    for (int i = 0; i < 5; i++) {
        fill_pattern(g_live, uint32_t(50 + i));
        CHECK(!g_stack.mark(g_live, edit(GestureKind::field, 7)));
        CHECK_EQ(g_stack.depth(), depth);
        CHECK_EQ(g_stack.redo_depth(), redo);
        CHECK_EQ(int(g_stack.pending()), int(pending));
    }

    // Still the state from before the run started: a refused mark must not have overwritten it.
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(1));
}

TEST(an_undo_ends_the_gesture) {
    g_stack.clear();
    fill_pattern(g_live, 0);
    const Gesture sweep = edit(GestureKind::lock, 3);

    CHECK(g_stack.mark(g_live, sweep));
    fill_pattern(g_live, 1);
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(0));

    // The same gesture again must be stored. If it coalesced into the edit just taken back,
    // the next undo would jump straight past the edit the user is about to make.
    CHECK(g_stack.mark(g_live, sweep));
    fill_pattern(g_live, 2);
    CHECK_EQ(g_stack.depth(), 1);
    CHECK_EQ(g_stack.redo_depth(), 0);
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(0));
}

TEST(a_redo_ends_the_gesture) {
    // The header's contract, pinned from the outside: an edit made after a redo is its own undo
    // level even when it repeats the gesture of the mark before the undo. As the code stands it
    // holds because the undo on the way down already ended the gesture -- a redo is only
    // reachable through an undo -- so this cannot distinguish `redo` clearing the gesture from
    // `undo` doing it. It is here for the contract, which app/ reads, not for the mechanism.
    g_stack.clear();
    fill_pattern(g_live, 0);
    const Gesture sweep = edit(GestureKind::lock, 4);

    CHECK(g_stack.mark(g_live, sweep));
    fill_pattern(g_live, 1);
    CHECK(g_stack.undo(g_live));
    CHECK(g_stack.redo(g_live));
    CHECK(live_is(1));

    CHECK(g_stack.mark(g_live, sweep));
    fill_pattern(g_live, 2);
    CHECK_EQ(g_stack.depth(), 2);
    CHECK(g_stack.undo(g_live));
    CHECK(live_is(1));
}

// --- the pending name -----------------------------------------------------

TEST(pending_names_the_edit_the_next_undo_takes_back) {
    g_stack.clear();
    fill_pattern(g_live, 0);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::none));

    CHECK(g_stack.mark(g_live, edit(GestureKind::trig, 1)));
    fill_pattern(g_live, 1);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::trig));
    CHECK(g_stack.mark(g_live, edit(GestureKind::lock, 2)));
    fill_pattern(g_live, 2);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::lock));
    CHECK(g_stack.mark(g_live, edit(GestureKind::erase, 3)));
    fill_pattern(g_live, 3);
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::erase));

    // Walking back renames it each time: the screen says what the NEXT undo would take back, so
    // this follows the cursor rather than the last mark.
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::lock));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::trig));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::none));

    CHECK(g_stack.redo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::trig));
    CHECK(g_stack.redo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::lock));
}

TEST(pending_follows_the_states_that_survive_a_drop) {
    g_stack.clear();
    fill_pattern(g_live, 0);
    for (int k = 1; k <= 20; k++) {
        const GestureKind kind = (k & 1) != 0 ? GestureKind::trig : GestureKind::lock;
        CHECK(g_stack.mark(g_live, edit(kind, k)));
        fill_pattern(g_live, uint32_t(k));
    }

    // The live state was made by mark 20, and the names have to have shifted along with the
    // states they belong to when the oldest four were dropped.
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::lock));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::trig));
    CHECK(g_stack.undo(g_live));
    CHECK_EQ(int(g_stack.pending()), int(GestureKind::lock));
}

// --- locks ----------------------------------------------------------------

TEST(undo_restores_a_relocated_lock_run) {
    g_stack.clear();
    pattern_init(g_live);

    // Three runs at the bottom of the pool in allocation order: slots 0-1, 2-3 and 4-5.
    CHECK(pattern_set_lock(g_live, 0, 4, 3, 30));
    CHECK(pattern_set_lock(g_live, 0, 4, 4, 40));
    CHECK(pattern_set_lock(g_live, 1, 9, 5, 50));
    CHECK(pattern_set_lock(g_live, 1, 9, 6, 60));
    CHECK(pattern_set_lock(g_live, 2, 20, 7, 70));
    CHECK(pattern_set_lock(g_live, 2, 20, 8, 80));
    CHECK_EQ(int(g_live.tracks[1].steps[9].lock_first), 2);
    CHECK_EQ(int(g_live.locks_used), 6);

    g_other = g_live;   // what the undo has to reproduce

    CHECK(g_stack.mark(g_live, edit(GestureKind::lock, 1)));

    // Growing a run that is not at the top of the pool copies it to the top and abandons its
    // old slots (core/pattern.cpp). The edit therefore moves memory it never mentions, which is
    // the case a delta history would get wrong -- and the reason this one stores snapshots.
    CHECK(pattern_set_lock(g_live, 1, 9, 11, 110));
    CHECK_EQ(int(g_live.tracks[1].steps[9].lock_first), 6);
    CHECK_EQ(int(g_live.tracks[1].steps[9].lock_count), 3);
    CHECK_EQ(int(g_live.locks_used), 9);

    g_expected = g_live;   // for the redo below

    CHECK(g_stack.undo(g_live));

    // The pool is back byte for byte, abandoned slots included ...
    CHECK(patterns_equal(g_live, g_other));
    CHECK_EQ(int(g_live.locks_used), 6);
    CHECK_EQ(int(g_live.tracks[1].steps[9].lock_first), 2);

    // ... and every step reads back the locks it had. locks_used alone would not show a run
    // restored to the wrong slots, which is exactly what a half-right undo would leave.
    CHECK(lock_views_equal(g_live, g_other));
    CHECK_EQ(int(pattern_locks_reachable(g_live)), 6);

    uint8_t value = 0;
    CHECK(!pattern_get_lock(g_live, 1, 9, 11, value));
    CHECK(pattern_get_lock(g_live, 1, 9, 5, value));
    CHECK_EQ(int(value), 50);
    const LockView view = pattern_step_locks(g_live, 1, 9);
    CHECK_EQ(int(view.count), 2);
    CHECK_EQ(int(view.locks[0].param), 5);
    CHECK_EQ(int(view.locks[1].param), 6);
    CHECK_EQ(int(view.locks[1].value), 60);

    // And a redo puts the relocated run back exactly where the edit left it.
    CHECK(g_stack.redo(g_live));
    CHECK(patterns_equal(g_live, g_expected));
    CHECK(lock_views_equal(g_live, g_expected));
    CHECK_EQ(int(g_live.tracks[1].steps[9].lock_first), 6);
    CHECK(pattern_get_lock(g_live, 1, 9, 11, value));
    CHECK_EQ(int(value), 110);
}

TEST(a_ladder_of_lock_edits_unwinds_through_every_relocation) {
    // Growing four runs in rotation means every one of them is somewhere in the middle of the
    // pool when it grows, so every level of this ladder differs from its neighbour by a block of
    // pool memory that moved and a block that was abandoned.
    static const int kSites = 4;
    const int tracks[kSites] = {0, 3, 5, 7};
    const int steps[kSites] = {0, 7, 31, 63};

    g_stack.clear();
    pattern_init(g_live);
    for (int s = 0; s < kSites; s++) CHECK(pattern_set_lock(g_live, tracks[s], steps[s], 0, 1));

    const uint16_t first_used = g_live.locks_used;
    g_other = g_live;

    for (int k = 1; k <= kUndoLevels; k++) {
        CHECK(g_stack.mark(g_live, edit(GestureKind::lock, k)));
        const int s = k & (kSites - 1);
        CHECK(pattern_set_lock(g_live, tracks[s], steps[s], uint8_t(k), uint8_t(100 + k)));
    }
    CHECK(g_live.locks_used > first_used);
    CHECK_EQ(int(pattern_locks_reachable(g_live)), kSites + kUndoLevels);

    for (int k = kUndoLevels; k >= 1; k--) {
        CHECK(g_stack.undo(g_live));
        CHECK_EQ(int(pattern_locks_reachable(g_live)), kSites + k - 1);
        const int s = k & (kSites - 1);
        uint8_t value = 0;
        CHECK(!pattern_get_lock(g_live, tracks[s], steps[s], uint8_t(k), value));
    }
    CHECK(patterns_equal(g_live, g_other));
    CHECK(lock_views_equal(g_live, g_other));
    CHECK_EQ(int(g_live.locks_used), int(first_used));
}

// --- the whole timeline ---------------------------------------------------

TEST(a_long_random_sequence_matches_the_timeline) {
    // The model below is core/undo.hpp's timeline written out again in the plainest possible
    // form: `ids[i]` is which state sits at index i. Anything the ring gets wrong -- a slot
    // reused too early, a drop that shifts the wrong way, a coalesce that fires after an undo --
    // shows up as a state that comes back with the wrong contents, not merely a wrong depth.
    g_stack.clear();

    uint32_t ids[kUndoLevels + 1] = {};
    int cursor = 0;
    int newest = 0;
    uint32_t live_id = 1;
    uint32_t next_id = 2;
    Gesture last{};
    bool has_last = false;

    fill_pattern(g_live, live_id);

    uint32_t rng = 0x5EED1234u;
    bool depth_ok = true;
    bool agrees = true;
    bool content_ok = true;
    bool pending_ok = true;
    int marks = 0;
    int undos = 0;
    int redos = 0;
    int coalesced = 0;

    for (int i = 0; i < 400; i++) {
        const uint32_t r = next_random(rng);
        const int action = int(r & 3u);   // half the actions are marks, so history keeps growing

        if (action <= 1) {
            // Drawn from a handful of gestures rather than at random over all their fields:
            // with the whole space to pick from, two consecutive marks would never repeat one
            // and the coalescing path would go untested here. None of them is `none`.
            static const GestureKind kinds[5] = {GestureKind::trig, GestureKind::lock,
                                                 GestureKind::field, GestureKind::erase,
                                                 GestureKind::length};
            const int pick = int((r >> 2) % 5u);
            const Gesture gesture = edit(kinds[pick], pick);

            // Predicted without calling gesture_same: a model that leans on the code it checks
            // agrees with it even when both are wrong.
            const bool coalesces = has_last && last.kind == gesture.kind &&
                                   last.track == gesture.track && last.step == gesture.step &&
                                   last.seq == gesture.seq;
            const bool stored = g_stack.mark(g_live, gesture);
            if (stored == coalesces) agrees = false;

            if (stored) {
                marks++;
                ids[cursor] = live_id;
                cursor++;
                if (cursor > kUndoLevels) {
                    for (int k = 0; k < kUndoLevels; k++) ids[k] = ids[k + 1];
                    cursor = kUndoLevels;
                }
                newest = cursor;
                last = gesture;
                has_last = true;
                live_id = next_id++;
                fill_pattern(g_live, live_id);   // the edit the mark was taken for
            } else {
                coalesced++;
            }
        } else if (action == 2) {
            const bool possible = cursor > 0;
            const bool did = g_stack.undo(g_live);
            if (did != possible) agrees = false;
            if (did) {
                undos++;
                ids[cursor] = live_id;
                cursor--;
                live_id = ids[cursor];
                has_last = false;
            }
        } else {
            const bool possible = cursor < newest;
            const bool did = g_stack.redo(g_live);
            if (did != possible) agrees = false;
            if (did) {
                redos++;
                ids[cursor] = live_id;
                cursor++;
                live_id = ids[cursor];
                has_last = false;
            }
        }

        if (g_stack.depth() != cursor) agrees = false;
        if (g_stack.redo_depth() != newest - cursor) agrees = false;
        if (g_stack.depth() < 0 || g_stack.depth() > kUndoLevels) depth_ok = false;
        if (g_stack.redo_depth() < 0 || g_stack.redo_depth() > kUndoLevels) depth_ok = false;

        // No gesture here is ever `none`, so a name is available for every level that exists.
        if ((g_stack.pending() == GestureKind::none) != (cursor == 0)) pending_ok = false;

        if (!live_is(live_id)) content_ok = false;
    }

    CHECK(depth_ok);
    CHECK(agrees);
    CHECK(content_ok);
    CHECK(pending_ok);

    // The walk has to have exercised all three movements and the coalescing path, or the checks
    // above passed by never doing anything interesting.
    CHECK(marks > 50);
    CHECK(undos > 20);
    CHECK(redos > 20);
    CHECK(coalesced > 0);
    CHECK(int(next_id) > kUndoLevels + 1);   // the ring really did wrap
}
