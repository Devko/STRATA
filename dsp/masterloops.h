/* The master bus's per-sample loops, as plain C for TI's cl6x (masterloops.ti.c).
 *
 * These four are the fixed per-block tax: they run whether one track sounds or eight (the
 * detector and the interleave on every block, the ramp whenever the glue compressor or the
 * output level is not at unity, the limiter whenever the block can reach the threshold), which
 * is why they are worth cl6x even though each is only a few flops a sample. Everything else on
 * the master -- the DJ filter's two cascades, the delay and the reverb -- is already an Enaga
 * block function and so already goes through cl6x in Enaga's own library.
 *
 * Each function is the body of what was a loop in buses.cpp, moved with its arithmetic
 * untouched: the ramp still steps before the first sample, and the limiter is still unity below
 * the threshold, so a quiet master stays bit-exact. Nothing branches or divides. */
#ifndef STRATA_MASTERLOOPS_H
#define STRATA_MASTERLOOPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The block's peak across both channels: the glue compressor's detector and the limiter's skip
 * test, which is why it is one pass and not two. */
float strata_master_peak(const float *left, const float *right, int n);

/* The compressor's gain and the output level as one ramped pass. `gain` steps by `step` BEFORE
 * each sample, as buses.cpp's loop did, so a block whose gain has not moved is still exact. */
void strata_master_ramp(float *left, float *right, int n, float gain, float step);

/* The soft limiter: unity below `threshold`, the excess above it through enaga_soft_clip scaled
 * by `headroom` (with `inv_headroom` = 1 / headroom from the caller, so nothing divides), sign
 * put back. The ceiling is +-1 whatever comes in. */
void strata_master_limit(float *left, float *right, int n, float threshold, float headroom,
                         float inv_headroom);

/* The planes into audio()'s interleaved L R L R buffer, and silence in any frames past n. */
void strata_interleave(float *out, const float *left, const float *right, int n, int frames);

#ifdef __cplusplus
}
#endif

#endif
