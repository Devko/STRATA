// The per-track LFO: dsp/lfo.hpp's byte meanings and oscillator, and what app/engine.cpp does
// with it -- block rate, after the locks, off when its bytes are zero, and bit-exact when off.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include <cstring>

#include "lfo.hpp"
#include "project.hpp"
#include "va.hpp"
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

TEST(lfo_wave_and_dest_are_numbers_and_one_click_is_one_of_them) {
    // Format decision C (2026-11-26): WAV is the shape's number and DST 0 for OFF or 1-16, not
    // bands of the 0-127 byte, so a seventh shape would not move the meaning of any stored byte.
    for (int w = 0; w < kLfoWaves; w++) {
        CHECK_EQ(int(lfo_wave_byte(LfoWave(w))), w);
        CHECK_EQ(int(lfo_wave(uint8_t(w))), w);
    }
    // Past the last is the last, whatever put it there.
    CHECK_EQ(int(lfo_wave(uint8_t(kLfoWaves))), kLfoWaves - 1);
    CHECK_EQ(int(lfo_wave(127)), kLfoWaves - 1);
    CHECK_EQ(int(lfo_wave(255)), kLfoWaves - 1);

    CHECK_EQ(lfo_dest(0), -1);                       // zero is OFF
    for (int p = -1; p < kParams; p++) {
        CHECK_EQ(int(lfo_dest_byte(p)), p + 1);
        CHECK_EQ(lfo_dest(lfo_dest_byte(p)), p);
    }
    CHECK_EQ(lfo_dest(uint8_t(kParams + 1)), kParams - 1);
    CHECK_EQ(lfo_dest(127), kParams - 1);

    // One click is one shape, one destination, one count: the same everywhere now.
    CHECK_EQ(lfo_click(kLfoWave), 1);
    CHECK_EQ(lfo_click(kLfoRate), 1);
    CHECK_EQ(lfo_click(kLfoDepth), 1);
    CHECK_EQ(lfo_click(kLfoDest), 1);

    // And the highest value a knob may set (core/project.hpp): the last shape and destination.
    CHECK_EQ(int(kit_param_max(kLfoWave)), kLfoWaves - 1);
    CHECK_EQ(int(kit_param_max(kLfoDest)), kParams);
    CHECK_EQ(int(kit_param_max(kLfoRate)), 127);
    CHECK_EQ(int(kit_param_max(kLfoDepth)), 127);
    CHECK_EQ(int(kit_param_max(0)), 127);
}

TEST(lfo_an_old_band_byte_reads_as_the_shape_or_destination_it_meant) {
    // What a file from before decision C holds, converted once on read (kit_lfo_from_band): the
    // bands' old decoding, kept here so every stored byte keeps its sound.
    for (int b = 0; b < 128; b++) {
        CHECK_EQ(int(kit_lfo_from_band(kLfoWave, uint8_t(b))), (b * kLfoWaves) >> 7);
        CHECK_EQ(int(kit_lfo_from_band(kLfoDest, uint8_t(b))), (b * (kParams + 1)) >> 7);
        CHECK_EQ(int(kit_lfo_from_band(kLfoRate, uint8_t(b))), b);      // not WAV or DST: as is
    }
    // The bytes the old factory and tests wrote to pick each one: a band's first byte.
    for (int w = 0; w < kLfoWaves; w++) {
        const uint8_t old = uint8_t((w * 128 + kLfoWaves - 1) / kLfoWaves);
        CHECK_EQ(int(kit_lfo_from_band(kLfoWave, old)), w);
    }
    for (int p = -1; p < kParams; p++) {
        const uint8_t old = p < 0 ? uint8_t(0) : uint8_t(((p + 1) * 128 + kParams) / (kParams + 1));
        CHECK_EQ(int(kit_lfo_from_band(kLfoDest, old)), p + 1);
    }
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

TEST(lfo_apply_stops_at_the_floor_but_never_lifts_the_knob) {
    // Full depth down from 64 would reach 0; the floor holds it at 30.
    CHECK_EQ(int(lfo_apply(64, -1.0f, 127, 30)), 30);
    CHECK_EQ(int(lfo_apply(64, 1.0f, 127, 30)), 127);    // upwards is untouched
    CHECK_EQ(int(lfo_apply(64, -0.25f, 127, 30)), 48);   // above the floor: as before
    // A knob already under the floor stays where it is; the LFO cannot push it lower.
    CHECK_EQ(int(lfo_apply(10, -1.0f, 127, 30)), 10);
    CHECK_EQ(int(lfo_apply(10, 0.0f, 127, 30)), 10);
    CHECK_EQ(int(lfo_apply(10, 1.0f, 127, 30)), 74);
}

TEST(lfo_cutoff_floor_is_about_100_hz_on_each_cutoff) {
    const int fltr_cut = page_first(Page::fltr) + 1;
    const int src_cut = page_first(Page::src) + 2;
    CHECK_EQ(int(lfo_cutoff_floor(fltr_cut, false, true)), int(kLfoInsertCutoffFloor));
    CHECK_EQ(int(lfo_cutoff_floor(fltr_cut, true, true)), int(kLfoInsertCutoffFloor));
    CHECK_EQ(int(lfo_cutoff_floor(fltr_cut, false, false)), 0);     // MID
    CHECK_EQ(int(lfo_cutoff_floor(src_cut, true, true)), int(kLfoVaCutoffFloor));
    CHECK_EQ(int(lfo_cutoff_floor(src_cut, false, true)), 0);       // a drum's third byte
    CHECK_EQ(int(lfo_cutoff_floor(page_first(Page::fltr) + 2, true, true)), 0);
    // The bytes sit on 100 Hz of each curve, within a few hertz.
    CHECK_NEAR(20.0f * enaga::dsp::exp2(9.8138f * kLfoInsertCutoffFloor / 127.0f), 100.0f, 3.0f);
    CHECK_NEAR(35.0f * enaga::dsp::exp2(8.4214f * kLfoVaCutoffFloor / 127.0f), 100.0f, 3.0f);
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

TEST(lfo_on_a_cutoff_stops_at_the_floor) {
    Engine &engine = fresh();
    const int cut = page_first(Page::fltr) + 1;
    set(cut, 64);
    lfo(LfoWave::square, 63, 127, cut);
    bool high = false;
    bool low = false;
    for (int i = 0; i < 60; i++) {
        pump();
        const int v = engine.sounding(kTrack).v[cut];
        // Full depth on a cutoff is about three octaves either side (39 counts of FLTR CUT),
        // not 64 counts; and down it stops at the floor (64 - 39 = 25 is under it).
        CHECK(v == 64 + int(kLfoInsertCutoffSpan) || v == int(kLfoInsertCutoffFloor));
        high = high || v == 64 + int(kLfoInsertCutoffSpan);
        low = low || v == int(kLfoInsertCutoffFloor);
    }
    CHECK(high && low);
    // The snare's third SRC byte is TONE, not a cutoff: it still swings to 0 (the test above).
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

// --- TRIG: FREE, RETRIG, ONE-SHOT (dsp/lfo.hpp, "TRIG") ----------------------------------------

TEST(lfo_trig_bytes_names_and_a_damaged_byte_runs_free) {
    CHECK(lfo_trig(0) == LfoTrig::free);
    CHECK(lfo_trig(1) == LfoTrig::retrig);
    CHECK(lfo_trig(2) == LfoTrig::one_shot);
    CHECK(lfo_trig(3) == LfoTrig::free);
    CHECK(lfo_trig(255) == LfoTrig::free);
    CHECK(std::strcmp(lfo_trig_name(LfoTrig::free), "FREE") == 0);
    CHECK(std::strcmp(lfo_trig_name(LfoTrig::retrig), "RETRIG") == 0);
    CHECK(std::strcmp(lfo_trig_name(LfoTrig::one_shot), "ONE-SHOT") == 0);
    // The kit's numbers are the enum's (app/engine.cpp asserts it too).
    CHECK_EQ(int(kLfoTrigOneShot), int(LfoTrig::one_shot));
}

TEST(lfo_one_shot_shapes_are_envelopes_that_start_and_end_at_the_knob) {
    using W = LfoWave;
    // SIN: one wobble, 0 at both ends.
    CHECK_EQ(TrackLfo::one_shot_value(W::sine, 0.0f, 0.0f), 0.0f);
    CHECK_NEAR(TrackLfo::one_shot_value(W::sine, 0.25f, 0.0f), 1.0f, 1e-3f);
    CHECK_NEAR(TrackLfo::one_shot_value(W::sine, 0.75f, 0.0f), -1.0f, 1e-3f);
    CHECK_EQ(TrackLfo::one_shot_value(W::sine, 1.0f, 0.0f), 0.0f);
    // TRI: attack to the top half way, decay back.
    CHECK_EQ(TrackLfo::one_shot_value(W::triangle, 0.0f, 0.0f), 0.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::triangle, 0.25f, 0.0f), 0.5f);
    CHECK_EQ(TrackLfo::one_shot_value(W::triangle, 0.5f, 0.0f), 1.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::triangle, 0.75f, 0.0f), 0.5f);
    CHECK_EQ(TrackLfo::one_shot_value(W::triangle, 1.0f, 0.0f), 0.0f);
    // RMP rises and holds; SAW is a decay from the top.
    CHECK_EQ(TrackLfo::one_shot_value(W::ramp, 0.0f, 0.0f), 0.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::ramp, 0.5f, 0.0f), 0.5f);
    CHECK_EQ(TrackLfo::one_shot_value(W::ramp, 1.0f, 0.0f), 1.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::saw, 0.0f, 0.0f), 1.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::saw, 0.5f, 0.0f), 0.5f);
    CHECK_EQ(TrackLfo::one_shot_value(W::saw, 1.0f, 0.0f), 0.0f);
    // SQR: a gate for the first half.
    CHECK_EQ(TrackLfo::one_shot_value(W::square, 0.49f, 0.0f), 1.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::square, 0.5f, 0.0f), 0.0f);
    CHECK_EQ(TrackLfo::one_shot_value(W::square, 1.0f, 0.0f), 0.0f);
    // S+H: its value, the whole cycle and after.
    CHECK_EQ(TrackLfo::one_shot_value(W::sample_hold, 0.3f, -0.4f), -0.4f);
    CHECK_EQ(TrackLfo::one_shot_value(W::sample_hold, 1.0f, -0.4f), -0.4f);
}

TEST(lfo_one_shot_waits_for_a_note_runs_one_cycle_and_stops) {
    TrackLfo lfo;
    lfo.init(7);
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.3f), 0.0f);     // no note yet: at rest
    CHECK_EQ(lfo.one_shot(LfoWave::ramp, 0.3f), 0.0f);    // RMP too, before its first rise
    lfo.restart();
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.0f), 1.0f);
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.25f), 0.75f);
    CHECK(!lfo.one_shot_done());
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.9f), 0.0f);     // past the end: stopped, at rest
    CHECK(lfo.one_shot_done());
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.25f), 0.0f);    // and it stays there
    CHECK_EQ(lfo.one_shot(LfoWave::ramp, 0.25f), 1.0f);   // a finished rise holds the top
    // Each note draws a new S+H value and keeps it.
    lfo.restart();
    const float first = lfo.one_shot(LfoWave::sample_hold, 0.0f);
    CHECK_EQ(lfo.one_shot(LfoWave::sample_hold, 2.0f), first);
    lfo.restart();
    CHECK(lfo.one_shot(LfoWave::sample_hold, 0.0f) != first);
    lfo.rest();
    CHECK_EQ(lfo.one_shot(LfoWave::saw, 0.1f), 0.0f);
}

TEST(lfo_restart_goes_back_to_phase_zero_and_draws_a_new_sample) {
    TrackLfo lfo;
    lfo.init(11);
    lfo.advance(LfoWave::saw, 0.4f);
    CHECK_NEAR(lfo.phase(), 0.4f, 1e-6f);
    lfo.restart();
    CHECK_EQ(lfo.phase(), 0.0f);
    CHECK_EQ(lfo.advance(LfoWave::saw, 0.0f), 1.0f);      // SAW starts at its top
    const float held = lfo.advance(LfoWave::sample_hold, 0.0f);
    lfo.restart();
    CHECK(lfo.advance(LfoWave::sample_hold, 0.0f) != held);
}

namespace {

void trig_mode(uint8_t mode) {
    post(Cmd::set_lfo_trig, kTrack, 0, mode);
    pump();
}

void note() { post(Cmd::trigger_note, kTrack, 0, 60, 100); }

}  // namespace

TEST(lfo_retrig_restarts_the_cycle_on_every_note_and_free_does_not) {
    Engine &engine = fresh();
    lfo(LfoWave::saw, 28, 127, kDest);               // 1 Hz, falling from the top
    // From the top of a cycle: a FREE LFO keeps its phase through a project load, so without
    // this it starts wherever the test before left it (a synced one, on the clock's position) and
    // "part of a cycle in" below held only in the suite's own order. A RETRIG note puts it there.
    trig_mode(kLfoTrigRetrig);
    note();
    pump();
    trig_mode(kLfoTrigFree);
    pump(200);
    const int free_before = sounding();
    CHECK(free_before < 120);                        // part of a cycle in
    note();
    pump();
    CHECK(sounding() <= free_before);                // FREE: the note moved nothing

    trig_mode(kLfoTrigRetrig);
    CHECK_EQ(int(engine.project().kit.tracks[kTrack].lfo_trig), int(kLfoTrigRetrig));
    pump(100);
    CHECK(sounding() < 120);
    note();
    pump();
    CHECK_EQ(int(sounding()), 127);                  // back to the top on the note's block
    pump(40);
    const int later = sounding();
    CHECK(later < 127 && later > 100);               // and falling again from there
    note();
    pump();
    CHECK_EQ(int(sounding()), 127);

    trig_mode(kLfoTrigFree);
    lfo(LfoWave::saw, 28, 0, kDest);
}

TEST(lfo_one_shot_is_an_envelope_on_each_note) {
    Engine &engine = fresh();
    trig_mode(kLfoTrigOneShot);
    lfo(LfoWave::saw, 63, 127, kDest);               // 32 Hz: one cycle is 31 ms
    pump(10);
    CHECK(engine.lfo_on(kTrack));
    CHECK_EQ(int(sounding()), 64);                   // waiting for a note, at the knob
    note();
    pump();
    CHECK_EQ(int(sounding()), 127);                  // the note: DEP up
    int last = 127;
    const int cycle_blocks = int(float(ENAGA_SAMPLE_RATE) / (32.0f * float(kBlk))) + 2;
    for (int i = 0; i < cycle_blocks; i++) {
        pump();
        CHECK(int(sounding()) <= last);              // decaying, never rising
        last = sounding();
    }
    CHECK_EQ(int(sounding()), 64);                   // and back at the knob
    pump(50);
    CHECK_EQ(int(sounding()), 64);                   // where it stays until the next note
    note();
    pump();
    CHECK_EQ(int(sounding()), 127);

    // TRI is an attack and a decay: up to DEP half way, back to the knob.
    set(kLfoWave, lfo_wave_byte(LfoWave::triangle));
    note();
    pump();
    CHECK_EQ(int(sounding()), 64);
    int top = 0;
    for (int i = 0; i < cycle_blocks; i++) {
        pump();
        if (int(sounding()) > top) top = sounding();
    }
    CHECK(top >= 120);
    CHECK_EQ(int(sounding()), 64);

    trig_mode(kLfoTrigFree);
    lfo(LfoWave::saw, 63, 0, kDest);
}

TEST(lfo_on_tune_glides_on_a_synth_and_steps_the_byte_elsewhere) {
    Engine &engine = fresh();
    constexpr int kVa = 5;                           // VA MONO in the rig's kit
    CHECK_EQ(int(engine.machine_id(kVa)), int(MachineId::va_mono));
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoWave), lfo_wave_byte(LfoWave::sine));
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoRate), 20);
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoDepth), 100);
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoDest), lfo_dest_byte(0));
    pump(40);
    CHECK(engine.lfo_on(kVa));
    // The byte stays on the knob; the machine gets the offset as a float.
    CHECK_EQ(int(engine.sounding(kVa).v[0]), 64);
    const float mod = engine.lfo_pitch_mod(kVa);
    CHECK(mod != 0.0f);
    CHECK_EQ(mod, lfo_pitch_semis(engine.lfo_value(kVa), 100));
    const auto *va = static_cast<const VaMono *>(engine.machine(kVa));
    CHECK_EQ(va->settings().pitch_mod, mod);
    // What the screen shows is the byte it would have been.
    CHECK_EQ(int(engine.lfo_shown(kVa)), int(lfo_apply(64, engine.lfo_value(kVa), 100)));
    // And the screen reads it from Status (Engine::publish_heard), the same byte.
    CHECK_EQ(int(engine.status().lfo_shown[kVa].load()), int(engine.lfo_shown(kVa)));

    // Off: the machine is handed 0.
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoDepth), 0);
    pump(2);
    CHECK_EQ(engine.lfo_pitch_mod(kVa), 0.0f);
    CHECK_EQ(va->settings().pitch_mod, 0.0f);
    post(Cmd::set_param, kVa, 0, uint8_t(kLfoDest), 0);
    pump();

    // A drum's TUN is still the byte.
    lfo(LfoWave::square, 63, 127, 0);
    pump(30);
    CHECK(engine.sounding(kTrack).v[0] == 127 || engine.sounding(kTrack).v[0] == 0);
    CHECK_EQ(engine.lfo_pitch_mod(kTrack), 0.0f);
    lfo(LfoWave::square, 63, 0, 0);
}

TEST(lfo_pitch_semis_is_the_byte_offset_unrounded) {
    // DEP 127 at full swing: 64 counts of TUN, 24 semitones.
    CHECK_NEAR(lfo_pitch_semis(1.0f, 127), 24.0f, 1e-4f);
    CHECK_NEAR(lfo_pitch_semis(-0.5f, 127), -12.0f, 1e-4f);
    CHECK_EQ(lfo_pitch_semis(0.7f, 0), 0.0f);
    // Where the byte would round to a whole count, the float does not.
    CHECK_EQ(int(lfo_apply(64, 0.01f, 127)), 65);
    CHECK_NEAR(lfo_pitch_semis(0.01f, 127), 0.24f, 1e-4f);
}

TEST(lfo_a_cutoff_swings_about_three_octaves_and_other_bytes_64_counts) {
    const int fltr_cut = page_first(Page::fltr) + 1;
    const int src_cut = page_first(Page::src) + 2;
    CHECK_EQ(lfo_span(fltr_cut, false, true), kLfoInsertCutoffSpan);
    CHECK_EQ(lfo_span(src_cut, true, false), kLfoVaCutoffSpan);
    CHECK_EQ(lfo_span(fltr_cut, true, false), kLfoSpan);   // TON on a synth
    CHECK_EQ(lfo_span(src_cut, false, true), kLfoSpan);    // a drum's third byte
    CHECK_EQ(lfo_span(fltr_cut, false, false), kLfoSpan);  // MID
    CHECK_EQ(int(lfo_apply(64, 1.0f, 127, 0, kLfoInsertCutoffSpan)), 103);
    CHECK_EQ(int(lfo_apply(64, 1.0f, 127, 0, kLfoVaCutoffSpan)), 109);
    // 39 counts of FLTR's 9.8138 octaves, and 45 of the VAs' 8.4214: about three each.
    CHECK_NEAR(kLfoInsertCutoffSpan * 9.8138f / 127.0f, 3.0f, 0.05f);
    CHECK_NEAR(kLfoVaCutoffSpan * 8.4214f / 127.0f, 3.0f, 0.05f);
    // The default span is the old arithmetic, bit for bit.
    for (int d = 0; d < 128; d += 9) {
        for (float v = -1.0f; v <= 1.0f; v += 0.0625f) {
            const float old = v * float(d) * (64.0f / 127.0f);
            const int rounded = old >= 0.0f ? int(old + 0.5f) : -int(-old + 0.5f);
            const int sum = 64 + rounded;
            const int want = sum < 0 ? 0 : (sum > 127 ? 127 : sum);
            CHECK_EQ(int(lfo_apply(64, v, uint8_t(d))), want);
        }
    }
    CHECK_EQ(lfo_cutoff_param(true, false), src_cut);
    CHECK_EQ(lfo_cutoff_param(false, true), fltr_cut);
    CHECK_EQ(lfo_cutoff_param(false, false), -1);
}

// --- the floor for the morph and the macros (D9's leftover) -----------------------------------

TEST(lfo_floor_a_morph_or_macro_stops_a_cutoff_at_about_100_hz_but_the_knob_does_not) {
    Engine &engine = fresh();
    constexpr int kKick = 0;
    const int cut = page_first(Page::fltr) + 1;
    // Scene 4 holds the cutoff shut; the kit is open.
    post(Cmd::set_param, kKick, 0, uint8_t(cut), 0);
    post(Cmd::scene_capture, kKick, 0, 3, 1);
    post(Cmd::set_param, kKick, 0, uint8_t(cut), 100);
    post(Cmd::scene_select, 0, 0, 0, 5);             // A: an empty scene, the kit
    post(Cmd::scene_select, 0, 0, 1, 3);
    Command morph;
    morph.cmd = Cmd::set_morph;
    morph.u = kMorphScale;
    strata::test::engine().commands().post(morph);
    pump(2);
    CHECK_EQ(int(engine.performed(kKick, cut)), int(kLfoInsertCutoffFloor));
    // Half way, above the floor: the plain morph.
    morph.u = kMorphScale / 2;
    strata::test::engine().commands().post(morph);
    pump(2);
    CHECK(int(engine.performed(kKick, cut)) > int(kLfoInsertCutoffFloor));
    // The knob itself under the floor: the morph can take it no lower than the knob, and the
    // knob still closes the filter.
    post(Cmd::set_param, kKick, 0, uint8_t(cut), 10);
    morph.u = kMorphScale;
    strata::test::engine().commands().post(morph);
    pump(2);
    CHECK_EQ(int(engine.performed(kKick, cut)), 10);
    CHECK_EQ(int(kit_value(kKick, cut)), 10);
    morph.u = 0;
    strata::test::engine().commands().post(morph);
    post(Cmd::scene_erase, 0, 0, 3);
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);
    pump(2);

    // A macro sweeping a VA's CUT (SRC's third byte) down to 0 stops at its floor.
    constexpr int kVa = 5;
    const int va_cut = page_first(Page::src) + 2;
    post(Cmd::set_param, kVa, 0, uint8_t(va_cut), 90);
    Command assign;
    assign.cmd = Cmd::macro_assign;
    assign.track = kVa;
    assign.a = 0;
    assign.b = uint8_t(va_cut);
    assign.c = 0;
    assign.d = 0;
    strata::test::engine().commands().post(assign);
    post(Cmd::set_macro, 0, 0, 0, 127);
    pump(2);
    CHECK_EQ(int(engine.performed(kVa, va_cut)), int(kLfoVaCutoffFloor));
    post(Cmd::set_macro, 0, 0, 0, 0);
    post(Cmd::macro_clear, 0, 0, 0);
    pump(2);
    CHECK_EQ(int(engine.performed(kVa, va_cut)), 90);
}

TEST(lfo_a_knob_or_a_lock_on_wav_or_dst_stops_at_the_last_one) {
    // Numbers (format decision C): turning WAV past S+H or DST past the 16th parameter stays
    // there, so turning back is one click and not a hundred.
    Engine &engine = fresh();
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), 0);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), uint8_t(int8_t(40)), kDeltaFlag);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoDest), uint8_t(int8_t(100)), kDeltaFlag);
    pump();
    CHECK_EQ(int(engine.params(kTrack).v[kLfoWave]), kLfoWaves - 1);
    CHECK_EQ(int(engine.params(kTrack).v[kLfoDest]), kParams);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), uint8_t(int8_t(-1)), kDeltaFlag);
    pump();
    CHECK_EQ(int(engine.params(kTrack).v[kLfoWave]), kLfoWaves - 2);   // one click back
    // An absolute value past the last is the last too.
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoDest), 90);
    pump();
    CHECK_EQ(int(engine.params(kTrack).v[kLfoDest]), kParams);

    // A lock turned on a step, the same.
    post(Cmd::lock_delta, kTrack, 3, uint8_t(kLfoWave), uint8_t(int8_t(60)));
    pump();
    uint8_t locked = 0;
    CHECK(pattern_get_lock(engine.pattern(), kTrack, 3, uint8_t(kLfoWave), locked));
    CHECK_EQ(int(locked), kLfoWaves - 1);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), 0);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoDest), 0);
    pump();
}
