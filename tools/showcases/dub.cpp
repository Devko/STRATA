// DUB: a dub techno showcase for STRATA. 118 BPM, F minor. One minor-seventh chord, short,
// thrown into a long dark echo and reverb; its filter written into the steps and breathing on an
// 8-bar LFO; a round sub bass; hats with a hiss under them. Factory machines and sounds only.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "DUB";
constexpr uint16_t kTempoCenti = 11800;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"INTRO", "CHORD", "DEEP", "DUBOUT", "FULL", "OUTRO"};
enum Track : int { kKick = 0, kRim, kClosed, kOpen, kKnock, kBass, kChord, kHiss };
constexpr const char *kTrackNames[kTracks] = {"KICK", "RIM", "CLOSED", "OPEN", "KNOCK", "BASS", "CHORD", "HISS"};
constexpr int kStemSlot = 4;

constexpr uint8_t F1 = 29, Db2 = 37, Eb2 = 39, F2 = 41, C3 = 48, Db3 = 49, F3 = 53;
constexpr uint8_t kRoot = 5;     // F
constexpr uint8_t kSwing = 54;

void kit(Kit &kit) {
    clear_kit(kit);
    // KICK: DEEP, tuned to F1 (TUN 45 = 43.7 Hz), soft and long.
    preset(kit.tracks[kKick], MachineId::kick, "DEEP");
    kit.tracks[kKick].params[kSrc0] = 45;
    kit.tracks[kKick].params[kAmp0] = 96;
    // RIM: SNARE's rimshot, into the echo.
    preset(kit.tracks[kRim], MachineId::snare, "RIMSN");
    kit.tracks[kRim].params[kAmp3] = 100;
    kit.tracks[kRim].params[kPan] = 52;
    kit.tracks[kRim].params[kSna] = 70;
    kit.tracks[kRim].params[kSnb] = 40;
    // Hats: dark, the open one rings a little.
    preset(kit.tracks[kClosed], MachineId::hat, "DARK");
    kit.tracks[kClosed].params[kAmp3] = 74;
    kit.tracks[kClosed].params[kPan] = 70;
    preset(kit.tracks[kOpen], MachineId::hat, "OPEN");
    kit.tracks[kOpen].params[kSrc1] = 80;
    kit.tracks[kOpen].params[kAmp0] = 52;
    kit.tracks[kOpen].params[kAmp3] = 54;
    kit.tracks[kOpen].params[kPan] = 58;
    kit.tracks[kOpen].params[kSnb] = 30;
    // KNOCK: PERC's wooden knock, low, answered by the echo.
    preset(kit.tracks[kKnock], MachineId::perc, "KNOCK");
    kit.tracks[kKnock].params[kAmp3] = 76;
    kit.tracks[kKnock].params[kPan] = 40;
    kit.tracks[kKnock].params[kSna] = 60;
    // BASS: VA MONO's DUBBAS, the sub with a soft top.
    preset(kit.tracks[kBass], MachineId::va_mono, "DUBBAS");
    kit.tracks[kBass].params[kSrc2] = 30;
    kit.tracks[kBass].params[kRel] = 20;
    kit.tracks[kBass].level = 1.6f;
    // CHORD: VA POLY's DUBCHD, short, deep in both sends; an 8-bar triangle on its cutoff.
    preset(kit.tracks[kChord], MachineId::va_poly, "DUBCHD");
    kit.tracks[kChord].params[kSrc2] = 50;
    kit.tracks[kChord].params[kSrc3] = 64;
    kit.tracks[kChord].params[kAmp0] = 58;
    kit.tracks[kChord].params[kSna] = 100;
    kit.tracks[kChord].params[kSnb] = 76;
    lfo(kit.tracks[kChord], LfoWave::triangle, sync_rate(k8B), 40, kSrc2);
    // HISS: SNARE's NOISE, long and quiet, high-passed by its tone: the tape hiss under the hats.
    preset(kit.tracks[kHiss], MachineId::snare, "NOISE");
    kit.tracks[kHiss].params[kSrc2] = 110;
    kit.tracks[kHiss].params[kAmp0] = 110;
    kit.tracks[kHiss].params[kAmp3] = 30;
    kit.tracks[kHiss].params[kSnb] = 50;

    const uint8_t duck[kTracks] = {0, 0, 10, 10, 10, 50, 35, 30};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 3;
    kit.duck.hold = 8;
    kit.duck.release = 60;
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayDiv] = kDefaultDelayDivision;     // dotted 1/8
    x[sDelayFb] = 82; x[sDelayTone] = 38; x[sDelayPing] = 127; x[sDelayLevel] = 118;
    x[sDelayToRev] = 64;
    x[sRevDecay] = 86; x[sRevDamp] = 92; x[sRevHp] = 70; x[sRevLevel] = 112;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 44;
    s.master[3] = 118;
}

void kicks(Pattern &p, uint8_t vel) {
    for (int i = 0; i < 16; i++) trig(p, kKick, i * 4, D, i % 4 == 0 ? 118 : vel);
}

void hats(Pattern &p, uint8_t beat, uint8_t soft, bool open) {
    for (int i = 0; i < 64; i++) {
        if (open && (i & 3) == 2) { trig(p, kOpen, i, D, uint8_t(beat - 4)); continue; }
        Step &s = trig(p, kClosed, i, D, (i & 3) == 2 ? beat : soft);
        if ((i & 3) == 1 || (i & 3) == 3) s.probability = 80;
    }
}

void hiss(Pattern &p) {
    for (int i = 0; i < 64; i += 8) trig(p, kHiss, i + 4, D, 70);
}

// The chord: Fm7 on the "and" of beat 1 and the "e" of beat 3, its cutoff a little different
// every time; the fourth bar answers on Dbmaj7. The echo does the rest.
void chords(Pattern &p, uint8_t vel, bool busy) {
    static const uint8_t cut[8] = {46, 58, 40, 66, 50, 62, 44, 72};
    int k = 0;
    for (int b = 0; b < 4; b++) {
        const bool answer = b == 3;
        const uint8_t root = answer ? Db3 : F3;
        const ChordType type = answer ? ChordType::maj7 : ChordType::min7;
        for (const int at : {2, 9}) {
            const int step = b * 16 + at;
            chord(p, kChord, step, root, type, at == 2 ? vel : uint8_t(vel - 14), L1);
            pattern_set_lock(p, kChord, step, uint8_t(kSrc2), cut[k++ & 7]);
        }
        if (busy && (b & 1) == 0) {
            Step &s = chord(p, kChord, b * 16 + 14, root, type, uint8_t(vel - 24), L1);
            s.probability = 60;
        }
    }
}

void rims(Pattern &p) {
    for (int b = 0; b < 4; b++) trig(p, kRim, b * 16 + 12, D, 84);
    trig(p, kRim, 31, D, 60).probability = 70;
    trig(p, kRim, 63, D, 64);
}

void knocks(Pattern &p) {
    static const int at[] = {7, 22, 27, 39, 54, 59};
    static const uint8_t tun[] = {40, 34, 46, 40, 30, 46};
    for (int i = 0; i < 6; i++) {
        trig(p, kKnock, at[i], D, uint8_t(80 - (i & 1) * 12));
        pattern_set_lock(p, kKnock, at[i], uint8_t(kSrc0), tun[i]);
    }
}

// The bass: the root under the kick's gaps, the fifth now and then, Db and Eb under the answer.
void bass(Pattern &p, bool busy) {
    for (int b = 0; b < 4; b++) {
        const uint8_t r = b == 3 ? Db2 : F2;
        const int at = b * 16;
        trig(p, kBass, at + 2, r, 100, L3);
        trig(p, kBass, at + 10, r, 92, L2);
        if (busy) trig(p, kBass, at + 7, b == 3 ? Eb2 : C3, 70, L1);
    }
    trig(p, kBass, 63, F1, 80, L1).flags |= kStepSlide;
}

void fill_rim(Pattern &p) {
    for (int i = 60; i < 64; i++) {
        Step &s = trig(p, kRim, i, D, uint8_t(60 + (i - 60) * 8));
        s.condition = condition_pack(Condition::FILL);
    }
}

void intro(Pattern &p) {
    begin(p, 0xD0B01u, kRoot, Scale::minor, kSwing);
    kicks(p, 108);
    hats(p, 70, 40, false);
    hiss(p);
}
void chord_in(Pattern &p) {
    begin(p, 0xD0B02u, kRoot, Scale::minor, kSwing);
    kicks(p, 112);
    hats(p, 74, 42, false);
    hiss(p);
    chords(p, 96, false);
    rims(p);
    fill_rim(p);
}
void deep(Pattern &p) {
    begin(p, 0xD0B03u, kRoot, Scale::minor, kSwing);
    kicks(p, 114);
    hats(p, 78, 44, true);
    hiss(p);
    chords(p, 100, false);
    rims(p);
    knocks(p);
    bass(p, false);
    fill_rim(p);
}
// DUBOUT: no kick, no bass. The chords alone in the echoes, the knock and the hiss.
void dubout(Pattern &p) {
    begin(p, 0xD0B04u, kRoot, Scale::minor, kSwing);
    chords(p, 100, true);
    for (int i = 0; i < 64; i += 16) pattern_set_lock(p, kChord, i + 2, uint8_t(kSna), 124);
    knocks(p);
    hiss(p);
    for (int i = 2; i < 64; i += 4) trig(p, kClosed, i, D, 58);
    for (int i = 60; i < 64; i++) trig(p, kKick, i, D, uint8_t(64 + (i - 60) * 14));
}
void full(Pattern &p) {
    begin(p, 0xD0B05u, kRoot, Scale::minor, kSwing);
    kicks(p, 118);
    hats(p, 84, 48, true);
    hiss(p);
    chords(p, 104, true);
    rims(p);
    knocks(p);
    bass(p, true);
    fill_rim(p);
}
void outro(Pattern &p) {
    begin(p, 0xD0B06u, kRoot, Scale::minor, kSwing);
    kicks(p, 110);
    hats(p, 72, 40, false);
    hiss(p);
    bass(p, false);
    for (int b = 0; b < 4; b += 2) chord(p, kChord, b * 16 + 2, F3, ChordType::min7, 84, L1);
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
        0, mutes({kKick, kBass}), mutes({kChord}), mutes({kRim, kOpen, kKnock, kChord}),
        mutes({kClosed, kOpen, kHiss}), mutes({kBass}), mutes({kKick, kClosed, kOpen, kBass}), mutes({kKnock, kRim}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    intro(bank[0]); chord_in(bank[1]); deep(bank[2]); dubout(bank[3]); full(bank[4]); outro(bank[5]);
    set_chain(project, {{0, 0, 2, 0}, {0, 1, 2, 0}, {0, 2, 4, 0}, {0, 3, 2, 0}, {0, 4, 4, 0}, {0, 5, 2, 0}});

    // Scene 2 DUB: everything further into the echo, longer chord, rim and knock thrown.
    Scene &dub = scenes.scenes[1];
    scene_set(dub, kChord, kSna, 127);
    scene_set(dub, kChord, kAmp0, 74);
    scene_set(dub, kRim, kSna, 110);
    scene_set(dub, kKnock, kSna, 100);
    scene_set(dub, kOpen, kSnb, 70);
    // Scene 3 DARK: chord and bass close, hats lose their top.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kChord, kSrc2, 30);
    scene_set(dark, kBass, kSrc2, 18);
    scene_set(dark, kClosed, kSrc1, 50);
    scene_set(dark, kOpen, kSrc1, 60);
    // Scene 4 OPEN: the chord opens and resonates.
    Scene &open = scenes.scenes[3];
    scene_set(open, kChord, kSrc2, 84);
    scene_set(open, kChord, kSrc3, 90);
    scene_set(open, kHiss, kAmp3, 50);

    Macro &filter = macros.macros[1];           // B FILTER: the chord opens and rings
    target(filter, 0, project.kit, kChord, kSrc2, 92);
    target(filter, 1, project.kit, kChord, kSrc3, 96);
    target(filter, 2, project.kit, kChord, kAmp0, 76);
    Macro &throwm = macros.macros[2];           // C THROW: rim, knock and chord into the echo
    target(throwm, 0, project.kit, kChord, kSna, 127);
    target(throwm, 1, project.kit, kRim, kSna, 120);
    target(throwm, 2, project.kit, kKnock, kSna, 112);
    target(throwm, 3, project.kit, kOpen, kSna, 80);
    Macro &space = macros.macros[3];            // D SPACE: everything into the reverb
    target(space, 0, project.kit, kChord, kSnb, 120);
    target(space, 1, project.kit, kRim, kSnb, 100);
    target(space, 2, project.kit, kHiss, kSnb, 100);
    target(space, 3, project.kit, kOpen, kSnb, 80);
    project.bank = 0;
    project.slot = 0;
}

// Intro 8, chord 8, deep 16, dubout 8, full 16, outro 8 = 64 bars, 2:10 at 118.
constexpr int kArrangementBars = 64;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {7, Cmd::set_fill, 0, 1, 0, 0}, {8, Cmd::set_fill, 0, 0, 0, 0},
    {15, Cmd::set_fill, 0, 1, 0, 0}, {16, Cmd::set_fill, 0, 0, 0, 0},
    // DEEP: macro B FILTER opens over four bars and closes over four
    {22, Cmd::set_macro, 0, 1, 40, 0}, {23, Cmd::set_macro, 0, 1, 80, 0}, {24, Cmd::set_macro, 0, 1, 110, 0},
    {26, Cmd::set_macro, 0, 1, 70, 0}, {27, Cmd::set_macro, 0, 1, 35, 0}, {28, Cmd::set_macro, 0, 1, 0, 0},
    {31, Cmd::set_fill, 0, 1, 0, 0}, {32, Cmd::set_fill, 0, 0, 0, 0},
    // DUBOUT: morph to the DUB scene and back by the drop
    {32, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {34, Cmd::set_morph, 0, 0, 0, kFull},
    {38, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {39, Cmd::set_morph, 0, 0, 0, 0},
    // FULL: C THROW for a bar, twice
    {44, Cmd::set_macro, 0, 2, 110, 0}, {45, Cmd::set_macro, 0, 2, 0, 0},
    {52, Cmd::set_macro, 0, 2, 110, 0}, {53, Cmd::set_macro, 0, 2, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
