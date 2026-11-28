// CHAOS: BREAKS, much crazier. 150 BPM, D phrygian. The synthesized break is hit harder and
// pulled apart: rolls over LEN at 1/32 and 1/16T, ratchets on the kick, ghost snares in random
// order every step (TRACK PLAY RND1), hats at double speed shuffled in pairs, a hoover stab on a
// 12-step loop playing ping-pong against the 16, zaps on a 10-step loop played backwards, and a
// driven reese bass whose filter jumps on a 1/16 sample-and-hold. Snare pitch and lo-fi locks, a
// half-time drop. Factory machines and sounds only; every note written here.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "CHAOS";
constexpr uint16_t kTempoCenti = 15000;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"IGNITE", "RIOT", "HOOVER", "MELTDN", "CHAOS", "HALF"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kGhost, kBass, kStab, kZap };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "GHOST", "BASS", "HOOVER", "ZAP"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 2;     // D
constexpr uint8_t kSwing = 53;
constexpr uint8_t D1 = 26, Eb1 = 27, F1 = 29, G1 = 31, A1 = 33, C2 = 36, D2 = 38, Eb2 = 39;
constexpr uint8_t D3 = 50, Eb3 = 51, C3 = 48, A2 = 45;

// Roll rates (core/pattern.hpp kRatchetRateTicks): 0 1/64, 2 1/32, 3 1/16T, 4 1/16.
constexpr uint8_t r64 = 0, r32 = 2, r16t = 3, r16 = 4;

void kit(Kit &kit) {
    clear_kit(kit);
    // KICK: HARD, tuned to D1-ish (TUN 50), driven.
    preset(kit.tracks[kKick], MachineId::kick, "HARD");
    kit.tracks[kKick].params[kSrc0] = 50;
    kit.tracks[kKick].params[kAmp0] = 48;
    kit.tracks[kKick].params[kAmp3] = 70;
    // SNARE: DIRTY, a short room; pitch and crush come from locks.
    preset(kit.tracks[kSnare], MachineId::snare, "DIRTY");
    kit.tracks[kSnare].params[kAmp3] = 84;
    kit.tracks[kSnare].params[kSnb] = 30;
    // HATS: TIGHT closed, TRASH open (choked by the closed).
    preset(kit.tracks[kClosed], MachineId::hat, "TIGHT");
    kit.tracks[kClosed].params[kAmp3] = 78;
    kit.tracks[kClosed].params[kPan] = 76;
    preset(kit.tracks[kOpen], MachineId::hat, "TRASH");
    kit.tracks[kOpen].params[kAmp3] = 62;
    kit.tracks[kOpen].params[kPan] = 54;
    // GHOST: SNARE's SHORT, quiet, everywhere.
    preset(kit.tracks[kGhost], MachineId::snare, "SHORT");
    kit.tracks[kGhost].params[kSrc2] = 75;    // TON as heard (SHORT's TON was 75 before the preset review)
    kit.tracks[kGhost].params[kAmp3] = 70;
    kit.tracks[kGhost].params[kPan] = 48;
    kit.tracks[kGhost].params[kSna] = 30;
    // BASS: VA MONO's REESE driven hard, a 1/16 sample-and-hold on its cutoff.
    preset(kit.tracks[kBass], MachineId::va_mono, "REESE");
    kit.tracks[kBass].params[kOscDrv] = 96;     // grit
    kit.tracks[kBass].params[kSrc2] = 58;
    kit.tracks[kBass].level = 1.4f;
    lfo(kit.tracks[kBass], LfoWave::sample_hold, sync_rate(13), 44, kSrc2);   // 1/16
    // HOOVER: VA POLY's HOOVER, short, into the delay.
    preset(kit.tracks[kStab], MachineId::va_poly, "HOOVER");
    kit.tracks[kStab].params[kAmp0] = 44;
    kit.tracks[kStab].params[kAmp3] = 84;
    kit.tracks[kStab].params[kSna] = 70;
    kit.tracks[kStab].params[kSnb] = 36;
    // ZAP: PERC's ZAP, wide right, into the delay.
    preset(kit.tracks[kZap], MachineId::perc, "ZAP");
    kit.tracks[kZap].params[kAmp3] = 70;
    kit.tracks[kZap].params[kPan] = 90;
    kit.tracks[kZap].params[kSna] = 60;

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 25, 35, 0};
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
    x[sDelayFb] = 62; x[sDelayTone] = 70; x[sDelayLevel] = 112; x[sDelayToRev] = 24;
    x[sRevDecay] = 50; x[sRevLevel] = 100;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 56;
    s.master[3] = 92;
}

// The break: two bars, harder than BREAKS', with a kick ratchet and a pitched snare answer.
void drums(Pattern &p, bool wild) {
    for (int half = 0; half < 2; half++) {
        const int o = half * 32;
        trig(p, kKick, o + 0, D, 124);
        trig(p, kKick, o + 6, D, 100).probability = wild ? 70 : 100;
        trig(p, kKick, o + 10, D, 112);
        trig(p, kSnare, o + 4, D, 116);
        trig(p, kSnare, o + 12, D, 118);
        trig(p, kKick, o + 16, D, 120);
        Step &k2 = trig(p, kKick, o + 19, D, 104);
        k2.ratchet = ratchet_pack(wild ? 3 : 2, RatchetRamp::down);
        trig(p, kKick, o + 26, D, 112);
        trig(p, kSnare, o + 20, D, 116);
        Step &late = trig(p, kSnare, o + 23, D, 90);
        late.condition = condition_pack_a_b(2, 2);
        pattern_set_lock(p, kSnare, o + 23, uint8_t(kSrc0), 92);      // up a fifth-ish
        trig(p, kSnare, o + 28, D, 120);
        Step &ans = trig(p, kSnare, o + 30, D, 96);
        ans.ratchet = ratchet_pack(wild ? 4 : 2, RatchetRamp::up);
        pattern_set_lock(p, kSnare, o + 30, uint8_t(kSrc0), 100);
        if (wild) pattern_set_lock(p, kSnare, o + 30, uint8_t(kLof), 70);   // crushed
    }
    // Ghosts on every other 16th; in wild patterns played in random order every step.
    for (int i = 1; i < 64; i += 2) {
        Step &s = trig(p, kGhost, i, D, uint8_t(30 + (i * 7) % 28));
        s.probability = 65;
    }
    if (wild) p.tracks[kGhost].play = uint8_t(PlayMode::rnd1);
    // Hats: in wild patterns at double speed (32nds over two bars), shuffled in pairs.
    if (wild) {
        p.tracks[kClosed].speed = uint8_t(Speed::x2);
        p.tracks[kClosed].play = uint8_t(PlayMode::rnd2);
    }
    for (int i = 0; i < 64; i++) {
        if (!wild && i % 16 == 14) { trig(p, kOpen, i, D, 84); continue; }
        Step &s = trig(p, kClosed, i, D, (i & 3) == 0 ? 96 : ((i & 1) ? 46 : 74));
        if (i & 1) s.probability = wild ? 55 : 40;
    }
    if (wild) for (const int i : {14, 46}) trig(p, kOpen, i, D, 88);
    // The fill: a 1/32 snare roll over the last beat, rising, its pitch climbing too.
    Step &roll = trig(p, kSnare, 60, D, 80, L4);
    roll.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
    roll.condition = condition_pack(Condition::FILL);
    pattern_set_lock(p, kSnare, 60, uint8_t(kSrc0), 96);
}

// The bass: off the kick, octave jumps and slides, the phrygian Eb.
void bass(Pattern &p, bool busy) {
    struct N { int step; uint8_t note; uint8_t flags; uint8_t len; };
    static const N line[] = {
        {2, D2, kStepAccent, L1}, {3, D1, 0, L1}, {7, D2, kStepSlide, L1h}, {8, Eb2, kStepAccent, L1},
        {11, D1, 0, L2}, {14, C2, kStepSlide, L1}, {15, D2, kStepAccent, L1},
        {18, D2, kStepAccent, L1}, {21, F1, 0, L1}, {22, G1, kStepSlide, L1}, {24, A1, kStepAccent, L2},
        {27, D1, 0, L1}, {29, Eb1, kStepSlide, L1}, {30, D2, kStepAccent, L1},
    };
    for (int half = 0; half < 2; half++)
        for (const N &n : line) {
            if (!busy && (n.step == 3 || n.step == 21 || n.step == 27)) continue;
            Step &s = trig(p, kBass, half * 32 + n.step, n.note, (n.flags & kStepAccent) ? 116 : 90, n.len == L1 ? L1h : n.len);
            s.flags |= n.flags;
        }
}

// The hoover: 12 steps, played ping-pong, against the 16-step break.
void hoover(Pattern &p, uint8_t vel) {
    TrackPattern &tp = p.tracks[kStab];
    tp.length = 12;
    tp.play = uint8_t(PlayMode::ping);
    chord(p, kStab, 0, D3, ChordType::fifth_octave, vel, L2);
    chord(p, kStab, 3, Eb3, ChordType::fifth_octave, uint8_t(vel - 10), L1);
    chord(p, kStab, 6, C3, ChordType::fifth_octave, uint8_t(vel - 4), L1);
    chord(p, kStab, 9, A2, ChordType::minor, uint8_t(vel - 12), L1).probability = 70;
}

// Zaps: 10 steps, backwards.
void zaps(Pattern &p) {
    TrackPattern &tp = p.tracks[kZap];
    tp.length = 10;
    tp.play = uint8_t(PlayMode::rev);
    static const int at[] = {0, 3, 7};
    static const uint8_t tun[] = {110, 70, 90};
    for (int i = 0; i < 3; i++) {
        trig(p, kZap, at[i], D, uint8_t(90 - i * 10));
        pattern_set_lock(p, kZap, at[i], uint8_t(kSrc0), tun[i]);
    }
    Step &z = trig(p, kZap, 5, D, 70, L2);
    z.ratchet = ratchet_pack_span(r16t, RatchetRamp::down);
}

void cb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::phrygian, kSwing); }

void ignite(Pattern &p) { cb(p, 0xC4A051u); drums(p, false); zaps(p); }
void riot(Pattern &p) { cb(p, 0xC4A052u); drums(p, false); bass(p, false); zaps(p); }
void hoover_in(Pattern &p) { cb(p, 0xC4A053u); drums(p, true); bass(p, true); hoover(p, 100); }
// MELTDOWN: the build. Rolls over LEN: the kick in 1/16s for two bars, the snare in 1/16T then
// 1/32 then 1/64 rising, the hats random at double speed; the bass holds a D and slides down.
void meltdown(Pattern &p) {
    cb(p, 0xC4A054u);
    Step &k = trig(p, kKick, 0, D, 110, L32);
    k.ratchet = ratchet_pack_span(r16, RatchetRamp::none);
    Step &s1 = trig(p, kSnare, 32, D, 70, L16);
    s1.ratchet = ratchet_pack_span(r16t, RatchetRamp::up);
    Step &s2 = trig(p, kSnare, 48, D, 90, L8);
    s2.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 48, uint8_t(kSrc0), 90);
    Step &s3 = trig(p, kSnare, 56, D, 110, L8);
    s3.ratchet = ratchet_pack_span(r64, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 56, uint8_t(kSrc0), 110);
    pattern_set_lock(p, kSnare, 56, uint8_t(kLof), 60);
    p.tracks[kClosed].speed = uint8_t(Speed::x2);
    p.tracks[kClosed].play = uint8_t(PlayMode::rnd1);
    for (int i = 0; i < 64; i++) trig(p, kClosed, i, D, uint8_t(40 + i)).probability = 70;
    trig(p, kBass, 0, D2, 110, L32);
    trig(p, kBass, 32, D1, 100, L32).flags |= kStepSlide;
    hoover(p, 90);
}
void chaos(Pattern &p) { cb(p, 0xC4A055u); drums(p, true); bass(p, true); hoover(p, 104); zaps(p); }
// HALF: the drop, half time: drums at 1/2 speed on 32 steps, the bass in long notes.
void half(Pattern &p) {
    cb(p, 0xC4A056u);
    drums(p, false);
    for (const int t : {kKick, kSnare, kClosed, kOpen, kGhost}) {
        p.tracks[t].speed = uint8_t(Speed::x1_2);
        p.tracks[t].length = 32;
    }
    static const uint8_t roots[4] = {D1, D1, Eb1, C2};
    for (int b = 0; b < 4; b++) {
        trig(p, kBass, b * 16, roots[b], 116, L6).flags |= kStepAccent;
        trig(p, kBass, b * 16 + 10, uint8_t(roots[b] + 12), 96, L4).flags |= kStepSlide;
    }
    zaps(p);
}

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 62;
    s.scale = uint8_t(Scale::phrygian);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kKick, kSnare, kGhost}), mutes({kStab, kZap}), mutes({kBass, kStab, kZap}),
        mutes({kClosed, kOpen, kGhost}), mutes({kBass}), mutes({kKick, kBass}), mutes({kStab}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    ignite(bank[0]); riot(bank[1]); hoover_in(bank[2]); meltdown(bank[3]); chaos(bank[4]); half(bank[5]);
    set_chain(project, {{0, 0, 1, 0}, {0, 1, 2, 0}, {0, 2, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0},
                        {0, 5, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0}});

    // Scene 2 CRUSH: lo-fi and drive on everything.
    Scene &crush = scenes.scenes[1];
    scene_set(crush, kKick, kFlt0, 110);
    scene_set(crush, kSnare, kLof, 80);
    scene_set(crush, kClosed, kLof, 90);
    scene_set(crush, kGhost, kLof, 90);
    scene_set(crush, kStab, kLof, 70);
    scene_set(crush, kBass, kSrc3, 100);
    // Scene 3 DARK: bass, hoover and hats close.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kBass, kSrc2, 20);
    scene_set(dark, kStab, kSrc2, 30);
    scene_set(dark, kClosed, kSrc1, 70);

    Macro &wobble = macros.macros[1];           // B SCREAM: the reese opens, resonates, envelope up
    target(wobble, 0, project.kit, kBass, kSrc2, 96);
    target(wobble, 1, project.kit, kBass, kSrc3, 110);
    target(wobble, 2, project.kit, kBass, kAmp1, 110);
    Macro &echo = macros.macros[2];             // C ECHO: hoover, zaps and snare into the delay
    target(echo, 0, project.kit, kStab, kSna, 127);
    target(echo, 1, project.kit, kZap, kSna, 120);
    target(echo, 2, project.kit, kSnare, kSna, 90);
    target(echo, 3, project.kit, kGhost, kSna, 100);
    project.bank = 0;
    project.slot = 0;
}

// IGNITE 4, RIOT 8, HOOVER 8, MELTDN 4, CHAOS 12, HALF 8, MELTDN 4, CHAOS 12 = 60 bars, 1:36.
constexpr int kArrangementBars = 60;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {3, Cmd::set_fill, 0, 1, 0, 0}, {4, Cmd::set_fill, 0, 0, 0, 0},
    {11, Cmd::set_fill, 0, 1, 0, 0}, {12, Cmd::set_fill, 0, 0, 0, 0},
    // HOOVER: B SCREAM up and back
    {14, Cmd::set_macro, 0, 1, 60, 0}, {15, Cmd::set_macro, 0, 1, 120, 0},
    {17, Cmd::set_macro, 0, 1, 60, 0}, {18, Cmd::set_macro, 0, 1, 0, 0},
    // MELTDOWN: morph into CRUSH over the build, back at the drop
    {20, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 3)}, {21, Cmd::set_morph, 0, 0, 0, uint16_t(kFull * 2 / 3)},
    {22, Cmd::set_morph, 0, 0, 0, kFull}, {24, Cmd::set_morph, 0, 0, 0, 0},
    // CHAOS: echo throws, the drums alone, fills
    {27, Cmd::set_macro, 0, 2, 120, 0}, {28, Cmd::set_macro, 0, 2, 0, 0},
    {31, Cmd::set_fill, 0, 1, 0, 0}, {32, Cmd::set_fill, 0, 0, 0, 0},
    {33, Cmd::mute_recall, 0, 3, 0, 0}, {34, Cmd::mute_recall, 0, 0, 0, 0},
    // HALF: B SCREAM on the bass
    {38, Cmd::set_macro, 0, 1, 80, 0}, {40, Cmd::set_macro, 0, 1, 0, 0},
    {42, Cmd::set_macro, 0, 1, 100, 0}, {43, Cmd::set_macro, 0, 1, 0, 0},
    // second MELTDOWN and CHAOS
    {44, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {46, Cmd::set_morph, 0, 0, 0, kFull},
    {48, Cmd::set_morph, 0, 0, 0, 0},
    {51, Cmd::set_macro, 0, 2, 120, 0}, {52, Cmd::set_macro, 0, 2, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
    {57, Cmd::set_macro, 0, 1, 110, 0}, {58, Cmd::set_macro, 0, 1, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
