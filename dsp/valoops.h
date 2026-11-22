/* The VA voice's output tail, as plain C for TI's cl6x (valoops.ti.c).
 *
 * dsp/va.cpp's header has named this extraction since M2: "a single fused pass doing soft clip
 * -> multiply by the amplitude envelope -> ramped add into the bus would replace three passes
 * over the slice with one and save two buffer round-trips". It is worth doing now because
 * docs/BUDGET.md measured VA POLY at 4.3 points per voice -- 11x what docs/SPEC.md budgets for
 * the whole machine -- and 82% of that is the per-sample loop. Six voices run this tail six
 * times a block, four slices each.
 *
 * Why a *.ti.c file rather than a loop in va.cpp: CLAUDE.md's rule, and the reason behind it --
 * cl6x pipelines a branch-free C loop about 2.2x better than GCC does (docs/BUDGET.md, M2), and
 * the 60% target depends on it. Everything else in the slice is already an Enaga block function
 * and so already goes through cl6x inside Enaga's own library; this was the one hand-written
 * part of the voice.
 *
 * The arithmetic is va.cpp's, unchanged, in the same order and the same associations, so the
 * voice is BIT-EXACT against the three passes it replaces -- which is what
 * tests/test_va.cpp's and tests/test_vapoly.cpp's existing expectations hold it to. */
#ifndef STRATA_VALOOPS_H
#define STRATA_VALOOPS_H

#include <enaga/dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The whole output tail of one control slice, in one pass:
 *
 *     out[i] += gain * (soft_clip(drive * work[i]) * env[i]);  gain += step;
 *
 * which is `enaga_soft_clip_buffer(work, n, drive)`, then `enaga_multiply(work, env, n)`, then
 * va.cpp's own `mix_ramp(out, work, n, gain, step)` -- three passes over the slice, with `work`
 * written and read back twice between them.
 *
 * `gain` steps AFTER each sample, exactly as `mix_ramp` did, so the first sample of a slice is
 * still at the gain the previous slice ended on. Getting that backwards would put a one-sample
 * step in the bus on every change of level, velocity or accent.
 *
 * `work` is not modified, which is the other half of the saving: the slice buffer is read once
 * instead of being round-tripped through memory twice. */
void strata_va_tail(float *out, const float *work, const float *env, int n, float drive,
                    float gain, float step);

/* The filter's modes, as strata_va_back takes them: VaMode's order. */
enum { STRATA_VA_LP24, STRATA_VA_LP12, STRATA_VA_BP12, STRATA_VA_HP12 };

/* The whole back half of a slice -- filter, amplitude envelope and output tail -- in ONE loop:
 *
 *     LP24:  enaga_svf_cascade(s1, c1, s2, c2, ENAGA_SVF_LOW, work, work, n)
 *     else:  enaga_svf_process(s2, c2, LOW / BAND / HIGH, work, work, n)
 *     then   enaga_envelope_render(amp, shape, env, n)
 *     then   strata_va_tail(out, work, env, n, drive, gain, step)
 *
 * which is what dsp/va.cpp ran as three passes. Each of those loops is bound by a recurrence
 * rather than by arithmetic -- the filter's state, the envelope's level, the gain ramp -- and on
 * the C674x a float add or multiply takes four cycles to come back, so each pass spent most of
 * its cycles waiting with the units idle. In one loop the three recurrences are independent and
 * overlap, so the slice costs about as much as the slowest of them instead of the sum (docs/
 * BUDGET.md, "The cl6x vectorisation pass"). The envelope's level never goes through memory.
 *
 * Bit-exact, by the same rule as the tail: the filter is Enaga's inline step, the envelope update
 * is enaga_envelope_render's loop body as it stands in Enaga's runtime/dsp/envelope.c (stage
 * flags carried as ints, branch-free) with its stage written back the same way, and the tail is
 * strata_va_tail's expression. tests/test_va.cpp holds it to `==` against the three Enaga calls
 * in every mode, every stage and across stage changes inside a slice, so a pin bump that changes
 * Enaga's envelope fails there rather than drifting.
 *
 * `work` holds the oscillators' sum and is only read. An idle envelope (not reachable:
 * VaVoice::run returns before a silent voice gets here) holds its level. */
void strata_va_back(float *out, const float *work, int n, int mode, enaga_svf *s1,
                    const enaga_svf_coefficients *c1, enaga_svf *s2,
                    const enaga_svf_coefficients *c2, enaga_envelope *amp,
                    const enaga_envelope_shape *shape, float drive, float gain, float step);

#ifdef __cplusplus
}
#endif

#endif
