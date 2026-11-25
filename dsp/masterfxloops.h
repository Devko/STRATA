/* The performance mixer's master effects (masterfxloops.ti.c), plain C for TI's cl6x.
 *
 * Each works in place on audio()'s interleaved stereo buffer (L R L R), `n` frames, and is
 * called only while its effect is up, so an idle mixer costs nothing (dsp/masterfx.hpp). No
 * branch in a loop body (?: only), no division, no libm: every coefficient arrives from the
 * control-rate side, and state is carried in and out through pointers. */
#ifndef STRATA_MASTERFXLOOPS_H
#define STRATA_MASTERFXLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bit and rate reduction. The input is sampled every 1 / `rate` frames (rate 0-1, held in
 * between: `*phase` carries the count and `held` the held pair), quantized to steps of `step`
 * (`inv_step` = 1 / step), and mixed in at `wet`. */
void strata_fx_crush(float *io, int n, float rate, float step, float inv_step, float wet,
                     float *phase, float *held);

/* The gain moves: a dip at every beat (`pump` 0-1 deep, recovering over the beat) times a
 * sixteenth-note chopper (`gate` 0-1 deep), its edges smoothed by a one-pole of coefficient
 * `smooth` carried in `*gate_state`. `*phase` is the place in the beat (0-1), advancing `step`
 * a frame. */
void strata_fx_gain(float *io, int n, float *phase, float step, float pump, float gate,
                    float smooth, float *gate_state);

/* White noise, high-passed by a one-pole of coefficient `k` (state `*lp`), added at `level`,
 * with the program scaled by `keep` so that the sum stays inside the limiter's ceiling.
 * `*seed` is an xorshift32 state, never 0. */
void strata_fx_noise(float *io, int n, uint32_t *seed, float k, float *lp, float level,
                     float keep);

#ifdef __cplusplus
}
#endif

#endif
