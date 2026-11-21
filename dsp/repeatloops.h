/* Beat repeat's per-sample loops, as plain C for TI's cl6x (repeatloops.ti.c).
 *
 * Two loops, both on the master's interleaved stereo output and both running on every block the
 * effect is in: the capture whenever it is idle, the playback whenever it is held. That is the
 * same "fixed per-block tax" argument masterloops.h makes, which is why they are here rather
 * than in repeat.cpp.
 *
 * No branches and no division. The wraps are compare-and-subtract rather than a modulo, which
 * matters twice over: this chip has no integer divide instruction at all, and a 64-bit modulo
 * does not even LINK on it (see the upstream Enaga fix noted in docs/HANDOFF.md). The read
 * position is therefore a 32-bit frame offset beside a Q32 fraction, never one 64-bit word. */
#ifndef STRATA_REPEATLOOPS_H
#define STRATA_REPEATLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `n` interleaved stereo frames from `in` into the ring at `write`, wrapping. Returns the
 * new write index. `ring_frames` is the ring's length in FRAMES; the ring itself is
 * 2 * ring_frames floats. */
int strata_repeat_capture(float *ring, int ring_frames, int write, const float *in, int n);

/* Read `n` interleaved stereo frames out of the loop that starts at frame `start` and is
 * `length` frames long, interpolating linearly, and advance the position.
 *
 * The types are uint32_t rather than `unsigned int` for a reason the host build cannot show:
 * on tic6x uint32_t is `long unsigned int`, a DIFFERENT type from `unsigned int` even though both
 * are 32 bits, so a bare `unsigned int` here is a compile error at every call site on the device
 * and silently fine on x86. dsp/chainloops.h and dsp/drumloops.h take <stdint.h> for the same
 * reason.
 *
 * `offset` is the read position inside the loop, in frames, and `frac` its Q32 fraction; both
 * are updated. The step is `step_int` frames plus `step_frac` in Q32 -- so 1.0 is (1, 0) and a
 * half-rate octave down is (0, 0x80000000).
 *
 * The step MUST be less than `length`, which it always is in practice: the slowest loop is a
 * thirty-second note and the fastest step is one frame. That is what lets the wrap be a single
 * compare-and-subtract instead of a modulo. */
void strata_repeat_play(const float *ring, int ring_frames, int start, int length, int *offset,
                        uint32_t *frac, int step_int, uint32_t step_frac, float *out, int n);

#ifdef __cplusplus
}
#endif

#endif
