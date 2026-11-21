// Chords: what the byte in Step::chord means, and how one becomes notes.
//
// docs/SPEC.md gives track 7 *"Poly VA, 6 voices, chord per step"* and the step table one line:
// *"Note 0-127 -- POLY tracks add chord type and inversion"*. core/pattern.hpp has carried the
// byte since M3 -- serialized, copied, undone and compared with every other field -- but
// nothing said what it means. This says, and defines the arithmetic over it.
//
// ## The packing
//
// Four bits of chord type and two of inversion. The shape is core/pattern.hpp's: one byte,
// named constants, pack and unpack through functions rather than by hand, and a byte this build
// does not recognise reads back as the harmless value rather than as something arbitrary -- the
// rule `condition_kind` states and `ratchet_pack` follows with its nibbles.
//
//     bits 0-3   chord type: a permanent id. 0 is `none`, which is a single note
//     bits 4-5   inversion, 0-3
//     bits 6-7   reserved and must be zero; a byte with either set is not a chord
//
// **Type 0 with inversion 0 is a single note**, so zeroed memory -- `step_clear`, a cleared
// pattern, a zeroed file -- plays one note. That is why the TYPE and not the inversion owns the
// low bits: a cleared pattern must not sprout chords, and `chord_pack` therefore has exactly
// one encoding of "no chord", 0, rather than four that differ only in an inversion nobody can
// hear.
//
// Six bits and not seven, because core/stepedit.cpp already publishes this field's editable
// range as 0..63 -- `step_field_positions(StepField::chord)` is 64 -- and that file is not this
// module's to change. A packing that reached bit 6 would encode chords the step editor could
// never walk to, which is worse than a table two entries shorter.
//
// ## The ids are permanent
//
// core/serialize.cpp writes this byte raw into every pattern and `step_sanitize` deliberately
// does not touch it, so a byte a later build wrote survives a load and a save here.
// core/scale.hpp makes the same promise about scale ids for the same reason, and it applies
// word for word: **never renumber these.** A pattern saved by an older build would come back
// with every chord a different chord, which is silent and musical rather than loud and obvious.
// Add new types at the next free id and change nothing else.
//
// Ids 14 and 15 are deliberately left free. Four bits hold sixteen types and the editor's 0..63
// cannot be widened from here, so the growth room has to come out of the table -- and a build
// that meets an id it does not know plays a single note (`chord_type`), which is the quiet
// degradation rather than a random chord.
//
// ## Which chords exist is a decision made here
//
// docs/SPEC.md does not say. STRATA is a techno box, so the table is weighted that way: the
// minor family four deep -- MIN, MIN7, MIN9, MIN11, the dub stab and its lusher versions --
// both suspensions, the bright triad and its two sevenths, one diminished for tension, and the
// power-chord group (5TH, OCT, 5TH OCT) a rave stab is built from. Nothing has more than six
// notes, because VA POLY has six voices (docs/SPEC.md, "Machines (v1)") and a chord that cannot
// be voiced is not a chord.
//
// ## Semitone intervals, then folded -- not scale degrees
//
// A chord here is a FIXED SEMITONE STACK whose every note is folded into the key with
// `scale_fold`. The alternative -- stacking scale DEGREES, so a chord is diatonically whatever
// its degree gives -- is defensible, and was rejected for four reasons:
//
//  - docs/SPEC.md already names the mechanism for staying in a key: *"Melodic tracks fold notes
//    into the scale."* Degrees would be a second mechanism for the same job, and core/mutate.cpp
//    already folds by that rule.
//  - This table names QUALITIES. Someone who picks MIN7 wants that sound. Degrees can promise
//    diatonic function but cannot promise a quality, which would make every name on the screen
//    a different kind of claim.
//  - Folding gives the diatonic behaviour anyway, which is the surprise that settled it. A
//    major stack folded onto each degree of the natural minor comes out as exactly that
//    degree's diatonic triad -- i, ii dim, III, iv, v, VI, VII -- and the same holds in every
//    diatonic mode. `chord_a_major_stack_is_the_diatonic_triad_on_every_degree` checks it. So a
//    chord on the sixth really does differ from one on the root, with no second mechanism.
//  - CHROMATIC is a scale here, under which folding is the identity, so chords come out exactly
//    as named for anyone who wants a stab moved around without a key. Under degrees, chromatic
//    would make every chord in the table a whole-tone cluster.
//
// What folding costs is the name in the other direction: ask for MIN where the key has a major
// third and you get the nearest voicing the key allows, which is a suspension and not a minor
// chord. That is the trade -- audible, in key, and escapable by choosing CHROMATIC.
//
// ## The root is folded too
//
// `chord_expand` folds the step's note before stacking on it. A chord is one harmonic object:
// folding its third and fifth into the key while leaving its root outside would put the one
// note that defines the chord -- and the loudest one -- out of the key. Nothing else in the
// playback path folds today (core/mutate.cpp folds at edit time), so a POLY step's note is
// treated a little differently from a MONO step's; if the engine later folds every melodic note
// at playback, this becomes the same rule rather than an exception.
//
// ## Totality
//
// Every function here is total, in the sense core/scale.hpp uses: no root, no byte, no key and
// no scale id produces a note outside 0..127, more notes than `kChordMaxNotes`, two notes on one
// pitch, or notes out of order. The caller is a fixed pool of voices in audio context, which
// cannot refuse a seventh note when one arrives -- so the bound is in the signature rather than
// in a comment.
//
// core/ is portable C++20 with no Enaga headers; nothing here allocates, divides in a loop or
// keeps state between calls.
#pragma once

#include <cstdint>

namespace strata {

// --- the table ------------------------------------------------------------

// Permanent ids; see the header comment. `count` is not a chord, and 14 and 15 are free for
// later.
enum class ChordType : uint8_t {
    none = 0,       // a single note, which is what a zeroed Step means
    minor,          // MIN:     the techno chord
    major,          // MAJ
    sus2,           // SUS2
    sus4,           // SUS4
    dim,            // DIM
    min7,           // MIN7:    the dub-techno stab
    dom7,           // DOM7
    maj7,           // MAJ7
    min9,           // MIN9
    min11,          // MIN11:   all six voices
    fifth,          // 5TH:     two voices, the power stab
    octave,         // OCT
    fifth_octave,   // 5TH OCT: the rave stab
    count
};

// The ids this build defines, `none` included. Four bits hold `kChordTypeIds` of them; the
// difference is the growth room, and an id inside it plays as a single note.
constexpr int kChordTypeCount = int(ChordType::count);
constexpr int kChordTypeIds = 16;
static_assert(kChordTypeCount <= kChordTypeIds, "a chord type id must fit in four bits");

// VA POLY has six voices (docs/SPEC.md, "Machines (v1)"), so no chord has more than six notes
// and no expansion writes more. A caller with fewer free voices passes its own bound.
constexpr int kChordMaxNotes = 6;

// The fields of the packed byte. The reserved bits are CHECKED, not masked away: a build that
// does not understand a byte must not play a chord guessed from the bits it does understand.
constexpr uint8_t kChordTypeMask = 0x0Fu;
constexpr uint8_t kChordInversionShift = 4;
constexpr uint8_t kChordInversionMask = 0x03u;   // after the shift
constexpr uint8_t kChordInversionMax = 3;
constexpr uint8_t kChordReservedMask = 0xC0u;

// A chord's semitones above its root, lowest first, and how many there are. Entries past
// `count` are 0 and must not be read.
struct ChordIntervals {
    int8_t semitones[kChordMaxNotes];
    uint8_t count;
};

// The stack a chord type is. Written as the intervals themselves, and kept in the header for
// the reason core/scale.hpp keeps its masks there: one line per chord, so someone checking the
// table against a reference reads the code and not a comment that nothing verifies.
//
// `none` and an id this build does not have both give the root alone -- one note, no chord --
// which is what makes a zeroed pattern a pattern of single notes, and a chord from a later
// build quiet rather than wrong.
constexpr ChordIntervals chord_intervals(ChordType type) {
    switch (type) {
        case ChordType::minor:        return {{0, 3, 7}, 3};
        case ChordType::major:        return {{0, 4, 7}, 3};
        case ChordType::sus2:         return {{0, 2, 7}, 3};
        case ChordType::sus4:         return {{0, 5, 7}, 3};
        case ChordType::dim:          return {{0, 3, 6}, 3};
        case ChordType::min7:         return {{0, 3, 7, 10}, 4};
        case ChordType::dom7:         return {{0, 4, 7, 10}, 4};
        case ChordType::maj7:         return {{0, 4, 7, 11}, 4};
        case ChordType::min9:         return {{0, 3, 7, 10, 14}, 5};
        case ChordType::min11:        return {{0, 3, 7, 10, 14, 17}, 6};
        case ChordType::fifth:        return {{0, 7}, 2};
        case ChordType::octave:       return {{0, 12}, 2};
        case ChordType::fifth_octave: return {{0, 7, 12}, 3};
        case ChordType::none:
        case ChordType::count:
            break;
    }
    return {{0}, 1};
}

// The deepest inversion that moves a chord's voicing: one less than its note count, capped at
// what two bits hold. An inversion as deep as the chord has notes lifts every note an octave,
// which is a transpose and not an inversion -- and the step's note field already has octaves.
constexpr int chord_max_inversion(ChordType type) {
    const int notes = int(chord_intervals(type).count);
    const int deepest = notes > 0 ? notes - 1 : 0;
    return deepest < int(kChordInversionMax) ? deepest : int(kChordInversionMax);
}

// A short upper-case name for the screen; the device font has upper case, digits and a few
// symbols only. "1 NOTE" for `none`, because a zero in this field does not switch anything off
// -- the step plays its note and nothing else -- and "?" for an id this build does not have, so
// an unknown chord reads as unknown rather than as a chord it is not.
const char *chord_name(ChordType type);

// --- the packed byte ------------------------------------------------------

// The byte for a chord. `none`, and any id this build does not define, give 0: there is exactly
// one encoding of a single note. The inversion is clamped into 0..`chord_max_inversion(type)`,
// so a writer never produces two bytes that sound the same -- readers still accept a deeper
// one, because a hand-made file might hold it, in the same spirit as `condition_pack`.
uint8_t chord_pack(ChordType type, uint8_t inversion);

// `ChordType::none` for a byte with a reserved bit set or an id this build does not have, so an
// unknown chord is one note rather than a guess.
ChordType chord_type(uint8_t packed);

// 0..3, and 0 whenever `chord_type` is `none`: an inversion of nothing is nothing.
uint8_t chord_inversion(uint8_t packed);

// True only for a byte `chord_pack` can produce: no reserved bits, a type this build has, and
// an inversion that chord can use. Every other byte still PLAYS -- as a single note -- which is
// the difference between this and a refusal.
bool chord_valid(uint8_t packed);

// --- expansion ------------------------------------------------------------

// The notes a step plays. `root` is the step's note, `packed` its chord byte, `key` the
// pattern's root and `scale` its scale id (core/scale.hpp). Returns how many notes were written
// to `notes`: at least 1, and never more than `max` or `kChordMaxNotes`, whichever is smaller.
// `notes` must have room for that many. A `max` of 0 or less, or a null `notes`, writes nothing
// and returns 0 -- the one case where a chord comes out empty.
//
// `key` is `Pattern::root`, a pitch class; only its pitch class is read, as core/scale.hpp's
// root is, so 0, 12 and 60 are the same key. A scale id this build does not have plays
// chromatically, which is core/scale.hpp's promise and not this module's.
//
// The notes come out strictly ascending, all in the scale, all in 0..127, and no two the same.
// Three things can make a chord thinner than its table entry, each documented where it happens
// in core/chord.cpp: a note above 127 is dropped rather than clamped, a note the key cannot
// separate from the one below it is dropped rather than doubled, and `max` cuts the stack.
//
// `max` cuts the stack BEFORE folding and inverting, not the finished voicing. A caller with
// three voices playing MIN11 therefore hears a minor triad, inverted, and not three notes from
// the middle of an eleventh chord: the voices you have decide how much of the chord you get,
// and the low intervals are the ones that make it that chord.
int chord_expand(uint8_t root, uint8_t packed, uint8_t key, int scale, uint8_t *notes,
                 int max = kChordMaxNotes);

// --- the two halves of it -------------------------------------------------
//
// Exposed because each has a property worth checking on its own, and because one branch of the
// fold cannot be reached through `chord_expand` at all. Two of a chord's intervals land on one
// pitch in exactly eight (scale, key, root) cases across the whole table -- SUS4 at the top of
// the MIDI range in harmonic minor and the three pentatonic-ish scales, where the fourth and
// the fifth both fold onto the highest note the key has -- and in every one of them the note
// below is already as high as the scale goes, so the repair can only drop. The LIFT is headroom
// for a sparser scale added later, the way `scale_fold`'s six-step search is, and an untested
// branch is a broken branch the day that scale arrives. A test drives it with a stack of its
// own; core/mutate.hpp exposes `mutate_draw` for the same kind of reason.

// Stack `semitones` on the folded `root`, folding each note into the key. Returns how many
// notes were written, ascending, at most min(`count`, `max`, `kChordMaxNotes`).
//
// A folded note that lands on or below the note before it is lifted by whole scale steps until
// it clears it, and dropped if it cannot: folding two intervals onto one pitch would spend two
// of six voices on one note at twice the level, which is not a chord and is not something the
// audio side can notice.
int chord_fold(const int8_t *semitones, int count, uint8_t root, uint8_t key, int scale,
               uint8_t *notes, int max);

// Invert `notes` in place: the lowest note goes up an octave, `inversion` times, keeping the
// array ascending. Returns how many of those rotations actually happened.
//
// Fewer than asked means the voicing could not go higher -- the octave up would leave the MIDI
// range, or it is already in the chord (OCT's own first inversion, where rotating would double
// a note). The note COUNT and the multiset of pitch classes never change, which is what
// separates an inversion from everything else in this file.
int chord_invert(uint8_t *notes, int count, int inversion);

}  // namespace strata
