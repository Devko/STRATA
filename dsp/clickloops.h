/* The metronome click's per-sample loop, as plain C for TI's cl6x (clickloops.ti.c).
 *
 * A recursive sine (y[n] = coef y[n-1] - y[n-2]) under an exponential envelope, added to both
 * channels of an interleaved stereo block. dsp/click.hpp says when it runs: about 30 ms a beat,
 * nothing in between. Nothing branches, nothing divides, nothing allocates. */
#ifndef STRATA_CLICKLOOPS_H
#define STRATA_CLICKLOOPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Add n frames of the click into out (L R L R). s1 and s2 are the oscillator's last two
 * outputs, carried between blocks by pointer; the envelope `env` multiplies each sample and then
 * decays by `decay`. Returns the envelope the next block starts from. */
float strata_click_add(float *out, int n, float *s1, float *s2, float coef, float env, float decay);

#ifdef __cplusplus
}
#endif

#endif
