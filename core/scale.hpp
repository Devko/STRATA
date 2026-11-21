// Keys and scales: which notes a melodic track is allowed to play.
//
// docs/SPEC.md asks for "Global key and scale. Melodic tracks fold notes into the scale, and
// KICK tunes to the root", and makes "notes in scale" one of the mutations. It does not say
// WHICH scales, so the table below is a decision made here and recorded here. A techno set
// mostly lives in natural minor, phrygian and the minor pentatonic; the modes and the symmetric
// scales are there because they cost one 16-bit constant each.
//
// ## The ids are permanent
//
// A scale id is stored in `ProjectSettings::scale` and `Pattern::scale` and goes on disk.
// `core/project.cpp` deliberately does NOT clamp it on load -- "a scale id this build does not
// have should survive a round trip rather than be thrown away" -- so a project written by a
// later build keeps its scale through a load and a save on this one. **Never renumber these**:
// a pattern saved by an older build would come back in a different key, which is silent and
// musical rather than loud and obvious. Add new scales at the end.
//
// Everything here treats an id this build does not know as `chromatic`, which is what
// core/project.hpp promises the UI will draw.
//
// ## Why bitmasks and not interval lists
//
// A scale is a 12-bit mask of the semitones it contains, which makes "is this note in the
// scale" one shift and one and. The alternative, a list of intervals, makes containment a loop
// and folding a search. These functions are called per step on a mutation and per note in KEYS
// mode, neither of which is an audio loop -- but the mask also makes the table one line per
// scale, which is the part that matters when someone has to check it against a reference.
//
// core/ is portable C++20 with no Enaga headers and no allocation; nothing here allocates and
// every function is total -- there is no note, root or scale id that produces anything but a
// note in 0..127.
#pragma once

#include <cstdint>

namespace strata {

// --- the table ------------------------------------------------------------

// Permanent ids; see the header comment. `count` is not a scale.
enum class Scale : uint8_t {
    chromatic = 0,
    minor,              // natural minor / aeolian: the default techno key
    major,              // ionian
    dorian,
    phrygian,
    lydian,
    mixolydian,
    locrian,
    harmonic_minor,
    melodic_minor,      // ascending
    minor_pentatonic,
    major_pentatonic,
    blues,
    whole_tone,
    diminished,         // octatonic, whole-half
    count
};

constexpr int kScaleCount = int(Scale::count);

// The semitones a scale contains, as a 12-bit mask with bit 0 the root. 0xFFF is chromatic.
// An id this build does not know gives 0xFFF, so an unknown scale never filters a note out.
//
// Written as the shifts themselves rather than as a binary literal with the intervals in a
// comment. The first draft of this table did it the other way round and six of the fourteen
// masks disagreed with their own comment -- a 12-bit literal has to be read right to left, and
// nothing checks a comment. This way the code IS the interval list.
constexpr uint16_t scale_mask(int scale) {
    switch (Scale(scale)) {
        // 0 2 3 5 7 8 10
        case Scale::minor:
            return (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 8) |
                   (1u << 10);
        // 0 2 4 5 7 9 11
        case Scale::major:
            return (1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) |
                   (1u << 11);
        // 0 2 3 5 7 9 10
        case Scale::dorian:
            return (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 9) |
                   (1u << 10);
        // 0 1 3 5 7 8 10
        case Scale::phrygian:
            return (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 8) |
                   (1u << 10);
        // 0 2 4 6 7 9 11
        case Scale::lydian:
            return (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 9) |
                   (1u << 11);
        // 0 2 4 5 7 9 10
        case Scale::mixolydian:
            return (1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) |
                   (1u << 10);
        // 0 1 3 5 6 8 10
        case Scale::locrian:
            return (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 8) |
                   (1u << 10);
        // 0 2 3 5 7 8 11
        case Scale::harmonic_minor:
            return (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 8) |
                   (1u << 11);
        // 0 2 3 5 7 9 11, ascending
        case Scale::melodic_minor:
            return (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 9) |
                   (1u << 11);
        // 0 3 5 7 10
        case Scale::minor_pentatonic:
            return (1u << 0) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 10);
        // 0 2 4 7 9
        case Scale::major_pentatonic:
            return (1u << 0) | (1u << 2) | (1u << 4) | (1u << 7) | (1u << 9);
        // 0 3 5 6 7 10
        case Scale::blues:
            return (1u << 0) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 10);
        // 0 2 4 6 8 10
        case Scale::whole_tone:
            return (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 8) | (1u << 10);
        // 0 2 3 5 6 8 9 11, whole-half octatonic
        case Scale::diminished:
            return (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 8) |
                   (1u << 9) | (1u << 11);
        case Scale::chromatic:
        default:
            return 0x0FFFu;
    }
}

// A short upper-case name for the screen; the device font has no lower case. "?" for an id this
// build does not have, so an unknown scale reads as unknown rather than as chromatic -- the UI
// should say it cannot name it even though it plays it chromatically.
const char *scale_name(int scale);

// How many notes the scale has in an octave: 1 to 12. An unknown id gives 12.
int scale_size(int scale);

// --- naming a key ---------------------------------------------------------

// The twelve pitch classes, 0 being C. Only `note % 12` is read, so a MIDI note and a pitch
// class both work.
//
// Spelled with FLATS -- C, DB, D, EB ... -- and in upper case, for two reasons. The device font
// has "upper case, digits and a few symbols only" (docs/ENAGA_API.md) and nothing in the app
// draws a '#' today, so a sharp sign is unproven; and techno lives in minor keys, where the
// flat spelling is the one a musician would write anyway. 'B' therefore does double duty as the
// note B and as the flat sign, which reads correctly in context: "BB" is B flat.
const char *note_name(uint8_t note);

// Which octave a MIDI note is in, in the convention where middle C (60) is C4. For the screen
// only; nothing here depends on it.
constexpr int note_octave(uint8_t note) { return int(note) / 12 - 1; }

// --- notes ----------------------------------------------------------------
//
// `root` is a MIDI note, and only its pitch class matters: root 60 and root 0 are the same key.
// docs/SPEC.md's "global key" is `ProjectSettings::root`, which a project stores as 0..127.

// True when `note` is in the scale rooted at `root`.
bool scale_contains(uint8_t note, uint8_t root, int scale);

// The nearest note in the scale, searching upwards and downwards together and preferring the
// LOWER note on a tie. A tie only happens in the symmetric scales (whole tone's every other
// semitone, diminished's), and preferring down keeps a bass line from drifting up each time a
// mutation folds it.
//
// Always in 0..127: near the ends of the MIDI range the search gives up on the direction that
// would leave it and takes the other, so a note at 127 in a scale that has nothing at 127 folds
// down rather than wrapping or clamping to a note outside the scale.
uint8_t scale_fold(uint8_t note, uint8_t root, int scale);

// `degrees` scale steps from `note`, up or down, folding first if `note` is not in the scale.
// Saturates at the ends of the MIDI range rather than wrapping: a bass line walked off the
// bottom should sit on the lowest note it can play, not reappear at the top.
//
// This is what "notes in scale" means for a mutation and what KEYS mode will walk: moving by
// degrees rather than semitones is the difference between a line that stays in the key and one
// that is transposed out of it.
uint8_t scale_step(uint8_t note, uint8_t root, int scale, int degrees);

// The scale degree of `note` within its octave, 0 for the root; -1 when the note is not in the
// scale. For the screen, and for tests that want to say "a third up" rather than "four
// semitones up".
int scale_degree_of(uint8_t note, uint8_t root, int scale);

}  // namespace strata
