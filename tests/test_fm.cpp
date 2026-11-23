// The FM machine's voice (dsp/fm.cpp) and its operator loop (dsp/fmloops.ti.c).
#include "check.hpp"

#include "fm.hpp"

#include <cmath>

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
