// Factory sounds. core/sounds.hpp says what a preset is, what it writes and what it leaves.
//
// As in core/factory.cpp, every value is made from the knob mappings in dsp/*.cpp and the
// references in docs/SOUND.md; each comment names what the preset is for and the one or two
// knobs that make it that. Levels are balanced against the factory kit (host/render.cpp), so a
// preset loaded into the default beat lands near the part it replaces.
//
// The pitched drums stay on the factory kit's tuning (KICK TUN 55 is G1, the fifth of C), so a
// preset does not knock a kick out of the key the factory patterns are written in.
#include "sounds.hpp"

namespace strata {

namespace {

// The chain's FLTR page as the factory kit parks it: drive off, the filter OFF but open with a
// little resonance. Most presets use it; the ones that want drive say so.
#define FLAT {0, 127, 20, 0}
#define NO_ASR {0, 0, 0, 0}
#define NO_OSC {0, 0, 0, 0}

// --- KICK: TUN FMA FMD CLK / DEC TON DRT LVL (dsp/kick.cpp) ---------------
constexpr SoundPreset kKickPresets[] = {
    // The factory kick: a 909-ish techno kick, 27 ms pitch drop, 280 ms to -40 dB, a little drive.
    {"909", {55, 72, 56, 72}, {28, 127, 20, 0}, {72, 60, 44, 110}, NO_ASR, NO_OSC},
    // The long one: DEC 118 is a 400 ms time constant, little click and sweep, a clean sine.
    {"808", {55, 30, 40, 20}, FLAT, {118, 40, 10, 94}, NO_ASR, NO_OSC},
    // Deep house: a rounder, slower drop (FMD 70) and a dark top, a little longer than the 909.
    {"DEEP", {55, 55, 70, 40}, {10, 127, 20, 0}, {90, 45, 20, 112}, NO_ASR, NO_OSC},
    // Short and hard: a big fast sweep, full click, a bright top and real drive. Hard techno.
    {"PUNCH", {55, 90, 45, 100}, {40, 127, 20, 0}, {56, 80, 60, 106}, NO_ASR, NO_OSC},
    // Industrial: DRT at the top and the drive near it, a long body that distorts into rumble.
    {"DIST", {55, 80, 60, 90}, {100, 127, 20, 0}, {84, 100, 127, 52}, NO_ASR, NO_OSC},
    // Dub: soft attack, no click, a dull top. A kick that sits under a chord rather than on it.
    {"SOFT", {55, 40, 50, 10}, FLAT, {70, 20, 0, 127}, NO_ASR, NO_OSC},
};

// --- SNARE: TUN SNP TON CLP / DEC SPR BDY LVL (dsp/snare.cpp) ------------
constexpr SoundPreset kSnarePresets[] = {
    // The factory clap: three bursts 10 ms apart, mostly noise.
    {"CLAP", {64, 100, 64, 64}, FLAT, {54, 28, 40, 100}, NO_ASR, NO_OSC},
    // The 909 snare: 200 Hz (TUN 64), more drum than noise, a bright band, no clap.
    {"909SN", {64, 70, 70, 0}, FLAT, {50, 28, 40, 100}, NO_ASR, NO_OSC},
    // The 808 snare: tuned higher, less snap, a darker band, a short body.
    {"808SN", {80, 50, 56, 0}, FLAT, {40, 28, 20, 104}, NO_ASR, NO_OSC},
    // A big clap: four bursts (CLP 100), spread to 40 ms, a longer tail.
    {"CLAP4", {64, 110, 72, 100}, FLAT, {70, 127, 30, 100}, NO_ASR, NO_OSC},
    // A snare rim: tuned up, almost no noise, very short. A tick for a sparse groove.
    {"RIMSN", {110, 20, 90, 0}, FLAT, {10, 10, 0, 96}, NO_ASR, NO_OSC},
    // Dirty: noisy, darker, driven hard in the chain. Breaks and electro.
    {"DIRTY", {58, 90, 50, 0}, {50, 127, 20, 0}, {64, 28, 50, 70}, NO_ASR, NO_OSC},
};

// --- HAT: TUN TON NOI DTN / DEC CHO CHK LVL (dsp/hat.cpp) -----------------
// CHK (slot 10) is never written (sound_preset_writes); the 64 here is only a placeholder.
constexpr SoundPreset kHatPresets[] = {
    // The factory closed hat: 3.8 kHz band, 17 ms time constant.
    {"CLOSED", {64, 104, 40, 64}, FLAT, {24, 64, 64, 84}, NO_ASR, NO_OSC},
    // The factory open hat: a little darker and fuller, 102 ms, choked by the closed one.
    {"OPEN", {64, 96, 48, 64}, FLAT, {70, 50, 64, 70}, NO_ASR, NO_OSC},
    // Tight: brighter (6 kHz) and shorter, a fast choke. Busy 16ths.
    {"TIGHT", {70, 116, 30, 64}, FLAT, {12, 40, 64, 84}, NO_ASR, NO_OSC},
    // The 808's squares with no noise, darker, a little longer: the classic metallic tick.
    {"808HH", {64, 90, 10, 64}, FLAT, {30, 60, 64, 88}, NO_ASR, NO_OSC},
    // A shaker: almost all noise, high, short, squares nearly in unison.
    {"SHAKE", {64, 110, 110, 40}, FLAT, {20, 30, 64, 78}, NO_ASR, NO_OSC},
    // A ride: lower, clangy (DTN 110), long. Use it open, on the offbeat or on every beat.
    {"RIDE", {50, 100, 40, 110}, FLAT, {100, 90, 64, 46}, NO_ASR, NO_OSC},
    // Trash: noisy and metallic, medium length. An industrial open hat.
    {"TRASH", {80, 84, 90, 127}, FLAT, {56, 60, 64, 52}, NO_ASR, NO_OSC},
};

// --- PERC: TUN RAT IDX IDD / DEC SWP SWT LVL (dsp/perc.cpp) ---------------
// RAT 0-63 picks one of sixteen ratios four knob steps wide (kRatioTable); the values below sit
// in the middle of their step.
constexpr SoundPreset kPercPresets[] = {
    // The factory rimshot: high, short, inharmonic (ratio pi).
    {"RIM", {85, 48, 75, 20}, FLAT, {12, 30, 10, 100}, NO_ASR, NO_OSC},
    // A tom: 120 Hz, ratio 2 (tuned), a soft index that dies fast, a long body and a boing.
    {"TOM", {32, 29, 30, 40}, FLAT, {90, 40, 60, 80}, NO_ASR, NO_OSC},
    // A cowbell: about 800 Hz, ratio sqrt(2) (metallic), the index held, no sweep.
    {"COWBEL", {80, 17, 60, 90}, FLAT, {50, 0, 10, 96}, NO_ASR, NO_OSC},
    // A bell: high, ratio 3.5, a bright index that rings (IDD 110) over a long decay.
    {"BELL", {100, 53, 80, 110}, FLAT, {110, 0, 10, 62}, NO_ASR, NO_OSC},
    // A blip: high and pure (IDX low), very short: the bleep in bleep techno.
    {"BLIP", {108, 9, 20, 30}, FLAT, {20, 10, 20, 92}, NO_ASR, NO_OSC},
    // A conga: 160 Hz, ratio 1, a short boing on a medium body.
    {"CONGA", {40, 9, 40, 30}, FLAT, {60, 50, 40, 84}, NO_ASR, NO_OSC},
    // Metal: a ratio swept past 4 (inharmonic clang), a heavy index. Industrial hits.
    {"METAL", {70, 90, 110, 70}, FLAT, {70, 20, 30, 86}, NO_ASR, NO_OSC},
};

// --- VA MONO: TUN MOD CUT RES / DEC ENV GLD LVL, ASR, OSC (dsp/va.cpp) ----
// MOD: 0-63 LP24, 64-95 LP12, 96-111 BP12, 112-127 HP12. CUT 35 Hz * 2^(8.42 * CUT/127).
// RES: about +12 dB of peak at 64, self-oscillating from 124. OSC is WAV OS2 SUB DRV: WAV 0-31
// saw, 32 square, up to a thin pulse at 127; OS2 0 off, 1-63 detune (D14 at 30, D46 at 55),
// 64-127 sync (S+0 to S+24); SUB the triangle an octave down; DRV the drive into the filter.
constexpr SoundPreset kMonoPresets[] = {
    // The factory bass: 24 dB at 290 Hz with some resonance, a short three-octave "wow", and
    // the sub under it for the weight.
    {"ROLL", {64, 40, 46, 56}, FLAT, {46, 72, 18, 127}, NO_ASR, {0, 0, 90, 24}},
    // Acid: the 303 move. Closed (CUT 26, 145 Hz), resonance high, the envelope wide, driven.
    // SUS holds each note for its LEN, so DEC is only the filter's sweep: accent and slide do
    // the rest. A raw saw, no sub.
    {"ACID", {64, 20, 26, 100}, FLAT, {44, 100, 0, 90}, {0, 100, 8, 0}, {0, 0, 0, 48}},
    // The same line on a square: hollower, rubberier.
    {"ACIDSQ", {64, 20, 30, 98}, FLAT, {44, 96, 0, 80}, {0, 100, 8, 0}, {40, 0, 0, 40}},
    // A thick bass: two saws 14 cents apart, the sub at the top, driven into a 24 dB filter.
    // Holds for its LEN.
    {"FAT", {64, 30, 42, 36}, FLAT, {70, 40, 0, 101}, {0, 100, 24, 24}, {0, 30, 110, 72}},
    // A reese: wider detune (46 cents), slower beating, darker, long. Drum and bass, dubstep.
    {"REESE", {64, 30, 50, 30}, FLAT, {80, 24, 0, 92}, {0, 110, 30, 60}, {0, 55, 70, 56}},
    // A sub: 12 dB at 100 Hz, the triangle sub at full and little else. Weight under a busy kit.
    {"SUB", {64, 70, 22, 0}, FLAT, {90, 10, 0, 112}, NO_ASR, {0, 0, 127, 0}},
    // A pluck: a 33% pulse, brighter, short, a fast sweep with some bite. Lines and arps.
    {"PLUCK", {64, 40, 52, 64}, FLAT, {30, 90, 0, 118}, NO_ASR, {64, 0, 0, 20}},
    // A growl: the band-pass, nasal and hollow, driven in the voice and in the chain. Mid bass.
    {"GROWL", {64, 100, 46, 40}, {60, 127, 20, 0}, {84, 60, 10, 124}, NO_ASR, {0, 20, 60, 80}},
    // A glide bass: portamento on every note (GLD 64, about 40 ms), a rounder filter, the sub.
    {"GLIDE", {64, 40, 56, 50}, FLAT, {60, 70, 64, 122}, NO_ASR, {0, 0, 80, 16}},
    // A sync lead: OSC 2 synced an octave and a bit above (S+14), held, a little glide. Play it
    // an octave or two up.
    {"SYNC", {64, 70, 88, 30}, FLAT, {80, 40, 30, 98}, {0, 110, 40, 80}, {0, 100, 0, 40}},
    // A lead: a square with a slight detune, 12 dB, resonance and glide. Melodies.
    {"LEAD", {64, 70, 78, 60}, FLAT, {70, 46, 36, 82}, {0, 115, 40, 90}, {40, 22, 0, 30}},
    // A drone: SUS up, so each note holds for its LEN; a slow attack and a long tail, the
    // filter held half open (FSU), two saws beating slowly. Long notes, one or two a bar.
    {"DRONE", {64, 40, 60, 50}, FLAT, {80, 40, 20, 74}, {40, 110, 80, 64}, {0, 40, 60, 0}},
};

// --- VA POLY: TUN MOD CUT RES / DEC ENV DET LVL, ASR, OSC (dsp/vapoly.cpp) ----
// Presets that hold (SUS above 0) have LVL well down: five voices held are far more energy than a
// 200 ms stab (core/factory.cpp's A05 PAD measured it: 5 dB). A sub on every note of a chord is
// mud, so only the stab and the keys keep a little.
constexpr SoundPreset kPolyPresets[] = {
    // The factory stab: 12 dB at 870 Hz, 210 ms, a little detune. The dub-techno chord.
    {"STAB", {64, 70, 70, 36}, FLAT, {66, 56, 28, 92}, NO_ASR, {0, 0, 50, 0}},
    // A05 PAD's sound: 0.4 s swell, 70% sustain, 2 s release, darker, wider: two saws a voice.
    {"PAD", {64, 70, 58, 30}, FLAT, {96, 56, 80, 60}, {85, 89, 91, 0}, {0, 40, 0, 0}},
    // Strings: brighter and wider than PAD, a slower swell, the filter held open (FSU).
    {"STRING", {64, 70, 80, 16}, FLAT, {100, 30, 100, 65}, {95, 100, 96, 50}, {0, 50, 0, 0}},
    // Keys: a quick attack, half sustain, a short release, on a square: an organ-ish comp.
    {"KEYS", {64, 70, 64, 24}, FLAT, {80, 50, 40, 55}, {0, 64, 60, 30}, {40, 0, 40, 0}},
    // The rave stab: two saws a voice, open, a short decay, resonance, driven. 5TH OCT chords.
    {"RAVE", {64, 64, 90, 56}, FLAT, {50, 70, 60, 88}, NO_ASR, {0, 45, 0, 50}},
    // A poly pluck: a 33% pulse, the 24 dB filter, a fast deep sweep. Arps and chord riffs.
    {"PLUCK", {64, 40, 50, 64}, FLAT, {40, 80, 30, 71}, NO_ASR, {64, 0, 0, 16}},
    // Brass: a swell on two saws, the 24 dB filter opening with it and holding (FSU).
    {"BRASS", {64, 40, 56, 30}, FLAT, {70, 50, 50, 57}, {40, 100, 40, 60}, {0, 20, 0, 30}},
};

// --- WAVE MONO: TBL POS CUT RES / DEC ENV MOT LVL, ASR, OSC WRP DET SUB GLD (dsp/va.cpp) ----
// TBL is eight counts a table (dsp/wavetable.cpp's order): 4 CLS, 12 PWM, 20 SYN, 28 FMT, 36 RSW,
// 44 HRM, 52 FLD, 60 PHD, 68 FM1, 76 TIN, 84 DGT, 92 VOW, 100 GRL, 108 EPN, 116 PLK, 124 MLT.
// POS 0-127 is frames 0-15. MOT 64 is none: above it the filter envelope moves the position up,
// below it down. The filter is the VA's 24 dB one, so CUT, RES, DEC and ENV read as on VA MONO.
// DET 0 is one reader; 40 is 6 cents between two, 80 is 24.
constexpr SoundPreset kWaveMonoPresets[] = {
    // The envelope sweeps the classic table from its sine to its square: a wavetable "wow".
    {"SWEEP", {4, 0, 70, 40}, FLAT, {60, 40, 110, 120}, NO_ASR, {0, 0, 60, 0}},
    // A resonant sweep with no filter in it: the RESO SAW table's peak moves with the envelope.
    {"RESO", {36, 10, 100, 0}, FLAT, {60, 10, 120, 127}, NO_ASR, {0, 0, 80, 0}},
    // A growl bass: the GROWL table half way, held, the envelope pulling it into the snarl.
    {"GROWL", {100, 40, 60, 50}, FLAT, {70, 50, 100, 110}, {0, 100, 20, 0}, {0, 0, 70, 0}},
    // A talking lead: FORMANT, two readers a little apart, a touch of glide.
    {"FORMNT", {28, 64, 80, 20}, FLAT, {60, 30, 90, 110}, {0, 100, 20, 40}, {0, 30, 40, 30}},
    // Early digital: the stepping DIGITAL spectra, short and bright. Bleeps and arps.
    {"DIGI", {84, 30, 110, 30}, FLAT, {56, 40, 100, 127}, NO_ASR, {0, 0, 0, 0}},
    // A vowel lead: VOWELS opening from "oo" as the envelope rises, glide on every note.
    {"VOWEL", {92, 20, 90, 20}, FLAT, {80, 20, 120, 100}, {0, 110, 30, 60}, {0, 40, 0, 40}},
    // A bent bass: PHASE DIST with WRP bending the cycle as well. CZ-style.
    {"WARP", {60, 40, 70, 50}, FLAT, {60, 60, 90, 127}, NO_ASR, {60, 0, 80, 0}},
    // An FM bass: the FM RATIO table, the sub under it.
    {"FMBASS", {68, 50, 70, 30}, FLAT, {60, 50, 40, 127}, NO_ASR, {0, 0, 100, 0}},
};

// --- WAVE POLY: TBL POS CUT RES / DEC ENV MOT LVL, ASR, OSC WRP DET SUB DRV (dsp/vapoly.cpp) --
// Held presets keep LVL down, as on VA POLY. Their DET is each voice's two readers.
constexpr SoundPreset kWavePolyPresets[] = {
    // A pad on HARMONICS: a slow swell, two readers beating, the envelope brightening it.
    {"PAD", {44, 30, 80, 20}, FLAT, {96, 30, 90, 70}, {85, 89, 91, 40}, {0, 60, 0, 0}},
    // An electric piano: E-PIANO barking as it is struck and mellowing as it holds.
    {"EPIANO", {108, 20, 100, 10}, FLAT, {90, 20, 90, 70}, {0, 70, 60, 64}, {0, 20, 0, 0}},
    // A bell: FM TINE from its sine, the tine rising on the strike and ringing away.
    {"BELL", {76, 0, 110, 10}, FLAT, {110, 20, 110, 62}, NO_ASR, {0, 30, 0, 0}},
    // Mallets: the MALLET table at marimba, struck brighter. Chords and patterns.
    {"MALLET", {124, 30, 110, 10}, FLAT, {80, 20, 100, 70}, NO_ASR, {0, 0, 0, 0}},
    // A pluck: PLUCK from its bright end to its dull one as the envelope falls.
    {"PLUCK", {116, 127, 100, 20}, FLAT, {70, 20, 0, 100}, NO_ASR, {0, 0, 0, 0}},
    // A choir: VOWELS held, two readers, a slow swell.
    {"CHOIR", {92, 50, 80, 10}, FLAT, {100, 20, 80, 64}, {90, 100, 96, 50}, {0, 50, 0, 0}},
    // A PWM pad: the PWM table's widths, two readers, held.
    {"PWMPAD", {12, 40, 80, 20}, FLAT, {96, 30, 100, 66}, {80, 95, 90, 50}, {0, 40, 0, 0}},
};

// --- FM4: ALG RAT IDX FBK / DEC IEN VEL LVL, ASR ATK SUS REL ISU, OSC DET FIN BRT CAR ----------
// (dsp/fm.cpp). ALG is sixteen counts an algorithm: 8 is A1, 24 A2 ... 72 A5 (two pairs), 88 A6,
// 120 A8 (the organ). RAT the same over 0.5 1 2 3 3.5 4 7 14: 24 is 1, 72 is 3.5, 104 is 7.
// IDX is square law. FIN 64 is the ratio as RAT says it; CAR 13 is a carrier at 1, 38 at 0.5.
// The insert filter stays open: FM's brightness is its index. Held presets keep LVL down.
constexpr SoundPreset kFmPresets[] = {
    // An electric piano: two pairs at 1:1, the strike brighter than the ring, a hair of detune.
    {"EPIANO", {72, 24, 50, 0}, FLAT, {100, 100, 90, 80}, NO_ASR, {30, 64, 60, 13}},
    // A bell: 1:3.5, inharmonic, ringing for seconds while the index falls away.
    {"BELL", {72, 72, 60, 0}, FLAT, {115, 110, 60, 72}, NO_ASR, {40, 64, 30, 13}},
    // A bass: the full stack with feedback, short and growling, the index snapping shut.
    {"BASS", {8, 24, 70, 40}, FLAT, {60, 110, 70, 110}, NO_ASR, {0, 64, 0, 13}},
    // Brass: a slow index rise to a held, half-open tone. Chords.
    {"BRASS", {40, 24, 55, 30}, FLAT, {80, 90, 60, 72}, {40, 100, 30, 80}, {20, 64, 40, 13}},
    // An organ: four sines at 1 and 2, detuned into a chorus, held.
    {"ORGAN", {120, 40, 0, 0}, FLAT, {64, 0, 0, 72}, {0, 127, 20, 0}, {30, 64, 0, 13}},
    // Mallets: 1:4, the index all in the strike.
    {"MALLET", {72, 88, 40, 0}, FLAT, {75, 127, 80, 100}, NO_ASR, {0, 64, 40, 13}},
    // A pad: one modulator under three carriers, a slow swell, detuned, held.
    {"PAD", {88, 24, 30, 20}, FLAT, {90, 60, 30, 72}, {90, 110, 90, 60}, {60, 64, 30, 13}},
    // Glass: 1:7, bright and brittle, the feedback adding a breath.
    {"GLASS", {56, 104, 45, 20}, FLAT, {85, 110, 70, 92}, NO_ASR, {20, 64, 50, 13}},
};

// --- GRAIN: POS SIZ DEN SPR / PIT SHP DEC LVL / ATK SUS REL --- / SCN PSP (dsp/grain.cpp) ----
// POS and PIT are never written (sound_preset_writes): where in YOUR sample the cloud reads, and
// the key it is tuned to, belong to the sample, not to the sound. The 0 and 64 here are only
// placeholders. Levels: a sustained cloud of a normalised sample near the VA POLY pads.
constexpr SoundPreset kGrainPresets[] = {
    // The all-rounder: 150 ms grains, four in the air, 70 ms of spray, a hair of detune, held.
    // Where to start with any sample.
    {"CLOUD", {0, 84, 104, 48}, FLAT, {64, 0, 127, 118}, {20, 0, 40, 0}, {0, 24, 0, 0}},
    // A pad: long grains, little spray, a slow swell that holds while the key or LEN does and
    // fades over three seconds.
    {"PAD", {0, 100, 104, 26}, FLAT, {64, 0, 127, 124}, {70, 127, 85, 0}, {0, 20, 0, 0}},
    // A freeze: the moment at POS held for ever -- long grains, dense, almost no spray.
    {"FREEZE", {0, 108, 120, 6}, FLAT, {64, 0, 127, 108}, {40, 0, 90, 0}, {0, 10, 0, 0}},
    // A shimmer: the pad with a third of its grains an octave up, a longer release.
    {"SHIMMR", {0, 100, 112, 30}, FLAT, {64, 10, 127, 124}, {80, 120, 100, 0}, {0, 104, 0, 0}},
    // A drift: a slow scan through the sample (7% of real time) under a wide spray and detune.
    // It never repeats; lock POS per bar to give it a shape.
    {"DRIFT", {0, 96, 110, 60}, FLAT, {64, 0, 127, 112}, {60, 0, 80, 0}, {24, 30, 0, 0}},
    // A stretch: the sample at half speed and its own pitch. A loop at half tempo, or a voice
    // drawn out.
    {"STRTCH", {0, 90, 96, 3}, FLAT, {64, 0, 127, 112}, {0, 0, 0, 0}, {62, 0, 0, 0}},
    // A stutter: plucked 90 ms grains with gaps between them, half a second long. Put it on
    // every step and lock POS.
    {"STUTTR", {0, 70, 20, 0}, FLAT, {64, 90, 74, 116}, NO_ASR, NO_OSC},
    // Chops: short percussive 60 ms grains fading over a third of a second, for rhythmic slicing
    // with POS locks.
    {"CHOPS", {0, 56, 40, 2}, FLAT, {64, 127, 66, 116}, NO_ASR, NO_OSC},
};

#undef FLAT
#undef NO_ASR
#undef NO_OSC

struct Bank {
    const SoundPreset *presets;
    int count;
};

template <int N>
constexpr Bank bank(const SoundPreset (&presets)[N]) {
    return {presets, N};
}

Bank bank_for(uint8_t machine) {
    switch (MachineId(machine)) {
        case MachineId::kick: return bank(kKickPresets);
        case MachineId::snare: return bank(kSnarePresets);
        case MachineId::hat: return bank(kHatPresets);
        case MachineId::perc: return bank(kPercPresets);
        case MachineId::va_mono: return bank(kMonoPresets);
        case MachineId::va_poly: return bank(kPolyPresets);
        case MachineId::wave_mono: return bank(kWaveMonoPresets);
        case MachineId::wave_poly: return bank(kWavePolyPresets);
        case MachineId::fm: return bank(kFmPresets);
        case MachineId::grain: return bank(kGrainPresets);
        default: return {nullptr, 0};
    }
}

constexpr uint32_t kSrcFltrAmp = 0x0FFFu;                       // params 0-11
constexpr uint32_t kAsrBits = 0xFu << kSoundAsrFirst;           // params 24-27
constexpr uint32_t kOscBits = 0xFu << kSoundOscFirst;           // params 28-31
constexpr int kHatChoke = 10;
constexpr int kGrainPos = 0;
constexpr int kGrainPit = 8;

static_assert(kSoundOscFirst + 4 == kKitParams, "the OSC page is the last four kit bytes");

}  // namespace

int sound_preset_count(uint8_t machine) { return bank_for(machine).count; }

const SoundPreset *sound_preset(uint8_t machine, int index) {
    const Bank b = bank_for(machine);
    return index >= 0 && index < b.count ? &b.presets[index] : nullptr;
}

uint32_t sound_preset_writes(uint8_t machine) {
    switch (MachineId(machine)) {
        case MachineId::kick:
        case MachineId::snare:
        case MachineId::perc: return kSrcFltrAmp;
        case MachineId::hat: return kSrcFltrAmp & ~(1u << kHatChoke);
        case MachineId::va_mono:
        case MachineId::va_poly:
        case MachineId::wave_mono:
        case MachineId::wave_poly:
        case MachineId::fm: return kSrcFltrAmp | kAsrBits | kOscBits;
        case MachineId::grain:
            return (kSrcFltrAmp | kAsrBits | kOscBits) & ~(1u << kGrainPos) & ~(1u << kGrainPit);
        default: return 0;
    }
}

namespace {

// The preset's value for KitTrack parameter p (only meaningful where the mask is set).
uint8_t value_of(const SoundPreset &s, int p) {
    if (p < 4) return s.src[p];
    if (p < 8) return s.fltr[p - 4];
    if (p < 12) return s.amp[p - 8];
    if (p >= kSoundAsrFirst && p < kSoundAsrFirst + 4) return s.asr[p - kSoundAsrFirst];
    if (p >= kSoundOscFirst && p < kSoundOscFirst + 4) return s.osc[p - kSoundOscFirst];
    return 0;
}

}  // namespace

bool sound_preset_apply(uint8_t machine, int index, uint8_t *params) {
    const SoundPreset *s = sound_preset(machine, index);
    if (s == nullptr || params == nullptr) return false;
    const uint32_t mask = sound_preset_writes(machine);
    for (int p = 0; p < kKitParams; p++) {
        if ((mask >> p) & 1u) params[p] = value_of(*s, p);
    }
    return true;
}

bool sound_preset_matches(uint8_t machine, int index, const uint8_t *params) {
    const SoundPreset *s = sound_preset(machine, index);
    if (s == nullptr || params == nullptr) return false;
    const uint32_t mask = sound_preset_writes(machine);
    for (int p = 0; p < kKitParams; p++) {
        if (((mask >> p) & 1u) && params[p] != value_of(*s, p)) return false;
    }
    return true;
}

bool machine_defaults_apply(uint8_t machine, uint8_t *params) {
    if (params == nullptr) return false;
    if (machine == uint8_t(MachineId::sample)) {
        // PIT unity, STA 0, LEN all, SLC 1; drive off, filter open, no resonance, mode OFF;
        // no attack, DEC hold, forward one-shot, the factory LOOP track's level. STA, LEN, SLC
        // and DEC agree with the browser's pick (kSamplePickResets; tests/test_sounds.cpp).
        static constexpr uint8_t kSample[12] = {64, 0, 127, 0, 0, 127, 0, 0, 0, 127, 0, 100};
        for (int p = 0; p < 12; p++) params[p] = kSample[p];
        return true;
    }
    const SoundPreset *s = sound_preset(machine, 0);
    if (s == nullptr) return false;
    for (int p = 0; p < 12; p++) params[p] = value_of(*s, p);
    if (machine_reads_asr(machine)) {
        for (int p = 0; p < 4; p++) params[kSoundAsrFirst + p] = s->asr[p];
        for (int p = 0; p < 4; p++) params[kSoundOscFirst + p] = s->osc[p];
    }
    return true;
}

}  // namespace strata
