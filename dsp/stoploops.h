/* Tape stop and delay freeze: their per-sample loops, as plain C for TI's cl6x (stoploops.ti.c).
 *
 * Both run only while their effect is held or fading out, so neither is a per-block tax on an
 * idle box (dsp/tapestop.hpp, dsp/freeze.hpp). No branches and no division: every wrap and clamp
 * is a ?:, and the read positions are a frame index beside a float fraction. */
#ifndef STRATA_STOPLOOPS_H
#define STRATA_STOPLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tape stop, on audio()'s interleaved stereo buffer, in place.
 *
 * Each frame is written into `ring` (interleaved, `ring_frames` frames) at *write, then read back
 * `*lag` frames behind the newest write, linearly interpolated. The read speed is `*speed`
 * (1 = the live speed, 0 = stopped), so `*lag` grows by 1 - speed a frame, and `*speed` falls by
 * `speed_step` a frame down to 0. The read is faded by min(1, 4 * speed) -- a real tape loses
 * its level with its speed, and at a standstill it is silent rather than a held sample -- and
 * crossfaded with the live input by `*wet`, which moves by `wet_step` a frame within 0-1.
 *
 * `*lag` is clamped to ring_frames - 2, which only matters once the tape has stopped (silent).
 * At speed 1 and lag 0 the read is the frame just written, so the press itself is seamless. */
void strata_tape_stop(float *io, int n, float *ring, int ring_frames, int *write, float *lag,
                      float *speed, float speed_step, float *wet, float wet_step);

/* Copy `n` frames of the two planes into `ring` (interleaved) from frame `at`. The caller keeps
 * at + n within the ring. */
void strata_freeze_capture(float *ring, int at, const float *left, const float *right, int n);

/* Play the frozen loop -- `length` frames of `ring` from frame *read, wrapping -- over the two
 * planes: out = out + (loop - out) * g, with g stepping by `g_step` a frame from `g` and clamped
 * to 0-1. g = 1 throughout is the loop alone; a falling g crossfades back to what was in `out`. */
void strata_freeze_play(const float *ring, int length, int *read, float *left, float *right,
                        int n, float g, float g_step);

#ifdef __cplusplus
}
#endif

#endif
