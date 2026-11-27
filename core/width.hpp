// WID: the stereo width of a POLY track (KitTrack::width), as numbers the machines play.
//
// One byte in zones, as OS2's (dsp/va.cpp) is, so one knob on MIXER's WID 1-4 / 5-8 reaches both
// halves of what the genre review asked for -- a chord spread across the field, and unison:
//
//     0        mono. The machine renders one channel and the track is exactly what it was
//     1-63     SPREAD: each voice slot sits at its own place in the field (kWidthFan), as far
//              out as the knob says. Slot 0 is the middle, and a single note always takes slot
//              0 (dsp/vapoly.hpp's allocator), so a lone note stays centred and a chord fans out
//     64-127   UNISON: every note takes two voices, one panned hard left and one hard right,
//              tuned apart by kUnisonLowCents at 64 up to kUnisonHighCents at 127. A machine of
//              six voices then holds three notes, and each pair counts two against the shared
//              POLY budget (Engine::kPolyVoiceBudget), which is what bounds its cost
//
// Here rather than in dsp/ so it is host-tested without Enaga (core/ never includes it); the pan
// LAW, which needs a cosine, is dsp/polypool.hpp's.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kWidthSlots = 6;   // PolyMachine::kVoices, which core/ cannot see

// Where each voice slot sits at full spread, -1 left ... +1 right. Slot 0 in the middle; then
// the next notes of a chord alternate outward and back in, as VA POLY's DET detunes them
// (dsp/vapoly.cpp, kDetune), so a triad is centre, left, right.
constexpr float kWidthFan[kWidthSlots] = {0.0f, -1.0f, 1.0f, -0.6f, 0.6f, -0.3f};

// Unison's detune, the whole gap between the two voices of a note, in cents.
constexpr float kUnisonLowCents = 6.0f;
constexpr float kUnisonHighCents = 40.0f;

constexpr bool width_unison(uint8_t width) { return width >= 64 && width <= 127; }

// SPREAD's amount, 0-1: 0 mono, 1 at 63 and in all of unison (where the pair is hard left and
// right whatever the knob).
constexpr float width_spread(uint8_t width) {
    return width == 0 ? 0.0f : (width >= 63 ? 1.0f : float(width) * (1.0f / 63.0f));
}

// Where voice slot `slot` sits in SPREAD, -1 ... +1. 0 outside it.
constexpr float width_slot_pan(uint8_t width, int slot) {
    return width_unison(width) || slot < 0 || slot >= kWidthSlots
               ? 0.0f
               : width_spread(width) * kWidthFan[slot];
}

// Half the unison gap, in semitones: what each voice of a pair is tuned away from the note
// (the left one flat, the right one sharp). 0 outside unison.
constexpr float width_unison_semis(uint8_t width) {
    return width_unison(width)
               ? (kUnisonLowCents + (kUnisonHighCents - kUnisonLowCents) * float(width - 64) *
                                        (1.0f / 63.0f)) *
                     (0.5f / 100.0f)
               : 0.0f;
}

// Whether a track with this width renders in stereo.
constexpr bool width_stereo(uint8_t width) { return width != 0 && width <= 127; }

}  // namespace strata
