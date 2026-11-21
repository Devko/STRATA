// VA MONO: the bass voice in dsp/va.cpp.
//
// These test measurable properties rather than sample values -- that a note sounds and dies,
// that the pitch follows the note and the tuning, that each filter mode filters, that glide
// slews and slide does not retrigger, that an accent changes more than the volume -- because a
// test that pins a property is what survives a later tidy-up, while a test that pins a sample
// only pins the compiler.
#include "check.hpp"

#include <cstring>

#include <enaga/abi.h>

#include "va.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kCapture = ENAGA_SAMPLE_RATE * 2;        // 2 s: longer than the longest DEC tail

// The eight parameters this machine owns. dsp/va.cpp documents each mapping.
constexpr int kTun = 0, kMod = 1, kCut = 2, kRes = 3;
constexpr int kDec = 8, kEnv = 9, kGld = 10, kLvl = 11;

// MOD values inside each of the four zones (the zones are deliberately unequal: see va.cpp).
constexpr uint8_t kLp24 = 10, kLp12 = 70, kBp12 = 100, kHp12 = 120;

// Static, because two seconds of audio does not belong on a stack frame.
float g_a[kCapture];
float g_b[kCapture];

void clear(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = 0.0f;
}

void fill(float *x, int n, float value) {
    for (int i = 0; i < n; i++) x[i] = value;
}

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

// A usable starting patch: a long note, a wide open static low-pass, no glide, full level. Each
// test moves only the knobs it is about.
TrackParams patch() {
    TrackParams p = uniform(64);
    p.set(kTun, 64);      // no transposition
    p.set(kMod, kLp12);
    p.set(kCut, 127);     // wide open
    p.set(kRes, 0);
    p.set(kDec, 127);     // 2.4 s: it sustains through any window below
    p.set(kEnv, 0);       // a static filter, so the waveform does not move while measured
    p.set(kGld, 0);
    p.set(kLvl, 127);
    return p;
}

Trig hit(uint8_t note = 60, uint8_t velocity = 100, bool accent = false, bool slide = false,
         uint32_t frame = 0) {
    Trig t;
    t.note = note;
    t.velocity = velocity;
    t.accent = accent;
    t.slide = slide;
    t.frame = frame;
    return t;
}

// Render `blocks` blocks of 64 frames into out, re-reading the parameters every block exactly as
// audio() does. The buffer is the caller's to clear.
int render(Machine &m, const TrackParams &p, float *out, int blocks) {
    for (int b = 0; b < blocks; b++) {
        m.prepare(p);
        m.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

int blocks_for(float seconds) {
    return int(seconds * kRate) / kBlk;
}

float peak(const float *x, int n) {
    float best = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > best) best = a;
    }
    return best;
}

float energy(const float *x, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += x[i] * x[i];
    return sum;
}

// NaN fails x == x and an infinity fails the bounds, so this needs no <cmath>.
bool finite_within(const float *x, int n, float bound) {
    for (int i = 0; i < n; i++) {
        if (!(x[i] == x[i])) return false;
        if (!(x[i] > -bound && x[i] < bound)) return false;
    }
    return true;
}

// A proxy for brightness: scale-free, so it compares two signals of different levels directly.
int sign_changes(const float *x, int n) {
    int count = 0;
    for (int i = 1; i < n; i++) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) count++;
    }
    return count;
}

// How nearly the signal repeats itself after `lag` samples: the normalised autocorrelation at
// one lag, 1 for an exact repeat and 0 for none. Unlike a zero-crossing count this does not
// care how rich the waveform is, which is what makes it the right way to pin a pitch here --
// the voice is a saw, a synced pulse and a sub triangle at once. No sqrt in the host either:
// the geometric mean goes through exp2(0.5 * log2(x)), as docs/ENAGA_API.md prescribes.
float periodicity(const float *x, int n, int lag) {
    if (lag <= 0 || lag >= n) return 0.0f;
    double num = 0.0, ea = 0.0, eb = 0.0;
    for (int i = 0; i + lag < n; i++) {
        num += double(x[i]) * double(x[i + lag]);
        ea += double(x[i]) * double(x[i]);
        eb += double(x[i + lag]) * double(x[i + lag]);
    }
    const double product = ea * eb;
    if (product <= 1e-24) return 0.0f;
    const float root = enaga::dsp::exp2(0.5f * enaga::dsp::log2(float(product)));
    return float(num) * enaga::dsp::reciprocal(root);
}

// The composite period of this voice at a given note, in samples. OSC 1 runs at the note and
// the sub an octave below it, so the waveform as a whole repeats at the SUB's rate -- half the
// note's frequency -- whatever the filter is doing to the harmonics above.
int period_of(int note) {
    const float hz = 440.0f * enaga::dsp::semitones(float(note) - 69.0f);
    return int(2.0f * kRate * enaga::dsp::reciprocal(hz) + 0.5f);
}

}  // namespace

// --- silence, sounding, dying ---------------------------------------------------------------

TEST(va_is_silent_and_inactive_before_any_trigger) {
    VaMono va;
    va.init(kRate);
    CHECK(!va.active());

    const TrackParams p = patch();
    clear(g_a, 8 * kBlk);
    render(va, p, g_a, 8);
    CHECK_EQ(peak(g_a, 8 * kBlk), 0.0f);
    CHECK(!va.active());
}

TEST(va_trigger_sounds_and_sets_active) {
    VaMono va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit());
    CHECK(va.active());        // true before the first render, or the engine would skip it

    clear(g_a, 16 * kBlk);
    render(va, p, g_a, 16);
    CHECK(peak(g_a, 16 * kBlk) > 0.05f);
    CHECK(va.active());
    CHECK(finite_within(g_a, 16 * kBlk, 4.0f));
}

TEST(va_note_decays_to_silence_and_goes_inactive) {
    VaMono va;
    va.init(kRate);
    TrackParams p = patch();
    p.set(kDec, 0);            // the shortest decay: 15 ms
    va.prepare(p);
    va.trigger(hit());

    clear(g_a, blocks_for(0.5f) * kBlk);
    const int n = render(va, p, g_a, blocks_for(0.5f));
    CHECK(peak(g_a, 2 * kBlk) > 0.05f);                 // it did sound
    CHECK(peak(g_a + n - 4 * kBlk, 4 * kBlk) < 1e-4f);  // and then it did not
    CHECK(!va.active());
}

TEST(va_trigger_frame_is_sample_accurate) {
    VaMono va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit(60, 100, false, false, 40));

    clear(g_a, 4 * kBlk);
    render(va, p, g_a, 4);
    CHECK_EQ(peak(g_a, 40), 0.0f);                 // nothing before the trig's frame
    CHECK(peak(g_a + 40, 4 * kBlk - 40) > 0.01f);  // and something after it
}

// --- pitch ----------------------------------------------------------------------------------

TEST(va_pitch_tracks_the_note) {
    const TrackParams p = patch();
    const int blocks = blocks_for(0.4f);
    const int from = int(0.05f * kRate);        // past the attack
    const int window = int(0.25f * kRate);

    VaMono low, high;
    low.init(kRate);
    high.init(kRate);
    low.prepare(p);
    high.prepare(p);
    low.trigger(hit(60));
    high.trigger(hit(72));

    clear(g_a, blocks * kBlk);
    clear(g_b, blocks * kBlk);
    render(low, p, g_a, blocks);
    render(high, p, g_b, blocks);

    const int lag60 = period_of(60);            // 367 samples: 130.81 Hz, note 60's sub
    const int lag72 = period_of(72);            // 183: an octave up, so half of it
    CHECK_EQ(lag60, 367);
    CHECK_EQ(lag72, 183);

    // Note 60 repeats at its own period and note 72 at half of it: the frequency doubled. The
    // absolute figure is pinned too, since lag60 is derived from 440 Hz and note 60.
    CHECK(periodicity(g_a + from, window, lag60) > 0.85f);
    CHECK(periodicity(g_b + from, window, lag72) > 0.85f);
    CHECK(periodicity(g_a + from, window, lag72) < 0.7f);    // note 60 does not repeat there
    CHECK(periodicity(g_a + from, window, 275) < 0.7f);      // nor at a wrong period between
    CHECK(periodicity(g_b + from, window, 137) < 0.7f);
}

TEST(va_pitch_follows_the_tuning_knob) {
    TrackParams flat = patch();
    TrackParams up = patch();
    TrackParams down = patch();
    up.set(kTun, 96);        // (96 - 64) * 24/64 = +12 semitones exactly
    down.set(kTun, 32);      // and -12

    const int blocks = blocks_for(0.4f);
    const int from = int(0.05f * kRate);
    const int window = int(0.25f * kRate);
    const int lag48 = period_of(48), lag60 = period_of(60), lag72 = period_of(72);

    VaMono a, b, c;
    a.init(kRate);
    b.init(kRate);
    c.init(kRate);
    a.prepare(flat);
    b.prepare(up);
    c.prepare(down);
    a.trigger(hit(60));
    b.trigger(hit(60));
    c.trigger(hit(60));

    // The same note on all three, so any pitch difference is the knob's doing.
    clear(g_a, blocks * kBlk);
    render(a, flat, g_a, blocks);
    CHECK_NEAR(a.pitch(), 60.0f, 0.01f);
    CHECK(periodicity(g_a + from, window, lag60) > 0.85f);
    CHECK(periodicity(g_a + from, window, lag72) < 0.7f);

    clear(g_a, blocks * kBlk);
    render(b, up, g_a, blocks);
    CHECK_NEAR(b.pitch(), 72.0f, 0.01f);        // TUN 96 is exactly +12 semitones
    CHECK(periodicity(g_a + from, window, lag72) > 0.85f);

    clear(g_a, blocks * kBlk);
    render(c, down, g_a, blocks);
    CHECK_NEAR(c.pitch(), 48.0f, 0.01f);        // and TUN 32 exactly -12
    CHECK(periodicity(g_a + from, window, lag48) > 0.85f);
    CHECK(periodicity(g_a + from, window, lag60) < 0.7f);
}

// --- the voice filter -----------------------------------------------------------------------

TEST(va_each_filter_mode_filters) {
    // ENV at 0 throughout, so the envelope is not quietly opening the filter under the test.
    TrackParams open = patch();
    open.set(kMod, kLp12);
    open.set(kCut, 127);

    TrackParams lp24 = open, lp12 = open, bp12 = open, hp12 = open;
    lp24.set(kMod, kLp24);
    lp24.set(kCut, 0);          // 35 Hz, far below note 72's 523 Hz
    lp12.set(kMod, kLp12);
    lp12.set(kCut, 0);
    bp12.set(kMod, kBp12);
    bp12.set(kCut, 0);          // the note sits far above the band
    hp12.set(kMod, kHp12);
    hp12.set(kCut, 127);        // 12 kHz, far above the note

    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;

    const TrackParams sets[5] = {open, lp24, lp12, bp12, hp12};
    float e[5] = {};
    for (int i = 0; i < 5; i++) {
        VaMono va;
        va.init(kRate);
        va.prepare(sets[i]);
        va.trigger(hit(72));
        clear(g_a, n);
        render(va, sets[i], g_a, blocks);
        CHECK(finite_within(g_a, n, 4.0f));
        e[i] = energy(g_a, n);
    }

    CHECK(e[0] > 1.0f);              // the open reference actually has signal in it
    CHECK(e[1] < 0.05f * e[0]);      // LP24 closed
    CHECK(e[2] < 0.25f * e[0]);      // LP12 closed
    CHECK(e[3] < 0.25f * e[0]);      // BP12, note far above the band
    CHECK(e[4] < 0.20f * e[0]);      // HP12 open, note far below the corner
    CHECK(e[1] < 0.5f * e[2]);       // and 24 dB attenuates more than 12 dB
}

TEST(va_lp24_attenuates_more_than_lp12) {
    // The same cutoff, two slopes: the only difference is the number of poles.
    TrackParams a = patch(), b = patch();
    a.set(kMod, kLp24);
    a.set(kCut, 20);
    b.set(kMod, kLp12);
    b.set(kCut, 20);

    const int blocks = blocks_for(0.25f);
    const int n = blocks * kBlk;

    VaMono four, two;
    four.init(kRate);
    two.init(kRate);
    four.prepare(a);
    two.prepare(b);
    four.trigger(hit(72));
    two.trigger(hit(72));

    clear(g_a, n);
    clear(g_b, n);
    render(four, a, g_a, blocks);
    render(two, b, g_b, blocks);

    CHECK(energy(g_b, n) > 0.0f);
    CHECK(energy(g_a, n) < 0.4f * energy(g_b, n));
    CHECK(sign_changes(g_a, n) < sign_changes(g_b, n));   // and it is duller, not just quieter
}

TEST(va_resonance_raises_the_response_near_cutoff) {
    // CUT 44 puts the base cutoff on note 60's fundamental; the accessor pins that rather than
    // leaving the test to guess at the knob curve.
    TrackParams flat = patch(), squelchy = patch();
    flat.set(kMod, kLp12);
    flat.set(kCut, 44);
    flat.set(kRes, 0);
    squelchy = flat;
    squelchy.set(kRes, 127);

    VaMono a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(flat);
    b.prepare(squelchy);
    CHECK_NEAR(a.base_cutoff_hz(), 261.6f, 20.0f);
    CHECK_NEAR(b.base_cutoff_hz(), 261.6f, 20.0f);

    const int blocks = blocks_for(0.25f);
    const int n = blocks * kBlk;
    a.trigger(hit(60));
    b.trigger(hit(60));
    clear(g_a, n);
    clear(g_b, n);
    render(a, flat, g_a, blocks);
    render(b, squelchy, g_b, blocks);

    CHECK(energy(g_a, n) > 0.0f);
    CHECK(energy(g_b, n) > 1.5f * energy(g_a, n));
    CHECK(finite_within(g_b, n, 4.0f));            // squelchy, not self-destructing
    CHECK(peak(g_b, n) < 1.5f);
}

TEST(va_full_resonance_is_finite_in_every_mode) {
    const uint8_t modes[4] = {kLp24, kLp12, kBp12, kHp12};
    for (int i = 0; i < 4; i++) {
        TrackParams p = patch();
        p.set(kMod, modes[i]);
        p.set(kRes, 127);
        p.set(kCut, 40);
        p.set(kEnv, 100);        // and sweeping across the cutoff while it rings

        VaMono va;
        va.init(kRate);
        va.prepare(p);
        va.trigger(hit(60, 127, true));
        const int blocks = blocks_for(0.4f);
        clear(g_a, blocks * kBlk);
        render(va, p, g_a, blocks);
        CHECK(finite_within(g_a, blocks * kBlk, 4.0f));
    }
}

TEST(va_cutoff_knob_spans_its_range) {
    VaMono va;
    va.init(kRate);
    TrackParams p = patch();

    p.set(kCut, 0);
    va.prepare(p);
    const float low = va.base_cutoff_hz();
    p.set(kCut, 64);
    va.prepare(p);
    const float mid = va.base_cutoff_hz();
    p.set(kCut, 127);
    va.prepare(p);
    const float high = va.base_cutoff_hz();

    CHECK_NEAR(low, 35.0f, 1.0f);
    CHECK(mid > 400.0f && mid < 1000.0f);     // usable in the middle, not bunched at one end
    CHECK(high > 10000.0f);
    CHECK(low < mid && mid < high);
}

TEST(va_mode_zones_are_weighted_so_the_default_is_a_low_pass) {
    CHECK(VaMono::mode_from(0) == VaMono::Mode::lp24);
    CHECK(VaMono::mode_from(63) == VaMono::Mode::lp24);
    CHECK(VaMono::mode_from(64) == VaMono::Mode::lp12);   // the default kit's untouched knob
    CHECK(VaMono::mode_from(95) == VaMono::Mode::lp12);
    CHECK(VaMono::mode_from(96) == VaMono::Mode::bp12);
    CHECK(VaMono::mode_from(111) == VaMono::Mode::bp12);
    CHECK(VaMono::mode_from(112) == VaMono::Mode::hp12);
    CHECK(VaMono::mode_from(127) == VaMono::Mode::hp12);
}

// --- glide ----------------------------------------------------------------------------------

TEST(va_glide_starts_at_the_old_note_and_arrives_at_the_new_one) {
    TrackParams p = patch();
    p.set(kGld, 110);        // about 260 ms
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    CHECK(va.glide_seconds() > 0.15f);

    clear(g_a, blocks_for(0.3f) * kBlk);
    va.trigger(hit(48));
    render(va, p, g_a, blocks_for(0.3f));
    CHECK_NEAR(va.pitch(), 48.0f, 0.2f);

    va.trigger(hit(72));
    clear(g_b, blocks_for(1.2f) * kBlk);
    render(va, p, g_b, 1);                 // one block: 1.33 ms into a 260 ms glide
    CHECK(va.pitch() < 50.0f);             // it starts near the note it left
    CHECK_NEAR(va.target_pitch(), 72.0f, 0.01f);

    const int rest = blocks_for(1.2f) - 1;
    render(va, p, g_b + kBlk, rest);
    CHECK_NEAR(va.pitch(), 72.0f, 0.2f);   // and arrives

    // The same thing heard rather than read: a second into the note the waveform repeats at
    // the new note's period, and 30 ms in -- with the glide still climbing -- it does not.
    const int lag72 = period_of(72);
    CHECK(periodicity(g_b + int(1.0f * kRate), int(0.2f * kRate), lag72) > 0.85f);
    CHECK(periodicity(g_b, int(0.03f * kRate), lag72) < 0.7f);
}

TEST(va_glide_at_zero_arrives_immediately) {
    TrackParams p = patch();
    p.set(kGld, 0);
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    CHECK_EQ(va.glide_seconds(), 0.0f);

    clear(g_a, blocks_for(0.2f) * kBlk);
    va.trigger(hit(48));
    render(va, p, g_a, blocks_for(0.2f));
    CHECK_NEAR(va.pitch(), 48.0f, 0.01f);

    va.trigger(hit(72));
    clear(g_b, blocks_for(0.4f) * kBlk);
    render(va, p, g_b, 1);
    CHECK_NEAR(va.pitch(), 72.0f, 0.01f);      // there on the first control step

    render(va, p, g_b + kBlk, blocks_for(0.4f) - 1);
    // There is no slew to hear either: the new note's period is already there 3 ms in, where
    // the glide test above still measures nothing of the kind.
    const int lag72 = period_of(72);
    CHECK(periodicity(g_b + 2 * kBlk, int(0.05f * kRate), lag72) > 0.85f);
    CHECK(periodicity(g_b + int(0.25f * kRate), int(0.1f * kRate), lag72) > 0.85f);
}

// --- slide ----------------------------------------------------------------------------------

TEST(va_slide_does_not_restart_the_amplitude_envelope) {
    TrackParams p = patch();
    p.set(kDec, 90);        // about 0.55 s, so there is a decay to interrupt
    p.set(kGld, 0);         // a slide still slews: it has a floor of its own

    const int first = blocks_for(0.15f);
    const int second = blocks_for(0.1f);
    const int window = int(0.015f * kRate);

    float before[2] = {}, after[2] = {}, level_before[2] = {}, level_after[2] = {};
    for (int slid = 0; slid < 2; slid++) {
        VaMono va;
        va.init(kRate);
        va.prepare(p);
        va.trigger(hit(60));
        clear(g_a, first * kBlk);
        const int n = render(va, p, g_a, first);
        before[slid] = peak(g_a + n - window, window);
        level_before[slid] = va.amp_level();

        va.trigger(hit(62, 100, false, slid != 0));
        clear(g_b, second * kBlk);
        render(va, p, g_b, second);
        // Measured a little after the trig, so the 2.5 ms attack has had time to land.
        after[slid] = peak(g_b + int(0.006f * kRate), window);
        level_after[slid] = va.amp_level();
    }

    CHECK(before[0] > 0.01f);
    CHECK(before[1] > 0.01f);
    CHECK(after[0] > 1.5f * before[0]);     // not slid: the envelope restarted, so it is loud
    CHECK(after[1] < before[1]);            // slid: it carried on decaying
    CHECK(level_after[0] > level_before[0]);
    CHECK(level_after[1] < level_before[1]);
}

TEST(va_slide_still_sounds_when_nothing_is_playing) {
    // A slide has nothing to slide from if the voice is silent, so it has to start normally.
    TrackParams p = patch();
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, true));
    const int blocks = blocks_for(0.2f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK(peak(g_a, blocks * kBlk) > 0.05f);
}

TEST(va_slide_slews_the_pitch_even_with_glide_at_zero) {
    TrackParams p = patch();
    p.set(kGld, 0);
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(48));
    clear(g_a, blocks_for(0.2f) * kBlk);
    render(va, p, g_a, blocks_for(0.2f));

    va.trigger(hit(72, 100, false, true));
    clear(g_b, blocks_for(0.4f) * kBlk);
    render(va, p, g_b, 1);
    CHECK(va.pitch() < 60.0f);             // a slide is a slide even when GLD says 0
    render(va, p, g_b + kBlk, blocks_for(0.4f) - 1);
    CHECK_NEAR(va.pitch(), 72.0f, 0.2f);
}

// --- accent ---------------------------------------------------------------------------------

TEST(va_accent_changes_more_than_volume) {
    TrackParams p = patch();
    p.set(kMod, kLp24);
    p.set(kCut, 30);         // closed enough that opening it is audible
    p.set(kRes, 60);
    p.set(kEnv, 70);
    p.set(kDec, 70);

    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;

    VaMono plain, accented;
    plain.init(kRate);
    accented.init(kRate);
    plain.prepare(p);
    accented.prepare(p);
    plain.trigger(hit(60, 100, false));
    accented.trigger(hit(60, 100, true));
    clear(g_a, n);
    clear(g_b, n);
    render(plain, p, g_a, blocks);
    render(accented, p, g_b, blocks);

    const float pa = peak(g_a, n);
    const float pb = peak(g_b, n);
    CHECK(pa > 0.01f);
    CHECK(pb > pa);                  // louder, which is the least of what it does

    // Scale the plain note up to the accented note's peak: whatever is left is not volume.
    const float scale = pb / pa;
    for (int i = 0; i < n; i++) g_a[i] *= scale;
    CHECK_NEAR(peak(g_a, n), pb, 1e-4f);

    // Spectral content: the accent opens the filter and lifts the synced oscillator and the
    // noise, so the accented note is brighter at the same peak.
    CHECK(sign_changes(g_b, n) > sign_changes(g_a, n) * 6 / 5);

    // Attack shape: its envelope is sharper and its sweep shorter, so the energy sits further
    // forward in the note even after the levels are matched.
    const int head = int(0.03f * kRate);
    const float share_plain = energy(g_a, head) / energy(g_a, n);
    const float share_accent = energy(g_b, head) / energy(g_b, n);
    CHECK(share_accent > share_plain);
}

// --- monophony ------------------------------------------------------------------------------

TEST(va_second_trigger_steals_the_voice_instead_of_layering) {
    const TrackParams p = patch();          // DEC 127, so the first note is still going strong
    const int head = blocks_for(0.1f);
    const int tail = blocks_for(0.4f);
    const int n = (head + tail) * kBlk;

    VaMono one, two;
    one.init(kRate);
    two.init(kRate);
    one.prepare(p);
    two.prepare(p);

    one.trigger(hit(60));
    two.trigger(hit(60));
    clear(g_a, n);
    clear(g_b, n);
    render(one, p, g_a, head);
    render(two, p, g_b, head);

    // A fifth up, while the first note is loud: a polyphonic machine would now be twice as big.
    two.trigger(hit(67));
    render(one, p, g_a + head * kBlk, tail);
    render(two, p, g_b + head * kBlk, tail);

    CHECK(energy(g_a, n) > 1.0f);
    CHECK(energy(g_b, n) < 1.5f * energy(g_a, n));   // one voice, not two
    CHECK(peak(g_b, n) < 1.5f * peak(g_a, n));

    // And it really is the new note that is sounding, not the old one or a sum of the two.
    const int from = head * kBlk + int(0.2f * kRate);
    const int window = int(0.15f * kRate);
    CHECK(periodicity(g_b + from, window, period_of(67)) > 0.85f);
    CHECK(periodicity(g_b + from, window, period_of(60)) < 0.7f);
}

TEST(va_a_trigger_replaces_one_that_has_not_started_yet) {
    // Monophonic means one note, so a second trig arriving inside the same block before the
    // first has reached its frame takes its place rather than queueing behind it.
    const TrackParams p = patch();
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, false, 48));
    va.trigger(hit(67, 100, false, false, 16));

    const int blocks = blocks_for(0.3f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK_EQ(peak(g_a, 16), 0.0f);                   // it starts on the SECOND trig's frame
    CHECK(peak(g_a, blocks * kBlk) > 0.05f);
    CHECK_NEAR(va.target_pitch(), 67.0f, 0.01f);     // and plays the second trig's note
    const int from = int(0.1f * kRate);
    const int window = int(0.15f * kRate);
    CHECK(periodicity(g_a + from, window, period_of(67)) > 0.85f);
}

TEST(va_overlapping_triggers_at_extremes_stay_finite) {
    const uint8_t values[2] = {0, 127};
    for (int v = 0; v < 2; v++) {
        TrackParams p = uniform(values[v]);
        VaMono va;
        va.init(kRate);
        va.prepare(p);

        // Notes and velocities at both ends, accent and slide on, several hits in one block.
        va.trigger(hit(0, 1, true, false, 0));
        va.trigger(hit(127, 127, true, true, 16));
        va.trigger(hit(127, 1, false, true, 17));
        va.trigger(hit(60, 127, true, false, 63));

        const int blocks = blocks_for(0.5f);
        clear(g_a, blocks * kBlk);
        render(va, p, g_a, blocks);
        CHECK(finite_within(g_a, blocks * kBlk, 4.0f));

        va.trigger(hit(127, 127, true, true));
        va.trigger(hit(0, 127, true, false));
        clear(g_b, blocks * kBlk);
        render(va, p, g_b, blocks);
        CHECK(finite_within(g_b, blocks * kBlk, 4.0f));
    }
}

TEST(va_all_zero_parameters_are_silent_not_broken) {
    TrackParams p = uniform(0);          // LVL 0
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 127, true));
    const int blocks = blocks_for(0.2f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK(finite_within(g_a, blocks * kBlk, 4.0f));
    CHECK_EQ(peak(g_a, blocks * kBlk), 0.0f);
}

// --- the buffer contract --------------------------------------------------------------------

TEST(va_render_add_accumulates_and_never_clears) {
    const TrackParams p = patch();
    const int blocks = 24;
    const int n = blocks * kBlk;

    VaMono a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(p);
    b.prepare(p);
    a.trigger(hit(60));
    b.trigger(hit(60));

    clear(g_a, n);
    fill(g_b, n, 0.25f);
    render(a, p, g_a, blocks);
    render(b, p, g_b, blocks);

    for (int i = 0; i < n; i += 37) {
        CHECK_NEAR(g_b[i] - 0.25f, g_a[i], 1e-6f);
    }
    CHECK(peak(g_a, n) > 0.05f);       // and there was something to add
}

TEST(va_kill_all_silences_at_once) {
    const TrackParams p = patch();
    VaMono va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60));
    clear(g_a, blocks_for(0.1f) * kBlk);
    render(va, p, g_a, blocks_for(0.1f));
    CHECK(va.active());

    va.kill_all();
    CHECK(!va.active());
    clear(g_b, 16 * kBlk);
    render(va, p, g_b, 16);
    CHECK_EQ(peak(g_b, 16 * kBlk), 0.0f);
}

TEST(va_release_all_leaves_a_tail) {
    const TrackParams p = patch();      // DEC 127, so the note would hold for seconds

    VaMono released, killed;
    released.init(kRate);
    killed.init(kRate);
    released.prepare(p);
    killed.prepare(p);
    released.trigger(hit(60));
    killed.trigger(hit(60));
    clear(g_a, blocks_for(0.1f) * kBlk);
    clear(g_b, blocks_for(0.1f) * kBlk);
    render(released, p, g_a, blocks_for(0.1f));
    render(killed, p, g_b, blocks_for(0.1f));

    released.release_all();
    killed.kill_all();

    const int blocks = blocks_for(0.05f);
    const int n = blocks * kBlk;
    clear(g_a, n);
    clear(g_b, n);
    render(released, p, g_a, blocks);
    render(killed, p, g_b, blocks);

    CHECK(energy(g_a, n) > 0.0f);           // a tail, not a cut
    CHECK(peak(g_a, 2 * kBlk) > 0.01f);
    CHECK_EQ(peak(g_b, n), 0.0f);

    // And the tail ends: the release reaches idle, which is what frees the voice.
    render(released, p, g_a, blocks_for(0.5f));
    CHECK(!released.active());
}

// --- the parameter surface ------------------------------------------------------------------

TEST(va_parameter_names_are_three_upper_case_letters) {
    VaMono va;
    va.init(kRate);
    CHECK_EQ(va.used_params(), 8);

    const char *const expected[kParams] = {
        "TUN", "MOD", "CUT", "RES", "---", "---", "---", "---",
        "DEC", "ENV", "GLD", "LVL", "---", "---", "---", "---",
    };
    for (int i = 0; i < kParams; i++) {
        const char *name = va.param_name(i);
        CHECK_EQ(int(std::strlen(name)), 3);
        CHECK(std::strcmp(name, expected[i]) == 0);
        for (int c = 0; c < 3; c++) {
            const char ch = name[c];
            CHECK(ch == '-' || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
        }
    }
    CHECK(std::strcmp(va.param_name(-1), "---") == 0);
    CHECK(std::strcmp(va.param_name(kParams), "---") == 0);
}

TEST(va_decay_knob_spans_its_range) {
    VaMono va;
    va.init(kRate);
    TrackParams p = patch();

    p.set(kDec, 0);
    va.prepare(p);
    const float shortest = va.decay_seconds();
    p.set(kDec, 127);
    va.prepare(p);
    const float longest = va.decay_seconds();

    CHECK_NEAR(shortest, 0.015f, 0.002f);
    CHECK(longest > 2.0f && longest < 3.0f);
}

TEST(va_longer_decay_makes_a_longer_note) {
    TrackParams shorter = patch(), longer = patch();
    shorter.set(kDec, 20);
    longer.set(kDec, 90);

    const int blocks = blocks_for(0.6f);
    const int n = blocks * kBlk;

    VaMono a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(shorter);
    b.prepare(longer);
    a.trigger(hit(60));
    b.trigger(hit(60));
    clear(g_a, n);
    clear(g_b, n);
    render(a, shorter, g_a, blocks);
    render(b, longer, g_b, blocks);

    CHECK(energy(g_b, n) > 2.0f * energy(g_a, n));
    CHECK(!a.active());      // the short note has finished inside the window
    CHECK(b.active());       // the long one has not
}

TEST(va_env_amount_opens_the_filter) {
    TrackParams closed = patch(), swept = patch();
    closed.set(kMod, kLp24);
    closed.set(kCut, 20);
    closed.set(kEnv, 0);
    swept = closed;
    swept.set(kEnv, 127);

    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;

    VaMono a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(closed);
    b.prepare(swept);
    a.trigger(hit(60));
    b.trigger(hit(60));
    clear(g_a, n);
    clear(g_b, n);
    render(a, closed, g_a, blocks);
    render(b, swept, g_b, blocks);

    CHECK(energy(g_b, n) > 4.0f * energy(g_a, n));
    CHECK(sign_changes(g_b, n) > sign_changes(g_a, n));
    CHECK(finite_within(g_b, n, 4.0f));
}
