// app/spectrum: the output's spectrum for the master page.
#include "spectrum.hpp"

#include "check.hpp"

#include "paint.hpp"

using namespace strata;
using namespace strata::spectrum;

namespace {

ScopeRing g_ring;
ScopeHistory g_history;
Work g_work;

// `frames` of a sine at `cycles` per 256 frames AFTER decimation (the ring keeps every second
// frame), amplitude `amp` of full scale, both channels, pushed and drained.
void feed_sine(int cycles, float amp) {
    float block[128];
    for (int b = 0; b < 16; b++) {
        for (int f = 0; f < 64; f++) {
            const int n = b * 64 + f;                     // the 48 kHz frame
            // sin at cycles / 512 a 48 kHz frame, from the table (256 a turn): n * cycles / 2.
            const int angle = (n * cycles / 2) & (paint::kTurn - 1);
            const float v = amp * float(paint::sin_q12(angle)) / 4096.0f;
            block[2 * f] = v;
            block[2 * f + 1] = v;
        }
        g_ring.push(block, 64);
    }
    g_history.drain(g_ring);
}

// Fills the whole history (1,024 frames after decimation) from `level(ago)`, a function of how
// far back the frame is from the newest (0 the newest), in -1..1 of full scale.
template <typename F>
void feed_history(F level) {
    constexpr int kKept = int(ScopeHistory::kHistory);
    float block[128];
    for (int b = 0; b < kKept * 2 / 64; b++) {
        for (int f = 0; f < 64; f++) {
            const int n = b * 64 + f;                     // the 48 kHz frame
            const int ago = kKept - 1 - n / 2;            // its decimated frame, from the newest
            const float v = level(ago);
            block[2 * f] = v;
            block[2 * f + 1] = v;
        }
        g_ring.push(block, 64);
        g_history.drain(g_ring);
    }
}

// What analyse() must give, worked out independently in double: the newest kPoints frames of
// left plus right, oldest first, a Hann window from paint's table, a plain DFT, each band's
// loudest bin, full-scale sine = 1000.
void reference(const ScopeHistory &history, double *milli) {
    double x[kPoints];
    for (int n = 0; n < kPoints; n++) {
        const unsigned ago = unsigned(kPoints - 1 - n);
        const double hann = 0.5 - 0.5 * paint::cos_q12(n) / 4096.0;
        x[n] = (history.left(ago) + history.right(ago)) / 65536.0 * hann;
    }
    for (int b = 0; b < kBands; b++) {
        double best = 0.0;
        for (int k = kEdges[b]; k < kEdges[b + 1]; k++) {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < kPoints; n++) {
                const int a = (k * n) & (paint::kTurn - 1);
                re += x[n] * paint::cos_q12(a) / 4096.0;
                im -= x[n] * paint::sin_q12(a) / 4096.0;
            }
            const double m = re * re + im * im;
            best = m > best ? m : best;
        }
        double r = best;                                  // sqrt by Newton, as no libm here
        if (r > 0.0) {
            for (int i = 0; i < 60; i++) r = 0.5 * (r + best / r);
        }
        milli[b] = r * 1000.0 / (kPoints / 4.0);
    }
}

int band_of_bin(int bin) {
    for (int b = 0; b < kBands; b++) {
        if (bin >= kEdges[b] && bin < kEdges[b + 1]) return b;
    }
    return -1;
}

}  // namespace

TEST(spectrum_the_fft_of_an_impulse_is_flat_and_of_a_cosine_two_lines) {
    float re[kPoints] = {}, im[kPoints] = {};
    re[0] = 1.0f;
    fft(re, im);
    for (int k = 0; k < kPoints; k++) {
        CHECK_NEAR(re[k], 1.0, 1e-4);
        CHECK_NEAR(im[k], 0.0, 1e-4);
    }
    for (int n = 0; n < kPoints; n++) {
        re[n] = float(paint::cos_q12(n * 8)) / 4096.0f;   // 8 cycles
        im[n] = 0.0f;
    }
    fft(re, im);
    CHECK_NEAR(re[8], kPoints / 2.0, 0.5);
    CHECK_NEAR(re[kPoints - 8], kPoints / 2.0, 0.5);
    for (int k = 0; k < kPoints; k++) {
        if (k == 8 || k == kPoints - 8) continue;
        CHECK(re[k] * re[k] + im[k] * im[k] < 0.01f);
    }
}

TEST(spectrum_the_bands_cover_the_bins_in_order) {
    CHECK_EQ(int(kEdges[0]), 1);
    CHECK_EQ(int(kEdges[kBands]), kPoints / 2);
    for (int b = 0; b < kBands; b++) CHECK(kEdges[b] < kEdges[b + 1]);
}

TEST(spectrum_a_sine_lights_its_own_band_loudest_and_silence_none) {
    uint16_t milli[kBands];
    feed_sine(20, 0.5f);                                   // bin 20 after decimation
    analyse(g_history, g_work, milli);
    const int band = band_of_bin(20);
    CHECK(band >= 0);
    int loudest = 0;
    for (int b = 0; b < kBands; b++) loudest = milli[b] > milli[loudest] ? b : loudest;
    CHECK_EQ(loudest, band);
    // Half scale reads about half of full: 500, within a window's scalloping.
    CHECK(milli[band] > 350 && milli[band] < 650);
    // Far bands are far down (the Hann window's sidelobes).
    CHECK(milli[0] < 30);
    CHECK(milli[kBands - 1] < 30);

    feed_sine(0, 0.0f);
    analyse(g_history, g_work, milli);
    for (int b = 0; b < kBands; b++) CHECK_EQ(int(milli[b]), 0);
}

// A step at the window's peak: the newest half of the window loud, everything older silent. The
// step puts energy in every band, and its edge sits where the window weighs a frame most, so a
// window one frame early or late, read backwards, or flipped moves the bands by far more than the
// rounding. Every band against the reference, not just the loudest.
TEST(spectrum_every_band_matches_a_plain_dft_of_the_newest_frames) {
    feed_history([](int ago) { return ago < kPoints / 2 ? 0.9f : 0.0f; });
    uint16_t milli[kBands];
    analyse(g_history, g_work, milli);
    double want[kBands];
    reference(g_history, want);
    CHECK(want[0] > 500.0);
    for (int b = 0; b < kBands; b++) CHECK_NEAR(double(milli[b]), want[b], 1.5);

    // A sine in the first bin of each band lights that band, not the one below it.
    for (int b = 1; b < kBands; b++) {
        const int bin = kEdges[b];
        feed_history([bin](int ago) {
            return 0.7f * float(paint::sin_q12((bin * ago) & (paint::kTurn - 1))) / 4096.0f;
        });
        analyse(g_history, g_work, milli);
        reference(g_history, want);
        for (int c = 0; c < kBands; c++) CHECK_NEAR(double(milli[c]), want[c], 1.5);
    }
}

// The caller keeps one Work across frames: what the last analysis left in it must not leak into
// the next one (frame 0 is written even though its window weight is 0).
TEST(spectrum_a_reused_work_gives_what_a_fresh_one_does) {
    uint16_t milli[kBands], fresh[kBands];
    feed_history([](int) { return 0.8f; });              // a loud DC term left in re[0]
    analyse(g_history, g_work, milli);
    feed_history([](int ago) { return ago < 40 ? -0.6f : 0.2f; });
    analyse(g_history, g_work, milli);
    static Work clean;
    for (int n = 0; n < kPoints; n++) clean.re[n] = clean.im[n] = 0.0f;
    analyse(g_history, clean, fresh);
    for (int b = 0; b < kBands; b++) CHECK_EQ(int(milli[b]), int(fresh[b]));
}
