/* The FM operator loop (fmloops.h). Plain C, no branches, no division. */
#include "fmloops.h"

#include "tihints.h"

/* sin(2 pi (p / 2^32 + mod)), mod in turns, |mod| < 512. The modulation joins the phase as an
 * integer, so the table index and the fraction come from shifts and a mask: no float floor. */
static inline float fm_sine(const float *restrict table, uint32_t p, float mod) {
    const uint32_t q = p + ((uint32_t)(int32_t)(mod * 4194304.0f) << 10);
    const uint32_t k = q >> (32 - STRATA_FM_SINE_BITS);
    const float f = (float)(q & 0x3FFFFFu) * (1.0f / 4194304.0f);
    const float a = table[k];
    return a + f * (table[k + 1] - a);
}

void strata_fm_add(float *restrict out, int n, const float *restrict env, strata_fm_state *restrict s,
                   const strata_fm_block *restrict b) {
    uint32_t p1 = s->phase[0], p2 = s->phase[1], p3 = s->phase[2], p4 = s->phase[3];
    const uint32_t d1 = b->inc[0], d2 = b->inc[1], d3 = b->inc[2], d4 = b->inc[3];
    const float m12 = b->m12, m13 = b->m13, m14 = b->m14, m23 = b->m23, m24 = b->m24, m34 = b->m34;
    const float c1 = b->c[0], c2 = b->c[1], c3 = b->c[2], c4 = b->c[3];
    float i2 = b->i2, i3 = b->i3, i4 = b->i4, fb = b->fb;
    const float di2 = b->di2, di3 = b->di3, di4 = b->di4, dfb = b->dfb;
    float last4 = s->last4;
    const float *restrict sine = b->sine;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float o4 = fm_sine(sine, p4, fb * last4);
        const float o3 = fm_sine(sine, p3, m34 * i4 * o4);
        const float o2 = fm_sine(sine, p2, (m23 * i3) * o3 + (m24 * i4) * o4);
        const float o1 = fm_sine(sine, p1, (m12 * i2) * o2 + (m13 * i3) * o3 + (m14 * i4) * o4);
        out[i] += env[i] * (c1 * o1 + c2 * o2 + c3 * o3 + c4 * o4);
        last4 = o4;
        p1 += d1;
        p2 += d2;
        p3 += d3;
        p4 += d4;
        i2 += di2;
        i3 += di3;
        i4 += di4;
        fb += dfb;
    }
    s->phase[0] = p1;
    s->phase[1] = p2;
    s->phase[2] = p3;
    s->phase[3] = p4;
    s->last4 = last4;
}

void strata_fm_add_free(float *restrict out, int n, const float *restrict env,
                        strata_fm_state *restrict s, const strata_fm_block *restrict b) {
    uint32_t p1 = s->phase[0], p2 = s->phase[1], p3 = s->phase[2], p4 = s->phase[3];
    const uint32_t d1 = b->inc[0], d2 = b->inc[1], d3 = b->inc[2], d4 = b->inc[3];
    const float m12 = b->m12, m13 = b->m13, m14 = b->m14, m23 = b->m23, m24 = b->m24, m34 = b->m34;
    const float c1 = b->c[0], c2 = b->c[1], c3 = b->c[2], c4 = b->c[3];
    float i2 = b->i2, i3 = b->i3, i4 = b->i4;
    const float di2 = b->di2, di3 = b->di3, di4 = b->di4;
    const float *restrict sine = b->sine;
    float o4 = 0.0f;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        o4 = fm_sine(sine, p4, 0.0f);
        const float o3 = fm_sine(sine, p3, m34 * i4 * o4);
        const float o2 = fm_sine(sine, p2, (m23 * i3) * o3 + (m24 * i4) * o4);
        const float o1 = fm_sine(sine, p1, (m12 * i2) * o2 + (m13 * i3) * o3 + (m14 * i4) * o4);
        out[i] += env[i] * (c1 * o1 + c2 * o2 + c3 * o3 + c4 * o4);
        p1 += d1;
        p2 += d2;
        p3 += d3;
        p4 += d4;
        i2 += di2;
        i3 += di3;
        i4 += di4;
    }
    s->phase[0] = p1;
    s->phase[1] = p2;
    s->phase[2] = p3;
    s->phase[3] = p4;
    s->last4 = o4;
}
