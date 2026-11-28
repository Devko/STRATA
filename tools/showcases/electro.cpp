// ELECTRO: an electro showcase for STRATA. 128 BPM, E phrygian. The 808 kit with a broken,
// syncopated kick, a cowbell and zaps, a rubbery 16th bass with slides and octave jumps, and a
// hard-synced lead riff into the delay. Factory machines and sounds only, and light on the CPU:
// no POLY voices at all.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "ELECTRO";
constexpr uint16_t kTempoCenti = 12800;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"BEAT", "BELL", "LEAD", "BREAK", "FULL", "OUTRO"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kCowbell, kBass, kLead, kZap };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "COWBELL", "BASS", "LEAD", "ZAP"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 4;     // E
constexpr uint8_t E1 = 28, E2 = 40, F2 = 41, G2 = 43, A2 = 45, B2 = 47, D3 = 50, E3 = 52;
constexpr uint8_t kSwing = 52;

void kit(Kit &kit) {
    clear_kit(kit);
    // KICK: the 808, tuned to E1 (TUN 41 = 41.4 Hz), long boom.
    preset(kit.tracks[kKick], MachineId::kick, "808");
    kit.tracks[kKick].params[kSrc0] = 41;
    kit.tracks[kKick].params[kAmp0] = 100;
    // SNARE: the 808 snare, a little reverb.
    preset(kit.tracks[kSnare], MachineId::snare, "808SN");
    kit.tracks[kSnare].params[kAmp3] = 100;
    kit.tracks[kSnare].params[kSnb] = 36;
    // HATS: the 808's, closed tight, open choked by the closed.
    preset(kit.tracks[kClosed], MachineId::hat, "808HH");
    kit.tracks[kClosed].params[kAmp0] = 18;
    kit.tracks[kClosed].params[kAmp3] = 78;
    kit.tracks[kClosed].params[kPan] = 72;
    preset(kit.tracks[kOpen], MachineId::hat, "808HH");
    kit.tracks[kOpen].params[kAmp0] = 66;
    kit.tracks[kOpen].params[kAmp3] = 60;
    kit.tracks[kOpen].params[kPan] = 60;
    // COWBELL: PERC's cowbell, pitched by locks into a two-note figure.
    preset(kit.tracks[kCowbell], MachineId::perc, "COWBEL");
    kit.tracks[kCowbell].params[kAmp3] = 80;
    kit.tracks[kCowbell].params[kPan] = 44;
    kit.tracks[kCowbell].params[kSna] = 30;
    // BASS: VA MONO's RUBBER, short and bouncy.
    preset(kit.tracks[kBass], MachineId::va_mono, "RUBBER");
    kit.tracks[kBass].params[kSrc2] = 36;
    kit.tracks[kBass].params[kAmp0] = 34;
    kit.tracks[kBass].level = 1.2f;
    // LEAD: VA MONO's SYNC, the riff, into the dotted delay; an LFO on the sync amount.
    preset(kit.tracks[kLead], MachineId::va_mono, "SYNC");
    kit.tracks[kLead].params[kAmp3] = 70;
    kit.tracks[kLead].params[kPan] = 64;
    kit.tracks[kLead].params[kSna] = 60;
    kit.tracks[kLead].params[kSnb] = 30;
    lfo(kit.tracks[kLead], LfoWave::triangle, sync_rate(k2B), 30, kSrc2);
    // ZAP: PERC's ZAP, the laser blips.
    preset(kit.tracks[kZap], MachineId::perc, "ZAP");
    kit.tracks[kZap].params[kAmp3] = 74;
    kit.tracks[kZap].params[kPan] = 84;
    kit.tracks[kZap].params[kSna] = 50;

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 30, 15, 0};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 2;
    kit.duck.hold = 6;
    kit.duck.release = 40;
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayFb] = 60; x[sDelayTone] = 70; x[sDelayLevel] = 110;
    x[sRevDecay] = 50; x[sRevLevel] = 100;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 48;
    s.master[3] = 118;
}

// The kick: a two-bar break, the second bar answering the first.
void kicks(Pattern &p, bool busy) {
    static const int a[] = {0, 7, 10};
    static const int b[] = {0, 3, 10, 13};
    for (int bar = 0; bar < 4; bar++) {
        const int at = bar * 16;
        if ((bar & 1) == 0) for (int s : a) trig(p, kKick, at + s, D, s == 0 ? 124 : 108);
        else for (int s : b) trig(p, kKick, at + s, D, s == 0 ? 120 : 104);
    }
    if (busy) trig(p, kKick, 62, D, 96).ratchet = ratchet_pack(2, RatchetRamp::up);
}

void snares(Pattern &p, bool ghosts) {
    for (int bar = 0; bar < 4; bar++) {
        trig(p, kSnare, bar * 16 + 4, D, 110);
        trig(p, kSnare, bar * 16 + 12, D, 114);
        if (ghosts) trig(p, kSnare, bar * 16 + 15, D, 56).probability = 50;
    }
    Step &r = trig(p, kSnare, 63, D, 90);
    r.ratchet = ratchet_pack(4, RatchetRamp::up);
    r.condition = condition_pack(Condition::FILL);
}

void hats(Pattern &p, bool open) {
    for (int i = 0; i < 64; i++) {
        if (open && (i % 8) == 6) { trig(p, kOpen, i, D, 84); continue; }
        trig(p, kClosed, i, D, (i & 1) ? 54 : ((i & 3) == 0 ? 92 : 74));
    }
    p.tracks[kClosed].steps[31].ratchet = ratchet_pack(3, RatchetRamp::down);
}

void cowbell(Pattern &p) {
    static const int at[] = {2, 5, 11, 18, 21, 27, 30};
    for (int half = 0; half < 2; half++) {
        for (int i = 0; i < 7; i++) {
            const int s = half * 32 + at[i];
            trig(p, kCowbell, s, D, uint8_t(i == 0 ? 96 : 76));
            if (i == 2 || i == 5) pattern_set_lock(p, kCowbell, s, uint8_t(kSrc0), 70);
        }
    }
}

// The bass: 16ths around the kick, E with jumps to the octave and the phrygian F, slides back.
void bass(Pattern &p, bool busy) {
    struct N { int step; uint8_t note; uint8_t flags; };
    static const N line[] = {
        {1, E2, 0}, {2, E3, kStepAccent}, {4, E2, 0}, {5, E2, 0}, {8, G2, kStepSlide}, {9, E2, 0},
        {11, E2, 0}, {12, E3, kStepAccent}, {14, F2, kStepSlide}, {15, E2, 0},
    };
    for (int bar = 0; bar < 4; bar++) {
        for (const N &n : line) {
            if (!busy && (n.step == 5 || n.step == 9)) continue;
            Step &s = trig(p, kBass, bar * 16 + n.step, n.note, (n.flags & kStepAccent) ? 112 : 88);
            s.flags |= n.flags;
        }
    }
    p.tracks[kBass].steps[62] = Step{};
    trig(p, kBass, 62, E1, 100).flags |= kStepSlide;
}

// The lead: a call over two bars, the answer climbs.
void lead(Pattern &p, uint8_t vel) {
    struct N { int step; uint8_t note; uint8_t len; };
    static const N line[] = {
        {0, 64, L1h}, {3, 64, L1}, {6, 67, L2}, {10, 65, L1}, {12, 64, L3},
        {16, 64, L1h}, {19, 64, L1}, {22, 71, L2}, {26, 69, L1}, {28, 67, L1}, {30, 74, L2},
    };
    for (int half = 0; half < 2; half++) {
        for (const N &n : line) {
            Step &s = trig(p, kLead, half * 32 + n.step, n.note, vel, n.len);
            if (n.step == 30) s.flags |= kStepSlide;
        }
    }
    pattern_set_lock(p, kLead, 22, uint8_t(kSrc2), 110);
    pattern_set_lock(p, kLead, 54, uint8_t(kSrc2), 118);
}

void zaps(Pattern &p) {
    static const int at[] = {14, 30, 46, 57};
    static const uint8_t tun[] = {90, 70, 100, 60};
    for (int i = 0; i < 4; i++) {
        trig(p, kZap, at[i], D, 84).probability = i == 3 ? 60 : 100;
        pattern_set_lock(p, kZap, at[i], uint8_t(kSrc0), tun[i]);
    }
}

void eb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::phrygian, kSwing); }
void beat(Pattern &p) { eb(p, 0xE1EC01u); kicks(p, false); snares(p, false); hats(p, false); }
void bell(Pattern &p) {
    eb(p, 0xE1EC02u); kicks(p, false); snares(p, false); hats(p, true); cowbell(p); bass(p, false);
}
void lead_in(Pattern &p) {
    eb(p, 0xE1EC03u); kicks(p, true); snares(p, true); hats(p, true); cowbell(p); bass(p, true); lead(p, 96);
}
// BREAK: no kick or snare, the bass and lead with zaps, the hats thin.
void breakdown(Pattern &p) {
    eb(p, 0xE1EC04u); bass(p, false); lead(p, 90); zaps(p);
    for (int i = 2; i < 64; i += 4) trig(p, kClosed, i, D, 60);
    for (int i = 56; i < 64; i++) trig(p, kSnare, i, D, uint8_t(56 + (i - 56) * 8));
}
void full(Pattern &p) {
    eb(p, 0xE1EC05u); kicks(p, true); snares(p, true); hats(p, true); cowbell(p); bass(p, true);
    lead(p, 104); zaps(p);
}
void outro(Pattern &p) { eb(p, 0xE1EC06u); kicks(p, false); snares(p, false); hats(p, false); cowbell(p); }

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 64;
    s.scale = uint8_t(Scale::phrygian);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kKick, kSnare}), mutes({kLead}), mutes({kBass, kLead, kZap}),
        mutes({kClosed, kOpen}), mutes({kBass}), mutes({kCowbell, kZap}), mutes({kKick, kBass}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    beat(bank[0]); bell(bank[1]); lead_in(bank[2]); breakdown(bank[3]); full(bank[4]); outro(bank[5]);
    set_chain(project, {{0, 0, 2, 0}, {0, 1, 2, 0}, {0, 2, 4, 0}, {0, 3, 2, 0}, {0, 4, 4, 0}, {0, 5, 2, 0}});

    // Scene 2 ROBOT: bass and lead crushed by the lo-fi stage, the lead into the delay.
    Scene &robot = scenes.scenes[1];
    scene_set(robot, kBass, kLof, 70);
    scene_set(robot, kLead, kLof, 80);
    scene_set(robot, kLead, kSna, 110);
    scene_set(robot, kCowbell, kSna, 90);
    // Scene 3 DARK: bass and lead close.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kBass, kSrc2, 20);
    scene_set(dark, kLead, kSrc2, 40);
    scene_set(dark, kClosed, kSrc1, 60);

    Macro &squelch = macros.macros[1];          // B SQUELCH: the bass opens and resonates
    target(squelch, 0, project.kit, kBass, kSrc2, 90);
    target(squelch, 1, project.kit, kBass, kSrc3, 110);
    target(squelch, 2, project.kit, kBass, kAmp1, 110);
    Macro &echo = macros.macros[2];             // C ECHO: lead, cowbell and zaps into the delay
    target(echo, 0, project.kit, kLead, kSna, 124);
    target(echo, 1, project.kit, kCowbell, kSna, 110);
    target(echo, 2, project.kit, kZap, kSna, 120);
    project.bank = 0;
    project.slot = 0;
}

// Beat 8, bell 8, lead 16, break 8, full 16, outro 8 = 64 bars, 2:00 at 128.
constexpr int kArrangementBars = 64;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {7, Cmd::set_fill, 0, 1, 0, 0}, {8, Cmd::set_fill, 0, 0, 0, 0},
    {15, Cmd::set_fill, 0, 1, 0, 0}, {16, Cmd::set_fill, 0, 0, 0, 0},
    // LEAD: B SQUELCH up over four bars, back over two
    {24, Cmd::set_macro, 0, 1, 40, 0}, {25, Cmd::set_macro, 0, 1, 80, 0}, {26, Cmd::set_macro, 0, 1, 110, 0},
    {28, Cmd::set_macro, 0, 1, 50, 0}, {29, Cmd::set_macro, 0, 1, 0, 0},
    {31, Cmd::set_fill, 0, 1, 0, 0}, {32, Cmd::set_fill, 0, 0, 0, 0},
    // BREAK: morph to ROBOT and back
    {34, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {35, Cmd::set_morph, 0, 0, 0, kFull},
    {38, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {39, Cmd::set_morph, 0, 0, 0, 0},
    // FULL: C ECHO for a bar, and the drums alone for two
    {46, Cmd::set_macro, 0, 2, 110, 0}, {47, Cmd::set_macro, 0, 2, 0, 0},
    {52, Cmd::mute_recall, 0, 3, 0, 0}, {54, Cmd::mute_recall, 0, 0, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
