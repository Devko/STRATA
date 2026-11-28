// The WAVE machines' wavetable library (dsp/wavetable.cpp) and oscillator loop
// (dsp/waveloops.ti.c).
#include "check.hpp"

#include "wavetable.hpp"
#include "waveloops.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>

using namespace strata;

namespace {

const WaveLibrary &library() {
    static WaveLibrary lib;
    static bool built = false;
    if (!built) {
        const auto t0 = std::chrono::steady_clock::now();
        built = lib.init();
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("    wavetables: %u bytes, built in %.1f ms on the host\n", unsigned(kWaveLibraryBytes),
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return lib;
}

double value(const WaveLibrary &lib, int t, int f, int mip, int i) {
    return double(lib.frame(t, f, mip)[i]) * double(lib.scale(t, f));
}

// Amplitude of harmonic h in level `mip` of a frame, by a direct DFT.
double harmonic(const WaveLibrary &lib, int t, int f, int mip, int h) {
    const int n = wave_mip_length(mip);
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) {
        const double w = 2.0 * M_PI * double(h) * double(i) / double(n);
        const double v = value(lib, t, f, mip, i);
        re += v * std::cos(w);
        im += v * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(n);
}

int table_named(const char *name) {
    for (int t = 0; t < kWaveTables; t++) {
        const char *s = WaveLibrary::short_name(t);
        if (s[0] == name[0] && s[1] == name[1] && s[2] == name[2]) return t;
    }
    return -1;
}

}  // namespace

TEST(wave_library_builds_every_table_with_a_name) {
    const WaveLibrary &lib = library();
    CHECK(lib.ready());
    for (int t = 0; t < kWaveTables; t++) {
        const char *s = WaveLibrary::short_name(t);
        CHECK(s[0] != 0 && s[1] != 0 && s[2] != 0 && s[3] == 0);
        CHECK(WaveLibrary::long_name(t)[0] != 0);
        for (int u = 0; u < t; u++) CHECK(table_named(s) != u);   // short names are unique
    }
    CHECK(std::string_view(WaveLibrary::short_name(kWaveTables)) == "---");
}

TEST(wave_every_frame_peaks_at_one_or_under_and_none_is_silent) {
    const WaveLibrary &lib = library();
    for (int t = 0; t < kWaveTables; t++) {
        double table_peak = 0.0;
        for (int f = 0; f < kWaveFrames; f++) {
            double pk = 0.0;
            for (int i = 0; i < kWaveTopLength; i++) pk = std::fmax(pk, std::fabs(value(lib, t, f, 0, i)));
            CHECK(pk <= 1.0001);
            CHECK(pk > 0.02);
            table_peak = std::fmax(table_peak, pk);
        }
        CHECK(table_peak > 0.999);   // normalised: per frame, or once for the table (PLK)
    }
}

TEST(wave_every_level_has_its_guard_sample) {
    const WaveLibrary &lib = library();
    for (int t = 0; t < kWaveTables; t++)
        for (int f = 0; f < kWaveFrames; f += 5)
            for (int k = 0; k < kWaveMipLevels; k++) {
                const int16_t *s = lib.frame(t, f, k);
                CHECK_EQ(s[wave_mip_length(k)], s[0]);
            }
}

TEST(wave_classic_starts_on_a_sine_and_ends_on_a_square) {
    const WaveLibrary &lib = library();
    const int cls = table_named("CLS");
    CHECK_EQ(cls, 0);
    double err = 0.0;
    for (int i = 0; i < kWaveTopLength; i++)
        err = std::fmax(err, std::fabs(value(lib, cls, 0, 0, i) - std::sin(2.0 * M_PI * i / kWaveTopLength)));
    CHECK(err < 2e-4);
    const int last = kWaveFrames - 1;
    const double h1 = harmonic(lib, cls, last, 0, 1);
    CHECK(std::fabs(harmonic(lib, cls, last, 0, 3) / h1 - 1.0 / 3.0) < 0.002);
    CHECK(std::fabs(harmonic(lib, cls, last, 0, 5) / h1 - 1.0 / 5.0) < 0.002);
    CHECK(harmonic(lib, cls, last, 0, 2) / h1 < 1e-3);   // no even harmonics
}

TEST(wave_each_level_stops_at_its_top_harmonic) {
    const WaveLibrary &lib = library();
    const int cls = table_named("CLS");
    const int saw = 10;   // about two thirds along: between saw and square, every harmonic
    for (int k = 3; k < kWaveMipLevels - 1; k++) {
        const int top = kWaveHarmonics >> k;
        const double kept = harmonic(lib, cls, saw, k, top);
        const double above = harmonic(lib, cls, saw, k, top + 1);
        CHECK(kept > 0.002);
        CHECK(above < 2e-4);
    }
}

TEST(wave_time_domain_recipes_are_clean_below_the_top) {
    // SYN frame 0 is a plain saw: harmonic h at 2 / (pi h), and nothing from the sampling.
    const WaveLibrary &lib = library();
    const int syn = table_named("SYN");
    const double h1 = harmonic(lib, syn, 0, 0, 1);
    for (int h = 2; h < 200; h += 37) CHECK(std::fabs(harmonic(lib, syn, 0, 0, h) * h / h1 - 1.0) < 0.01);
}

TEST(wave_pluck_keeps_one_gain_so_its_dull_end_is_quieter) {
    const WaveLibrary &lib = library();
    const int plk = table_named("PLK");
    auto peak = [&](int f) {
        double pk = 0.0;
        for (int i = 0; i < kWaveTopLength; i++) pk = std::fmax(pk, std::fabs(value(lib, plk, f, 0, i)));
        return pk;
    };
    CHECK(peak(kWaveFrames - 1) < 0.8 * peak(0));
}

TEST(wave_mip_for_keeps_aliases_above_18_khz) {
    CHECK_EQ(wave_mip_for(1e-5f), 0);
    for (float hz = 20.0f; hz < 20000.0f; hz *= 1.1f) {
        const float inc = hz / 48000.0f;
        const int k = wave_mip_for(inc);
        CHECK(float(kWaveHarmonics >> k) * inc <= kWaveAliasLimit || k == kWaveMipLevels - 1);
        if (k > 0) CHECK(float(kWaveHarmonics >> (k - 1)) * inc > kWaveAliasLimit);
    }
}

TEST(wave_loop_plays_the_frame_at_the_right_pitch_and_level) {
    const WaveLibrary &lib = library();
    const int k = wave_mip_for(480.0f / 48000.0f);
    const int bits = kWaveMipBits[k];
    const int16_t *a = lib.frame(0, 0, k);   // CLS frame 0: a sine
    float out[480] = {};
    uint32_t phase = 0;
    const uint32_t inc = uint32_t(480.0 / 48000.0 * 4294967296.0);
    const float s = lib.scale(0, 0);
    strata_wave_add(out, 240, a, a, bits, &phase, inc, 0.5f * s, 0.0f, 0.0f, 0.0f);
    strata_wave_add(out + 240, 240, a, a, bits, &phase, inc, 0.25f * s, 0.25f * s, 0.0f, 0.0f);
    double err = 0.0;
    for (int i = 0; i < 480; i++) err = std::fmax(err, std::fabs(out[i] - 0.5 * std::sin(2.0 * M_PI * i / 100.0)));
    CHECK(err < 1e-3);
    CHECK_EQ(phase, inc * 480u);
}

TEST(wave_gains_ramp_across_the_block) {
    const WaveLibrary &lib = library();
    const int16_t *a = lib.frame(0, kWaveFrames - 1, 9);   // one harmonic, 256 samples
    float out[64] = {};
    uint32_t phase = 0x40000000u;                           // a quarter turn: the square's top
    strata_wave_add(out, 64, a, a, kWaveMipBits[9], &phase, 0, 0.0f, 0.0f, 1.0f / 64.0f, 0.0f);
    CHECK(out[0] == 0.0f);
    CHECK(out[63] > out[32] && out[32] > out[1]);
}

TEST(wave_unbent_warp_matches_the_plain_loop) {
    const WaveLibrary &lib = library();
    const int16_t *a = lib.frame(table_named("FMT"), 7, 2);
    const int16_t *b = lib.frame(table_named("FMT"), 8, 2);
    float plain[256] = {}, bent[256] = {};
    uint32_t p1 = 12345, p2 = 12345;
    const strata_wave_warp none = {0.5f, 1.0f, 1.0f};
    strata_wave_add(plain, 256, a, b, kWaveMipBits[2], &p1, 9876543u, 1e-4f, 2e-4f, 1e-7f, -1e-7f);
    strata_wave_add_warp(bent, 256, a, b, kWaveMipBits[2], &p2, 9876543u, 1e-4f, 2e-4f, 1e-7f, -1e-7f, &none);
    double err = 0.0;
    for (int i = 0; i < 256; i++) err = std::fmax(err, std::fabs(plain[i] - bent[i]));
    CHECK(err < 5e-3);   // the bent loop's phase is a float: 24 bits, not 32
    CHECK_EQ(p1, p2);
}

TEST(wave_hard_warp_never_reads_past_the_guard) {
    const WaveLibrary &lib = library();
    const int16_t *a = lib.frame(0, 3, 9);
    const strata_wave_warp hard = {0.02f, 25.0f, 0.5f / 0.98f};
    float out[64] = {};
    uint32_t phase = 0xFFFFFF00u;   // the last instants of the cycle, where q rounds towards 1
    strata_wave_add_warp(out, 64, a, a, kWaveMipBits[9], &phase, 3u, 1.0f, 0.0f, 0.0f, 0.0f, &hard);
    for (float v : out) CHECK(std::isfinite(v) && std::fabs(v) <= 32768.0f);
}

namespace {
bool same_table(const WaveLibrary &x, const WaveLibrary &y, int t) {
    bool same = true;
    for (int f = 0; f < kWaveFrames; f++) {
        same = same && x.scale(t, f) == y.scale(t, f);
        const int16_t *a = x.frame(t, f, 0);
        const int16_t *b = y.frame(t, f, 0);
        for (int i = 0; i < kWaveFrameStride; i++) same = same && a[i] == b[i];
    }
    return same;
}

// Steps until the library has nothing to do; the number of pieces.
int settle(WaveLibrary &lib) {
    int pieces = 0;
    while (lib.step() && pieces < 100000) pieces++;
    return pieces;
}

}  // namespace

// The app builds a table only when a WAVE track sounds it (Engine::step_waves), a small piece at
// a time: all sixteen at boot outlasted Enaga's 1 s watchdog on the C6748, and pieces any bigger
// left the keys unpolled long enough to lose presses. A table built alone, in any slot, in any
// order, is the same bit for bit as the whole library's.
TEST(wavetable_a_table_built_on_demand_is_the_same_in_any_slot_and_order) {
    const WaveLibrary &whole = library();
    static WaveLibrary lib;
    CHECK(lib.reserve(3));
    CHECK_EQ(lib.slots(), 3);
    CHECK(!lib.step());                 // nothing wanted: nothing to do
    for (int t = 0; t < kWaveTables; t++) CHECK(!lib.resident(t));
    // Tables in a scrambled order, one at a time, so slots are reused (the audio side never
    // ticks here, so a taken-out slot needs two audio_block()s before it is written).
    const int order[kWaveTables] = {11, 2, 15, 0, 7, 12, 4, 9, 14, 1, 6, 13, 3, 8, 10, 5};
    for (int t : order) {
        lib.want(1u << t);
        int pieces = settle(lib);
        if (!lib.resident(t)) {   // waiting for a slot to settle
            lib.audio_block();
            lib.audio_block();
            pieces += settle(lib);
        }
        CHECK(lib.resident(t));
        CHECK(pieces >= 1 + WaveLibrary::kStoreSteps && pieces <= WaveLibrary::kMostSteps + 1);
        CHECK(same_table(whole, lib, t));
    }
    CHECK_EQ(lib.builds(), kWaveTables);
}

TEST(wavetable_slots_keep_wanted_tables_and_reuse_the_least_recently_wanted) {
    static WaveLibrary lib;
    CHECK(lib.reserve(2));
    lib.want((1u << 3) | (1u << 5));
    settle(lib);
    CHECK(lib.resident(3) && lib.resident(5));
    // A third table while both are wanted: it waits, and the two stay.
    lib.want((1u << 3) | (1u << 5) | (1u << 9));
    CHECK(!lib.step());
    CHECK(!lib.resident(9) && lib.resident(3) && lib.resident(5));
    // 5 no longer wanted: its slot is taken out at once, and written only after two blocks.
    lib.want((1u << 3) | (1u << 9));
    CHECK(!lib.step());
    CHECK(!lib.resident(5));
    CHECK(!lib.step());               // still settling
    lib.audio_block();
    CHECK(!lib.step());
    lib.audio_block();
    settle(lib);
    CHECK(lib.resident(9) && lib.resident(3) && !lib.resident(5));
    CHECK_EQ(lib.slot_of(9), lib.slot_of(9));
    CHECK(lib.slot_of(9) != lib.slot_of(3));
    // A table not built reads as silence by table, and -1 by slot.
    CHECK_EQ(lib.slot_of(5), -1);
    CHECK(lib.scale(5, 0) == 0.0f);
    CHECK_EQ(int(lib.frame(5, 0, 0)[0]), 0);
    CHECK_EQ(lib.slot_of(-1), -1);
    CHECK_EQ(lib.slot_of(kWaveTables), -1);
    // A library with no slots has nothing to step.
    WaveLibrary none;
    CHECK(!none.step());
    CHECK(!none.ready());
}

// The slot choices exactly: a free slot is the lowest one free; with none free, the slot whose
// table was wanted least recently (and is not wanted now) is taken out, waits two audio blocks
// counted from when it was taken out, and is then the one written.
TEST(wavetable_slot_choice_follows_when_each_table_was_last_wanted) {
    static WaveLibrary lib;
    CHECK(lib.reserve(3));
    for (int i = 0; i < 5; i++) lib.audio_block();   // the block count is not zero
    lib.want(0x7u);
    settle(lib);
    CHECK_EQ(lib.slot_of(0), 0);
    CHECK_EQ(lib.slot_of(1), 1);
    CHECK_EQ(lib.slot_of(2), 2);
    CHECK_EQ(lib.last_built(), 2);
    // 1 goes unwanted first, then 0: 1 was wanted less recently, so its slot goes.
    lib.want((1u << 0) | (1u << 2));
    for (int i = 0; i < 3; i++) CHECK(!lib.step());
    lib.want(1u << 2);
    for (int i = 0; i < 3; i++) CHECK(!lib.step());
    lib.want((1u << 2) | (1u << 5));
    CHECK(!lib.step());
    CHECK(!lib.resident(1) && lib.resident(0) && lib.resident(2));
    CHECK(!lib.step());
    lib.audio_block();
    CHECK(!lib.step());                // one block is not enough
    CHECK(!lib.resident(5));
    lib.audio_block();
    settle(lib);
    CHECK_EQ(lib.slot_of(5), 1);
    CHECK_EQ(lib.last_built(), 5);
    CHECK_EQ(lib.builds(), 4);
    // Now 0 has gone longest unwanted, before 2: slot 0 goes, and 5 (wanted) is never touched.
    lib.want(1u << 5);
    for (int i = 0; i < 3; i++) CHECK(!lib.step());
    lib.want((1u << 5) | (1u << 7));
    CHECK(!lib.step());
    CHECK(!lib.resident(0) && lib.resident(2) && lib.resident(5));
    lib.audio_block();
    lib.audio_block();
    settle(lib);
    CHECK_EQ(lib.slot_of(7), 0);
    CHECK_EQ(lib.slot_of(5), 1);
    CHECK_EQ(lib.slot_of(2), 2);
    CHECK_EQ(lib.last_built(), 7);
}

TEST(wavetable_reserve_clamps_its_slots_and_happens_once) {
    static WaveLibrary one, all;
    CHECK(one.reserve(0));
    CHECK_EQ(one.slots(), 1);
    CHECK(one.reserve(3));             // already reserved: kept as it is
    CHECK_EQ(one.slots(), 1);
    one.want(1u << 4);
    settle(one);
    CHECK(one.resident(4));
    CHECK_EQ(one.slot_of(4), 0);
    CHECK(all.reserve(kWaveTables + 9));
    CHECK_EQ(all.slots(), kWaveTables);
}

// Stale bytes from the audio side are clamped into the slots, and a table not built reads the
// silent frame at the asked level.
TEST(wavetable_reads_clamp_every_argument) {
    const WaveLibrary &lib = library();
    const int last = lib.slots() - 1;
    CHECK(lib.frame_at(-1, 3, 2) == lib.frame_at(0, 3, 2));
    CHECK(lib.frame_at(last + 1, 3, 2) == lib.frame_at(last, 3, 2));
    CHECK(lib.frame_at(1, -1, 2) == lib.frame_at(1, 0, 2));
    CHECK(lib.frame_at(1, kWaveFrames, 2) == lib.frame_at(1, kWaveFrames - 1, 2));
    CHECK(lib.frame_at(1, 3, -1) == lib.frame_at(1, 3, 0));
    CHECK(lib.frame_at(1, 3, kWaveMipLevels) == lib.frame_at(1, 3, kWaveMipLevels - 1));
    CHECK(lib.scale_at(-1, 3) == lib.scale_at(0, 3));
    CHECK(lib.scale_at(WaveLibrary::kMaxSlots, 3) == lib.scale_at(WaveLibrary::kMaxSlots - 1, 3));
    CHECK(lib.scale_at(1, -1) == lib.scale_at(1, 0));
    CHECK(lib.scale_at(1, kWaveFrames) == lib.scale_at(1, kWaveFrames - 1));
    static WaveLibrary empty;
    CHECK(empty.reserve(1));
    const int16_t *silent = empty.frame(6, 0, 0);
    for (int k = 0; k < kWaveMipLevels; k++) CHECK(empty.frame(6, 2, k) == silent + kWaveMipOffsets.at[k]);
    CHECK(empty.frame(6, 2, -1) == silent);
    CHECK(empty.frame(6, 2, kWaveMipLevels) == silent + kWaveMipOffsets.at[kWaveMipLevels - 1]);
    for (int i = 0; i < kWaveFrameStride; i++) CHECK_EQ(int(silent[i]), 0);
    CHECK(std::string_view(WaveLibrary::long_name(0)) != "---");
    CHECK(std::string_view(WaveLibrary::short_name(0)) != "---");
    CHECK(std::string_view(WaveLibrary::long_name(-1)) == "---");
    CHECK(std::string_view(WaveLibrary::long_name(kWaveTables)) == "---");
    CHECK(std::string_view(WaveLibrary::short_name(-1)) == "---");
}

TEST(wave_the_mip_offset_table_is_the_layout) {
    // frame_at() reads kWaveMipOffsets (worked out at compile time) instead of calling the
    // recursion at run time: the two must agree at every level, and the levels must not overlap.
    for (int k = 0; k < kWaveMipLevels; k++) {
        CHECK_EQ(kWaveMipOffsets.at[k], wave_mip_offset(k));
        if (k > 0) CHECK_EQ(kWaveMipOffsets.at[k], kWaveMipOffsets.at[k - 1] + wave_mip_length(k - 1) + 1);
    }
    const WaveLibrary &lib = library();
    for (int k = 0; k < kWaveMipLevels; k++) {
        CHECK(lib.frame(0, 3, k) == lib.frame(0, 3, 0) + kWaveMipOffsets.at[k]);
    }
}
