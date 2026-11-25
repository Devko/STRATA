// The factory content. core/factory.hpp says what it is and why it lives here.
//
// Every sound choice below is made from the references in docs/SOUND.md and the knob mappings
// in dsp/*.cpp, without anyone having listened -- docs/UNIT_CHECKLIST.md lists what to audition.
// Where a value is derived (a frequency, a time) the comment gives the arithmetic so it can be
// checked against the mapping rather than taken on trust.
#include "factory.hpp"

#include "chord.hpp"
#include "scale.hpp"
#include "timing.hpp"

#include <initializer_list>

namespace strata {

namespace {

// --- parameter slots ------------------------------------------------------
//
// Named per page so a value reads as what it is. The machines own SRC and AMP and give the four
// slots their own names (dsp/*.cpp, `param_name`); the chain owns FLTR and FX for every machine.

constexpr int kSrc0 = kFactorySrc + 0, kSrc1 = kFactorySrc + 1, kSrc2 = kFactorySrc + 2,
              kSrc3 = kFactorySrc + 3;
constexpr int kAmp0 = kFactoryAmp + 0, kAmp1 = kFactoryAmp + 1, kAmp2 = kFactoryAmp + 2,
              kAmp3 = kFactoryAmp + 3;
constexpr int kDrv = kFactoryFltr + 0, kCut = kFactoryFltr + 1, kRes = kFactoryFltr + 2,
              kMod = kFactoryFltr + 3;
constexpr int kAtk = kFactoryAsr + 0, kSus = kFactoryAsr + 1, kRel = kFactoryAsr + 2;
constexpr int kLof = kFactoryFx + 0, kPan = kFactoryFx + 1, kSna = kFactoryFx + 2,
              kSnb = kFactoryFx + 3;

// The tracks of the default kit, by role.
enum Track : int { kKick = 0, kClap, kClosed, kOpen, kPerc, kBass, kChord, kLoop };

// The key: C minor, as it always was. Notes below are written in it.
constexpr uint8_t kRootPc = 0;                    // C, as the pattern's pitch class
constexpr uint8_t kRootNote = 60;                 // and the project's, in an octave
constexpr uint8_t G1 = 31, Ab1 = 32, C2 = 36, Eb2 = 39, G2 = 43, Ab2 = 44, Bb2 = 46, C3 = 48,
                  Eb3 = 51, G3 = 55, Ab3 = 56, C4 = 60, G4 = 67, C5 = 72;

// SAMPLE's placeholder loop (app/engine.cpp, build_placeholder_sample) is eight blips a fourth
// apart from 220 Hz: A3 D4 G4 C5 F5 Bb5 Eb6 Ab6, and note 60 + k plays slice k. Slice 0 (A) is
// the one that is not in C minor, so the factory patterns never ask for it.
constexpr uint8_t kSliceD4 = 61, kSliceG4 = 62, kSliceC5 = 63, kSliceF5 = 64, kSliceBb5 = 65,
                  kSliceEb6 = 66;

void set(KitTrack &track, const uint8_t (&src)[4], const uint8_t (&amp)[4]) {
    for (int i = 0; i < 4; i++) {
        track.params[kFactorySrc + i] = src[i];
        track.params[kFactoryAmp + i] = amp[i];
    }
}

// The VAs' OSC page: WAV OS2 SUB DRV (dsp/va.cpp).
void set_osc(KitTrack &track, const uint8_t (&osc)[4]) {
    for (int i = 0; i < 4; i++) track.params[kFactoryOsc + i] = osc[i];
}

// --- the kit --------------------------------------------------------------

void factory_kit(Kit &kit) {
    // The chain's pages first, the same on every track: drive off, the filter OFF but parked
    // fully open with a little resonance (so turning MOD on is not a sudden muffle), lo-fi off,
    // centred, both sends closed. Filter and lo-fi off are also the cheap settings
    // (docs/BUDGET.md), and a closed send costs nothing.
    for (int t = 0; t < kTracks; t++) {
        KitTrack &k = kit.tracks[t];
        k.params[kDrv] = 0;
        k.params[kCut] = 127;
        k.params[kRes] = 20;
        k.params[kMod] = 0;
        k.params[kLof] = 0;
        k.params[kPan] = 64;
        k.params[kSna] = 0;
        k.params[kSnb] = 0;
        k.level = 1.0f;
    }

    // KICK (dsp/kick.cpp, the 909-ish model). TUN 55 is 25 * 2^(2.2572 * 55/127) = 49.2 Hz,
    // G1: the fifth of C, the usual place for a kick in a minor key when the root would be
    // either 33 Hz (too low to punch) or 65 Hz (too high to be a kick). FMA/FMD give a pitch
    // drop with an 8 * 2^(4 * 56/127) = 27 ms time constant, inside the 20-70 ms docs/SOUND.md
    // says gives the click. DEC 72: 20 ms * 2^(5 * 0.567^2) = 61 ms time constant, about 280 ms
    // to -40 dB -- a techno kick, not an 808 boom. A little drive in the chain, which the 909
    // takes well.
    set(kit.tracks[kKick], {55, 72, 56, 72}, {72, 60, 44, 110});
    kit.tracks[kKick].params[kDrv] = 28;

    // CLAP: the SNARE machine in clap mode (CLP 64 = three bursts), mostly noise (SNP 100), the
    // bursts 2 + 38 * 28/127 = 10 ms apart, which is a 909 clap's spacing. A short body, and a
    // touch of reverb so the backbeat has a room.
    set(kit.tracks[kClap], {64, 100, 64, 64}, {54, 28, 40, 100});
    kit.tracks[kClap].params[kSnb] = 24;

    // HATS (dsp/hat.cpp, the 808 six squares). TON is the band-pass: 150 Hz * 2^(6 * TON/127),
    // so the knob's middle is 1.2 kHz -- a dull, clanky hat. 104 is 3.8 kHz for the closed hat
    // and 96 is 3.5 kHz for the open, a little darker and fuller. Closed DEC 24 is a 7 ms *
    // 2^(7 * 0.19) = 17 ms time constant; open DEC 70 is 102 ms, and the closed hat chokes it
    // (track 3 cuts track 4, app/engine.cpp) so an offbeat open hat closes on the next 16th.
    // Panned a little apart, the open one into the reverb.
    set(kit.tracks[kClosed], {64, 104, 40, 64}, {24, 64, 64, 84});
    kit.tracks[kClosed].params[kPan] = 54;
    set(kit.tracks[kOpen], {64, 96, 48, 64}, {70, 50, 64, 70});
    kit.tracks[kOpen].params[kPan] = 76;
    kit.tracks[kOpen].params[kSnb] = 16;

    // PERC as a rimshot, dsp/perc.cpp's documented settings for one: high, short, inharmonic.
    // Into the delay: a dub rim is half the point of a delay on a techno kit.
    set(kit.tracks[kPerc], {85, 48, 75, 20}, {12, 30, 10, 100});
    kit.tracks[kPerc].params[kPan] = 44;
    kit.tracks[kPerc].params[kSna] = 40;
    kit.tracks[kPerc].params[kSnb] = 20;

    // BASS (VA MONO). MOD 40 is the 24 dB low-pass; CUT 46 is 35 Hz * 2^(8.42 * 46/127) = 290 Hz
    // with some resonance (RES 56, about +8 dB of peak), and a short envelope (DEC 46: 94 ms)
    // sweeping about three octaves (ENV 72), so each note has a "wow" at its front. OSC: a saw
    // over the triangle sub (SUB 90) and a little drive (DRV 24) for the weight. Dry and centred: the bass is the one part
    // that must stay mono. LVL at the top and the fader at 1.25: balanced by rendering
    // (host/render.cpp), the bass sat 13 dB under the kick at 104, and wants to be 6-8 under.
    set(kit.tracks[kBass], {64, 40, 46, 56}, {46, 72, 18, 127});
    set_osc(kit.tracks[kBass], {0, 0, 90, 24});
    kit.tracks[kBass].level = 1.25f;

    // CHORD (VA POLY): a dub stab. MOD 70 is the 12 dB low-pass, CUT 70 = 870 Hz, a short
    // decay (DEC 66: 15 ms * 2^(7.32 * 0.52) = 210 ms) with some envelope, and DET 28 to spread
    // the voices a little. Then it lives in the sends: a dotted-eighth ping-pong and the reverb,
    // which is the dub-techno chord.
    set(kit.tracks[kChord], {64, 70, 70, 36}, {66, 56, 28, 92});
    set_osc(kit.tracks[kChord], {0, 0, 50, 0});   // a saw a voice, a little sub under it
    kit.tracks[kChord].params[kSna] = 72;
    kit.tracks[kChord].params[kSnb] = 64;

    // LOOP (SAMPLE). Eight slices of whatever is loaded -- the placeholder until a WAV is.
    // SLC must be 60, not 64: the slice count is 1 + knob * 16 / 128 in zones eight wide, so 64
    // asks for nine. ATK 0, a long DEC and forward one-shot: a slice's own transient is the
    // sound. Off to the right, with some delay so the blips answer the rim on the left.
    set(kit.tracks[kLoop], {64, 0, 127, 60}, {0, 110, 0, 100});
    kit.tracks[kLoop].params[kPan] = 84;
    kit.tracks[kLoop].params[kSna] = 48;
    kit.tracks[kLoop].params[kSnb] = 32;

    // The two synth tracks have one filter (D8): their slots 4-7 are FDE and TON, set to the
    // filter decay tied to DEC and TON flat, which is exactly the sound they had.
    for (int t = 0; t < kTracks; t++) {
        if (machine_has_voice_filter(kit.tracks[t].machine)) {
            uint8_t *p = kit.tracks[t].params;
            p[kSlotFdec] = 0;
            p[kSlotTone] = kToneFlat;
            p[6] = 0;
            p[7] = 0;
        }
    }
}

// --- the sends and the master ---------------------------------------------

void factory_mixer(ProjectSettings &s) {
    // dsp/buses.hpp's SendField order. The delay: dotted 1/8 (the dub division), a little more
    // feedback than the struct default so a throw rings for a bar, a slightly darker tone,
    // ping-pong, full return, and some of it into the reverb. The reverb: 2 s, a little darker,
    // the 180 Hz input high-pass that keeps the kick out of it, full return.
    const uint8_t sends[kSendFieldsV1] = {kDefaultDelayDivision, 60, 56, 127, 127, 28, 58, 72, 61, 127};
    for (int i = 0; i < kSendFieldsV1; i++) s.sends[i] = sends[i];
    // The ambient modes stay classic, with their knobs where a fresh project has them.
    const ProjectSettings defaults;
    for (int i = kSendFieldsV1; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
    // The master as ProjectSettings{} has it: flat DJ filter, gentle glue, the limiter, unity.
}

// --- patterns -------------------------------------------------------------

Step &trig(Pattern &p, int track, int step, uint8_t note, uint8_t velocity) {
    Step &s = p.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = note;
    s.velocity = velocity;
    s.length = 8;    // one step
    return s;
}

// The parts every pattern starts from: 16 steps everywhere, the key, a seed of its own.
void begin(Pattern &p, uint32_t seed, uint8_t swing) {
    pattern_init(p);
    p.root = kRootPc;
    p.scale = uint8_t(Scale::minor);
    p.seed = seed;
    p.master_length = 16;
    p.swing = swing;
    for (int t = 0; t < kTracks; t++) {
        p.tracks[t].length = 16;
        p.tracks[t].speed = uint8_t(Speed::x1);
    }
}

void four_on_the_floor(Pattern &p, uint8_t velocity) {
    for (int b = 0; b < 4; b++) trig(p, kKick, b * 4, C4, velocity);
    // Techno lives on the downbeat.
    Step &one = p.tracks[kKick].steps[0];
    one.flags |= kStepAccent;
    one.velocity = 127;
}

// Closed hats on every 16th the open hat does not take, with the beat louder than the
// in-betweens; open hats on the offbeat 8ths, each choked by the closed hat after it.
void offbeat_hats(Pattern &p, uint8_t on_beat, uint8_t between, uint8_t open) {
    for (int i = 0; i < 16; i++) {
        if ((i & 3) == 2) {
            trig(p, kOpen, i, C4, open);
        } else {
            trig(p, kClosed, i, C4, (i & 3) == 0 ? on_beat : between);
        }
    }
}

// A01 GROOVE: the beat a fresh project plays. Four on the floor, a backbeat clap, 16th hats
// with offbeat opens, a rolling offbeat bass with one slide, a syncopated dub stab, a rim and
// a few blips. Slight swing, so the 16ths breathe.
void groove(Pattern &p) {
    begin(p, 0x5713A7Au, 54);
    four_on_the_floor(p, 112);
    // The third kick shorter: a lock, the feature that is STRATA's edge.
    pattern_set_lock(p, kKick, 8, uint8_t(kAmp0), 60);

    trig(p, kClap, 4, C4, 100);
    trig(p, kClap, 12, C4, 104);

    offbeat_hats(p, 72, 50, 96);

    trig(p, kPerc, 3, C4, 90);
    trig(p, kPerc, 11, C4, 84);
    trig(p, kPerc, 15, C4, 70).probability = 50;

    trig(p, kBass, 2, C2, 108).flags |= kStepAccent;
    trig(p, kBass, 3, C2, 70);
    trig(p, kBass, 6, C2, 100);
    trig(p, kBass, 10, C2, 100);
    trig(p, kBass, 11, Eb2, 86).flags |= kStepSlide;
    trig(p, kBass, 14, C2, 108).flags |= kStepAccent;

    // Four notes, so VA POLY's allocator is exercised past a triad every time this plays.
    Step &stab1 = trig(p, kChord, 3, C4, 88);
    stab1.chord = chord_pack(ChordType::min7, 0);
    stab1.length = 6;                                   // half a step: a stab
    Step &stab2 = trig(p, kChord, 10, C4, 78);
    stab2.chord = chord_pack(ChordType::min7, 0);
    stab2.length = 6;

    trig(p, kLoop, 1, kSliceG4, 80);
    trig(p, kLoop, 9, kSliceC5, 72);
    trig(p, kLoop, 13, kSliceD4, 76).probability = 50;
}

// A02 GROOVE+: the same beat, developing. Trig conditions (a kick pickup every fourth bar, a
// hat roll into every bar, a clap on FILL), a perc line on 12 steps against the 16 -- polymeter,
// so it lands somewhere new each bar for three bars -- with locked pitches, a second chord, and
// blips on a 7-step loop.
void groove_plus(Pattern &p) {
    groove(p);
    p.seed = 0x6A0B1E5u;

    Step &pickup = trig(p, kKick, 14, C4, 84);
    pickup.condition = condition_pack_a_b(4, 4);

    trig(p, kClap, 15, C4, 76).condition = condition_pack(Condition::FILL);

    // A hat roll into the next bar. Every bar, not on a condition: the ratchet sits on a hit
    // offbeat_hats already placed, so a 2:2 condition here would drop that hit on the other bars.
    Step &roll = p.tracks[kClosed].steps[15];
    roll.ratchet = ratchet_pack(3, RatchetRamp::up);

    // Perc: 12 steps, pitched by locks into a three-note figure.
    for (int i = 0; i < 16; i++) p.tracks[kPerc].steps[i] = Step{};
    p.tracks[kPerc].length = 12;
    trig(p, kPerc, 3, C4, 90);
    trig(p, kPerc, 7, C4, 76);
    trig(p, kPerc, 10, C4, 84);
    pattern_set_lock(p, kPerc, 7, uint8_t(kSrc0), 99);     // TUN is ~0.5 st a step: a fifth up
    pattern_set_lock(p, kPerc, 10, uint8_t(kSrc0), 109);   // and an octave up

    // The bass grows a note and moves to the fifth at the end of the bar.
    trig(p, kBass, 7, C2, 64);
    Step &fifth = p.tracks[kBass].steps[14];
    fifth.note = G2;
    fifth.flags |= kStepSlide;                          // keeps A01's accent

    // Cm7 then Abmaj7: i to VI, the most-used move in minor-key techno. Ab C Eb G is inside
    // C minor, so the fold leaves it alone.
    Step &second = p.tracks[kChord].steps[10];
    second.note = Ab3;
    second.chord = chord_pack(ChordType::maj7, 0);

    for (int i = 0; i < 16; i++) p.tracks[kLoop].steps[i] = Step{};
    p.tracks[kLoop].length = 7;
    trig(p, kLoop, 1, kSliceG4, 80);
    trig(p, kLoop, 4, kSliceBb5, 64);
    trig(p, kLoop, 5, kSliceC5, 72).probability = 70;
}

// A03 BREAK: kick and bass out, the chord in long dub tails. Locks open the chord's decay and
// both sends on its two hits; the perc ticks on 16ths at 35% probability over 12 steps, the
// blips run 7 steps, so the breakdown never repeats exactly.
void breakdown(Pattern &p) {
    begin(p, 0x0B4EA4Du, 54);

    trig(p, kClap, 12, C4, 92);
    pattern_set_lock(p, kClap, 12, uint8_t(kSnb), 96);

    for (int b = 0; b < 4; b++) trig(p, kClosed, b * 4 + 2, C4, 56);
    trig(p, kOpen, 14, C4, 80);

    p.tracks[kPerc].length = 12;
    for (int i = 0; i < 12; i++) {
        Step &s = trig(p, kPerc, i, C5, i == 0 ? 80 : 54);
        s.probability = i == 0 ? 100 : 35;
    }

    const uint8_t roots[2] = {C4, Ab3};
    const ChordType types[2] = {ChordType::min7, ChordType::maj7};
    for (int i = 0; i < 2; i++) {
        const int step = i * 8;
        Step &s = trig(p, kChord, step, roots[i], 84);
        s.chord = chord_pack(types[i], 0);
        s.length = 10;                                       // two steps
        pattern_set_lock(p, kChord, step, uint8_t(kAmp0), 96);    // DEC: about 0.7 s
        pattern_set_lock(p, kChord, step, uint8_t(kSna), 110);
        pattern_set_lock(p, kChord, step, uint8_t(kSnb), 104);
    }

    p.tracks[kLoop].length = 7;
    trig(p, kLoop, 0, kSliceG4, 70);
    trig(p, kLoop, 3, kSliceBb5, 58);
    trig(p, kLoop, 5, kSliceEb6, 52).probability = 60;
}

// A04 PEAK: everything up. Straighter swing, hats on every 16th with ratchets, an acid line on
// 16ths with slides and accents, the rave stab (5TH OCT, three voices), and FILL rolls on the
// kick and clap.
void peak(Pattern &p) {
    begin(p, 0x9EA4C0Du, 52);
    four_on_the_floor(p, 118);
    Step &kick_roll = trig(p, kKick, 15, C4, 90);
    kick_roll.ratchet = ratchet_pack(4, RatchetRamp::up);
    kick_roll.condition = condition_pack(Condition::FILL);

    trig(p, kClap, 4, C4, 104);
    trig(p, kClap, 12, C4, 108);
    for (int i = 13; i < 16; i++) {
        Step &s = trig(p, kClap, i, C4, 70);
        s.ratchet = ratchet_pack(2, RatchetRamp::up);
        s.condition = condition_pack(Condition::FILL);
    }

    offbeat_hats(p, 84, 60, 100);
    for (int step : {7, 15}) {
        Step &s = p.tracks[kClosed].steps[step];
        s.ratchet = ratchet_pack(2, RatchetRamp::down);
    }

    trig(p, kPerc, 3, C4, 90);
    trig(p, kPerc, 6, G4, 80);
    trig(p, kPerc, 11, C4, 86);
    trig(p, kPerc, 14, C5, 76);

    // The acid line. Off the kick's steps except the turnaround; octave jumps, slides into the
    // jumps, accents on the high notes. Every note is in C minor.
    struct Note { int step; uint8_t note; uint8_t flags; };
    const Note acid[] = {
        {1, C2, 0},           {2, C3, kStepAccent}, {3, C2, 0},
        {5, Eb2, kStepSlide}, {6, C2, 0},           {7, G2, kStepAccent},
        {9, C2, 0},           {10, C3, kStepSlide}, {11, Bb2, 0},
        {13, C2, 0},          {14, Eb3, kStepAccent}, {15, G2, kStepSlide},
    };
    for (const Note &n : acid) {
        Step &s = trig(p, kBass, n.step, n.note, (n.flags & kStepAccent) ? 112 : 92);
        s.flags |= n.flags;
    }
    // The resonance opens on the accented high note, as a 303 player's hand would.
    pattern_set_lock(p, kBass, 7, uint8_t(kSrc3), 108);
    pattern_set_lock(p, kBass, 14, uint8_t(kSrc3), 112);

    for (int step : {3, 10}) {
        Step &s = trig(p, kChord, step, C4, 92);
        s.chord = chord_pack(ChordType::fifth_octave, 0);
        s.length = 6;
    }

    trig(p, kLoop, 1, kSliceG4, 76);
    trig(p, kLoop, 5, kSliceC5, 76);
    trig(p, kLoop, 9, kSliceF5, 72);
    trig(p, kLoop, 13, kSliceEb6, 68);
}

// A05 PAD: VA POLY's ASR page, ready to hear. Four bars of held chords, each one a bar long,
// fading in, holding and ringing out into the next, over a quiet beat that leaves them room.
//
// The pad is in the CHORD track's LOCKS, not in the kit. The kit's chord is A01-A04's dub stab,
// SUS 0, and a pad kit would make those stabs fade in over 400 ms and vanish. A lock lasts until
// the track's next step that fires (app/engine.cpp, `fire`), and the chord track fires only on
// its four chords, so each chord's locks hold for its whole bar -- the release included, which
// the next chord's identical locks keep. Turning a locked knob in EDIT is heard until the next
// chord; to play with the pad's ASR for good, clear a lock (hold the step, press the knob).
//
// The values, by the curves in dsp/va.cpp:
//   ATK 85  2.5 ms * 2^(10.97 * 85/127) = 0.40 s: a swell, and the filter opens with it
//   SUS 89  0.70 of the peak
//   REL 91  60 ms * 2^(7.06 * 91/127) = 2.0 s: each chord rings on under the next
//   DEC 96  15 ms * 2^(7.32 * 96/127) = 0.69 s: the fall from the swell's peak to SUS
//   DET 80  the six voices spread about 0.16 semitone: beating, not out of tune
//   CUT 58  35 Hz * 2^(8.42 * 58/127) = 500 Hz: darker than the stab, so five held voices are
//           warm rather than buzzy. The kit's ENV opens it as each chord swells and FSU 0
//           lets it settle back while the chord holds
//   LVL 60  (60/127)^2 = 0.22, from the stab's 0.52: five voices held at SUS are far more
//           energy than a 210 ms stab. Balanced by rendering (host/render.cpp, A05 stems): the
//           pad sits about 2 dB under the kick in RMS, as A01's mix has its parts
//   SNB 104 well into the reverb; the kit's SNA 72 keeps the dotted-eighth echo
//
// The chords are i9, VImaj7, IIImaj7 and v7 in C minor, voiced so that each shares notes with
// the next. VA POLY retriggers a voice already sounding a note rather than taking another
// (dsp/vapoly.hpp, rule 1), so common tones carry on and swell again, and only the new notes
// fade in. Every change, the loop back to the first included, needs at most six notes at once,
// the voices one VA POLY has: nothing is stolen, so no sounding note jumps to another pitch.
//   Cm9      C3 Eb3 G3 Bb3 D4
//   Abmaj7   Ab2 C3 Eb3 G3              (C Eb G carry on)
//   Ebmaj7   Eb3 G3 Bb3 D4              (Eb G carry on)
//   Gm7      G3 Bb3 D4 F4               (G Bb D carry on; back into Cm9, which keeps them too)

// A bar: index 16 of core/timing.hpp's note-length table is 16 steps.
constexpr uint8_t kPadLength = 16;

// tic6x-elf-g++ 13.3 (the device build: -O2 -funroll-loops, from Enaga's build_app.py) hits an
// internal compiler error compiling this function: "in priority, at haifa-sched.cc:1597", in the
// C6x scheduler after the unroller. #pragma GCC unroll 1 on either loop does not avoid it; turning
// the unroller off for this function alone does. pad() runs once, when the factory bank is built,
// never in audio(), so the cost is nothing. Clang has no optimize attribute, hence the guard.
#if defined(__GNUC__) && !defined(__clang__)
#define STRATA_NO_UNROLL __attribute__((optimize("no-unroll-loops")))
#else
#define STRATA_NO_UNROLL
#endif

STRATA_NO_UNROLL void pad(Pattern &p) {
    begin(p, 0x9AD5EEDu, 50);
    p.master_length = 64;

    // A soft four on the floor, closed hats on the offbeat 8ths, an open hat at the end of each
    // bar into the reverb, and a rim into the delay now and then: a deep-house bed, not a peak.
    four_on_the_floor(p, 96);
    for (int b = 0; b < 4; b++) trig(p, kClosed, b * 4 + 2, C4, b & 1 ? 52 : 60);
    trig(p, kOpen, 14, C4, 54).probability = 50;
    trig(p, kPerc, 11, C4, 70).probability = 60;

    // The bass follows the chord roots, one bar each, on the 64 steps the chords use. Dry and
    // short, as the kit has it: the pad is the long sound here.
    struct Bar { uint8_t chord_root; ChordType type; uint8_t bass; };
    const Bar bars[4] = {
        {C3, ChordType::min9, C2},
        {Ab2, ChordType::maj7, Ab1},
        {Eb3, ChordType::maj7, Eb2},
        {G3, ChordType::min7, G1},
    };
    p.tracks[kBass].length = 64;
    p.tracks[kChord].length = 64;
    for (int b = 0; b < 4; b++) {
        const int at = b * 16;
        trig(p, kBass, at + 2, bars[b].bass, 96);
        trig(p, kBass, at + 10, bars[b].bass, 84);
        trig(p, kBass, at + 13, bars[b].bass, 64).probability = 50;

        Step &s = trig(p, kChord, at, bars[b].chord_root, 76);
        s.chord = chord_pack(bars[b].type, 0);
        s.length = kPadLength;
        const struct { int param; uint8_t value; } locks[] = {
            {kAtk, 85}, {kSus, 89}, {kRel, 91}, {kAmp3, 60},
            {kAmp0, 96}, {kAmp2, 80}, {kSrc2, 58}, {kSnb, 104},
        };
        static_assert(sizeof locks / sizeof locks[0] <= kLocksPerStep, "one step's locks");
        for (const auto &l : locks) pattern_set_lock(p, kChord, at, uint8_t(l.param), l.value);
    }
}

// --- the performance layer ------------------------------------------------

// Bit t set means track t muted, which is `mute_state_apply`'s reading.
constexpr uint8_t mutes(std::initializer_list<int> tracks) {
    uint8_t bits = 0;
    for (int t : tracks) bits = uint8_t(bits | (1u << t));
    return bits;
}

void factory_mute_states(ProjectSettings &s) {
    const uint8_t states[8] = {
        0,                                                    // 1: everything
        mutes({kKick}),                                       // 2: no kick, the breakdown
        mutes({kBass, kChord, kLoop}),                        // 3: drums only
        mutes({kClosed, kOpen}),                              // 4: no hats
        mutes({kBass}),                                       // 5: no bass
        mutes({kClap, kClosed, kOpen, kPerc, kChord, kLoop}), // 6: kick and bass
        mutes({kKick, kClap, kClosed, kOpen, kBass}),         // 7: the melodic parts alone
        mutes({kPerc, kLoop}),                                // 8: the bare groove
    };
    for (int i = 0; i < 8; i++) s.mute_states[i] = states[i];
}

void factory_scenes(SceneSet &scenes) {
    // Scene 1 (slot A) stores nothing, so the morph starts from the kit as it stands. Scene 2
    // (slot B) is DUB: the chord, rim, blips and clap pushed into the sends with longer tails.
    // Turning VALUE in PERFORM fades the dry groove into a dub version of itself.
    Scene &dub = scenes.scenes[1];
    scene_set(dub, kChord, kSna, 112);
    scene_set(dub, kChord, kSnb, 104);
    scene_set(dub, kChord, kAmp0, 90);
    scene_set(dub, kChord, kSrc2, 56);
    scene_set(dub, kPerc, kSna, 100);
    scene_set(dub, kLoop, kSna, 100);
    scene_set(dub, kLoop, kSnb, 72);
    scene_set(dub, kClap, kSnb, 72);
    scene_set(dub, kOpen, kAmp0, 88);

    // Scene 3, DARK: the filters close -- bass, chord and both hats -- and the kick loses top.
    Scene &dark = scenes.scenes[2];
    scene_set(dark, kBass, kSrc2, 24);
    scene_set(dark, kBass, kAmp1, 50);
    scene_set(dark, kChord, kSrc2, 40);
    scene_set(dark, kClosed, kSrc1, 80);
    scene_set(dark, kOpen, kSrc1, 74);
    scene_set(dark, kKick, kAmp1, 40);

    // Scene 4, TENSION: the bass opens and screams, hats and kick tighten, the chord sweeps.
    Scene &tension = scenes.scenes[3];
    scene_set(tension, kBass, kSrc2, 90);
    scene_set(tension, kBass, kSrc3, 116);
    scene_set(tension, kBass, kAmp1, 100);
    scene_set(tension, kClosed, kAmp0, 44);
    scene_set(tension, kKick, kAmp0, 50);
    scene_set(tension, kChord, kAmp1, 100);
}

// Three macros, on encoders B-D in PERFORM. A macro sweeps from the parameter's LIVE value to
// its `high` and writes nothing at 0 (core/perform.hpp), so these never hold a knob or a scene:
// every parameter they drive answers its EDIT knob and the morph until the macro moves, and the
// first detent up is a nudge from wherever the sound is. `low` records the kit value for the
// screen.
//
// Macro A is left empty on purpose: pad pressure bends macro 1 whichever track is selected
// (docs/SPEC.md), and a factory macro there would make every firm pad hit move one track's
// sound. Assign A to the selected track and the pads bend it.
void target(Macro &macro, int slot, const Kit &kit, int track, int param, uint8_t high) {
    MacroTarget &t = macro.targets[slot];
    t.enabled = 1;
    t.track = uint8_t(track);
    t.param = uint8_t(param);
    t.low = kit.tracks[track].params[param];
    t.high = high;
}

void factory_macros(MacroSet &macros, const Kit &kit) {
    // B: ACID -- the bass filter opens, the resonance and the envelope rise. The 303 move.
    Macro &acid = macros.macros[1];
    target(acid, 0, kit, kBass, kSrc2, 112);
    target(acid, 1, kit, kBass, kSrc3, 118);
    target(acid, 2, kit, kBass, kAmp1, 104);

    // C: DUB -- the chord and the rim into the delay and the reverb.
    Macro &dub = macros.macros[2];
    target(dub, 0, kit, kChord, kSna, 120);
    target(dub, 1, kit, kChord, kSnb, 120);
    target(dub, 2, kit, kPerc, kSna, 110);

    // D: DRIVE -- the kick longer and driven harder, both hats longer.
    Macro &drive = macros.macros[3];
    target(drive, 0, kit, kKick, kAmp0, 100);
    target(drive, 1, kit, kKick, kDrv, 90);
    target(drive, 2, kit, kClosed, kAmp0, 56);
    target(drive, 3, kit, kOpen, kAmp0, 100);
}

void factory_chain(Project &project) {
    // GROOVE x2, GROOVE+ x2, BREAK, PEAK x2, GROOVE+: about two minutes at 130 BPM. Built, and
    // off -- PROJECT, F1 + UP turns it on.
    const ChainEntry chain[] = {{0, 0, 2}, {0, 1, 2}, {0, 2, 1}, {0, 3, 2}, {0, 1, 1}};
    const int n = int(sizeof chain / sizeof chain[0]);
    for (int i = 0; i < n; i++) project.chain[i] = chain[i];
    project.settings.chain_length = uint8_t(n);
    project.settings.chain_enabled = false;
}

}  // namespace

// tic6x-elf-g++ 13.3.0 crashes on this function at -O2 (internal compiler error in
// haifa-sched.cc:1597, priority(), during c6x_reorg's post-reload scheduling), with every helper
// above inlined into it. The crash moves with small edits: -fno-unroll-loops moved it to
// emit_nop_after, and turning off only the post-reload scheduling pass fixed one tree and not an
// older one. -O1 compiled every tree tried. The function runs once, at first start and on NEW, so
// the slower code costs nothing. Remove once the cross GCC is fixed.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O1")))
#endif
void factory_project(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);

    // project_init's kit_clear has already assigned docs/SPEC.md's eight machines.
    factory_kit(project.kit);

    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = 13000;
    s.root = kRootNote;
    s.scale = uint8_t(Scale::minor);
    factory_mixer(s);
    factory_mute_states(s);

    Pattern *bank = project.patterns[0];
    groove(bank[0]);
    groove_plus(bank[1]);
    breakdown(bank[2]);
    peak(bank[3]);
    pad(bank[kFactoryPadSlot]);
    static_assert(kFactoryPatterns == 5 && kFactoryPadSlot == 4, "the five patterns above");

    factory_chain(project);
    factory_scenes(scenes);
    factory_macros(macros, project.kit);

    project.bank = 0;
    project.slot = 0;
}

void blank_project(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    factory_kit(project.kit);
    for (int t = 0; t < kTracks; t++) project.kit.tracks[t].params[kPan] = 64;
    ProjectSettings &s = project.settings;
    s.root = kRootNote;
    s.scale = uint8_t(Scale::minor);
    factory_mixer(s);
    project.bank = 0;
    project.slot = 0;
}

}  // namespace strata
