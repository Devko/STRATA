/* The STRING machine's loops, as plain C for TI's cl6x (pluckloops.ti.c).
 *
 * Two models, one loop each:
 *
 *   STRING  Karplus-Strong. A delay line one period long, read with linear interpolation for the
 *           fraction, through a one-pole low-pass and a loop gain, written back. The excitation
 *           is put into the line when the note starts (plucked.cpp), so the loop itself is only
 *           the recirculation.
 *   BAR     Four two-pole resonators at a struck bar's mode frequencies, driven by a short
 *           excitation burst. Marimba, vibes, kalimba, glass.
 *
 * Neither branches or divides. The delay line is a power of two long, so the read and the write
 * wrap with a mask.
 *
 * <stdint.h> types, for the reason dsp/grainloops.h gives. */
#ifndef STRATA_PLUCKLOOPS_H
#define STRATA_PLUCKLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STRATA_PLUCK_LINE 2048            /* samples: a period down to 23.4 Hz */
#define STRATA_PLUCK_MASK (STRATA_PLUCK_LINE - 1)
#define STRATA_BAR_MODES 4

typedef struct {
    float *line;          /* STRATA_PLUCK_LINE floats */
    uint32_t write;       /* the next sample written */
    float lp;             /* the loop filter's state */
} strata_pluck_state;

/* out[i] += level[i] * y, where y = gain * lowpass(line[write - delay]) is written back.
 * `delay` is the period in samples (2 <= delay < STRATA_PLUCK_LINE - 1), `coef` the one-pole's
 * coefficient (1 is no filtering). Returns the largest |y| of the slice, which says when the
 * string has died away. n > 0. */
float strata_pluck_run(float *out, int n, const float *level, strata_pluck_state *s, float delay,
                       float coef, float gain);

/* n samples of white noise through a one-pole low-pass of coefficient `coef` (1 is none), into
 * e. *rng is an xorshift32 state, left where it ends. The string's excitation, made once per note. */
void strata_pluck_noise(float *e, int n, uint32_t *rng, float coef);

/* line[(write - n + i) & mask] += gain * (e[i] - e[i - comb]) for i in 0..n-1, the second term
 * only once i >= comb: the excitation added into the period about to be read, with the
 * pick-position comb applied (comb >= 1). */
void strata_pluck_inject(float *line, uint32_t write, const float *e, int n, int comb, float gain);

typedef struct {
    float y1[STRATA_BAR_MODES];
    float y2[STRATA_BAR_MODES];
} strata_bar_state;

typedef struct {
    float a1[STRATA_BAR_MODES];   /* 2 r cos(w) */
    float a2[STRATA_BAR_MODES];   /* -r^2 */
    float g[STRATA_BAR_MODES];    /* each mode's input gain */
} strata_bar_coefs;

/* out[i] += level[i] * the four modes' sum, each driven by in[i]. Returns the largest |sum|. */
float strata_bar_run(float *out, int n, const float *in, const float *level, strata_bar_state *s,
                     const strata_bar_coefs *c);

#ifdef __cplusplus
}
#endif

#endif
