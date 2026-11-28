// The performance mixer and its looper: core/perffx (the slots), dsp/masterfx (the five master
// effects), dsp/looper (slot 16), and their wiring in app/engine.
//
// What each has to do. Amount 0 is the dry mix, bit for bit, whatever the slot: VALUE turned
// back down lets go, as the morph and the macros do. Every slot does what its name says and
// only that. The master effects add no level past the limiter's ceiling and cost nothing once
// settled. The looper records exactly its bars from the bar line, plays them back in time at
// its mix, and shares RESAMPLE's buffer without ever taking it from a take.
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "apprig.hpp"
#include "fakehost.hpp"
#include "looper.hpp"
#include "masterfx.hpp"
#include "perffx.hpp"
#include "repeat.hpp"

#include <cmath>
#include <cstring>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::last_block;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

bool in_font(const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        const char c = *p;
        const bool ok = c == ' ' || c == '!' || c == '#' || c == '+' || c == '-' || c == '.' ||
                        c == '/' || (c >= '0' && c <= ':') || c == '=' || c == '?' ||
                        (c >= 'A' && c <= 'Z') || c == '%' || c == '>';
        if (!ok) return false;
    }
    return true;
}

// A loud stereo block: a saw on the left, its negative on the right.
float test_value(int frame) { return float(frame % 37) * (1.6f / 37.0f) - 0.8f; }

void test_block(float *io, int frames, int offset = 0) {
    for (int i = 0; i < frames; i++) {
        const float x = float((i + offset) % 37) * (1.6f / 37.0f) - 0.8f;
        io[2 * i] = x;
        io[2 * i + 1] = -x;
    }
}

}  // namespace

// --- core/perffx: the slots ---------------------------------------------------------------------

TEST(perffx_amount_zero_is_the_dry_plan_for_every_slot) {
    for (int f = 0; f < int(PerfFx::count); f++) {
        CHECK(perf_plan(PerfFx(f), 0) == PerfPlan{});
    }
    // And none and the looper are dry at any amount: the looper's mix is its own, not a plan.
    CHECK(perf_plan(PerfFx::none, 127) == PerfPlan{});
    CHECK(perf_plan(PerfFx::looper, 127) == PerfPlan{});
}

TEST(perffx_the_steps_are_the_sixteen_slots_and_sixteen_is_the_looper) {
    CHECK(perf_fx_for_step(0) == PerfFx::lp_sweep);
    CHECK(perf_fx_for_step(3) == PerfFx::echo_riser);
    CHECK(perf_fx_for_step(15) == PerfFx::looper);
    CHECK(perf_fx_for_step(16) == PerfFx::none);
    CHECK(perf_fx_for_step(-1) == PerfFx::none);
    CHECK_EQ(kLooperSlot, 16);
    for (int f = 0; f < int(PerfFx::count); f++) {
        CHECK(std::strlen(perf_fx_name(PerfFx(f))) <= 12);
        CHECK(in_font(perf_fx_name(PerfFx(f))));
        CHECK(std::strlen(perf_fx_short(PerfFx(f))) <= 5);
        CHECK(in_font(perf_fx_short(PerfFx(f))));
    }
}

TEST(perffx_each_slot_moves_what_its_name_says) {
    const PerfPlan lp = perf_plan(PerfFx::lp_sweep, 127);
    CHECK(lp.dj_on);
    CHECK_NEAR(lp.dj, -1.0f, 1e-6f);
    const PerfPlan hp = perf_plan(PerfFx::hp_sweep, 64);
    CHECK(hp.dj_on && hp.dj > 0.45f && hp.dj < 0.55f);

    const PerfPlan riser = perf_plan(PerfFx::hp_riser, 127);
    CHECK(riser.dj_on && riser.dj > 0.0f && riser.dj < 1.0f);
    CHECK_EQ(int(riser.send_reverb), 127);

    const PerfPlan echo = perf_plan(PerfFx::echo_riser, 127);
    CHECK_EQ(int(echo.send_delay), 127);
    CHECK(echo.delay_feedback > 0.85f && echo.delay_feedback <= 0.95f);
    CHECK(!echo.delay_freeze);

    CHECK(!perf_plan(PerfFx::echo_freeze, kPerfFreezeThreshold - 1).delay_freeze);
    CHECK(perf_plan(PerfFx::echo_freeze, kPerfFreezeThreshold).delay_freeze);

    const PerfPlan wash = perf_plan(PerfFx::reverb_wash, 127);
    CHECK_EQ(int(wash.send_reverb), 127);
    CHECK(wash.reverb_decay_s > 9.0f);

    CHECK(!perf_plan(PerfFx::tape_stop, kPerfHoldThreshold - 1).tape_stop);
    CHECK(perf_plan(PerfFx::tape_stop, kPerfHoldThreshold).tape_stop);

    CHECK_NEAR(perf_plan(PerfFx::bass_kill, 127).eq_low, 0.0f, 1e-6f);
    CHECK_NEAR(perf_plan(PerfFx::pump, 127).pump, 1.0f, 1e-6f);
    CHECK_NEAR(perf_plan(PerfFx::crush, 127).crush, 1.0f, 1e-6f);
    CHECK_NEAR(perf_plan(PerfFx::noise_riser, 127).noise, 1.0f, 1e-6f);
    CHECK_NEAR(perf_plan(PerfFx::gate, 127).gate, 1.0f, 1e-6f);

    const PerfPlan build = perf_plan(PerfFx::build, 127);
    CHECK(build.dj_on && build.dj > 0.0f);
    CHECK(build.send_delay > 0 && build.noise > 0.0f);

    // A slot moves nothing it does not name: the LP sweep has no sends and no master effect.
    CHECK_EQ(int(lp.send_delay) + int(lp.send_reverb), 0);
    CHECK(lp.noise == 0.0f && lp.pump == 0.0f && lp.tracks == 1.0f && !lp.tape_stop);
}

TEST(perffx_the_mute_bus_fades_every_track_and_nothing_else) {
    // Slot 7 since 2026-11-25 (CyDrums' P5): every fader down, the sends' feeds with them (they
    // are post-fader), the delay and reverb left to ring. It raises no send and holds nothing.
    CHECK_EQ(perf_plan(PerfFx::mute_bus, 0).tracks, 1.0f);
    CHECK_EQ(perf_plan(PerfFx::mute_bus, 127).tracks, 0.0f);
    float last = 1.0f;
    for (int a = 1; a <= 127; a++) {
        const PerfPlan plan = perf_plan(PerfFx::mute_bus, uint8_t(a));
        CHECK(plan.tracks < last);              // always further down as VALUE turns
        last = plan.tracks;
        CHECK_EQ(int(plan.send_delay) + int(plan.send_reverb), 0);
        CHECK(!plan.delay_freeze && !plan.tape_stop && !plan.dj_on);
    }
    // Half way is a quarter of the level: squared, so the fade is even to the ear.
    CHECK_NEAR(perf_plan(PerfFx::mute_bus, 64).tracks, 0.2464f, 0.001f);
}

TEST(perffx_the_morph_and_the_looper_are_controls_not_effects) {
    // Slot 13 since 2026-11-25 (backlog D2): the app turns the scene morph with it, so it plans
    // nothing here at any amount, like the looper.
    CHECK(perf_plan(PerfFx::morph, 127) == PerfPlan{});
    CHECK(perf_fx_is_control(PerfFx::morph));
    CHECK(perf_fx_is_control(PerfFx::looper));
    CHECK(perf_fx_for_step(12) == PerfFx::morph);
    CHECK(perf_fx_for_step(6) == PerfFx::mute_bus);
    for (int f = 0; f < int(PerfFx::count); f++) {
        const PerfFx fx = PerfFx(f);
        CHECK_EQ(perf_fx_is_control(fx), fx == PerfFx::morph || fx == PerfFx::looper);
    }
}

// --- dsp/masterfx: the five master effects ----------------------------------------------------

namespace {
MasterFx g_fx;   // 8 KiB of ring: file scope, not the stack
}

TEST(masterfx_idle_leaves_the_output_alone_to_the_bit) {
    g_fx.init(kRate);
    float io[2 * kBlk], ref[2 * kBlk];
    test_block(io, kBlk);
    test_block(ref, kBlk);
    g_fx.process(io, kBlk, 0.0f, 0.001f);
    CHECK(!g_fx.active());
    CHECK(std::memcmp(io, ref, sizeof io) == 0);
}

TEST(masterfx_each_effect_changes_the_sound_and_stays_inside_the_ceiling) {
    float MasterFxSettings::*const fields[] = {&MasterFxSettings::pump, &MasterFxSettings::crush,
                                               &MasterFxSettings::noise, &MasterFxSettings::gate};
    for (const auto field : fields) {
        g_fx.init(kRate);
        MasterFxSettings s;
        s.*field = 1.0f;
        g_fx.set(s);
        float changed = 0.0f, peak = 0.0f;
        float phase = 0.0f;
        for (int b = 0; b < 200; b++) {
            float io[2 * kBlk], ref[2 * kBlk];
            // Full scale, the most the limiter lets through.
            for (int i = 0; i < 2 * kBlk; i++) io[i] = ref[i] = (i / 2 + b) % 2 ? 1.0f : -1.0f;
            g_fx.process(io, kBlk, phase, 2.0f / 24000.0f);
            phase += float(kBlk) * 2.0f / 24000.0f;
            phase -= phase >= 1.0f ? 1.0f : 0.0f;
            for (int i = 0; i < 2 * kBlk; i++) {
                const float d = io[i] - ref[i];
                changed += d < 0.0f ? -d : d;
                const float a = io[i] < 0.0f ? -io[i] : io[i];
                peak = a > peak ? a : peak;
            }
        }
        CHECK(changed > 1.0f);
        CHECK(peak <= 1.0001f);
    }
}

TEST(masterfx_a_depth_turned_back_to_zero_settles_and_goes_bit_exact_again) {
    g_fx.init(kRate);
    MasterFxSettings s;
    s.crush = 1.0f;
    s.noise = 0.5f;
    g_fx.set(s);
    float io[2 * kBlk];
    for (int b = 0; b < 50; b++) {
        test_block(io, kBlk, b);
        g_fx.process(io, kBlk, 0.0f, 0.0f);
    }
    g_fx.set(MasterFxSettings{});
    for (int b = 0; b < 200; b++) {
        test_block(io, kBlk, b);
        g_fx.process(io, kBlk, 0.0f, 0.0f);
    }
    CHECK(!g_fx.active());
    float ref[2 * kBlk];
    test_block(io, kBlk, 7);
    test_block(ref, kBlk, 7);
    g_fx.process(io, kBlk, 0.0f, 0.0f);
    CHECK(std::memcmp(io, ref, sizeof io) == 0);
}

TEST(masterfx_the_pump_is_deepest_on_the_beat_and_open_before_the_next) {
    g_fx.init(kRate);
    MasterFxSettings s;
    s.pump = 1.0f;
    g_fx.set(s);
    // Let the depth settle, then read the gain on a constant signal at two places in the beat.
    float io[2 * kBlk];
    for (int b = 0; b < 100; b++) {
        for (float &x : io) x = 0.5f;
        g_fx.process(io, kBlk, 0.5f, 0.0f);
    }
    for (float &x : io) x = 0.5f;
    g_fx.process(io, kBlk, 0.0f, 0.0f);          // a step of 0: the whole block at that phase
    const float on_beat = io[0];
    for (float &x : io) x = 0.5f;
    g_fx.process(io, kBlk, 0.95f, 0.0f);
    const float before_next = io[0];
    CHECK(on_beat < 0.05f);
    CHECK(before_next > 0.49f);
}

// --- dsp/looper: slot 16 ------------------------------------------------------------------------

namespace {
constexpr uint32_t kLoopCapacity = 4096;
int16_t g_loop_buffer[2 * kLoopCapacity];
Looper g_looper;
}

TEST(looper_refuses_what_it_cannot_record) {
    g_looper.attach(nullptr, 0);
    CHECK(!g_looper.arm(1000, 1));
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(!g_looper.arm(0, 1));
    CHECK(!g_looper.arm(kLoopCapacity + 1, 1));
    CHECK(!g_looper.arm(1000, 3));
    CHECK(g_looper.arm(1000, 4));
    CHECK(g_looper.state() == Looper::State::armed);
}

TEST(looper_records_its_frames_from_the_bar_and_plays_them_back_at_its_mix) {
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    const uint32_t frames = 640;                    // ten blocks, one bar for the test
    CHECK(g_looper.arm(frames, 1));

    // Armed: nothing happens until a bar line.
    float io[2 * kBlk], ref[2 * kBlk];
    test_block(io, kBlk);
    g_looper.process(io, kBlk, 1.0f, true);
    CHECK(g_looper.state() == Looper::State::armed);

    // The bar line, at frame 0 of the coming block: ten blocks record, the output untouched.
    g_looper.on_bar(1, 0);
    for (int b = 0; b < 10; b++) {
        test_block(io, kBlk, b * kBlk);
        test_block(ref, kBlk, b * kBlk);
        g_looper.process(io, kBlk, 1.0f, true);
        CHECK(std::memcmp(io, ref, sizeof io) == 0);
    }
    CHECK(g_looper.state() == Looper::State::playing);
    CHECK_EQ(g_looper.recorded(), frames);
    CHECK_EQ(g_looper.position(), 0u);

    // Mix 0: live, to the bit, and the loop still keeps its place.
    for (float &x : io) x = 0.25f;
    for (float &x : ref) x = 0.25f;
    g_looper.process(io, kBlk, 0.0f, true);
    CHECK(std::memcmp(io, ref, sizeof io) == 0);
    CHECK_EQ(g_looper.position(), uint32_t(kBlk));

    // Up to 1 over a block (the ramp), then the next block is the loop alone: block 2 of the take.
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, true);
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, true);
    test_block(ref, kBlk, 2 * kBlk);
    float worst = 0.0f;
    for (int i = 0; i < 2 * kBlk; i++) {
        const float d = io[i] - ref[i];
        worst = (d < 0.0f ? -d : d) > worst ? (d < 0.0f ? -d : d) : worst;
    }
    CHECK(worst < 1.0f / 16384.0f);               // int16's step
}

TEST(looper_resyncs_on_its_bars_and_a_stop_fades_it_out) {
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(g_looper.arm(1280, 2));                   // two "bars" of 640
    g_looper.on_bar(10, 0);
    float io[2 * kBlk];
    for (int b = 0; b < 20; b++) {
        test_block(io, kBlk, b * kBlk);
        g_looper.process(io, kBlk, 0.0f, true);
    }
    CHECK(g_looper.playing());
    CHECK_EQ(g_looper.bars(), 2u);
    // Bar 11 is not one of the loop's own (10 + 2k): a read a few frames from its place there
    // is not moved (dsp/loopsync.hpp; a read that is lost is, in the test after this one).
    for (int b = 0; b < 10; b++) g_looper.process(io, kBlk, 0.0f, true);
    const uint32_t before = g_looper.position();
    CHECK_EQ(before, 640u);
    g_looper.on_bar(11, 3);
    CHECK_EQ(g_looper.position(), before);
    // Bar 12 is: the bar line, 5 frames into the coming block, is loop frame 0.
    g_looper.on_bar(12, 5);
    CHECK_EQ(g_looper.position(), 1280u - 5u);

    // Stopped: faded to live within a block and silent after.
    for (int b = 0; b < 3; b++) {
        for (float &x : io) x = 0.0f;
        g_looper.process(io, kBlk, 1.0f, true);
    }
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, false);
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, false);
    for (int i = 0; i < 2 * kBlk; i++) CHECK(io[i] == 0.0f);
    g_looper.clear();
    CHECK(!g_looper.holding());
}

TEST(looper_a_lost_read_comes_back_on_the_first_bar_after_a_start) {
    // Two bars of 2,048 frames from bar 5. After a stop and start the clock is on bar 0, the
    // loop's second bar: before, it played on out of time until bar 1 came round.
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(g_looper.arm(4096, 2));
    g_looper.on_bar(5, 0);
    float io[2 * kBlk];
    for (int b = 0; b < 64; b++) {
        test_block(io, kBlk, b * kBlk);
        g_looper.process(io, kBlk, 0.0f, true);
    }
    CHECK(g_looper.playing());
    for (int b = 0; b < 10; b++) g_looper.process(io, kBlk, 0.0f, false);
    CHECK_EQ(g_looper.position(), 640u);
    g_looper.on_bar(0, 0);
    CHECK_EQ(g_looper.position(), 2048u);
    // Its own bar puts it exactly in place; a frame or two off on the bar between is left
    // alone.
    g_looper.process(io, kBlk, 0.0f, true);
    g_looper.on_bar(1, 0);                         // its own bar: exact
    CHECK_EQ(g_looper.position(), 0u);
    for (int b = 0; b < 32; b++) g_looper.process(io, kBlk, 0.0f, true);
    g_looper.on_bar(2, 2);
    CHECK_EQ(g_looper.position(), 2048u);
    g_looper.clear();
}

TEST(looper_a_new_take_keeps_the_loop_playing_to_the_bar_and_fades_it_out) {
    // A one-bar loop of the test saw, playing at full mix. A new take is armed: before, the loop
    // dropped out at once (a click, and a bar of silence where the loop was). Now it plays on
    // until the bar line, fades out over the take's first block, and the take is the live mix.
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(g_looper.arm(640, 1));
    g_looper.on_bar(1, 0);
    float io[2 * kBlk], ref[2 * kBlk];
    for (int b = 0; b < 10; b++) {
        test_block(io, kBlk, b * kBlk);
        g_looper.process(io, kBlk, 1.0f, true);
    }
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, true);       // the ramp up
    CHECK(g_looper.playing());

    CHECK(g_looper.arm(1280, 2));
    CHECK(g_looper.state() == Looper::State::armed);
    CHECK_EQ(g_looper.bars(), 2u);                // the take's, for the screen
    // Armed: still the loop alone on a silent input, block 1 of the old take.
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 1.0f, true);
    test_block(ref, kBlk, 1 * kBlk);
    for (int i = 0; i < 2 * kBlk; i++) CHECK(std::fabs(io[i] - ref[i]) < 1.0f / 16384.0f);

    // The bar line: the take's first block fades the old loop (block 2) out over a live input
    // of 0.25, and records the live input, not the fade.
    g_looper.on_bar(2, 0);
    for (float &x : io) x = 0.25f;
    g_looper.process(io, kBlk, 1.0f, true);
    CHECK(g_looper.state() == Looper::State::recording);
    test_block(ref, kBlk, 2 * kBlk);
    CHECK(std::fabs(io[0] - ref[0]) < 1.0f / 16384.0f);          // the old loop, still whole
    CHECK(std::fabs(io[2 * kBlk - 2] - 0.25f) < 0.03f);          // almost only the live input
    for (int i = 0; i < 2 * kBlk; i++) {
        CHECK_EQ(g_loop_buffer[i], int16_t(0.25f * 32767.0f));
    }
    // The rest of the take, and then the new loop plays from its start at the mix it is given.
    for (int b = 1; b < 20; b++) {
        for (float &x : io) x = 0.25f;
        g_looper.process(io, kBlk, 1.0f, true);
        CHECK_EQ(io[5], 0.25f);                    // nothing over the live mix while recording
    }
    CHECK(g_looper.playing());
    CHECK_EQ(g_looper.frames(), 1280u);
    CHECK_EQ(g_looper.bars(), 2u);
    g_looper.clear();
}

TEST(looper_a_take_armed_from_idle_is_silent_until_it_plays) {
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(g_looper.arm(640, 1));
    float io[2 * kBlk];
    for (float &x : io) x = 0.5f;
    g_looper.process(io, kBlk, 1.0f, true);
    for (int i = 0; i < 2 * kBlk; i++) CHECK_EQ(io[i], 0.5f);
    g_looper.clear();
}

namespace {

// A block against the crossfade law, sample by sample: live `in` + m (loop - in), with m ramping
// from `from` to `to` over the block and the loop read from `pos` (wrapping at `frames`) in the
// buffer the take was recorded into.
int crossfade_misses(const float *out, const float *in, uint32_t pos, uint32_t frames, float from,
                     float to) {
    int wrong = 0;
    for (int i = 0; i < kBlk; i++) {
        const float m = from + (to - from) * float(i) / float(kBlk);
        const uint32_t at = (pos + uint32_t(i)) % frames;
        for (int c = 0; c < 2; c++) {
            const float loop = float(g_loop_buffer[2 * at + uint32_t(c)]) / 32768.0f;
            const float want = in[2 * i + c] + m * (loop - in[2 * i + c]);
            wrong += std::fabs(out[2 * i + c] - want) > 2e-6f ? 1 : 0;
        }
    }
    return wrong;
}

// Records a one-bar take of `frames` (a multiple of the block) from the test saw, leaving it
// playing at mix 0 at position 0.
void record_take(uint32_t frames) {
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(g_looper.arm(frames, 1));
    g_looper.on_bar(1, 0);
    float io[2 * kBlk];
    for (uint32_t b = 0; b < frames / kBlk; b++) {
        test_block(io, kBlk, int(b) * kBlk);
        g_looper.process(io, kBlk, 0.0f, true);
    }
    CHECK(g_looper.playing());
}

}  // namespace

// Mix up from 0 across the loop's end, steady, and the stop's fade back down: each block on the
// crossfade law, sample by sample, with a live input under it.
TEST(looper_every_ramp_is_the_crossfade_law_across_the_loops_end) {
    record_take(640);
    float io[2 * kBlk], in[2 * kBlk];
    // Walk to 40 frames before the end, in blocks that keep the loop's place.
    for (int b = 0; b < 9; b++) g_looper.process(io, kBlk, 0.0f, true);
    g_looper.process(io, 600 - 9 * kBlk, 0.0f, true);
    CHECK_EQ(g_looper.position(), 600u);
    const float steps[][2] = {{0.0f, 0.8f}, {0.8f, 0.8f}, {0.8f, 0.3f}};
    uint32_t pos = 600;
    for (const auto &st : steps) {
        test_block(in, kBlk, 11);
        for (int i = 0; i < 2 * kBlk; i++) io[i] = in[i] * 0.5f;
        for (int i = 0; i < 2 * kBlk; i++) in[i] = io[i];
        g_looper.process(io, kBlk, st[1], true);
        CHECK_EQ(crossfade_misses(io, in, pos, 640, st[0], st[1]), 0);
        pos = (pos + kBlk) % 640;
    }
    CHECK_EQ(g_looper.position(), pos);
    // Stopped: down from 0.3 to nothing over one block, then live and untouched.
    test_block(in, kBlk, 5);
    for (int i = 0; i < 2 * kBlk; i++) io[i] = in[i];
    g_looper.process(io, kBlk, 0.3f, false);
    CHECK_EQ(crossfade_misses(io, in, pos, 640, 0.3f, 0.0f), 0);
    CHECK_EQ(g_looper.mix(), 0.0f);
    g_looper.clear();
}

// The loop a new take replaces fades out over the take's first block from where it was, not
// from its start: sample by sample, at its mix, over the live input.
TEST(looper_the_outgoing_loop_fades_from_its_own_place) {
    record_take(640);
    float io[2 * kBlk], in[2 * kBlk];
    for (int b = 0; b < 9; b++) {
        for (float &x : io) x = 0.0f;
        g_looper.process(io, kBlk, 0.6f, true);
    }
    CHECK_EQ(g_looper.position(), 576u);
    CHECK(g_looper.arm(1280, 2));
    CHECK_EQ(g_looper.recorded(), 0u);
    // Arming again before the bar keeps the old loop outgoing.
    CHECK(g_looper.arm(1280, 2));
    for (float &x : io) x = 0.0f;
    g_looper.process(io, 24, 0.6f, true);             // to 40 frames before the old loop's end
    CHECK_EQ(g_looper.position(), 600u);
    g_looper.on_bar(7, 10);                           // the bar line 10 frames into the block
    CHECK(g_looper.state() == Looper::State::recording);
    CHECK_EQ(g_looper.position(), 0u);
    // A take cannot be armed over one recording.
    CHECK(!g_looper.arm(640, 1));
    CHECK(g_looper.state() == Looper::State::recording);
    test_block(in, kBlk, 3);
    for (int i = 0; i < 2 * kBlk; i++) io[i] = in[i];
    // The fade reads the old loop before the take's block lands on the buffer's front, so the
    // reference is taken from a copy made before the block.
    static int16_t before[2 * 640];
    for (int i = 0; i < 2 * 640; i++) before[i] = g_loop_buffer[i];
    g_looper.process(io, kBlk, 0.6f, true);
    int wrong = 0;
    for (int i = 0; i < kBlk; i++) {
        const float m = 0.6f - 0.6f * float(i) / float(kBlk);
        for (int c = 0; c < 2; c++) {
            const float loop = float(before[2 * ((600 + i) % 640) + c]) / 32768.0f;
            const float want = in[2 * i + c] + m * (loop - in[2 * i + c]);
            wrong += std::fabs(io[2 * i + c] - want) > 2e-6f ? 1 : 0;
        }
    }
    CHECK_EQ(wrong, 0);
    // The rest of the take; then its own bar line, at the block's start, is loop frame 10.
    for (int b = 1; b < 20; b++) g_looper.process(io, kBlk, 0.6f, true);
    CHECK(g_looper.playing());
    g_looper.on_bar(9, 0);
    CHECK_EQ(g_looper.position(), 10u);
    for (float &x : io) x = 0.0f;
    g_looper.process(io, kBlk, 0.6f, true);
    CHECK(g_looper.position() != 0u && g_looper.mix() != 0.0f);
    g_looper.clear();
    CHECK_EQ(g_looper.frames(), 0u);
    CHECK_EQ(g_looper.recorded(), 0u);
    CHECK_EQ(g_looper.position(), 0u);
    CHECK_EQ(g_looper.mix(), 0.0f);

    // From idle, arming twice and running a block leaves the output alone (no loop to play).
    CHECK(g_looper.arm(640, 1));
    CHECK(g_looper.arm(640, 1));
    for (float &x : io) x = 0.5f;
    g_looper.process(io, kBlk, 1.0f, true);
    for (int i = 0; i < 2 * kBlk; i++) CHECK_EQ(io[i], 0.5f);
    g_looper.clear();
    // A buffer attached (a project load hands it over again) starts with nothing held.
    record_take(640);
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    CHECK(!g_looper.holding());
    CHECK_EQ(g_looper.frames(), 0u);
}

// A take that is not a whole number of blocks (a bar at most tempos is not): the last block
// records only what is left, and the loop plays on from where that leaves it in the block.
TEST(looper_a_take_ends_mid_block_and_plays_on_from_there) {
    g_looper.attach(g_loop_buffer, kLoopCapacity);
    for (int i = 0; i < 2 * 700; i++) g_loop_buffer[i] = 1234;    // marks past the take's end
    CHECK(g_looper.arm(650, 1));
    g_looper.on_bar(1, 0);
    float io[2 * kBlk];
    for (int b = 0; b < 11; b++) {
        test_block(io, kBlk, b * kBlk);
        g_looper.process(io, kBlk, 0.0f, true);
    }
    CHECK(g_looper.playing());
    CHECK_EQ(g_looper.recorded(), 650u);
    CHECK_EQ(g_looper.position(), 11u * kBlk - 650u);              // 54 frames into the loop
    CHECK_EQ(g_loop_buffer[2 * 650], int16_t(1234));               // nothing written past it
    CHECK_EQ(g_loop_buffer[2 * 649], int16_t(test_value(649) * 32767.0f));
    g_looper.clear();
}

// --- app/engine: the wiring ---------------------------------------------------------------------

TEST(perffx_engine_amount_zero_hands_the_mixer_nothing) {
    // A picked effect at amount 0 is the dry plan, so process() hands the mixer the project's
    // own sends and master and runs no master effect: the same path as no effect at all, which
    // the goldens (tests/test_golden.cpp) hold to the bit.
    Engine &engine = fresh();
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::noise_riser), 0);
    pump(4);
    CHECK(engine.perf_fx() == PerfFx::noise_riser);
    CHECK(engine.perf_plan_now() == PerfPlan{});
    CHECK(!engine.master_fx().active());
    CHECK(!engine.delay_frozen());
    CHECK(!engine.tape_stop().held());
    CHECK(!engine.repeat().active());
    post(Cmd::perf_fx, 0, 0, 0, 0);
    pump();
}

TEST(perffx_engine_applies_the_plan_and_refuses_the_looper_and_the_morph_as_effects) {
    Engine &engine = fresh();
    const uint32_t refused = engine.status().refused.load(std::memory_order_relaxed);
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::looper), 100);
    pump();
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused + 1);
    CHECK(engine.perf_fx() == PerfFx::none);
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::morph), 100);
    pump();
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused + 2);
    CHECK(engine.perf_fx() == PerfFx::none);
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::count), 100);
    pump();
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused + 3);

    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::echo_freeze), 127);
    pump(2);
    CHECK(engine.perf_fx() == PerfFx::echo_freeze);
    CHECK_EQ(int(engine.perf_amount()), 127);
    CHECK(engine.delay_frozen());                 // the plan holds the freeze

    // The tape stop is held by its slot alone since its chord retired (backlog D5).
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::tape_stop), 127);
    pump(2);
    CHECK(!engine.delay_frozen());                // a new effect let the last one go
    CHECK(engine.tape_stop().held());
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::noise_riser), 127);
    pump(2);
    CHECK(!engine.tape_stop().held());
    CHECK(engine.master_fx().active());
    post(Cmd::perf_fx, 0, 0, 0, 0);
    pump(200);
    CHECK(!engine.master_fx().active());
}

TEST(perffx_engine_the_mute_bus_scales_the_faders_and_so_the_sends_feeds) {
    // The faders and the post-fader sends are one gain in the chain (dsp/chain.cpp), so the
    // send gains say what the MUTE BUS did: half way a quarter, at the top nothing fed, and at 0
    // exactly what the kit set -- the faders were scaled, never written.
    Engine &engine = fresh();
    FakeHost::the().set_running(true);
    for (int s = 0; s < 16; s++) put_trig(0, s);
    for (int b = 0; b < 40; b++) {
        FakeHost::the().deliver_tick(uint32_t(b), 0);
        pump();
    }
    const float send_a = engine.mixer().chain(0).send_a_gain();
    const float send_b = engine.mixer().chain(0).send_b_gain();
    CHECK(send_a > 0.1f && send_b > 0.1f);             // the test kit sends half to each

    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::mute_bus), 64);
    pump(2);
    CHECK_NEAR(engine.mixer().chain(0).send_a_gain(), send_a * 0.2464f, 0.001f);
    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::mute_bus), 127);
    pump(2);
    CHECK_EQ(engine.mixer().chain(0).send_a_gain(), 0.0f);
    CHECK_EQ(engine.mixer().chain(0).send_b_gain(), 0.0f);
    CHECK(engine.perf_plan_now().send_delay == 0);      // and nothing raised to make up for it

    post(Cmd::perf_fx, 0, 0, uint8_t(PerfFx::mute_bus), 0);
    pump(2);
    CHECK_EQ(engine.mixer().chain(0).send_a_gain(), send_a);
    CHECK_EQ(engine.mixer().chain(0).send_b_gain(), send_b);
    post(Cmd::perf_fx, 0, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(perffx_engine_the_looper_and_resample_share_the_buffer_and_never_both) {
    Engine &engine = fresh();
    FakeHost::the().set_running(true);
    pump();
    post(Cmd::looper, 0, 0, 1, 1);                 // one bar from the next bar line
    pump();
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 1);
    CHECK_EQ(int(engine.status().looper_bars.load(std::memory_order_relaxed)), 1);

    // RESAMPLE is refused while the looper holds the buffer.
    const uint32_t refused = engine.status().refused.load(std::memory_order_relaxed);
    post(Cmd::resample_arm, 0, 0, 1);
    pump();
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused + 1);
    CHECK_EQ(int(engine.status().resample_state.load(std::memory_order_relaxed)), 0);

    // The bar line starts the take.
    FakeHost::the().deliver_tick(384, 0);
    pump();
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 2);

    // A stop under a take throws it away: it would be out of time.
    FakeHost::the().set_running(false);
    pump();
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 0);

    // And the other way round: no loop while a take is armed.
    FakeHost::the().set_running(true);
    pump();
    post(Cmd::resample_arm, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.status().resample_state.load(std::memory_order_relaxed)), 1);
    const uint32_t refused2 = engine.status().refused.load(std::memory_order_relaxed);
    post(Cmd::looper, 0, 0, 1, 4);
    pump();
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused2 + 1);
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 0);
    post(Cmd::resample_cancel);
    pump();
}

TEST(perffx_engine_the_looper_records_a_bar_and_plays_it_under_its_mix) {
    Engine &engine = fresh();
    FakeHost &host = FakeHost::the();
    put_trig(0, 0);
    put_trig(0, 4);
    host.set_running(true);
    pump();
    post(Cmd::looper, 0, 0, 1, 1);
    pump();
    host.deliver_tick(384, 0);
    for (int i = 0; i < 1600; i++) {
        if (i % 8 == 0) host.deliver_tick(uint32_t(384 + i), 0);
        pump();
        if (engine.status().looper_state.load(std::memory_order_relaxed) == 3u) break;
    }
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 3);
    CHECK(engine.looper().frames() > 80000u);       // a bar at 130 BPM is 88,615 frames
    // The mix goes where it is told, and a clear frees the buffer for RESAMPLE again.
    post(Cmd::looper, 0, 0, 0, 127);
    pump(4);
    CHECK_EQ(int(engine.looper_mix()), 127);
    CHECK(engine.looper().mix() > 0.99f);
    post(Cmd::looper, 0, 0, 2);
    pump();
    CHECK_EQ(int(engine.status().looper_state.load(std::memory_order_relaxed)), 0);
    post(Cmd::looper, 0, 0, 0, 0);
    pump();
}

// --- FX GLIDE: a button's jump rides (core/perffx.hpp) -----------------------------------------

TEST(perffx_glide_lengths_follow_the_tempo) {
    CHECK_EQ(int(perf_glide_ms(PerfGlide::off, 120.0f)), 0);
    CHECK_EQ(int(perf_glide_ms(PerfGlide::sixteenth, 120.0f)), 125);
    CHECK_EQ(int(perf_glide_ms(PerfGlide::beat, 120.0f)), 500);
    CHECK_EQ(int(perf_glide_ms(PerfGlide::bar, 120.0f)), 2000);
    CHECK_EQ(int(perf_glide_ms(PerfGlide::beat, 60.0f)), 1000);
    CHECK_EQ(int(perf_glide_ms(PerfGlide::beat, 1000.0f)), 200);     // clamped to 300 BPM
    CHECK_EQ(int(perf_glide_ms(PerfGlide::beat, 0.0f)), 3000);       // and to 20
    CHECK(kPerfGlideDefault == PerfGlide::beat);
    for (int g = 0; g < int(PerfGlide::count); g++) CHECK(in_font(perf_glide_name(PerfGlide(g))));
    CHECK(std::strcmp(perf_glide_name(PerfGlide::beat), "1 BEAT") == 0);
}

TEST(perffx_the_ride_back_to_rest_is_at_most_a_sixteenth) {
    CHECK_EQ(int(perf_release_ms(PerfGlide::off, 120.0f)), 0);        // OFF still cuts
    CHECK_EQ(int(perf_release_ms(PerfGlide::sixteenth, 120.0f)), 125);
    CHECK_EQ(int(perf_release_ms(PerfGlide::beat, 120.0f)), 125);
    CHECK_EQ(int(perf_release_ms(PerfGlide::bar, 120.0f)), 125);
    CHECK_EQ(int(perf_release_ms(PerfGlide::bar, 130.0f)), 115);
    CHECK_EQ(int(perf_release_ms(PerfGlide::beat, 1000.0f)), 50);     // clamped to 300 BPM
}

TEST(perffx_a_ride_to_zero_takes_the_whole_length_then_sets_the_target) {
    PerfRide ride;
    ride.from_fx = PerfFx::crush;
    ride.from_amount = 100;
    ride.to_fx = PerfFx::pump;                       // a new slot, picked at 0
    ride.to_amount = 0;
    ride.length_ms = 400;
    PerfFx fx = PerfFx::none;
    uint8_t amount = 0;
    perf_ride_at(ride, 0, fx, amount);
    CHECK(fx == PerfFx::crush);
    CHECK_EQ(int(amount), 100);
    perf_ride_at(ride, 200, fx, amount);
    CHECK(fx == PerfFx::crush);
    CHECK_EQ(int(amount), 50);
    perf_ride_at(ride, 399, fx, amount);
    CHECK(fx == PerfFx::crush);
    CHECK(amount <= 1);
    perf_ride_at(ride, 400, fx, amount);
    CHECK(fx == PerfFx::pump);
    CHECK_EQ(int(amount), 0);
    perf_ride_at(ride, 5000, fx, amount);
    CHECK(fx == PerfFx::pump);
}

TEST(perffx_a_ride_up_and_a_ride_on_one_effect_are_straight_lines) {
    PerfRide ride;
    ride.from_fx = PerfFx::none;
    ride.to_fx = PerfFx::gate;
    ride.to_amount = 127;
    ride.length_ms = 100;
    PerfFx fx = PerfFx::none;
    uint8_t amount = 0;
    perf_ride_at(ride, 0, fx, amount);
    CHECK(fx == PerfFx::gate);                        // up at once, from 0
    CHECK_EQ(int(amount), 0);
    perf_ride_at(ride, 50, fx, amount);
    CHECK_EQ(int(amount), 63);
    ride.from_fx = PerfFx::gate;
    ride.from_amount = 27;
    perf_ride_at(ride, 50, fx, amount);
    CHECK(fx == PerfFx::gate);
    CHECK_EQ(int(amount), 77);
    // Never past the target, never back below where it started.
    uint8_t last = 27;
    for (uint32_t t = 0; t <= 100; t++) {
        perf_ride_at(ride, t, fx, amount);
        CHECK(amount >= last);
        CHECK(amount <= 127);
        last = amount;
    }
    CHECK_EQ(int(last), 127);
}

TEST(perffx_a_ride_between_two_effects_up_is_one_at_a_time) {
    PerfRide ride;
    ride.from_fx = PerfFx::lp_sweep;
    ride.from_amount = 80;
    ride.to_fx = PerfFx::hp_sweep;
    ride.to_amount = 80;
    ride.length_ms = 200;
    PerfFx fx = PerfFx::none;
    uint8_t amount = 0;
    perf_ride_at(ride, 50, fx, amount);
    CHECK(fx == PerfFx::lp_sweep);
    CHECK_EQ(int(amount), 40);
    perf_ride_at(ride, 100, fx, amount);
    CHECK(fx == PerfFx::hp_sweep);
    CHECK_EQ(int(amount), 0);
    perf_ride_at(ride, 150, fx, amount);
    CHECK(fx == PerfFx::hp_sweep);
    CHECK_EQ(int(amount), 40);
    perf_ride_at(ride, 200, fx, amount);
    CHECK_EQ(int(amount), 80);
}

TEST(perffx_a_ride_of_length_zero_is_the_target) {
    PerfRide ride;
    ride.from_fx = PerfFx::crush;
    ride.from_amount = 90;
    ride.to_fx = PerfFx::none;
    ride.length_ms = 0;
    PerfFx fx = PerfFx::crush;
    uint8_t amount = 90;
    perf_ride_at(ride, 0, fx, amount);
    CHECK(fx == PerfFx::none);
    CHECK_EQ(int(amount), 0);
}
