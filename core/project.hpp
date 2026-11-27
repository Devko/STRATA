// A project: 8 banks of 16 patterns, the kit, the chains, and the files they live in.
//
// docs/SPEC.md, "Data model and storage". A project is one folder on the card and loads fully
// into RAM when it is opened:
//
//     PROJECT.BIN   tempo, root, scale, chains, mute states, the sample budget
//     KIT.BIN       per track: machine, 16 parameters, mixer level, sample path
//     BANK_A.BIN .. BANK_H.BIN   16 patterns each
//
// Three files rather than one because saving writes only what changed: a bank is about 130 KiB,
// which stalls the UI for tens of milliseconds and the audio for none, where one 1 MiB file
// would rewrite 127 untouched patterns every time a step is toggled.
//
// ## This is the live data, not a copy of it
//
// The sequencer plays a pattern *inside* this structure -- `live()` -- rather than a copy of it.
// So an edit lands in the pattern that will be saved, a pattern change is an index change with
// no 8 KiB copy in the audio path, and saving a bank reads exactly the memory the audio side
// plays from. The cost is that an undo history belongs to one pattern: `core/undo.hpp`'s
// `clear()` has to be called on a pattern change, which its own header already says.
//
// ## Why the kit is bytes here and not dsp/'s structs
//
// `core/` does not depend on `dsp/` (core/pattern.hpp says why), so this file cannot name
// `TrackParams`. The kit is therefore declared as what it is on disk -- 16 parameter bytes per
// track -- and `app/` copies between the two, with a static_assert there that the counts still
// agree. A format that followed a struct in another layer would change meaning the day that
// struct did.
//
// ## Where this runs
//
// Writing is a main-context operation over a caller-owned buffer, like everything in
// core/serialize.hpp, and never modifies the project. Reading fills a Project the caller owns.
// Nothing here allocates, and nothing here touches a file: `app/` is where this meets
// `enaga::files`.
#pragma once

#include "arp.hpp"         // KitArp, the arp's kit settings
#include "midiout.hpp"     // kMidiOutOff and kMidiChannels: ProjectSettings::midi_out's domain
#include "modslot.hpp"     // KitModSlot
#include "pattern.hpp"
#include "serialize.hpp"

#include <cstdint>

namespace strata {

// --- sizes ----------------------------------------------------------------

constexpr int kBanks = 8;                    // A..H
constexpr int kPatternsPerBank = 16;
constexpr int kProjectPatterns = kBanks * kPatternsPerBank;   // 128

// Parameters per track, mirroring dsp/track.hpp's kTrackParams without depending on it. app/
// asserts that the three agree: 16 SOUND parameters (the machine's and the insert chain's) and
// then the 4 bytes of the track's LFO (dsp/lfo.hpp). The split matters to the files: KIT.BIN and
// PERFORM.BIN carried 16 a track before the LFO existed, so the four LFO bytes travel in sections
// of their own that an older file simply does not have. The arpeggiator's four (core/arp.hpp)
// follow the LFO's in the same way, and in a section of their own again, and so do the four of
// the VA machines' ASR page (attack, sustain, release, filter sustain; dsp/va.cpp) and the four
// of their OSC page (wave, OSC 2, sub, drive).
constexpr int kKitSoundParams = 16;
constexpr int kKitLfoParams = 4;
constexpr int kKitLfoEnd = kKitSoundParams + kKitLfoParams;
constexpr int kKitArpParams = 4;
constexpr int kKitArpEnd = kKitLfoEnd + kKitArpParams;
constexpr int kKitAsrParams = 4;
constexpr int kKitAsrEnd = kKitArpEnd + kKitAsrParams;
constexpr int kKitOscParams = 4;
constexpr int kKitParams = kKitAsrEnd + kKitOscParams;

// --- the LFO's WAV and DST bytes: numbers, not bands (format decision C, 2026-11-26) ----------
//
// WAV is the shape's number, 0-5 (dsp/lfo.hpp's LfoWave), and DST is 0 for OFF or 1-16 for a
// sound parameter. Until this they were bands of 0-127 (six and seventeen), so a seventh shape or
// a destination past the sound page would have moved every band edge and re-read every stored
// byte as something else. A file written before this marks no layout and is converted once on
// read (`kit_lfo_from_band`): in the kit, the scenes, the macros and the steps' locks.
constexpr int kKitLfoWaveIndex = kKitSoundParams + 0;
constexpr int kKitLfoDestIndex = kKitSoundParams + 3;
constexpr int kKitLfoWaves = 6;                              // static_asserted in dsp/lfo.hpp
constexpr uint8_t kKitLfoWaveMax = uint8_t(kKitLfoWaves - 1);
constexpr uint8_t kKitLfoDestMax = uint8_t(kKitSoundParams);  // OFF, then the 16
// The marker byte the layout sections hold: 1 = WAV and DST are numbers.
constexpr uint8_t kLfoLayoutNumbers = 1;

// The highest value a parameter byte may hold: 127 for all but WAV and DST. Knobs, locks,
// random ranges and LIVE RND clamp to it, so a knob does not wind past the last shape.
constexpr uint8_t kit_param_max(int index) {
    return index == kKitLfoWaveIndex ? kKitLfoWaveMax
                                     : (index == kKitLfoDestIndex ? kKitLfoDestMax : uint8_t(127));
}

// A byte of the old band layout as the number it meant: WAV in six bands, DST in seventeen; any
// other parameter unchanged. Integer multiply-shift, as dsp/lfo.hpp decoded the bands.
constexpr uint8_t kit_lfo_from_band(int index, uint8_t byte) {
    const int b = byte > 127 ? 127 : int(byte);
    return index == kKitLfoWaveIndex   ? uint8_t((b * kKitLfoWaves) >> 7)
           : index == kKitLfoDestIndex ? uint8_t((b * (kKitSoundParams + 1)) >> 7)
                                       : byte;
}

// What a kit from before the OSC page loads as: a saw, OSC 2 off, the sub at 102 (the 0.4 it was
// fixed at) and no drive -- the old voice without its fixed synced-pulse bite, which the OSC
// page's SYN zone now does properly (dsp/va.cpp).
constexpr uint8_t kKitOscLegacy[kKitOscParams] = {0, 0, 102, 0};

// How many machines a track can host, mirroring dsp/'s machine list the same way and for the
// same reason as kKitParams: docs/SPEC.md, "Machines (v1)" -- KICK, SNARE, HAT, PERC, SAMPLE,
// GRAIN, VA MONO and VA POLY -- plus MID, which plays nothing here and sends MIDI (dsp/extmidi.hpp),
// and the wavetable pair WAVE MONO and WAVE POLY (dsp/va.hpp, dsp/wavetable.hpp), and FM4
// (dsp/fmpoly.hpp). The file format needs the domain of KitTrack::machine, because a
// kit naming machine 12 has to become something this build can host rather than an index into
// app/'s machine pool. app/ asserts that the two agree.
constexpr int kMachines = 12;

// Which machine a track hosts, as the number KIT.BIN stores.
//
// **These ids are PERMANENT.** They are on disk the moment a kit is saved, so renumbering them
// would make every existing project play the wrong machines -- the same rule core/scale.hpp
// states for its scale ids, and for the same reason. New machines go on the END.
//
// The order is docs/SPEC.md's "Machines (v1)" list, read left to right, so a reader comparing
// the two sees the same sequence. A kit naming an id this build does not have plays NOTHING
// rather than being reinterpreted as an index into app/'s pool: a silent track is recoverable
// and a wrong pointer is not.
enum class MachineId : uint8_t {
    kick = 0,
    snare,
    hat,
    perc,
    sample,
    grain,
    va_mono,
    va_poly,
    midi,        // MID: no audio; the track's notes and eight CCs go to MIDI OUT
    wave_mono,   // WMN: the VA MONO voice with a wavetable oscillator (dsp/va.hpp)
    wave_poly,   // WPL: and the VA POLY machine with it, sharing the POLY voice budget
    fm,          // FM4: four-operator FM, six voices, in the POLY voice budget too (dsp/fmpoly.hpp)
    count
};
static_assert(int(MachineId::count) == kMachines,
              "kMachines is the domain KIT.BIN stores: it has to match the id list exactly");

// The three-letter name for the screen, or "---" for an id this build does not have.
const char *machine_name(uint8_t id);

// Does this track read a step's note as a PITCH IN THE KEY, so the sequencer should fold the
// note into the scale before it arrives?
//
// docs/SPEC.md: "melodic tracks fold notes into the scale". That sentence needs a notion of
// which tracks are melodic, and this is it:
//
//   - The two VAs and GRAIN always fold. They read `note` as a pitch, and a part written on
//     them is a melody.
//   - SAMPLE folds only when it is UNSLICED (SLC = 1), where the note transposes the whole
//     window and the track plays like an instrument. With SLC > 1 the note picks the SLICE, and
//     folding it would play a different slice -- not a musical operation at all -- so a sliced
//     sample never folds. Roland's decision, 2026-11-22.
//   - MID never folds. The note goes to someone else's instrument, which may be a drum machine
//     mapping notes to sounds, so it leaves exactly as written -- a chord step still sends its
//     chord, which `chord_expand` folds as it builds it.
//   - The DRUM machines read `note` as a pitch too (dsp/kick.cpp tunes the body from it), but
//     folding it would RETUNE them: a kick written at D on a C minor track would move, and
//     nobody writing a drum part is thinking in degrees.
//
// `slices` is the Sampler's slice count as its last `prepare` derived it (locks and scenes
// included); it is read only for SAMPLE. Here rather than in the engine because it is a property
// of the machine, and core/ is where `MachineId` lives. An id this build does not have does not
// fold -- it plays nothing anyway.
constexpr bool machine_folds_note(uint8_t id, int slices) {
    return id == uint8_t(MachineId::va_mono) || id == uint8_t(MachineId::va_poly) ||
           id == uint8_t(MachineId::wave_mono) || id == uint8_t(MachineId::wave_poly) ||
           id == uint8_t(MachineId::fm) ||
           id == uint8_t(MachineId::grain) || (id == uint8_t(MachineId::sample) && slices <= 1);
}

// What picking a new file in the sample browser sets on a SAMPLE track, so the file plays WHOLE
// from its first frame: STA 0, LEN 127, SLC one slice and DEC 127 (hold, the end of the sample
// ends the note). Without it the track kept whatever the knobs held -- the factory LOOP track's
// eight slices, or 64s from another machine, which is STA half way in and nine slices -- so a new
// file started at what sounded like a random point and DEC cut it short. Roland, 2026-11-23.
// PIT, ATK, PLY and LVL are left alone: they are how the track plays, not where in the file.
// Only a pick does this; a project or kit load keeps the values it saved. Slot numbers are
// dsp/sample.cpp's (SRC 0-3, AMP 8-11), which core/ cannot include.
struct ParamReset {
    uint8_t slot;
    uint8_t value;
};
inline constexpr ParamReset kSamplePickResets[] = {
    {1, 0},      // STA: the first frame
    {2, 127},    // LEN: all of it
    {3, 0},      // SLC: one slice, so the trig note pitches instead of picking a slice
    {9, 127},    // DEC: hold
};
inline constexpr int kSamplePickResetCount =
    int(sizeof kSamplePickResets / sizeof kSamplePickResets[0]);

// Does this machine read the ASR page (ATK SUS REL FSU)? The VAs, the WAVE pair, which share the
// VA voice, FM4, which reads it with its own fourth knob (FSU is its index sustain), GRAIN (ATK
// SUS REL) and SAMPLE, which reads SUS and REL only: its attack has always been on AMP, so the
// page's ATK and fourth knob are not its own (dsp/sample.cpp). Everything that shows or saves
// the page asks here, so a machine that starts reading it is one line.
constexpr bool machine_reads_asr(uint8_t id) {
    return id == uint8_t(MachineId::va_mono) || id == uint8_t(MachineId::va_poly) ||
           id == uint8_t(MachineId::wave_mono) || id == uint8_t(MachineId::wave_poly) ||
           id == uint8_t(MachineId::fm) || id == uint8_t(MachineId::grain) ||
           id == uint8_t(MachineId::sample);
}

// GRAIN: the granular player, whose ASR and OSC pages mean something of their own.
constexpr bool machine_is_grain(uint8_t id) { return id == uint8_t(MachineId::grain); }
// And the OSC page: the synths and GRAIN (SCN PSP), not SAMPLE.
constexpr bool machine_reads_osc(uint8_t id) {
    return machine_reads_asr(id) && id != uint8_t(MachineId::sample);
}
// SAMPLE's ASR page is SUS and REL alone: ATK is AMP's, and there is no fourth.
constexpr bool machine_reads_asr_slot(uint8_t id, int slot) {
    return machine_reads_asr(id) && (id != uint8_t(MachineId::sample) || slot == 1 || slot == 2);
}

// The WAVE pair: the VA voice with a wavetable where its oscillators were.
constexpr bool machine_is_wave(uint8_t id) {
    return id == uint8_t(MachineId::wave_mono) || id == uint8_t(MachineId::wave_poly);
}

// FM4: four sine operators in one of eight algorithms (dsp/fm.hpp).
constexpr bool machine_is_fm(uint8_t id) { return id == uint8_t(MachineId::fm); }

// --- one filter on the synth machines (D8) --------------------------------------------------
//
// The VAs and the WAVE pair have a filter of their own (CUT RES on SRC, ENV on AMP), and they
// used to have the insert chain's whole filter on top of it: two cutoffs on one track, which
// confused on the unit. On these machines the insert filter shrinks to ONE byte, TON, and the
// byte that frees takes the filter envelope's own decay, FDE. No byte is added or moved:
//
//     slot 4  FDE  filter decay. 0 is the old tie, 0.55 x DEC (and what every kit from before
//                  this loads as); 1-127 is its own time, DEC's curve, 15 ms to 2.4 s
//     slot 5  TON  one tone knob in the insert chain: 64 flat (the filter is skipped), below it
//                  a 12 dB low-pass closing from 18 kHz to 250 Hz, above it a 12 dB high-pass
//                  rising from 20 Hz to 1.5 kHz
//     slot 6, 7    unused: the insert's RES and MOD have nothing to do here
//
// The insert's DRV is off on these machines too (the OSC page has the voice's own DRV). FM4 has
// no filter of its own, so it keeps the insert filter.
constexpr bool machine_has_voice_filter(uint8_t id) {
    return id == uint8_t(MachineId::va_mono) || id == uint8_t(MachineId::va_poly) ||
           id == uint8_t(MachineId::wave_mono) || id == uint8_t(MachineId::wave_poly);
}
constexpr int kSlotFdec = 4;
constexpr int kSlotTone = 5;
constexpr uint8_t kToneFlat = 64;
// TON's two curves, which dsp/chain.cpp maps and kit_tone_from_insert inverts. 6.1699 octaves is
// 18 kHz down to 250 Hz; 6.2288 is 20 Hz up to 1.5 kHz.
constexpr float kToneLpTopHz = 18000.0f;
constexpr float kToneLpOctaves = 6.1699f;
constexpr float kToneHpLowHz = 20.0f;
constexpr float kToneHpOctaves = 6.2288f;

// The EDIT pages of such a machine show the filter section together, so FLTR is that machine's
// filter: CUT RES ENV FDE. SRC keeps the voice (TUN MOD, or TBL POS) and TON; AMP keeps DEC GLD
// LVL (or DEC MOT LVL). Only the order on screen changes -- the bytes, the locks, the macros and
// the files keep their slots -- so this map is the whole of it. Every other machine, and every
// page from LFO on, is page x 4 + knob.
constexpr int edit_slot(uint8_t machine, int page, int knob) {
    constexpr int8_t kVoiceFilter[3][4] = {{0, 1, 5, 6}, {2, 3, 9, 4}, {8, 7, 10, 11}};
    // GRAIN's AMP shows DEC first, as every other machine does (Roland, 2026-11-26): knob A turns
    // slot 10, B and C PIT and SHP. Only where the knobs are; the bytes stay where KIT.BIN has them.
    constexpr int8_t kGrainAmp[4] = {10, 8, 9, 11};
    if (machine_is_grain(machine) && page == 2 && knob >= 0 && knob <= 3) return kGrainAmp[knob];
    if (!machine_has_voice_filter(machine) || page < 0 || page > 2 || knob < 0 || knob > 3) {
        return page * 4 + knob;
    }
    return kVoiceFilter[page][knob];
}

// The page a slot shows on: edit_slot's inverse.
constexpr int edit_page_of(uint8_t machine, int slot) {
    for (int page = 0; page < 3; page++) {
        for (int knob = 0; knob < 4; knob++) {
            if (edit_slot(machine, page, knob) == slot) return page;
        }
    }
    return slot / 4;
}

// What an older kit's insert filter becomes on such a machine: the nearest TON, from its CUT
// (slot 5) and MOD (slot 7) bytes as dsp/chain.cpp mapped them (20 Hz to 18 kHz; OFF LP12 LP24
// BP12 BP24 HP12 HP24 in seven equal zones). OFF is flat; a low-pass, and a band-pass, whose top
// cut is the part you hear most, become TON's low-pass at the same cutoff; a high-pass becomes
// its high-pass. RES and DRV have no TON equivalent and are dropped. Pure arithmetic: core/ has
// no libm, and both curves are linear in octaves.
constexpr uint8_t kit_tone_from_insert(uint8_t cut, uint8_t mod) {
    const int zone = (int(mod) * 7) / 128;
    if (zone == 0) return kToneFlat;
    const float octaves = 9.8138f * float(cut > 127 ? 127 : cut) * (1.0f / 127.0f);  // over 20 Hz
    if (zone >= 5) {
        const int v = int(64.0f + 63.0f * octaves * (1.0f / kToneHpOctaves) + 0.5f);
        return uint8_t(v > 127 ? 127 : (v < 65 && cut > 0 ? 65 : v));
    }
    const float below = 9.8138f - octaves;             // octaves under 18 kHz
    const int v = int(64.0f - 64.0f * below * (1.0f / kToneLpOctaves) + 0.5f);
    return uint8_t(v < 0 ? 0 : (v > 63 && cut < 127 ? 63 : v));
}

// An older kit track's slots 4-7 rewritten for one filter: FDE tied, TON from the insert filter.
inline void kit_track_to_voice_filter(uint8_t *params) {
    const uint8_t tone = kit_tone_from_insert(params[5], params[7]);
    params[kSlotFdec] = 0;
    params[kSlotTone] = tone;
    params[6] = 0;
    params[7] = 0;
}

// A sample path, including its terminator. Enaga's paths are absolute and UTF-8
// ("/SAMPLES/Kick 01.wav"), and long names are supported, so this has to hold a real one rather
// than an 8.3 name.
constexpr int kSamplePathBytes = 112;

// docs/SPEC.md: "Chains: up to 64 entries per project, each a pattern plus a repeat count."
constexpr int kChainEntries = 64;
// The most passes one entry repeats for from the CHAIN page (the file allows up to 255). Kept at
// 16 by Roland's decision on 2026-11-22: a longer section is two entries of the same pattern.
constexpr int kChainRepeatsMax = 16;

// --- the kit --------------------------------------------------------------

// The sidechain duck's settings (dsp/chain.hpp's Duck), as bytes the kit stores. The factory
// values are the ones the duck was fixed at before it became editable (v0.1.0): every track but
// track 1 ducks 35% under track 1's trigs, 3 ms in, 40 ms held, 120 ms out. A KIT.BIN written
// before the duck section existed reads back as exactly these.
//
// Depth is a percentage so that the default is exactly 0.35 and the screen shows what the file
// holds. The times are in steps the encoder can walk: 1 ms for the attack, 5 ms for hold and
// release. The source track's own depth is kept but ignored -- a track never ducks under itself --
// so moving the source and moving it back loses nothing.
constexpr uint8_t kDuckDepthMax = 100;           // percent
constexpr uint8_t kDuckDepthDefault = 35;
constexpr uint8_t kDuckAttackMin = 1;            // ms; dsp/chain.cpp clamps at 0.5 ms anyway
constexpr uint8_t kDuckAttackMax = 50;
constexpr uint8_t kDuckAttackDefault = 3;
constexpr uint8_t kDuckHoldMax = 100;            // x 5 ms: 0-500 ms
constexpr uint8_t kDuckHoldDefault = 8;          // 40 ms
constexpr uint8_t kDuckReleaseMin = 2;           // x 5 ms: 10 ms-1 s
constexpr uint8_t kDuckReleaseMax = 200;
constexpr uint8_t kDuckReleaseDefault = 24;      // 120 ms
constexpr int kDuckTimeStepMs = 5;

// The duck's settings that are one per kit rather than one per track.
struct KitDuck {
    uint8_t source = 0;                          // the track whose trigs fire it, 0-7
    uint8_t attack_ms = kDuckAttackDefault;
    uint8_t hold = kDuckHoldDefault;             // x kDuckTimeStepMs
    uint8_t release = kDuckReleaseDefault;       // x kDuckTimeStepMs
};

// One track's sound, as the project stores it.
struct KitTrack {
    uint8_t params[kKitParams] = {};
    uint8_t machine = 0;                        // which machine the track hosts
    float level = 1.0f;                         // the mixer fader
    char sample[kSamplePathBytes] = {};         // empty when the track plays no file
    // SLC AUTO: 1 cuts a SAMPLE track's slices at the hits found in its sample
    // (core/onsets.hpp), 0 into equal slices. Not a parameter, so no lock, scene or LFO moves
    // it: it says what kind of sample this is, like the path beside it.
    uint8_t slice_mode = 0;
    // How far this track ducks under the sidechain duck, in percent: 0 never, 100 fully. Not a
    // parameter either -- like the fader beside it, it is the mixer's, and no lock moves it.
    uint8_t duck = kDuckDepthDefault;
    // LFO TRIG (dsp/lfo.hpp): when the track's LFO starts a cycle -- kLfoTrigFree, Retrig or
    // OneShot. A kit setting on the LFO page (push RAT), not a parameter: there is no 33rd byte
    // for a lock to name, and the 32 a track has are all spoken for.
    uint8_t lfo_trig = 0;
    // The two mod slots (core/modslot.hpp): a source, a destination and a depth each, learnt on
    // the LFO page. Kit settings for the same reason as TRIG; KIT.BIN's optional section 10.
    KitModSlot mod[kModSlots];
    // CHOKE (S4): 0 none, or a group 1-kChokeGroups. A note on this track cuts every other
    // track of the same group with a 64-frame fade (Engine::choke_group). A kit setting, not a
    // parameter, for TRIG's reason; KIT.BIN's optional section 11.
    uint8_t choke = 0;
    // The arp's kit settings (core/arp.hpp): VAR, RHYTHM, ACCENT and RATCHET, pushed round on the
    // ARP page or set by an ARPS preset. Not parameters, for TRIG's reason; KIT.BIN's optional
    // section 12.
    KitArp arp;
    // How much of the track goes to the chorus, send C (dsp/buses.hpp), 0-127: 0 none, 127 all,
    // post-fader as SNA and SNB are. A kit setting turned on MIXER's CHO 1-4 / 5-8, not a
    // parameter, for TRIG's reason: no lock, scene or LFO moves it. KIT.BIN's optional section 14.
    uint8_t chorus = 0;
};

// The chorus send (KitTrack::chorus): 0-127, a byte past it is damage and reads as the top.
constexpr uint8_t kChorusSendMax = 127;
// What a push on MIXER's CHO 1-4 / 5-8 sets a track's send to from OFF: half, a chorus you hear.
constexpr uint8_t kChorusSendToggle = 64;
constexpr uint8_t chorus_send_sanitized(uint8_t send) {
    return send <= kChorusSendMax ? send : kChorusSendMax;
}

// Choke groups (KitTrack::choke): 1-4, 0 none. A byte past the last group reads as none.
constexpr uint8_t kChokeGroups = 4;
constexpr uint8_t choke_group_sanitized(uint8_t group) {
    return group <= kChokeGroups ? group : uint8_t(0);
}

// LFO TRIG's values, as KIT.BIN stores them: PERMANENT. dsp/lfo.hpp's LfoTrig mirrors them and
// app/ asserts the two agree. Anything else reads as FREE.
constexpr uint8_t kLfoTrigFree = 0;
constexpr uint8_t kLfoTrigRetrig = 1;
constexpr uint8_t kLfoTrigOneShot = 2;
constexpr uint8_t kLfoTrigs = 3;

constexpr uint8_t kSliceEqual = 0;
constexpr uint8_t kSliceAuto = 1;

struct Kit {
    KitTrack tracks[kTracks];
    KitDuck duck;
};

// --- chains ---------------------------------------------------------------

// One entry of the chain: a pattern, played `repeats` times before the next entry.
//
// `repeats` of 0 means the entry is unused, which is also what zeroed memory means -- a chain
// of 64 zeroed entries is an empty chain rather than 64 infinite repeats of pattern A01.
//
// `mutes` is song mode's: 0 leaves the mutes as they are, 1-8 recalls that stored mute state
// (`ProjectSettings::mute_states`) as the entry starts, so a chain can hold sections -- an intro
// without the kick, a breakdown -- and not only patterns. Zeroed, it changes nothing.
struct ChainEntry {
    uint8_t bank = 0;
    uint8_t slot = 0;
    uint8_t repeats = 0;
    uint8_t mutes = 0;
};

// The highest `ChainEntry::mutes`: one per stored mute state.
constexpr uint8_t kChainMutesMax = 8;

// --- the project ----------------------------------------------------------

// The tempo's domain, in centi-BPM. Enaga's clock takes 20-300 BPM (docs/ENAGA_API.md), so a
// project cannot usefully hold anything else and a damaged byte is clamped into it. Here rather
// than in project.cpp because app/ sets the tempo from the same range: a UI that let the user
// past these would have the value quietly changed by the next load.
constexpr uint16_t kTempoCentiMin = 2000;     // 20.00 BPM
constexpr uint16_t kTempoCentiMax = 30000;    // 300.00 BPM

// How many send and master fields a file holds. Stated here rather than taken from
// dsp/buses.hpp's enums because core/ cannot include dsp/ -- dsp/buses.hpp reaches Enaga and
// CLAUDE.md says core/ never does. dsp/buses.hpp static_asserts that the two agree, so the day
// a field is added the build stops here rather than writing a short section.
constexpr int kSendFields = 23;
// What a PROJECT.BIN written before the ambient modes holds; the other nine follow the master's.
constexpr int kSendFieldsV1 = 10;
// And before the chorus (send C): the ambient modes' nine after the master's, no chorus bytes.
constexpr int kSendFieldsV2 = 19;
constexpr int kMasterFields = 7;
// What a PROJECT.BIN written before the kill EQ holds: the DJ filter, compressor, limiter and
// level. The three EQ bands were appended after them, so a short mixer section is an older file.
constexpr int kMasterFieldsV1 = 4;

// The delay's default division: dotted 1/8, index 5 of dsp/buses.hpp's table, as the byte an
// encoder at that position produces. Named here because `ProjectSettings` needs it as an
// initialiser and a bare 64 next to nine other numbers says nothing.
constexpr uint8_t kDefaultDelayDivision = 64;
// The LONG delay's default multiplier: X2, index 1 of dsp/buses.hpp's six, as a band's middle.
constexpr uint8_t kDefaultDelayMultiplier = 32;

// EVOLVE's saved mutation when a file has none: VELOCITY, core/mutate.hpp's Mutation::velocity.
// A number here rather than the enum because core/project.hpp does not include core/mutate.hpp;
// tests/test_project.cpp checks the two agree.
constexpr uint8_t kEvolveKindDefault = 4;
// The highest byte the kind may hold (Mutation::mix), and the highest period index.
constexpr uint8_t kEvolveKindMax = 8;
constexpr uint8_t kEvolvePeriodMax = 4;

// Settings that belong to the project rather than to a pattern.
struct ProjectSettings {
    uint16_t tempo_centi_bpm = 13000;    // 130.00 BPM; Enaga's clock takes BPM x 100
    uint8_t root = 60;
    uint8_t scale = 0;
    // No swing here. The file kept a project-wide swing that nothing read; the sequencer only
    // ever used the PATTERN's (core/pattern.hpp), so the field is gone and its byte in
    // PROJECT.BIN is reserved (core/project.cpp).
    uint8_t quantize = 100;              // the live-record quantize amount, 0-100
    // REC MODE, what a recorded note does to a step of a chord track that already plays:
    // false REPLACE (the first note of a pass takes the step over), true ADD (it joins the
    // step's notes). core/stepnotes.hpp holds the notes.
    bool rec_add = false;
    uint8_t mute_states[8] = {};         // 8 stored mute states, one bit per track

    // docs/SPEC.md: 64 MiB by default since 2026-11-23 (it was 32, raisable per project to 64).
    // Stored in MiB rather than bytes: a byte count would let a file ask for 4 GiB. The app
    // applies it as a floor of the default (app/storage.cpp), so a stored 32 means 64 today.
    uint8_t sample_budget_mib = 64;

    uint8_t chain_length = 0;            // how many chain entries are in use
    bool chain_enabled = false;
    // Song mode: the chain plays through ONCE and the transport stops at its end, back on entry
    // 1, instead of looping. Off, a chain loops as it always has.
    bool chain_song = false;

    // What was live when the project was saved: the mutes as they stood (one bit per track,
    // bit t = track t muted) and which scenes sat in the morph's slots A and B. Restored on
    // open, so a set comes back the way it was left rather than all unmuted. The engine owns
    // them while the project is open; these are its copies for the file. The scene ids are
    // kept as read and validated by the engine, which knows how many scenes there are.
    uint8_t live_mutes = 0;
    uint8_t scene_a = 0;
    uint8_t scene_b = 1;

    // Each track's MIDI OUT channel: 0 off, 1-16 the channel as printed on gear. A track with a
    // channel sends every note it plays there (core/midiout.hpp). In the PROJECT rather than the
    // kit because it is cabling -- which outboard synth listens where -- not a sound: recalling a
    // kit must not re-route the synths on stage.
    uint8_t midi_out[kTracks] = {};

    // EVOLVE (core/evolve.hpp), saved so a set comes back evolving (Roland, 2026-11-27). Each
    // track's period as kEvolvePeriod's index (0 OFF, then 1, 2, 4 or 8 loops), and the mode,
    // mutation and amount every track shares. The mutation and amount are also PERFORM's MUTATE
    // setting. HOME is not saved: a track that opens evolving takes the pattern as loaded as its
    // HOME. Bytes with the enums' values, which core/evolve.hpp and core/mutate.hpp keep put.
    uint8_t evolve_period[kTracks] = {};
    uint8_t evolve_mode = 0;             // EvolveMode: 0 ORBIT, 1 DRIFT
    uint8_t evolve_kind = kEvolveKindDefault;
    uint8_t evolve_amount = 50;

    // The two send effects and the master bus, as encoder bytes rather than as the floats the
    // mixer runs on. One byte per field, in dsp/buses.hpp's `SendField` and `MasterField`
    // order, which is why those two enums are declared permanent: these indices reach a file.
    //
    // Bytes, not floats, for three reasons. The encoder produces a byte, so a knob round-trips
    // through a save EXACTLY rather than coming back a quantisation step away. A byte cannot
    // hold a NaN or an infinity, so a damaged file cannot hand the mixer a coefficient that
    // poisons every later block. And the serializer has no float method, so storing one would
    // mean bit-casting through a buffer -- which is the thing CLAUDE.md forbids.
    //
    // dsp/buses.hpp owns the byte -> value mapping, because only it knows what each field
    // means; `delay_division` is not even a time until the tempo is known.
    // The bytes that reproduce dsp/buses.hpp's documented defaults through that mapping:
    // dotted 1/8, 40% feedback, mid tone, ping-pong on, full return, no delay into the reverb,
    // a 2 s reverb, mid damping, a 180 Hz input high-pass, full return.
    uint8_t sends[kSendFields] = {kDefaultDelayDivision, 53, 64, 127, 127, 0, 58, 64, 61, 127,
                                  // The ambient modes: off (classic), and where their knobs
                                  // start when switched on -- X2, a little wow, half diffusion,
                                  // a 120 Hz low cut; a big hall, some sweep, no shimmer.
                                  0, kDefaultDelayMultiplier, 57, 64, 58, 0, 96, 51, 0,
                                  // The chorus (send C): a 0.6 Hz sweep, half depth, full
                                  // return, none of it into the reverb. Nothing sounds until a
                                  // track sends to it (KitTrack::chorus), which no track does.
                                  69, 64, 127, 0};
    // Centre (flat) DJ filter, the glue compressor at 0.3, the limiter at 0.8, unity out, and the
    // kill EQ's three bands at their flat (bypassed) centre.
    uint8_t master[kMasterFields] = {64, 38, 101, 127, 64, 64, 64};
    // The kill EQ's kill switches, bit 0 LOW, bit 1 MID, bit 2 HIGH. Live only: PROJECT.BIN
    // keeps their byte, after the master bytes, but writes it as 0 and ignores it on read
    // (Roland, 2026-11-22), so a project never opens with a band killed.
    uint8_t eq_kills = 0;
};

struct Project {
    ProjectSettings settings;
    Kit kit;
    ChainEntry chain[kChainEntries];
    Pattern patterns[kBanks][kPatternsPerBank];

    // Which pattern is playing. The sequencer plays `live()`, in place.
    uint8_t bank = 0;
    uint8_t slot = 0;

    // Per bank, set by an edit and cleared by a save, so a save writes only what changed.
    // Not serialized: a freshly loaded project has nothing dirty.
    bool bank_dirty[kBanks] = {};

    Pattern &live() { return patterns[bank][slot]; }
    const Pattern &live() const { return patterns[bank][slot]; }
};

// About 1.03 MiB, which is a member of something long-lived and never a local: a stack frame is
// capped at 16 KiB (CLAUDE.md).
static_assert(sizeof(Pattern) * kProjectPatterns > 1000000,
              "if a Pattern shrank, the figures in docs/SPEC.md want revisiting");

// An empty project: every pattern cleared, the kit zeroed, no chain, bank A slot 1 live.
void project_init(Project &project);

// True when `bank` and `slot` name a pattern that exists. Used on every value read from a file
// and on every queued change, because an out-of-range index would read past the array.
//
// Inline, like `bank_letter` below, so that core/queue.cpp needs this HEADER and not
// core/project.cpp. The two modules are written independently and a link-time dependency
// between them would mean neither could be built until both were finished.
constexpr bool project_index_valid(int bank, int slot) {
    return bank >= 0 && bank < kBanks && slot >= 0 && slot < kPatternsPerBank;
}

// Bank letters for the screen and for file names: 'A'..'H'. '?' for an invalid index.
constexpr char bank_letter(int bank) {
    return bank >= 0 && bank < kBanks ? char('A' + bank) : '?';
}

// --- file names -----------------------------------------------------------

// Writes the bank's file name ("BANK_A.BIN") into `out`, NUL-terminated, at most `max` bytes
// including the terminator. False, with `out` untouched, for an invalid bank or too small a
// buffer. app/ joins it to the project folder; core/ does not know what a path is.
bool bank_file_name(int bank, char *out, int max);

// --- versions and sections ------------------------------------------------
//
// The container is core/serialize.hpp's (magic, version, section table, CRC-32 over the
// payload). Section ids are per FILE TYPE, so a project's ids and a pattern's ids are separate
// numbering spaces and these do not collide with kSectionPatternHeader and friends.
//
// Permanent, like the pattern's: a new section takes the next free number, and a section that
// goes away leaves its number burnt.

constexpr uint16_t kProjectVersion = 1;
constexpr uint16_t kSectionProjectSettings = 1;
constexpr uint16_t kSectionProjectChain = 2;
// The sends and the master. OPTIONAL on read: a PROJECT.BIN written before it existed has two
// sections and still loads, with the defaults above. That is what the format was built for --
// the section table is a lookup by id, so an older build reading a file with this section
// simply does not ask for it. The version therefore does NOT move: the meaning of no existing
// byte has changed, which is the only thing kProjectVersion tracks.
constexpr uint16_t kSectionProjectMixer = 3;
// The live mutes and the A/B scene choice. Optional on read for the same reason as the mixer:
// an older file without it opens with everything unmuted and scenes 1 and 2, as it always did.
constexpr uint16_t kSectionProjectLive = 4;
// Song mode: the SONG switch and each chain entry's mute recall (`ChainEntry::mutes`). Optional
// on read: an older file has a looping chain with no mute recalls, which is what zeroes mean.
constexpr uint16_t kSectionProjectSong = 5;
// Each track's MIDI OUT channel (core/midiout.hpp): eight bytes, 0 off or 1-16. Optional on read
// like the ones above: an older file has every track's MIDI out off, which is what it played.
constexpr uint16_t kSectionProjectMidi = 6;
// EVOLVE (core/evolve.hpp): each track's period and the shared mode, mutation and amount.
// Optional on read like the ones above: an older file has every track's EVOLVE off, ORBIT,
// VELOCITY at 50, which is what a fresh project has.
constexpr uint16_t kSectionProjectEvolve = 7;

constexpr uint16_t kKitVersion = 1;
constexpr uint16_t kSectionKitTracks = 1;
// The LFO's four bytes per track (core/project.hpp, kKitLfoParams). OPTIONAL on read, like the
// project's mixer section: a KIT.BIN from before the LFO has one section and loads with every
// LFO off (all four bytes 0), which is also what zeroed bytes mean to dsp/lfo.hpp. The version
// does not move: no existing byte changed its meaning.
constexpr uint16_t kSectionKitLfo = 2;
// The arpeggiator's four bytes per track (kKitArpParams), OPTIONAL in the same way: a KIT.BIN
// from before the arp loads with every arp OFF.
constexpr uint16_t kSectionKitArp = 3;
// SLC AUTO, one byte per track (KitTrack::slice_mode). Optional on read like the LFO and the
// arp: a KIT.BIN from before it loads with every track slicing equally, as slicing always did.
constexpr uint16_t kSectionKitSlices = 4;
// The VA machines' ASR page, four bytes per track (kKitAsrParams: ATK SUS REL FSU). Optional on
// read like the others: a KIT.BIN from before it loads with all four 0, which dsp/va.cpp reads as
// exactly the envelope the VAs always had (2.5 ms attack, no sustain, 60 ms release).
constexpr uint16_t kSectionKitAsr = 5;
// The sidechain duck: each track's depth, then the kit's source, attack, hold and release
// (KitTrack::duck, KitDuck). Optional on read: a KIT.BIN from before it loads with the duck as it
// was fixed before, which is what KitDuck's defaults are.
constexpr uint16_t kSectionKitDuck = 6;
// The VA machines' OSC page, four bytes per track (kKitOscParams: WAV OS2 SUB DRV). Optional on
// read: a KIT.BIN from before it loads kKitOscLegacy on every track.
constexpr uint16_t kSectionKitOsc = 7;
// The filter layout, one byte: kKitFilterLayout when the VA and WAVE tracks' slots 4-7 are FDE
// and TON (D8, machine_has_voice_filter). Optional on read: a KIT.BIN from before it holds the
// insert filter there, which loads mapped onto TON.
constexpr uint16_t kSectionKitFilter = 8;
constexpr uint8_t kKitFilterLayout = 1;
// LFO TRIG, one byte per track (KitTrack::lfo_trig). Optional on read: a KIT.BIN from before it
// loads every LFO as FREE, which is how every LFO ran before.
constexpr uint16_t kSectionKitLfoTrig = 9;
// The mod slots, 3 bytes a slot, two slots a track (KitTrack::mod). Optional on read: a KIT.BIN
// from before them loads every slot OFF, which changes no sound.
constexpr uint16_t kSectionKitMod = 10;
// The choke groups, one byte per track (KitTrack::choke). Optional on read: a KIT.BIN from before
// them loads every track in no group, which is how every track played before.
constexpr uint16_t kSectionKitChoke = 11;
// The arp's kit settings, 4 bytes a track (KitTrack::arp: VAR RHYTHM ACCENT RATCHET). Optional
// on read: a KIT.BIN from before it loads every arp with all four 0, the arp as it always played.
constexpr uint16_t kSectionKitArpExt = 12;
// The LFO layout, one byte: kLfoLayoutNumbers when WAV and DST are numbers (format decision C).
// Optional on read: a KIT.BIN without it holds the old bands, which load converted
// (kit_lfo_from_band). An older build reading a newer kit reads the numbers as low bands: SIN
// and OFF mostly, which the release notes say.
constexpr uint16_t kSectionKitLfoLayout = 13;
// The chorus sends, one byte per track (KitTrack::chorus). Optional on read: a KIT.BIN from
// before the chorus loads every track sending nothing to it, which is how every track played.
// An older build ignores the section.
constexpr uint16_t kSectionKitChorus = 14;

constexpr uint16_t kBankVersion = 1;
constexpr uint16_t kSectionBankPatterns = 1;

// --- round trips ----------------------------------------------------------
//
// Each of the three returns and reports in the same style as `pattern_write` / `pattern_read`:
// a buffer the caller owns, a result enum, and the byte count. Loading is value-tolerant but
// structure-strict, exactly as docs/SPEC.md decided for patterns -- an out-of-range tempo or a
// chain entry naming a pattern that cannot exist is clamped or dropped, while a bad magic,
// an unknown version, a bad CRC or a malformed section table is refused.

// PROJECT.BIN: settings, the chain, and which pattern was live. `project_read` touches nothing
// else -- the patterns come from the bank files, and app/ reads this file first, so a damaged
// PROJECT.BIN (a few hundred bytes) must not cost 128 patterns that are about to load.
uint32_t project_bytes();
SerializeResult project_write(const Project &project, uint8_t *buffer, uint32_t capacity,
                              uint32_t *written = nullptr);
SerializeResult project_read(Project &project, const uint8_t *buffer, uint32_t size);

// KIT.BIN: the eight tracks' sounds.
uint32_t kit_bytes();
SerializeResult kit_write(const Kit &kit, uint8_t *buffer, uint32_t capacity,
                          uint32_t *written = nullptr);
SerializeResult kit_read(Kit &kit, const uint8_t *buffer, uint32_t size);

// BANK_x.BIN: 16 patterns, each one `pattern_write`'s own container back to back, which is what
// `pattern_read`'s `consumed` was built for. A pattern that fails to read leaves that slot empty
// and the rest of the bank still loads -- one damaged pattern should cost one pattern.
//
// `kBankMaxBytes` sizes a static buffer that `bank_write` can never refuse.
constexpr uint32_t kBankMaxBytes =
    kHeaderBytes + kSectionEntryBytes + uint32_t(kPatternsPerBank) * kPatternMaxBytes;

uint32_t bank_bytes(const Project &project, int bank);
SerializeResult bank_write(const Project &project, int bank, uint8_t *buffer, uint32_t capacity,
                           uint32_t *written = nullptr);

// `refused` receives how many of the 16 patterns were damaged and left empty, which the UI shows
// rather than pretending the bank loaded cleanly. It is 0 on any result other than `ok`: the
// file itself failed rather than a pattern in it, and then all 16 slots are left empty.
//
// A successful read clears the bank's dirty flag, including for the slots it refused -- what is
// in RAM is what is in the file, and marking it dirty would let the next save overwrite a
// damaged file. Neither of these takes a bank index no pattern exists at: both report
// `malformed` for one, since SerializeResult has no value for "no such bank".
SerializeResult bank_read(Project &project, int bank, const uint8_t *buffer, uint32_t size,
                          int *refused = nullptr);

}  // namespace strata
