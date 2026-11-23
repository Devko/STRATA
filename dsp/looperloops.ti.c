/* The master looper's loops (looperloops.h). Built by cl6x when it is installed, else GCC. */
#include "looperloops.h"

#include "tihints.h"

void strata_loop_record(int16_t *restrict dst, const float *restrict src, int n) {
    int i;
    if (n <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float x = src[i] * 32767.0f;
        const float c = x > 32767.0f ? 32767.0f : (x < -32767.0f ? -32767.0f : x);
        dst[i] = (int16_t)c;
    }
}

void strata_loop_mix(float *restrict io, const int16_t *restrict loop, int frames, float mix,
                     float step) {
    const float k = 1.0f / 32768.0f;
    int i;
    if (frames <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < frames; i++) {
        const float m = mix + step * (float)i;
        const float l = io[2 * i];
        const float r = io[2 * i + 1];
        io[2 * i] = l + m * ((float)loop[2 * i] * k - l);
        io[2 * i + 1] = r + m * ((float)loop[2 * i + 1] * k - r);
    }
}
