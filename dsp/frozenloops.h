/* A frozen track's read loop (frozenloops.ti.c), plain C for TI's cl6x.
 *
 * A frozen track plays a mono int16 loop that was recorded from it (dsp/frozen.hpp), so its
 * per-sample work is a load, a convert and a ramped multiply-add: the whole point of freezing
 * is that this replaces the machine's synthesis. No branch, no division: the caller splits a
 * block at the loop's end, so each call reads one contiguous run. */
#ifndef STRATA_FROZENLOOPS_H
#define STRATA_FROZENLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dst[i] += src[i] * (gain + step * i) for i in [0, n). `gain` and `step` already carry the
 * 1/32768 that takes int16 to float. */
void strata_frozen_add(float *dst, const int16_t *src, int n, float gain, float step);

#ifdef __cplusplus
}
#endif

#endif
