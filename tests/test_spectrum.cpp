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
