/* The VA voice's output tail (valoops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler (docs/archive/BUDGET_MILESTONES.md, M2).
 *
 * The arithmetic is va.cpp's, unchanged: same expressions, same order, same associations, same
 * inline enaga math, so the voice is bit-exact against the three Enaga passes this replaces. */
#include "valoops.h"

#include <enaga/dsp/math.h>

#include "tihints.h"

void strata_va_tail(float *restrict out, const float *restrict work, const float *restrict env,
                    int n, float drive, float gain, float step) {
    int i;
    for (i = 0; i < n; i++) {
        /* enaga_soft_clip_buffer's body, then enaga_multiply's, then mix_ramp's -- in that
         * order and with those associations. `(clipped * env[i])` is bracketed because
         * enaga_multiply formed that product before mix_ramp multiplied by the gain, and float
         * multiplication is not associative. */
        const float clipped = enaga_soft_clip(drive * work[i]);
        out[i] += gain * (clipped * env[i]);
        gain += step;
    }
}

/* ---- the driven filter (valoops.h, strata_va_drive_filter) ---- */

#define VA_DRIVE(x) (enaga_soft_clip(pre * (x)) * inv_pre)

void strata_va_drive_filter(float *restrict work, int n, int mode, enaga_svf *s1,
                            const enaga_svf_coefficients *c1, enaga_svf *s2,
                            const enaga_svf_coefficients *c2, float pre, float inv_pre) {
    const enaga_svf_coefficients k2 = *c2;
    enaga_svf t2 = *s2;
    float o[3];
    int i;
    if (mode == STRATA_VA_LP24) {
        const enaga_svf_coefficients k1 = *c1;
        enaga_svf t1 = *s1;
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) {
            float p[3];
            enaga_svf_step(&t1, &k1, VA_DRIVE(work[i]), &p[0], &p[1], &p[2]);
            enaga_svf_step(&t2, &k2, p[0], &o[0], &o[1], &o[2]);
            work[i] = o[0];
        }
        *s1 = t1;
    } else {
        const int out = mode == STRATA_VA_LP12 ? 0 : (mode == STRATA_VA_BP12 ? 1 : 2);
        STRATA_BLOCK_LOOP
        for (i = 0; i < n; i++) {
            enaga_svf_step(&t2, &k2, VA_DRIVE(work[i]), &o[0], &o[1], &o[2]);
            work[i] = o[out];
        }
    }
    *s2 = t2;
}

/* ---- the fused back half (valoops.h) ---- */

/* The filter, one sample: x in, y out. LP24 is Enaga's cascade body, the others its single
 * stage, each giving one output. */
#define VA_FILTER_LP24(x, y)                                                   \
    do {                                                                       \
        float o_[3], p_[3];                                                    \
        enaga_svf_step(&t1, &k1, (x), &o_[0], &o_[1], &o_[2]);                 \
        enaga_svf_step(&t2, &k2, o_[0], &p_[0], &p_[1], &p_[2]);               \
        (y) = p_[0];                                                           \
    } while (0)
#define VA_FILTER_ONE(OUT, x, y)                                               \
    do {                                                                       \
        float p_[3];                                                           \
        enaga_svf_step(&t2, &k2, (x), &p_[0], &p_[1], &p_[2]);                 \
        (y) = p_[OUT];                                                         \
    } while (0)
#define VA_FILTER_LP12(x, y) VA_FILTER_ONE(0, x, y)
#define VA_FILTER_BP12(x, y) VA_FILTER_ONE(1, x, y)
#define VA_FILTER_HP12(x, y) VA_FILTER_ONE(2, x, y)

/* The envelope, one sample, as enaga_envelope_render's two loops have it. */
#define VA_ENV_ADS                                                             \
    do {                                                                       \
        const float la = l + a * (ENAGA_ENV_ATTACK_TARGET - l);                \
        const float ld = l + d * (sustain - l);                                \
        const int peak = attack & (la >= 1.0f);                                \
        const int settled = (!attack) & decay & (ld - sustain < 1e-4f);        \
        l = attack ? (peak ? 1.0f : la) : ld;                                  \
        decay = (decay | peak) & (!settled);                                   \
        attack &= (!peak);                                                     \
    } while (0)
#define VA_ENV_RELEASE                                                         \
    do {                                                                       \
        const float lr = l + r * (-0.001f - l);                                \
        l = lr <= 0.0f ? 0.0f : lr;                                            \
    } while (0)
#define VA_ENV_HOLD do { } while (0)

#define VA_BACK_LOOP(FILTER, ENV)                                              \
    STRATA_BLOCK_LOOP                                                          \
    for (i = 0; i < n; i++) {                                                  \
        float y;                                                               \
        const float x_ = pre > 0.0f ? VA_DRIVE(work[i]) : work[i];             \
        FILTER(x_, y);                                                         \
        ENV;                                                                   \
        out[i] += g * (enaga_soft_clip(drive * y) * l);                        \
        g += step;                                                             \
    }

/* One function per filter mode, each with the envelope's three cases: twelve specialised,
 * branch-free loops, as Enaga keeps its own loops specialised on the output. */
#define VA_BACK_FUNCTION(NAME, FILTER)                                                         \
    static void NAME(float *restrict out, const float *restrict work, int n, enaga_svf *s1,    \
                     const enaga_svf_coefficients *c1, enaga_svf *s2,                          \
                     const enaga_svf_coefficients *c2, float pre, float inv_pre,               \
                     enaga_envelope *amp,                                                      \
                     const enaga_envelope_shape *shape, float drive, float g, float step) {    \
        const enaga_svf_coefficients k1 = *c1, k2 = *c2;                                       \
        enaga_svf t1 = *s1, t2 = *s2;                                                          \
        float l = amp->level;                                                                  \
        int i;                                                                                 \
        (void)k1;    /* the 12 dB modes run only the second stage */                          \
        switch (amp->stage) {                                                                  \
        case ENAGA_ENV_ATTACK:                                                                 \
        case ENAGA_ENV_DECAY:                                                                  \
        case ENAGA_ENV_SUSTAIN: {                                                              \
            const float a = shape->attack, d = shape->decay, sustain = shape->sustain;        \
            int attack = amp->stage == ENAGA_ENV_ATTACK, decay = amp->stage == ENAGA_ENV_DECAY; \
            VA_BACK_LOOP(FILTER, VA_ENV_ADS)                                                   \
            amp->stage = attack ? ENAGA_ENV_ATTACK : decay ? ENAGA_ENV_DECAY : ENAGA_ENV_SUSTAIN; \
            break;                                                                             \
        }                                                                                      \
        case ENAGA_ENV_RELEASE: {                                                              \
            const float r = shape->release;                                                    \
            VA_BACK_LOOP(FILTER, VA_ENV_RELEASE)                                               \
            if (l <= 0.0f) amp->stage = ENAGA_ENV_IDLE;                                        \
            break;                                                                             \
        }                                                                                      \
        default:                                                                               \
            VA_BACK_LOOP(FILTER, VA_ENV_HOLD)                                                  \
            break;                                                                             \
        }                                                                                      \
        amp->level = l;                                                                        \
        *s1 = t1;                                                                              \
        *s2 = t2;                                                                              \
    }

VA_BACK_FUNCTION(va_back_lp24, VA_FILTER_LP24)
VA_BACK_FUNCTION(va_back_lp12, VA_FILTER_LP12)
VA_BACK_FUNCTION(va_back_bp12, VA_FILTER_BP12)
VA_BACK_FUNCTION(va_back_hp12, VA_FILTER_HP12)

void strata_va_back(float *restrict out, const float *restrict work, int n, int mode, enaga_svf *s1,
                    const enaga_svf_coefficients *c1, enaga_svf *s2,
                    const enaga_svf_coefficients *c2, float pre, float inv_pre, enaga_envelope *amp,
                    const enaga_envelope_shape *shape, float drive, float gain, float step) {
    if (n <= 0) return;
    switch (mode) {
    case STRATA_VA_LP24:
        va_back_lp24(out, work, n, s1, c1, s2, c2, pre, inv_pre, amp, shape, drive, gain, step);
        break;
    case STRATA_VA_LP12:
        va_back_lp12(out, work, n, s1, c1, s2, c2, pre, inv_pre, amp, shape, drive, gain, step);
        break;
    case STRATA_VA_BP12:
        va_back_bp12(out, work, n, s1, c1, s2, c2, pre, inv_pre, amp, shape, drive, gain, step);
        break;
    default:
        va_back_hp12(out, work, n, s1, c1, s2, c2, pre, inv_pre, amp, shape, drive, gain, step);
        break;
    }
}
