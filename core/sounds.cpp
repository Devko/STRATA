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

// --- KICK: TUN FMA FMD CLK / DEC TON DRT LVL (dsp/kick.cpp) ---------------
constexpr SoundPreset kKickPresets[] = {
    // The factory kick: a 909-ish techno kick, 27 ms pitch drop, 280 ms to -40 dB, a little drive.
    {"909", {55, 72, 56, 72}, {28, 127, 20, 0}, {72, 60, 44, 110}, NO_ASR},
    // The long one: DEC 118 is a 400 ms time constant, little click and sweep, a clean sine.
    {"808", {55, 30, 40, 20}, FLAT, {118, 40, 10, 94}, NO_ASR},
    // Deep house: a rounder, slower drop (FMD 70) and a dark top, a little longer than the 909.
    {"DEEP", {55, 55, 70, 40}, {10, 127, 20, 0}, {90, 45, 20, 112}, NO_ASR},
    // Short and hard: a big fast sweep, full click, a bright top and real drive. Hard techno.
    {"PUNCH", {55, 90, 45, 100}, {40, 127, 20, 0}, {56, 80, 60, 106}, NO_ASR},
    // Industrial: DRT at the top and the drive near it, a long body that distorts into rumble.
    {"DIST", {55, 80, 60, 90}, {100, 127, 20, 0}, {84, 100, 127, 52}, NO_ASR},
    // Dub: soft attack, no click, a dull top. A kick that sits under a chord rather than on it.
    {"SOFT", {55, 40, 50, 10}, FLAT, {70, 20, 0, 127}, NO_ASR},
};

// --- SNARE: TUN SNP TON CLP / DEC SPR BDY LVL (dsp/snare.cpp) ------------
constexpr SoundPreset kSnarePresets[] = {
    // The factory clap: three bursts 10 ms apart, mostly noise.
    {"CLAP", {64, 100, 64, 64}, FLAT, {54, 28, 40, 100}, NO_ASR},
    // The 909 snare: 200 Hz (TUN 64), more drum than noise, a bright band, no clap.
    {"909SN", {64, 70, 70, 0}, FLAT, {50, 28, 40, 100}, NO_ASR},
    // The 808 snare: tuned higher, less snap, a darker band, a short body.
    {"808SN", {80, 50, 56, 0}, FLAT, {40, 28, 20, 104}, NO_ASR},
    // A big clap: four bursts (CLP 100), spread to 40 ms, a longer tail.
    {"CLAP4", {64, 110, 72, 100}, FLAT, {70, 127, 30, 100}, NO_ASR},
    // A snare rim: tuned up, almost no noise, very short. A tick for a sparse groove.
    {"RIMSN", {110, 20, 90, 0}, FLAT, {10, 10, 0, 96}, NO_ASR},
    // Dirty: noisy, darker, driven hard in the chain. Breaks and electro.
    {"DIRTY", {58, 90, 50, 0}, {50, 127, 20, 0}, {64, 28, 50, 70}, NO_ASR},
};

// --- HAT: TUN TON NOI DTN / DEC CHO CHK LVL (dsp/hat.cpp) -----------------
// CHK (slot 10) is never written (sound_preset_writes); the 64 here is only a placeholder.
constexpr SoundPreset kHatPresets[] = {
    // The factory closed hat: 3.8 kHz band, 17 ms time constant.
    {"CLOSED", {64, 104, 40, 64}, FLAT, {24, 64, 64, 84}, NO_ASR},
    // The factory open hat: a little darker and fuller, 102 ms, choked by the closed one.
    {"OPEN", {64, 96, 48, 64}, FLAT, {70, 50, 64, 70}, NO_ASR},
    // Tight: brighter (6 kHz) and shorter, a fast choke. Busy 16ths.
    {"TIGHT", {70, 116, 30, 64}, FLAT, {12, 40, 64, 84}, NO_ASR},
    // The 808's squares with no noise, darker, a little longer: the classic metallic tick.
    {"808HH", {64, 90, 10, 64}, FLAT, {30, 60, 64, 88}, NO_ASR},
    // A shaker: almost all noise, high, short, squares nearly in unison.
    {"SHAKE", {64, 110, 110, 40}, FLAT, {20, 30, 64, 78}, NO_ASR},
    // A ride: lower, clangy (DTN 110), long. Use it open, on the offbeat or on every beat.
    {"RIDE", {50, 100, 40, 110}, FLAT, {100, 90, 64, 46}, NO_ASR},
    // Trash: noisy and metallic, medium length. An industrial open hat.
    {"TRASH", {80, 84, 90, 127}, FLAT, {56, 60, 64, 52}, NO_ASR},
};

// --- PERC: TUN RAT IDX IDD / DEC SWP SWT LVL (dsp/perc.cpp) ---------------
// RAT 0-63 picks one of sixteen ratios four knob steps wide (kRatioTable); the values below sit
// in the middle of their step.
constexpr SoundPreset kPercPresets[] = {
    // The factory rimshot: high, short, inharmonic (ratio pi).
    {"RIM", {85, 48, 75, 20}, FLAT, {12, 30, 10, 100}, NO_ASR},
    // A tom: 120 Hz, ratio 2 (tuned), a soft index that dies fast, a long body and a boing.
    {"TOM", {32, 29, 30, 40}, FLAT, {90, 40, 60, 80}, NO_ASR},
    // A cowbell: about 800 Hz, ratio sqrt(2) (metallic), the index held, no sweep.
    {"COWBEL", {80, 17, 60, 90}, FLAT, {50, 0, 10, 96}, NO_ASR},
    // A bell: high, ratio 3.5, a bright index that rings (IDD 110) over a long decay.
    {"BELL", {100, 53, 80, 110}, FLAT, {110, 0, 10, 62}, NO_ASR},
    // A blip: high and pure (IDX low), very short: the bleep in bleep techno.
    {"BLIP", {108, 9, 20, 30}, FLAT, {20, 10, 20, 92}, NO_ASR},
    // A conga: 160 Hz, ratio 1, a short boing on a medium body.
    {"CONGA", {40, 9, 40, 30}, FLAT, {60, 50, 40, 84}, NO_ASR},
    // Metal: a ratio swept past 4 (inharmonic clang), a heavy index. Industrial hits.
    {"METAL", {70, 90, 110, 70}, FLAT, {70, 20, 30, 86}, NO_ASR},
};

// --- VA MONO: TUN MOD CUT RES / DEC ENV GLD LVL, ASR (dsp/va.cpp) ---------
// MOD: 0-63 LP24, 64-95 LP12, 96-111 BP12, 112-127 HP12. CUT 35 Hz * 2^(8.42 * CUT/127).
constexpr SoundPreset kMonoPresets[] = {
    // The factory bass: 24 dB at 290 Hz with real resonance and a short three-octave "wow".
    {"ROLL", {64, 40, 46, 84}, FLAT, {46, 72, 18, 127}, NO_ASR},
    // Acid: the 303 move. Closed (CUT 30, 175 Hz), resonance near the top, the envelope wide.
    {"ACID", {64, 20, 30, 110}, FLAT, {40, 100, 30, 120}, NO_ASR},
    // A sub: 12 dB at 130 Hz, no resonance, almost no sweep, long. Weight under a busy kit.
    {"SUB", {64, 70, 26, 0}, FLAT, {90, 10, 0, 100}, NO_ASR},
    // A pluck: brighter, short, a fast sweep with some bite. Melodic lines and arps.
    {"PLUCK", {64, 40, 52, 60}, FLAT, {30, 90, 0, 120}, NO_ASR},
    // A growl: the band-pass, nasal and hollow, driven a little in the chain. Mid bass.
    {"GROWL", {64, 100, 46, 30}, {60, 127, 20, 0}, {84, 60, 10, 127}, NO_ASR},
    // A glide bass: portamento on every note (GLD 64, about 40 ms), a rounder filter.
    {"GLIDE", {64, 40, 56, 70}, FLAT, {60, 70, 64, 122}, NO_ASR},
    // A drone: SUS up, so each note holds for its LEN; a slow attack and a long tail, the
    // filter held half open (FSU). Long notes, one or two a bar.
    {"DRONE", {64, 40, 60, 60}, FLAT, {80, 40, 20, 74}, {40, 110, 80, 64}},
};

// --- VA POLY: TUN MOD CUT RES / DEC ENV DET LVL, ASR (dsp/vapoly.cpp) -----
// Presets that hold (SUS above 0) have LVL well down: five voices held are far more energy than a
// 200 ms stab (core/factory.cpp's A05 PAD measured it: 5 dB).
constexpr SoundPreset kPolyPresets[] = {
    // The factory stab: 12 dB at 870 Hz, 210 ms, a little detune. The dub-techno chord.
    {"STAB", {64, 70, 70, 48}, FLAT, {66, 56, 28, 92}, NO_ASR},
    // A05 PAD's sound: 0.4 s swell, 70% sustain, 2 s release, darker, wider.
    {"PAD", {64, 70, 58, 48}, FLAT, {96, 56, 80, 60}, {85, 89, 91, 0}},
    // Strings: brighter and wider than PAD, a slower swell, the filter held open (FSU).
    {"STRING", {64, 70, 80, 20}, FLAT, {100, 30, 100, 56}, {95, 100, 96, 50}},
    // Keys: a quick attack, half sustain, a short release: an organ-ish comp that follows LEN.
    {"KEYS", {64, 70, 64, 30}, FLAT, {80, 50, 40, 72}, {0, 64, 60, 30}},
    // The rave stab: open and bright, a short decay, more resonance. Best with 5TH OCT chords.
    {"RAVE", {64, 64, 90, 60}, FLAT, {50, 70, 60, 92}, NO_ASR},
    // A poly pluck: the 24 dB filter, a fast deep sweep. Arps and chord riffs.
    {"PLUCK", {64, 40, 50, 60}, FLAT, {40, 80, 30, 92}, NO_ASR},
};

#undef FLAT
#undef NO_ASR

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
        default: return {nullptr, 0};
    }
}

constexpr uint32_t kSrcFltrAmp = 0x0FFFu;                       // params 0-11
constexpr uint32_t kAsrBits = 0xFu << kSoundAsrFirst;           // params 24-27
constexpr int kHatChoke = 10;

static_assert(kSoundAsrFirst + 4 == kKitParams, "the ASR page is the last four kit bytes");

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
        case MachineId::va_poly: return kSrcFltrAmp | kAsrBits;
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

}  // namespace strata
