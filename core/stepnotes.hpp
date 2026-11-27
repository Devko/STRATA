// A step's EXTRA notes: what lets a POLY step hold the chord that was played into it, note for
// note, rather than one note and a chord type.
//
// ## Why locks
//
// A Step is 12 bytes and must stay 12 (core/pattern.hpp says why: the memory budget and every
// file ever written). Six notes in every step would grow the project by about 330 KB for the
// one or two tracks that play chords. A step's locks are already a variable-length run in the
// pattern's pool, and a lock's param byte holds 64 ids of which a track uses kTrackParams (32):
// ids 32-36 here mean "extra note 1-5", and the lock's value is the note. So a step holds its
// own `note` plus up to five more -- six, which is the voices VA POLY has.
//
// That needs no file change. serialize.cpp writes a lock's param byte as it stands, a build
// from before this skips the entries (its engine ignores every param at or past kTrackParams),
// and an older project has none, so it loads and plays exactly as before.
//
// What it costs is a lock slot per extra note: a six-note step has three left for parameter
// locks, and the pool's 1024 hold 320 six-note steps. All of a step's notes share its velocity,
// LEN and micro-timing.
//
// ## What the notes are
//
// The step's `note` is still its root: what a MONO reading, LEN, the chord byte and the step
// editor's NOTE field mean. The extras are the other notes, in the order they arrived; nothing
// here sorts them. A chord TYPE and extras can both be on one step -- the type's stack on the
// root, with the extras added -- because the type is how a chord is programmed and the extras
// are how one is played, and neither should throw the other away.
//
// core/ is portable C++20 with no Enaga headers; nothing here allocates.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

// The lock ids. 32 is the first id past a track's parameters (dsp/track.hpp's kTrackParams,
// which core/ cannot include; tests/test_stepnotes.cpp checks the two agree).
constexpr uint8_t kLockNoteFirst = 32;
constexpr int kStepExtraNotes = 5;
constexpr int kStepMaxNotes = 1 + kStepExtraNotes;

// A note lock: one of the five ids, with neither the range nor the slide bit.
constexpr bool lock_is_note(uint8_t param_byte) {
    return param_byte >= kLockNoteFirst && param_byte < kLockNoteFirst + kStepExtraNotes;
}

// The step's extra notes, in the order their ids were taken, into `out` (room for
// kStepExtraNotes). Returns how many.
int step_extra_notes(const Pattern &pattern, int track, int step, uint8_t *out);

// True if `note` is the step's own note or one of its extras.
bool step_has_note(const Pattern &pattern, int track, int step, uint8_t note);

// Adds `note` to the step. `present` when it is the step's note or an extra already (nothing
// changes), `added` when it took a free id, `full` when the step has five extras already or
// no lock slot is left (the pattern unchanged). A full pool is the caller's to compact and
// retry, as every other lock write does.
enum class NoteAdd : uint8_t { added, present, full };
NoteAdd step_add_note(Pattern &pattern, int track, int step, uint8_t note);

// Removes every extra note; parameter locks stay.
void step_clear_extra_notes(Pattern &pattern, int track, int step);

// Moves every extra note by `semitones`, clamped into 0..127: what a transpose or a NOTE edit
// of the root does to the rest of the chord, so the chord moves as one.
void step_shift_extra_notes(Pattern &pattern, int track, int step, int semitones);

// The step's locks that are NOT notes: what the screen and the keys call its locks.
int step_param_lock_count(const Pattern &pattern, int track, int step);

// Adds `extras` to the notes a step already plays -- `notes[0..count)`, ascending, as
// `chord_expand` returns them -- each folded into the key first when `fold` is set (a POLY
// track folds its whole chord, core/chord.hpp). The result is ascending with no two the same,
// and at most `max` long (and never more than kStepMaxNotes + kChordMaxNotes): the highest
// notes are the ones dropped. `notes` must have room for `max`. Returns the new count.
int notes_merge(uint8_t *notes, int count, const uint8_t *extras, int extra_count,
                uint8_t key, int scale, bool fold, int max);

}  // namespace strata
