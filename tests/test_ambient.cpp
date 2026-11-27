// The sends' ambient modes: the LONG delay and the HALL reverb (dsp/ambient.hpp), on their own
// and inside the mixer.
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "ambient.hpp"
#include "buses.hpp"
#include "chain.hpp"
#include "track.hpp"

#include <cstdio>

using namespace strata;
namespace ed = enaga::dsp;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kFrames = int(ENAGA_BLOCK_FRAMES);

// File scope: each holds megabytes of lines, and init() allocates.
LongDelay g_long;
HallReverb g_hall;

float abs_of(float x) { return x < 0.0f ? -x : x; }

LongDelay &long_delay(float seconds, float feedback, float wow, float diffusion, bool ping_pong) {
    static bool ready = false;
    if (!ready) {
        ready = g_long.init(kRate);
        CHECK(ready);
    }
    g_long.hold(false);
    // Clear it by running it silent at no feedback past its longest period, then set it up.
    g_long.set(0.05f, 0.0f, 1.0f, 20.0f, 0.0f, 0.0f, false, true);
    float in[kFrames] = {}, l[kFrames], r[kFrames];
    for (int b = 0; b < 4; b++) g_long.process(in, l, r, kFrames, 1.0f);
    for (int b = 0; b < int(LongDelay::kMaxSeconds * kRate) / kFrames + 2; b++) {
        g_long.process(in, l, r, kFrames, 1.0f);
    }
    g_long.set(seconds, feedback, 1.0f, 20.0f, wow, diffusion, ping_pong, true);
    for (int b = 0; b < 2; b++) g_long.process(in, l, r, kFrames, 1.0f);   // ramps settle
    return g_long;
}

// Runs `frames` of the delay with an impulse at frame 0, into two planes.
void impulse_response(LongDelay &d, float *left, float *right, int frames) {
    float in[kFrames];
    for (int at = 0; at < frames; at += kFrames) {
        for (int i = 0; i < kFrames; i++) in[i] = (at + i == 0) ? 1.0f : 0.0f;
        for (int i = 0; i < kFrames; i++) {
            left[at + i] = 0.0f;
            right[at + i] = 0.0f;
        }
        d.process(in, left + at, right + at, kFrames, 1.0f);
    }
}

int loudest(const float *x, int from, int to) {
    int best = from;
    for (int i = from; i < to; i++) best = abs_of(x[i]) > abs_of(x[best]) ? i : best;
    return best;
}

constexpr int kResponse = 4 * 48000;
float g_left[kResponse], g_right[kResponse];

}  // namespace

// --- the LONG delay on its own ----------------------------------------------------------------

TEST(ambient_long_delay_echoes_land_on_the_period) {
    // 0.5 s, so 24,000 frames: the all-passes are inside the loop and the read is shortened by
    // them, so the first echo and every one after land on the period with diffusion off ...
    LongDelay &d = long_delay(0.5f, 0.5f, 0.0f, 0.0f, false);
    impulse_response(d, g_left, g_right, kResponse);
    for (int i = 0; i < 24000; i++) CHECK(abs_of(g_left[i]) < 1e-6f);
    CHECK_EQ(loudest(g_left, 1000, 36000), 24000);
    CHECK_NEAR(g_left[24000], 1.0f, 1e-5);
    CHECK_NEAR(g_right[24000], 1.0f, 1e-5);         // no ping-pong: both sides
    const int second = loudest(g_left, 36000, 60000);
    CHECK(second >= 48000 && second <= 48002);      // the tone's one-pole lags a frame or so
    CHECK(abs_of(g_left[second]) < 0.55f && abs_of(g_left[second]) > 0.3f);
    CHECK_NEAR(d.loop_frames(), 24000.0f, 1e-3);
}

TEST(ambient_long_delay_diffusion_smears_without_moving_the_echo) {
    LongDelay &d = long_delay(0.5f, 0.5f, 0.0f, 1.0f, false);
    impulse_response(d, g_left, g_right, kResponse);
    // The feed skips the all-passes: nothing before the period, and the first echo clean on it.
    double early = 0.0;
    for (int i = 0; i < 24000; i++) early += double(g_left[i]) * g_left[i];
    CHECK(early < 1e-12);
    CHECK_NEAR(g_left[24000], 1.0f, 1e-5);
    // Each pass after it goes through them: the second echo is spread around its period, its
    // peak well under the undiffused 0.5 though its energy is still there.
    double second = 0.0;
    for (int i = 44000; i < 52000; i++) second += double(g_left[i]) * g_left[i];
    const float peak = abs_of(g_left[loudest(g_left, 44000, 52000)]);
    std::printf("    diffused second echo: peak %.3f, energy %.3f\n", double(peak), second);
    CHECK(peak < 0.35f);
    CHECK(second > 0.1);
}

TEST(ambient_long_delay_ping_pong_alternates_sides) {
    LongDelay &d = long_delay(0.25f, 0.6f, 0.0f, 0.0f, true);
    impulse_response(d, g_left, g_right, kResponse);
    CHECK_NEAR(g_left[12000], 1.0f, 1e-5);
    CHECK(abs_of(g_right[12000]) < 1e-6f);
    // The second pass is on the right only.
    double l2 = 0.0, r2 = 0.0;
    for (int i = 23000; i < 25000; i++) {
        l2 += double(g_left[i]) * g_left[i];
        r2 += double(g_right[i]) * g_right[i];
    }
    CHECK(r2 > 0.1);
    CHECK(l2 < 1e-9);
}

TEST(ambient_long_delay_reaches_twelve_seconds) {
    LongDelay &d = long_delay(LongDelay::kMaxSeconds, 0.5f, 0.0f, 0.0f, false);
    CHECK_NEAR(d.loop_frames(), LongDelay::kMaxSeconds * kRate, 1e-3);
    float in[kFrames], l[kFrames], r[kFrames];
    const int period = int(LongDelay::kMaxSeconds * kRate);
    int heard = -1;
    for (int at = 0; at < period + 4 * kFrames; at += kFrames) {
        for (int i = 0; i < kFrames; i++) {
            in[i] = at + i == 0 ? 1.0f : 0.0f;
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        d.process(in, l, r, kFrames, 1.0f);
        for (int i = 0; i < kFrames; i++) {
            if (heard < 0 && abs_of(l[i]) > 0.5f) heard = at + i;
        }
    }
    CHECK_EQ(heard, period);
}

TEST(ambient_long_delay_glides_to_a_new_time_while_ringing) {
    LongDelay &d = long_delay(0.5f, 0.5f, 0.0f, 0.0f, false);
    float in[kFrames] = {}, l[kFrames], r[kFrames];
    d.set(1.0f, 0.5f, 1.0f, 20.0f, 0.0f, 0.0f, false, false);    // not snapped: a glide
    d.process(in, l, r, kFrames, 1.0f);
    const float moved = d.loop_frames() - 24000.0f;
    CHECK_NEAR(moved, 0.5f * float(kFrames), 1e-3);              // half a frame a frame
    for (int b = 0; b < 2000; b++) d.process(in, l, r, kFrames, 1.0f);
    CHECK_NEAR(d.loop_frames(), 48000.0f, 1e-3);                 // and it lands exactly
}

TEST(ambient_long_delay_freeze_holds_the_loop_and_shuts_the_feed) {
    LongDelay &d = long_delay(0.25f, 0.3f, 0.6f, 0.6f, false);
    float in[kFrames], l[kFrames], r[kFrames];
    ed::Random noise(7);
    // A burst of noise, then a held freeze.
    for (int b = 0; b < 100; b++) {
        for (int i = 0; i < kFrames; i++) in[i] = 0.3f * noise.bipolar();
        d.process(in, l, r, kFrames, 1.0f);
    }
    d.hold(true);
    CHECK(d.held());
    // Measure one period of loop energy, then feed loud noise for a while, then measure again,
    // and again ten seconds on: the loop neither takes the new sound nor fades.
    const int period_blocks = 12000 / kFrames + 1;
    const auto energy = [&](bool feed) {
        double e = 0.0;
        for (int b = 0; b < period_blocks * 4; b++) {
            for (int i = 0; i < kFrames; i++) {
                in[i] = feed ? 0.9f * noise.bipolar() : 0.0f;
                l[i] = 0.0f;
                r[i] = 0.0f;
            }
            d.process(in, l, r, kFrames, 1.0f);
            for (int i = 0; i < kFrames; i++) e += double(l[i]) * l[i] + double(r[i]) * r[i];
        }
        return e;
    };
    energy(false);                                  // the fade of the feed and the wow
    const double held = energy(false);
    const double fed = energy(true);
    for (int k = 0; k < 40; k++) energy(false);
    const double later = energy(false);
    std::printf("    freeze: held %.2f, while fed %.2f, ten seconds on %.2f\n", held, fed, later);
    CHECK(held > 1.0);
    CHECK(fed / held > 0.97 && fed / held < 1.03);
    CHECK(later / held > 0.9 && later / held < 1.03);
    // Released, the feedback setting takes over again and it dies away.
    d.hold(false);
    for (int k = 0; k < 20; k++) energy(false);
    CHECK(energy(false) < 1e-3 * held);
}

TEST(ambient_long_delay_stays_bounded_at_every_extreme) {
    // Full feedback, full diffusion, full wow, the brightest tone and no low cut, fed full-scale
    // noise for twenty seconds: what goes round is soft-ceilinged, so nothing runs away.
    LongDelay &d = long_delay(0.3f, 0.95f, 1.0f, 1.0f, true);
    d.set(0.3f, 0.95f, 1.0f, 20.0f, 1.0f, 1.0f, true, false);
    float in[kFrames], l[kFrames], r[kFrames];
    ed::Random noise(11);
    float peak = 0.0f;
    for (int b = 0; b < 15000; b++) {
        for (int i = 0; i < kFrames; i++) {
            in[i] = noise.bipolar();
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        d.process(in, l, r, kFrames, 1.0f);
        for (int i = 0; i < kFrames; i++) {
            CHECK(l[i] == l[i]);                     // no NaN
            peak = abs_of(l[i]) > peak ? abs_of(l[i]) : peak;
            peak = abs_of(r[i]) > peak ? abs_of(r[i]) : peak;
        }
    }
    std::printf("    long delay worst peak %.3f\n", double(peak));
    CHECK(peak < 4.5f);           // a full-scale feed plus a loop held under 2
}

// --- the HALL reverb on its own ---------------------------------------------------------------

namespace {

HallReverb &hall(float decay, float damping, float size, float mod, float shimmer) {
    static bool ready = false;
    if (!ready) {
        ready = g_hall.init(kRate);
        CHECK(ready);
    }
    // Empty it: no decay to speak of, run past the longest line.
    g_hall.set(0.2f, 1.0f, size, 0.0f, 0.0f, true);
    float in[kFrames] = {}, l[kFrames], r[kFrames];
    for (int b = 0; b < 2000; b++) g_hall.process(in, l, r, kFrames, 1.0f);
    g_hall.set(decay, damping, size, mod, shimmer, true);
    return g_hall;
}

// The tail's energy, both sides, over [from, to) seconds after an impulse.
void hall_energies(HallReverb &h, const float *edges, int count, double *out) {
    float in[kFrames], l[kFrames], r[kFrames];
    int edge = 0;
    for (int k = 0; k < count - 1; k++) out[k] = 0.0;
    const int total = int(edges[count - 1] * kRate);
    for (int at = 0; at < total; at += kFrames) {
        for (int i = 0; i < kFrames; i++) {
            in[i] = at + i == 0 ? 1.0f : 0.0f;
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        h.process(in, l, r, kFrames, 1.0f);
        const float t = float(at) / kRate;
        while (edge < count - 1 && t >= edges[edge + 1]) edge++;
        if (edge < count - 1 && t >= edges[edge]) {
            for (int i = 0; i < kFrames; i++) out[edge] += double(l[i]) * l[i] + double(r[i]) * r[i];
        }
    }
}

}  // namespace

TEST(ambient_hall_decays_at_its_decay_time) {
    // RT60 of 2 s: from the first half second to the window two seconds later, about -60 dB.
    HallReverb &h = hall(2.0f, 0.3f, 1.0f, 0.4f, 0.0f);
    const float edges[] = {0.2f, 0.7f, 2.2f, 2.7f};
    double e[3];
    hall_energies(h, edges, 4, e);
    const double drop_db = 10.0 * __builtin_log10(e[2] / e[0]);
    std::printf("    hall RT60 2 s: %.1f dB over 2 s\n", drop_db);
    CHECK(e[0] > 1e-4);
    CHECK(drop_db < -45.0 && drop_db > -80.0);

    // And a 20 s decay barely falls over the same span.
    HallReverb &long_hall = hall(20.0f, 0.3f, 1.0f, 0.4f, 0.0f);
    hall_energies(long_hall, edges, 4, e);
    const double long_drop = 10.0 * __builtin_log10(e[2] / e[0]);
    CHECK(long_drop > -12.0);
}

TEST(ambient_hall_is_stereo) {
    HallReverb &h = hall(3.0f, 0.3f, 1.0f, 0.5f, 0.0f);
    float in[kFrames], l[kFrames], r[kFrames];
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (int at = 0; at < 48000; at += kFrames) {
        for (int i = 0; i < kFrames; i++) {
            in[i] = at + i == 0 ? 1.0f : 0.0f;
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        h.process(in, l, r, kFrames, 1.0f);
        for (int i = 0; i < kFrames; i++) {
            ll += double(l[i]) * l[i];
            rr += double(r[i]) * r[i];
            lr += double(l[i]) * r[i];
        }
    }
    const double correlation = lr / __builtin_sqrt(ll * rr);
    std::printf("    hall L/R correlation %.3f\n", correlation);
    CHECK(ll > 1e-3 && rr > 1e-3);
    CHECK(correlation < 0.5 && correlation > -0.5);
}

TEST(ambient_hall_shimmer_adds_to_the_tail_and_stays_bounded) {
    const float edges[] = {0.0f, 1.0f, 4.0f, 6.0f};
    double plain[3], shimmer[3];
    hall_energies(hall(4.0f, 0.3f, 1.0f, 0.4f, 0.0f), edges, 4, plain);
    hall_energies(hall(4.0f, 0.3f, 1.0f, 0.4f, 1.0f), edges, 4, shimmer);
    std::printf("    shimmer tail %.3g against %.3g plain\n", shimmer[2], plain[2]);
    CHECK(shimmer[2] > 1.5 * plain[2]);

    // The worst case: the longest decay, no damping, full shimmer, fed noise for thirty seconds.
    HallReverb &h = hall(30.0f, 0.0f, HallReverb::kMaxSize, 1.0f, 1.0f);
    float in[kFrames], l[kFrames], r[kFrames];
    ed::Random noise(3);
    float peak = 0.0f;
    for (int b = 0; b < 22500; b++) {
        for (int i = 0; i < kFrames; i++) {
            in[i] = 0.5f * noise.bipolar();
            l[i] = 0.0f;
            r[i] = 0.0f;
        }
        h.process(in, l, r, kFrames, 1.0f);
        for (int i = 0; i < kFrames; i++) {
            CHECK(l[i] == l[i]);
            peak = abs_of(l[i]) > peak ? abs_of(l[i]) : peak;
        }
    }
    std::printf("    hall worst peak %.3f\n", double(peak));
    CHECK(peak < 8.0f);

    // And a short decay with full shimmer still dies away.
    const float late[] = {0.0f, 1.0f, 20.0f, 21.0f};
    double e[3];
    hall_energies(hall(2.0f, 0.5f, 1.0f, 0.4f, 1.0f), late, 4, e);
    CHECK(e[2] < 1e-6 * e[0]);
}

// --- inside the mixer -------------------------------------------------------------------------

namespace {

// The FX page's pan and send knobs (core/factory.cpp names them the same way).
constexpr int kPan = 13, kSna = 14, kSnb = 15;

struct Rig {
    Mixer mixer;
    SendSettings sends;
    MasterSettings master;
    TrackParams params{};
    ChainMix mix;
    float out[2 * kFrames] = {};

    void setup() {
        mixer.init(kRate);
        // Every setting back to its default, not just the ones set below: the rig is shared, and
        // a test that switched the long delay or the hall on handed it to the next one.
        sends = SendSettings{};
        master = MasterSettings{};
        for (int i = 0; i < kParams; i++) params.set(i, 0);
        params.set(kPan, 64);
        params.set(kSna, 127);
        params.set(kSnb, 127);
        sends.delay_seconds = 0.25f;
        sends.delay_feedback = 0.5f;
        master.compressor = 0.0f;
        master.limit = 0.98f;
    }

    // One block with `level` of a click on its first frame, on track 0; the output's energy.
    double run(float level) {
        mixer.prepare(sends, master);
        mixer.chain(0).prepare(params, mix);
        mixer.begin_block(kFrames);
        float *mono = mixer.scratch();
        for (int i = 0; i < kFrames; i++) mono[i] = i == 0 ? level : 0.0f;
        mixer.add_track(0, mono);
        mixer.finish_block(out, kFrames);
        double e = 0.0;
        for (int i = 0; i < 2 * kFrames; i++) e += double(out[i]) * out[i];
        return e;
    }
};

Rig g_rig;

}  // namespace

TEST(ambient_mixer_runs_only_the_mode_that_is_on_and_rings_the_other_out) {
    Rig &rig = g_rig;
    rig.setup();
    rig.run(0.5f);
    CHECK(rig.mixer.classic_delay_running());
    CHECK(!rig.mixer.long_delay_running());
    CHECK(rig.mixer.classic_reverb_running());
    CHECK(!rig.mixer.hall_running());

    // Switched to the ambient modes: they are fed, the classic ones ring out unfed.
    rig.sends.delay_long = true;
    rig.sends.reverb_hall = true;
    rig.run(0.5f);
    CHECK(rig.mixer.long_delay_running());
    CHECK(rig.mixer.hall_running());
    CHECK(rig.mixer.classic_delay_running());
    // Both sends closed on the track: a send gain above zero is what "fed" means to the mixer.
    rig.params.set(kSna, 0);
    rig.params.set(kSnb, 0);
    // The track silent, the tails end in their own time: the classic ones first, as nothing
    // feeds them any more.
    for (int b = 0; b < 4000 && rig.mixer.classic_delay_running(); b++) rig.run(0.0f);
    CHECK(!rig.mixer.classic_delay_running());
    for (int b = 0; b < 40000 && rig.mixer.delay_running(); b++) rig.run(0.0f);
    for (int b = 0; b < 40000 && rig.mixer.reverb_running(); b++) rig.run(0.0f);
    CHECK(!rig.mixer.delay_running());
    CHECK(!rig.mixer.reverb_running());
    // And silent, since nothing runs: an idle ambient mode costs nothing.
    CHECK(rig.run(0.0f) == 0.0);
}

TEST(ambient_mixer_long_echo_follows_division_times_multiplier) {
    Rig &rig = g_rig;
    rig.setup();
    rig.sends.delay_long = true;
    rig.sends.reverb_level = 0.0f;
    rig.sends.delay_diffusion = 0.0f;
    rig.sends.delay_wow = 0.0f;
    rig.sends.delay_ping_pong = false;
    rig.sends.delay_seconds = 0.25f;
    rig.sends.delay_multiplier = 4.0f;           // one second
    CHECK_NEAR(effective_delay_seconds(rig.sends), 1.0f, 1e-6);
    // The click, then silence until the echo a second on.
    rig.run(0.5f);
    const int echo_block = 48000 / kFrames;
    double before = 0.0, at = 0.0;
    for (int b = 1; b < echo_block; b++) before += rig.run(0.0f);
    at = rig.run(0.0f);
    CHECK(before < 1e-12);
    CHECK(at > 1e-3);
    // A slow tempo's 1/1 times 8 is clamped to the 12 s the line holds.
    rig.sends.delay_seconds = 3.0f;
    rig.sends.delay_multiplier = 8.0f;
    CHECK_NEAR(effective_delay_seconds(rig.sends), Mixer::kMaxLongDelaySeconds, 1e-6);
    rig.sends.delay_long = false;
    CHECK_NEAR(effective_delay_seconds(rig.sends), Mixer::kMaxDelaySeconds, 1e-6);
}

TEST(ambient_mixer_freeze_goes_to_the_long_delay_in_long_mode) {
    Rig &rig = g_rig;
    rig.setup();
    rig.sends.delay_long = true;
    rig.run(0.5f);
    rig.mixer.freeze_delay(true);
    CHECK(rig.mixer.delay_frozen());
    CHECK(rig.mixer.long_delay().held());
    CHECK(!rig.mixer.freeze().held());
    // The mode switched while held: the release still reaches the long delay.
    rig.sends.delay_long = false;
    rig.run(0.0f);
    rig.mixer.freeze_delay(false);
    CHECK(!rig.mixer.delay_frozen());
    CHECK(!rig.mixer.long_delay().held());
    // In the classic mode the classic freeze takes the press, as before.
    rig.mixer.freeze_delay(true);
    CHECK(rig.mixer.freeze().held());
    CHECK(!rig.mixer.long_delay().held());
    rig.mixer.freeze_delay(false);
    CHECK(!rig.mixer.delay_frozen());
}

TEST(ambient_mixer_long_delay_feeds_the_hall) {
    Rig &rig = g_rig;
    rig.setup();
    rig.params.set(kSnb, 0);                     // nothing reaches the reverb but the delay
    rig.sends.delay_long = true;
    rig.sends.reverb_hall = true;
    rig.sends.delay_to_reverb = 0.0f;
    for (int b = 0; b < 20000 && rig.mixer.reverb_running(); b++) rig.run(0.0f);
    rig.run(0.5f);
    CHECK(!rig.mixer.hall_running());
    rig.sends.delay_to_reverb = 1.0f;
    for (int b = 0; b < 300; b++) rig.run(0.0f);
    CHECK(rig.mixer.hall_running());
}
