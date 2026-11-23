/* The WAVE oscillator's loops (waveloops.h). Plain C, no branches, no division. */
#include "waveloops.h"

#include "tihints.h"

void strata_wave_add(float *restrict out, int n, const int16_t *restrict a, const int16_t *restrict b,
                     int bits, uint32_t *phase, uint32_t inc, float ga, float gb, float dga, float dgb) {
    uint32_t ph = *phase;
    const int shift = 32 - bits;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const uint32_t idx = ph >> shift;
        /* The fraction below the index: its top 24 bits as a float in [0, 1). */
        const float fr = (float)((ph << bits) >> 8) * (1.0f / 16777216.0f);
        const float a0 = (float)a[idx], a1 = (float)a[idx + 1];
        const float b0 = (float)b[idx], b1 = (float)b[idx + 1];
        out[i] += ga * (a0 + (a1 - a0) * fr) + gb * (b0 + (b1 - b0) * fr);
        ga += dga;
        gb += dgb;
        ph += inc;
    }
    *phase = ph;
}

void strata_wave_add_warp(float *restrict out, int n, const int16_t *restrict a,
                          const int16_t *restrict b, int bits, uint32_t *phase, uint32_t inc,
                          float ga, float gb, float dga, float dgb, const strata_wave_warp *warp) {
    uint32_t ph = *phase;
    const float knee = warp->knee, k1 = warp->k1, k2 = warp->k2;
    const float len = (float)(1 << bits);
    const int last = (1 << bits) - 1;
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float p = (float)(ph >> 8) * (1.0f / 16777216.0f); /* [0, 1) exactly */
        const float q = p < knee ? p * k1 : 0.5f + (p - knee) * k2;
        const float pos = q * len;
        int idx = (int)pos;
        idx = idx > last ? last : idx; /* q can round up to 1 */
        {
            const float fr = pos - (float)idx;
            const float a0 = (float)a[idx], a1 = (float)a[idx + 1];
            const float b0 = (float)b[idx], b1 = (float)b[idx + 1];
            out[i] += ga * (a0 + (a1 - a0) * fr) + gb * (b0 + (b1 - b0) * fr);
        }
        ga += dga;
        gb += dgb;
        ph += inc;
    }
    *phase = ph;
}
