// dsp/filterloops.ti.c: STRATA's in-place filter kernels against the Enaga block functions they
// stand in for.
//
// The kernels exist for cl6x's sake (one restrict buffer instead of an `in` and an `out` that may
// alias, and two channels in one loop), and they must not change a single sample to get there:
// every call site that moved to them -- the insert chain, the DJ filter, the reverb's high-pass,
// KICK, SNARE, HAT and the VA voice -- is pinned by the golden renders, and these tests pin the
// kernels themselves with `==`, over every output, odd and even lengths, a zero length, loud and
// resonant settings, and state carried from one call into the next.
#include "check.hpp"

#include <cstring>

#include <enaga/dsp.h>

#include "filterloops.h"

namespace {

constexpr int kMax = 67;    // longer than a block, and odd

// A deterministic test signal with transients and a DC step: the filters ring on it.
void signal(float *x, int n, uint32_t seed) {
    uint32_t s = seed;
    for (int i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        x[i] = float(int32_t(s)) * (1.0f / 2147483648.0f) + (i > n / 2 ? 0.75f : -0.25f);
    }
}

bool same(const float *a, const float *b, int n) { return std::memcmp(a, b, sizeof(float) * size_t(n)) == 0; }

bool same_svf(const enaga_svf &a, const enaga_svf &b) { return std::memcmp(&a, &b, sizeof a) == 0; }

enaga_svf_coefficients coefficients(float g, float k) {
    enaga_svf_coefficients c;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    c.k = k;
    c.a1 = a1;
    c.a2 = g * a1;
    c.a3 = g * c.a2;
    return c;
}

const int kLengths[] = {0, 1, 2, 3, 16, 17, 64, kMax};
const float kG[] = {0.01f, 0.3f, 2.5f};
const float kK[] = {2.0f, 0.4f, 0.02f};    // none, some, nearly self-oscillating

}  // namespace

TEST(filterloops_the_in_place_svf_is_bit_exact_against_enaga_svf_process) {
    for (int output = 0; output < 3; output++) {
        for (float g : kG) {
            for (float k : kK) {
                const enaga_svf_coefficients c = coefficients(g, k);
                enaga_svf ours{0.1f, -0.2f}, theirs{0.1f, -0.2f};
                for (int n : kLengths) {    // one filter through every length: state carries
                    float a[kMax], b[kMax];
                    signal(a, n, 0x1234u + uint32_t(n));
                    std::memcpy(b, a, sizeof a);
                    enaga_svf_process(&theirs, &c, output, a, a, n);
                    strata_svf_inplace(&ours, &c, output, b, n);
                    CHECK(same(a, b, n));
                    CHECK(same_svf(ours, theirs));
                }
            }
        }
    }
}

TEST(filterloops_the_in_place_cascade_is_bit_exact_against_enaga_svf_cascade) {
    for (int output = 0; output < 3; output++) {
        for (float g : kG) {
            for (float k : kK) {
                // Two different stages, as the VA voice's LP24 has (a fixed k on the first).
                const enaga_svf_coefficients c1 = coefficients(g, 1.0f), c2 = coefficients(g, k);
                enaga_svf o1{}, o2{0.05f, 0.0f}, t1{}, t2{0.05f, 0.0f};
                for (int n : kLengths) {
                    float a[kMax], b[kMax];
                    signal(a, n, 0x9876u + uint32_t(n));
                    std::memcpy(b, a, sizeof a);
                    enaga_svf_cascade(&t1, &c1, &t2, &c2, output, a, a, n);
                    strata_svf_cascade_inplace(&o1, &c1, &o2, &c2, output, b, n);
                    CHECK(same(a, b, n));
                    CHECK(same_svf(o1, t1));
                    CHECK(same_svf(o2, t2));
                }
            }
        }
    }
}

TEST(filterloops_the_stereo_cascade_is_two_enaga_cascades) {
    for (int output = 0; output < 3; output++) {
        const enaga_svf_coefficients c = coefficients(0.4f, 0.3f);
        enaga_svf ol1{}, ol2{}, or1{0.3f, 0.1f}, or2{}, tl1{}, tl2{}, tr1{0.3f, 0.1f}, tr2{};
        for (int n : kLengths) {
            float l[kMax], r[kMax], l2[kMax], r2[kMax];
            signal(l, n, 0x55u + uint32_t(n));
            signal(r, n, 0xAAu + uint32_t(n));    // different channels, different states
            std::memcpy(l2, l, sizeof l);
            std::memcpy(r2, r, sizeof r);
            enaga_svf_cascade(&tl1, &c, &tl2, &c, output, l, l, n);
            enaga_svf_cascade(&tr1, &c, &tr2, &c, output, r, r, n);
            strata_svf_cascade_stereo_inplace(&ol1, &ol2, &or1, &or2, &c, &c, output, l2, r2, n);
            CHECK(same(l, l2, n));
            CHECK(same(r, r2, n));
            CHECK(same_svf(ol1, tl1) && same_svf(ol2, tl2));
            CHECK(same_svf(or1, tr1) && same_svf(or2, tr2));
        }
    }
}

TEST(filterloops_the_in_place_one_pole_and_biquad_are_bit_exact_against_enaga) {
    const float poles[] = {0.005f, 0.05f, 0.9f};
    for (float a : poles) {
        enaga_one_pole ours{0.3f}, theirs{0.3f};
        for (int n : kLengths) {
            float x[kMax], y[kMax];
            signal(x, n, 0x777u + uint32_t(n));
            std::memcpy(y, x, sizeof x);
            enaga_one_pole_process(&theirs, a, x, x, n);
            strata_one_pole_inplace(&ours, a, y, n);
            CHECK(same(x, y, n));
            CHECK_EQ(ours.y, theirs.y);
        }
    }
    for (int type = ENAGA_BIQUAD_LOW_PASS; type <= ENAGA_BIQUAD_HIGH_PASS; type++) {
        enaga_biquad_coefficients c;
        enaga_biquad_design(&c, type, 180.0f, 0.707f, 0.0f, 48000.0f);
        enaga_biquad ours{}, theirs{};
        for (int n : kLengths) {
            float x[kMax], y[kMax];
            signal(x, n, 0x4242u + uint32_t(n));
            std::memcpy(y, x, sizeof x);
            enaga_biquad_process(&theirs, &c, x, x, n);
            strata_biquad_inplace(&ours, &c, y, n);
            CHECK(same(x, y, n));
            CHECK(std::memcmp(&ours, &theirs, sizeof ours) == 0);
        }
    }
}
