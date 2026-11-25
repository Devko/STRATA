/* The load-time anti-aliasing low-pass (pcmloops.h). Built by cl6x when it is installed, else
 * GCC -- build_app.py routes *.ti.c to TI's compiler (docs/archive/BUDGET_MILESTONES.md, M2).
 *
 * The coefficients and the state live in locals for the whole call, so the cascade is registers
 * and multiply-adds with one load and one store a sample. Nothing branches (?: throughout) and
 * nothing divides. */
#include "pcmloops.h"

/* One section of the cascade, as a statement: y from x and the section's state, the state
 * advanced, then x = y for the next section. A macro rather than a function so cl6x and GCC
 * both see one straight run of multiply-adds with every value in a register. */
#define STRATA_AA_SECTION(b0, b1, b2, a1, a2, s1, s2) \
    do {                                             \
        const float y_ = (b0) * x + (s1);            \
        (s1) = (b1) * x - (a1) * y_ + (s2);          \
        (s2) = (b2) * x - (a2) * y_;                 \
        x = y_;                                      \
    } while (0)

/* Rounding to the nearest int16, halves away from zero, clamped: pcm.cpp's. */
static inline int16_t strata_aa_round(float x) {
    const float r = x + (x >= 0.0f ? 0.5f : -0.5f);
    const int32_t v = r >= 32767.0f ? 32767 : (r <= -32768.0f ? -32768 : (int32_t)r);
    return (int16_t)v;
}

void strata_antialias(int16_t *samples, uint32_t frames, int stride, const float *b0,
                      const float *b1, const float *b2, const float *a1, const float *a2,
                      float *z1, float *z2) {
    const float b00 = b0[0], b01 = b0[1], b02 = b0[2], b03 = b0[3];
    const float b10 = b1[0], b11 = b1[1], b12 = b1[2], b13 = b1[3];
    const float b20 = b2[0], b21 = b2[1], b22 = b2[2], b23 = b2[3];
    const float a10 = a1[0], a11 = a1[1], a12 = a1[2], a13 = a1[3];
    const float a20 = a2[0], a21 = a2[1], a22 = a2[2], a23 = a2[3];
    float s10 = z1[0], s11 = z1[1], s12 = z1[2], s13 = z1[3];
    float s20 = z2[0], s21 = z2[1], s22 = z2[2], s23 = z2[3];
    int16_t *p = samples;
    uint32_t i;

    for (i = 0; i < frames; i++, p += stride) {
        float x = (float)*p;
        /* Transposed direct form II, one section after another -- core/pcm.cpp's order. */
        STRATA_AA_SECTION(b00, b10, b20, a10, a20, s10, s20);
        STRATA_AA_SECTION(b01, b11, b21, a11, a21, s11, s21);
        STRATA_AA_SECTION(b02, b12, b22, a12, a22, s12, s22);
        STRATA_AA_SECTION(b03, b13, b23, a13, a23, s13, s23);
        *p = strata_aa_round(x);
    }

    z1[0] = s10; z1[1] = s11; z1[2] = s12; z1[3] = s13;
    z2[0] = s20; z2[1] = s21; z2[2] = s22; z2[3] = s23;
}

void strata_antialias_stereo(int16_t *samples, uint32_t frames, const float *b0, const float *b1,
                             const float *b2, const float *a1, const float *a2, float *z1l,
                             float *z2l, float *z1r, float *z2r) {
    const float b00 = b0[0], b01 = b0[1], b02 = b0[2], b03 = b0[3];
    const float b10 = b1[0], b11 = b1[1], b12 = b1[2], b13 = b1[3];
    const float b20 = b2[0], b21 = b2[1], b22 = b2[2], b23 = b2[3];
    const float a10 = a1[0], a11 = a1[1], a12 = a1[2], a13 = a1[3];
    const float a20 = a2[0], a21 = a2[1], a22 = a2[2], a23 = a2[3];
    float l10 = z1l[0], l11 = z1l[1], l12 = z1l[2], l13 = z1l[3];
    float l20 = z2l[0], l21 = z2l[1], l22 = z2l[2], l23 = z2l[3];
    float r10 = z1r[0], r11 = z1r[1], r12 = z1r[2], r13 = z1r[3];
    float r20 = z2r[0], r21 = z2r[1], r22 = z2r[2], r23 = z2r[3];
    int16_t *p = samples;
    uint32_t i;

    for (i = 0; i < frames; i++, p += 2) {
        /* The two channels' cascades are independent, so each one's chain of multiply-adds
         * fills the other's latency: twice the work per sample in about the time of one. */
        float x = (float)p[0];
        STRATA_AA_SECTION(b00, b10, b20, a10, a20, l10, l20);
        STRATA_AA_SECTION(b01, b11, b21, a11, a21, l11, l21);
        STRATA_AA_SECTION(b02, b12, b22, a12, a22, l12, l22);
        STRATA_AA_SECTION(b03, b13, b23, a13, a23, l13, l23);
        const float left = x;
        x = (float)p[1];
        STRATA_AA_SECTION(b00, b10, b20, a10, a20, r10, r20);
        STRATA_AA_SECTION(b01, b11, b21, a11, a21, r11, r21);
        STRATA_AA_SECTION(b02, b12, b22, a12, a22, r12, r22);
        STRATA_AA_SECTION(b03, b13, b23, a13, a23, r13, r23);
        p[0] = strata_aa_round(left);
        p[1] = strata_aa_round(x);
    }

    z1l[0] = l10; z1l[1] = l11; z1l[2] = l12; z1l[3] = l13;
    z2l[0] = l20; z2l[1] = l21; z2l[2] = l22; z2l[3] = l23;
    z1r[0] = r10; z1r[1] = r11; z1r[2] = r12; z1r[3] = r13;
    z2r[0] = r20; z2r[1] = r21; z2r[2] = r22; z2r[3] = r23;
}

/* The windowed-sinc resampler (pcmloops.h). The fraction's top bits pick a row of the table, the
 * rest weight the row after it; the kernel is interpolated tap by tap and applied in the same
 * pass, so every tap is one multiply-add for the kernel and one per channel for the sum. */
#define STRATA_SINC_PHASE_SHIFT 25          /* 32 - log2(STRATA_SINC_PHASES) */
#define STRATA_SINC_BEFORE (STRATA_SINC_TAPS / 2 - 1)

uint64_t strata_sinc_resample(const int16_t *in, int stride, uint64_t position, uint64_t step,
                              const float *table, int16_t *out, uint32_t count) {
    const float scale = 1.0f / (float)(1u << STRATA_SINC_PHASE_SHIFT);
    uint32_t n;
    for (n = 0; n < count; n++) {
        const uint32_t frame = (uint32_t)(position >> 32);
        const uint32_t fraction = (uint32_t)position;
        const float *r0 = table + (fraction >> STRATA_SINC_PHASE_SHIFT) * STRATA_SINC_TAPS;
        const float *r1 = r0 + STRATA_SINC_TAPS;
        const float t = (float)(fraction & ((1u << STRATA_SINC_PHASE_SHIFT) - 1u)) * scale;
        const int16_t *src = in + (int32_t)(frame - STRATA_SINC_BEFORE) * stride;
        float acc = 0.0f;
        int k;
        for (k = 0; k < STRATA_SINC_TAPS; k++) {
            const float c = r0[k] + t * (r1[k] - r0[k]);
            acc += c * (float)src[k * stride];
        }
        out[n * stride] = strata_aa_round(acc);
        position += step;
    }
    return position;
}

uint64_t strata_sinc_resample_stereo(const int16_t *in, uint64_t position, uint64_t step,
                                     const float *table, int16_t *out, uint32_t count) {
    const float scale = 1.0f / (float)(1u << STRATA_SINC_PHASE_SHIFT);
    uint32_t n;
    for (n = 0; n < count; n++) {
        const uint32_t frame = (uint32_t)(position >> 32);
        const uint32_t fraction = (uint32_t)position;
        const float *r0 = table + (fraction >> STRATA_SINC_PHASE_SHIFT) * STRATA_SINC_TAPS;
        const float *r1 = r0 + STRATA_SINC_TAPS;
        const float t = (float)(fraction & ((1u << STRATA_SINC_PHASE_SHIFT) - 1u)) * scale;
        const int16_t *src = in + (int32_t)(frame - STRATA_SINC_BEFORE) * 2;
        float left = 0.0f, right = 0.0f;
        int k;
        for (k = 0; k < STRATA_SINC_TAPS; k++) {
            const float c = r0[k] + t * (r1[k] - r0[k]);
            left += c * (float)src[2 * k];
            right += c * (float)src[2 * k + 1];
        }
        out[2 * n] = strata_aa_round(left);
        out[2 * n + 1] = strata_aa_round(right);
        position += step;
    }
    return position;
}
