/* The insert chain's hand-written per-sample loops, as plain C for TI's cl6x (chainloops.ti.c).
 *
 * chain.cpp's own cost comment names exactly these three -- drive, lo-fi and the duck -- as the
 * loops CLAUDE.md's *.ti.c rule bites on: everything else in the chain is already an Enaga block
 * function (Svf::process, Svf::cascade, enaga_ramp_stereo_add, enaga_mix), which cl6x builds as
 * part of Enaga's own DSP library. Eight chains run every block, so these are eight passes each.
 *
 * Each function is the body of what was a loop in chain.cpp, moved with its arithmetic
 * untouched, through the same inline enaga math. State that carries between blocks (the lo-fi
 * sample-and-hold) arrives by pointer. Nothing branches, nothing divides, nothing allocates. */
#ifndef STRATA_CHAINLOOPS_H
#define STRATA_CHAINLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive, in place: an asymmetric soft clip, `negative` driving the negative half less than
 * `positive` drives the positive one, which is where the even harmonics come from -- and then a
 * DC blocker, because an asymmetric clip also shifts the wave off zero: by up to 7% of full
 * scale at middle settings on a sustained note. `dc` is the blocker's state (last input, last
 * output) and `pole` its coefficient (chain.cpp, kDcPole). */
void strata_chain_drive(float *mono, int n, float positive, float negative, float pole, float *dc);

/* Lo-fi, in place: the sample-and-hold whose clock is a 32-bit phase wrap, then the bit crush,
 * in one pass. `levels` is the number of quantisation steps and `step` its reciprocal (both
 * control rate, so the loop does not divide); `ceiling` bounds the held sample before it goes
 * through an int. *phase and *hold carry on from the last block. */
void strata_chain_lofi(float *mono, int n, uint32_t increment, float levels, float step,
                       float ceiling, uint32_t *phase, float *hold);

/* The sidechain duck, in place: the mixer's one curve for the whole block, at this track's
 * depth. One multiply-add a sample, which is the point of the mixer rendering the envelope once
 * for all eight tracks. */
void strata_chain_duck(float *mono, const float *duck, int n, float depth);

/* A choke group's fade (Engine::choke_group), in place over a track's block from where the
 * choke lands: `gain` steps down by `step` BEFORE each sample and stops at 0, so the voices it
 * cut go quiet over 1 / step frames instead of clicking. Returns the gain the next block starts
 * from; 0 means the fade is over. Runs only while a choke is fading. */
float strata_choke_fade(float *mono, int n, float gain, float step);

#ifdef __cplusplus
}
#endif

#endif
