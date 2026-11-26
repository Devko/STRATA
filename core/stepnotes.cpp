// A step's extra notes, as locks. core/stepnotes.hpp says why they are locks.
#include "stepnotes.hpp"

#include "chord.hpp"
#include "clamp.hpp"
#include "scale.hpp"

namespace strata {

int step_extra_notes(const Pattern &pattern, int track, int step, uint8_t *out) {
    int count = 0;
    if (out == nullptr) return 0;
    for (const Lock &lock : pattern_step_locks(pattern, track, step)) {
        if (!lock_is_note(lock.param) || count >= kStepExtraNotes) continue;
        out[count++] = uint8_t(lock.value & 0x7Fu);
    }
    return count;
}

bool step_has_note(const Pattern &pattern, int track, int step, uint8_t note) {
    if (track < 0 || track >= kTracks || step < 0 || step >= kStepsPerTrack) return false;
    if (pattern.tracks[track].steps[step].note == note) return true;
    uint8_t extras[kStepExtraNotes];
    const int count = step_extra_notes(pattern, track, step, extras);
    for (int i = 0; i < count; i++) {
        if (extras[i] == note) return true;
    }
    return false;
}

NoteAdd step_add_note(Pattern &pattern, int track, int step, uint8_t note) {
    if (track < 0 || track >= kTracks || step < 0 || step >= kStepsPerTrack) return NoteAdd::full;
    const uint8_t wanted = uint8_t(note & 0x7Fu);
    if (step_has_note(pattern, track, step, wanted)) return NoteAdd::present;
    // The first id the step does not use. An id is the slot, not the order of the notes.
    for (int i = 0; i < kStepExtraNotes; i++) {
        const uint8_t id = uint8_t(kLockNoteFirst + i);
        uint8_t ignored = 0;
        if (pattern_get_lock(pattern, track, step, id, ignored)) continue;
        return pattern_set_lock(pattern, track, step, id, wanted) ? NoteAdd::added : NoteAdd::full;
    }
    return NoteAdd::full;
}

void step_clear_extra_notes(Pattern &pattern, int track, int step) {
    for (int i = 0; i < kStepExtraNotes; i++) {
        pattern_clear_lock(pattern, track, step, uint8_t(kLockNoteFirst + i));
    }
}

void step_shift_extra_notes(Pattern &pattern, int track, int step, int semitones) {
    if (semitones == 0) return;
    const int by = clamp_int(semitones, -127, 127);
    for (int i = 0; i < kStepExtraNotes; i++) {
        const uint8_t id = uint8_t(kLockNoteFirst + i);
        uint8_t value = 0;
        if (!pattern_get_lock(pattern, track, step, id, value)) continue;
        // An edit of a lock the step has: never allocates, so it cannot fail.
        pattern_set_lock(pattern, track, step, id, uint8_t(clamp_int(int(value) + by, 0, 127)));
    }
}

int step_param_lock_count(const Pattern &pattern, int track, int step) {
    int count = 0;
    for (const Lock &lock : pattern_step_locks(pattern, track, step)) {
        count += lock_is_note(lock.param) ? 0 : 1;
    }
    return count;
}

int notes_merge(uint8_t *notes, int count, const uint8_t *extras, int extra_count,
                uint8_t key, int scale, bool fold, int max) {
    if (notes == nullptr || max <= 0) return 0;
    constexpr int kMost = kStepMaxNotes + kChordMaxNotes;
    const int cap = max < kMost ? max : kMost;
    int have = clamp_int(count, 0, cap);
    if (extras == nullptr) return have;
    for (int i = 0; i < extra_count; i++) {
        const uint8_t note = fold ? scale_fold(uint8_t(extras[i] & 0x7Fu), key, scale)
                                  : uint8_t(extras[i] & 0x7Fu);
        // Insertion into the ascending list; a note already there is not doubled.
        int at = 0;
        while (at < have && notes[at] < note) at++;
        if (at < have && notes[at] == note) continue;
        if (at >= cap) continue;                       // above everything, and no room: dropped
        const int last = have < cap ? have : cap - 1;  // full: the highest note falls off
        for (int j = last; j > at; j--) notes[j] = notes[j - 1];
        notes[at] = note;
        have = have < cap ? have + 1 : cap;
    }
    return have;
}

}  // namespace strata
