// GEN: a new part for one track, drum or melodic, written from the pattern's seed.
//
// Roland, 2026-11-27: "an option to generate pattern, like drum and melodic pattern? random and
// on scale". The Euclidean generator (core/mutate.hpp) writes a rhythm and nothing else; this
// writes a PART -- where the hits fall, how hard they are, and on a melodic track which notes of
// the key they play.
//
// ## The rhythm: metric weights, not coin flips
//
// Every step gets a chance from where it sits in the bar, scaled by DENSITY. A plain coin per
// step gives as many hits on the "a" of four as on the one, which sounds like noise rather than
// like a part; weighting the positions is what makes a random rhythm groove. Per 16-step bar:
//
//   the one                100     a hit unless DENSITY is very low
//   the other three beats   80
//   the off-beat 8ths       50
//   the 16ths between       25
//
// and the chance is `weight * density / 60`, capped at 100. At DENSITY 50 that is about six hits
// a bar, at 100 about eleven and at 20 two or three. The bar is sixteen STEPS whatever the
// track's speed, which is what the grid shows.
//
// On a drum track step 1 is always a hit once DENSITY reaches kGenDownbeatDensity, so a
// generated kick starts the pattern where a techno kick starts it. Below that DENSITY means what
// it says: a sparse part may start late.
//
// ## Velocity: accents and ghosts from the same weights
//
// A drum hit on the one is accented (110-127), on a beat strong (95-115), on an 8th medium
// (75-100) and on a 16th a ghost (40-70). A melodic hit varies less (80-120): a bass line with
// ghost notes in it sounds broken rather than played.
//
// ## The notes: a walk through the key
//
// A melodic part is a random walk in SCALE DEGREES of the pattern's key, between the root at the
// track's own octave and RANGE octaves above it, so every note is in the scale by construction
// and the part sits where the track already plays. LEAP is the chance in 100 that a move jumps
// two to five degrees instead of stepping one (or staying). A walk that would leave the range is
// reflected back into it rather than clamped, so it does not stick to the edges. On the one of
// each bar the walk lands on a root (of whichever octave it is nearest) more often than not:
// that is what makes a random line sound like it is in a key rather than merely of one.
//
// The octave comes from the track: the first trig's note, rooted down to the key's root. A track
// with no trigs starts from C3 (48), a bass octave, rather than from the middle C a fresh step
// holds.
//
// Gates are mostly one step, now and then half a step or two; LEAP also gives a chance of a
// slide into the next note, MONO machines' only (the flag does nothing elsewhere).
//
// ## WHAT
//
//   BOTH    a new rhythm, and on a melodic track new notes on it
//   RHYTHM  a new rhythm; a step that gains a hit takes its note from the part, as Euclid does
//   NOTES   the rhythm kept, new notes (and velocities) on the hits already there
//
// NOTES on a drum track has nothing to do and reports no change.
//
// ## What it leaves alone
//
// Exactly what `euclid_generate` leaves alone, for the same reason: locks, conditions, ratchets,
// micro-timing and probability. A step that loses its trig loses only the trig bit, so GEN then
// undo, or GEN over a part with its locks programmed, keeps the sound. A step whose note moves
// takes its chord with it (core/stepnotes.hpp).
//
// ## Seeded
//
// Every draw comes from the pattern seed, the track, the step, what is being drawn and a
// `variant` the caller counts, never from a running RNG (core/mutate.hpp says why). The same
// press on the same pattern gives the same part; the next press gives another one.
//
// Nothing here allocates. It runs in the audio context through the command ring, once per press:
// one pass over at most 64 steps, multiplies and shifts; the one modulo in the loop runs once a
// bar at most.
#pragma once

#include "pattern.hpp"

#include <cstdint>

namespace strata {

enum class GenWhat : uint8_t { both = 0, rhythm, notes, count };

const char *gen_what_name(GenWhat what);

constexpr int kGenRangeMin = 1;              // octaves
constexpr int kGenRangeMax = 3;
constexpr int kGenDownbeatDensity = 25;      // from here a drum part always starts on the one
constexpr uint8_t kGenDefaultBase = 48;      // C3: where a melodic part with no notes starts

// The four GEN knobs, settled at control rate (CLAUDE.md: settings come from a static make).
struct GenSettings {
    uint8_t density = 50;                    // 0..100
    uint8_t range = 1;                       // octaves, kGenRangeMin..kGenRangeMax
    uint8_t leap = 25;                       // 0..100: the chance a move jumps
    GenWhat what = GenWhat::both;

    // Every argument clamped, so an encoder or a damaged byte cannot ask for anything the
    // generator has to refuse.
    static GenSettings make(int density, int range, int leap, int what);
};

// The chance in 100 that step `index` gets a hit at `density`, from its place in a 16-step bar.
// Exposed for the tests and for nothing else.
int gen_hit_chance(int index, int density);

// Writes a new part on `track` (the steps of its own length) and reports whether anything
// changed. `melodic` is the machine's (core/project.hpp, `machine_folds_note`): false writes a
// drum part, whose notes are left as they are.
bool generate_track(Pattern &pattern, int track, bool melodic, const GenSettings &settings,
                    uint32_t variant);

}  // namespace strata
