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
// The most notes one cycle can hold: up to 8 held keys over 4 octaves. A chord is at most 6.
constexpr int kArpMaxInput = 8;
constexpr int kArpMaxCycle = 2 * kArpMaxInput * kArpMaxOctaves;

struct ArpSettings {
    ArpMode mode = ArpMode::off;
    uint8_t rate_ticks = 96;      // the hit spacing at 96 PPQN
    uint8_t octaves = 1;          // 1-4
    uint8_t gate_percent = 50;    // of the hit spacing, 50-99

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
// Returns its length, 0 for no notes or OFF. `out` holds kArpMaxCycle.
int arp_cycle(const uint8_t *notes, int count, const ArpSettings &settings, uint8_t *out);

// The note of hit `hit` (0 = the first) of a cycle of `length`. RND picks from the cycle with a
// hash of `seed` and `hit`, so a pattern's arp is the same every pass; the rest walk it in order.
uint8_t arp_note(const uint8_t *cycle, int length, ArpMode mode, uint32_t hit, uint32_t seed);

}  // namespace strata
