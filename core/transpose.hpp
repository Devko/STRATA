// TRANSPOSE: the melodic tracks moved through the key (or by semitones), live, from any screen.
//
// Backlog (After 1.0 > Performance, "live transpose of the melodic tracks, held in a LOCK").
// docs/CONTROLS.md "TRANSPOSE" has the gestures:
//
//   FUNC + SEL<   one step down     FUNC + SEL>   one step up
//   FUNC + both screen keys         back to 0
//
// held to -12 .. +12, on every screen. A step is a SCALE STEP by default and a semitone with the
// FUNC menu's TRANSPOSE row on SEMI (Roland, 2026-11-28: "in scale, with an option in the menu
// to make it semitones"). The row is not saved, like CLICK and FX GLIDE: every start is SCALE.
// The screen keys with FUNC did nothing before (the help turned its pages on them, and turns them
// on LEFT/RIGHT alone now), so no chord changed meaning.
//
// **Which tracks move: exactly the ones that fold into the scale** (`machine_folds_note`,
// core/project.hpp): the two VAs, the two WAVEs, FM4, GRAIN, and SAMPLE while it is unsliced.
// One rule for both, Roland's (2026-11-22): a sliced SAMPLE's note picks the slice, a drum's note
// is its tuning and MID's goes to someone else's instrument, maybe a drum machine. So the beat,
// the chopped loops and the outboard gear stay where they are and the harmony moves.
//
// The shift is applied AFTER the fold, to every note a track hands its machine: a step's, a
// chord's, an arp's, a pad's, a KEYS key's, a roll's.
//
// **SCALE (the default) stays in the key.** Each note walks n steps of the project's scale
// (`scale_step` on the folded note), so a line moves the way a progression does in that key:
// in A minor, a riff on A moved -2 plays from F, every note still one of A minor's, and a
// chord's notes each walk n steps, so the triad on A minor's i becomes the one on its VI, major
// where the key makes it major. A note outside the key (a KEYS pad between the lit ones) keeps
// its distance from the in-key note it folds to. Under CHROMATIC a scale step is a semitone.
// The notice says `TRANSPOSE +2 IN KEY`.
//
// **SEMI moves the key with it.** Folding into the key and then moving by n semitones is the
// same as moving by n and folding into the key n higher (fold(x, root) + n == fold(x + n,
// root + n)), so the part keeps its shape and its chords keep their colour: A minor +3 plays C
// minor. The notice names that key in sharps (`TRANSPOSE +3 C`).
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

constexpr int kTransposeRange = 12;      // steps each way: an octave of semitones

// The transpose after `clicks` from `current`, a step a click, held to the range. A damaged
// byte comes back inside it.
int8_t transpose_turn(int8_t current, int clicks);

// Does a track with this machine (and, for SAMPLE, this slice count) move? The scale fold's
// rule, word for word.
bool transpose_moves(uint8_t machine, int slices);

// `note` moved by `semis`, held to 0..127 (an edge note sits on the edge rather than wrapping
// round to the other end, as range_transpose does).
uint8_t transpose_note(uint8_t note, int semis);

// `note` moved `steps` steps of the scale rooted at `root`: `scale_step` for a note in the key,
// and for one outside it the in-key note it folds to, moved, plus the same distance. Saturates
// at the MIDI range's ends as `scale_step` does. 0 steps is the note itself.
uint8_t transpose_in_key(uint8_t note, int steps, uint8_t root, int scale);

// What TRANSPOSE does to a note on a track that moves: `amount` scale steps, or semitones when
// `semitones` is set.
inline uint8_t transpose_apply(uint8_t note, int amount, bool semitones, uint8_t root, int scale) {
    return semitones ? transpose_note(note, amount) : transpose_in_key(note, amount, root, scale);
}

// The key root the transposed parts are in: `root` (a pitch class) moved by `semis`, 0..11.
uint8_t transpose_root(uint8_t root, int semis);

// For the notice: in semitones the key the parts are in, "TRANSPOSE +3 C", "TRANSPOSE -1 G#";
// in scale steps "TRANSPOSE +2 IN KEY"; at rest "TRANSPOSE 0". At most kTransposeTextMax bytes,
// terminator included.
constexpr int kTransposeTextMax = 21;
void transpose_text(int8_t amount, bool semitones, uint8_t root, char *out, int max);

// The short form for PERFORM's status line and the live strip: "TR+3", "TR-12"; "" at rest.
constexpr int kTransposeShortMax = 6;
void transpose_short(int8_t semis, char *out, int max);

}  // namespace strata
