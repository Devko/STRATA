/* The ambient send modes, per sample (ambientloops.h). Built by cl6x when it is installed, else
 * GCC. Nothing here branches or divides.
 *
 * Both effects run a block as PASSES over chunks of frames rather than as one loop over frames.
 * cl6x software-pipelines only an innermost, branch-free loop of moderate size; the frame loops
 * these used to be (with the all-passes and the lines as small loops inside) were not pipelined
 * at all, and were the two dearest functions in the worst-case scenario (docs/BUDGET.md, "Where
 * the worst case goes"). Every value is computed by the same operations in the same order as the
 * frame loops did, so the passes are bit-exact: tests/test_ambientloops.cpp runs the frame loops
 * beside them and compares every output and every byte of state, and the emulator renders the
 * cl6x build identically. What makes a split legal is that no pass reads a ring slot that a later
 * pass of the same chunk writes (each function's comment shows it from its shortest read). Loads
 * and stores of a ring are kept in different passes, so cl6x need not assume a store feeds the
 * next frame's load. */
#include "ambientloops.h"

/* --- the LONG delay ------------------------------------------------------------------------ */

#define LONG_MASK (STRATA_LONG_DIFFUSER_RING - 1)
#define LONG_CHUNK 32

/* The main rings are read at least min(delay_whole, target) - 1 >= 15 frames back: the glide
 * moves the delay by one frame at most and never past its target. A chunk is never longer than
 * that. The all-passes are read 241 frames back or more (ambient.cpp, kLongDiffuse); the feed goes
 * STRATA_LONG_AHEAD frames ahead, past the longest read. */
void strata_long_delay_process(strata_long_delay *restrict d,
                               const strata_long_delay_block *restrict b,
                               const float *restrict in, float *restrict out_l,
                               float *restrict out_r, int n) {
    float *restrict left = d->left;
    float *restrict right = d->right;
    float *restrict diffuse = d->diffuse;
    const int size = d->size;
    int w = d->write, dw = d->diffuse_write;
    int whole = d->delay_whole;
    float frac = d->delay_frac;
    float lp_l = d->lp_l, lp_r = d->lp_r, hp_l = d->hp_l, hp_r = d->hp_r;
    float s = d->lfo_s, c = d->lfo_c;
    float feedback = b->feedback, input = b->input, wow = b->wow;
    const float g = b->diffusion, lp = b->low_pass, hp = b->high_pass, level = b->level;
    const float rc = b->rot_c, rs = b->rot_s, glide = b->glide;
    const float pp = (float)b->ping_pong, straight = 1.0f - pp;
    const float feedback_step = b->feedback_step, input_step = b->input_step;
    const float wow_step = b->wow_step;
    const int target = b->target;
    const int l0 = b->diffuse_len[0], l1 = b->diffuse_len[1];
    const int l2 = b->diffuse_len[2], l3 = b->diffuse_len[3];
    const int r0 = b->diffuse_len[4], r1 = b->diffuse_len[5];
    const int r2 = b->diffuse_len[6], r3 = b->diffuse_len[7];
    float *restrict ring_l0 = diffuse + 0 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_l1 = diffuse + 1 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_l2 = diffuse + 2 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_l3 = diffuse + 3 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_r0 = diffuse + 4 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_r1 = diffuse + 5 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_r2 = diffuse + 6 * STRATA_LONG_DIFFUSER_RING;
    float *restrict ring_r3 = diffuse + 7 * STRATA_LONG_DIFFUSER_RING;
    const int reach = (whole < target ? whole : target) - 1;
    const int chunk = reach < 1 ? 1 : (reach < LONG_CHUNK ? reach : LONG_CHUNK);
    float yl[LONG_CHUNK], yr[LONG_CHUNK], xl[LONG_CHUNK], xr[LONG_CHUNK];
    float head_l[LONG_CHUNK], head_r[LONG_CHUNK], fl[LONG_CHUNK], fr[LONG_CHUNK];
    int tap_l[LONG_CHUNK], tap_r[LONG_CHUNK];
    float zl0[LONG_CHUNK], zl1[LONG_CHUNK], zl2[LONG_CHUNK], zl3[LONG_CHUNK];
    float zr0[LONG_CHUNK], zr1[LONG_CHUNK], zr2[LONG_CHUNK], zr3[LONG_CHUNK];
    int base, m, i, wi;

    for (base = 0; base < n; base += m) {
        m = n - base < chunk ? n - base : chunk;

        /* Where to read: the glide and the wow. */
        wi = w;
        for (i = 0; i < m; i++) {
            /* The glide: at most `glide` frames a frame toward the target, as a tape speeding up
             * or slowing down, carried between the whole and fractional parts. */
            const float gap = (float)(target - whole) - frac;
            /* (Flat selects, not nested ones: the same values, and cl6x pipelines them.) */
            const float floor_move = gap < -glide ? -glide : gap;
            frac += gap > glide ? glide : floor_move;
            const int up = frac >= 1.0f, down = frac < 0.0f;   /* never both */
            const float carry = down ? 1.0f : 0.0f;
            whole += up - down;
            frac += up ? -1.0f : carry;

            /* The wow, always a positive offset so the read never comes nearer than the glide's. */
            const float s1 = s * rc + c * rs;
            const float c1 = c * rc - s * rs;
            s = s1;
            c = c1;
            const float off_l = frac + wow * (1.0f + s);
            const float off_r = frac + wow * (1.0f + c);
            const int wl = (int)off_l, wr = (int)off_r;
            fl[i] = off_l - (float)wl;
            fr[i] = off_r - (float)wr;

            /* Where the two interpolated reads are, `whole + wl` and one more frames ago. */
            int al = wi - whole - wl;
            al = al < 0 ? al + size : al;
            int ar = wi - whole - wr;
            ar = ar < 0 ? ar + size : ar;
            tap_l[i] = al;
            tap_r[i] = ar;

            wow += wow_step;
            wow = wow < 0.0f ? 0.0f : wow;
            wi++;
            wi = wi >= size ? 0 : wi;
        }

        /* The reads themselves. Loads only. */
        for (i = 0; i < m; i++) {
            const int al = tap_l[i], ar = tap_r[i];
            int bl = al - 1;
            bl = bl < 0 ? bl + size : bl;
            int br = ar - 1;
            br = br < 0 ? br + size : br;
            const float ya = left[al] + (left[bl] - left[al]) * fl[i];
            const float yb = right[ar] + (right[br] - right[ar]) * fr[i];
            yl[i] = ya;
            yr[i] = yb;
            out_l[base + i] += level * ya;
            out_r[base + i] += level * yb;
        }

        /* The feedback: straight back, or crossed for ping-pong; then low cut and tone, and a
         * soft ceiling on what goes round: x - x^5 / 195, flat at +-2.5 -> +-2. A quintic rather
         * than a cubic so a held loop at ordinary levels loses nothing to it (0.03% a pass at
         * 0.5), while a feedback of 1 (the freeze) still cannot grow without bound. */
        for (i = 0; i < m; i++) {
            float a = straight * yl[i] + pp * yr[i];
            float e = straight * yr[i] + pp * yl[i];
            hp_l += hp * (a - hp_l);
            hp_r += hp * (e - hp_r);
            a -= hp_l;
            e -= hp_r;
            lp_l += lp * (a - lp_l);
            lp_r += lp * (e - lp_r);
            a = lp_l;
            e = lp_r;
            a = a > 2.5f ? 2.5f : (a < -2.5f ? -2.5f : a);
            e = e > 2.5f ? 2.5f : (e < -2.5f ? -2.5f : e);
            const float a2 = a * a, e2 = e * e;
            xl[i] = feedback * (a - 0.00512f * a2 * a2 * a);
            xr[i] = feedback * (e - 0.00512f * e2 * e2 * e);
            feedback += feedback_step;
        }

        /* What the all-passes and the head will read, from slots no store in this chunk touches. */
        wi = w;
        for (i = 0; i < m; i++) {
            const int di = (dw + i) & LONG_MASK;
            zl0[i] = ring_l0[(di - l0) & LONG_MASK];
            zl1[i] = ring_l1[(di - l1) & LONG_MASK];
            zl2[i] = ring_l2[(di - l2) & LONG_MASK];
            zl3[i] = ring_l3[(di - l3) & LONG_MASK];
            zr0[i] = ring_r0[(di - r0) & LONG_MASK];
            zr1[i] = ring_r1[(di - r1) & LONG_MASK];
            zr2[i] = ring_r2[(di - r2) & LONG_MASK];
            zr3[i] = ring_r3[(di - r3) & LONG_MASK];
            head_l[i] = left[wi];
            head_r[i] = right[wi];
            wi++;
            wi = wi >= size ? 0 : wi;
        }

        /* Diffusion: four all-passes a side on what goes round, which smear each pass a little
         * more into a wash. Their delay is part of the period, so the read is shortened by it
         * (ambient.cpp). The feed does not go through them, so the first echo is clean: it is
         * written the all-passes' delay AHEAD of the write head instead, which puts it on the
         * period too; that slot is past the longest read, and this is its first write of the
         * lap, so it is assigned, and the loop's own frame added later. Stores only. */
        for (i = 0; i < m; i++) {
            const int di = (dw + i) & LONG_MASK;
            float a = xl[i], e = xr[i], v, u;
            v = a - g * zl0[i];
            u = e - g * zr0[i];
            ring_l0[di] = v;
            ring_r0[di] = u;
            a = g * v + zl0[i];
            e = g * u + zr0[i];
            v = a - g * zl1[i];
            u = e - g * zr1[i];
            ring_l1[di] = v;
            ring_r1[di] = u;
            a = g * v + zl1[i];
            e = g * u + zr1[i];
            v = a - g * zl2[i];
            u = e - g * zr2[i];
            ring_l2[di] = v;
            ring_r2[di] = u;
            a = g * v + zl2[i];
            e = g * u + zr2[i];
            v = a - g * zl3[i];
            u = e - g * zr3[i];
            ring_l3[di] = v;
            ring_r3[di] = u;
            a = g * v + zl3[i];
            e = g * u + zr3[i];

            const float feed = in[base + i] * input;
            int ahead = w + STRATA_LONG_AHEAD;
            ahead = ahead >= size ? ahead - size : ahead;
            left[w] = head_l[i] + a;
            right[w] = head_r[i] + e;
            left[ahead] = feed;
            right[ahead] = straight * feed;
            input += input_step;
            w++;
            w = w >= size ? 0 : w;
        }
        dw = (dw + m) & LONG_MASK;
    }

    d->write = w;
    d->diffuse_write = dw;
    d->delay_whole = whole;
    d->delay_frac = frac;
    d->lp_l = lp_l;
    d->lp_r = lp_r;
    d->hp_l = hp_l;
    d->hp_r = hp_r;
    d->lfo_s = s;
    d->lfo_c = c;
}

/* --- the HALL reverb ----------------------------------------------------------------------- */

#define HALL_MASK (STRATA_HALL_RING - 1)
#define HALL_DIFFUSER_MASK (STRATA_HALL_DIFFUSER_RING - 1)
#define SHIMMER_MASK (STRATA_SHIMMER_RING - 1)
#define HALL_CHUNK 32

/* The lines are read at least 63 frames back (each length is held at 64 or more, ambient.cpp,
 * and the modulation only adds), the input all-passes 241 or more (kHallDiffuse), so a chunk of
 * 32 never reads its own writes there. The shimmer is read 2 frames back or more, so its pass
 * comes after the pass that writes the chunk's frames into its ring, as the frame loop wrote
 * each frame before reading. */

/* One line read at its gliding, modulated length, damped and scaled by its loss. */
#define HALL_LINE(j, sweep)                                                                       \
    do {                                                                                          \
        const float *restrict ring = lines + (j) * STRATA_HALL_RING;                             \
        const float l = length##j + mod * (1.0f + (sweep));                                       \
        const int whole = (int)l;                                                                 \
        const float frac = l - (float)whole;                                                      \
        const int a = (wi - whole) & HALL_MASK;                                                   \
        const int bb = (a - 1) & HALL_MASK;                                                       \
        const float y = ring[a] + (ring[bb] - ring[a]) * frac;                                    \
        damp##j += damping * (y - damp##j);                                                       \
        r##j[i] = damp##j * gain##j;                                                              \
        length##j += step##j;                                                                     \
    } while (0)

void strata_hall_passes(strata_hall *restrict h, const strata_hall_block *restrict b,
                        const float *restrict in, float *restrict out_l, float *restrict out_r,
                        int n) {
    float *restrict lines = h->lines;
    float *restrict diffuse = h->diffuse;
    float *restrict shimmer = h->shimmer;
    int w = h->write;
    float s = h->lfo_s, c = h->lfo_c;
    float phase = h->shimmer_phase, shimmer_out = h->shimmer_out;
    const float g = b->diffusion, damping = b->damping, mod = b->mod, level = b->level;
    const float rc = b->rot_c, rs = b->rot_s, shimmer_amount = b->shimmer;
    const float inv_window = 1.0f / STRATA_SHIMMER_WINDOW;   /* a constant: no division at run time */
    float damp0 = h->damp[0], damp1 = h->damp[1], damp2 = h->damp[2], damp3 = h->damp[3];
    float damp4 = h->damp[4], damp5 = h->damp[5], damp6 = h->damp[6], damp7 = h->damp[7];
    float length0 = h->length[0], length1 = h->length[1], length2 = h->length[2];
    float length3 = h->length[3], length4 = h->length[4], length5 = h->length[5];
    float length6 = h->length[6], length7 = h->length[7];
    const float gain0 = b->gain[0], gain1 = b->gain[1], gain2 = b->gain[2], gain3 = b->gain[3];
    const float gain4 = b->gain[4], gain5 = b->gain[5], gain6 = b->gain[6], gain7 = b->gain[7];
    const float step0 = b->length_step[0], step1 = b->length_step[1];
    const float step2 = b->length_step[2], step3 = b->length_step[3];
    const float step4 = b->length_step[4], step5 = b->length_step[5];
    const float step6 = b->length_step[6], step7 = b->length_step[7];
    const int len0 = b->diffuse_len[0], len1 = b->diffuse_len[1];
    const int len2 = b->diffuse_len[2], len3 = b->diffuse_len[3];
    float *restrict ring0 = diffuse + 0 * STRATA_HALL_DIFFUSER_RING;
    float *restrict ring1 = diffuse + 1 * STRATA_HALL_DIFFUSER_RING;
    float *restrict ring2 = diffuse + 2 * STRATA_HALL_DIFFUSER_RING;
    float *restrict ring3 = diffuse + 3 * STRATA_HALL_DIFFUSER_RING;
    float sn[HALL_CHUNK], cs[HALL_CHUNK];
    float r0[HALL_CHUNK], r1[HALL_CHUNK], r2[HALL_CHUNK], r3[HALL_CHUNK];
    float r4[HALL_CHUNK], r5[HALL_CHUNK], r6[HALL_CHUNK], r7[HALL_CHUNK];
    float shifted[HALL_CHUNK + 1];
    float z0[HALL_CHUNK], z1[HALL_CHUNK], z2[HALL_CHUNK], z3[HALL_CHUNK];
    int base, m, i, wi;

    for (base = 0; base < n; base += m) {
        m = n - base < HALL_CHUNK ? n - base : HALL_CHUNK;

        /* The modulator: one phasor, its sine and cosine and their negatives over the lines. */
        for (i = 0; i < m; i++) {
            const float s1 = s * rc + c * rs;
            const float c1 = c * rc - s * rs;
            s = s1;
            c = c1;
            sn[i] = s;
            cs[i] = c;
        }

        /* Read each line at its (gliding, modulated) length, damp it, and take its loss. Four
         * lines a pass: their damping recurrences overlap. Loads only. */
        for (i = 0; i < m; i++) {
            wi = (w + i) & HALL_MASK;
            HALL_LINE(0, sn[i]);
            HALL_LINE(1, cs[i]);
            HALL_LINE(2, -sn[i]);
            HALL_LINE(3, -cs[i]);
        }
        for (i = 0; i < m; i++) {
            wi = (w + i) & HALL_MASK;
            HALL_LINE(4, sn[i]);
            HALL_LINE(5, cs[i]);
            HALL_LINE(6, -sn[i]);
            HALL_LINE(7, -cs[i]);
        }

        /* Out: the even lines left, the odd right, before they mix; the eight-point Hadamard,
         * normalised, as three butterfly stages, back into r; and the tail into the shimmer's
         * ring. Stores only. */
        for (i = 0; i < m; i++) {
            const float wet_l = (r0[i] + r2[i] + r4[i] + r6[i]) * 0.5f;
            const float wet_r = (r1[i] + r3[i] + r5[i] + r7[i]) * 0.5f;
            out_l[base + i] += level * wet_l;
            out_r[base + i] += level * wet_r;

            const float t0 = r0[i] + r1[i], t1 = r0[i] - r1[i];
            const float t2 = r2[i] + r3[i], t3 = r2[i] - r3[i];
            const float t4 = r4[i] + r5[i], t5 = r4[i] - r5[i];
            const float t6 = r6[i] + r7[i], t7 = r6[i] - r7[i];
            const float u0 = t0 + t2, u1 = t1 + t3, u2 = t0 - t2, u3 = t1 - t3;
            const float u4 = t4 + t6, u5 = t5 + t7, u6 = t4 - t6, u7 = t5 - t7;
            const float norm = 0.35355339f;
            r0[i] = (u0 + u4) * norm;
            r1[i] = (u1 + u5) * norm;
            r2[i] = (u2 + u6) * norm;
            r3[i] = (u3 + u7) * norm;
            r4[i] = (u0 - u4) * norm;
            r5[i] = (u1 - u5) * norm;
            r6[i] = (u2 - u6) * norm;
            r7[i] = (u3 - u7) * norm;

            shimmer[(w + i) & SHIMMER_MASK] = (wet_l + wet_r) * 0.5f;
        }

        /* The shimmer: the tail an octave up, as two grains a half turn apart whose read heads
         * move at twice the write speed, each faded by a triangle so the two always sum to one.
         * shifted[i] is what goes into frame i: the last frame's shifter output. Loads only. */
        shifted[0] = shimmer_out;
        for (i = 0; i < m; i++) {
            wi = w + i;
            float p2 = phase + 0.5f;
            p2 = p2 >= 1.0f ? p2 - 1.0f : p2;
            const float d1 = 2.0f + STRATA_SHIMMER_WINDOW * (1.0f - phase);
            const float d2 = 2.0f + STRATA_SHIMMER_WINDOW * (1.0f - p2);
            const int w1 = (int)d1, w2 = (int)d2;
            const float f1 = d1 - (float)w1, f2 = d2 - (float)w2;
            const int a1 = (wi - w1) & SHIMMER_MASK, b1 = (a1 - 1) & SHIMMER_MASK;
            const int a2 = (wi - w2) & SHIMMER_MASK, b2 = (a2 - 1) & SHIMMER_MASK;
            const float y1 = shimmer[a1] + (shimmer[b1] - shimmer[a1]) * f1;
            const float y2 = shimmer[a2] + (shimmer[b2] - shimmer[a2]) * f2;
            const float e1 = 2.0f * phase - 1.0f, e2 = 2.0f * p2 - 1.0f;
            const float g1 = 1.0f - (e1 < 0.0f ? -e1 : e1);
            const float g2 = 1.0f - (e2 < 0.0f ? -e2 : e2);
            const float out = g1 * y1 + g2 * y2;
            shifted[i + 1] = out > 1.0f ? 1.0f : (out < -1.0f ? -1.0f : out);
            phase += inv_window;
            phase = phase >= 1.0f ? phase - 1.0f : phase;
        }
        shimmer_out = shifted[m];

        /* What the input all-passes will read, from slots no store in this chunk touches. */
        for (i = 0; i < m; i++) {
            wi = w + i;
            z0[i] = ring0[(wi - len0) & HALL_DIFFUSER_MASK];
            z1[i] = ring1[(wi - len1) & HALL_DIFFUSER_MASK];
            z2[i] = ring2[(wi - len2) & HALL_DIFFUSER_MASK];
            z3[i] = ring3[(wi - len3) & HALL_DIFFUSER_MASK];
        }

        /* The input, with the shifted tail of the last frame, through four all-passes, then back
         * into the lines with the Hadamard's mix, alternating sign so the lines start apart
         * (m - x is m + (-1) * x exactly). Stores only. */
        for (i = 0; i < m; i++) {
            const int di = (w + i) & HALL_DIFFUSER_MASK;
            wi = (w + i) & HALL_MASK;
            float x = in[base + i] + shimmer_amount * shifted[i], v;
            v = x - g * z0[i];
            ring0[di] = v;
            x = g * v + z0[i];
            v = x - g * z1[i];
            ring1[di] = v;
            x = g * v + z1[i];
            v = x - g * z2[i];
            ring2[di] = v;
            x = g * v + z2[i];
            v = x - g * z3[i];
            ring3[di] = v;
            x = g * v + z3[i];
            lines[0 * STRATA_HALL_RING + wi] = r0[i] + x;
            lines[1 * STRATA_HALL_RING + wi] = r1[i] - x;
            lines[2 * STRATA_HALL_RING + wi] = r2[i] + x;
            lines[3 * STRATA_HALL_RING + wi] = r3[i] - x;
            lines[4 * STRATA_HALL_RING + wi] = r4[i] + x;
            lines[5 * STRATA_HALL_RING + wi] = r5[i] - x;
            lines[6 * STRATA_HALL_RING + wi] = r6[i] + x;
            lines[7 * STRATA_HALL_RING + wi] = r7[i] - x;
        }
        w = (w + m) & HALL_MASK;
    }

    h->damp[0] = damp0;
    h->damp[1] = damp1;
    h->damp[2] = damp2;
    h->damp[3] = damp3;
    h->damp[4] = damp4;
    h->damp[5] = damp5;
    h->damp[6] = damp6;
    h->damp[7] = damp7;
    h->length[0] = length0;
    h->length[1] = length1;
    h->length[2] = length2;
    h->length[3] = length3;
    h->length[4] = length4;
    h->length[5] = length5;
    h->length[6] = length6;
    h->length[7] = length7;
    h->write = w;
    h->lfo_s = s;
    h->lfo_c = c;
    h->shimmer_phase = phase;
    h->shimmer_out = shimmer_out;
}

/* The frame loop the passes replaced, verbatim: what tests/test_ambientloops.cpp holds them to,
 * and what a GCC build of the package runs (below). */
void strata_hall_frames(strata_hall *restrict h, const strata_hall_block *restrict b,
                        const float *restrict in, float *restrict out_l, float *restrict out_r,
                        int n) {
    float *restrict lines = h->lines;
    float *restrict diffuse = h->diffuse;
    float *restrict shimmer = h->shimmer;
    int w = h->write;
    float s = h->lfo_s, c = h->lfo_c;
    float phase = h->shimmer_phase, shimmer_out = h->shimmer_out;
    const float g = b->diffusion, damping = b->damping, mod = b->mod, level = b->level;
    const float rc = b->rot_c, rs = b->rot_s, shimmer_amount = b->shimmer;
    const float inv_window = 1.0f / STRATA_SHIMMER_WINDOW;   /* a constant: no division at run time */
    int i, j;

    for (i = 0; i < n; i++) {
        /* The input, with the shifted tail of the last frame, through four all-passes. */
        float x = in[i] + shimmer_amount * shimmer_out;
        for (j = 0; j < STRATA_HALL_DIFFUSERS; j++) {
            float *restrict ring = diffuse + j * STRATA_HALL_DIFFUSER_RING;
            const float z = ring[(w - b->diffuse_len[j]) & HALL_DIFFUSER_MASK];
            const float v = x - g * z;
            ring[w & HALL_DIFFUSER_MASK] = v;
            x = g * v + z;
        }

        /* The modulator: one phasor, its sine and cosine and their negatives over the lines. */
        const float s1 = s * rc + c * rs;
        const float c1 = c * rc - s * rs;
        s = s1;
        c = c1;
        const float sweep[4] = {s, c, -s, -c};

        /* Read each line at its (gliding, modulated) length, damp it, and take its loss. */
        float r[STRATA_HALL_LINES];
        for (j = 0; j < STRATA_HALL_LINES; j++) {
            const float *restrict ring = lines + j * STRATA_HALL_RING;
            const float len = h->length[j] + mod * (1.0f + sweep[j & 3]);
            const int whole = (int)len;
            const float frac = len - (float)whole;
            const int a = (w - whole) & HALL_MASK;
            const int bb = (a - 1) & HALL_MASK;
            const float y = ring[a] + (ring[bb] - ring[a]) * frac;
            h->damp[j] += damping * (y - h->damp[j]);
            r[j] = h->damp[j] * b->gain[j];
            h->length[j] += b->length_step[j];
        }

        /* Out: the even lines left, the odd right, before they mix. */
        const float wet_l = (r[0] + r[2] + r[4] + r[6]) * 0.5f;
        const float wet_r = (r[1] + r[3] + r[5] + r[7]) * 0.5f;
        out_l[i] += level * wet_l;
        out_r[i] += level * wet_r;

        /* The eight-point Hadamard, normalised, as three butterfly stages. */
        float t0 = r[0] + r[1], t1 = r[0] - r[1], t2 = r[2] + r[3], t3 = r[2] - r[3];
        float t4 = r[4] + r[5], t5 = r[4] - r[5], t6 = r[6] + r[7], t7 = r[6] - r[7];
        float u0 = t0 + t2, u1 = t1 + t3, u2 = t0 - t2, u3 = t1 - t3;
        float u4 = t4 + t6, u5 = t5 + t7, u6 = t4 - t6, u7 = t5 - t7;
        const float norm = 0.35355339f;
        float m[STRATA_HALL_LINES];
        m[0] = (u0 + u4) * norm;
        m[1] = (u1 + u5) * norm;
        m[2] = (u2 + u6) * norm;
        m[3] = (u3 + u7) * norm;
        m[4] = (u0 - u4) * norm;
        m[5] = (u1 - u5) * norm;
        m[6] = (u2 - u6) * norm;
        m[7] = (u3 - u7) * norm;

        /* Back in, with the input on every line, alternating sign so the lines start apart. */
        for (j = 0; j < STRATA_HALL_LINES; j++) {
            const float sign = (j & 1) ? -1.0f : 1.0f;
            lines[j * STRATA_HALL_RING + (w & HALL_MASK)] = m[j] + sign * x;
        }

        /* The shimmer: the tail an octave up, as two grains a half turn apart whose read heads
         * move at twice the write speed, each faded by a triangle so the two always sum to one. */
        const float mono = (wet_l + wet_r) * 0.5f;
        shimmer[w & SHIMMER_MASK] = mono;
        float p2 = phase + 0.5f;
        p2 = p2 >= 1.0f ? p2 - 1.0f : p2;
        const float d1 = 2.0f + STRATA_SHIMMER_WINDOW * (1.0f - phase);
        const float d2 = 2.0f + STRATA_SHIMMER_WINDOW * (1.0f - p2);
        const int w1 = (int)d1, w2 = (int)d2;
        const float f1 = d1 - (float)w1, f2 = d2 - (float)w2;
        const int a1 = (w - w1) & SHIMMER_MASK, b1 = (a1 - 1) & SHIMMER_MASK;
        const int a2 = (w - w2) & SHIMMER_MASK, b2 = (a2 - 1) & SHIMMER_MASK;
        const float y1 = shimmer[a1] + (shimmer[b1] - shimmer[a1]) * f1;
        const float y2 = shimmer[a2] + (shimmer[b2] - shimmer[a2]) * f2;
        const float e1 = 2.0f * phase - 1.0f, e2 = 2.0f * p2 - 1.0f;
        const float g1 = 1.0f - (e1 < 0.0f ? -e1 : e1);
        const float g2 = 1.0f - (e2 < 0.0f ? -e2 : e2);
        shimmer_out = g1 * y1 + g2 * y2;
        shimmer_out = shimmer_out > 1.0f ? 1.0f : (shimmer_out < -1.0f ? -1.0f : shimmer_out);
        phase += inv_window;
        phase = phase >= 1.0f ? phase - 1.0f : phase;

        w = (w + 1) & HALL_MASK;
    }

    h->write = w;
    h->lfo_s = s;
    h->lfo_c = c;
    h->shimmer_phase = phase;
    h->shimmer_out = shimmer_out;
}

/* cl6x and the host get the passes; tic6x GCC, which does not software-pipeline, gets the frame
 * loop: the passes only add memory traffic there (scenario_ambient 58.2% -> 60.6% GCC-only). */
void strata_hall_process(strata_hall *h, const strata_hall_block *b, const float *in,
                         float *out_l, float *out_r, int n) {
#if defined(__TI_COMPILER_VERSION__) || !defined(__TMS320C6X__)
    strata_hall_passes(h, b, in, out_l, out_r, n);
#else
    strata_hall_frames(h, b, in, out_l, out_r, n);
#endif
}
