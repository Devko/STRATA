/* The master looper's two loops (looperloops.ti.c), plain C for TI's cl6x.
 *
 * The looper records the finished master into int16 stereo and later crossfades it against the
 * live master (dsp/looper.hpp). Both loops are a load, a convert and a multiply-add a sample; the
 * caller splits a block at the loop's end, so each call works on one contiguous run. */
#ifndef STRATA_LOOPERLOOPS_H
#define STRATA_LOOPERLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dst[i] = src[i] as int16, clamped to +-32767, for `n` SAMPLES (twice the frames). */
void strata_loop_record(int16_t *dst, const float *src, int n);

/* io is interleaved stereo, `frames` frames; loop the same, int16. Each frame becomes
 * live + m * (loop - live), with m = mix + step * frame. `mix` and `step` are 0-1 units; the
 * 1/32768 is applied inside. */
void strata_loop_mix(float *io, const int16_t *loop, int frames, float mix, float step);

#ifdef __cplusplus
}
#endif

#endif
