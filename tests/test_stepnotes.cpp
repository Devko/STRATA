// A step's extra notes (core/stepnotes.hpp): stored as locks 32-36, so a POLY step can hold the
// chord that was played into it note for note.
//
// Patterns are file-scope statics: one is about 8 KiB and a stack frame is capped at 16 KiB.
#include "check.hpp"

#include "chord.hpp"
#include "editops.hpp"
#include "pattern.hpp"
#include "scale.hpp"
#include "stepnotes.hpp"
#include "track.hpp"

#include <cstring>

using namespace strata;

namespace {

Pattern g_pattern;

Pattern &blank() {
    pattern_init(g_pattern);
    return g_pattern;
}

}  // namespace

TEST(stepnotes_the_ids_start_past_every_track_parameter) {
    // The engine applies a lock only below kTrackParams, so a note lock can never be heard as a
    // parameter -- and a build from before this skips them for the same reason.
    CHECK(int(kLockNoteFirst) >= kTrackParams);
    CHECK(int(kLockNoteFirst) + kStepExtraNotes <= int(kLockParamMask) + 1);
    CHECK(!lock_is_note(uint8_t(kLockNoteFirst - 1)));
    CHECK(lock_is_note(kLockNoteFirst));
    CHECK(lock_is_note(uint8_t(kLockNoteFirst + kStepExtraNotes - 1)));
    CHECK(!lock_is_note(uint8_t(kLockNoteFirst + kStepExtraNotes)));
    CHECK(!lock_is_note(uint8_t(kLockNoteFirst | kLockRange)));
    CHECK(!lock_is_note(uint8_t(kLockNoteFirst | kLockSlide)));
    CHECK_EQ(kStepMaxNotes, kChordMaxNotes);     // six notes: VA POLY's voices
}

TEST(stepnotes_a_step_takes_five_extras_and_refuses_a_sixth) {
    Pattern &p = blank();
    p.tracks[6].steps[4].note = 60;
    CHECK(step_add_note(p, 6, 4, 60) == NoteAdd::present);        // its own note
    CHECK(step_add_note(p, 6, 4, 64) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 4, 64) == NoteAdd::present);        // never doubled
    CHECK(step_add_note(p, 6, 4, 67) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 4, 71) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 4, 74) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 4, 77) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 4, 81) == NoteAdd::full);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(p, 6, 4, extras), 5);
    CHECK_EQ(int(extras[0]), 64);
    CHECK_EQ(int(extras[4]), 77);
    CHECK(step_has_note(p, 6, 4, 60));
    CHECK(step_has_note(p, 6, 4, 74));
    CHECK(!step_has_note(p, 6, 4, 81));
    CHECK_EQ(step_param_lock_count(p, 6, 4), 0);
    CHECK_EQ(int(p.tracks[6].steps[4].lock_count), 5);
}

TEST(stepnotes_notes_and_parameter_locks_share_the_eight_slots) {
    Pattern &p = blank();
    for (int param = 0; param < 6; param++) CHECK(pattern_set_lock(p, 6, 0, uint8_t(param), 90));
    CHECK(step_add_note(p, 6, 0, 64) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 0, 67) == NoteAdd::added);
    CHECK(step_add_note(p, 6, 0, 71) == NoteAdd::full);           // eight locks
    CHECK_EQ(step_param_lock_count(p, 6, 0), 6);

    // Clearing the notes leaves the parameter locks, and the reverse.
    step_clear_extra_notes(p, 6, 0);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(p, 6, 0, extras), 0);
    CHECK_EQ(step_param_lock_count(p, 6, 0), 6);
    uint8_t value = 0;
    CHECK(pattern_get_lock(p, 6, 0, 5, value));
    CHECK_EQ(int(value), 90);
}

TEST(stepnotes_a_shift_moves_every_extra_and_clamps) {
    Pattern &p = blank();
    step_add_note(p, 6, 2, 64);
    step_add_note(p, 6, 2, 120);
    step_shift_extra_notes(p, 6, 2, 12);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(p, 6, 2, extras), 2);
    CHECK_EQ(int(extras[0]), 76);
    CHECK_EQ(int(extras[1]), 127);
    step_shift_extra_notes(p, 6, 2, -200);
    step_extra_notes(p, 6, 2, extras);
    CHECK_EQ(int(extras[0]), 0);
    CHECK_EQ(int(extras[1]), 0);
}

TEST(stepnotes_a_merge_is_ascending_unique_and_capped_from_the_top) {
    uint8_t notes[kStepMaxNotes + kChordMaxNotes] = {48, 55};
    const uint8_t extras[] = {64, 52, 55, 60};
    int count = notes_merge(notes, 2, extras, 4, 0, 0, false, 6);
    CHECK_EQ(count, 5);
    const uint8_t want[] = {48, 52, 55, 60, 64};
    for (int i = 0; i < 5; i++) CHECK_EQ(int(notes[i]), int(want[i]));

    // Capped at three: the lowest three survive, whatever order they arrived in.
    uint8_t few[3] = {60};
    const uint8_t more[] = {72, 67, 64, 50};
    count = notes_merge(few, 1, more, 4, 0, 0, false, 3);
    CHECK_EQ(count, 3);
    CHECK_EQ(int(few[0]), 50);
    CHECK_EQ(int(few[1]), 60);
    CHECK_EQ(int(few[2]), 64);
}

TEST(stepnotes_a_folded_merge_folds_each_extra_into_the_key) {
    // C minor (natural): E (64) is not in it and folds as core/scale does it.
    const int minor = 1;
    uint8_t notes[kStepMaxNotes] = {60};
    const uint8_t extras[] = {64};
    const int count = notes_merge(notes, 1, extras, 1, 0, minor, true, kStepMaxNotes);
    CHECK_EQ(count, 2);
    CHECK_EQ(int(notes[1]), int(scale_fold(64, 0, minor)));
    CHECK(notes[1] != 64 || scale_fold(64, 0, minor) == 64);
}

TEST(stepnotes_a_transpose_moves_the_whole_chord) {
    Pattern &p = blank();
    p.tracks[6].steps[3].note = 60;
    step_add_note(p, 6, 3, 64);
    step_add_note(p, 6, 3, 67);
    range_transpose(p, 6, 0, 8, 5);
    CHECK_EQ(int(p.tracks[6].steps[3].note), 65);
    uint8_t extras[kStepExtraNotes];
    CHECK_EQ(step_extra_notes(p, 6, 3, extras), 2);
    CHECK_EQ(int(extras[0]), 69);
    CHECK_EQ(int(extras[1]), 72);
}

// The first track and the first step are ones; a merge into an empty list, into a list whose
// next byte happens to equal the note, and into a full list keep to their counts and `max`.
TEST(stepnotes_edges_first_step_and_merge_counts) {
    Pattern &p = blank();
    p.tracks[0].steps[0].note = 48;
    CHECK(step_has_note(p, 0, 0, 48));
    CHECK(step_add_note(p, 0, 0, 52) == NoteAdd::added);
    CHECK(step_has_note(p, 0, 0, 52));
    CHECK(!step_has_note(p, kTracks, 0, 48));
    CHECK(step_add_note(p, kTracks, 0, 52) == NoteAdd::full);
    CHECK_EQ(step_extra_notes(p, 0, 0, nullptr), 0);

    const uint8_t one[1] = {60};
    uint8_t notes[8];
    std::memset(notes, 0xEE, sizeof notes);
    CHECK_EQ(notes_merge(notes, 0, one, 1, 0, 0, false, 4), 1);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(notes_merge(nullptr, 0, one, 1, 0, 0, false, 4), 0);
    CHECK_EQ(notes_merge(notes, 0, one, 1, 0, 0, false, 0), 0);
    std::memset(notes, 0xEE, sizeof notes);
    CHECK_EQ(notes_merge(notes, 0, one, 1, 0, 0, false, 1), 1);
    CHECK_EQ(int(notes[0]), 60);
    // A stale 67 just past the list is not a note it has.
    const uint8_t sixty_seven[1] = {67};
    notes[0] = 60;
    notes[1] = 64;
    notes[2] = 67;
    CHECK_EQ(notes_merge(notes, 2, sixty_seven, 1, 0, 0, false, 4), 3);
    // Full at max 3: the highest falls off, and nothing is written past max.
    std::memset(notes, 0xEE, sizeof notes);
    notes[0] = 60;
    notes[1] = 64;
    notes[2] = 67;
    const uint8_t sixty_two[1] = {62};
    CHECK_EQ(notes_merge(notes, 3, sixty_two, 1, 0, 0, false, 3), 3);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 62);
    CHECK_EQ(int(notes[2]), 64);
    CHECK_EQ(int(notes[3]), 0xEE);
}
