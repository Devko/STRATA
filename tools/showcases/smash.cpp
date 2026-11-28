// SMASH: drum chaos first. 155 BPM, F minor. Six of the eight tracks are drums: the break on
// KICK and SNARE, ghost snares in random order, hats at double speed shuffled, a metal perc on a
// 7-step loop, and rolls and ratchets everywhere: up to 8-hit ratchets, rolls over LEN from 1/16
// to 1/64, A:B variations so no two passes match. On the melodic side only a driven reese bass
// and a sparse pluck in the echo. The arrangement adds beat repeats (the REPEAT pad) at 1/16 and
// 1/32. Factory machines and sounds only; every note written here.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "SMASH";
constexpr uint16_t kTempoCenti = 15500;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"BREAK", "SHRED", "ROLLS", "STUTTR", "SMASH", "HALF"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kGhost, kBass, kPluck, kMetal };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "GHOST", "BASS", "PLUCK", "METAL"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 5;     // F
constexpr uint8_t kSwing = 52;
constexpr uint8_t F1 = 29, Ab1 = 32, Bb1 = 34, C2 = 36, Db2 = 37, Eb2 = 39, F2 = 41;

constexpr uint8_t r64 = 0, r48 = 1, r32 = 2, r16t = 3, r16 = 4;

void kit(Kit &kit) {
    clear_kit(kit);
    preset(kit.tracks[kKick], MachineId::kick, "HARD");
    kit.tracks[kKick].params[kSrc0] = 46;
    kit.tracks[kKick].params[kAmp0] = 46;
    kit.tracks[kKick].params[kAmp3] = 72;
    preset(kit.tracks[kSnare], MachineId::snare, "DIRTY");
    kit.tracks[kSnare].params[kAmp3] = 84;
    kit.tracks[kSnare].params[kSnb] = 30;
    preset(kit.tracks[kClosed], MachineId::hat, "TIGHT");
    kit.tracks[kClosed].params[kAmp3] = 78;
    kit.tracks[kClosed].params[kPan] = 76;
    preset(kit.tracks[kOpen], MachineId::hat, "TRASH");
    kit.tracks[kOpen].params[kAmp3] = 60;
    kit.tracks[kOpen].params[kPan] = 54;
    preset(kit.tracks[kGhost], MachineId::snare, "SHORT");
    kit.tracks[kGhost].params[kSrc2] = 75;    // TON as heard (SHORT's TON was 75 before the preset review)
    kit.tracks[kGhost].params[kAmp3] = 72;
    kit.tracks[kGhost].params[kPan] = 46;
    kit.tracks[kGhost].params[kSna] = 24;
    // BASS: REESE driven, a 1/8 sample-and-hold on the cutoff (calmer than CHAOS' 1/16).
    preset(kit.tracks[kBass], MachineId::va_mono, "REESE");
    kit.tracks[kBass].params[kOscDrv] = 96;
    kit.tracks[kBass].level = 1.4f;
    lfo(kit.tracks[kBass], LfoWave::sample_hold, sync_rate(11), 40, kSrc2);   // 1/8
    // PLUCK: VA MONO's PLUCK, a few notes, deep in the dotted echo.
    preset(kit.tracks[kPluck], MachineId::va_mono, "PLUCK");
    kit.tracks[kPluck].params[kAmp3] = 122;
    kit.tracks[kPluck].params[kPan] = 70;
    kit.tracks[kPluck].params[kSna] = 96;
    kit.tracks[kPluck].params[kSnb] = 50;
    // METAL: PERC's METAL, pitched by locks, a 7-step loop.
    preset(kit.tracks[kMetal], MachineId::perc, "METAL");
    kit.tracks[kMetal].params[kAmp3] = 62;
    kit.tracks[kMetal].params[kPan] = 30;
    kit.tracks[kMetal].params[kSna] = 30;

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 25, 20, 0};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 2;
    kit.duck.hold = 4;
    kit.duck.release = 35;
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayFb] = 66; x[sDelayTone] = 64; x[sDelayLevel] = 112; x[sDelayToRev] = 30;
    x[sRevDecay] = 46; x[sRevLevel] = 96;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 58;
    s.master[3] = 92;
}

// How hard a pattern hits the break: 0 straight, 1 shredded, 2 rolls everywhere.
void drums(Pattern &p, int mode) {
    for (int half = 0; half < 2; half++) {
        const int o = half * 32;
        trig(p, kKick, o + 0, D, 124);
        trig(p, kKick, o + 7, D, 100).condition = condition_pack_a_b(1, 2);
        trig(p, kKick, o + 10, D, 112);
        trig(p, kSnare, o + 4, D, 116);
        trig(p, kSnare, o + 12, D, 118);
        trig(p, kKick, o + 16, D, 120);
        trig(p, kKick, o + 18, D, 104).ratchet = ratchet_pack(mode ? 3 : 2, RatchetRamp::down);
        trig(p, kKick, o + 25, D, 108).probability = 70;
        trig(p, kKick, o + 26, D, 112);
        trig(p, kSnare, o + 20, D, 116);
        Step &l = trig(p, kSnare, o + 22, D, 84);
        l.condition = condition_pack_a_b(2, 2);
        pattern_set_lock(p, kSnare, o + 22, uint8_t(kSrc0), 90);
        trig(p, kSnare, o + 28, D, 120);
        Step &a = trig(p, kSnare, o + 30, D, 98);
        a.ratchet = ratchet_pack(mode == 2 ? 6 : (mode ? 4 : 2), RatchetRamp::up);
        pattern_set_lock(p, kSnare, o + 30, uint8_t(kSrc0), 100);
        if (mode) pattern_set_lock(p, kSnare, o + 30, uint8_t(kLof), 70);
    }
    if (mode == 2) {
        // Rolls everywhere: ratchets on the backbeat, a 1/48 roll across beat 4 of bar 2.
        for (const int s : {4, 36}) p.tracks[kSnare].steps[s].ratchet = ratchet_pack(3, RatchetRamp::down);
        for (const int s : {12, 44}) p.tracks[kSnare].steps[s].ratchet = ratchet_pack(2, RatchetRamp::none);
        Step &r = trig(p, kSnare, 60, D, 86, L4);
        r.ratchet = ratchet_pack_span(r48, RatchetRamp::up);
        r.condition = condition_pack(Condition::NOT_FILL);
        p.tracks[kKick].steps[0].ratchet = ratchet_pack(2, RatchetRamp::none);
        p.tracks[kKick].steps[32].ratchet = ratchet_pack(8, RatchetRamp::down);
    }
    for (int i = 1; i < 64; i += 2) trig(p, kGhost, i, D, uint8_t(30 + (i * 11) % 30)).probability = 60;
    if (mode) p.tracks[kGhost].play = uint8_t(PlayMode::rnd1);
    if (mode) {
        p.tracks[kClosed].speed = uint8_t(Speed::x2);
        p.tracks[kClosed].play = uint8_t(mode == 2 ? PlayMode::rnd1 : PlayMode::rnd2);
    }
    for (int i = 0; i < 64; i++) {
        if (!mode && i % 16 == 14) { trig(p, kOpen, i, D, 84); continue; }
        Step &s = trig(p, kClosed, i, D, (i & 3) == 0 ? 96 : ((i & 1) ? 46 : 74));
        if (i & 1) s.probability = mode ? 55 : 40;
        if (mode == 2 && i % 8 == 7) s.ratchet = ratchet_pack(3, RatchetRamp::up);
    }
    if (mode) for (const int i : {14, 46}) trig(p, kOpen, i, D, 88);
    Step &f = trig(p, kSnare, 60, D, 80, L4);
    if (mode != 2) {
        f.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
        f.condition = condition_pack(Condition::FILL);
        pattern_set_lock(p, kSnare, 60, uint8_t(kSrc0), 96);
    } else {
        f.ratchet = ratchet_pack_span(r48, RatchetRamp::up);   // the 1/48 roll above, kept
    }
}

void metal(Pattern &p, bool rolls) {
    TrackPattern &tp = p.tracks[kMetal];
    tp.length = 7;
    static const int at[] = {0, 3, 5};
    static const uint8_t tun[] = {80, 60, 96};
    for (int i = 0; i < 3; i++) {
        trig(p, kMetal, at[i], D, uint8_t(88 - i * 10));
        pattern_set_lock(p, kMetal, at[i], uint8_t(kSrc0), tun[i]);
    }
    if (rolls) p.tracks[kMetal].steps[5].ratchet = ratchet_pack(4, RatchetRamp::down);
}

// The reese: long notes and octave jumps, slides; Db and Eb for the darkness.
void bass(Pattern &p, bool busy) {
    struct N { int step; uint8_t note; uint8_t flags; uint8_t len; };
    static const N line[] = {
        {0, F1, kStepAccent, L4}, {7, F2, kStepSlide, L2}, {10, F1, 0, L3},
        {16, Db2, kStepAccent, L4}, {23, Eb2, kStepSlide, L2}, {26, C2, 0, L3}, {30, F2, kStepSlide, L1h},
    };
    for (int half = 0; half < 2; half++)
        for (const N &n : line) {
            if (!busy && (n.step == 7 || n.step == 30)) continue;
            Step &s = trig(p, kBass, half * 32 + n.step, n.note, (n.flags & kStepAccent) ? 116 : 96, n.len);
            s.flags |= n.flags;
        }
    if (busy) {
        p.tracks[kBass].steps[48].note = Bb1;
        p.tracks[kBass].steps[58].note = Ab1;
    }
}

// The pluck: three notes over four bars, the echo plays the rest.
void pluck(Pattern &p) {
    trig(p, kPluck, 3, 72, 100, L1);       // C5
    trig(p, kPluck, 22, 75, 92, L1);       // Eb5
    trig(p, kPluck, 43, 68, 96, L1);       // Ab4
    trig(p, kPluck, 62, 70, 80, L1).probability = 60;   // Bb4
}

void sb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::minor, kSwing); }
void brk(Pattern &p) { sb(p, 0x5A5401u); drums(p, 0); bass(p, false); }
void shred(Pattern &p) { sb(p, 0x5A5402u); drums(p, 1); bass(p, true); metal(p, false); }
void rolls(Pattern &p) { sb(p, 0x5A5403u); drums(p, 2); bass(p, true); metal(p, true); pluck(p); }
// STUTTR: the build. No kick until the end; the snare in rolls over LEN, faster and higher; the
// ghosts and hats random; the bass holds F and slides down an octave; the pluck answers.
void stutter(Pattern &p) {
    sb(p, 0x5A5404u);
    Step &s1 = trig(p, kSnare, 0, D, 64, L16);
    s1.ratchet = ratchet_pack_span(r16, RatchetRamp::up);
    Step &s2 = trig(p, kSnare, 16, D, 76, L16);
    s2.ratchet = ratchet_pack_span(r16t, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 16, uint8_t(kSrc0), 80);
    Step &s3 = trig(p, kSnare, 32, D, 90, L16);
    s3.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 32, uint8_t(kSrc0), 96);
    Step &s4 = trig(p, kSnare, 48, D, 110, L12);
    s4.ratchet = ratchet_pack_span(r64, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 48, uint8_t(kSrc0), 112);
    pattern_set_lock(p, kSnare, 48, uint8_t(kLof), 64);
    for (int i = 1; i < 64; i += 2) trig(p, kGhost, i, D, uint8_t(30 + i / 2)).probability = 70;
    p.tracks[kGhost].play = uint8_t(PlayMode::rnd1);
    p.tracks[kClosed].speed = uint8_t(Speed::x2);
    p.tracks[kClosed].play = uint8_t(PlayMode::rnd1);
    for (int i = 0; i < 64; i++) trig(p, kClosed, i, D, uint8_t(36 + i)).probability = 65;
    Step &k = trig(p, kKick, 60, D, 110, L4);
    k.ratchet = ratchet_pack_span(r16, RatchetRamp::up);
    trig(p, kBass, 0, F2, 110, L32);
    trig(p, kBass, 32, F1, 100, L32).flags |= kStepSlide;
    pluck(p);
    metal(p, true);
}
void smash(Pattern &p) { sb(p, 0x5A5405u); drums(p, 2); bass(p, true); metal(p, true); pluck(p); }
void half(Pattern &p) {
    sb(p, 0x5A5406u);
    drums(p, 1);
    for (const int t : {kKick, kSnare, kOpen, kGhost}) {
        p.tracks[t].speed = uint8_t(Speed::x1_2);
        p.tracks[t].length = 32;
    }
    trig(p, kBass, 0, F1, 116, L16).flags |= kStepAccent;
    trig(p, kBass, 16, Db2, 110, L8);
    trig(p, kBass, 28, C2, 100, L4).flags |= kStepSlide;
    trig(p, kBass, 32, F1, 116, L16).flags |= kStepAccent;
    trig(p, kBass, 48, Ab1, 110, L16).flags |= kStepSlide;
    metal(p, true);
}

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 65;
    s.scale = uint8_t(Scale::minor);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kKick, kBass}), mutes({kPluck}), mutes({kBass, kPluck}),
        mutes({kClosed, kOpen, kGhost}), mutes({kBass, kPluck, kMetal}), mutes({kKick, kSnare}), mutes({kMetal}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    brk(bank[0]); shred(bank[1]); rolls(bank[2]); stutter(bank[3]); smash(bank[4]); half(bank[5]);
    set_chain(project, {{0, 0, 1, 0}, {0, 1, 2, 0}, {0, 2, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0},
                        {0, 5, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0}});

    // Scene 2 CRUSH: lo-fi and drive on the drums.
    Scene &crush = scenes.scenes[1];
    scene_set(crush, kKick, kFlt0, 110);
    scene_set(crush, kSnare, kLof, 84);
    scene_set(crush, kClosed, kLof, 90);
    scene_set(crush, kGhost, kLof, 90);
    scene_set(crush, kMetal, kLof, 80);
    // Scene 3 TIGHT: every drum shorter.
    Scene &tight = scenes.scenes[2];
    scene_set(tight, kKick, kAmp0, 24);
    scene_set(tight, kSnare, kAmp0, 20);
    scene_set(tight, kOpen, kAmp0, 20);
    scene_set(tight, kMetal, kAmp0, 20);

    Macro &scream = macros.macros[1];           // B SCREAM: the reese opens and resonates
    target(scream, 0, project.kit, kBass, kSrc2, 96);
    target(scream, 1, project.kit, kBass, kSrc3, 110);
    target(scream, 2, project.kit, kBass, kAmp1, 110);
    Macro &echo = macros.macros[2];             // C ECHO: snare, ghosts and metal into the delay
    target(echo, 0, project.kit, kSnare, kSna, 100);
    target(echo, 1, project.kit, kGhost, kSna, 110);
    target(echo, 2, project.kit, kMetal, kSna, 120);
    target(echo, 3, project.kit, kPluck, kSna, 127);
    project.bank = 0;
    project.slot = 0;
}

// BREAK 4, SHRED 8, ROLLS 8, STUTTR 4, SMASH 12, HALF 8, STUTTR 4, SMASH 12 = 60 bars, 1:33.
// Beat repeats (dsp/repeat.hpp RepeatLength: 2 = 1/16, 3 = 1/32, 6 = 1/16T): b 1 holds, 0 lets go.
constexpr int kArrangementBars = 60;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {3, Cmd::set_fill, 0, 1, 0, 0}, {4, Cmd::set_fill, 0, 0, 0, 0},
    {9, Cmd::set_repeat, 0, 2, 1, 0}, {10, Cmd::set_repeat, 0, 2, 0, 0},
    {11, Cmd::set_fill, 0, 1, 0, 0}, {12, Cmd::set_fill, 0, 0, 0, 0},
    {15, Cmd::set_repeat, 0, 6, 1, 0}, {16, Cmd::set_repeat, 0, 6, 0, 0},
    {17, Cmd::set_macro, 0, 2, 120, 0}, {18, Cmd::set_macro, 0, 2, 0, 0},
    {19, Cmd::set_repeat, 0, 3, 1, 0}, {20, Cmd::set_repeat, 0, 3, 0, 0},
    // STUTTR: morph into CRUSH over the build, back at the drop
    {21, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {22, Cmd::set_morph, 0, 0, 0, kFull},
    {24, Cmd::set_morph, 0, 0, 0, 0},
    {27, Cmd::set_repeat, 0, 2, 1, 0}, {28, Cmd::set_repeat, 0, 2, 0, 0},
    {29, Cmd::set_macro, 0, 1, 100, 0}, {30, Cmd::set_macro, 0, 1, 0, 0},
    {31, Cmd::set_fill, 0, 1, 0, 0}, {32, Cmd::set_fill, 0, 0, 0, 0},
    {33, Cmd::mute_recall, 0, 5, 0, 0}, {34, Cmd::mute_recall, 0, 0, 0, 0},
    {35, Cmd::set_repeat, 0, 3, 1, 0}, {36, Cmd::set_repeat, 0, 3, 0, 0},
    // HALF: TIGHT scene for two bars, a scream
    {38, Cmd::set_morph, 0, 0, 0, 0},
    {40, Cmd::set_macro, 0, 1, 90, 0}, {41, Cmd::set_macro, 0, 1, 0, 0},
    {43, Cmd::set_repeat, 0, 6, 1, 0}, {44, Cmd::set_repeat, 0, 6, 0, 0},
    {45, Cmd::set_morph, 0, 0, 0, kFull}, {47, Cmd::set_morph, 0, 0, 0, 0},
    {51, Cmd::set_repeat, 0, 2, 1, 0}, {52, Cmd::set_repeat, 0, 2, 0, 0},
    {53, Cmd::set_macro, 0, 2, 120, 0}, {54, Cmd::set_macro, 0, 2, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
    {58, Cmd::set_repeat, 0, 3, 1, 0}, {59, Cmd::set_repeat, 0, 3, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
