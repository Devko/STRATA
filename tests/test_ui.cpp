// The UI: modes, gestures, the encoder routing and the unsaved-changes mark.
//
// `app/app.hpp` holds the App class -- every mode, every gesture, every encoder binding -- and
// until it was a header none of it was in this suite: `ENAGA_APP(Strata)` defines the package's
// entry points, so `host/Makefile` cannot compile the translation unit that holds them, and the
// class lived there too. The emulator could press one key and nothing else (`emulate.py` exposes
// `--key`: no pads, no encoders), so a review of `app/` is what found the bugs below rather than
// a test. This file drives the same class the unit runs, through the same entry points Enaga
// calls, with no `#ifdef` anywhere in it.
//
// ## Why this file does not use the rig's Engine
//
// `tests/apprig.hpp` owns one Engine and one Storage because `Engine::init` takes about 85 MiB.
// `Strata` owns BOTH BY VALUE as private members and takes them in its own `init()`, so a test of
// the App cannot borrow the rig's pair: `pump()` there drives the rig's engine, and the App would
// sit beside it doing nothing. So there is one App for this whole file, `init()` runs once, and
// `pump()` below drives ITS two contexts -- `audio()` then `update()`, which is the order Enaga
// calls them in. That is 85 MiB more in the test binary, once, which is the price of testing the
// object the unit actually runs. `peak_of` and the fake host are shared as before.
//
// ## How the state is read
//
// Through `strata::test::UiProbe`, the one friend `app/app.hpp` declares. The UI state a gesture
// leaves behind -- the held step, the selection, the mode -- has no accessor because nothing in
// the app needs one, and a friend is cheaper than sixteen test-only accessors. Everything else is
// read the way the app itself reads it: pattern memory through `Engine`, the card through
// `Storage` and `FakeHost`, the clock through `FakeHost`.
//
// What is NOT checkable here: anything about how it FEELS -- whether a gesture is findable,
// whether a hold is long enough, whether the LED language reads at a glance. `FakeHost::t_text`
// draws nothing and `t_led` only counts, so the screen is checked by its pixels (the grid test at
// the end) and the LEDs are not checked at all. docs/UNIT_CHECKLIST.md carries those.
#include "app.hpp"

#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include <enaga/abi.h>
#include <enaga/clock.hpp>

using strata::test::FakeHost;
using strata::test::peak_of;

// --- the probe -------------------------------------------------------------
//
// Read-only, static, and the only thing in the build that touches these members.

namespace strata::test {

struct UiProbe {
    static Engine &engine(Strata &app) { return app.engine_; }
    static Storage &storage(Strata &app) { return app.storage_; }

    static int mode(const Strata &app) { return int(app.mode_); }
    static int selected(const Strata &app) { return app.selected_; }
    static int page(const Strata &app) { return app.page_; }
    static int param_page(const Strata &app) { return app.param_page_; }
    static int mixer_bank(const Strata &app) { return app.mixer_bank_; }

    static int held_step(const Strata &app) { return app.held_step_; }
    static int held_key(const Strata &app) { return app.held_key_; }
    static bool held_used(const Strata &app) { return app.held_used_; }
    static int range_first(const Strata &app) { return app.range_first_; }
    static int range_last(const Strata &app) { return app.range_last_; }
    static bool step_page(const Strata &app) { return app.step_page_; }
    static int page_step(const Strata &app) { return app.page_step_; }

    static bool rec(const Strata &app) { return app.rec_; }
    static bool mute_mode(const Strata &app) { return app.mute_mode_; }
    static int scene_holding(const Strata &app) { return app.scene_holding_; }
    static int keys_octave(const Strata &app) { return app.keys_octave_; }

    static const char *notice(const Strata &app) { return app.notice_; }
    static bool notice_showing(const Strata &app) { return app.notice_showing(); }

    static uint32_t tempo_tenths(const Strata &app) { return app.tempo_tenths_; }
    static bool clock_refused(const Strata &app) { return app.clock_refused_; }
};

}  // namespace strata::test

using strata::test::UiProbe;

namespace {

constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kStereo = 2 * kBlk;

// Parameter 11 is LVL on every machine (dsp/track.hpp), which is what `fresh()` turns up so that
// a track can be heard at all.
constexpr int kLvl = 11;

float g_out[kStereo];
uint32_t g_ms = 0;
uint8_t g_pixels[enaga::Canvas::width * enaga::Canvas::height];

// The one App. A function-local static so that `init()` -- 85 MiB and a card read -- happens on
// the first test that asks for it and not before `FakeHost::reset()` has wiped the card.
Strata &the_app() {
    static Strata app;
    return app;
}

Engine &app_engine() { return UiProbe::engine(the_app()); }
Storage &app_storage() { return UiProbe::storage(the_app()); }

// One audio block and then one pass of the main context, which is the order `App::audio` and
// `App::update` really run in. Returns the loudest sample of the blocks it rendered.
//
// Both sides have to be driven by hand for the reason tests/apprig.hpp gives: `service()` waits
// while the command ring is non-empty -- that is how a save is deferred past every pending edit
// -- and only `Engine::process` drains it, so a test that calls one without the other waits for
// ever. A millisecond per block is about real time (a block is 1.33 ms), which matters because
// `notice_showing()` is a deadline in milliseconds.
float pump(int blocks = 1) {
    Strata &app = the_app();
    float peak = 0.0f;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < kStereo; i++) g_out[i] = 0.0f;
        app.audio(nullptr, g_out, uint32_t(kBlk));
        const float block_peak = peak_of(g_out, kStereo);
        if (block_peak > peak) peak = block_peak;
        app.update(g_ms);
        g_ms += 1;
        FakeHost::the().complete_loads();
    }
    return peak;
}

// Wait for a card job to start and then finish, exactly as tests/apprig.hpp's does and for the
// same reason: a request only sets a flag, and the job does not begin until `service()` finds the
// ring drained.
bool pump_until_idle(int limit = 4000) {
    bool started = false;
    for (int i = 0; i < limit; i++) {
        pump();
        if (app_storage().busy_text() != nullptr) {
            started = true;
        } else if (started && FakeHost::the().pending_loads() == 0) {
            return true;
        }
    }
    return false;
}

// A command straight into the app's own ring, for the two things the UI cannot do: hand the
// engine a whole project, and cut the tails the test before left ringing.
void post_cmd(Cmd cmd, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
    Command command;
    command.cmd = cmd;
    command.a = a;
    command.b = b;
    command.c = c;
    app_engine().commands().post(command);
}

void key(int code) {                       // a tap: press and release
    the_app().on_key(code, true);
    the_app().on_key(code, false);
}

void shift_key(int code) {                 // F1 + that key, F1 released after
    the_app().on_key(ENAGA_KEY_F1, true);
    the_app().on_key(code, true);
    the_app().on_key(code, false);
    the_app().on_key(ENAGA_KEY_F1, false);
}

void step_down(int raw) { the_app().on_key(ENAGA_KEY_STEP1 + raw, true); }
void step_up(int raw) { the_app().on_key(ENAGA_KEY_STEP1 + raw, false); }

const Step &step_of(int track, int step) {
    return app_engine().pattern().tracks[track].steps[step];
}

bool trig_at(int track, int step) { return (step_of(track, step).flags & kStepTrig) != 0; }

int locks_on_track(int track) {
    int total = 0;
    for (int s = 0; s < kStepsPerTrack; s++) total += int(step_of(track, s).lock_count);
    return total;
}

uint8_t undo_depth() {
    return app_engine().status().undo_depth.load(std::memory_order_relaxed);
}

// The parameter index encoder A writes on the page that is showing. Read rather than assumed, so
// that a test does not depend on which parameter page a test before it left open.
int encoder_param(int encoder) {
    return page_first(Page(UiProbe::param_page(the_app()))) + encoder;
}

// Back to a known state, by the keys and pads that do it rather than by writing the fields: F4 is
// the production path that closes the step page and drops a hold, and a fresh single STEP press is
// the one that drops a selection. Then a blank project, as tests/apprig.hpp's `fresh()` does it --
// `Engine::init` loads the default beat once, and a test that inherited it would also inherit
// whatever the test before it did to it.
Strata &fresh() {
    FakeHost &host = FakeHost::the();
    host.reset();

    Strata &app = the_app();
    static bool ready = false;
    if (!ready) {
        ready = app.init();
        CHECK(ready);
    }

    // A card SWAP, not just "a card is there": `reset()` wiped the card without telling Storage,
    // which is what pulling an SD card does on the unit, and the transition is what clears its
    // "the project is complete on the card" flag.
    app.on_card(false);
    app.on_card(true);

    // Every modifier up, every held pad released.
    app.on_key(ENAGA_KEY_F1, false);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
    app.on_pad_release(1);                 // FILL
    app.on_pad_release(3);                 // REPEAT
    app.on_pad_release(7);                 // scene A
    app.on_pad_release(9);                 // scene B
    app.on_pad_release(11);                // THROW
    if (UiProbe::mute_mode(app)) {         // the MUTE pad is a toggle, so it takes another press
        app.on_pad_press(5, 100);
        app.on_pad_release(5);
    }
    if (UiProbe::rec(app)) key(ENAGA_KEY_F2);

    key(ENAGA_KEY_F4);                     // GRID: closes the step page and drops any hold
    for (int i = 0; i < 4; i++) key(ENAGA_KEY_OCTAVE_LEFT);   // step page 1 of 4, clamped at 0
    step_down(0);                          // a fresh single press drops any selection...
    step_up(0);                            // ...and its release toggles a trig, wiped below
    app.on_pad_press(0, 100);              // track 1 selected
    app.on_pad_release(0);

    // Drain what those presses posted BEFORE the project is replaced. The other order looks the
    // same and is not: the commands are applied to `project_.live()`, which is the memory
    // `project_for_load` hands out, so a toggle still in the ring lands on the blank pattern
    // AFTER it was blanked and the next test starts with a trig it did not put down.
    pump(2);

    // A blank project with a kit that can make a sound. `project_init` zeroes the kit, so LVL is 0
    // on every track and the whole instrument is silent -- correct for a blank project and useless
    // as a starting point.
    Project &incoming = app_engine().project_for_load();
    project_init(incoming);
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) incoming.kit.tracks[t].params[i] = 64;
        incoming.kit.tracks[t].params[kLvl] = 127;
    }
    post_cmd(Cmd::project_loaded);
    post_cmd(Cmd::stop, 0, 0, 1);          // the app's own second STOP press: cut the tails
    pump(4);

    CHECK_EQ(UiProbe::held_step(app), -1);        // the reset above really did clear them
    CHECK_EQ(UiProbe::range_first(app), -1);
    CHECK_EQ(int(undo_depth()), 0);               // project_loaded cleared the history
    for (int s = 0; s < kStepsPerTrack; s++) CHECK(!trig_at(0, s));   // and a blank pattern
    return app;
}

// A save, so that `dirty()` means "since this point". Returns with the project clean.
void save_and_clean() {
    app_storage().request_save();
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(!app_storage().dirty());
}

}  // namespace

// --- the tap, and the hold that is not a tap ------------------------------------------------

TEST(ui_a_tap_toggles_a_trig_and_a_second_tap_clears_it) {
    Strata &app = fresh();

    step_down(4);
    CHECK_EQ(UiProbe::held_step(app), 4);          // held, and nothing posted yet
    CHECK(!UiProbe::held_used(app));
    pump();
    CHECK(!trig_at(0, 4));                         // the press alone does not toggle

    step_up(4);
    pump();
    CHECK(trig_at(0, 4));
    CHECK_EQ(UiProbe::held_step(app), -1);
    CHECK_EQ(UiProbe::held_key(app), -1);
    CHECK_EQ(int(undo_depth()), 1);                // one gesture, one undo level

    key(ENAGA_KEY_STEP1 + 4);
    pump();
    CHECK(!trig_at(0, 4));
    CHECK_EQ(int(undo_depth()), 2);

    // And only that step: a tap is not a range edit.
    for (int s = 0; s < kStepsPerTrack; s++) CHECK(!trig_at(0, s));
}

TEST(ui_a_step_held_with_an_encoder_locks_that_step_and_does_not_toggle_its_trig) {
    // docs/SPEC.md, "Step editing", and the half of it to watch (docs/UNIT_CHECKLIST.md): if a
    // held step toggles its trig on release, every lock edit also flips a trig.
    Strata &app = fresh();
    const int kStep = 2;

    step_down(kStep);
    const int param = encoder_param(0);
    const uint8_t kit_before = app_engine().params(0).v[param];

    app.on_encoder(0, +3);
    CHECK(UiProbe::held_used(app));                // the hold was spent as a modifier
    pump();

    uint8_t locked = 0;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, kStep, uint8_t(param), locked));
    CHECK_EQ(int(locked), int(kit_before) + 3);    // from what you are hearing, not from zero
    CHECK_EQ(int(app_engine().params(0).v[param]), int(kit_before));   // the KIT did not move

    step_up(kStep);
    pump();
    CHECK(!trig_at(0, kStep));                     // the release was swallowed, as it must be
    CHECK_EQ(UiProbe::held_step(app), -1);

    // On THAT step and no other.
    CHECK_EQ(int(step_of(0, kStep).lock_count), 1);
    CHECK_EQ(locks_on_track(0), 1);
}

TEST(ui_an_encoder_with_no_step_held_writes_the_kit_and_not_a_lock) {
    // The other half of the routing: in GRID with nothing held, A-D are the four parameters of
    // the current page on the selected track (case 5 in `on_encoder`).
    Strata &app = fresh();
    CHECK_EQ(UiProbe::held_step(app), -1);

    const int param = encoder_param(1);
    const uint8_t before = app_engine().params(0).v[param];
    app.on_encoder(1, +5);
    pump();

    CHECK_EQ(int(app_engine().params(0).v[param]), int(before) + 5);
    CHECK_EQ(locks_on_track(0), 0);
    CHECK_EQ(int(undo_depth()), 0);                // a kit edit is not a pattern edit
}

TEST(ui_the_parameter_page_decides_which_parameter_an_encoder_writes) {
    // Found by mutation testing: dropping `page_first(Page(param_page_))` from the lock index
    // survived every test above, because none of them ever left the SRC page -- where the page
    // base is zero and the bug is invisible. docs/SPEC.md gives EDIT's selection keys "SRC, FLTR,
    // AMP, FX", and each page is four parameters along (dsp/track.hpp), for the kit and for a
    // lock alike.
    Strata &app = fresh();

    key(ENAGA_KEY_F8);                             // EDIT
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    // To a known page from wherever the test before this one left it: the selection keys cycle.
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);

    app.on_encoder(0, +1);                         // SRC: encoder A is parameter 0
    pump();
    CHECK_EQ(int(app_engine().params(0).v[0]), 65);
    CHECK_EQ(int(app_engine().params(0).v[page_first(Page::fltr)]), 64);

    key(ENAGA_KEY_SELECT_RIGHT);                   // FLTR: four parameters along
    CHECK_EQ(UiProbe::param_page(app), int(Page::fltr));
    app.on_encoder(0, +2);
    pump();
    CHECK_EQ(int(app_engine().params(0).v[page_first(Page::fltr)]), 66);
    CHECK_EQ(int(app_engine().params(0).v[0]), 65);               // and not the SRC page again

    // The pages wrap, so the four of them are reachable in either direction.
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::param_page(app), int(Page::src));
    for (int i = 0; i < kPages; i++) key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::param_page(app), int(Page::src));

    // And a lock follows the same page. The step page is not open and the mode is GRID, but
    // `param_page_` is what the lock index is built from either way.
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::param_page(app), int(Page::amp));
    key(ENAGA_KEY_F4);
    step_down(3);
    app.on_encoder(1, +4);                         // AMP, encoder B
    step_up(3);
    pump();

    uint8_t value = 0;
    const int expected = page_first(Page::amp) + 1;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, 3, uint8_t(expected), value));
    CHECK_EQ(int(value), 68);
    CHECK_EQ(int(step_of(0, 3).lock_count), 1);     // one lock, on the AMP page's second knob
    CHECK(!pattern_get_lock(app_engine().pattern(), 0, 3, 1, value));
}

TEST(ui_a_lower_pad_selects_its_track_and_everything_else_follows) {
    // Also found by mutation testing: `selected_ = track` could be deleted and nothing noticed,
    // because every test above plays pad 1 while track 1 is already selected. docs/SPEC.md:
    // "Tap: select track and play it with velocity", and the selected track is what the STEP
    // keys, the encoders and the screen are all about.
    Strata &app = fresh();
    CHECK_EQ(UiProbe::selected(app), 0);

    app.on_pad_press(4, 100);                      // the lower row is the even ids: pad 4 = track 3
    app.on_pad_release(4);
    CHECK_EQ(UiProbe::selected(app), 2);
    const float peak = pump(4);
    CHECK(peak > 0.0f);                            // and it played, which is the other half

    // The STEP keys now edit track 3.
    key(ENAGA_KEY_STEP1 + 5);
    pump();
    CHECK(trig_at(2, 5));
    CHECK(!trig_at(0, 5));

    // So does an encoder.
    const int param = encoder_param(0);
    app.on_encoder(0, +6);
    pump();
    CHECK_EQ(int(app_engine().params(2).v[param]), 70);
    CHECK_EQ(int(app_engine().params(0).v[param]), 64);

    // An upper pad is not a track at all: the performance row is fixed in every mode.
    app.on_pad_press(5, 100);                      // MUTE mode on
    CHECK(UiProbe::mute_mode(app));
    CHECK_EQ(UiProbe::selected(app), 2);
    app.on_pad_release(5);

    // And in MUTE mode the lower pads mute instead of playing, without changing the selection.
    app.on_pad_press(4, 100);
    app.on_pad_release(4);
    pump(2);
    CHECK(app_engine().muted(2));
    CHECK_EQ(UiProbe::selected(app), 2);
}

TEST(ui_the_step_page_maps_sixteen_keys_onto_sixty_four_steps) {
    // A third mutation-testing gap: dropping `page_ * 16` from `on_step_key` survived everything
    // above, because every test before this one works on page 1 where the offset is zero -- and
    // the offset is the whole of how a 64-step track is reached from sixteen keys.
    Strata &app = fresh();
    CHECK_EQ(UiProbe::page(app), 0);

    key(ENAGA_KEY_OCTAVE_RIGHT);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::page(app), 2);               // "step page 1-4 in GRID" (docs/SPEC.md)

    key(ENAGA_KEY_STEP1);                          // STEP 1 of page 3 is absolute step 33
    pump();
    CHECK(trig_at(0, 32));
    CHECK(!trig_at(0, 0));
    CHECK(!trig_at(0, 16));

    // A hold on this page locks the step the page names, not the key's number.
    step_down(1);
    app.on_encoder(0, +5);
    step_up(1);
    pump();
    const int param = encoder_param(0);
    uint8_t value = 0;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, 33, uint8_t(param), value));
    CHECK_EQ(int(value), 69);
    CHECK_EQ(int(step_of(0, 1).lock_count), 0);
    CHECK(!trig_at(0, 33));                        // the hold was spent on the lock

    // The octave keys CLAMP at both ends: four pages, and walking off either end stays there.
    for (int i = 0; i < 4; i++) key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::page(app), 3);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_OCTAVE_LEFT);
    CHECK_EQ(UiProbe::page(app), 0);

    // The selection keys move the page in GRID as well, and those WRAP -- the spec's key table
    // gives both, and `select()` is the one that goes round.
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::page(app), 3);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::page(app), 0);
}

TEST(ui_the_value_encoder_is_the_tempo_and_a_refused_transport_is_recorded) {
    // CLAUDE.md: "STRATA does not own the tempo. CLOCK SRC is the user's setting; start/stop/
    // set_tempo return false under external sync. Always handle the refusal." tests/test_app.cpp
    // pins that at the clock; this pins what the UI does with it -- it asks, it records the
    // refusal for the screen, and it does not pretend the transport moved.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();

    const int tenths = int(UiProbe::tempo_tenths(app));
    app.on_encoder(4, +10);                        // VALUE, nothing pushed: one BPM
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), tenths + 10);
    CHECK_NEAR(host.tempo(), float(tenths + 10) * 0.1f, 0.01);
    CHECK(!UiProbe::clock_refused(app));

    // The ends of the range, walked in real clicks. NOT with one big negative delta: the clamp
    // is written on the uint32_t, so a delta large enough to take the sum below zero wraps and
    // clamps to the MAXIMUM instead of the minimum. An encoder event carries a click or two, so
    // nothing on the panel can produce one -- but it is a latent sign bug rather than a safe
    // idiom, and a test that fed it -5000 would be pinning a value no hand can reach.
    for (int i = 0; i < 1200; i++) app.on_encoder(4, -1);
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 200);
    CHECK_NEAR(host.tempo(), 20.0f, 0.01);
    for (int i = 0; i < 3000; i++) app.on_encoder(4, +1);
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 3000);
    CHECK_NEAR(host.tempo(), 300.0f, 0.01);

    // VALUE pushed and turned is the pattern's swing instead, as the spec's key table says, and
    // the tempo does not move with it.
    const int swing_before = int(app_engine().pattern().swing);
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    app.on_encoder(4, +2);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
    pump(2);
    CHECK_EQ(int(app_engine().pattern().swing), swing_before + 2);
    CHECK_NEAR(host.tempo(), 300.0f, 0.01);

    // And the refusal itself.
    host.set_source(ENAGA_CLOCK_MIDI);
    const int starts = host.control_calls(ENAGA_CLOCK_START);
    key(ENAGA_KEY_F5);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_START), starts + 1);   // it asked...
    CHECK(!host.running());                                        // ...and was told no
    CHECK(UiProbe::clock_refused(app));

    host.set_source(ENAGA_CLOCK_INTERNAL);
    key(ENAGA_KEY_F5);
    CHECK(host.running());
    CHECK(!UiProbe::clock_refused(app));
    key(ENAGA_KEY_F6);
    CHECK(!host.running());
}

// --- the two ways a hold used to be stranded ------------------------------------------------
//
// docs/UNIT_CHECKLIST.md: "Hold a STEP key, change page or mode, let go, then turn an encoder. No
// parameter lock should appear. The hold was resolved against the CURRENT page on release and
// against the press's page on the way in, and KEYS, PERFORM and MUTE swallow the STEP keys
// entirely -- so a hold could be stranded forever, and the next encoder move wrote a lock onto a
// phantom step."

TEST(ui_a_hold_that_crosses_a_page_change_resolves_to_the_step_it_started_on) {
    Strata &app = fresh();
    CHECK_EQ(UiProbe::page(app), 0);

    step_down(3);                                  // page 1, so absolute step 3
    CHECK_EQ(UiProbe::held_step(app), 3);
    CHECK_EQ(UiProbe::held_key(app), 3);

    key(ENAGA_KEY_OCTAVE_RIGHT);                   // the page moves under the finger
    CHECK_EQ(UiProbe::page(app), 1);
    CHECK_EQ(UiProbe::held_step(app), 3);          // the press's step, not the current page's

    step_up(3);
    pump();

    // The key you let go of is the key you pressed: step 3 toggled, step 19 untouched.
    CHECK(trig_at(0, 3));
    CHECK(!trig_at(0, 19));
    CHECK_EQ(UiProbe::held_step(app), -1);
    CHECK_EQ(UiProbe::held_key(app), -1);
    CHECK(!UiProbe::held_used(app));

    // And the hold is not stranded: the next encoder move is a kit edit, not a lock on a step
    // nobody is touching.
    const int param = encoder_param(0);
    const uint8_t before = app_engine().params(0).v[param];
    app.on_encoder(0, +7);
    pump();
    CHECK_EQ(int(app_engine().params(0).v[param]), int(before) + 7);
    CHECK_EQ(locks_on_track(0), 0);
    CHECK_EQ(int(undo_depth()), 1);                // the toggle only; no lock was recorded
}

TEST(ui_a_hold_that_crosses_a_mode_change_is_dropped_rather_than_stranded) {
    // KEYS and PERFORM take the STEP keys ahead of `on_step_key`, so a key held across the change
    // never delivers its release. The mode change therefore has to drop the hold itself, and the
    // step must NOT toggle: nothing resolved it, so nothing happened to it.
    struct Case {
        const char *name;
        int mode_key;
        bool shift;
        Mode expected;
    };
    const Case cases[] = {
        {"KEYS", ENAGA_KEY_F4, true, Mode::keys},
        {"PERFORM", ENAGA_KEY_F7, true, Mode::perform},
        {"PROJECT", ENAGA_KEY_F3, false, Mode::project},
        {"EDIT", ENAGA_KEY_F8, false, Mode::edit},
        {"MIXER", ENAGA_KEY_F7, false, Mode::mixer},
    };

    for (const Case &c : cases) {
        Strata &app = fresh();
        const int kStep = 5;

        step_down(kStep);
        CHECK_EQ(UiProbe::held_step(app), kStep);

        if (c.shift) shift_key(c.mode_key); else key(c.mode_key);
        CHECK_EQ(UiProbe::mode(app), int(c.expected));
        CHECK_EQ(UiProbe::held_step(app), -1);     // dropped by the mode change
        CHECK_EQ(UiProbe::held_key(app), -1);

        step_up(kStep);                            // swallowed, or matched against nothing
        pump();
        CHECK(!trig_at(0, kStep));                 // either way, no trig toggled

        key(ENAGA_KEY_F4);                         // back to GRID
        const int param = encoder_param(2);
        const uint8_t before = app_engine().params(0).v[param];
        app.on_encoder(2, +2);
        pump();
        CHECK_EQ(int(app_engine().params(0).v[param]), int(before) + 2);
        CHECK_EQ(locks_on_track(0), 0);            // no phantom lock
        CHECK_EQ(int(undo_depth()), 0);            // and no pattern edit at all
    }
}

TEST(ui_a_performance_pad_pressed_during_a_hold_still_strands_it) {
    // The third route the review's wording covers and the fix does not: MUTE mode and the scene
    // pads swallow the STEP keys too, and neither is a MODE -- they are performance-row pads, and
    // `on_perform_pad` does not call `leave_mode()`. So "hold STEP 1, tap the MUTE pad, let go"
    // leaves `held_step_` set with no key down, and the next encoder move writes a lock onto it.
    //
    // This test pins what the code does TODAY rather than what it should do, which is the same
    // choice tests/test_app.cpp makes for the cancelled save: a decision rather than a surprise.
    // The fix is one line in each of the two swallowing branches (drop the hold before returning)
    // and it is not made here, because this change is a move plus its tests.
    {
        Strata &app = fresh();
        step_down(0);
        app.on_pad_press(5, 100);                  // MUTE mode on, with STEP 1 still down
        CHECK(UiProbe::mute_mode(app));
        app.on_pad_release(5);
        step_up(0);                                // swallowed by the MUTE branch

        CHECK_EQ(UiProbe::held_step(app), 0);      // <-- the strand, with no key held
        app.on_encoder(0, +4);
        pump();
        CHECK_EQ(locks_on_track(0), 1);            // <-- the phantom lock
        CHECK_EQ(int(step_of(0, 0).lock_count), 1);
    }
    {
        Strata &app = fresh();
        step_down(2);
        app.on_pad_press(7, 100);                  // scene A held: STEP keys become the scenes
        CHECK_EQ(UiProbe::scene_holding(app), 0);
        step_up(2);                                // swallowed by the scene branch
        app.on_pad_release(7);

        CHECK_EQ(UiProbe::held_step(app), 2);
        app.on_encoder(0, +4);
        pump();
        CHECK_EQ(locks_on_track(0), 1);
    }
}

// --- two steps are a range ------------------------------------------------------------------

TEST(ui_two_held_steps_make_a_range_that_survives_until_a_fresh_single_press) {
    Strata &app = fresh();

    // Eight trigs, so what an erase took is visible.
    for (int s = 0; s < 8; s++) key(ENAGA_KEY_STEP1 + s);
    pump(2);
    for (int s = 0; s < 8; s++) CHECK(trig_at(0, s));

    step_down(2);
    step_down(6);                                  // the second key makes the pair a selection
    CHECK_EQ(UiProbe::range_first(app), 2);
    CHECK_EQ(UiProbe::range_last(app), 6);
    CHECK_EQ(UiProbe::notice(app), "RANGE");
    CHECK(UiProbe::held_used(app));                // so the first key's release is not a tap

    step_up(6);
    step_up(2);
    pump();
    CHECK_EQ(UiProbe::range_first(app), 2);        // the range outlives both keys
    CHECK_EQ(UiProbe::range_last(app), 6);
    CHECK_EQ(UiProbe::held_step(app), -1);
    for (int s = 0; s < 8; s++) CHECK(trig_at(0, s));   // and neither key toggled anything

    // A range is what CLR erases, and only that range.
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "RANGE CLEARED");
    CHECK(trig_at(0, 0));
    CHECK(trig_at(0, 1));
    for (int s = 2; s <= 6; s++) CHECK(!trig_at(0, s));
    CHECK(trig_at(0, 7));

    // Backwards makes the same range: the pair is sorted, not ordered.
    step_down(5);
    step_down(1);
    CHECK_EQ(UiProbe::range_first(app), 1);
    CHECK_EQ(UiProbe::range_last(app), 5);
    step_up(1);
    step_up(5);

    // A fresh single press drops it. Without that, a range made minutes ago is still what the
    // next CLR erases, which is the sort of thing that loses someone's pattern.
    step_down(9);
    CHECK_EQ(UiProbe::range_first(app), -1);
    CHECK_EQ(UiProbe::range_last(app), -1);
    step_up(9);
    pump();

    // So CLR now takes the whole track, which is the gesture the selection was standing in front
    // of.
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "TRACK CLEARED  F1+CLR UNDO");
    for (int s = 0; s < kStepsPerTrack; s++) CHECK(!trig_at(0, s));
}

// --- the mode keys --------------------------------------------------------------------------

TEST(ui_every_mode_key_closes_the_step_page) {
    // "Every one of these ends a STEP hold as well" is the test above; this one is the other half
    // of `leave_mode`, and it matters because the step page is a screen you can forget you are in:
    // while it is open, A-D edit one step's fields instead of the track's sound.
    struct Case {
        int mode_key;
        bool shift;
        Mode expected;
    };
    const Case cases[] = {
        {ENAGA_KEY_F3, false, Mode::project}, {ENAGA_KEY_F4, false, Mode::grid},
        {ENAGA_KEY_F7, false, Mode::mixer},   {ENAGA_KEY_F8, false, Mode::edit},
        {ENAGA_KEY_F4, true, Mode::keys},     {ENAGA_KEY_F7, true, Mode::perform},
    };

    for (const Case &c : cases) {
        Strata &app = fresh();

        // Hold STEP + OK opens the step page on that step (docs/SPEC.md, "Step editing").
        step_down(7);
        key(ENAGA_KEY_OK);
        CHECK(UiProbe::step_page(app));
        CHECK_EQ(UiProbe::page_step(app), 7);
        CHECK_EQ(UiProbe::mode(app), int(Mode::grid));   // it is a GRID screen, whatever was on
        step_up(7);
        pump();
        CHECK(!trig_at(0, 7));                           // OK spent the hold, so no tap

        if (c.shift) shift_key(c.mode_key); else key(c.mode_key);
        CHECK_EQ(UiProbe::mode(app), int(c.expected));
        CHECK(!UiProbe::step_page(app));
    }
}

TEST(ui_f1_picks_keys_over_grid_and_perform_over_mixer) {
    // The two gestures that share a key with a plain mode. Getting the shift test backwards here
    // would make KEYS and PERFORM unreachable, which no other test would notice.
    Strata &app = fresh();

    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));

    key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::mixer));
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));

    key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));
}

// --- KEYS mode ------------------------------------------------------------------------------

TEST(ui_keys_mode_plays_sixteen_degrees_of_the_scale_and_not_semitones) {
    // docs/SPEC.md: "KEYS mode: STEP keys play notes in scale". By DEGREES, which is the whole
    // reason core/scale exists -- sixteen keys then span two octaves of a seven-note scale and
    // stay in the key. Checked against core/scale itself rather than against a table copied out
    // of it, plus the semitones of natural minor written out, so that a change to either side has
    // to be deliberate.
    Strata &app = fresh();

    // The key and the scale, through PROJECT's own encoders B and C (the global key is what every
    // melodic track and every mutation folds into).
    key(ENAGA_KEY_F3);
    app.on_encoder(1, +2);                         // root: C -> D
    app.on_encoder(2, +1);                         // scale: chromatic -> minor
    pump(2);
    CHECK_EQ(int(app_engine().pattern().root), 2);
    CHECK_EQ(int(app_engine().pattern().scale), int(Scale::minor));
    CHECK_EQ(app_engine().pattern().root, app_engine().project().settings.root % 12);

    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));

    const uint8_t root = 2;
    const int scale = int(Scale::minor);
    const uint8_t base = app.keys_note(0);
    CHECK_EQ(int(base % 12), int(root));           // key 1 is the root of the current octave
    CHECK_EQ(scale_size(scale), 7);

    for (int i = 0; i < 16; i++) {
        CHECK_EQ(int(app.keys_note(i)), int(scale_step(base, root, scale, i)));
    }

    // Natural minor is 0 2 3 5 7 8 10, so sixteen degrees are these semitones and not 0..15.
    const int semitones[16] = {0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19, 20, 22, 24, 26};
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(int(app.keys_note(i)) - int(base), semitones[i]);
    }
    CHECK_EQ(int(app.keys_note(7)) - int(base), 12);      // seven degrees is an octave
    CHECK(int(app.keys_note(15)) - int(base) != 15);      // and sixteen keys are not sixteen frets

    // The octave keys move the whole lot by twelve, and they are the octave in KEYS rather than
    // the step page (docs/SPEC.md's key table).
    const int octave_before = UiProbe::keys_octave(app);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::keys_octave(app), octave_before + 1);
    CHECK_EQ(UiProbe::page(app), 0);                      // not the step page
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(int(app.keys_note(i)), int(scale_step(base, root, scale, i)) + 12);
    }
    key(ENAGA_KEY_OCTAVE_LEFT);
    CHECK_EQ(UiProbe::keys_octave(app), octave_before);

    // Every scale this build has, through the same encoder: the note a key plays is always in the
    // scale, which is the property that makes the mode playable at all.
    key(ENAGA_KEY_F3);
    for (int s = int(Scale::minor) + 1; s < kScaleCount; s++) {
        app.on_encoder(2, +1);
        pump();
        CHECK_EQ(int(app_engine().pattern().scale), s);
        shift_key(ENAGA_KEY_F4);
        for (int i = 0; i < 16; i++) CHECK(scale_contains(app.keys_note(i), root, s));
        key(ENAGA_KEY_F3);
    }
}

TEST(ui_a_step_key_in_keys_mode_plays_and_records_the_scale_note) {
    // The wiring, rather than the arithmetic: the note `keys_note` worked out is the note that
    // reaches the engine. A recorded hit is the one place a played note becomes something a test
    // can read back, which is why this is recorded rather than listened to.
    Strata &app = fresh();

    key(ENAGA_KEY_F3);
    app.on_encoder(2, +1);                         // minor, so a degree is not a semitone
    pump();
    shift_key(ENAGA_KEY_F4);

    FakeHost &host = FakeHost::the();
    host.set_running(true);
    host.set_position(96);                         // a bar in, so the pad-latency correction has
                                                   // somewhere to go
    key(ENAGA_KEY_F2);                             // REC on
    CHECK(UiProbe::rec(app));

    const uint8_t expected = app.keys_note(4);
    step_down(4);                                  // KEYS takes the press and ignores the release
    step_up(4);
    pump(2);

    int trigs = 0;
    int where = -1;
    for (int s = 0; s < kStepsPerTrack; s++) {
        if (trig_at(0, s)) { trigs++; where = s; }
    }
    CHECK_EQ(trigs, 1);                            // one key, one step
    CHECK(where >= 0);
    CHECK_EQ(int(step_of(0, where).note), int(expected));
    CHECK_EQ(int(step_of(0, where).velocity), 100);     // a key has no velocity of its own

    key(ENAGA_KEY_F2);
    host.set_running(false);
}

// --- PLAY while the card is busy ------------------------------------------------------------

TEST(ui_play_is_refused_while_a_project_is_loading) {
    // docs/UNIT_CHECKLIST.md: "Press PLAY while a project is loading. It should refuse and say
    // LOADING, not start the transport over pattern memory that `step_load` is halfway through
    // rewriting." `step_load` reads one file per pass, so a load is in flight for ten passes --
    // which is the window this test sits in, and which the emulator cannot produce on purpose.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();

    key(ENAGA_KEY_F3);                             // PROJECT
    key(ENAGA_KEY_OK);                             // save, so there is something to read back
    CHECK_EQ(UiProbe::notice(app), "SAVING");
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);

    shift_key(ENAGA_KEY_OK);                       // F1 + OK: read the card again
    CHECK_EQ(UiProbe::notice(app), "RELOADING");
    pump();                                        // one pass of service(): the load starts
    CHECK(app_storage().loading_project());

    const int starts_before = host.control_calls(ENAGA_CLOCK_START);
    key(ENAGA_KEY_F5);                             // PLAY
    CHECK_EQ(UiProbe::notice(app), "LOADING");
    CHECK(UiProbe::notice_showing(app));
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_START), starts_before);   // it did not even ask
    CHECK(!host.running());
    CHECK(app_storage().loading_project());        // and the load was not disturbed

    // Once the load is done the same key works, so this is about the load and not about PLAY.
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(!app_storage().loading_project());
    key(ENAGA_KEY_F5);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_START), starts_before + 1);
    CHECK(host.running());

    key(ENAGA_KEY_F6);                             // and stop again, for the test after this one
    CHECK(!host.running());
}

// --- the unsaved-changes mark ---------------------------------------------------------------

TEST(ui_the_five_wide_value_gestures_that_change_a_file_mark_the_project_dirty) {
    // docs/UNIT_CHECKLIST.md: six gestures built their command by hand and so skipped
    // `note_command` completely -- "a mutate, a live-recorded hit, a queued pattern, a macro
    // assignment, a morph move and a mixer level each changed a file while the screen went on
    // claiming the project was saved". Every post now goes through `post_command`, which is the
    // one place the dirty flags are fed.
    //
    // FIVE of the six, not six: `Cmd::set_morph` is deliberately in none of app/storage.cpp's
    // `mutates_*` lists, because "the morph POSITION and a macro's position do not [save] -- they
    // are where the performer's hands are, not part of the project". The test below this one pins
    // that, and the checklist line is wrong about the sixth rather than the code being wrong.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();

    // 1. MUTATE, the upper pad. One undo level, then the mutation (it writes the pattern).
    save_and_clean();
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    CHECK(app_storage().dirty());

    // 2. A live-recorded hit: a pad played with REC on and the transport running.
    save_and_clean();
    key(ENAGA_KEY_F4);
    key(ENAGA_KEY_F2);
    CHECK(UiProbe::rec(app));
    host.set_running(true);
    host.set_position(96);
    app.on_pad_press(0, 100);
    app.on_pad_release(0);
    CHECK(app_storage().dirty());
    key(ENAGA_KEY_F2);
    host.set_running(false);

    // 3. A queued pattern change: PROJECT mode, a STEP key.
    save_and_clean();
    key(ENAGA_KEY_F3);
    step_down(1);
    step_up(1);
    CHECK(app_storage().dirty());

    // 4. A macro assignment: PERFORM mode, a STEP key picks the parameter.
    save_and_clean();
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    step_down(0);
    step_up(0);
    CHECK(app_storage().dirty());

    // 5. A mixer level, which is a float and so cannot travel through `post`'s byte arguments.
    save_and_clean();
    key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::mixer));
    const float level_before = app_engine().level(UiProbe::mixer_bank(app) * 4);
    app.on_encoder(0, +3);
    pump(2);
    CHECK(app_storage().dirty());
    CHECK(app_engine().level(UiProbe::mixer_bank(app) * 4) > level_before);
}

TEST(ui_a_morph_move_is_heard_but_does_not_make_the_project_dirty) {
    // The sixth gesture, and the one that is not a file change: the morph is where the performer's
    // hands are. It still has to go through `post_command` -- that is what the one-function rule is
    // for -- and `note_command` is what decides it changes no file. So the assertion is that the
    // morph MOVED and the project stayed clean, which is the pair that says the post happened and
    // was classified rather than skipped.
    Strata &app = fresh();

    shift_key(ENAGA_KEY_F7);                       // PERFORM
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    save_and_clean();

    CHECK_EQ(int(app_engine().morph()), 0);
    app.on_encoder(4, +8);                         // VALUE is the morph in this mode
    pump(2);
    CHECK(app_engine().morph() > 0);
    CHECK(!app_storage().dirty());

    // And a macro position, for the same reason: a knob under a hand is not an edit.
    app.on_encoder(0, +10);
    pump(2);
    CHECK(!app_storage().dirty());
    CHECK(app_engine().macro_position(0) > 0);
}

// --- the screen -----------------------------------------------------------------------------

TEST(ui_the_grid_draws_a_trig_where_the_trig_is) {
    // `FakeHost::t_text` draws nothing, so text cannot be read back -- but `Canvas::rect` writes
    // real pixels, and the grid is rects. This pins the mapping docs/SPEC.md gives: "an 8 x 16
    // mini-grid of all tracks (8 px per step), the selected track highlighted", a filled cell for
    // a trig and a thin dash for an empty step.
    Strata &app = fresh();
    enaga::Canvas canvas(g_pixels);

    const int top = 20;                            // track 1's row
    const uint8_t *px = g_pixels;
    const auto at = [px](int x, int y) { return int(px[y * enaga::Canvas::width + x]); };

    app.draw(canvas);
    CHECK_EQ(at(1, top + 3), 2);                   // an empty step: the dash, two rows high
    CHECK_EQ(at(1, top + 1), 0);                   // and nothing above it
    CHECK_EQ(at(0, top), 12);                      // the selected track's marker, at x = 0

    key(ENAGA_KEY_STEP1);                          // a trig on step 1 of track 1
    pump(2);
    CHECK(trig_at(0, 0));
    app.draw(canvas);

    CHECK_EQ(at(1, top + 1), 11);                  // the cell is filled now, 6 x 6 from (1, 21)
    CHECK_EQ(at(6, top + 6), 11);
    CHECK_EQ(at(7, top + 1), 0);                   // and it stops before the next step's cell
    CHECK_EQ(at(9, top + 3), 2);                   // step 2 is still a dash
    CHECK_EQ(at(9, top + 1), 0);

    // Track 2's row is eight pixels lower and has no trig, so the mapping is per track as well.
    CHECK_EQ(at(1, top + 8 + 3), 2);
    CHECK_EQ(at(1, top + 8 + 1), 0);
}
