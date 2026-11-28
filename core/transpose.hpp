// TRANSPOSE: the melodic tracks moved by semitones, live, from any screen.
//
// Backlog (After 1.0 > Performance, "live transpose of the melodic tracks, held in a LOCK").
// docs/CONTROLS.md "TRANSPOSE" has the gestures:
//
//   FUNC + SEL<   one semitone down     FUNC + SEL>   one semitone up
//   FUNC + both screen keys             back to 0
//
// held to -12 .. +12, on every screen. The screen keys with FUNC did nothing before (the help
// turned its pages on them, and turns them on LEFT/RIGHT alone now), so no chord changed meaning.
//
// **Which tracks move: exactly the ones that fold into the scale** (`machine_folds_note`,
// core/project.hpp): the two VAs, the two WAVEs, FM4, GRAIN, and SAMPLE while it is unsliced.
// One rule for both, Roland's (2026-11-22): a sliced SAMPLE's note picks the slice, a drum's note
// is its tuning and MID's goes to someone else's instrument, maybe a drum machine. So the beat,
// the chopped loops and the outboard gear stay where they are and the harmony moves.
//
// **The key moves with it.** The shift is applied AFTER the fold, to every note a track hands
// its machine: a step's, a chord's, an arp's, a pad's, a KEYS key's, a roll's. Folding into the
// key and then moving by n is the same as moving by n and folding into the key n higher
// (fold(x, root) + n == fold(x + n, root + n)), so the part keeps its shape and its chords keep
// their colour: A minor +3 plays C minor. The screen names the key it is in (`TRANSPOSE +3 C`),
// in sharps like every note name.
//
// **Recoverable by construction**: nothing is written. The pattern, the kit and the recorder
// keep the notes as programmed and as played; a KEYS key recorded while transposed is stored as
// the key, and plays back moved by the transpose of the moment, as it sounded. Not saved and not
// undone; a LOCK holds it, like ENERGY and REL ALL. A held note keeps the pitch it started at
// and is let go of at that pitch (the engine remembers the shift each held key took).
//
// Later, perhaps: a song section field (CHAIN's SECTION view) so a chain entry can set the
// transpose, as NRG sets ENERGY. Not built: one byte on the end of the section entry.
//
// core/: no Enaga headers, no allocation, integer only.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kTransposeRange = 12;      // semitones each way: an octave

// The transpose after `clicks` from `current`, a semitone a click, held to the range. A
// damaged byte comes back inside it.
int8_t transpose_turn(int8_t current, int clicks);

// Does a track with this machine (and, for SAMPLE, this slice count) move? The scale fold's
// rule, word for word.
bool transpose_moves(uint8_t machine, int slices);

// `note` moved by `semis`, held to 0..127 (an edge note sits on the edge rather than wrapping
// round to the other end, as range_transpose does).
uint8_t transpose_note(uint8_t note, int semis);

// The key root the transposed parts are in: `root` (a pitch class) moved by `semis`, 0..11.
uint8_t transpose_root(uint8_t root, int semis);

// For the notice and the screen: "TRANSPOSE +3 C", "TRANSPOSE -1 G#", and at rest "TRANSPOSE 0".
// At most kTransposeTextMax bytes, terminator included.
constexpr int kTransposeTextMax = 18;
void transpose_text(int8_t semis, uint8_t root, char *out, int max);

// The short form for PERFORM's status line and the live strip: "TR+3", "TR-12"; "" at rest.
constexpr int kTransposeShortMax = 6;
void transpose_short(int8_t semis, char *out, int max);

}  // namespace strata
