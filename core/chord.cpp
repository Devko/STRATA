// Chords. core/chord.hpp has the packing, the table and the reasoning behind both.
#include "chord.hpp"

#include "scale.hpp"

namespace strata {
namespace {

constexpr uint8_t kNoteMax = 127;

// An octave. Named because it is the inversion's whole mechanism and because twelve is also the
// number of pitch classes, which it is not here.
constexpr int kOctave = 12;

}  // namespace

const char *chord_name(ChordType type) {
    switch (type) {
        case ChordType::none:         return "1 NOTE";
        case ChordType::minor:        return "MIN";
        case ChordType::major:        return "MAJ";
        case ChordType::sus2:         return "SUS2";
        case ChordType::sus4:         return "SUS4";
        case ChordType::dim:          return "DIM";
        case ChordType::min7:         return "MIN7";
        case ChordType::dom7:         return "DOM7";
        case ChordType::maj7:         return "MAJ7";
        case ChordType::min9:         return "MIN9";
        case ChordType::min11:        return "MIN11";
        case ChordType::fifth:        return "5TH";
        case ChordType::octave:       return "OCT";
        case ChordType::fifth_octave: return "5TH OCT";
        case ChordType::count:
            break;
    }
    return "?";
}

// --- the packed byte ------------------------------------------------------

uint8_t chord_pack(ChordType type, uint8_t inversion) {
    const uint8_t id = uint8_t(type);
    // An id this build does not define would be stored as a chord nothing can play, and 0 is
    // the one byte that means a single note, so that is what it becomes. `none` takes the same
    // path for the reason the header gives: one encoding of "no chord" and not four.
    if (id == 0 || id >= uint8_t(kChordTypeCount)) return 0;

    const uint8_t deepest = uint8_t(chord_max_inversion(type));
    const uint8_t inv = inversion > deepest ? deepest : inversion;
    return uint8_t(id | uint8_t(uint8_t(inv & kChordInversionMask) << kChordInversionShift));
}

ChordType chord_type(uint8_t packed) {
    if ((packed & kChordReservedMask) != 0) return ChordType::none;
    const uint8_t id = uint8_t(packed & kChordTypeMask);
    return id < uint8_t(kChordTypeCount) ? ChordType(id) : ChordType::none;
}

uint8_t chord_inversion(uint8_t packed) {
    // Read through `chord_type`, so a reserved bit or an unknown id takes the inversion with it.
    // Otherwise byte 0x10 -- an inversion of nothing -- would report a first inversion for a
    // step that plays one note, and the UI would draw a chord that is not there.
    if (chord_type(packed) == ChordType::none) return 0;
    return uint8_t((packed >> kChordInversionShift) & kChordInversionMask);
}

bool chord_valid(uint8_t packed) {
    if ((packed & kChordReservedMask) != 0) return false;
    const uint8_t id = uint8_t(packed & kChordTypeMask);
    if (id >= uint8_t(kChordTypeCount)) return false;
    const uint8_t inv = uint8_t((packed >> kChordInversionShift) & kChordInversionMask);
    return inv <= uint8_t(chord_max_inversion(ChordType(id)));
}

// --- expansion ------------------------------------------------------------

int chord_fold(const int8_t *semitones, int count, uint8_t root, uint8_t key, int scale,
               uint8_t *notes, int max) {
    if (semitones == nullptr || notes == nullptr) return 0;

    int wanted = count < max ? count : max;
    if (wanted > kChordMaxNotes) wanted = kChordMaxNotes;
    if (wanted <= 0) return 0;

    // The stack sits on the FOLDED root, not on the note the step holds: see "The root is folded
    // too" in core/chord.hpp. Interval 0 therefore always survives untouched, which is what
    // makes `none` -- and a chord byte from a later build -- exactly the step's own note folded.
    const uint8_t base = scale_fold(root, key, scale);

    int written = 0;
    for (int i = 0; i < wanted; i++) {
        const int raw = int(base) + int(semitones[i]);
        // Dropped, not clamped. Clamping would pile the top of a chord onto note 127: several
        // voices on one pitch at the top of the range, which is louder and less like the chord
        // than not having that note at all. A negative one cannot come from the table, but this
        // function takes a caller's stack.
        if (raw < 0 || raw > int(kNoteMax)) continue;

        uint8_t note = scale_fold(uint8_t(raw), key, scale);

        if (written > 0) {
            // Two intervals can fold onto one pitch, or even cross. It happens at the top of the
            // MIDI range today -- SUS4's fourth and fifth both fold onto the highest note four
            // of the keys have -- and it would happen anywhere in the range for a scale with a
            // gap wider than any in core/scale.hpp's table. Lift by whole scale steps until the
            // note clears the one below it. Each step strictly increases it and `scale_step`
            // saturates rather than wrapping, so this ends either clear of the previous note or
            // on the highest note the scale has: it cannot spin.
            while (note <= notes[written - 1]) {
                const uint8_t lifted = scale_step(note, key, scale, 1);
                if (lifted == note) break;                  // the MIDI range ran out
                note = lifted;
            }
            if (note <= notes[written - 1]) continue;       // nowhere left to put it: dropped
        }

        notes[written++] = note;
    }
    return written;
}

int chord_invert(uint8_t *notes, int count, int inversion) {
    // One note has no voicing to move, and an inversion as deep as the chord has notes would
    // lift all of them: a transpose, which the step's note field already does.
    if (notes == nullptr || count < 2 || inversion <= 0) return 0;
    const int deepest = count - 1;
    const int wanted = inversion < deepest ? inversion : deepest;

    int done = 0;
    for (int i = 0; i < wanted; i++) {
        const int up = int(notes[0]) + kOctave;
        if (up > int(kNoteMax)) break;

        // A note the chord already holds. OCT is the case that matters: its own octave up is
        // its top note, and rotating would put two of six voices on one pitch at twice the
        // level. Stopping leaves the voicing as it was, which is the honest answer to "invert
        // something that is already inverted", and `done` tells the caller it did not move.
        bool doubled = false;
        for (int j = 1; j < count; j++) {
            if (int(notes[j]) == up) doubled = true;
        }
        if (doubled) break;

        // Inserted where it belongs rather than appended. A chord can span more than an octave
        // -- MIN9 is fourteen semitones -- so the old bottom note lifted is not always the new
        // top note, and an array that stops ascending breaks the promise every caller of
        // `chord_expand` reads.
        int at = 0;
        while (at + 1 < count && notes[at + 1] < uint8_t(up)) {
            notes[at] = notes[at + 1];
            at++;
        }
        notes[at] = uint8_t(up);
        done++;
    }
    return done;
}

int chord_expand(uint8_t root, uint8_t packed, uint8_t key, int scale, uint8_t *notes, int max) {
    if (notes == nullptr || max <= 0) return 0;

    const ChordIntervals stack = chord_intervals(chord_type(packed));
    const int written =
        chord_fold(stack.semitones, int(stack.count), root, key, scale, notes, max);

    // Fold first, then invert. The inversion works on the notes that will actually sound, so it
    // can neither undo a drop nor lift a note the key moved somewhere else.
    chord_invert(notes, written, int(chord_inversion(packed)));
    return written;
}

}  // namespace strata
