// dsp/ambientloops.ti.c: the LONG delay and the HALL reverb in passes over chunks, against the
// frame loops they replaced, byte for byte.
//
// The passes exist for cl6x's sake (it pipelines only an innermost loop, and the frame loops with
// small loops inside were not pipelined at all), and they must not change a single sample to get
// there. The LONG delay's frame loop is kept here verbatim as its reference; the HALL's stays in
// the source as strata_hall_frames, because a GCC build of the package runs it. Each test runs
// both from the same state through blocks of every length from 1 to 64 with the parameters
// moving -- the delay gliding down to its 16-frame floor (where its chunks are shortest) and back,
// ping-pong on and off, the feedback ramping to the freeze; the hall's lines gliding and sitting
// at their 64-frame floor, the shimmer and the modulation on -- and compares every output and
// every byte of the rings and of the state.
#include "check.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ambientloops.h"

namespace {

// --- the LONG delay's frame loop, as it was (2026-11-26, a079b9f); the HALL's is in the source ---

#define LONG_MASK (STRATA_LONG_DIFFUSER_RING - 1)

void reference_long(strata_long_delay *__restrict d, const strata_long_delay_block *__restrict b,
                    const float *__restrict in, float *__restrict out_l, float *__restrict out_r,
                    int n) {
    float *__restrict left = d->left;
    float *__restrict right = d->right;
    float *__restrict diffuse = d->diffuse;
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
        const float gap = (float)(target - whole) - frac;
        const float move = gap > glide ? glide : (gap < -glide ? -glide : gap);
        frac += move;
        whole += frac >= 1.0f ? 1 : (frac < 0.0f ? -1 : 0);
        frac += frac >= 1.0f ? -1.0f : (frac < 0.0f ? 1.0f : 0.0f);

        const float s1 = s * rc + c * rs;
        const float c1 = c * rc - s * rs;
        s = s1;
        c = c1;
        const float off_l = frac + wow * (1.0f + s);
        const float off_r = frac + wow * (1.0f + c);
        const int wl = (int)off_l, wr = (int)off_r;
        const float fl = off_l - (float)wl, fr = off_r - (float)wr;

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

        xl = xl > 2.5f ? 2.5f : (xl < -2.5f ? -2.5f : xl);
        xr = xr > 2.5f ? 2.5f : (xr < -2.5f ? -2.5f : xr);
        const float xl2 = xl * xl, xr2 = xr * xr;
        xl = feedback * (xl - 0.00512f * xl2 * xl2 * xl);
        xr = feedback * (xr - 0.00512f * xr2 * xr2 * xr);

        for (k = 0; k < STRATA_LONG_DIFFUSERS; k++) {
            float *__restrict ring_l = diffuse + k * STRATA_LONG_DIFFUSER_RING;
            float *__restrict ring_r = diffuse + (k + STRATA_LONG_DIFFUSERS) * STRATA_LONG_DIFFUSER_RING;
            const float zl = ring_l[(dw - b->diffuse_len[k]) & LONG_MASK];
            const float zr = ring_r[(dw - b->diffuse_len[k + STRATA_LONG_DIFFUSERS]) & LONG_MASK];
            const float vl = xl - g * zl;
            const float vr = xr - g * zr;
            ring_l[dw] = vl;
            ring_r[dw] = vr;
            xl = g * vl + zl;
            xr = g * vr + zr;
        }

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

// --- the comparison -------------------------------------------------------------------------

uint32_t next(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float noise(uint32_t &s) { return float(int32_t(next(s))) * (1.0f / 2147483648.0f); }

bool same(const void *a, const void *b, std::size_t bytes) { return std::memcmp(a, b, bytes) == 0; }

// The scalar part of a state: everything but the ring pointers, which differ by construction.
bool same_scalars(strata_hall a, strata_hall b) {
    a.lines = b.lines = nullptr;
    a.diffuse = b.diffuse = nullptr;
    a.shimmer = b.shimmer = nullptr;
    return same(&a, &b, sizeof a);
}

bool same_scalars(strata_long_delay a, strata_long_delay b) {
    a.left = b.left = nullptr;
    a.right = b.right = nullptr;
    a.diffuse = b.diffuse = nullptr;
    return same(&a, &b, sizeof a);
}

constexpr int kLongSize = 9000;
constexpr int kLongDiffuse = 2 * STRATA_LONG_DIFFUSERS * STRATA_LONG_DIFFUSER_RING;
float long_rings[2][2 * kLongSize + kLongDiffuse];

constexpr int kHallLines = STRATA_HALL_LINES * STRATA_HALL_RING;
constexpr int kHallDiffuse = STRATA_HALL_DIFFUSERS * STRATA_HALL_DIFFUSER_RING;
float hall_rings[2][kHallLines + kHallDiffuse + STRATA_SHIMMER_RING];

}  // namespace

TEST(ambientloops_the_long_delay_in_passes_is_bit_exact_against_the_frame_loop) {
    uint32_t seed = 0x2468ACE1u;
    for (float &x : long_rings[0]) x = 0.5f * noise(seed);
    std::memcpy(long_rings[1], long_rings[0], sizeof long_rings[0]);

    strata_long_delay d[2];
    for (int k = 0; k < 2; k++) {
        d[k] = strata_long_delay{};
        d[k].left = long_rings[k];
        d[k].right = long_rings[k] + kLongSize;
        d[k].diffuse = long_rings[k] + 2 * kLongSize;
        d[k].size = kLongSize;
        d[k].write = 8990;            // near the end: the head and the feed wrap in the run
        d[k].diffuse_write = 1000;
        d[k].delay_whole = 3000;
        d[k].lfo_s = 0.0f;
        d[k].lfo_c = 1.0f;
    }
    strata_long_delay_block b{};
    const int lengths[8] = {241, 383, 563, 829, 263, 359, 601, 793};
    for (int k = 0; k < 8; k++) b.diffuse_len[k] = lengths[k];
    b.glide = 0.5f;
    b.rot_c = 0.99999f;
    b.rot_s = 0.0044721f;
    b.diffusion = 0.6f;
    b.low_pass = 0.7f;
    b.high_pass = 0.01f;
    b.level = 0.8f;
    b.input = 1.0f;
    b.feedback = 0.5f;

    float in[64], out[2][2][64] = {};
    int frames = 0, shortest = 1 << 30;
    for (int block = 0; block < 900; block++) {
        const int n = 1 + block % 64;
        // The target: long, then the shortest (16) so the glide runs down into the chunk bound,
        // then long again; ping-pong on for a while; the feedback to the freeze and back.
        b.target = (block / 300) % 2 == 0 ? 3000 + block : 16;
        b.ping_pong = (block / 100) % 2;
        b.feedback_step = ((block / 50) % 2 == 0 ? 0.004f : -0.004f) / float(n);
        b.input_step = 0.0f;
        b.wow = 40.0f * float(block % 7) / 7.0f;
        b.wow_step = 0.01f;
        for (int i = 0; i < n; i++) in[i] = noise(seed);
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < n; i++) out[k][0][i] = out[k][1][i] = 0.25f;
        }
        reference_long(&d[0], &b, in, out[0][0], out[0][1], n);
        strata_long_delay_process(&d[1], &b, in, out[1][0], out[1][1], n);
        b.feedback += b.feedback_step * float(n);
        b.feedback = b.feedback > 1.0f ? 1.0f : (b.feedback < 0.0f ? 0.0f : b.feedback);
        b.wow += b.wow_step * float(n);
        frames += n;
        shortest = d[1].delay_whole < shortest ? d[1].delay_whole : shortest;
        if (!same(out[0], out[1], sizeof out[0]) || !same_scalars(d[0], d[1])) {
            std::printf("long block %d n %d out %d state %d\n", block, n, int(same(out[0], out[1], sizeof out[0])), int(same_scalars(d[0], d[1])));
            CHECK(false);
            return;
        }
    }
    CHECK(same(long_rings[0], long_rings[1], sizeof long_rings[0]));
    CHECK(frames > 2 * kLongSize);   // the heads went round the rings twice
    CHECK(shortest <= 16);           // and the delay glided all the way down to its floor
}

TEST(ambientloops_the_hall_in_passes_is_bit_exact_against_the_frame_loop) {
    uint32_t seed = 0x13579BDFu;
    for (float &x : hall_rings[0]) x = 0.25f * noise(seed);
    std::memcpy(hall_rings[1], hall_rings[0], sizeof hall_rings[0]);

    strata_hall h[2];
    const float lengths[STRATA_HALL_LINES] = {2903.0f, 64.0f, 4093.0f, 4723.0f,
                                              64.25f, 1201.5f, 333.0f, 7001.0f};
    for (int k = 0; k < 2; k++) {
        h[k] = strata_hall{};
        h[k].lines = hall_rings[k];
        h[k].diffuse = hall_rings[k] + kHallLines;
        h[k].shimmer = hall_rings[k] + kHallLines + kHallDiffuse;
        h[k].write = 16350;           // near the end: every ring wraps in the run
        h[k].lfo_s = 0.0f;
        h[k].lfo_c = 1.0f;
        h[k].shimmer_phase = 0.3f;
        h[k].shimmer_out = 0.1f;
        for (int j = 0; j < STRATA_HALL_LINES; j++) {
            h[k].length[j] = lengths[j];
            h[k].damp[j] = 0.01f * float(j);
        }
    }
    strata_hall_block b{};
    const int diffuse[STRATA_HALL_DIFFUSERS] = {241, 347, 461, 587};
    for (int j = 0; j < STRATA_HALL_DIFFUSERS; j++) b.diffuse_len[j] = diffuse[j];
    for (int j = 0; j < STRATA_HALL_LINES; j++) b.gain[j] = 0.95f - 0.02f * float(j);
    b.damping = 0.6f;
    b.mod = 40.0f;
    b.rot_c = 0.99999f;
    b.rot_s = 0.0044721f;
    b.diffusion = 0.65f;
    b.shimmer = 0.4f;
    b.level = 0.9f;

    float in[64], out[2][2][64] = {};
    for (int block = 0; block < 700; block++) {
        const int n = 1 + (block * 7) % 64;
        // The lines glide: the short ones sit at the 64-frame floor, the long ones swing.
        for (int j = 0; j < STRATA_HALL_LINES; j++) {
            const bool floor = lengths[j] < 100.0f;
            b.length_step[j] = floor ? 0.0f : ((block / 40) % 2 == 0 ? 0.25f : -0.25f);
        }
        b.mod = (block / 90) % 2 == 0 ? 40.0f : 0.0f;
        for (int i = 0; i < n; i++) in[i] = noise(seed);
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < n; i++) out[k][0][i] = out[k][1][i] = -0.5f;
        }
        strata_hall_frames(&h[0], &b, in, out[0][0], out[0][1], n);
        strata_hall_passes(&h[1], &b, in, out[1][0], out[1][1], n);
        if (!same(out[0], out[1], sizeof out[0]) || !same_scalars(h[0], h[1])) {
            std::printf("hall block %d n %d out %d state %d\n", block, n, int(same(out[0], out[1], sizeof out[0])), int(same_scalars(h[0], h[1])));
            CHECK(false);
            return;
        }
    }
    CHECK(same(hall_rings[0], hall_rings[1], sizeof hall_rings[0]));
}
