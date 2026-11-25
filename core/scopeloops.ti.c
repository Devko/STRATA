/* scopeloops.h: the scope tap's decimation. */
#include "scopeloops.h"

static int16_t scope_to_int16(float x) {
    float s = x * 32767.0f;
    s = s > 32767.0f ? 32767.0f : s;
    s = s < -32768.0f ? -32768.0f : s;
    return (int16_t)s;
}

void strata_scope_decimate(const float *in, int n, int16_t *left, int16_t *right) {
    int i;
    for (i = 0; i < n; i++) {
        left[i] = scope_to_int16(in[4 * i]);
        right[i] = scope_to_int16(in[4 * i + 1]);
    }
}
