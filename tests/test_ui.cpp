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

    static int pressure_pad(const Strata &app) { return app.pressure_pad_; }
    static int take_bars_index(const Strata &app) { return app.take_bars_index_; }
    static int resample_track(const Strata &app) { return app.resample_track_; }
    static uint8_t macro_knob(const Strata &app, int macro) {
        return app.macro_position_[macro];
    }

    static uint32_t tempo_tenths(const Strata &app) { return app.tempo_tenths_; }
    static bool clock_refused(const Strata &app) { return app.clock_refused_; }
    static bool project_setup(const Strata &app) { return app.project_setup_; }
    static int slot_cursor(const Strata &app) { return app.slot_cursor_; }
    static int macro_selected(const Strata &app) { return app.macro_selected_; }
    static int quantize(const Strata &app) { return app.quantize(); }
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

TEST(ui_a_step_held_with_a_knob_pushed_clears_that_lock_and_only_that_one) {
    // Hold STEP + push A-D takes that parameter's lock off the step: the undo of hold STEP +
    // turn, on the same knob. Before it, Cmd::clear_lock had a handler and no gesture
    // (tools/audit_commands.sh), so a lock could only be removed by erasing the whole step.
    Strata &app = fresh();
    const int kStep = 5;
    const int param_a = encoder_param(0);
    const int param_b = encoder_param(1);

    step_down(kStep);
    app.on_encoder(0, +4);
    app.on_encoder(1, +6);
    pump();
    step_up(kStep);
    pump();
    CHECK_EQ(int(step_of(0, kStep).lock_count), 2);
    const int depth = int(undo_depth());

    // B, which the ABI numbers 0 -- the mapping is by name, and this is what would catch it.
    step_down(kStep);
    key(ENAGA_KEY_PUSH_B);
    CHECK(UiProbe::held_used(app));
    pump();
    step_up(kStep);
    pump();

    uint8_t value = 0;
    CHECK(!pattern_get_lock(app_engine().pattern(), 0, kStep, uint8_t(param_b), value));
    CHECK(pattern_get_lock(app_engine().pattern(), 0, kStep, uint8_t(param_a), value));
    CHECK_EQ(int(step_of(0, kStep).lock_count), 1);
    CHECK(!trig_at(0, kStep));                     // the release was swallowed, not a toggle
    CHECK_EQ(int(undo_depth()), depth + 1);        // its own undo level

    // F1 + CLR brings it back.
    shift_key(ENAGA_KEY_CLR);
    pump();
    CHECK(pattern_get_lock(app_engine().pattern(), 0, kStep, uint8_t(param_b), value));
    CHECK_EQ(int(step_of(0, kStep).lock_count), 2);

    // A push with no step held clears nothing.
    key(ENAGA_KEY_PUSH_A);
    pump();
    CHECK_EQ(int(step_of(0, kStep).lock_count), 2);
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

TEST(ui_every_click_in_a_block_counts_on_the_knobs_the_swing_and_the_length) {
    // These four computed "engine value + delta" on the UI side and posted the result. The
    // engine applies commands once a block, so three clicks before the next block all read the
    // same value and the knob moved one click. They now post deltas the audio side applies.
    Strata &app = fresh();

    const int param = encoder_param(1);
    const int before = int(app_engine().params(0).v[param]);
    app.on_encoder(1, +1);
    app.on_encoder(1, +1);
    app.on_encoder(1, +1);
    pump();
    CHECK_EQ(int(app_engine().params(0).v[param]), before + 3);

    const int length = int(app_engine().pattern().tracks[0].length);
    app.on_key(ENAGA_KEY_F1, true);
    for (int i = 0; i < 3; i++) {
        app.on_key(ENAGA_KEY_OCTAVE_LEFT, true);
        app.on_key(ENAGA_KEY_OCTAVE_LEFT, false);
    }
    app.on_key(ENAGA_KEY_F1, false);
    pump();
    CHECK_EQ(int(app_engine().pattern().tracks[0].length), length - 3);

    const int swing = int(app_engine().pattern().swing);
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    app.on_encoder(4, +1);
    app.on_encoder(4, +1);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
    pump();
    CHECK_EQ(int(app_engine().pattern().swing), swing + 2);
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
    //
    // Pumped as it goes, because a tempo click now also POSTS `Cmd::set_tempo` -- the project is
    // where the tempo is saved, and the engine re-derives the tempo-synced delay from it. 4,200
    // clicks with no audio block in between would overflow the command ring and the drops would
    // land on whatever the next test posted. On the unit a block runs every 1.33 ms, so a hand
    // cannot get more than one or two clicks ahead of the drain.
    for (int i = 0; i < 1200; i++) {
        app.on_encoder(4, -1);
        if ((i & 63) == 63) pump();
    }
    pump();
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 200);
    CHECK_NEAR(host.tempo(), 20.0f, 0.01);
    for (int i = 0; i < 3000; i++) {
        app.on_encoder(4, +1);
        if ((i & 63) == 63) pump();
    }
    pump();
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

TEST(ui_stop_under_a_running_outside_clock_resets_nothing_until_the_clock_stops) {
    // F6 under MIDI or SYNC is refused by the clock, which keeps running. It used to post
    // `Cmd::stop` anyway, so every voice was cut and the playheads went back to step 1 while the
    // outside transport carried on. And an outside STOP, delivered through on_transport, posted
    // nothing, so nothing was released or reset when the master really stopped.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    host.set_source(ENAGA_CLOCK_MIDI);
    host.set_running(true);
    for (uint32_t t = 0; t <= 24 * 5; t += 24) {
        host.deliver_tick(t, 0);
        pump();
    }
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 5);

    key(ENAGA_KEY_F6);
    pump(2);
    CHECK(UiProbe::clock_refused(app));
    CHECK(host.running());
    CHECK_EQ(UiProbe::notice(app), "EXT CLOCK");
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 5);

    // The master stops: STRATA follows it.
    host.set_running(false);
    app.on_transport(ENAGA_TRANSPORT_STOP, 24 * 5);
    pump(2);
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 0);

    host.set_source(ENAGA_CLOCK_INTERNAL);
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

TEST(ui_a_performance_pad_pressed_during_a_hold_drops_the_hold) {
    // The third of three ways a STEP hold could outlive its key, and the one a mode change does
    // not cover: MUTE mode and the scene pads swallow the STEP keys too, and neither is a MODE --
    // they are performance-row pads, and the row deliberately works in every mode, so
    // `on_perform_pad` does not and should not call `leave_mode()`. It calls `drop_hold()`.
    //
    // Both branches in `on_key` return on the RELEASE as well as the press, so without this the
    // release never reached `on_step_key`: "hold STEP 1, tap the MUTE pad, let go" left
    // `held_step_` set with no key down, and the next encoder turn wrote a parameter lock onto a
    // step nobody was touching. The other two routes were a page change and a mode change.
    {
        Strata &app = fresh();
        step_down(0);
        app.on_pad_press(5, 100);                  // MUTE mode on, with STEP 1 still down
        CHECK(UiProbe::mute_mode(app));
        CHECK_EQ(UiProbe::held_step(app), -1);     // the hold went with it
        app.on_pad_release(5);
        step_up(0);                                // swallowed by the MUTE branch, harmlessly

        CHECK_EQ(UiProbe::held_step(app), -1);
        app.on_encoder(0, +4);
        pump();
        CHECK_EQ(locks_on_track(0), 0);            // no phantom lock
        CHECK_EQ(int(step_of(0, 0).lock_count), 0);
    }
    {
        Strata &app = fresh();
        step_down(2);
        app.on_pad_press(7, 100);                  // scene A held: STEP keys become the scenes
        CHECK_EQ(UiProbe::scene_holding(app), 0);
        CHECK_EQ(UiProbe::held_step(app), -1);
        step_up(2);
        app.on_pad_release(7);

        CHECK_EQ(UiProbe::held_step(app), -1);
        app.on_encoder(0, +4);
        pump();
        CHECK_EQ(locks_on_track(0), 0);
    }

    // And the pad still does its job: dropping the hold must not have swallowed the gesture it
    // was pressed for.
    {
        Strata &app = fresh();
        app.on_pad_press(5, 100);
        CHECK(UiProbe::mute_mode(app));
        app.on_pad_release(5);
        app.on_pad_press(5, 100);
        CHECK(!UiProbe::mute_mode(app));
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
    CHECK_EQ(UiProbe::notice(app), "CLEARED  F1+CLR UNDO");
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

    shift_key(ENAGA_KEY_F3);                       // F1 + F3 twice: save, so there is
    shift_key(ENAGA_KEY_F3);                       // something to read back
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

// --- pad pressure ---------------------------------------------------------------------------
//
// docs/SPEC.md: "Pad pressure modulates the selected track's macro 1 while a pad is held", using
// Enaga's `on_pad_pressure` event, which is already 0-127 through its own cube curve. The spec's
// `[rev]` note is explicit that the app-side smoothing this document originally planned over raw
// readings is no longer needed, so there is none -- adding any would be undoing Enaga's shaping
// with a second guess at it.
//
// None of this can be FELT here. Whether a cube curve under a finger reads as expressive is a
// question for the unit, and docs/UNIT_CHECKLIST.md carries it. What these pin is the part that
// can be wrong without anyone noticing on hardware: which pad owns the macro, that letting go
// puts the knob's value back rather than leaving it where the finger lifted, and that a pad held
// steady does not flood the command ring.

// Wind macro 1's knob to the bottom, then up to `value`, and come back to GRID.
//
// `fresh()` resets the fake host and the project; it does NOT reset the App, which is one shared
// static whose UI state -- the selected mode, the knob positions, the page -- survives from test
// to test. That is deliberate (there is no App::reset and inventing one for tests would be a
// behaviour-free function in production code), and it means a test about pressure cannot assume
// macro 1's knob is at zero: the test before it may have left it at 127, and pressure only ever
// pushes the macro UP from the knob.
void set_macro_knob(Strata &app, int value) {
    const int mode_before = UiProbe::mode(app);
    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_F7, true);
    app.on_key(ENAGA_KEY_F7, false);
    app.on_key(ENAGA_KEY_F1, false);
    // Pumped as it winds, not once at the end. Each click posts a command and the audio side
    // takes a bounded number per block (Commands::kMaxPerBlock), so 135 clicks into one block
    // overruns the ring and the last value never arrives -- which is a real property of the two
    // contexts and not a quirk of the test: a fast knob sweep is paced by the audio side.
    for (int i = 0; i < 80; i++) {
        app.on_encoder(0, -1);                             // the encoder moves by 2 a click
        if (i % 8 == 7) pump();
    }
    for (int i = 0; i < value / 2; i++) {
        app.on_encoder(0, +1);
        if (i % 8 == 7) pump();
    }
    pump(2);
    if (mode_before != int(Mode::perform)) {
        app.on_key(ENAGA_KEY_F4, true);
        app.on_key(ENAGA_KEY_F4, false);
    }
}

TEST(ui_pad_pressure_bends_macro_one_and_lets_it_go) {
    Strata &app = fresh();
    set_macro_knob(app, 0);

    // A macro has to be ASSIGNED to do anything audible, but what is tested here is the value
    // the UI sends, which is observable whether or not a target exists.
    app.on_pad_press(0, 100);
    pump();
    const int track = UiProbe::selected(app);
    CHECK_EQ(track, 0);
    CHECK_EQ(UiProbe::pressure_pad(app), -1);          // a press alone is not a bend

    app.on_pad_pressure(0, 90);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), 0);
    CHECK_EQ(int(app_engine().macro_position(0)), 90);

    app.on_pad_pressure(0, 120);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 120);

    // Let go: back to where the ENCODER left it, not to zero and not to where the finger lifted.
    app.on_pad_release(0);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), -1);
    CHECK_EQ(int(app_engine().macro_position(0)), int(UiProbe::macro_knob(app, 0)));
}

TEST(ui_pad_pressure_only_pushes_the_macro_up_from_the_knob) {
    // A pad that bent the macro DOWN would mean resting a finger on a pad changed the sound away
    // from what the knob said. A performer leaning on a pad expects more of something.
    Strata &app = fresh();

    // Put the knob at 110 through PERFORM's encoder A, which is what owns macro 1.
    set_macro_knob(app, 110);
    const uint8_t knob = UiProbe::macro_knob(app, 0);
    CHECK(knob > 100);
    CHECK_EQ(int(app_engine().macro_position(0)), int(knob));

    // The pads play in GRID, so lean on one gently.
    app.on_pad_press(0, 100);
    app.on_pad_pressure(0, 10);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), int(knob));    // not pulled down to 10

    app.on_pad_pressure(0, 127);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 127);          // and pushed up when it exceeds the knob
    app.on_pad_release(0);
}

TEST(ui_only_one_pad_owns_the_macro_and_only_the_selected_track) {
    Strata &app = fresh();
    set_macro_knob(app, 0);

    app.on_pad_press(0, 100);
    app.on_pad_pressure(0, 80);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), 0);
    CHECK_EQ(int(app_engine().macro_position(0)), 80);

    // A second pad held does not take the macro from the first: two streams of 262 events a
    // second fighting over one value is not a feature.
    app.on_pad_pressure(1, 127);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), 0);
    CHECK_EQ(int(app_engine().macro_position(0)), 80);

    // Releasing the second changes nothing; releasing the owner hands it back.
    app.on_pad_release(1);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), 0);
    app.on_pad_release(0);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), -1);

    // And a pad on a track that is not selected does nothing: "the SELECTED track's macro 1".
    app.on_pad_press(0, 100);                            // selects track 0
    pump();
    app.on_pad_pressure(4, 127);                         // a different track's pad
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), -1);
    app.on_pad_release(0);
}

TEST(ui_a_pad_held_steady_does_not_flood_the_command_ring) {
    // The event arrives about 262 times a second per pad (docs/SPEC.md). A pad held still sends
    // the same number over and over, and the audio side drains a bounded number of commands per
    // block -- so repeating it would push every other edit back behind a stream of identical
    // macro writes.
    Strata &app = fresh();
    set_macro_knob(app, 0);
    app.on_pad_press(0, 100);
    app.on_pad_pressure(0, 70);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 70);

    // Fifty identical events, then one that differs.
    for (int i = 0; i < 50; i++) app.on_pad_pressure(0, 70);
    CHECK(app_engine().commands().empty());              // nothing was posted
    app.on_pad_pressure(0, 71);
    CHECK(!app_engine().commands().empty());             // a change is
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 71);
    app.on_pad_release(0);
}

TEST(ui_the_performance_row_is_not_pressure_sensitive) {
    // The seven upper pads are tools, not keys: fill, repeat, throw, the scene selectors. A
    // pressure event on one must not bend a macro, or leaning on FILL would change the sound.
    Strata &app = fresh();
    app.on_pad_press(5, 100);                            // MUTE, a performance pad
    app.on_pad_pressure(5, 127);
    pump();
    CHECK_EQ(UiProbe::pressure_pad(app), -1);
    app.on_pad_release(5);
}

TEST(ui_pad_pressure_clamps_a_value_outside_the_events_range) {
    // <enaga/app.hpp> documents the event as 0-127 and Enaga shapes it with its own cube curve,
    // so a value outside that should not arrive. The clamp is defensive, and `Cmd::set_macro`
    // carries the value in a uint8_t -- so without it a pressure of 200 would reach the macro as
    // 200 and 300 would arrive as 44, which is the wrong end of the knob.
    Strata &app = fresh();
    set_macro_knob(app, 0);
    app.on_pad_press(0, 100);

    app.on_pad_pressure(0, 200);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 127);

    app.on_pad_pressure(0, 300);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 127);   // not 44

    app.on_pad_pressure(0, -5);
    pump();
    CHECK_EQ(int(app_engine().macro_position(0)), 0);     // the knob's position, not a wrap
    app.on_pad_release(0);
}

// --- the resample gesture -------------------------------------------------------------------
//
// docs/SPEC.md's key table gives F1 + F2 to "Resample setup". Everything behind it was built and
// tested in tests/test_app.cpp; these are about it being REACHABLE, which is the step this
// project has forgotten before -- a feature with no gesture is a feature nobody has.

TEST(ui_f1_f2_opens_resample_setup_and_ok_arms_a_take) {
    Strata &app = fresh();
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));

    // F2 on its own is still REC, which is the other half of the key.
    const bool rec_before = UiProbe::rec(app);
    key(ENAGA_KEY_F2);
    CHECK(UiProbe::rec(app) != rec_before);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
    key(ENAGA_KEY_F2);                                   // back off

    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_F2, true);
    app.on_key(ENAGA_KEY_F2, false);
    app.on_key(ENAGA_KEY_F1, false);
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));
    CHECK_EQ(UiProbe::rec(app) != rec_before, false);    // and it did not toggle REC as well

    // The two settings: encoder A walks the four lengths SPEC allows, the selection keys the
    // destination.
    CHECK_EQ(UiProbe::take_bars_index(app), 0);          // 1 bar
    app.on_encoder(0, +1);
    CHECK_EQ(UiProbe::take_bars_index(app), 1);          // 2 bars
    app.on_encoder(0, +2);
    CHECK_EQ(UiProbe::take_bars_index(app), 3);          // 8 bars
    app.on_encoder(0, +1);
    CHECK_EQ(UiProbe::take_bars_index(app), 0);          // and round again: only four choices

    CHECK_EQ(UiProbe::resample_track(app), -1);          // the file only, by default
    app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
    app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
    CHECK_EQ(UiProbe::resample_track(app), 0);
    app.on_key(ENAGA_KEY_SELECT_LEFT, true);
    app.on_key(ENAGA_KEY_SELECT_LEFT, false);
    CHECK_EQ(UiProbe::resample_track(app), -1);

    // OK arms it, and the engine agrees.
    key(ENAGA_KEY_OK);
    pump();
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 1);
    CHECK_EQ(UiProbe::notice(app), "ARMED: PRESS PLAY");  // the clock is not running

    // CLR cancels it, and does NOT erase a step on the way past.
    key(ENAGA_KEY_CLR);
    pump();
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 0);
    CHECK_EQ(UiProbe::notice(app), "CANCELLED");
}

TEST(ui_the_resample_gesture_records_and_writes_through_the_panel_alone) {
    // The whole feature from the keys, with nothing posted by hand: this is what a person can
    // actually do with the box.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();

    // Something to record, and a transport.
    key(ENAGA_KEY_STEP1);
    pump(2);
    host.set_running(true);

    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_F2, true);
    app.on_key(ENAGA_KEY_F2, false);
    app.on_key(ENAGA_KEY_F1, false);
    key(ENAGA_KEY_OK);                                   // one bar, to the file
    pump();
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 1);

    host.deliver_tick(384, 0);                           // a bar line starts it
    bool written = false;
    for (int i = 0; i < 4000 && !written; i++) {
        if (i % 8 == 0) host.deliver_tick(uint32_t(384 + i), 0);
        pump();
        written = host.has_file("/RECORD/STRATA_1.WAV");
    }
    CHECK(written);
    CHECK(app_storage().last_error() == nullptr);
    CHECK_EQ(app_storage().last_take_name(), "STRATA_1.WAV");   // and the screen can say so
}

// --- machine switching ----------------------------------------------------------------------
//
// docs/SPEC.md's key table gives F1 + F8 to "Kit and machine select", and docs/PLAN.md deferred
// the whole thing because "a track switching to VA POLY would want six voices it does not
// currently own". That turned out to be a memory question with a measured answer: every machine
// of every track is 72.6 KiB, so every track has one of each and switching is a pointer swap.
//
// The thing this finally makes true is that GRAIN is reachable at all. It has been in the tree,
// host-tested and mutation-tested, with no way to hear it -- the default kit fills all eight
// tracks and nothing could free a slot.

TEST(ui_f1_f8_switches_a_tracks_machine_and_grain_is_reachable_at_last) {
    Strata &app = fresh();

    // F8 alone is still EDIT, which is the other half of the key.
    key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));

    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_F8, true);
    app.on_key(ENAGA_KEY_F8, false);
    app.on_key(ENAGA_KEY_F1, false);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));

    // The default kit, by id: KICK SNARE HAT HAT PERC MNO PLY SMP.
    const MachineId expected[kTracks] = {
        MachineId::kick,  MachineId::snare,   MachineId::hat,     MachineId::hat,
        MachineId::perc,  MachineId::va_mono, MachineId::va_poly, MachineId::sample,
    };
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(app_engine().machine_id(t)), int(expected[t]));
    }

    // Walk track 1 to GRAIN. The encoder posts on every click, so each one is a real change.
    const int steps = int(MachineId::grain) - int(MachineId::kick);
    for (int i = 0; i < steps; i++) {
        app.on_encoder(0, +1);
        pump();
    }
    CHECK_EQ(int(app_engine().machine_id(0)), int(MachineId::grain));
    CHECK_EQ(machine_name(app_engine().machine_id(0)), "GRN");

    // And it is the machine the engine renders: a trig on that track makes a GRAIN sound.
    key(ENAGA_KEY_F4);                               // back to GRID so a STEP key edits
    key(ENAGA_KEY_STEP1);
    pump(2);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(40) > 0.0f);

    // The kit holds it, so it survives a save: KitTrack::machine has been on disk since M4 and
    // this is the first thing to honour it.
    CHECK_EQ(int(app_engine().project().kit.tracks[0].machine), int(MachineId::grain));
    CHECK(app_storage().dirty());
}

TEST(ui_switching_a_machine_silences_the_one_it_leaves) {
    // The machine being left will never be rendered again, so it cannot release its own tail --
    // a voice left sounding in an object nothing calls is a note that never ends and never shows
    // up in the voice count either.
    Strata &app = fresh();
    key(ENAGA_KEY_STEP1);
    pump(2);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(2) > 0.01f);                          // the kick is ringing

    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_F8, true);
    app.on_key(ENAGA_KEY_F8, false);
    app.on_key(ENAGA_KEY_F1, false);
    app.on_encoder(0, +1);                           // KICK -> SNARE, mid-note
    pump();

    // Nothing of the kick is left. The master reverb still rings, so what is checked is that the
    // machine itself is idle rather than that the output is silent.
    CHECK(app_engine().machine(0) != nullptr);
    CHECK(!app_engine().machine(0)->active());
    CHECK_EQ(int(app_engine().machine_id(0)), int(MachineId::snare));
}

TEST(ui_a_machine_id_this_build_does_not_have_is_refused) {
    // The ids are on disk, so a kit from a later build can name a machine this one lacks.
    // core/project.hpp's rule is that it plays NOTHING rather than being reinterpreted as an
    // index -- a silent track is recoverable and a wrong pointer is not.
    Strata &app = fresh();
    const uint32_t refused = app_engine().status().refused.load(std::memory_order_relaxed);
    post_cmd(Cmd::set_machine, uint8_t(MachineId::count));
    pump();
    CHECK(app_engine().status().refused.load(std::memory_order_relaxed) > refused);
    CHECK_EQ(int(app_engine().machine_id(0)), int(MachineId::kick));   // unchanged
    (void)app;
}

// MIXER mode, on a named page. Walked rather than set, because `mixer_bank_` is UI state that
// `fresh()` does not reset -- it is where the user left the mode, which is the right behaviour
// and means a test pressing the selection key a fixed number of times lands wherever the test
// before it had got to. That cost half an hour: the routing page's test was editing the master.
void mixer_page(Strata &app, int page) {
    app.on_key(ENAGA_KEY_F7, true);
    app.on_key(ENAGA_KEY_F7, false);
    for (int guard = 0; guard < 12 && UiProbe::mixer_bank(app) != page; guard++) {
        app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
        app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
    }
    CHECK_EQ(UiProbe::mixer_bank(app), page);
}

// PROJECT mode's SETUP page, on the octave keys.
void setup_page(Strata &app, bool on) {
    app.on_key(ENAGA_KEY_F3, true);
    app.on_key(ENAGA_KEY_F3, false);
    app.on_key(on ? ENAGA_KEY_OCTAVE_RIGHT : ENAGA_KEY_OCTAVE_LEFT, true);
    app.on_key(on ? ENAGA_KEY_OCTAVE_RIGHT : ENAGA_KEY_OCTAVE_LEFT, false);
    CHECK_EQ(UiProbe::project_setup(app), on);
}

// --- the sends, the master and the SETUP page -------------------------------------------------
//
// FIXED. `Cmd::set_sends` and `Cmd::set_master` had no handler AND no caller: nothing in `app/`
// posted either, so every delay and reverb setting and docs/SPEC.md's "Master DJ filter" was
// frozen at a compile-time default for the life of the app. These pin the gestures that reach
// them.

TEST(ui_the_mixer_pages_walk_the_faders_the_sends_and_the_master) {
    Strata &app = fresh();

    mixer_page(app, 0);
    CHECK_EQ(UiProbe::mode(app), int(Mode::mixer));

    // The selection keys page through: two pages of faders, then DLY, REV, ROUTE and MST.
    for (int i = 1; i < 6; i++) {
        app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
        app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
        CHECK_EQ(UiProbe::mixer_bank(app), i);
    }
    // And it wraps rather than sticking.
    app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
    app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
    CHECK_EQ(UiProbe::mixer_bank(app), 0);
    app.on_key(ENAGA_KEY_SELECT_LEFT, true);
    app.on_key(ENAGA_KEY_SELECT_LEFT, false);
    CHECK_EQ(UiProbe::mixer_bank(app), 5);
}

TEST(ui_a_knob_on_the_delay_page_changes_the_delay) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    mixer_page(app, 2);                                // DLY

    // B is the feedback. Two counts per click, so ten clicks is twenty counts.
    const int before = int(engine.project().settings.sends[int(SendField::delay_feedback)]);
    app.on_encoder(1, +10);
    pump(2);
    CHECK_EQ(int(engine.project().settings.sends[int(SendField::delay_feedback)]), before + 20);
    CHECK_NEAR(engine.sends().delay_feedback, float(before + 20) / 127.0f * 0.95f, 1e-6);

    // And the edit dirtied the PROJECT, which is the file that now holds it.
    CHECK(app_storage().dirty());

    // A is the delay's TIME, and one click is one DIVISION rather than one count: the file
    // stores a division because docs/SPEC.md makes send A tempo-synced.
    const int division = delay_division_index(engine.project().settings.sends[0]);
    app.on_encoder(0, +1);
    pump(2);
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), division + 1);

    // The top and the bottom of the division list HOLD rather than wrapping: a delay that
    // jumped from a bar to a 32nd because the knob went one click too far would not be a
    // musical control.
    app.on_encoder(0, +30);
    pump(2);
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), kDelayDivisions - 1);
    app.on_encoder(0, -30);
    pump(2);
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), 0);
}

TEST(ui_an_empty_slot_on_the_routing_page_does_nothing) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    mixer_page(app, 4);                                // ROUTE

    // A is the ping-pong switch: either direction throws it, because a switch is not a knob.
    CHECK(engine.sends().delay_ping_pong);
    app.on_encoder(0, -1);
    pump(2);
    CHECK(!engine.sends().delay_ping_pong);
    app.on_encoder(0, +1);
    pump(2);
    CHECK(engine.sends().delay_ping_pong);

    // C and D are empty -- ten send fields do not divide into four. Turning them changes
    // nothing and, in particular, does not fall through to the track faders below.
    const uint32_t refused_before = engine.status().refused.load(std::memory_order_relaxed);
    const float level_before = engine.level(0);
    app.on_encoder(2, +5);
    app.on_encoder(3, +5);
    pump(2);
    CHECK_NEAR(engine.level(0), level_before, 1e-6);
    CHECK_EQ(int(engine.status().refused.load(std::memory_order_relaxed) - refused_before), 0);
}

TEST(ui_f1_and_value_is_the_master_dj_filter) {
    // docs/SPEC.md's key table: VALUE with F1 is the "Master DJ filter". It held the record
    // quantize amount until the master had any settable field at all.
    Strata &app = fresh();
    Engine &engine = app_engine();

    CHECK_NEAR(engine.master().dj, 0.0f, 1e-6);        // the flat, bypassed centre

    app.on_key(ENAGA_KEY_F1, true);
    app.on_encoder(4, +16);
    pump(2);
    CHECK_NEAR(engine.master().dj, 32.0f / 63.0f, 1e-6);   // above centre: a high-pass
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::dj)]), 96);

    app.on_encoder(4, -60);
    pump(2);
    CHECK_NEAR(engine.master().dj, -1.0f, 1e-6);           // and it clamps at full low-pass
    app.on_key(ENAGA_KEY_F1, false);

    // The TEMPO did not move with it, which is the whole point of the modifier.
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 1300);
}

TEST(ui_the_tempo_is_posted_to_the_project_and_refused_under_an_external_clock) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    FakeHost &host = FakeHost::the();

    // A tempo click reaches PROJECT.BIN's field, which nothing wrote before: a project saved at
    // 142 reopened at 130 for ever.
    app.on_encoder(4, +120);
    pump(2);
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 1420);
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), 14200);
    CHECK(app_storage().dirty());

    // And the tempo-synced delay followed it, without the division moving.
    const float at_142 = engine.sends().delay_seconds;
    const int division = delay_division_index(engine.project().settings.sends[0]);
    app.on_encoder(4, -420);
    pump(2);
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), 10000);
    CHECK(engine.sends().delay_seconds > at_142 * 1.3f);
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), division);

    // Under an outside clock the knob is REFUSED rather than moved and then ignored
    // (docs/SPEC.md: VALUE "shows EXT and refuses edits when CLOCK SRC is not INTERNAL").
    // Before this the app's idea of the tempo drifted from the unit's with every click.
    host.set_source(ENAGA_CLOCK_MIDI);
    const int tenths = int(UiProbe::tempo_tenths(app));
    const int tempo_calls = host.control_calls(ENAGA_CLOCK_TEMPO);
    app.on_encoder(4, +50);
    pump(2);
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), tenths);
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), 10000);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_TEMPO), tempo_calls);
    CHECK(UiProbe::clock_refused(app));
    host.set_source(ENAGA_CLOCK_INTERNAL);
}

TEST(ui_a_loaded_project_brings_its_tempo_with_it) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    FakeHost &host = FakeHost::the();

    // A load writes the project through `project_for_load` and posts `Cmd::project_loaded`,
    // which is what the card side does. The tempo field is the one being tested.
    Project &incoming = engine.project_for_load();
    incoming.settings.tempo_centi_bpm = 14537;
    post_cmd(Cmd::project_loaded);
    pump(2);

    // Adopted on the next update pass: the clock set, and the UI's own number with it.
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 1453);
    CHECK_NEAR(host.tempo(), 145.37f, 0.01);

    // And it is adopted ONCE: further passes must not keep setting the clock, or the knob could
    // never move off a loaded project's tempo.
    const int tempo_calls = host.control_calls(ENAGA_CLOCK_TEMPO);
    pump(4);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_TEMPO), tempo_calls);

    app.on_encoder(4, +10);                            // ten clicks, so one BPM
    pump(4);
    CHECK_EQ(int(UiProbe::tempo_tenths(app)), 1463);   // and the echo did not drag it back
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), 14630);
}

TEST(ui_the_setup_page_holds_the_quantize_amount) {
    Strata &app = fresh();
    Engine &engine = app_engine();

    app.on_key(ENAGA_KEY_F3, true);                    // PROJECT
    app.on_key(ENAGA_KEY_F3, false);
    CHECK(!UiProbe::project_setup(app));

    // The octave keys, which do nothing else in this mode (a recorded deviation: every key in
    // docs/SPEC.md's table is taken, plain and with F1).
    setup_page(app, true);

    // A is the record quantize amount, which lived in `app/` alone and so was lost on every
    // restart although PROJECT.BIN has a field for it. Five per click.
    CHECK_EQ(UiProbe::quantize(app), 100);
    // In ONE event, which is the point of the command carrying a delta: six clicks computed on
    // this side would all have read the same stale 100 and only the last would have landed.
    app.on_encoder(0, -6);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), 70);
    CHECK_EQ(int(engine.project().settings.quantize), 70);
    CHECK(app_storage().dirty());

    // It clamps at both ends rather than wrapping.
    app.on_encoder(0, -40);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), 0);
    app.on_encoder(0, +40);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), 100);

    // B, C and D are not settings here: the other three on the page are the USER'S, in the
    // Enaga menu, and <enaga/clock.hpp> and <enaga/midi.hpp> expose no way to write them. In
    // particular they must not fall through to PROJECT mode's key and scale encoders.
    const int root_before = int(engine.pattern().root);
    const int scale_before = int(engine.pattern().scale);
    app.on_encoder(1, +5);
    app.on_encoder(2, +5);
    app.on_encoder(3, +5);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), 100);
    CHECK_EQ(int(engine.pattern().root), root_before);
    CHECK_EQ(int(engine.pattern().scale), scale_before);

    setup_page(app, false);
}

// Every mode draws. The SETUP page is new and reads the clock and the MIDI channel, so it goes
// through the same check the other screens do: a draw that touched a null machine or ran off a
// buffer would show up here and nowhere else until a unit was in hand.
TEST(ui_the_setup_page_and_the_mixer_pages_draw) {
    Strata &app = fresh();
    enaga::Canvas canvas(g_pixels);

    setup_page(app, true);
    app.draw(canvas);

    FakeHost &host = FakeHost::the();
    const int sources[] = {ENAGA_CLOCK_MIDI, ENAGA_CLOCK_SYNC, ENAGA_CLOCK_INTERNAL};
    for (int i = 0; i < 3; i++) {
        host.set_source(sources[i]);
        app.draw(canvas);
    }
    setup_page(app, false);

    // And every MIXER page, for the same reason.
    for (int page = 0; page < 6; page++) {
        mixer_page(app, page);
        app.draw(canvas);
    }
    CHECK(true);        // it did not fault, which is the claim
}

// --- the screen's layout ----------------------------------------------------------------------
//
// STRATA's first boot on a SmplTrek showed text drawn over other text and over the bars. Nothing
// here could have caught it: `FakeHost::t_text` drew nothing, so no test knew where a string
// landed. It records every call now, and these checks hold every screen to the font's real
// metrics, taken from Enaga rather than guessed:
//
//   - a glyph is 5 x 7 and the advance is 6 (third_party/enaga/firmware/hal/gfx.c, gfx_text;
//     <enaga/ui.hpp> char_width), so a string of n characters covers 6n - 1 by 7 pixels;
//   - the screen is 128 x 128, so 21 characters is a full line;
//   - the font has A-Z, 0-9, space and ! # + - . / : = ? % > (gfx.c font_chars). Anything
//     else draws as '?', which is how "A*" and "CLOCK SRC, CLOCK OUT," would look on the unit.
//
// A string may not cross another string, run off the screen, or PARTLY cover a rect. Sitting
// wholly on a rect is allowed, because that is a label on a box (KEYS' notes, PROJECT's slots).

namespace {

constexpr char kFontChars[] = " !#+-./0123456789:=?ABCDEFGHIJKLMNOPQRSTUVWXYZ%>";
constexpr int kAdvance = 6;
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

bool in_font(char c) {
    for (const char *p = kFontChars; *p; p++) {
        if (*p == c) return true;
    }
    return false;
}

struct Box {
    int x0, y0, x1, y1;    // inclusive
};

Box box_of(const FakeHost::TextDraw &d) {
    return Box{d.x, d.y, d.x + kAdvance * d.length - (kAdvance - kGlyphW) - 1, d.y + kGlyphH - 1};
}

bool overlap(const Box &a, const Box &b) {
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// Draw the screen and report every layout fault on it. `where` names the state in the report.
int layout_faults(Strata &app, const char *where) {
    FakeHost &host = FakeHost::the();
    enaga::Canvas canvas(g_pixels);
    host.clear_text();
    app.draw(canvas);                  // rects into g_pixels; text only into the record

    int faults = 0;
    const auto report = [&](const FakeHost::TextDraw &d, const char *what) {
        if (faults == 0) std::printf("    layout faults on %s:\n", where);
        std::printf("      \"%s\" at (%d, %d): %s\n", d.text, d.x, d.y, what);
        faults++;
    };

    if (host.text_dropped() != 0) {
        std::printf("    %s: more than %d strings, the record is incomplete\n", where,
                    FakeHost::kTextDraws);
        faults++;
    }

    for (int i = 0; i < host.text_count(); i++) {
        const FakeHost::TextDraw &d = host.text_draw(i);
        if (d.length == 0) continue;
        const Box b = box_of(d);

        for (int c = 0; c < d.length && c < int(sizeof d.text) - 1; c++) {
            if (!in_font(d.text[c])) {
                report(d, "a character the font does not have (draws as ?)");
                break;
            }
        }
        if (b.x0 < 0 || b.y0 < 0 || b.x1 >= enaga::Canvas::width || b.y1 >= enaga::Canvas::height) {
            report(d, "runs off the screen");
        }

        for (int j = 0; j < i; j++) {
            const FakeHost::TextDraw &other = host.text_draw(j);
            if (other.length == 0) continue;
            if (overlap(b, box_of(other))) {
                char what[96];
                std::snprintf(what, sizeof what, "crosses \"%s\" at (%d, %d)", other.text, other.x,
                              other.y);
                report(d, what);
            }
        }

        int lit = 0, area = 0;
        for (int y = b.y0 < 0 ? 0 : b.y0; y <= b.y1 && y < enaga::Canvas::height; y++) {
            for (int x = b.x0 < 0 ? 0 : b.x0; x <= b.x1 && x < enaga::Canvas::width; x++) {
                area++;
                if (g_pixels[y * enaga::Canvas::width + x] != 0) lit++;
            }
        }
        if (lit != 0 && lit != area) report(d, "partly covers a bar or box");
    }
    return faults;
}

void post_track(Cmd cmd, int track, uint8_t a) {
    Command command;
    command.cmd = cmd;
    command.track = uint8_t(track);
    command.a = a;
    app_engine().commands().post(command);
}

// The longest thing each screen can show, where the length depends on state: a three-character
// speed and a two-digit length on every track, the longest scale name, every machine.
void worst_case_content(Strata &app) {
    for (int t = 0; t < kTracks; t++) {
        post_track(Cmd::set_track_speed, t, uint8_t(Speed::x3_4));
        post_track(Cmd::set_track_length, t, 64);
        pump(1);
    }
    post_cmd(Cmd::set_root, 1);                 // C#: a two-character note name
    post_cmd(Cmd::set_scale, uint8_t(Scale::mixolydian));
    pump(2);
    (void)app;
}

void check_layout(Strata &app, const char *where) {
    CHECK_EQ(layout_faults(app, where), 0);
}

}  // namespace

TEST(ui_layout_grid_and_its_step_page) {
    Strata &app = fresh();
    worst_case_content(app);
    key(ENAGA_KEY_F4);
    the_app().on_key(ENAGA_KEY_F1, true);       // F1 + pad: track 1 muted
    app.on_pad_press(0, 100);
    app.on_pad_release(0);
    the_app().on_key(ENAGA_KEY_F1, false);
    pump(2);
    for (int page = 0; page < 4; page++) {
        check_layout(app, "GRID");
        key(ENAGA_KEY_SELECT_RIGHT);
    }

    // Recording, with a selection and undo to show: the edit-state line at its longest.
    key(ENAGA_KEY_F2);
    pump(200);                                  // past the REC ON notice
    step_down(0);
    step_down(15);
    step_up(15);
    step_up(0);
    pump(2);
    check_layout(app, "GRID, recording with a selection");
    key(ENAGA_KEY_F2);

    // The step page, every field page.
    step_down(2);
    key(ENAGA_KEY_OK);
    step_up(2);
    CHECK(UiProbe::step_page(app));
    for (int p = 0; p < kFieldPages; p++) {
        check_layout(app, "STEP PAGE");
        key(ENAGA_KEY_SELECT_RIGHT);
    }
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_edit_on_every_machine) {
    Strata &app = fresh();
    for (int machine = 0; machine < int(MachineId::count); machine++) {
        post_track(Cmd::set_machine, 0, uint8_t(machine));
        pump(4);
        key(ENAGA_KEY_F8);
        for (int page = 0; page < kPages; page++) {
            check_layout(app, machine_name(uint8_t(machine)));
            key(ENAGA_KEY_SELECT_RIGHT);
        }
    }
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_mixer_perform_keys_machine_resample) {
    Strata &app = fresh();
    worst_case_content(app);
    for (int page = 0; page < kMixerPageCount; page++) {
        mixer_page(app, page);
        check_layout(app, "MIXER");
    }

    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    check_layout(app, "PERFORM");
    // Two-digit scenes, a macro selected on the right, the mutation at its widest.
    post_cmd(Cmd::scene_select, 0, kScenes - 1);
    post_cmd(Cmd::scene_select, 1, kScenes - 1);
    pump(2);
    for (int i = 0; i < 12; i++) key(ENAGA_KEY_SELECT_RIGHT);   // the amount, up to 100
    key(ENAGA_KEY_OCTAVE_LEFT);                                 // macro D
    check_layout(app, "PERFORM, scene 16");
    key(ENAGA_KEY_OCTAVE_RIGHT);

    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));
    for (int scale = 0; scale < kScaleCount; scale++) {
        post_cmd(Cmd::set_scale, uint8_t(scale));
        pump(2);
        check_layout(app, scale_name(scale));
    }

    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    check_layout(app, "MACHINE");

    shift_key(ENAGA_KEY_F2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));
    check_layout(app, "RESAMPLE");
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_project_and_setup) {
    Strata &app = fresh();
    worst_case_content(app);
    key(ENAGA_KEY_F3);
    check_layout(app, "PROJECT");
    for (int scale = 0; scale < kScaleCount; scale++) {
        post_cmd(Cmd::set_scale, uint8_t(scale));
        pump(2);
        check_layout(app, scale_name(scale));
    }
    setup_page(app, true);
    FakeHost &host = FakeHost::the();
    const uint32_t sources[] = {ENAGA_CLOCK_MIDI, ENAGA_CLOCK_SYNC, ENAGA_CLOCK_INTERNAL};
    for (uint32_t source : sources) {
        host.set_source(source);
        check_layout(app, "SETUP");
    }
    setup_page(app, false);
    key(ENAGA_KEY_F4);
}

// --- the arrow keys ---------------------------------------------------------------------------
//
// One model in every mode, Enaga's own (its menu and SETTINGS page): UP and DOWN move through the
// rows on screen, LEFT and RIGHT move sideways, OK picks and CLR goes back. Before this, LEFT and
// RIGHT did nothing anywhere and UP and DOWN were copy and paste.

TEST(ui_arrows_in_grid_walk_tracks_and_pages_and_f1_copies_and_pastes) {
    Strata &app = fresh();
    key(ENAGA_KEY_F4);
    const int page = UiProbe::page(app);

    key(ENAGA_KEY_DOWN);                           // the rows are the tracks
    CHECK_EQ(UiProbe::selected(app), 1);
    key(ENAGA_KEY_UP);
    key(ENAGA_KEY_UP);                             // and they wrap, as the selection keys do
    CHECK_EQ(UiProbe::selected(app), kTracks - 1);
    key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::selected(app), 0);

    key(ENAGA_KEY_RIGHT);                          // sideways is the step page
    CHECK_EQ(UiProbe::page(app), (page + 1) % 4);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::page(app), page);

    // F1 + UP copies the track, F1 + DOWN pastes it onto the track the arrows moved to.
    for (int i = 0; i < 4 && UiProbe::page(app) != 0; i++) key(ENAGA_KEY_LEFT);
    key(ENAGA_KEY_STEP1);
    pump(2);
    CHECK(trig_at(0, 0));
    shift_key(ENAGA_KEY_UP);
    pump(2);
    key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::selected(app), 1);
    CHECK(!trig_at(1, 0));
    shift_key(ENAGA_KEY_DOWN);
    pump(2);
    CHECK(trig_at(1, 0));

    // With a STEP held, plain UP and DOWN still copy and paste that step: a key pressed with a
    // step held is a chord on the step, not navigation.
    step_down(0);
    key(ENAGA_KEY_UP);
    step_up(0);
    CHECK_EQ(UiProbe::selected(app), 1);           // it did not move the track
    CHECK(trig_at(1, 0));                          // and the release did not toggle the trig
    step_down(5);
    key(ENAGA_KEY_DOWN);
    step_up(5);
    pump(2);
    CHECK(trig_at(1, 5));
}

TEST(ui_arrows_on_the_step_page_walk_the_step_and_clr_goes_back) {
    Strata &app = fresh();
    key(ENAGA_KEY_F4);
    key(ENAGA_KEY_STEP1);                          // a trig on track 1, step 1
    pump(2);
    step_down(0);
    key(ENAGA_KEY_OK);
    step_up(0);
    CHECK(UiProbe::step_page(app));
    CHECK_EQ(UiProbe::page_step(app), 0);

    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::page_step(app), 1);
    key(ENAGA_KEY_LEFT);
    key(ENAGA_KEY_LEFT);                           // wraps to the track's last step
    CHECK_EQ(UiProbe::page_step(app),
             int(app_engine().pattern().tracks[0].length) - 1);

    // CLR is back. It used to erase the whole track from here.
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(!UiProbe::step_page(app));
    CHECK(trig_at(0, 0));
}

TEST(ui_arrows_in_edit_keys_perform_machine_and_resample) {
    Strata &app = fresh();

    key(ENAGA_KEY_F8);                             // EDIT: sideways is the parameter page
    const int param_page = UiProbe::param_page(app);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::param_page(app), (param_page + 1) % kPages);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::param_page(app), param_page);
    key(ENAGA_KEY_DOWN);                           // and the rows are the tracks
    CHECK_EQ(UiProbe::selected(app), 1);
    key(ENAGA_KEY_UP);

    shift_key(ENAGA_KEY_F4);                       // KEYS: sideways is the octave
    const int octave = UiProbe::keys_octave(app);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::keys_octave(app), octave + 1);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::keys_octave(app), octave);

    shift_key(ENAGA_KEY_F7);                       // PERFORM: sideways is the macro
    const int macro = UiProbe::macro_selected(app);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::macro_selected(app), (macro + 1) % kMacros);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::macro_selected(app), macro);

    shift_key(ENAGA_KEY_F8);                       // MACHINE: the rows are the machines
    const int machine = int(app_engine().machine_id(0));
    key(ENAGA_KEY_DOWN);
    pump(4);
    CHECK_EQ(int(app_engine().machine_id(0)), (machine + 1) % int(MachineId::count));
    key(ENAGA_KEY_UP);
    pump(4);
    CHECK_EQ(int(app_engine().machine_id(0)), machine);
    key(ENAGA_KEY_RIGHT);                          // and sideways is the track
    CHECK_EQ(UiProbe::selected(app), 1);
    key(ENAGA_KEY_LEFT);

    shift_key(ENAGA_KEY_F2);                       // RESAMPLE: rows are the length, sideways
    const int bars = UiProbe::take_bars_index(app);   // where the take goes
    const int where = UiProbe::resample_track(app);
    key(ENAGA_KEY_DOWN);
    CHECK(UiProbe::take_bars_index(app) != bars);
    key(ENAGA_KEY_UP);
    CHECK_EQ(UiProbe::take_bars_index(app), bars);
    key(ENAGA_KEY_RIGHT);
    CHECK(UiProbe::resample_track(app) != where);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::resample_track(app), where);
    key(ENAGA_KEY_F4);
}

TEST(ui_arrows_in_project_move_a_slot_cursor_and_ok_queues_it) {
    Strata &app = fresh();
    key(ENAGA_KEY_F3);
    for (int i = 0; i < 20; i++) key(ENAGA_KEY_LEFT);   // to slot 1; it stops, never wraps
    CHECK_EQ(UiProbe::slot_cursor(app), 0);
    key(ENAGA_KEY_UP);
    CHECK_EQ(UiProbe::slot_cursor(app), 0);

    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_DOWN);                           // a row is four slots
    CHECK_EQ(UiProbe::slot_cursor(app), 5);

    // OK queues the slot under the cursor. Stopped, the change lands at once.
    key(ENAGA_KEY_OK);
    pump(4);
    CHECK_EQ(int(app_engine().status().live_slot.load(std::memory_order_relaxed)), 5);
    CHECK(app_storage().busy_text() == nullptr);   // and OK did not start a save

    // A STEP key queues too, and the cursor follows it.
    key(ENAGA_KEY_STEP1);
    pump(4);
    CHECK_EQ(UiProbe::slot_cursor(app), 0);
    CHECK_EQ(int(app_engine().status().live_slot.load(std::memory_order_relaxed)), 0);

    // On the SETUP page CLR goes back, and the arrows leave the cursor alone.
    setup_page(app, true);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::slot_cursor(app), 0);
    key(ENAGA_KEY_CLR);
    CHECK(!UiProbe::project_setup(app));
    key(ENAGA_KEY_F4);
}

TEST(ui_f1_f3_twice_saves_and_once_does_not) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));
    CHECK_EQ(UiProbe::notice(app), "F1+F3 AGAIN TO SAVE");
    pump(4);
    CHECK(app_storage().busy_text() == nullptr);   // once is not a save

    pump(2100);                                    // too late: this arms again instead
    shift_key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::notice(app), "F1+F3 AGAIN TO SAVE");

    shift_key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::notice(app), "SAVING");
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(!app_storage().dirty());
    key(ENAGA_KEY_F4);
}
