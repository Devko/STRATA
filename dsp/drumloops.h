/* The drum machines' per-sample loops, as plain C for TI's cl6x (drumloops.ti.c).
 *
 * CLAUDE.md: hot inner loops go in *.ti.c, which build_app.py sends to cl6x while everything
 * else goes through GCC. These are the loops the three machines' own cost comments named as the
 * candidates -- KICK's body shaper and click, SNARE's tuned pair, HAT's square bank and VCA --
 * plus the declick ramp all three share. Each one is the body of what was a loop in kick.cpp,
 * snare.cpp or hat.cpp, moved unchanged: same expressions, same order, same enaga math
 * (<enaga/dsp/math.h>, which is inline C and so identical from either compiler). Anything
 * already an Enaga block function (Svf::process, Envelope::render, OnePole::process, the
 * enaga_* buffer calls) is left alone -- those are in Enaga's own cl6x-built library already.
 *
 * State the C++ carries between blocks is passed by pointer and written back, so a voice's
 * phase, slew and RNG stream continue exactly as they did.
 *
 * Buffers never overlap unless a function says it works in place. n is a whole block or a
 * slice of one; nothing here divides or branches. */
#ifndef STRATA_DRUMLOOPS_H
#define STRATA_DRUMLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- KICK ---- */

/* The body: the FM envelope's decay through the `hold` samples whose phase is pinned (where the
 * shaper reads 0, so the caller has cleared them), then the phase accumulator and Plaits'
 * distorted sine into out[hold..n). *phase and *fm_env carry on from the last block. */
void strata_kick_body(float *out, int hold, int n, float f0_norm, float fm_amount,
                      float fm_decay, float dirtiness, float *phase, float *fm_env);

/* The click: a gate high for the first `gate` samples, slewed 0.5 rising / 0.1 falling, less its
 * one-pole high-pass. *slew and *high_pass carry on from the last block. */
void strata_kick_click(float *out, int n, int gate, float *slew, float *high_pass);

/* ---- SNARE ---- */

/* The tuned pair: two phase accumulators through the rational soft sine, with the reference's
 * -0.1 offset and its 0.60 / 0.25 weights. *phase_a and *phase_b carry on from the last block. */
void strata_snare_pair(float *out, int n, float dp_a, float dp_b, float *phase_a, float *phase_b);

/* ---- HAT ---- */

#define STRATA_HAT_OSCILLATORS 6

/* The metallic source: six squares read off the top bit of a 32-bit accumulator, mixed as
 * 0.33 * (how many are high) - 1, crossfaded towards clocked sample-and-hold noise by
 * `noisiness`. phase[] (STRATA_HAT_OSCILLATORS of them), *hold_phase, *hold and *rng all carry
 * on from the last block; the RNG advances only on a sample-and-hold clock edge, as the
 * machine's own loop did, so the noise stream is unchanged. *rng must be non-zero. */
void strata_hat_squares(float *out, int n, uint32_t *phase, const uint32_t *step,
                        uint32_t sample_hold_step, uint32_t *hold_phase, float *hold,
                        uint32_t *rng, float noisiness);

/* The VCA, in place: the envelope times the trigger level, then asymmetric (x4 positive, x0.1
 * negative) through the soft clip. */
void strata_hat_vca(float *buf, const float *env, int n, float trigger_level);

/* ---- shared ---- */

/* out += gain * in, gain walking by step each sample: the ramp a stolen voice fades out with.
 * All three machines' steal path, which is why it lives here once. */
void strata_mix_ramp(float *out, const float *in, int n, float gain, float step);

#ifdef __cplusplus
}
#endif

#endif
