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

#include <enaga/dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- KICK ---- */

/* The body: the FM envelope's decay through the `hold` samples whose phase is pinned (where the
 * shaper reads 0, so the caller has cleared them), then the phase accumulator and Plaits'
 * distorted sine into out[hold..n). *phase and *fm_env carry on from the last block. */
void strata_kick_body(float *out, int hold, int n, float f0_norm, float fm_amount,
                      float fm_decay, float dirtiness, float *phase, float *fm_env);

/* The body with KNK, the knock (kick.cpp): a second, much faster pitch envelope on top of the
 * FM one, the increment f0 * (1 + 3.5 * fm_amount * fm_env + knock_amount * knock_env), still
 * capped at 0.5. A loop of its own so that a kick with KNK at 0 runs strata_kick_body exactly as
 * before, bit for bit and cycle for cycle. *knock_env carries on from the last block. */
void strata_kick_body_knock(float *out, int hold, int n, float f0_norm, float fm_amount,
                            float fm_decay, float knock_amount, float knock_decay,
                            float dirtiness, float *phase, float *fm_env, float *knock_env);

/* TCK, the tick: src (high-passed noise) times an exponential envelope, added into out.
 * *env carries on from the last block. */
void strata_kick_tick(float *out, const float *src, int n, float gain, float decay, float *env);

/* The rumble (kick.cpp, TAIL): the kick's own output through a one-pole low-pass, two
 * all-passes (the smear) and two parallel feedback combs (the tail), soft-clipped at `drive`
 * (gain into enaga_soft_clip, its output scaled back by `makeup`) and ducked: the gain is
 * 1 - *duck, and *duck falls by `release` a sample, so each hit pulls the rumble under it and it
 * swells back between hits. Writes out[0..n) (does not add). Delay lines are circular, their
 * positions carried by pointer. */
void strata_kick_rumble(float *out, const float *in, int n, float lp_coef, float *lp,
                        float *ap1, int ap1_len, int *ap1_pos, float *ap2, int ap2_len,
                        int *ap2_pos, float *c1, int c1_len, int *c1_pos, float *c2, int c2_len,
                        int *c2_pos, float feedback, float drive, float makeup, float *duck,
                        float release);

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

/* The hat's back half in ONE loop: the band-pass, the VCA (strata_hat_vca) and the high-pass, as
 *
 *     strata_svf_inplace(band, band_c, ENAGA_SVF_BAND, buf, n)
 *     strata_hat_vca(buf, env, n, trigger_level)
 *     strata_svf_inplace(high, high_c, ENAGA_SVF_HIGH, buf, n)
 *
 * ran it as three passes. Each filter's state is a recurrence of about 21 cycles a sample on
 * cl6x, and two of them in one loop overlap. Same steps, same order: bit-exact (test_drums.cpp). */
void strata_hat_back(float *buf, const float *env, int n, enaga_svf *band,
                     const enaga_svf_coefficients *band_c, enaga_svf *high,
                     const enaga_svf_coefficients *high_c, float trigger_level);

/* ---- shared ---- */

/* Steps the xorshift generator *state n times without using the numbers: what enaga_noise_add
 * would leave in *state after n samples. */
void strata_rng_skip(uint32_t *state, int n);


/* out += gain * in, gain walking by step each sample: the ramp a stolen voice fades out with.
 * All three machines' steal path, which is why it lives here once. */
void strata_mix_ramp(float *out, const float *in, int n, float gain, float step);

#ifdef __cplusplus
}
#endif

#endif
