// ENERGY: one knob from a sparse breakdown to the peak, turned in live from PERFORM.
//
// Roland's pick of the bold ideas (backlog, After 1.0 > Performance, 2026-11-27): one bipolar
// control, rest at the centre, in the Moves layer beside REL ALL, LEN ALL and LIVE RND.
//
//   FUNC + turn C in PERFORM   ENERGY, -64 .. +64, two counts a click, 0 a detent and rest
//
// **Down thins.** Every track drops its weakest hits first, and the tracks go in an order: the
// hats start at once, then the percussion, the snare and the samples, the bass and lead lines,
// the pads, and the kick last. At -64 the drums and the lines are gone and the pads and textures
// hold on to their strongest hits, which is a breakdown. A hit's strength is where it sits in the
// bar (GEN's metric weights, core/generate.hpp: the one, the beats, the 8ths, the 16ths), an
// accent counting for more, and a fixed draw that orders hits of the same weight.
//
// **Up fills.** An EMPTY step of a track that plays anything at all may get a ghost: a quiet hit
// with the note and the locks of the hit before it on that track, so a ghost on a tuned kick is
// the same kick and a ghost on a bass line repeats the note it follows. Its chance is GEN's
// metric weight times the amount times the track's appetite: the hats fill most, then the
// percussion, the samples, the snare and the lines; the kick only near the top; pads, textures
// and MID tracks never (a chord made up from nothing, or a note sent to someone else's
// instrument, is not a fill). Up also lifts a step's probability towards 100, so a 50% hat plays
// more often as the energy climbs.
//
// **Recoverable by construction**: the pattern is never written, and at 0 the scheduler takes no
// branch of this at all, so rest is the programmed part to the bit. Each draw is a hash of the
// pattern's seed, the track and the step and NOT of the pass: a given position of the knob plays
// the same groove every bar, the next click down drops exactly one more set of hits (never a
// different set), and turning back brings each one back where it was. Not saved and not undone;
// a LOCK holds it, as it holds REL ALL.
//
// What is never touched: trigless steps (their locks are the sweep someone programmed),
// conditions and the PRE/NEI chain (a thinned hit still counts as a pass: thinning comes after
// the probability stage, as a failed roll does), ratchets (a kept step keeps its roll, a thinned
// one loses it whole) and the recorder.
//
// Everything here is integer: multiplies and shifts, no divide, so it is the same on the host
// and on the C674x.
#pragma once

#include "pattern.hpp"   // kTracks

#include <cstdint>

namespace strata {

constexpr int kEnergyRange = 64;     // the same span and feel as REL ALL (core/moveall.hpp)
constexpr int kEnergyStep = 2;       // counts a click

// Where a machine sits in the order. The engine looks it up per track (`energy_tier`) and hands
// it to the scheduler in PerformState, so core/ needs no machine pointers.
enum class EnergyTier : uint8_t {
    hats = 0,    // HAT: thins first, fills most
    perc,        // PERC
    mid,         // SNARE, SAMPLE: the backbeat and the loops
    line,        // VA MONO, WAVE MONO: bass and lead lines
    pad,         // VA POLY, WAVE POLY, FM4, GRAIN: thin late and only half way, never fill
    kick,        // KICK: thins last, fills only near the top
    midi,        // MID: thins with the snare, never fills
    count
};

// The tier of a machine id (core/project.hpp, MachineId). An id this build does not have is
// `mid`: it plays nothing anyway.
EnergyTier energy_tier(uint8_t machine);

// The amount after `clicks` encoder clicks from `current`: kEnergyStep counts a click, held to
// -kEnergyRange .. +kEnergyRange, stopping on 0 when a turn would cross it.
int8_t energy_turn(int8_t current, int clicks);

// How far down a track of `tier` has gone at `energy`, 0 (nothing thinned) .. 100 (every hit
// gone). 0 for any energy >= 0. Each tier starts at its own point and reaches its own ceiling.
int energy_cut(int8_t energy, EnergyTier tier);

// A step's strength, 0..99: its metric weight band (the one 75-99, a beat 50-74, an 8th 25-49, a
// 16th 0-24), a fixed draw inside the band, and +15 for an accent (at most 99).
int energy_strength(uint32_t seed, int track, int step, bool accent);

// Does the trig on `step` play at `energy`? Always true at energy >= 0.
bool energy_keeps(int8_t energy, EnergyTier tier, uint32_t seed, int track, int step,
                  bool accent);

// The probability a step plays with at `energy`: `probability` lifted towards 100 by the amount
// above rest (100 at +64), unchanged at or below rest. 0 stays 0: a step someone switched off
// with 0% is off.
uint8_t energy_probability(uint8_t probability, int8_t energy);

// The chance in 100 that EMPTY step `step` of a track of `tier` gets a ghost at `energy`. 0 at
// energy <= 0, and for the tiers that never fill.
int energy_ghost_chance(int8_t energy, EnergyTier tier, int step);

// Whether empty step `step` gets a ghost, and how hard: true with `velocity` set (40 .. 100,
// louder as the energy climbs; a line's ghost sits higher, 70 .. 110, for the reason GEN's
// melodic parts carry no ghosts). The draw is fixed per step, like the thinning's.
bool energy_ghost(int8_t energy, EnergyTier tier, uint32_t seed, int track, int step,
                  uint8_t &velocity);

// The step a ghost on `step` borrows its note, length and locks from: the nearest trig before it
// on the track, wrapping round the track's `length`. -1 when the track has no trig at all, and a
// track that plays nothing gets no ghost.
int energy_source_step(const TrackPattern &track, int length, int step);

}  // namespace strata
