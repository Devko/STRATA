/* The ambient send modes' per-sample loops, as plain C for TI's cl6x (ambientloops.ti.c):
 * the LONG delay (send A) and the HALL reverb (send B). dsp/ambient.hpp has the design.
 *
 * Both run only while their send has a tail to render (dsp/buses.cpp), so neither is a tax on a
 * project that keeps the classic modes. No branches and no division: every wrap and clamp is a
 * ?:, every ring that can be a power of two is one, and the long delay's ring, which cannot
 * (12 s is not a power of two of frames, and rounding it up would double 4.4 MiB), wraps by a
 * compare. */
#ifndef STRATA_AMBIENTLOOPS_H
#define STRATA_AMBIENTLOOPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- the LONG delay ------------------------------------------------------------------------ */

#define STRATA_LONG_DIFFUSERS 4             /* all-passes in each channel's feedback path */
#define STRATA_LONG_DIFFUSER_RING 1024      /* frames per all-pass ring; a power of two */
#define STRATA_LONG_AHEAD 2016              /* the four all-passes' delay, a side: the feed's lead */

typedef struct {
    float *left, *right;     /* the two main rings, `size` frames each */
    float *diffuse;          /* 2 x STRATA_LONG_DIFFUSERS rings: the left's four, then the right's */
    int size;                /* the main rings' length in frames */
    int write;               /* where the next frame goes, 0 to size - 1 */
    int diffuse_write;       /* the same for every all-pass ring, masked */
    int delay_whole;         /* the read delay in frames, whole part (>= 16) ... */
    float delay_frac;        /* ... and its fraction, 0 to 1; split so 12 s keeps sub-frame precision */
    float lp_l, lp_r;        /* the feedback's low-pass state */
    float hp_l, hp_r;        /* the feedback's high-pass (as the low-pass it subtracts) */
    float lfo_s, lfo_c;      /* the wow's phasor: sine and cosine, rotated a frame at a time */
} strata_long_delay;

typedef struct {
    int target;              /* the read delay the glide is heading for, in whole frames */
    float glide;             /* the most the read delay moves in one frame (tape speed change) */
    float feedback, feedback_step;   /* ramped across the block */
    float input, input_step;         /* the feed's gain, ramped: 1 open, 0 while frozen */
    float wow, wow_step;             /* the wow's depth in frames, ramped */
    float rot_c, rot_s;              /* the phasor's rotation per frame */
    float low_pass;          /* one-pole coefficient, 1 = open */
    float high_pass;         /* one-pole coefficient of the subtracted low-pass, 0 = open */
    float diffusion;         /* the all-passes' coefficient, 0 to 0.7 */
    int ping_pong;           /* 1: the feed goes left and each side feeds the other */
    int diffuse_len[2 * STRATA_LONG_DIFFUSERS];   /* each < STRATA_LONG_DIFFUSER_RING */
    float level;             /* the echoes into out_l / out_r */
} strata_long_delay_block;

/* Feeds the mono `in` through the delay and ADDS level x the echoes to out_l / out_r. */
void strata_long_delay_process(strata_long_delay *d, const strata_long_delay_block *b,
                               const float *in, float *out_l, float *out_r, int n);

/* --- the HALL reverb ----------------------------------------------------------------------- */

#define STRATA_HALL_LINES 8
#define STRATA_HALL_RING 16384              /* frames per line; a power of two */
#define STRATA_HALL_DIFFUSERS 4
#define STRATA_HALL_DIFFUSER_RING 1024
#define STRATA_SHIMMER_RING 8192
#define STRATA_SHIMMER_WINDOW 4096.0f       /* frames each grain of the octave-up shifter lasts */

typedef struct {
    float *lines;            /* STRATA_HALL_LINES rings of STRATA_HALL_RING */
    float *diffuse;          /* STRATA_HALL_DIFFUSERS rings of STRATA_HALL_DIFFUSER_RING */
    float *shimmer;          /* one ring of STRATA_SHIMMER_RING */
    int write;               /* shared by every ring, masked by each ring's size */
    float damp[STRATA_HALL_LINES];   /* each line's damping low-pass */
    float length[STRATA_HALL_LINES]; /* each line's read length, gliding */
    float lfo_s, lfo_c;
    float shimmer_phase;     /* 0 to 1, the first grain's; the second is half a turn on */
    float shimmer_out;       /* the shifter's last frame, fed back into the next */
} strata_hall;

typedef struct {
    float length_step[STRATA_HALL_LINES];   /* the glide of each line's length per frame */
    float gain[STRATA_HALL_LINES];          /* each line's loss per pass, from the decay time */
    float damping;           /* one-pole coefficient, 1 = bright */
    float mod;               /* the lines' modulation depth in frames */
    float rot_c, rot_s;
    float diffusion;         /* the input all-passes' coefficient */
    float shimmer;           /* how much of the shifted tail goes back in */
    int diffuse_len[STRATA_HALL_DIFFUSERS];
    float level;
} strata_hall_block;

/* Feeds the mono `in` through the reverb and ADDS level x its stereo tail to out_l / out_r. */
void strata_hall_process(strata_hall *h, const strata_hall_block *b, const float *in,
                         float *out_l, float *out_r, int n);

/* The two ways strata_hall_process runs, bit-identical (ambientloops.ti.c): passes over chunks,
 * which cl6x pipelines, and the frame loop, which GCC builds run. Both kept for the tests. */
void strata_hall_passes(strata_hall *h, const strata_hall_block *b, const float *in,
                        float *out_l, float *out_r, int n);
void strata_hall_frames(strata_hall *h, const strata_hall_block *b, const float *in,
                        float *out_l, float *out_r, int n);

#ifdef __cplusplus
}
#endif

#endif
