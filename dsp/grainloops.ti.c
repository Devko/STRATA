/* GRAIN's per-sample loops (grainloops.h). Built by cl6x when it is installed, else GCC --
 * build_app.py routes *.ti.c to TI's compiler (docs/BUDGET.md, M2).
 *
 * Four things happen per sample per grain, and the shape of each one is a decision dsp/grain.hpp
 * argues for. In short:
 *
 *   PCM read     two int16 taps and a linear blend, not the four taps and Catmull-Rom that
 *                dsp/sample.cpp uses. Eight grains would make that 32 int16 loads an output
 *                sample, and sample.cpp's own cost note says that machine is already memory-bound
 *                on four. A grain is a few milliseconds under a window that is zero at both ends,
 *                so what linear interpolation gives away is spread under a window whose own
 *                spectrum is wider than the error, eight times over.
 *   window       one table read, linearly interpolated, off a phase that the caller warps so the
 *                peak can sit anywhere from the middle (a symmetric Hann grain) to the very
 *                start (a plucked one). Warping the phase rather than holding a table per shape
 *                costs two flops and a select, and because Hann's slope is zero at its peak the
 *                kink the warp puts in the phase does not reach the window.
 *   VCA          the grain's own gain, which is the velocity of the note that spawned it. The
 *                voice envelope and the level are applied to the SUM of the grains afterwards, so
 *                they cost one pass a block instead of one per grain.
 *   advance      a 32.16 fixed-point add with the carry folded into the frame index. No division
 *                anywhere: this core has no integer divide instruction at all, and a float
 *                division under cl6x is a call, which stops a loop pipelining.
 *
 * Nothing here checks a bound. The grain's length was settled exactly at its onset by
 * strata_grain_advance, which is the same arithmetic as the advance below -- that is why the two
 * sit in one file. An out-of-range read on this chip does not fault, it silently returns garbage,
 * so if those two ever disagree the symptom is a crackle nobody can trace. */
#include "grainloops.h"

#include <enaga/dsp/math.h>

/* Q16 to float, and the window phase to a table index. Both constants, so cl6x keeps them in
 * registers across the loop. */
#define STRATA_GRAIN_INV_FRAC (1.0f / (float)STRATA_GRAIN_FRAC_ONE)
#define STRATA_GRAIN_WINDOW_SCALE ((float)STRATA_GRAIN_WINDOW)

int32_t strata_grain_advance(int32_t samples, int32_t istep, uint32_t fstep) {
    /* The loop below adds fstep into a 16-bit fraction once a sample and carries the overflow
     * into the frame index, so after `samples` samples the index has moved by
     * samples * istep + floor(samples * fstep / 2^16) -- this, with no rounding of any kind.
     * grainloops.h's preconditions keep both products inside 32 bits. */
    const uint32_t j = (uint32_t)samples;
    return samples * istep + (int32_t)((j * fstep) >> STRATA_GRAIN_FRAC_BITS);
}

/* The window, as both loops read it: the phase warp, the clamp that bounds the table read, and
 * the linear interpolation. `static inline` and branch-free, so each loop gets its own copy with
 * the constants folded -- the same way drumloops.ti.c shares distorted_sine and soft_sine.
 *
 * The phase warp is what SHP does. q runs 0 to 1 over the grain; `peak` is where the window's
 * peak should sit; the two linear segments map [0, peak) onto [0, 0.5) and [peak, 1] onto
 * [0.5, 1], so the table is always read over its whole span and the grain always starts and ends
 * at zero whatever the shape. The kink the warp leaves at q = peak does not reach the output,
 * because a Hann window's slope at its peak is zero: that is the reason the table is Hann and the
 * reason the warp is in the phase rather than in a second table.
 *
 * The clamp is the table's bound, not defensive padding. q is a float accumulator stepped up to
 * 32,768 times, so its error can reach a few thousandths by the end of a long grain; one select
 * each side turns that into a read of the guard entry instead of a read past the table. Clamping
 * to the table's SIZE rather than to one below it is deliberate: at exactly 256 the index
 * saturates to 255 and the fraction becomes 1, which reads the guard entry and so gives the
 * window's final zero, rather than parking on the second-to-last table value. */
static inline float strata_grain_window(const float *window, float q, float peak, float rise,
                                        float fall) {
    const float warped = q < peak ? q * rise : 0.5f + (q - peak) * fall;
    const float x = enaga_clamp(warped * STRATA_GRAIN_WINDOW_SCALE, 0.0f,
                                STRATA_GRAIN_WINDOW_SCALE);
    int32_t k = (int32_t)x;
    k = k > STRATA_GRAIN_WINDOW - 1 ? STRATA_GRAIN_WINDOW - 1 : k;
    const float w0 = window[k];
    return w0 + (window[k + 1] - w0) * (x - (float)k);
}

void strata_grain_add_mono(float *restrict out, int n, const int16_t *restrict src,
                           const float *restrict window, strata_grain *restrict g) {
    int32_t pos = g->pos;
    uint32_t frac = g->frac;
    float q = g->q;
    const int32_t istep = g->istep;
    const uint32_t fstep = g->fstep;
    const float dq = g->dq;
    const float peak = g->peak;
    const float rise = g->rise;
    const float fall = g->fall;
    const float gain = g->gain;
    int i;

    for (i = 0; i < n; i++) {
        /* frac is under 2^16, so the signed cast is value-preserving and is the cheap convert on
         * this core -- the same one dsp/sample.cpp's generate() uses. */
        const float t = (float)(int32_t)frac * STRATA_GRAIN_INV_FRAC;
        const float a = (float)src[pos];
        const float b = (float)src[pos + 1];
        const float s = a + (b - a) * t;

        out[i] += gain * strata_grain_window(window, q, peak, rise, fall) * s;

        const uint32_t sum = frac + fstep;
        pos += istep + (int32_t)(sum >> STRATA_GRAIN_FRAC_BITS);
        frac = sum & STRATA_GRAIN_FRAC_MASK;
        q += dq;
    }

    g->pos = pos;
    g->frac = frac;
    g->q = q;
}

void strata_grain_add_stereo(float *restrict out, int n, const int16_t *restrict src,
                             const float *restrict window, strata_grain *restrict g) {
    int32_t pos = g->pos;
    uint32_t frac = g->frac;
    float q = g->q;
    const int32_t istep = g->istep;
    const uint32_t fstep = g->fstep;
    const float dq = g->dq;
    const float peak = g->peak;
    const float rise = g->rise;
    const float fall = g->fall;
    const float gain = g->gain;
    int i;

    for (i = 0; i < n; i++) {
        const float t = (float)(int32_t)frac * STRATA_GRAIN_INV_FRAC;
        /* The pair summed as integers, so a stereo grain still costs one int-to-float convert
         * per tap; the halving rides on the caller's output gain. */
        const int32_t o = pos + pos;
        const float a = (float)((int32_t)src[o] + (int32_t)src[o + 1]);
        const float b = (float)((int32_t)src[o + 2] + (int32_t)src[o + 3]);
        const float s = a + (b - a) * t;

        out[i] += gain * strata_grain_window(window, q, peak, rise, fall) * s;

        const uint32_t sum = frac + fstep;
        pos += istep + (int32_t)(sum >> STRATA_GRAIN_FRAC_BITS);
        frac = sum & STRATA_GRAIN_FRAC_MASK;
        q += dq;
    }

    g->pos = pos;
    g->frac = frac;
    g->q = q;
}
