// VariSpeed: the whole unit faster or slower like a turntable, tempo and pitch together.
//
// CyDrums' P7 (backlog, After 1.0 > Performance). FUNC + turn VALUE, on every screen: VALUE alone
// is the tempo, so the same knob with FUNC is the tempo with the pitch tied to it, the way a
// record's speed control moves both (docs/CONTROLS.md, "VariSpeed"). One signed amount in tenths
// of a semitone, -kVariRange .. +kVariRange (an octave each way), and **0 is rest**: at 0 the
// tempo is the project's and every machine plays at its own pitch, to the bit.
//
// What it moves, both by the same ratio 2^(tenths / 120):
//
//   the clock   Enaga's tempo is the project's tempo times the ratio. The project keeps its own
//               tempo, so a save while VariSpeed is up stores the tempo the song was written at,
//               and VALUE still turns that tempo (the clock follows, times the ratio). The
//               tempo-synced delay follows the clock, so its echoes stay on the grid.
//   the pitch   every audio machine: the synths (VA, WAVE, FM4) by the same semitones the LFO
//               on TUN adds, held notes included; the drums, SAMPLE and GRAIN by their read rate
//               or base frequency, ringing hits included. Envelope times do not move, and MID's
//               notes leave the unit as they are.
//
// **Internal clock only** (Enaga owns the clock, CLAUDE.md "Platform"): under MIDI or SYNC the
// turn is refused with `EXT CLOCK`, and VariSpeed drops to 0 when the source changes, so the
// pitch never leaves the tempo behind. The tempo stays inside Enaga's 20-300 BPM: a turn that
// would take it past stops where it still fits (`vari_fit`).
//
// Not saved, not undone, not in a LOCK (the tempo is not either). One tenth a click, and 0 is a
// detent: a turn that would cross it stops on it, as REL ALL's does. FUNC + tap VALUE with no
// effect picked snaps it back to 0.
//
// core/: no Enaga headers, no libm, no allocation. The ratio is two small tables, the same on
// the host and the C674x.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kVariRange = 120;           // tenths of a semitone: an octave
constexpr int kVariTextMax = 6;           // "+12.0" and the terminator

// The amount after `clicks` encoder clicks from `current`: one tenth a click, held to
// -kVariRange .. +kVariRange, stopping on 0 when the turn would cross it.
int8_t vari_turn(int8_t current, int clicks);

// 2^(tenths / 120): the speed and pitch ratio. Exactly 1 at 0; held to the range.
float vari_ratio(int8_t tenths);

// The semitones the synths add, tenths / 10. Exactly 0 at 0.
float vari_semis(int8_t tenths);

// `tenths`, or the nearest amount towards 0 at which the project's tempo `base_centi` (centi-BPM)
// times the ratio stays inside min_centi .. max_centi. 0 always fits a base inside the range.
int8_t vari_fit(int8_t tenths, uint32_t base_centi, uint32_t min_centi, uint32_t max_centi);

// The tempo the clock runs at, in tenths of a BPM, rounded: `base_tenths` times the ratio.
uint32_t vari_tempo_tenths(uint32_t base_tenths, int8_t tenths);

// The amount as the screen says it, in semitones: "0", "+1.2", "-12.0". At most kVariTextMax
// bytes, terminator included.
void vari_text(int8_t tenths, char *out, int max);

}  // namespace strata
