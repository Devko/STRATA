// app/spectrum.hpp: the output's spectrum for the screen.
#include "spectrum.hpp"

#include "paint.hpp"

namespace strata::spectrum {

const uint8_t kEdges[kBands + 1] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                    14, 17, 21, 25, 31, 38, 46, 57, 69, 85, 104, 128};

static_assert(kPoints == paint::kTurn, "one sine-table step a point: twiddles need no interpolation");

void fft(float *re, float *im) {
    // Bit reversal.
    for (int i = 1, j = 0; i < kPoints; i++) {
        int bit = kPoints >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const float tr = re[i]; re[i] = re[j]; re[j] = tr;
            const float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    // Butterflies: the twiddle for k of a span `len` is e^(-2 pi i k / len), table step
    // k * (kPoints / len).
    constexpr float kQ12 = 1.0f / 4096.0f;
    for (int len = 2; len <= kPoints; len <<= 1) {
        const int half = len >> 1;
        const int step = kPoints / len;
        for (int k = 0; k < half; k++) {
            const float wr = float(paint::cos_q12(k * step)) * kQ12;
            const float wi = -float(paint::sin_q12(k * step)) * kQ12;
            for (int i = k; i < kPoints; i += len) {
                const int j = i + half;
                const float xr = re[j] * wr - im[j] * wi;
                const float xi = re[j] * wi + im[j] * wr;
                re[j] = re[i] - xr;
                im[j] = im[i] - xi;
                re[i] += xr;
                im[i] += xi;
            }
        }
    }
}

void analyse(const ScopeHistory &history, Work &work, uint16_t *milli) {
    // Oldest first, so the window is centred on the middle of the span. Left plus right as a
    // sum (a mono mix at +6 dB, taken back out in the scale below), Hann window from the table.
    constexpr float kIn = 1.0f / 65536.0f;              // L + R to -1..1
    for (int n = 0; n < kPoints; n++) {
        const uint32_t ago = uint32_t(kPoints - 1 - n);
        const float x = float(history.left(ago) + history.right(ago)) * kIn;
        const float hann = 0.5f - 0.5f * float(paint::cos_q12(n)) * (1.0f / 4096.0f);
        work.re[n] = x * hann;
        work.im[n] = 0.0f;
    }
    fft(work.re, work.im);
    // A full-scale sine through a Hann window peaks at kPoints / 4 in its bin. Squared
    // magnitudes are compared; the one square root a band is the loop below, with no libm:
    // a few Newton steps from a power-of-two guess.
    constexpr float kFull = float(kPoints) / 4.0f;
    for (int b = 0; b < kBands; b++) {
        float best = 0.0f;
        for (int k = kEdges[b]; k < kEdges[b + 1]; k++) {
            const float m = work.re[k] * work.re[k] + work.im[k] * work.im[k];
            best = m > best ? m : best;
        }
        // sqrt(best) by Newton's method from 1, enough steps for a screen's 60 dB.
        float r = best > 1.0f ? best : 1.0f;
        if (best > 0.0f) {
            for (int i = 0; i < 24; i++) r = 0.5f * (r + best / r);
        } else {
            r = 0.0f;
        }
        const float level = r * (1000.0f / kFull);
        milli[b] = uint16_t(level > 65535.0f ? 65535.0f : level);
    }
}

}  // namespace strata::spectrum
