// The per-track LFO: dsp/lfo.hpp's byte meanings and oscillator, and what app/engine.cpp does
// with it -- block rate, after the locks, off when its bytes are zero, and bit-exact when off.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include "lfo.hpp"
#include "perform.hpp"
#include "track.hpp"

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::kit_value;
using strata::test::post;
using strata::test::pump;

namespace {

constexpr int kTrack = 1;          // the snare in the rig's kit
constexpr int kDest = 2;           // a SRC parameter: the machine re-prepares from it
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

void set(int param, uint8_t value) { post(Cmd::set_param, kTrack, 0, uint8_t(param), value); }

void lfo(LfoWave wave, uint8_t rate, uint8_t depth, int dest) {
    set(kLfoWave, lfo_wave_byte(wave));
    set(kLfoRate, rate);
    set(kLfoDepth, depth);
    set(kLfoDest, lfo_dest_byte(dest));
    pump();
}

uint8_t sounding() { return strata::test::engine().sounding(kTrack).v[kDest]; }

}  // namespace

// --- the bytes ------------------------------------------------------------

TEST(lfo_wave_and_dest_bands_cover_the_byte_and_one_click_is_one_band) {
    int last = -1;
    for (int b = 0; b < 128; b++) {
        const int w = int(lfo_wave(uint8_t(b)));
        CHECK(w >= last && w <= last + 1);
        last = w;
    }
    CHECK_EQ(last, kLfoWaves - 1);
    for (int w = 0; w < kLfoWaves; w++) {
        CHECK_EQ(int(lfo_wave(lfo_wave_byte(LfoWave(w)))), w);
        if (w > 0) CHECK_EQ(int(lfo_wave(uint8_t(lfo_wave_byte(LfoWave(w)) - 1))), w - 1);
        // From a band's start, one click (lfo_click) is the next band.
        const int next = int(lfo_wave_byte(LfoWave(w))) + lfo_click(kLfoWave);
        CHECK_EQ(int(lfo_wave(uint8_t(next > 127 ? 127 : next))),
                 w + 1 < kLfoWaves ? w + 1 : kLfoWaves - 1);
    }

    CHECK_EQ(lfo_dest(0), -1);                       // zero is OFF
    CHECK_EQ(lfo_dest(127), kParams - 1);
    for (int p = -1; p < kParams; p++) {
        CHECK_EQ(lfo_dest(lfo_dest_byte(p)), p);
        const int next = int(lfo_dest_byte(p)) + lfo_click(kLfoDest);
        CHECK_EQ(lfo_dest(uint8_t(next > 127 ? 127 : next)), p + 1 < kParams ? p + 1 : kParams - 1);
    }
    // Eight clicks from 0 walk every destination: a click is one band, never two.
    for (int k = 0; k <= kParams; k++) {
        const int byte = k * lfo_click(kLfoDest);
        CHECK_EQ(lfo_dest(uint8_t(byte > 127 ? 127 : byte)), k - 1);
    }
    CHECK_EQ(lfo_click(kLfoRate), 1);
    CHECK_EQ(lfo_click(kLfoDepth), 1);
}

TEST(lfo_rate_runs_free_below_64_and_synced_above) {
    CHECK_EQ(lfo_sync_ticks(0), 0u);
    CHECK_EQ(lfo_sync_ticks(63), 0u);
    CHECK_EQ(lfo_sync_ticks(64), 3072u);             // 8 bars
    CHECK_EQ(lfo_sync_ticks(67), 3072u);             // four counts a division
    CHECK_EQ(lfo_sync_ticks(76), 384u);              // 1 bar
    CHECK_EQ(lfo_sync_ticks(96), 96u);               // a quarter note
    CHECK_EQ(lfo_sync_ticks(116), 24u);              // a sixteenth
    CHECK_EQ(lfo_sync_ticks(127), 12u);              // a 1/32
    uint32_t last = 4000;
    for (int r = 64; r < 128; r++) {
        CHECK(lfo_sync_ticks(uint8_t(r)) <= last);
        last = lfo_sync_ticks(uint8_t(r));
    }
    CHECK_EQ(lfo_sync_name(63), "---");
    CHECK_EQ(lfo_sync_name(64), "8B");               // the first synced count is a rate too
    CHECK_EQ(lfo_sync_name(76), "1B");
    CHECK_EQ(lfo_sync_name(96), "1/4");
    CHECK_EQ(lfo_sync_name(100), "1/8.");
    CHECK_EQ(lfo_sync_name(127), "1/32");

    CHECK_NEAR(lfo_free_hz(0), 0.0625f, 1e-4f);
    CHECK_NEAR(lfo_free_hz(7), 0.125f, 1e-3f);       // an octave every 7 counts
    CHECK_NEAR(lfo_free_hz(63), 32.0f, 0.1f);
    CHECK_EQ(lfo_free_centi_hz(0), 6u);
    CHECK_EQ(lfo_free_centi_hz(63), 3200u);
    for (int r = 1; r < 64; r++) CHECK(lfo_free_hz(uint8_t(r)) > lfo_free_hz(uint8_t(r - 1)));
}

TEST(lfo_settings_take_the_free_rate_from_the_rat_byte) {
    // LfoSettings::make: a free rate's Hz comes from RAT (byte 1) and only while free; a synced
    // one has none. WAV, DEP and DST are set to other values so a wrong byte shows.
    const uint8_t free_bytes[kLfoParams] = {30, 21, 100, 5};
    const LfoSettings f = LfoSettings::make(free_bytes);
    CHECK_EQ(f.sync_ticks, 0u);
    CHECK_NEAR(f.hz, lfo_free_hz(21), 1e-6f);
    CHECK(f.hz > 0.0f);
    const uint8_t synced_bytes[kLfoParams] = {30, 96, 100, 5};
    const LfoSettings s = LfoSettings::make(synced_bytes);
    CHECK_EQ(s.sync_ticks, 96u);
    CHECK_EQ(s.hz, 0.0f);
    CHECK_NEAR(s.inv_sync_ticks, 1.0f / 96.0f, 1e-6f);
}

TEST(lfo_apply_moves_the_byte_by_depth_and_clamps) {
    CHECK_EQ(int(lfo_apply(64, 1.0f, 0)), 64);
    CHECK_EQ(int(lfo_apply(64, 1.0f, 127)), 127);
    CHECK_EQ(int(lfo_apply(64, -1.0f, 127)), 0);
    CHECK_EQ(int(lfo_apply(64, 0.5f, 127)), 96);
    CHECK_EQ(int(lfo_apply(64, -0.5f, 127)), 32);
    CHECK_EQ(int(lfo_apply(100, 1.0f, 127)), 127);   // clamped, not wrapped
    CHECK_EQ(int(lfo_apply(10, -1.0f, 127)), 0);
    CHECK_EQ(int(lfo_apply(64, 0.0f, 127)), 64);
    CHECK_EQ(int(lfo_apply(64, 1.0f, 200)), 127);    // a damaged depth is 127
}

TEST(lfo_settings_are_off_unless_both_depth_and_dest_are_set) {
    uint8_t bytes[kLfoParams] = {};
    CHECK(!LfoSettings::make(bytes).on());           // zeroed bytes are OFF
    bytes[2] = 100;
    CHECK(!LfoSettings::make(bytes).on());           // a depth with no destination
    bytes[2] = 0;
    bytes[3] = lfo_dest_byte(5);
    CHECK(!LfoSettings::make(bytes).on());           // a destination with no depth
    bytes[2] = 1;
    const LfoSettings s = LfoSettings::make(bytes);
    CHECK(s.on());
    CHECK_EQ(s.dest, 5);
    CHECK_EQ(s.sync_ticks, 0u);
    CHECK(s.hz > 0.0f);
    bytes[1] = 96;
    const LfoSettings synced = LfoSettings::make(bytes);
    CHECK_EQ(synced.sync_ticks, 96u);
    CHECK_NEAR(synced.inv_sync_ticks * 96.0f, 1.0f, 1e-6f);
}

TEST(lfo_shapes_run_a_cycle_and_saw_falls_where_ramp_rises) {
    TrackLfo a;
    TrackLfo b;
    a.init(1);
    b.init(1);
    // A quarter of a cycle on each.
    const float ramp = a.advance(LfoWave::ramp, 0.25f);
    const float saw = b.advance(LfoWave::saw, 0.25f);
    CHECK_NEAR(ramp, -0.5f, 1e-5f);
    CHECK_NEAR(saw, 0.5f, 1e-5f);

    TrackLfo sine;
    sine.init(1);
    CHECK_NEAR(sine.advance(LfoWave::sine, 0.25f), 1.0f, 1e-3f);
    CHECK_NEAR(sine.advance(LfoWave::sine, 0.5f), -1.0f, 1e-3f);
    CHECK_NEAR(sine.advance(LfoWave::sine, 0.25f), 0.0f, 1e-3f);   // a whole cycle
    CHECK_NEAR(sine.phase(), 0.0f, 1e-5f);

    // A seek backwards is a wrap, so S&H draws a new value at each synced cycle.
    TrackLfo sh;
    sh.init(7);
    sh.seek(LfoWave::sample_hold, 0.9f);
    const float first = sh.seek(LfoWave::sample_hold, 0.1f);
    const float held = sh.seek(LfoWave::sample_hold, 0.5f);
    CHECK_EQ(first, held);                            // held within the cycle
    const float second = sh.seek(LfoWave::sample_hold, 0.2f);
    CHECK(second != first);
    CHECK(second >= -1.0f && second <= 1.0f);
}

// --- in the engine ----------------------------------------------------------

TEST(lfo_off_by_default_and_with_either_byte_at_zero) {
    Engine &engine = fresh();
    for (int t = 0; t < kTracks; t++) {
        CHECK(!engine.lfo_on(t));
        for (int p = kParams; p < kTrackParams; p++) CHECK_EQ(int(kit_value(t, p)), 0);
    }
    lfo(LfoWave::square, 63, 127, -1);               // full depth, nowhere to go
    CHECK(!engine.lfo_on(kTrack));
    lfo(LfoWave::square, 63, 0, kDest);              // a destination, no depth
    CHECK(!engine.lfo_on(kTrack));
    for (int i = 0; i < 20; i++) {
        pump();
        CHECK_EQ(int(sounding()), 64);
    }
}

TEST(lfo_moves_its_destination_about_the_value_and_leaves_the_kit_alone) {
    Engine &engine = fresh();
    lfo(LfoWave::square, 63, 127, kDest);            // 32 Hz: a flip every dozen blocks
    CHECK(engine.lfo_on(kTrack));
    bool high = false;
    bool low = false;
    for (int i = 0; i < 60; i++) {
        pump();
        const int v = sounding();
        CHECK(v == 127 || v == 0);
        high = high || v == 127;
        low = low || v == 0;
        CHECK_EQ(int(kit_value(kTrack, kDest)), 64);   // the kit never moves
        CHECK_EQ(int(engine.performed(kTrack, kDest)), 64);
    }
    CHECK(high && low);
    // Every other track is untouched.
    for (int t = 0; t < kTracks; t++) {
        if (t != kTrack) CHECK(!engine.lfo_on(t));
    }

    // Half the depth, half the swing.
    set(kLfoDepth, 64);
    pump();
    for (int i = 0; i < 30; i++) {
        pump();
        const int v = sounding();
        CHECK(v == 96 || v == 32);
    }
    lfo(LfoWave::square, 63, 0, kDest);              // off again: the kit value
    CHECK(!engine.lfo_on(kTrack));
    CHECK_EQ(int(sounding()), 64);
}

TEST(lfo_centres_on_a_locked_value_and_a_step_can_lock_the_lfo_itself) {
    Engine &engine = fresh();
    lfo(LfoWave::square, 63, 40, kDest);             // about +-20 counts
    // Step 0: the destination locked to 20. Step 1: the LFO's depth locked to 0.
    post(Cmd::toggle_trig, kTrack, 0, 1);            // trigless: locks without a note
    post(Cmd::toggle_trig, kTrack, 1, 1);
    post(Cmd::lock_delta, kTrack, 0, kDest, uint8_t(int8_t(-44)));
    post(Cmd::lock_delta, kTrack, 1, kLfoDepth, uint8_t(int8_t(-40)));
    pump();

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    bool seen_high = false;
    for (int i = 0; i < 40; i++) {
        pump();
        const int v = sounding();
        CHECK(v == 40 || v == 0);                    // 20 +- 20
        seen_high = seen_high || v == 40;
    }
    CHECK(seen_high);
    CHECK(engine.lfo_on(kTrack));

    FakeHost::the().deliver_tick(24, 0);             // step 1: DEP locked to 0
    pump();
    CHECK(!engine.lfo_on(kTrack));
    CHECK_EQ(int(sounding()), 64);
    FakeHost::the().set_running(false);
}

TEST(lfo_synced_takes_its_phase_from_the_clock_position) {
    Engine &engine = fresh();
    lfo(LfoWave::square, 76, 127, kDest);            // one cycle a bar: 384 ticks
    FakeHost::the().set_running(true);
    FakeHost::the().set_position(0, 0.5f);
    pump();
    CHECK(engine.lfo_on(kTrack));
    CHECK_EQ(int(sounding()), 127);                  // first half of the bar
    FakeHost::the().set_position(100);
    pump();
    CHECK_EQ(int(sounding()), 127);
    FakeHost::the().set_position(200);               // second half
    pump();
    CHECK_EQ(int(sounding()), 0);
    FakeHost::the().set_position(384 * 37 + 10);     // any bar, the same place in it
    pump();
    CHECK_EQ(int(sounding()), 127);
    FakeHost::the().set_position(384 * 37 + 300);
    pump();
    CHECK_EQ(int(sounding()), 0);
    FakeHost::the().set_running(false);
    FakeHost::the().set_position(0);
}

TEST(lfo_synced_keeps_moving_at_the_tempo_while_stopped) {
    fresh();
    FakeHost::the().set_running(false);
    lfo(LfoWave::square, 116, 127, kDest);           // a sixteenth: 24 ticks
    // At 130 BPM a tick is about 231 frames, so a sixteenth is about 87 blocks.
    int flips = 0;
    int last = sounding();
    for (int i = 0; i < 400; i++) {
        pump();
        if (sounding() != last) flips++;
        last = sounding();
    }
    CHECK(flips >= 6 && flips <= 12);                // about 9 half-cycles in 400 blocks
}

TEST(lfo_a_macro_can_drive_the_depth) {
    Engine &engine = fresh();
    lfo(LfoWave::square, 63, 0, kDest);
    CHECK(!engine.lfo_on(kTrack));
    Command assign;
    assign.cmd = Cmd::macro_assign;
    assign.track = kTrack;
    assign.a = 0;
    assign.b = uint8_t(kLfoDepth);
    assign.c = 0;
    assign.d = 127;
    strata::test::engine().commands().post(assign);
    post(Cmd::set_macro, 0, 0, 0, 127);
    pump(2);
    CHECK_EQ(int(engine.performed(kTrack, kLfoDepth)), 127);
    CHECK(engine.lfo_on(kTrack));
    post(Cmd::set_macro, 0, 0, 0, 0);
    post(Cmd::macro_clear, 0, 0, 0);
    pump(2);
    CHECK(!engine.lfo_on(kTrack));
}

TEST(lfo_a_scene_stores_and_morphs_the_lfo) {
    Engine &engine = fresh();
    lfo(LfoWave::square, 63, 100, kDest);
    post(Cmd::scene_capture, kTrack, 0, 3, 1);       // scene 4, this track
    pump();
    CHECK(scene_has(engine.scenes().scenes[3], kTrack, kLfoDepth));
    CHECK_EQ(int(engine.scenes().scenes[3].values[kTrack][kLfoDepth]), 100);

    set(kLfoDepth, 0);                               // the kit's LFO off
    post(Cmd::scene_select, 0, 0, 0, 5);             // A: an empty scene
    post(Cmd::scene_select, 0, 0, 1, 3);             // B: the one with the LFO
    Command morph;
    morph.cmd = Cmd::set_morph;
    morph.u = kMorphScale;
    strata::test::engine().commands().post(morph);
    pump(2);
    CHECK_EQ(int(engine.performed(kTrack, kLfoDepth)), 100);
    CHECK(engine.lfo_on(kTrack));
    morph.u = 0;
    strata::test::engine().commands().post(morph);
    pump(2);
    CHECK(!engine.lfo_on(kTrack));
    post(Cmd::scene_erase, 0, 0, 3);
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);
    pump();
}

// Two engines, one with every LFO byte untouched and one with WAV and RAT turned on every track
// and depth or destination -- never both -- set: the LFO is off on both, and the output must be
// the same bits. This is the guarantee the render goldens lean on.
TEST(lfo_an_lfo_that_is_off_is_bit_exact) {
    static Engine *plain = nullptr;
    static Engine *primed = nullptr;
    if (plain == nullptr) {
        plain = new Engine;
        primed = new Engine;
        FakeHost::the().set_tempo(130.0f);
        CHECK(plain->init(float(ENAGA_SAMPLE_RATE)));
        CHECK(primed->init(float(ENAGA_SAMPLE_RATE)));
    }
    auto post_to = [](Engine *e, int track, int param, uint8_t value) {
        Command command;
        command.cmd = Cmd::set_param;
        command.track = uint8_t(track);
        command.a = uint8_t(param);
        command.b = value;
        CHECK(e->commands().post(command));
    };
    for (int t = 0; t < kTracks; t++) {
        post_to(primed, t, kLfoWave, uint8_t(20 * t));
        post_to(primed, t, kLfoRate, uint8_t(16 * t + 5));
        if ((t & 1) != 0) post_to(primed, t, kLfoDepth, 127);
        else post_to(primed, t, kLfoDest, lfo_dest_byte(t));
    }

    FakeHost::the().set_running(true);
    const double frames_per_tick = double(ENAGA_SAMPLE_RATE) * 60.0 / (130.0 * 96.0);
    double next = 0.0;
    uint32_t tick = 0;
    static float out[2][2 * kBlk];
    int differing = 0;
    float peak = 0.0f;
    Engine *const engines[2] = {plain, primed};
    for (int b = 0; b < 3000; b++) {
        const double block_start = double(b * kBlk);
        for (int e = 0; e < 2; e++) {
            for (double t = next; t < block_start + double(kBlk); t += frames_per_tick) {
                FakeHost::the().deliver_tick(tick + uint32_t((t - next) / frames_per_tick + 0.5),
                                             uint32_t(t - block_start));
            }
            engines[e]->process(nullptr, out[e], uint32_t(kBlk));
        }
        while (next < block_start + double(kBlk)) {
            next += frames_per_tick;
            tick++;
        }
        if (__builtin_memcmp(out[0], out[1], sizeof out[0]) != 0) differing++;
        for (int i = 0; i < 2 * kBlk; i++) {
            const float a = out[0][i] < 0.0f ? -out[0][i] : out[0][i];
            if (a > peak) peak = a;
        }
    }
    FakeHost::the().set_running(false);
    for (int t = 0; t < kTracks; t++) CHECK(!primed->lfo_on(t));
    CHECK_EQ(differing, 0);
    CHECK(peak > 0.1f);                              // and it really played
}
