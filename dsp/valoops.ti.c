/* The VA voice's output tail (valoops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler (docs/BUDGET.md, M2).
 *
 * The arithmetic is va.cpp's, unchanged: same expressions, same order, same associations, same
 * inline enaga math, so the voice is bit-exact against the three Enaga passes this replaces. */
#include "valoops.h"

#include <enaga/dsp/math.h>

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
