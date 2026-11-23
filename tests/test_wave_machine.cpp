// WAVE MONO and WAVE POLY: the VA voice with a wavetable where its two oscillators were
// (dsp/va.cpp, render_wave; the tables in dsp/wavetable.cpp).
//
// What is pinned here:
//   - a note plays at its pitch, and POS moves the timbre along the table (CLASSIC's sine to
//     its square);
//   - MOT moves the position with the filter envelope, either way;
//   - DET adds a second reader, which beats against the first;
//   - the knobs' screen mappings, and the pages the two machines name;
//   - with no library the voice plays its sub alone rather than reading through nullptr;
//   - every table, every corner, stays finite and bounded; every factory preset sounds.
#include "check.hpp"

#include <enaga/abi.h>

#include "sounds.hpp"
#include "va.hpp"
#include "vapoly.hpp"
#include "wavetable.hpp"

#include <cmath>
#include <string_view>

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kLen = ENAGA_SAMPLE_RATE / 2;   // half a second

constexpr int kTbl = 0, kPos = 1, kCut = 2, kRes = 3, kDec = 8, kEnv = 9, kMot = 10, kLvl = 11;
constexpr int kSus = kAsrFirst + 1;
constexpr int kWrp = kOscFirst + 0, kDet = kOscFirst + 1, kSub = kOscFirst + 2;

WaveLibrary g_lib;
float g_out[kLen];

const WaveLibrary &library() {
    if (!g_lib.ready()) CHECK(g_lib.init());
    set_wave_library(&g_lib);
    return g_lib;
}

// A held, open patch: the table alone through a filter that does nothing to it.
TrackParams patch(int table, uint8_t pos) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.set(i, 0);
    p.set(kTbl, uint8_t(table * 8 + 4));
    p.set(kPos, pos);
    p.set(kCut, 127);
    p.set(kRes, 0);
    p.set(kDec, 100);
    p.set(kEnv, 0);
    p.set(kMot, 64);
    p.set(kLvl, 127);
    p.set(kSus, 127);
    return p;
}

Trig hit(uint8_t note) {
    Trig t;
    t.note = note;
    t.velocity = 127;
    t.held = true;
    return t;
}

void render(Machine &m, const TrackParams &p, float *out, int frames) {
    for (int i = 0; i < frames; i++) out[i] = 0.0f;
    for (int b = 0; b + kBlk <= frames; b += kBlk) {
        m.prepare(p);
        m.render_add(out + b, kBlk);
    }
}

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

// Note 57, 220 Hz: a tail of 9600 samples is 5 Hz bins, and 220 lands on one.
void play(Machine &m, const TrackParams &p, uint8_t note = 57) {
    m.kill_all();
    m.prepare(p);
    m.trigger(hit(note));
    render(m, p, g_out, kLen);
}

}  // namespace

TEST(wave_knob_mappings) {
    CHECK_EQ(va_wave_table(0), 0);
    CHECK_EQ(va_wave_table(7), 0);
    CHECK_EQ(va_wave_table(8), 1);
    CHECK_EQ(va_wave_table(127), kWaveTables - 1);
    CHECK(va_wave_position(0) == 0.0f);
    CHECK(std::fabs(va_wave_position(127) - float(kWaveFrames - 1)) < 1e-4f);
    CHECK(va_wave_motion(64) == 0.0f);
    CHECK(va_wave_motion(127) == float(kWaveFrames - 1));
    CHECK(va_wave_motion(0) == -float(kWaveFrames - 1));
    CHECK(va_wave_detune_cents(0) == 0.0f);
    CHECK(std::fabs(va_wave_detune_cents(127) - 60.0f) < 0.01f);
}

TEST(wave_machines_name_their_own_pages) {
    VaMono mono;
    mono.set_wave(true);
    mono.init(kRate);
    VaPoly poly;
    poly.set_wave(true);
    poly.init(kRate);
    CHECK_EQ(mono.param_name(0), "TBL");
    CHECK_EQ(mono.param_name(1), "POS");
    CHECK_EQ(mono.param_name(10), "MOT");
    CHECK_EQ(poly.param_name(10), "MOT");
    // The filter is fixed at LP24 whatever byte 1 holds: it is POS here.
    mono.prepare(patch(0, 127));
    CHECK(mono.mode() == VaMode::lp24);
    CHECK_EQ(mono.settings().table, 0);
    poly.prepare(patch(5, 0));
    CHECK_EQ(poly.settings().table, 5);
    CHECK(poly.detune_semitones() == 0.0f);   // slot 10 is MOT, not the chord detune
    // A VA keeps its oscillators.
    VaMono va;
    va.init(kRate);
    va.prepare(patch(0, 0));
    CHECK_EQ(va.settings().table, -1);
}

TEST(wave_mono_plays_the_note_and_pos_walks_the_table) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    const int from = kLen - 9600;

    play(m, patch(0, 0));   // CLASSIC frame 0: a sine
    const double f_sine = magnitude(g_out + from, 9600, 220.0);
    const double h3_sine = magnitude(g_out + from, 9600, 660.0);
    CHECK(f_sine > 0.05);
    CHECK(h3_sine < 0.02 * f_sine);
    CHECK(f_sine > 20.0 * magnitude(g_out + from, 9600, 230.0));

    play(m, patch(0, 127));   // CLASSIC frame 15: a square, a third at a third of the fundamental
    const double f_sq = magnitude(g_out + from, 9600, 220.0);
    const double h3_sq = magnitude(g_out + from, 9600, 660.0);
    CHECK(f_sq > 0.05);
    CHECK(h3_sq > 0.2 * f_sq);
    CHECK(magnitude(g_out + from, 9600, 440.0) < 0.05 * f_sq);   // and no even harmonics
}

TEST(wave_mot_moves_the_position_with_the_filter_envelope) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    TrackParams p = patch(0, 0);
    p.set(kDec, 70);                     // a filter envelope that falls well inside the note
    p.set(kAsrFirst + 3, 0);             // FSU 0: and falls all the way
    p.set(kMot, 127);                    // +15 frames at its top
    play(m, p);
    // Early, the envelope has the position at the square end; late, back at the sine.
    const double early = magnitude(g_out + 240, 1200, 660.0) / magnitude(g_out + 240, 1200, 220.0);
    const double late = magnitude(g_out + kLen - 9600, 9600, 660.0) /
                        magnitude(g_out + kLen - 9600, 9600, 220.0);
    CHECK(early > 0.1);   // 0.16 measured: a square's third is 0.33, the sine's none
    CHECK(late < 0.03);

    // Negative MOT from the square end: the envelope pulls it down to the sine first.
    p.set(kPos, 127);
    p.set(kMot, 0);
    play(m, p);
    const double early_down = magnitude(g_out + 240, 1200, 660.0) / magnitude(g_out + 240, 1200, 220.0);
    const double late_down = magnitude(g_out + kLen - 9600, 9600, 660.0) /
                             magnitude(g_out + kLen - 9600, 9600, 220.0);
    CHECK(early_down < 0.1);
    CHECK(late_down > 0.2);
}

TEST(wave_det_adds_a_second_reader_that_beats) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    TrackParams p = patch(0, 0);
    play(m, p);
    // One reader: the level is steady across the tail.
    const double a1 = rms(g_out + kLen - 9600, 2400), b1 = rms(g_out + kLen - 4800, 2400);
    CHECK(std::fabs(a1 - b1) < 0.02 * a1);
    CHECK(!m.settings().warped);

    p.set(kDet, 127);   // 60 cents between them at 220 Hz: about 7.6 Hz of beating
    play(m, p);
    CHECK(m.settings().osc2 == VaOsc2::detune);
    double lo = 1e9, hi = 0.0;
    for (int w = kLen - 24000; w + 960 <= kLen; w += 480) {
        const double r = rms(g_out + w, 960);
        lo = r < lo ? r : lo;
        hi = r > hi ? r : hi;
    }
    CHECK(hi > 2.0 * lo);
}

TEST(wave_with_no_library_plays_only_its_sub) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    TrackParams p = patch(0, 0);
    set_wave_library(nullptr);
    play(m, p);
    CHECK(rms(g_out + kLen - 9600, 9600) < 0.02);   // the trace of noise, nothing more
    p.set(kSub, 127);
    play(m, p);
    CHECK(magnitude(g_out + kLen - 9600, 9600, 110.0) > 0.05);
    set_wave_library(&g_lib);
}

TEST(wave_every_table_and_corner_stays_finite_and_bounded) {
    library();
    static VaPoly m;
    m.set_wave(true);
    m.init(kRate);
    static float out[kBlk * 64];
    for (int table = 0; table < kWaveTables; table++) {
        for (int corner = 0; corner < 4; corner++) {
            TrackParams p = patch(table, corner & 1 ? 127 : 0);
            p.set(kWrp, corner & 2 ? 127 : 0);
            p.set(kDet, corner & 2 ? 127 : 0);
            p.set(kRes, corner == 3 ? 127 : 0);
            p.set(kMot, corner == 1 ? 0 : 127);
            p.set(kEnv, 127);
            m.kill_all();
            m.prepare(p);
            for (uint8_t note : {uint8_t(0), uint8_t(60), uint8_t(127)}) m.trigger(hit(note));
            render(m, p, out, kBlk * 64);
            bool ok = true;
            for (float x : out) ok = ok && std::isfinite(x) && std::fabs(x) < 4.0f;
            CHECK(ok);
            CHECK(rms(out, kBlk * 64) > 1e-4);
        }
    }
}

TEST(wave_every_factory_preset_sounds) {
    library();
    static VaMono mono;
    mono.set_wave(true);
    mono.init(kRate);
    static VaPoly poly;
    poly.set_wave(true);
    poly.init(kRate);
    for (int which = 0; which < 2; which++) {
        const uint8_t id = uint8_t(which == 0 ? MachineId::wave_mono : MachineId::wave_poly);
        Machine &m = which == 0 ? static_cast<Machine &>(mono) : static_cast<Machine &>(poly);
        CHECK(sound_preset_count(id) >= 5);
        for (int i = 0; i < sound_preset_count(id); i++) {
            const SoundPreset *s = sound_preset(id, i);
            TrackParams p{};
            for (int k = 0; k < 4; k++) {
                p.set(k, s->src[k]);
                p.set(8 + k, s->amp[k]);
                p.set(kAsrFirst + k, s->asr[k]);
                p.set(kOscFirst + k, s->osc[k]);
            }
            play(m, p, 48);
            bool ok = true;
            for (float x : g_out) ok = ok && std::isfinite(x) && std::fabs(x) < 2.0f;
            CHECK(ok);
            CHECK(rms(g_out, 9600) > 0.01);
        }
    }
}

// WRP squeezes the first half of the cycle into as little as 2% of it, so that half plays up to
// 25 times faster than the note. The mip level has to allow for it, and on a high note the bend
// itself is eased (dsp/va.cpp, kWarpFastest), or the note aliases: 10 dB down before, 32 now.
TEST(wave_warp_picks_a_mip_level_that_does_not_alias) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    TrackParams p = patch(0, 127);   // CLASSIC's square: every odd harmonic
    p.set(kWrp, 127);
    play(m, p, 93);                  // 1,760 Hz: harmonics at multiples, nothing between
    // A periodic note has energy only at multiples of its pitch. Measure what lies between them
    // (the aliases fold to arbitrary places) against what lies on them.
    const float *x = g_out + kLen - 9600;   // 5 Hz bins; 1,760 is bin 352
    double on = 0.0, off = 0.0;
    for (int bin = 20; bin < 3600; bin++) {   // 100 Hz to 18 kHz, where the tables keep aliases out
        const double hz = 5.0 * bin;
        const double mag = magnitude(x, 9600, hz);
        const double harmonic = hz / 1760.0;
        const double nearest = std::floor(harmonic + 0.5);
        if (std::fabs(harmonic - nearest) * 1760.0 < 15.0) on += mag * mag;
        else off += mag * mag;
    }
    CHECK(off < 1e-3 * on);   // aliases at least 30 dB down
}
