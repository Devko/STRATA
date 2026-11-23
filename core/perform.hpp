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
//   - PROJECT.BIN's layout was fixed at 251 bytes and its format is pinned by tests. Adding
//     required sections would refuse every file an M4 build wrote, and adding optional ones
//     needed a reader that tolerates a missing section, which `find_sections` did not then do.
//     A separate file whose absence means "no scenes" costs nothing and breaks nothing.
//
//     Since then the sends and the master DID go into PROJECT.BIN, as an OPTIONAL third
//     section, and `find_sections` grew the tolerance to allow it (core/project.cpp says how).
//     The reasoning above still decides THIS file: fourteen bytes that change when a knob moves
//     belong with the tempo and the chain, while 2.4 KiB of scenes and macro setup do not, and
//     "saving writes only what changed" is the whole point of there being four files.
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

// --- the master kill EQ's bands, as the performance layer sees them --------
//
// core/ cannot include dsp/buses.hpp, so the EQ's place in ProjectSettings::master is stated here
// and app/ static_asserts it against MasterField::eq_low and kKillBands.
constexpr int kMasterBands = 3;
constexpr int kMasterEqFirst = 4;
static_assert(kMasterEqFirst + kMasterBands == kMasterFields,
              "the kill EQ's bands are the last three master bytes");

// A macro target on the master EQ rather than a track: `track` is this, `param` the band 0-2.
// One past the last track, so a build without it drops such a target as a track it does not
// have, which is what an older build reading a newer PERFORM.BIN should do.
constexpr uint8_t kMacroMasterTrack = uint8_t(kTracks);

// --- scenes ---------------------------------------------------------------

// One scene: a value for any subset of the 8 x 32 track parameters (16 sound, then the LFO's 4,
// the arp's 4, the ASR page's 4 and the OSC page's 4).
//
// `stored` is what makes "any tracks" work -- a scene that only touches the bass line must not
// drag every other track's sound with it when it is morphed to. One bit per parameter, four
// bytes per track (the third holds the LFO's and the arp's four bits each, the fourth the ASR
// page's in its low half and the OSC page's in its high half), and zeroed memory means a scene that
// stores nothing, which is what an empty scene is.
constexpr int kSceneStoredBytes = (kKitParams + 7) / 8;
struct Scene {
    uint8_t values[kTracks][kKitParams] = {};
    uint8_t stored[kTracks][kSceneStoredBytes] = {};
    // The master kill EQ's three band bytes (ProjectSettings::master, from kMasterEqFirst), and
    // which of them the scene stores, bit 0 LOW. A whole-kit capture stores all three, so a
    // morph to a scene with the lows cut sweeps them out; the kill SWITCHES are not in a scene,
    // they are a hand on the panel.
    uint8_t master[kMasterBands] = {};
    uint8_t master_stored = 0;
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

// Store the master EQ's three band bytes, which a whole-kit capture also does in the engine.
void scene_capture_master(Scene &scene, const uint8_t bands[kMasterBands]);

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
// `position` is clamped. `out` comes in holding the LIVE kit and is also where the end a scene
// does not store is read from. It used to take a separate `Kit`, and the engine passed
// `project_.kit` -- the kit as last saved, refreshed only by `Cmd::kit_store` -- so a morph
// towards an empty scene pulled every knob turned since the last save back to its saved value.
void scene_morph(const Scene &a, const Scene &b, uint16_t position,
                 uint8_t out[kTracks][kKitParams]);

// The same crossfade for the master EQ's three band bytes, by the same rule: `out` holds the
// live bytes on the way in, and the end a scene does not store is that live value.
void scene_morph_master(const Scene &a, const Scene &b, uint16_t position,
                        uint8_t out[kMasterBands]);

// --- macros ---------------------------------------------------------------

// One parameter a macro drives. `high` is the value at 127, and it may be BELOW where the
// parameter starts: an inverted target is how one knob opens a filter while it closes a send,
// which is most of what makes a macro worth having.
//
// `low` is the parameter's value when the target was assigned. `macro_apply` no longer reads it: since 2026-11-22 a macro sweeps from the parameter's LIVE
// value (see below), so `low` is kept for the screen, for the file, and for `macro_value`.
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

// A target range that passes through `now` at the knob's current `position` and heads for `far`
// at the top of the knob (or the bottom, when the knob is already at the top). Assigning a macro
// with `low = now, high = far` made the parameter jump to wherever `position` put it in that
// range the moment it was assigned; this keeps it where it is, to within one step of rounding.
//
// `high` is `far` whenever a `low` in 0..127 can reach `now` at `position`; otherwise `low` is
// clamped and `high` solved for instead. At position 127 the range runs from `far` up to `now`.
// The one exception: `now` at an end of 0..127 and `far` the other end, with the knob in
// between, has no such line except a flat one; it gets the plain range and jumps once.
void macro_range_through(uint8_t now, uint8_t far, uint8_t position, uint8_t &low, uint8_t &high);

// Write every enabled target of `macro` into `out` at `position`. Targets naming a track or
// parameter that does not exist are skipped -- a macro set up for a build with more parameters
// must not write past the array.
//
// **A macro sweeps from the parameter's live value to `high`.** `out` comes in holding what the
// machines would play without macros -- the kit, then the morph -- and each target writes
//
//     live + (high - live) * position / 127
//
// So at position 0 a macro writes exactly what is already there: the EDIT knob and the morph have
// the parameter back. It used to write `low` at 0 like at every other position, so an assigned
// macro held its parameters for as long as it stayed assigned: turning that knob in EDIT moved
// the screen and not the sound, and a scene morphing the same parameter was overwritten.
// Roland's decision, 2026-11-22. And because the sweep starts from the live value, the first
// detent up is a nudge from wherever the sound is -- after a knob turn or a morph too -- never
// a jump to a stored `low`.
//
// Later targets win over earlier ones when two drive the same parameter. That is a consequence
// of writing them in order rather than a decision worth defending; the UI should not offer it.
void macro_apply(const Macro &macro, uint8_t position, uint8_t out[kTracks][kKitParams]);

// And the targets on the master EQ (`track == kMacroMasterTrack`), into its three band bytes,
// the same sweep from the live value. `macro_apply` skips these, this skips the track ones.
void macro_apply_master(const Macro &macro, uint8_t position, uint8_t out[kMasterBands]);

// Does this target name something this build has: a track parameter, or a master EQ band?
constexpr bool macro_target_usable(const MacroTarget &target) {
    return target.enabled != 0 &&
           ((target.track < kTracks && target.param < kKitParams) ||
            (target.track == kMacroMasterTrack && target.param < kMasterBands));
}

// One target's value at `position`, swept from `live` towards `high` as `macro_apply` does it.
// Rounded to nearest; exactly `live` at 0 and exactly `high` at 127.
uint8_t macro_value_from(uint8_t live, const MacroTarget &target, uint8_t position);

// How many targets are enabled and usable, for the screen.
int macro_count(const Macro &macro);

// --- mutes ----------------------------------------------------------------
//
// docs/SPEC.md: "Lower pads toggle track mutes, instantly or at the next bar. 8 stored mute
// states per project, recalled from the upper row." The 8 states live in
// `ProjectSettings::mute_states` and go on disk with the project; this is the arithmetic over
// them, which is here rather than in app/engine.cpp so it can be tested.
//
// A state is one bit per track, bit 0 being track 1. Zeroed memory means nothing muted, which is
// what an unused slot should be.

// Read a stored state into the eight flags the mixer uses.
void mute_state_apply(uint8_t state, bool muted[kTracks]);

// And back, for storing the current mutes into a slot.
uint8_t mute_state_capture(const bool muted[kTracks]);

// "Instantly or at the next bar". The UI posts a change; whether it lands now or at the boundary
// is the user's setting, and the latch is what holds it in between.
//
// Audio-side state: the arming and the "is it time yet" both happen there, for the same reason
// core/queue.hpp's Queue needs no atomics.
struct MuteLatch {
    bool pending = false;
    uint8_t state = 0;      // the mute set to take effect
};

// Arm a change for the next boundary. Replaces any change already armed -- a performer pressing
// three pads before the bar means the third one, not a queue of three.
void mute_latch_arm(MuteLatch &latch, uint8_t state);

// The boundary came: take the armed state if there is one. False when nothing was armed, leaving
// `state` untouched.
bool mute_latch_release(MuteLatch &latch, uint8_t &state);

void mute_latch_clear(MuteLatch &latch);

// --- auto-fill ------------------------------------------------------------
//
// docs/SPEC.md: "Hold to activate FILL conditions; optional auto-fill every 4, 8 or 16 bars."

// The periods the UI offers. 0 is off, and it is first so that zeroed memory means off.
enum class AutoFill : uint8_t { off = 0, every_4, every_8, every_16, count };

const char *auto_fill_name(AutoFill period);   // "OFF", "4", "8", "16"

// How many bars the period is; 0 for off.
constexpr int auto_fill_bars(AutoFill period) {
    switch (period) {
        case AutoFill::every_4:  return 4;
        case AutoFill::every_8:  return 8;
        case AutoFill::every_16: return 16;
        default:                 return 0;
    }
}

// Is auto-fill active during `bar`? True on the LAST bar of each period, which is what a fill
// is: the bar that leads back into the loop. So at every 4, bars 3, 7, 11 ... fill, counting
// from 0.
//
// `bar` is a count from the start of the transport, not a position inside the pattern: a fill
// every 8 bars has to mean the same thing whether the pattern is 16 steps or 64.
constexpr bool auto_fill_active(AutoFill period, uint32_t bar) {
    const int bars = auto_fill_bars(period);
    if (bars <= 0) return false;
    return int(bar % uint32_t(bars)) == bars - 1;
}

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
// The scenes' LFO bytes: per scene, per track, the four values and then the bitmask byte whose
// low four bits say which are stored. OPTIONAL on read: a PERFORM.BIN from before the LFO loads
// with no scene storing any LFO byte. Section 1 keeps its 16 values and two mask bytes a track,
// so an older build reads a newer file unchanged and simply never asks for this one.
constexpr uint16_t kSectionPerformSceneLfo = 3;
// The scenes' master EQ bytes: 4 per scene (three bands, then the stored bits). OPTIONAL on read:
// a PERFORM.BIN from before it loads with no scene storing the EQ.
constexpr uint16_t kSectionPerformSceneMaster = 4;
// The scenes' arp bytes: per scene, per track, the four values and then a byte whose low four
// bits say which are stored. OPTIONAL on read, like the LFO's.
constexpr uint16_t kSectionPerformSceneArp = 5;
// The scenes' ASR bytes (the VA machines' attack, sustain, release and filter sustain): per scene,
// per track, the four values and then a byte whose low four bits say which are stored, which
// is the low half of the fourth mask byte. OPTIONAL on read, like the LFO's.
constexpr uint16_t kSectionPerformSceneAsr = 6;
// The scenes' OSC bytes (the VA machines' WAV, OS2, SUB and DRV), laid out as the ASR section is:
// the four values, then a byte whose low four bits say which are stored -- the HIGH half of the
// fourth mask byte. OPTIONAL on read.
constexpr uint16_t kSectionPerformSceneOsc = 7;

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
