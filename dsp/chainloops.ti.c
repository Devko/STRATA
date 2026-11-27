/* The insert chain's hand-written per-sample loops (chainloops.h). Built by cl6x when it is
 * installed, else GCC -- build_app.py routes *.ti.c to TI's compiler (docs/archive/BUDGET_MILESTONES.md, M2).
 *
 * The arithmetic is chain.cpp's, unchanged: same expressions, same order, same inline enaga
 * math, so a bypassed stage stays bit-identical and lo-fi quantises to the same levels. */
#include "chainloops.h"

#include <enaga/dsp/math.h>

void strata_chain_drive(float *restrict mono, int n, float positive, float negative, float pole,
                        float *dc) {
    float x1 = dc[0], y1 = dc[1];
    int i;
    for (i = 0; i < n; i++) {
        const float x = mono[i];
        /* One select picks the half's gain, so there is no branch; the cubic inside
         * enaga_soft_clip is the whole cost, and the output cannot leave +-1. */
        const float c = enaga_soft_clip(x * (x < 0.0f ? negative : positive));
        /* The DC blocker: y = c - c' + pole y', a zero at DC and a pole just inside it. Taking
         * the offset out lets a fully clipped wave tilt a little past +-1, so the stage's bound
         * is kept by a clamp on the way out; the state keeps the unclamped value. */
        y1 = c - x1 + pole * y1;
        x1 = c;
        mono[i] = enaga_clamp(y1, -1.0f, 1.0f);
    }
    dc[0] = x1;
    dc[1] = y1;
}

void strata_chain_lofi(float *restrict mono, int n, uint32_t increment, float levels, float step,
                       float ceiling, uint32_t *phase, float *hold) {
    uint32_t p = *phase;
    float held = *hold;
    int i;
    for (i = 0; i < n; i++) {
        const uint32_t previous = p;
        p += increment;
        /* The wrap is the clock edge: that is the whole sample-and-hold. The held sample is
         * bounded first, because the bit crush rounds through an int. */
        held = (p < previous) ? enaga_clamp(mono[i], -ceiling, ceiling) : held;
        mono[i] = (float)enaga_floor_int(held * levels + 0.5f) * step;
    }
    *phase = p;
    *hold = held;
}

void strata_chain_duck(float *restrict mono, const float *restrict duck, int n, float depth) {
    int i;
    for (i = 0; i < n; i++) mono[i] *= 1.0f - depth * duck[i];
}

float strata_choke_fade(float *restrict mono, int n, float gain, float step) {
    int i;
    for (i = 0; i < n; i++) {
        gain = gain - step;
        gain = gain > 0.0f ? gain : 0.0f;
        mono[i] *= gain;
    }
    return gain;
}
