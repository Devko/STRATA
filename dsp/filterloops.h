/* In-place filter kernels, as plain C for TI's cl6x (filterloops.ti.c).
 *
 * The same filters as Enaga's block functions, step for step: each kernel runs Enaga's own inline
 * step (enaga_svf_step, enaga_one_pole_step), so the arithmetic is Enaga's by construction and
 * the output is bit-identical (tests/test_filterloops.cpp holds every kernel to `==` against the
 * Enaga function it stands in for).
 *
 * What is different is the signature. Enaga's enaga_svf_process(s, c, output, in, out, n) takes
 * `in` and `out` as two unqualified pointers ("out may be in"), so cl6x has to assume that the
 * store to out[i] can change in[i + 1]. Every STRATA call site filters in place, and for those
 * that assumption adds a memory edge to the loop's recurrence: the next load cannot issue until
 * this sample's store has, which is the whole filter's latency plus a load and a store per
 * sample. With ONE restrict pointer read and written at the same index, cl6x can see that sample
 * i + 1 does not depend on the store of sample i, and the loop is bound only by the filter's own
 * state recurrence. docs/BUDGET.md, "The cl6x vectorisation pass", has the cycle reasoning and
 * the measurement owed.
 *
 * The stereo cascade runs two channels' filters in ONE loop. A trapezoidal SVF is recurrence
 * bound -- about four dependent float operations per sample, four cycles each on this core -- so
 * the loop leaves most of the eight functional units idle; a second, independent recurrence fills
 * them for nearly nothing. That is the C674x's version of SIMD: it has no packed float
 * instructions, but it can run independent chains side by side. */
#ifndef STRATA_FILTERLOOPS_H
#define STRATA_FILTERLOOPS_H

#include <enaga/dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* enaga_svf_process(s, c, output, buf, buf, n), in place. */
void strata_svf_inplace(enaga_svf *s, const enaga_svf_coefficients *c, int output, float *buf,
                        int n);

/* enaga_svf_cascade(s1, c1, s2, c2, output, buf, buf, n), in place. */
void strata_svf_cascade_inplace(enaga_svf *s1, const enaga_svf_coefficients *c1, enaga_svf *s2,
                                const enaga_svf_coefficients *c2, int output, float *buf, int n);

/* The cascade above on two channels with the same coefficients, in one loop: the same as
 * strata_svf_cascade_inplace on (l1, l2, left) and then on (r1, r2, right). */
void strata_svf_cascade_stereo_inplace(enaga_svf *l1, enaga_svf *l2, enaga_svf *r1, enaga_svf *r2,
                                       const enaga_svf_coefficients *c1,
                                       const enaga_svf_coefficients *c2, int output, float *left,
                                       float *right, int n);

/* enaga_biquad_process(s, c, buf, buf, n), in place. */
void strata_biquad_inplace(enaga_biquad *s, const enaga_biquad_coefficients *c, float *buf, int n);

/* enaga_one_pole_process(p, a, buf, buf, n), in place. */
void strata_one_pole_inplace(enaga_one_pole *p, float a, float *buf, int n);

#ifdef __cplusplus
}
#endif

#endif
