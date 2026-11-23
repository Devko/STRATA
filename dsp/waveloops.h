/* The WAVE machines' oscillator loop, as plain C for TI's cl6x (waveloops.ti.c).
 *
 * One wavetable oscillator reading two neighbouring frames of dsp/wavetable.hpp's library at one
 * mip level, straight from the int16 samples: two linear interpolations and a crossfade a sample.
 * PolyWerk reads its 16-bit frames the same way while the position moves. Reading int16 rather
 * than a float copy means a moving position costs nothing extra -- there is no copy to refill.
 *
 * The crossfade gains carry the frames' scales, the position's morph and the oscillator's level,
 * and ramp linearly across the block so a moving position never steps.
 *
 * <stdint.h> types throughout, for the reason dsp/grainloops.h gives: on tic6x uint32_t is not
 * `unsigned int`. */
#ifndef STRATA_WAVELOOPS_H
#define STRATA_WAVELOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bend WRP applies to the phase, Casio CZ style: the first half of the cycle is read over
 * [0, knee) and the second over [knee, 1). knee 0.5 with k1 = k2 = 1 is no bend. k1 = 0.5 / knee
 * and k2 = 0.5 / (1 - knee), worked out at control rate so the loop never divides. */
typedef struct {
    float knee;
    float k1;
    float k2;
} strata_wave_warp;

/* out[i] += ga * a(phase) + gb * b(phase), ga and gb stepping by dga and dgb each sample.
 * `a` and `b` are one mip level of two frames, 2^bits samples plus a guard sample each. `phase`
 * is a 32-bit fraction of a turn, left where the block ends. n > 0. */
void strata_wave_add(float *out, int n, const int16_t *a, const int16_t *b,
                     int bits, uint32_t *phase, uint32_t inc, float ga, float gb, float dga, float dgb);

/* The same through WRP's phase bend. A separate loop so the unbent one stays the cheap one. */
void strata_wave_add_warp(float *out, int n, const int16_t *a,
                          const int16_t *b, int bits, uint32_t *phase, uint32_t inc,
                          float ga, float gb, float dga, float dgb, const strata_wave_warp *warp);

#ifdef __cplusplus
}
#endif

#endif
