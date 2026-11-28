// DEEP: a deep house showcase project for STRATA, built with core/'s own types and written with
// core/'s own serializers, so the files are exactly what Storage would save. Renders it through
// the real Engine (host build, tests/fakehost.cpp clock), like host/render.cpp. Modelled on the
// TECHNO showcase's generator (build/techno-v2/techno.cpp).
//
//   deephouse OUTDIR [--no-render]   writes OUTDIR/card/ENAGA/DATA/STRATA/PROJECTS/DEEP/*.BIN
//                                    and OUTDIR/audio/*.wav
//
// Not part of the repo's build: compiled against build/host's objects. Factory machines and
// factory sounds only; every note and rhythm here was written for this project.
//
// What it shows that TECHNO does not: swing (57 on the 16ths), extended chords (min9, maj7) on
// FM4's electric piano, held VA POLY pads from the ENV page, a melody, the HALL reverb, a walking
// bass that follows the chords, and an LFO panning the keys (tremolo).
#include "chord.hpp"
#include "engine.hpp"
#include "factory.hpp"
#include "fakehost.hpp"
#include "lfo.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "scale.hpp"
#include "sounds.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>

#include <enaga/abi.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

// --- parameter slots (dsp/track.hpp's layout, as core/factory.cpp names them) ---
constexpr int kSrc0 = 0, kSrc1 = 1, kSrc2 = 2, kSrc3 = 3;
constexpr int kFlt0 = 4, kFlt1 = 5;                 // VA: FDE TON; drums: DRV CUT
constexpr int kAmp0 = 8, kAmp1 = 9, kAmp2 = 10, kAmp3 = 11;
constexpr int kPan = 13, kSna = 14, kSnb = 15;
constexpr int kLfoWav = 16, kLfoRat = 17, kLfoDep = 18, kLfoDst = 19;
constexpr int kAtk = 24, kSus = 25, kRel = 26;
constexpr int kOscWav = 28, kOscOs2 = 29, kOscSub = 30, kOscDrv = 31;

enum Track : int { kKick = 0, kClap, kOpen, kShaker, kConga, kBass, kKeys, kPad };

// A minor. Drums play C4 (their neutral note).
constexpr uint8_t D = 60;
// Bass roots, octave 2.
constexpr uint8_t A1 = 33, C2 = 36, D2 = 38, E2 = 40, F2 = 41, G2 = 43, A2 = 45;
// Chord roots for the keys and the pad.
constexpr uint8_t D3 = 50, E3 = 52, F3 = 53, A3 = 57;

constexpr int kTempoCenti = 12200;

// Lengths: core/timing.hpp's note-length table (8 = one step).
constexpr uint8_t L1 = 8, L1h = 9, L2 = 10, L3 = 11, L4 = 12, L8 = 14, L16 = 16;

// The four chords: Am9 Dm9 Fmaj7 Em7, one a bar.
struct Bar { uint8_t bass_root; uint8_t chord_root; ChordType chord; };
constexpr Bar kProgression[4] = {
    {A2, A3, ChordType::min9},
    {D2, D3, ChordType::min9},
    {F2, F3, ChordType::maj7},
    {E2, E3, ChordType::min7},
};

void apply(KitTrack &k, uint8_t machine, int preset) {
    k.machine = machine;
    sound_preset_apply(machine, preset, k.params);
}

int preset_index(uint8_t machine, const char *name) {
    for (int i = 0; i < sound_preset_count(machine); i++)
        if (std::strcmp(sound_preset(machine, i)->name, name) == 0) return i;
    std::fprintf(stderr, "no preset %s\n", name);
    std::exit(1);
}

// --- the kit ---
void deep_kit(Kit &kit) {
    const uint8_t KICK = uint8_t(MachineId::kick), SN = uint8_t(MachineId::snare),
                  HAT = uint8_t(MachineId::hat), PERC = uint8_t(MachineId::perc),
                  MONO = uint8_t(MachineId::va_mono), POLY = uint8_t(MachineId::va_poly),
                  FM = uint8_t(MachineId::fm);
    for (int t = 0; t < kTracks; t++) {
        KitTrack &k = kit.tracks[t];
        for (int i = 0; i < kKitParams; i++) k.params[i] = 0;
        k.params[kPan] = 64;
        k.sample[0] = '\0';
        k.slice_mode = kSliceEqual;
        k.level = 1.0f;
    }

    // KICK: DEEP, tuned to A1 (TUN 64 = 55 Hz), round and a little long: the deep house thud,
    // softer at the top than TECHNO's driven 909.
    apply(kit.tracks[kKick], KICK, preset_index(KICK, "DEEP"));
    kit.tracks[kKick].params[kSrc0] = 64;
    kit.tracks[kKick].params[kAmp0] = 84;

    // CLAP: the classic clap, a touch longer, into the reverb.
    apply(kit.tracks[kClap], SN, preset_index(SN, "CLAP"));
    kit.tracks[kClap].params[kSrc2] = 64;      // TON as heard (CLAP's was 64 before the preset review)
    kit.tracks[kClap].params[kAmp0] = 60;
    kit.tracks[kClap].params[kAmp3] = 84;
    kit.tracks[kClap].params[kSnb] = 52;

    // OPEN hat on track 3 (it chokes track 4, the shaker: harmless, the shaker is short).
    apply(kit.tracks[kOpen], HAT, preset_index(HAT, "OPEN"));
    kit.tracks[kOpen].params[kSrc1] = 88;      // TON: softer than the factory open
    kit.tracks[kOpen].params[kAmp0] = 56;      // DEC
    kit.tracks[kOpen].params[kAmp3] = 58;
    kit.tracks[kOpen].params[kPan] = 72;
    kit.tracks[kOpen].params[kSnb] = 20;

    // SHAKER: the 16ths, swung. Leans the other way from the open hat.
    apply(kit.tracks[kShaker], HAT, preset_index(HAT, "SHAKE"));
    kit.tracks[kShaker].params[kAmp3] = 70;
    kit.tracks[kShaker].params[kPan] = 54;

    // CONGA: the FM perc's conga, pitched by locks into a two-drum figure, a little delay.
    apply(kit.tracks[kConga], PERC, preset_index(PERC, "CONGA"));
    kit.tracks[kConga].params[kAmp3] = 80;
    kit.tracks[kConga].params[kPan] = 46;
    kit.tracks[kConga].params[kSna] = 24;
    kit.tracks[kConga].params[kSnb] = 26;

    // BASS: VA MONO's FAT, darker and rounder: the sub carries it, the saws only colour the
    // attack. SUS holds each note for its step's LEN, so the rhythm is in the lengths.
    apply(kit.tracks[kBass], MONO, preset_index(MONO, "FAT"));
    kit.tracks[kBass].params[kSrc2] = 34;      // CUT
    kit.tracks[kBass].params[kSrc3] = 28;      // RES
    kit.tracks[kBass].params[kAmp1] = 36;      // ENV: a soft pluck
    kit.tracks[kBass].params[kRel] = 14;       // REL: short, the notes stay apart
    kit.tracks[kBass].params[kOscDrv] = 40;
    kit.tracks[kBass].level = 1.3f;

    // KEYS: FM4's EPIANO, the deep house Rhodes. A synced sine on PAN (a half note) is the
    // electric piano's tremolo, gentle; a little delay and reverb.
    apply(kit.tracks[kKeys], FM, preset_index(FM, "EPIANO"));
    // EPIANO as heard: IDX FBK and IEN VEL from before the preset review.
    kit.tracks[kKeys].params[kSrc2] = 50;
    kit.tracks[kKeys].params[kSrc3] = 0;
    kit.tracks[kKeys].params[kAmp1] = 100;
    kit.tracks[kKeys].params[kAmp2] = 90;
    kit.tracks[kKeys].params[kAmp0] = 84;      // DEC: shorter, so a chord is gone before the next
    kit.tracks[kKeys].params[kSna] = 44;
    kit.tracks[kKeys].params[kSnb] = 56;
    kit.tracks[kKeys].params[kLfoWav] = lfo_wave_byte(LfoWave::sine);
    kit.tracks[kKeys].params[kLfoRat] = uint8_t(kLfoSyncFirst + 4 * 5);   // 1/2 (dsp/lfo.cpp)
    kit.tracks[kKeys].params[kLfoDep] = 34;
    kit.tracks[kKeys].params[kLfoDst] = lfo_dest_byte(kPan);

    // PAD: VA POLY's WARM, a bar long, swelling in, into the HALL. Release shortened so one
    // bar's chord has gone before the next bar's has fully grown (the 12-voice budget).
    apply(kit.tracks[kPad], POLY, preset_index(POLY, "WARM"));
    kit.tracks[kPad].params[kSrc2] = 46;       // CUT: under the keys
    kit.tracks[kPad].params[kAtk] = 62;
    kit.tracks[kPad].params[kRel] = 66;
    kit.tracks[kPad].params[kAmp3] = 70;       // LVL
    kit.tracks[kPad].params[kSnb] = 84;
    kit.tracks[kPad].params[kLfoWav] = lfo_wave_byte(LfoWave::triangle);
    kit.tracks[kPad].params[kLfoRat] = uint8_t(kLfoSyncFirst + 4 * 1);    // 4B
    kit.tracks[kPad].params[kLfoDep] = 20;
    kit.tracks[kPad].params[kLfoDst] = lfo_dest_byte(kSrc2);

    // The sidechain: gentler than TECHNO's. The bass and the pad breathe with the kick.
    const uint8_t duck[kTracks] = {0, 0, 10, 10, 10, 40, 20, 45};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 3;
    kit.duck.hold = 8;
    kit.duck.release = 50;
}

void deep_mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    // Delay: dotted 1/8, moderate feedback, dark, ping-pong, a little into the reverb.
    x[0] = kDefaultDelayDivision;
    x[1] = 58; x[2] = 44; x[3] = 127; x[4] = 110; x[5] = 36;
    // Reverb: the HALL, long and dark, some sweep, no shimmer.
    x[6] = 80; x[7] = 82; x[8] = 64; x[9] = 116;
    x[15] = 127;      // reverb_mode: HALL
    x[16] = 104;      // reverb_size
    x[17] = 60;       // reverb_mod
    x[18] = 0;        // reverb_shimmer
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 46;   // glue
    s.master[3] = 118;
}

// --- patterns ---
Step &trig(Pattern &p, int track, int step, uint8_t note, uint8_t velocity, uint8_t length = L1) {
    Step &s = p.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = note;
    s.velocity = velocity;
    s.length = length;
    return s;
}

void begin(Pattern &p, uint32_t seed, uint8_t swing) {
    pattern_init(p);
    p.root = 9;                         // A
    p.scale = uint8_t(Scale::minor);
    p.seed = seed;
    p.master_length = 64;
    p.swing = swing;
    for (int t = 0; t < kTracks; t++) {
        p.tracks[t].length = 64;
        p.tracks[t].speed = uint8_t(Speed::x1);
    }
}

void kicks(Pattern &p, uint8_t vel, int bars = 4) {
    for (int i = 0; i < bars * 4; i++) trig(p, kKick, i * 4, D, i % 4 == 0 ? 120 : vel);
}

void claps(Pattern &p, uint8_t vel) {
    for (int b = 0; b < 4; b++) {
        trig(p, kClap, b * 16 + 4, D, vel);
        trig(p, kClap, b * 16 + 12, D, uint8_t(vel + 4));
    }
}

// The shaker: every 16th, the beat and the offbeat 8th louder, the "e" and the "a" soft.
void shaker(Pattern &p, uint8_t beat, uint8_t soft, int from = 0, int to = 64) {
    static const uint8_t shape[4] = {0, 2, 1, 3};   // beat, e, and, a
    for (int i = from; i < to; i++) {
        const int r = shape[i & 3];
        const uint8_t v = r == 0 ? beat : (r == 1 ? uint8_t(beat - 8) : soft);
        trig(p, kShaker, i, D, v);
    }
}

void open_hats(Pattern &p, uint8_t vel) {
    for (int i = 2; i < 64; i += 4) trig(p, kOpen, i, D, vel);
}

// The conga: a two-drum figure over two bars, high and low by a TUN lock.
void congas(Pattern &p, bool busy) {
    struct N { int step; uint8_t vel; uint8_t tun; };
    static const N fig[] = {
        {3, 84, 52}, {6, 70, 40}, {10, 90, 52}, {11, 60, 40}, {14, 76, 40},
        {19, 84, 52}, {22, 70, 40}, {26, 88, 52}, {29, 64, 52}, {30, 80, 40},
    };
    for (int half = 0; half < 2; half++) {
        for (const N &n : fig) {
            const int at = half * 32 + n.step;
            trig(p, kConga, at, D, n.vel);
            pattern_set_lock(p, kConga, at, uint8_t(kSrc0), n.tun);
        }
    }
    if (busy) {
        for (int at : {15, 47}) {
            Step &s = trig(p, kConga, at, D, 66);
            s.probability = 70;
            pattern_set_lock(p, kConga, at, uint8_t(kSrc0), 58);
        }
    }
}

// The bass follows the chords: the root off the kick, an octave pop, the fifth, and a slide
// into the next bar's root.
void bassline(Pattern &p, bool busy) {
    for (int b = 0; b < 4; b++) {
        const uint8_t r = kProgression[b].bass_root;
        const uint8_t next = kProgression[(b + 1) & 3].bass_root;
        const int at = b * 16;
        trig(p, kBass, at + 2, r, 100, L2);
        trig(p, kBass, at + 6, r, 76, L1);
        trig(p, kBass, at + 7, uint8_t(r + 12), 88, L1h).flags |= kStepAccent;
        trig(p, kBass, at + 10, r, 100, L2);
        if (busy) trig(p, kBass, at + 13, uint8_t(r + 7), 74, L1);
        Step &s = trig(p, kBass, at + 15, next, 84, L1);
        s.flags |= kStepSlide;
    }
}

// The keys: the chord on the 16th after the beat's "and", answered, then held.
void keys_chords(Pattern &p, uint8_t vel, bool busy) {
    for (int b = 0; b < 4; b++) {
        const Bar &bar = kProgression[b];
        const int at = b * 16;
        Step &a = trig(p, kKeys, at + 3, bar.chord_root, vel, L2);
        a.chord = chord_pack(bar.chord, 0);
        Step &c = trig(p, kKeys, at + 6, bar.chord_root, uint8_t(vel - 16), L1h);
        c.chord = chord_pack(bar.chord, 0);
        Step &d = trig(p, kKeys, at + 11, bar.chord_root, uint8_t(vel - 6), L3);
        d.chord = chord_pack(bar.chord, 0);
        if (busy && (b & 1)) {
            Step &e = trig(p, kKeys, at + 14, bar.chord_root, uint8_t(vel - 20), L1);
            e.chord = chord_pack(bar.chord, 0);
        }
    }
}

// The melody: four bars over the chords, for the break. Single notes on the keys.
void keys_melody(Pattern &p) {
    struct N { int step; uint8_t note; uint8_t vel; uint8_t len; };
    static const N line[] = {
        {0, 76, 96, L3}, {3, 79, 80, L2}, {6, 81, 92, L4}, {12, 79, 74, L2}, {14, 76, 70, L1h},
        {16, 77, 94, L3}, {19, 76, 78, L2}, {22, 74, 90, L4}, {28, 72, 72, L3},
        {32, 72, 90, L2}, {35, 76, 80, L2}, {38, 79, 92, L4}, {44, 81, 84, L2}, {46, 84, 96, L2},
        {48, 83, 98, L4}, {54, 79, 80, L2}, {56, 76, 86, L8},
    };
    for (const N &n : line) {
        trig(p, kKeys, n.step, n.note, n.vel, n.len);
        pattern_set_lock(p, kKeys, n.step, uint8_t(kAmp3), 112);   // LVL: one note is quieter than a chord
    }
}

void pad_chords(Pattern &p, uint8_t vel) {
    for (int b = 0; b < 4; b++) {
        const Bar &bar = kProgression[b];
        // The pad plays the chord's four lowest notes: min7 for min9 (the keys have the ninth).
        const ChordType type = bar.chord == ChordType::min9 ? ChordType::min7 : bar.chord;
        Step &s = trig(p, kPad, b * 16, uint8_t(bar.chord_root + 12), vel, L16);
        s.chord = chord_pack(type, 0);
    }
}

void clap_fill(Pattern &p) {
    for (int i = 60; i < 64; i++) {
        Step &s = trig(p, kClap, i, D, uint8_t(60 + (i - 60) * 10));
        s.condition = condition_pack(Condition::FILL);
        if (i == 63) s.ratchet = ratchet_pack(3, RatchetRamp::up);
    }
}

constexpr uint8_t kSwing = 57;

// A01 INTRO: kick, shaker, conga. The open hat comes in for the fourth bar.
void intro(Pattern &p) {
    begin(p, 0xDEE01u, kSwing);
    kicks(p, 112);
    shaker(p, 70, 44);
    congas(p, false);
    for (int i = 50; i < 64; i += 4) trig(p, kOpen, i, D, 70);
    clap_fill(p);
}

// A02 GROOVE: clap and open hats, and the bass.
void groove(Pattern &p) {
    begin(p, 0xDEE02u, kSwing);
    kicks(p, 116);
    claps(p, 92);
    shaker(p, 76, 48);
    open_hats(p, 82);
    congas(p, false);
    bassline(p, false);
    clap_fill(p);
}

// A03 KEYS: the chords arrive.
void main_keys(Pattern &p) {
    begin(p, 0xDEE03u, kSwing);
    kicks(p, 118);
    claps(p, 96);
    shaker(p, 80, 50);
    open_hats(p, 86);
    congas(p, true);
    bassline(p, true);
    keys_chords(p, 96, false);
    clap_fill(p);
}

// A04 BREAK: no kick, no bass. The pad swells, the melody on the keys into the delay, the
// shaker alone, and a kick drum roll-in on the last beat.
void breakdown(Pattern &p) {
    begin(p, 0xDEE04u, kSwing);
    pad_chords(p, 92);
    keys_melody(p);
    for (const int step : {0, 16, 32, 48}) {
        pattern_set_lock(p, kKeys, step, uint8_t(kSna), 80);   // the first note of a bar echoes
    }
    shaker(p, 60, 36, 0, 48);
    shaker(p, 64, 44, 48, 64);
    for (int i = 48; i < 64; i += 4) trig(p, kOpen, i + 2, D, uint8_t(50 + (i - 48) * 2));
    for (int i = 60; i < 64; i++) trig(p, kKick, i, D, uint8_t(70 + (i - 60) * 12));
    clap_fill(p);
}

// A05 PEAK: everything. The pad under the keys, the busy conga and bass.
void peak(Pattern &p) {
    begin(p, 0xDEE05u, kSwing);
    kicks(p, 120);
    claps(p, 100);
    shaker(p, 84, 54);
    open_hats(p, 90);
    congas(p, true);
    bassline(p, true);
    keys_chords(p, 100, true);
    pad_chords(p, 62);
    clap_fill(p);
}

// A06 OUTRO: kick, shaker, bass, the open hats: a DJ tool to mix out of.
void outro(Pattern &p) {
    begin(p, 0xDEE06u, kSwing);
    kicks(p, 116);
    shaker(p, 74, 46);
    open_hats(p, 78);
    bassline(p, false);
    congas(p, false);
}

constexpr uint8_t mutes(std::initializer_list<int> tracks) {
    uint8_t bits = 0;
    for (int t : tracks) bits = uint8_t(bits | (1u << t));
    return bits;
}

void deep_mutes(ProjectSettings &s) {
    const uint8_t states[8] = {
        0,                                             // 1 everything
        mutes({kKick, kBass}),                         // 2 the break: no kick, no bass
        mutes({kKeys, kPad}),                          // 3 drums and bass
        mutes({kClap, kOpen, kConga, kKeys, kPad}),    // 4 kick, shaker, bass
        mutes({kOpen, kShaker}),                       // 5 no hats
        mutes({kBass}),                                // 6 no bass
        mutes({kKick, kClap, kOpen, kShaker, kConga, kBass}),   // 7 keys and pad alone
        mutes({kPad}),                                 // 8 no pad
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
}

void deep_scenes(SceneSet &scenes) {
    // Scene 2 DUB: the keys and conga thrown into the sends, the clap into the hall.
    Scene &dub = scenes.scenes[1];
    scene_set(dub, kKeys, kSna, 104);
    scene_set(dub, kKeys, kSnb, 96);
    scene_set(dub, kConga, kSna, 90);
    scene_set(dub, kClap, kSnb, 90);
    scene_set(dub, kOpen, kAmp0, 76);

    // Scene 3 DARK: the pad and bass close, the hats lose their top, the keys soften.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kPad, kSrc2, 26);
    scene_set(dark, kBass, kSrc2, 22);
    scene_set(dark, kShaker, kSrc1, 84);
    scene_set(dark, kOpen, kSrc1, 70);
    scene_set(dark, kKeys, kSrc2, 30);

    // Scene 4 OPEN: the pad and bass open, the keys brighter, the hats ring.
    Scene &open = scenes.scenes[3];
    scene_set(open, kPad, kSrc2, 84);
    scene_set(open, kBass, kSrc2, 60);
    scene_set(open, kBass, kAmp1, 70);
    scene_set(open, kKeys, kSrc2, 80);
    scene_set(open, kOpen, kAmp0, 80);
}

void target(Macro &m, int slot, const Kit &kit, int track, int param, uint8_t high) {
    MacroTarget &t = m.targets[slot];
    t.enabled = 1;
    t.track = uint8_t(track);
    t.param = uint8_t(param);
    t.low = kit.tracks[track].params[param];
    t.high = high;
}

void deep_macros(MacroSet &macros, const Kit &kit) {
    Macro &bright = macros.macros[1];           // B BRIGHT: the pad, keys and bass open
    target(bright, 0, kit, kPad, kSrc2, 96);
    target(bright, 1, kit, kKeys, kSrc2, 86);
    target(bright, 2, kit, kBass, kSrc2, 64);
    target(bright, 3, kit, kShaker, kSrc1, 124);
    Macro &dub = macros.macros[2];              // C DUB
    target(dub, 0, kit, kKeys, kSna, 110);
    target(dub, 1, kit, kKeys, kSnb, 104);
    target(dub, 2, kit, kConga, kSna, 96);
    target(dub, 3, kit, kClap, kSnb, 100);
    Macro &deep = macros.macros[3];             // D DEEP: a longer kick and bass, a longer pad
    target(deep, 0, kit, kKick, kAmp0, 110);
    target(deep, 1, kit, kBass, kRel, 60);
    target(deep, 2, kit, kPad, kRel, 100);
    target(deep, 3, kit, kKeys, kAmp0, 110);
}

void deep_chain(Project &project) {
    // Intro 8 bars, groove 8, keys 16, break 8, peak 16, keys 8, outro 8: 72 bars, 2:22 at 122.
    const ChainEntry chain[] = {
        {0, 0, 2, 0}, {0, 1, 2, 0}, {0, 2, 4, 0}, {0, 3, 2, 0}, {0, 4, 4, 0}, {0, 2, 2, 0}, {0, 5, 2, 0},
    };
    const int n = int(sizeof chain / sizeof chain[0]);
    for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
    for (int i = 0; i < n; i++) project.chain[i] = chain[i];
    project.settings.chain_length = uint8_t(n);
    project.settings.chain_enabled = true;    // opens ready to play the arrangement
    project.settings.chain_song = false;
}

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    deep_kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 69;                                  // A
    s.scale = uint8_t(Scale::minor);
    deep_mixer(s);
    deep_mutes(s);
    s.live_mutes = 0;
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    intro(bank[0]);
    groove(bank[1]);
    main_keys(bank[2]);
    breakdown(bank[3]);
    peak(bank[4]);
    outro(bank[5]);
    deep_chain(project);
    deep_scenes(scenes);
    deep_macros(macros, project.kit);
    project.bank = 0;
    project.slot = 0;
}

// --- files ---
static uint8_t g_buf[kBankMaxBytes + 65536];
static Project g_project, g_check;
static SceneSet g_scenes, g_scenes_check;
static MacroSet g_macros, g_macros_check;

void mkdirs(const char *path) {
    char tmp[512];
    std::snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

void save(const char *dir, const char *leaf, uint32_t n) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = std::fopen(path, "wb");
    if (!f || std::fwrite(g_buf, 1, n, f) != n) { std::fprintf(stderr, "write %s\n", path); std::exit(1); }
    std::fclose(f);
    std::printf("wrote %-12s %6u bytes\n", leaf, n);
}

#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "CHECK FAILED: %s\n", what); std::exit(1); } } while (0)

void write_and_verify(const char *dir) {
    mkdirs(dir);
    uint32_t n = 0;
    CHECK(project_write(g_project, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "project_write");
    save(dir, "PROJECT.BIN", n);
    project_init(g_check);
    CHECK(project_read(g_check, g_buf, n) == SerializeResult::ok, "project_read");
    CHECK(g_check.settings.tempo_centi_bpm == kTempoCenti, "tempo");
    CHECK(g_check.settings.chain_length == g_project.settings.chain_length, "chain");
    for (int i = 0; i < 8; i++) CHECK(g_check.settings.mute_states[i] == g_project.settings.mute_states[i], "mutes");
    for (int i = 0; i < kSendFields; i++) CHECK(g_check.settings.sends[i] == g_project.settings.sends[i], "sends");

    CHECK(kit_write(g_project.kit, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "kit_write");
    save(dir, "KIT.BIN", n);
    CHECK(kit_read(g_check.kit, g_buf, n) == SerializeResult::ok, "kit_read");
    for (int t = 0; t < kTracks; t++) {
        CHECK(g_check.kit.tracks[t].machine == g_project.kit.tracks[t].machine, "machine");
        CHECK(std::memcmp(g_check.kit.tracks[t].params, g_project.kit.tracks[t].params, kKitParams) == 0, "params");
        CHECK(g_check.kit.tracks[t].duck == g_project.kit.tracks[t].duck, "duck");
        CHECK(std::fabs(g_check.kit.tracks[t].level - g_project.kit.tracks[t].level) < 0.01f, "level");
    }

    CHECK(perform_write(g_scenes, g_macros, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "perform_write");
    save(dir, "PERFORM.BIN", n);
    CHECK(perform_read(g_scenes_check, g_macros_check, g_buf, n) == SerializeResult::ok, "perform_read");
    CHECK(std::memcmp(&g_scenes_check, &g_scenes, sizeof g_scenes) == 0, "scenes");

    for (int b = 0; b < kBanks; b++) {
        char leaf[16];
        CHECK(bank_file_name(b, leaf, sizeof leaf), "bank name");
        CHECK(bank_write(g_project, b, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "bank_write");
        save(dir, leaf, n);
        int refused = -1;
        CHECK(bank_read(g_check, b, g_buf, n, &refused) == SerializeResult::ok && refused == 0, "bank_read");
    }
    for (int slot = 0; slot < 6; slot++) {
        const Pattern &a = g_project.patterns[0][slot];
        const Pattern &c = g_check.patterns[0][slot];
        CHECK(a.swing == c.swing, "swing");
        for (int t = 0; t < kTracks; t++) {
            CHECK(a.tracks[t].length == c.tracks[t].length, "track length");
            for (int i = 0; i < kStepsPerTrack; i++) {
                const Step &x = a.tracks[t].steps[i], &y = c.tracks[t].steps[i];
                CHECK(x.flags == y.flags && x.note == y.note && x.velocity == y.velocity &&
                      x.length == y.length && x.condition == y.condition && x.ratchet == y.ratchet &&
                      x.chord == y.chord && x.lock_count == y.lock_count && x.probability == y.probability,
                      "step");
            }
        }
    }
}

// --- rendering (host/render.cpp's loop, at this project's tempo) ---
Engine g_engine;
constexpr int kBlock = int(ENAGA_BLOCK_FRAMES);
constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kMaxFrames = 48000 * 240;
static float g_block[2 * kBlock];
static int16_t g_pcm[2 * kMaxFrames];

void post(Command c) { g_engine.commands().post(c); }
void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0) {
    Command c; c.cmd = cmd; c.track = track; c.step = step; c.a = a; c.b = b; post(c);
}

void le16(FILE *f, uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); }
void le32(FILE *f, uint32_t v) { le16(f, uint16_t(v)); le16(f, uint16_t(v >> 16)); }
void write_wav(const char *path, int frames) {
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
    const uint32_t data = uint32_t(frames) * 4u;
    std::fwrite("RIFF", 1, 4, f); le32(f, 36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 2); le32(f, 48000); le32(f, 48000 * 4); le16(f, 4); le16(f, 16);
    std::fwrite("data", 1, 4, f); le32(f, data);
    for (int i = 0; i < 2 * frames; i++) le16(f, uint16_t(g_pcm[i]));
    std::fclose(f);
}

void load_into_engine(bool chain_on) {
    post(Cmd::stop, 0, 0, 1);
    for (int i = 0; i < 3000; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    Project &p = g_engine.project_for_load();
    std::memcpy(&p, &g_project, sizeof p);
    p.settings.chain_enabled = chain_on;
    g_engine.scenes_for_load() = g_scenes;
    g_engine.macros_for_load() = g_macros;
    g_engine.performance_changed();
    post(Cmd::project_loaded);
    for (int i = 0; i < 20; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
}

struct Event { int bar; Cmd cmd; uint8_t track, a, b; uint16_t morph; };
struct Stats { double peak, rms, balance_db; };

Stats play(int slot, int bars, int tail_bars, int *frames_out, const Event *events, int n_events,
           int solo = -1) {
    FakeHost &host = FakeHost::the();
    Project &p = g_engine.project_for_load();
    p.bank = 0;
    p.slot = uint8_t(slot);
    post(Cmd::project_loaded);
    for (int t = 0; t < kTracks; t++) post(Cmd::set_mute, uint8_t(t), 0, uint8_t(solo >= 0 && t != solo));
    for (int i = 0; i < 20; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    const float bpm = float(kTempoCenti) / 100.0f;
    host.set_tempo(bpm);
    host.set_running(true);
    const double fpt = double(kRate) * 60.0 / (double(bpm) * 96.0);
    const uint32_t ticks = uint32_t(bars) * 384u;
    const int frames = int(double(ticks) * fpt) + int(kRate * 60.0f / bpm * 4.0f * float(tail_bars));
    int done = 0, ev = 0;
    uint32_t next_tick = 0;
    double peak = 0, sum = 0, sum_l = 0, sum_r = 0;
    while (done + kBlock <= frames && done + kBlock <= kMaxFrames) {
        const int bar_now = int(double(done) / (fpt * 384.0));
        while (ev < n_events && events[ev].bar <= bar_now) {
            const Event &e = events[ev++];
            Command c; c.cmd = e.cmd; c.track = e.track; c.a = e.a; c.b = e.b;
            c.u = e.morph;
            post(c);
        }
        while (next_tick < ticks) {
            const double at = double(next_tick) * fpt;
            if (at >= double(done + kBlock)) break;
            host.deliver_tick(next_tick, uint32_t(at - double(done)));
            next_tick++;
        }
        for (float &x : g_block) x = 0.0f;
        g_engine.process(nullptr, g_block, uint32_t(kBlock));
        for (int i = 0; i < 2 * kBlock; i++) {
            const float x = g_block[i];
            peak = std::fmax(peak, std::fabs(double(x)));
            sum += double(x) * double(x);
            ((i & 1) ? sum_r : sum_l) += double(x) * double(x);
            const float c = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
            g_pcm[2 * done + i] = int16_t(std::lround(c * 32767.0f));
        }
        done += kBlock;
    }
    host.set_running(false);
    post(Cmd::stop, 0, 0, 1);
    for (int i = 0; i < 3000; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    *frames_out = done;
    const double balance = sum_l > 0 && sum_r > 0 ? 10.0 * std::log10(sum_l / sum_r) : 0.0;
    return {peak, std::sqrt(sum / double(2 * (done ? done : 1))), balance};
}

double db(double x) { return x > 0 ? 20.0 * std::log10(x) : -999.0; }

constexpr double kMaxBalanceDb = 3.0;
int g_balance_failures = 0;
void check_balance(const char *what, const Stats &s) {
    const bool ok = std::fabs(s.balance_db) <= kMaxBalanceDb;
    std::printf("    balance L-R %+5.2f dB %s\n", s.balance_db, ok ? "ok" : "OFF CENTRE");
    if (!ok) {
        std::fprintf(stderr, "%s: off centre by %+.2f dB\n", what, s.balance_db);
        g_balance_failures++;
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR [--no-render]\n", argv[0]); return 2; }
    const char *out = argv[1];
    build(g_project, g_scenes, g_macros);

    char dir[512];
    std::snprintf(dir, sizeof dir, "%s/card/ENAGA/DATA/STRATA/PROJECTS/DEEP", out);
    write_and_verify(dir);
    std::printf("files verified (read back with core/'s readers)\n");
    if (argc > 2 && std::strcmp(argv[2], "--no-render") == 0) return 0;

    FakeHost::the().reset();
    if (!g_engine.init(kRate)) { std::fprintf(stderr, "engine init failed\n"); return 1; }
    char path[512];
    std::snprintf(path, sizeof path, "%s/audio", out);
    mkdirs(path);

    static const char *names[6] = {"INTRO", "GROOVE", "KEYS", "BREAK", "PEAK", "OUTRO"};
    load_into_engine(false);
    for (int slot = 0; slot < 6; slot++) {
        int frames = 0;
        const Stats s = play(slot, 8, 1, &frames, nullptr, 0);
        std::snprintf(path, sizeof path, "%s/audio/A%02d_%s.wav", out, slot + 1, names[slot]);
        write_wav(path, frames);
        std::printf("A%02d %-6s peak %6.1f dBFS rms %6.1f dBFS\n", slot + 1, names[slot], db(s.peak), db(s.rms));
        check_balance(names[slot], s);
    }
    static const char *tracks[kTracks] = {"KICK", "CLAP", "OPEN", "SHAKER", "CONGA", "BASS", "KEYS", "PAD"};
    for (int stem_slot : {3, 4}) {
        for (int t = 0; t < kTracks; t++) {
            int frames = 0;
            const Stats s = play(stem_slot, 4, 1, &frames, nullptr, 0, t);
            std::snprintf(path, sizeof path, "%s/audio/STEM_A%02d_T%d_%s.wav", out, stem_slot + 1, t + 1, tracks[t]);
            write_wav(path, frames);
            std::printf("A%02d T%d %-6s peak %6.1f dBFS rms %6.1f dBFS  L-R %+5.2f dB\n", stem_slot + 1, t + 1,
                        tracks[t], db(s.peak), db(s.rms), s.balance_db);
        }
    }

    // The arrangement: the chain on, with performance moves that each come back to rest.
    load_into_engine(true);
    const uint16_t full = kMorphScale;
    const Event moves[] = {
        // bar 7 and 15: FILL into the next part
        {7, Cmd::set_fill, 0, 1, 0, 0},
        {8, Cmd::set_fill, 0, 0, 0, 0},
        {15, Cmd::set_fill, 0, 1, 0, 0},
        {16, Cmd::set_fill, 0, 0, 0, 0},
        // KEYS, bars 24-31: morph towards DUB (scene B) and back, the keys into the echoes
        {24, Cmd::set_morph, 0, 0, 0, uint16_t(full / 3)},
        {26, Cmd::set_morph, 0, 0, 0, uint16_t(full * 2 / 3)},
        {29, Cmd::set_morph, 0, 0, 0, uint16_t(full / 3)},
        {31, Cmd::set_morph, 0, 0, 0, 0},
        {31, Cmd::set_fill, 0, 1, 0, 0},
        {32, Cmd::set_fill, 0, 0, 0, 0},
        // BREAK into PEAK: macro B BRIGHT rises over the last four bars, back down in the drop's
        // first bars
        {36, Cmd::set_macro, 0, 1, 30, 0},
        {37, Cmd::set_macro, 0, 1, 60, 0},
        {38, Cmd::set_macro, 0, 1, 90, 0},
        {39, Cmd::set_macro, 0, 1, 110, 0},
        {39, Cmd::set_fill, 0, 1, 0, 0},
        {40, Cmd::set_fill, 0, 0, 0, 0},
        {42, Cmd::set_macro, 0, 1, 70, 0},
        {44, Cmd::set_macro, 0, 1, 30, 0},
        {46, Cmd::set_macro, 0, 1, 0, 0},
        // PEAK's second half: keys and pad alone for two bars, then everything back
        {52, Cmd::mute_recall, 0, 6, 0, 0},
        {54, Cmd::mute_recall, 0, 0, 0, 0},
        {55, Cmd::set_fill, 0, 1, 0, 0},
        {56, Cmd::set_fill, 0, 0, 0, 0},
    };
    int frames = 0;
    const Stats s = play(0, 72, 3, &frames, moves, int(sizeof moves / sizeof moves[0]));
    std::snprintf(path, sizeof path, "%s/audio/DEEP_ARRANGEMENT.wav", out);
    write_wav(path, frames);
    std::printf("ARRANGEMENT %.1f s peak %6.1f dBFS rms %6.1f dBFS\n", frames / 48000.0, db(s.peak), db(s.rms));
    check_balance("ARRANGEMENT", s);
    return g_balance_failures == 0 ? 0 : 1;
}
