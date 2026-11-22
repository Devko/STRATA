/* The master bus's per-sample loops (masterloops.h). Built by cl6x when it is installed, else
 * GCC -- build_app.py routes *.ti.c to TI's compiler (docs/BUDGET.md, M2).
 *
 * The arithmetic is buses.cpp's, unchanged: same expressions, same order, same inline enaga
 * math, so the limiter still cannot be driven past full scale and a quiet master is still
 * bit-exact. */
#include "masterloops.h"

#include <enaga/dsp/math.h>

float strata_master_peak(const float *restrict left, const float *restrict right, int n) {
    float peak = 0.0f;
    int i;
    for (i = 0; i < n; i++) {
        const float l = left[i] < 0.0f ? -left[i] : left[i];
        const float r = right[i] < 0.0f ? -right[i] : right[i];
        const float m = l > r ? l : r;
        peak = m > peak ? m : peak;
    }
    return peak;
}

void strata_master_ramp(float *restrict left, float *restrict right, int n, float gain,
                        float step) {
    int i;
    for (i = 0; i < n; i++) {
        gain += step;
        left[i] *= gain;
        right[i] *= gain;
    }
}

void strata_master_limit(float *restrict left, float *restrict right, int n, float threshold,
                         float headroom, float inv_headroom) {
    int i;
    for (i = 0; i < n; i++) {
        const float l = left[i];
        const float la = l < 0.0f ? -l : l;
        const float lb = la < threshold ? la : threshold;
        const float lm = lb + headroom * enaga_soft_clip((la - lb) * inv_headroom);
        left[i] = l < 0.0f ? -lm : lm;

        const float r = right[i];
        const float ra = r < 0.0f ? -r : r;
        const float rb = ra < threshold ? ra : threshold;
        const float rm = rb + headroom * enaga_soft_clip((ra - rb) * inv_headroom);
        right[i] = r < 0.0f ? -rm : rm;
    }
}

void strata_interleave(float *restrict out, const float *restrict left,
                       const float *restrict right, int n, int frames) {
    int i;
    for (i = 0; i < n; i++) {
        out[2 * i] = left[i];
        out[2 * i + 1] = right[i];
    }
    for (i = n; i < frames; i++) {   /* only if something ever hands us more than a block */
        out[2 * i] = 0.0f;
        out[2 * i + 1] = 0.0f;
    }
}

/* One trapezoidal SVF step, enaga_svf_step's arithmetic with the state in two locals the
 * compiler can keep in registers. `low` and `band` out; the high-pass is x - k band - low. */
#define STRATA_SVF(x, a1, a2, a3, s1, s2, low, band)          \
    do {                                                       \
        const float v3_ = (x) - (s2);                          \
        const float v1_ = (a1) * (s1) + (a2) * v3_;            \
        const float v2_ = (s2) + (a2) * (s1) + (a3) * v3_;     \
        (s1) = 2.0f * v1_ - (s1);                              \
        (s2) = 2.0f * v2_ - (s2);                              \
        (low) = v2_;                                           \
        (band) = v1_;                                          \
    } while (0)

/* One channel's sample through both crossovers. `s` is its 14 state floats, by name so they
 * live in registers across the loop. */
#define STRATA_KILL_EQ_CHANNEL(x, s, out)                                                      \
    do {                                                                                       \
        float lo_, bd_, low_a_, high_a_, low_, rest_, mid_a_, high_b_, mid_, high_, ap_;       \
        /* the lower crossover: one Butterworth stage, then a second on each side (LR4) */    \
        STRATA_SVF(x, la1, la2, la3, s[0], s[1], lo_, bd_);                                    \
        low_a_ = lo_;                                                                          \
        high_a_ = (x) - lk * bd_ - lo_;                                                        \
        STRATA_SVF(low_a_, la1, la2, la3, s[2], s[3], lo_, bd_);                               \
        low_ = lo_;                                                                            \
        STRATA_SVF(high_a_, la1, la2, la3, s[4], s[5], lo_, bd_);                              \
        rest_ = high_a_ - lk * bd_ - lo_;                                                      \
        /* the upper crossover, on what is above the lower one */                             \
        STRATA_SVF(rest_, ha1, ha2, ha3, s[6], s[7], lo_, bd_);                                \
        mid_a_ = lo_;                                                                          \
        high_b_ = rest_ - hk * bd_ - lo_;                                                      \
        STRATA_SVF(mid_a_, ha1, ha2, ha3, s[8], s[9], lo_, bd_);                               \
        mid_ = lo_;                                                                            \
        STRATA_SVF(high_b_, ha1, ha2, ha3, s[10], s[11], lo_, bd_);                            \
        high_ = high_b_ - hk * bd_ - lo_;                                                      \
        /* LOW through the upper crossover's all-pass, x - 2 k band, to stay in phase */      \
        STRATA_SVF(low_, ha1, ha2, ha3, s[12], s[13], lo_, bd_);                               \
        ap_ = low_ - 2.0f * hk * bd_;                                                          \
        (out) = (x) + wet * (gl * ap_ + gm * mid_ + gh * high_ - (x));                         \
    } while (0)

void strata_kill_eq(float *restrict left, float *restrict right, int n, float *restrict state_l,
                    float *restrict state_r, const float *restrict low_split,
                    const float *restrict high_split, const float *restrict gains,
                    const float *restrict gain_steps, float wet, float wet_step) {
    const float la1 = low_split[0], la2 = low_split[1], la3 = low_split[2], lk = low_split[3];
    const float ha1 = high_split[0], ha2 = high_split[1], ha3 = high_split[2], hk = high_split[3];
    const float sl = gain_steps[0], sm = gain_steps[1], sh = gain_steps[2];
    float gl = gains[0], gm = gains[1], gh = gains[2];
    float l[STRATA_KILL_EQ_STATES], r[STRATA_KILL_EQ_STATES];
    int i;
    for (i = 0; i < STRATA_KILL_EQ_STATES; i++) {
        l[i] = state_l[i];
        r[i] = state_r[i];
    }
    for (i = 0; i < n; i++) {
        const float xl = left[i];
        const float xr = right[i];
        float yl, yr;
        gl += sl;
        gm += sm;
        gh += sh;
        wet += wet_step;
        STRATA_KILL_EQ_CHANNEL(xl, l, yl);
        STRATA_KILL_EQ_CHANNEL(xr, r, yr);
        left[i] = yl;
        right[i] = yr;
    }
    for (i = 0; i < STRATA_KILL_EQ_STATES; i++) {
        state_l[i] = l[i];
        state_r[i] = r[i];
    }
}
