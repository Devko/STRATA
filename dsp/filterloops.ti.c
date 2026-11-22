/* In-place filter kernels (filterloops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler.
 *
 * Each loop is Enaga's, with Enaga's inline step: the switch on the output sits outside the loop,
 * as in runtime/dsp/filter.c, so every loop is specialised and branch-free. Only the pointers
 * differ -- one restrict buffer instead of `in` and `out` that may alias (filterloops.h says why
 * that matters to the pipeliner). */
#include "filterloops.h"

#include "tihints.h"

void strata_svf_inplace(enaga_svf *s, const enaga_svf_coefficients *c, int output,
                        float *restrict buf, int n) {
    if (n <= 0) return;
    const enaga_svf_coefficients k = *c;
    enaga_svf t = *s;
    float low, band, high;
    int i;
    switch (output) {
    case ENAGA_SVF_LOW:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, buf[i], &low, &band, &high); buf[i] = low; }
        break;
    case ENAGA_SVF_BAND:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, buf[i], &low, &band, &high); buf[i] = band; }
        break;
    default:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, buf[i], &low, &band, &high); buf[i] = high; }
        break;
    }
    *s = t;
}

/* One sample of a two-stage cascade giving output OUT (0 low, 1 band, 2 high): Enaga's
 * enaga_svf_cascade body. */
#define STRATA_CASCADE_STEP(T1, K1, T2, K2, OUT, X, Y)                                    \
    do {                                                                                  \
        float o_[3], p_[3];                                                               \
        enaga_svf_step(&(T1), &(K1), (X), &o_[0], &o_[1], &o_[2]);                        \
        enaga_svf_step(&(T2), &(K2), o_[OUT], &p_[0], &p_[1], &p_[2]);                    \
        (Y) = p_[OUT];                                                                    \
    } while (0)

void strata_svf_cascade_inplace(enaga_svf *s1, const enaga_svf_coefficients *c1, enaga_svf *s2,
                                const enaga_svf_coefficients *c2, int output,
                                float *restrict buf, int n) {
    if (n <= 0) return;
    const enaga_svf_coefficients k1 = *c1, k2 = *c2;
    enaga_svf t1 = *s1, t2 = *s2;
    int i;
    switch (output) {
    case ENAGA_SVF_LOW:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) STRATA_CASCADE_STEP(t1, k1, t2, k2, 0, buf[i], buf[i]);
        break;
    case ENAGA_SVF_BAND:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) STRATA_CASCADE_STEP(t1, k1, t2, k2, 1, buf[i], buf[i]);
        break;
    default:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) STRATA_CASCADE_STEP(t1, k1, t2, k2, 2, buf[i], buf[i]);
        break;
    }
    *s1 = t1;
    *s2 = t2;
}

void strata_svf_cascade_stereo_inplace(enaga_svf *l1, enaga_svf *l2, enaga_svf *r1, enaga_svf *r2,
                                       const enaga_svf_coefficients *c1,
                                       const enaga_svf_coefficients *c2, int output,
                                       float *restrict left, float *restrict right, int n) {
    if (n <= 0) return;
    const enaga_svf_coefficients k1 = *c1, k2 = *c2;
    enaga_svf a1 = *l1, a2 = *l2, b1 = *r1, b2 = *r2;
    int i;
    switch (output) {
    case ENAGA_SVF_LOW:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) {
            STRATA_CASCADE_STEP(a1, k1, a2, k2, 0, left[i], left[i]);
            STRATA_CASCADE_STEP(b1, k1, b2, k2, 0, right[i], right[i]);
        }
        break;
    case ENAGA_SVF_BAND:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) {
            STRATA_CASCADE_STEP(a1, k1, a2, k2, 1, left[i], left[i]);
            STRATA_CASCADE_STEP(b1, k1, b2, k2, 1, right[i], right[i]);
        }
        break;
    default:
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) {
            STRATA_CASCADE_STEP(a1, k1, a2, k2, 2, left[i], left[i]);
            STRATA_CASCADE_STEP(b1, k1, b2, k2, 2, right[i], right[i]);
        }
        break;
    }
    *l1 = a1;
    *l2 = a2;
    *r1 = b1;
    *r2 = b2;
}

void strata_one_pole_inplace(enaga_one_pole *p, float a, float *restrict buf, int n) {
    if (n <= 0) return;
    enaga_one_pole t = *p;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) buf[i] = enaga_one_pole_step(&t, buf[i], a);
    *p = t;
}

void strata_biquad_inplace(enaga_biquad *s, const enaga_biquad_coefficients *c,
                           float *restrict buf, int n) {
    if (n <= 0) return;
    const enaga_biquad_coefficients k = *c;
    enaga_biquad t = *s;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) buf[i] = enaga_biquad_step(&t, &k, buf[i]);
    *s = t;
}
