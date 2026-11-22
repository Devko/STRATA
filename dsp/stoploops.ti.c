/* Tape stop and delay freeze, per sample (stoploops.h). Built by cl6x when it is installed, else
 * GCC. Nothing here branches or divides. */
#include "stoploops.h"

void strata_tape_stop(float *restrict io, int n, float *restrict ring, int ring_frames,
                      int *restrict write, float *restrict lag, float *restrict speed,
                      float speed_step, float *restrict wet, float wet_step) {
    int w = *write;
    float l = *lag, s = *speed, g = *wet;
    const float max_lag = (float)(ring_frames - 2);
    int i;
    for (i = 0; i < n; i++) {
        const float x0 = io[2 * i + 0], x1 = io[2 * i + 1];
        ring[2 * w + 0] = x0;
        ring[2 * w + 1] = x1;

        /* lag frames behind the frame just written, and its older neighbour for the fraction. */
        const int whole = (int)l;
        const float frac = l - (float)whole;
        int a = w - whole;
        a = a < 0 ? a + ring_frames : a;
        int b = a - 1;
        b = b < 0 ? b + ring_frames : b;
        const float level = s * 4.0f > 1.0f ? 1.0f : s * 4.0f;
        const float y0 = (ring[2 * a + 0] + (ring[2 * b + 0] - ring[2 * a + 0]) * frac) * level;
        const float y1 = (ring[2 * a + 1] + (ring[2 * b + 1] - ring[2 * a + 1]) * frac) * level;
        io[2 * i + 0] = x0 + (y0 - x0) * g;
        io[2 * i + 1] = x1 + (y1 - x1) * g;

        w++;
        w = w >= ring_frames ? 0 : w;
        l += 1.0f - s;
        l = l > max_lag ? max_lag : l;
        s -= speed_step;
        s = s < 0.0f ? 0.0f : s;
        g += wet_step;
        g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
    }
    *write = w;
    *lag = l;
    *speed = s;
    *wet = g;
}

void strata_freeze_capture(float *restrict ring, int at, const float *restrict left,
                           const float *restrict right, int n) {
    int i;
    for (i = 0; i < n; i++) {
        ring[2 * (at + i) + 0] = left[i];
        ring[2 * (at + i) + 1] = right[i];
    }
}

void strata_freeze_play(const float *restrict ring, int length, int *restrict read,
                        float *restrict left, float *restrict right, int n, float g,
                        float g_step) {
    int r = *read;
    int i;
    for (i = 0; i < n; i++) {
        g += g_step;
        g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
        left[i] += (ring[2 * r + 0] - left[i]) * g;
        right[i] += (ring[2 * r + 1] - right[i]) * g;
        r++;
        r = r >= length ? 0 : r;
    }
    *read = r;
}
