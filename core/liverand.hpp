// LIVE RND: random step playback, turned in live from PERFORM.
//
// CyDrums' P1 (backlog Phase 5, "random step playback per track"; the controls concept puts the
// setting on the pattern and one knob gesture in PERFORM). The pattern half is TRACK PLAY's
// RND1-RND16 (core/timing.hpp, PlayMode). This is the live half, in the Moves layer beside the
// macros, the morph and REL/LEN ALL: one unit for every ARMED track at once, which overrides
// that track's own play mode while it is on.
//
//   FUNC + turn D in PERFORM   the unit: OFF, 1, 2, 4, 8, 16 steps, stopping at both ends
//   FUNC + push D in PERFORM   the selected track in or out of the armed set (all eight at start)
//
// A unit of n cuts each pass into blocks of n slots and plays a random block of n steps in each
// (PlayMode::rnd1 .. rnd16 exactly: same draw, same seed). RND 1 scatters single steps, RND 4 at
// 1x shuffles beats, RND 16 on a 64-step track shuffles bars.
//
// **Recoverable by construction**: the pattern is never written. OFF is the programmed order to
// the step, at once, because the play order is a pure function of the slot and the pass; a block
// switched in mid-way keeps its place inside the block, so the jump lands on the grid. Not saved
// and not undone; a LOCK holds the unit and the armed set (D4), like REL ALL and LEN ALL.
//
// The draw is the RND play modes' (a hash of the pattern's seed, the track, the block and the
// pass): every emulator run and every reload plays the same, and each pass differs from the last.
#pragma once

#include "pattern.hpp"  // kTracks
#include "timing.hpp"   // PlayMode

#include <cstdint>

namespace strata {

// The unit as stored: 0 OFF, 1..5 the blocks of 1, 2, 4, 8 and 16 steps.
constexpr uint8_t kLiveRndOff = 0;
constexpr uint8_t kLiveRndMax = 5;
constexpr uint8_t kLiveRndAllTracks = 0xFF;   // the armed set at start: every track

// The unit `clicks` encoder clicks from `current`, one unit a click, held to OFF .. 16. A
// damaged byte counts as OFF.
uint8_t live_rnd_turn(uint8_t current, int clicks);

// The play mode track `track` plays under `unit` and the armed set `armed` (bit t = track t):
// the unit's RND mode when the unit is on and the track is armed, the pattern's own byte
// otherwise (FWD for a byte this build does not know). The one place the override is decided:
// the scheduler and the recorder both ask it, so a hit recorded under LIVE RND lands on the
// step that was sounding.
PlayMode live_play_mode(uint8_t pattern_play, uint8_t unit, uint8_t armed, int track);

// For the screen: "OFF", "RND1" .. "RND16".
const char *live_rnd_name(uint8_t unit);

// How many tracks `armed` holds, 0..8.
int live_rnd_count(uint8_t armed);

}  // namespace strata
