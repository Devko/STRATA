/* Beat repeat's per-sample loops (repeatloops.h). Built by cl6x when it is installed, else GCC
 * -- build_app.py routes *.ti.c to TI's compiler (docs/archive/BUDGET_MILESTONES.md, M2).
 *
 * Nothing here branches or divides; every wrap is a compare-and-subtract through ?:. */
#include "repeatloops.h"

int strata_repeat_capture(float *restrict ring, int ring_frames, int write,
                          const float *restrict in, int n) {
    int w = write;
    int i;
    for (i = 0; i < n; i++) {
        ring[2 * w + 0] = in[2 * i + 0];
        ring[2 * w + 1] = in[2 * i + 1];
        w++;
        w = w >= ring_frames ? 0 : w;
    }
    return w;
}

void strata_repeat_play(const float *restrict ring, int ring_frames, int start, int length,
                        int *restrict offset, uint32_t *restrict frac, int step_int,
                        uint32_t step_frac, float *restrict out, int n) {
    /* 1 / 2^32, so the Q32 fraction becomes a float by one multiply. */
    const float kFracScale = 2.3283064365386963e-10f;
    int o = *offset;
    uint32_t f = *frac;
    int i;

    for (i = 0; i < n; i++) {
        /* This frame, and the next one INSIDE the loop: at the loop's last frame the partner is
         * the loop's first, so the interpolation closes the circle instead of reading whatever
         * the ring holds after it. */
        const int next_offset = o + 1 >= length ? 0 : o + 1;

        int a = start + o;
        a = a >= ring_frames ? a - ring_frames : a;
        int b = start + next_offset;
        b = b >= ring_frames ? b - ring_frames : b;

        const float t = (float)f * kFracScale;
        const float al = ring[2 * a + 0];
        const float ar = ring[2 * a + 1];
        out[2 * i + 0] = al + (ring[2 * b + 0] - al) * t;
        out[2 * i + 1] = ar + (ring[2 * b + 1] - ar) * t;

        /* Advance. The carry out of the fraction is one more frame; the step is under `length`,
         * so one subtract is enough to wrap. */
        const uint32_t sum = f + step_frac;
        o += step_int + (sum < f ? 1 : 0);
        f = sum;
        o = o >= length ? o - length : o;
    }

    *offset = o;
    *frac = f;
}
