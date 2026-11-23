// Golden renders: every machine, the insert chain, the mixer and beat repeat, each pinned to the
// output it produces today for a fixed patch.
//
// The other DSP tests measure properties -- a hit sounds, decays, a knob moves what it says --
// and a mutation run (docs/MUTATION.md) showed what that leaves open: a coefficient off by one,
// an envelope stage deleted or a filter term with its sign flipped changes the SOUND while every
// property still holds. These catch that. They do not say the sound is right; docs/SOUND.md and
// docs/UNIT_CHECKLIST.md are for that. They say it has not changed by accident.
//
// **A deliberate sound change regenerates them**, after listening:
//
//     make -C host build && STRATA_GOLDEN=print build/host/strata-tests > /tmp/g.txt
//     python3 tools/golden_update.py /tmp/g.txt      # rewrites tests/golden_data.inc
//
// and the diff of tests/golden_data.inc goes in the same commit as the DSP change, so a reviewer
// sees which renders moved.
//
// Each render is summarised as 24 samples at fixed positions plus the mean square, compared with
// a tolerance of 1e-7 absolute plus 1e-4 relative: loose enough for a compiler's choice of
// instruction order, far tighter than any change a person could hear.
#include "check.hpp"

#include <enaga/abi.h>

#include "buses.hpp"
#include "chain.hpp"
#include "grain.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "perc.hpp"
#include "repeat.hpp"
#include "sample.hpp"
#include "snare.hpp"
#include "va.hpp"
#include "vapoly.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kPoints = 24;
constexpr int kSummary = kPoints + 1;
constexpr int kMaxFrames = ENAGA_SAMPLE_RATE;          // one second, mono or interleaved
constexpr int kFltr = 4;                               // the chain's pages (core/factory.hpp)
constexpr int kFx = 12;

struct Golden {
    const char *name;
    float values[kSummary];
};

#include "golden_data.inc"

constexpr int kGoldenCount = int(sizeof(kGolden) / sizeof(kGolden[0]));

float g_out[2 * kMaxFrames];
int16_t g_sample[48000];

bool printing() {
    const char *mode = std::getenv("STRATA_GOLDEN");
    return mode != nullptr && std::strcmp(mode, "print") == 0;
}

void summarise(const float *x, int n, float *summary) {
    for (int k = 0; k < kPoints; k++) summary[k] = x[(int64_t(k) * n) / kPoints + 7 % n];
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += double(x[i]) * double(x[i]);
    summary[kPoints] = float(sum / double(n));
}

// Compares a render with its stored summary, or prints the summary in print mode.
void check_golden(const char *name, const float *x, int n) {
    float got[kSummary];
    summarise(x, n, got);
    if (printing()) {
        std::printf("GOLDEN {\"%s\", {", name);
        for (int k = 0; k < kSummary; k++) std::printf("%s%.9g", k ? ", " : "", double(got[k]));
        std::printf("}},\n");
        return;
    }
    const Golden *want = nullptr;
    for (int i = 0; i < kGoldenCount; i++) {
        if (std::strcmp(kGolden[i].name, name) == 0) want = &kGolden[i];
    }
    CHECK(want != nullptr);
    if (want == nullptr) return;
    int wrong = 0;
    for (int k = 0; k < kSummary; k++) {
        const float w = want->values[k];
        const float tolerance = 1e-7f + 1e-4f * (w < 0.0f ? -w : w);
        const float d = got[k] - w;
        if (!(d <= tolerance && d >= -tolerance)) {
            if (wrong == 0) {
                std::printf("    golden %s: value %d is %.9g, stored %.9g\n", name, k,
                            double(got[k]), double(w));
            }
            wrong++;
        }
    }
    CHECK_EQ(wrong, 0);
}

TrackParams patch(int which) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) {
        p.set(i, which == 0 ? uint8_t(64) : uint8_t((i * 37 + 11 + which * 23) % 128));
    }
    return p;
}

Trig trig(uint8_t note, uint8_t velocity, uint32_t frame, bool accent = false,
          uint32_t length = 0) {
    Trig t;
    t.note = note;
    t.velocity = velocity;
    t.frame = frame;
    t.accent = accent;
    t.gate_frames = length;
    return t;
}

// Renders `frames` frames of `m` with `params`, prepared every block as audio() does, firing
// each trig in the block its frame falls in (frame made block-relative).
int render(Machine &m, const TrackParams &params, const Trig *trigs, int count, int frames) {
    for (int i = 0; i < frames; i++) g_out[i] = 0.0f;
    for (int at = 0; at < frames; at += kBlk) {
        m.prepare(params);
        for (int k = 0; k < count; k++) {
            if (int(trigs[k].frame) >= at && int(trigs[k].frame) < at + kBlk) {
                Trig t = trigs[k];
                t.frame = uint32_t(int(t.frame) - at);
                m.trigger(t);
            }
        }
        m.render_add(g_out + at, kBlk);
    }
    return frames;
}

// A source with something in every band: a decaying saw plus a sine, int16, mono.
void make_sample() {
    uint32_t phase = 0;
    for (int i = 0; i < 48000; i++) {
        phase += 0x01000000u + uint32_t(i) * 37u;
        const float saw = float(int32_t(phase)) * (1.0f / 2147483648.0f);
        const float env = 1.0f - float(i) / 48000.0f;
        const float sine = float(((i * 17) % 200) - 100) / 100.0f;
        g_sample[i] = int16_t((0.6f * saw * env + 0.3f * sine) * 32000.0f);
    }
}

template <typename M>
void golden_machine(const char *name, int which, const Trig *trigs, int count,
                    int frames = 24000) {
    static M machine;     // some machines are large; never on the stack
    machine.init(kRate);
    const TrackParams p = patch(which);
    render(machine, p, trigs, count, frames);
    check_golden(name, g_out, frames);
}

}  // namespace

TEST(golden_drum_machines) {
    const Trig hits[] = {trig(60, 110, 0), trig(60, 70, 9000, true), trig(48, 127, 15000)};
    for (int which = 0; which < 2; which++) {
        char name[32];
        std::snprintf(name, sizeof name, "kick/%d", which);
        golden_machine<Kick>(name, which, hits, 3);
        std::snprintf(name, sizeof name, "snare/%d", which);
        golden_machine<Snare>(name, which, hits, 3);
        std::snprintf(name, sizeof name, "hat/%d", which);
        golden_machine<Hat>(name, which, hits, 3);
        std::snprintf(name, sizeof name, "perc/%d", which);
        golden_machine<Perc>(name, which, hits, 3);
    }
}

TEST(golden_va_machines) {
    // A gated note, an accent, and a slide onto a held note for the mono; a chord with one note
    // released early for the poly.
    const Trig mono[] = {trig(48, 100, 0, false, 6000), trig(55, 120, 8000, true, 0),
                         trig(60, 90, 16000, false, 4000)};
    Trig slid[3] = {mono[0], mono[1], mono[2]};
    slid[2].slide = true;
    const Trig chord[] = {trig(48, 100, 0, false, 9000), trig(55, 100, 0, false, 18000),
                          trig(63, 90, 0, false, 18000), trig(70, 127, 12000, true, 6000)};
    for (int which = 0; which < 2; which++) {
        char name[32];
        std::snprintf(name, sizeof name, "vamono/%d", which);
        golden_machine<VaMono>(name, which, mono, 3);
        std::snprintf(name, sizeof name, "vamono-slide/%d", which);
        golden_machine<VaMono>(name, which, slid, 3);
        std::snprintf(name, sizeof name, "vapoly/%d", which);
        golden_machine<VaPoly>(name, which, chord, 4);
    }
}

TEST(golden_sample_machines) {
    make_sample();
    const Trig hits[] = {trig(60, 100, 0), trig(67, 120, 7000, true), trig(53, 80, 14000, false, 3000)};
    for (int which = 0; which < 2; which++) {
        char name[32];
        static Sampler sampler;
        sampler.init(kRate);
        sampler.set_sample(g_sample, 48000, 1, 44100.0f);
        render(sampler, patch(which), hits, 3, 24000);
        std::snprintf(name, sizeof name, "sample/%d", which);
        check_golden(name, g_out, 24000);

        static Granular grain;
        grain.init(kRate);
        grain.set_sample(g_sample, 48000, 1, 44100.0f);
        render(grain, patch(which), hits, 3, 24000);
        std::snprintf(name, sizeof name, "grain/%d", which);
        check_golden(name, g_out, 24000);
    }
}

TEST(golden_insert_chain) {
    // Every filter mode, with drive, lo-fi and both sends, on a sine-plus-click source.
    for (int mode = 0; mode < 7; mode++) {
        TrackParams p = patch(1);
        p.set(kFltr + 0, 90);                          // drive
        p.set(kFltr + 1, uint8_t(20 + mode * 15));     // cutoff, through the modes
        p.set(kFltr + 2, 70);                          // resonance
        p.set(kFltr + 3, uint8_t(mode * 21));          // filter mode
        InsertChain chain;
        chain.init(kRate);
        ChainMix mix;
        mix.level = 0.8f;
        mix.duck = 0.5f;
        static float l[12800], r[12800], a[12800], b[12800], duck[kBlk];
        for (int i = 0; i < kBlk; i++) duck[i] = 0.3f;
        const int frames = 12800;
        for (int at = 0; at < frames; at += kBlk) {
            float mono[kBlk];
            for (int i = 0; i < kBlk; i++) {
                const int n = at + i;
                mono[i] = 0.5f * float((n * 7) % 100 - 50) / 50.0f + (n % 3000 == 0 ? 0.9f : 0.0f);
            }
            ChainOut out;
            out.dry_l = l + at;
            out.dry_r = r + at;
            out.send_a = a + at;
            out.send_b = b + at;
            out.duck = (at / kBlk) % 2 ? duck : nullptr;
            for (int i = 0; i < kBlk; i++) l[at + i] = r[at + i] = a[at + i] = b[at + i] = 0.0f;
            chain.prepare(p, mix);
            chain.process(mono, kBlk, out);
        }
        char name[32];
        std::snprintf(name, sizeof name, "chain-l/%d", mode);
        check_golden(name, l, frames);
        std::snprintf(name, sizeof name, "chain-r/%d", mode);
        check_golden(name, r, frames);
        std::snprintf(name, sizeof name, "chain-a/%d", mode);
        check_golden(name, a, frames);
        std::snprintf(name, sizeof name, "chain-b/%d", mode);
        check_golden(name, b, frames);
    }
}

TEST(golden_mixer) {
    // Two tracks into the buses with both sends, the delay feeding the reverb, the DJ filter
    // both ways, the compressor and the limiter working, and the duck firing.
    static Mixer mixer;
    mixer.init(kRate);
    for (int variant = 0; variant < 3; variant++) {
        SendSettings sends;
        sends.delay_feedback = 0.6f;
        sends.delay_to_reverb = 0.4f;
        sends.reverb_decay_s = 3.0f;
        MasterSettings master;
        master.dj = variant == 0 ? -0.5f : (variant == 1 ? 0.6f : 0.0f);
        master.compressor = 0.8f;
        master.limit = 0.5f;
        mixer.set_duck(0.002f, 0.02f, 0.15f);
        const int frames = 24000;
        for (int at = 0; at < frames; at += kBlk) {
            if (at % 6000 == 0) mixer.trigger_duck(0);
            mixer.prepare(sends, master);
            ChainMix mix;
            mix.duck = 0.7f;
            for (int t = 0; t < 2; t++) {
                TrackParams p = patch(t + variant);
                p.set(kFx + 2, 100);     // send A
                p.set(kFx + 3, 90);      // send B
                mixer.chain(t).prepare(p, mix);
            }
            mixer.begin_block(kBlk);
            for (int t = 0; t < 2; t++) {
                float *mono = mixer.scratch();
                for (int i = 0; i < kBlk; i++) {
                    const int n = at + i;
                    mono[i] = (n % (4000 + t * 1500) < 300) ? float((n * (5 + t)) % 64 - 32) / 40.0f
                                                             : 0.0f;
                }
                mixer.add_track(t, mono);
            }
            mixer.finish_block(g_out + 2 * at, kBlk);
        }
        char name[32];
        std::snprintf(name, sizeof name, "mixer/%d", variant);
        check_golden(name, g_out, 2 * frames);
    }
}

TEST(golden_beat_repeat) {
    static BeatRepeat repeat;
    CHECK(repeat.init(kRate));
    for (int pitch = 0; pitch < 2; pitch++) {
        repeat.set(3000, pitch != 0);
        const int frames = 24000;
        for (int at = 0; at < frames; at += kBlk) {
            for (int i = 0; i < kBlk; i++) {
                const int n = at + i;
                g_out[2 * n] = float((n * 3) % 200 - 100) / 120.0f;
                g_out[2 * n + 1] = float((n * 5) % 160 - 80) / 100.0f;
            }
            repeat.hold(at >= 6000 && at < 18000);
            repeat.process(g_out + 2 * at, kBlk);
        }
        char name[32];
        std::snprintf(name, sizeof name, "repeat/%d", pitch);
        check_golden(name, g_out, 2 * frames);
    }
}

namespace {

// Like render(), plus the calls a performance makes between trigs: a key up on one note, STOP
// (release_all), a second STOP (kill_all), and a key up after the kill.
int render_with_releases(Machine &m, const TrackParams &params, const Trig *trigs, int count,
                         int frames) {
    for (int i = 0; i < frames; i++) g_out[i] = 0.0f;
    for (int at = 0, block = 0; at < frames; at += kBlk, block++) {
        m.prepare(params);
        if (block == 30) m.release(60);
        if (block == 50) m.release_all();
        if (block == 70) m.kill_all();
        if (block == 90) m.release(67);
        for (int k = 0; k < count; k++) {
            if (int(trigs[k].frame) >= at && int(trigs[k].frame) < at + kBlk) {
                Trig t = trigs[k];
                t.frame = uint32_t(int(t.frame) - at);
                m.trigger(t);
            }
        }
        m.render_add(g_out + at, kBlk);
    }
    return frames;
}

}  // namespace

// Dense: two hits in one block, retriggers while every voice still sounds (so later hits steal
// one and fade or choke it), a chord bigger than any voice pool, accents and mixed gate lengths,
// key ups, STOP and STOP again -- over a short render, so a steal's fade is a large part of what
// is summarised. Four more patches: everything at the top, everything at the bottom, and two
// more scatterings, which between them reach knob zones (clap counts, ratio slots, detune
// halves) the two patches above do not.
TEST(golden_dense_retriggers) {
    make_sample();
    const Trig dense[] = {
        trig(60, 127, 0, false, 400),   trig(64, 60, 40, false, 900),  trig(67, 127, 300, true, 0),
        trig(48, 100, 650, false, 200), trig(72, 30, 700, false, 2000), trig(55, 127, 1400, true, 50),
        trig(60, 90, 1430, false, 0),   trig(43, 127, 2600, false, 700), trig(79, 80, 2610, true, 1),
        trig(36, 127, 2700, false, 0),  trig(40, 120, 2700, false, 0),  trig(45, 110, 2701, false, 0),
        trig(50, 100, 2702, false, 0),  trig(53, 90, 2703, false, 0),   trig(57, 80, 2704, false, 0),
        trig(62, 70, 2705, false, 0),   trig(65, 60, 2706, false, 0),   trig(69, 127, 2707, true, 0),
        trig(74, 127, 2708, false, 0),  trig(77, 127, 2709, false, 0),  trig(81, 127, 2710, false, 0),
        trig(60, 110, 4100, false, 0),  trig(52, 110, 4300, false, 1500), trig(67, 127, 4700, true, 0),
        trig(67, 127, 4701, false, 0),  trig(60, 127, 5000, false, 300)};
    const int count = int(sizeof(dense) / sizeof(dense[0]));
    const int frames = 6400;
    TrackParams patches[4]{};   // zeroed: the ASR page (24-27) at 0 is the envelope these were recorded with
    for (int i = 0; i < kParams; i++) {
        patches[0].set(i, 127);
        patches[1].set(i, 0);
        patches[2].set(i, uint8_t((i * 53 + 5) % 128));
        patches[3].set(i, uint8_t((i * 29 + 97) % 128));
    }
    for (int which = 0; which < 4; which++) {
        const TrackParams &p = patches[which];
        char name[40];
#define STRATA_DENSE(Type, label)                                         \
        {                                                                 \
            static Type machine;                                          \
            machine.init(kRate);                                          \
            render_with_releases(machine, p, dense, count, frames);       \
            std::snprintf(name, sizeof name, "dense-" label "/%d", which); \
            check_golden(name, g_out, frames);                            \
        }
        STRATA_DENSE(Kick, "kick")
        STRATA_DENSE(Snare, "snare")
        STRATA_DENSE(Hat, "hat")
        STRATA_DENSE(Perc, "perc")
        STRATA_DENSE(VaMono, "vamono")
        STRATA_DENSE(VaPoly, "vapoly")
#undef STRATA_DENSE
        static Sampler sampler;
        sampler.init(kRate);
        sampler.set_sample(g_sample, 48000, 1, 44100.0f);
        render_with_releases(sampler, p, dense, count, frames);
        std::snprintf(name, sizeof name, "dense-sample/%d", which);
        check_golden(name, g_out, frames);

        static Granular grain;
        grain.init(kRate);
        grain.set_sample(g_sample, 48000, 1, 44100.0f);
        render_with_releases(grain, p, dense, count, frames);
        std::snprintf(name, sizeof name, "dense-grain/%d", which);
        check_golden(name, g_out, frames);
    }
}

// A steal's fade, sample by sample. Two hits fill the voices, and a third at frame 1970 steals
// one: the stolen voice fades over the first 24 frames of that block (1920), and the new hit
// starts on its frame. The 64 frames from 1916 are summarised, so most samples of the fade are
// pinned rather than one in a thousand.
TEST(golden_a_steal_splice_sample_by_sample) {
    const Trig hits[] = {trig(60, 127, 0), trig(60, 127, 960), trig(60, 127, 1970, false, 0)};
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, 100);
#define STRATA_FADE(Type, label)                                      \
    {                                                                 \
        static Type machine;                                          \
        machine.init(kRate);                                          \
        render(machine, p, hits, 3, 2048);                            \
        check_golden("fade-" label, g_out + 1916, 64);                \
    }
    STRATA_FADE(Kick, "kick")
    STRATA_FADE(Snare, "snare")
    STRATA_FADE(Hat, "hat")
    STRATA_FADE(Perc, "perc")
    STRATA_FADE(VaMono, "vamono")
    STRATA_FADE(VaPoly, "vapoly")
#undef STRATA_FADE
}
