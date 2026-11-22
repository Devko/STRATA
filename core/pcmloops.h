/* The load-time anti-aliasing low-pass's per-sample loop, as plain C for TI's cl6x
 * (pcmloops.ti.c). core/pcm.cpp's Antialias holds the coefficients and the state.
 *
 * Why a *.ti.c file for something that is not in the audio block: it runs over every sample of
 * every file above 48 kHz, four biquads in series, and it is main-loop time a load is waiting
 * on (docs/BUDGET.md, "Load time"). Four sections in series are a long dependency chain per
 * sample; cl6x software-pipelines a cascade across samples (section 2 of one sample beside
 * section 3 of the one before), which GCC does not, so the loop is worth handing to it. Enaga's
 * own biquad block function measured 1,699 cycles a block with GCC against 628 with cl6x.
 *
 * Plain C with no Enaga header: core/ never includes one, and this needs none. */
#ifndef STRATA_PCMLOOPS_H
#define STRATA_PCMLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STRATA_AA_SECTIONS 4

/* Filter `frames` samples of one channel in place, `stride` int16 apart, through the four
 * transposed direct form II sections whose coefficients are b0..a2 and whose state is z1, z2
 * (STRATA_AA_SECTIONS each, carried between calls), then round to the nearest int16 with the
 * halves away from zero and clamp. The arithmetic is core/pcm.cpp's loop, unchanged: the same
 * expressions in the same order, so the result is bit-identical. */
void strata_antialias(int16_t *samples, uint32_t frames, int stride, const float *b0,
                      const float *b1, const float *b2, const float *a1, const float *a2,
                      float *z1, float *z2);

/* Both channels of interleaved stereo in one pass, the left's state in z1l/z2l and the right's
 * in z1r/z2r. Bit-identical to two calls of strata_antialias, one per channel: the channels
 * share nothing but the coefficients. */
void strata_antialias_stereo(int16_t *samples, uint32_t frames, const float *b0, const float *b1,
                             const float *b2, const float *a1, const float *a2, float *z1l,
                             float *z2l, float *z1r, float *z2r);

#ifdef __cplusplus
}
#endif

#endif
