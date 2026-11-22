// The tape stop and the delay freeze (dsp/tapestop.hpp, dsp/freeze.hpp, dsp/stoploops.ti.c).
//
// What each has to do. A tape stop costs nothing idle, does not change the sound on the press,
// slows and drops in pitch, ends in true silence, and comes back to the live master without a
// click and then bypasses again. A freeze records one echo period, loops exactly that, cuts the
// delay's feed while held, and crossfades back to the delay's own tail on release.
//
// The rings are megabytes, so the objects live at file scope (CLAUDE.md caps a stack frame).
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "buses.hpp"
#include "chain.hpp"
#include "freeze.hpp"
#include "tapestop.hpp"
#include "track.hpp"

#include <cstdio>
#include <initializer_list>

using namespace strata;
namespace ed = enaga::dsp;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kFrames = int(ENAGA_BLOCK_FRAMES);

TapeStop g_tape;
DelayFreeze g_freeze;

TapeStop &tape() {
    static bool ready = false;
    if (!ready) {
        ready = g_tape.init(kRate);
        CHECK(ready);
    }
    g_tape.hold(false);
    // Run the release out, so every test starts idle.
    float silence[2 * kFrames] = {};
    for (int b = 0; b < 64 && g_tape.running(); b++) g_tape.process(silence, kFrames);
    return g_tape;
}

// A 220 Hz sine on both channels, block `b`.
void sine_block(float *io, int b, float hz = 220.0f) {
    for (int i = 0; i < kFrames; i++) {
        const float t = float(b * kFrames + i) * hz / kRate;
        io[2 * i] = io[2 * i + 1] = 0.5f * ed::sin_turns(t - float(int(t)));
    }
}

int sign_changes(const float *io, int n, float &last) {
    int changes = 0;
    for (int i = 0; i < n; i++) {
        const float x = io[2 * i];
        if ((x < 0.0f) != (last < 0.0f)) changes++;
        last = x;
    }
    return changes;
}

}  // namespace

TEST(tapestop_idle_is_a_wire) {
    TapeStop &t = tape();
    float io[2 * kFrames], ref[2 * kFrames];
    for (int b = 0; b < 8; b++) {
        sine_block(io, b);
        for (int i = 0; i < 2 * kFrames; i++) ref[i] = io[i];
        t.process(io, kFrames);
        for (int i = 0; i < 2 * kFrames; i++) CHECK_EQ(io[i], ref[i]);
    }
    CHECK(!t.running());
}

TEST(tapestop_slows_drops_in_pitch_and_ends_in_silence) {
    TapeStop &t = tape();
    t.set(0.5f * kRate);                         // half a second to a standstill
    t.hold(true);
    float io[2 * kFrames];
    float x0 = 0.0f;

    // The press itself is seamless: the first frame is the live one.
    sine_block(io, 0);
    x0 = io[0];
    t.process(io, kFrames);
    CHECK_EQ(io[0], x0);

    // A quarter of the way in, the live sine crosses zero more often than the tape does; three
    // quarters in, the tape crosses far less.
    const int blocks = int(0.5f * kRate) / kFrames;
    float live_last = 0.0f, tape_last = 0.0f;
    int live = 0, slow = 0;
    for (int b = 1; b < blocks + 40; b++) {
        sine_block(io, b);
        if (b > blocks / 2 && b < blocks * 3 / 4) live += sign_changes(io, kFrames, live_last);
        t.process(io, kFrames);
        if (b > blocks / 2 && b < blocks * 3 / 4) slow += sign_changes(io, kFrames, tape_last);
    }
    if (!(slow * 2 < live)) std::printf("    live %d, tape %d crossings\n", live, slow);
    CHECK(slow * 2 < live);

    // Past the stop, silence, exactly.
    sine_block(io, blocks + 40);
    t.process(io, kFrames);
    for (int i = 0; i < 2 * kFrames; i++) CHECK_EQ(io[i], 0.0f);
    CHECK(t.running());
    CHECK_EQ(t.speed(), 0.0f);
}

TEST(tapestop_release_fades_back_without_a_click_and_bypasses_again) {
    TapeStop &t = tape();
    t.set(0.25f * kRate);
    t.hold(true);
    float io[2 * kFrames];
    int b = 0;
    for (; b < 300; b++) {                       // well past the stop: silent
        sine_block(io, b);
        t.process(io, kFrames);
    }
    t.hold(false);
    // The return is a 256-frame fade from silence to the sine: no step bigger than the sine's own
    // slope plus the fade's (0.5 / 256 a frame).
    const float slope = 2.0f * 3.14159265f * 220.0f / kRate * 0.5f;
    float last = 0.0f, worst = 0.0f;
    int blocks = 0;
    while (t.running() && blocks < 20) {
        sine_block(io, b++);
        t.process(io, kFrames);
        for (int i = 0; i < kFrames; i++) {
            const float d = io[2 * i] - last;
            worst = (d < 0.0f ? -d : d) > worst ? (d < 0.0f ? -d : d) : worst;
            last = io[2 * i];
        }
        blocks++;
    }
    CHECK(!t.running());
    CHECK(blocks <= kTapeStopFadeFrames / kFrames + 1);
    // And no shorter: the whole fade is heard, not cut to the live signal after one block (a
    // mutation run found `!held_ && wet_ <= 0` could become `||` with nothing noticing).
    CHECK(blocks >= kTapeStopFadeFrames / kFrames);
    if (!(worst < slope + 0.5f / 256.0f + 1e-4f)) std::printf("    worst %g\n", double(worst));
    CHECK(worst < slope + 0.5f / 256.0f + 1e-4f);

    float ref[2 * kFrames];
    sine_block(io, b);
    for (int i = 0; i < 2 * kFrames; i++) ref[i] = io[i];
    t.process(io, kFrames);
    for (int i = 0; i < 2 * kFrames; i++) CHECK_EQ(io[i], ref[i]);
}

TEST(tapestop_a_second_press_starts_again_from_full_speed) {
    // Press, stop, let go, press again: the second stop starts from the live frame at full speed,
    // as the first did -- not from the standstill and the full lag the first one ended on, which
    // would make the second press an instant silence (both resets were mutation survivors).
    TapeStop &t = tape();
    t.set(0.25f * kRate);
    float io[2 * kFrames];
    int b = 0;
    t.hold(true);
    for (; b < 300; b++) {
        sine_block(io, b);
        t.process(io, kFrames);
    }
    CHECK_EQ(t.speed(), 0.0f);
    t.hold(false);
    for (int k = 0; k < 20 && t.running(); k++) {
        sine_block(io, b++);
        t.process(io, kFrames);
    }
    CHECK(!t.running());

    t.hold(true);
    CHECK_EQ(t.speed(), 1.0f);
    sine_block(io, b);
    float live[2 * kFrames];
    for (int i = 0; i < 2 * kFrames; i++) live[i] = io[i];
    t.process(io, kFrames);
    CHECK_EQ(io[0], live[0]);
    // One block in, the tape has slowed by a block's worth of a quarter-second ramp: it still
    // sounds, close to the live sine.
    float worst = 0.0f;
    for (int i = 0; i < 2 * kFrames; i++) {
        const float d = io[i] - live[i];
        worst = (d < 0.0f ? -d : d) > worst ? (d < 0.0f ? -d : d) : worst;
    }
    if (!(worst < 0.05f)) std::printf("    worst %g\n", double(worst));
    CHECK(worst < 0.05f);
}

TEST(tapestop_the_length_is_clamped_and_lag_never_leaves_the_ring) {
    TapeStop &t = tape();
    t.set(100.0f * kRate);
    CHECK_EQ(t.stop_frames(), kTapeStopMaxSeconds * kRate);
    t.set(0.0f);
    CHECK_EQ(t.stop_frames(), kTapeStopMinSeconds * kRate);
    // The longest stop's furthest lag is half of it, inside the ring.
    CHECK(0.5f * kTapeStopMaxSeconds * kRate < float(t.ring_frames()) - 2.0f);
}

// --- the freeze, on its own -------------------------------------------------------------------

namespace {

DelayFreeze &freeze() {
    static bool ready = false;
    if (!ready) {
        ready = g_freeze.init(kRate, 4.0f);
        CHECK(ready);
    }
    g_freeze.hold(false, 0);
    float l[kFrames] = {}, r[kFrames] = {};
    for (int b = 0; b < 16 && g_freeze.active(); b++) g_freeze.process(l, r, kFrames);
    return g_freeze;
}

// Left carries the running frame number, right its negative.
void counter(float *l, float *r, int &frame) {
    for (int i = 0; i < kFrames; i++, frame++) {
        l[i] = float(frame);
        r[i] = -float(frame);
    }
}

}  // namespace

TEST(freeze_records_one_period_then_loops_exactly_that) {
    DelayFreeze &f = freeze();
    const int length = 100;                      // not a multiple of the block, on purpose
    int frame = 1000;
    f.hold(true, length);
    float l[kFrames], r[kFrames];
    // While recording, the wet signal passes untouched.
    counter(l, r, frame);
    f.process(l, r, kFrames);
    CHECK_EQ(l[0], 1000.0f);
    CHECK_EQ(l[kFrames - 1], float(1000 + kFrames - 1));
    // The recording ends 100 frames in; the frames after it are the loop from its start.
    counter(l, r, frame);
    f.process(l, r, kFrames);
    CHECK(f.looping());
    const int first_loop = length - kFrames;     // index in this block where the loop begins
    CHECK_EQ(l[first_loop - 1], float(1000 + length - 1));
    CHECK_EQ(l[first_loop], 1000.0f);
    CHECK_EQ(r[first_loop], -1000.0f);
    // And it keeps looping the same 100 frames, whatever comes in.
    int out = kFrames - first_loop;              // frames of the loop played so far
    for (int b = 0; b < 10; b++) {
        counter(l, r, frame);
        f.process(l, r, kFrames);
        for (int i = 0; i < kFrames; i++, out++) CHECK_EQ(l[i], float(1000 + out % length));
    }
}

TEST(freeze_release_crossfades_back_and_goes_idle) {
    DelayFreeze &f = freeze();
    int frame = 0;
    float l[kFrames], r[kFrames];
    f.hold(true, 64);
    for (int b = 0; b < 4; b++) {
        counter(l, r, frame);
        f.process(l, r, kFrames);
    }
    f.hold(false, 0);
    CHECK(f.active());
    int blocks = 0;
    while (f.active() && blocks < 20) {
        counter(l, r, frame);
        f.process(l, r, kFrames);
        blocks++;
    }
    CHECK(!f.active());
    CHECK_EQ(blocks, kFreezeFadeFrames / kFrames);
    counter(l, r, frame);
    f.process(l, r, kFrames);
    CHECK_EQ(l[0], float(frame - kFrames));

    // A release while still recording stops at once: the output was live all along.
    f.hold(true, 1000);
    counter(l, r, frame);
    f.process(l, r, kFrames);
    f.hold(false, 0);
    CHECK(!f.active());
}

// --- the freeze in the mixer: the echoes stay, and nothing new gets in ------------------------

namespace {

constexpr int kSna = 14, kPan = 13;

struct DelayRig {
    Mixer mixer;
    SendSettings sends;
    MasterSettings master;
    TrackParams params{};
    ChainMix mix;
    float out[2 * kFrames] = {};

    DelayRig() {
        mixer.init(kRate);
        for (int i = 0; i < kParams; i++) params.set(i, 0);
        params.set(kPan, 64);
        params.set(kSna, 127);
        sends.delay_seconds = 0.25f;
        sends.delay_feedback = 0.4f;
        sends.delay_level = 1.0f;
        sends.reverb_level = 0.0f;
        master.compressor = 0.0f;
        master.limit = 0.98f;
    }

    // One block with `level` of a 300 Hz sine on track 0; the output's energy.
    double run(float level, int b) {
        mixer.prepare(sends, master);
        mixer.chain(0).prepare(params, mix);
        mixer.begin_block(kFrames);
        float *mono = mixer.scratch();
        for (int i = 0; i < kFrames; i++) {
            const float t = float(b * kFrames + i) * 300.0f / kRate;
            mono[i] = level * ed::sin_turns(t - float(int(t)));
        }
        mixer.add_track(0, mono);
        mixer.finish_block(out, kFrames);
        double e = 0.0;
        for (int i = 0; i < 2 * kFrames; i++) e += double(out[i]) * out[i];
        return e;
    }
};

DelayRig g_rig_a, g_rig_b;

}  // namespace

TEST(freeze_in_the_mixer_holds_the_echoes_and_cuts_the_feed) {
    // Two rigs, the same burst into the delay; one freezes after it.
    for (DelayRig *rig : {&g_rig_a, &g_rig_b}) {
        for (int b = 0; b < 40; b++) rig->run(0.5f, b);      // a burst, echoing
    }
    g_rig_b.mixer.freeze_delay(true);
    double plain = 0.0, frozen = 0.0;
    for (int b = 40; b < 1200; b++) {
        // Silence on the track: only the delay is heard.
        const double a = g_rig_a.run(0.0f, b);
        const double f = g_rig_b.run(0.0f, b);
        if (b >= 1000) {
            plain += a;
            frozen += f;
        }
    }
    // Four seconds on, feedback 0.4 has long gone quiet; the frozen loop has not.
    if (!(frozen > 1000.0 * (plain + 1e-12))) std::printf("    plain %g frozen %g\n", plain, frozen);
    CHECK(frozen > 1e-3);
    CHECK(frozen > 1000.0 * (plain + 1e-12));
    CHECK(g_rig_b.mixer.freeze().looping());

    // Feed the track while frozen: the loop does not change (nothing new gets in). Measured over
    // whole loops -- 0.25 s with ping-pong is 24,000 frames, 375 blocks -- since most of a loop
    // of a short burst is silence.
    double before = 0.0, during = 0.0;
    for (int b = 1200; b < 1575; b++) before += g_rig_b.run(0.0f, b);
    for (int b = 1575; b < 1639; b++) g_rig_b.run(0.5f, b);
    for (int b = 1639; b < 2014; b++) during += g_rig_b.run(0.0f, b);
    const double ratio = during / before;
    if (!(ratio > 0.999 && ratio < 1.001)) std::printf("    loop energy ratio %g\n", ratio);
    CHECK(ratio > 0.999 && ratio < 1.001);

    // Released, it fades back to the delay's own (long-decayed) tail.
    g_rig_b.mixer.freeze_delay(false);
    for (int b = 2014; b < 2022; b++) g_rig_b.run(0.0f, b);
    CHECK(!g_rig_b.mixer.freeze().active());
    double after = 0.0;
    for (int b = 2022; b < 2397; b++) after += g_rig_b.run(0.0f, b);
    CHECK(after < 1e-3 * before);
}
