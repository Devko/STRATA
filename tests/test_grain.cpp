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

// KEYS mode lets a key go with release(note). A cloud at DEC 127 (hold) played from KEYS used
// to sound for ever: Granular had no release, so the base class's no-op swallowed the key-up.
TEST(grain_releasing_its_note_ends_a_held_cloud) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    const TrackParams p = plain();      // DEC 127: hold
    for (int pass = 0; pass < 3; pass++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        g.prepare(p);
        Trig t = hit();
        if (pass == 2) t.frame = 3 * uint32_t(kBlk);   // still waiting for its frame
        g.trigger(t);
        if (pass < 2) {
            clear(g_out, 24 * kBlk);
            CHECK(peak(g_out, render_blocks(g, p, g_out, 24)) > 0.0f);
        }
        // pass 0: another note is let go, and the cloud holds. 1 and 2: its own note is.
        g.release(uint8_t(pass == 0 ? t.note + 1 : t.note));
        clear(g_out, kCapture);
        const int n = render_blocks(g, p, g_out, 600);
        if (pass == 0) {
            CHECK(g.active());
            CHECK(peak(g_out + n - 8 * kBlk, 8 * kBlk) > 0.0f);
        } else {
            if (pass == 1) CHECK(peak(g_out, n) > 0.0f);   // a release, not a cut
            CHECK(!g.active());
            CHECK(all_zero(g_out + n - 8 * kBlk, 8 * kBlk));
        }
    }
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
    const float hard = 8.01f;              // kGrains * kHeadroom
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
    CHECK(worst > 2.0f);       // the sweep does reach loud settings
    CHECK(worst < 3.5f);       // and kHeadroom holds them where it was calibrated to

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
            CHECK(finite_within(g_out, rendered, 3.5f));
            CHECK(peak(g_out, rendered) > 0.2f);
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

TEST(grain_size_knob_spans_eight_milliseconds_to_two_thirds_of_a_second) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);

    TrackParams p = plain();
    p.set(1, 0);
    g.prepare(p);
    CHECK_NEAR(float(g.grain_length()), 0.008f * kRate, 1.0f);      // 8 ms

    p.set(1, 64);
    g.prepare(p);
    CHECK(g.grain_length() > int(0.070f * kRate));                  // about 75 ms: a cloud
    CHECK(g.grain_length() < int(0.080f * kRate));

    p.set(1, 127);
    g.prepare(p);
    CHECK(g.grain_length() > int(0.67f * kRate));                   // about 680 ms
    CHECK(g.grain_length() < int(0.69f * kRate));
    CHECK(g.grain_length() <= Granular::kGrainMax);
    CHECK_NEAR(grain_size_seconds(127), 0.68f, 0.002f);             // what the screen says

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
    CHECK_NEAR(g.read_step(), 0.25f, 0.002);      // -24 semitones: whole semitones only

    // An octave up is exactly 2, not the 19 cents sharp or flat the old continuous knob gave at
    // the two bytes either side of it.
    p.set(8, 96);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 2.0f, 1e-4);
    p.set(8, 95);
    g.prepare(p);
    CHECK_NEAR(g.read_step(), 2.0f, 1e-4);
    // And every byte plays a whole semitone.
    for (int knob = 0; knob <= 127; knob++) {
        const float semis = (float(knob) - 64.0f) * (24.0f / 63.0f);
        CHECK(grain_pitch_semitones(uint8_t(knob)) - semis <= 0.5f);
        CHECK(semis - grain_pitch_semitones(uint8_t(knob)) <= 0.5f);
    }

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

// --- the interpolator and the window, against exact oracles -----------------

TEST(grain_reads_a_fractional_position_exactly_on_a_ramp) {
    // Linear interpolation of a RAMP is exact, which makes a ramp an oracle for the whole read
    // path -- the fixed-point advance, its carry, the fraction, and the blend -- with no need for
    // this test to know anything about the window.
    //
    // One sparse grain from frame 0, rendered twice over the same ramp: once at a step of exactly
    // 1.0 and once at exactly 1.5. The step comes from the SOURCE RATE rather than from PIT,
    // because 1.5 is not a whole number of the knob's semitone steps and the point here is an
    // exact ratio. Sample i of the first is window(i/len) * i; of the second, window(i/len) * 1.5i.
    // So wherever the first is not zero the second is 1.5 times it -- and that holds only if every
    // piece of the read path is right.
    const int frames = 4096;
    for (int i = 0; i < frames; i++) g_mono[i] = int16_t(i);

    TrackParams p = plain();
    p.set(0, 0);      // POS: frame 0
    p.set(1, 0);      // SIZ: the shortest grain, well inside the ramp
    p.set(2, 0);      // DEN: sparse, so the first grain is alone in the buffer
    p.set(3, 0);      // SPR: no jitter, so both renders schedule identically

    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(frames), 1, kRate);          // step 1.0
    a.prepare(p);
    CHECK_NEAR(a.read_step(), 1.0f, 1e-6);
    const int len = a.grain_length();
    CHECK(len > 16);
    a.trigger(hit());
    clear(g_out, 8 * kBlk);
    render_blocks(a, p, g_out, 8);

    Granular b;
    b.init(kRate);
    b.set_sample(g_mono, uint32_t(frames), 1, 1.5f * kRate);   // step 1.5
    b.prepare(p);
    CHECK_NEAR(b.read_step(), 1.5f, 1e-6);
    CHECK_EQ(b.grain_length(), len);
    b.trigger(hit());
    clear(g_other, 8 * kBlk);
    render_blocks(b, p, g_other, 8);

    int compared = 0;
    for (int i = 1; i < len && i < 8 * kBlk; i++) {
        if (g_out[i] == 0.0f) continue;
        CHECK_NEAR(g_other[i] * enaga::dsp::reciprocal(g_out[i]), 1.5f, 2e-4);
        compared++;
    }
    CHECK(compared > len / 2);     // and it really did compare the grain, not three samples of it

    // The first sample of both is the window's own zero, whatever the step.
    CHECK_EQ(g_out[0], 0.0f);
    CHECK_EQ(g_other[0], 0.0f);
}

TEST(grain_the_window_never_exceeds_unity) {
    // One sparse grain over a full-scale constant sample is the window, unmixed and uncompensated:
    // the first grain of a cloud latches kCrowdGain[0] = 1, the envelope is at 1 from its first
    // sample, and every tap reads the same maximum value. So the peak is LVL's trim times full
    // scale and not a hair more -- which is what says the window table's clamp is holding and that
    // the table is being read inside itself. A read past its last entry lands in the scratch
    // buffer, whose values are a previous block's grains and can be anything.
    fill_const(g_mono, kSrcFrames, 1, 32767);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 110);     // a long grain
    p.set(2, 0);       // sparse: one at a time
    p.set(9, 0);       // the symmetric window, whose peak is exactly 1
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(g, p, g_out, 400);

    // kHeadroom (1.0) times LVL 127 squared (1.0) times 32767/32768.
    const float expected = 1.0f * (32767.0f / 32768.0f);
    CHECK(peak(g_out, n) <= expected * 1.00005f);
    CHECK(peak(g_out, n) >= expected * 0.9999f);

    // The plucked window reaches the same peak: SHP warps where the peak is, not how tall it is.
    g.kill_all();
    p.set(9, 127);
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, kCapture);
    const int m = render_blocks(g, p, g_out, 400);
    CHECK(peak(g_out, m) <= expected * 1.00005f);
    CHECK(peak(g_out, m) >= expected * 0.9999f);
}

// --- the bound as arithmetic rather than as audio ---------------------------
//
// The oracle test above can only see a read whose VALUE reaches the output. It cannot see one that
// lands on an out-of-range frame with an interpolation weight of exactly zero -- which is what
// happens whenever the read position arrives exactly on a frame boundary, and is exactly the case
// an off-by-one in the bound produces. On this chip such a read does not fault and returns
// garbage; the garbage is simply multiplied by zero this time. These two tests close that gap by
// checking the arithmetic directly instead of the signal.

namespace {

// The frames the read position will still move over a grain's remaining samples, from its CURRENT
// position and fraction. This is deliberately NOT the expression dsp/grain.cpp bounds a grain with
// at its onset -- that one starts from a fraction of zero -- so the two agreeing says something
// rather than nothing. Both products stay inside 32 bits for every length the machine allows.
int32_t remaining_advance(const strata_grain &s, int left) {
    const uint32_t j = uint32_t(left - 1);
    return int32_t(j) * s.istep +
           int32_t((s.frac + j * s.fstep) >> STRATA_GRAIN_FRAC_BITS);
}

}  // namespace

TEST(grain_advance_matches_the_loops_own_accumulation) {
    // strata_grain_advance is the whole of the bounds policy, and it is only right if it is the
    // SAME arithmetic the hot loop accumulates. So: a reference loop that does exactly what
    // dsp/grainloops.ti.c does per sample, against the closed form, over the step range the
    // machine clamps to and up to the longest grain it allows.
    const uint32_t steps[] = {0, 1, 7, 1024, 16384, 32768, 49152, 65534, 65535};
    const int32_t wholes[] = {0, 1, 2, 3, 31, 32};
    const int32_t counts[] = {1, 2, 3, 17, 64, 1000, 18910, STRATA_GRAIN_LEN_MAX - 1};

    for (int32_t istep : wholes) {
        for (uint32_t fstep : steps) {
            int32_t pos = 0;
            uint32_t frac = 0;
            int32_t next = 0;
            for (int32_t j = 0; j <= counts[int(sizeof(counts) / sizeof(counts[0])) - 1]; j++) {
                // Compare before advancing: advance(j) is where sample j reads.
                if (j == counts[next]) {
                    CHECK_EQ(strata_grain_advance(j, istep, fstep), pos);
                    if (next + 1 < int(sizeof(counts) / sizeof(counts[0]))) next++;
                }
                const uint32_t sum = frac + fstep;
                pos += istep + int32_t(sum >> STRATA_GRAIN_FRAC_BITS);
                frac = sum & STRATA_GRAIN_FRAC_MASK;
            }
        }
    }

    // Zero samples of advance is zero frames, whatever the step, and the whole part alone is
    // exact when there is no fraction.
    CHECK_EQ(strata_grain_advance(0, 32, 65535), 0);
    CHECK_EQ(strata_grain_advance(100, 3, 0), 300);
    // Half a frame a sample: after an odd count the position has moved by the floor.
    CHECK_EQ(strata_grain_advance(7, 0, STRATA_GRAIN_FRAC_ONE / 2), 3);
    CHECK_EQ(strata_grain_advance(8, 0, STRATA_GRAIN_FRAC_ONE / 2), 4);
    // And the extreme the 32-bit product is sized for does not wrap into a negative.
    CHECK(strata_grain_advance(STRATA_GRAIN_LEN_MAX - 1, 32, STRATA_GRAIN_FRAC_MASK) > 0);
    CHECK_EQ(strata_grain_advance(STRATA_GRAIN_LEN_MAX - 1, 0, STRATA_GRAIN_FRAC_MASK),
             STRATA_GRAIN_LEN_MAX - 2);
}

TEST(grain_every_live_grain_stays_inside_the_buffer_by_arithmetic) {
    // Every live grain, after every block, must satisfy: its current position is in the buffer,
    // its linear partner is in the buffer, and the frames it will still advance over its remaining
    // samples leave both in the buffer at the last of them. Checked on the small buffers, where the
    // bound has to cut a grain's length rather than slide its start, and over the parameters that
    // move a read.
    fill_sine(g_mono, kTinyMax, 1, 900.0f, kRate, 0.6f);
    const uint8_t positions[3] = {0, 63, 127};
    const uint8_t sizes[3] = {0, 64, 127};
    const uint8_t sprays[2] = {0, 127};
    const uint8_t pitches[8] = {0, 1, 2, 63, 64, 65, 126, 127};
    // And the OSC page, which moves a read too: SCN's head and PSP's per-grain step. Off, a
    // detune at its widest with the fastest scan, and half the grains an octave up with it.
    const uint8_t osc[3][2] = {{0, 0}, {127, 63}, {127, 127}};

    int grains_seen = 0;
    for (const auto &scan_spray : osc) {
    for (int frames : kTinyFrames) {
        for (uint8_t pos : positions) {
            for (uint8_t siz : sizes) {
                for (uint8_t spr : sprays) {
                    for (uint8_t pit : pitches) {
                        Granular g;
                        g.init(kRate);
                        g.set_sample(g_mono, uint32_t(frames), 1, kRate);
                        TrackParams p = plain();
                        p.set(0, pos);
                        p.set(1, siz);
                        p.set(3, spr);
                        p.set(8, pit);
                        p.set(kOscFirst + 0, scan_spray[0]);
                        p.set(kOscFirst + 1, scan_spray[1]);
                        g.prepare(p);
                        g.trigger(hit());
                        clear(g_out, 8 * kBlk);
                        for (int b = 0; b < 8; b++) {
                            g.prepare(p);
                            g.render_add(g_out + b * kBlk, kBlk);
                            for (int i = 0; i < Granular::kGrains; i++) {
                                if (!g.grain_live(i)) continue;
                                const strata_grain &s = g.grain_state(i);
                                const int left = g.grain_remaining(i);
                                grains_seen++;
                                CHECK(left > 0);
                                CHECK(s.pos >= 0);
                                CHECK(s.pos + 1 <= frames - 1);
                                const int32_t last = s.pos + remaining_advance(s, left);
                                CHECK(last >= 0);
                                CHECK(last + 1 <= frames - 1);
                            }
                        }
                        g.kill_all();
                    }
                }
            }
        }
    }
    }
    // The buffers are small, so a grain is not always possible; plenty must still have been seen.
    CHECK(grains_seen > 2500);
}

TEST(grain_the_last_sample_of_a_grain_is_the_windows_zero) {
    // A grain's final sample reads the END of the window table: at a phase inside the last 1/256
    // of the window the index is 255 and the read is of entry 255 and the GUARD entry after it. So
    // this is what says the guard is the window's zero rather than anything else -- and it is a
    // click on every single grain if it is not.
    //
    // Then the same thing at 192 kHz, where the longest grain is long enough that the phase's
    // accumulated float error outgrows the 1/len it falls short of 1 by: measured, it reaches
    // 1.000015, which is past the table. At 48 kHz with this compiler it does not quite (0.99943
    // is the worst), but the machine takes its rate as a parameter and cl6x is not obliged to
    // round as GCC does, which is why the loop clamps the phase and the table carries two guards.
    const float rates[2] = {kRate, 192000.0f};
    for (int which = 0; which < 2; which++) {
        const float rate = rates[which];
        fill_const(g_mono, kSrcFrames, 1, 24000);
        Granular g;
        g.init(rate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, rate);
        TrackParams p = plain();
        p.set(1, 127);     // the longest grain this rate allows
        p.set(2, 0);       // sparse, so the grain is alone and what follows it is silence
        p.set(9, 0);       // the symmetric window
        g.prepare(p);
        const int len = g.grain_length();
        CHECK(len > 1000);
        g.trigger(hit());

        const int blocks = (len + 4 * kBlk) / kBlk;
        CHECK(blocks * kBlk <= kCapture);
        clear(g_out, blocks * kBlk);
        const int n = render_blocks(g, p, g_out, blocks);
        CHECK(finite_within(g_out, n, 4.01f));
        CHECK(peak(g_out, n) > 0.1f);                 // it did play

        // The window's value over the grain's last samples is (pi/len)^2 or smaller -- call it
        // silence. The sample after the grain is silence too, the next onset being 4 x len away.
        for (int i = len - 2; i < len + 2; i++) {
            if (i < 1 || i >= n) continue;
            CHECK(g_out[i] < 1e-4f);
            CHECK(g_out[i] > -1e-4f);
        }
        // No step anywhere near the end of the grain, either: the biggest jump between
        // consecutive samples over the grain's last tenth is a fraction of its peak.
        float jump = 0.0f;
        for (int i = len - len / 10; i < len && i < n; i++) {
            if (i < 1) continue;
            const float d = g_out[i] - g_out[i - 1];
            const float a = d < 0.0f ? -d : d;
            if (a > jump) jump = a;
        }
        CHECK(jump < 0.01f * peak(g_out, n));
    }
}

TEST(grain_the_first_grain_after_a_note_uses_that_notes_step) {
    // The note is latched in the same call that starts the voice, and the fixed-point step has to
    // be rebuilt there and not left until the next prepare(): the first grain starts on the trig's
    // own frame, inside this block, and it is the attack of the note. If the step were a prepare()
    // behind, that first grain would play at the PREVIOUS note's pitch -- audible on every note
    // change, and invisible to any test that calls prepare() again before looking.
    fill_sine(g_mono, kSrcFrames, 1, 400.0f, kRate, 0.7f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 127);     // the longest grain, so a block moves its phase barely at all
    p.set(2, 0);       // sparse, so the only grains in the air are the ones the triggers spawned
    g.prepare(p);

    // Note 60 first, so note_ratio_ holds unity when the octave-up note arrives.
    g.trigger(hit(Granular::kRootNote));
    clear(g_out, kBlk);
    g.render_add(g_out, kBlk);
    CHECK_NEAR(g.read_step(), 1.0f, 1e-5);

    // An octave up, and NO prepare() between the trigger and the render.
    g.trigger(hit(uint8_t(Granular::kRootNote + 12)));
    clear(g_out, kBlk);
    g.render_add(g_out, kBlk);
    CHECK_NEAR(g.read_step(), 2.0f, 1e-5);

    // The grain spawned in that block -- the one whose window phase has moved least -- walks the
    // sample at two frames a sample, not one. The grain from the first note is still in the air
    // beside it, which is the point: the new note must not be read off the old one's step.
    int newest = -1;
    float youngest = 2.0f;
    for (int i = 0; i < Granular::kGrains; i++) {
        if (g.grain_live(i) && g.grain_state(i).q < youngest) {
            youngest = g.grain_state(i).q;
            newest = i;
        }
    }
    CHECK_EQ(g.live_grains(), 2);
    CHECK(newest >= 0);
    if (newest >= 0) {
        CHECK_EQ(g.grain_state(newest).istep, 2);
        CHECK_EQ(int(g.grain_state(newest).fstep), 0);
    }

    // And an octave down, the same way.
    g.kill_all();
    g.trigger(hit(uint8_t(Granular::kRootNote - 12)));
    clear(g_out, kBlk);
    g.render_add(g_out, kBlk);
    CHECK_NEAR(g.read_step(), 0.5f, 1e-5);
    newest = -1;
    for (int i = 0; i < Granular::kGrains; i++) {
        if (g.grain_live(i)) newest = i;
    }
    CHECK_EQ(g.live_grains(), 1);
    CHECK(newest >= 0);
    if (newest >= 0) {
        CHECK_EQ(g.grain_state(newest).istep, 0);
        CHECK_EQ(int(g.grain_state(newest).fstep), STRATA_GRAIN_FRAC_ONE / 2);
    }
}

// --- two oracles found by mutation testing this file -----------------------------------------
//
// Both of these close a gap a mutation walked through. The window's INTERPOLATION was untested
// (replacing it with the lower table entry survived, which turns a smooth window into a
// 256-step staircase), as was the upper end of its index CLAMP (clamping an entry lower survived,
// which parks the last 1/256 of every grain on the second-to-last entry instead of running down
// to the guard zero), and so was the stereo read's SECOND TAP (taking the next sample rather than
// the next frame survived, which reads one channel of one frame and one of the next).
//
// Neither oracle measures a tolerance. Both assert something exact, which is what makes them
// able to see a mutation this small.

TEST(grain_one_whole_grain_over_a_constant_source_traces_the_window) {
    // Over a CONSTANT sample every read returns the same value, so the output is the window
    // itself, scaled: the PCM read, the pitch and the position all cancel out of the shape. DEN 0
    // is the lowest overlap, so grains run one after another rather than on top of each other,
    // and the first silence after frame 0 is therefore the end of grain one -- a whole grain,
    // tail included, with nothing else in it.
    //
    // A window is a Hann curve: it rises STRICTLY to its peak and falls STRICTLY to its end.
    // Not "approximately" and not "mostly" -- every adjacent pair, so a single plateau of two
    // equal samples fails. That is the assertion a staircase cannot satisfy, because a 256-entry
    // table over a grain of 18,910 samples would hold each value for about 74 samples.
    fill_const(g_mono, kSrcFrames, 1, 8000);

    const uint8_t sizes[] = {0, 32, 64, 100, 127};
    const uint8_t shapes[] = {0, 32, 64, 96, 127};
    for (uint8_t siz : sizes) {
        for (uint8_t shp : shapes) {
            Granular g;
            g.init(kRate);
            TrackParams p = plain();
            p.set(1, siz);
            p.set(2, 0);      // DEN: the lowest overlap, so one grain at a time
            p.set(9, shp);
            g.set_sample(g_mono, kSrcFrames, 1, kRate);
            g.prepare(p);
            g.trigger(hit());

            const int blocks = kSrcFrames / kBlk;
            clear(g_out, blocks * kBlk);
            for (int b = 0; b < blocks; b++) {
                g.prepare(p);
                g.render_add(g_out + b * kBlk, kBlk);
            }

            int end = 1;
            while (end < blocks * kBlk && g_out[end] != 0.0f) end++;
            CHECK(end > 300);                       // the shortest grain is 8 ms = 384 frames
            CHECK(end < blocks * kBlk);             // and it ended inside the window measured

            int peak = 0;
            for (int i = 0; i < end; i++) {
                if (g_out[i] > g_out[peak]) peak = i;
            }
            CHECK(peak > 0);
            CHECK(peak < end - 1);                  // a peak, not an edge

            // Except within a few parts per million of the peak, where a Hann curve's slope is
            // below a float's resolution: the longest grain (680 ms) holds two equal samples
            // there however exactly it is computed, and a staircase would show far below it.
            const float flat = g_out[peak] * (1.0f - 4e-6f);
            int plateaus = 0, wrong_way = 0;
            for (int i = 1; i <= peak; i++) {
                if (g_out[i] == g_out[i - 1]) plateaus += g_out[i] < flat ? 1 : 0;
                else if (g_out[i] < g_out[i - 1]) wrong_way++;
            }
            for (int i = peak + 1; i < end; i++) {
                if (g_out[i] == g_out[i - 1]) plateaus += g_out[i] < flat ? 1 : 0;
                else if (g_out[i] > g_out[i - 1]) wrong_way++;
            }
            CHECK_EQ(plateaus, 0);
            CHECK_EQ(wrong_way, 0);

            // The tail runs down to the guard zero rather than stopping on a table entry: the
            // last sample is four orders of magnitude below the peak, and the next is exactly 0.
            CHECK(g_out[end - 1] < g_out[peak] * 1e-3f);
            CHECK_EQ(g_out[end], 0.0f);

            // SHP moves where the peak sits; it does not change how loud the window gets. Every
            // (SIZ, SHP) pair peaks at the same value, which is what "a phase warp, not a second
            // table" means -- the table is always read over its whole span.
            CHECK_NEAR(g_out[peak], 0.244141f, 2e-4);
        }
    }
}

TEST(grain_a_stereo_source_whose_channels_cancel_is_exactly_silent) {
    // GRAIN sums a stereo pair to mono (dsp/grainloops.ti.c: `src[o] + src[o + 1]`), so a source
    // whose right channel is the negation of its left must come out as exact silence -- for any
    // position, any pitch, any grain size. Nothing else about the machine has to be known.
    //
    // It is an oracle for the TAP ALIGNMENT, which a value test cannot reach: the second tap must
    // be the next FRAME, `src[o + 2] + src[o + 3]`. Reading `src[o + 1] + src[o + 2]` instead --
    // the right channel of this frame and the left of the next -- is one index out, sounds almost
    // identical on any real stereo source, and here produces a signal where there must be none.
    for (int i = 0; i < kSrcFrames; i++) {
        const int16_t v = int16_t((i * 7) % 20000 - 10000);
        g_stereo[i * 2] = v;
        g_stereo[i * 2 + 1] = int16_t(-v);
    }

    Granular g;
    g.init(kRate);
    g.set_sample(g_stereo, kSrcFrames, 2, kRate);
    CHECK_EQ(g.sample_channels(), 2);

    const uint8_t positions[] = {0, 40, 127};
    const uint8_t pitches[] = {0, 40, 64, 100, 127};
    const uint8_t sizes[] = {0, 64, 127};
    for (uint8_t pos : positions) {
        for (uint8_t pit : pitches) {
            for (uint8_t siz : sizes) {
                TrackParams p = plain();
                p.set(0, pos);
                p.set(1, siz);
                p.set(2, 100);    // a dense cloud: several grains reading at once
                p.set(8, pit);
                g.kill_all();
                g.prepare(p);
                g.trigger(hit());

                const int blocks = 200;
                clear(g_out, blocks * kBlk);
                for (int b = 0; b < blocks; b++) {
                    g.prepare(p);
                    g.render_add(g_out + b * kBlk, kBlk);
                }
                for (int i = 0; i < blocks * kBlk; i++) {
                    if (g_out[i] != 0.0f) {
                        CHECK_EQ(g_out[i], 0.0f);   // reports the first offending sample
                        break;
                    }
                }
            }
        }
    }

    // And the sweep is not vacuous: the same machine on a source that does NOT cancel sounds.
    fill_const(g_stereo, kSrcFrames, 2, 8000);
    TrackParams p = plain();
    p.set(2, 100);
    g.kill_all();
    g.set_sample(g_stereo, kSrcFrames, 2, kRate);
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, 64 * kBlk);
    for (int b = 0; b < 64; b++) {
        g.prepare(p);
        g.render_add(g_out + b * kBlk, kBlk);
    }
    float loudest = 0.0f;
    for (int i = 0; i < 64 * kBlk; i++) {
        const float a = g_out[i] < 0.0f ? -g_out[i] : g_out[i];
        if (a > loudest) loudest = a;
    }
    CHECK(loudest > 0.05f);
}

// --- gaps a mutation run found (docs/MUTATION.md) -------------------------------------------

TEST(grain_a_new_sample_s_rate_takes_effect_without_a_prepare) {
    // The engine prepares a machine only when its parameters change (docs/BUDGET.md,
    // "Optimization review"), so a sample swapped in under unchanged knobs has to bring its own
    // read step with it.
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.6f);
    Granular g;
    g.init(kRate);
    g.prepare(plain());
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, 24000.0f);
    CHECK_NEAR(g.read_step(), 0.5f, 1e-5);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, 96000.0f);
    CHECK_NEAR(g.read_step(), 2.0f, 1e-5);
}

TEST(grain_before_the_first_prepare_it_plays_the_mid_settings) {
    // What init() promises: a trigger before any prepare() plays every knob at 64, POS included.
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.6f);
    for (int i = kSrcFrames / 2; i < kSrcFrames; i++) g_mono[i] = int16_t(g_mono[i] / 4);
    static Granular a, b;
    a.init(kRate);
    b.init(kRate);
    b.prepare(uniform(64));
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    a.trigger(hit());
    b.trigger(hit());
    clear(g_out, 24 * kBlk);
    clear(g_other, 24 * kBlk);
    for (int k = 0; k < 24; k++) {
        a.render_add(g_out + k * kBlk, kBlk);
        b.render_add(g_other + k * kBlk, kBlk);
    }
    CHECK(peak(g_out, 24 * kBlk) > 0.0f);
    int different = 0;
    for (int i = 0; i < 24 * kBlk; i++) different += g_out[i] != g_other[i];
    CHECK_EQ(different, 0);
}

TEST(grain_accepts_a_sample_of_exactly_the_largest_size) {
    // kMaxFrames is a size it takes, not the first it refuses. Nothing is rendered, so the
    // buffer behind the pointer never has to be that long.
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(Granular::kMaxFrames), 1, kRate);
    g.trigger(hit());
    CHECK(g.active());
    g.set_sample(g_mono, uint32_t(Granular::kMaxFrames) + 1u, 1, kRate);
    g.trigger(hit());
    CHECK(!g.active());
}

TEST(grain_a_retrigger_cancels_a_release_waiting_for_the_last_one) {
    // A key up before its note has started marks that note to release on start; a new trig
    // that takes the voice is a new note, and holds.
    fill_sine(g_mono, kSrcFrames, 1, 440.0f, kRate, 0.6f);
    static Granular once, twice;
    const TrackParams p = plain();
    Granular *const both[] = {&once, &twice};
    for (Granular *g : both) {
        g->init(kRate);
        g->set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        g->prepare(p);
    }
    once.trigger(hit(Granular::kRootNote, 127, 200));
    twice.trigger(hit(Granular::kRootNote, 127, 200));
    twice.release(Granular::kRootNote);
    twice.trigger(hit(Granular::kRootNote, 127, 200));
    clear(g_out, kCapture);
    clear(g_other, kCapture);
    const int blocks = kCapture / kBlk;
    render_blocks(once, p, g_out, blocks);
    render_blocks(twice, p, g_other, blocks);
    CHECK(peak(g_out + kCapture - 4800, 4800) > 0.05f);
    int different = 0;
    for (int i = 0; i < kCapture; i++) different += g_out[i] != g_other[i];
    CHECK_EQ(different, 0);
}

TEST(grain_a_spawn_takes_a_free_grain_and_steals_only_the_one_furthest_through) {
    // Watched one frame at a time, every birth is checked against the pool just before it: with
    // a grain free, a free one is taken and nothing is cut; with all eight busy, the grain
    // replaced is the one furthest through its window (its quietest point), and the newcomer
    // takes the full-pool crowd gain.
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.8f);
    static Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 127);    // SIZ: the longest grain
    p.set(2, 127);    // DEN: the deepest overlap, so the pool fills
    g.prepare(p);
    g.trigger(hit());

    bool live[Granular::kGrains];
    int left[Granular::kGrains];
    float q[Granular::kGrains];
    int free_births = 0, steals = 0, wrong = 0;
    static float frame_out[1];
    for (int f = 0; f < 3 * int(ENAGA_SAMPLE_RATE); f++) {
        int count = 0;
        float furthest = -1.0f;
        for (int i = 0; i < Granular::kGrains; i++) {
            live[i] = g.grain_live(i);
            left[i] = g.grain_remaining(i);
            q[i] = g.grain_state(i).q;
            count += live[i] ? 1 : 0;
            if (live[i] && q[i] > furthest) furthest = q[i];
        }
        frame_out[0] = 0.0f;
        g.render_add(frame_out, 1);
        for (int i = 0; i < Granular::kGrains; i++) {
            const bool born = g.grain_live(i) && (!live[i] || g.grain_remaining(i) > left[i]);
            if (!born) continue;
            if (count < Granular::kGrains) {
                free_births++;
                wrong += live[i] ? 1 : 0;
            } else {
                steals++;
                wrong += q[i] == furthest ? 0 : 1;
                wrong += g.grain_state(i).gain == Granular::kCrowdGain[Granular::kGrains - 1] ? 0
                                                                                           : 1;
            }
        }
    }
    CHECK(free_births >= Granular::kGrains);
    CHECK(steals > 10);
    CHECK_EQ(wrong, 0);
}

// --- the CPU guard's pool (set_grain_limit, core/cpuguard.hpp) --------------------------------

TEST(grain_the_guard_limit_thins_the_cloud_and_never_steals) {
    // The deepest overlap: the full pool is in the air. Lowered to three, no new grain is born
    // until the count is under three, and none already in the air is taken over -- every live
    // grain's remaining count only ever goes down, which a steal (a reset to a fresh window)
    // would break.
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.8f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 127);
    p.set(2, 127);
    g.prepare(p);
    g.trigger(hit());
    clear(g_out, kCapture);
    for (int b = 0; b < 200; b++) g.render_add(g_out, kBlk);
    CHECK(g.live_grains() > 3);   // a cloud well past the limit about to be set

    g.set_grain_limit(3);
    CHECK_EQ(g.grain_limit(), 3);
    int left[Granular::kGrains];
    bool live[Granular::kGrains];
    for (int i = 0; i < Granular::kGrains; i++) {
        left[i] = g.grain_remaining(i);
        live[i] = g.grain_live(i);
    }
    int previous = g.live_grains();
    bool reached = false;
    for (int b = 0; b < 2000; b++) {
        g.render_add(g_out, kBlk);
        const int now = g.live_grains();
        // Never more than before until the count has come down to the limit.
        if (!reached) CHECK(now <= previous);
        if (now <= 3) reached = true;
        if (reached) CHECK(now <= 3);
        for (int i = 0; i < Granular::kGrains; i++) {
            // A grain that was live when the limit fell keeps counting down until it ends: none
            // of those is stolen. (One that ended may later be reborn in the same slot.)
            if (live[i] && g.grain_live(i)) {
                CHECK(g.grain_remaining(i) < left[i]);
                left[i] = g.grain_remaining(i);
            } else {
                live[i] = false;
            }
        }
        previous = now;
    }
    CHECK(reached);
    // And the cloud still sounds: thinned, not stopped.
    clear(g_out, kCapture);
    for (int b = 0; b < 100; b++) g.render_add(g_out + b * kBlk, kBlk);
    CHECK(g.live_grains() >= 1);
    CHECK(finite_within(g_out, 100 * kBlk, 3.0f));
    float energy = 0.0f;
    for (int i = 0; i < 100 * kBlk; i++) energy += g_out[i] * g_out[i];
    CHECK(energy > 1.0f);
}

TEST(grain_the_guard_limit_at_the_full_pool_changes_nothing) {
    // set_grain_limit(kGrains) is level 0: bit-exact against a machine never told anything, under
    // the steal-heavy pattern of grain_live_grains_never_exceed_the_pool.
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.8f);
    Granular a;
    Granular b;
    a.init(kRate);
    b.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    b.set_grain_limit(2);
    b.set_grain_limit(Granular::kGrains);
    TrackParams p = plain();
    p.set(1, 127);
    p.set(2, 127);
    p.set(3, 127);
    a.prepare(p);
    b.prepare(p);
    clear(g_out, kCapture);
    clear(g_other, kCapture);
    for (int k = 0; k < 400; k++) {
        a.trigger(hit(uint8_t(48 + (k % 25))));
        b.trigger(hit(uint8_t(48 + (k % 25))));
        a.render_add(g_out + (k % 100) * kBlk, kBlk);
        b.render_add(g_other + (k % 100) * kBlk, kBlk);
    }
    CHECK(__builtin_memcmp(g_out, g_other, sizeof(float) * 100 * kBlk) == 0);
}

TEST(grain_the_guard_limit_is_clamped_to_the_pool) {
    Granular g;
    g.init(kRate);
    g.set_grain_limit(0);
    CHECK_EQ(g.grain_limit(), 1);
    g.set_grain_limit(-5);
    CHECK_EQ(g.grain_limit(), 1);
    g.set_grain_limit(99);
    CHECK_EQ(g.grain_limit(), Granular::kGrains);
}

// --- the ASR and OSC pages (dsp/grain.hpp, "the GRN pages beyond SRC and AMP") -----------------

namespace {

Trig gated(uint32_t gate_frames, bool held = false) {
    Trig t = hit();
    t.gate_frames = gate_frames;
    t.held = held;
    return t;
}

}  // namespace

TEST(grain_sus_zero_ignores_len_as_it_always_did) {
    // A kit from before GRAIN read the ASR page has zeros there: DEC at the top still holds the
    // cloud past a step's LEN, until the next trig or STOP.
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    g.prepare(p);
    CHECK(!g.sustains());
    g.trigger(gated(4800));                         // a 100 ms LEN
    clear(g_out, kCapture);
    const int n = render_blocks(g, p, g_out, 700);
    CHECK(g.active());
    CHECK(peak(g_out + n - 4 * kBlk, 4 * kBlk) > 0.05f);
}

TEST(grain_sus_up_holds_while_len_lasts_and_then_releases_over_rel) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(10, 40);                                  // DEC 90 ms: the fall to SUS
    p.set(kAsrFirst + 1, 64);                       // SUS about half
    g.prepare(p);
    CHECK(g.sustains());
    g.trigger(gated(24000));                        // a half-second LEN
    clear(g_out, kCapture);
    render_blocks(g, p, g_out, 360);                // 0.48 s: inside LEN, long after DEC
    // Holding at SUS, well past where DEC alone would have faded it out.
    CHECK(g.active());
    CHECK(peak(g_out + 300 * kBlk, 60 * kBlk) > 0.05f);
    // After LEN, REL 0 is the old 180 ms tail: silent and finished within a second.
    render_blocks(g, p, g_out, 700);
    CHECK(!g.active());
}

TEST(grain_sus_up_a_held_key_holds_until_it_comes_up) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(kAsrFirst + 1, 127);
    p.set(kAsrFirst + 2, 30);                       // REL about 470 ms
    g.trigger(gated(0, true));
    clear(g_out, kCapture);
    render_blocks(g, p, g_out, 1500);               // two seconds held
    CHECK(g.active());
    CHECK(peak(g_out + 1400 * kBlk, 100 * kBlk) > 0.05f);
    g.release(Granular::kRootNote);
    clear(g_out, kCapture);
    render_blocks(g, p, g_out, 1500);
    CHECK(!g.active());
    CHECK(all_zero(g_out + 1400 * kBlk, 100 * kBlk));
}

TEST(grain_sus_up_a_one_shot_still_ends_by_itself) {
    // No LEN and no key-up to come: with SUS up nothing else would ever end it.
    fill_const(g_mono, kSrcFrames, 1, 20000);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(kAsrFirst + 1, 127);
    g.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(g, p, g_out, 1500);
    CHECK(!g.active());
}

TEST(grain_attack_rises_and_release_knob_lengthens_the_tail) {
    fill_const(g_mono, kSrcFrames, 1, 20000);
    // ATK: the first 50 ms are far quieter than the cloud at a second.
    Granular a;
    a.init(kRate);
    a.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(kAsrFirst + 0, 80);                       // about 300 ms
    a.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(a, p, g_out, 750);
    CHECK(peak(g_out, 37 * kBlk) < 0.5f * peak(g_out + 700 * kBlk, 50 * kBlk));
    CHECK_EQ(grain_attack_seconds(0), 0.0f);
    CHECK_NEAR(grain_attack_seconds(127), 5.0f, 0.05f);
    // REL: 0 is the old 180 ms, the top 10 s, and it only ever climbs.
    CHECK_NEAR(grain_release_seconds(0), 0.18f, 1e-4f);
    CHECK_NEAR(grain_release_seconds(127), 10.0f, 0.1f);
    for (int v = 1; v <= 127; v++) {
        CHECK(grain_release_seconds(uint8_t(v)) > grain_release_seconds(uint8_t(v - 1)));
    }
}

TEST(grain_scan_moves_the_read_head_at_the_knobs_rate) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(kOscFirst + 0, 64);                       // about half real time
    g.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(g, p, g_out, 375);                // half a second
    const float expected = grain_scan_rate(64) * 375.0f * float(kBlk);
    CHECK_NEAR(float(g.scan_offset()), expected, expected * 0.01f);
    CHECK_NEAR(grain_scan_rate(90), 1.0f, 0.01f);   // real time at 90
    CHECK_EQ(grain_scan_rate(0), 0.0f);
    // It wraps inside the sample, and every trig starts it from POS again.
    render_blocks(g, p, g_out, 1500);
    CHECK(g.scan_offset() >= 0);
    CHECK(g.scan_offset() < kSrcFrames);
    g.trigger(hit());
    render_blocks(g, p, g_out, 1);
    CHECK(g.scan_offset() < kBlk);
}

TEST(grain_scan_reaches_a_part_of_the_sample_pos_alone_does_not) {
    // Silence everywhere but the second half; POS 0 with SCN off never gets there.
    fill_band(g_mono, kSrcFrames, 1, kSrcFrames / 2, kSrcFrames, 500.0f, kRate);
    for (int scan = 0; scan < 2; scan++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(kOscFirst + 0, scan ? 90 : 0);
        g.trigger(hit());
        clear(g_out, kCapture);
        const int n = render_blocks(g, p, g_out, 1400);
        CHECK_EQ(peak(g_out, n) > 0.05f, scan == 1);
    }
}

TEST(grain_pitch_spray_octave_sends_its_share_of_grains_up_an_octave) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    const uint8_t knobs[3] = {0, 96, 127};
    const float shares[3] = {0.0f, 0.258f, 0.5f};
    for (int k = 0; k < 3; k++) {
        Granular g;
        g.init(kRate);
        g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
        TrackParams p = plain();
        p.set(1, 20);                               // short grains: many onsets to count
        p.set(2, 100);
        p.set(kOscFirst + 1, knobs[k]);
        g.trigger(hit());
        // Count each grain once, at the block after its onset (its remaining length is then
        // within a block of the whole).
        int up = 0, all = 0;
        for (int b = 0; b < 1500; b++) {
            g.prepare(p);
            clear(g_out, kBlk);
            g.render_add(g_out, kBlk);
            for (int i = 0; i < Granular::kGrains; i++) {
                if (!g.grain_live(i)) continue;
                if (g.grain_remaining(i) <= g.grain_length() - kBlk) continue;
                const strata_grain &s = g.grain_state(i);
                CHECK(s.istep == 1 || s.istep == 2);
                CHECK_EQ(s.fstep, 0u);              // exactly unity or exactly an octave
                up += s.istep == 2 ? 1 : 0;
                all++;
            }
        }
        CHECK(all > 200);
        CHECK_NEAR(float(up) / float(all), shares[k], 0.03f);
    }
    CHECK_NEAR(grain_octave_chance(127), 0.5f, 1e-6f);
    CHECK(grain_pitch_spray_mode(0) == GrainSpray::off);
    CHECK(grain_pitch_spray_mode(63) == GrainSpray::detune);
    CHECK(grain_pitch_spray_mode(64) == GrainSpray::octave);
}

TEST(grain_pitch_spray_detune_stays_inside_its_width) {
    fill_sine(g_mono, kSrcFrames, 1, 300.0f, kRate, 0.5f);
    Granular g;
    g.init(kRate);
    g.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 20);
    p.set(2, 100);
    p.set(kOscFirst + 1, 63);                       // a semitone either way
    CHECK_NEAR(grain_detune_cents(63), 100.0f, 1e-3f);
    g.trigger(hit());
    float lo = 2.0f, hi = 0.0f;
    for (int b = 0; b < 1000; b++) {
        g.prepare(p);
        clear(g_out, kBlk);
        g.render_add(g_out, kBlk);
        for (int i = 0; i < Granular::kGrains; i++) {
            if (!g.grain_live(i)) continue;
            const strata_grain &s = g.grain_state(i);
            const float step = float(s.istep) + float(s.fstep) * (1.0f / 65536.0f);
            lo = step < lo ? step : lo;
            hi = step > hi ? step : hi;
        }
    }
    CHECK(lo >= 0.9438f - 1e-3f);                   // 2^(-1/12)
    CHECK(hi <= 1.0595f + 1e-3f);                   // 2^(+1/12)
    CHECK(lo < 0.97f);                              // and it does spread
    CHECK(hi > 1.03f);
}
