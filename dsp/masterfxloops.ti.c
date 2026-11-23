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

static float triangle(float p) { return p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p; }

static float tap(const float *ring, int mask, int write, float delay, int channel) {
    const int whole = (int)delay;
    const float frac = delay - (float)whole;
    const int a = (write - whole) & mask;
    const int b = (write - whole - 1) & mask;
    return ring[2 * a + channel] * (1.0f - frac) + ring[2 * b + channel] * frac;
}

void strata_fx_flanger(float *restrict io, int n, float *restrict ring, int mask, int *write,
                       float *lfo, float lfo_step, float base, float span, float feedback,
                       float dry, float wet) {
    int w = *write;
    float p = *lfo;
    int i;
    if (n <= 0) return;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        const float l = io[2 * i];
        const float r = io[2 * i + 1];
        float pr;
        float dl;
        float dr;
        float el;
        float er;
        p += lfo_step;
        p -= p >= 1.0f ? 1.0f : 0.0f;
        pr = p + 0.25f;
        pr -= pr >= 1.0f ? 1.0f : 0.0f;
        dl = base + span * triangle(p);
        dr = base + span * triangle(pr);
        el = tap(ring, mask, w, dl, 0);
        er = tap(ring, mask, w, dr, 1);
        ring[2 * w] = l + feedback * el;
        ring[2 * w + 1] = r + feedback * er;
        w = (w + 1) & mask;
        io[2 * i] = dry * l + wet * el;
        io[2 * i + 1] = dry * r + wet * er;
    }
    *write = w;
    *lfo = p;
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
