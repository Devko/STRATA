/* The FM machine's operator loop, as plain C for TI's cl6x (fmloops.ti.c).
 *
 * Four sine operators in one fixed network, the algorithm chosen by which links are open:
 *
 *     o4 = sin(p4 + fb * o4')                        o4' is o4's previous sample
 *     o3 = sin(p3 + m34 * o4)
 *     o2 = sin(p2 + m23 * o3 + m24 * o4)
 *     o1 = sin(p1 + m12 * o2 + m13 * o3 + m14 * o4)
 *     out += env * (c1 o1 + c2 o2 + c3 o3 + c4 o4)
 *
 * Every one of Yamaha's eight 4-operator algorithms is this network with some links at zero, so
 * one branch-free loop plays all eight (dsp/fm.cpp has the table). A link is the modulator's
 * index (in turns of the target's phase) times 0 or 1, and the indices ramp across the slice so
 * an index envelope never steps.
 *
 * Each sine is a read of a 1024-point table with linear interpolation (error under 5e-6, the
 * same as enaga_sin_turns), with the phase kept as an integer: the modulation, in turns, is
 * scaled to 22 fractional bits and added to the operator's own 32-bit phase. That is several
 * times cheaper than the polynomial, which needs a float floor per operator, and the four sines
 * of a sample are a chain -- each waits on the one before -- so their latency is the loop's.
 *
 * <stdint.h> types, for the reason dsp/grainloops.h gives. */
#ifndef STRATA_FMLOOPS_H
#define STRATA_FMLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t phase[4];   /* operators 1-4, 32-bit fractions of a turn */
    float last4;         /* operator 4's previous output, for its feedback */
} strata_fm_state;

/* The sine table: STRATA_FM_SINE_SIZE points over one turn plus a guard point equal to the
 * first, so the interpolation never wraps. dsp/fm.cpp builds it once, at init. */
#define STRATA_FM_SINE_BITS 10
#define STRATA_FM_SINE_SIZE (1 << STRATA_FM_SINE_BITS)

typedef struct {
    const float *sine;   /* STRATA_FM_SINE_SIZE + 1 points */
    uint32_t inc[4];     /* each operator's increment */
    /* The links, as masks: 1 where the algorithm connects modulator j to target i. */
    float m12, m13, m14, m23, m24, m34;
    float c[4];          /* output weights, 0 for a modulator */
    /* The modulators' indices in turns, ramping by their step each sample. */
    float i2, i3, i4, fb;
    float di2, di3, di4, dfb;
} strata_fm_block;

/* out[i] += env[i] * the network's output. env carries the level. n > 0. */
void strata_fm_add(float *out, int n, const float *env, strata_fm_state *s, const strata_fm_block *b);

/* The same with FBK at 0 for the whole slice (b->fb and b->dfb both 0): bit-identical output,
 * but no sample waits on the one before it. With feedback, operator 4's sine sits in a loop-
 * carried chain, and cl6x cannot start a sample before the last one's sine is done; without it
 * the pipeliner overlaps samples and the loop is bound by its units instead (docs/BUDGET.md,
 * "FM4"). dsp/fm.cpp picks this one at control rate. */
void strata_fm_add_free(float *out, int n, const float *env, strata_fm_state *s,
                        const strata_fm_block *b);

#ifdef __cplusplus
}
#endif

#endif
