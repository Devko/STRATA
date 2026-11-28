// KICK, SNARE and HAT: the three drum machines in dsp/.
//
// These test measurable properties rather than sample values -- that a hit sounds, that it dies
// away, that a knob moves what it says it moves -- plus the published constants the machines
// follow (docs/SOUND.md), because a test that pins a reference number is what stops a later
// tidy-up from quietly changing the sound.
#include "check.hpp"

#include <enaga/abi.h>

#include "hat.hpp"
#include "kick.hpp"
#include "snare.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kCapture = ENAGA_SAMPLE_RATE * 3;      // 3 s, long enough for the longest tail

// Static, because three seconds of audio does not belong on a stack frame.
float g_buffer[kCapture];
float g_other[kCapture];

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

Trig hit(uint8_t note = 60, uint8_t velocity = 100, uint32_t frame = 0, bool accent = false) {
    Trig t;
    t.note = note;
    t.velocity = velocity;
    t.frame = frame;
    t.accent = accent;
    return t;
}

void clear(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = 0.0f;
}

void fill(float *x, int n, float value) {
    for (int i = 0; i < n; i++) x[i] = value;
}

// Render `blocks` blocks of 64 frames, re-reading the parameters each block exactly as audio()
// does. The buffer is the caller's to clear.
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

int sign_changes(const float *x, int n) {
    int count = 0;
    for (int i = 1; i < n; i++) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) count++;
    }
    return count;
}

// The envelope read as the peak inside short windows: enough for a decaying drum, and it needs
// no filtering or Hilbert transform.
float window_peak(const float *x, int n, int from, int window) {
    if (from + window > n) return 0.0f;
    return peak(x + from, window);
}

// The same for noise, where energy is steadier than the peak.
float window_energy(const float *x, int n, int from, int window) {
    if (from + window > n) return 0.0f;
    return energy(x + from, window);
}

// Where the windowed envelope first falls below `fraction` of its largest value, in samples from
// the start of the signal.
int envelope_falls_below(const float *x, int n, int window, float fraction) {
    float best = 0.0f;
    for (int i = 0; i + window <= n; i += window) {
        const float p = peak(x + i, window);
        if (p > best) best = p;
    }
    const float target = best * fraction;
    for (int i = 0; i + window <= n; i += window) {
        if (peak(x + i, window) < target) return i;
    }
    return n;
}

// Where a given share of the whole signal's energy has arrived, in samples. A steadier measure
// of "how long did that take" than a threshold crossing, because it does not care about the
// shape in between.
int energy_quantile(const float *x, int n, float fraction) {
    const float target = energy(x, n) * fraction;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += x[i] * x[i];
        if (sum >= target) return i;
    }
    return n;
}

// A harmonic-content meter that does not care about amplitude or about a slow envelope. A pure
// sampled sinusoid satisfies x[n+1] + x[n-1] == 2 cos(w) x[n] exactly, whatever its amplitude,
// so the energy left in that recurrence -- relative to the signal's own -- is zero for a pure
// sine at `frequency` and grows with every harmonic sitting on top of it. Far more selective
// here than a high-pass would be: the third harmonic of this body is 26 dB down, which no
// cheap filter separates from the fundamental an octave and a half below it.
float harmonic_residual(const float *x, int from, int count, float frequency) {
    const float twice_cosine = 2.0f * enaga::dsp::cos_turns(frequency / kRate);
    float residual = 0.0f;
    float total = 0.0f;
    for (int i = from; i < from + count; i++) {
        const float r = x[i + 1] + x[i - 1] - twice_cosine * x[i];
        residual += r * r;
        total += x[i] * x[i];
    }
    return total > 0.0f ? residual / total : 0.0f;
}

// The three candidate bodies, from the reference's own formulas, so a test can say which one a
// machine is actually making rather than only that two settings differ.
enum class Body { clean_sine, rational_fold, bare_triangle };

void synthesise(float *out, int n, float frequency, Body body) {
    const float dt = frequency / kRate;
    float phase = 0.25f;
    for (int i = 0; i < n; i++) {
        phase += dt;
        phase -= phase >= 1.0f ? 1.0f : 0.0f;
        const float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
        const float magnitude = triangle < 0.0f ? -triangle : triangle;
        switch (body) {
        case Body::clean_sine:
            out[i] = enaga::dsp::sin_turns(phase + 0.75f);
            break;
        case Body::rational_fold:
            out[i] = 2.0f * triangle * enaga::dsp::reciprocal(1.0f + magnitude);
            break;
        case Body::bare_triangle:
            out[i] = triangle;
            break;
        }
    }
}

// Counts how many times the windowed envelope rises back over a threshold: the clap's bursts.
int count_bursts(const float *x, int n, int window, float fraction) {
    float best = 0.0f;
    for (int i = 0; i + window <= n; i += window) {
        const float p = peak(x + i, window);
        if (p > best) best = p;
    }
    const float threshold = best * fraction;
    int bursts = 0;
    bool above = false;
    for (int i = 0; i + window <= n; i += window) {
        const bool now = peak(x + i, window) > threshold;
        if (now && !above) bursts++;
        above = now;
    }
    return bursts;
}

// --- the properties every machine has to have ----------------------------

template <class M>
void silent_until_triggered() {
    M m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    const int n = 8 * kBlk;
    clear(g_buffer, n);
    CHECK(!m.active());
    render_blocks(m, p, g_buffer, 8);
    CHECK(!m.active());
    CHECK_EQ(peak(g_buffer, n), 0.0f);
}

template <class M>
void a_trigger_sounds() {
    M m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    m.prepare(p);
    m.trigger(hit());
    CHECK(m.active());
    const int n = 32 * kBlk;
    clear(g_buffer, n);
    render_blocks(m, p, g_buffer, 32);
    CHECK(m.active());
    CHECK(peak(g_buffer, n) > 0.01f);
}

template <class M>
void decays_back_to_silence() {
    M m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(8, 0);   // the shortest decay every machine has on AMP 8
    m.prepare(p);
    m.trigger(hit());
    const int blocks = 600;   // 800 ms: longer than the shortest tail of any of them
    clear(g_buffer, blocks * kBlk);
    render_blocks(m, p, g_buffer, blocks);
    CHECK(!m.active());
    // The last 16 blocks must be exactly zero: once nothing sounds, render_add adds nothing.
    CHECK_EQ(peak(g_buffer + (blocks - 16) * kBlk, 16 * kBlk), 0.0f);
}

template <class M>
void stays_finite_at_parameter_extremes() {
    for (int extreme = 0; extreme < 2; extreme++) {
        const TrackParams p = uniform(extreme == 0 ? uint8_t(0) : uint8_t(127));
        M m;
        m.init(kRate);
        const int blocks = 400;
        clear(g_buffer, blocks * kBlk);
        // Low, middle and high notes, quiet and loud, accented, and overlapping hits so both
        // voices sound at once and the steal path is taken.
        m.prepare(p);
        m.trigger(hit(24, 1));
        m.trigger(hit(60, 127, 17, true));
        for (int b = 0; b < blocks; b++) {
            if (b == 3) m.trigger(hit(96, 127, 40, true));
            if (b == 4) m.trigger(hit(127, 64));
            if (b == 40) m.trigger(hit(60, 127));
            m.prepare(p);
            m.render_add(g_buffer + b * kBlk, kBlk);
        }
        CHECK(finite_within(g_buffer, blocks * kBlk, 6.0f));
    }
}

template <class M>
void louder_velocity_is_a_louder_peak() {
    const TrackParams p = uniform(64);
    const int blocks = 64;
    float peaks[3] = {};
    const uint8_t velocities[3] = {20, 70, 127};
    for (int i = 0; i < 3; i++) {
        M m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, velocities[i]));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        peaks[i] = peak(g_buffer, blocks * kBlk);
    }
    CHECK(peaks[0] < peaks[1]);
    CHECK(peaks[1] < peaks[2]);
}

template <class M>
void render_add_accumulates() {
    // Two fresh machines, so their noise generators start from the same seed and the only
    // difference between the runs is what was already in the buffer.
    const TrackParams p = uniform(64);
    const int blocks = 24;
    const int n = blocks * kBlk;

    M a;
    a.init(kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_buffer, n);
    render_blocks(a, p, g_buffer, blocks);

    M b;
    b.init(kRate);
    b.prepare(p);
    b.trigger(hit());
    fill(g_other, n, 0.25f);
    render_blocks(b, p, g_other, blocks);

    for (int i = 0; i < n; i++) CHECK_NEAR(g_other[i], g_buffer[i] + 0.25f, 1e-6);
}

template <class M>
void kill_all_silences_at_once() {
    M m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    m.prepare(p);
    m.trigger(hit(60, 127));
    clear(g_buffer, 8 * kBlk);
    render_blocks(m, p, g_buffer, 4);
    CHECK(peak(g_buffer, 4 * kBlk) > 0.01f);

    m.kill_all();
    CHECK(!m.active());
    render_blocks(m, p, g_buffer + 4 * kBlk, 4);
    CHECK_EQ(peak(g_buffer + 4 * kBlk, 4 * kBlk), 0.0f);
}

template <class M>
void release_all_lets_the_tail_finish() {
    M m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(8, 100);   // a long decay, so there is a tail to let finish
    m.prepare(p);
    m.trigger(hit(60, 127));
    clear(g_buffer, 64 * kBlk);
    render_blocks(m, p, g_buffer, 4);

    m.release_all();
    CHECK(m.active());   // a one-shot's decay is its release: the tail keeps going
    render_blocks(m, p, g_buffer + 4 * kBlk, 12);
    CHECK(peak(g_buffer + 4 * kBlk, 12 * kBlk) > 0.005f);
    CHECK(m.active());
}

template <class M>
void a_trig_frame_offset_delays_the_hit() {
    M m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    m.prepare(p);
    m.trigger(hit(60, 127, 40));
    clear(g_buffer, kBlk);
    m.render_add(g_buffer, kBlk);
    CHECK_EQ(peak(g_buffer, 40), 0.0f);          // silence up to the trig's frame
    CHECK(peak(g_buffer + 40, kBlk - 40) > 0.0f);
}

template <class M>
void a_long_call_matches_the_same_frames_in_blocks() {
    // render_add must cut a longer call into its own slices rather than overrun a buffer, and
    // the result has to be the same as the caller doing the slicing.
    const TrackParams p = uniform(64);
    const int n = 200;

    M a;
    a.init(kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_buffer, n);
    a.render_add(g_buffer, n);

    M b;
    b.init(kRate);
    b.prepare(p);
    b.trigger(hit());
    clear(g_other, n);
    b.render_add(g_other, 64);
    b.render_add(g_other + 64, 64);
    b.render_add(g_other + 128, 64);
    b.render_add(g_other + 192, 8);

    for (int i = 0; i < n; i++) CHECK_NEAR(g_other[i], g_buffer[i], 1e-6);
}

template <class M>
void retriggering_does_not_step_the_waveform() {
    // Both voices busy, then a third hit: the stolen voice is faded rather than cut, so no
    // sample-to-sample jump should be anywhere near full scale.
    M m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(8, 110);   // long decays, so the third hit has to steal a loud voice
    const int blocks = 96;
    clear(g_buffer, blocks * kBlk);
    m.prepare(p);
    m.trigger(hit(60, 127));
    for (int b = 0; b < blocks; b++) {
        if (b == 8) m.trigger(hit(60, 127));
        if (b == 16) m.trigger(hit(60, 127));
        if (b == 24) m.trigger(hit(60, 127));
        m.prepare(p);
        m.render_add(g_buffer + b * kBlk, kBlk);
    }
    const int n = blocks * kBlk;
    float step = 0.0f;
    for (int i = 1; i < n; i++) {
        const float d = g_buffer[i] - g_buffer[i - 1];
        const float a = d < 0.0f ? -d : d;
        if (a > step) step = a;
    }
    // A hat's squares and any noise move fast by nature, so this is a ceiling on a glitch and
    // not a smoothness measure: a hard cut of a loud voice would land well above it.
    CHECK(step < 1.0f);
    CHECK(finite_within(g_buffer, n, 6.0f));
}

template <class M>
void names_are_three_characters_the_font_has() {
    M m;
    m.init(kRate);
    CHECK_EQ(m.used_params(), 8);
    for (int i = 0; i < kParams; i++) {
        const char *name = m.param_name(i);
        CHECK(name != nullptr);
        int length = 0;
        bool legal = true;
        for (const char *c = name; *c != '\0'; c++) {
            length++;
            const bool upper = *c >= 'A' && *c <= 'Z';
            const bool digit = *c >= '0' && *c <= '9';
            if (!upper && !digit && *c != '-') legal = false;
        }
        CHECK_EQ(length, 3);
        CHECK(legal);
    }
    // The pages a machine does not own are blanked, the ones it does are named.
    for (int i = 4; i < 8; i++) CHECK_EQ(m.param_name(i)[0], '-');
    for (int i = 12; i < 16; i++) CHECK_EQ(m.param_name(i)[0], '-');
    for (int i = 0; i < 4; i++) CHECK(m.param_name(i)[0] != '-');
    for (int i = 8; i < 12; i++) CHECK(m.param_name(i)[0] != '-');
}

}  // namespace

// --- the shared contract, once per machine --------------------------------

TEST(kick_is_silent_until_it_is_triggered) { silent_until_triggered<Kick>(); }
TEST(snare_is_silent_until_it_is_triggered) { silent_until_triggered<Snare>(); }
TEST(hat_is_silent_until_it_is_triggered) { silent_until_triggered<Hat>(); }

TEST(kick_sounds_when_triggered) { a_trigger_sounds<Kick>(); }
TEST(snare_sounds_when_triggered) { a_trigger_sounds<Snare>(); }
TEST(hat_sounds_when_triggered) { a_trigger_sounds<Hat>(); }

TEST(kick_decays_back_to_silence) { decays_back_to_silence<Kick>(); }
TEST(snare_decays_back_to_silence) { decays_back_to_silence<Snare>(); }
TEST(hat_decays_back_to_silence) { decays_back_to_silence<Hat>(); }

TEST(kick_stays_finite_at_parameter_extremes) { stays_finite_at_parameter_extremes<Kick>(); }
TEST(snare_stays_finite_at_parameter_extremes) { stays_finite_at_parameter_extremes<Snare>(); }
TEST(hat_stays_finite_at_parameter_extremes) { stays_finite_at_parameter_extremes<Hat>(); }

TEST(kick_gets_louder_with_velocity) { louder_velocity_is_a_louder_peak<Kick>(); }
TEST(snare_gets_louder_with_velocity) { louder_velocity_is_a_louder_peak<Snare>(); }
TEST(hat_gets_louder_with_velocity) { louder_velocity_is_a_louder_peak<Hat>(); }

TEST(kick_render_add_accumulates) { render_add_accumulates<Kick>(); }
TEST(snare_render_add_accumulates) { render_add_accumulates<Snare>(); }
TEST(hat_render_add_accumulates) { render_add_accumulates<Hat>(); }

TEST(kick_kill_all_silences_at_once) { kill_all_silences_at_once<Kick>(); }
TEST(snare_kill_all_silences_at_once) { kill_all_silences_at_once<Snare>(); }
TEST(hat_kill_all_silences_at_once) { kill_all_silences_at_once<Hat>(); }

TEST(kick_release_all_lets_the_tail_finish) { release_all_lets_the_tail_finish<Kick>(); }
TEST(snare_release_all_lets_the_tail_finish) { release_all_lets_the_tail_finish<Snare>(); }
TEST(hat_release_all_lets_the_tail_finish) { release_all_lets_the_tail_finish<Hat>(); }

TEST(kick_honours_the_trig_frame) { a_trig_frame_offset_delays_the_hit<Kick>(); }
TEST(snare_honours_the_trig_frame) { a_trig_frame_offset_delays_the_hit<Snare>(); }
TEST(hat_honours_the_trig_frame) { a_trig_frame_offset_delays_the_hit<Hat>(); }

TEST(kick_renders_a_long_call_in_slices) { a_long_call_matches_the_same_frames_in_blocks<Kick>(); }
TEST(snare_renders_a_long_call_in_slices) {
    a_long_call_matches_the_same_frames_in_blocks<Snare>();
}
TEST(hat_renders_a_long_call_in_slices) { a_long_call_matches_the_same_frames_in_blocks<Hat>(); }

TEST(kick_retriggers_without_a_step) { retriggering_does_not_step_the_waveform<Kick>(); }
TEST(snare_retriggers_without_a_step) { retriggering_does_not_step_the_waveform<Snare>(); }
TEST(hat_retriggers_without_a_step) { retriggering_does_not_step_the_waveform<Hat>(); }

TEST(kick_parameter_names_fit_the_font) { names_are_three_characters_the_font_has<Kick>(); }
TEST(snare_parameter_names_fit_the_font) { names_are_three_characters_the_font_has<Snare>(); }
TEST(hat_parameter_names_fit_the_font) { names_are_three_characters_the_font_has<Hat>(); }

// --- levels ---------------------------------------------------------------

namespace {

template <class M>
float peak_of_one_hit(const TrackParams &p, bool accent) {
    const int blocks = 400;
    M m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, 127, 0, accent));
    clear(g_buffer, blocks * kBlk);
    render_blocks(m, p, g_buffer, blocks);
    return peak(g_buffer, blocks * kBlk);
}

}  // namespace

TEST(machines_land_at_a_usable_level) {
    // Each machine has one trim constant, so that at the default parameter set it arrives at a
    // sensible level with no tweaking and the three sit in the same place next to each other --
    // what docs/SOUND.md asks for. This is the regression guard on those trims.
    const TrackParams mid = uniform(64);
    const TrackParams loud = uniform(127);

    const float kick = peak_of_one_hit<Kick>(mid, false);
    const float snare = peak_of_one_hit<Snare>(mid, false);
    const float hat = peak_of_one_hit<Hat>(mid, false);
    CHECK(kick > 0.1f && kick < 0.8f);
    CHECK(snare > 0.1f && snare < 0.8f);
    CHECK(hat > 0.1f && hat < 0.8f);

    // And with every knob up and an accent, loud but not absurd: the insert chain's drive and
    // the master limiter are after this.
    CHECK(peak_of_one_hit<Kick>(loud, true) < 2.0f);
    CHECK(peak_of_one_hit<Snare>(loud, true) < 2.0f);
    CHECK(peak_of_one_hit<Hat>(loud, true) < 2.0f);
    CHECK(peak_of_one_hit<Kick>(loud, true) > 0.5f);
    CHECK(peak_of_one_hit<Snare>(loud, true) > 0.5f);
    CHECK(peak_of_one_hit<Hat>(loud, true) > 0.5f);
}

TEST(an_accent_is_more_than_volume) {
    // The reference turns accent into the envelope's trigger level, which feeds a non-linear
    // stage, so an accented hit should not be just a scaled copy of a plain one.
    const TrackParams p = uniform(64);
    const int blocks = 64;

    Hat plain;
    plain.init(kRate);
    plain.prepare(p);
    plain.trigger(hit(60, 100, 0, false));
    clear(g_buffer, blocks * kBlk);
    render_blocks(plain, p, g_buffer, blocks);

    Hat accented;
    accented.init(kRate);
    accented.prepare(p);
    accented.trigger(hit(60, 100, 0, true));
    clear(g_other, blocks * kBlk);
    render_blocks(accented, p, g_other, blocks);

    const float a = peak(g_buffer, blocks * kBlk);
    const float b = peak(g_other, blocks * kBlk);
    CHECK(b > a);
    // Scale the plain hit up to the accented one's peak: if accent were only volume the two
    // would then match sample for sample. Through the asymmetric VCA they do not.
    const float scale = b / a;
    float worst = 0.0f;
    for (int i = 0; i < blocks * kBlk; i++) {
        const float d = g_other[i] - scale * g_buffer[i];
        const float m = d < 0.0f ? -d : d;
        if (m > worst) worst = m;
    }
    CHECK(worst > 0.02f * b);
}

// --- KICK -----------------------------------------------------------------

TEST(kick_tunes_to_55_hz_in_the_middle_of_the_knob) {
    Kick m;
    m.init(kRate);
    TrackParams p = uniform(64);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 55.0f, 0.6);
    p.set(0, 0);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 25.0f, 0.3);
    p.set(0, 127);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 120.0f, 1.5);
}

TEST(kick_pitch_envelope_sweeps_down) {
    Kick m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(1, 127);   // FMA: the full sweep
    p.set(2, 64);    // FMD: a 32 ms sweep, so there is still motion to measure
    p.set(3, 0);     // CLK off: the click's noise would add crossings of its own
    p.set(8, 127);   // a long decay, so the late window still has signal
    p.set(9, 127);   // TON wide open
    p.set(10, 0);    // DRT clean
    m.prepare(p);
    m.trigger(hit(96, 127));   // high enough that a short window holds several cycles

    const int blocks = 200;
    clear(g_buffer, blocks * kBlk);
    render_blocks(m, p, g_buffer, blocks);

    const int early = sign_changes(g_buffer + 128, 512);
    const int late = sign_changes(g_buffer + 4096, 512);
    CHECK(early > 2 * late);   // the sweep is at least an octave of it by then
    CHECK(late > 0);
}

TEST(kick_body_follows_the_trig_note) {
    Kick m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(1, 0);     // no sweep: the note is the only thing setting the pitch
    p.set(3, 0);
    p.set(8, 127);
    p.set(9, 127);
    const int blocks = 64;

    m.prepare(p);
    m.trigger(hit(72, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(m, p, g_buffer, blocks);
    const int high = sign_changes(g_buffer + 256, 2048);

    Kick n;
    n.init(kRate);
    n.prepare(p);
    n.trigger(hit(60, 127));
    clear(g_other, blocks * kBlk);
    render_blocks(n, p, g_other, blocks);
    const int low = sign_changes(g_other + 256, 2048);

    // An octave up is twice the crossings, within the window's own rounding.
    CHECK(high > low);
    CHECK_NEAR(float(high), 2.0f * float(low), 0.15 * double(high));
}

TEST(kick_decay_covers_the_references_twenty_to_640_ms) {
    // The reference's amplitude time constant is 20 ms * 2^(5 * decay^2). The energy of an
    // exponentially decaying tone is proportional to its time constant, so the energy ratio
    // across the knob pins both the range and the squaring of `decay`.
    TrackParams p = uniform(64);
    p.set(1, 0);     // no sweep and no click: just the body, so the energy is the envelope's
    p.set(3, 0);
    p.set(9, 127);
    const int blocks = 1400;   // 1.9 s: over 99% of the energy even at the longest setting
    float e[3] = {};
    const uint8_t decays[3] = {0, 64, 127};
    for (int i = 0; i < 3; i++) {
        p.set(8, decays[i]);
        Kick m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(72, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        e[i] = energy(g_buffer, blocks * kBlk);
    }
    CHECK(e[0] > 0.0f);
    // 640 / 20 = 32 at the top of the knob.
    CHECK_NEAR(e[2] / e[0], 32.0f, 8.0);
    // 20 ms * 2^(5 * 0.504^2) = 48 ms in the middle, which is 2.4. Without the squaring it
    // would be 2^2.52 = 5.7, so this is the test that `decay` is squared.
    CHECK_NEAR(e[1] / e[0], 2.4f, 0.7);
}

TEST(kick_dirtiness_runs_from_a_clean_sine_to_the_rational_fold) {
    // The reference's body is lerp(clean sine, rational soft sine, dirtiness), so DRT 0 has to
    // be a pure sine and DRT 127 the fold. The bare triangle is only the intermediate the fold
    // is computed from and must not appear at either end. Measured against all three waveforms
    // synthesised here from the reference's formulas.
    TrackParams p = uniform(64);
    p.set(0, 127);   // f0 at the top of TUN, and note 96 on top of that: the meter needs a
    p.set(1, 0);     // usable w. No sweep, so the frequency is one known constant.
    p.set(3, 0);     // no click and no attack noise
    p.set(8, 127);   // the longest decay, so the envelope is nearly flat across the window
    p.set(9, 127);   // TON wide open: the output low-pass is pass-through
    const int blocks = 200;
    const int from = 2048;
    const int count = 8192;

    float frequency = 0.0f;
    float measured[2] = {};
    const uint8_t dirt[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(10, dirt[i]);
        Kick m;
        m.init(kRate);
        m.prepare(p);
        frequency = m.base_frequency() * enaga::dsp::semitones(36.0f);
        m.trigger(hit(96, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        measured[i] = harmonic_residual(g_buffer, from, count, frequency);
    }

    synthesise(g_buffer, blocks * kBlk, frequency, Body::clean_sine);
    const float ideal_sine = harmonic_residual(g_buffer, from, count, frequency);
    synthesise(g_buffer, blocks * kBlk, frequency, Body::rational_fold);
    const float ideal_fold = harmonic_residual(g_buffer, from, count, frequency);
    synthesise(g_buffer, blocks * kBlk, frequency, Body::bare_triangle);
    const float ideal_triangle = harmonic_residual(g_buffer, from, count, frequency);

    // What these come out as, so a reader knows what the bounds are made of: the clean sine
    // 1.0e-11 (the meter's own floating-point floor), the fold 3.9e-4, the bare triangle
    // 1.7e-3, DRT 0 7.6e-11 and DRT 127 2.7e-4.
    //
    // First that the meter does what it claims: a sine has no harmonics at all, the fold has
    // some, and a bare triangle has four times as many again.
    CHECK(ideal_sine < 0.01f * ideal_fold);
    CHECK(ideal_fold < 0.3f * ideal_triangle);

    // DRT 0 is not merely cleaner than DRT 127, it is a pure sine: its residual sits on the
    // meter's floating-point floor, seven orders below the fold's.
    CHECK(measured[0] < 0.01f * ideal_fold);
    // DRT 127 is the fold. It lands at 0.70 of it rather than on it because the reference
    // scales dirtiness by max(1 - 8*f0, 0) first, which is 0.84 at this pitch, and the residual
    // goes as the square of that: 0.84^2 = 0.71.
    CHECK(measured[1] > 0.3f * ideal_fold);
    CHECK(measured[1] < 1.3f * ideal_fold);
    // And the bare triangle is at neither end. DRT 127 is an order of magnitude closer to the
    // fold than to the triangle.
    const float to_fold = measured[1] > ideal_fold ? measured[1] - ideal_fold
                                                   : ideal_fold - measured[1];
    const float to_triangle = ideal_triangle - measured[1];
    CHECK(to_fold < 0.5f * to_triangle);
    CHECK(measured[1] < 0.25f * ideal_triangle);
    CHECK(measured[0] < 0.01f * ideal_triangle);
    // The direction of the knob, on its own, so that a sign flip cannot hide behind the bounds.
    CHECK(measured[1] > 1000.0f * measured[0]);
}

TEST(kick_tone_knob_opens_the_output_low_pass) {
    TrackParams p = uniform(64);
    p.set(10, 127);   // DRT full, so there are harmonics for the filter to take away
    p.set(8, 100);
    const int blocks = 64;
    float e[2] = {};
    const uint8_t tones[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(9, tones[i]);
        Kick m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        e[i] = energy(g_buffer, blocks * kBlk);
    }
    CHECK(e[1] > 4.0f * e[0]);   // shut, the body is a thud; open, it is all there
}

TEST(kick_holds_the_body_for_1_3_ms_and_clicks_instead) {
    // The reference pins the phase at 0.25 for 1.3 ms after a trigger, and the shaper reads 0
    // there: the body is silent for 62 frames at 48 kHz and the click is what is heard. Those
    // two facts together are the 909's attack, so both are worth pinning.
    TrackParams p = uniform(64);
    p.set(1, 0);
    p.set(8, 40);
    const int blocks = 64;
    const int hold = 62;   // int(0.0013 * 48000)

    p.set(3, 0);           // no click: the hold is audible as silence
    Kick quiet;
    quiet.init(kRate);
    quiet.prepare(p);
    quiet.trigger(hit(60, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(quiet, p, g_buffer, blocks);
    CHECK_EQ(energy(g_buffer, hold), 0.0f);
    CHECK(energy(g_buffer + hold, 256) > 0.0f);   // and the body does start right after it

    p.set(3, 127);         // the click fills that window
    Kick clicking;
    clicking.init(kRate);
    clicking.prepare(p);
    clicking.trigger(hit(60, 127));
    clear(g_other, blocks * kBlk);
    render_blocks(clicking, p, g_other, blocks);
    CHECK(energy(g_other, hold) > 0.001f);
    CHECK(energy(g_other, 128) > 3.0f * energy(g_buffer, 128));
    // The click is a 1 ms gate through a 5 ms envelope, so it is over well inside 40 ms and does
    // not go on colouring the body.
    const float late = energy(g_buffer + 2048, 2048);
    CHECK(late > 0.0f);
    CHECK_NEAR(energy(g_other + 2048, 2048), late, 0.05 * double(late));
}

// --- KICK's PNCH page (KNK KND TCK DRP, dsp/kick.hpp) -----------------------

namespace {

// One kick hit with `p` on a fresh machine (so the click's noise starts from the same seed),
// 9,600 frames into `out`. `before`, when given, is prepared first, as a knob that was elsewhere.
void kick_hit(const TrackParams &p, float *out, const TrackParams *before = nullptr) {
    Kick m;
    m.init(kRate);
    if (before != nullptr) m.prepare(*before);
    m.prepare(p);
    m.trigger(hit(60, 110));
    clear(out, 9600);
    render_blocks(m, p, out, 9600 / kBlk);
}

// Energy above `hz` in x[from, from + n): three one-pole high-passes in a row (18 dB an octave),
// so a 50 Hz body does not leak two octaves up.
float energy_above(const float *x, int from, int n, float hz) {
    const float a = 1.0f - enaga::dsp::exp(-6.2831853f * hz / kRate);
    float lp[3] = {0.0f, 0.0f, 0.0f};
    float sum = 0.0f;
    for (int i = 0; i < from + n; i++) {
        float y = x[i];
        for (float &state : lp) {
            state += a * (y - state);
            y -= state;
        }
        if (i >= from) sum += y * y;
    }
    return sum;
}

// The factory 909 (core/sounds.cpp) as TrackParams, the page at 0.
TrackParams kick_909() {
    TrackParams p{};
    const uint8_t src[4] = {55, 72, 56, 72}, amp[4] = {72, 60, 44, 110};
    for (int i = 0; i < 4; i++) {
        p.set(i, src[i]);
        p.set(8 + i, amp[i]);
    }
    return p;
}

}  // namespace

TEST(kick_pnch_at_zero_is_the_kick_it_was) {
    // Bytes 24-27 at 0 must not touch a sample: every saved kit has them at 0. A kick whose
    // page was turned up and back to 0 plays exactly the plain one.
    static float a[9600], b[9600];
    const TrackParams p = kick_909();
    kick_hit(p, a);
    TrackParams up = p;
    for (int i = 0; i < 4; i++) up.set(kAsrFirst + i, 100);
    kick_hit(p, b, &up);
    for (int i = 0; i < 9600; i++) CHECK(a[i] == b[i]);
}

TEST(kick_knock_puts_the_mid_punch_into_the_attack) {
    // KNK: a second, fast pitch envelope. The first 20 ms above 300 Hz grows by well over 6 dB,
    // and the knock is gone by 60 ms: the tail above 300 Hz stays within 3 dB of the plain kick.
    static float plain[9600], knock[9600];
    TrackParams p = kick_909();
    kick_hit(p, plain);
    p.set(kAsrFirst + 0, 127);   // KNK
    p.set(kAsrFirst + 1, 64);    // KND: 4 ms
    kick_hit(p, knock);
    CHECK(energy_above(knock, 0, 960, 300.0f) > 4.0f * energy_above(plain, 0, 960, 300.0f));
    const float tail_plain = energy_above(plain, 2880, 2880, 300.0f);
    const float tail_knock = energy_above(knock, 2880, 2880, 300.0f);
    CHECK(tail_knock < 2.0f * tail_plain);
    CHECK(finite_within(knock, 9600, 4.0f));
}

TEST(kick_tick_is_a_top_click_that_is_over_in_milliseconds) {
    // TCK: noise high-passed at 2 kHz under a 1.5 ms envelope. The first 5 ms above 2 kHz grows
    // by well over 10 dB; after 30 ms the tick adds nothing measurable.
    static float plain[9600], tick[9600];
    TrackParams p = kick_909();
    kick_hit(p, plain);
    p.set(kAsrFirst + 2, 127);   // TCK
    kick_hit(p, tick);
    CHECK(energy_above(tick, 0, 240, 2000.0f) > 10.0f * energy_above(plain, 0, 240, 2000.0f));
    float late = 0.0f;
    for (int i = 1440; i < 9600; i++) late += (tick[i] - plain[i]) * (tick[i] - plain[i]);
    CHECK(late < 1e-6f * energy(plain, 9600));
}

TEST(kick_drop_keeps_the_pitch_falling_below_tun) {
    // DRP at the top: the pitch falls towards an octave below TUN on the amplitude's time
    // constant, so late in a long hit the body crosses zero far less often than without it.
    static float plain[9600], drop[9600];
    TrackParams p = kick_909();
    p.set(3, 0);                 // CLK off: its noise would add crossings
    p.set(8, 100);               // a 170 ms time constant: long, and the fall well along by 150 ms
    kick_hit(p, plain);
    p.set(kAsrFirst + 3, 127);   // DRP
    kick_hit(p, drop);
    const int plain_late = sign_changes(plain + 4800, 4800);
    const int drop_late = sign_changes(drop + 4800, 4800);
    CHECK(drop_late < plain_late * 17 / 20);
    CHECK(drop_late > plain_late / 3);   // falls towards an octave, never past it
    // and the start is untouched: the first 10 ms cross as often either way, give or take one.
    const int plain_early = sign_changes(plain, 480), drop_early = sign_changes(drop, 480);
    CHECK(drop_early >= plain_early - 1 && drop_early <= plain_early + 1);
}

// --- SNARE ----------------------------------------------------------------

TEST(snare_carries_the_909_oscillator_ratio) {
    // Straight off the 909 schematic, by way of Plaits. If a tidy-up changes it, the snare stops
    // being a 909 snare.
    CHECK_NEAR(Snare::kDrumRatio, 1.47f, 1e-6);
    CHECK_NEAR(Snare::kClapRatio, 2.00f, 1e-6);
}

TEST(snare_tunes_to_200_hz_in_the_middle_of_the_knob) {
    Snare m;
    m.init(kRate);
    TrackParams p = uniform(64);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 200.0f, 2.0);
    p.set(0, 0);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 90.0f, 1.0);
}

TEST(snare_noise_envelope_holds_before_it_decays) {
    // The reference holds the noise envelope at full level for 0.04 + 0.03 * decay seconds
    // before it decays at all, and missing that is what makes a synthesised snare sound wrong.
    // So: at the shortest setting the hold is 40 ms, at the longest it is 70 ms. Both are still
    // flat at 20 ms; at 55 ms only the long one is. Measured on the noise alone (SNP full),
    // which also keeps the tail's own length out of the measurement.
    TrackParams p = uniform(64);
    p.set(1, 127);   // all noise, so the drum does not colour the envelope
    p.set(3, 0);     // not a clap
    const int blocks = 200;
    const int window = 512;                    // 10.7 ms
    const int early = int(0.020f * kRate);     // inside both holds
    const int later = int(0.055f * kRate);     // past the 40 ms hold, inside the 70 ms one
    float plateau[2] = {};
    float after[2] = {};
    const uint8_t decays[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(8, decays[i]);
        Snare m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        plateau[i] = window_energy(g_buffer, blocks * kBlk, early, window);
        after[i] = window_energy(g_buffer, blocks * kBlk, later, window);
    }
    CHECK(plateau[0] > 0.0f);
    CHECK(plateau[1] > 0.0f);
    CHECK(after[0] < 0.3f * plateau[0]);    // the 40 ms hold has let go by 55 ms
    CHECK(after[1] > 0.8f * plateau[1]);    // the 70 ms hold has not
}

TEST(snare_snappy_trades_drum_for_noise) {
    TrackParams p = uniform(64);
    p.set(3, 0);
    p.set(8, 80);
    const int blocks = 64;
    int changes[2] = {};
    const uint8_t snappy[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(1, snappy[i]);
        Snare m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        changes[i] = sign_changes(g_buffer + 512, 2048);
    }
    // All drum is a tuned pair and crosses zero a few hundred times in 2048 frames; all noise
    // crosses far more often.
    CHECK(changes[1] > 3 * changes[0]);
}

TEST(snare_clap_mode_fires_several_bursts) {
    TrackParams p = uniform(64);
    p.set(1, 127);   // noise only, so a burst is unmistakable
    p.set(8, 0);     // the shortest tail, so the last burst does not smear into the rest
    p.set(9, 127);   // the widest spread: 40 ms between bursts
    const int blocks = 200;

    p.set(3, 127);   // four bursts
    Snare four;
    four.init(kRate);
    four.prepare(p);
    four.trigger(hit(60, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(four, p, g_buffer, blocks);
    CHECK_EQ(count_bursts(g_buffer, blocks * kBlk, 128, 0.25f), 4);

    p.set(3, 64);    // three bursts
    Snare three;
    three.init(kRate);
    three.prepare(p);
    three.trigger(hit(60, 127));
    clear(g_other, blocks * kBlk);
    render_blocks(three, p, g_other, blocks);
    CHECK_EQ(count_bursts(g_other, blocks * kBlk, 128, 0.25f), 3);

    p.set(3, 0);     // off: one hit, with the hold instead of bursts
    Snare off;
    off.init(kRate);
    off.prepare(p);
    off.trigger(hit(60, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(off, p, g_buffer, blocks);
    CHECK_EQ(count_bursts(g_buffer, blocks * kBlk, 128, 0.25f), 1);
}

TEST(snare_clap_bursts_are_as_loud_as_the_tail) {
    // The factory clap's settings: three bursts about 10 ms apart, mostly noise. A clap's flutter
    // is its bursts, so the strongest of them must reach the tail's level, not sit 10-18 dB under
    // it as they did when each burst died after a third of the gap.
    TrackParams p = uniform(64);
    p.set(1, 100);   // SNP
    p.set(2, 22);    // TON
    p.set(3, 64);    // three bursts
    p.set(8, 54);    // DEC
    p.set(9, 28);    // SPR: 2 + 38 * 28/127 = 10 ms
    p.set(11, 127);
    Snare m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, 127));
    const int blocks = 40;
    clear(g_buffer, blocks * kBlk);
    render_blocks(m, p, g_buffer, blocks);
    const int gap = int((0.002f + 0.038f * 28.0f / 127.0f) * kRate);
    const float bursts = peak(g_buffer, 2 * gap);        // the first two bursts
    const float tail = peak(g_buffer + 2 * gap, blocks * kBlk - 2 * gap);
    CHECK(bursts > 0.5f * tail);                        // within 6 dB
}

TEST(snare_clap_spread_sets_how_far_apart_the_bursts_are) {
    TrackParams p = uniform(64);
    p.set(1, 127);
    p.set(3, 127);
    p.set(8, 0);
    const int blocks = 200;
    int span[2] = {};
    const uint8_t spreads[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(9, spreads[i]);
        Snare m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        span[i] = energy_quantile(g_buffer, blocks * kBlk, 0.95f);
    }
    // 2 ms apart the four bursts are over in a few milliseconds; 40 ms apart the last one does
    // not even start until 120 ms in.
    CHECK(span[1] > 3 * span[0]);
    CHECK(span[1] > int(0.09f * kRate));
    CHECK(span[0] < int(0.05f * kRate));
}

// --- HAT ------------------------------------------------------------------

TEST(hat_carries_the_808_square_ratios) {
    // The six ratios and the nominal f0 from hi_hat.h: 414, 540, 607, 740, 800 and 1050 Hz.
    CHECK_NEAR(Hat::kNominalHz, 414.0f, 1e-4);
    CHECK_NEAR(Hat::kRatio[0], 1.0f, 1e-6);
    CHECK_NEAR(Hat::kRatio[1], 1.304f, 1e-6);
    CHECK_NEAR(Hat::kRatio[2], 1.466f, 1e-6);
    CHECK_NEAR(Hat::kRatio[3], 1.787f, 1e-6);
    CHECK_NEAR(Hat::kRatio[4], 1.932f, 1e-6);
    CHECK_NEAR(Hat::kRatio[5], 2.536f, 1e-6);
    // And they come out as the frequencies the source comments give.
    for (int i = 0; i < Hat::kOscillators; i++) {
        CHECK(Hat::kNominalHz * Hat::kRatio[i] > 400.0f);
    }
    CHECK_NEAR(Hat::kNominalHz * Hat::kRatio[1], 540.0f, 1.0);
    CHECK_NEAR(Hat::kNominalHz * Hat::kRatio[2], 607.0f, 1.0);
    CHECK_NEAR(Hat::kNominalHz * Hat::kRatio[3], 740.0f, 1.0);
    CHECK_NEAR(Hat::kNominalHz * Hat::kRatio[4], 800.0f, 1.0);
    CHECK_NEAR(Hat::kNominalHz * Hat::kRatio[5], 1050.0f, 1.0);
}

TEST(hat_detune_knob_is_the_reference_in_the_middle) {
    Hat m;
    m.init(kRate);
    TrackParams p = uniform(64);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), Hat::kNominalHz, 1.0);
    for (int i = 0; i < Hat::kOscillators; i++) {
        CHECK_NEAR(m.detune_ratio(i), Hat::kRatio[i], 1e-5);
    }
    // Away from the middle the squares close on unison or spread past the 808.
    p.set(3, 0);
    m.prepare(p);
    CHECK(m.detune_ratio(5) < Hat::kRatio[5]);
    CHECK(m.detune_ratio(5) > 1.0f);
    p.set(3, 127);
    m.prepare(p);
    CHECK(m.detune_ratio(5) > Hat::kRatio[5]);
}

TEST(hat_being_choked_cuts_the_sound_short) {
    TrackParams p = uniform(64);
    p.set(8, 127);   // the longest tail, so there is plenty to cut
    p.set(9, 0);     // the fastest choke tail: 2 ms
    const int blocks = 400;

    Hat choked;
    choked.init(kRate);
    choked.prepare(p);
    choked.trigger(hit(60, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(choked, p, g_buffer, 8);
    choked.choke();
    render_blocks(choked, p, g_buffer + 8 * kBlk, blocks - 8);

    Hat ringing;
    ringing.init(kRate);
    ringing.prepare(p);
    ringing.trigger(hit(60, 127));
    clear(g_other, blocks * kBlk);
    render_blocks(ringing, p, g_other, blocks);

    const int from = 16 * kBlk;
    const int n = (blocks - 16) * kBlk;
    CHECK(energy(g_buffer + from, n) < 0.02f * energy(g_other + from, n));
    CHECK(!choked.active());    // and it lets go, rather than ringing on at a low level
    CHECK(ringing.active());
    // The cut is a fast tail, not a step: the choke happened at block 8.
    CHECK(window_peak(g_buffer, blocks * kBlk, 8 * kBlk, 64) > 0.0f);
}

TEST(hat_choke_target_lets_one_hat_cut_another) {
    TrackParams open_params = uniform(64);
    open_params.set(8, 127);   // a long open hat
    TrackParams closed_params = uniform(64);
    closed_params.set(8, 20);  // a short closed hat
    closed_params.set(9, 0);

    Hat open, closed;
    open.init(kRate);
    closed.init(kRate);
    CHECK(closed.choke_target() == nullptr);
    closed.set_choke_target(&open);
    CHECK(closed.choke_target() == &open);
    // A hat is not allowed to choke itself, however the app wires it.
    closed.set_choke_target(&closed);
    CHECK(closed.choke_target() == nullptr);
    closed.set_choke_target(&open);

    const int blocks = 300;
    clear(g_buffer, blocks * kBlk);
    open.prepare(open_params);
    open.trigger(hit(60, 127));
    for (int b = 0; b < blocks; b++) {
        if (b == 8) {
            closed.prepare(closed_params);
            closed.trigger(hit(60, 127));   // this is what chokes the open hat
        }
        open.prepare(open_params);
        open.render_add(g_buffer + b * kBlk, kBlk);
    }
    CHECK(!open.active());   // the open hat was taken away by the closed one's trigger

    clear(g_other, blocks * kBlk);
    Hat alone;
    alone.init(kRate);
    alone.prepare(open_params);
    alone.trigger(hit(60, 127));
    render_blocks(alone, open_params, g_other, blocks);
    CHECK(alone.active());
    const int from = 16 * kBlk;
    const int n = (blocks - 16) * kBlk;
    CHECK(energy(g_buffer + from, n) < 0.05f * energy(g_other + from, n));
}

TEST(hat_choke_group_reports_the_knob) {
    Hat m;
    m.init(kRate);
    TrackParams p = uniform(64);
    p.set(10, 0);
    m.prepare(p);
    CHECK_EQ(m.choke_group(), 0);
    p.set(10, 127);
    m.prepare(p);
    CHECK_EQ(m.choke_group(), 4);
    p.set(10, 64);
    m.prepare(p);
    CHECK(m.choke_group() >= 1 && m.choke_group() <= 3);
}

TEST(hat_decay_knob_sweeps_from_a_tick_to_a_ride) {
    TrackParams p = uniform(64);
    const int blocks = 1400;
    int length[3] = {};
    const uint8_t decays[3] = {0, 64, 127};
    for (int i = 0; i < 3; i++) {
        p.set(8, decays[i]);
        Hat m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        length[i] = envelope_falls_below(g_buffer, blocks * kBlk, 256, 0.05f);
    }
    CHECK(length[0] < length[1]);
    CHECK(length[1] < length[2]);
    // 7 ms at the bottom of the knob: three time constants of it is well inside 100 ms.
    CHECK(length[0] < 4800);
    // 0.9 s at the top.
    CHECK(length[2] > 24000);
}

TEST(hat_tone_knob_moves_the_band) {
    TrackParams p = uniform(64);
    p.set(8, 80);
    const int blocks = 64;
    int changes[2] = {};
    const uint8_t tones[2] = {0, 127};
    for (int i = 0; i < 2; i++) {
        p.set(1, tones[i]);
        Hat m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127));
        clear(g_buffer, blocks * kBlk);
        render_blocks(m, p, g_buffer, blocks);
        changes[i] = sign_changes(g_buffer + 256, 2048);
    }
    // 150 Hz at the bottom of the knob against 9.6 kHz at the top: the output has to get busier.
    CHECK(changes[1] > 4 * changes[0]);
}

TEST(hat_noise_knob_blends_in_the_clocked_noise) {
    TrackParams p = uniform(64);
    p.set(8, 80);
    const int blocks = 64;

    p.set(2, 0);
    Hat metal;
    metal.init(kRate);
    metal.prepare(p);
    metal.trigger(hit(60, 127));
    clear(g_buffer, blocks * kBlk);
    render_blocks(metal, p, g_buffer, blocks);

    p.set(2, 127);
    Hat noisy;
    noisy.init(kRate);
    noisy.prepare(p);
    noisy.trigger(hit(60, 127));
    clear(g_other, blocks * kBlk);
    render_blocks(noisy, p, g_other, blocks);

    // The squares are periodic, the clocked noise is not: the two differ.
    bool different = false;
    for (int i = 0; i < blocks * kBlk; i++) {
        const float d = g_other[i] - g_buffer[i];
        if (d > 1e-4f || d < -1e-4f) different = true;
    }
    CHECK(different);
    CHECK(finite_within(g_other, blocks * kBlk, 6.0f));
}
