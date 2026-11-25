// The output's spectrum for the screen (live visuals, part 6): a 256-point FFT of the newest
// output (core/scope.hpp's history, 24 kHz, so 94 Hz a bin), folded into kBands bands from about
// 94 Hz to 12 kHz, linear up to 1.2 kHz and logarithmic above, each band's level in thousandths
// of full scale for paint::level_rows.
//
// Main context only, once a frame while the master page is up: one Hann window, one radix-2 FFT
// in float (1,024 butterflies), no divide in the loops, no libm (the twiddles and the window
// come from paint's quarter-sine table: 256 points is one table step a point). About 0.1-0.2 ms
// on the C674x. Plain functions over arrays the caller owns; no Enaga header, so it is tested on
// the host (tests/test_spectrum.cpp).
#pragma once

#include <cstdint>

#include "scope.hpp"

namespace strata::spectrum {

constexpr int kPoints = 256;
constexpr int kBands = 24;

// The band edges in FFT bins: band b is bins kEdges[b] .. kEdges[b + 1] - 1.
extern const uint8_t kEdges[kBands + 1];

// Scratch the caller keeps (2 KiB), so nothing here is on the stack twice.
struct Work {
    float re[kPoints];
    float im[kPoints];
};

// In place: the complex FFT of re + i im, kPoints long, bit-reversed in and out in order.
void fft(float *re, float *im);

// The newest kPoints frames of `history` (left plus right), windowed, transformed, and each
// band's loudest bin as thousandths of full scale: a full-scale sine reads about 1000.
void analyse(const ScopeHistory &history, Work &work, uint16_t *milli);

}  // namespace strata::spectrum
