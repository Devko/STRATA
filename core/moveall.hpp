// REL ALL and LEN ALL: one hand on every track's release and every note's length at once.
//
// CyDrums' P4 (backlog Phase 5 item 5). Two live controls in the Moves layer, beside the macros
// and the morph: FUNC + turn A in PERFORM is REL ALL, FUNC + turn B is LEN ALL (docs/CONTROLS.md,
// "REL ALL and LEN ALL"). Each is a signed amount, -kAllRange .. +kAllRange, and **0 is rest**:
// at 0 the sound is the programmed sound to the bit, the rule the macros and the morph follow.
// Neither is saved or undone; a LOCK holds both, as it holds the macro positions.
//
//   REL ALL  added to every track's release bytes: the AMP page's DEC on every machine, and the
//            ASR page's REL on the machines that read it (the VAs, WAVE, FM4, GRAIN). The bytes
//            map to time exponentially on every machine, so a count is a ratio, not a time:
//            -64 is as short as each track goes, +64 a long way out, and each track keeps its
//            own character relative to the others. A step's p-lock on a release byte is moved
//            too: the control is "relative to their own values", and the step's value is the
//            one it plays.
//   LEN ALL  every sequenced note's gate (Trig::gate_frames) times 2^(amount / 32): x0.25 at
//            -64, x4 at +64, an octave of length every 32. A note with no gate (the drums, an
//            arp, a held key) is untouched; REL ALL is the drums' control.
//
// Turned by two counts a click, like the macros, and **0 is a detent**: a turn that would cross
// it stops on it, so the hand finds rest without looking.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kAllRange = 64;
constexpr int kAllStep = 2;              // counts a click

// The amount after `clicks` encoder clicks from `current`: kAllStep counts a click, held to
// -kAllRange .. +kAllRange, and stopping on 0 when the turn would cross it.
int8_t all_turn(int8_t current, int clicks);

// A release byte moved by REL ALL, held to 0-127. `amount` 0 returns `value` unchanged.
uint8_t rel_all_byte(uint8_t value, int8_t amount);

// The release bytes a machine reads (track parameter indices, dsp/track.hpp numbering): the AMP
// page's DEC, then ASR's REL where the machine reads ASR. Writes at most two into `out` and
// returns how many; 0 for MID and an unknown id.
int machine_release_params(uint8_t machine, uint8_t out[2]);

// A gate in frames scaled by LEN ALL. `amount` 0, or a gate of 0 (no gate), returns `frames`
// unchanged; any other gate stays at least one frame, so a note never becomes "no gate".
// Integer: a Q16 table of 2^(k / 32) and shifts, so it is the same on the host and the C674x.
uint32_t len_all_frames(uint32_t frames, int8_t amount);

// LEN ALL as the screen says it: the note length in percent of the programmed one, rounded
// (25 .. 400; 100 at rest).
int len_all_percent(int8_t amount);

}  // namespace strata
