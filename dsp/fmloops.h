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

typedef struct {
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

#ifdef __cplusplus
}
#endif

#endif
