// The arpeggiator's note logic: the four ARP bytes as settings, the cycle of notes an arp walks,
// and which note of it plays on each hit. No timing and no Enaga here (core/ never includes it):
// the engine schedules the hits on the clock's ticks, exactly as it does steps.
//
// The four bytes are track parameters 20-23 (dsp/track.hpp's ARP page), so the kit, step locks,
// scenes, the morph and macros all reach them. Zeroed bytes are OFF, which is what makes an
// older kit, and every factory sound, play exactly as before.
#pragma once

#include <cstdint>

namespace strata {

enum class ArpMode : uint8_t { off = 0, up, down, up_down, order, random, count };

// MOD, RAT, OCT, GATE.
constexpr int kArpBytes = 4;
constexpr int kArpRates = 7;
constexpr int kArpMaxOctaves = 4;
// The most notes one cycle can hold: up to 8 held keys over 4 octaves, there and back (U/D),
// and twice that for a pedal VAR, which puts the pedal note between every two. A chord is at
// most 6.
constexpr int kArpMaxInput = 8;
constexpr int kArpMaxWalk = 2 * kArpMaxInput * kArpMaxOctaves;
constexpr int kArpMaxCycle = 2 * kArpMaxWalk;
static_assert(kArpMaxCycle <= 255, "a cycle's length is a byte in the engine");

// ## The arp's kit settings: VAR, RHYTHM, ACCENT, RATCHET
//
// Four more choices per track that are NOT parameters, for LFO TRIG's reason (docs/HANDOFF.md):
// all 32 bytes a track has are spoken for and the lock mask is 32 bits, so no lock, scene, morph
// or macro reaches these. They are kit settings like TRIG and the mod slots -- `KitTrack::arp`,
// KIT.BIN's optional section 12 -- each an index into a fixed table in core/arp.cpp, pushed
// round on the ARP page (push MOD, RAT, OCT, GAT) or set all at once by an ARPS preset
// (core/arppresets.hpp). All four 0 is the arp exactly as it was before them, which is what a
// kit without the section loads.
//
//   VAR      reshapes MOD's walk: CONVERGE (the outside notes first, closing in), DIVERGE (from
//            the middle out), THUMB (the lowest note between every two) and PINKY (the highest).
//   RHYTHM   which hits of the arp's grid sound: ALL, or a cycle of up to 16 slots with rests.
//            A rest does NOT advance the note, so a 3-slot rhythm over a 4-note cycle shifts
//            the melody against the bar: the polymetric roll of a techno arp.
//   ACCENT   which hits are accented (the step's ACC: each machine's accent, 127 to MIDI), on
//            the same grid of slots, or CYC: the first note of every cycle.
//   RATCHET  which hits repeat inside their own slot, 2-4 times, on a fixed slot or by chance.
//
// The values are what KIT.BIN stores: PERMANENT. New entries go on the END of a table. Anything
// past a table's end reads as 0.
struct KitArp {
    uint8_t var = 0;
    uint8_t rhythm = 0;
    uint8_t accent = 0;
    uint8_t ratchet = 0;
};

enum class ArpVar : uint8_t { off = 0, converge, diverge, thumb, pinky, count };

constexpr int kArpVars = int(ArpVar::count);
constexpr int kArpRhythms = 12;
constexpr int kArpAccents = 8;
constexpr int kArpRatchets = 8;
// The four, in the order of the ARP page's knobs they are pushed on (MOD RAT OCT GAT).
constexpr int kArpExtFields = 4;
// The most hits one slot ratchets into.
constexpr int kArpMaxRatchet = 4;

// Each byte in its table's range: a file's damage reads as 0 (OFF, ALL).
KitArp kit_arp_sanitized(KitArp arp);

// The four as one indexed field, for the ARP page's pushes: 0 VAR, 1 RHYTHM, 2 ACCENT, 3 RATCHET.
int arp_ext_count(int field);
uint8_t arp_ext_get(const KitArp &arp, int field);
void arp_ext_set(KitArp &arp, int field, uint8_t value);
// "VAR" "RHY" "ACC" "RAT" -- and the value's name, five characters at most (an out-of-range
// value reads as its table's 0, as a file's would).
const char *arp_ext_label(int field);
const char *arp_ext_name(int field, uint8_t value);

// Does slot `slot` (0 = the run's first) of RHYTHM `rhythm` sound?
bool arp_rhythm_plays(uint8_t rhythm, uint32_t slot);
// Is the hit on slot `slot`, playing note `note_hit` (0 = the run's first note) of a cycle of
// `length` notes, accented?
bool arp_accent_on(uint8_t accent, uint32_t slot, uint32_t note_hit, int length);
// How many hits slot `slot` plays: 1, or a ratchet's 2-4. The chance entries draw from a hash of
// `seed` and `slot`, so a pattern's arp ratchets the same way every pass, as RND picks the same
// notes.
int arp_ratchet_count(uint8_t ratchet, uint32_t slot, uint32_t seed);

struct ArpSettings {
    ArpMode mode = ArpMode::off;
    uint8_t rate_ticks = 96;      // the hit spacing at 96 PPQN
    uint8_t octaves = 1;          // 1-4
    uint8_t gate_percent = 50;    // of the hit spacing, 50-99
    ArpVar var = ArpVar::off;     // the kit's VAR, not a byte: `make` leaves it OFF

    bool on() const { return mode != ArpMode::off; }

    // From the four bytes, 0-127 each. Control rate: a multiply and a shift per byte, no divide.
    //   MOD   six zones: OFF UP DN U/D ORD RND
    //   RAT   seven zones, slow to fast: 1/4 1/8 1/8T 1/16 1/16T 1/32 1/32T
    //   OCT   four zones: 1-4 octaves
    //   GATE  50% at 0 up to 99% at 127, so a zeroed byte is a sensible half-length note
    static ArpSettings make(const uint8_t bytes[kArpBytes]);
};

// How many encoder counts one click is worth on byte `offset` (0 MOD ... 3 GATE), so one click
// on MOD, RAT or OCT is one zone: 22 x 6 zones, 19 x 7 and 32 x 4 each land in every zone from
// either end. GATE is a plain knob.
constexpr int arp_click(int offset) {
    return offset == 0 ? 22 : (offset == 1 ? 19 : (offset == 2 ? 32 : 1));
}

// The zone index a byte falls in, for the screen and for `make`.
int arp_mode_index(uint8_t byte);
int arp_rate_index(uint8_t byte);
int arp_octaves(uint8_t byte);
int arp_gate_percent(uint8_t byte);
uint8_t arp_rate_ticks(int index);

const char *arp_mode_name(uint8_t byte);    // "OFF" "UP" "DN" "U/D" "ORD" "RND"
const char *arp_rate_name(uint8_t byte);    // "1/4" ... "1/32T"
const char *arp_param_name(int offset);     // "MOD" "RAT" "OCT" "GAT"

// The cycle an arp walks: `notes` (count of them, in the order they were played or stacked),
// sorted for UP/DOWN/U-D/RND or as given for ORD, repeated an octave up per extra octave, with
// notes past 127 dropped. U/D goes up and back down without repeating the top or bottom note.
// Then VAR reshapes that walk (above). Returns its length, 0 for no notes or OFF. `out` holds
// kArpMaxCycle.
int arp_cycle(const uint8_t *notes, int count, const ArpSettings &settings, uint8_t *out);

// The note of hit `hit` (0 = the first) of a cycle of `length`. RND picks from the cycle with a
// hash of `seed` and `hit`, so a pattern's arp is the same every pass; the rest walk it in order.
uint8_t arp_note(const uint8_t *cycle, int length, ArpMode mode, uint32_t hit, uint32_t seed);

}  // namespace strata
