// FM4: the four-operator machine (dsp/fmpoly.hpp), six voices on the slot logic FM and STRING
// share (dsp/polypool.hpp). dsp/fm.hpp's voice is pinned in tests/test_fm.cpp; this file pins
// the machine around it:
//   - its pages' names, and that the knobs reach the voice through prepare();
//   - chords take a voice a note, the same note retriggers in place, a seventh steals;
//   - SUS 0 is a one-shot, SUS up holds until release, a gate lets go on its frame;
//   - the budget's four calls answer as VA POLY's do;
//   - every factory preset sounds, stays bounded, and sits near VA POLY's level.
#include "check.hpp"

#include <enaga/abi.h>

#include "fmpoly.hpp"
#include "sounds.hpp"
#include "vapoly.hpp"

#include <cmath>
#include <string_view>

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kLen = ENAGA_SAMPLE_RATE / 2;   // half a second

constexpr int kAlg = 0, kRat = 1, kIdx = 2, kDec = 8, kLvl = 11;
constexpr int kSus = kAsrFirst + 1, kRel = kAsrFirst + 2;

float g_out[kLen];
FmPoly g_fm;

// A plain patch: a 1:1 pair, some index, long decay, full level, held.
TrackParams patch(uint8_t sus = 127) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.set(i, 0);
    p.set(kAlg, 8);
    p.set(kRat, 24);
    p.set(kIdx, 40);
    p.set(kDec, 110);
    p.set(kLvl, 127);
    p.set(kSus, sus);
    p.set(kRel, 0);
    p.set(kOscFirst + 1, 64);
    return p;
}

Trig hit(uint8_t note, bool held = true, uint32_t frame = 0) {
    Trig t;
    t.note = note;
    t.velocity = 127;
    t.held = held;
    t.frame = frame;
    return t;
}

void render(Machine &m, const TrackParams &p, float *out, int frames) {
    for (int i = 0; i < frames; i++) out[i] = 0.0f;
    for (int b = 0; b + kBlk <= frames; b += kBlk) {
        m.prepare(p);
        m.render_add(out + b, kBlk);
    }
}

double rms(const float *x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += double(x[i]) * x[i];
    return std::sqrt(s / n);
}

float peak(const float *x, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; i++) m = std::fabs(x[i]) > m ? std::fabs(x[i]) : m;
    return m;
}

FmPoly &fresh(const TrackParams &p) {
    g_fm.init(kRate);
    g_fm.prepare(p);
    return g_fm;
}

TrackParams preset_params(const SoundPreset &s) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.set(i, 0);
    for (int k = 0; k < 4; k++) {
        p.set(k, s.src[k]);
        p.set(8 + k, s.amp[k]);
        p.set(kAsrFirst + k, s.asr[k]);
        p.set(kOscFirst + k, s.osc[k]);
    }
    return p;
}

// One note's level over its first half second, the way the factory lists are balanced: a
// three-note chord, since both machines are mostly played in chords.
double chord_rms(Machine &m, const TrackParams &p) {
    m.kill_all();
    m.prepare(p);
    m.trigger(hit(48));
    m.trigger(hit(55));
    m.trigger(hit(64));
    render(m, p, g_out, kLen);
    return rms(g_out, kLen);
}

}  // namespace

TEST(fm_machine_names_its_pages) {
    FmPoly &fm = fresh(patch());
    CHECK_EQ(fm.param_name(0), "ALG");
    CHECK_EQ(fm.param_name(1), "RAT");
    CHECK_EQ(fm.param_name(2), "IDX");
    CHECK_EQ(fm.param_name(3), "FBK");
    CHECK_EQ(fm.param_name(8), "DEC");
    CHECK_EQ(fm.param_name(9), "IEN");
    CHECK_EQ(fm.param_name(10), "VEL");
    CHECK_EQ(fm.param_name(11), "LVL");
    CHECK_EQ(fm.param_name(4), "---");
    CHECK_EQ(fm.param_name(99), "---");
    CHECK_EQ(fm.used_params(), 8);
}

TEST(fm_machine_prepare_reaches_the_settings) {
    TrackParams p = patch();
    p.set(kAlg, 120);          // A8
    p.set(kRat, 72);           // 3.5
    p.set(kOscFirst + 3, 38);  // carrier 0.5
    FmPoly &fm = fresh(p);
    CHECK_EQ(fm.settings().algorithm, 7);
    CHECK(std::fabs(fm.settings().ratio[1] - 3.5f) < 1e-5f);
    CHECK(std::fabs(fm.settings().ratio[0] - 0.5f) < 1e-5f);
    CHECK(fm.sustains());
    p.set(kSus, 0);
    fm.prepare(p);
    CHECK(!fm.sustains());
}

TEST(fm_machine_a_chord_takes_a_voice_a_note_and_a_repeat_retriggers_in_place) {
    const TrackParams p = patch();
    FmPoly &fm = fresh(p);
    fm.trigger(hit(48));
    fm.trigger(hit(52));
    fm.trigger(hit(55));
    CHECK_EQ(fm.busy_voices(), 3);
    render(fm, p, g_out, kBlk * 4);
    CHECK_EQ(fm.sounding_voices(), 3);
    CHECK(rms(g_out, kBlk * 4) > 0.01);
    int slot_of_52 = -1;
    for (int s = 0; s < FmPoly::kVoices; s++) {
        if (fm.voice_note(s) == 52) slot_of_52 = s;
    }
    CHECK(slot_of_52 >= 0);
    fm.trigger(hit(52));
    render(fm, p, g_out, kBlk);
    CHECK_EQ(fm.sounding_voices(), 3);
    CHECK_EQ(fm.voice_note(slot_of_52), 52);
}

TEST(fm_machine_a_seventh_note_steals_one_and_the_pool_stays_six) {
    const TrackParams p = patch();
    FmPoly &fm = fresh(p);
    for (int n = 0; n < FmPoly::kVoices; n++) fm.trigger(hit(uint8_t(48 + 2 * n)));
    render(fm, p, g_out, kBlk * 2);
    CHECK_EQ(fm.sounding_voices(), FmPoly::kVoices);
    fm.trigger(hit(80));
    render(fm, p, g_out, kBlk * 2);
    CHECK_EQ(fm.busy_voices(), FmPoly::kVoices);
    bool has80 = false;
    for (int s = 0; s < FmPoly::kVoices; s++) has80 = has80 || fm.voice_note(s) == 80;
    CHECK(has80);
}

TEST(fm_machine_sus_0_is_a_one_shot_and_sus_up_holds_until_release) {
    // SUS 0, a short decay: the note dies by itself, held or not.
    TrackParams p = patch(0);
    p.set(kDec, 20);
    FmPoly &fm = fresh(p);
    fm.trigger(hit(60));
    render(fm, p, g_out, kLen);
    CHECK(fm.sounding_voices() == 0);
    CHECK(!fm.active());

    // SUS up: still sounding after half a second, then silent soon after a release.
    p = patch(127);
    fresh(p).trigger(hit(60));
    render(fm, p, g_out, kLen);
    CHECK_EQ(fm.sounding_voices(), 1);
    CHECK(rms(g_out + kLen - 4800, 4800) > 0.01);
    fm.release(60);
    render(fm, p, g_out, kLen);
    CHECK_EQ(fm.sounding_voices(), 0);
}

TEST(fm_machine_a_gate_lets_go_on_its_frame) {
    const TrackParams p = patch(127);
    FmPoly &fm = fresh(p);
    Trig t = hit(60, false);
    t.gate_frames = uint32_t(kBlk * 3);
    fm.trigger(t);
    render(fm, p, g_out, kBlk * 2);
    CHECK(fm.voice(0).sounding() && !fm.voice(0).released());
    render(fm, p, g_out, kBlk * 2);
    CHECK(fm.voice(0).released());
}

TEST(fm_machine_a_trig_starts_on_its_own_frame) {
    const TrackParams p = patch();
    FmPoly &fm = fresh(p);
    fm.trigger(hit(60, true, 37));
    render(fm, p, g_out, kBlk);
    for (int i = 0; i < 37; i++) CHECK(g_out[i] == 0.0f);
    CHECK(peak(g_out + 37, kBlk - 37) > 0.0f);
}

TEST(fm_machine_answers_the_budget_as_va_poly_does) {
    const TrackParams p = patch();
    FmPoly &fm = fresh(p);
    PolyMachine &pm = fm;
    CHECK_EQ(pm.budget_voices(), 0);
    CHECK(pm.would_add_voice(60));
    fm.trigger(hit(60));
    CHECK_EQ(pm.budget_voices(), 1);    // pending counts
    render(fm, p, g_out, kBlk);
    CHECK(!pm.would_add_voice(60));     // retriggers in place
    CHECK(pm.would_add_voice(62));
    float level = -1.0f;
    const int slot = pm.quietest_voice(level);
    CHECK(slot >= 0 && level > 0.0f);
    pm.choke(slot);
    CHECK_EQ(pm.budget_voices(), 0);    // a choke is paid for
    render(fm, p, g_out, kBlk * 2);
    CHECK_EQ(fm.sounding_voices(), 0);
    // Out-of-range slots are ignored.
    pm.choke(-1);
    pm.choke(FmPoly::kVoices);
}

TEST(fm_machine_kill_all_silences_at_once) {
    const TrackParams p = patch();
    FmPoly &fm = fresh(p);
    fm.trigger(hit(60));
    fm.trigger(hit(64, true, 500));
    render(fm, p, g_out, kBlk);
    fm.kill_all();
    CHECK(!fm.active());
    CHECK_EQ(fm.busy_voices(), 0);
    render(fm, p, g_out, kBlk);
    CHECK(peak(g_out, kBlk) == 0.0f);
}

TEST(fm_machine_every_factory_preset_sounds_and_sits_near_va_poly) {
    const uint8_t id = uint8_t(MachineId::fm);
    CHECK(sound_preset_count(id) >= 5);
    // VA POLY's list, as the reference level.
    static VaPoly va;
    va.init(kRate);
    double va_sum = 0.0;
    const int va_n = sound_preset_count(uint8_t(MachineId::va_poly));
    for (int i = 0; i < va_n; i++) {
        va_sum += chord_rms(va, preset_params(*sound_preset(uint8_t(MachineId::va_poly), i)));
    }
    const double va_mean = va_sum / va_n;
    g_fm.init(kRate);
    for (int i = 0; i < sound_preset_count(id); i++) {
        const TrackParams p = preset_params(*sound_preset(id, i));
        const double level = chord_rms(g_fm, p);
        bool ok = true;
        for (float x : g_out) ok = ok && std::isfinite(x) && std::fabs(x) < 2.0f;
        CHECK(ok);
        // Within 3 dB of VA POLY's average, so switching machine on a track does not jump
        // the mix.
        CHECK(level > va_mean * 0.7);
        CHECK(level < va_mean * 1.4);
    }
}

TEST(fm_machine_epiano_strikes_bright_and_mellows) {
    // An electric piano's character is its strike: bright for the first few tens of ms, then
    // mellowing as the note rings. FM4 EPIANO had almost nothing above 1 kHz even in its attack
    // (the sound review, 2026-11-27). Brightness here is the energy of the first difference over
    // the energy, which grows with the square of frequency.
    const uint8_t id = uint8_t(MachineId::fm);
    const SoundPreset *ep = nullptr;
    for (int i = 0; i < sound_preset_count(id); i++) {
        if (std::string_view(sound_preset(id, i)->name) == "EPIANO") ep = sound_preset(id, i);
    }
    CHECK(ep != nullptr);
    if (ep == nullptr) return;
    const TrackParams p = preset_params(*ep);
    FmPoly &fm = fresh(p);
    Trig t = hit(60, false);
    t.velocity = 100;
    fm.trigger(t);
    render(fm, p, g_out, kLen);
    auto brightness = [](const float *x, int n) {
        double e = 0.0, d = 0.0;
        for (int i = 1; i < n; i++) {
            e += double(x[i]) * x[i];
            d += double(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        }
        return d / (e + 1e-30);
    };
    const int w = int(0.03f * kRate);
    const double strike = brightness(g_out, w);
    const double ring = brightness(g_out + int(0.3f * kRate), w);
    // Before: 0.0051 against 0.0014, a strike only 3.6x the ring. Now 0.020 against 0.0014.
    CHECK(strike > 8.0 * ring);
    CHECK(strike > 0.01);
}

TEST(fm_machine_unison_puts_the_sharp_voice_on_the_right) {
    FmPoly &fm = fresh(patch());
    fm.set_width(127);
    fm.trigger(hit(60));
    render(fm, patch(), g_out, kBlk);
    CHECK(fm.voice_pan(0) < 0.0f && fm.voice_pan(1) > 0.0f);
    CHECK(std::fabs(fm.voice(1).pitch() - fm.voice(0).pitch() - 0.40f) < 1e-4f);
    fm.set_width(0);
    CHECK_EQ(fm.param_name(kParams), "---");
    CHECK_EQ(fm.param_name(-1), "---");
}

// Triggered before any prepare(), FM4 plays the patch init() promises: every byte at 64, the ASR
// page at 0, and the OSC page at rest (no detune, no keytrack, carrier 1; fine at its centre).
TEST(fm_machine_plays_its_rest_patch_before_the_first_prepare) {
    static float before[kLen / 4], after[kLen / 4];
    TrackParams rest{};
    for (int i = 0; i < kTrackParams; i++) rest.set(i, 64);
    for (int i = 0; i < kAsrParams; i++) rest.set(kAsrFirst + i, 0);
    rest.set(kOscFirst + 0, 0);
    rest.set(kOscFirst + 2, 0);
    rest.set(kOscFirst + 3, 0);
    for (int pass = 0; pass < 2; pass++) {
        g_fm.init(kRate);
        if (pass == 1) g_fm.prepare(rest);
        g_fm.trigger(hit(64));
        float *out = pass == 0 ? before : after;
        for (int i = 0; i < kLen / 4; i++) out[i] = 0.0f;
        for (int b = 0; b + kBlk <= kLen / 4; b += kBlk) g_fm.render_add(out + b, kBlk);
    }
    int wrong = 0;
    for (int i = 0; i < kLen / 4; i++) wrong += before[i] == after[i] ? 0 : 1;
    CHECK_EQ(wrong, 0);
    CHECK(rms(after, kLen / 4) > 0.01);
}
