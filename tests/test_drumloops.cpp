// dsp/drumloops.ti.c: the hat's fused back half against the three passes it replaced.
//
// strata_hat_back exists for cl6x's sake (two filter recurrences in one loop overlap), and it must
// not change a sample to get there. The passes it replaced are still in the build, so the test
// runs them beside it -- every mode the hat uses, odd and short lengths, loud input that drives
// the VCA's clip, state carried from one call into the next -- and compares with `==`.
#include "check.hpp"

#include <cstring>

#include <enaga/dsp.h>

#include "drumloops.h"
#include "filterloops.h"

namespace {

void signal(float *x, int n, uint32_t &s) {
    for (int i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        x[i] = float(int32_t(s)) * (1.5f / 2147483648.0f);
    }
}

enaga_svf_coefficients coefficients(float g, float k) {
    enaga_svf_coefficients c;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    c.k = k;
    c.a1 = a1;
    c.a2 = g * a1;
    c.a3 = g * c.a2;
    return c;
}

}  // namespace

TEST(drumloops_the_hat_back_half_is_bit_exact_against_its_three_passes) {
    const enaga_svf_coefficients band = coefficients(0.9f, 0.6f), high = coefficients(0.3f, 1.4f);
    const int lengths[] = {1, 2, 7, 16, 63, 64, 0, 33};
    enaga_svf a_band{}, a_high{}, b_band{}, b_high{};
    uint32_t seed = 0xC0FFEEu;
    float level = 0.3f;
    for (int round = 0; round < 40; round++) {
        for (const int n : lengths) {
            float x[64], env[64], passes[64], fused[64];
            signal(x, n, seed);
            for (int i = 0; i < n; i++) env[i] = 1.0f - float(i) * (1.0f / 64.0f);
            std::memcpy(passes, x, sizeof x);
            std::memcpy(fused, x, sizeof x);

            strata_svf_inplace(&a_band, &band, ENAGA_SVF_BAND, passes, n);
            strata_hat_vca(passes, env, n, level);
            strata_svf_inplace(&a_high, &high, ENAGA_SVF_HIGH, passes, n);

            strata_hat_back(fused, env, n, &b_band, &band, &b_high, &high, level);

            CHECK(std::memcmp(passes, fused, sizeof(float) * size_t(n)) == 0);
            CHECK(std::memcmp(&a_band, &b_band, sizeof a_band) == 0);
            CHECK(std::memcmp(&a_high, &b_high, sizeof a_high) == 0);
        }
        level = level < 1.0f ? level + 0.05f : 0.3f;
    }
}

TEST(drumloops_the_rng_skip_leaves_the_state_noise_would) {
    // KICK steps its noise generator with strata_rng_skip while its click path is spent, so the
    // next hit draws the noise it always did: the state must be exactly what enaga_noise_add
    // leaves after the same number of samples.
    uint32_t a = 0x5F2A41C7u, b = a;
    float scratch[64] = {};
    const int lengths[] = {1, 64, 0, 17, 63, 2};
    for (int round = 0; round < 50; round++) {
        for (const int n : lengths) {
            enaga_noise_add(&a, scratch, n, 1.0f);
            strata_rng_skip(&b, n);
            CHECK_EQ(a, b);
        }
    }
}
