// Host tests for the master kill EQ (dsp/buses.hpp, dsp/masterloops.ti.c).
//
// What a DJ isolator has to do: flat is a wire (bypassed, bit-exact), a killed band is gone, the
// other two bands are untouched while it is, the three bands at unity sum flat, and neither a
// kill nor its return clicks. Gains glide for a few blocks and the stage fades in and out, so
// every measurement here lets it settle first.
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "buses.hpp"
#include "chain.hpp"
#include "masterloops.h"
#include "track.hpp"

using namespace strata;
namespace ed = enaga::dsp;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kFrames = int(ENAGA_BLOCK_FRAMES);
constexpr int kPan = 13;

TrackParams hard_left() {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, 0);
    p.set(kPan, 0);      // hard left: the left output is the track, exactly
    return p;
}

MasterSettings transparent_master() {
    MasterSettings master;
    master.compressor = 0.0f;
    master.limit = 0.98f;
    master.level = 1.0f;
    return master;
}

SendSettings silent_sends() {
    SendSettings sends;
    sends.delay_level = 0.0f;
    sends.delay_to_reverb = 0.0f;
    sends.reverb_level = 0.0f;
    return sends;
}

// A sine on track 0, block `block` of it, through one mixer block. Returns the left channel's
// energy and fills `left` with it.
struct Rig {
    Mixer mixer;
    SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();
    TrackParams params = hard_left();
    ChainMix mix;
    float out[2 * kFrames] = {};
    float in[kFrames] = {};
    int block = 0;

    Rig() { mixer.init(kRate); }

    void run(float hz, float level) {
        for (int i = 0; i < kFrames; i++) {
            const float t = float(block * kFrames + i) * hz / kRate;
            in[i] = level * ed::sin_turns(t - float(int(t)));
        }
        mixer.prepare(sends, master);
        mixer.chain(0).prepare(params, mix);
        mixer.begin_block(kFrames);
        float *mono = mixer.scratch();
        for (int i = 0; i < kFrames; i++) mono[i] = in[i];
        mixer.add_track(0, mono);
        mixer.finish_block(out, kFrames);
        block++;
    }

    // The ratio of output to input energy over `blocks` blocks, after `settle` blocks.
    float gain_db(float hz, int settle = 40, int blocks = 40) {
        for (int b = 0; b < settle; b++) run(hz, 0.5f);
        double in_e = 0.0, out_e = 0.0;
        for (int b = 0; b < blocks; b++) {
            run(hz, 0.5f);
            for (int i = 0; i < kFrames; i++) {
                in_e += double(in[i]) * in[i];
                out_e += double(out[2 * i]) * out[2 * i];
            }
        }
        const float ratio = float(out_e / in_e);
        return 10.0f * ed::log2(ratio < 1e-12f ? 1e-12f : ratio) * 0.30103f;
    }
};

}  // namespace

// Flat is a wire: at the default settings the stage never runs, so the output is the input to
// the bit -- which is what keeps every render that never touches the EQ unchanged.
TEST(killeq_flat_is_bypassed_and_bit_exact) {
    Rig rig;
    for (int b = 0; b < 8; b++) {
        rig.run(60.0f, 0.5f);
        CHECK(!rig.mixer.kill_eq_running());
        if (b < 2) continue;   // the insert chain's own gains ramp across its first block
        for (int i = 0; i < kFrames; i++) {
            CHECK_EQ(rig.out[2 * i], rig.in[i]);
            CHECK_EQ(rig.out[2 * i + 1], 0.0f);
        }
    }
}

// A killed band is gone and the others pass. One sine per band: 60 Hz, 1 kHz and 8 kHz, each a
// good way from the 250 Hz and 2.5 kHz crossovers.
TEST(killeq_each_kill_takes_out_its_band_and_leaves_the_others) {
    const float freqs[kKillBands] = {60.0f, 1000.0f, 8000.0f};
    for (int killed = 0; killed < kKillBands; killed++) {
        for (int probe = 0; probe < kKillBands; probe++) {
            Rig rig;
            rig.master.kills = uint8_t(1u << killed);
            const float db = rig.gain_db(freqs[probe]);
            if (probe == killed) {
                if (!(db < -30.0f)) std::printf("    kill %d at %g Hz: %g dB\n", killed, double(freqs[probe]), double(db));
                CHECK(db < -30.0f);
            } else {
                if (!(db > -1.5f && db < 0.5f)) std::printf("    kill %d at %g Hz: %g dB\n", killed, double(freqs[probe]), double(db));
                CHECK(db > -1.5f);
                CHECK(db < 0.5f);
            }
        }
    }
}

// A knob at 0 is the same kill as the switch, and a knob at the top boosts by 6 dB.
TEST(killeq_the_knobs_reach_a_kill_and_a_boost) {
    {
        Rig rig;
        rig.master.eq_mid = master_value_from_byte(uint8_t(MasterField::eq_mid), 0);
        CHECK(rig.gain_db(1000.0f) < -30.0f);
    }
    {
        Rig rig;
        rig.master.eq_low = master_value_from_byte(uint8_t(MasterField::eq_low), 127);
        const float db = rig.gain_db(60.0f);
        CHECK(db > 5.0f);
        CHECK(db < 6.5f);
    }
}

// The three bands at unity sum flat. Run the loop itself with the stage fully in and every gain
// at 1, across the audio band: an isolator that dipped at its crossovers would colour every
// sound the moment one knob left the centre.
TEST(killeq_the_bands_at_unity_sum_flat) {
    const ed::SvfCoefficients low = ed::SvfCoefficients::from_g(ed::tan_pi(kKillLowHz / kRate), 1.41421356f);
    const ed::SvfCoefficients high = ed::SvfCoefficients::from_g(ed::tan_pi(kKillHighHz / kRate), 1.41421356f);
    const float freqs[] = {40.0f, 150.0f, 250.0f, 400.0f, 1000.0f, 2500.0f, 4000.0f, 12000.0f};
    for (float hz : freqs) {
        float sl[STRATA_KILL_EQ_STATES] = {}, sr[STRATA_KILL_EQ_STATES] = {};
        const float gains[3] = {1.0f, 1.0f, 1.0f}, steps[3] = {};
        double in_e = 0.0, out_e = 0.0;
        for (int b = 0; b < 200; b++) {
            float l[kFrames], r[kFrames];
            for (int i = 0; i < kFrames; i++) {
                const float t = float(b * kFrames + i) * hz / kRate;
                l[i] = r[i] = 0.5f * ed::sin_turns(t - float(int(t)));
            }
            float x[kFrames];
            for (int i = 0; i < kFrames; i++) x[i] = l[i];
            strata_kill_eq(l, r, kFrames, sl, sr, &low.a1, &high.a1, gains, steps, 1.0f, 0.0f);
            if (b < 100) continue;
            for (int i = 0; i < kFrames; i++) {
                in_e += double(x[i]) * x[i];
                out_e += double(l[i]) * l[i];
                CHECK_EQ(l[i], r[i]);
            }
        }
        const float db = 10.0f * ed::log2(float(out_e / in_e)) * 0.30103f;
        if (!(db > -0.2f && db < 0.2f)) std::printf("    %g Hz: %g dB\n", double(hz), double(db));
        CHECK(db > -0.2f);
        CHECK(db < 0.2f);
    }
}

// A kill does not click, and nor does its return. On a full-scale 50 Hz sine a hard switch would
// move one sample by up to the whole amplitude; the glide halves the gain over a 64-frame ramp,
// which adds at most half the amplitude over 64 samples to the sine's own slope. So no sample may
// move more than three times as far as the sine itself can (0.02 of full scale, against 1.0 for
// a hard switch).
TEST(killeq_a_kill_and_its_return_do_not_click) {
    Rig rig;
    const float max_slope = 2.0f * 3.14159265f * 50.0f / kRate;   // per sample, at level 1
    float last = 0.0f;
    float worst = 0.0f;
    for (int b = 0; b < 120; b++) {
        rig.master.kills = (b >= 20 && b < 60) ? uint8_t(1) : uint8_t(0);
        rig.run(50.0f, 1.0f);
        for (int i = 0; i < kFrames; i++) {
            const float d = rig.out[2 * i] - last;
            const float a = d < 0.0f ? -d : d;
            worst = a > worst ? a : worst;
            last = rig.out[2 * i];
        }
    }
    if (!(worst < 3.0f * max_slope)) std::printf("    worst %g, slope %g\n", double(worst), double(max_slope));
    CHECK(worst < 3.0f * max_slope);
}

// And once a kill is let go, the stage glides back and BYPASSES again: the output is the input to
// the bit, so an EQ used once in a set costs nothing for the rest of it.
TEST(killeq_returns_to_bypass_after_a_kill) {
    Rig rig;
    rig.master.kills = 0x1;
    for (int b = 0; b < 10; b++) rig.run(60.0f, 0.5f);
    CHECK(rig.mixer.kill_eq_running());
    rig.master.kills = 0;
    int blocks = 0;
    while (rig.mixer.kill_eq_running() && blocks < 100) {
        rig.run(60.0f, 0.5f);
        blocks++;
    }
    CHECK(!rig.mixer.kill_eq_running());
    CHECK(blocks < 40);   // ~20 blocks of glide and 4 of fade
    rig.run(60.0f, 0.5f);
    for (int i = 0; i < kFrames; i++) CHECK_EQ(rig.out[2 * i], rig.in[i]);
}

// The byte domain: 64 is exactly flat, 0 exactly a kill, 127 exactly +6 dB, and halfway down is
// -12 dB.
TEST(killeq_the_band_bytes_map_onto_kill_flat_and_boost) {
    for (int f = int(MasterField::eq_low); f <= int(MasterField::eq_high); f++) {
        CHECK_EQ(master_value_from_byte(uint8_t(f), 64), 1.0f);
        CHECK_EQ(master_value_from_byte(uint8_t(f), 0), 0.0f);
        CHECK_EQ(master_value_from_byte(uint8_t(f), 127), 2.0f);
        CHECK_NEAR(master_value_from_byte(uint8_t(f), 32), 0.25f, 1e-6);
        // And the bytes a fresh project holds come out flat, so a new project is bypassed.
        MasterSettings m;
        const ProjectSettings defaults;
        master_from_bytes(m, defaults.master);
        CHECK_EQ(m.eq_low, 1.0f);
        CHECK_EQ(m.eq_mid, 1.0f);
        CHECK_EQ(m.eq_high, 1.0f);
        CHECK_EQ(int(m.kills), 0);
    }
}
