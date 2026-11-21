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
