/* GRAIN's per-sample loops, as plain C for TI's cl6x (grainloops.ti.c).
 *
 * CLAUDE.md: hot inner loops go in *.ti.c, which build_app.py sends to cl6x while everything else
 * goes through GCC. GRAIN is the one machine where that routing is not an optimisation but the
 * budget: docs/BUDGET.md prices a grain at 2,690 cycles a block and dsp/grain.hpp runs eight of
 * them, so this loop is 21,520 cycles -- 4.3% of the block -- and cl6x is worth about 2.2x on it.
 * Nothing here branches or divides.
 *
 * Two loops rather than one, mono and stereo, for the reason dsp/sample.cpp templates its
 * generate(): the channel count is fixed for the whole grain, so making it a parameter would put
 * a test in the inner loop and stop a mono source's loop from being the cheap one.
 *
 * The types are <stdint.h>'s and never bare `unsigned int`, for a reason the host build cannot
 * show: on tic6x uint32_t is `long unsigned int`, a DIFFERENT type from `unsigned int` even
 * though both are 32 bits, so a bare `unsigned int` here is a compile error at every call site on
 * the device and silently fine on x86. dsp/repeatloops.h, dsp/chainloops.h and dsp/drumloops.h
 * take <stdint.h> for the same reason.
 */
#ifndef STRATA_GRAINLOOPS_H
#define STRATA_GRAINLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the read position's fixed point ----
 *
 * 16 fractional bits, not the 24 dsp/sample.cpp uses, and the difference is a correctness
 * property rather than a tuning choice. The grain's length is bounded BEFORE the loop runs by
 * strata_grain_advance below, which must reproduce the loop's accumulation exactly; with 16 bits
 * that check is `samples * fstep` against a ceiling of 2^31, which fits in 32 bits for every
 * length this machine allows, so the bound needs no 64-bit arithmetic. A 64-bit division or
 * modulo does not even link on this target and a 64-bit multiply would cost more than the pitch
 * resolution is worth: 1/65536 of a frame is 0.026 cents at unity, which is inaudible. */
#define STRATA_GRAIN_FRAC_BITS 16
#define STRATA_GRAIN_FRAC_ONE 65536
#define STRATA_GRAIN_FRAC_MASK 0xFFFFu

/* The longest grain, in OUTPUT samples. This is what keeps strata_grain_advance exact in 32
 * bits: (STRATA_GRAIN_LEN_MAX - 1) * STRATA_GRAIN_FRAC_MASK = 32767 * 65535 = 2,147,385,345,
 * just under 2^31. Raising it past 32768 breaks the bound silently, which on this chip means
 * reads outside the sample buffer that do not fault -- they return garbage. dsp/grain.cpp
 * static_asserts the arithmetic so that a later raise fails the build instead. */
#define STRATA_GRAIN_LEN_MAX 32768

/* The grain window table's size. The caller passes STRATA_GRAIN_WINDOW + 1 floats: one cycle of
 * the window plus a guard entry equal to the first, so the linear read of index and index+1
 * never leaves the table. It is a macro and not a parameter so that cl6x can fold the clamp's
 * constants and keep the loop pipelined. */
#define STRATA_GRAIN_WINDOW 256

/* One grain's per-sample state, a plain struct so that this C and dsp/grain.cpp's C++ work on
 * the same bytes -- the convention <enaga/dsp.h> uses for enaga_oscillator and the rest.
 *
 * `pos` is an absolute frame index into the sample, not an offset from the grain's start, because
 * then the loop's load address is one add rather than two. `peak`, `rise` and `fall` warp the
 * window phase (see grainloops.ti.c) and are precomputed by the caller, who may divide; this loop
 * may not. */
typedef struct {
    int32_t pos;      /* the source frame being read, absolute */
    uint32_t frac;    /* its fraction of a frame, Q16 */
    int32_t istep;    /* the read step's whole frames */
    uint32_t fstep;   /* the read step's fraction, Q16 */
    float q;          /* the window phase: 0 at the grain's first sample, 1 at its end */
    float dq;         /* its step, 1 / length, so the loop never divides */
    float peak;       /* where the window's peak sits in q, 0.02 to 0.5 */
    float rise;       /* 0.5 / peak */
    float fall;       /* 0.5 / (1 - peak) */
    float gain;       /* the grain's constant gain: the velocity of the note that spawned it */
} strata_grain;

/* The frames the read position moves over `samples` samples, EXACTLY as the loops below
 * accumulate it from frac = 0: samples * istep + floor(samples * fstep / 2^16).
 *
 * This is the whole of GRAIN's bounds policy and the reason it lives beside the loop instead of
 * in grain.cpp: the bound and the loop have to be the same arithmetic, and an estimate is not
 * good enough. enaga_reciprocal is accurate to about 2e-7, which is fine for audio and not fine
 * for deciding how many samples it is safe to read -- "never read outside the buffer" is not a
 * 2e-7 kind of rule (docs/ENAGA_API.md, "Gotchas"). dsp/grain.cpp uses a float estimate only to
 * choose where to start checking, then settles it here.
 *
 * Preconditions, all enforced by dsp/grain.cpp and static_asserted there: 0 <= samples <
 * STRATA_GRAIN_LEN_MAX, 0 <= istep <= 32, fstep <= STRATA_GRAIN_FRAC_MASK. Inside those there is
 * no overflow and no division. */
int32_t strata_grain_advance(int32_t samples, int32_t istep, uint32_t fstep);

/* Add `n` samples of one grain into out[0..n), reading `src` and windowing with
 * `window[0..STRATA_GRAIN_WINDOW]`. ADDS, like everything else in dsp/: the grains of a cloud
 * accumulate in one scratch buffer and the voice's envelope is applied to the sum afterwards.
 * `g` carries on from the last call, so a grain crosses block boundaries unchanged.
 *
 * The caller guarantees every read is inside the buffer: `n` must not take the position past the
 * length strata_grain_advance sanctioned at the grain's onset. Nothing in here re-checks that,
 * which is the point -- a per-sample clamp on the tap index would cost two selects per tap per
 * grain, 1,024 of them a block at eight grains, to re-prove something the onset already settled
 * exactly.
 *
 * `src` is int16 and the 1/32768 (with the stereo halving) is folded into the caller's output
 * gain, so it costs one multiply a block rather than one a sample. */
void strata_grain_add_mono(float *out, int n, const int16_t *src, const float *window,
                           strata_grain *g);

/* The same, for a two-channel source: the pair is summed as integers so that a stereo grain
 * still pays one int-to-float convert per tap, and the halving rides on the output gain. */
void strata_grain_add_stereo(float *out, int n, const int16_t *src, const float *window,
                             strata_grain *g);

#ifdef __cplusplus
}
#endif

#endif
