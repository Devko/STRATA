// The FM machine's voice (dsp/fm.cpp) and its operator loop (dsp/fmloops.ti.c).
#include "check.hpp"

#include "fm.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace strata;

namespace {

constexpr float kRate = 48000.0f;

TrackParams fm_params() {
    TrackParams p{};
    p.set(0, 0);      // ALG 1
    p.set(1, 16);     // RAT 1
    p.set(2, 0);      // IDX 0
    p.set(3, 0);      // FBK 0
    p.set(8, 80);     // DEC
    p.set(9, 0);      // IEN 0: a static index
    p.set(10, 0);     // VEL 0
    p.set(11, 127);   // LVL
    return p;
}

// Renders `frames` of one voice at `note`, in control slices as a machine would.
void render(FmVoice &v, const FmSettings &s, float *out, int frames) {
    float env[FmVoice::kControl];
    for (int i = 0; i < frames; i += FmVoice::kControl) {
        const int n = frames - i < FmVoice::kControl ? frames - i : FmVoice::kControl;
        v.run(s, out + i, n, env);
    }
}

double harmonic(const float *x, int n, double hz) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) {
        const double w = 2.0 * M_PI * hz * i / kRate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

}  // namespace

TEST(fm_knob_zones_cover_the_byte) {
    CHECK_EQ(fm_algorithm(0), 0);
    CHECK_EQ(fm_algorithm(127), kFmAlgorithms - 1);
    for (int v = 1; v < 128; v++) CHECK(fm_algorithm(uint8_t(v)) - fm_algorithm(uint8_t(v - 1)) <= 1);
    CHECK(fm_ratio(0) == 0.5f);
    CHECK(fm_ratio(127) == 14.0f);
    CHECK(fm_fine(64) == 0.0f);
    CHECK(fm_carrier_ratio(0) == 1.0f);
    CHECK(fm_detune_cents(0) == 0.0f);
    CHECK(fm_detune_cents(127) > 24.0f);
}

TEST(fm_with_no_index_is_a_sine_at_the_note) {
    TrackParams p = fm_params();
    p.set(kAsrFirst + 1, 127);   // SUS: a steady level, so the decay does not smear the spectrum
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(69.0f, 1.0f);
    v.gate_on();
    static float out[4800];
    render(v, s, out, 4800);
    const float *tail = out + 2400;   // past the attack
    const double fund = harmonic(tail, 2400, 440.0);
    CHECK(fund > 0.2);
    CHECK(harmonic(tail, 2400, 880.0) < fund * 1e-3);
    CHECK(harmonic(tail, 2400, 1320.0) < fund * 1e-3);
}

TEST(fm_index_brings_in_the_sidebands) {
    TrackParams p = fm_params();
    p.set(2, 70);   // IDX
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(57.0f, 1.0f);   // 220 Hz, modulator at 1:1
    v.gate_on();
    static float out[4800];
    render(v, s, out, 4800);
    const float *tail = out + 2400;
    const double fund = harmonic(tail, 2400, 220.0);
    CHECK(harmonic(tail, 2400, 440.0) > fund * 0.1);
    CHECK(harmonic(tail, 2400, 660.0) > fund * 0.02);
}

TEST(fm_index_envelope_mellows_the_note_as_it_rings) {
    TrackParams p = fm_params();
    p.set(2, 90);
    p.set(9, 127);   // IEN: the whole index follows its envelope
    p.set(8, 110);   // a long amplitude decay, so the level barely moves meanwhile
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(57.0f, 1.0f);
    v.gate_on();
    static float out[48000];
    render(v, s, out, 48000);
    auto brightness = [&](int at) {
        const double f = harmonic(out + at, 2400, 220.0);
        return harmonic(out + at, 2400, 660.0) / (f > 1e-9 ? f : 1e-9);
    };
    CHECK(brightness(1200) > 4.0 * brightness(40000));
}

TEST(fm_two_pair_algorithm_detunes_the_second_pair) {
    TrackParams p = fm_params();
    p.set(0, 4 * 16);   // ALG 5
    const FmSettings s0 = FmSettings::make(p, kRate, 1.0f / kRate);
    CHECK(s0.ratio[2] == s0.ratio[0]);
    constexpr int det = kAsrFirst + kAsrParams;   // the OSC page's first byte, DET
    if constexpr (det < kTrackParams) {
        p.v[det] = 127;
        const FmSettings s1 = FmSettings::make(p, kRate, 1.0f / kRate);
        CHECK(s1.ratio[2] > s1.ratio[0] * 1.01f);
        CHECK(s1.ratio[3] > s1.ratio[1] * 1.01f);
    }
}

TEST(fm_one_shot_note_ends_and_frees_the_voice) {
    TrackParams p = fm_params();
    p.set(8, 20);   // short
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(60.0f, 1.0f);
    v.gate_on();
    CHECK(v.sounding());
    static float out[48000];
    render(v, s, out, 48000);
    CHECK(!v.sounding());
}

TEST(fm_choke_fades_to_silence_in_its_frames) {
    const TrackParams p = fm_params();
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(60.0f, 1.0f);
    v.gate_on();
    static float out[2048];
    render(v, s, out, 1024);
    v.choke();
    CHECK(v.choking());
    render(v, s, out + 1024, 1024);
    CHECK(!v.sounding());
    for (int i = 1024 + FmVoice::kChokeFrames; i < 2048; i++) CHECK(out[i] == 0.0f);
    // and the fade is a fade: no sample jumps by more than the note's own slope allows
    for (int i = 1025; i < 1024 + FmVoice::kChokeFrames; i++) CHECK(std::fabs(out[i] - out[i - 1]) < 0.1f);
}

// The choke, sample by sample against the same voice unchoked: a straight line from full level
// to nothing over kChokeFrames. A choke on a silent voice is nothing, a second one does not
// restart the fade, and a new note on a choking voice cancels it.
TEST(fm_choke_is_a_straight_fade_and_a_new_note_cancels_it) {
    TrackParams p = fm_params();
    p.set(2, 60);                                       // some index: not a pure sine
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    static FmVoice a, b;
    static float oa[2048], ob[2048];
    FmVoice *both[2] = {&a, &b};
    for (FmVoice *v : both) {
        v->reset();
        v->set_note(57.0f, 1.0f);
        v->gate_on();
    }
    for (float &x : oa) x = 0.0f;
    for (float &x : ob) x = 0.0f;
    render(a, s, oa, 1024);
    render(b, s, ob, 1024);
    a.choke();
    render(a, s, oa + 1024, 32);
    a.choke();                                          // again, halfway: no restart
    render(a, s, oa + 1056, 64);
    render(b, s, ob + 1024, 96);
    int wrong = 0;
    for (int k = 0; k < 96; k++) {
        const float gain = k < FmVoice::kChokeFrames ? 1.0f - float(k) / FmVoice::kChokeFrames : 0.0f;
        wrong += std::fabs(oa[1024 + k] - ob[1024 + k] * gain) > 1e-6f ? 1 : 0;
    }
    CHECK_EQ(wrong, 0);
    CHECK(!a.choking());

    FmVoice quiet;
    quiet.reset();
    quiet.choke();
    CHECK(!quiet.choking());

    // b chokes, and halfway through takes a new note: it plays on at full level.
    b.choke();
    render(b, s, ob, 32);
    b.set_note(60.0f, 1.0f);
    b.gate_on();
    CHECK(!b.choking());
    for (float &x : ob) x = 0.0f;
    render(b, s, ob, 1024);
    CHECK(b.sounding());
    float peak = 0.0f;
    for (int i = 512; i < 1024; i++) peak = std::fabs(ob[i]) > peak ? std::fabs(ob[i]) : peak;
    CHECK(peak > 0.05f);
    // And a later choke fades from full level again, not from where the cancelled one stopped:
    // against a copy of b left unchoked.
    static FmVoice twin;
    twin = b;
    static float ot[64];
    for (float &x : ob) x = 0.0f;
    for (float &x : ot) x = 0.0f;
    b.choke();
    render(b, s, ob, 64);
    render(twin, s, ot, 64);
    wrong = 0;
    for (int k = 0; k < 64; k++) {
        const float gain = 1.0f - float(k) / FmVoice::kChokeFrames;
        wrong += std::fabs(ob[k] - ot[k] * gain) > 1e-6f ? 1 : 0;
    }
    CHECK_EQ(wrong, 0);
}

// A voice that has rung out and takes a high note sounds exactly as a fresh voice does: the band
// cap is cached per note, so a new note must recompute it rather than keep the last note's.
TEST(fm_a_reused_voice_caps_its_new_note_as_a_fresh_one_does) {
    TrackParams p = fm_params();
    p.set(2, 127);                                      // full index: the cap binds up high
    p.set(8, 0);                                        // the shortest DEC: rings out quickly
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    static FmVoice used, fresh;
    static float ou[4096], of[4096];
    used.reset();
    used.set_note(36.0f, 1.0f);
    used.gate_on();
    render(used, s, ou, 4096);
    CHECK(!used.sounding());
    fresh.reset();
    for (FmVoice *v : {&used, &fresh}) {
        v->set_note(108.0f, 1.0f);
        v->gate_on();
    }
    for (float &x : ou) x = 0.0f;
    for (float &x : of) x = 0.0f;
    render(used, s, ou, 1024);
    render(fresh, s, of, 1024);
    int wrong = 0;
    for (int i = 0; i < 1024; i++) wrong += ou[i] == of[i] ? 0 : 1;
    CHECK_EQ(wrong, 0);
    CHECK(used.last_index(2) < 0.5f * 2.0f);            // and the cap did bind
}

TEST(fm_high_notes_at_full_index_stay_bounded) {
    TrackParams p = fm_params();
    p.set(1, 127);   // RAT 14
    p.set(2, 127);
    p.set(3, 127);
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(108.0f, 1.0f);
    v.gate_on();
    static float out[4800];
    render(v, s, out, 4800);
    for (float x : out) CHECK(std::isfinite(x) && std::fabs(x) <= 0.51f);
    CHECK(v.last_index(2) <= 0.01f);   // capped: a 14:1 modulator on C8 is far past Nyquist
}

// --- the band limit for the whole chain (fm_index_cap) --------------------------------------

TEST(fm_index_cap_leaves_low_notes_and_additive_alone) {
    const float low[4] = {65.4f / kRate, 65.4f / kRate, 65.4f / kRate, 65.4f / kRate};
    // A pair (ALG 5) at IDX 127 on C2 is far inside the band: no cap. (The four-deep stack is
    // not: its bands multiply, so even C2 is capped a little.)
    CHECK(fm_index_cap(4, low, 0.0f, 1.0f) == 2.0f);
    CHECK(fm_index_cap(0, low, 0.0f, 1.0f) < 2.0f);
    // ALG 8 has no links, so no index can fold anything.
    const float high[4] = {0.2f, 0.2f, 0.2f, 0.2f};
    CHECK(fm_index_cap(7, high, 0.0f, 1.0f) == 2.0f);
    // A carrier already past the limit takes no index at all.
    const float past[4] = {1.2f, 0.01f, 0.01f, 0.01f};
    CHECK(fm_index_cap(0, past, 0.0f, 1.0f) == 0.0f);
}

TEST(fm_index_cap_sees_the_stack_a_pair_cap_misses) {
    // Middle C, every ratio 1. A pair (ALG 5) may take more index than the four-deep stack
    // (ALG 1), whose bands multiply; the old cap treated both as a pair.
    const float f = 261.63f / kRate;
    const float c4[4] = {f, f, f, f};
    const float stack = fm_index_cap(0, c4, 0.0f, 1.0f);
    const float pair = fm_index_cap(4, c4, 0.0f, 1.0f);
    CHECK(stack < pair);
    CHECK(stack > 0.1f);   // still a bright FM, not a sine
    // And it falls with the note.
    const float c5[4] = {2 * f, 2 * f, 2 * f, 2 * f};
    CHECK(fm_index_cap(0, c5, 0.0f, 1.0f) < stack);
    // Feedback widens op 4, so it lowers the cap of a stack it feeds.
    CHECK(fm_index_cap(0, c4, 0.2f, 1.0f) < stack);
}

namespace {

// The eight algorithms as the manual draws them, written out again here: which operator
// modulates which (from > to), and which are carriers.
struct Links {
    bool m12, m13, m14, m23, m24, m34;
    bool c2, c3, c4;            // operator 1 is always a carrier
};
constexpr Links kDrawn[8] = {
    {true, false, false, true, false, true, false, false, false},     // 1: 4 > 3 > 2 > 1
    {true, false, false, true, true, false, false, false, false},     // 2: (3 + 4) > 2 > 1
    {true, false, true, true, false, false, false, false, false},     // 3: 3 > 2 > 1, 4 > 1
    {true, true, false, false, false, true, false, false, false},     // 4: 4 > 3 > 1, 2 > 1
    {true, false, false, false, false, true, false, true, false},     // 5: 2 > 1, 4 > 3
    {false, false, true, false, true, true, true, true, false},       // 6: 4 > 1, 2, 3
    {false, false, false, false, false, true, true, true, false},     // 7: 4 > 3; 1, 2 plain
    {false, false, false, false, false, false, true, true, true},     // 8: four sines
};

// Carson's band of the highest carrier at index `turns`, with op 4's feedback.
double drawn_band(const Links &a, const float f[4], float feedback, double turns) {
    const double fb = 6.2831853 * feedback;
    const double b4 = f[3] * (1.0 + fb + (fb < 1.0 ? fb : 1.0));
    const double rad = 6.2831853 * turns;
    const double q = rad + (rad < 1.0 ? rad : 1.0);
    const double b3 = f[2] + (a.m34 ? q * b4 : 0.0);
    const double b2 = f[1] + (a.m23 ? q * b3 : 0.0) + (a.m24 ? q * b4 : 0.0);
    const double b1 = f[0] + (a.m12 ? q * b2 : 0.0) + (a.m13 ? q * b3 : 0.0) + (a.m14 ? q * b4 : 0.0);
    double top = b1;
    if (a.c2 && b2 > top) top = b2;
    if (a.c3 && b3 > top) top = b3;
    if (a.c4 && b4 > top) top = b4;
    return top;
}

}  // namespace

// The cap is the largest index (to the bisection's 2/65536) whose band stays under the limit,
// for every algorithm, over a grid of operator frequencies and feedbacks; no index at all
// when a modulator would be past 0.45 of the rate; the full 2 when even that fits.
TEST(fm_index_cap_is_the_largest_index_under_the_limit_for_every_algorithm) {
    const float freqs[] = {0.0004f, 0.003f, 0.011f, 0.04f, 0.13f, 0.3f, 0.449f, 0.45f, 0.6f};
    const float feedbacks[] = {0.0f, 0.1f, 0.4f};
    const double step = 2.0 / 65536.0;
    int wrong = 0, capped = 0, refused = 0;
    for (int alg = 0; alg < 8; alg++) {
        const Links &a = kDrawn[alg];
        for (int i = 0; i < 9; i++) {
            for (int j = 0; j < 9; j++) {
                for (int k = 0; k < 9; k++) {
                    for (float fb : feedbacks) {
                        const float f[4] = {freqs[(i + k) % 9] * 0.5f, freqs[i], freqs[j], freqs[k]};
                        const float cap = fm_index_cap(alg, f, fb, 1.0f);
                        const bool mod2 = a.m12, mod3 = a.m13 || a.m23, mod4 = a.m14 || a.m24 || a.m34;
                        if ((mod2 && f[1] >= 0.45f) || (mod3 && f[2] >= 0.45f) ||
                            (mod4 && f[3] >= 0.45f)) {
                            wrong += cap == 0.0f ? 0 : 1;
                            refused++;
                        } else if (drawn_band(a, f, fb, 2.0) <= 1.0) {
                            wrong += cap == 2.0f ? 0 : 1;
                        } else if (drawn_band(a, f, fb, 0.0) >= 1.0) {
                            wrong += cap == 0.0f ? 0 : 1;
                        } else {
                            capped++;
                            const bool fits = drawn_band(a, f, fb, cap) <= 1.0 + 1e-6;
                            const bool most = drawn_band(a, f, fb, cap + 2.0 * step) > 1.0 - 1e-6;
                            if (!(fits && most)) {
                                if (wrong == 0) {
                                    std::printf("    alg %d f %g %g %g %g fb %g: cap %g\n", alg + 1,
                                                double(f[0]), double(f[1]), double(f[2]),
                                                double(f[3]), double(fb), double(cap));
                                }
                                wrong++;
                            }
                        }
                    }
                }
            }
        }
        // Out of range: a negative algorithm is the first, and the count wraps.
        const float f[4] = {0.01f, 0.02f, 0.03f, 0.04f};
        wrong += fm_index_cap(alg + 8, f, 0.1f, 1.0f) == fm_index_cap(alg, f, 0.1f, 1.0f) ? 0 : 1;
    }
    const float f[4] = {0.01f, 0.02f, 0.03f, 0.04f};
    CHECK(fm_index_cap(-3, f, 0.1f, 1.0f) == fm_index_cap(0, f, 0.1f, 1.0f));
    CHECK_EQ(wrong, 0);
    CHECK(capped > 200);
    CHECK(refused > 100);
}

TEST(fm_stack_at_high_index_does_not_fold_tones_under_5k) {
    // ALG 1 at IDX 90 on A4, held: before the chain-wide cap a folded tone stood 9 dB under the
    // loudest harmonic. A4 is 44 cycles in 4,800 frames, so every harmonic lands on a bin of a
    // 4,800-point DFT (10 Hz apart) and anything on another bin under 5 kHz is a fold.
    TrackParams p = fm_params();
    p.set(1, 20);    // RAT 1
    p.set(2, 90);
    p.set(kAsrFirst + 1, 127);   // SUS: hold
    p.set(kOscFirst + 1, 64);    // FIN 0 (a zeroed byte is -0.5)
    const FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    FmVoice v;
    v.reset();
    v.set_note(69.0f, 1.0f);
    v.gate_on();
    static float out[9600];
    render(v, s, out, 9600);
    const float *x = out + 4800;
    double harm = 0.0, fold = 0.0;
    for (int bin = 1; bin <= 500; bin++) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < 4800; i++) {
            const double w = 2.0 * M_PI * bin * i / 4800.0;
            re += x[i] * std::cos(w);
            im += x[i] * std::sin(w);
        }
        const double e = re * re + im * im;
        if (bin % 44 == 0) harm = e > harm ? e : harm;
        else fold = e > fold ? e : fold;
    }
    CHECK(harm > 0.0);
    CHECK(10.0 * std::log10(fold / harm) < -40.0);
}
