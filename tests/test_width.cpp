// WID, the stereo width of a POLY track (core/width.hpp): the zones, where each voice of VA POLY
// and FM4 sits and how unison tunes its pair, and the insert chain's stereo path.
#include "check.hpp"

#include <cstring>

#include <enaga/abi.h>

#include "chain.hpp"
#include "fmpoly.hpp"
#include "vapoly.hpp"
#include "width.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kBlocks = 200;

float g_mono[kBlocks * kBlk];
float g_left[kBlocks * kBlk];
float g_right[kBlocks * kBlk];

float absolute(float v) { return v < 0.0f ? -v : v; }

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.set(i, value);
    return p;
}

// A held VA POLY chord voice: SUS up (ASR slot 1), a mid filter, a long decay.
TrackParams va_params() {
    TrackParams p = uniform(64);
    p.set(kAsrFirst + 1, 127);      // SUS: held
    p.set(10, 0);                   // DET 0: the slots in tune with each other
    return p;
}

Trig note(uint8_t n) {
    Trig t;
    t.note = n;
    t.velocity = 100;
    t.held = true;
    return t;
}

// Renders `blocks` blocks of `m` into g_mono, or in stereo into g_left / g_right.
void render(PolyMachine &m, bool stereo, int blocks) {
    for (int b = 0; b < blocks; b++) {
        float *l = (stereo ? g_left : g_mono) + b * kBlk;
        float *r = g_right + b * kBlk;
        for (int i = 0; i < kBlk; i++) {
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        if (stereo) m.render_add_stereo(l, r, kBlk);
        else m.render_add(l, kBlk);
    }
}

float energy(const float *x, int n) {
    float e = 0.0f;
    for (int i = 0; i < n; i++) e += x[i] * x[i];
    return e;
}

}  // namespace

// --- the zones --------------------------------------------------------------------------------

TEST(width_zones_are_mono_spread_and_unison) {
    CHECK(!width_stereo(0));
    CHECK(width_stereo(1));
    CHECK(width_stereo(127));
    CHECK(!width_unison(63));
    CHECK(width_unison(64));
    CHECK_EQ(width_spread(0), 0.0f);
    CHECK_EQ(width_spread(63), 1.0f);
    CHECK_EQ(width_spread(100), 1.0f);
    CHECK(width_spread(20) > 0.3f && width_spread(20) < 0.33f);
    // Slot 0 is always the middle; at full spread slots 1 and 2 are hard left and right.
    for (uint8_t w = 0; w < 64; w++) CHECK_EQ(width_slot_pan(w, 0), 0.0f);
    CHECK_EQ(width_slot_pan(63, 1), -1.0f);
    CHECK_EQ(width_slot_pan(63, 2), 1.0f);
    // In unison the pan is the pair's, not the slot's.
    CHECK_EQ(width_slot_pan(100, 1), 0.0f);
    // Unison's half gap: 3 cents at 64, 20 at 127, and none outside it.
    CHECK_EQ(width_unison_semis(63), 0.0f);
    CHECK(absolute(width_unison_semis(64) - 0.03f) < 1e-6f);
    CHECK(absolute(width_unison_semis(127) - 0.20f) < 1e-6f);
    // The zones are monotonic.
    for (uint8_t w = 65; w <= 127; w++) CHECK(width_unison_semis(w) > width_unison_semis(uint8_t(w - 1)));
}

TEST(width_gains_are_one_in_the_middle_and_constant_power) {
    float l = 0.0f, r = 0.0f;
    width_gains(0.0f, l, r);
    CHECK(absolute(l - 1.0f) < 1e-5f && absolute(r - 1.0f) < 1e-5f);
    width_gains(-1.0f, l, r);
    CHECK(absolute(l - 1.41421356f) < 1e-5f && absolute(r) < 1e-5f);
    for (int k = -10; k <= 10; k++) {
        width_gains(float(k) * 0.1f, l, r);
        CHECK(absolute(l * l + r * r - 2.0f) < 1e-4f);
    }
}

// --- VA POLY ----------------------------------------------------------------------------------

TEST(width_a_single_note_in_spread_is_the_mono_note_on_both_sides) {
    static VaPoly mono, wide;
    mono.init(kRate);
    wide.init(kRate);
    mono.prepare(va_params());
    wide.prepare(va_params());
    wide.set_width(40);
    CHECK(wide.stereo());
    CHECK_EQ(wide.voices_per_note(), 1);
    mono.trigger(note(60));
    wide.trigger(note(60));
    render(mono, false, 40);
    render(wide, true, 40);
    float most = 0.0f;
    for (int i = 0; i < 40 * kBlk; i++) {
        most = absolute(g_mono[i]) > most ? absolute(g_mono[i]) : most;
        CHECK(absolute(g_left[i] - g_mono[i]) < 1e-5f);
        CHECK(absolute(g_right[i] - g_mono[i]) < 1e-5f);
    }
    CHECK(most > 0.01f);
    CHECK_EQ(wide.voice_pan(0), 0.0f);
}

TEST(width_a_chord_in_spread_fans_its_notes_left_and_right) {
    static VaPoly wide;
    wide.init(kRate);
    wide.prepare(va_params());
    wide.set_width(63);
    wide.trigger(note(60));     // slot 0: the middle
    wide.trigger(note(64));     // slot 1: left
    wide.trigger(note(67));     // slot 2: right
    CHECK_EQ(wide.voice_pan(0), 0.0f);
    CHECK_EQ(wide.voice_pan(1), -1.0f);
    CHECK_EQ(wide.voice_pan(2), 1.0f);
    render(wide, true, 60);
    // The sides differ: the third is only on the right, the second only on the left.
    float diff = 0.0f;
    for (int i = 0; i < 60 * kBlk; i++) diff += absolute(g_left[i] - g_right[i]);
    CHECK(diff > 1.0f);
}

TEST(width_unison_takes_two_voices_tuned_apart_and_panned_hard) {
    static VaPoly wide;
    wide.init(kRate);
    wide.prepare(va_params());
    wide.set_width(127);
    CHECK_EQ(wide.voices_per_note(), 2);
    wide.trigger(note(60));
    CHECK_EQ(wide.busy_voices(), 2);
    CHECK_EQ(wide.voice_note(0), 60);
    CHECK_EQ(wide.voice_note(1), 60);
    CHECK_EQ(wide.voice_pan(0), -1.0f);
    CHECK_EQ(wide.voice_pan(1), 1.0f);
    CHECK(absolute(wide.voice_shift(0) + 0.20f) < 1e-6f);
    CHECK(absolute(wide.voice_shift(1) - 0.20f) < 1e-6f);
    render(wide, true, 4);
    CHECK(absolute(wide.voice_pitch(1) - wide.voice_pitch(0) - 0.40f) < 1e-4f);

    // Three notes fill the six voices; a fourth steals, and never the pair's own slot twice.
    wide.trigger(note(64));
    wide.trigger(note(67));
    CHECK_EQ(wide.busy_voices(), 6);
    wide.trigger(note(71));
    int seventy_one = 0;
    for (int s = 0; s < VaPoly::kVoices; s++) seventy_one += wide.voice_note(s) == 71 ? 1 : 0;
    CHECK_EQ(seventy_one, 2);

    // And a retrigger of a sounding note reuses its own pair rather than taking two more.
    static VaPoly again;
    again.init(kRate);
    again.prepare(va_params());
    again.set_width(100);
    again.trigger(note(60));
    render(again, true, 4);
    again.trigger(note(60));
    CHECK_EQ(again.busy_voices(), 2);
}

TEST(width_unison_on_both_sides_is_wider_than_spread_alone) {
    // A single note: in spread it is the same on both sides, in unison it is not.
    static VaPoly wide;
    wide.init(kRate);
    wide.prepare(va_params());
    wide.set_width(110);
    wide.trigger(note(57));
    render(wide, true, 100);
    float diff = 0.0f;
    for (int i = 0; i < 100 * kBlk; i++) diff += absolute(g_left[i] - g_right[i]);
    CHECK(diff > 1.0f);
    CHECK(energy(g_left, 100 * kBlk) > 0.0f && energy(g_right, 100 * kBlk) > 0.0f);
}

TEST(width_wave_poly_spreads_as_va_poly_does) {
    static VaPoly wave;
    wave.set_wave(true);
    wave.init(kRate);
    wave.prepare(va_params());
    wave.set_width(127);
    wave.trigger(note(48));
    CHECK_EQ(wave.busy_voices(), 2);
    CHECK_EQ(wave.voice_pan(0), -1.0f);
    CHECK_EQ(wave.voice_pan(1), 1.0f);
}

// --- FM4 --------------------------------------------------------------------------------------

TEST(width_fm4_unison_and_spread_use_the_same_rules) {
    static FmPoly fm;
    fm.init(kRate);
    TrackParams p = uniform(64);
    p.set(kAsrFirst + 1, 127);
    fm.prepare(p);
    fm.set_width(127);
    fm.trigger(note(60));
    CHECK_EQ(fm.busy_voices(), 2);
    CHECK_EQ(fm.voice_pan(0), -1.0f);
    CHECK_EQ(fm.voice_pan(1), 1.0f);
    CHECK(absolute(fm.voice_shift(0) + 0.20f) < 1e-6f);
    render(fm, true, 60);
    float diff = 0.0f;
    for (int i = 0; i < 60 * kBlk; i++) diff += absolute(g_left[i] - g_right[i]);
    CHECK(diff > 0.1f);

    // Spread: a lone note in the middle, the next two out to the sides.
    static FmPoly spread;
    spread.init(kRate);
    spread.prepare(p);
    spread.set_width(63);
    spread.trigger(note(60));
    spread.trigger(note(64));
    spread.trigger(note(67));
    CHECK_EQ(spread.busy_voices(), 3);
    CHECK_EQ(spread.voice_pan(0), 0.0f);
    CHECK_EQ(spread.voice_pan(1), -1.0f);
    CHECK_EQ(spread.voice_pan(2), 1.0f);
}

// --- the insert chain's stereo path -----------------------------------------------------------

TEST(width_a_pair_with_the_same_block_on_both_sides_is_the_mono_track) {
    // Drive, a 24 dB filter and the lo-fi on, a send on each bus, panned a little right.
    TrackParams p = uniform(0);
    p.set(4, 60);       // DRV
    p.set(5, 80);       // CUT
    p.set(6, 40);       // RES
    p.set(7, 30);       // MOD: a 24 dB mode
    p.set(12, 30);      // LOF
    p.set(13, 80);      // PAN
    p.set(14, 50);      // SNA
    p.set(15, 70);      // SNB
    ChainMix mix;
    mix.chorus = 0.4f;
    static InsertChain mono, pair;
    mono.init(kRate);
    pair.init(kRate);
    float a_l[kBlk], a_r[kBlk], a_sa[kBlk], a_sb[kBlk], a_sc[kBlk], a_tap[kBlk];
    float b_l[kBlk], b_r[kBlk], b_sa[kBlk], b_sb[kBlk], b_sc[kBlk], b_tap[kBlk];
    float in[kBlk], left[kBlk], right[kBlk];
    for (int b = 0; b < 30; b++) {
        mono.prepare(p, mix);
        pair.prepare(p, mix);
        for (int i = 0; i < kBlk; i++) {
            in[i] = 0.6f * enaga::dsp::sin_turns(float(b * kBlk + i) * 0.013f);
            left[i] = in[i];
            right[i] = in[i];
            a_l[i] = a_r[i] = a_sa[i] = a_sb[i] = a_sc[i] = a_tap[i] = 0.0f;
            b_l[i] = b_r[i] = b_sa[i] = b_sb[i] = b_sc[i] = b_tap[i] = 0.0f;
        }
        ChainOut oa{a_l, a_r, a_sa, a_sb, a_sc, nullptr, a_tap};
        ChainOut ob{b_l, b_r, b_sa, b_sb, b_sc, nullptr, b_tap};
        mono.process(in, kBlk, oa);
        pair.process_pair(left, right, kBlk, ob);
        for (int i = 0; i < kBlk; i++) {
            CHECK(absolute(a_l[i] - b_l[i]) < 1e-5f);
            CHECK(absolute(a_r[i] - b_r[i]) < 1e-5f);
            CHECK(absolute(a_sa[i] - b_sa[i]) < 1e-5f);
            CHECK(absolute(a_sb[i] - b_sb[i]) < 1e-5f);
            CHECK(absolute(a_sc[i] - b_sc[i]) < 1e-5f);
            CHECK(absolute(a_tap[i] - b_tap[i]) < 1e-5f);
        }
    }
}

TEST(width_pan_is_a_balance_on_a_pair) {
    // Hard left on PAN: the right side of the pair does not reach the bus.
    TrackParams p = uniform(0);
    p.set(13, 0);
    ChainMix mix;
    static InsertChain pair;
    pair.init(kRate);
    float out_l[kBlk] = {}, out_r[kBlk] = {};
    float left[kBlk], right[kBlk];
    for (int b = 0; b < 4; b++) {
        pair.prepare(p, mix);
        for (int i = 0; i < kBlk; i++) {
            left[i] = 0.0f;
            right[i] = 0.5f;
            out_l[i] = out_r[i] = 0.0f;
        }
        ChainOut o{out_l, out_r, nullptr, nullptr, nullptr, nullptr, nullptr};
        pair.process_pair(left, right, kBlk, o);
    }
    for (int i = 0; i < kBlk; i++) {
        CHECK_EQ(out_l[i], 0.0f);
        CHECK_EQ(out_r[i], 0.0f);
    }
}
