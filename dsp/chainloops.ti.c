/* The insert chain's hand-written per-sample loops (chainloops.h). Built by cl6x when it is
 * installed, else GCC -- build_app.py routes *.ti.c to TI's compiler (docs/BUDGET.md, M2).
 *
 * The arithmetic is chain.cpp's, unchanged: same expressions, same order, same inline enaga
 * math, so a bypassed stage stays bit-identical and lo-fi quantises to the same levels. */
#include "chainloops.h"

#include <enaga/dsp/math.h>

void strata_chain_drive(float *restrict mono, int n, float positive, float negative) {
    int i;
    for (i = 0; i < n; i++) {
        const float x = mono[i];
        /* One select picks the half's gain, so there is no branch; the cubic inside
         * enaga_soft_clip is the whole cost, and the output cannot leave +-1. */
        mono[i] = enaga_soft_clip(x * (x < 0.0f ? negative : positive));
    }
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
