/* The scope tap's per-sample loop, as plain C for TI's cl6x (scopeloops.ti.c). core/scope.hpp's
 * ScopeRing calls it once or twice a block (twice when the ring wraps).
 *
 * It runs on every audio block whatever screen is up, so it is a hot loop however small: 32
 * frames a block. Branch-free (the clamps are ?:) and division-free. No Enaga header: core/
 * never includes one. */
#ifndef STRATA_SCOPELOOPS_H
#define STRATA_SCOPELOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every second frame of interleaved stereo `in` (L R L R), `n` frames OUT, so 2n frames in: frame
 * 2i's left to left[i] and its right to right[i], scaled to int16 (full scale 32767) and clamped.
 * No anti-alias filter: this is for a picture, not for listening. */
void strata_scope_decimate(const float *in, int n, int16_t *left, int16_t *right);

#ifdef __cplusplus
}
#endif

#endif
