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

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "sample.hpp"

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
