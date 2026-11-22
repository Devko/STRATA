/* SAMPLE's interior read loop (sampleloops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler.
 *
 * sample.cpp's arithmetic, unchanged: hermite4's expressions in its order, the 24-bit fraction
 * converted through int32 (exact, since it is under 2^24) and scaled by 2^-24, and the pointer
 * stepped with a select on the carry rather than a branch, as the C++ did. */
#include "sampleloops.h"

#include "tihints.h"

#define STRATA_SAMPLE_FRAC_BITS 24
#define STRATA_SAMPLE_FRAC_MASK 0xFFFFFFu
#define STRATA_SAMPLE_INV_FRAC 5.9604644775390625e-08f    /* 2^-24, exact */

static inline float hermite4(float x0, float x1, float x2, float x3, float t) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

#define TAP_MONO(q) ((float)(q)[0])
#define TAP_STEREO(q) ((float)STRATA_PAIR_SUM(q))

#define SAMPLE_READ_BODY(TAP)                                                                     \
    uint32_t f = *frac;                                                                           \
    int32_t at = *pos;                                                                            \
    const int16_t *q = p;                                                                         \
    int j;                                                                                        \
    if (k <= 0) return;                                                                           \
    for (j = 0; j < k - 1; j++) {                                                                 \
        const float t = (float)(int32_t)f * STRATA_SAMPLE_INV_FRAC;                               \
        dst[j] = hermite4(TAP(q - stride), TAP(q), TAP(q + stride), TAP(q + 2 * stride), t);      \
        f += fstep;                                                                               \
        {                                                                                         \
            const uint32_t carry = f >> STRATA_SAMPLE_FRAC_BITS;                                  \
            f &= STRATA_SAMPLE_FRAC_MASK;                                                         \
            q += istride + (carry != 0u ? stride : 0);                                            \
            at += istep + (int32_t)carry;                                                         \
        }                                                                                         \
    }                                                                                             \
    dst[k - 1] = hermite4(TAP(q - stride), TAP(q), TAP(q + stride), TAP(q + 2 * stride),         \
                          (float)(int32_t)f * STRATA_SAMPLE_INV_FRAC);                            \
    *frac = f;                                                                                    \
    *pos = at;

void strata_sample_read_mono(float *restrict dst, int k, const int16_t *restrict p, int stride,
                             int istride, uint32_t *frac, uint32_t fstep, int32_t *pos,
                             int32_t istep) {
    SAMPLE_READ_BODY(TAP_MONO)
}

void strata_sample_read_stereo(float *restrict dst, int k, const int16_t *restrict p, int stride,
                               int istride, uint32_t *frac, uint32_t fstep, int32_t *pos,
                               int32_t istep) {
    SAMPLE_READ_BODY(TAP_STEREO)
}
