// DRIFT: an ambient showcase for STRATA. 80 BPM, D dorian. Held WAVE POLY chords and a VA drone
// in the HALL with shimmer and the LONG delay; chimes, bells and a glass melody on tracks of 11,
// 13 and 7 steps at different speeds, so they never line up the same way twice; probability on
// the ticks. Factory machines and sounds only.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "DRIFT";
constexpr uint16_t kTempoCenti = 8000;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"DAWN", "BELLS", "PULSE", "HAZE", "BLOOM", "DUSK"};
enum Track : int { kPulse = 0, kTick, kChime, kBell, kGlass, kDrone, kPad, kShimmer };
constexpr const char *kTrackNames[kTracks] = {"PULSE", "TICK", "CHIME", "BELL", "GLASS", "DRONE", "PAD", "SHIMMER"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 2;     // D
// D dorian: D E F G A B C. Chords a bar each: Dm7, G, Fmaj7, Cmaj7 (sus2 colours in BLOOM).
struct Bar { uint8_t drone; uint8_t root; ChordType type; };
constexpr Bar kBars[4] = {
    {38, 50, ChordType::min7},   // D2, Dm7 on D3
    {43, 55, ChordType::sus2},   // G2, Gsus2 on G3
    {41, 53, ChordType::maj7},   // F2, Fmaj7 on F3
    {36, 48, ChordType::maj7},   // C2, Cmaj7 on C3
};

void kit(Kit &kit) {
    clear_kit(kit);
    // PULSE: KICK's SOFT, low and round, a heartbeat at half speed.
    preset(kit.tracks[kPulse], MachineId::kick, "SOFT");
    kit.tracks[kPulse].params[kSrc0] = 44;
    kit.tracks[kPulse].params[kAmp3] = 90;
    kit.tracks[kPulse].params[kSnb] = 30;
    // TICK: the faintest hat, a clock in the distance.
    preset(kit.tracks[kTick], MachineId::hat, "TICK");
    kit.tracks[kTick].params[kAmp3] = 56;
    kit.tracks[kTick].params[kPan] = 76;
    kit.tracks[kTick].params[kSna] = 40;
    // CHIME and BELL: PERC's FM chime and bell, panned apart, deep in the delay and the hall.
    preset(kit.tracks[kChime], MachineId::perc, "CHIME");
    kit.tracks[kChime].params[kAmp3] = 44;
    kit.tracks[kChime].params[kPan] = 44;
    kit.tracks[kChime].params[kSna] = 70;
    kit.tracks[kChime].params[kSnb] = 90;
    preset(kit.tracks[kBell], MachineId::perc, "BELL");
    kit.tracks[kBell].params[kAmp3] = 46;
    kit.tracks[kBell].params[kPan] = 86;
    kit.tracks[kBell].params[kSna] = 50;
    kit.tracks[kBell].params[kSnb] = 96;
    // GLASS: FM4's GLASS, single notes, the melody.
    preset(kit.tracks[kGlass], MachineId::fm, "GLASS");
    kit.tracks[kGlass].params[kAmp3] = 70;
    kit.tracks[kGlass].params[kSna] = 64;
    kit.tracks[kGlass].params[kSnb] = 90;
    // DRONE: VA MONO's DRONE, a note a bar, slow LFO on the cutoff.
    preset(kit.tracks[kDrone], MachineId::va_mono, "DRONE");
    kit.tracks[kDrone].params[kSrc2] = 44;
    kit.tracks[kDrone].params[kAmp3] = 64;
    kit.tracks[kDrone].params[kSnb] = 60;
    lfo(kit.tracks[kDrone], LfoWave::sine, sync_rate(k4B), 30, kSrc2);
    // PAD: WAVE POLY's PAD, a chord a bar, slow in, into the hall; the table position drifts.
    preset(kit.tracks[kPad], MachineId::wave_poly, "PAD");
    kit.tracks[kPad].params[kAtk] = 90;
    kit.tracks[kPad].params[kRel] = 80;
    kit.tracks[kPad].params[kAmp3] = 62;
    kit.tracks[kPad].params[kSnb] = 100;
    lfo(kit.tracks[kPad], LfoWave::triangle, sync_rate(k8B), 50, kSrc1);
    // SHIMMER: the long shimmering hat, rarely, into the hall.
    preset(kit.tracks[kShimmer], MachineId::hat, "SHIMMR");
    kit.tracks[kShimmer].params[kAmp3] = 34;
    kit.tracks[kShimmer].params[kPan] = 30;
    kit.tracks[kShimmer].params[kSnb] = 110;

    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = 0;   // no pumping here
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayFb] = 70; x[sDelayTone] = 50; x[sDelayLevel] = 110; x[sDelayToRev] = 80;
    x[sDelayMode] = 127;                       // LONG: wow and diffusion, a wash
    x[sDelayWow] = 70; x[sDelayDiff] = 96;
    x[sRevLevel] = 120;
    x[sRevMode] = 127;                         // HALL, big, a little shimmer
    x[sRevSize] = 116; x[sRevMod] = 70; x[sRevShimmer] = 40;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 30;
    s.master[3] = 122;
}

void drift_begin(Pattern &p, uint32_t seed) {
    begin(p, seed, kRoot, Scale::dorian, 50);
}

void pad(Pattern &p, uint8_t vel, bool bloom) {
    for (int b = 0; b < 4; b++) {
        const ChordType type = bloom && b == 0 ? ChordType::min9 : kBars[b].type;
        chord(p, kPad, b * 16, kBars[b].root, type, vel, L16);
    }
}

void drone(Pattern &p, uint8_t vel) {
    for (int b = 0; b < 4; b++) trig(p, kDrone, b * 16, kBars[b].drone, vel, L16);
}

// CHIME: 11 steps, three strikes pitched by TUN locks.
void chime(Pattern &p, uint8_t vel) {
    TrackPattern &tp = p.tracks[kChime];
    tp.length = 11;
    trig(p, kChime, 0, D, vel);
    trig(p, kChime, 4, D, uint8_t(vel - 16)).probability = 70;
    trig(p, kChime, 7, D, uint8_t(vel - 8));
    pattern_set_lock(p, kChime, 4, uint8_t(kSrc0), 100);
    pattern_set_lock(p, kChime, 7, uint8_t(kSrc0), 118);
}

// BELL: 13 steps at half speed (6.5 beats), two strikes.
void bell(Pattern &p, uint8_t vel) {
    TrackPattern &tp = p.tracks[kBell];
    tp.length = 13;
    tp.speed = uint8_t(Speed::x1_2);
    trig(p, kBell, 0, D, vel);
    trig(p, kBell, 8, D, uint8_t(vel - 10));
    pattern_set_lock(p, kBell, 8, uint8_t(kSrc0), 92);
}

// GLASS: 7 notes a quarter apart (1/4 speed), a melody that turns against the 4-bar chords.
void glass(Pattern &p, uint8_t vel) {
    TrackPattern &tp = p.tracks[kGlass];
    tp.length = 7;
    tp.speed = uint8_t(Speed::x1_4);
    static const uint8_t notes[7] = {74, 0, 77, 81, 0, 79, 72};   // D5 . F5 A5 . G5 C5
    for (int i = 0; i < 7; i++) {
        if (!notes[i]) continue;
        Step &s = trig(p, kGlass, i, notes[i], uint8_t(vel - (i & 1) * 12), L2);
        if (i == 3) s.probability = 75;
    }
}

void ticks(Pattern &p) {
    TrackPattern &tp = p.tracks[kTick];
    tp.length = 15;
    for (int i = 0; i < 15; i++) {
        Step &s = trig(p, kTick, i, D, (i % 4 == 0) ? 70 : 44);
        s.probability = (i % 4 == 0) ? 90 : 35;
    }
}

void pulse(Pattern &p, uint8_t vel) {
    for (int b = 0; b < 4; b++) {             // a heartbeat: the one, and a soft echo of it
        trig(p, kPulse, b * 16, D, vel);
        trig(p, kPulse, b * 16 + 6, D, uint8_t(vel - 30)).probability = 60;
        trig(p, kPulse, b * 16 + 8, D, uint8_t(vel - 14));
    }
}

void shimmer(Pattern &p) {
    for (int b = 0; b < 4; b++) {
        Step &s = trig(p, kShimmer, b * 16 + 10, D, 60);
        s.probability = 50;
    }
}

void dawn(Pattern &p) { drift_begin(p, 0xD21F01u); pad(p, 84, false); drone(p, 80); chime(p, 80); }
void bells(Pattern &p) {
    drift_begin(p, 0xD21F02u); pad(p, 84, false); drone(p, 84); chime(p, 84); bell(p, 84); glass(p, 86);
}
void pulse_in(Pattern &p) {
    drift_begin(p, 0xD21F03u); pad(p, 80, false); drone(p, 88); chime(p, 80); bell(p, 80); glass(p, 88);
    ticks(p); pulse(p, 96);
}
void haze(Pattern &p) {
    drift_begin(p, 0xD21F04u); pad(p, 92, false); glass(p, 80); shimmer(p);
    for (int i = 0; i < 7; i++) pattern_set_lock(p, kGlass, i, uint8_t(kSna), 110);
}
void bloom(Pattern &p) {
    drift_begin(p, 0xD21F05u); pad(p, 88, true); drone(p, 90); chime(p, 88); bell(p, 86); glass(p, 92);
    ticks(p); pulse(p, 100); shimmer(p);
}
void dusk(Pattern &p) { drift_begin(p, 0xD21F06u); pad(p, 76, false); drone(p, 76); bell(p, 70); }

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 62;
    s.scale = uint8_t(Scale::dorian);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kPulse, kTick}), mutes({kPad, kDrone}), mutes({kChime, kBell, kGlass}),
        mutes({kPulse, kTick, kChime, kBell}), mutes({kGlass}), mutes({kDrone}), mutes({kShimmer, kTick}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    dawn(bank[0]); bells(bank[1]); pulse_in(bank[2]); haze(bank[3]); bloom(bank[4]); dusk(bank[5]);
    set_chain(project, {{0, 0, 2, 0}, {0, 1, 2, 0}, {0, 2, 2, 0}, {0, 3, 2, 0}, {0, 4, 2, 0}, {0, 5, 2, 0}});

    // Scene 2 WASH: everything further into the hall and the echo, the pad brighter.
    Scene &wash = scenes.scenes[1];
    scene_set(wash, kGlass, kSna, 120);
    scene_set(wash, kChime, kSna, 110);
    scene_set(wash, kPad, kSnb, 127);
    scene_set(wash, kPad, kSrc2, 100);
    scene_set(wash, kBell, kSnb, 120);
    // Scene 3 UNDER: everything darker, the drone forward.
    Scene &under = scenes.scenes[2];
    scene_set(under, kPad, kSrc2, 40);
    scene_set(under, kDrone, kSrc2, 70);
    scene_set(under, kGlass, kAmp3, 50);

    Macro &light = macros.macros[1];            // B LIGHT: the pad and drone open
    target(light, 0, project.kit, kPad, kSrc2, 110);
    target(light, 1, project.kit, kDrone, kSrc2, 76);
    target(light, 2, project.kit, kPad, kSrc1, 100);
    Macro &wash_m = macros.macros[2];           // C WASH: the bells into the hall
    target(wash_m, 0, project.kit, kGlass, kSnb, 127);
    target(wash_m, 1, project.kit, kChime, kSnb, 127);
    target(wash_m, 2, project.kit, kBell, kSnb, 127);
    project.bank = 0;
    project.slot = 0;
}

// 6 patterns x 2 passes x 4 bars = 48 bars, 2:24 at 80.
constexpr int kArrangementBars = 48;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    // BELLS: morph towards WASH and back
    {10, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {12, Cmd::set_morph, 0, 0, 0, kFull},
    {14, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {15, Cmd::set_morph, 0, 0, 0, 0},
    // HAZE into BLOOM: macro B LIGHT up, then home
    {28, Cmd::set_macro, 0, 1, 40, 0}, {30, Cmd::set_macro, 0, 1, 80, 0}, {32, Cmd::set_macro, 0, 1, 110, 0},
    {36, Cmd::set_macro, 0, 1, 60, 0}, {38, Cmd::set_macro, 0, 1, 20, 0}, {39, Cmd::set_macro, 0, 1, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
