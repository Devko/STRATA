// STEEL: breakbeat, rolls and a strong reese, dead straight. 140 BPM, E minor. No swing, no
// micro-timing, no FEEL, and the tracks play forward: the energy comes from the break, ratchets
// and rolls over LEN, all on the grid. A loud driven reese carries it, a held pad sits low under
// it, and a pluck plays two or three notes in four bars. Factory machines and sounds only; every
// note written here.
#include "showcase_head.inc"

namespace {

constexpr const char *kName = "STEEL";
constexpr uint16_t kTempoCenti = 14000;
constexpr int kPatterns = 6;
constexpr const char *kPatternNames[kPatterns] = {"BREAK", "REESE", "ROLLS", "BUILD", "STEEL", "HALF"};
enum Track : int { kKick = 0, kSnare, kClosed, kOpen, kGhost, kBass, kPad, kPluck };
constexpr const char *kTrackNames[kTracks] = {"KICK", "SNARE", "CLOSED", "OPEN", "GHOST", "REESE", "PAD", "PLUCK"};
constexpr int kStemSlot = 4;

constexpr uint8_t kRoot = 4;     // E
constexpr uint8_t kSwing = 50;   // straight
constexpr uint8_t r64 = 0, r48 = 1, r32 = 2, r16t = 3, r16 = 4;
constexpr uint8_t E1 = 28, G1 = 31, A1 = 33, B1 = 35, C2 = 36, D2 = 38, E2 = 40;

void kit(Kit &kit) {
    clear_kit(kit);
    preset(kit.tracks[kKick], MachineId::kick, "PUNCH");
    kit.tracks[kKick].params[kSrc0] = 52;
    kit.tracks[kKick].params[kAmp0] = 52;
    preset(kit.tracks[kSnare], MachineId::snare, "909SN");
    kit.tracks[kSnare].params[kAmp3] = 100;
    kit.tracks[kSnare].params[kSnb] = 40;
    preset(kit.tracks[kClosed], MachineId::hat, "TIGHT");
    kit.tracks[kClosed].params[kAmp3] = 74;
    kit.tracks[kClosed].params[kPan] = 74;
    preset(kit.tracks[kOpen], MachineId::hat, "OPEN");
    kit.tracks[kOpen].params[kAmp0] = 48;
    kit.tracks[kOpen].params[kAmp3] = 56;
    kit.tracks[kOpen].params[kPan] = 56;
    preset(kit.tracks[kGhost], MachineId::snare, "SHORT");
    kit.tracks[kGhost].params[kSrc2] = 75;    // TON as heard (SHORT's TON was 75 before the preset review)
    kit.tracks[kGhost].params[kAmp3] = 66;
    kit.tracks[kGhost].params[kPan] = 50;
    // REESE: the star. Driven, loud, a slow half-bar LFO opening its filter.
    preset(kit.tracks[kBass], MachineId::va_mono, "REESE");
    kit.tracks[kBass].params[kSrc2] = 48;
    kit.tracks[kBass].params[kOscDrv] = 88;
    kit.tracks[kBass].level = 1.8f;
    lfo(kit.tracks[kBass], LfoWave::triangle, sync_rate(kHalf), 26, kSrc2);
    // PAD: VA POLY's WARM, low and dark, in the hall.
    preset(kit.tracks[kPad], MachineId::va_poly, "WARM");
    kit.tracks[kPad].params[kSrc2] = 40;
    kit.tracks[kPad].params[kAmp3] = 52;
    kit.tracks[kPad].params[kSnb] = 80;
    // PLUCK: two or three notes in the echo.
    preset(kit.tracks[kPluck], MachineId::va_mono, "PLUCK");
    kit.tracks[kPluck].params[kAmp3] = 110;
    kit.tracks[kPluck].params[kPan] = 72;
    kit.tracks[kPluck].params[kSna] = 96;
    kit.tracks[kPluck].params[kSnb] = 50;

    const uint8_t duck[kTracks] = {0, 0, 0, 0, 0, 30, 40, 15};
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
    x[sDelayFb] = 62; x[sDelayTone] = 56; x[sDelayLevel] = 112; x[sDelayToRev] = 36;
    x[sRevDecay] = 66; x[sRevDamp] = 80; x[sRevLevel] = 108;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
    s.master[1] = 56;
    s.master[3] = 98;
}

// The break, straight: mode 0 plain, 1 ratchets, 2 rolls everywhere.
void drums(Pattern &p, int mode) {
    for (int half = 0; half < 2; half++) {
        const int o = half * 32;
        trig(p, kKick, o + 0, D, 124);
        trig(p, kKick, o + 10, D, 114);
        trig(p, kSnare, o + 4, D, 116);
        trig(p, kSnare, o + 12, D, 118);
        trig(p, kKick, o + 16, D, 120);
        trig(p, kKick, o + 18, D, 104).ratchet = ratchet_pack(mode ? 2 : 1, RatchetRamp::none);
        trig(p, kKick, o + 26, D, 112);
        trig(p, kSnare, o + 20, D, 116);
        trig(p, kSnare, o + 28, D, 120);
        Step &a = trig(p, kSnare, o + 30, D, 96);
        a.ratchet = ratchet_pack(mode == 2 ? 4 : 2, RatchetRamp::up);
    }
    if (mode == 2) {
        for (const int s : {12, 44}) p.tracks[kSnare].steps[s].ratchet = ratchet_pack(3, RatchetRamp::down);
        Step &r = trig(p, kSnare, 22, D, 84, L2);
        r.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
        p.tracks[kKick].steps[32].ratchet = ratchet_pack(4, RatchetRamp::down);
    }
    for (const int g : {7, 9, 15, 23, 25, 31, 39, 41, 47, 55, 57, 63}) trig(p, kGhost, g, D, 44);
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 14) { trig(p, kOpen, i, D, 80); continue; }
        trig(p, kClosed, i, D, (i & 3) == 0 ? 92 : ((i & 1) ? 48 : 72));
        if (mode && i % 16 == 15) p.tracks[kClosed].steps[i].ratchet = ratchet_pack(mode == 2 ? 4 : 2, RatchetRamp::up);
    }
    Step &f = trig(p, kSnare, 60, D, 80, L4);
    f.ratchet = ratchet_pack_span(mode == 2 ? r48 : r32, RatchetRamp::up);
    f.condition = condition_pack(Condition::FILL);
}

// The reese: long notes, octave pops and slides, E minor.
void reese(Pattern &p) {
    struct N { int step; uint8_t note; uint8_t flags; uint8_t len; };
    static const N line[] = {
        {0, E1, kStepAccent, L6}, {6, E2, kStepSlide, L2}, {8, E1, 0, L4}, {12, D2, kStepSlide, L4},
        {16, C2, kStepAccent, L6}, {22, B1, kStepSlide, L2}, {24, G1, 0, L6}, {30, A1, kStepSlide, L2},
    };
    for (int half = 0; half < 2; half++)
        for (const N &n : line) {
            Step &s = trig(p, kBass, half * 32 + n.step, n.note, (n.flags & kStepAccent) ? 120 : 100, n.len);
            s.flags |= n.flags;
        }
}

void pad(Pattern &p, uint8_t vel) {
    chord(p, kPad, 0, 52, ChordType::minor, vel, L32);           // Em
    chord(p, kPad, 32, 48, ChordType::maj7, vel, L32);           // Cmaj7
}

void pluck(Pattern &p) {
    trig(p, kPluck, 6, 71, 96, L1);        // B4
    trig(p, kPluck, 38, 76, 92, L1);       // E5
    trig(p, kPluck, 54, 74, 80, L1).probability = 50;   // D5
}

void sb(Pattern &p, uint32_t seed) { begin(p, seed, kRoot, Scale::minor, kSwing); }
void brk(Pattern &p) { sb(p, 0x57EE01u); drums(p, 0); }
void reese_in(Pattern &p) { sb(p, 0x57EE02u); drums(p, 0); reese(p); }
void rolls(Pattern &p) { sb(p, 0x57EE03u); drums(p, 1); reese(p); pad(p, 80); pluck(p); }
// BUILD: no kick; the reese held, the pad up, the snare in rolls over LEN getting faster.
void build_up(Pattern &p) {
    sb(p, 0x57EE04u);
    pad(p, 96);
    pluck(p);
    trig(p, kBass, 0, E1, 110, L32);
    trig(p, kBass, 32, E2, 100, L32).flags |= kStepSlide;
    Step &s1 = trig(p, kSnare, 16, D, 64, L16);
    s1.ratchet = ratchet_pack_span(r16, RatchetRamp::up);
    Step &s2 = trig(p, kSnare, 32, D, 78, L16);
    s2.ratchet = ratchet_pack_span(r16t, RatchetRamp::up);
    Step &s3 = trig(p, kSnare, 48, D, 94, L8);
    s3.ratchet = ratchet_pack_span(r32, RatchetRamp::up);
    Step &s4 = trig(p, kSnare, 56, D, 110, L8);
    s4.ratchet = ratchet_pack_span(r64, RatchetRamp::up);
    for (int i = 0; i < 64; i += 2) trig(p, kClosed, i, D, uint8_t(40 + i));
}
void steel(Pattern &p) { sb(p, 0x57EE05u); drums(p, 2); reese(p); pad(p, 76); pluck(p); }
void half(Pattern &p) {
    sb(p, 0x57EE06u);
    drums(p, 1);
    for (const int t : {kKick, kSnare, kOpen, kGhost}) {
        p.tracks[t].speed = uint8_t(Speed::x1_2);
        p.tracks[t].length = 32;
    }
    reese(p);
    pad(p, 84);
}

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = 64;
    s.scale = uint8_t(Scale::minor);
    mixer(s);
    const uint8_t states[8] = {
        0, mutes({kKick, kBass}), mutes({kPad, kPluck}), mutes({kBass, kPad, kPluck}),
        mutes({kClosed, kOpen, kGhost}), mutes({kBass}), mutes({kKick, kSnare, kClosed, kOpen, kGhost}), mutes({kPad}),
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
    s.scene_a = 0;
    s.scene_b = 1;
    Pattern *bank = project.patterns[0];
    brk(bank[0]); reese_in(bank[1]); rolls(bank[2]); build_up(bank[3]); steel(bank[4]); half(bank[5]);
    set_chain(project, {{0, 0, 1, 0}, {0, 1, 2, 0}, {0, 2, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0},
                        {0, 5, 2, 0}, {0, 3, 1, 0}, {0, 4, 3, 0}});

    // Scene 2 GRIT: the reese opens and drives, the drums crushed a little.
    Scene &grit = scenes.scenes[1];
    scene_set(grit, kBass, kSrc2, 80);
    scene_set(grit, kBass, kSrc3, 70);
    scene_set(grit, kSnare, kLof, 60);
    scene_set(grit, kClosed, kLof, 70);
    // Scene 3 DEEP: reese and pad close, the pad into the hall.
    Scene &deep = scenes.scenes[2];
    scene_set(deep, kBass, kSrc2, 26);
    scene_set(deep, kPad, kSnb, 120);

    Macro &scream = macros.macros[1];           // B SCREAM: the reese opens and resonates
    target(scream, 0, project.kit, kBass, kSrc2, 100);
    target(scream, 1, project.kit, kBass, kSrc3, 104);
    target(scream, 2, project.kit, kBass, kAmp1, 110);
    Macro &echo = macros.macros[2];             // C ECHO: snare and pluck into the delay
    target(echo, 0, project.kit, kSnare, kSna, 100);
    target(echo, 1, project.kit, kPluck, kSna, 127);
    target(echo, 2, project.kit, kGhost, kSna, 100);
    project.bank = 0;
    project.slot = 0;
}

// BREAK 4, REESE 8, ROLLS 8, BUILD 4, STEEL 12, HALF 8, BUILD 4, STEEL 12 = 60 bars, 1:43.
constexpr int kArrangementBars = 60;
const uint16_t kFull = kMorphScale;
const Event kMoves[] = {
    {3, Cmd::set_fill, 0, 1, 0, 0}, {4, Cmd::set_fill, 0, 0, 0, 0},
    {9, Cmd::set_macro, 0, 1, 60, 0}, {10, Cmd::set_macro, 0, 1, 110, 0},
    {11, Cmd::set_macro, 0, 1, 50, 0}, {12, Cmd::set_macro, 0, 1, 0, 0},
    {15, Cmd::set_repeat, 0, 2, 1, 0}, {16, Cmd::set_repeat, 0, 2, 0, 0},
    {19, Cmd::set_fill, 0, 1, 0, 0}, {20, Cmd::set_fill, 0, 0, 0, 0},
    {20, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {22, Cmd::set_morph, 0, 0, 0, kFull},
    {24, Cmd::set_morph, 0, 0, 0, 0},
    {27, Cmd::set_macro, 0, 2, 120, 0}, {28, Cmd::set_macro, 0, 2, 0, 0},
    {31, Cmd::set_repeat, 0, 3, 1, 0}, {32, Cmd::set_repeat, 0, 3, 0, 0},
    {33, Cmd::mute_recall, 0, 3, 0, 0}, {34, Cmd::mute_recall, 0, 0, 0, 0},
    {35, Cmd::set_fill, 0, 1, 0, 0}, {36, Cmd::set_fill, 0, 0, 0, 0},
    {40, Cmd::set_macro, 0, 1, 90, 0}, {42, Cmd::set_macro, 0, 1, 0, 0},
    {44, Cmd::set_morph, 0, 0, 0, uint16_t(kFull / 2)}, {46, Cmd::set_morph, 0, 0, 0, kFull},
    {48, Cmd::set_morph, 0, 0, 0, 0},
    {51, Cmd::set_repeat, 0, 2, 1, 0}, {52, Cmd::set_repeat, 0, 2, 0, 0},
    {53, Cmd::set_macro, 0, 1, 110, 0}, {54, Cmd::set_macro, 0, 1, 0, 0},
    {55, Cmd::set_fill, 0, 1, 0, 0}, {56, Cmd::set_fill, 0, 0, 0, 0},
    {58, Cmd::set_repeat, 0, 3, 1, 0}, {59, Cmd::set_repeat, 0, 3, 0, 0},
};

}  // namespace

#include "showcase_tail.inc"
