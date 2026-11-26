/* The metronome click's loop (clickloops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler. */
#include "clickloops.h"

float strata_click_add(float *restrict out, int n, float *restrict s1, float *restrict s2,
                       float coef, float env, float decay) {
    float y1 = *s1;
    float y2 = *s2;
    int i;
    for (i = 0; i < n; i++) {
        const float y = coef * y1 - y2;
        const float v = y * env;
        out[2 * i] += v;
        out[2 * i + 1] += v;
        y2 = y1;
        y1 = y;
        env *= decay;
    }
    *s1 = y1;
    *s2 = y2;
    return env;
}
