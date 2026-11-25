/* The performance mixer's master effects (masterfxloops.h). Built by cl6x when it is installed,
 * else GCC. */
#include "masterfxloops.h"

#include "tihints.h"

/* Rounded to the nearest step, and held inside +-1: a step that does not divide 1 would round a
 * full-scale sample past the limiter's ceiling. */
static float quantize(float x, float step, float inv_step) {
    const float scaled = x * inv_step + (x >= 0.0f ? 0.5f : -0.5f);
    const float q = (float)(int32_t)scaled * step;
    return q > 1.0f ? 1.0f : (q < -1.0f ? -1.0f : q);
}

void strata_fx_crush(float *restrict io, int n, float rate, float step, float inv_step, float wet,
                     float *phase, float *held) {
    float p = *phase;
    float hl = held[0];
    float hr = held[1];
    int i;
    if (n <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float l = io[2 * i];
        const float r = io[2 * i + 1];
        int take;
        p += rate;
        take = p >= 1.0f;
        p -= take ? 1.0f : 0.0f;
        hl = take ? quantize(l, step, inv_step) : hl;
        hr = take ? quantize(r, step, inv_step) : hr;
        io[2 * i] = l + wet * (hl - l);
        io[2 * i + 1] = r + wet * (hr - r);
    }
    *phase = p;
    held[0] = hl;
    held[1] = hr;
}

void strata_fx_gain(float *restrict io, int n, float *phase, float step, float pump, float gate,
                    float smooth, float *gate_state) {
    float p = *phase;
    float g = *gate_state;
    int i;
    if (n <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        float sixteenth;
        float fall;
        float gain;
        p += step;
        p -= p >= 1.0f ? 1.0f : 0.0f;
        /* Four sixteenths a beat: the second half of each is closed. */
        sixteenth = 4.0f * p;
        sixteenth -= (float)(int32_t)sixteenth;
        g += smooth * ((sixteenth < 0.5f ? 1.0f : 1.0f - gate) - g);
        /* The pump: down at the beat, back up over it, squared so most of the beat is open. */
        fall = 1.0f - p;
        gain = g * (1.0f - pump * fall * fall);
        io[2 * i] *= gain;
        io[2 * i + 1] *= gain;
    }
    *phase = p;
    *gate_state = g;
}

void strata_fx_noise(float *restrict io, int n, uint32_t *seed, float k, float *lp, float level,
                     float keep) {
    uint32_t s = *seed;
    float low = *lp;
    int i;
    if (n <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        float white;
        float high;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        /* The top 24 bits as a signed value, to [-1, 1). */
        white = (float)((int32_t)s >> 8) * (1.0f / 8388608.0f);
        low += k * (white - low);
        high = (white - low) * level;
        io[2 * i] = io[2 * i] * keep + high;
        io[2 * i + 1] = io[2 * i + 1] * keep + high;
    }
    *seed = s;
    *lp = low;
}
