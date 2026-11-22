/* SAMPLE's interior read loop, as plain C for TI's cl6x (sampleloops.ti.c).
 *
 * The one per-sample loop in the sampler that ran on every voice of every SAMPLE track was C++
 * in dsp/sample.cpp, so the cl6x build compiled it with GCC: the same position M1 was in before
 * the drum loops moved (docs/BUDGET.md, M2, where cl6x bought 1.11x until they did and 1.80x
 * after). This is that move for the sampler -- the same expressions in the same order, so the
 * read is bit-identical (tests/test_sample.cpp pins it against the loop it replaced, and the
 * golden renders pin every SAMPLE sound).
 *
 * The interior is the part of a run where all four Hermite taps are inside the buffer, so the
 * loop needs no clamp: sample.cpp works out how many samples fit (k) and keeps the guarded path
 * for the handful at the edges. */
#ifndef STRATA_SAMPLELOOPS_H
#define STRATA_SAMPLELOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes k >= 1 samples to dst: a 4-point Catmull-Rom read around p at fraction frac / 2^24,
 * then the position advances by istep + fstep / 2^24 frames, k - 1 times (the last sample is read
 * without advancing, so p is never walked past the final valid tap). `stride` is +-channels, a
 * frame in the playing direction; `istride` is istep * stride. *frac and *pos come back advanced
 * by k - 1 steps, exactly as sample.cpp's loop left them. A stereo tap is L + R as an integer:
 * the halving rides on the caller's gain. */
void strata_sample_read_mono(float *dst, int k, const int16_t *p, int stride, int istride,
                             uint32_t *frac, uint32_t fstep, int32_t *pos, int32_t istep);
void strata_sample_read_stereo(float *dst, int k, const int16_t *p, int stride, int istride,
                               uint32_t *frac, uint32_t fstep, int32_t *pos, int32_t istep);

#ifdef __cplusplus
}
#endif

#endif
