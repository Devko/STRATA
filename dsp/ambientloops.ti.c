/* The ambient send modes, per sample (ambientloops.h). Built by cl6x when it is installed, else
 * GCC. Nothing here branches or divides. */
#include "ambientloops.h"

/* --- the LONG delay ------------------------------------------------------------------------ */

#define LONG_MASK (STRATA_LONG_DIFFUSER_RING - 1)

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
    const int target = b->target;
    int i, k;

    for (i = 0; i < n; i++) {
        /* The glide: at most `glide` frames a frame toward the target, as a tape speeding up or
         * slowing down, carried between the whole and fractional parts. */
        const float gap = (float)(target - whole) - frac;
        const float move = gap > glide ? glide : (gap < -glide ? -glide : gap);
        frac += move;
        whole += frac >= 1.0f ? 1 : (frac < 0.0f ? -1 : 0);
        frac += frac >= 1.0f ? -1.0f : (frac < 0.0f ? 1.0f : 0.0f);

        /* The wow, always a positive offset so the read never comes nearer than the glide's. */
        const float s1 = s * rc + c * rs;
        const float c1 = c * rc - s * rs;
        s = s1;
        c = c1;
        const float off_l = frac + wow * (1.0f + s);
        const float off_r = frac + wow * (1.0f + c);
        const int wl = (int)off_l, wr = (int)off_r;
        const float fl = off_l - (float)wl, fr = off_r - (float)wr;

        /* Two interpolated reads, `whole + wl` and one more frames ago. */
        int al = w - whole - wl;
        al = al < 0 ? al + size : al;
        int bl = al - 1;
        bl = bl < 0 ? bl + size : bl;
        int ar = w - whole - wr;
        ar = ar < 0 ? ar + size : ar;
        int br = ar - 1;
        br = br < 0 ? br + size : br;
        const float yl = left[al] + (left[bl] - left[al]) * fl;
        const float yr = right[ar] + (right[br] - right[ar]) * fr;

        out_l[i] += level * yl;
        out_r[i] += level * yr;

        /* The feedback: straight back, or crossed for ping-pong; then low cut and tone. */
        float xl = straight * yl + pp * yr;
        float xr = straight * yr + pp * yl;
        hp_l += hp * (xl - hp_l);
        hp_r += hp * (xr - hp_r);
        xl -= hp_l;
        xr -= hp_r;
        lp_l += lp * (xl - lp_l);
        lp_r += lp * (xr - lp_r);
        xl = lp_l;
        xr = lp_r;

        /* A soft ceiling on what goes round: x - x^5 / 195, flat at +-2.5 -> +-2. A quintic
         * rather than a cubic so a held loop at ordinary levels loses nothing to it (0.03% a pass
         * at 0.5), while a feedback of 1 (the freeze) still cannot grow without bound. */
        xl = xl > 2.5f ? 2.5f : (xl < -2.5f ? -2.5f : xl);
        xr = xr > 2.5f ? 2.5f : (xr < -2.5f ? -2.5f : xr);
        const float xl2 = xl * xl, xr2 = xr * xr;
        xl = feedback * (xl - 0.00512f * xl2 * xl2 * xl);
        xr = feedback * (xr - 0.00512f * xr2 * xr2 * xr);

        /* Diffusion: four all-passes a side on what goes round, which smear each pass a little
         * more into a wash. Their delay is part of the period, so the read is shortened by it
         * (ambient.cpp). */
        for (k = 0; k < STRATA_LONG_DIFFUSERS; k++) {
            float *restrict ring_l = diffuse + k * STRATA_LONG_DIFFUSER_RING;
            float *restrict ring_r = diffuse + (k + STRATA_LONG_DIFFUSERS) * STRATA_LONG_DIFFUSER_RING;
            const float zl = ring_l[(dw - b->diffuse_len[k]) & LONG_MASK];
            const float zr = ring_r[(dw - b->diffuse_len[k + STRATA_LONG_DIFFUSERS]) & LONG_MASK];
            const float vl = xl - g * zl;
            const float vr = xr - g * zr;
            ring_l[dw] = vl;
            ring_r[dw] = vr;
            xl = g * vl + zl;
            xr = g * vr + zr;
        }

        /* The feed does not go through the all-passes, so the first echo is clean. It is
         * written the all-passes' delay AHEAD of the write head instead, which puts it on the
         * period too; that slot is past the longest read (ambient.cpp keeps it so), and this is
         * its first write of the lap, so it is assigned, and the loop's own frame added later. */
        const float feed = in[i] * input;
        int ahead = w + STRATA_LONG_AHEAD;
        ahead = ahead >= size ? ahead - size : ahead;
        left[w] += xl;
        right[w] += xr;
        left[ahead] = feed;
        right[ahead] = straight * feed;

        w++;
        w = w >= size ? 0 : w;
        dw = (dw + 1) & LONG_MASK;
        feedback += b->feedback_step;
        input += b->input_step;
        wow += b->wow_step;
        wow = wow < 0.0f ? 0.0f : wow;
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

void strata_hall_process(strata_hall *restrict h, const strata_hall_block *restrict b,
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
