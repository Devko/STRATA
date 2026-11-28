// NIGHT: SMASH's drum chaos on a night drive. 132 BPM, A minor. A synthwave car-at-night
// progression (Am F C G) on a detuned string pad, a driving octave bass in 8ths, a gliding saw
// lead with the melody into the echo; under it the SMASH treatment: ghost snares in random order,
// hats at double speed shuffled, ratchets and rolls over LEN, a big roomy snare, a TUNNEL build of
// rolls, beat repeats in the arrangement. Factory machines and sounds only; every note written
// here.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "NIGHT";
constexpr uint16_t kTempoCenti = 13200;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"IGNITE", "CRUISE", "NEON", "TUNNEL", "OVRDRV", "HALF"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kGhost, kBass, kPad, kLead };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "GHOST", "BASS", "PAD", "LEAD"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 9;     // A
constexpr uint8_t kSwing = 50;   // straight: synthwave drives
constexpr uint8_t r64 = 0, r48 = 1, r32 = 2, r16t = 3, r16 = 4;

// Am F C G, a bar each.
struct Bar { uint8_t bass; uint8_t pad; ChordType type; };
constexpr Bar kBars[4] = {
    {33, 57, ChordType::minor}, {29, 53, ChordType::major}, {36, 60, ChordType::major}, {31, 55, ChordType::major},
};

void kit(Kit &kit) {
    clear_kit(kit);
    // KICK: PUNCH, tuned to A1, a little longer than SMASH's.
    preset(kit.tracks[kKick], MachineId::kick, "PUNCH");
    kit.tracks[kKick].params[kSrc0] = 64;
    kit.tracks[kKick].params[kAmp0] = 60;
    // SNARE: the 909 snare, bodied, deep in a short bright room: the 80s snare.
    preset(kit.tracks[kSnare], MachineId::snare, "909SN");
    kit.tracks[kSnare].params[kAmp3] = 96;
    kit.tracks[kSnare].params[kAmp2] = 70;
    kit.tracks[kSnare].params[kSnb] = 90;
    preset(kit.tracks[kClosed], MachineId::hat, "TIGHT");
    kit.tracks[kClosed].params[kAmp3] = 72;
    kit.tracks[kClosed].params[kPan] = 76;
    preset(kit.tracks[kOpen], MachineId::hat, "OPEN");
    kit.tracks[kOpen].params[kAmp0] = 50;
    kit.tracks[kOpen].params[kAmp3] = 56;
    kit.tracks[kOpen].params[kPan] = 54;
    preset(kit.tracks[kGhost], MachineId::snare, "SHORT");
    kit.tracks[kGhost].params[kAmp3] = 66;
    kit.tracks[kGhost].params[kPan] = 46;
    kit.tracks[kGhost].params[kSnb] = 30;
    // BASS: VA MONO's FAT, a plucky saw: the octave-8ths drive.
    preset(kit.tracks[kBass], MachineId::va_mono, "FAT");
    kit.tracks[kBass].params[kSrc2] = 64;
    kit.tracks[kBass].params[kAmp0] = 50;
    kit.tracks[kBass].params[kAmp1] = 52;
    kit.tracks[kBass].params[kSus] = 0;
    kit.tracks[kBass].level = 1.8f;
    // PAD: VA POLY's STRING, the detuned string machine, a slow LFO on its cutoff, in the hall.
    preset(kit.tracks[kPad], MachineId::va_poly, "STRING");
    kit.tracks[kPad].params[kAtk] = 70;
    kit.tracks[kPad].params[kRel] = 70;
    kit.tracks[kPad].params[kAmp3] = 64;
    kit.tracks[kPad].params[kSnb] = 70;
    lfo(kit.tracks[kPad], LfoWave::triangle, sync_rate(k4B), 26, kSrc2);
    // LEAD: VA MONO's LEAD, more glide, into the dotted echo.
    preset(kit.tracks[kLead], MachineId::va_mono, "LEAD");
    kit.tracks[kLead].params[kAmp2] = 56;     // GLD
    kit.tracks[kLead].params[kAmp3] = 60;
    kit.tracks[kLead].params[kSna] = 80;
    kit.tracks[kLead].params[kSnb] = 50;
    lfo(kit.tracks[kLead], LfoWave::sine, sync_rate(kQuarter), 6, kSrc0);   // a little vibrato

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 35, 40, 15};
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = duck[t];
    kit.duck.source = 0;
    kit.duck.attack_ms = 2;
    kit.duck.hold = 6;
    kit.duck.release = 60;
}

void mixer(ProjectSettings &s) {
    const ProjectSettings defaults;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    uint8_t *x = s.sends;
    x[sDelayFb] = 64; x[sDelayTone] = 60; x[sDelayLevel] = 112; x[sDelayToRev] = 40;
    x[sRevDecay] = 60; x[sRevDamp] = 50; x[sRevLevel] = 112;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 54;
    s.master[3] = 96;
}

// SMASH's break, straightened for the drive: mode 0 steady, 1 shredded, 2 rolls everywhere.
void drums(Pattern &p, int mode) {
    for (int half = 0; half < 2; half++) {
        const int o = half * 32;
        trig(p, kKick, o + 0, D, 124);
        trig(p, kKick, o + 7, D, 100).condition = condition_pack_a_b(1, 2);
        trig(p, kKick, o + 10, D, 114);
        trig(p, kSnare, o + 4, D, 116);
        trig(p, kSnare, o + 12, D, 118);
        trig(p, kKick, o + 16, D, 120);
        trig(p, kKick, o + 18, D, 104).ratchet = ratchet_pack(mode ? 3 : 2, RatchetRamp::down);
        trig(p, kKick, o + 26, D, 112);
        trig(p, kSnare, o + 20, D, 116);
        trig(p, kSnare, o + 22, D, 80).condition = condition_pack_a_b(2, 2);
        trig(p, kSnare, o + 28, D, 120);
        Step &a = trig(p, kSnare, o + 30, D, 96);
        a.ratchet = ratchet_pack(mode == 2 ? 6 : (mode ? 4 : 2), RatchetRamp::up);
        pattern_set_lock(p, kSnare, o + 30, uint8_t(kSrc0), 96);
    }
    if (mode == 2) {
        for (const int s : {4, 36}) p.tracks[kSnare].steps[s].ratchet = ratchet_pack(3, RatchetRamp::down);
        p.tracks[kKick].steps[32].ratchet = ratchet_pack(8, RatchetRamp::down);
    }
    for (int i = 1; i < 64; i += 2) trig(p, kGhost, i, D, uint8_t(28 + (i * 11) % 28)).probability = 55;
    if (mode) p.tracks[kGhost].play = uint8_t(PlayMode::rnd1);
    if (mode) {
        p.tracks[kClosed].speed = uint8_t(Speed::x2);
        p.tracks[kClosed].play = uint8_t(mode == 2 ? PlayMode::rnd1 : PlayMode::rnd2);
    }
    for (int i = 0; i < 64; i++) {
        if (i % 8 == 6 && !mode) { trig(p, kOpen, i, D, 80); continue; }
        Step &s = trig(p, kClosed, i, D, (i & 3) == 0 ? 92 : ((i & 1) ? 46 : 72));
        if (i & 1) s.probability = mode ? 60 : 50;
        if (mode == 2 && i % 8 == 7) s.ratchet = ratchet_pack(3, RatchetRamp::up);
    }
    if (mode) for (const int i : {14, 46}) trig(p, kOpen, i, D, 86);
    Step &f = trig(p, kSnare, 60, D, 80, L4);
    f.ratchet = ratchet_pack_span(mode == 2 ? r48 : r32, RatchetRamp::up);
    f.condition = condition_pack(mode == 2 ? Condition::NONE : Condition::FILL);
    pattern_set_lock(p, kSnare, 60, uint8_t(kSrc0), 96);
}

// The drive: octave 8ths on the root, the last 8th of the bar a passing note.
void bass(Pattern &p) {
    for (int b = 0; b < 4; b++) {
        const uint8_t r = kBars[b].bass;
        for (int e = 0; e < 8; e++) {
            const uint8_t note = (e & 1) ? uint8_t(r + 12) : r;
            Step &s = trig(p, kBass, b * 16 + e * 2, e == 7 ? uint8_t(r + 7) : note, (e & 1) ? 88 : 108, L1h);
            if (e == 0) s.flags |= kStepAccent;
        }
    }
}

void pad(Pattern &p, uint8_t vel) {
    for (int b = 0; b < 4; b++) chord(p, kPad, b * 16, kBars[b].pad, kBars[b].type, vel, L16);
}

// The lead: eighth notes over Am F C G, gliding between them.
void lead(Pattern &p, uint8_t vel) {
    struct N { int step; uint8_t note; uint8_t len; };
    static const N line[] = {
        {0, 76, L2}, {4, 81, L2}, {8, 83, L1h}, {10, 84, L2}, {12, 83, L1h}, {14, 81, L2},
        {16, 84, L4}, {22, 81, L2}, {26, 77, L2}, {28, 81, L1h}, {30, 84, L2},
        {32, 76, L2}, {34, 79, L3}, {38, 76, L1h}, {40, 72, L4}, {46, 76, L2},
        {48, 74, L3}, {52, 79, L3}, {56, 83, L4}, {60, 86, L2}, {62, 83, L2},
    };
    for (const N &n : line) trig(p, kLead, n.step, n.note, vel, n.len);
}

void nb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::minor, kSwing); }
void ignite(Pattern &p) { nb(p, 0x416401u); drums(p, 0); bass(p); }
void cruise(Pattern &p) { nb(p, 0x416402u); drums(p, 0); bass(p); pad(p, 84); }
void neon(Pattern &p) { nb(p, 0x416403u); drums(p, 1); bass(p); pad(p, 80); lead(p, 100); }
// TUNNEL: no kick or bass; the pad swells, the lead in the echo, the snare in rolls over LEN,
// faster and higher, the hats random at double speed.
void tunnel(Pattern &p) {
    nb(p, 0x416404u);
    pad(p, 96);
    lead(p, 90);
    for (int i = 0; i < 64; i += 16) pattern_set_lock(p, kLead, i, uint8_t(kSna), 120);
    Step &s1 = trig(p, kSnare, 16, D, 66, L16);
    s1.ratchet = ratchet_pack_span(r16, RatchetRamp::up);
    Step &s2 = trig(p, kSnare, 32, D, 80, L16);
    s2.ratchet = ratchet_pack_span(r16t, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 32, uint8_t(kSrc0), 84);
    Step &s3 = trig(p, kSnare, 48, D, 100, L12);
    s3.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
    pattern_set_lock(p, kSnare, 48, uint8_t(kSrc0), 100);
    p.tracks[kClosed].speed = uint8_t(Speed::x2);
    p.tracks[kClosed].play = uint8_t(PlayMode::rnd1);
    for (int i = 0; i < 64; i++) trig(p, kClosed, i, D, uint8_t(30 + i)).probability = 60;
    Step &k = trig(p, kKick, 60, D, 110, L4);
    k.ratchet = ratchet_pack_span(r16, RatchetRamp::up);
}
void overdrive(Pattern &p) { nb(p, 0x416405u); drums(p, 2); bass(p); pad(p, 76); lead(p, 104); }
void half(Pattern &p) {
    nb(p, 0x416406u);
    drums(p, 1);
    for (const int t : {kKick, kSnare, kOpen, kGhost}) {
        p.tracks[t].speed = uint8_t(Speed::x1_2);
        p.tracks[t].length = 32;
    }
    for (int b = 0; b < 4; b++) trig(p, kBass, b * 16, kBars[b].bass, 108, L8).flags |= kStepAccent;
    pad(p, 90);
    lead(p, 92);
}

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
        0, mutes({kKick, kBass}), mutes({kLead}), mutes({kPad, kLead}),
        mutes({kClosed, kOpen, kGhost}), mutes({kBass}), mutes({kKick, kSnare, kClosed, kOpen, kGhost}), mutes({kPad}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    ignite(bank[0]); cruise(bank[1]); neon(bank[2]); tunnel(bank[3]); overdrive(bank[4]); half(bank[5]);
    set_chain(project, {{0, 0, 1, 0}, {0, 1, 2, 0}, {0, 2, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0},
                        {0, 5, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0}});

    // Scene 2 NEON: pad and lead open, everything deeper in the echo and the hall.
    Scene &neon_s = scenes.scenes[1];
    scene_set(neon_s, kPad, kSrc2, 110);
    scene_set(neon_s, kLead, kSna, 120);
    scene_set(neon_s, kLead, kSrc2, 100);
    scene_set(neon_s, kSnare, kSnb, 120);
    // Scene 3 CRUSH: SMASH's crush on the drums.
    Scene &crush = scenes.scenes[2];
    scene_set(crush, kKick, kFlt0, 110);
    scene_set(crush, kSnare, kLof, 80);
    scene_set(crush, kClosed, kLof, 90);
    scene_set(crush, kGhost, kLof, 90);

    Macro &bright = macros.macros[1];           // B HEADLIGHTS: pad, lead and bass open
    target(bright, 0, project.kit, kPad, kSrc2, 112);
    target(bright, 1, project.kit, kLead, kSrc2, 110);
    target(bright, 2, project.kit, kBass, kSrc2, 90);
    target(bright, 3, project.kit, kBass, kSrc3, 80);
    Macro &echo = macros.macros[2];             // C ECHO: lead and snare into the delay
    target(echo, 0, project.kit, kLead, kSna, 127);
    target(echo, 1, project.kit, kSnare, kSna, 90);
    target(echo, 2, project.kit, kGhost, kSna, 110);
    project.bank = 0;
    project.slot = 0;
}

// IGNITE 4, CRUISE 8, NEON 8, TUNNEL 4, OVRDRV 12, HALF 8, TUNNEL 4, OVRDRV 12 = 60 bars, 1:49.
constexpr int kArrangementBars = 60;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {3, Cmd::set_fill, 0, 1, 0, 0}, {4, Cmd::set_fill, 0, 0, 0, 0},
    {9, Cmd::set_macro, 0, 1, 60, 0}, {10, Cmd::set_macro, 0, 1, 110, 0},
    {11, Cmd::set_macro, 0, 1, 50, 0}, {12, Cmd::set_macro, 0, 1, 0, 0},
    {15, Cmd::set_repeat, 0, 2, 1, 0}, {16, Cmd::set_repeat, 0, 2, 0, 0},
    {19, Cmd::set_repeat, 0, 6, 1, 0}, {20, Cmd::set_repeat, 0, 6, 0, 0},
    // TUNNEL: morph to NEON over the build, back at the drop
    {20, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {22, Cmd::set_morph, 0, 0, 0, kFull},
    {24, Cmd::set_morph, 0, 0, 0, 0},
    {27, Cmd::set_macro, 0, 2, 120, 0}, {28, Cmd::set_macro, 0, 2, 0, 0},
    {31, Cmd::set_repeat, 0, 3, 1, 0}, {32, Cmd::set_repeat, 0, 3, 0, 0},
    {33, Cmd::mute_recall, 0, 6, 0, 0}, {34, Cmd::mute_recall, 0, 0, 0, 0},
    {35, Cmd::set_fill, 0, 1, 0, 0}, {36, Cmd::set_fill, 0, 0, 0, 0},
    {40, Cmd::set_macro, 0, 1, 90, 0}, {42, Cmd::set_macro, 0, 1, 0, 0},
    {43, Cmd::set_repeat, 0, 6, 1, 0}, {44, Cmd::set_repeat, 0, 6, 0, 0},
    {44, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {46, Cmd::set_morph, 0, 0, 0, kFull},
    {48, Cmd::set_morph, 0, 0, 0, 0},
    {51, Cmd::set_repeat, 0, 2, 1, 0}, {52, Cmd::set_repeat, 0, 2, 0, 0},
    {53, Cmd::set_macro, 0, 2, 120, 0}, {54, Cmd::set_macro, 0, 2, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
    {58, Cmd::set_repeat, 0, 3, 1, 0}, {59, Cmd::set_repeat, 0, 3, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
