// SAMPLE: the one-shot and slice player in dsp/sample.cpp.
//
// The PCM is synthesised here -- a sine for pitch and interpolation quality, a ramp for the
// window and the direction, a constant for voice counting and levels -- so that every property
// is measurable without a fixture file. Nothing in these tests touches the card, because the
// machine never loads anything: `set_sample` is the whole input side.
//
// Two things get pinned deliberately rather than just exercised. The Catmull-Rom interpolator is
// measured against nearest-neighbour on the same material, so that a later tidy-up to a cheaper
// interpolator fails here instead of being noticed on the unit. And every documented edge -- no
// sample, a sample taken away mid-note, a window shorter than the four taps, a window pressed
// against both ends of the buffer -- is checked for silence and finiteness, because an
// out-of-bounds read on the device does not fault, it silently returns garbage.
#include "check.hpp"

#include <cstdlib>
#include <cstring>

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "sample.hpp"
#include "sampleloops.h"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

// One second of source and two of output, static because that does not belong on a stack frame.
constexpr int kSrcFrames = ENAGA_SAMPLE_RATE;
constexpr int kCapture = ENAGA_SAMPLE_RATE * 2;

int16_t g_mono[kSrcFrames];
int16_t g_stereo[kSrcFrames * 2];
int16_t g_short[64];
float g_out[kCapture];
float g_other[kCapture];

// A tiny sample with full-scale guard regions either side of it in the SAME array, so that a
// read past either end shows up as a level no interpolation of the sample itself could produce.
constexpr int kGuard = 8;
constexpr int kTinyFrames = 64;
int16_t g_guarded[kGuard + kTinyFrames + kGuard];
int16_t *const g_tiny = g_guarded + kGuard;

// --- parameter helpers ----------------------------------------------------
//
// PIT 64 is unity, STA 0 is the start, LEN 127 is "to the end", SLC 0 is one slice, ATK 0 is no
// attack at all, DEC 127 is hold (the sound ends when the sample does), PLY 0 is forward
// one-shot and LVL 127 is unity. So this is the set that plays a sample exactly as it is.
TrackParams plain() {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, 0);
    p.set(0, 64);     // PIT: unity
    p.set(2, 127);    // LEN: to the end
    p.set(9, 127);    // DEC: hold
    p.set(11, 127);   // LVL: unity
    return p;
}

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

Trig hit(uint8_t note = Sampler::kRootNote, uint8_t velocity = 127, uint32_t frame = 0) {
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

// A ramp over the whole buffer: played back it says which frames were read and in which
// direction, which is what the window, slice and reverse tests need.
void fill_ramp(int16_t *dst, int frames, int channels, int16_t from, int16_t to) {
    const float span = float(to - from) * enaga::dsp::reciprocal(float(frames - 1));
    for (int i = 0; i < frames; i++) {
        const int16_t v = int16_t(float(from) + span * float(i) + 0.5f);
        for (int c = 0; c < channels; c++) dst[i * channels + c] = v;
    }
}

void fill_const(int16_t *dst, int frames, int channels, int16_t value) {
    for (int i = 0; i < frames * channels; i++) dst[i] = value;
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

int sign_changes(const float *x, int n) {
    int count = 0;
    for (int i = 1; i < n; i++) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) count++;
    }
    return count;
}

// Zero crossings over a window, as a frequency in Hz. Two crossings per cycle.
float measured_hz(const float *x, int from, int n) {
    return 0.5f * float(sign_changes(x + from, n)) * kRate * enaga::dsp::reciprocal(float(n));
}

int first_nonzero(const float *x, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != 0.0f) return i;
    }
    return -1;
}

// The largest sample count not above `span` that holds a whole number of cycles of `hz`, so that
// the single-bin fit below has no leakage to speak of and the residual it leaves is distortion.
int whole_cycles(int span, float hz) {
    const float cycles = float(span) * hz * enaga::dsp::reciprocal(kRate);
    const int whole = int(cycles);
    const int n = int(float(whole) * kRate * enaga::dsp::reciprocal(hz));
    return n > span ? span : n;
}

// What is left after the best-fitting sinusoid at `hz` is removed, as a fraction of the signal's
// own RMS: a THD+N figure for something that should be one pure tone. One DFT bin by
// correlation, so no FFT and no libm.
float residual_fraction(const float *x, int n, float hz) {
    const float dt = hz * enaga::dsp::reciprocal(kRate);
    float sum_sin = 0.0f, sum_cos = 0.0f, total = 0.0f;
    for (int i = 0; i < n; i++) {
        const float phase = dt * float(i);
        sum_sin += x[i] * enaga::dsp::sin_turns(phase);
        sum_cos += x[i] * enaga::dsp::cos_turns(phase);
        total += x[i] * x[i];
    }
    const float inv = 2.0f * enaga::dsp::reciprocal(float(n));
    const float a = sum_sin * inv, b = sum_cos * inv;
    float residual = 0.0f;
    for (int i = 0; i < n; i++) {
        const float phase = dt * float(i);
        const float fitted = a * enaga::dsp::sin_turns(phase) + b * enaga::dsp::cos_turns(phase);
        const float e = x[i] - fitted;
        residual += e * e;
    }
    if (total <= 0.0f) return 1.0f;
    // A square root without libm: exp2(0.5 * log2(x)), as docs/ENAGA_API.md suggests.
    return enaga::dsp::exp2(0.5f * enaga::dsp::log2(residual * enaga::dsp::reciprocal(total)));
}

// Nearest-neighbour and linear playback of the same PCM at the same rate: what 4-point
// interpolation has to beat. Deliberately naive in the interpolation, but NOT in the position:
// the read position is double and computed as i * step rather than accumulated, because a float
// accumulator drifts by a fraction of a sample over a few thousand frames and that drift alone
// measures as distortion -- it would flatter the machine instead of testing it. (The machine
// itself does not have that problem: its position is exact fixed point, which is the reason for
// the fixed point as much as the missing integer divide is.)
void play_nearest(const int16_t *src, int frames, double step, float *dst, int n) {
    for (int i = 0; i < n; i++) {
        int index = int(step * double(i) + 0.5);
        if (index > frames - 1) index = frames - 1;
        dst[i] = float(src[index]) * (1.0f / 32768.0f);
    }
}

void play_linear(const int16_t *src, int frames, double step, float *dst, int n) {
    for (int i = 0; i < n; i++) {
        const double pos = step * double(i);
        int a = int(pos);
        if (a > frames - 2) a = frames - 2;
        const float t = float(pos - double(a));
        const float x0 = float(src[a]), x1 = float(src[a + 1]);
        dst[i] = (x0 + (x1 - x0) * t) * (1.0f / 32768.0f);
    }
}

}  // namespace

// --- nothing to play -----------------------------------------------------

TEST(sample_without_pcm_is_silent) {
    Sampler s;
    s.init(kRate);
    CHECK(!s.has_sample());
    CHECK(!s.active());

    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    // Still not active: a trigger with nothing resident is a no-op, not a voice playing zeros.
    CHECK(!s.active());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 64);
    CHECK(all_zero(g_out, n));
    CHECK(!s.active());
    CHECK_EQ(s.playing_voices(), 0);
}

TEST(sample_null_pointer_with_frames_is_refused) {
    Sampler s;
    s.init(kRate);
    s.set_sample(nullptr, 48000, 1, kRate);
    CHECK(!s.has_sample());
    CHECK_EQ(s.sample_frames(), 0);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 16);
    CHECK(all_zero(g_out, n));
    CHECK(!s.active());
}

TEST(sample_removed_mid_note_goes_silent) {
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());

    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 8);
    CHECK(peak(g_out, 8 * kBlk) > 0.4f);     // it was sounding
    CHECK(s.active());

    // The loader's buffer is taken away between blocks. The spec's answer is silence, not a
    // glitch, and above all not a read into a buffer that has been handed back.
    s.set_sample(nullptr, 0, 1, kRate);
    CHECK(!s.active());
    CHECK_EQ(s.playing_voices(), 0);
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 16);
    CHECK(all_zero(g_out, n));
    CHECK(!s.active());

    // And triggering again while there is nothing resident stays silent.
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n2 = render_blocks(s, p, g_out, 8);
    CHECK(all_zero(g_out, n2));
    CHECK(!s.active());
}

// --- a note plays and ends ------------------------------------------------

TEST(sample_one_shot_plays_then_ends) {
    const int frames = 4800;                 // 100 ms
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    CHECK(s.has_sample());
    CHECK_EQ(s.sample_frames(), frames);
    CHECK_EQ(s.sample_channels(), 1);

    const TrackParams p = plain();
    s.prepare(p);
    CHECK_EQ(s.window_start(), 0);
    CHECK_EQ(s.window_length(), frames);
    CHECK_EQ(s.slices(), 1);
    CHECK_EQ(s.play_length(), frames);

    s.trigger(hit());
    CHECK(s.active());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 150);     // 9600 frames, twice the sample
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK_NEAR(peak(g_out, n), 0.5, 0.01);             // 16384 of 32768 at unity
    // It stops when the sample stops, and the rest of the capture is untouched.
    CHECK(all_zero(g_out + frames + kBlk, n - frames - kBlk));
    CHECK(!s.active());
    CHECK_EQ(s.playing_voices(), 0);
}

TEST(sample_decay_shortens_a_hit) {
    const int frames = 24000;                // 500 ms
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(9, 20);                            // DEC well below 127: decay, not hold
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 400);
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK(peak(g_out, kBlk) > 0.1f);
    CHECK(!s.active());                      // finished well before the sample ran out
    CHECK(peak(g_out, 2000) > 10.0f * peak(g_out + 12000, 2000));
}

// --- pitch ----------------------------------------------------------------

TEST(sample_root_note_reproduces_the_source_frequency) {
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, kRate, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit(Sampler::kRootNote));
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);
    // Measured away from the start of the note, over a whole number of blocks.
    CHECK_NEAR(measured_hz(g_out, 2048, 16384), 500.0, 2.0);
}

TEST(sample_octave_up_doubles_the_frequency) {
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, kRate, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit(Sampler::kRootNote + 12));
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);
    CHECK_NEAR(measured_hz(g_out, 2048, 16384), 1000.0, 4.0);

    // And an octave down halves it.
    Sampler d;
    d.init(kRate);
    d.set_sample(g_mono, kSrcFrames, 1, kRate);
    d.prepare(p);
    d.trigger(hit(Sampler::kRootNote - 12));
    clear(g_other, kCapture);
    render_blocks(d, p, g_other, 320);
    CHECK_NEAR(measured_hz(g_other, 2048, 16384), 250.0, 2.0);
}

TEST(sample_pitch_knob_transposes_without_a_note) {
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, kRate, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    TrackParams p = plain();
    p.set(0, 64 + 31);                       // +31 * 24/63 semitones, just under an octave
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);
    const float expected = 500.0f * enaga::dsp::semitones(31.0f * (24.0f / 63.0f));
    CHECK_NEAR(measured_hz(g_out, 2048, 16384), expected, 6.0);
}

TEST(sample_source_rate_below_48k_plays_at_the_right_pitch) {
    // A 24 kHz sample holding a 500 Hz tone: 48 samples per cycle. Read at 48 kHz it must still
    // come out at 500 Hz, which means stepping at 0.5 -- the whole point of source_rate.
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, 24000.0f, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, 24000.0f);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);
    CHECK_NEAR(measured_hz(g_out, 2048, 16384), 500.0, 2.0);

    // Ignoring source_rate would give 1000 Hz, so pin that that is not what happens.
    Sampler ignored;
    ignored.init(kRate);
    ignored.set_sample(g_mono, kSrcFrames, 1, kRate);
    ignored.prepare(p);
    ignored.trigger(hit());
    clear(g_other, kCapture);
    render_blocks(ignored, p, g_other, 320);
    CHECK_NEAR(measured_hz(g_other, 2048, 16384), 1000.0, 4.0);
}

TEST(sample_source_rate_above_48k_plays_at_the_right_pitch) {
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, 96000.0f, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, 96000.0f);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);
    CHECK_NEAR(measured_hz(g_out, 2048, 16384), 500.0, 3.0);
}

// --- interpolation quality -----------------------------------------------

TEST(sample_four_point_interpolation_beats_nearest_neighbour) {
    // A pure 1 kHz tone resampled at a step that is not a simple fraction, so almost no output
    // sample lands on a source sample. Whatever is left after the fundamental is removed is
    // resampling distortion.
    const float hz = 1000.0f;
    fill_sine(g_mono, kSrcFrames, 1, hz, kRate, 0.8f);

    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    TrackParams p = plain();
    p.set(0, 64 + 7);                        // +2.667 semitones: a step of about 1.1665
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 320);

    const float step = enaga::dsp::semitones(7.0f * (24.0f / 63.0f));
    const float out_hz = hz * step;
    const int from = 4096;
    const int span = whole_cycles(8192, out_hz);
    const float hermite = residual_fraction(g_out + from, span, out_hz);

    clear(g_other, kCapture);
    play_nearest(g_mono, kSrcFrames, double(step), g_other, from + span + 16);
    const float nearest = residual_fraction(g_other + from, span, out_hz);

    clear(g_other, kCapture);
    play_linear(g_mono, kSrcFrames, double(step), g_other, from + span + 16);
    const float linear = residual_fraction(g_other + from, span, out_hz);

    // Measured: nearest 3.78e-2, linear 6.43e-4, Catmull-Rom 7.31e-5. So the cubic is 517x
    // better than nearest-neighbour and 8.8x better than linear on this material, and the bounds
    // below are those figures with at least a 2x margin. They are deliberately tight enough that
    // dropping to linear interpolation -- let alone nearest -- fails this test instead of
    // passing it quietly.
    //
    // 8.8x rather than hundreds because the comparison has a floor: the source is int16, so its
    // own quantisation noise measures about 1.6e-5 here and both figures sit on top of it. The
    // separation is also frequency-dependent (linear's error grows as f^2 and the cubic's as
    // f^4), so 1 kHz is close to where this measure discriminates best.
    CHECK(nearest > 0.01f);
    CHECK(linear > 0.0003f);
    CHECK(hermite < 0.0002f);                // about -82 dB
    CHECK(hermite * 100.0f < nearest);
    CHECK(hermite * 4.0f < linear);
    // And no gross distortion in absolute terms: the tone is still a tone at its own level.
    CHECK_NEAR(peak(g_out + from, span), 0.8, 0.02);
}

TEST(sample_interpolation_is_exact_on_whole_frames) {
    // At unity step every output sample is a source sample, and Catmull-Rom at t = 0 returns the
    // base tap exactly. So playback at the root note is faithful, not merely close.
    fill_sine(g_mono, kSrcFrames, 1, 777.0f, kRate, 0.9f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    TrackParams p = plain();
    p.set(1, 32);                            // START away from the buffer edge: the fast path
    s.prepare(p);
    const int start = s.window_start();
    CHECK(start > 4);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 64);
    for (int i = 0; i < 2048; i++) {
        CHECK_NEAR(g_out[i], float(g_mono[start + i]) * (1.0f / 32768.0f), 1e-6);
    }
}

// --- the window -----------------------------------------------------------

TEST(sample_start_windows_the_sample) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(1, 64);                            // START halfway
    s.prepare(p);
    const int start = s.window_start();
    CHECK_NEAR(float(start), 0.50394f * float(frames - 1), 2.0);
    CHECK_EQ(s.window_length(), frames - start);

    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 128);
    CHECK(finite_within(g_out, n, 4.0f));
    // The ramp is entered halfway up, so nothing below that value ever appears while it plays,
    // and the top of the output is the end of the ramp.
    const float entry = float(g_mono[start]) * (1.0f / 32768.0f);
    CHECK_NEAR(g_out[0], entry, 0.005);
    CHECK_NEAR(peak(g_out, n), 32000.0 / 32768.0, 0.01);
    for (int i = 0; i < s.window_length(); i++) CHECK(g_out[i] >= entry - 0.005f);
    CHECK(all_zero(g_out + s.window_length() + kBlk, n - s.window_length() - kBlk));
}

TEST(sample_length_windows_the_sample) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(2, 32);                            // LEN about a quarter
    s.prepare(p);
    const int len = s.window_length();
    CHECK_NEAR(float(len), 0.2520f * float(frames), 2.0);

    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 200);
    CHECK(finite_within(g_out, n, 4.0f));
    // Only the first quarter of the ramp was read, so the peak is a quarter of the way up.
    CHECK_NEAR(peak(g_out, n), float(g_mono[len - 1]) * (1.0f / 32768.0f), 0.02);
    CHECK(all_zero(g_out + len + kBlk, n - len - kBlk));
}

TEST(sample_loop_repeats_the_window_and_one_shot_does_not) {
    const int frames = 4096;
    fill_ramp(g_mono, frames, 1, 1000, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams looped = plain();
    looped.set(10, 32 + 4);                  // PLY: forward loop
    s.prepare(looped);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, looped, g_out, 400);     // 25600 frames, six passes
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK(s.active());                       // a loop does not end
    // Each pass of the ramp ends high and restarts low, so the sawtooth repeats at the window
    // length: four resets inside five windows' worth of output.
    int resets = 0;
    for (int i = 1; i < 5 * frames; i++) {
        if (g_out[i] < g_out[i - 1] - 0.5f) resets++;
    }
    CHECK_EQ(resets, 4);
    CHECK_NEAR(g_out[frames + 4], g_out[4], 0.01);
    s.kill_all();

    // The same sample as a one-shot plays the ramp once and stops.
    Sampler once;
    once.init(kRate);
    once.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    once.prepare(p);
    once.trigger(hit());
    clear(g_other, kCapture);
    const int n2 = render_blocks(once, p, g_other, 400);
    CHECK(!once.active());
    int resets2 = 0;
    for (int i = 1; i < n2; i++) {
        if (g_other[i] < g_other[i - 1] - 0.5f) resets2++;
    }
    CHECK_EQ(resets2, 1);                    // only the drop to silence at the end
}

TEST(sample_reverse_plays_the_window_backwards) {
    const int frames = 4096;
    fill_ramp(g_mono, frames, 1, 1000, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(10, 64 + 4);                       // PLY: reverse one-shot
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 100);
    CHECK(finite_within(g_out, n, 4.0f));

    // An ascending ramp comes out descending: it starts at the top and falls, monotonically.
    CHECK_NEAR(g_out[0], 32000.0 / 32768.0, 0.005);
    for (int i = 1; i < frames; i++) CHECK(g_out[i] <= g_out[i - 1] + 1e-4f);
    CHECK(g_out[frames - 64] < 0.1f);
    CHECK(all_zero(g_out + frames + kBlk, n - frames - kBlk));
    CHECK(!s.active());
}

TEST(sample_reverse_loop_repeats_backwards) {
    const int frames = 4096;
    fill_ramp(g_mono, frames, 1, 1000, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(10, 96 + 4);                       // PLY: reverse loop
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 300);
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK(s.active());
    // Descending, so the seams are jumps upwards, one per window.
    int jumps = 0;
    for (int i = 1; i < 4 * frames; i++) {
        if (g_out[i] > g_out[i - 1] + 0.5f) jumps++;
    }
    CHECK_EQ(jumps, 3);
    s.kill_all();
}

// --- slices ---------------------------------------------------------------

TEST(sample_slices_start_at_successive_offsets) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);

    TrackParams p = plain();
    p.set(3, 127);                           // SLC: 16 slices
    s.prepare(p);
    CHECK_EQ(s.slices(), 16);
    CHECK_EQ(s.play_length(), frames / 16);

    // Note 60 is slice 0, 61 is slice 1, and so on: each enters at its own height up the ramp,
    // which is what "successive notes start at successive offsets" means on this material.
    float entry[16];
    for (int slice = 0; slice < 16; slice++) {
        Sampler one;
        one.init(kRate);
        one.set_sample(g_mono, uint32_t(frames), 1, kRate);
        one.prepare(p);
        one.trigger(hit(uint8_t(Sampler::kRootNote + slice)));
        clear(g_out, kCapture);
        const int n = render_blocks(one, p, g_out, 16);
        CHECK(finite_within(g_out, n, 4.0f));
        entry[slice] = g_out[0];
        CHECK_NEAR(entry[slice], float(g_mono[slice * (frames / 16)]) * (1.0f / 32768.0f), 0.005);
        // One slice plays one sixteenth of the window and then stops.
        CHECK(!one.active());
        CHECK(all_zero(g_out + s.play_length() + kBlk, n - s.play_length() - kBlk));
    }
    for (int slice = 1; slice < 16; slice++) CHECK(entry[slice] > entry[slice - 1]);

    // Notes past the last slice hold on it rather than wrapping or reading past the sample.
    Sampler far;
    far.init(kRate);
    far.set_sample(g_mono, uint32_t(frames), 1, kRate);
    far.prepare(p);
    far.trigger(hit(127));
    clear(g_out, kCapture);
    const int n = render_blocks(far, p, g_out, 16);
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK_NEAR(g_out[0], entry[15], 0.005);

    // And a note below the root holds on slice 0.
    Sampler low;
    low.init(kRate);
    low.set_sample(g_mono, uint32_t(frames), 1, kRate);
    low.prepare(p);
    low.trigger(hit(0));
    clear(g_other, kCapture);
    render_blocks(low, p, g_other, 16);
    CHECK_NEAR(g_other[0], entry[0], 0.005);
}

TEST(sample_slices_ignore_the_note_for_pitch) {
    // In slice mode the note picks the slice, so it must NOT also transpose: the same tone comes
    // out of every slice at the same frequency.
    fill_sine(g_mono, kSrcFrames, 1, 500.0f, kRate, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, kSrcFrames, 1, kRate);
    TrackParams p = plain();
    p.set(3, 8);                             // SLC: 2 slices
    s.prepare(p);
    CHECK_EQ(s.slices(), 2);
    s.trigger(hit(uint8_t(Sampler::kRootNote + 1)));
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 200);
    CHECK_NEAR(measured_hz(g_out, 2048, 8192), 500.0, 3.0);
}

TEST(sample_slice_loop_stays_inside_the_slice) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(3, 25);                            // SLC: 4 slices
    p.set(10, 32);                           // PLY: forward loop
    s.prepare(p);
    CHECK_EQ(s.slices(), 4);
    const int slice_len = s.play_length();
    CHECK_EQ(slice_len, frames / 4);
    s.trigger(hit(uint8_t(Sampler::kRootNote + 1)));
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 200);
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK(s.active());
    // It never leaves the second quarter of the ramp, and it repeats at the slice length.
    const float lo = float(g_mono[slice_len]) * (1.0f / 32768.0f);
    const float hi = float(g_mono[2 * slice_len]) * (1.0f / 32768.0f);
    for (int i = 0; i < n; i++) {
        CHECK(g_out[i] > lo - 0.005f);
        CHECK(g_out[i] < hi + 0.005f);
    }
    CHECK_NEAR(g_out[slice_len + 4], g_out[4], 0.005);
    s.kill_all();
}

// --- edges ----------------------------------------------------------------

TEST(sample_start_at_the_very_end_is_silent) {
    const int frames = 4096;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 127);                           // START on the last frame: a 1-frame window
    s.prepare(p);
    CHECK_EQ(s.window_start(), frames - 1);
    CHECK(s.play_length() < Sampler::kMinFrames);
    s.trigger(hit());
    CHECK(!s.active());                      // refused, so not even one block of noise
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 16);
    CHECK(all_zero(g_out, n));
}

TEST(sample_zero_length_is_silent) {
    const int frames = 4096;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(2, 0);                             // LEN 0
    s.prepare(p);
    CHECK_EQ(s.window_length(), 0);
    CHECK_EQ(s.play_length(), 0);
    s.trigger(hit());
    CHECK(!s.active());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 16);
    CHECK(all_zero(g_out, n));
}

TEST(sample_shorter_than_the_interpolator_is_silent) {
    // Four taps need four frames, so 1, 2 and 3 frames do not play: documented in sample.hpp,
    // and the reason there is no special case inside the inner loop.
    for (uint32_t frames = 1; frames < uint32_t(Sampler::kMinFrames); frames++) {
        fill_const(g_short, int(frames), 1, 20000);
        Sampler s;
        s.init(kRate);
        s.set_sample(g_short, frames, 1, kRate);
        CHECK(!s.has_sample());
        const TrackParams p = plain();
        s.prepare(p);
        s.trigger(hit());
        CHECK(!s.active());
        clear(g_out, kCapture);
        const int n = render_blocks(s, p, g_out, 8);
        CHECK(all_zero(g_out, n));
    }
    // Exactly four frames does play, and stays inside the buffer at both ends.
    fill_const(g_short, Sampler::kMinFrames, 1, 20000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_short, uint32_t(Sampler::kMinFrames), 1, kRate);
    CHECK(s.has_sample());
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 8);
    CHECK(finite_within(g_out, n, 2.0f));
    CHECK(peak(g_out, n) > 0.0f);
    CHECK(peak(g_out, n) < 0.62f);           // 20000/32768, edges held, never overshooting
    CHECK(!s.active());
}

TEST(sample_window_against_both_buffer_edges_is_safe) {
    // The window is the whole buffer, so the first output sample wants a tap before frame 0 and
    // the last wants two past the end. The guard regions either side of g_tiny are full scale,
    // so a read past either end lands well outside what the sample itself can produce. Checked
    // in all four play modes and at five steps: unity, two fractional ones where the edge
    // samples are not frame-aligned, and both ends of the pitch range, where a step of about 4
    // crosses the whole window in 16 samples and one of 1/16 takes a thousand.
    for (int i = 0; i < kGuard; i++) {
        g_guarded[i] = 32767;
        g_guarded[kGuard + kTinyFrames + i] = -32768;
    }
    fill_sine(g_tiny, kTinyFrames, 1, 750.0f, kRate, 0.5f);

    const uint8_t modes[4] = {0, 32, 64, 96};
    const uint8_t pitches[5] = {64, 64 + 5, 64 - 5, 0, 127};
    for (int m = 0; m < 4; m++) {
        for (int q = 0; q < 5; q++) {
            Sampler s;
            s.init(kRate);
            s.set_sample(g_tiny, uint32_t(kTinyFrames), 1, kRate);
            TrackParams p = plain();
            p.set(0, pitches[q]);
            p.set(10, modes[m]);
            s.prepare(p);
            CHECK_EQ(s.window_start(), 0);
            CHECK_EQ(s.window_length(), kTinyFrames);
            s.trigger(hit());
            clear(g_out, kCapture);
            const int n = render_blocks(s, p, g_out, 40);
            CHECK(finite_within(g_out, n, 1.0f));
            // Never louder than the source: holding the edge sample cannot overshoot, and one
            // sniff of a full-scale guard frame would show up here.
            CHECK(peak(g_out, n) < 0.53f);
            CHECK(peak(g_out, n) > 0.1f);
            s.kill_all();
        }
    }
}

TEST(sample_window_pressed_against_the_end_is_safe) {
    // START as late as the knob can put it, so the last frames of the buffer are the ones being
    // interpolated, with LEN swept as well: the other edge of the same guard. Reversed, so the
    // taps run off the far side too.
    const int frames = 2048;
    fill_sine(g_mono, frames, 1, 400.0f, kRate, 0.6f);
    for (int start = 120; start <= 127; start++) {
        for (int len = 120; len <= 127; len++) {
            Sampler s;
            s.init(kRate);
            s.set_sample(g_mono, uint32_t(frames), 1, kRate);
            TrackParams p = plain();
            p.set(1, uint8_t(start));
            p.set(2, uint8_t(len));
            p.set(10, 64);                   // reversed
            s.prepare(p);
            s.trigger(hit());
            clear(g_out, kCapture);
            const int n = render_blocks(s, p, g_out, 20);
            CHECK(finite_within(g_out, n, 1.0f));
            CHECK(peak(g_out, n) < 0.65f);
            s.kill_all();
        }
    }
}

// --- stereo ---------------------------------------------------------------

TEST(sample_stereo_source_sums_to_mono) {
    const int frames = 4096;
    fill_sine(g_stereo, frames, 2, 600.0f, kRate, 0.8f);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_stereo, uint32_t(frames), 2, kRate);
    CHECK_EQ(s.sample_channels(), 2);
    CHECK_EQ(s.sample_frames(), frames);

    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 60);
    CHECK(finite_within(g_out, n, 4.0f));
    // Both channels hold the same tone, so the mono sum is that tone at its own level and not
    // twice it: the halving is folded into the output gain.
    CHECK_NEAR(peak(g_out, n), 0.8, 0.02);
    CHECK_NEAR(measured_hz(g_out, 512, 2048), 600.0, 8.0);
}

TEST(sample_stereo_opposite_channels_cancel) {
    // L = -R is the strongest statement that both channels are really read and summed.
    const int frames = 2048;
    fill_sine(g_stereo, frames, 1, 600.0f, kRate, 0.8f);       // mono first, then interleave
    for (int i = frames - 1; i >= 0; i--) {
        const int16_t v = g_stereo[i];
        g_stereo[i * 2] = v;
        g_stereo[i * 2 + 1] = int16_t(-v);
    }
    Sampler s;
    s.init(kRate);
    s.set_sample(g_stereo, uint32_t(frames), 2, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 40);
    CHECK(peak(g_out, n) < 0.001f);
}

TEST(sample_stereo_windowing_and_reverse_work) {
    const int frames = 4096;
    fill_ramp(g_stereo, frames, 2, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_stereo, uint32_t(frames), 2, kRate);
    TrackParams p = plain();
    p.set(10, 64);                           // reverse one-shot
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 80);
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK_NEAR(g_out[0], 32000.0 / 32768.0, 0.005);
    for (int i = 1; i < frames; i++) CHECK(g_out[i] <= g_out[i - 1] + 1e-4f);
    CHECK(!s.active());
}

TEST(sample_odd_channel_count_is_refused) {
    fill_const(g_mono, 1024, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, 1024, 3, kRate);    // nothing but 1 and 2 is supported
    CHECK(!s.has_sample());
    s.set_sample(g_mono, 1024, 0, kRate);
    CHECK(!s.has_sample());
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 8);
    CHECK(all_zero(g_out, n));
}

// --- voices ---------------------------------------------------------------

TEST(sample_four_voices_overlap_and_a_fifth_steals) {
    const int frames = 24000;
    fill_const(g_mono, frames, 1, 16384);    // 0.5 per voice at unity
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);

    for (int i = 0; i < Sampler::kVoices; i++) {
        s.trigger(hit());
        clear(g_out, kCapture);
        render_blocks(s, p, g_out, 4);
        CHECK_EQ(s.playing_voices(), i + 1);
        // Each voice adds its own 0.5: four independent voices, not one retriggering.
        CHECK_NEAR(peak(g_out, 4 * kBlk), 0.5 * double(i + 1), 0.02);
    }

    // The fifth steals rather than adding a voice, and the splice does not overshoot.
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 8);
    CHECK_EQ(s.playing_voices(), Sampler::kVoices);
    CHECK(finite_within(g_out, 8 * kBlk, 8.0f));
    CHECK(peak(g_out, 8 * kBlk) < 2.1f);
    s.kill_all();
}

TEST(sample_voices_are_independent) {
    // Four voices on four slices of a ramp: each holds its own read position, so the sum is the
    // sum of four different heights and not four copies of one.
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = plain();
    p.set(3, 25);                            // 4 slices
    p.set(11, 64);                           // LVL down, so four voices stay well inside bounds
    s.prepare(p);
    CHECK_EQ(s.slices(), 4);

    const int slice_len = s.play_length();
    const float gain = (64.0f / 127.0f) * (64.0f / 127.0f);
    float expected = 0.0f;
    for (int i = 0; i < 4; i++) {
        s.trigger(hit(uint8_t(Sampler::kRootNote + i)));
        expected += float(g_mono[i * slice_len]) * (1.0f / 32768.0f) * gain;
    }
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 2);
    CHECK_NEAR(g_out[0], expected, 0.01);
    CHECK_EQ(s.playing_voices(), 4);
    s.kill_all();
}

TEST(sample_trig_frame_is_sample_accurate) {
    const int frames = 4096;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();

    for (uint32_t frame = 0; frame < 64; frame += 7) {
        Sampler one;
        one.init(kRate);
        one.set_sample(g_mono, uint32_t(frames), 1, kRate);
        one.prepare(p);
        one.trigger(hit(Sampler::kRootNote, 127, frame));
        clear(g_out, kCapture);
        one.render_add(g_out, kBlk);
        CHECK_EQ(first_nonzero(g_out, kBlk), int(frame));
        one.kill_all();
    }

    // A frame beyond this block waits for the next one.
    s.prepare(p);
    s.trigger(hit(Sampler::kRootNote, 127, 80));
    clear(g_out, kCapture);
    s.render_add(g_out, kBlk);
    CHECK(all_zero(g_out, kBlk));
    s.render_add(g_out + kBlk, kBlk);
    CHECK_EQ(first_nonzero(g_out, 2 * kBlk), 80);
}

TEST(sample_velocity_scales_the_level) {
    const int frames = 4096;
    fill_const(g_mono, frames, 1, 32767);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit(Sampler::kRootNote, 64));
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 8);
    CHECK_NEAR(peak(g_out, 8 * kBlk), 64.0 / 127.0, 0.02);
    s.kill_all();
}

// --- transport ------------------------------------------------------------

TEST(sample_render_add_accumulates) {
    const int frames = 4096;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());

    for (int i = 0; i < kBlk; i++) g_out[i] = 1.0f;
    s.prepare(p);
    s.render_add(g_out, kBlk);
    // Whatever was there is still there, with the voice added on top.
    for (int i = 0; i < kBlk; i++) CHECK_NEAR(g_out[i], 1.5, 0.01);

    // And an idle machine leaves the buffer completely alone.
    s.kill_all();
    for (int i = 0; i < kBlk; i++) g_out[i] = -0.25f;
    s.prepare(p);
    s.render_add(g_out, kBlk);
    for (int i = 0; i < kBlk; i++) CHECK_EQ(g_out[i], -0.25f);
}

TEST(sample_kill_all_silences_at_once) {
    const int frames = 24000;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 4);
    CHECK(s.active());

    s.kill_all();
    CHECK(!s.active());
    CHECK_EQ(s.playing_voices(), 0);
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 8);
    CHECK(all_zero(g_out, n));

    // A pending trig is dropped too, not held over the kill.
    s.trigger(hit(Sampler::kRootNote, 127, 32));
    s.kill_all();
    clear(g_out, kCapture);
    const int n2 = render_blocks(s, p, g_out, 4);
    CHECK(all_zero(g_out, n2));
}

TEST(sample_release_all_leaves_a_tail) {
    const int frames = 48000;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 8);

    s.release_all();
    CHECK(s.active());                       // still sounding: this is not a cut
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 8);
    CHECK(peak(g_out, kBlk) > 0.3f);         // the tail starts from where the note was
    CHECK(g_out[n - 1] < g_out[0]);          // and it falls
    // 30 ms of release, so it is gone inside 100 ms and the machine goes idle by itself.
    clear(g_other, kCapture);
    render_blocks(s, p, g_other, 80);
    CHECK(!s.active());
    CHECK(all_zero(g_other + 40 * kBlk, 40 * kBlk));
}

TEST(sample_release_lets_go_of_one_note_and_leaves_the_others) {
    // KEYS mode: two keys held, one let go. Its voice tails out; the other carries on exactly as
    // if it had been played alone.
    const int frames = 48000;
    fill_const(g_mono, frames, 1, 16384);
    const TrackParams p = plain();
    const uint8_t a = Sampler::kRootNote;
    const uint8_t b = uint8_t(Sampler::kRootNote + 12);

    Sampler both;
    both.init(kRate);
    both.set_sample(g_mono, uint32_t(frames), 1, kRate);
    both.prepare(p);
    both.trigger(hit(a));
    both.trigger(hit(b));
    clear(g_out, kCapture);
    render_blocks(both, p, g_out, 8);
    both.release(a);
    both.release(uint8_t(a + 1));                    // a note nothing holds: nothing happens
    clear(g_out, kCapture);
    render_blocks(both, p, g_out, 80);
    CHECK(both.active());

    Sampler alone;
    alone.init(kRate);
    alone.set_sample(g_mono, uint32_t(frames), 1, kRate);
    alone.prepare(p);
    alone.trigger(hit(b));
    clear(g_other, kCapture);
    render_blocks(alone, p, g_other, 8);
    clear(g_other, kCapture);
    render_blocks(alone, p, g_other, 80);

    // Once A's 30 ms tail is over, what is left is B alone, to the bit.
    bool same = true;
    for (int i = 40 * kBlk; i < 80 * kBlk; i++) same = same && g_out[i] == g_other[i];
    CHECK(same);
    CHECK(peak(g_out + 40 * kBlk, 40 * kBlk) > 0.3f);
}

// --- the ASR page: SUS and REL (2026-11-26) --------------------------------

namespace {

// A looped constant sample, so the level the envelope leaves is the only thing that moves.
TrackParams looped(uint8_t sus, uint8_t rel, uint8_t dec = 127) {
    TrackParams p = plain();
    p.set(9, dec);
    p.set(10, 32);                   // PLY: forward loop
    p.set(kAsrFirst + 1, sus);
    p.set(kAsrFirst + 2, rel);
    return p;
}

Trig gated(uint32_t frames, bool held = false) {
    Trig t = hit();
    t.gate_frames = frames;
    t.held = held;
    return t;
}

int play(Sampler &s, const TrackParams &p, const Trig &t, float *out, int blocks) {
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    s.prepare(p);
    s.trigger(t);
    clear(out, kCapture);
    return render_blocks(s, p, out, blocks);
}

}  // namespace

TEST(sample_asr_at_zero_is_the_machine_as_it_was) {
    // SUS 0 and REL 0: a LEN changes nothing (DEC decides, here holding to the loop's end), and
    // the page's ATK and fourth knob are not this machine's, so they change nothing either.
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler a, b;
    play(a, looped(0, 0), hit(), g_out, 200);
    TrackParams other = looped(0, 0);
    other.set(kAsrFirst + 0, 99);
    other.set(kAsrFirst + 3, 99);
    play(b, other, gated(10 * kBlk), g_other, 200);
    bool same = true;
    for (int i = 0; i < 200 * kBlk; i++) same = same && g_out[i] == g_other[i];
    CHECK(same);
    CHECK(b.active());
    CHECK(peak(g_other + 190 * kBlk, 10 * kBlk) > 0.4f);
}

TEST(sample_sus_up_holds_for_len_then_fades_over_rel) {
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler s;
    play(s, looped(127, 0), gated(20 * kBlk), g_out, 120);
    CHECK(peak(g_out + 15 * kBlk, kBlk) > 0.45f);    // held at the peak through LEN
    CHECK(peak(g_out + 100 * kBlk, 20 * kBlk) < 1e-3f);   // then 30 ms of REL, and gone
    clear(g_out, kCapture);
    render_blocks(s, looped(127, 0), g_out, 4);
    CHECK(!s.active());

    // REL up: the same note is still sounding well after LEN.
    Sampler slow;
    play(slow, looped(127, 100), gated(20 * kBlk), g_other, 120);
    CHECK(peak(g_other + 100 * kBlk, 20 * kBlk) > 0.1f);
    CHECK(slow.active());
}

TEST(sample_sus_is_the_level_dec_falls_to) {
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler half, none;
    // DEC 20 is a few tens of milliseconds, so by half a second it has landed.
    play(half, looped(64, 0, 20), gated(kCapture), g_out, 400);
    play(none, looped(0, 0, 20), gated(kCapture), g_other, 400);
    const float held = peak(g_out + 380 * kBlk, 20 * kBlk);
    CHECK(held > 0.2f && held < 0.3f);                 // 0.5 of the sample's 0.5
    CHECK(peak(g_other + 380 * kBlk, 20 * kBlk) < 1e-3f);   // SUS 0: decayed away
}

TEST(sample_sus_up_holds_a_key_until_it_comes_up_and_lets_a_one_shot_go) {
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler key;
    play(key, looped(127, 0), gated(0, true), g_out, 200);
    CHECK(peak(g_out + 190 * kBlk, 10 * kBlk) > 0.45f);    // no LEN: the key holds it
    key.release(Sampler::kRootNote);
    clear(g_out, kCapture);
    render_blocks(key, looped(127, 0), g_out, 80);
    CHECK(!key.active());

    // Neither a LEN nor a key-up to come: with SUS up nothing would end it, so it lets go.
    Sampler loose;
    play(loose, looped(127, 0), hit(), g_other, 80);
    CHECK(!loose.active());
}

TEST(sample_release_curve_starts_at_the_old_declick) {
    CHECK(sample_release_seconds(0) == 0.03f);
    CHECK(sample_release_seconds(127) > 7.9f && sample_release_seconds(127) < 8.1f);
}

TEST(sample_a_note_released_before_its_frame_does_not_hang) {
    // Pressed and released inside one 1.3 ms block, which a 6 ms key scan never delivers: the
    // point is only that the voice is let go rather than left holding.
    const int frames = 48000;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    const TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit(Sampler::kRootNote, 127, 40));
    s.release(Sampler::kRootNote);
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 80);
    CHECK(!s.active());
    CHECK(all_zero(g_out + 40 * kBlk, 40 * kBlk));
}

// --- parameters -----------------------------------------------------------

TEST(sample_extreme_parameters_stay_finite) {
    const int frames = 8192;
    fill_sine(g_mono, frames, 1, 900.0f, kRate, 1.0f);
    const uint8_t values[2] = {0, 127};
    for (int v = 0; v < 2; v++) {
        Sampler s;
        s.init(kRate);
        s.set_sample(g_mono, uint32_t(frames), 1, kRate);
        const TrackParams p = uniform(values[v]);
        s.prepare(p);
        s.trigger(hit(0, 1));
        s.trigger(hit(127, 127));
        clear(g_out, kCapture);
        const int n = render_blocks(s, p, g_out, 300);
        CHECK(finite_within(g_out, n, 8.0f));
        s.kill_all();
    }

    // All parameters at 127 puts START on the last frame, which is silent by design, so pin the
    // loud extreme too: everything at maximum except a window that exists.
    Sampler loud;
    loud.init(kRate);
    loud.set_sample(g_mono, uint32_t(frames), 1, kRate);
    TrackParams p = uniform(127);
    p.set(1, 0);                             // START at the beginning
    p.set(3, 0);                             // one slice, so the note transposes as far as it can
    loud.prepare(p);
    for (int i = 0; i < 8; i++) loud.trigger(hit(127, 127));
    clear(g_out, kCapture);
    const int n = render_blocks(loud, p, g_out, 400);
    CHECK(finite_within(g_out, n, 8.0f));
    CHECK(peak(g_out, n) > 0.1f);
    loud.kill_all();

    // Every PLY zone and a spread of slice counts, at a spread of pitches: finite and in bounds.
    for (int ply = 0; ply < 128; ply += 16) {
        for (int slc = 0; slc < 128; slc += 31) {
            Sampler s;
            s.init(kRate);
            s.set_sample(g_mono, uint32_t(frames), 1, kRate);
            TrackParams q = plain();
            q.set(0, uint8_t(ply));          // doubles as a pitch sweep
            q.set(3, uint8_t(slc));
            q.set(10, uint8_t(ply));
            s.prepare(q);
            s.trigger(hit(uint8_t(40 + ply / 2), 100));
            clear(g_out, kCapture);
            const int n2 = render_blocks(s, q, g_out, 120);
            CHECK(finite_within(g_out, n2, 4.0f));
            s.kill_all();
        }
    }
}

TEST(sample_param_names_are_three_upper_case_letters) {
    Sampler s;
    s.init(kRate);
    CHECK_EQ(s.used_params(), 8);
    const char *const expected[kParams] = {
        "PIT", "STA", "LEN", "SLC", "---", "---", "---", "---",
        "ATK", "DEC", "PLY", "LVL", "---", "---", "---", "---",
    };
    for (int i = 0; i < kParams; i++) {
        const char *name = s.param_name(i);
        CHECK_EQ(name[0], expected[i][0]);
        CHECK_EQ(name[1], expected[i][1]);
        CHECK_EQ(name[2], expected[i][2]);
        CHECK_EQ(name[3], '\0');
        for (int c = 0; c < 3; c++) {
            const char ch = name[c];
            CHECK((ch >= 'A' && ch <= 'Z') || ch == '-');
        }
    }
    // Out of range is answered, not read past.
    CHECK_EQ(s.param_name(-1)[0], '-');
    CHECK_EQ(s.param_name(kParams)[0], '-');
}

TEST(sample_is_safe_to_trigger_before_the_first_prepare) {
    const int frames = 4096;
    fill_const(g_mono, frames, 1, 16384);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    s.trigger(hit());                        // no prepare() at all: init()'s defaults stand
    clear(g_out, kCapture);
    int n = 0;
    for (int b = 0; b < 100; b++) {
        s.render_add(g_out + b * kBlk, kBlk);
        n += kBlk;
    }
    CHECK(finite_within(g_out, n, 4.0f));
    CHECK(peak(g_out, n) > 0.0f);
    s.kill_all();
}

TEST(sample_swapping_pcm_recomputes_the_window) {
    // The window is in frames, so a new sample of a different length changes what START and LEN
    // mean. set_sample must redo that itself: prepare() may not run in between.
    fill_const(g_mono, kSrcFrames, 1, 16384);
    Sampler s;
    s.init(kRate);
    TrackParams p = plain();
    p.set(1, 64);                            // START halfway
    s.prepare(p);
    CHECK_EQ(s.play_length(), 0);            // nothing resident yet

    s.set_sample(g_mono, 8192, 1, kRate);
    CHECK_NEAR(float(s.window_start()), 0.50394f * 8191.0f, 2.0);
    CHECK_EQ(s.window_length(), 8192 - s.window_start());

    s.set_sample(g_mono, 1024, 1, kRate);
    CHECK_NEAR(float(s.window_start()), 0.50394f * 1023.0f, 2.0);
    CHECK_EQ(s.window_length(), 1024 - s.window_start());
    s.trigger(hit());
    clear(g_out, kCapture);
    const int n = render_blocks(s, p, g_out, 40);
    CHECK(finite_within(g_out, n, 2.0f));
    CHECK(peak(g_out, n) > 0.4f);
    CHECK(!s.active());                      // half of 1024 frames is 10 ms: long gone
}

// --- stream mode (set_stream) ------------------------------------------------------------------
//
// A streamed file: the head resident, the rest arriving in a StreamFifo that the engine fills
// every block. These feed the fifo by hand from the same PCM a whole-loaded Sampler plays, so the
// two can be compared sample for sample: with the fifo kept full, streaming is the same sound.

namespace {

StreamFifo g_fifo;
Sampler g_streamer;   // big: file scope
Sampler g_whole;

// What the engine's top_up_streams does: as many frames as fit, read on from where it got to.
// `limit` caps one call, as kStreamReadFrames does; 0 frames is a card that is late.
void feed(const int16_t *pcm, uint32_t total, int channels, uint32_t limit) {
    uint32_t room = 0;
    int16_t *span = g_fifo.write_span(&room);
    const uint32_t next = g_fifo.next_frame();
    uint32_t n = room < limit ? room : limit;
    if (n > total - next) n = total - next;
    for (uint32_t i = 0; i < n * uint32_t(channels); i++) span[i] = pcm[next * uint32_t(channels) + i];
    g_fifo.commit(n);
}

int render_streamed(const TrackParams &p, const int16_t *pcm, uint32_t total, int channels,
                    float *out, int blocks, uint32_t limit) {
    for (int b = 0; b < blocks; b++) {
        feed(pcm, total, channels, limit);
        g_streamer.prepare(p);
        g_streamer.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

}  // namespace

TEST(sample_stream_plays_exactly_what_a_whole_load_plays) {
    constexpr uint32_t kHead = 3000;
    fill_sine(g_mono, kSrcFrames, 1, 331.0f, kRate, 0.8f);
    const TrackParams p = plain();
    static const int kPitches[] = {64, 90, 30};
    for (int pitch : kPitches) {
        TrackParams q = p;
        q.set(0, uint8_t(pitch));
        g_whole.init(kRate);
        g_whole.set_sample(g_mono, kSrcFrames, 1, kRate);
        g_whole.prepare(q);
        g_whole.trigger(hit());
        clear(g_other, kCapture);
        render_blocks(g_whole, q, g_other, 600);

        g_streamer.init(kRate);
        // A late count only ever grows (the engine reads it as a difference), and init() does not
        // clear it: a test before this one may have starved the stream. Measured from here.
        const uint32_t late_before = g_streamer.stream_late();
        g_streamer.set_stream(g_mono, kHead, kSrcFrames, 1, kRate, &g_fifo);
        CHECK(g_streamer.streaming());
        CHECK_EQ(g_streamer.stream_frames(), uint32_t(kSrcFrames));
        g_streamer.prepare(q);
        g_streamer.trigger(hit());
        clear(g_out, kCapture);
        const int n = render_streamed(q, g_mono, kSrcFrames, 1, g_out, 600, 512);
        int wrong = 0;
        for (int i = 0; i < n; i++) wrong += g_out[i] != g_other[i] ? 1 : 0;
        CHECK_EQ(wrong, 0);
        CHECK_EQ(g_streamer.stream_late(), late_before);
        CHECK(peak(g_out, n) > 0.5f);
    }
}

TEST(sample_stream_stereo_matches_too) {
    constexpr uint32_t kHead = 2048;
    fill_sine(g_stereo, kSrcFrames, 2, 517.0f, kRate, 0.7f);
    const TrackParams p = plain();
    g_whole.init(kRate);
    g_whole.set_sample(g_stereo, kSrcFrames, 2, kRate);
    g_whole.prepare(p);
    g_whole.trigger(hit(Sampler::kRootNote + 5));
    clear(g_other, kCapture);
    render_blocks(g_whole, p, g_other, 400);

    g_streamer.init(kRate);
    g_streamer.set_stream(g_stereo, kHead, kSrcFrames, 2, kRate, &g_fifo);
    g_streamer.prepare(p);
    g_streamer.trigger(hit(Sampler::kRootNote + 5));
    clear(g_out, kCapture);
    const int n = render_streamed(p, g_stereo, kSrcFrames, 2, g_out, 400, 512);
    int wrong = 0;
    for (int i = 0; i < n; i++) wrong += g_out[i] != g_other[i] ? 1 : 0;
    CHECK_EQ(wrong, 0);
}

TEST(sample_stream_ignores_start_slices_and_reverse) {
    // A stream reads on from frame 0: none of these can make it jump (docs/FEATURES.md says
    // why each is left out).
    constexpr uint32_t kHead = 3000;
    fill_ramp(g_mono, kSrcFrames, 1, -20000, 20000);
    TrackParams p = plain();
    p.set(1, 64);       // STA halfway
    p.set(3, 127);      // SLC 16
    p.set(10, 64);      // PLY reverse one-shot
    g_streamer.init(kRate);
    g_streamer.set_stream(g_mono, kHead, kSrcFrames, 1, kRate, &g_fifo);
    g_streamer.prepare(p);
    CHECK_EQ(g_streamer.window_start(), 0);
    CHECK_EQ(g_streamer.play_length(), kSrcFrames);
    g_streamer.trigger(hit(Sampler::kRootNote + 3));
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kSrcFrames, 1, g_out, 4, 512);
    // Forward from the bottom of the ramp, transposed by the note rather than slicing.
    CHECK(g_out[0] < -0.1f);
    CHECK(g_out[200] > g_out[0]);
    CHECK(g_streamer.stream_position() > 256u);
}

TEST(sample_stream_len_is_an_end_point) {
    // LEN stops the voice that far into the FILE, and a loop goes back to the head from there.
    constexpr uint32_t kHead = 3000;
    constexpr uint32_t kTotal = 20000;
    fill_const(g_mono, kSrcFrames, 1, 16000);
    TrackParams p = plain();
    p.set(2, 64);       // LEN about half
    g_streamer.init(kRate);
    const uint32_t late_before = g_streamer.stream_late();   // only ever grows: see above
    g_streamer.set_stream(g_mono, kHead, kTotal, 1, kRate, &g_fifo);
    g_streamer.prepare(p);
    const int32_t half = int32_t(p.unit(2) * float(kTotal) + 0.5f);
    CHECK_EQ(g_streamer.play_length(), half);
    CHECK_EQ(g_streamer.window_start(), 0);
    g_streamer.trigger(hit());
    clear(g_out, kCapture);
    const int blocks = int(kTotal / kBlk) + 4;
    render_streamed(p, g_mono, kTotal, 1, g_out, blocks, 512);
    // Sound until LEN, silence after it: well before the file's end.
    CHECK(g_out[half - 400] > 0.2f);
    CHECK(all_zero(g_out + half + kBlk, kBlk * 4));
    CHECK(!g_streamer.active());
    CHECK_EQ(g_streamer.stream_late(), late_before);

    // LEN at the top is the whole file, to the frame, however long.
    p.set(2, 127);
    g_streamer.prepare(p);
    CHECK_EQ(g_streamer.play_length(), int32_t(kTotal));
    g_streamer.set_stream(g_mono, kHead, (1u << 25) + 1u, 1, kRate, &g_fifo);
    g_streamer.prepare(p);
    CHECK_EQ(g_streamer.play_length(), int32_t((1u << 25) + 1u));
}

TEST(sample_stream_a_len_loop_inside_the_head_never_reopens_the_stream) {
    constexpr uint32_t kHead = 6000;
    constexpr uint32_t kTotal = 60000;
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.5f);
    TrackParams p = plain();
    p.set(10, 32);      // PLY forward loop
    p.set(2, 6);        // LEN about 4.7% of 60,000: 2,835 frames, inside the head
    g_streamer.init(kRate);
    const uint32_t late_before = g_streamer.stream_late();   // only ever grows: see above
    g_streamer.set_stream(g_mono, kHead, kTotal, 1, kRate, &g_fifo);
    g_streamer.prepare(p);
    CHECK(g_streamer.play_length() < int32_t(kHead));
    const uint32_t before = g_streamer.stream_restarts();
    g_streamer.trigger(hit());
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kTotal, 1, g_out, 200, 512);   // 12,800 frames: four times round
    CHECK_EQ(g_streamer.stream_restarts(), before + 1);       // the trigger only
    CHECK(g_streamer.active());
    CHECK(g_streamer.stream_position() < uint32_t(g_streamer.play_length()));
    CHECK(peak(g_out + kBlk * 150, kBlk * 40) > 0.3f);
    CHECK_EQ(g_streamer.stream_late(), late_before);
}

TEST(sample_stream_restarts_on_a_trigger_and_on_a_loop) {
    constexpr uint32_t kHead = 3000;
    constexpr uint32_t kTotal = 9000;
    fill_sine(g_mono, kSrcFrames, 1, 200.0f, kRate, 0.5f);
    TrackParams p = plain();
    p.set(10, 32);      // PLY forward loop
    g_streamer.init(kRate);
    g_streamer.set_stream(g_mono, kHead, kTotal, 1, kRate, &g_fifo);
    const uint32_t before = g_streamer.stream_restarts();
    g_streamer.prepare(p);
    g_streamer.trigger(hit());
    render_streamed(p, g_mono, kTotal, 1, g_out, 1, 512);
    CHECK_EQ(g_streamer.stream_restarts(), before + 1);      // the trigger
    // The fifo now waits for the frame after the head, as the reopened stream will deliver.
    CHECK_EQ(g_fifo.next_frame() <= kHead + 512u, true);
    // Through the file and round again: the loop wrap is a restart too.
    render_streamed(p, g_mono, kTotal, 1, g_out, int(kTotal / kBlk) + 2, 512);
    CHECK_EQ(g_streamer.stream_restarts(), before + 2);
    CHECK(g_streamer.active());
    CHECK(g_streamer.stream_position() < kHead);
    // A second trigger mid-file: one voice, so it restarts rather than adding a voice.
    g_streamer.trigger(hit());
    render_streamed(p, g_mono, kTotal, 1, g_out, 2, 512);
    CHECK_EQ(g_streamer.stream_restarts(), before + 3);
    CHECK_EQ(g_streamer.playing_voices(), 1);
}

TEST(sample_stream_a_late_card_is_silence_and_time_goes_on) {
    constexpr uint32_t kHead = 1024;
    fill_const(g_mono, kSrcFrames, 1, 16000);
    const TrackParams p = plain();
    g_streamer.init(kRate);
    g_streamer.set_stream(g_mono, kHead, kSrcFrames, 1, kRate, &g_fifo);
    g_streamer.prepare(p);
    g_streamer.trigger(hit());
    // The head plays; then nothing arrives for 40 blocks.
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kSrcFrames, 1, g_out, 15, 0);   // 960 frames: taps inside the head
    CHECK(g_out[kBlk * 4] > 0.3f);
    CHECK_EQ(g_streamer.stream_late(), 0u);
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kSrcFrames, 1, g_out, 41, 0);
    CHECK(g_streamer.stream_late() > 0u);
    CHECK(all_zero(g_out + kBlk * 4, kBlk * 30));
    const uint32_t at = g_streamer.stream_position();
    CHECK(at >= uint32_t(56 * kBlk) - 2u);                     // it kept time
    // The card catches up: what is behind the voice is thrown away and it plays again.
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kSrcFrames, 1, g_out, 30, 512);
    const uint32_t late = g_streamer.stream_late();
    clear(g_out, kCapture);
    render_streamed(p, g_mono, kSrcFrames, 1, g_out, 10, 512);
    CHECK_EQ(g_streamer.stream_late(), late);
    CHECK(g_out[kBlk * 5] > 0.3f);
}

TEST(sample_stream_refuses_what_is_not_a_stream_and_set_sample_leaves_it) {
    fill_const(g_mono, kSrcFrames, 1, 1000);
    g_streamer.init(kRate);
    g_streamer.set_stream(g_mono, 4000, 4000, 1, kRate, &g_fifo);     // no tail
    CHECK(!g_streamer.streaming());
    CHECK(!g_streamer.has_sample());
    g_streamer.set_stream(g_mono, 4000, 9000, 1, kRate, nullptr);     // no fifo
    CHECK(!g_streamer.streaming());
    g_streamer.set_stream(g_mono, 4000, 9000, 3, kRate, &g_fifo);     // three channels
    CHECK(!g_streamer.streaming());
    g_streamer.set_stream(g_mono, 4000, 9000, 1, kRate, &g_fifo);
    CHECK(g_streamer.streaming());
    g_streamer.set_sample(g_mono, 4000, 1, kRate);
    CHECK(!g_streamer.streaming());
    CHECK(g_streamer.has_sample());
}

// --- SLC AUTO: slices at the sample's detected points --------------------------

namespace {

// Three points: 3000 the strongest, 5000 next, 1000 the weakest.
SlicePoints three_points() {
    SlicePoints points;
    points.count = 3;
    points.at[0] = 1000;
    points.at[1] = 3000;
    points.at[2] = 5000;
    points.rank[0] = 2;
    points.rank[1] = 0;
    points.rank[2] = 1;
    return points;
}

// SLC set to n slices: slices_ = 1 + round(unit * 15).
uint8_t slc_for(int slices) {
    for (int v = 0; v <= 127; v++) {
        TrackParams p = plain();
        p.set(3, uint8_t(v));
        Sampler s;
        s.init(kRate);
        s.prepare(p);
        if (s.slices() == slices) return uint8_t(v);
    }
    return 0;
}

}  // namespace

TEST(sample_auto_slices_cut_at_the_strongest_points) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    TrackParams p = plain();
    p.set(3, slc_for(3));

    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    s.set_slice_points(three_points());
    s.prepare(p);
    CHECK(!s.slicing_by_points());                     // off until asked for
    CHECK_EQ(s.slice_count(), 3);
    s.set_auto_slices(true);
    CHECK(s.slicing_by_points());
    // Three slices: cut at the two strongest, 3000 and 5000, not at 1000.
    CHECK_EQ(s.slice_count(), 3);
    CHECK_EQ(s.slice_start(0), 0);
    CHECK_EQ(s.slice_start(1), 3000);
    CHECK_EQ(s.slice_start(2), 5000);

    const int lengths[] = {3000, 2000, frames - 5000};
    for (int slice = 0; slice < 3; slice++) {
        Sampler one;
        one.init(kRate);
        one.set_sample(g_mono, uint32_t(frames), 1, kRate);
        one.set_slice_points(three_points());
        one.set_auto_slices(true);
        one.prepare(p);
        one.trigger(hit(uint8_t(Sampler::kRootNote + slice)));
        clear(g_out, kCapture);
        const int n = render_blocks(one, p, g_out, 64);
        CHECK(finite_within(g_out, n, 4.0f));
        CHECK_NEAR(g_out[0], float(g_mono[s.slice_start(slice)]) * (1.0f / 32768.0f), 0.005);
        CHECK(!one.active());
        CHECK(!all_zero(g_out + lengths[slice] - 64, 32));          // still playing near its end
        CHECK(all_zero(g_out + lengths[slice] + kBlk, n - lengths[slice] - kBlk));
    }

    // SLC 4 uses all three; SLC 16 cannot have more slices than there are points.
    p.set(3, slc_for(4));
    s.prepare(p);
    CHECK_EQ(s.slice_count(), 4);
    CHECK_EQ(s.slice_start(1), 1000);
    p.set(3, 127);
    s.prepare(p);
    CHECK_EQ(s.slice_count(), 4);
    // SLC 1 is a one-shot, AUTO or not.
    p.set(3, 0);
    s.prepare(p);
    CHECK(!s.slicing_by_points());
    CHECK_EQ(s.slice_count(), 1);
}

TEST(sample_auto_slices_keep_to_the_window_and_fall_back_to_equal) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    s.set_slice_points(three_points());
    s.set_auto_slices(true);
    TrackParams p = plain();
    p.set(3, 127);
    p.set(1, 32);                                      // START about a quarter in: 2047
    s.prepare(p);
    // 1000 is before the window: the slices start at the window and cut at 3000 and 5000.
    CHECK_EQ(s.slice_count(), 3);
    CHECK_EQ(s.slice_start(0), s.window_start());
    CHECK_EQ(s.slice_start(1), 3000);

    // A window with no point in it: equal slices, exactly as without AUTO.
    p.set(1, 90);                                      // START past 5000
    s.prepare(p);
    CHECK(!s.slicing_by_points());
    CHECK_EQ(s.slice_count(), 16);

    // A new sample forgets the old one's points: equal again until the engine hands new ones.
    p.set(1, 0);
    s.set_sample(g_mono, uint32_t(frames), 1, kRate);
    s.prepare(p);
    CHECK(s.auto_slices());
    CHECK(!s.slicing_by_points());
    CHECK_EQ(s.point_count(), 0);
}

TEST(sample_auto_slices_off_is_bit_exact_with_equal_slicing) {
    const int frames = 8192;
    fill_ramp(g_mono, frames, 1, 0, 32000);
    TrackParams p = plain();
    p.set(3, slc_for(4));
    for (int note = 0; note < 4; note++) {
        Sampler plain_one;
        Sampler with_points;
        plain_one.init(kRate);
        with_points.init(kRate);
        plain_one.set_sample(g_mono, uint32_t(frames), 1, kRate);
        with_points.set_sample(g_mono, uint32_t(frames), 1, kRate);
        with_points.set_slice_points(three_points());       // points, but AUTO off
        plain_one.prepare(p);
        with_points.prepare(p);
        plain_one.trigger(hit(uint8_t(Sampler::kRootNote + note)));
        with_points.trigger(hit(uint8_t(Sampler::kRootNote + note)));
        clear(g_out, kCapture);
        clear(g_other, kCapture);
        const int n = render_blocks(plain_one, p, g_out, 24);
        render_blocks(with_points, p, g_other, 24);
        bool same = true;
        for (int i = 0; i < n; i++) same = same && g_out[i] == g_other[i];
        CHECK(same);
    }
}

// dsp/sampleloops.ti.c against the interior loop it replaced in Sampler::generate, written out
// here as it was (Catmull-Rom over four taps, a Q24 fraction, the pointer stepped on the carry):
// mono and stereo, forwards and backwards, integer and fractional steps, below and above unity,
// runs of one sample and of a whole block. memcmp, so the read is the same to the bit, and the
// position it hands back is the same to the frame and the fraction.
namespace {

float reference_tap(const int16_t *p, int ch) {
    return ch == 2 ? float(int(p[0]) + int(p[1])) : float(p[0]);
}

float reference_hermite(float x0, float x1, float x2, float x3, float t) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

void reference_read(float *dst, int k, const int16_t *p, int stride, int istride, uint32_t &frac,
                    uint32_t fstep, int32_t &pos, int32_t istep, int ch) {
    const auto read4 = [&](const int16_t *q, float t) {
        return reference_hermite(reference_tap(q - stride, ch), reference_tap(q, ch),
                                 reference_tap(q + stride, ch), reference_tap(q + 2 * stride, ch),
                                 t);
    };
    for (int j = 0; j < k - 1; j++) {
        dst[j] = read4(p, float(int32_t(frac)) * (1.0f / 16777216.0f));
        frac += fstep;
        const uint32_t carry = frac >> 24;
        frac &= 0xFFFFFFu;
        p += istride + (carry != 0u ? stride : 0);
        pos += istep + int32_t(carry);
    }
    dst[k - 1] = read4(p, float(int32_t(frac)) * (1.0f / 16777216.0f));
}

int16_t g_pcm[2 * 8192];

}  // namespace

TEST(sample_the_cl6x_read_loop_is_bit_exact_against_the_loop_it_replaced) {
    uint32_t seed = 0x2468ACEu;
    for (int16_t &s : g_pcm) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        s = int16_t(seed >> 16);    // full scale, both signs: -32768 included sooner or later
    }
    const float steps[] = {1.0f, 0.5f, 0.0313f, 1.37f, 2.0f, 7.91f, 31.5f};
    const int runs[] = {1, 2, 15, 64};
    int compared = 0;
    for (int ch = 1; ch <= 2; ch++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            for (float step : steps) {
                for (int k : runs) {
                    const int32_t istep = int32_t(step);
                    const uint32_t fstep = uint32_t((step - float(istep)) * 16777216.0f);
                    const int stride = dir * ch;
                    // Start mid-buffer: 64 samples at 32x never reach either end from here.
                    const int16_t *p = g_pcm + 4096 * ch;
                    uint32_t rf = 0x123456u, of = rf;
                    int32_t rp = 77, op = rp;
                    float ref[64], ours[64];
                    reference_read(ref, k, p, stride, istep * stride, rf, fstep, rp, istep, ch);
                    if (ch == 2) {
                        strata_sample_read_stereo(ours, k, p, stride, istep * stride, &of, fstep,
                                                  &op, istep);
                    } else {
                        strata_sample_read_mono(ours, k, p, stride, istep * stride, &of, fstep,
                                                &op, istep);
                    }
                    CHECK(std::memcmp(ref, ours, sizeof(float) * size_t(k)) == 0);
                    CHECK_EQ(of, rf);
                    CHECK_EQ(op, rp);
                    compared++;
                }
            }
        }
    }
    CHECK_EQ(compared, 2 * 2 * 7 * 4);
}

// --- the screen's view (core/overview.hpp, WaveView) ----------------------------------------
//
// EDIT's waveform draws where the machine is, in the overview's columns. The window, the slice
// starts and the playheads have to land on the columns the overview builder put those frames in,
// or a STA line sits beside the hit it starts on.

TEST(sample_wave_view_shows_the_window_and_the_slices) {
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    p.set(1, 32);                                  // STA a quarter in
    p.set(2, 64);                                  // LEN about half of the rest
    p.set(3, 25);                                  // SLC 4
    s.prepare(p);
    CHECK_EQ(s.slices(), 4);
    WaveView view;
    s.wave_view(view);
    const auto column = [](int frame) {
        return int(int64_t(frame) * kWaveColumns / kSrcFrames);
    };
    CHECK(std::abs(int(view.lo) - column(s.window_start())) <= 1);
    CHECK(std::abs(int(view.hi) - column(s.window_start() + s.window_length() - 1)) <= 1);
    int cuts = 0;
    for (int x = 0; x < kWaveColumns; x++) cuts += wave_has_cut(view.cuts, x) ? 1 : 0;
    CHECK_EQ(cuts, 3);
    for (int i = 1; i < 4; i++) {
        const int c = column(s.slice_start(i));
        CHECK(wave_has_cut(view.cuts, c) || wave_has_cut(view.cuts, c - 1) ||
              wave_has_cut(view.cuts, c + 1));
    }
    // Nothing playing: no marks.
    for (int i = 0; i < WaveView::kMarks; i++) CHECK_EQ(int(view.mark[i]), int(WaveView::kNone));
}

TEST(sample_wave_view_puts_a_playhead_on_each_voice_and_follows_reverse) {
    Sampler s;
    s.init(kRate);
    s.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams p = plain();
    s.prepare(p);
    s.trigger(hit());
    clear(g_out, kCapture);
    render_blocks(s, p, g_out, 375);               // half a second: half way through
    WaveView view;
    s.wave_view(view);
    CHECK(std::abs(int(view.mark[0]) - kWaveColumns / 2) <= 1);
    CHECK(view.glow[0] >= 14);                     // DEC holds: the voice is at full level
    CHECK_EQ(int(view.mark[1]), int(WaveView::kNone));

    // Reversed, the same time in is the same distance from the END.
    Sampler r;
    r.init(kRate);
    r.set_sample(g_mono, uint32_t(kSrcFrames), 1, kRate);
    TrackParams rev = plain();
    rev.set(10, 64);                               // PLY: reverse one-shot
    r.prepare(rev);
    r.trigger(hit());
    render_blocks(r, rev, g_out, 94);              // an eighth of a second
    r.wave_view(view);
    CHECK(std::abs(int(view.mark[0]) - (kWaveColumns - 1 - kWaveColumns / 8)) <= 1);

    // Two notes, two playheads.
    s.trigger(hit());
    render_blocks(s, p, g_out, 2);
    s.wave_view(view);
    CHECK(view.mark[0] < kWaveColumns);
    CHECK(view.mark[1] < kWaveColumns);
}

TEST(sample_wave_view_of_no_sample_is_empty) {
    Sampler s;
    s.init(kRate);
    WaveView view;
    view.lo = 3;
    s.wave_view(view);
    CHECK_EQ(int(view.lo), int(WaveView::kNone));
    CHECK_EQ(int(view.hi), int(WaveView::kNone));
}
