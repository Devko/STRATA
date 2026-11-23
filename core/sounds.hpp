// Factory sounds: a short list of ready-made settings for each machine, loaded into ONE track.
//
// The kit library (app/storage.hpp) swaps all eight tracks at once, which is the wrong size for
// "try a different kick": the rest of the beat goes with it. A sound preset is one machine's
// worth of knobs -- SRC, the insert chain's FLTR page and AMP, and on the VAs the ASR and OSC pages --
// written into the selected track and nothing else. Opened with OK from EDIT or MACHINE on any
// track whose machine has some (app/app.hpp, SOUNDS), the way OK opens the sample browser on a
// SAMPLE or GRAIN track.
//
// ## What a preset leaves alone
//
//   - the FX page: lo-fi, pan and both sends are where a track sits in the MIX, not what it
//     sounds like, and a preset that moved them would undo the user's balance on every try;
//   - the LFO and ARP pages, which are performance, not timbre;
//   - the track's fader, its machine and its sample;
//   - HAT's CHK (slot 10), the choke group: it says which hat cuts which, a wiring choice;
//   - the ASR and OSC pages on the drums and on every machine that does not read them.
//
// `sound_preset_writes` is that list as a bit mask over a KitTrack's parameters.
//
// ## The first preset is the factory kit's
//
// Entry 0 of each machine is the sound core/factory.cpp gives the default kit (both HAT entries
// 0 and 1, closed and open; VA POLY's 0 the stab), so the list always has a way back to the
// sound a fresh project started with. tests/test_sounds.cpp holds the two files to that.
//
// Values are 0-127 knob bytes, chosen from the mappings in dsp/*.cpp, which core/ cannot include
// -- the same arrangement as core/factory.cpp, and app/engine.cpp static_asserts the page layout
// both rely on. Each preset's comment in core/sounds.cpp says what it is for.
#pragma once

#include "project.hpp"

#include <cstdint>

namespace strata {

struct SoundPreset {
    const char *name;        // six characters at most, upper case: the 5x7 font has nothing else
    uint8_t src[4];          // params 0-3
    uint8_t fltr[4];         // 4-7: DRV CUT RES MOD, the insert chain's
    uint8_t amp[4];          // 8-11
    uint8_t asr[4];          // 24-27: ATK SUS REL FSU, written on the machines that read ASR
    uint8_t osc[4];          // 28-31: the OSC page (WAV OS2 SUB DRV on a VA), the same
};

constexpr int kSoundNameChars = 6;

// The first parameter of the ASR page in a KitTrack, as dsp/track.hpp has it.
constexpr int kSoundAsrFirst = kKitArpEnd;
// And of the OSC page.
constexpr int kSoundOscFirst = kKitAsrEnd;

// How many presets `machine` has: 0 for SAMPLE, GRAIN, MID and any id this build lacks.
int sound_preset_count(uint8_t machine);

// Preset `index` of `machine`, or nullptr outside 0 .. count - 1.
const SoundPreset *sound_preset(uint8_t machine, int index);

// Bit p set: a preset of `machine` writes KitTrack parameter p. 0 for a machine with none.
uint32_t sound_preset_writes(uint8_t machine);

// Write preset `index` of `machine` into `params` (kKitParams bytes), the masked bytes only.
// False, and nothing written, for a preset that does not exist.
bool sound_preset_apply(uint8_t machine, int index, uint8_t *params);

// True when every byte the preset writes already has the preset's value in `params`: the list
// marks the sound a track has now.
bool sound_preset_matches(uint8_t machine, int index, const uint8_t *params);

// What switching a track TO `machine` writes (Roland, 2026-11-23: "switching a track's machine
// loads that machine's default sound"). Before, the knobs kept their positions across a switch,
// so a track switched to SAMPLE inherited STA half way in, nine slices and a short DEC from
// whatever it hosted before, and the new sample "started at a random position".
//
// Every parameter the machine reads for its SOUND is written: SRC, FLTR and AMP, and ASR and OSC
// on a machine that reads them. That is MORE than a preset writes (sound_preset_writes): a preset
// leaves HAT's choke group and GRAIN's POS and PIT alone because they say how the track sits in
// the kit or the sample, but values left there by another machine mean nothing, so a switch sets
// them too. The values are preset 0 of the machine's SOUNDS list, the factory kit's sound; SAMPLE,
// which has no list, plays the whole file from its start (kSamplePickResets) with the filter open.
// FX (lo-fi, pan, sends), LFO and ARP are not written: they are the track's mix and performance.
//
// False, and nothing written, for a machine with no defaults: MID (its knobs are CCs for someone
// else's instrument) and any machine without a SOUNDS list.
bool machine_defaults_apply(uint8_t machine, uint8_t *params);

}  // namespace strata
