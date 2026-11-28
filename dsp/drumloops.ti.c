/* The drum machines' per-sample loops (drumloops.h). Built by cl6x when it is installed, else
 * GCC -- build_app.py routes *.ti.c to TI's compiler, whose software pipelining is what makes
 * these loops worth moving out of C++ at all (docs/archive/BUDGET_MILESTONES.md, M2).
 *
 * Every loop here is a loop that used to sit in kick.cpp, snare.cpp or hat.cpp, moved with its
 * arithmetic untouched: the same expressions in the same order, through the same inline enaga
 * math, so the sound is bit-identical from either compiler. This is a compilation-unit change,
 * not a rewrite -- if a shaper is ever made cheaper (the polynomial fit for 1/(1+|t|) that
 * kick.cpp and snare.cpp both suggest, say), that is a separate change with its own listening
 * test.
 *
 * Nothing branches (`?:` throughout, as cl6x will not software-pipeline a loop that does),
 * nothing divides (enaga_reciprocal is 1/x without a division), nothing allocates and the state
 * each loop carries between blocks arrives by pointer. */
#include "drumloops.h"

#include <enaga/dsp/math.h>

/* ---- KICK ---- */

/* Plaits' distorted sine: lerp(clean sine, rational soft sine, dirtiness). The fold is the
 * expensive half -- a reciprocal per sample -- and the clean sine is a second polynomial, which
 * is why kick.cpp calls the shaper its largest stage. The 0.75 turn puts the clean sine in phase
 * with the fold. kick.cpp documents the model. */
static inline float distorted_sine(float phase, float dirtiness) {
    const float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
    const float magnitude = triangle < 0.0f ? -triangle : triangle;
    const float fold = 2.0f * triangle * enaga_reciprocal(1.0f + magnitude);
    const float clean = enaga_sin_turns(phase + 0.75f);
    return clean + (fold - clean) * dirtiness;
}

void strata_kick_body(float *restrict out, int hold, int n, float f0_norm, float fm_amount,
                      float fm_decay, float dirtiness, float *phase, float *fm_env) {
    float p = *phase;
    float e = *fm_env;
    int i;
    /* While the phase is pinned the shaper reads 0, so those samples are silence the caller has
     * already cleared -- but the FM envelope still runs through them. */
    for (i = 0; i < hold; i++) e *= fm_decay;
    for (i = hold; i < n; i++) {
        float dp = f0_norm * (1.0f + 3.5f * fm_amount * e);
        dp = dp > 0.5f ? 0.5f : dp;
        p += dp;
        p -= p >= 1.0f ? 1.0f : 0.0f;
        out[i] = distorted_sine(p, dirtiness);
        e *= fm_decay;
    }
    *phase = p;
    *fm_env = e;
}

void strata_kick_body_knock(float *restrict out, int hold, int n, float f0_norm, float fm_amount,
                            float fm_decay, float knock_amount, float knock_decay,
                            float dirtiness, float *phase, float *fm_env, float *knock_env) {
    float p = *phase;
    float e = *fm_env;
    float k = *knock_env;
    int i;
    for (i = 0; i < hold; i++) {
        e *= fm_decay;
        k *= knock_decay;
    }
    for (i = hold; i < n; i++) {
        float dp = f0_norm * (1.0f + 3.5f * fm_amount * e + knock_amount * k);
        dp = dp > 0.5f ? 0.5f : dp;
        p += dp;
        p -= p >= 1.0f ? 1.0f : 0.0f;
        out[i] = distorted_sine(p, dirtiness);
        e *= fm_decay;
        k *= knock_decay;
    }
    *phase = p;
    *fm_env = e;
    *knock_env = k;
}

void strata_kick_tick(float *restrict out, const float *restrict src, int n, float gain,
                      float decay, float *env) {
    float e = *env;
    int i;
    for (i = 0; i < n; i++) {
        out[i] += src[i] * gain * e;
        e *= decay;
    }
    *env = e;
}

void strata_kick_rumble(float *restrict out, const float *restrict in, int n, float lp_coef,
                        float *lp, float *restrict ap1, int ap1_len, int *ap1_pos,
                        float *restrict ap2, int ap2_len, int *ap2_pos, float *restrict c1,
                        int c1_len, int *c1_pos, float *restrict c2, int c2_len, int *c2_pos,
                        float feedback, float drive, float makeup, float *duck, float release) {
    float l = *lp, d = *duck;
    int p1 = *ap1_pos, p2 = *ap2_pos, q1 = *c1_pos, q2 = *c2_pos;
    const float g = 0.6f;
    int i;
    for (i = 0; i < n; i++) {
        l += lp_coef * (in[i] - l);
        /* two all-passes: y = -g x + z, z' = x + g y */
        const float z1 = ap1[p1];
        const float y1 = z1 - g * l;
        ap1[p1] = l + g * y1;
        p1 = p1 + 1 >= ap1_len ? 0 : p1 + 1;
        const float z2 = ap2[p2];
        const float y2 = z2 - g * y1;
        ap2[p2] = y1 + g * y2;
        p2 = p2 + 1 >= ap2_len ? 0 : p2 + 1;
        /* two combs in parallel */
        const float o1 = c1[q1];
        const float o2 = c2[q2];
        c1[q1] = y2 + feedback * o1;
        c2[q2] = y2 + feedback * o2;
        q1 = q1 + 1 >= c1_len ? 0 : q1 + 1;
        q2 = q2 + 1 >= c2_len ? 0 : q2 + 1;
        const float wet = enaga_soft_clip((o1 + o2) * drive) * makeup;
        out[i] = wet * (1.0f - d);
        d *= release;
    }
    *lp = l;
    *duck = d;
    *ap1_pos = p1;
    *ap2_pos = p2;
    *c1_pos = q1;
    *c2_pos = q2;
}

void strata_kick_click(float *restrict out, int n, int gate, float *slew, float *high_pass) {
    float s = *slew;
    float hp = *high_pass;
    int i;
    for (i = 0; i < n; i++) {
        const float level = i < gate ? 1.0f : 0.0f;    /* the 1 ms gate */
        s += (level > s ? 0.5f : 0.1f) * (level - s);  /* slewed 0.5 rising, 0.1 falling */
        hp += 0.04f * (s - hp);
        out[i] = s - hp;                               /* the gate less its one-pole low-pass */
    }
    *slew = s;
    *high_pass = hp;
}

/* ---- SNARE ---- */

/* The rational soft sine both of the snare's oscillators run through: a triangle folded by
 * 2x/(1+|x|). One reciprocal per oscillator per sample, and there are two oscillators, which is
 * what snare.cpp's cost comment calls its largest stage. */
static inline float soft_sine(float phase) {
    const float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
    const float magnitude = triangle < 0.0f ? -triangle : triangle;
    return 2.0f * triangle * enaga_reciprocal(1.0f + magnitude);
}

void strata_snare_pair(float *restrict out, int n, float dp_a, float dp_b, float *phase_a,
                       float *phase_b) {
    float pa = *phase_a;
    float pb = *phase_b;
    int i;
    for (i = 0; i < n; i++) {
        pa += dp_a;
        pa -= pa >= 1.0f ? 1.0f : 0.0f;
        pb += dp_b;
        pb -= pb >= 1.0f ? 1.0f : 0.0f;
        /* the reference's -0.1 offset and its 0.60 / 0.25 weights */
        out[i] = -0.1f + 0.60f * soft_sine(pa) + 0.25f * soft_sine(pb);
    }
    *phase_a = pa;
    *phase_b = pb;
}

/* ---- HAT ---- */

/* enaga_xorshift's step without its store, so the next value can be computed every sample and
 * committed only on a sample-and-hold clock edge. That is what keeps the noise stream identical
 * to hat.cpp's `if (wrapped) hold = rng_.bipolar();` while the loop stays branch-free. */
static inline uint32_t xorshift_step(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void strata_hat_squares(float *restrict out, int n, uint32_t *restrict phase,
                        const uint32_t *restrict step, uint32_t sample_hold_step,
                        uint32_t *hold_phase, float *hold, uint32_t *rng, float noisiness) {
    /* The six accumulators and their increments in locals: the whole oscillator bank is then
     * registers and integer adds, with no load in the loop at all. */
    uint32_t p0 = phase[0], p1 = phase[1], p2 = phase[2];
    uint32_t p3 = phase[3], p4 = phase[4], p5 = phase[5];
    const uint32_t s0 = step[0], s1 = step[1], s2 = step[2];
    const uint32_t s3 = step[3], s4 = step[4], s5 = step[5];
    uint32_t clock = *hold_phase;
    uint32_t state = *rng;
    float held = *hold;
    int i;
    for (i = 0; i < n; i++) {
        p0 += s0;
        p1 += s1;
        p2 += s2;
        p3 += s3;
        p4 += s4;
        p5 += s5;
        const uint32_t high = (p0 >> 31) + (p1 >> 31) + (p2 >> 31) + (p3 >> 31) + (p4 >> 31) +
                              (p5 >> 31);
        const float squares = 0.33f * (float)high - 1.0f;   /* 0.33, not 1/3: see hat.hpp */
        const uint32_t before = clock;
        clock += sample_hold_step;
        const uint32_t next = xorshift_step(state);
        const int edge = clock < before;                    /* the wrap is the noise clock */
        state = edge ? next : state;
        held = edge ? enaga_bipolar(next) : held;
        out[i] = squares + (held - squares) * noisiness;
    }
    phase[0] = p0;
    phase[1] = p1;
    phase[2] = p2;
    phase[3] = p3;
    phase[4] = p4;
    phase[5] = p5;
    *hold_phase = clock;
    *hold = held;
    *rng = state;
}

void strata_hat_vca(float *restrict buf, const float *restrict env, int n, float trigger_level) {
    int i;
    for (i = 0; i < n; i++) {
        const float x = buf[i] * env[i] * trigger_level;
        buf[i] = enaga_soft_clip(x > 0.0f ? 4.0f * x : 0.1f * x);
    }
}

void strata_hat_back(float *restrict buf, const float *restrict env, int n, enaga_svf *band,
                     const enaga_svf_coefficients *band_c, enaga_svf *high,
                     const enaga_svf_coefficients *high_c, float trigger_level) {
    const enaga_svf_coefficients kb = *band_c, kh = *high_c;
    enaga_svf tb = *band, th = *high;
    float low, mid, top;
    int i;
    for (i = 0; i < n; i++) {
        enaga_svf_step(&tb, &kb, buf[i], &low, &mid, &top);
        const float x = mid * env[i] * trigger_level;
        enaga_svf_step(&th, &kh, enaga_soft_clip(x > 0.0f ? 4.0f * x : 0.1f * x), &low, &mid, &top);
        buf[i] = top;
    }
    *band = tb;
    *high = th;
}

/* ---- shared ---- */

void strata_rng_skip(uint32_t *state, int n) {
    uint32_t x = *state;
    int i;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    *state = x;
}


void strata_mix_ramp(float *restrict out, const float *restrict in, int n, float gain,
                     float step) {
    int i;
    for (i = 0; i < n; i++) {
        out[i] += gain * in[i];
        gain += step;
    }
}
