// Keys and scales. core/scale.hpp has the table and the reasoning.
#include "scale.hpp"

namespace strata {
namespace {

constexpr uint8_t kNoteMax = 127;

// The pitch class of `note` relative to `root`, 0..11. Only the root's pitch class matters, so
// root 60 and root 0 are the same key -- which is why this reduces both.
int interval_of(uint8_t note, uint8_t root) {
    // The +132 is what keeps this non-negative: the smallest `note - root` is 0 - 127, and
    // -127 + 132 is still positive. That matters because a negative operand to % in C++ gives a
    // negative result, which would index outside the mask.
    //
    // Reducing the root is redundant -- `note - root` and `note - root % 12` are congruent mod
    // 12 -- and is kept because it states the intent: only the root's pitch class is the key.
    return (int(note) - int(root % 12) + 12 * 11) % 12;
}

bool in_mask(uint16_t mask, int interval) { return (mask >> interval & 1u) != 0; }

}  // namespace

const char *scale_name(int scale) {
    switch (Scale(scale)) {
        case Scale::chromatic:        return "CHROMATIC";
        case Scale::minor:            return "MINOR";
        case Scale::major:            return "MAJOR";
        case Scale::dorian:           return "DORIAN";
        case Scale::phrygian:         return "PHRYGIAN";
        case Scale::lydian:           return "LYDIAN";
        case Scale::mixolydian:       return "MIXOLYDIAN";
        case Scale::locrian:          return "LOCRIAN";
        case Scale::harmonic_minor:   return "HARM MINOR";
        case Scale::melodic_minor:    return "MEL MINOR";
        case Scale::minor_pentatonic: return "MINOR PENT";
        case Scale::major_pentatonic: return "MAJOR PENT";
        case Scale::blues:            return "BLUES";
        case Scale::whole_tone:       return "WHOLE TONE";
        case Scale::diminished:       return "DIMINISHED";
        default:                      return "?";
    }
}

const char *note_name(uint8_t note) {
    switch (note % 12) {
        case 0:  return "C";
        case 1:  return "C#";
        case 2:  return "D";
        case 3:  return "D#";
        case 4:  return "E";
        case 5:  return "F";
        case 6:  return "F#";
        case 7:  return "G";
        case 8:  return "G#";
        case 9:  return "A";
        case 10: return "A#";
        default: return "B";
    }
}

int scale_size(int scale) {
    const uint16_t mask = scale_mask(scale);
    int n = 0;
    for (int i = 0; i < 12; i++) {
        if (in_mask(mask, i)) n++;
    }
    return n;
}

bool scale_contains(uint8_t note, uint8_t root, int scale) {
    return in_mask(scale_mask(scale), interval_of(note, root));
}

uint8_t scale_fold(uint8_t note, uint8_t root, int scale) {
    const uint16_t mask = scale_mask(scale);
    if (in_mask(mask, interval_of(note, root))) return note;

    // Outwards from the note, a semitone at a time, taking the lower of the two on a tie. Six
    // steps is enough for ANY scale: the sparsest possible one has only its root, and six up or
    // down reaches it from anywhere in the octave.
    //
    // No scale in the current table needs more than one step -- the widest gap in any of them is
    // three semitones, so the worst note sits one away from a scale note. The bound is headroom
    // for a scale added later, and `scale_folding_is_never_further_than_half_a_gap` is what
    // would catch one this loop could not reach.
    //
    // Each direction also has to stay inside 0..127, and they are tested separately: near the
    // ends of the range one direction runs out and the other must still be taken, which is why
    // this is not a single symmetric loop.
    for (int distance = 1; distance <= 6; distance++) {
        const int down = int(note) - distance;
        if (down >= 0 && in_mask(mask, interval_of(uint8_t(down), root))) return uint8_t(down);
        const int up = int(note) + distance;
        if (up <= int(kNoteMax) && in_mask(mask, interval_of(uint8_t(up), root))) {
            return uint8_t(up);
        }
    }
    // Unreachable for any scale with a bit set, and `scale_mask` never returns 0: an unknown id
    // gives chromatic. Returning the note is the harmless answer if that ever changes.
    return note;
}

int scale_degree_of(uint8_t note, uint8_t root, int scale) {
    const uint16_t mask = scale_mask(scale);
    const int interval = interval_of(note, root);
    if (!in_mask(mask, interval)) return -1;
    int degree = 0;
    for (int i = 0; i < interval; i++) {
        if (in_mask(mask, i)) degree++;
    }
    return degree;
}

uint8_t scale_step(uint8_t note, uint8_t root, int scale, int degrees) {
    const uint16_t mask = scale_mask(scale);
    uint8_t at = scale_fold(note, root, scale);
    if (degrees == 0) return at;

    // One scale note at a time rather than arithmetic on the degree, because the gaps are
    // uneven: in the minor pentatonic a degree is three semitones in one place and two in
    // another, and in the diminished scale it alternates. Walking the mask cannot get that
    // wrong, and the most it ever costs is 12 iterations per degree.
    const int direction = degrees > 0 ? 1 : -1;
    int left = degrees > 0 ? degrees : -degrees;
    while (left > 0) {
        int next = int(at);
        bool found = false;
        // At most 12 semitones to the next note of the scale, whatever the scale.
        for (int tries = 0; tries < 12; tries++) {
            next += direction;
            if (next < 0 || next > int(kNoteMax)) break;      // the range ran out
            if (in_mask(mask, interval_of(uint8_t(next), root))) {
                found = true;
                break;
            }
        }
        // Saturating, not wrapping: a line walked off the bottom sits on the lowest note it can
        // play rather than reappearing at the top, which would be a jump of ten octaves in the
        // middle of a bar.
        if (!found) return at;
        at = uint8_t(next);
        left--;
    }
    return at;
}

}  // namespace strata
