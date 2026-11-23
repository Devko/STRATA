// The STRING machine's voice (dsp/plucked.cpp) and its loops (dsp/pluckloops.ti.c).
#include "check.hpp"

#include "plucked.hpp"

#include <cmath>

using namespace strata;

namespace {

constexpr float kRate = 48000.0f;

TrackParams pluck_params(bool bar) {
    TrackParams p{};
    p.set(0, bar ? 100 : 0);   // MOD
    p.set(1, 0);               // EXC pluck
    p.set(2, 80);              // BRT
    p.set(3, 30);              // POS
    p.set(8, 80);              // DEC
    p.set(9, 0);               // INH
    p.set(10, 0);              // VEL
    p.set(11, 127);            // LVL
    return p;
}

struct Rig {
    PluckVoice voice;
    float scratch[STRATA_PLUCK_LINE];
    float work[PluckVoice::kControl];
    float env[PluckVoice::kControl];

    Rig() { voice.init(); }

    void start(const PluckSettings &s, float note, float velocity = 1.0f) {
        voice.set_note(note, velocity);
        voice.gate_on(s, scratch);
    }
    void render(const PluckSettings &s, float *out, int frames) {
        for (int i = 0; i < frames; i += PluckVoice::kControl) {
            const int n = frames - i < PluckVoice::kControl ? frames - i : PluckVoice::kControl;
            voice.run(s, out + i, n, work, env);
        }
    }
};

double magnitude(const float *x, int n, double hz) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) {
        const double w = 2.0 * M_PI * hz * i / kRate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

double rms(const float *x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += double(x[i]) * x[i];
    return std::sqrt(s / n);
}

}  // namespace

TEST(pluck_knob_zones) {
    CHECK(pluck_model(0) == PluckModel::string);
    CHECK(pluck_model(127) == PluckModel::bar);
    CHECK(pluck_exciter(0) == PluckExciter::pluck);
    CHECK(pluck_exciter(64) == PluckExciter::noise);
    CHECK(pluck_exciter(127) == PluckExciter::mallet);
    CHECK(std::fabs(pluck_decay_seconds(0) - 0.1f) < 1e-3f);
    CHECK(std::fabs(pluck_decay_seconds(127) - 20.0f) < 0.05f);
}

TEST(pluck_string_rings_at_the_note) {
    const PluckSettings s = PluckSettings::make(pluck_params(false), kRate, 1.0f / kRate);
    static Rig rig;
    rig.voice.reset();
    rig.start(s, 57.0f);   // 220 Hz
    static float out[28800];
    rig.render(s, out, 28800);
    const float *tail = out + 4800;   // half a second: 2 Hz resolution
    const double f = magnitude(tail, 24000, 220.0);
    CHECK(f > 0.005);
    CHECK(f > 5.0 * magnitude(tail, 24000, 213.0));
    CHECK(f > 5.0 * magnitude(tail, 24000, 227.0));
    // and in tune: the loop filter's delay is taken out of the line, so 220 Hz beats 218 and 222
    CHECK(f > magnitude(tail, 24000, 218.0) && f > magnitude(tail, 24000, 222.0));
}

TEST(pluck_string_decays_and_frees_its_voice) {
    TrackParams p = pluck_params(false);
    p.set(8, 0);   // DEC 0.1 s
    const PluckSettings s = PluckSettings::make(p, kRate, 1.0f / kRate);
    static Rig rig;
    rig.voice.reset();
    rig.start(s, 60.0f);
    CHECK(rig.voice.sounding());
    static float out[48000];
    rig.render(s, out, 48000);
    CHECK(rms(out, 2400) > 10.0 * rms(out + 9600, 2400));
    CHECK(!rig.voice.sounding());
}

TEST(pluck_release_mutes_the_string) {
    TrackParams p = pluck_params(false);
    p.set(8, 127);   // DEC 20 s: it would ring for ever
    const PluckSettings s = PluckSettings::make(p, kRate, 1.0f / kRate);
    static Rig ringing, muted;
    ringing.voice.reset();
    muted.voice.reset();
    ringing.start(s, 60.0f);
    muted.start(s, 60.0f);
    static float a[24000], b[24000];
    ringing.render(s, a, 4800);
    muted.render(s, b, 4800);
    muted.voice.gate_off();
    CHECK(muted.voice.released());
    ringing.render(s, a + 4800, 19200);
    muted.render(s, b + 4800, 19200);
    CHECK(rms(b + 19200, 4800) < 0.01 * rms(a + 19200, 4800));
}

TEST(pluck_bar_modes_move_with_inh) {
    TrackParams p = pluck_params(true);
    p.set(1, 127);   // mallet
    p.set(2, 127);   // bright: the upper modes loud
    p.set(3, 15);    // struck near the end, where every mode moves
    p.set(8, 100);
    const float hz = 220.0f;
    static Rig rig;
    static float out[9600];
    {
        const PluckSettings s = PluckSettings::make(p, kRate, 1.0f / kRate);
        rig.voice.reset();
        rig.start(s, 57.0f);
        rig.render(s, out, 9600);
        CHECK(magnitude(out, 9600, hz * 4.0) > 5.0 * magnitude(out, 9600, hz * 2.756));
    }
    p.set(9, 127);   // INH: a free bar
    {
        const PluckSettings s = PluckSettings::make(p, kRate, 1.0f / kRate);
        for (float &x : out) x = 0.0f;
        rig.voice.reset();
        rig.start(s, 57.0f);
        rig.render(s, out, 9600);
        CHECK(magnitude(out, 9600, hz * 2.756) > 5.0 * magnitude(out, 9600, hz * 4.0));
        CHECK(magnitude(out, 9600, hz) > 0.005);
    }
}

TEST(pluck_choke_ends_the_voice_in_its_frames) {
    const PluckSettings s = PluckSettings::make(pluck_params(false), kRate, 1.0f / kRate);
    static Rig rig;
    rig.voice.reset();
    rig.start(s, 48.0f);
    static float out[2048];
    rig.render(s, out, 1024);
    rig.voice.choke();
    rig.render(s, out + 1024, 1024);
    CHECK(!rig.voice.sounding());
    for (int i = 1024 + PluckVoice::kChokeFrames; i < 2048; i++) CHECK(out[i] == 0.0f);
}

TEST(pluck_extremes_stay_finite_and_bounded) {
    for (int bar = 0; bar < 2; bar++)
        for (int exc = 0; exc < 128; exc += 64)
            for (int note = 0; note <= 127; note += 127) {
                TrackParams p = pluck_params(bar != 0);
                p.set(1, uint8_t(exc));
                p.set(2, 127);
                p.set(8, 127);
                const PluckSettings s = PluckSettings::make(p, kRate, 1.0f / kRate);
                static Rig rig;
                rig.voice.reset();
                rig.start(s, float(note));
                static float out[4800];
                for (float &x : out) x = 0.0f;
                rig.render(s, out, 4800);
                for (float x : out) CHECK(std::isfinite(x) && std::fabs(x) < 4.0f);
            }
}
