// The performance layer's stored setup: scenes, the A-to-B morph, and the four macros.
//
// docs/SPEC.md's performance table:
//
//   Scenes and morph   "A scene stores parameter values for any tracks. Assign scene A and
//                       scene B, then VALUE crossfades the live sound from A to B. 16 scenes
//                       per project." Scene edits save; the morph position does not.
//   Macros             "4 macros on encoders A-D in PERFORM mode. Each drives up to 4
//                       parameters on any tracks, each with its own range." Macro setup saves.
//
// ## Base kit values only
//
// docs/REVIEW.md decided this and it is not to be re-litigated here: *"Should scenes morph
// locks? Base kit values only in v1. A lock is a per-step override and a morph is continuous, so
// crossfading the two has no single sensible meaning."* So a scene holds the same 16 values per
// track that `KitTrack::params` does, and a step's locks still win over whatever the morph
// produced -- which is what `app/engine.cpp` already does by rebuilding `effective_` from the
// kit and writing locks over the top.
//
// ## A fourth file, not a fourth section
//
// This saves to PERFORM.BIN, next to PROJECT.BIN, KIT.BIN and the banks, rather than as new
// sections of PROJECT.BIN. Two reasons, the second being the one that decided it:
//
//   - core/project.hpp's own reasoning for having three files: "saving writes only what
//     changed". Scene edits and pattern edits happen at different times.
//   - PROJECT.BIN's layout is fixed at 251 bytes and its format is pinned by tests. Adding
//     required sections would refuse every file an M4 build wrote, and adding optional ones
//     would need a reader that tolerates a missing section, which `find_sections` deliberately
//     does not. A separate file whose absence means "no scenes" costs nothing and breaks
//     nothing.
//
// ## Where this runs
//
// Like everything in core/, these are pure functions over caller-owned memory: no allocation, no
// files, no Enaga. `scene_morph` and `macro_apply` are CONTROL RATE -- once per block at most,
// from the audio side -- and they are written to be called that way: integer arithmetic only, no
// division in the inner loop, and nothing that touches more than 8 x 16 bytes.
#pragma once

#include "project.hpp"
#include "serialize.hpp"

#include <cstdint>

namespace strata {

// --- sizes ----------------------------------------------------------------

// docs/SPEC.md: "16 scenes per project".
constexpr int kScenes = 16;

// "4 macros on encoders A-D", "each drives up to 4 parameters".
constexpr int kMacros = 4;
constexpr int kMacroTargets = 4;

// The morph position's resolution: 0 is all of scene A, kMorphScale is all of scene B.
//
// A POWER OF TWO, not a thousand. The morph interpolates 8 x 16 parameters every time the
// position moves, and this chip has no integer divide instruction at all -- core/scheduler.cpp
// makes the same point about deriving samples-per-tick. 1024 makes each one a shift; 1000 would
// make it 128 calls to a software divide routine per block. The resolution is finer than either
// the encoder or the ear either way.
constexpr uint16_t kMorphScale = 1024;
constexpr int kMorphShift = 10;
static_assert((1u << kMorphShift) == kMorphScale,
              "the morph's shift and its scale have diverged, so it is dividing by the wrong "
              "number");
static_assert((kMorphScale & (kMorphScale - 1)) == 0,
              "kMorphScale must be a power of two: the morph shifts rather than divides");

// --- scenes ---------------------------------------------------------------

// One scene: a value for any subset of the 8 x 16 track parameters.
//
// `stored` is what makes "any tracks" work -- a scene that only touches the bass line must not
// drag every other track's sound with it when it is morphed to. One bit per parameter, two bytes
// per track, and zeroed memory means a scene that stores nothing, which is what an empty scene
// is.
struct Scene {
    uint8_t values[kTracks][kKitParams] = {};
    uint8_t stored[kTracks][2] = {};
};

struct SceneSet {
    Scene scenes[kScenes];
};

// Is this parameter stored in the scene?
constexpr bool scene_has(const Scene &scene, int track, int param) {
    if (track < 0 || track >= kTracks || param < 0 || param >= kKitParams) return false;
    return (scene.stored[track][param >> 3] >> (param & 7) & 1u) != 0;
}

// Store a value, or forget one. Out-of-range indices are ignored rather than clamped: clamping
// would silently write someone else's parameter.
void scene_set(Scene &scene, int track, int param, uint8_t value);
void scene_clear(Scene &scene, int track, int param);

// Store every parameter of every track from a kit: what "capture the current sound" does.
void scene_capture(Scene &scene, const Kit &kit);

// Store one track's parameters only, which is what capturing with a track selected does.
void scene_capture_track(Scene &scene, const Kit &kit, int track);

// Forget everything. A scene that stores nothing is skipped entirely by the morph.
void scene_erase(Scene &scene);

// How many parameters the scene stores, for the screen.
int scene_count(const Scene &scene);

// --- the morph ------------------------------------------------------------

// Crossfade from scene `a` to scene `b` at `position` in 0..kMorphScale, writing the result over
// `out`, which the caller has already filled with the kit values the morph starts from.
//
// The rule for a parameter only ONE of them stores is the one worth knowing: the other end is
// taken to be the kit value, not the stored value held constant. So assigning scene A and
// sweeping to B fades A's changes away where B says nothing about them, which is what a
// crossfade between two sounds means. Holding the value instead would make the sweep's end
// depend on which parameters happen to be stored in which scene, and two scenes that each
// touched different tracks would never fully cross over at all.
//
// A parameter neither stores is left exactly as `out` had it. A scene that stores nothing
// therefore behaves as the kit, which is what an unassigned slot should do.
//
// `position` is clamped. `out` is a copy of the kit, not the kit itself: the caller owns the
// distinction between the kit a file holds and the values the machines read
// (app/engine.cpp's `params_` against `project_.kit`).
void scene_morph(const Scene &a, const Scene &b, uint16_t position, const Kit &kit,
                 uint8_t out[kTracks][kKitParams]);

// --- macros ---------------------------------------------------------------

// One parameter a macro drives, over its own range. `low` is the value at macro 0 and `high` the
// value at 127, and low may be ABOVE high: an inverted target is how one knob opens a filter
// while it closes a send, which is most of what makes a macro worth having.
//
// `enabled` 0 means the target is unused, which is also what zeroed memory means -- the same
// convention as `ChainEntry::repeats` (core/project.hpp).
struct MacroTarget {
    uint8_t enabled = 0;
    uint8_t track = 0;
    uint8_t param = 0;
    uint8_t low = 0;
    uint8_t high = 127;
};

struct Macro {
    MacroTarget targets[kMacroTargets];
};

struct MacroSet {
    Macro macros[kMacros];
};

// The value `target` asks for at macro position `position` (0..127). Rounded to nearest, and
// correct for an inverted target. Meaningless for a disabled target, which `macro_apply` skips.
uint8_t macro_value(const MacroTarget &target, uint8_t position);

// Write every enabled target of `macro` into `out` at `position`. Targets naming a track or
// parameter that does not exist are skipped -- a macro set up for a build with more parameters
// must not write past the array.
//
// Later targets win over earlier ones when two drive the same parameter. That is a consequence
// of writing them in order rather than a decision worth defending; the UI should not offer it.
void macro_apply(const Macro &macro, uint8_t position, uint8_t out[kTracks][kKitParams]);

// How many targets are enabled and usable, for the screen.
int macro_count(const Macro &macro);

// --- PERFORM.BIN ----------------------------------------------------------
//
// The container is core/serialize.hpp's, and the section ids are per file type as
// core/project.hpp's are: a project's ids and this file's are separate numbering spaces.
//
// Permanent, like every other section id in this project: a new section takes the next free
// number and one that goes away leaves its number burnt.

constexpr uint16_t kPerformVersion = 1;
constexpr uint16_t kSectionPerformScenes = 1;
constexpr uint16_t kSectionPerformMacros = 2;

// Loading is value-tolerant but structure-strict, exactly as the other three file types are: a
// macro naming a track that does not exist is dropped, while a bad magic, an unknown version, a
// bad CRC or a malformed section table is refused. The ORDER of those checks is the same too,
// so all five file types give the same reason for the same damage.
uint32_t perform_bytes();
SerializeResult perform_write(const SceneSet &scenes, const MacroSet &macros, uint8_t *buffer,
                              uint32_t capacity, uint32_t *written = nullptr);
SerializeResult perform_read(SceneSet &scenes, MacroSet &macros, const uint8_t *buffer,
                             uint32_t size);

// An empty set: every scene storing nothing, every macro target disabled.
void perform_init(SceneSet &scenes, MacroSet &macros);

}  // namespace strata
