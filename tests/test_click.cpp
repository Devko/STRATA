// The metronome and the count-in (CyDrums S6): dsp/click.hpp, Cmd::set_click, Cmd::count_in and
// Engine::metronome_tick.
//
// Pinned here: the click is silent before its frame and gone after kFrames, a bar's is louder;
// it clicks on every beat while CLICK is on and on no other tick; a count-in clicks its bars
// whether CLICK is on or not, holds the sequencer back for exactly those bars and then plays
// step 1 on the tick a plain START would have, with the playhead and Status::tick counted from
// there; and a STOP or a restart ends it.
#include "apprig.hpp"
#include "check.hpp"
#include "click.hpp"
#include "fakehost.hpp"

#include <cstring>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;

namespace {

constexpr int kKick = 0;
constexpr uint32_t kBeat = 96;
constexpr uint32_t kBar = 384;

float energy(const float *lr, int from, int to) {
    float e = 0.0f;
    for (int i = from; i < to; i++) e += lr[2 * i] * lr[2 * i] + lr[2 * i + 1] * lr[2 * i + 1];
    return e;
}

uint8_t triggers(int track) {
    return strata::test::engine().status().triggered[track].load(std::memory_order_relaxed);
}

// One tick in the next block, and how many notes the kick started in it.
int fired(uint32_t tick) {
    const uint8_t before = triggers(kKick);
    FakeHost::the().deliver_tick(tick, 0);
    pump();
    return int(uint8_t(triggers(kKick) - before));
}

// The blank project with one trig, on the kick's step 1, and the clock stopped.
Engine &one_trig() {
    Engine &engine = fresh();
    FakeHost::the().set_running(false);
    post(Cmd::set_click, 0, 0, 0);
    post(Cmd::toggle_trig, kKick, 0);
    pump();
    return engine;
}

// The blank project, no trigs, nothing ringing: any output is the click.
Engine &silent() {
    Engine &engine = fresh();
    FakeHost::the().set_running(false);
    post(Cmd::set_click, 0, 0, 0);
    pump();
    CHECK(strata::test::settle());
    return engine;
}

// Did this block click? The master path is silent with no notes, so any output is the click.
bool clicked(uint32_t tick) {
    FakeHost::the().deliver_tick(tick, 0);
    pump();
    const float *out = strata::test::last_block();
    return energy(out, 0, strata::test::kRigBlock) > 0.0f;
}

}  // namespace

TEST(click_starts_on_its_frame_decays_and_ends) {
    static float out[2 * 4096];
    Click click;
    click.init(48000.0f);
    std::memset(out, 0, sizeof out);
    click.trigger(100, false);
    CHECK(click.active());
    click.render_add(out, 512);
    CHECK_EQ(energy(out, 0, 100), 0.0f);
    CHECK(energy(out, 100, 200) > 0.0f);
    // The two channels are the same click.
    for (int i = 0; i < 512; i++) CHECK_EQ(out[2 * i], out[2 * i + 1]);
    // It decays: its first 2 ms hold far more than 2 ms a quarter of the way through.
    CHECK(energy(out, 100, 196) > 20.0f * energy(out, 676, 772));
    // Rendered on through kFrames, it is over, and then it adds nothing.
    for (int b = 0; b < 3; b++) click.render_add(out + 2 * 512 * (b + 1), 512);
    CHECK(!click.active());
    float after[256] = {};
    click.render_add(after, 128);
    CHECK_EQ(energy(after, 0, 128), 0.0f);

    // A bar's click is louder than a beat's.
    static float bar[2 * 512];
    static float beat[2 * 512];
    std::memset(bar, 0, sizeof bar);
    std::memset(beat, 0, sizeof beat);
    click.trigger(0, true);
    click.render_add(bar, 512);
    click.trigger(0, false);
    click.render_add(beat, 512);
    CHECK(energy(bar, 0, 512) > 1.5f * energy(beat, 0, 512));
    // A frame past the block starts on its first frame (the engine never asks for that).
    click.stop();
    std::memset(beat, 0, sizeof beat);
    click.trigger(9999, false);
    click.render_add(beat, 64);
    CHECK(energy(beat, 0, 4) > 0.0f);
}

TEST(click_over_a_loud_mix_stays_inside_full_scale) {
    // The click goes on after the master limiter, over a mix that may sit at full scale. Its
    // header promised the sum was clamped as the audition's is; nothing clamped it.
    Click click;
    click.init(48000.0f);
    click.trigger(0, true);                     // the bar's click, the louder
    float out[2 * 64];
    for (int i = 0; i < 2 * 64; i++) out[i] = (i & 2) ? -0.95f : 0.95f;
    click.render_add(out, 64);
    for (int i = 0; i < 2 * 64; i++) {
        CHECK(out[i] <= 1.0f);
        CHECK(out[i] >= -1.0f);
    }
}

TEST(click_on_every_beat_while_on_and_never_off_the_beat) {
    silent();
    FakeHost::the().set_running(true);
    pump();
    // Off: nothing on a beat.
    CHECK(!clicked(kBar));
    post(Cmd::set_click, 0, 0, 1);
    pump();
    CHECK(clicked(2 * kBar));
    pump(40);                                    // let it ring out
    CHECK(!clicked(2 * kBar + 1));               // not between beats
    pump(40);
    CHECK(clicked(2 * kBar + kBeat));            // a beat that is not a bar's
    pump(40);
    post(Cmd::set_click, 0, 0, 0);
    pump(40);
    CHECK(!clicked(3 * kBar));
    FakeHost::the().set_running(false);
    pump();
}

TEST(click_a_count_in_holds_the_pattern_for_its_bars_then_plays_step_1) {
    Engine &engine = one_trig();
    // The kick's notes are counted (Status::triggered), so the count-in's clicks and the
    // pattern's notes are told apart.
    post(Cmd::count_in, 0, 0, 1);
    pump();
    FakeHost::the().set_running(true);
    // The whole count-in bar: no note, and the count-in counts down 4 3 2 1.
    CHECK_EQ(fired(0), 0);
    CHECK_EQ(int(engine.status().count_in_beats.load()), 4);
    CHECK_EQ(int(engine.status().tick.load()), 0);
    int notes = 0;
    for (uint32_t t = 1; t < kBar; t++) notes += fired(t);
    CHECK_EQ(notes, 0);
    // Step 1 on the tick after it: the pattern's tick 0.
    CHECK_EQ(fired(kBar), 1);
    CHECK_EQ(int(engine.status().count_in_beats.load()), 0);
    CHECK_EQ(int(engine.status().count_in_ticks.load()), int(kBar));
    // And a whole pass on, again, as from a plain START.
    const uint32_t pass = 64 * 24;              // a blank pattern's 64 steps
    notes = 0;
    for (uint32_t t = kBar + 1; t < kBar + pass; t++) notes += fired(t);
    CHECK_EQ(notes, 0);
    CHECK_EQ(fired(kBar + pass), 1);

    // A STOP ends it: the next START is a plain one.
    FakeHost::the().set_running(false);
    pump();
    CHECK_EQ(int(engine.status().count_in_ticks.load()), 0);
    post(Cmd::restart);
    pump();
    FakeHost::the().set_running(true);
    CHECK_EQ(fired(0), 1);
    FakeHost::the().set_running(false);
    pump();
}

TEST(click_a_count_in_clicks_with_click_off_and_a_restart_cancels_it) {
    Engine &engine = one_trig();
    post(Cmd::count_in, 0, 0, 2);
    pump();
    FakeHost::the().set_running(true);
    CHECK(clicked(0));
    pump(40);
    CHECK(clicked(kBar + kBeat));                // the second bar of two still clicks
    CHECK_EQ(int(engine.status().count_in_beats.load()) > 0, true);
    // A restart (F1 + PLAY) jumps to the pattern's start: no more count-in.
    post(Cmd::restart);
    pump();
    CHECK_EQ(fired(0), 1);
    CHECK_EQ(int(engine.status().count_in_ticks.load()), 0);
    // A count-in of more than two bars is two.
    FakeHost::the().set_running(false);
    pump();
    post(Cmd::count_in, 0, 0, 9);
    pump();
    FakeHost::the().set_running(true);
    pump();
    CHECK_EQ(int(engine.status().count_in_ticks.load()), int(2 * kBar));
    FakeHost::the().set_running(false);
    pump();
    // The UI posts it and then starts the clock, so it may land in a block that already reads
    // running: it still counts in.
    post(Cmd::count_in, 0, 0, 1);
    FakeHost::the().set_running(true);
    CHECK_EQ(fired(0), 0);
    CHECK_EQ(int(engine.status().count_in_ticks.load()), int(kBar));
    FakeHost::the().set_running(false);
    pump();
}
