// Two mod slots per track: a source moving one parameter by a signed depth (backlog Phase 5,
// "two learnable mod slots", D15's follow-up; docs/CONTROLS.md "MOD slots").
//
// A slot is three bytes in the kit, beside LFO TRIG and for the same reason: the 32 bytes a
// track has are all spoken for and `locked_` is a 32-bit mask, so a slot is not a parameter. No
// lock, scene, morph or macro moves it; it says how the track's sound responds, like SLC AUTO.
// KIT.BIN carries the slots in an optional section 10; a kit from before it loads every slot OFF.
//
// Set by learning, as the macros are (D11): on the LFO page push C (slot 1) or D (slot 2), push
// again to walk the source, then turn the knob that should move. Where that knob was left, less
// where it started, is the depth, and the knob goes back to its start.
//
// Sources, and what 1.0 means for each:
//
//     LFO  the track's LFO as it runs, whatever its own DEP and DST say (DEP 0 makes it a
//          source only). Bipolar: depth either side of the knob.
//     ENV  the LFO's shape and RAT run once from each note (dsp/lfo.hpp's ONE-SHOT shapes),
//          whatever LFO TRIG says: SAW a decay, TRI an attack and decay, RMP a rise.
//     VEL  the note's velocity, 0-127 as 0..1: the full depth at 127.
//     PRS  the track pad's pressure while it is held (KEYS: any piano pad's, on the selected
//          track), 0..1; back to 0 when the pad is let go.
//     RND  a random value per note, -1..1, held for the note.
//
// Destinations are the parameters a machine plays: the 16 sound parameters, ASR and OSC. Not
// the LFO's (it would modulate its own source) nor the arp's (read when a note is scheduled,
// not by the sound). A cutoff stops at the same ~100 Hz floor as the LFO, the macros and the
// morph (D9): a slot can dip a filter but never shut a track the knob left open.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kModSlots = 2;

// The source byte, as KIT.BIN stores it: PERMANENT. Anything past RND reads as OFF.
constexpr uint8_t kModSrcOff = 0;
constexpr uint8_t kModSrcLfo = 1;
constexpr uint8_t kModSrcEnv = 2;
constexpr uint8_t kModSrcVel = 3;
constexpr uint8_t kModSrcPrs = 4;
constexpr uint8_t kModSrcRnd = 5;
constexpr uint8_t kModSrcCount = 6;

// Depth is in counts of the destination's 0-127 byte at a source value of 1.0.
constexpr int kModDepthMax = 127;

struct KitModSlot {
    uint8_t source = kModSrcOff;
    uint8_t dest = 0;            // a track parameter index (dsp/track.hpp numbering)
    int8_t depth = 0;            // -kModDepthMax .. +kModDepthMax

    bool on() const { return source != kModSrcOff && depth != 0; }
};

// "OFF" "LFO" "ENV" "VEL" "PRS" "RND": three characters each, for the screen.
const char *mod_source_name(uint8_t source);

// The next source a push walks to: LFO ENV VEL PRS RND OFF, and round.
uint8_t mod_source_next(uint8_t source);

// Whether a parameter index can be a slot's destination (see above).
bool mod_dest_allowed(int param);

// A slot as read from a file, made safe: an unknown source or a destination a slot cannot have
// is OFF, and -128 is -127.
KitModSlot mod_slot_sanitized(uint8_t source, uint8_t dest, uint8_t depth_byte);

// The depth a learn leaves: where the knob ended less where it started.
constexpr int8_t mod_depth_from_turn(uint8_t start, uint8_t end) {
    return int8_t(int(end > 127 ? 127 : end) - int(start > 127 ? 127 : start));
}

// The destination byte moved by a slot: `base` plus `value` x `depth` counts, rounded half away
// from zero and held to 0-127. Never below `floor`, or below `base` where the knob itself sits
// under the floor -- the floor limits the slot, not the knob (as dsp/lfo.hpp's lfo_apply).
uint8_t mod_apply(uint8_t base, float value, int8_t depth, uint8_t floor = 0);

// The same offset unrounded, in semitones of TUN (24/64 a count): how far a slot on a synth's
// TUN bends its voices, so a slot glides there as the LFO does.
float mod_pitch_semis(float value, int8_t depth);

// VEL's value for a note velocity.
constexpr float mod_velocity_value(uint8_t velocity) {
    return float(velocity > 127 ? 127 : velocity) * (1.0f / 127.0f);
}

}  // namespace strata
