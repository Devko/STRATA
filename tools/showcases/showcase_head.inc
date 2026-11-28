// Shared head of the STRATA showcase generators (DEEP, DUB, DRIFT, ELECTRO): includes, the
// parameter slot names and the small builders. Each genre's .cpp includes this, defines its
// project, then includes showcase_tail.inc for the files, the renders and main().
//
// Not part of the repo's build: compiled against the host build's objects (build.sh).
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "chord.hpp"
#include "engine.hpp"
#include "factory.hpp"
#include "fakehost.hpp"
#include "lfo.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "scale.hpp"
#include "sounds.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>

#include <enaga/abi.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

// --- parameter slots (dsp/track.hpp's layout, as core/factory.cpp names them) ---
constexpr int kSrc0 = 0, kSrc1 = 1, kSrc2 = 2, kSrc3 = 3;
constexpr int kFlt0 = 4, kFlt1 = 5, kFlt2 = 6, kFlt3 = 7;   // VA: FDE TON; others: DRV CUT RES MOD
constexpr int kAmp0 = 8, kAmp1 = 9, kAmp2 = 10, kAmp3 = 11;
constexpr int kLof = 12, kPan = 13, kSna = 14, kSnb = 15;
constexpr int kLfoWav = 16, kLfoRat = 17, kLfoDep = 18, kLfoDst = 19;
constexpr int kAtk = 24, kSus = 25, kRel = 26;
constexpr int kOscWav = 28, kOscOs2 = 29, kOscSub = 30, kOscDrv = 31;

constexpr uint8_t D = 60;    // drums' neutral note

// Lengths: core/timing.hpp's note-length table (8 = one step).
constexpr uint8_t L1 = 8, L1h = 9, L2 = 10, L3 = 11, L4 = 12, L6 = 13, L8 = 14, L12 = 15, L16 = 16,
                  L24 = 17, L32 = 18;

// Synced LFO rates (dsp/lfo.cpp, longest first).
constexpr uint8_t sync_rate(int index) { return uint8_t(kLfoSyncFirst + 4 * index); }
constexpr int k8B = 0, k4B = 1, k2B = 2, k1B = 3, kHalf = 5, kQuarter = 8;

// Send fields (dsp/buses.hpp SendField order).
enum SendByte : int {
    sDelayDiv = 0, sDelayFb, sDelayTone, sDelayPing, sDelayLevel, sDelayToRev,
    sRevDecay, sRevDamp, sRevHp, sRevLevel,
    sDelayMode, sDelayMult, sDelayWow, sDelayDiff, sDelayLowCut,
    sRevMode, sRevSize, sRevMod, sRevShimmer,
};

void apply(KitTrack &k, uint8_t machine, int preset) {
    k.machine = machine;
    sound_preset_apply(machine, preset, k.params);
}

int preset_index(uint8_t machine, const char *name) {
    for (int i = 0; i < sound_preset_count(machine); i++)
        if (std::strcmp(sound_preset(machine, i)->name, name) == 0) return i;
    std::fprintf(stderr, "no preset %s\n", name);
    std::exit(1);
}

void preset(KitTrack &k, MachineId machine, const char *name) {
    apply(k, uint8_t(machine), preset_index(uint8_t(machine), name));
}

void clear_kit(Kit &kit) {
    for (int t = 0; t < kTracks; t++) {
        KitTrack &k = kit.tracks[t];
        for (int i = 0; i < kKitParams; i++) k.params[i] = 0;
        k.params[kPan] = 64;
        k.sample[0] = '\0';
        k.slice_mode = kSliceEqual;
        k.level = 1.0f;
    }
}

void lfo(KitTrack &k, LfoWave wave, uint8_t rate, uint8_t depth, int dest) {
    k.params[kLfoWav] = lfo_wave_byte(wave);
    k.params[kLfoRat] = rate;
    k.params[kLfoDep] = depth;
    k.params[kLfoDst] = lfo_dest_byte(dest);
}

Step &trig(Pattern &p, int track, int step, uint8_t note, uint8_t velocity, uint8_t length = L1) {
    Step &s = p.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = note;
    s.velocity = velocity;
    s.length = length;
    return s;
}

Step &chord(Pattern &p, int track, int step, uint8_t root, ChordType type, uint8_t velocity,
            uint8_t length, uint8_t inversion = 0) {
    Step &s = trig(p, track, step, root, velocity, length);
    s.chord = chord_pack(type, inversion);
    return s;
}

void begin(Pattern &p, uint32_t seed, uint8_t root, Scale scale, uint8_t swing, int length = 64) {
    pattern_init(p);
    p.root = root;
    p.scale = uint8_t(scale);
    p.seed = seed;
    p.master_length = uint8_t(length);
    p.swing = swing;
    for (int t = 0; t < kTracks; t++) {
        p.tracks[t].length = uint8_t(length);
        p.tracks[t].speed = uint8_t(Speed::x1);
    }
}

constexpr uint8_t mutes(std::initializer_list<int> tracks) {
    uint8_t bits = 0;
    for (int t : tracks) bits = uint8_t(bits | (1u << t));
    return bits;
}

void target(Macro &m, int slot, const Kit &kit, int track, int param, uint8_t high) {
    MacroTarget &t = m.targets[slot];
    t.enabled = 1;
    t.track = uint8_t(track);
    t.param = uint8_t(param);
    t.low = kit.tracks[track].params[param];
    t.high = high;
}

void set_chain(Project &project, std::initializer_list<ChainEntry> entries) {
    for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
    int n = 0;
    for (const ChainEntry &e : entries) project.chain[n++] = e;
    project.settings.chain_length = uint8_t(n);
    project.settings.chain_enabled = true;    // opens ready to play the arrangement
    project.settings.chain_song = false;
}

// A performance event at a bar of the arrangement render.
struct Event { int bar; Cmd cmd; uint8_t track, a, b; uint16_t morph; };

}  // namespace
