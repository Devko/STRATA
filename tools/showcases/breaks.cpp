// BREAKS: a breakbeat jam showcase for STRATA. 134 BPM, A minor. No sampled break: the break is
// played on KICK, SNARE and HAT with a second, quiet SNARE track for the ghost notes, and it moves
// on its own: A:B conditions and probability vary it bar to bar, the ghost track plays its steps
// in shuffled blocks (TRACK PLAY RND4), and a half-time section runs the drums at 1/2 speed.
// A reese bass and rave stabs on top. Factory machines and sounds only; every note written here.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "BREAKS";
constexpr uint16_t kTempoCenti = 13400;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"BREAK", "ROLLER", "STABS", "HALF", "JAM", "OUTRO"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kGhost, kBass, kStab, kTom };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "GHOST", "BASS", "STAB", "TOM"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 9;     // A
constexpr uint8_t kSwing = 55;
// A minor: Am Am F G, a bar each.
struct Bar { uint8_t bass; uint8_t stab; ChordType type; };
constexpr Bar kBars[4] = {
    {33, 57, ChordType::minor}, {33, 57, ChordType::minor}, {29, 53, ChordType::major}, {31, 55, ChordType::major},
};

void kit(Kit &kit) {
    clear_kit(kit);
    // KICK: PUNCH, tuned to A1 (TUN 64), short: a break kick, not a four-on-the-floor one.
    preset(kit.tracks[kKick], MachineId::kick, "PUNCH");
    kit.tracks[kKick].params[kSrc0] = 64;
    kit.tracks[kKick].params[kAmp0] = 50;
    // SNARE: the 909 snare, crisp, a short room.
    preset(kit.tracks[kSnare], MachineId::snare, "909SN");
    kit.tracks[kSnare].params[kAmp3] = 110;
    kit.tracks[kSnare].params[kSnb] = 34;
    // HATS: tight closed, an open one choked by it.
    preset(kit.tracks[kClosed], MachineId::hat, "TIGHT");
    kit.tracks[kClosed].params[kAmp3] = 80;
    kit.tracks[kClosed].params[kPan] = 72;
    preset(kit.tracks[kOpen], MachineId::hat, "OPEN");
    kit.tracks[kOpen].params[kAmp0] = 58;
    kit.tracks[kOpen].params[kAmp3] = 60;
    kit.tracks[kOpen].params[kPan] = 60;
    // GHOST: SNARE's TIGHT, the ghost notes, a little left.
    preset(kit.tracks[kGhost], MachineId::snare, "TIGHT");
    kit.tracks[kGhost].params[kAmp3] = 80;
    kit.tracks[kGhost].params[kPan] = 52;
    // BASS: VA MONO's REESE, darker, a synced LFO on the cutoff (half a bar) for the movement.
    preset(kit.tracks[kBass], MachineId::va_mono, "REESE");
    kit.tracks[kBass].params[kSrc2] = 40;
    kit.tracks[kBass].level = 1.3f;
    lfo(kit.tracks[kBass], LfoWave::triangle, sync_rate(kHalf), 30, kSrc2);
    // STAB: VA POLY's RAVE, short, into the delay.
    preset(kit.tracks[kStab], MachineId::va_poly, "RAVE");
    kit.tracks[kStab].params[kAmp0] = 40;
    kit.tracks[kStab].params[kAmp3] = 104;
    kit.tracks[kStab].params[kSna] = 66;
    kit.tracks[kStab].params[kSnb] = 40;
    // TOM: PERC's TOM, the fills.
    preset(kit.tracks[kTom], MachineId::perc, "TOM");
    kit.tracks[kTom].params[kAmp3] = 80;
    kit.tracks[kTom].params[kPan] = 44;
    kit.tracks[kTom].params[kSnb] = 30;

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 40, 30, 0};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 2;
    kit.duck.hold = 4;
    kit.duck.release = 40;
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayFb] = 56; x[sDelayTone] = 60; x[sDelayLevel] = 110; x[sDelayToRev] = 30;
    x[sRevDecay] = 54; x[sRevLevel] = 104;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 52;
    s.master[3] = 106;
}

// The break, two bars that repeat over four, varied by conditions and chance.
void drums(Pattern &p, bool jam) {
    for (int half = 0; half < 2; half++) {
        const int o = half * 32;
        // Bar A
        trig(p, kKick, o + 0, D, 120);
        trig(p, kKick, o + 10, D, 108);
        trig(p, kSnare, o + 4, D, 112);
        trig(p, kSnare, o + 12, D, 116);
        // Bar B: the kick pushes, a late snare answers
        trig(p, kKick, o + 16, D, 118);
        trig(p, kKick, o + 18, D, 98);
        trig(p, kKick, o + 26, D, 110);
        trig(p, kKick, o + 27, D, 92).probability = 60;
        trig(p, kSnare, o + 20, D, 112);
        trig(p, kSnare, o + 28, D, 116);
        trig(p, kSnare, o + 30, D, 88).condition = condition_pack_a_b(2, 2);   // every other time
    }
    // Ghosts: quiet snares between the hits.
    static const int ghost[] = {7, 9, 15, 17, 22, 25, 31};
    static const uint8_t gvel[] = {44, 36, 52, 34, 46, 38, 56};
    for (int half = 0; half < 2; half++)
        for (int i = 0; i < 7; i++) {
            Step &s = trig(p, kGhost, half * 32 + ghost[i], D, gvel[i]);
            if (i == 1 || i == 4) s.probability = 70;
        }
    if (jam) p.tracks[kGhost].play = uint8_t(PlayMode::rnd4);
    // Hats: 8ths, the 16ths between by chance; an open hat at the end of bar A.
    for (int i = 0; i < 64; i++) {
        if (i % 32 == 14) { trig(p, kOpen, i, D, 80); continue; }
        if ((i & 1) == 0) trig(p, kClosed, i, D, (i & 3) == 0 ? 92 : 72);
        else trig(p, kClosed, i, D, 42).probability = jam ? 60 : 35;
    }
    if (jam) p.tracks[kClosed].steps[47].ratchet = ratchet_pack(3, RatchetRamp::up);
    // The fill: a snare roll on the last beat.
    for (int i = 60; i < 64; i++) {
        Step &s = trig(p, kSnare, i, D, uint8_t(70 + (i - 60) * 12));
        s.condition = condition_pack(Condition::FILL);
        s.ratchet = ratchet_pack(i < 62 ? 2 : 3, RatchetRamp::up);
    }
}

void bass(Pattern &p, bool busy) {
    for (int b = 0; b < 4; b++) {
        const int at = b * 16;
        const uint8_t r = kBars[b].bass;
        trig(p, kBass, at, r, 108, L6);
        trig(p, kBass, at + 7, uint8_t(r + 12), 92, L2).flags |= kStepSlide;
        trig(p, kBass, at + 10, r, 100, L4);
        if (busy) trig(p, kBass, at + 14, uint8_t(r + 7), 84, L1);
    }
}

void stabs(Pattern &p, uint8_t vel, bool busy) {
    for (int b = 0; b < 4; b++) {
        const int at = b * 16;
        chord(p, kStab, at + 3, kBars[b].stab, kBars[b].type, vel, L1);
        chord(p, kStab, at + 6, kBars[b].stab, kBars[b].type, uint8_t(vel - 14), L1);
        if (busy && (b & 1)) chord(p, kStab, at + 14, kBars[b].stab, ChordType::fifth_octave, uint8_t(vel - 6), L1);
    }
}

void toms(Pattern &p) {
    static const int at[] = {56, 57, 59, 61, 62};
    static const uint8_t tun[] = {50, 44, 38, 32, 26};
    for (int i = 0; i < 5; i++) {
        trig(p, kTom, at[i], D, uint8_t(80 + i * 6));
        pattern_set_lock(p, kTom, at[i], uint8_t(kSrc0), tun[i]);
    }
}

void bb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::minor, kSwing); }
void brk(Pattern &p) { bb(p, 0xB4EA01u); drums(p, false); }
void roller(Pattern &p) { bb(p, 0xB4EA02u); drums(p, false); bass(p, false); toms(p); }
void stabs_in(Pattern &p) { bb(p, 0xB4EA03u); drums(p, false); bass(p, true); stabs(p, 96, false); toms(p); }
// HALF: the drums at half speed over 32 steps (still four bars), the bass held, stabs sparse:
// the same break, half-time.
void half(Pattern &p) {
    bb(p, 0xB4EA04u);
    drums(p, false);
    for (const int t : {kKick, kSnare, kClosed, kOpen, kGhost}) {
        p.tracks[t].speed = uint8_t(Speed::x1_2);
        p.tracks[t].length = 32;
    }
    for (int b = 0; b < 4; b++) trig(p, kBass, b * 16, kBars[b].bass, 104, L16);
    for (int b = 0; b < 4; b += 2) chord(p, kStab, b * 16 + 3, kBars[b].stab, kBars[b].type, 90, L1);
}
void jam(Pattern &p) { bb(p, 0xB4EA05u); drums(p, true); bass(p, true); stabs(p, 100, true); toms(p); }
void outro(Pattern &p) { bb(p, 0xB4EA06u); drums(p, false); bass(p, false); }

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 69;
    s.scale = uint8_t(Scale::minor);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kKick, kSnare, kGhost}), mutes({kStab}), mutes({kBass, kStab, kTom}),
        mutes({kClosed, kOpen}), mutes({kBass}), mutes({kKick, kBass}), mutes({kGhost, kTom}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    brk(bank[0]); roller(bank[1]); stabs_in(bank[2]); half(bank[3]); jam(bank[4]); outro(bank[5]);
    set_chain(project, {{0, 0, 2, 0}, {0, 1, 2, 0}, {0, 2, 4, 0}, {0, 3, 2, 0}, {0, 4, 4, 0}, {0, 5, 2, 0}});

    // Scene 2 DIRTY: drive and crush on the drums, the bass opens.
    Scene &dirty = scenes.scenes[1];
    scene_set(dirty, kKick, kFlt0, 90);
    scene_set(dirty, kSnare, kLof, 60);
    scene_set(dirty, kGhost, kLof, 70);
    scene_set(dirty, kBass, kSrc2, 70);
    // Scene 3 DARK: bass and stab close, hats darker.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kBass, kSrc2, 24);
    scene_set(dark, kStab, kSrc2, 40);
    scene_set(dark, kClosed, kSrc1, 80);

    Macro &wobble = macros.macros[1];           // B WOBBLE: the reese opens, its envelope and resonance up
    target(wobble, 0, project.kit, kBass, kSrc2, 84);
    target(wobble, 1, project.kit, kBass, kAmp1, 90);
    target(wobble, 2, project.kit, kBass, kSrc3, 70);
    Macro &echo = macros.macros[2];             // C ECHO: stabs and snare into the delay
    target(echo, 0, project.kit, kStab, kSna, 124);
    target(echo, 1, project.kit, kSnare, kSna, 80);
    target(echo, 2, project.kit, kTom, kSna, 100);
    project.bank = 0;
    project.slot = 0;
}

// Break 8, roller 8, stabs 16, half 8, jam 16, outro 8 = 64 bars, 1:55 at 134.
constexpr int kArrangementBars = 64;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {7, Cmd::set_fill, 0, 1, 0, 0}, {8, Cmd::set_fill, 0, 0, 0, 0},
    {15, Cmd::set_fill, 0, 1, 0, 0}, {16, Cmd::set_fill, 0, 0, 0, 0},
    // STABS: B WOBBLE up and back
    {20, Cmd::set_macro, 0, 1, 50, 0}, {21, Cmd::set_macro, 0, 1, 100, 0},
    {23, Cmd::set_macro, 0, 1, 50, 0}, {24, Cmd::set_macro, 0, 1, 0, 0},
    {28, Cmd::set_macro, 0, 2, 110, 0}, {29, Cmd::set_macro, 0, 2, 0, 0},
    {31, Cmd::set_fill, 0, 1, 0, 0}, {32, Cmd::set_fill, 0, 0, 0, 0},
    // HALF: morph to DIRTY and back
    {34, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {36, Cmd::set_morph, 0, 0, 0, kFull},
    {38, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {39, Cmd::set_morph, 0, 0, 0, 0},
    {39, Cmd::set_fill, 0, 1, 0, 0}, {40, Cmd::set_fill, 0, 0, 0, 0},
    // JAM: the drums alone for two bars, the stabs echo, fills
    {46, Cmd::mute_recall, 0, 3, 0, 0}, {48, Cmd::mute_recall, 0, 0, 0, 0},
    {50, Cmd::set_macro, 0, 2, 110, 0}, {51, Cmd::set_macro, 0, 2, 0, 0},
    {51, Cmd::set_fill, 0, 1, 0, 0}, {52, Cmd::set_fill, 0, 0, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
