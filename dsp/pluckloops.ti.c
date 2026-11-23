/* The STRING machine's loops (pluckloops.h). Plain C, no branches, no division. */
#include "pluckloops.h"

#include "tihints.h"

float strata_pluck_run(float *restrict out, int n, const float *restrict level, strata_pluck_state *s,
                       float delay, float coef, float gain) {
    /* The line is read and written in the same loop, so it is NOT restrict: when the period is
     * shorter than the slice a sample written here is read again a period later. */
    float *line = s->line;
    uint32_t w = s->write;
    float lp = s->lp;
    float peak = 0.0f;
    const int whole = (int)delay;
    const float frac = delay - (float)whole;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const uint32_t r0 = (w - (uint32_t)whole) & STRATA_PLUCK_MASK;
        const uint32_t r1 = (r0 - 1u) & STRATA_PLUCK_MASK;
        const float x = line[r0] + (line[r1] - line[r0]) * frac;
        float y;
        lp += coef * (x - lp);
        y = gain * lp;
        line[w & STRATA_PLUCK_MASK] = y;
        out[i] += level[i] * y;
        {
            const float a = y < 0.0f ? -y : y;
            peak = a > peak ? a : peak;
        }
        w++;
    }
    s->write = w & STRATA_PLUCK_MASK;
    s->lp = lp;
    return peak;
}

float strata_bar_run(float *restrict out, int n, const float *restrict in, const float *restrict level,
                     strata_bar_state *restrict s, const strata_bar_coefs *restrict c) {
    float y10 = s->y1[0], y11 = s->y1[1], y12 = s->y1[2], y13 = s->y1[3];
    float y20 = s->y2[0], y21 = s->y2[1], y22 = s->y2[2], y23 = s->y2[3];
    const float a10 = c->a1[0], a11 = c->a1[1], a12 = c->a1[2], a13 = c->a1[3];
    const float a20 = c->a2[0], a21 = c->a2[1], a22 = c->a2[2], a23 = c->a2[3];
    const float g0 = c->g[0], g1 = c->g[1], g2 = c->g[2], g3 = c->g[3];
    float peak = 0.0f;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float x = in[i];
        const float m0 = a10 * y10 + a20 * y20 + g0 * x;
        const float m1 = a11 * y11 + a21 * y21 + g1 * x;
        const float m2 = a12 * y12 + a22 * y22 + g2 * x;
        const float m3 = a13 * y13 + a23 * y23 + g3 * x;
        const float sum = m0 + m1 + m2 + m3;
        y20 = y10;
        y21 = y11;
        y22 = y12;
        y23 = y13;
        y10 = m0;
        y11 = m1;
        y12 = m2;
        y13 = m3;
        out[i] += level[i] * sum;
        {
            const float a = sum < 0.0f ? -sum : sum;
            peak = a > peak ? a : peak;
        }
    }
    s->y1[0] = y10;
    s->y1[1] = y11;
    s->y1[2] = y12;
    s->y1[3] = y13;
    s->y2[0] = y20;
    s->y2[1] = y21;
    s->y2[2] = y22;
    s->y2[3] = y23;
    return peak;
}

void strata_pluck_noise(float *restrict e, int n, uint32_t *rng, float coef) {
    uint32_t x = *rng;
    float lp = 0.0f;
    int i;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        lp += coef * ((float)(int32_t)x * (1.0f / 2147483648.0f) - lp);
        e[i] = lp;
    }
    *rng = x;
}

void strata_pluck_inject(float *restrict line, uint32_t write, const float *restrict e, int n, int comb,
                         float gain) {
    const uint32_t start = write - (uint32_t)n;
    int i;
    for (i = 0; i < n; i++) {
        const int back = i - comb;
        const float early = e[back < 0 ? 0 : back];
        const float d = back < 0 ? 0.0f : early;
        line[(start + (uint32_t)i) & STRATA_PLUCK_MASK] += gain * (e[i] - d);
    }
}
