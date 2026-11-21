// GRAIN: the granular player in dsp/grain.cpp and dsp/grainloops.ti.c.
//
// The PCM is synthesised here -- a sine for pitch, a marked band for position and spray, a
// constant for levels -- so every property is measurable without a fixture file. Nothing touches
// the card: `set_sample` is the whole input side, exactly as for SAMPLE.
//
// **The test that matters most is grain_never_reads_outside_the_buffer**, and it is built around
// an oracle rather than a tolerance. The sample handed to the machine is ALL ZEROS and sits
// between full-scale guard regions in the same array, so every legal read contributes exactly
// 0.0f and any read outside the buffer contributes something that is not. The assertion is
// therefore bit-exact silence over every parameter extreme, five buffer lengths and both channel
// layouts -- no threshold to argue about. An out-of-bounds read on the device does not fault, it
// silently returns garbage, so this is the one property that cannot be checked by listening.
//
// Its companion, grain_the_bounds_sweep_is_not_vacuous, runs the identical sweep over a sample
// that is NOT zero and insists most of it sounds: an implementation that passed the first test by
// never playing anything would fail the second.
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "grain.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

// One second of source and two of output, static because that does not belong on a stack frame.
constexpr int kSrcFrames = ENAGA_SAMPLE_RATE;
constexpr int kCapture = ENAGA_SAMPLE_RATE * 2;

int16_t g_mono[kSrcFrames];
int16_t g_stereo[kSrcFrames * 2];
float g_out[kCapture];
float g_other[kCapture];

// A small sample with full-scale guard regions either side of it in the SAME array, so that a read
// past either end shows up in the output. The guards are 32 frames because the widest legal read
// is two frames and the widest illegal one this could produce is a whole grain's span.
constexpr int kGuard = 32;
constexpr int kTinyMax = 512;
int16_t g_guarded[(kGuard + kTinyMax + kGuard)];
int16_t *const g_tiny = g_guarded + kGuard;
int16_t g_guarded_st[(kGuard + kTinyMax + kGuard) * 2];
int16_t *const g_tiny_st = g_guarded_st + kGuard * 2;

// --- parameter helpers ----------------------------------------------------
//
// POS 0 is the start, SIZ 64 is a middling grain, DEN 64 a middling overlap, SPR 0 no spray,
// PIT 64 is unity, SHP 0 the symmetric window, DEC 127 is hold (the cloud runs until it is
// stopped) and LVL 127 is unity. So this is the set that makes a plain cloud at the sample's own
// pitch.
TrackParams plain() {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, 0);
    p.set(1, 64);     // SIZ
    p.set(2, 64);     // DEN
    p.set(8, 64);     // PIT: unity
    p.set(10, 127);   // DEC: hold
    p.set(11, 127);   // LVL: unity
    return p;
}

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

Trig hit(uint8_t note = Granular::kRootNote, uint8_t velocity = 127, uint32_t frame = 0) {
    Trig t;
    t.note = note;
    t.velocity = velocity;
    t.frame = frame;
    return t;
}

// --- signal helpers -------------------------------------------------------

void clear(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = 0.0f;
}

void fill(float *x, int n, float value) {
    for (int i = 0; i < n; i++) x[i] = value;
}

// enaga::dsp::sin_turns rather than <cmath>: the same no-libm rule as the device code, and it is
// accurate to 4e-6, which is finer than anything measured here.
void fill_sine(int16_t *dst, int frames, int channels, float hz, float rate, float amplitude) {
    const float dt = hz * enaga::dsp::reciprocal(rate);
    for (int i = 0; i < frames; i++) {
        const float s = amplitude * 32767.0f * enaga::dsp::sin_turns(dt * float(i));
        const int16_t v = int16_t(s < 0.0f ? s - 0.5f : s + 0.5f);
        for (int c = 0; c < channels; c++) dst[i * channels + c] = v;
    }
}

void fill_const(int16_t *dst, int frames, int channels, int16_t value) {
    for (int i = 0; i < frames * channels; i++) dst[i] = value;
}

// Zero everywhere except [from, to), which holds a loud tone: played back it says WHICH frames
// were read, which is what the position and spray tests need.
void fill_band(int16_t *dst, int frames, int channels, int from, int to, float hz, float rate) {
    for (int i = 0; i < frames * channels; i++) dst[i] = 0;
    const float dt = hz * enaga::dsp::reciprocal(rate);
    for (int i = from; i < to && i < frames; i++) {
        const float s = 0.9f * 32767.0f * enaga::dsp::sin_turns(dt * float(i));
        const int16_t v = int16_t(s < 0.0f ? s - 0.5f : s + 0.5f);
        for (int c = 0; c < channels; c++) dst[i * channels + c] = v;
    }
}

int render_blocks(Machine &m, const TrackParams &p, float *out, int blocks) {
    for (int b = 0; b < blocks; b++) {
        m.prepare(p);
        m.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

float peak(const float *x, int n) {
    float best = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > best) best = a;
    }
    return best;
}

// NaN fails x == x and an infinity fails the bounds, so this needs no <cmath>.
bool finite_within(const float *x, int n, float bound) {
    for (int i = 0; i < n; i++) {
        if (!(x[i] == x[i])) return false;
        if (!(x[i] > -bound && x[i] < bound)) return false;
    }
    return true;
}

bool all_zero(const float *x, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != 0.0f) return false;
    }
    return true;
}

// Transitions between strictly positive and strictly negative, ignoring the silent runs a sparse
// cloud leaves between its grains. Twice the number of cycles in the window.
int sign_changes(const float *x, int n) {
    int count = 0;
    int last = 0;
    for (int i = 0; i < n; i++) {
        const int s = x[i] > 1e-6f ? 1 : (x[i] < -1e-6f ? -1 : 0);
        if (s != 0) {
            if (last != 0 && s != last) count++;
            last = s;
        }
    }
    return count;
}

int argmax_abs(const float *x, int n) {
    int best = 0;
    float level = -1.0f;
    for (int i = 0; i < n; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > level) {
            level = a;
            best = i;
        }
    }
    return best;
}

// --- the bounds sweep -----------------------------------------------------
//
// Every parameter that can move a read: POS and PIT at both ends and just inside them, SIZ over
// its range, DEN and SPR and SHP at both ends. DEC stays at hold so the cloud keeps spawning for
// the whole render rather than decaying out of the interesting region.
constexpr uint8_t kPos[] = {0, 1, 63, 126, 127};
constexpr uint8_t kSiz[] = {0, 64, 127};
constexpr uint8_t kDen[] = {0, 127};
constexpr uint8_t kSpr[] = {0, 127};
constexpr uint8_t kPit[] = {0, 1, 64, 127};
constexpr uint8_t kShp[] = {0, 127};
constexpr int kTinyFrames[] = {4, 5, 7, 16, 64, 300};

// Runs the sweep over `frames` frames of `data` and hands each render's output to `check`.
// Returns the number of renders.
template <class Check>
int sweep(const int16_t *data, int frames, int channels, Check check) {
    int renders = 0;
    for (uint8_t pos : kPos) {
        for (uint8_t siz : kSiz) {
            for (uint8_t den : kDen) {
                for (uint8_t spr : kSpr) {
                    for (uint8_t pit : kPit) {
                        for (uint8_t shp : kShp) {
                            Granular g;
                            g.init(kRate);
                            g.set_sample(data, uint32_t(frames), channels, kRate);
                            TrackParams p = plain();
                            p.set(0, pos);
                            p.set(1, siz);
                            p.set(2, den);
                            p.set(3, spr);
                            p.set(8, pit);
                            p.set(9, shp);
                            g.prepare(p);
                            g.trigger(hit());
                            clear(g_out, 12 * kBlk);
                            const int n = render_blocks(g, p, g_out, 12);
                            check(g_out, n);
                            renders++;
                            g.kill_all();
                        }
                    }
                }
            }
        }
    }
    return renders;
}

}  // namespace

// --- no sample ------------------------------------------------------------

TEST(grain_with_no_sample_is_silent) {
    Granular g;
    g.init(kRate);
    CHECK(!g.has_sample());
    CHECK(!g.active());
    CHECK_EQ(g.sample_frames(), 0);
    CHECK(g.sample_data() == nullptr);

    const TrackParams p = plain();
    g.prepare(p);
    g.trigger(hit());
    // A trigger with no PCM must not even make the machine active: the mixer skips it entirely.
    CHECK(!g.active());
    clear(g_out, 64 * kBlk);
    const int n = render_blocks(g, p, g_out, 64);
    CHECK(all_zero(g_out, n));
    CHECK_EQ(g.live_grains(), 0);
    CHECK(!g.active());
}

TEST(grain_refuses_a_sample_it_cannot_play) {
    fill_sine(g_mono, kSrcFrames, 1, 400.0f, kRate, 0.5f);
    const TrackParams p = plain();

    // Too short for anything to interpolate, a null pointer, and channel counts that are not 1
    // or 2: all legal inputs, all leaving the track silent rather than reading something.
    struct Case {
        const int16_t *data;
        uint32_t frames;
        int channels;
    };
    const Case cases[] = {
        {nullptr, 1000, 1},
        {g_mono, 0, 1},
        {g_mono, 1, 1},
        {g_mono, uint32_t(Granular::kMinFrames) - 1, 1},
        {g_mono, 1000, 0},
        {g_mono, 1000, 3},
        {g_mono, 1000, -1},
        {g_mono, uint32_t(Granular::kMaxFrames) + 1, 1},
    };
    for (const Case &c : cases) {
        Granular g;
        g.init(kRate);
        g.set_sample(c.data, c.frames, c.channels, kRate);
        CHECK(!g.has_sample());
        CHECK_EQ(g.sample_frames(), 0);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, 16 * kBlk);
        const int n = render_blocks(g, p, g_out, 16);
        CHECK(all_zero(g_out, n));
        CHECK(!g.active());
    }

    // And the smallest sample that IS accepted really is accepted.
    Granular ok;
    ok.init(kRate);
    ok.set_sample(g_mono, uint32_t(Granular::kMinFrames), 1, kRate);
    CHECK(ok.has_sample());
    CHECK_EQ(ok.sample_frames(), Granular::kMinFrames);
}

TEST(grain_a_sample_taken_away_mid_cloud_goes_silent) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.7f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    const TrackParams p = plain();
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, 32 * kBlk);
    int n = render_blocks(g, p, g_out, 32);
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(g.active());

    // The loader swapped the buffer out from under the cloud. Every grain was reading it, so they
    // all stop; the machine is silent and inactive from the next block.
    g.set_sample(nullptr, 0, 1, kRate);
    CHECK_EQ(g.live_grains(), 0);
    CHECK(!g.active());
    clear(g_out, 32 * kBlk);
    n = render_blocks(g, p, g_out, 32);
    CHECK(all_zero(g_out, n));
}

// --- the bounds oracle ----------------------------------------------------

TEST(grain_never_reads_outside_the_buffer) {
    // The sample is all zeros and the guards either side of it are full scale, so every legal read
    // contributes exactly 0.0f and any read past either end does not. The assertion is bit-exact
    // silence: no tolerance, no interpolation argument, no threshold.
    for (int i = 0; i < kGuard; i++) {
        g_guarded[i] = (i & 1) ? 32767 : -32768;
        g_guarded[kGuard + kTinyMax + i] = (i & 1) ? -32768 : 32767;
        g_guarded_st[2 * i + 0] = 32767;
        g_guarded_st[2 * i + 1] = 32767;
        g_guarded_st[2 * (kGuard + kTinyMax + i) + 0] = -32768;
        g_guarded_st[2 * (kGuard + kTinyMax + i) + 1] = -32768;
    }

    int renders = 0;
    for (int frames : kTinyFrames) {
        // Zero the playable region, and the guards stay as they are. The frames between `frames`
        // and kTinyMax are inside the array but outside what the machine was told it owns, so
        // they are full scale too: reading one frame too far is caught as readily as reading
        // thirty-two.
        for (int i = 0; i < kTinyMax; i++) {
            // Both channels of a playable frame must be EXACTLY zero: the stereo tap sums the
            // pair, so an R channel of -1 would make every legal read nonzero and the oracle
            // worthless. Outside the playable region both are full scale and the SAME sign, so
            // the sum cannot cancel either.
            const int16_t fill16 = i < frames ? int16_t(0) : int16_t((i & 1) ? 32767 : -32768);
            g_tiny[i] = fill16;
            g_tiny_st[2 * i + 0] = fill16;
            g_tiny_st[2 * i + 1] = fill16;
        }
        renders += sweep(g_tiny, frames, 1,
                         [](const float *out, int n) { CHECK(all_zero(out, n)); });
        renders += sweep(g_tiny_st, frames, 2,
                         [](const float *out, int n) { CHECK(all_zero(out, n)); });
    }
    // The sweep is 480 parameter combinations per length and layout; if it ever shrinks silently
    // this test stops being the one that matters.
    CHECK_EQ(renders, 480 * 2 * int(sizeof(kTinyFrames) / sizeof(kTinyFrames[0])));
}

TEST(grain_the_bounds_sweep_is_not_vacuous) {
    // The identical sweep over a sample that is NOT zero. An implementation that passed the test
    // above by never playing anything fails here.
    int sounded = 0;
    int renders = 0;
    for (int frames : kTinyFrames) {
        fill_sine(g_tiny, kTinyMax, 1, 1200.0f, kRate, 0.6f);
        renders += sweep(g_tiny, frames, 1, [&sounded](const float *out, int n) {
            CHECK(finite_within(out, n, 3.0f));
            if (peak(out, n) > 0.0f) sounded++;
        });
    }
    CHECK_EQ(renders, 480 * int(sizeof(kTinyFrames) / sizeof(kTinyFrames[0])));
    // A 4-frame buffer cannot support a grain at every pitch, so not every combination sounds;
    // most must.
    CHECK(sounded > (renders * 3) / 5);
}

TEST(grain_a_four_frame_sample_neither_hangs_nor_reads_past_its_end) {
    // The smallest accepted sample, which is smaller than the shortest grain at unity: the start
    // cannot slide anywhere, so the LENGTH is what gets cut, and at the top of the pitch range it
    // is cut below kMinGrain and nothing spawns at all. The guards catch a read either side.
    for (int i = 0; i < kGuard; i++) {
        g_guarded[i] = 32767;
        g_guarded[kGuard + kTinyMax + i] = -32768;
    }
    for (int i = 0; i < kTinyMax; i++) g_tiny[i] = i < 4 ? int16_t(0) : int16_t(32767);

    for (uint8_t pit = 0; pit <= 127; pit++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_tiny, 4, 1, kRate);
        CHECK(g.has_sample());
        TrackParams p = plain();
        p.set(8, pit);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, 8 * kBlk);
        const int n = render_blocks(g, p, g_out, 8);
        CHECK(all_zero(g_out, n));     // the four playable frames are zero; the guards are not
        g.kill_all();
    }
}

TEST(grain_position_at_the_very_end_slides_the_grain_back_instead_of_going_silent) {
    // POS 127 points at the last frame, where a whole grain cannot start. The grain's start slides
    // back until it fits, so the top of the knob plays the LAST grain of the sample. Without that
    // the useful end of POS would be silent.
    const int frames = 8192;
    fill_sine(g_mono, frames, 1, 500.0f, kRate, 0.6f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(0, 127);
    p.set(1, 0);        // the shortest grain, so plenty of room to slide into
    g.prepare(p);
    CHECK_EQ(g.position_frames(), frames - 1);
    g.trigger(hit());
    clear(g_out, 40 * kBlk);
    const int n = render_blocks(g, p, g_out, 40);
    CHECK(finite_within(g_out, n, 2.0f));
    CHECK(peak(g_out, n) > 0.05f);
}

TEST(grain_extreme_pitch_against_both_buffer_edges_is_safe) {
    // The pitch decides how far a grain walks, so it is the parameter that pushes a read off the
    // end. Swept over its whole range at both ends of POS, against guards, with the playable
    // region zero: bit-exact silence again.
    for (int i = 0; i < kGuard; i++) {
        g_guarded[i] = 32767;
        g_guarded[kGuard + kTinyMax + i] = -32768;
    }
    const int frames = 256;
    for (int i = 0; i < kTinyMax; i++) g_tiny[i] = i < frames ? int16_t(0) : int16_t(-32768);

    const uint8_t positions[3] = {0, 64, 127};
    for (uint8_t pos : positions) {
        for (uint8_t pit = 0; pit <= 127; pit++) {
            Granular g;
            g.init(kRate);
            g.set_sample(g_tiny, uint32_t(frames), 1, kRate);
            TrackParams p = plain();
            p.set(0, pos);
            p.set(1, 127);     // the longest grain, so the bound has the most work to do
            p.set(2, 127);     // and the deepest overlap, so eight of them are in the air
            p.set(8, pit);
            g.prepare(p);
            g.trigger(hit());
            clear(g_out, 10 * kBlk);
            const int n = render_blocks(g, p, g_out, 10);
            CHECK(all_zero(g_out, n));
            g.kill_all();
        }
    }
}

// --- the Machine contract -------------------------------------------------

TEST(grain_render_add_accumulates_and_never_clears) {
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.7f);
    const TrackParams p = plain();

    // Two machines with the same seed see the same spawns, so their clouds are identical: one
    // renders into silence and one into a buffer that already holds something.
    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_out, 24 * kBlk);
    const int n = render_blocks(a, p, g_out, 24);
    CHECK(peak(g_out, n) > 0.0f);

    Granular b;
    b.init(kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.prepare(p);
    b.trigger(hit());
    fill(g_other, kCapture, 0.25f);
    render_blocks(b, p, g_other, 24);
    for (int i = 0; i < n; i++) CHECK_NEAR(g_other[i], 0.25f + g_out[i], 1e-6);

    // And an inactive machine does not touch the buffer at all.
    Granular idle;
    idle.init(kRate);
    idle.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    idle.prepare(p);
    fill(g_other, 8 * kBlk, 0.5f);
    render_blocks(idle, p, g_other, 8);
    for (int i = 0; i < 8 * kBlk; i++) CHECK_EQ(g_other[i], 0.5f);
}

TEST(grain_kill_all_silences_at_once) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    const TrackParams p = plain();
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, 24 * kBlk);
    int n = render_blocks(g, p, g_out, 24);
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(g.live_grains() > 0);

    g.kill_all();
    CHECK(!g.active());
    CHECK_EQ(g.live_grains(), 0);
    clear(g_out, 24 * kBlk);
    n = render_blocks(g, p, g_out, 24);
    CHECK(all_zero(g_out, n));     // no tail whatsoever: a hard cut
}

TEST(grain_release_all_leaves_a_tail) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    const TrackParams p = plain();      // DEC 127: hold, so only a release can end it
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, 24 * kBlk);
    int n = render_blocks(g, p, g_out, 24);
    const float held = peak(g_out, n);
    CHECK(held > 0.0f);

    // STOP once. The cloud thins out instead of stopping dead, so the next tenth of a second
    // still sounds -- and quieter than the held cloud was.
    g.release_all();
    CHECK(g.active());
    clear(g_out, kCapture);
    n = render_blocks(g, p, g_out, 60);                    // 80 ms
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(peak(g_out, n) < held * 1.05f);

    // And it does end: well past the release the machine is silent and inactive again.
    clear(g_out, kCapture);
    n = render_blocks(g, p, g_out, 600);                   // 800 ms
    CHECK(!g.active());
    CHECK_EQ(g.live_grains(), 0);
    CHECK(all_zero(g_out + n - 8 * kBlk, 8 * kBlk));
}

TEST(grain_parameter_names_are_three_upper_case_letters) {
    Granular g;
    g.init(kRate);
    for (int i = 0; i < kParams; i++) {
        const char *name = g.param_name(i);
        CHECK(name != nullptr);
        int length = 0;
        while (name[length] != '\0') length++;
        CHECK_EQ(length, 3);
        for (int c = 0; c < 3; c++) {
            const char ch = name[c];
            // The device font has upper case, digits and a few symbols only (docs/ENAGA_API.md),
            // and '-' is what every machine uses for a slot it does not own.
            CHECK((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-');
        }
    }
    // The pages it owns are SRC and AMP; FLTR and FX belong to the insert chain.
    for (int i = 0; i < 4; i++) CHECK(g.param_name(i)[0] != '-');
    for (int i = 4; i < 8; i++) CHECK_EQ(g.param_name(i)[0], '-');
    for (int i = 8; i < 12; i++) CHECK(g.param_name(i)[0] != '-');
    for (int i = 12; i < 16; i++) CHECK_EQ(g.param_name(i)[0], '-');
    // Eight parameters, which is what Machine::used_params already returns.
    CHECK_EQ(g.used_params(), 8);
    // Out of range is answered, not indexed.
    CHECK_EQ(g.param_name(-1)[0], '-');
    CHECK_EQ(g.param_name(kParams)[0], '-');
    CHECK_EQ(g.param_name(1000)[0], '-');
}

TEST(grain_is_safe_to_trigger_before_the_first_prepare) {
    // init() derives a usable set from mid parameters, so a trigger that beats the first prepare
    // sounds rather than reading uninitialised coefficients.
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.6f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    g.trigger(hit());
    clear(g_out, 24 * kBlk);
    int n = 0;
    for (int b = 0; b < 24; b++) {
        g.render_add(g_out + b * kBlk, kBlk);      // no prepare at all
        n += kBlk;
    }
    CHECK(finite_within(g_out, n, 2.0f));
    CHECK(peak(g_out, n) > 0.0f);
}

// --- finiteness at the extremes -------------------------------------------

TEST(grain_all_zero_parameters_are_silent_not_broken) {
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.9f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    const TrackParams p = uniform(0);
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(g, p, g_out, 200);
    CHECK(finite_within(g_out, n, 1.0f));
    CHECK(all_zero(g_out, n));     // LVL 0 is silence, and silence is not a crash
}

TEST(grain_all_127_parameters_stay_finite) {
    fill_sine(g_mono, kSrcFrames, 1, 110.0f, kRate, 0.95f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    const TrackParams p = uniform(127);
    g.prepare(p);
    g.trigger(hit(127, 127));
    clear(g_out, kCapture);
    const int n = render_blocks(g, p, g_out, 400);
    // kGrains windows of at most 1, with latched gains of at most 1, times LVL's trim: 8 * 0.5 = 4
    // is the hard ceiling and nothing can pass it without the window, the gains or the trim being
    // wrong (grain_output_is_bounded_whatever_the_trigger_pattern has the argument).
    CHECK(finite_within(g_out, n, 4.01f));
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(g.live_grains() <= Granular::kGrains);
}

TEST(grain_each_parameter_alone_at_both_extremes_stays_finite) {
    fill_sine(g_mono, kSrcFrames, 1, 220.0f, kRate, 0.8f);
    for (int index = 0; index < kParams; index++) {
        for (int end = 0; end < 2; end++) {
            Granular g;
            g.init(kRate);
            g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
            TrackParams p = uniform(64);
            p.set(index, end == 0 ? 0 : 127);
            g.prepare(p);
            g.trigger(hit());
            clear(g_out, 100 * kBlk);
            const int n = render_blocks(g, p, g_out, 100);
            CHECK(finite_within(g_out, n, 4.01f));
            CHECK(g.live_grains() <= Granular::kGrains);
            g.kill_all();
        }
    }
}

// --- the grain pool -------------------------------------------------------

TEST(grain_live_grains_never_exceed_the_pool) {
    // The deepest overlap the knob reaches, the longest grain, and a trigger every block on a
    // different note: the scheduler must steal rather than layer, and the count must never pass
    // kGrains whatever it is asked for.
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.8f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 127);    // SIZ: the longest grain
    p.set(2, 127);    // DEN: the deepest overlap
    p.set(3, 127);    // SPR: and the onsets jittered
    g.prepare(p);

    int highest = 0;
    clear(g_out, kCapture);
    for (int b = 0; b < 400; b++) {
        g.trigger(hit(uint8_t(48 + (b % 25))));
        g.prepare(p);
        g.render_add(g_out + (b % 100) * kBlk, kBlk);
        const int live = g.live_grains();
        CHECK(live <= Granular::kGrains);
        if (live > highest) highest = live;
    }
    // ... and it does fill up, so the ceiling is being tested rather than never reached.
    CHECK_EQ(highest, Granular::kGrains);
    CHECK(finite_within(g_out, 100 * kBlk, 3.0f));
}

TEST(grain_a_retrigger_does_not_layer_a_second_cloud) {
    // One voice. A retrigger restarts the envelope and spawns a grain on its own frame, and the
    // pool is what bounds the result: a trigger every block cannot stack more than kGrains grains,
    // and because each grain latches 1 / sqrt(the crowd it was born into), the burst gets quieter
    // grains rather than a louder cloud. Compared with one trigger, the level moves by a factor,
    // not by a multiple of the pool.
    fill_const(g_mono, kSrcFrames, 1, 16000);
    const TrackParams p = plain();

    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_out, 200 * kBlk);
    const int n = render_blocks(a, p, g_out, 200);
    const float once = peak(g_out, n);
    CHECK(once > 0.0f);

    Granular b;
    b.init(kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.prepare(p);
    clear(g_other, 200 * kBlk);
    for (int block = 0; block < 200; block++) {
        b.trigger(hit());                      // a trigger every 64 frames: 750 a second
        b.prepare(p);
        b.render_add(g_other + block * kBlk, kBlk);
        CHECK(b.live_grains() <= Granular::kGrains);
    }
    CHECK(peak(g_other, n) > 0.0f);
    // Eight grains of unity gain would be eight times louder; the compensation holds it to well
    // under three, and the sum of all eight latched gains (4.37) is the arithmetic ceiling.
    CHECK(peak(g_other, n) < once * 3.0f);

    // The crowd compensation is exactly 1 / sqrt(k + 1).
    for (int k = 0; k < Granular::kGrains; k++) {
        const float g = Granular::kCrowdGain[k];
        CHECK_NEAR(g * g * float(k + 1), 1.0f, 1e-6);
    }
}

TEST(grain_output_is_bounded_whatever_the_trigger_pattern) {
    // Two bounds, and the test is here to keep both honest.
    //
    // The HARD one is arithmetic: at most kGrains windows of at most 1, each with a latched gain
    // of at most 1, times LVL's trim. Nothing about the material or the trigger pattern can pass
    // it. The MEASURED one is the number kHeadroom was calibrated against: the worst peak over the
    // whole SIZ/DEN/SPR sweep on the loudest material there is -- a full-scale constant, where
    // every grain reads the same maximum and nothing anywhere cancels -- hammered with a trigger
    // every block. If a later change to the compensation or the window makes the machine louder,
    // this is where it shows up rather than on the unit.
    fill_const(g_mono, kSrcFrames, 1, 32767);
    const float hard = 4.01f;              // kGrains * kHeadroom
    float worst = 0.0f;
    for (int siz = 0; siz <= 127; siz += 21) {
        for (int den = 0; den <= 127; den += 21) {
            for (int spr = 0; spr <= 127; spr += 42) {
                Granular g;
                g.init(kRate);
                g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
                TrackParams p = plain();
                p.set(1, uint8_t(siz));
                p.set(2, uint8_t(den));
                p.set(3, uint8_t(spr));
                g.prepare(p);
                clear(g_out, 200 * kBlk);
                for (int block = 0; block < 200; block++) {
                    g.trigger(hit(uint8_t(60 + (block % 3))));
                    g.prepare(p);
                    g.render_add(g_out + block * kBlk, kBlk);
                    CHECK(g.live_grains() <= Granular::kGrains);
                }
                CHECK(finite_within(g_out, 200 * kBlk, hard));
                const float pk = peak(g_out, 200 * kBlk);
                if (pk > worst) worst = pk;
            }
        }
    }
    CHECK(worst > 1.0f);       // the sweep does reach loud settings
    CHECK(worst < 1.75f);      // and kHeadroom holds them where it was calibrated to

    // The same sweep at a musical trigger rate -- one every 400 blocks, about twice a second --
    // never falls silent and never gets near the hard bound either.
    for (int siz = 0; siz <= 127; siz += 42) {
        for (int den = 0; den <= 127; den += 42) {
            Granular g;
            g.init(kRate);
            g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
            TrackParams p = plain();
            p.set(1, uint8_t(siz));
            p.set(2, uint8_t(den));
            g.prepare(p);
            g.trigger(hit());
            clear(g_out, 400 * kBlk);
            const int rendered = render_blocks(g, p, g_out, 400);
            CHECK(finite_within(g_out, rendered, 1.75f));
            CHECK(peak(g_out, rendered) > 0.1f);
        }
    }
}

TEST(grain_density_controls_how_many_grains_are_in_the_air) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.7f);
    int highest[2] = {0, 0};
    const uint8_t density[2] = {0, 127};
    for (int which = 0; which < 2; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(1, 100);                 // a long grain, so the overlap is what decides
        p.set(2, density[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, kCapture);
        for (int b = 0; b < 300; b++) {
            g.prepare(p);
            g.render_add(g_out + (b % 100) * kBlk, kBlk);
            const int live = g.live_grains();
            if (live > highest[which]) highest[which] = live;
        }
    }
    // DEN 0 asks for an overlap of 0.25, so one grain sounds at a time with gaps between; DEN 127
    // asks for 8 and fills the pool.
    CHECK_EQ(highest[0], 1);
    CHECK_EQ(highest[1], Granular::kGrains);
}

// --- the parameter mappings -----------------------------------------------

TEST(grain_size_knob_spans_five_milliseconds_to_a_third_of_a_second) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);

    TrackParams p = plain();
    p.set(1, 0);
    g.prepare(p);
    CHECK_NEAR(float(g.grain_length()), 0.005f * kRate, 1.0f);      // 5 ms

    p.set(1, 127);
    g.prepare(p);
    CHECK(g.grain_length() > int(0.35f * kRate));                   // about 390 ms
    CHECK(g.grain_length() < int(0.42f * kRate));
    CHECK(g.grain_length() <= Granular::kGrainMax);

    // Monotone, and never below the floor.
    int last = 0;
    for (int knob = 0; knob <= 127; knob++) {
        p.set(1, uint8_t(knob));
        g.prepare(p);
        CHECK(g.grain_length() >= Granular::kMinGrain);
        CHECK(g.grain_length() >= last);
        last = g.grain_length();
    }
}

TEST(grain_density_knob_sets_the_hop_against_the_size) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);

    TrackParams p = plain();
    p.set(1, 100);                      // a grain long enough that the hop floor never binds
    p.set(2, 127);
    g.prepare(p);
    const int len = g.grain_length();
    CHECK_NEAR(float(g.hop_length()), float(len) / 8.0f, float(len) * 0.02f);   // overlap 8

    p.set(2, 0);
    g.prepare(p);
    CHECK_NEAR(float(g.hop_length()), float(len) * 4.0f, float(len) * 0.05f);   // overlap 0.25

    // The hop never goes below its floor, whatever SIZ and DEN ask for: that floor is what bounds
    // how many grains one block can spawn, and what keeps render_slice terminating.
    for (int siz = 0; siz <= 127; siz += 7) {
        for (int den = 0; den <= 127; den += 7) {
            p.set(1, uint8_t(siz));
            p.set(2, uint8_t(den));
            g.prepare(p);
            CHECK(g.hop_length() >= 8);
        }
    }
}

TEST(grain_position_knob_maps_over_the_whole_sample) {
    const int frames = 8192;
    fill_sine(g_mono, frames, 1, 400.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(0, 0);
    g.prepare(p);
    CHECK_EQ(g.position_frames(), 0);
    p.set(0, 127);
    g.prepare(p);
    CHECK_EQ(g.position_frames(), frames - 1);
    p.set(0, 64);
    g.prepare(p);
    CHECK_NEAR(float(g.position_frames()), float(frames - 1) * 64.0f / 127.0f, 2.0f);

    int last = -1;
    for (int knob = 0; knob <= 127; knob++) {
        p.set(0, uint8_t(knob));
        g.prepare(p);
        CHECK(g.position_frames() > last);
        CHECK(g.position_frames() < frames);
        last = g.position_frames();
    }

    // A new sample changes what the knob means, so the position is recomputed on the swap and not
    // left pointing into a buffer that is no longer there.
    p.set(0, 127);
    g.prepare(p);
    g.set_sample(g_mono, 1024, 1, kRate);
    CHECK_EQ(g.position_frames(), 1023);
}

TEST(grain_reads_from_the_position_the_knob_asks_for) {
    // Zero everywhere but one loud band. With no spray a grain reads only around POS, so pointing
    // it at the silence gives bit-exact silence and pointing it at the band gives sound. That is
    // what pins POS to the buffer rather than to some offset of it.
    const int frames = 16384;
    fill_band(g_mono, frames, 1, 8192, 10240, 600.0f, kRate);

    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 0);      // a 5 ms grain: much narrower than the band and than the gap
    p.set(3, 0);      // no spray

    p.set(0, 8);      // about frame 1032: in the silence
    g.prepare(p);
    CHECK(g.position_frames() < 8000);
    g.trigger(hit());
    clear(g_out, 40 * kBlk);
    int n = render_blocks(g, p, g_out, 40);
    CHECK(all_zero(g_out, n));

    g.kill_all();
    p.set(0, 70);     // about frame 9031: inside the band
    g.prepare(p);
    CHECK(g.position_frames() > 8192);
    CHECK(g.position_frames() < 10240);
    g.trigger(hit());
    clear(g_out, 40 * kBlk);
    n = render_blocks(g, p, g_out, 40);
    CHECK(peak(g_out, n) > 0.05f);
}

TEST(grain_spray_moves_the_read_position_and_zero_spray_does_not) {
    // The same marked band, with POS parked in the silence 10,000 frames away from it. No spray
    // is exactly silent; full spray is half a second wide, so grains reach the band.
    const int frames = 48000;
    fill_band(g_mono, frames, 1, 20000, 24000, 600.0f, kRate);

    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(0, 26);     // about frame 9826
    p.set(1, 0);
    p.set(3, 0);
    g.prepare(p);
    CHECK(g.position_frames() < 12000);
    CHECK_EQ(g.spray_frames(), 0);
    g.trigger(hit());
    clear(g_out, 100 * kBlk);
    int n = render_blocks(g, p, g_out, 100);
    CHECK(all_zero(g_out, n));

    g.kill_all();
    p.set(3, 127);
    g.prepare(p);
    CHECK_NEAR(float(g.spray_frames()), 0.5f * kRate, 1.0f);
    g.trigger(hit());
    clear(g_out, 400 * kBlk);
    n = render_blocks(g, p, g_out, 400);
    CHECK(peak(g_out, n) > 0.01f);
    CHECK(finite_within(g_out, n, 2.0f));

    // And the spray is clamped to the sample, so a short buffer cannot be sprayed off the end.
    g.set_sample(g_mono, 2048, 1, kRate);
    g.prepare(p);
    CHECK(g.spray_frames() <= 2048);
}

TEST(grain_pitch_knob_is_unity_at_sixty_four) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);

    TrackParams p = plain();
    p.set(8, 64);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 1.0f, 1e-5);

    p.set(8, 127);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 4.0f, 0.02);        // +24 semitones

    p.set(8, 0);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 0.2441f, 0.002);    // -24.4 semitones

    int last_scaled = 0;
    for (int knob = 0; knob <= 127; knob++) {
        p.set(8, uint8_t(knob));
        g.prepare(p);
        const int scaled = int(g.read_step() * 1000.0f);
        CHECK(scaled >= last_scaled);
        last_scaled = scaled;
    }
}

TEST(grain_pitch_changes_the_read_rate) {
    // A sparse cloud of long grains over a sine: one octave up on PIT doubles the zero crossings.
    const int frames = kSrcFrames;
    fill_sine(g_mono, frames, 1, 500.0f, kRate, 0.8f);

    int crossings[2] = {0, 0};
    const uint8_t pitch[2] = {64, 96};     // 64 is unity; 96 is +12.19 semitones
    for (int which = 0; which < 2; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(frames), 1, kRate);
        TrackParams p = plain();
        p.set(0, 0);
        p.set(1, 110);     // long grains
        p.set(2, 127);     // overlapping, so there are no silent gaps to confuse the count
        p.set(8, pitch[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, 300 * kBlk);
        const int n = render_blocks(g, p, g_out, 300);
        crossings[which] = sign_changes(g_out, n);
        CHECK(crossings[which] > 0);
    }
    const float ratio = float(crossings[1]) * enaga::dsp::reciprocal(float(crossings[0]));
    CHECK_NEAR(ratio, 2.02f, 0.2f);
}

TEST(grain_the_trig_note_transposes_the_cloud) {
    const int frames = kSrcFrames;
    fill_sine(g_mono, frames, 1, 400.0f, kRate, 0.8f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 110);
    p.set(2, 127);
    g.prepare(p);

    // Note 60 is the root, so it reads at the sample's own rate.
    g.trigger(hit(Granular::kRootNote));
    clear(g_out, 8 * kBlk);
    render_blocks(g, p, g_out, 8);
    CHECK_NEAR(g.read_step(), 1.0f, 1e-5);

    // An octave up doubles it; an octave down halves it.
    g.kill_all();
    g.trigger(hit(uint8_t(Granular::kRootNote + 12)));
    render_blocks(g, p, g_out, 8);
    CHECK_NEAR(g.read_step(), 2.0f, 0.01);

    g.kill_all();
    g.trigger(hit(uint8_t(Granular::kRootNote - 12)));
    render_blocks(g, p, g_out, 8);
    CHECK_NEAR(g.read_step(), 0.5f, 0.01);

    // And the extremes clamp instead of overflowing the fixed-point step.
    g.kill_all();
    g.trigger(hit(127));
    render_blocks(g, p, g_out, 8);
    CHECK(g.read_step() <= 32.0f);
    g.kill_all();
    g.trigger(hit(0));
    render_blocks(g, p, g_out, 8);
    CHECK(g.read_step() >= 1.0f / 64.0f);
}

TEST(grain_a_source_at_another_rate_is_resampled) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.6f);
    Granular g;
    g.init(kRate);
    const TrackParams p = plain();

    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, 24000.0f);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 0.5f, 1e-5);      // half the output rate: read half as fast

    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, 96000.0f);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 2.0f, 1e-5);

    // A nonsense rate falls back to the output rate rather than producing a step of zero.
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, 0.0f);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 1.0f, 1e-5);
}

TEST(grain_shape_moves_the_window_peak) {
    // One long grain on its own (DEN at the bottom leaves a long gap after it), over a constant
    // sample, so the output IS the window. SHP 0 peaks in the middle, SHP 127 near the start.
    fill_const(g_mono, kSrcFrames, 1, 20000);
    const int blocks = 160;

    int where[2] = {0, 0};
    const uint8_t shape[2] = {0, 127};
    for (int which = 0; which < 2; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(1, 100);                 // a long grain
        p.set(2, 0);                   // sparse: one grain, then a gap
        p.set(9, shape[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, blocks * kBlk);
        const int n = render_blocks(g, p, g_out, blocks);
        where[which] = argmax_abs(g_out, n);
        CHECK(peak(g_out, n) > 0.0f);
        // Whatever the shape, the grain begins in silence: the window's first value is zero, so
        // an onset cannot click.
        CHECK_EQ(g_out[0], 0.0f);
        // And it ends in silence too.
        const int len = g.grain_length();
        if (len + 2 < n) CHECK(g_out[len + 1] == 0.0f || (g_out[len + 1] < 1e-3f &&
                                                          g_out[len + 1] > -1e-3f));
    }
    // The symmetric window peaks near the middle of the grain; the plucked one within the first
    // tenth of it.
    Granular ref;
    ref.init(kRate);
    ref.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 100);
    ref.prepare(p);
    const int len = ref.grain_length();
    CHECK(where[0] > (len * 2) / 5);
    CHECK(where[0] < (len * 3) / 5);
    CHECK(where[1] < len / 10);
    CHECK(where[1] < where[0]);
}

TEST(grain_decay_at_the_top_holds_and_below_it_fades) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    const TrackParams held = [] {
        TrackParams p = plain();
        p.set(10, 127);
        return p;
    }();
    const TrackParams fading = [] {
        TrackParams p = plain();
        p.set(10, 0);      // 5 ms to silence
        return p;
    }();

    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    a.prepare(held);
    a.trigger(hit());
    clear(g_out, kCapture);
    int n = render_blocks(a, held, g_out, 700);     // most of a second
    CHECK(a.active());
    CHECK(peak(g_out + n - 4 * kBlk, 4 * kBlk) > 0.0f);

    Granular b;
    b.init(kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.prepare(fading);
    b.trigger(hit());
    clear(g_out, kCapture);
    n = render_blocks(b, fading, g_out, 700);
    CHECK(peak(g_out, 2 * kBlk) > 0.0f);            // it did sound
    CHECK(!b.active());                             // and then it finished on its own
    CHECK(all_zero(g_out + n - 4 * kBlk, 4 * kBlk));

    // A longer decay makes a longer note.
    float tail[2] = {0.0f, 0.0f};
    const uint8_t decay[2] = {40, 80};
    for (int which = 0; which < 2; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(10, decay[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, kCapture);
        const int rendered = render_blocks(g, p, g_out, 400);
        tail[which] = peak(g_out + rendered - 20 * kBlk, 20 * kBlk);
    }
    CHECK(tail[1] > tail[0]);
}

TEST(grain_level_is_square_law) {
    fill_const(g_mono, kSrcFrames, 1, 24000);
    float level[3] = {0.0f, 0.0f, 0.0f};
    const uint8_t knob[3] = {127, 90, 64};
    for (int which = 0; which < 3; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(11, knob[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, 100 * kBlk);
        const int n = render_blocks(g, p, g_out, 100);
        level[which] = peak(g_out, n);
    }
    CHECK(level[0] > 0.0f);
    // Square law: knob 64 is (64/127)^2 = 0.254 of knob 127, and knob 90 is 0.502 of it.
    CHECK_NEAR(level[2] * enaga::dsp::reciprocal(level[0]), 0.254f, 0.03f);
    CHECK_NEAR(level[1] * enaga::dsp::reciprocal(level[0]), 0.502f, 0.03f);
}

TEST(grain_velocity_and_accent_scale_the_cloud) {
    fill_const(g_mono, kSrcFrames, 1, 24000);
    const TrackParams p = plain();

    float level[3] = {0.0f, 0.0f, 0.0f};
    for (int which = 0; which < 3; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        g.prepare(p);
        Trig t = hit(Granular::kRootNote, which == 0 ? 127 : 64);
        t.accent = which == 2;
        g.trigger(t);
        clear(g_out, 100 * kBlk);
        const int n = render_blocks(g, p, g_out, 100);
        level[which] = peak(g_out, n);
    }
    CHECK(level[0] > 0.0f);
    CHECK_NEAR(level[1] * enaga::dsp::reciprocal(level[0]), 64.0f / 127.0f, 0.03f);
    CHECK(level[2] > level[1] * 1.15f);     // accent is 1.25x, clamped at 1
    CHECK(level[2] <= level[0] * 1.02f);
}

// --- the source -----------------------------------------------------------

TEST(grain_stereo_source_sums_to_mono) {
    const int frames = 8192;
    fill_sine(g_mono, frames, 1, 600.0f, kRate, 0.8f);
    fill_sine(g_stereo, frames, 2, 600.0f, kRate, 0.8f);
    const TrackParams p = plain();

    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(frames), 1, kRate);
    CHECK_EQ(a.sample_channels(), 1);
    a.prepare(p);
    a.trigger(hit());
    clear(g_out, 60 * kBlk);
    const int n = render_blocks(a, p, g_out, 60);

    Granular b;
    b.init(kRate);
    b.set_sample(g_stereo, uint32_t(frames), 2, kRate);
    CHECK_EQ(b.sample_channels(), 2);
    CHECK_EQ(b.sample_frames(), frames);
    b.prepare(p);
    b.trigger(hit());
    clear(g_other, 60 * kBlk);
    render_blocks(b, p, g_other, 60);

    // Both channels hold the same tone, so the mono sum is that tone at its own level and not
    // twice it: the halving is folded into the output gain. Same seed, same spawns, so the two
    // clouds are the same cloud.
    CHECK(peak(g_out, n) > 0.0f);
    for (int i = 0; i < n; i++) CHECK_NEAR(g_other[i], g_out[i], 2e-6);
}

TEST(grain_a_stereo_source_reads_both_channels_in_bounds) {
    // The stereo tap reads four int16 for one frame pair, so its last read is three words past
    // 2 * pos. Guards either side of an all-zero stereo region catch any of the four.
    for (int i = 0; i < kGuard; i++) {
        g_guarded_st[2 * i + 0] = 32767;
        g_guarded_st[2 * i + 1] = 32767;
        g_guarded_st[2 * (kGuard + kTinyMax + i) + 0] = -32768;
        g_guarded_st[2 * (kGuard + kTinyMax + i) + 1] = -32768;
    }
    for (int frames : kTinyFrames) {
        for (int i = 0; i < kTinyMax; i++) {
            // Same sign in both channels outside the playable region: the tap sums the pair, so
            // opposite signs would cancel and hide exactly the read this test is looking for.
            const int16_t v = i < frames ? int16_t(0) : int16_t(32767);
            g_tiny_st[2 * i + 0] = v;
            g_tiny_st[2 * i + 1] = v;
        }
        for (uint8_t pos = 120; pos <= 127; pos++) {
            Granular g;
            g.init(kRate);
            g.set_sample(g_tiny_st, uint32_t(frames), 2, kRate);
            TrackParams p = plain();
            p.set(0, pos);
            p.set(1, 127);
            p.set(2, 127);
            p.set(3, 127);
            p.set(8, 127);
            g.prepare(p);
            g.trigger(hit());
            clear(g_out, 10 * kBlk);
            const int n = render_blocks(g, p, g_out, 10);
            CHECK(all_zero(g_out, n));
            g.kill_all();
        }
    }
}

// --- scheduling -----------------------------------------------------------

TEST(grain_the_first_grain_starts_on_the_trigs_own_frame) {
    // The block is cut at the trig's frame, so a trig at frame 40 leaves the first 40 samples of
    // the block untouched and starts the cloud at 40.
    fill_const(g_mono, kSrcFrames, 1, 24000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(9, 127);     // the plucked window, so the grain rises fast enough to see
    g.prepare(p);
    g.trigger(hit(Granular::kRootNote, 127, 40));
    clear(g_out, 8 * kBlk);
    const int n = render_blocks(g, p, g_out, 8);
    CHECK(all_zero(g_out, 40));
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(peak(g_out + 40, 8 * kBlk - 40) > 0.0f);

    // A frame past the block still arrives, just later: the wait is carried across blocks.
    g.kill_all();
    g.trigger(hit(Granular::kRootNote, 127, 200));
    clear(g_out, 8 * kBlk);
    render_blocks(g, p, g_out, 8);
    CHECK(all_zero(g_out, 200));
    CHECK(peak(g_out + 200, 8 * kBlk - 200) > 0.0f);
}

TEST(grain_render_add_handles_a_call_longer_than_one_block) {
    // The device always calls with ENAGA_BLOCK_FRAMES, but render_add slices anything longer, and
    // the result must be the same cloud as the equivalent run of block-sized calls.
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.7f);
    const TrackParams p = plain();

    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_out, 16 * kBlk);
    a.render_add(g_out, 16 * kBlk);

    Granular b;
    b.init(kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.prepare(p);
    b.trigger(hit());
    clear(g_other, 16 * kBlk);
    for (int i = 0; i < 16; i++) b.render_add(g_other + i * kBlk, kBlk);

    CHECK(peak(g_out, 16 * kBlk) > 0.0f);
    for (int i = 0; i < 16 * kBlk; i++) CHECK_NEAR(g_out[i], g_other[i], 1e-6);

    // And an odd length, which is not a multiple of the slice, is handled too.
    Granular c;
    c.init(kRate);
    c.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    c.prepare(p);
    c.trigger(hit());
    clear(g_out, 200);
    c.render_add(g_out, 100);
    c.render_add(g_out + 100, 37);
    CHECK(finite_within(g_out, 137, 2.0f));
    CHECK(all_zero(g_out + 137, 63));
}

TEST(grain_a_sparse_cloud_leaves_gaps_and_a_dense_one_does_not) {
    // DEN at the bottom asks for an overlap of 0.25, so three quarters of the time nothing is
    // sounding; at the top the pool is full and the output is continuous. That is the audible
    // difference the hop is there to make.
    fill_const(g_mono, kSrcFrames, 1, 24000);
    int zeros[2] = {0, 0};
    const uint8_t density[2] = {0, 127};
    for (int which = 0; which < 2; which++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(1, 60);
        p.set(2, density[which]);
        g.prepare(p);
        g.trigger(hit());
        clear(g_out, 400 * kBlk);
        const int n = render_blocks(g, p, g_out, 400);
        for (int i = 0; i < n; i++) zeros[which] += g_out[i] == 0.0f ? 1 : 0;
    }
    CHECK(zeros[0] > 400 * kBlk / 4);
    CHECK(zeros[1] < 400 * kBlk / 50);
}
