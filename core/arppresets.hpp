// Factory arps: ready-made settings for the ARP page, loaded into ONE track.
//
// A preset is the arp whole: the four ARP bytes (MOD RAT OCT GAT, parameters 20-23) and the four
// kit settings beside them (KitTrack::arp: VAR RHYTHM ACCENT RATCHET, core/arp.hpp). Opened with
// OK on EDIT's ARP page (app/ui_browsers.hpp, ARPS), the way OK on the sound pages opens SOUNDS,
// and with the same AS WAS row first, so trying arps can never lose the one the track had.
//
// What an arp preset leaves alone: every other parameter -- the sound, the mix and the LFO are
// SOUNDS's and the user's -- and the machine. Any machine takes one: the arp runs on every track
// (a KICK arp at 1/32 is a roll, a SAMPLE arp walks slices).
//
// Values are 0-127 bytes in the middle of their zone (core/arp.hpp's MOD RAT OCT zones), so a
// preset sits where one click from either side of it lands. Each preset's comment in
// core/arppresets.cpp says what it is for. Like core/sounds.hpp, this is content: the names and
// numbers are a sound choice, not a contract, and tests/test_arp.cpp only pins that each is
// valid, drawable and applies what it says.
#pragma once

#include "arp.hpp"

#include <cstdint>

namespace strata {

struct ArpPreset {
    const char *name;            // six characters at most, in the 5x7 font
    uint8_t bytes[kArpBytes];    // MOD RAT OCT GAT
    KitArp ext;                  // VAR RHYTHM ACCENT RATCHET
};

constexpr int kArpPresetNameChars = 6;

int arp_preset_count();

// Preset `index`, or nullptr outside 0 .. count - 1.
const ArpPreset *arp_preset(int index);

// Write preset `index` into a track's four ARP bytes and its kit settings. False, and nothing
// written, for a preset that does not exist.
bool arp_preset_apply(int index, uint8_t *arp_bytes, KitArp &ext);

// True when the four bytes and the kit settings are the preset's exactly: the list marks the
// arp a track has now.
bool arp_preset_matches(int index, const uint8_t *arp_bytes, const KitArp &ext);

}  // namespace strata
