// PERC: the 2-operator FM percussion machine in dsp/perc.cpp.
//
// These test measurable properties rather than sample values -- that a hit sounds, that it dies
// away, that a knob moves what it says it moves -- plus the two things that make this machine FM
// rather than anything else: that the carrier's pitch tracks the note, and that the modulation
// index both adds harmonics and FALLS over the note. A static index is an organ; a test that
// pins the fall is what stops a later tidy-up from quietly turning this into one.
//
// Two measures do most of the work here, and neither needs a spectrum:
//
//   harmonic_residual -- a pure sampled sinusoid satisfies x[n+1] + x[n-1] == 2 cos(w) x[n]
//       EXACTLY, whatever its amplitude, so the energy left in that recurrence relative to the
//       signal's own is zero for a bare sine at `frequency` and grows with everything stacked on
//       top of it. Amplitude-independent, which matters because every signal here is under a
//       decaying envelope. The same idea as tests/test_machines.cpp uses on the drum bodies.
//
//   periodicity_error -- the difference between the signal and itself one carrier period later.
//       An FM pair at an INTEGER ratio puts every sideband on a harmonic of the carrier, so the
//       whole waveform is periodic at the carrier's period; at an irrational ratio there is no
//       common period at all. With the carrier placed at exactly 300 Hz (TUN 64, by design) the
//       period is exactly 160 samples at 48 kHz, so this is a direct measure of the one thing
//       that separates a tom from a rim -- and it cannot be faked by a brightness change, which
//       a harmonic-content measure alone could be.
#include "check.hpp"

#include <enaga/abi.h>

#include "perc.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kCapture = ENAGA_SAMPLE_RATE * 2;      // 2 s, longer than the longest tail
// TUN 64 is exactly 300 Hz, which is exactly 160 samples at 48 kHz: the lag the periodicity
// measure uses. Both numbers are properties of the mapping, so a test asserts them too.
constexpr int kCarrierPeriod = 160;

// Static, because two seconds of audio does not belong on a stack frame.
float g_buffer[kCapture];
float g_other[kCapture];

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

// The parameter set the pitch tests want: a bare carrier (no modulation, no sweep) under a long
// envelope, so the output is a clean sine at base_frequency() for as long as it is measured.
TrackParams bare_carrier() {
    TrackParams p = uniform(64);
    p.set(2, 0);     // IDX: no modulation at all, so the carrier is a pure sine
    p.set(8, 127);   // DEC: the longest tail, so there is signal to measure
    p.set(9, 0);     // SWP: no pitch sweep, so the frequency is constant
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

int sign_changes(const float *x, int from, int count) {
    int changes = 0;
    for (int i = from + 1; i < from + count; i++) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) changes++;
    }
    return changes;
}

float largest_step(const float *x, int n) {
    float worst = 0.0f;
    for (int i = 1; i < n; i++) {
        const float d = x[i] - x[i - 1];
        const float a = d < 0.0f ? -d : d;
        if (a > worst) worst = a;
    }
    return worst;
}

// Where the windowed envelope first falls below `fraction` of its largest value, in samples.
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

// How much energy is NOT a pure sinusoid at `frequency`, relative to the signal's own. Zero for
// a bare sine of any amplitude; grows with every harmonic or sideband on top of it. See the file
// header for why this is exact.
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

// How much the signal fails to repeat one carrier period later, relative to its own energy.
// Near zero for an integer modulator ratio (every sideband is a harmonic of the carrier, so the
// waveform is periodic); large for an irrational one.
float periodicity_error(const float *x, int from, int count, int period) {
    float difference = 0.0f;
    float total = 0.0f;
    for (int i = from; i < from + count; i++) {
        const float d = x[i] - x[i + period];
        difference += d * d;
        total += x[i] * x[i];
    }
    return total > 0.0f ? difference / total : 0.0f;
}

// One hit rendered into g_buffer with the given parameters. Returns the number of samples.
int render_one(Perc &m, const TrackParams &p, const Trig &t, int blocks) {
    m.init(kRate);
    m.prepare(p);
    m.trigger(t);
    clear(g_buffer, blocks * kBlk);
    return render_blocks(m, p, g_buffer, blocks);
}

}  // namespace

// --- the shared contract --------------------------------------------------

TEST(perc_is_silent_until_it_is_triggered) {
    Perc m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    const int n = 8 * kBlk;
    clear(g_buffer, n);
    CHECK(!m.active());
    render_blocks(m, p, g_buffer, 8);
    CHECK(!m.active());
    CHECK_EQ(peak(g_buffer, n), 0.0f);
}

TEST(perc_sounds_when_triggered) {
    Perc m;
    const TrackParams p = uniform(64);
    const int n = render_one(m, p, hit(), 32);
    CHECK(m.active());
    CHECK(peak(g_buffer, n) > 0.01f);
}

TEST(perc_decays_back_to_silence) {
    Perc m;
    TrackParams p = uniform(64);
    p.set(8, 0);   // DEC at its shortest: a 4 ms time constant
    const int blocks = 600;
    const int n = render_one(m, p, hit(), blocks);
    CHECK(!m.active());
    // Once nothing sounds, render_add adds nothing at all: the tail is exactly zero, not small.
    CHECK_EQ(peak(g_buffer + (blocks - 16) * kBlk, 16 * kBlk), 0.0f);
    CHECK(peak(g_buffer, n) > 0.01f);
}

TEST(perc_stays_finite_at_parameter_extremes) {
    for (int extreme = 0; extreme < 2; extreme++) {
        const TrackParams p = uniform(extreme == 0 ? uint8_t(0) : uint8_t(127));
        Perc m;
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
        // Two voices, each a sine of unit amplitude through a gain of at most 1: 2 is the hard
        // ceiling and 3 leaves room for nothing in particular.
        CHECK(finite_within(g_buffer, blocks * kBlk, 3.0f));
    }
}

TEST(perc_gets_louder_with_velocity) {
    const TrackParams p = uniform(64);
    const int blocks = 64;
    float peaks[3] = {};
    const uint8_t velocities[3] = {20, 70, 127};
    for (int i = 0; i < 3; i++) {
        Perc m;
        render_one(m, p, hit(60, velocities[i]), blocks);
        peaks[i] = peak(g_buffer, blocks * kBlk);
    }
    CHECK(peaks[0] < peaks[1]);
    CHECK(peaks[1] < peaks[2]);
}

TEST(perc_render_add_accumulates) {
    // Two fresh machines are bit-identical (there is no noise source anywhere in this machine),
    // so the only difference between the runs is what was already in the buffer.
    const TrackParams p = uniform(64);
    const int blocks = 24;
    const int n = blocks * kBlk;

    Perc a;
    render_one(a, p, hit(), blocks);

    Perc b;
    b.init(kRate);
    b.prepare(p);
    b.trigger(hit());
    fill(g_other, n, 0.25f);
    render_blocks(b, p, g_other, blocks);

    for (int i = 0; i < n; i++) CHECK_NEAR(g_other[i], g_buffer[i] + 0.25f, 1e-6);
}

TEST(perc_kill_all_silences_at_once) {
    Perc m;
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

TEST(perc_release_all_lets_the_tail_finish) {
    Perc m;
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

TEST(perc_a_trig_frame_offset_delays_the_hit) {
    Perc m;
    m.init(kRate);
    const TrackParams p = uniform(64);
    m.prepare(p);
    m.trigger(hit(60, 127, 40));
    clear(g_buffer, kBlk);
    m.render_add(g_buffer, kBlk);
    CHECK_EQ(peak(g_buffer, 40), 0.0f);          // untouched up to the trig's frame
    CHECK(peak(g_buffer + 40, kBlk - 40) > 0.0f);
}

TEST(perc_a_trig_frame_offset_is_sample_accurate_across_a_block) {
    // Every frame of the block, not just one: the hit starts on exactly the frame asked for.
    const TrackParams p = uniform(64);
    for (uint32_t frame = 0; frame < 64u; frame += 7) {
        Perc m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 127, frame));
        clear(g_buffer, kBlk);
        m.render_add(g_buffer, kBlk);
        for (uint32_t i = 0; i < frame; i++) CHECK_EQ(g_buffer[i], 0.0f);
        if (frame < 60u) CHECK(peak(g_buffer + frame, kBlk - int(frame)) > 0.0f);
    }
}

TEST(perc_a_long_call_matches_the_same_frames_in_blocks) {
    const TrackParams p = uniform(64);
    const int n = 200;

    Perc a;
    a.init(kRate);
    a.prepare(p);
    a.trigger(hit());
    clear(g_buffer, n);
    a.render_add(g_buffer, n);

    Perc b;
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

TEST(perc_names_are_three_characters_the_font_has) {
    Perc m;
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
    // The pages PERC does not own are blanked, the ones it does are named.
    for (int i = 4; i < 8; i++) CHECK_EQ(m.param_name(i)[0], '-');
    for (int i = 12; i < 16; i++) CHECK_EQ(m.param_name(i)[0], '-');
    for (int i = 0; i < 4; i++) CHECK(m.param_name(i)[0] != '-');
    for (int i = 8; i < 12; i++) CHECK(m.param_name(i)[0] != '-');
    // Out of range is safe and blank, because the UI asks for all 16 slots whatever the page.
    CHECK_EQ(m.param_name(-1)[0], '-');
    CHECK_EQ(m.param_name(kParams)[0], '-');
}

// --- TUN: the carrier's pitch ---------------------------------------------

TEST(perc_tune_maps_as_documented) {
    Perc m;
    m.init(kRate);
    TrackParams p = uniform(64);

    p.set(0, 0);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 48.0f, 0.1);

    // Knob 64 is exactly 300 Hz, and 300 Hz is exactly 160 samples at 48 kHz: the lag the
    // periodicity tests below rely on.
    p.set(0, 64);
    m.prepare(p);
    CHECK_NEAR(m.base_frequency(), 300.0f, 0.5);
    CHECK_NEAR(kRate / m.base_frequency(), float(kCarrierPeriod), 0.01);

    p.set(0, 127);
    m.prepare(p);
    CHECK(m.base_frequency() > 1790.0f);
    CHECK(m.base_frequency() < 1850.0f);

    // Monotonic, with no flat spot: every knob step moves the carrier.
    float previous = 0.0f;
    for (int knob = 0; knob < 128; knob++) {
        p.set(0, uint8_t(knob));
        m.prepare(p);
        CHECK(m.base_frequency() > previous);
        previous = m.base_frequency();
    }
}

TEST(perc_carrier_pitch_tracks_the_note) {
    // With the index at zero the carrier is a clean sine, so counting zero crossings measures
    // its frequency directly. An octave up has to double it.
    const TrackParams p = bare_carrier();
    const int blocks = 160;                  // 0.21 s
    const int from = 128;                    // past the 0.2 ms attack
    const int count = 9600;                  // 0.2 s

    Perc low;
    render_one(low, p, hit(60, 127), blocks);
    const int crossings_60 = sign_changes(g_buffer, from, count);
    const float base = low.base_frequency();

    Perc high;
    render_one(high, p, hit(72, 127), blocks);
    const int crossings_72 = sign_changes(g_buffer, from, count);

    // 2 crossings per cycle, 0.2 s of them: 120 at 300 Hz.
    CHECK_NEAR(float(crossings_60), 2.0f * base * 0.2f, 2.0);
    CHECK_NEAR(float(crossings_72), 2.0f * crossings_60, 3.0);

    // And an octave down halves it, so the mapping is not a one-sided accident.
    Perc lower;
    render_one(lower, p, hit(48, 127), blocks);
    const int crossings_48 = sign_changes(g_buffer, from, count);
    CHECK_NEAR(2.0f * crossings_48, float(crossings_60), 3.0);
}

// --- IDX: the index does what an FM index does ----------------------------

TEST(perc_index_zero_is_a_pure_sine) {
    Perc m;
    const TrackParams p = bare_carrier();
    render_one(m, p, hit(60, 127), 160);
    // A bare carrier: everything left in the sine recurrence is sin_turns' own 4e-6 error.
    const float residual = harmonic_residual(g_buffer, 1024, 4096, m.base_frequency());
    CHECK(residual >= 0.0f);
    CHECK(residual < 1e-6f);
}

TEST(perc_index_adds_harmonics) {
    TrackParams p = bare_carrier();
    // IDD at its longest, so the index is still near its peak across the window below. This is
    // what the machine is FOR -- the index falls, and perc_the_index_envelope_falls measures
    // that -- but a test of what the index DOES has to hold it still while it looks.
    p.set(3, 127);
    Perc quiet;
    render_one(quiet, p, hit(60, 127), 160);
    const float bare = harmonic_residual(g_buffer, 1024, 4096, quiet.base_frequency());

    p.set(2, 90);   // IDX up: sidebands either side of the carrier
    Perc loud;
    render_one(loud, p, hit(60, 127), 160);
    const float modulated = harmonic_residual(g_buffer, 1024, 4096, loud.base_frequency());

    CHECK(modulated > 0.05f);
    CHECK(modulated > 1000.0f * bare);

    // And it is monotonic in the knob, not just different at one setting.
    float previous = bare;
    const uint8_t knobs[4] = {30, 60, 90, 127};
    for (int i = 0; i < 4; i++) {
        p.set(2, knobs[i]);
        Perc m;
        render_one(m, p, hit(60, 127), 160);
        const float residual = harmonic_residual(g_buffer, 1024, 4096, m.base_frequency());
        CHECK(residual > previous);
        previous = residual;
    }
}

TEST(perc_the_index_envelope_falls) {
    TrackParams p = uniform(64);
    p.set(2, 100);   // IDX: plenty of modulation to lose
    p.set(3, 50);    // IDD: a 16 ms index time constant
    p.set(8, 127);   // DEC: a long tail, so there is still signal late in the note
    p.set(9, 0);     // SWP: no sweep, so the carrier stays where the measure expects it

    Perc m;
    render_one(m, p, hit(60, 127), 800);   // 1.07 s
    const float frequency = m.base_frequency();
    const float early = harmonic_residual(g_buffer, 128, 1024, frequency);
    const float late = harmonic_residual(g_buffer, 19200, 1024, frequency);

    // Measured: early 0.078, late 3.5e-11 -- the fall is seven orders of magnitude, and the
    // only reason `early` is not larger is that this window already spans one index time
    // constant. The relation is what matters; the absolute floor is a sanity check.
    CHECK(early > 0.02f);                  // it starts bright
    CHECK(late < 0.01f * early);           // and ends as very nearly the bare carrier
    CHECK(peak(g_buffer + 19200, 1024) > 0.01f);   // while still audible, so this is not silence

    // A longer index envelope keeps the brightness longer: the knob is the thing being measured,
    // not just the existence of a fall.
    p.set(3, 127);   // IDD at its longest, 400 ms
    Perc slow;
    render_one(slow, p, hit(60, 127), 800);
    const float late_slow = harmonic_residual(g_buffer, 19200, 1024, slow.base_frequency());
    CHECK(late_slow > 10.0f * late);
}

// --- RAT: the ratio, its quantisation, and the timbre it decides ----------

TEST(perc_ratio_knob_is_quantised_in_its_lower_half) {
    // Four knob steps per slot, 16 slots over knob 0-63.
    for (int knob = 0; knob <= Perc::kQuantisedTop; knob += 4) {
        const float r = Perc::ratio_for(uint8_t(knob));
        for (int i = 1; i < 4; i++) CHECK_EQ(Perc::ratio_for(uint8_t(knob + i)), r);
    }
    // Exactly 16 distinct values down there, and no more.
    int distinct = 1;
    for (int knob = 1; knob <= Perc::kQuantisedTop; knob++) {
        if (Perc::ratio_for(uint8_t(knob)) != Perc::ratio_for(uint8_t(knob - 1))) distinct++;
    }
    CHECK_EQ(distinct, Perc::kRatioSlots);
}

TEST(perc_ratio_reaches_integers_and_inharmonic_values) {
    // The integers the spec asks for, exactly, so a tom recalls from a lock as the same tom.
    CHECK_EQ(Perc::ratio_for(8), 1.0f);
    CHECK_EQ(Perc::ratio_for(28), 2.0f);
    CHECK_EQ(Perc::ratio_for(44), 3.0f);
    CHECK_EQ(Perc::ratio_for(60), 4.0f);
    // And the inharmonic ones, which are deliberately not near any small integer ratio.
    CHECK_NEAR(Perc::ratio_for(16), 1.41421f, 1e-4);   // sqrt(2)
    CHECK_NEAR(Perc::ratio_for(24), 1.61803f, 1e-4);   // the golden ratio
    CHECK_NEAR(Perc::ratio_for(36), 2.41421f, 1e-4);   // 1 + sqrt(2)
    CHECK_NEAR(Perc::ratio_for(48), 3.14159f, 1e-4);   // pi
}

TEST(perc_ratio_knob_is_monotonic_and_joins_without_a_jump) {
    float previous = 0.0f;
    for (int knob = 0; knob < 128; knob++) {
        const float r = Perc::ratio_for(uint8_t(knob));
        CHECK(r >= previous);
        previous = r;
    }
    // The seam between the table and the continuous sweep is at the same value on both sides.
    CHECK_EQ(Perc::ratio_for(uint8_t(Perc::kQuantisedTop)), 4.0f);
    CHECK_EQ(Perc::ratio_for(uint8_t(Perc::kQuantisedTop + 1)), 4.0f);
    // The upper half really sweeps, and reaches the documented top.
    CHECK(Perc::ratio_for(100) > Perc::ratio_for(80));
    CHECK(Perc::ratio_for(80) > Perc::ratio_for(64));
    CHECK_NEAR(Perc::ratio_for(127), 16.0f, 0.01);

    // prepare() reads the same mapping the UI will show.
    Perc m;
    m.init(kRate);
    TrackParams p = uniform(64);
    const uint8_t knobs[6] = {0, 8, 36, 63, 64, 127};
    for (int i = 0; i < 6; i++) {
        p.set(1, knobs[i]);
        m.prepare(p);
        CHECK_EQ(m.ratio(), Perc::ratio_for(knobs[i]));
    }
}

TEST(perc_an_integer_ratio_is_periodic_and_an_inharmonic_one_is_not) {
    // The same carrier, the same index: only the ratio differs. An integer ratio puts every
    // sideband on a harmonic of the carrier, so the waveform repeats every 160 samples; an
    // irrational one has no common period at all. This is the tom/rim distinction itself, and
    // unlike a brightness measure it cannot be satisfied by simply being brighter.
    TrackParams p = uniform(64);
    p.set(0, 64);    // TUN: 300 Hz, exactly 160 samples
    p.set(2, 40);    // IDX: enough modulation to matter, little enough that its own decay does
    p.set(3, 127);   // IDD: the longest index envelope, so the index is near-static here
    p.set(8, 127);   // DEC: the longest tail
    p.set(9, 0);     // SWP: no sweep

    const uint8_t integer_knobs[2] = {8, 28};         // ratio 1 and 2
    const uint8_t inharmonic_knobs[2] = {32, 36};     // sqrt(5) and 1 + sqrt(2)
    float worst_integer = 0.0f;
    float best_inharmonic = 1e9f;

    for (int i = 0; i < 2; i++) {
        p.set(1, integer_knobs[i]);
        Perc m;
        render_one(m, p, hit(60, 127), 300);
        const float e = periodicity_error(g_buffer, 2000, 4096, kCarrierPeriod);
        CHECK(e < 0.01f);
        if (e > worst_integer) worst_integer = e;
    }
    for (int i = 0; i < 2; i++) {
        p.set(1, inharmonic_knobs[i]);
        Perc m;
        render_one(m, p, hit(60, 127), 300);
        const float e = periodicity_error(g_buffer, 2000, 4096, kCarrierPeriod);
        CHECK(e > 0.05f);
        if (e < best_inharmonic) best_inharmonic = e;
    }
    CHECK(best_inharmonic > 20.0f * worst_integer);
}

TEST(perc_the_ratio_moves_the_harmonic_content) {
    // A second, independent reading: at a FIXED index, the ratio alone decides how far the
    // sidebands sit from the carrier, so harmonic content has to climb with it. This is the
    // brightness half of what RAT does; the periodicity test above is the half that isolates
    // integer from inharmonic, which a brightness measure on its own cannot.
    //
    // Measured at IDX 70: ratio 1.0 -> 0.0002, 2.0 -> 0.0018, 2.414 -> 0.0042, pi -> 0.0115,
    // so each step is at least a factor of 2.
    TrackParams p = uniform(64);
    p.set(2, 70);    // IDX, the same for every ratio below
    p.set(3, 127);   // IDD, so the index is near-static over the measurement
    p.set(8, 127);   // DEC
    p.set(9, 0);     // SWP

    const uint8_t knobs[4] = {8, 28, 36, 48};   // 1.0, 2.0, 1 + sqrt(2), pi
    float previous = 0.0f;
    for (int i = 0; i < 4; i++) {
        p.set(1, knobs[i]);
        Perc m;
        render_one(m, p, hit(60, 127), 200);
        const float residual = harmonic_residual(g_buffer, 1024, 4096, m.base_frequency());
        CHECK(residual > 1.5f * previous);
        previous = residual;
    }
    CHECK(previous > 0.005f);
}

// --- SWP and SWT: the pitch envelope --------------------------------------

TEST(perc_the_pitch_envelope_sweeps_downward) {
    TrackParams p = uniform(64);
    p.set(0, 40);    // TUN: about 150 Hz, so the swept attack stays well inside the band
    p.set(2, 0);     // IDX: a bare carrier, so zero crossings measure the frequency
    p.set(8, 127);   // DEC: a long tail
    p.set(9, 127);   // SWP: the full 8x sweep
    p.set(10, 90);   // SWT: about a 43 ms time constant

    Perc m;
    render_one(m, p, hit(60, 127), 400);
    const int early = sign_changes(g_buffer, 64, 480);        // 1.3-11.3 ms
    const int late = sign_changes(g_buffer, 9600, 480);       // 200-210 ms
    CHECK(early > 3 * late);
    // Late, the sweep has run out and the carrier is back at f0.
    CHECK_NEAR(float(late), 2.0f * m.base_frequency() * 0.01f, 2.0);

    // With no sweep the two windows measure the same frequency.
    p.set(9, 0);
    Perc flat;
    render_one(flat, p, hit(60, 127), 400);
    CHECK_NEAR(float(sign_changes(g_buffer, 64, 480)),
               float(sign_changes(g_buffer, 9600, 480)), 2.0);
}

TEST(perc_the_sweep_time_knob_lengthens_the_sweep) {
    TrackParams p = uniform(64);
    p.set(0, 40);
    p.set(2, 0);
    p.set(8, 127);
    p.set(9, 127);

    p.set(10, 10);   // SWT short: the sweep is over almost at once
    Perc fast;
    render_one(fast, p, hit(60, 127), 400);
    const int fast_mid = sign_changes(g_buffer, 4800, 480);   // 100-110 ms

    p.set(10, 110);  // SWT long: still falling at 100 ms
    Perc slow;
    render_one(slow, p, hit(60, 127), 400);
    const int slow_mid = sign_changes(g_buffer, 4800, 480);

    CHECK(slow_mid > fast_mid);
}

// --- DEC: the amplitude envelope ------------------------------------------

TEST(perc_the_decay_knob_lengthens_the_tail) {
    TrackParams p = uniform(64);
    p.set(2, 0);     // a bare carrier, so the envelope is what the window peak follows
    const uint8_t knobs[4] = {10, 50, 90, 127};
    int previous = 0;
    for (int i = 0; i < 4; i++) {
        p.set(8, knobs[i]);
        Perc m;
        const int n = render_one(m, p, hit(60, 127), 900);
        const int fell = envelope_falls_below(g_buffer, n, 128, 0.1f);
        CHECK(fell > previous);
        previous = fell;
    }
}

// --- two voices -----------------------------------------------------------

TEST(perc_two_overlapping_triggers_are_two_independent_voices) {
    TrackParams p = uniform(64);
    p.set(2, 0);     // bare carriers, so the two notes stay at distinct frequencies
    p.set(8, 110);   // a long decay, so both overlap for the whole measurement
    const int blocks = 120;
    const int n = blocks * kBlk;

    Perc one;
    render_one(one, p, hit(48, 127), blocks);
    const float single = energy(g_buffer, n);
    const int crossings_single = sign_changes(g_buffer, 2048, 4096);

    Perc two;
    two.init(kRate);
    two.prepare(p);
    two.trigger(hit(48, 127));
    two.trigger(hit(60, 127, 32));
    clear(g_other, n);
    render_blocks(two, p, g_other, blocks);
    const float both = energy(g_other, n);

    // Two voices at different pitches are incoherent, so the energies add.
    CHECK(both > 1.6f * single);
    // And the higher note is really in there: its crossings are on top of the low one's.
    CHECK(sign_changes(g_other, 2048, 4096) > crossings_single);
}

TEST(perc_a_third_trigger_steals_a_voice_rather_than_stacking_or_dropping_out) {
    TrackParams p = uniform(64);
    p.set(8, 120);   // long decays, so the third hit has to steal a loud voice
    const int blocks = 160;
    const int n = blocks * kBlk;

    Perc one;
    render_one(one, p, hit(60, 127), blocks);
    const float single_peak = peak(g_buffer, n);

    Perc m;
    m.init(kRate);
    clear(g_other, n);
    m.prepare(p);
    m.trigger(hit(60, 127));
    for (int b = 0; b < blocks; b++) {
        if (b == 8) m.trigger(hit(60, 127));
        if (b == 16) m.trigger(hit(60, 127));    // the steal
        if (b == 24) m.trigger(hit(60, 127));
        m.prepare(p);
        m.render_add(g_other + b * kBlk, kBlk);
    }

    // Never more than two voices: the peak cannot pass twice one voice's.
    CHECK(peak(g_other, n) < 2.1f * single_peak);
    // And it does not go silent either: the hit after the steal is there.
    CHECK(peak(g_other + 17 * kBlk, 8 * kBlk) > 0.5f * single_peak);
    CHECK(m.active());
    CHECK(finite_within(g_other, n, 3.0f));
}

TEST(perc_a_stolen_voice_is_faded_rather_than_cut) {
    // The declick has to be measured on a signal that is smooth to begin with. At a working
    // index this machine's waveform moves most of a cycle per sample during a swept attack --
    // that is FM, and a step measure there would be meaningless -- so this uses a bare carrier,
    // where the sample-to-sample step of a 300 Hz sine is 0.009 and a hard cut of a voice at
    // full level would be a step of one whole output level, about 0.25. Only the 0.2 ms
    // amplitude attack sits in between, at about 0.05.
    TrackParams p = bare_carrier();
    p.set(8, 120);   // long decays, so the third hit has to steal a loud voice
    const int blocks = 160;
    const int n = blocks * kBlk;

    Perc m;
    m.init(kRate);
    clear(g_other, n);
    m.prepare(p);
    m.trigger(hit(60, 127));
    for (int b = 0; b < blocks; b++) {
        if (b == 8) m.trigger(hit(60, 127));
        if (b == 16) m.trigger(hit(60, 127));    // both voices busy and loud: the steal
        if (b == 24) m.trigger(hit(60, 127));
        m.prepare(p);
        m.render_add(g_other + b * kBlk, kBlk);
    }
    CHECK(largest_step(g_other, n) < 0.12f);
    CHECK(peak(g_other, n) > 0.1f);              // and it is not quiet, so this means something
}

// --- accent, and the bound the engine has by construction -----------------

TEST(perc_a_harder_hit_is_brighter_as_well_as_louder) {
    // docs/SOUND.md asks accent to do more than lift the level: here velocity scales the index,
    // so a hard hit has measurably more harmonic content, not just more of it.
    TrackParams p = uniform(64);
    p.set(2, 90);    // IDX
    p.set(3, 127);   // IDD, so the index is near-static over the measurement
    p.set(8, 127);   // DEC
    p.set(9, 0);     // SWP

    Perc soft;
    render_one(soft, p, hit(60, 30), 200);
    const float soft_residual = harmonic_residual(g_buffer, 1024, 4096, soft.base_frequency());
    const float soft_peak = peak(g_buffer, 200 * kBlk);

    Perc hard;
    render_one(hard, p, hit(60, 127), 200);
    const float hard_residual = harmonic_residual(g_buffer, 1024, 4096, hard.base_frequency());
    const float hard_peak = peak(g_buffer, 200 * kBlk);

    CHECK(hard_peak > soft_peak);
    CHECK(hard_residual > 1.2f * soft_residual);
}

TEST(perc_one_voice_cannot_leave_the_unit_interval) {
    // An FM carrier is a sine of a modulated phase, so |output| <= 1 whatever the index is:
    // there is no soft clip in this machine and none is needed. Worth a test, because it is the
    // reason LVL needs no makeup trim.
    TrackParams p = uniform(127);   // LVL and IDX both at maximum
    Perc m;
    const int n = render_one(m, p, hit(60, 127), 200);
    CHECK(peak(g_buffer, n) <= 1.0001f);
    CHECK(peak(g_buffer, n) > 0.5f);
}

TEST(perc_all_zero_parameters_are_safe) {
    // LVL 0 is silence, not a crash, and the machine still finishes its note.
    const TrackParams p = uniform(0);
    Perc m;
    const int n = render_one(m, p, hit(60, 127), 200);
    CHECK(finite_within(g_buffer, n, 1e-3f));
    CHECK(!m.active());
}
