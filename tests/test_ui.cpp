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
#include <enaga/files.hpp>
#include <enaga/wav.hpp>

#include <cstdlib>
#include <cstring>

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
    static int shown_bank(const Strata &app) { return app.shown_bank_; }

    static int held_step(const Strata &app) { return app.held_step_; }
    static int held_key(const Strata &app) { return app.held_key_; }
    static bool held_used(const Strata &app) { return app.held_used_; }
    static int range_first(const Strata &app) { return app.range_first_; }
    static int range_last(const Strata &app) { return app.range_last_; }
    static bool step_page(const Strata &app) { return app.step_page_; }
    static int page_step(const Strata &app) { return app.page_step_; }

    static bool rec(const Strata &app) { return app.rec_; }
    static bool mute_mode(const Strata &app) { return app.mute_mode_; }
    static bool note_repeat_mode(const Strata &app) { return app.note_repeat_mode_; }
    static int scene_holding(const Strata &app) { return app.scene_holding_; }
    static int keys_octave(const Strata &app) { return app.keys_octave_; }
    static int pad_held_note(const Strata &app, int pad) { return app.pad_held_note_[pad]; }
    static int pad_note(const Strata &app, int pad) { return app.pad_note(pad); }
    static bool fill_held(const Strata &app) { return app.fill_held_; }
    static int macro_learn(const Strata &app) { return app.macro_learn_; }
    static int keys_held_note(const Strata &app, int key) { return app.keys_held_note_[key]; }

    static const char *notice(const Strata &app) { return app.notice_; }
    static bool tune_notes(const Strata &app) { return app.tune_notes_; }
    static void notes_view_on(Strata &app) { app.tune_notes_ = true; }   // as a fresh start
    static bool notice_showing(const Strata &app) { return app.notice_showing(); }

    static int pressure_pad(const Strata &app) { return app.pressure_pad_; }
    static int take_bars_index(const Strata &app) { return app.take_bars_index_; }
    static int resample_track(const Strata &app) { return app.resample_track_; }
    static uint8_t macro_knob(const Strata &app, int macro) {
        return app.macro_position_[macro];
    }

    static uint32_t tempo_tenths(const Strata &app) { return app.tempo_tenths_; }
    static bool clock_refused(const Strata &app) { return app.clock_refused_; }
    static bool stop_armed(const Strata &app) { return app.stop_armed_; }
    static bool project_setup(const Strata &app) { return app.project_setup(); }
    static int project_page(const Strata &app) { return int(app.project_page_); }
    static int kit_cursor(const Strata &app) { return app.kit_cursor_; }
    static int sample_cursor(const Strata &app) { return app.sample_cursor_; }
    static int chain_cursor(const Strata &app) { return app.chain_cursor_; }
    static int pattern_field(const Strata &app) { return app.pattern_field_; }
    static int slot_cursor(const Strata &app) { return app.slot_cursor_; }
    static int macro_selected(const Strata &app) { return app.macro_selected_; }
    static int quantize(const Strata &app) { return app.quantize(); }
    static int menu_cursor(const Strata &app) { return app.menu_cursor_; }
    static PerfFx perf_picked(const Strata &app) { return app.perf_picked_; }
    static uint8_t perf_amount(const Strata &app) { return app.perf_amount_; }
    static uint8_t loop_mix(const Strata &app) { return app.loop_mix_; }
    static bool func_help(const Strata &app) { return app.func_help_showing(); }
    static bool verdict_free(const Strata &app) { return app.verdict_keys_free(); }
    static bool mutation_pending(const Strata &app) { return app.mutation_pending_; }
    static uint8_t pad_r(const Strata &app, int pad) { return app.leds_.pad_r[pad]; }
    static uint8_t step_green(const Strata &app, int i) { return app.leds_.step_green[i]; }
    static uint8_t step_red(const Strata &app, int i) { return app.leds_.step_red[i]; }
    static uint8_t pad_g(const Strata &app, int pad) { return app.leds_.pad_g[pad]; }
    static uint8_t pad_b(const Strata &app, int pad) { return app.leds_.pad_b[pad]; }
    static uint8_t key_red(const Strata &app, int code) { return app.leds_.key_red[code]; }
    static uint8_t key_green(const Strata &app, int code) { return app.leds_.key_green[code]; }
    static uint32_t cpu_max(const Strata &app) { return app.cpu_max_; }
    static int project_cursor(const Strata &app) { return app.project_cursor_; }
    static bool name_entry_open(const Strata &app) { return app.name_entry_.active(); }
    static const char *name_entry_text(Strata &app) { return app.name_entry_.text(); }
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

// The morph position, which is a `u` field rather than the a/b/c bytes above.
void post_cmd_morph(uint32_t at) {
    Command command;
    command.cmd = Cmd::set_morph;
    command.u = at;
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

// HOME double-tapped is KEYS (docs/CONTROLS.md). From GRID one more press would leave KEYS
// again, so this presses until it is there.
void open_keys() {
    for (int i = 0; i < 3 && UiProbe::mode(the_app()) != int(Mode::keys); i++) key(ENAGA_KEY_F4);
}

// The FUNC menu's rows, in order (app/app.hpp, MenuItem).
enum MenuRow : int { kMenuSave, kMenuSaveAs, kMenuOpen, kMenuNew, kMenuReload, kMenuClear,
                     kMenuKits, kMenuSample, kMenuSpeed, kMenuChain, kMenuSong, kMenuAutofill,
                     kMenuQuantize, kMenuFreeze, kMenuLoopLen, kMenuRowCount };

// FUNC + HOME, then DOWN to `row`: the cursor starts on SAVE each time the menu opens.
void menu_to(int row) {
    if (UiProbe::mode(the_app()) == int(Mode::menu)) key(ENAGA_KEY_CLR);
    shift_key(ENAGA_KEY_F4);
    for (int i = 0; i < row; i++) key(ENAGA_KEY_DOWN);
}

// Open the menu on `row` and press OK once.
void menu_run(int row) {
    menu_to(row);
    key(ENAGA_KEY_OK);
}

// A setting row: OK steps it once, then CLR closes the menu, back to the screen it opened over.
void menu_toggle(int row) {
    menu_run(row);
    key(ENAGA_KEY_CLR);
}

void step_down(int raw) { the_app().on_key(ENAGA_KEY_STEP1 + raw, true); }
void step_up(int raw) { the_app().on_key(ENAGA_KEY_STEP1 + raw, false); }

const Step &step_of(int track, int step) {
    return app_engine().pattern().tracks[track].steps[step];
}

bool trig_at(int track, int step) { return (step_of(track, step).flags & kStepTrig) != 0; }

void post_cmd_track_param(int track, int param, uint8_t value) {
    Command command;
    command.cmd = Cmd::set_param;
    command.track = uint8_t(track);
    command.a = uint8_t(param);
    command.b = value;
    app_engine().commands().post(command);
}

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

// What a knob click leaves behind: `base` plus `delta`, unless the parameter is the machine's
// TUN or PIT, where a click is a SEMITONE and the engine steps to the nearest note (core/
// tuning.hpp, Cmd::lock_delta and Cmd::param_delta). A test that hard-codes "base + delta" on
// encoder A passes only while a test before it happens to leave a page open whose first
// parameter is not the tuning one -- which is how two tests here passed in the suite's order
// and failed alone, and in a 64-shard run on Roland's PC (2026-11-23).
int knob_result(int track, int param, int base, int delta) {
    const uint8_t machine = app_engine().machine_id(track);
    if (tune_param(machine) == param) return int(tune_step(machine, uint8_t(base), delta));
    const int value = base + delta;
    return value < 0 ? 0 : (value > 127 ? 127 : value);
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
    app.on_pad_release(13);                // MUTATE (a held freeze)
    app.on_key(ENAGA_KEY_PUSH_D, false);   // a held tape stop
    if (UiProbe::mute_mode(app)) {         // the MUTE pad is a toggle, so it takes another press
        app.on_pad_press(5, 100);
        app.on_pad_release(5);
    }
    if (UiProbe::note_repeat_mode(app)) {  // FUNC + FILL is a toggle too
        app.on_key(ENAGA_KEY_F1, true);
        app.on_pad_press(1, 100);
        app.on_pad_release(1);
        app.on_key(ENAGA_KEY_F1, false);
    }
    if (UiProbe::rec(app)) key(ENAGA_KEY_F2);
    UiProbe::notes_view_on(app);           // NOTE view is UI state, not saved: on at every start
    // The performance mixer off and any loop thrown away: push VALUE + CLR, twice.
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    key(ENAGA_KEY_CLR);
    pump();
    key(ENAGA_KEY_CLR);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);

    // HOME after the last test's HOME must not be a double tap (which is KEYS), and nor must the
    // test's own first HOME after this one: a second apart, as the app's clock sees it.
    g_ms += 1000;
    pump();
    key(ENAGA_KEY_F4);                     // GRID: closes the step page and drops any hold
    g_ms += 1000;
    pump();
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
        for (int i = 0; i < kKitSoundParams; i++) incoming.kit.tracks[t].params[i] = 64;
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
    const int param = encoder_param(1);   // B: A is TUN, which turns in notes (NOTE view)
    const uint8_t kit_before = app_engine().params(0).v[param];

    app.on_encoder(1, +3);
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
    key(ENAGA_KEY_PUSH_A);                         // TUN in numbers: one count a click
    CHECK(!UiProbe::tune_notes(app));

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
    for (int i = 0; i < kEditPages; i++) key(ENAGA_KEY_SELECT_LEFT);
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

TEST(ui_func_fill_turns_note_repeat_on_and_a_held_pad_rolls) {
    Strata &app = fresh();
    // FUNC + FILL toggles the mode and does not start a fill.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(1, 100);
    app.on_pad_release(1);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(UiProbe::note_repeat_mode(app));
    CHECK(std::strcmp(UiProbe::notice(app), "NOTE RPT") == 0);
    CHECK(!app_engine().fill());

    // A held lower pad rolls its track, selects it, and its pressure is the roll's velocity, not
    // macro 1.
    const uint8_t macro = app_engine().macro_position(0);
    app.on_pad_press(4, 100);
    pump(2);
    CHECK(app_engine().rolling(2));
    CHECK_EQ(UiProbe::selected(app), 2);
    app.on_pad_pressure(4, 127);
    pump(2);
    CHECK_EQ(int(app_engine().macro_position(0)), int(macro));
    app.on_pad_release(4);
    pump(2);
    CHECK(!app_engine().rolling(2));

    // Turning the mode off under a held pad does not leave the roll running.
    app.on_pad_press(6, 100);
    pump(2);
    CHECK(app_engine().rolling(3));
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(1, 100);
    app.on_pad_release(1);
    app.on_key(ENAGA_KEY_F1, false);
    CHECK(!UiProbe::note_repeat_mode(app));
    CHECK(std::strcmp(UiProbe::notice(app), "PADS PLAY") == 0);
    app.on_pad_release(6);
    pump(2);
    CHECK(!app_engine().rolling(3));

    // And with the mode off a pad plays one note again.
    app.on_pad_press(6, 100);
    pump(2);
    CHECK(!app_engine().rolling(3));
    app.on_pad_release(6);
}

TEST(ui_func_push_d_holds_a_tape_stop_and_func_mutate_a_delay_freeze) {
    Strata &app = fresh();
    pump(2);
    const uint8_t undo_before = undo_depth();

    // FUNC + push D: the tape stop, for as long as push D is held -- even after FUNC comes up.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_PUSH_D, true);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(app_engine().tape_stop().held());
    CHECK(std::strcmp(UiProbe::notice(app), "TAPE STOP") == 0);
    app.on_key(ENAGA_KEY_PUSH_D, false);
    pump(2);
    CHECK(!app_engine().tape_stop().held());
    // A plain push D is not a tape stop.
    app.on_key(ENAGA_KEY_PUSH_D, true);
    app.on_key(ENAGA_KEY_PUSH_D, false);
    pump(2);
    CHECK(!app_engine().tape_stop().held());

    // FUNC + MUTATE: the delay freeze, held, and no mutation (no undo level).
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(13, 100);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(app_engine().delay_freeze().held());
    CHECK(std::strcmp(UiProbe::notice(app), "FREEZE") == 0);
    app.on_pad_release(13);
    pump(2);
    CHECK(!app_engine().delay_freeze().held());
    CHECK_EQ(int(undo_depth()), int(undo_before));
}

TEST(ui_func_push_d_with_a_step_held_clears_its_lock_not_a_tape_stop) {
    // Roland, 2026-11-22: every push with a STEP held is the step's. FUNC + push A-C already
    // cleared that step's lock there; FUNC + push D was a tape stop instead.
    Strata &app = fresh();
    key(ENAGA_KEY_F4);
    pump(2);
    step_down(5);
    app.on_encoder(3, +6);                           // lock D on step 6
    pump(2);
    uint8_t value = 0;
    const int param = page_first(Page(UiProbe::param_page(app))) + 3;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, 5, uint8_t(param), value));
    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_PUSH_D, true);
    pump(2);
    CHECK(!app_engine().tape_stop().held());
    CHECK_EQ(UiProbe::notice(app), "LOCK CLEARED");
    app.on_key(ENAGA_KEY_PUSH_D, false);
    app.on_key(ENAGA_KEY_F1, false);
    step_up(5);
    pump(2);
    CHECK(!pattern_get_lock(app_engine().pattern(), 0, 5, uint8_t(param), value));
    CHECK(!app_engine().tape_stop().held());
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
    const int param = encoder_param(0);
    pump(2);
    const int live = int(app_engine().params(0).v[param]);
    step_down(1);
    app.on_encoder(0, +5);
    step_up(1);
    pump();
    uint8_t value = 0;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, 33, uint8_t(param), value));
    CHECK_EQ(int(value), knob_result(0, param, live, +5));
    CHECK_EQ(int(step_of(0, 1).lock_count), 0);
    CHECK(!trig_at(0, 33));                        // the hold was spent on the lock

    // The octave keys CLAMP at both ends: four pages, and walking off either end stays there.
    for (int i = 0; i < 4; i++) key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::page(app), 3);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_OCTAVE_LEFT);
    CHECK_EQ(UiProbe::page(app), 0);

    // LEFT and RIGHT move the step page in GRID as well, and those WRAP. The screen keys do
    // not: on GRID they turn the EDIT page that A-D work on (docs/CONTROLS.md).
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::page(app), 3);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::page(app), 0);
    const int param_page = UiProbe::param_page(app);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::page(app), 0);
    CHECK_EQ(UiProbe::param_page(app), (param_page + 1) % kEditPages);
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::param_page(app), param_page);
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

TEST(ui_f1_f5_restarts_every_track_from_step_1_while_playing) {
    // docs/SPEC.md's key table: F1 + F5 is "Restart from step 1". Running, the clock is located
    // to 0 and keeps going, and the playheads follow it back rather than carrying on from where
    // the pass count had got to.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    key(ENAGA_KEY_F5);
    CHECK(host.running());
    for (uint32_t t = 0; t <= 24 * 5; t += 24) {
        host.deliver_tick(t, 0);
        pump();
    }
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 5);

    const int locates = host.control_calls(ENAGA_CLOCK_LOCATE);
    shift_key(ENAGA_KEY_F5);
    pump(2);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_LOCATE), locates + 1);
    CHECK(host.running());
    CHECK_EQ(int(host.position()), 0);
    CHECK_EQ(UiProbe::notice(app), "RESTART");
    for (uint32_t t = 0; t <= 24 * 2; t += 24) {
        host.deliver_tick(t, 0);
        pump();
    }
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 2);

    // Under an outside clock it is refused and nothing moves.
    host.set_source(ENAGA_CLOCK_MIDI);
    shift_key(ENAGA_KEY_F5);
    pump(2);
    CHECK(UiProbe::clock_refused(app));
    CHECK_EQ(UiProbe::notice(app), "EXT CLOCK");
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 2);
    host.set_source(ENAGA_CLOCK_INTERNAL);
    key(ENAGA_KEY_F6);
}

TEST(ui_f1_f6_stops_cuts_and_resets_in_one_press) {
    // F1 + F6: "Stop and reset all tracks". One press does what STOP and its second press do,
    // drops a queued pattern change, and puts the clock back at 0.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    key(ENAGA_KEY_F5);
    for (uint32_t t = 0; t <= 24 * 3; t += 24) {
        host.deliver_tick(t, 0);
        pump();
    }
    post_cmd(Cmd::queue_pattern, 0, 1, 0);
    pump(2);
    CHECK(app_engine().queue().pending);

    shift_key(ENAGA_KEY_F6);
    pump(2);
    CHECK(!host.running());
    CHECK_EQ(int(host.position()), 0);
    CHECK(!app_engine().queue().pending);
    CHECK_EQ(int(app_engine().status().playhead[0].load(std::memory_order_relaxed)), 0);
    CHECK_EQ(UiProbe::notice(app), "RESET");
    CHECK(!UiProbe::stop_armed(app));          // the cut is done: the next F6 is a plain stop

    // Refused under a running outside clock, like plain F6.
    host.set_source(ENAGA_CLOCK_MIDI);
    host.set_running(true);
    shift_key(ENAGA_KEY_F6);
    pump(2);
    CHECK(host.running());
    CHECK_EQ(UiProbe::notice(app), "EXT CLOCK");
    host.set_running(false);
    host.set_source(ENAGA_CLOCK_INTERNAL);
}

TEST(ui_f1_f6_puts_a_running_chain_back_on_its_first_entry) {
    // Roland, 2026-11-22: FUNC + STOP resets a running chain too, so the set starts again from
    // the top. A chain that is off stays off and the pattern stays where it is.
    Strata &app = fresh();
    (void)app;
    FakeHost &host = FakeHost::the();
    Project &incoming = app_engine().project_for_load();
    project_init(incoming);
    incoming.settings.chain_length = 2;
    incoming.chain[0] = ChainEntry{0, 0, 1};
    incoming.chain[1] = ChainEntry{0, 1, 1};
    post_cmd(Cmd::project_loaded);
    pump(2);

    key(ENAGA_KEY_F5);
    host.deliver_tick(0, 0);
    pump();
    post_cmd(Cmd::set_chain, 1);
    pump(2);
    host.deliver_tick(1536, 0);                    // the pass ends: entry 2, A02
    pump(2);
    CHECK_EQ(int(app_engine().chain().entry), 1);
    CHECK_EQ(int(app_engine().project().slot), 1);

    shift_key(ENAGA_KEY_F6);
    pump(2);
    CHECK(app_engine().chain().active);
    CHECK_EQ(int(app_engine().chain().entry), 0);
    CHECK_EQ(int(app_engine().project().slot), 0);

    // Chain off: F1 + F6 leaves the pattern alone.
    post_cmd(Cmd::set_chain, 0);
    post_cmd(Cmd::queue_pattern, 0, 1, 0);        // stopped, so A02 at once
    pump(2);
    shift_key(ENAGA_KEY_F6);
    pump(2);
    CHECK(!app_engine().chain().active);
    CHECK_EQ(int(app_engine().project().slot), 1);
}

// --- the two ways a hold used to be stranded ------------------------------------------------
//
// docs/UNIT_CHECKLIST.md: "Hold a STEP key, change page or mode, let go, then turn an encoder. No
// parameter lock should appear. The hold was resolved against the CURRENT page on release and
// against the press's page on the way in, and KEYS, PERFORM and MUTE swallow the STEP keys
// entirely -- so a hold could be stranded forever, and the next encoder move wrote a lock onto a
// phantom step."

TEST(ui_hold_step_and_turn_value_sets_that_steps_note) {
    // docs/SPEC.md, "Step editing": hold STEP + VALUE: note. It used to turn the tempo.
    Strata &app = fresh();
    const Step &step = app_engine().pattern().tracks[0].steps[3];
    const int note = int(step.note);
    const uint8_t flags = step.flags;
    const uint32_t tempo = UiProbe::tempo_tenths(app);

    step_down(3);
    app.on_encoder(4, +2);
    pump(2);
    step_up(3);
    pump(2);
    CHECK_EQ(int(step.note), note + 2);
    CHECK_EQ(int(step.flags), int(flags));            // used as a modifier: no toggle
    CHECK_EQ(UiProbe::tempo_tenths(app), tempo);

    // One undo puts it back.
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(int(step.note), note);
}

TEST(ui_a_mutation_is_kept_with_ok_and_reverted_with_clr_in_any_mode) {
    // docs/SPEC.md: MUTATE, "OK keeps, CLR reverts". And F1 + CLR undoes one in PERFORM, where
    // plain CLR clears a macro -- before this a mutation could not be undone without leaving.
    Strata &app = fresh();
    for (int i = 0; i < 16; i += 4) {                // trigs for the velocity mutation to move
        if ((app_engine().pattern().tracks[0].steps[i].flags & kStepTrig) == 0) {
            step_down(i);
            step_up(i);
        }
    }
    pump(2);
    static TrackPattern before;
    std::memcpy(&before, &app_engine().pattern().tracks[0], sizeof before);
    const auto same = [] {
        return std::memcmp(&before, &app_engine().pattern().tracks[0], sizeof before) == 0;
    };
    const auto mutate = [&app] {
        app.on_pad_press(13, 100);
        app.on_pad_release(13);
        pump(2);
    };

    mutate();
    CHECK(!same());
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(same());
    CHECK_EQ(UiProbe::notice(app), "REVERTED");

    mutate();
    CHECK(!same());
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(!same());
    CHECK_EQ(UiProbe::notice(app), "KEPT");
    CHECK(!UiProbe::step_page(app));                 // OK was the verdict, not "open the step"
    shift_key(ENAGA_KEY_CLR);                        // and an undo after keeping still works
    pump(2);
    CHECK(same());

    // In PERFORM: F1 + CLR is undo, not "clear the macro".
    shift_key(ENAGA_KEY_F7);
    pump(2);
    mutate();
    key(ENAGA_KEY_OK);                               // kept
    CHECK(!same());
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(same());
    CHECK_EQ(UiProbe::notice(app), "UNDO");

    // A scene pad held + CLR erases that scene; F1 + OK is redo.
    post_cmd(Cmd::kit_store);
    post_cmd(Cmd::scene_capture, 0, 0, 0);
    pump(2);
    CHECK(scene_count(app_engine().scenes().scenes[0]) > 0);
    app.on_pad_press(7, 100);
    key(ENAGA_KEY_CLR);
    app.on_pad_release(7);
    pump(2);
    CHECK_EQ(scene_count(app_engine().scenes().scenes[0]), 0);
    CHECK_EQ(UiProbe::notice(app), "SCENE ERASED");
    shift_key(ENAGA_KEY_OK);
    pump(2);
    CHECK(!same());                                  // the mutation is back
    CHECK_EQ(UiProbe::notice(app), "REDO");
    key(ENAGA_KEY_F4);
    pump(2);
}

TEST(ui_a_pending_mutation_leaves_a_held_step_its_own_ok_and_clr) {
    // MUTATE, then hold a STEP + CLR to clear that step. The verdict used to take the CLR first:
    // the mutation was reverted instead, and because nothing marked the hold as used, its
    // release then TOGGLED the step's trig -- two edits nobody asked for. A held step, a range,
    // a scene pad and RESAMPLE all give OK and CLR a meaning of their own, and keep it.
    Strata &app = fresh();
    key(ENAGA_KEY_F4);
    for (int i = 0; i < 16; i += 4) {
        if ((app_engine().pattern().tracks[0].steps[i].flags & kStepTrig) == 0) {
            step_down(i);
            step_up(i);
        }
    }
    pump(2);
    const auto mutate = [&app] {
        app.on_pad_press(13, 100);
        app.on_pad_release(13);
        pump(2);
    };

    mutate();
    static TrackPattern mutated;
    std::memcpy(&mutated, &app_engine().pattern().tracks[0], sizeof mutated);
    CHECK((mutated.steps[4].flags & kStepTrig) != 0);
    step_down(4);
    key(ENAGA_KEY_CLR);                              // hold STEP 5 + CLR: clear that step
    step_up(4);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "STEP CLEARED");
    CHECK((app_engine().pattern().tracks[0].steps[4].flags & kStepTrig) == 0);
    // Nothing else moved: the mutation is still there on the other steps.
    CHECK_EQ(app_engine().pattern().tracks[0].steps[8].velocity, mutated.steps[8].velocity);
    CHECK_EQ(app_engine().pattern().tracks[0].steps[12].velocity, mutated.steps[12].velocity);

    // Hold STEP + OK opens the step page rather than keeping the mutation.
    mutate();
    step_down(8);
    key(ENAGA_KEY_OK);
    step_up(8);
    pump(2);
    CHECK(UiProbe::step_page(app));
    CHECK_EQ(UiProbe::page_step(app), 8);
    key(ENAGA_KEY_OK);                               // close it
    pump(2);

    // RESAMPLE: OK arms the take, CLR cancels it -- not a verdict on a mutation.
    mutate();
    shift_key(ENAGA_KEY_F2);                         // RESAMPLE: a mode change settles it
    pump(2);
    mutate();
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(std::strcmp(UiProbe::notice(app), "KEPT") != 0);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "CANCELLED");
    key(ENAGA_KEY_F4);
    pump(2);
}

TEST(ui_leaving_the_app_ends_a_held_midi_note) {
    // App::stop, the switch to another app or a power-off: a KEYS key still down with MIDI OUT
    // on sent a note nobody could stop once STRATA was gone.
    Strata &app = fresh();
    post_cmd(Cmd::set_midi_out, 7);                 // track 1 to channel 7
    pump(2);
    open_keys();                                     // KEYS
    FakeHost::the().clear_midi();
    step_down(0);
    pump(2);
    CHECK_EQ(FakeHost::the().midi_messages(0x90, 7), 1);
    app.stop();
    CHECK_EQ(FakeHost::the().midi_messages(0x80, 7), 1);
    step_up(0);
    post_cmd(Cmd::set_midi_out, 0);
    key(ENAGA_KEY_F4);
    pump(2);
}

TEST(ui_an_undo_settles_a_pending_mutation) {
    // MUTATE, then F1 + CLR takes it back. The mutation used to stay "pending", so the next plain
    // CLR undid a SECOND level -- the edit before the mutation -- and said REVERTED.
    Strata &app = fresh();
    key(ENAGA_KEY_F4);
    for (int i = 0; i < 16; i += 4) {
        if ((app_engine().pattern().tracks[0].steps[i].flags & kStepTrig) == 0) {
            step_down(i);
            step_up(i);
        }
    }
    step_down(1);                                    // the edit before the mutation: a trig
    step_up(1);
    pump(2);
    const bool had_trig = (app_engine().pattern().tracks[0].steps[1].flags & kStepTrig) != 0;

    app.on_pad_press(13, 100);                       // MUTATE
    app.on_pad_release(13);
    pump(2);
    shift_key(ENAGA_KEY_CLR);                        // undo: the mutation is gone
    pump(2);
    key(ENAGA_KEY_CLR);                              // plain CLR: erases the track, as in GRID
    pump(2);
    CHECK(std::strcmp(UiProbe::notice(app), "REVERTED") != 0);
    shift_key(ENAGA_KEY_CLR);                        // and that erase undoes back to the trig
    pump(2);
    CHECK_EQ((app_engine().pattern().tracks[0].steps[1].flags & kStepTrig) != 0, had_trig);
}

TEST(ui_a_keys_key_holds_its_note_until_it_comes_up) {
    // KEYS: the press plays, the release lets that same note go -- even after an octave change
    // while it was down -- and a mode change lets go of whatever is still held.
    Strata &app = fresh();
    open_keys();                                      // KEYS
    step_down(0);
    const int note = UiProbe::keys_held_note(app, 0);
    CHECK(note >= 0);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    step_up(0);
    CHECK_EQ(UiProbe::keys_held_note(app, 0), -1);

    step_down(2);
    CHECK(UiProbe::keys_held_note(app, 2) >= 0);
    key(ENAGA_KEY_F4);                                // away, with the key still down
    CHECK_EQ(UiProbe::keys_held_note(app, 2), -1);
    step_up(2);
    pump(2);
}

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
    pump(2);
    const uint8_t before = app_engine().params(0).v[param];
    app.on_encoder(0, +7);
    pump();
    CHECK_EQ(int(app_engine().params(0).v[param]), knob_result(0, param, int(before), +7));
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
        {"MENU", ENAGA_KEY_F4, true, Mode::menu},
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

TEST(ui_hold_a_note_and_press_a_later_step_and_its_len_reaches_that_step) {
    Strata &app = fresh();
    key(ENAGA_KEY_STEP1 + 0);                      // a note on step 1
    pump(2);
    CHECK(trig_at(0, 0));
    const uint8_t before = step_of(0, 0).length;

    // Step 1 held, step 4 pressed: four steps, set when the first key comes up.
    step_down(0);
    step_down(3);
    CHECK_EQ(UiProbe::notice(app), "LEN 4 STP");
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(before));   // nothing yet, while held
    step_up(3);
    step_up(0);
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(length_index_within_steps(4)));
    CHECK(trig_at(0, 0));                          // and the release toggled nothing
    CHECK(!trig_at(0, 3));

    // Five steps: the table has 4 and 6, and the note never runs past the step pressed.
    step_down(0);
    step_down(4);
    step_up(4);
    step_up(0);
    pump(2);
    CHECK_EQ(int(length_sixteenths(step_of(0, 0).length)), 4 * 16);

    // The selection is still a selection: a copy afterwards puts the old LEN back first, so
    // the copy is the only change.
    const uint8_t was = step_of(0, 0).length;
    step_down(0);
    step_down(1);                                  // two steps
    step_up(1);
    step_up(0);
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(length_index_within_steps(2)));
    app.on_key(ENAGA_KEY_F1, true);
    key(ENAGA_KEY_UP);                             // FUNC+UP: copy
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(was));

    // Used while still held -- a knob, CLR -- it is not a length at all.
    step_down(0);
    step_down(7);
    app.on_encoder(0, +1);                         // A: transpose the range
    step_up(7);
    step_up(0);
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(was));

    // Held on an EMPTY step, or pressed earlier: only a range.
    step_down(5);
    step_down(9);
    CHECK_EQ(UiProbe::notice(app), "RANGE");
    step_up(9);
    step_up(5);
    step_down(8);
    step_down(0);
    CHECK_EQ(UiProbe::notice(app), "RANGE");
    step_up(0);
    step_up(8);
    pump(2);
    CHECK_EQ(int(step_of(0, 0).length), int(was));
    key(ENAGA_KEY_CLR);                            // the range 1-9, and step 1 with it
    pump(2);
}

TEST(ui_the_step_page_wins_over_the_step_key_still_held_open_it) {
    // Hold STEP + OK opens the step page; a hand still on the STEP key turning C must change
    // LEN, the page's third field, and not lock C (CUT on a VA) on the step.
    Strata &app = fresh();
    key(ENAGA_KEY_STEP1 + 2);
    pump(2);
    const uint8_t before = step_of(0, 2).length;
    step_down(2);
    key(ENAGA_KEY_OK);
    CHECK(UiProbe::step_page(app));
    app.on_encoder(2, +2);
    pump(2);
    CHECK_EQ(int(step_of(0, 2).length), int(before) + 2);
    CHECK_EQ(int(step_of(0, 2).lock_count), 0);
    step_up(2);
    pump(2);
    CHECK(trig_at(0, 2));                          // the release is not a tap
    key(ENAGA_KEY_OK);                             // close the page
    key(ENAGA_KEY_CLR);
    pump(2);
}

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
    // Held on a note and pressed later: the selection also offers that note's LEN (the test
    // after this one); CLR below erases the step with it.
    CHECK_EQ(UiProbe::notice(app), "LEN 4 STP");
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
    CHECK_EQ(UiProbe::notice(app), "CLEARED FUNC+CLR UNDO");
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
        {ENAGA_KEY_F4, true, Mode::menu},     {ENAGA_KEY_F7, true, Mode::perform},
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
    open_keys();
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

    open_keys();
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
        open_keys();
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
    open_keys();

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

TEST(ui_a_key_held_while_recording_gives_the_step_its_length) {
    // KEYS on VA POLY, REC on: a key held for four steps records a step whose LEN is four steps,
    // so with SUS up the chord plays back as long as it was played. On a drum the hold is not
    // measured: LEN stays the one step a recorded hit always had.
    Strata &app = fresh();
    key(ENAGA_KEY_F8);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_DOWN);    // track 7, VA POLY
    CHECK_EQ(UiProbe::selected(app), 6);
    open_keys();
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));

    FakeHost &host = FakeHost::the();
    host.set_running(true);
    host.set_position(96);
    key(ENAGA_KEY_F2);                                  // REC on
    CHECK(UiProbe::rec(app));

    step_down(2);
    pump(2);
    host.set_position(96 + 4 * 24);                     // four steps later
    step_up(2);
    pump(2);

    int where = -1;
    for (int s = 0; s < kStepsPerTrack; s++) {
        if (trig_at(6, s)) where = s;
    }
    CHECK(where >= 0);
    CHECK_EQ(int(step_of(6, where).length), 12);        // 4 steps (core/timing.hpp's table)

    // A quick tap on the same step's note again replaces its length with the tap's.
    host.set_position(96);
    step_down(2);
    pump(2);
    host.set_position(96 + 6);                          // a quarter of a step
    step_up(2);
    pump(2);
    CHECK_EQ(int(step_of(6, where).length), 4);

    key(ENAGA_KEY_F2);
    host.set_running(false);
}

TEST(ui_a_held_key_on_a_drum_track_keeps_the_one_step_length) {
    Strata &app = fresh();
    open_keys();
    CHECK_EQ(UiProbe::selected(app), 0);                // track 1, KICK
    FakeHost &host = FakeHost::the();
    host.set_running(true);
    host.set_position(96);
    key(ENAGA_KEY_F2);
    step_down(4);
    pump(2);
    host.set_position(96 + 8 * 24);
    step_up(4);
    pump(2);
    int recorded = 0;
    for (int s = 0; s < kStepsPerTrack; s++) {
        if (trig_at(0, s) && step_of(0, s).note == app.keys_note(4)) {
            recorded++;
            CHECK_EQ(int(step_of(0, s).length), 8);
        }
    }
    CHECK(recorded > 0);
    key(ENAGA_KEY_F2);
    host.set_running(false);
}

TEST(ui_with_sus_up_a_track_pad_holds_its_note_until_it_comes_up) {
    // EDIT, ASR, SUS turned up, then the track's own pad: the note holds for as long as the
    // finger stays, as a KEYS pad does, and is let go with it. At SUS 0 the same pad is a hit
    // whose DEC runs on after the finger lifts, as it always was.
    Strata &app = fresh();
    strata::test::post(Cmd::scene_erase, 0, 0, uint8_t(app_engine().scene_slot(0)));
    strata::test::post(Cmd::scene_erase, 0, 0, uint8_t(app_engine().scene_slot(1)));
    pump();
    key(ENAGA_KEY_F8);
    while (UiProbe::param_page(app) != int(Page::asr)) key(ENAGA_KEY_SELECT_RIGHT);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_DOWN);    // track 7, VA POLY
    CHECK_EQ(UiProbe::selected(app), 6);
    const int pad = 12;                                 // track 7's pad
    const int second = 48000 / strata::test::kRigBlock;

    // SUS 0: a hit. Lifting the pad straight away does not end it -- DEC does.
    post_cmd_track_param(6, 8, 100);                    // DEC long
    pump();
    app.on_pad_press(pad, 100);
    pump(2);
    app.on_pad_release(pad);
    pump(second / 10);
    CHECK(pump(1) > 0.01f);                             // still ringing 0.1 s after the lift

    pump(second * 4);
    app.on_encoder(1, +127);                            // B: SUS up
    pump();
    CHECK(app_engine().sounding(6).v[kAsrFirst + 1] > 100);
    app.on_pad_press(pad, 100);
    pump(second);
    CHECK_EQ(static_cast<const VaPoly *>(app_engine().machine(6))->sounding_voices(), 1);
    const float early = pump(second / 4);
    pump(second * 2);
    const float late = pump(second / 4);                // three seconds held
    CHECK(late > 0.1f);
    CHECK(late > early * 0.8f);
    app.on_pad_release(pad);
    pump(second / 2);
    // Let go with the finger. The voice, not the output: the sends' tails ring on.
    CHECK_EQ(static_cast<const VaPoly *>(app_engine().machine(6))->sounding_voices(), 0);

    // Back to the old envelope for the tests after this one.
    post_cmd_track_param(6, uint8_t(kAsrFirst + 1), 0);
    post_cmd_track_param(6, 8, 64);
    pump();
}

// --- PLAY while the card is busy ------------------------------------------------------------

TEST(ui_play_is_refused_while_a_project_is_loading) {
    // docs/UNIT_CHECKLIST.md: "Press PLAY while a project is loading. It should refuse and say
    // LOADING, not start the transport over pattern memory that `step_load` is halfway through
    // rewriting." `step_load` reads one file per pass, so a load is in flight for ten passes --
    // which is the window this test sits in, and which the emulator cannot produce on purpose.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();

    menu_run(kMenuSave);                           // FUNC + HOME, OK: save, so there is
                                                   // something to read back
    CHECK_EQ(UiProbe::notice(app), "SAVING");
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);

    menu_run(kMenuReload);                         // RELOAD PROJECT, OK twice
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO RELOAD");
    key(ENAGA_KEY_OK);
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

TEST(ui_ok_in_resample_while_a_take_is_written_keeps_the_take_where_it_was_going) {
    // The take goes to the card a chunk per pass, so the write lasts seconds. OK pressed in that
    // time used to post a refused arm AND a write request, which retargeted the take in flight:
    // asked for TO FILE, it landed on the track now on screen, over that track's sample, and the
    // screen said ARMED.
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    key(ENAGA_KEY_STEP1);
    pump(2);
    host.set_running(true);
    shift_key(ENAGA_KEY_F2);                             // RESAMPLE
    CHECK_EQ(UiProbe::resample_track(app), -1);          // to the file only
    key(ENAGA_KEY_OK);
    pump();
    host.deliver_tick(384, 0);
    for (int i = 0; i < 2000; i++) {
        if (i % 8 == 0) host.deliver_tick(uint32_t(384 + i), 0);
        pump();
        if (app_storage().take_busy() && app_storage().busy_text() != nullptr) break;
    }
    CHECK(app_storage().take_busy());
    pump(2);                                             // a few chunks in
    CHECK(app_storage().take_busy());

    // Now point the screen at track 1 and press OK again.
    app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
    app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
    CHECK_EQ(UiProbe::resample_track(app), 0);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "TAKE STILL WRITING");
    CHECK(!app_storage().request_resample_write(0));     // and the card side refuses as well

    for (int i = 0; i < 4000 && app_storage().take_busy(); i++) pump();
    pump_until_idle();
    CHECK(!app_storage().take_busy());
    CHECK_EQ(app_engine().project().kit.tracks[0].sample[0], '\0');   // track 1 untouched
    CHECK(app_storage().last_error() == nullptr);
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 0);
    host.set_running(false);
    key(ENAGA_KEY_F4);
    pump(2);
}

TEST(ui_the_func_menu_never_offers_a_mutate_verdict_its_ok_would_not_give) {
    // In the FUNC menu OK runs the row under the cursor -- SAVE when it opens -- so a MUTATE made
    // there must not show "OK KEEPS CLR REVERTS".
    Strata &app = fresh();
    key(ENAGA_KEY_STEP1);
    pump(2);
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    pump(2);
    CHECK(UiProbe::mutation_pending(app));
    CHECK(!UiProbe::verdict_free(app));
    shift_key(ENAGA_KEY_F4);                             // close it
    pump(2);
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
    for (int guard = 0; guard < 16 && UiProbe::mixer_bank(app) != page; guard++) {
        app.on_key(ENAGA_KEY_SELECT_RIGHT, true);
        app.on_key(ENAGA_KEY_SELECT_RIGHT, false);
    }
    CHECK_EQ(UiProbe::mixer_bank(app), page);
}

// PROJECT mode's SETUP page, on the octave keys.
void setup_page(Strata &app, bool on) {
    app.on_key(ENAGA_KEY_F3, true);
    app.on_key(ENAGA_KEY_F3, false);
    for (int i = 0; i < 3; i++) {                   // past CHAIN and PATTERN, or back to the slots
        app.on_key(on ? ENAGA_KEY_SELECT_RIGHT : ENAGA_KEY_SELECT_LEFT, true);
        app.on_key(on ? ENAGA_KEY_SELECT_RIGHT : ENAGA_KEY_SELECT_LEFT, false);
    }
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

    // The selection keys page through: two pages of faders, two of duck depths and the duck's
    // own page, then DLY, DLY+, REV, REV+, ROUTE, MST and EQ.
    CHECK_EQ(kMixerPageCount, 12);
    for (int i = 1; i < kMixerPageCount; i++) {
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
    CHECK_EQ(UiProbe::mixer_bank(app), kMixerPageCount - 1);
}

// The master kill EQ (dsp/buses.hpp): its MIXER page, its knobs and its kill switches.

void press_key(Strata &app, int code) {
    app.on_key(code, true);
    app.on_key(code, false);
}

TEST(ui_the_eq_page_knobs_move_the_bands_and_a_push_kills_one) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    // fresh() does not reset the project's mixer bytes, so start from what is there.
    for (int b = 0; b < kKillBands; b++) post_cmd(Cmd::eq_kill, uint8_t(b), 0);
    pump(2);
    mixer_page(app, kMixerEqPage);

    // B is MID: two counts a click, like every master knob.
    const int before = int(engine.project().settings.master[int(MasterField::eq_mid)]);
    app.on_encoder(1, -5);
    pump(2);
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_mid)]), before - 10);
    CHECK(app_storage().dirty());
    app.on_encoder(1, +5);
    pump(2);

    // A plain push of A on this page throws LOW's kill switch, and a second push lets it go.
    press_key(app, ENAGA_KEY_PUSH_A);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0x1);
    CHECK_EQ(int(engine.master().kills), 0x1);
    CHECK(std::strcmp(UiProbe::notice(app), "KILL LOW") == 0);
    press_key(app, ENAGA_KEY_PUSH_A);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);
    CHECK(std::strcmp(UiProbe::notice(app), "LOW BACK") == 0);

    // D has no field on this page, and its push does nothing to the switches.
    press_key(app, ENAGA_KEY_PUSH_D);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);
}

// FUNC + push A/B/C is the kill switch from any mode, next to FUNC + VALUE's DJ filter; a plain
// push away from the EQ page is not, so nothing else that pushes a knob can kill the bass.
TEST(ui_func_and_a_knob_push_kill_a_band_from_any_mode) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    for (int b = 0; b < kKillBands; b++) post_cmd(Cmd::eq_kill, uint8_t(b), 0);
    pump(2);

    press_key(app, ENAGA_KEY_F4);                      // GRID
    press_key(app, ENAGA_KEY_PUSH_C);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);

    shift_key(ENAGA_KEY_PUSH_C);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0x4);
    CHECK(std::strcmp(UiProbe::notice(app), "KILL HI") == 0);

    // In PERFORM too, where A-D are the macros: the kill is a push, the macros are turns.
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    shift_key(ENAGA_KEY_PUSH_B);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0x6);
    shift_key(ENAGA_KEY_PUSH_C);
    shift_key(ENAGA_KEY_PUSH_B);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);
}

// The engine refuses a band or an operation it does not have, and counts it.
TEST(ui_an_eq_kill_for_a_band_that_does_not_exist_is_refused) {
    fresh();
    Engine &engine = app_engine();
    post_cmd(Cmd::eq_kill, uint8_t(kKillBands), 1);
    post_cmd(Cmd::eq_kill, 0, 3);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);
    post_cmd(Cmd::eq_kill, 1, 1);                    // set, then set again: still set
    post_cmd(Cmd::eq_kill, 1, 1);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0x2);
    post_cmd(Cmd::eq_kill, 1, 0);
    pump(2);
    CHECK_EQ(int(engine.project().settings.eq_kills), 0);
}

// In PERFORM, FUNC + STEP 5-7 puts the master EQ's LOW, MID or HI on the selected macro (1-4 are
// the LFO's); FUNC + STEP 8-16 assign nothing.
TEST(ui_func_and_step_5_to_7_put_a_master_eq_band_on_a_macro) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    // Whichever macro is selected, emptied first.
    const int selected = UiProbe::macro_selected(app);
    post_cmd(Cmd::macro_clear, uint8_t(selected));
    pump(2);

    app.on_key(ENAGA_KEY_F1, true);
    app.on_key(ENAGA_KEY_STEP1 + 5, true);              // STEP 6: MID
    app.on_key(ENAGA_KEY_STEP1 + 5, false);
    app.on_key(ENAGA_KEY_STEP1 + 9, true);              // nothing: not a band
    app.on_key(ENAGA_KEY_STEP1 + 9, false);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    const Macro &macro = engine.macros().macros[selected];
    CHECK_EQ(macro_count(macro), 1);
    CHECK_EQ(int(macro.targets[0].track), int(kMacroMasterTrack));
    CHECK_EQ(int(macro.targets[0].param), 1);            // MID
    CHECK_EQ(int(macro.targets[0].high), 0);             // from flat, towards a kill
    CHECK(std::strncmp(UiProbe::notice(app) + 1, " EQ MID ", 8) == 0);

    post_cmd(Cmd::macro_clear, uint8_t(selected));
    pump(2);
}

TEST(ui_a_knob_on_the_delay_page_changes_the_delay) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    mixer_page(app, kMixerDelayPage);  // DLY

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
    mixer_page(app, kMixerHallPage + 1);  // ROUTE

    // A is the ping-pong switch: either direction throws it, because a switch is not a knob.
    CHECK(engine.sends().delay_ping_pong);
    app.on_encoder(0, -1);
    pump(2);
    CHECK(!engine.sends().delay_ping_pong);
    app.on_encoder(0, +1);
    pump(2);
    CHECK(engine.sends().delay_ping_pong);

    // D is empty -- nineteen send fields do not divide into four. Turning it changes nothing
    // and, in particular, does not fall through to the track faders below.
    const uint32_t refused_before = engine.status().refused.load(std::memory_order_relaxed);
    const float level_before = engine.level(0);
    uint8_t sends_before[kSendFields];
    for (int i = 0; i < kSendFields; i++) sends_before[i] = engine.project().settings.sends[i];
    app.on_encoder(3, +5);
    pump(2);
    CHECK_NEAR(engine.level(0), level_before, 1e-6);
    CHECK_EQ(int(engine.status().refused.load(std::memory_order_relaxed) - refused_before), 0);
    for (int i = 0; i < kSendFields; i++) {
        CHECK_EQ(int(engine.project().settings.sends[i]), int(sends_before[i]));
    }
}

namespace {

// Does the drawn screen hold this text, as a whole string or inside one?
bool screen_has(Strata &app, const char *wanted) {
    FakeHost &host = FakeHost::the();
    enaga::Canvas canvas(g_pixels);
    host.clear_text();
    app.draw(canvas);
    for (int i = 0; i < host.text_count(); i++) {
        if (std::strstr(host.text_draw(i).text, wanted) != nullptr) return true;
    }
    return false;
}

}  // namespace

// The ambient modes' pages (dsp/ambient.hpp): DLY+ beside DLY, REV+ beside REV. Their TYP knob is
// a switch, MUL steps by one multiplier, and the delay's TIME line counts the multiplier in LONG.
TEST(ui_the_ambient_pages_switch_the_modes_and_reach_every_knob) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    mixer_page(app, kMixerLongPage);  // DLY+
    CHECK(!engine.sends().delay_long);
    app.on_encoder(0, +1);
    pump(2);
    CHECK(engine.sends().delay_long);
    CHECK(screen_has(app, "LONG"));

    // MUL: one click a multiplier, holding at both ends.
    CHECK_EQ(int(engine.sends().delay_multiplier), 2);
    app.on_encoder(1, +1);
    pump(2);
    CHECK_EQ(int(engine.sends().delay_multiplier), 3);
    app.on_encoder(1, +20);
    pump(2);
    CHECK_EQ(int(engine.sends().delay_multiplier), 8);
    CHECK(screen_has(app, "X8"));
    app.on_encoder(1, -20);
    pump(2);
    CHECK_EQ(int(engine.sends().delay_multiplier), 1);

    // Dotted 1/8 at the fresh project's 130 BPM is 0.346 s; X4 makes it 1.385 s, on the
    // screen too.
    app.on_encoder(1, +3);
    pump(2);
    CHECK_NEAR(effective_delay_seconds(engine.sends()), 4.0f * 0.75f * 60.0f / 130.0f, 1e-4);
    CHECK(screen_has(app, "TIME 1385 MS"));

    const float wow = engine.sends().delay_wow;
    app.on_encoder(2, +4);
    pump(2);
    CHECK(engine.sends().delay_wow > wow);
    const float diffusion = engine.sends().delay_diffusion;
    app.on_encoder(3, -4);
    pump(2);
    CHECK(engine.sends().delay_diffusion < diffusion);

    mixer_page(app, kMixerHallPage);  // REV+
    CHECK(!engine.sends().reverb_hall);
    app.on_encoder(0, +1);
    pump(2);
    CHECK(engine.sends().reverb_hall);
    CHECK(screen_has(app, "HALL"));
    const float size = engine.sends().reverb_size;
    app.on_encoder(1, -4);
    pump(2);
    CHECK(engine.sends().reverb_size < size);
    app.on_encoder(3, +10);
    pump(2);
    CHECK(engine.sends().reverb_shimmer > 0.0f);
    app.on_encoder(0, -1);
    pump(2);
    CHECK(!engine.sends().reverb_hall);
    CHECK(screen_has(app, "ROOM"));

    mixer_page(app, kMixerHallPage + 1);  // ROUTE: the LONG delay's low cut on C
    const float low_cut = engine.sends().delay_low_cut_hz;
    app.on_encoder(2, +6);
    pump(2);
    CHECK(engine.sends().delay_low_cut_hz > low_cut);

    // Push A on either page throws its switch too, both ways.
    mixer_page(app, kMixerLongPage);
    CHECK(engine.sends().delay_long);
    app.on_key(ENAGA_KEY_PUSH_A, true);
    app.on_key(ENAGA_KEY_PUSH_A, false);
    pump(2);
    CHECK(!engine.sends().delay_long);
    app.on_key(ENAGA_KEY_PUSH_A, true);
    app.on_key(ENAGA_KEY_PUSH_A, false);
    pump(2);
    CHECK(engine.sends().delay_long);
    mixer_page(app, kMixerHallPage);
    app.on_key(ENAGA_KEY_PUSH_A, true);
    app.on_key(ENAGA_KEY_PUSH_A, false);
    pump(2);
    CHECK(engine.sends().reverb_hall);
    app.on_key(ENAGA_KEY_PUSH_A, true);
    app.on_key(ENAGA_KEY_PUSH_A, false);
    pump(2);
    CHECK(!engine.sends().reverb_hall);

    // Back to classic for whoever comes next.
    mixer_page(app, kMixerLongPage);
    app.on_encoder(0, -1);
    pump(2);
    CHECK(!engine.sends().delay_long);
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

// The bottom line's MAX. The top line shows the LAST block's cost, which on the unit changes 750
// times a second and cannot be read (the first boot found that out). MAX holds the highest block
// since the transport last started, so it has to do two things: keep a peak against the quieter
// blocks after it, and forget it on PLAY so the figure belongs to what is playing now.
TEST(ui_the_held_cpu_max_keeps_the_highest_block_and_restarts_on_play) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    host.set_running(false);
    pump(2);

    host.set_audio_load(150);
    pump();
    host.set_audio_load(90);
    pump(3);
    CHECK_EQ(UiProbe::cpu_max(app), 150u);          // held against the later, cheaper blocks

    host.set_running(true);                         // PLAY: a fresh maximum, starting here
    host.set_audio_load(40);
    pump();
    CHECK_EQ(UiProbe::cpu_max(app), 40u);

    host.set_audio_load(260);
    pump();
    host.set_audio_load(30);
    pump(5);
    CHECK_EQ(UiProbe::cpu_max(app), 260u);          // still running: held

    host.set_running(false);                        // a stop alone does not clear it ...
    pump();
    CHECK_EQ(UiProbe::cpu_max(app), 260u);
    host.set_running(true);                         // ... the next start does
    host.set_audio_load(10);
    pump();
    CHECK_EQ(UiProbe::cpu_max(app), 10u);

    enaga::Canvas canvas(g_pixels);
    app.draw(canvas);                               // the bottom line with a figure in it

    host.set_running(false);
    host.set_audio_load(0);
    pump();
}

// The `ld <avg> <max>` log lines: one per BAR, averaged and maxed, in ONE burst of four after a
// track freezes or comes back, while the transport runs, skipping the part bar the change lands
// in and one whole bar after it. Not after PLAY or a pattern change: Enaga's log ring keeps
// sixteen lines, and most scenarios check lines from early in the run (a burst after PLAY broke
// six of them). tests/scenario_freeze.sh reads these.
TEST(ui_the_load_log_gives_four_bars_after_a_freeze_or_unfreeze) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    Status &status = const_cast<Status &>(UiProbe::engine(app).status());
    const auto at_bar = [&](uint32_t bar) { host.set_position(bar * kTicksPerBar + 5); };
    host.set_running(true);
    host.set_audio_load(300);
    at_bar(0);
    pump(50);
    at_bar(1);
    pump(50);
    CHECK(!host.logged("ld 30.0 30.0"));            // PLAY alone logs nothing

    status.frozen.store(64u, std::memory_order_relaxed);   // track 7 freezes in bar 1
    pump(10);
    host.set_audio_load(650);                       // bar 2 is skipped whatever it costs
    at_bar(2);
    pump(50);
    at_bar(3);
    host.set_audio_load(200);
    pump(99);
    host.set_audio_load(500);
    pump();
    host.set_audio_load(100);
    at_bar(4);
    pump();
    CHECK(host.logged("ld 20.3 50.0"));             // bar 3: (99 x 200 + 500) / 100 = 203 per mille
    CHECK(!host.logged("ld 65.0 65.0"));
    pump(49);
    for (uint32_t bar = 5; bar <= 7; bar++) {
        at_bar(bar);
        pump(50);
    }
    CHECK(host.logged("ld 10.0 10.0"));             // bars 4-6
    const int lines = host.log_lines();
    at_bar(8);
    pump(50);
    at_bar(9);
    pump(50);
    CHECK_EQ(host.log_lines(), lines);              // four bars, then quiet

    status.frozen.store(0u, std::memory_order_relaxed);    // and back: a new burst
    host.set_audio_load(150);
    for (uint32_t bar = 10; bar <= 13; bar++) {     // 9 part and 10 skipped, 11 logged at 12
        at_bar(bar);
        pump(50);
    }
    CHECK(host.logged("ld 15.0 15.0"));
    host.set_running(false);                        // a stop ends the burst
    host.set_audio_load(70);
    at_bar(14);
    pump(50);
    at_bar(15);
    pump(50);
    CHECK(!host.logged("ld 7.0 7.0"));
    host.set_audio_load(0);
    host.set_position(0);
    pump(2);
}

// The CPU guard (core/cpuguard.hpp) says so on the bottom line, in the device's place, so a chord
// that got thinner under load has a reason on screen. A dropped edit still wins the slot.
TEST(ui_the_bottom_line_shows_the_cpu_guard_while_it_is_up) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    CHECK(!screen_has(app, "GUARD"));
    host.set_audio_load(950);
    for (int i = 0; i < 40 && app_engine().cpu_guard_level() == 0; i++) pump();
    host.set_audio_load(600);
    CHECK_EQ(app_engine().cpu_guard_level(), 1);
    CHECK(screen_has(app, "GUARD1"));

    post_cmd(Cmd::stop, 1);                         // a hard stop starts the guard over
    pump();
    host.set_audio_load(0);
    CHECK(!screen_has(app, "GUARD"));
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
// The narrowest blank allowed between two strings on one line. Letters inside a string are 1 px
// apart; EDIT's "A" beside "TUN" is 5. 3 px read as one word on PERFORM ("3 PRM3 PRM4 PRM").
constexpr int kMinGap = 4;

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
            const Box o = box_of(other);
            if (overlap(b, o)) {
                char what[96];
                std::snprintf(what, sizeof what, "crosses \"%s\" at (%d, %d)", other.text, other.x,
                              other.y);
                report(d, what);
            } else if (b.y0 <= o.y1 && o.y0 <= b.y1) {
                // Two strings on the same line must not RUN TOGETHER. Inside a string the
                // glyphs are 1 px apart; two strings a couple of pixels apart read as one word
                // ("3 PRM3 PRM4 PRM" on PERFORM). So the blank between them must be at least
                // kMinGap pixels, which is clearly wider than the gap between letters.
                const int gap = b.x0 > o.x1 ? b.x0 - o.x1 - 1 : o.x0 - b.x1 - 1;
                if (gap < kMinGap) {
                    char what[96];
                    std::snprintf(what, sizeof what, "runs into \"%s\" at (%d, %d), %d px apart",
                                  other.text, other.x, other.y, gap);
                    report(d, what);
                }
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
        for (int page = 0; page < kEditPages; page++) {
            check_layout(app, machine_name(uint8_t(machine)));
            key(ENAGA_KEY_SELECT_RIGHT);
        }
    }
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    key(ENAGA_KEY_F4);
}

// Every way the EQ page can read -- a switched band, a knob at the bottom, at the top, flat --
// fits the screen.
TEST(ui_layout_the_eq_page_at_its_extremes) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    mixer_page(app, kMixerEqPage);
    post_cmd(Cmd::eq_kill, 0, 1);
    for (int i = 0; i < 40; i++) app.on_encoder(1, -5);
    for (int i = 0; i < 40; i++) app.on_encoder(2, +5);
    pump(4);
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_mid)]), 0);
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_high)]), 127);
    check_layout(app, "MIXER EQ, killed and at the ends");
    // And back to flat, so later tests start bypassed.
    post_cmd(Cmd::eq_kill, 0, 0);
    for (int i = 0; i < 32; i++) app.on_encoder(1, +1);     // 0 -> 64, two counts a click
    pump(4);
    post_cmd(Cmd::set_master, uint8_t(MasterField::eq_high), uint8_t(int8_t(-63)));
    pump(4);
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_mid)]), 64);
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_high)]), 64);
    check_layout(app, "MIXER EQ, flat");
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

    // The live line at its widest: fill, auto-fill 16, a pitched 1/16T repeat, a delay throw and
    // a freeze, with note repeat on the bar line.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(1, 100);                   // FUNC + FILL: NOTE RPT
    app.on_pad_release(1);
    app.on_pad_press(3, 100);                   // REPEAT, pitched down
    app.on_encoder(4, +10);                     // to the last length, 1/16T
    app.on_pad_press(13, 100);                  // FUNC + MUTATE: freeze
    app.on_key(ENAGA_KEY_F1, false);
    app.on_pad_press(1, 100);                   // FILL
    app.on_pad_press(11, 100);                  // THROW, delay
    post_cmd(Cmd::set_auto_fill, uint8_t(AutoFill::every_16));
    pump(2);
    CHECK(app_engine().delay_freeze().held());
    check_layout(app, "PERFORM, everything live");
    app.on_pad_release(11);
    app.on_pad_release(1);
    app.on_pad_release(13);
    app.on_pad_release(3);
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(1, 100);                   // NOTE RPT off again
    app.on_pad_release(1);
    app.on_key(ENAGA_KEY_F1, false);
    post_cmd(Cmd::set_auto_fill, uint8_t(AutoFill::off));
    pump(2);

    open_keys();
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));
    for (int scale = 0; scale < kScaleCount; scale++) {
        post_cmd(Cmd::set_scale, uint8_t(scale));
        pump(2);
        check_layout(app, scale_name(scale));
    }

    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    check_layout(app, "MACHINE");

    // And the kit browser behind it, on both pages, with every slot state on screen: a kit, an
    // empty slot, an unreadable one and one not yet read.
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::kit));
    check_layout(app, "KIT, reading");
    pump_until_idle();
    app_storage().request_kit_save(0);
    for (int i = 0; i < 4000 && app_storage().kit_slot(0).state != Storage::SlotState::used; i++) {
        pump();
    }
    CHECK_EQ(int(app_storage().kit_slot(0).state), int(Storage::SlotState::used));
    const char junk[] = "JUNK";
    FakeHost::the().write_file("/ENAGA/DATA/STRATA/KITS/KIT_02.BIN", junk, sizeof junk);
    app_storage().request_kit_scan();
    pump_until_idle();
    pump(400);                                  // past the notice
    CHECK_EQ(int(app_storage().kit_slot(1).state), int(Storage::SlotState::bad));
    check_layout(app, "KIT");
    key(ENAGA_KEY_DOWN);
    check_layout(app, "KIT, an unreadable slot");
    key(ENAGA_KEY_RIGHT);
    check_layout(app, "KIT, slots 9-16");
    key(ENAGA_KEY_OK);
    key(ENAGA_KEY_LEFT);
    key(ENAGA_KEY_OK);
    pump(4);
    check_layout(app, "KIT, after a refused load");
    FakeHost::the().set_card(false);
    app_storage().on_card(false);
    check_layout(app, "KIT, no card");
    FakeHost::the().set_card(true);
    app_storage().on_card(true);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));

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

TEST(ui_layout_chain_and_pattern_pages) {
    Strata &app = fresh();
    worst_case_content(app);
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 1);
    check_layout(app, "CHAIN, empty");
    // A full chain of the widest entries, with the cursor at the top, the middle and the end.
    for (int i = 0; i < kChainEntries; i++) {
        app_engine().project_for_load().chain[i] = ChainEntry{7, 15, 16, 8};
    }
    app_engine().project_for_load().settings.chain_length = uint8_t(kChainEntries);
    app_engine().project_for_load().settings.chain_song = true;
    post_cmd(Cmd::set_chain, 1);
    pump(2);
    check_layout(app, "CHAIN, full");
    for (int i = 0; i < 40; i++) key(ENAGA_KEY_DOWN);
    check_layout(app, "CHAIN, cursor in the middle");
    for (int i = 0; i < 40; i++) key(ENAGA_KEY_DOWN);
    check_layout(app, "CHAIN, cursor at the end");
    post_cmd(Cmd::set_chain, 0);
    for (int i = 0; i < kChainEntries; i++) app_engine().project_for_load().chain[i] = ChainEntry{};
    app_engine().project_for_load().settings.chain_length = 0;
    app_engine().project_for_load().settings.chain_song = false;
    pump(2);

    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 2);
    check_layout(app, "PATTERN");
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::project_page(app), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_chain_page_sets_mute_recalls_and_song_mode) {
    Strata &app = fresh();
    const Project &project = app_engine().project();
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 1);
    key(ENAGA_KEY_OK);                                // one entry: the playing pattern
    pump(2);
    CHECK_EQ(chain_length(project), 1);

    // B walks the entry's mute recall, --, M1 ... M8.
    app.on_encoder(1, +3);
    pump(2);
    CHECK_EQ(int(project.chain[0].mutes), 3);
    // OK copies the entry with its recall; a STEP key keeps it.
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(int(project.chain[1].mutes), 3);
    key(ENAGA_KEY_STEP1 + 4);
    pump(2);
    CHECK_EQ(int(project.chain[1].slot), 4);
    CHECK_EQ(int(project.chain[1].mutes), 3);
    check_layout(app, "CHAIN, with mute recalls");

    // The FUNC menu's CHAIN END row switches SONG and LOOP, and CLR comes back to CHAIN.
    CHECK(!project.settings.chain_song);
    menu_run(kMenuSong);
    pump(2);
    CHECK(project.settings.chain_song);
    CHECK_EQ(UiProbe::notice(app), "SONG: PLAY ONCE");
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(!project.settings.chain_song);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));
    CHECK_EQ(UiProbe::project_page(app), 1);

    // The end of a song stops the transport, from this side.
    menu_toggle(kMenuSong);
    pump(2);
    key(ENAGA_KEY_F5);
    CHECK(enaga::clock::running());
    const_cast<Status &>(app_engine().status()).bump_song_end();
    pump(2);
    CHECK(!enaga::clock::running());
    CHECK_EQ(UiProbe::notice(app), "SONG END");

    // CLR removes an entry only on a second press (Roland, 2026-11-22): not undoable.
    menu_toggle(kMenuSong);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(chain_length(project), 2);
    CHECK_EQ(UiProbe::notice(app), "AGAIN TO REMOVE");
    // Too late: past two seconds the first press is forgotten and this one only arms again.
    pump(2100);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(chain_length(project), 2);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(chain_length(project), 1);
    CHECK_EQ(UiProbe::notice(app), "ENTRY REMOVED");
    // Back as it was, for the tests after this one.
    for (int i = 0; i < 2; i++) {
        key(ENAGA_KEY_CLR);
        pump(2);
    }
    CHECK_EQ(chain_length(project), 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_chain_page_builds_a_chain_that_plays) {
    // docs/REVIEW.md: no gesture wrote a chain entry, so CHAIN ON always refused.
    Strata &app = fresh();
    const Project &project = app_engine().project();
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 1);
    CHECK_EQ(chain_length(project), 0);

    // OK on an empty chain adds the playing pattern; a STEP key then sets the entry's slot.
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(chain_length(project), 1);
    key(ENAGA_KEY_STEP1 + 2);                         // A03
    pump(2);
    CHECK_EQ(int(project.chain[0].slot), 2);

    // VALUE sets the repeats, in one event of several clicks, clamped to 1-16.
    app.on_encoder(4, +3);
    pump(2);
    CHECK_EQ(int(project.chain[0].repeats), 4);
    app.on_encoder(4, -10);
    pump(2);
    CHECK_EQ(int(project.chain[0].repeats), 1);

    // OK copies the entry after the cursor and moves onto it; RIGHT steps its pattern.
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(chain_length(project), 2);
    CHECK_EQ(UiProbe::chain_cursor(app), 1);
    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_RIGHT);
    pump(2);
    CHECK_EQ(int(project.chain[1].slot), 4);          // A05
    CHECK_EQ(int(project.chain[0].slot), 2);

    // UP goes back to the first; CLR twice removes it, and the second moves up.
    key(ENAGA_KEY_UP);
    CHECK_EQ(UiProbe::chain_cursor(app), 0);
    key(ENAGA_KEY_CLR);                               // the first press only arms it
    pump(2);
    CHECK_EQ(chain_length(project), 2);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(chain_length(project), 1);
    CHECK_EQ(int(project.chain[0].slot), 4);
    key(ENAGA_KEY_OK);                                // and back to two: A05, A05
    pump(2);

    // The FUNC menu's CHAIN row turns it on, which now works, and it is saved as a project edit.
    menu_toggle(kMenuChain);
    pump(2);
    CHECK(app_engine().chain().active);
    CHECK_EQ(int(app_engine().status().live_slot.load(std::memory_order_relaxed)), 4);
    CHECK(app_storage().dirty());

    // Removing every entry turns it off rather than leaving it following nothing.
    for (int i = 0; i < 2; i++) {
        key(ENAGA_KEY_CLR);
        key(ENAGA_KEY_CLR);
        pump(2);
    }
    CHECK_EQ(chain_length(project), 0);
    CHECK(!app_engine().chain().active);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::project_page(app), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_pattern_page_sets_the_master_length_and_the_swing) {
    Strata &app = fresh();
    const Pattern &pattern = app_engine().pattern();
    const int length = int(pattern.master_length);
    const int swing = int(pattern.swing);
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 2);
    CHECK_EQ(UiProbe::pattern_field(app), 0);

    app.on_encoder(4, -4);                            // VALUE: LEN
    pump(2);
    CHECK_EQ(int(pattern.master_length), length - 4);
    key(ENAGA_KEY_UP);                                // UP: one more
    pump(2);
    CHECK_EQ(int(pattern.master_length), length - 3);

    key(ENAGA_KEY_RIGHT);                             // onto SWG
    CHECK_EQ(UiProbe::pattern_field(app), 1);
    app.on_encoder(4, +3);
    pump(2);
    CHECK_EQ(int(pattern.swing), swing + 3);
    app.on_encoder(0, +100);                          // A: LEN again, clamped at 64
    pump(2);
    CHECK_EQ(int(pattern.master_length), 64);
    CHECK_EQ(UiProbe::pattern_field(app), 0);

    // A pattern edit, so undoable.
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(int(pattern.master_length) < 64);
}

TEST(ui_the_pattern_page_sets_each_tracks_own_swing) {
    Strata &app = fresh();
    const Pattern &pattern = app_engine().pattern();
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 2);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(pattern.tracks[t].swing), int(kSwingInherit));

    // RIGHT walks LEN, SWG, then the tracks; it stops at track 8.
    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_RIGHT);                             // track 2
    CHECK_EQ(UiProbe::pattern_field(app), 3);
    key(ENAGA_KEY_UP);                                // PAT -> 50
    pump(2);
    CHECK_EQ(int(pattern.tracks[1].swing), kSwingMin);
    app.on_encoder(4, +10);                           // VALUE: 60
    pump(2);
    CHECK_EQ(int(pattern.tracks[1].swing), kSwingMin + 10);
    CHECK_EQ(int(pattern.tracks[0].swing), int(kSwingInherit));
    check_layout(app, "PATTERN, a track swing set");
    for (int i = 0; i < 20; i++) key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::pattern_field(app), 9);
    check_layout(app, "PATTERN, cursor on track 8");

    // Encoder C edits the track under the cursor.
    app.on_encoder(2, +5);
    pump(2);
    CHECK_EQ(int(pattern.tracks[7].swing), kSwingMin + 4);

    // From LEN, C jumps to the selected track's swing (track 1 after fresh()).
    for (int i = 0; i < 20; i++) key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::pattern_field(app), 0);
    app.on_encoder(2, +1);
    pump(2);
    CHECK_EQ(UiProbe::pattern_field(app), 2);
    CHECK_EQ(int(pattern.tracks[0].swing), kSwingMin);
    // And down again to PAT.
    app.on_encoder(2, -3);
    pump(2);
    CHECK_EQ(int(pattern.tracks[0].swing), int(kSwingInherit));

    // A pattern edit, so undoable, one level per sweep: the last (track 1, up then back to PAT)
    // is one level, and the one before it is track 8's.
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(int(pattern.tracks[0].swing), int(kSwingInherit));
    CHECK_EQ(int(pattern.tracks[7].swing), kSwingMin + 4);
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(int(pattern.tracks[7].swing), int(kSwingInherit));
    CHECK_EQ(int(pattern.tracks[1].swing), kSwingMin + 10);
}

TEST(ui_undo_and_redo_work_in_project_and_the_card_actions_are_in_the_menu) {
    // F1 + CLR and F1 + OK were NEW PROJECT and RELOAD here, so undo and redo were the one pair
    // of keys that changed meaning in one mode. Now they are undo and redo everywhere, and the
    // card's two whole-project actions are FUNC menu rows, each needing OK twice
    // (docs/CONTROLS.md).
    Strata &app = fresh();
    const Pattern &pattern = app_engine().pattern();
    const int length = int(pattern.master_length);
    key(ENAGA_KEY_F3);
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);                      // PATTERN
    app.on_encoder(0, -2);
    pump(2);
    CHECK_EQ(int(pattern.master_length), length - 2);

    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK_EQ(int(pattern.master_length), length);
    CHECK_EQ(UiProbe::notice(app), "UNDO");
    CHECK(!app_storage().loading_project());
    shift_key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(int(pattern.master_length), length - 2);
    CHECK_EQ(UiProbe::notice(app), "REDO");

    // FUNC + arrows do nothing on the slots now.
    key(ENAGA_KEY_CLR);                               // back to the slots
    shift_key(ENAGA_KEY_RIGHT);
    shift_key(ENAGA_KEY_LEFT);
    pump(2);
    CHECK(!app_storage().loading_project());
    CHECK_EQ(int(pattern.master_length), length - 2);

    // One OK on NEW PROJECT only arms; a stray press does not throw the project away.
    menu_run(kMenuNew);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN FOR NEW");
    CHECK_EQ(int(pattern.master_length), length - 2);
    // Moving to another row disarms it: NEW, RELOAD, NEW is three first presses.
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO RELOAD");
    key(ENAGA_KEY_UP);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN FOR NEW");
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "NEW PROJECT");
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));  // the menu closed, back on PROJECT
    CHECK(pump_until_idle());
    pump(4);
    // The factory project's first pattern (core/factory.hpp), not the edited one.
    CHECK_EQ(int(pattern.master_length), int(strata::test::factory().patterns[0][0].master_length));
    key(ENAGA_KEY_F4);
}

TEST(ui_clear_pattern_in_the_menu_empties_every_track_in_one_undo_level) {
    // Roland, 2026-11-23: clearing a pattern took CLR once per track. FUNC menu, CLEAR PATTERN,
    // OK twice empties every track's trigs and locks; the kit and each track's layout stay, and
    // FUNC + CLR brings it all back in one go.
    Strata &app = fresh();
    const Pattern &pattern = app_engine().pattern();

    // Trigs on tracks 1 and 3 and a lock on track 1.
    key(ENAGA_KEY_STEP1 + 0);
    key(ENAGA_KEY_STEP1 + 4);
    step_down(2);
    const int param = encoder_param(0);
    const uint8_t kit_before = app_engine().params(0).v[param];
    app.on_encoder(0, +3);
    step_up(2);
    app.on_pad_press(4, 100);                         // track 3
    app.on_pad_release(4);
    key(ENAGA_KEY_STEP1 + 8);
    pump(2);
    CHECK_EQ(UiProbe::selected(app), 2);
    CHECK(trig_at(0, 0));
    CHECK(trig_at(0, 4));
    CHECK_EQ(locks_on_track(0), 1);
    CHECK(trig_at(2, 8));
    CHECK(pattern.locks_used > 0);
    const int length_3 = int(pattern.tracks[2].length);
    const int depth = int(undo_depth());

    // One OK only arms, and says so.
    menu_run(kMenuClear);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO CLEAR");
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    CHECK(trig_at(0, 0));

    // The second clears every track and closes the menu.
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "CLEARED FUNC+CLR UNDO");
    CHECK(UiProbe::mode(app) != int(Mode::menu));
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kStepsPerTrack; s++) {
            CHECK_EQ(int(step_of(t, s).flags & (kStepTrig | kStepTrigless)), 0);
            CHECK_EQ(int(step_of(t, s).lock_count), 0);
        }
    }
    CHECK_EQ(int(pattern.locks_used), 0);
    CHECK_EQ(int(pattern.tracks[2].length), length_3);           // the layout stays
    CHECK_EQ(int(app_engine().params(0).v[param]), int(kit_before));   // and the sound
    CHECK_EQ(int(undo_depth()), depth + 1);                      // one level

    // FUNC + CLR: everything back at once.
    shift_key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(trig_at(0, 0));
    CHECK(trig_at(0, 4));
    CHECK_EQ(locks_on_track(0), 1);
    CHECK(trig_at(2, 8));

    // A first OK that is not followed within the arm time does nothing.
    menu_run(kMenuClear);
    for (int i = 0; i < 2100; i++) pump();
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO CLEAR");
    CHECK(trig_at(0, 0));
    key(ENAGA_KEY_CLR);                               // close the menu
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

TEST(ui_the_lfo_is_edits_fifth_page) {
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                // EDIT, track 1
    while (UiProbe::param_page(app) != int(Page::lfo)) key(ENAGA_KEY_SELECT_RIGHT);
    check_layout(app, "EDIT LFO, off");

    // One click is one shape on A and one destination on D.
    app.on_encoder(0, +1);
    pump();
    CHECK_EQ(int(lfo_wave(app_engine().params(0).v[kLfoWave])), int(LfoWave::triangle));
    app.on_encoder(3, +3);
    pump();
    CHECK_EQ(lfo_dest(app_engine().params(0).v[kLfoDest]), 2);
    CHECK(!app_engine().lfo_on(0));                   // no depth yet
    app.on_encoder(2, +50);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[kLfoDepth]), 50);
    CHECK(app_engine().lfo_on(0));
    check_layout(app, "EDIT LFO, running");

    // The widest words the page can show: a free rate, every synced length, every destination.
    for (int r = 0; r < 128; r += 3) {
        post_cmd_track_param(0, kLfoRate, uint8_t(r));
        pump();
        check_layout(app, "EDIT LFO, rate");
    }
    for (int d = 0; d <= kParams; d++) {
        post_cmd_track_param(0, kLfoDest, lfo_dest_byte(d - 1));
        pump();
        check_layout(app, "EDIT LFO, destination");
    }

    // Hold a STEP and turn C: a lock on the LFO's depth, removed by pushing C.
    key(ENAGA_KEY_F4);                                // GRID; the page stays LFO
    step_down(5);
    app.on_encoder(2, -10);
    pump();
    uint8_t value = 0;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, 5, uint8_t(kLfoDepth), value));
    CHECK_EQ(int(value), 40);
    app.on_key(ENAGA_KEY_PUSH_C, true);
    app.on_key(ENAGA_KEY_PUSH_C, false);
    step_up(5);
    pump();
    CHECK(!pattern_get_lock(app_engine().pattern(), 0, 5, uint8_t(kLfoDepth), value));

    // PERFORM: FUNC + STEP 3 hands the LFO's depth to the selected macro.
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    shift_key(ENAGA_KEY_STEP1 + 2);
    pump();
    const MacroTarget &target = app_engine().macros().macros[0].targets[0];
    CHECK_EQ(int(target.enabled), 1);
    CHECK_EQ(int(target.param), kLfoDepth);
    CHECK_EQ(UiProbe::notice(app), "A T1 DEP 1/4");
    post_cmd(Cmd::macro_clear, 0);
    pump();

    // Back to SRC for the tests after this one, and the LFO off.
    post_cmd_track_param(0, kLfoDepth, 0);
    post_cmd_track_param(0, kLfoDest, 0);
    pump();
    key(ENAGA_KEY_F8);
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_arp_is_edits_sixth_page_and_keys_play_it) {
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                // EDIT, track 1
    while (UiProbe::param_page(app) != int(Page::arp)) key(ENAGA_KEY_SELECT_RIGHT);
    check_layout(app, "EDIT ARP, off");

    // One click is one zone on A, B and C: MOD UP, RAT one faster, OCT 2.
    app.on_encoder(0, +1);
    app.on_encoder(1, +1);
    app.on_encoder(2, +1);
    pump();
    const TrackParams &p = app_engine().params(0);
    CHECK_EQ(arp_mode_index(p.v[kArpFirst + 0]), int(ArpMode::up));
    CHECK_EQ(arp_rate_index(p.v[kArpFirst + 1]), 1);
    CHECK_EQ(arp_octaves(p.v[kArpFirst + 2]), 2);
    check_layout(app, "EDIT ARP, on");

    // The widest words the page can show: every mode, rate, octave count and gate.
    for (int b = 0; b < 128; b += 3) {
        for (int i = 0; i < kArpParams; i++) post_cmd_track_param(0, uint8_t(kArpFirst + i), uint8_t(b));
        pump();
        check_layout(app, "EDIT ARP, every value");
    }

    // KEYS: a key held on a track whose arp is on starts it; the key coming up stops it.
    post_cmd_track_param(0, uint8_t(kArpFirst + 0), 22);      // UP
    pump();
    open_keys();
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));
    app.on_key(ENAGA_KEY_STEP1, true);
    pump(2);
    CHECK(app_engine().arp_running(0));
    app.on_key(ENAGA_KEY_STEP1, false);
    pump(2);
    CHECK(!app_engine().arp_running(0));

    // Off again, and back to SRC for the tests after this one.
    for (int i = 0; i < kArpParams; i++) post_cmd_track_param(0, uint8_t(kArpFirst + i), 0);
    pump();
    key(ENAGA_KEY_F8);
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_F4);
}

TEST(ui_asr_is_edits_seventh_page_and_speaks_for_the_vas_only) {
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                // EDIT, track 1: a drum
    while (UiProbe::param_page(app) != int(Page::asr)) key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(int(Page::asr), kEditPages - 2);
    check_layout(app, "EDIT ASR, a drum");
    CHECK(screen_has(app, "VA MONO/POLY ONLY"));
    CHECK(!screen_has(app, "ATK"));

    // Track 7, VA POLY in the default kit: the four names, the old envelope's values at 0.
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::selected(app), 6);
    CHECK_EQ(int(app_engine().machine_id(6)), int(MachineId::va_poly));
    check_layout(app, "EDIT ASR, VA POLY");
    CHECK(screen_has(app, "ATK"));
    CHECK(screen_has(app, "FSU"));
    CHECK(screen_has(app, "3MS"));                    // 2.5 ms, as the VAs always had
    CHECK(screen_has(app, "60MS"));
    CHECK(screen_has(app, "SUS 0: ONE-SHOT"));

    // B is SUS: up, the note holds.
    app.on_encoder(1, +100);
    pump();
    CHECK(app_engine().params(6).v[kAsrFirst + 1] > 0);
    CHECK(screen_has(app, "HOLDS: PAD OR LEN"));

    // The widest values the page shows: every time and percentage fits.
    for (int b = 0; b < 128; b += 3) {
        for (int i = 0; i < kAsrParams; i++) post_cmd_track_param(6, uint8_t(kAsrFirst + i), uint8_t(b));
        pump();
        check_layout(app, "EDIT ASR, every value");
    }
    post_cmd_track_param(6, uint8_t(kAsrFirst + 0), 127);
    post_cmd_track_param(6, uint8_t(kAsrFirst + 2), 127);
    pump();
    CHECK(screen_has(app, "5.0S"));
    CHECK(screen_has(app, "8.0S"));

    // Back to the old envelope and SRC for the tests after this one.
    for (int i = 0; i < kAsrParams; i++) post_cmd_track_param(6, uint8_t(kAsrFirst + i), 0);
    pump();
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_UP);
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_F4);
}

TEST(ui_osc_is_edits_eighth_page_and_names_its_waves_and_modes) {
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                // EDIT, track 1: a drum
    while (UiProbe::param_page(app) != int(Page::osc)) key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(int(Page::osc), kEditPages - 1);
    check_layout(app, "EDIT OSC, a drum");
    CHECK(screen_has(app, "VA MONO/POLY ONLY"));
    CHECK(!screen_has(app, "WAV"));

    // Track 6, VA MONO in the default kit: the four names and what each value means.
    for (int i = 0; i < 5; i++) key(ENAGA_KEY_DOWN);
    CHECK_EQ(int(app_engine().machine_id(5)), int(MachineId::va_mono));
    const TrackParams original = app_engine().params(5);
    check_layout(app, "EDIT OSC, VA MONO");
    CHECK(screen_has(app, "WAV"));
    CHECK(screen_has(app, "OS2"));
    CHECK(screen_has(app, "SUB"));
    CHECK(screen_has(app, "DRV"));
    post_cmd_track_param(5, uint8_t(kOscFirst + 0), 0);
    post_cmd_track_param(5, uint8_t(kOscFirst + 1), 0);
    pump();
    CHECK(screen_has(app, "SAW"));
    CHECK(screen_has(app, "OFF"));
    post_cmd_track_param(5, uint8_t(kOscFirst + 0), 32);    // the square
    post_cmd_track_param(5, uint8_t(kOscFirst + 1), 30);    // a detune: D14
    pump();
    CHECK(screen_has(app, "SQR"));
    CHECK(screen_has(app, "D14"));
    post_cmd_track_param(5, uint8_t(kOscFirst + 0), 127);   // the thinnest pulse
    post_cmd_track_param(5, uint8_t(kOscFirst + 1), 127);   // sync two octaves up
    pump();
    CHECK(screen_has(app, "P8"));
    CHECK(screen_has(app, "S+24"));

    // Every value of every knob fits.
    for (int b = 0; b < 128; b++) {
        for (int i = 0; i < kOscParams; i++) post_cmd_track_param(5, uint8_t(kOscFirst + i), uint8_t(b));
        pump();
        check_layout(app, "EDIT OSC, every value");
    }

    // The factory bass's page again, and SRC for the tests after this one.
    for (int i = 0; i < kOscParams; i++) {
        post_cmd_track_param(5, uint8_t(kOscFirst + i), original.v[kOscFirst + i]);
    }
    pump();
    for (int i = 0; i < 5; i++) key(ENAGA_KEY_UP);
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_F4);
}

TEST(ui_arrows_in_edit_keys_perform_machine_and_resample) {
    Strata &app = fresh();

    key(ENAGA_KEY_F8);                             // EDIT: sideways is the parameter page
    const int param_page = UiProbe::param_page(app);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::param_page(app), (param_page + 1) % kEditPages);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::param_page(app), param_page);
    key(ENAGA_KEY_DOWN);                           // and the rows are the tracks
    CHECK_EQ(UiProbe::selected(app), 1);
    key(ENAGA_KEY_UP);

    open_keys();                                   // KEYS: sideways is the octave
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
    // Sideways is the next column of the machine grid, three down, and never the track:
    // Roland turned RIGHT for the next machine and changed track 7 (2026-11-23).
    key(ENAGA_KEY_RIGHT);
    pump(4);
    CHECK_EQ(int(app_engine().machine_id(0)), (machine + 3) % int(MachineId::count));
    CHECK_EQ(UiProbe::selected(app), 0);
    key(ENAGA_KEY_LEFT);
    pump(4);
    CHECK_EQ(int(app_engine().machine_id(0)), machine);
    // The track is the screen keys', and says so.
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::selected(app), 1);
    CHECK_EQ(UiProbe::notice(app), "TRACK 2");
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::selected(app), 0);

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

TEST(ui_func_home_opens_the_menu_and_ok_saves) {
    // docs/CONTROLS.md: FUNC + HOME (printed "system") is the FUNC menu, and its cursor starts on
    // SAVE, so FUNC + HOME, OK is a save -- two presses, as the old FUNC + PROJECT twice was.
    Strata &app = fresh();
    key(ENAGA_KEY_F7);                              // from MIXER, to see it come back there
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuSave));
    pump(4);
    CHECK(app_storage().busy_text() == nullptr);   // opening it is not a save

    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "SAVING");
    CHECK_EQ(UiProbe::mode(app), int(Mode::mixer)); // back where it opened
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(!app_storage().dirty());

    // FUNC + PROJECT is the sample browser now ("pool"), and saves nothing.
    shift_key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    pump_until_idle();                              // let its listing finish before the next test
    key(ENAGA_KEY_F4);
}

// --- the kit browser (FUNC+EDIT twice) -------------------------------------------------------

TEST(ui_f1_edit_twice_opens_the_kit_browser_and_clr_goes_back) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::kit));
    CHECK_EQ(UiProbe::notice(app), "KITS");
    // Opening it reads the card, so the slots say what is in them.
    CHECK(pump_until_idle());
    CHECK_EQ(int(app_storage().kit_slot(0).state), int(Storage::SlotState::empty));

    shift_key(ENAGA_KEY_F8);                       // and again: back to the machines
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::kit));
    key(ENAGA_KEY_CLR);                            // CLR goes back, as on every Enaga list
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));

    // From anywhere else, F1 + EDIT is still the machine select first.
    key(ENAGA_KEY_F4);
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    key(ENAGA_KEY_F4);
}

TEST(ui_the_kit_cursor_follows_the_arrows_the_step_keys_and_encoder_a) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F8);
    shift_key(ENAGA_KEY_F8);
    key(ENAGA_KEY_STEP1);                          // the cursor is remembered between visits
    CHECK_EQ(UiProbe::kit_cursor(app), 0);
    key(ENAGA_KEY_UP);                             // stops at the top rather than wrapping
    CHECK_EQ(UiProbe::kit_cursor(app), 0);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::kit_cursor(app), 2);
    key(ENAGA_KEY_RIGHT);                          // the page of eight
    CHECK_EQ(UiProbe::kit_cursor(app), 10);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::kit_cursor(app), 15);
    key(ENAGA_KEY_LEFT);
    CHECK_EQ(UiProbe::kit_cursor(app), 7);
    key(ENAGA_KEY_SELECT_RIGHT);                   // the selection keys flip the page too
    CHECK_EQ(UiProbe::kit_cursor(app), 15);
    app.on_encoder(0, -3);
    CHECK_EQ(UiProbe::kit_cursor(app), 12);
    key(ENAGA_KEY_STEP1 + 4);                      // a STEP key is that slot, and loads nothing
    CHECK_EQ(UiProbe::kit_cursor(app), 4);
    CHECK_EQ(int(UiProbe::mode(app)), int(Mode::kit));

    // B to D have nothing to set here and must not reach the sound behind the screen.
    const uint8_t before = app_engine().params(UiProbe::selected(app)).v[1];
    app.on_encoder(1, +5);
    pump(2);
    CHECK_EQ(int(app_engine().params(UiProbe::selected(app)).v[1]), int(before));
    key(ENAGA_KEY_F4);
}

TEST(ui_a_kit_is_saved_with_f1_down_and_recalled_with_ok_twice) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F8);
    shift_key(ENAGA_KEY_F8);
    CHECK(pump_until_idle());
    key(ENAGA_KEY_STEP1 + 1);                      // slot 2

    // A sound to keep: track 1's first knob.
    post_cmd(Cmd::set_param, 0, 90);               // a = param 0 on track 0, b = value
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 90);

    // Into an empty slot at once.
    shift_key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::notice(app), "SAVING KIT");
    // The save waits for an idle loader -- fresh() re-offers the samples -- and an empty ring.
    for (int i = 0; i < 4000 && app_storage().kit_slot(1).state != Storage::SlotState::used; i++) {
        pump();
    }
    CHECK_EQ(int(app_storage().kit_slot(1).state), int(Storage::SlotState::used));
    CHECK(FakeHost::the().has_file("/ENAGA/DATA/STRATA/KITS/KIT_02.BIN"));

    // Over a kit, only on the second press.
    shift_key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::notice(app), "AGAIN TO OVERWRITE");

    post_cmd(Cmd::set_param, 0, 11);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 11);

    // OK once arms; OK again within the window recalls.
    pump(2100);                                    // the overwrite arm lapses
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO LOAD");
    pump(6);
    CHECK_EQ(int(app_engine().params(0).v[0]), 11);            // once is not a recall
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "LOADING KIT");
    for (int i = 0; i < 4000 && app_engine().params(0).v[0] != 90; i++) pump();
    CHECK_EQ(int(app_engine().params(0).v[0]), 90);

    // An arm is for one slot: moving the cursor drops it.
    key(ENAGA_KEY_OK);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_UP);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "OK AGAIN TO LOAD");

    // An empty slot says so and loads nothing.
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "EMPTY SLOT");

    // F1 + OK and F1 + CLR stay redo and undo.
    shift_key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "REDO");
    shift_key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::notice(app), "UNDO");
    CHECK_EQ(int(UiProbe::mode(app)), int(Mode::kit));
    key(ENAGA_KEY_F4);
}

// --- the sample browser ------------------------------------------------------

namespace {

// A short 48 kHz mono WAV on the fake card: a header and `frames` of a loud square.
void card_wav(const char *path, uint32_t frames = 480) {
    static uint8_t bytes[44 + 2 * 4800];
    if (frames > 4800) frames = 4800;
    enaga::wav::Format format;
    format.channels = 1;
    format.rate = 48000;
    format.bits = 16;
    enaga::wav::header(bytes, format, frames * 2u);
    for (uint32_t f = 0; f < frames; f++) {
        const int16_t v = (f / 16u) % 2u == 0u ? 9000 : -9000;
        bytes[44 + 2 * f] = uint8_t(uint16_t(v) & 0xFFu);
        bytes[45 + 2 * f] = uint8_t(uint16_t(v) >> 8);
    }
    FakeHost::the().write_file(path, bytes, 44u + frames * 2u);
}

// A card with something in each place the browser shows.
void seed_sample_card() {
    enaga::files::make_folder("/SAMPLES");
    enaga::files::make_folder("/SAMPLES/KICKS");
    enaga::files::make_folder("/RECORD");
    card_wav("/SAMPLES/b loop.wav");
    card_wav("/SAMPLES/A_HIT.WAV");
    card_wav("/SAMPLES/KICKS/K1.WAV", 960);
    card_wav("/RECORD/STRATA_1.WAV");
    const char text[] = "not audio";
    FakeHost::the().write_file("/SAMPLES/README.TXT", text, sizeof text);
    FakeHost::the().write_file("/SAMPLES/._A_HIT.WAV", text, sizeof text);
}

// Track 1 as a SAMPLE track, on its EDIT page.
void sample_track_in_edit() {
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::sample));
    pump(4);
    key(ENAGA_KEY_F4);
    key(ENAGA_KEY_F8);
}

}  // namespace

TEST(ui_samples_ok_on_a_sample_track_opens_the_browser_in_samples) {
    Strata &app = fresh();
    seed_sample_card();
    sample_track_in_edit();
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    pump(4);
    const SampleList &list = app_storage().browse_list();
    CHECK(std::strcmp(list.folder(), "/SAMPLES") == 0);
    // KICKS first (a folder), then the two WAVs by name; the text file and the macOS fork are
    // not offered.
    CHECK_EQ(list.count(), 3);
    CHECK(std::strcmp(list.name(0), "KICKS") == 0);
    CHECK(std::strcmp(list.name(1), "A_HIT.WAV") == 0);
    CHECK(std::strcmp(list.name(2), "b loop.wav") == 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_ok_does_nothing_on_a_track_that_plays_no_file) {
    Strata &app = fresh();
    // MID: no file and no factory sounds (a drum or a VA would open SOUNDS instead).
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::midi));
    pump(4);
    key(ENAGA_KEY_F8);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    // And MACHINE's OK opens it once the machine is a sampler: the natural order of the two.
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::grain));
    pump(4);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    key(ENAGA_KEY_CLR);                            // SAMPLES -> the root
    pump(2);
    key(ENAGA_KEY_CLR);                            // the root -> back where it came from
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_ok_loads_the_file_into_the_track_while_playing) {
    Strata &app = fresh();
    seed_sample_card();
    sample_track_in_edit();
    key(ENAGA_KEY_F5);                             // playing: the load happens under it
    key(ENAGA_KEY_OK);
    pump(4);
    key(ENAGA_KEY_DOWN);                           // A_HIT.WAV
    CHECK_EQ(UiProbe::sample_cursor(app), 1);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "LOADING");
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));      // stays, for the next try
    CHECK(std::strcmp(app_engine().project().kit.tracks[0].sample, "/SAMPLES/A_HIT.WAV") == 0);
    CHECK(app_storage().dirty());
    for (int i = 0; i < 4000 && app_storage().samples().slot(0).state != SampleState::ready; i++) {
        pump();
    }
    CHECK_EQ(int(app_storage().samples().slot(0).state), int(SampleState::ready));
    CHECK_EQ(int(app_storage().samples().slot(0).frames), 480);

    // Into a folder and a second file: the newer pick replaces the first.
    key(ENAGA_KEY_UP);                             // KICKS
    key(ENAGA_KEY_OK);
    pump(4);
    CHECK(std::strcmp(app_storage().browse_list().folder(), "/SAMPLES/KICKS") == 0);
    key(ENAGA_KEY_OK);
    for (int i = 0; i < 4000 && app_storage().samples().slot(0).frames != 960; i++) pump();
    CHECK(std::strcmp(app_engine().project().kit.tracks[0].sample, "/SAMPLES/KICKS/K1.WAV") == 0);
    CHECK_EQ(int(app_storage().samples().slot(0).frames), 960);
    key(ENAGA_KEY_F6);
    key(ENAGA_KEY_F6);
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_clr_climbs_to_the_root_and_reaches_record) {
    Strata &app = fresh();
    seed_sample_card();
    sample_track_in_edit();
    key(ENAGA_KEY_OK);
    pump(4);
    key(ENAGA_KEY_OK);                             // into KICKS
    pump(4);
    CHECK(std::strcmp(app_storage().browse_list().folder(), "/SAMPLES/KICKS") == 0);
    key(ENAGA_KEY_CLR);                            // back up: the cursor lands on KICKS
    pump(4);
    CHECK(std::strcmp(app_storage().browse_list().folder(), "/SAMPLES") == 0);
    {
        enaga::Canvas canvas(g_pixels);
        app.draw(canvas);
    }
    CHECK_EQ(UiProbe::sample_cursor(app), 0);
    key(ENAGA_KEY_CLR);                            // the root: SAMPLES, RECORD
    pump(2);
    const SampleList &list = app_storage().browse_list();
    CHECK(list.at_root());
    CHECK_EQ(list.count(), 2);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_OK);                             // RECORD
    pump(4);
    CHECK(std::strcmp(list.folder(), "/RECORD") == 0);
    CHECK_EQ(list.count(), 1);
    CHECK(std::strcmp(list.name(0), "STRATA_1.WAV") == 0);
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_reopens_on_the_file_the_track_plays) {
    Strata &app = fresh();
    seed_sample_card();
    sample_track_in_edit();
    key(ENAGA_KEY_OK);
    pump(4);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_DOWN);                           // b loop.wav
    key(ENAGA_KEY_OK);
    pump(20);
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_OK);                             // again: straight to /SAMPLES, on that file
    pump(4);
    enaga::Canvas canvas(g_pixels);
    app.draw(canvas);
    CHECK_EQ(UiProbe::sample_cursor(app), 2);
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_reach_the_smpltrek_pool_and_projects) {
    // A card laid out by the stock SmplTrek firmware, with no /SAMPLES: its manual puts samples
    // in SmplTrek/Pool/Audio and each project's in SmplTrek/Project/<name>/Audio.
    Strata &app = fresh();
    // Seeded as a PC would lay it out (the fake does not refuse a write outside the app's areas,
    // and lists only the folders made, as FatFs has them).
    const char *const folders[] = {"/SmplTrek", "/SmplTrek/Pool", "/SmplTrek/Pool/Audio",
                                   "/SmplTrek/Preset", "/SmplTrek/Project",
                                   "/SmplTrek/Project/SONG1", "/SmplTrek/Project/SONG1/Audio",
                                   "/RECORD"};
    for (const char *f : folders) enaga::files::make_folder(f);
    card_wav("/SmplTrek/Pool/Audio/Kick 01.wav");
    card_wav("/SmplTrek/Pool/Audio/Snare.wav", 960);
    card_wav("/SmplTrek/Project/SONG1/Audio/Vox.wav");
    card_wav("/RECORD/STRATA_1.WAV");
    sample_track_in_edit();
    key(ENAGA_KEY_OK);                             // no /SAMPLES: straight into the pool
    pump(4);
    const SampleList &list = app_storage().browse_list();
    CHECK(std::strcmp(list.folder(), "/SmplTrek/Pool/Audio") == 0);
    CHECK_EQ(list.count(), 2);
    key(ENAGA_KEY_OK);                             // Kick 01.wav
    for (int i = 0; i < 4000 && app_storage().samples().slot(0).state != SampleState::ready; i++) {
        pump();
    }
    CHECK(std::strcmp(app_engine().project().kit.tracks[0].sample,
                      "/SmplTrek/Pool/Audio/Kick 01.wav") == 0);
    CHECK_EQ(int(app_storage().samples().slot(0).state), int(SampleState::ready));

    // CLR climbs the card's own tree to the root, which now offers the SmplTrek's folders.
    key(ENAGA_KEY_CLR);
    pump(4);
    CHECK(std::strcmp(list.folder(), "/SmplTrek/Pool") == 0);
    key(ENAGA_KEY_CLR);
    pump(4);
    CHECK(std::strcmp(list.folder(), "/SmplTrek") == 0);
    key(ENAGA_KEY_CLR);
    pump(4);
    CHECK(list.at_root());
    CHECK_EQ(list.count(), 4);
    {
        enaga::Canvas canvas(g_pixels);
        app.draw(canvas);
    }
    CHECK_EQ(UiProbe::sample_cursor(app), 2);      // back on SMPLTREK
    key(ENAGA_KEY_OK);                             // SMPLTREK -> Project -> SONG1 -> Audio
    pump(4);
    CHECK(std::strcmp(list.folder(), "/SmplTrek") == 0);
    CHECK_EQ(list.find("Project"), 2);             // Pool, Preset, Project
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_OK);
    pump(4);
    key(ENAGA_KEY_OK);
    pump(4);
    key(ENAGA_KEY_OK);
    pump(4);
    CHECK(std::strcmp(list.folder(), "/SmplTrek/Project/SONG1/Audio") == 0);
    key(ENAGA_KEY_OK);                             // Vox.wav
    pump(20);
    CHECK(std::strcmp(app_engine().project().kit.tracks[0].sample,
                      "/SmplTrek/Project/SONG1/Audio/Vox.wav") == 0);

    // Leave and come back: the browser opens where the track's file is, on it.
    for (int i = 0; i < 6 && UiProbe::mode(app) == int(Mode::samples); i++) {
        key(ENAGA_KEY_CLR);
        pump(4);
    }
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_OK);
    pump(4);
    CHECK(std::strcmp(list.folder(), "/SmplTrek/Project/SONG1/Audio") == 0);
    {
        enaga::Canvas canvas(g_pixels);
        app.draw(canvas);
    }
    CHECK_EQ(UiProbe::sample_cursor(app), 0);
    check_layout(app, "SAMPLES, a SmplTrek project folder");
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_root_hides_the_smpltrek_folders_on_a_card_without_them) {
    fresh();
    seed_sample_card();
    sample_track_in_edit();
    key(ENAGA_KEY_OK);
    pump(4);
    CHECK(std::strcmp(app_storage().browse_list().folder(), "/SAMPLES") == 0);
    key(ENAGA_KEY_CLR);
    pump(4);
    CHECK(app_storage().browse_list().at_root());
    CHECK_EQ(app_storage().browse_list().count(), 2);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_samples_says_why_a_folder_is_empty_or_missing) {
    Strata &app = fresh();
    sample_track_in_edit();
    key(ENAGA_KEY_OK);                             // no /SAMPLES on this card
    pump(4);
    CHECK(app_storage().browse_error() != nullptr);
    CHECK_EQ(app_storage().browse_list().count(), 0);
    key(ENAGA_KEY_OK);                             // nothing to pick: nothing happens
    CHECK_EQ(app_engine().project().kit.tracks[0].sample[0], '\0');
    key(ENAGA_KEY_CLR);                            // up to the root still works
    pump(2);
    CHECK(app_storage().browse_list().at_root());
    CHECK(app_storage().browse_error() == nullptr);
    (void)app;
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_samples) {
    Strata &app = fresh();
    seed_sample_card();
    // A name longer than the line, and enough files for a second page.
    card_wav("/SAMPLES/A VERY LONG SAMPLE NAME THAT RUNS OFF.WAV");
    char path[48];
    for (int i = 0; i < 12; i++) {
        std::snprintf(path, sizeof path, "/SAMPLES/hat_%02d.wav", i);
        card_wav(path);
    }
    sample_track_in_edit();
    check_layout(app, "EDIT on a SAMPLE track");
    key(ENAGA_KEY_OK);
    check_layout(app, "SAMPLES, reading");
    pump(400);                                     // past the notice
    check_layout(app, "SAMPLES");
    key(ENAGA_KEY_RIGHT);
    check_layout(app, "SAMPLES, page 2");
    for (int i = 0; i < 40; i++) key(ENAGA_KEY_DOWN);
    check_layout(app, "SAMPLES, last entry");
    key(ENAGA_KEY_OK);
    pump(2);
    check_layout(app, "SAMPLES, loading");
    pump_until_idle();
    pump(400);
    check_layout(app, "SAMPLES, loaded");
    key(ENAGA_KEY_CLR);
    pump(2);
    check_layout(app, "SAMPLES, root");
    FakeHost::the().set_card(false);
    app_storage().on_card(false);
    key(ENAGA_KEY_OK);
    pump(2);
    check_layout(app, "SAMPLES, no card");
    FakeHost::the().set_card(true);
    app_storage().on_card(true);
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_CLR);
    shift_key(ENAGA_KEY_F8);
    check_layout(app, "MACHINE on a SAMPLE track");
    key(ENAGA_KEY_F4);
}

TEST(storage_a_second_folder_asked_for_mid_listing_waits_its_turn) {
    // Only one files::list is in flight: the entries of the first would otherwise land in the
    // second's table.
    fresh();
    seed_sample_card();
    Storage &storage = app_storage();
    CHECK(storage.request_browse("/SAMPLES"));
    CHECK(storage.browsing());
    CHECK(storage.request_browse("/RECORD"));
    CHECK(storage.browsing());
    for (int i = 0; i < 8 && storage.browsing(); i++) FakeHost::the().complete_loads();
    CHECK(!storage.browsing());
    const SampleList &list = storage.browse_list();
    CHECK(std::strcmp(list.folder(), "/RECORD") == 0);
    CHECK_EQ(list.count(), 1);
    CHECK(std::strcmp(list.name(0), "STRATA_1.WAV") == 0);
    // Too long a folder is refused with nothing changed; "" is the root, with no card needed.
    char huge[SampleList::kFolderBytes + 8];
    std::memset(huge, 'X', sizeof huge);
    huge[0] = '/';
    huge[sizeof huge - 1] = '\0';
    CHECK(!storage.request_browse(huge));
    CHECK(std::strcmp(list.folder(), "/RECORD") == 0);
    FakeHost::the().set_card(false);
    storage.on_card(false);
    CHECK(storage.request_browse(""));
    CHECK(list.at_root());
    CHECK(storage.request_browse("/SAMPLES"));
    CHECK(std::strcmp(storage.browse_error(), "NO CARD") == 0);
    FakeHost::the().set_card(true);
    storage.on_card(true);
    // A path the kit cannot hold is refused before it touches the kit.
    char long_path[kSamplePathBytes + 4];
    std::memset(long_path, 'P', sizeof long_path);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    CHECK(!storage.request_sample_file(0, long_path));
    CHECK(!storage.request_sample_file(-1, "/SAMPLES/A_HIT.WAV"));
    CHECK(!storage.request_sample_file(kTracks, "/SAMPLES/A_HIT.WAV"));
    CHECK(!storage.request_sample_file(0, ""));
    CHECK_EQ(app_engine().project().kit.tracks[0].sample[0], '\0');
}

// --- RESAMPLE from the audio input ---------------------------------------------------------

TEST(ui_resample_b_picks_the_source_and_routes_the_input) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    shift_key(ENAGA_KEY_F2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));
    for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);   // UI state outlives fresh(): MASTER

    app.on_encoder(1, +1);                         // B: LINE
    CHECK_EQ(host.input_source(), int(ENAGA_INPUT_LINE_STEREO));
    CHECK_EQ(UiProbe::notice(app), "LINE");
    for (int i = 0; i < 3; i++) app.on_encoder(2, -1);
    CHECK_EQ(host.input_level(), int(ENAGA_INPUT_LOW));
    app.on_encoder(2, +1);
    CHECK_EQ(host.input_level(), int(ENAGA_INPUT_NORMAL));
    app.on_encoder(2, +1);                         // C: HIGH
    CHECK_EQ(host.input_level(), int(ENAGA_INPUT_HIGH));
    app.on_encoder(2, +5);                         // and no further
    CHECK_EQ(host.input_level(), int(ENAGA_INPUT_HIGH));
    for (int i = 0; i < 4; i++) app.on_encoder(1, +1);
    CHECK_EQ(host.input_source(), int(ENAGA_INPUT_HIZ));    // the last input; the tracks follow

    // OK arms a take FROM THE INPUT: the engine is told source 1.
    const int calls = host.input_calls();
    key(ENAGA_KEY_OK);
    CHECK_EQ(host.input_calls(), calls);           // already routed: no second 10 ms gap
    pump(2);
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 1);
    key(ENAGA_KEY_CLR);                            // cancel
    pump(2);

    // Back to the master: C has nothing to set, and the route is left alone.
    for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);
    const int before = host.input_calls();
    app.on_encoder(2, -1);
    CHECK_EQ(host.input_calls(), before);
    // D does nothing here, and never reaches a sound parameter.
    const uint8_t d_param = app_engine().params(0).v[3];
    app.on_encoder(3, +5);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[3]), int(d_param));
    key(ENAGA_KEY_F4);
}

TEST(ui_resample_a_refused_route_is_shown_and_tried_again_at_the_arm) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    shift_key(ENAGA_KEY_F2);
    for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);   // MASTER
    host.set_input_result(-2);                     // the codec did not answer
    app.on_encoder(1, +1);
    CHECK_EQ(UiProbe::notice(app), "INPUT REFUSED");
    host.set_input_result(0);
    const int calls = host.input_calls();
    key(ENAGA_KEY_OK);                             // not applied yet: the arm tries again
    CHECK_EQ(host.input_calls(), calls + 1);
    CHECK_EQ(host.input_source(), int(ENAGA_INPUT_LINE_STEREO));
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_resample_sources) {
    Strata &app = fresh();
    FakeHost &host = FakeHost::the();
    shift_key(ENAGA_KEY_F2);
    for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);   // MASTER
    pump(400);
    check_layout(app, "RESAMPLE, master");
    app.on_encoder(2, +1);
    app.on_encoder(2, +1);                         // HIGH: the longest level name
    for (int s = 1; s < kTakeSourceChoices; s++) {
        app.on_encoder(1, +1);
        pump(400);
        check_layout(app, kTakeSources[s].name);
    }
    // The mic without headphones says so; with them it does not.
    for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);
    app.on_encoder(1, +1);
    app.on_encoder(1, +1);
    app.on_encoder(1, +1);                         // MIC
    pump(400);
    check_layout(app, "RESAMPLE, mic without headphones");
    host.set_headphones(true);
    check_layout(app, "RESAMPLE, mic");
    host.set_headphones(false);
    host.set_input_result(-1);
    app.on_encoder(1, +1);
    pump(400);
    check_layout(app, "RESAMPLE, refused");
    host.set_input_result(0);
    // A full meter, clipping, and an armed take.
    static float in[2 * ENAGA_BLOCK_FRAMES];
    for (float &x : in) x = 1.0f;
    static float out[2 * ENAGA_BLOCK_FRAMES];
    app.audio(in, out, ENAGA_BLOCK_FRAMES);
    app.on_encoder(1, +1);
    key(ENAGA_KEY_OK);
    pump(400);
    app.audio(in, out, ENAGA_BLOCK_FRAMES);
    check_layout(app, "RESAMPLE, clipping and armed");
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_F4);
}

TEST(ui_a_late_stream_is_said_once_and_not_every_pass) {
    // Status::stream_late moves when the card could not keep up with a streamed file. Written by
    // the audio side; set by hand here, which is what a const_cast on the engine's own Status is.
    Strata &app = fresh();
    Status &status = const_cast<Status &>(UiProbe::engine(app).status());
    for (int i = 0; i < 2100; i++) app.update(uint32_t(i));     // past the quiet start
    status.stream_late.store(5, std::memory_order_relaxed);
    app.update(3000);
    CHECK_EQ(UiProbe::notice(app), "STREAM LATE");
    status.stream_late.store(9, std::memory_order_relaxed);
    app.update(4700);              // the notice has gone (1.5 s), and it is not said again yet
    CHECK(!UiProbe::notice_showing(app));
    status.stream_late.store(12, std::memory_order_relaxed);
    app.update(5200);
    CHECK_EQ(UiProbe::notice(app), "STREAM LATE");
    CHECK(UiProbe::notice_showing(app));
    status.stream_late.store(0, std::memory_order_relaxed);
}

// --- the PROJECTS page (PROJECT, the octave keys past SETUP) ------------------------------------

namespace {

// Two projects on the card, FIRST and SECOND, with SECOND the one open. Saved through Storage,
// which is what the page lists: the folders under PROJECTS/.
void two_projects() {
    CHECK(app_storage().request_save_as("FIRST"));
    CHECK(pump_until_idle());
    CHECK(app_storage().request_save_as("SECOND"));
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(std::strcmp(app_storage().project_name(), "SECOND") == 0);
}

// PROJECT, then the octave keys to the last page, then the card read.
void to_projects_page(Strata &app) {
    key(ENAGA_KEY_F3);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 4);
    for (int i = 0; i < 200 && app_storage().browsing(); i++) pump();
    pump();
}

// The name entry's text, made into `wanted` by its own controls: VALUE steps each character until
// it is the one wanted, encoder A moves on. Spaces past the end clear what the old name left.
void type_name(Strata &app, const char *wanted) {
    const int length = int(std::strlen(wanted));
    for (int i = 0; i < enaga::ui::TextEntry::max_chars; i++) {
        const char target = i < length ? wanted[i] : ' ';
        for (int tries = 0; tries < 64; tries++) {
            const char *text = UiProbe::name_entry_text(app);
            const char at = i < int(std::strlen(text)) ? text[i] : ' ';
            if (at == target) break;
            app.on_encoder(4, +1);
        }
        app.on_encoder(0, +1);
    }
    CHECK(std::strcmp(UiProbe::name_entry_text(app), wanted) == 0);
}

}  // namespace

TEST(ui_projects_lists_the_card_and_starts_on_the_open_one) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    const SampleList &list = app_storage().browse_list();
    CHECK(list.find("FIRST") >= 0);
    CHECK(list.find("SECOND") >= 0);
    CHECK_EQ(UiProbe::project_cursor(app), list.find("SECOND"));
    // The arrows and encoder A move it, and stop at the ends.
    for (int i = 0; i < 40; i++) key(ENAGA_KEY_UP);
    CHECK_EQ(UiProbe::project_cursor(app), 0);
    app.on_encoder(0, +100);
    CHECK_EQ(UiProbe::project_cursor(app), list.count() - 1);
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::project_page(app), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_ok_twice_opens_another_project) {
    Strata &app = fresh();
    two_projects();
    // Something only FIRST has: a trig saved into it, then SECOND saved without it.
    CHECK(app_storage().request_save_as("FIRST"));
    CHECK(pump_until_idle());
    step_down(4);
    step_up(4);
    pump(2);
    CHECK(trig_at(0, 4));
    CHECK(app_storage().request_save_as("SECOND"));
    CHECK(pump_until_idle());
    to_projects_page(app);

    // OK on the open one says so and does nothing.
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "ALREADY OPEN") == 0);

    const int first = app_storage().browse_list().find("FIRST");
    CHECK(first >= 0);
    app.on_encoder(0, first - UiProbe::project_cursor(app));
    CHECK_EQ(UiProbe::project_cursor(app), first);
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "AGAIN TO OPEN") == 0);
    pump(4);
    CHECK(std::strcmp(app_storage().project_name(), "SECOND") == 0);   // once is not enough
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "OPENING") == 0);
    CHECK_EQ(UiProbe::project_page(app), 0);
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(std::strcmp(app_storage().project_name(), "FIRST") == 0);
    CHECK(!trig_at(0, 4));
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_a_press_on_another_entry_disarms_the_open) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    const int first = app_storage().browse_list().find("FIRST");
    app.on_encoder(0, first - UiProbe::project_cursor(app));
    key(ENAGA_KEY_OK);
    key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_UP);
    key(ENAGA_KEY_OK);                               // armed again, not opened
    CHECK(std::strcmp(UiProbe::notice(app), "AGAIN TO OPEN") == 0);
    pump(4);
    CHECK(std::strcmp(app_storage().project_name(), "SECOND") == 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_func_down_saves_under_a_typed_name) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    shift_key(ENAGA_KEY_DOWN);
    CHECK(UiProbe::name_entry_open(app));
    CHECK(std::strcmp(UiProbe::name_entry_text(app), "SECOND") == 0);   // starts from its name

    // While it is open every key is its own: GRID does not leave.
    key(ENAGA_KEY_F4);
    CHECK(UiProbe::name_entry_open(app));
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));

    type_name(app, "THIRD 2");
    key(ENAGA_KEY_OK);
    CHECK(!UiProbe::name_entry_open(app));
    CHECK(std::strcmp(UiProbe::notice(app), "SAVING") == 0);
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    CHECK(std::strcmp(app_storage().project_name(), "THIRD 2") == 0);
    // The list is read again once the save is on the card, and the new one is in it.
    for (int i = 0; i < 400 && app_storage().browse_list().find("THIRD 2") < 0; i++) pump();
    CHECK(app_storage().browse_list().find("THIRD 2") >= 0);
    CHECK(app_storage().browse_list().find("SECOND") >= 0);           // the old one stays
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_save_as_will_not_take_another_projects_name) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    shift_key(ENAGA_KEY_DOWN);
    type_name(app, "FIRST");
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "NAME TAKEN") == 0);
    pump(8);
    CHECK(std::strcmp(app_storage().project_name(), "SECOND") == 0);

    // An empty name, and CLR, change nothing either.
    shift_key(ENAGA_KEY_DOWN);
    type_name(app, "");
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "NO NAME") == 0);
    shift_key(ENAGA_KEY_DOWN);
    key(ENAGA_KEY_CLR);
    CHECK(!UiProbe::name_entry_open(app));
    CHECK(std::strcmp(UiProbe::notice(app), "NAME KEPT") == 0);
    CHECK_EQ(UiProbe::project_page(app), 4);         // that CLR was the entry's, not the page's
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_the_projects_page_and_save_as) {
    Strata &app = fresh();
    to_projects_page(app);
    check_layout(app, "PROJECTS, only the default");
    // A full page and then some, with the longest names the panel can type.
    char name[24];
    for (int i = 0; i < 11; i++) {
        std::memcpy(name, "WWWWWWWWWWWWWW00", 17);
        name[14] = char('0' + i / 10);
        name[15] = char('0' + i % 10);
        CHECK(app_storage().request_save_as(name));
        CHECK(pump_until_idle());
    }
    shift_key(ENAGA_KEY_UP);
    for (int i = 0; i < 200 && app_storage().browsing(); i++) pump();
    pump();
    check_layout(app, "PROJECTS, a long list");
    key(ENAGA_KEY_F4);                               // an edit, then straight back
    step_down(0);
    step_up(0);
    pump(2);
    key(ENAGA_KEY_F3);
    for (int i = 0; i < 200 && app_storage().browsing(); i++) pump();
    pump();
    CHECK_EQ(UiProbe::project_page(app), 4);
    CHECK(app_storage().dirty());
    check_layout(app, "PROJECTS, unsaved");
    key(ENAGA_KEY_RIGHT);
    check_layout(app, "PROJECTS, page 2");
    shift_key(ENAGA_KEY_DOWN);
    app.on_encoder(0, +100);                         // the cursor at the last place
    check_layout(app, "SAVE AS");
    key(ENAGA_KEY_CLR);
    app.on_card(false);
    shift_key(ENAGA_KEY_UP);
    pump(4);
    check_layout(app, "PROJECTS, no card");
    app.on_card(true);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

// --- PROJECTS: rename (push B) and delete (push C, then OK twice) ---------------------------

namespace {

void cursor_to(Strata &app, const char *name) {
    const int at = app_storage().browse_list().find(name);
    CHECK(at >= 0);
    app.on_encoder(0, at - UiProbe::project_cursor(app));
    CHECK_EQ(UiProbe::project_cursor(app), at);
}

void wait_for_list(Strata &app, const char *name, bool there) {
    (void)app;
    for (int i = 0; i < 800 && (app_storage().browse_list().find(name) >= 0) != there; i++) pump();
    for (int i = 0; i < 200 && app_storage().browsing(); i++) pump();
}

}  // namespace

TEST(ui_projects_push_b_renames_the_project_under_the_cursor) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    cursor_to(app, "FIRST");
    key(ENAGA_KEY_PUSH_B);
    CHECK(UiProbe::name_entry_open(app));
    CHECK(std::strcmp(UiProbe::name_entry_text(app), "FIRST") == 0);   // starts from its name
    check_layout(app, "RENAME");
    type_name(app, "THIRD");
    key(ENAGA_KEY_OK);
    CHECK(!UiProbe::name_entry_open(app));
    CHECK(std::strcmp(UiProbe::notice(app), "RENAMING") == 0);
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    wait_for_list(app, "THIRD", true);
    CHECK(app_storage().browse_list().find("THIRD") >= 0);
    CHECK(app_storage().browse_list().find("FIRST") < 0);
    CHECK(std::strcmp(app_storage().project_name(), "SECOND") == 0);

    // The open one too: the project takes the new name.
    cursor_to(app, "SECOND");
    key(ENAGA_KEY_PUSH_B);
    type_name(app, "LIVE");
    key(ENAGA_KEY_OK);
    CHECK(pump_until_idle());
    CHECK(std::strcmp(app_storage().project_name(), "LIVE") == 0);
    wait_for_list(app, "SECOND", false);
    CHECK(app_storage().browse_list().find("SECOND") < 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_rename_refuses_a_taken_name_and_keeps_an_unchanged_one) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    cursor_to(app, "FIRST");
    key(ENAGA_KEY_PUSH_B);
    type_name(app, "SECOND");
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "NAME TAKEN") == 0);
    key(ENAGA_KEY_PUSH_B);
    key(ENAGA_KEY_OK);                               // the name as it was
    CHECK(std::strcmp(UiProbe::notice(app), "NAME KEPT") == 0);
    key(ENAGA_KEY_PUSH_B);
    key(ENAGA_KEY_CLR);
    CHECK(!UiProbe::name_entry_open(app));
    CHECK_EQ(UiProbe::project_page(app), 4);
    CHECK(!app_storage().project_op_pending());
    pump(8);
    CHECK(app_storage().browse_list().find("FIRST") >= 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_push_c_then_ok_twice_deletes) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    cursor_to(app, "FIRST");
    key(ENAGA_KEY_PUSH_C);
    CHECK(std::strcmp(UiProbe::notice(app), "DELETE? OK TWICE") == 0);
    check_layout(app, "PROJECTS, a delete asked");
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "OK AGAIN TO DELETE") == 0);
    check_layout(app, "PROJECTS, a delete once confirmed");
    pump(4);
    CHECK(!app_storage().project_op_pending());      // once is not enough
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "DELETING") == 0);
    CHECK(pump_until_idle());
    CHECK(app_storage().last_error() == nullptr);
    wait_for_list(app, "FIRST", false);
    CHECK(app_storage().browse_list().find("FIRST") < 0);
    CHECK(app_storage().browse_list().find("SECOND") >= 0);
    // OK now is an open again, not a delete.
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "ALREADY OPEN") == 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_a_delete_is_called_off_by_clr_or_another_key) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    cursor_to(app, "FIRST");
    key(ENAGA_KEY_PUSH_C);
    key(ENAGA_KEY_CLR);
    CHECK(std::strcmp(UiProbe::notice(app), "DELETE OFF") == 0);
    CHECK_EQ(UiProbe::project_page(app), 4);         // that CLR was the question's
    key(ENAGA_KEY_OK);
    key(ENAGA_KEY_OK);                               // an open now, not a delete
    CHECK(std::strcmp(UiProbe::notice(app), "OPENING") == 0);
    CHECK(pump_until_idle());
    CHECK(std::strcmp(app_storage().project_name(), "FIRST") == 0);
    pump_until_idle(2000);
    to_projects_page(app);
    cursor_to(app, "SECOND");
    key(ENAGA_KEY_PUSH_C);
    key(ENAGA_KEY_UP);                               // moving on lets it go
    cursor_to(app, "SECOND");
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "AGAIN TO OPEN") == 0);
    pump(8);
    CHECK(!app_storage().project_op_pending());
    CHECK(app_storage().browse_list().find("SECOND") >= 0);
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_projects_the_open_project_is_not_deleted) {
    Strata &app = fresh();
    two_projects();
    to_projects_page(app);
    cursor_to(app, "SECOND");
    key(ENAGA_KEY_PUSH_C);
    CHECK(std::strcmp(UiProbe::notice(app), "PROJECT IS OPEN") == 0);
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "ALREADY OPEN") == 0);
    CHECK(!app_storage().project_op_pending());
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

// --- SLC AUTO: push SLC's knob on a SAMPLE track ----------------------------------------------

TEST(ui_pushing_slc_on_a_sample_track_turns_auto_slicing_on_and_off) {
    Strata &app = fresh();
    const KitTrack &kit = app_engine().project().kit.tracks[0];
    sample_track_in_edit();
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(int(kit.slice_mode), int(kSliceEqual));

    // SLC is the fourth knob on SRC: push D.
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    CHECK_EQ(int(kit.slice_mode), int(kSliceAuto));
    CHECK(std::strcmp(UiProbe::notice(app), "SLICES AUTO") == 0);
    CHECK(app_storage().dirty());
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    CHECK_EQ(int(kit.slice_mode), int(kSliceEqual));
    CHECK(std::strcmp(UiProbe::notice(app), "SLICES EVEN") == 0);

    // Pushing another knob, or on another page, does not touch it.
    key(ENAGA_KEY_PUSH_A);
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    CHECK_EQ(int(kit.slice_mode), int(kSliceEqual));
    key(ENAGA_KEY_SELECT_LEFT);

    // FUNC + push is still the kill EQ, not this.
    shift_key(ENAGA_KEY_PUSH_A);
    shift_key(ENAGA_KEY_PUSH_A);
    pump(2);
    CHECK_EQ(int(kit.slice_mode), int(kSliceEqual));

    // Nor on a track that is not SAMPLE: GRAIN has no slices.
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::grain));
    pump(4);
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    CHECK_EQ(int(kit.slice_mode), int(kSliceEqual));
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_slc_auto_line) {
    Strata &app = fresh();
    sample_track_in_edit();
    while (UiProbe::param_page(app) != int(Page::src)) key(ENAGA_KEY_SELECT_RIGHT);
    check_layout(app, "EDIT SRC, SAMPLE, slices even");
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    pump(2000);                                      // let the SLICES AUTO notice go
    check_layout(app, "EDIT SRC, SAMPLE, AUTO with no hits");
    const_cast<Status &>(app_engine().status()).slice_points[0].store(15);
    check_layout(app, "EDIT SRC, SAMPLE, AUTO with 15 hits");
    const_cast<Status &>(app_engine().status()).slice_points[0].store(0);
    key(ENAGA_KEY_PUSH_D);
    pump(2);
    key(ENAGA_KEY_F4);
}

// --- MIDI in, and MIDI OUT's channel on the MACHINE page ------------------------------------
//
// Nothing drove `on_midi` before these: MIDI IN was note-on only and untested. Each message
// type here goes through the same Strata object the unit runs.

namespace {

enaga::MidiMessage midi(uint8_t status, uint8_t a, uint8_t b) {
    enaga::MidiMessage m;
    m.status = status;
    m.data1 = a;
    m.data2 = b;
    m.port = ENAGA_MIDI_DIN;
    return m;
}

// MIDI IN on track 1 is easiest to see through MIDI OUT: give track 1 a channel and the notes
// it is played come back out there, start and end.
void midi_through(int track, uint8_t channel) {
    post_track(Cmd::set_midi_out, track, channel);
    pump(2);
    FakeHost::the().clear_midi();
}

}  // namespace

TEST(ui_midi_in_a_note_is_held_until_its_note_off) {
    Strata &app = fresh();
    midi_through(0, 12);
    app.on_midi(midi(0x90, 60, 100));              // channel 1: track 1
    pump(400);                                     // far past a step: still held
    FakeHost &host = FakeHost::the();
    CHECK_EQ(host.midi_messages(0x90, 12, 60), 1);
    CHECK_EQ(host.midi_messages(0x80, 12, 60), 0);
    app.on_midi(midi(0x80, 60, 0));
    pump();
    CHECK_EQ(host.midi_messages(0x80, 12, 60), 1);
}

TEST(ui_midi_in_a_note_on_at_velocity_zero_is_a_note_off) {
    Strata &app = fresh();
    midi_through(0, 12);
    app.on_midi(midi(0x90, 62, 90));
    pump();
    app.on_midi(midi(0x90, 62, 0));
    pump();
    CHECK_EQ(FakeHost::the().midi_messages(0x80, 12, 62), 1);
}

TEST(ui_midi_in_a_note_off_finds_the_track_its_note_started_on) {
    // Channel 10 plays the SELECTED track. Select track 3, play, select track 5, let go: the
    // note on track 3 must end, and track 5 must get no stray release.
    Strata &app = fresh();
    midi_through(2, 3);
    midi_through(4, 5);
    app.on_pad_press(4, 100);                      // pad 4 is track 3: selected (and played)
    app.on_pad_release(4);
    pump(200);                                     // the pad's own MIDI note has ended
    FakeHost::the().clear_midi();
    app.on_midi(midi(0x99, 50, 100));
    pump();
    CHECK_EQ(FakeHost::the().midi_messages(0x90, 3, 50), 1);
    app.on_pad_press(8, 100);                      // track 5
    app.on_pad_release(8);
    pump();
    app.on_midi(midi(0x89, 50, 0));
    pump();
    CHECK_EQ(FakeHost::the().midi_messages(0x80, 3, 50), 1);
    CHECK_EQ(FakeHost::the().midi_messages(0x80, 5, 50), 0);
}

TEST(ui_midi_in_all_notes_off_lets_go_of_every_held_note_on_that_channel) {
    Strata &app = fresh();
    midi_through(1, 4);
    app.on_midi(midi(0x91, 60, 100));
    app.on_midi(midi(0x91, 63, 100));
    app.on_midi(midi(0x91, 67, 100));
    pump();
    app.on_midi(midi(0xB1, 123, 0));
    pump();
    CHECK_EQ(FakeHost::the().midi_messages(0x80, 4), 3);
    CHECK_EQ(app_engine().midi_notes().sounding(1), 0);
}

TEST(ui_midi_in_cc_16_to_31_set_the_tracks_sixteen_parameters) {
    Strata &app = fresh();
    app.on_midi(midi(0xB2, 16, 5));                // channel 3, CC 16: track 3's slot 0
    app.on_midi(midi(0xB2, 31, 120));              // CC 31: slot 15
    app.on_midi(midi(0xB2, 32, 1));                // past the map: nothing
    app.on_midi(midi(0xB2, 15, 1));
    pump();
    CHECK_EQ(int(app_engine().params(2).v[0]), 5);
    CHECK_EQ(int(app_engine().params(2).v[15]), 120);
    for (int p = 1; p < 15; p++) CHECK_EQ(int(app_engine().params(2).v[p]), p == kLvl ? 127 : 64);
    CHECK_EQ(int(app_engine().params(0).v[0]), 64);
}

TEST(ui_midi_in_program_change_on_the_midi_channel_queues_that_pattern) {
    Strata &app = fresh();
    FakeHost::the().set_midi_channel(5);           // the user's MIDI CH is 6
    app.on_midi(midi(0xC5, 37, 0));                // program 38: bank C, pattern 6
    pump(2);                                       // stopped: a queued change lands at once
    CHECK_EQ(int(app_engine().project().bank), 2);
    CHECK_EQ(int(app_engine().project().slot), 5);

    app.on_midi(midi(0xC0, 0, 0));                 // another channel: not ours
    pump(2);
    CHECK_EQ(int(app_engine().project().bank), 2);
    CHECK_EQ(int(app_engine().project().slot), 5);
}

TEST(ui_machine_encoder_b_sets_the_tracks_midi_out_channel) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    app.on_encoder(1, +3);
    pump();
    CHECK_EQ(int(app_engine().midi_out(0)), 3);
    app.on_encoder(1, -10);
    pump();
    CHECK_EQ(int(app_engine().midi_out(0)), 0);
    // C and D set nothing here, and do not fall through to track 1's parameters.
    app.on_encoder(2, +5);
    app.on_encoder(3, +5);
    pump();
    for (int p = 0; p < kParams; p++) {
        CHECK_EQ(int(app_engine().params(0).v[p]), p == kLvl ? 127 : 64);
    }
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_machine_with_mid_and_every_midi_out_state) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F8);
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::midi));
    pump(2);
    check_layout(app, "MACHINE, MID with MIDI OUT off");
    for (uint8_t channel = 1; channel <= 16; channel++) {
        post_track(Cmd::set_midi_out, 0, channel);
        pump(2);
        check_layout(app, "MACHINE, MID with a MIDI OUT channel");
    }
    for (int machine = 0; machine < int(MachineId::count); machine++) {
        post_track(Cmd::set_machine, 0, uint8_t(machine));
        pump(2);
        check_layout(app, "MACHINE, each machine lit");
    }
    key(ENAGA_KEY_F4);
}

// --- the controls concept (docs/CONTROLS.md) -------------------------------------------------
//
// Built from the SmplTrek's panel: each group of buttons has one job in every mode, FUNC + a
// function key is the label printed under it, the setup actions are in the FUNC menu, and the
// performance pads (printed TRK 1-7) are lit and named on the screen.

namespace {

int pad_level(const Strata &app, int pad) {
    const int r = UiProbe::pad_r(app, pad), g = UiProbe::pad_g(app, pad),
              b = UiProbe::pad_b(app, pad);
    return r > g ? (r > b ? r : b) : (g > b ? g : b);
}

}  // namespace

TEST(ui_home_is_grid_and_a_double_tap_is_keys) {
    Strata &app = fresh();
    key(ENAGA_KEY_F7);
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));    // from anywhere: GRID
    key(ENAGA_KEY_F4);                                 // again at once: KEYS
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));
    CHECK_EQ(UiProbe::notice(app), "KEYS");
    key(ENAGA_KEY_F4);                                 // from KEYS: GRID
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));

    // A second press after the double-tap window is just HOME again: a reflex press on GRID
    // stays on GRID.
    pump(500);
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
    pump(500);
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
}

TEST(ui_func_keys_follow_their_printed_second_labels) {
    // REC "sampling", PROJECT "pool", HOME "system", MIXER "fx", EDIT "rec src".
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));
    shift_key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    pump_until_idle();
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    shift_key(ENAGA_KEY_F4);                           // again: closed, back to SAMPLES
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    key(ENAGA_KEY_F4);
}

TEST(ui_the_menu_scrolls_runs_and_goes_back) {
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                  // over EDIT
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuSave));
    key(ENAGA_KEY_UP);                                  // wraps to the last row
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuRowCount) - 1);
    key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuSave));
    app.on_encoder(0, +2);                              // A scrolls too
    CHECK_EQ(UiProbe::menu_cursor(app), 2);

    // The STEP keys and the pad-row and screen keys do nothing under the menu.
    const int trigs_before = locks_on_track(0);
    key(ENAGA_KEY_STEP1 + 3);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);
    pump(2);
    CHECK(!trig_at(0, 3));
    CHECK_EQ(locks_on_track(0), trigs_before);
    CHECK_EQ(UiProbe::page(app), 0);
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));

    // FUNC + CLR / OK are still undo and redo in the menu.
    shift_key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::notice(app), "UNDO");
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));

    key(ENAGA_KEY_CLR);                                 // back where it opened
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    shift_key(ENAGA_KEY_F4);                            // and it opens on SAVE again
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuSave));
    key(ENAGA_KEY_F7);                                  // a mode key leaves it too
    CHECK_EQ(UiProbe::mode(app), int(Mode::mixer));
    key(ENAGA_KEY_F4);
}

TEST(ui_the_menu_freeze_row_arms_a_freeze_of_the_selected_track_and_gives_it_back) {
    // FREEZE (dsp/frozen.hpp): the row shows the selected track and the bars OK records, B and
    // LEFT/RIGHT set the bars, OK arms the freeze, and OK on a frozen or arming track undoes it.
    Strata &app = fresh();
    key(ENAGA_KEY_F8);                                  // EDIT, track 1
    menu_to(kMenuFreeze);
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuFreeze));
    // B walks 1, 2, 4, 8 and round; the length outlives fresh(), so walk to 1 by the notice.
    for (int i = 0; i < 4 && std::strcmp(UiProbe::notice(app), "FREEZE 1 BAR") != 0; i++) {
        app.on_encoder(1, +1);
    }
    CHECK_EQ(UiProbe::notice(app), "FREEZE 1 BAR");
    app.on_encoder(1, +1);
    CHECK_EQ(UiProbe::notice(app), "FREEZE 2 BAR");
    app.on_encoder(1, -1);
    CHECK_EQ(UiProbe::notice(app), "FREEZE 1 BAR");
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::notice(app), "ARMED: PRESS PLAY");
    CHECK_EQ(app_storage().freezing_track(), 0);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));      // the menu closed over EDIT
    pump(2);
    menu_run(kMenuFreeze);                              // again: the take is dropped
    CHECK_EQ(UiProbe::notice(app), "UNFROZEN");
    pump(2);
    CHECK_EQ(app_storage().freezing_track(), -1);
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 0);

    // A MID track has nothing to freeze.
    post_cmd(Cmd::set_machine, uint8_t(MachineId::midi));          // track 1
    pump(2);
    menu_run(kMenuFreeze);
    CHECK_EQ(UiProbe::notice(app), "MID HAS NO AUDIO");
    CHECK_EQ(app_storage().freezing_track(), -1);
    post_cmd(Cmd::set_machine, uint8_t(MachineId::kick));
    pump(2);
}

TEST(ui_layout_a_frozen_track_says_so_on_edit_and_in_the_menu) {
    // Frozen is the audio side's word (Status::frozen); set by hand here, as the late stream is.
    Strata &app = fresh();
    Status &status = const_cast<Status &>(UiProbe::engine(app).status());
    status.frozen.store(1u, std::memory_order_relaxed);
    key(ENAGA_KEY_F8);
    pump(400);
    check_layout(app, "EDIT, frozen");
    menu_to(kMenuFreeze);
    pump(400);
    check_layout(app, "FUNC menu, FREEZE on");
    key(ENAGA_KEY_CLR);
    status.frozen.store(0u, std::memory_order_relaxed);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_menu_setting_rows_change_with_left_right_b_and_ok) {
    Strata &app = fresh();
    const Pattern &pattern = app_engine().pattern();

    // TRACK SPEED: the selected track's, X1 to the right is slower.
    const uint8_t speed = pattern.tracks[0].speed;
    menu_to(kMenuSpeed);
    key(ENAGA_KEY_RIGHT);
    pump(2);
    CHECK(pattern.tracks[0].speed != speed);
    key(ENAGA_KEY_LEFT);
    pump(2);
    CHECK_EQ(int(pattern.tracks[0].speed), int(speed));
    // OK steps forward and goes round, so one key reaches every speed.
    for (int i = 0; i < int(Speed::count); i++) {
        key(ENAGA_KEY_OK);
        pump(2);
    }
    CHECK_EQ(int(pattern.tracks[0].speed), int(speed));

    // AUTO-FILL wraps OFF 4 8 16.
    key(ENAGA_KEY_CLR);
    menu_to(kMenuAutofill);
    const int fill = int(app_engine().auto_fill());
    app.on_encoder(1, +1);                               // B, like RIGHT
    pump(2);
    CHECK_EQ(int(app_engine().auto_fill()), (fill + 1) % int(AutoFill::count));
    key(ENAGA_KEY_LEFT);
    pump(2);
    CHECK_EQ(int(app_engine().auto_fill()), fill);

    // QUANTIZE, by fives.
    key(ENAGA_KEY_DOWN);
    CHECK_EQ(UiProbe::menu_cursor(app), int(kMenuQuantize));
    const int q = UiProbe::quantize(app);
    key(q >= 100 ? ENAGA_KEY_LEFT : ENAGA_KEY_RIGHT);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), q >= 100 ? q - 5 : q + 5);
    key(q >= 100 ? ENAGA_KEY_RIGHT : ENAGA_KEY_LEFT);
    pump(2);
    CHECK_EQ(UiProbe::quantize(app), q);

    // An action row ignores LEFT and RIGHT.
    key(ENAGA_KEY_CLR);
    menu_to(kMenuReload);
    key(ENAGA_KEY_RIGHT);
    pump(2);
    CHECK(!app_storage().loading_project());
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_menu_reaches_kits_projects_and_save_as) {
    Strata &app = fresh();
    menu_run(kMenuKits);
    CHECK_EQ(UiProbe::mode(app), int(Mode::kit));
    pump_until_idle();

    menu_run(kMenuOpen);
    CHECK_EQ(UiProbe::mode(app), int(Mode::project));
    CHECK_EQ(UiProbe::project_page(app), 4);            // PROJECTS
    pump_until_idle();

    menu_run(kMenuSaveAs);
    CHECK(UiProbe::name_entry_open(app));
    key(ENAGA_KEY_CLR);                                  // cancel the entry
    CHECK(!UiProbe::name_entry_open(app));
    pump_until_idle();

    key(ENAGA_KEY_SELECT_LEFT);
    for (int i = 0; i < 4; i++) key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::project_page(app), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_each_button_group_has_one_job_in_project) {
    // The screen keys turn PROJECT's pages; the pad-row keys pick the bank the STEP keys mean.
    Strata &app = fresh();
    key(ENAGA_KEY_F3);
    CHECK_EQ(UiProbe::project_page(app), 0);
    const int bank = UiProbe::shown_bank(app);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::shown_bank(app), (bank + 1) % 8);
    CHECK_EQ(UiProbe::project_page(app), 0);
    CHECK_EQ(UiProbe::notice(app)[0], 'B');
    key(ENAGA_KEY_OCTAVE_LEFT);
    CHECK_EQ(UiProbe::shown_bank(app), bank);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::project_page(app), 1);
    CHECK_EQ(UiProbe::shown_bank(app), bank);
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::project_page(app), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_keys_and_grid_screen_keys_turn_the_knob_page) {
    Strata &app = fresh();
    const int before = UiProbe::param_page(app);
    open_keys();
    const int track = UiProbe::selected(app);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK_EQ(UiProbe::param_page(app), (before + 1) % kEditPages);
    CHECK_EQ(UiProbe::selected(app), track);              // no longer the track
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::param_page(app), before);
    key(ENAGA_KEY_F4);
}

TEST(ui_pad_row_keys_do_nothing_where_the_step_keys_never_change) {
    // KIT: the STEP keys are always the sixteen slots, so the pad-row keys are not passed on to
    // a GRID that is not on screen. FUNC + them is still the track's length, as everywhere.
    Strata &app = fresh();
    const int length = int(app_engine().pattern().tracks[0].length);
    menu_run(kMenuKits);
    pump_until_idle();
    key(ENAGA_KEY_OCTAVE_RIGHT);
    shift_key(ENAGA_KEY_OCTAVE_LEFT);
    pump(2);
    CHECK_EQ(UiProbe::page(app), 0);
    CHECK_EQ(int(app_engine().pattern().tracks[0].length), length - 1);
    CHECK_EQ(UiProbe::mode(app), int(Mode::kit));
    key(ENAGA_KEY_F4);
}

TEST(ui_perform_pad_row_keys_do_nothing_and_arrows_pick_the_macro) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F7);
    const int macro = UiProbe::macro_selected(app);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    CHECK_EQ(UiProbe::macro_selected(app), macro);
    key(ENAGA_KEY_RIGHT);
    CHECK_EQ(UiProbe::macro_selected(app), (macro + 1) % 4);
    key(ENAGA_KEY_LEFT);
    key(ENAGA_KEY_F4);
}

TEST(ui_the_performance_pads_are_lit_and_named) {
    Strata &app = fresh();
    pump(2);
    // Every one of the seven is lit, dim, while nothing is held.
    for (int pad = 1; pad <= 13; pad += 2) {
        CHECK(pad_level(app, pad) > 0);
        CHECK(pad_level(app, pad) <= 2);
    }

    app.on_pad_press(1, 100);                             // FILL
    pump(2);
    CHECK(pad_level(app, 1) >= 8);
    CHECK_EQ(UiProbe::notice(app), "FILL");
    app.on_pad_release(1);
    pump(2);
    CHECK(pad_level(app, 1) <= 2);

    app.on_pad_press(5, 100);                             // MUTE MODE stays bright while on
    app.on_pad_release(5);
    pump(2);
    CHECK(pad_level(app, 5) >= 8);
    app.on_pad_press(5, 100);
    app.on_pad_release(5);
    pump(2);
    CHECK(pad_level(app, 5) <= 2);

    app.on_pad_press(11, 100);                            // THROW
    CHECK_EQ(UiProbe::notice(app), "THROW DELAY");
    pump(2);
    CHECK(pad_level(app, 11) >= 8);
    app.on_pad_release(11);

    app.on_pad_press(7, 100);                             // SCENE A
    CHECK_EQ(UiProbe::notice(app), "SCENE A: STEP PICKS");
    app.on_pad_release(7);

    for (int i = 0; i < 16; i += 4) { step_down(i); step_up(i); }   // something to mutate
    pump(4);
    app.on_pad_press(13, 100);                            // MUTATE: names what it did
    app.on_pad_release(13);
    pump(2);
    CHECK(std::strncmp(UiProbe::notice(app), "MUTATE T1 ", 10) == 0);
    CHECK(pad_level(app, 13) >= 8);                       // pending: OK keeps, CLR reverts
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(pad_level(app, 13) <= 2);

    check_layout(app, "GRID after the performance pads");
    key(ENAGA_KEY_F4);
}

TEST(ui_the_mode_keys_light_where_you_are) {
    Strata &app = fresh();
    pump(2);
    CHECK(UiProbe::key_green(app, ENAGA_KEY_F4) > 0);    // GRID: HOME green
    CHECK_EQ(int(UiProbe::key_red(app, ENAGA_KEY_F4)), 0);
    open_keys();
    pump(2);
    CHECK(UiProbe::key_green(app, ENAGA_KEY_F4) > 0);    // KEYS: amber
    CHECK(UiProbe::key_red(app, ENAGA_KEY_F4) > 0);
    shift_key(ENAGA_KEY_F4);
    pump(2);
    CHECK(UiProbe::key_red(app, ENAGA_KEY_F4) > 0);      // the menu: red
    CHECK_EQ(int(UiProbe::key_green(app, ENAGA_KEY_F4)), 0);
    key(ENAGA_KEY_CLR);
    shift_key(ENAGA_KEY_F7);
    pump(2);
    CHECK(UiProbe::key_red(app, ENAGA_KEY_F7) > 0);      // PERFORM: MIXER red
    key(ENAGA_KEY_F7);
    pump(2);
    CHECK(UiProbe::key_green(app, ENAGA_KEY_F7) > 0);
    CHECK_EQ(int(UiProbe::key_red(app, ENAGA_KEY_F7)), 0);
    shift_key(ENAGA_KEY_F2);
    pump(2);
    CHECK(UiProbe::key_green(app, ENAGA_KEY_F2) > 0);    // RESAMPLE: REC green
    app.on_key(ENAGA_KEY_F1, true);
    pump(2);
    CHECK(UiProbe::key_green(app, ENAGA_KEY_F1) > 0);    // FUNC lit while held
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK_EQ(int(UiProbe::key_green(app, ENAGA_KEY_F1)), 0);
    key(ENAGA_KEY_F4);
}

TEST(ui_func_held_alone_shows_the_help_and_a_chord_does_not) {
    Strata &app = fresh();
    app.on_key(ENAGA_KEY_F1, true);
    pump(100);
    CHECK(!UiProbe::func_help(app));                     // not at once: a chord is quick
    pump(600);
    CHECK(UiProbe::func_help(app));
    check_layout(app, "FUNC help");
    app.on_key(ENAGA_KEY_F1, false);
    CHECK(!UiProbe::func_help(app));

    // FUNC + a key (here a pad mute) means the hand knew: no help however long FUNC stays down.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(2, 100);
    app.on_pad_release(2);
    pump(800);
    CHECK(!UiProbe::func_help(app));
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    app.on_key(ENAGA_KEY_F1, true);                      // and the mute back
    app.on_pad_press(2, 100);
    app.on_pad_release(2);
    app.on_key(ENAGA_KEY_F1, false);
    pump(4);
    key(ENAGA_KEY_F4);
}

// The help is for the screen that is up (Roland, 2026-11-23; core/help.hpp): its own page
// first, and the screen keys or LEFT/RIGHT, with FUNC still held, turn the pages without
// touching the screen's own.
namespace {

void hold_func(Strata &app) {
    app.on_key(ENAGA_KEY_F1, true);
    pump(700);
    CHECK(UiProbe::func_help(app));
}

void let_go_of_func(Strata &app) {
    app.on_key(ENAGA_KEY_F1, false);
    CHECK(!UiProbe::func_help(app));
}

// Every page of the help on the screen that is up, through the layout check, turning with the
// screen key. Ends on the page it started on, so it can be called twice in a row.
void check_help_layout(Strata &app, const char *where) {
    hold_func(app);
    for (int p = 0; p < 12; p++) {
        check_layout(app, where);
        if (screen_has(app, "PAGE 1/") && p > 0) break;   // round again: every page seen
        key(ENAGA_KEY_SELECT_RIGHT);
        CHECK(UiProbe::func_help(app));
    }
    let_go_of_func(app);
}

}  // namespace

TEST(ui_the_func_help_is_for_the_screen_that_is_up) {
    Strata &app = fresh();
    key(ENAGA_KEY_F4);                                   // GRID, a kick on track 1
    hold_func(app);
    CHECK(screen_has(app, "GRID SRC KCK"));
    CHECK(screen_has(app, "B FMA FM AMOUNT"));
    CHECK(screen_has(app, "SCREEN KEYS: PAGE 1/9"));
    CHECK(!screen_has(app, "FUNC + KEY:"));

    // The screen keys turn the help's pages while FUNC is held, and not GRID's knob page.
    const int knob_page = UiProbe::param_page(app);
    key(ENAGA_KEY_SELECT_RIGHT);
    CHECK(UiProbe::func_help(app));
    CHECK(screen_has(app, "STEPS:"));
    CHECK(screen_has(app, "PAGE 2/9"));
    CHECK_EQ(UiProbe::param_page(app), knob_page);
    // LEFT goes back, and round: the last page is the performance mixer's slots, the upper pads
    // two before it.
    key(ENAGA_KEY_LEFT);
    key(ENAGA_KEY_LEFT);
    CHECK(UiProbe::func_help(app));
    CHECK(screen_has(app, "VALUE FX STEPS:"));
    key(ENAGA_KEY_LEFT);
    CHECK(screen_has(app, "VALUE FX MIXER:"));
    key(ENAGA_KEY_LEFT);
    CHECK(screen_has(app, "UPPER PADS TRK1-7:"));
    key(ENAGA_KEY_SELECT_LEFT);
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK(screen_has(app, "FUNC + KEY:"));
    CHECK(screen_has(app, "MIXER  PERFORM"));
    CHECK_EQ(UiProbe::param_page(app), knob_page);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
    let_go_of_func(app);
    // The next hold opens on the screen's own page again.
    hold_func(app);
    CHECK(screen_has(app, "GRID SRC KCK"));
    let_go_of_func(app);

    // FUNC + a screen key before the help is up is still a chord that does nothing to the help.
    app.on_key(ENAGA_KEY_F1, true);
    key(ENAGA_KEY_SELECT_RIGHT);
    pump(800);
    CHECK(!UiProbe::func_help(app));
    app.on_key(ENAGA_KEY_F1, false);

    // EDIT names its page and machine; the snare's knobs are the snare's.
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::snare));
    pump(4);
    key(ENAGA_KEY_F8);
    hold_func(app);
    CHECK(screen_has(app, "EDIT SRC SNR"));
    CHECK(screen_has(app, "B SNP SNAPPY"));
    CHECK(screen_has(app, "PUSH A     NOTE VIEW"));
    let_go_of_func(app);

    // MIXER: the page that is up, the duck's here.
    mixer_page(app, 4);
    hold_func(app);
    CHECK(screen_has(app, "MIXER DUCK:"));
    CHECK(screen_has(app, "A SRC SOURCE TRACK"));
    let_go_of_func(app);
    CHECK_EQ(UiProbe::mixer_bank(app), 4);

    // PERFORM, then MACRO LEARN from it: EDIT's help says what a push does now.
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    hold_func(app);
    CHECK(screen_has(app, "PUSH A-D   LEARN"));
    let_go_of_func(app);
    key(ENAGA_KEY_PUSH_B);
    CHECK_EQ(UiProbe::macro_learn(app), 1);
    hold_func(app);
    CHECK(screen_has(app, "MACRO LEARN:"));
    let_go_of_func(app);
    key(ENAGA_KEY_CLR);                                  // LEARN ends
    CHECK_EQ(UiProbe::macro_learn(app), -1);

    // The step page (hold STEP + OK).
    key(ENAGA_KEY_F4);
    step_down(2);
    key(ENAGA_KEY_OK);
    step_up(2);
    CHECK(UiProbe::step_page(app));
    hold_func(app);
    CHECK(screen_has(app, "STEP PAGE:"));
    let_go_of_func(app);
    key(ENAGA_KEY_CLR);
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_every_page_of_the_func_help) {
    Strata &app = fresh();
    worst_case_content(app);
    key(ENAGA_KEY_F4);
    for (int page = 0; page < kEditPages; page++) {      // GRID, on each EDIT page A-D turn
        check_help_layout(app, "help on GRID");
        key(ENAGA_KEY_SELECT_RIGHT);
    }
    for (int machine = 0; machine < int(MachineId::count); machine++) {
        post_track(Cmd::set_machine, 0, uint8_t(machine));
        pump(4);
        key(ENAGA_KEY_F8);
        for (int page = 0; page < kEditPages; page++) {
            check_help_layout(app, machine_name(uint8_t(machine)));
            key(ENAGA_KEY_SELECT_RIGHT);
        }
    }
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    for (int page = 0; page < kMixerPageCount; page++) {
        mixer_page(app, page);
        check_help_layout(app, "help on MIXER");
    }
    shift_key(ENAGA_KEY_F7);
    check_help_layout(app, "help on PERFORM");
    key(ENAGA_KEY_PUSH_A);                               // MACRO LEARN
    check_help_layout(app, "help in MACRO LEARN");
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
    key(ENAGA_KEY_F4);                                   // HOME twice: KEYS
    if (UiProbe::mode(app) != int(Mode::keys)) key(ENAGA_KEY_F4);
    check_help_layout(app, "help on KEYS");
    key(ENAGA_KEY_F4);
    step_down(2);
    key(ENAGA_KEY_OK);
    step_up(2);
    check_help_layout(app, "help on the STEP PAGE");
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F3);
    for (int page = 0; page < 5; page++) {               // PROJECT CHAIN PATTERN SETUP PROJECTS
        check_help_layout(app, "help on PROJECT");
        key(ENAGA_KEY_SELECT_RIGHT);
    }
    shift_key(ENAGA_KEY_F8);
    check_help_layout(app, "help on MACHINE");
    shift_key(ENAGA_KEY_F8);
    check_help_layout(app, "help on KIT");
    shift_key(ENAGA_KEY_F2);
    check_help_layout(app, "help on RESAMPLE");
    shift_key(ENAGA_KEY_F3);
    check_help_layout(app, "help on SAMPLES");
    key(ENAGA_KEY_F8);
    key(ENAGA_KEY_OK);                                   // a kick: SOUNDS
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));
    check_help_layout(app, "help on SOUNDS");
    shift_key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    check_help_layout(app, "help on the FUNC menu");
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_layout_the_func_menu) {
    Strata &app = fresh();
    worst_case_content(app);
    shift_key(ENAGA_KEY_F4);
    check_layout(app, "FUNC menu, top");
    for (int i = 0; i < kMenuRowCount; i++) {
        key(ENAGA_KEY_DOWN);
        check_layout(app, "FUNC menu, scrolled");
    }
    for (int t = 0; t < kTracks; t++) {                  // the speed row's value at every speed
        menu_to(kMenuSpeed);
        for (int i = 0; i < int(Speed::count); i++) {
            key(ENAGA_KEY_OK);
            pump(2);
            check_layout(app, "FUNC menu, TRACK SPEED");
        }
        key(ENAGA_KEY_CLR);
        key(ENAGA_KEY_DOWN);                              // next track (GRID rows)
    }
    key(ENAGA_KEY_F4);
}

// --- the control audit: does every input show that it did something? ------------------------
//
// Roland pressed upper pad 7 on the unit and saw nothing happen. It had worked -- a velocity
// mutation of the selected track, named for a second and a half by a bare "VELOCITY" -- and
// nothing on the panel said so. So this walks every mode, presses every key, knob, knob push and
// pad in it, plain and with FUNC, and checks that the SCREEN or an LED changes, while the input
// is held or after it is let go. Sound alone does not count: on a stopped box, or on a part that
// is quiet, it is not there.

namespace {

struct Look {
    uint32_t screen;      // the pixels and every string drawn, hashed
    uint32_t leds;        // every LED level, hashed
};

uint32_t fnv(uint32_t h, const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

Look look(Strata &app) {
    FakeHost &host = FakeHost::the();
    enaga::Canvas canvas(g_pixels);
    host.clear_text();
    app.draw(canvas);
    Look out{2166136261u, 2166136261u};
    out.screen = fnv(out.screen, g_pixels, sizeof g_pixels);
    for (int i = 0; i < host.text_count(); i++) {
        const auto &d = host.text_draw(i);
        out.screen = fnv(out.screen, d.text, std::strlen(d.text));
        out.screen = fnv(out.screen, &d.x, sizeof d.x);
        out.screen = fnv(out.screen, &d.y, sizeof d.y);
    }
    for (int a = 0; a < int(ENAGA_LED_COUNT); a++) {
        const FakeHost::LedLevel level = host.led(a);
        out.leds = fnv(out.leds, &level, sizeof level);
    }
    return out;
}

bool same(const Look &a, const Look &b) { return a.screen == b.screen && a.leds == b.leds; }


enum Screen { s_grid, s_keys, s_edit, s_machine, s_kit, s_mixer, s_perform, s_project, s_chain,
              s_pattern, s_setup, s_projects, s_resample, s_menu, s_sounds, s_count };

const char *screen_name(int s) {
    static const char *names[] = {"GRID", "KEYS", "EDIT", "MACHINE", "KIT", "MIXER", "PERFORM",
                                  "PROJECT", "CHAIN", "PATTERN", "SETUP", "PROJECTS",
                                  "RESAMPLE", "MENU", "SOUNDS"};
    return names[s];
}

void enter(Strata &app, int s) {
    switch (s) {
        case s_grid: key(ENAGA_KEY_F4); break;
        case s_keys: open_keys(); break;
        case s_menu: key(ENAGA_KEY_F4); shift_key(ENAGA_KEY_F4); break;   // over GRID
        case s_edit:
            key(ENAGA_KEY_F8);
            while (UiProbe::param_page(app) != 0) key(ENAGA_KEY_SELECT_RIGHT);   // SRC
            break;
        case s_machine: shift_key(ENAGA_KEY_F8); break;
        case s_kit:
            shift_key(ENAGA_KEY_F8);
            shift_key(ENAGA_KEY_F8);
            key(ENAGA_KEY_STEP1);              // the cursor on slot 1: it outlives fresh()
            break;
        case s_mixer: mixer_page(app, 0); break;     // the faders, whatever was open last
        case s_perform: shift_key(ENAGA_KEY_F7); break;
        case s_resample:
            shift_key(ENAGA_KEY_F2);
            // From LINE, every time: B walks the sources and the walk outlives fresh(), and on
            // the master or a track (past HI-Z) C has nothing to set.
            for (int i = 0; i < kTakeSourceChoices; i++) app.on_encoder(1, -1);
            app.on_encoder(1, +1);
            break;
        case s_sounds: key(ENAGA_KEY_F8); key(ENAGA_KEY_OK); break;   // track 1, the kick
        default:
            key(ENAGA_KEY_F3);
            // docs/CONTROLS.md: the screen keys walk PROJECT's pages, the pad-row keys the bank.
            for (int i = 0; i < 6; i++) key(ENAGA_KEY_SELECT_LEFT);   // the slots, first
            for (int i = 0; i < 8 && UiProbe::shown_bank(app) != 0; i++) key(ENAGA_KEY_OCTAVE_RIGHT);
            for (int i = s_project; i < s; i++) key(ENAGA_KEY_SELECT_RIGHT);
            break;
    }
    (void)app;
}

// Hold the screen still before a probe, so nothing moving on its own is taken for an answer:
// every notice from getting here faded, the track-select note's tail cut (MIXER's PEAK falls
// with it), and the card read KIT starts with finished.
void settle_screen(int s) {
    post_cmd(Cmd::stop, 0, 0, 1);
    pump(2);
    // KIT reads its sixteen slots from the card a pass at a time, and says READING KITS until
    // all sixteen are in, not only the eight on its first page.
    for (int i = 0; s == s_kit && i < 2000; i++) {
        bool reading = false;
        for (int k = 0; k < Storage::kKitSlots; k++) {
            reading = reading || app_storage().kit_slot(k).state == Storage::SlotState::unknown;
        }
        if (!reading) break;
        pump();
    }
    // MIXER draws the master PEAK, which moves with whatever is still ringing -- and the master
    // delay is silent BETWEEN its repeats, so it has to stay at 0 for a run of blocks (the same
    // reasoning as tests/apprig.hpp's settle()).
    for (int i = 0, quiet = 0; s == s_mixer && i < 8000 && quiet < 400; i++) {
        pump();
        quiet = app_engine().status().peak_milli.load(std::memory_order_relaxed) == 0 ? quiet + 1
                                                                                      : 0;
    }
    g_ms += 4000;
    pump(2);
}

// A track with something on it, so that a mutation, a copy or a clear has material.
void some_content() {
    for (int i = 0; i < 16; i += 2) {
        step_down(i);
        step_up(i);
    }
    pump(4);
}

enum Input { in_key, in_turn_up, in_turn_down, in_pad, in_none };

struct Probe {
    Input kind;
    int id;
    const char *name;
};

const Probe kProbes[] = {
    {in_key, ENAGA_KEY_PUSH_A, "PUSH A"}, {in_key, ENAGA_KEY_PUSH_B, "PUSH B"},
    {in_key, ENAGA_KEY_PUSH_C, "PUSH C"}, {in_key, ENAGA_KEY_PUSH_D, "PUSH D"},
    {in_key, ENAGA_KEY_PUSH_VALUE, "PUSH VALUE"},
    {in_key, ENAGA_KEY_SELECT_LEFT, "SEL LEFT"}, {in_key, ENAGA_KEY_SELECT_RIGHT, "SEL RIGHT"},
    {in_key, ENAGA_KEY_OCTAVE_LEFT, "OCT LEFT"}, {in_key, ENAGA_KEY_OCTAVE_RIGHT, "OCT RIGHT"},
    {in_key, ENAGA_KEY_UP, "UP"}, {in_key, ENAGA_KEY_DOWN, "DOWN"},
    {in_key, ENAGA_KEY_LEFT, "LEFT"}, {in_key, ENAGA_KEY_RIGHT, "RIGHT"},
    {in_key, ENAGA_KEY_OK, "OK"}, {in_key, ENAGA_KEY_CLR, "CLR"},
    {in_key, ENAGA_KEY_F2, "REC"}, {in_key, ENAGA_KEY_F5, "PLAY"}, {in_key, ENAGA_KEY_F6, "STOP"},
    {in_key, ENAGA_KEY_STEP1 + 1, "STEP 2"},
    {in_turn_up, 0, "A+"}, {in_turn_up, 1, "B+"}, {in_turn_up, 2, "C+"}, {in_turn_up, 3, "D+"},
    {in_turn_up, 4, "VALUE+"},
    {in_turn_down, 0, "A-"}, {in_turn_down, 4, "VALUE-"},
    {in_pad, 2, "LOWER 2"},
    {in_pad, 1, "UPPER 1"}, {in_pad, 3, "UPPER 2"}, {in_pad, 5, "UPPER 3"},
    {in_pad, 7, "UPPER 4"}, {in_pad, 9, "UPPER 5"}, {in_pad, 11, "UPPER 6"},
    {in_pad, 13, "UPPER 7"},
};

void press(Strata &app, const Probe &p, bool down) {
    switch (p.kind) {
        case in_key: app.on_key(p.id, down); break;
        // A short turn, three clicks: one click of a 0-127 value can be less than a pixel of a
        // bar, and nobody turns a knob one click to find out what it does.
        case in_turn_up: if (down) app.on_encoder(p.id, +3); break;
        case in_turn_down: if (down) app.on_encoder(p.id, -3); break;
        case in_pad:
            if (down) app.on_pad_press(p.id, 100);
            else app.on_pad_release(p.id);
            break;
        case in_none:
            break;
    }
}

// Does this input, in this screen, change what is visible? Held, and after it is let go.
bool visible(int s, const Probe &p, bool func) {
    Strata &app = fresh();
    some_content();
    if (s == s_projects) two_projects();
    enter(app, s);
    settle_screen(s);
    // FUNC goes down after the screen has settled and a moment before the probe, well inside
    // the 0.6 s that FUNC held alone takes to show its help (which any input then takes away,
    // and so would pass for an answer).
    if (func) app.on_key(ENAGA_KEY_F1, true);
    pump(1);
    const Look before = look(app);
    press(app, p, true);
    pump(3);
    const Look held = look(app);
    press(app, p, false);
    pump(3);
    const Look after = look(app);
    if (func) app.on_key(ENAGA_KEY_F1, false);
    // A name entry the probe opened (PROJECTS: FUNC + DOWN, push B) takes every key until it
    // closes, and the app's UI state outlives fresh(): close it, or every later probe types.
    if (UiProbe::name_entry_open(app)) {
        app.on_key(ENAGA_KEY_CLR, true);
        app.on_key(ENAGA_KEY_CLR, false);
    }
    return !same(before, held) || !same(before, after);
}

}  // namespace

namespace {

// The inputs that are ALLOWED to change nothing visible, each for a reason. Anything else that
// does is a key that looks broken on the unit, which is the bug this audit exists to catch; an
// entry here that starts doing something is reported too, so the list cannot go stale.
const char *quiet_reason(int s, const Probe &p, bool func) {
    const bool knob_push = p.kind == in_key && p.id <= ENAGA_KEY_PUSH_D;
    // The rules below allow a whole class of input to be quiet without requiring it (a knob
    // push that does have a job somewhere is fine); the table after them is exact.
    // A knob push on its own removes a lock only with a STEP held, and kills a band only on
    // MIXER's EQ page (and with FUNC, anywhere -- which does show).
    if (knob_push && !func) return "push: needs a held STEP";
    // VALUE pushed is a modifier: pushed AND turned is the swing.
    if (p.kind == in_key && p.id == ENAGA_KEY_PUSH_VALUE) return "push VALUE: a modifier";
    // docs/CONTROLS.md: FUNC + LEFT / RIGHT are free everywhere (PROJECT's card actions moved
    // to the FUNC menu).
    if (func && p.kind == in_key && (p.id == ENAGA_KEY_LEFT || p.id == ENAGA_KEY_RIGHT)) {
        return "FUNC + LEFT/RIGHT: free";
    }

    // docs/CONTROLS.md: the screen keys turn pages and have no FUNC job (TRACK SPEED, which
    // was FUNC + screen key, is in the FUNC menu).
    if (func && p.kind == in_key &&
        (p.id == ENAGA_KEY_SELECT_LEFT || p.id == ENAGA_KEY_SELECT_RIGHT)) {
        return "FUNC + screen key: free";
    }
    // The FUNC menu is a list: the arrows, A, B, OK and CLR. The STEP keys, the screen keys and
    // the pad-row keys do nothing in it, plain or with FUNC, and C and D have nothing to turn.
    if (s == s_menu && p.kind == in_key &&
        (p.id == ENAGA_KEY_SELECT_LEFT || p.id == ENAGA_KEY_SELECT_RIGHT ||
         p.id == ENAGA_KEY_OCTAVE_LEFT || p.id == ENAGA_KEY_OCTAVE_RIGHT ||
         p.id == ENAGA_KEY_STEP1 + 1)) {
        return "menu: no job";
    }
    if (s == s_menu && p.kind == in_turn_up && (p.id == 2 || p.id == 3)) return "menu: no job";

    static const struct { int screen; bool func; const char *name; const char *why; } kQuiet[] = {
        // At the end of a range, from where every probe starts.
        {s_grid, false, "OCT LEFT", "already step page 1"},
        {s_edit, false, "OCT LEFT", "already step page 1"},
        {s_machine, false, "OCT LEFT", "already step page 1"},
        {s_kit, false, "SEL LEFT", "the cursor is on slot 1"},
        {s_kit, false, "LEFT", "ditto"}, {s_kit, false, "UP", "ditto"},
        {s_kit, false, "A-", "ditto"}, {s_kit, true, "A-", "ditto"},
        // docs/CONTROLS.md: the pad-row keys choose which sixteen the STEP keys mean, and in
        // KIT, SAMPLES, PERFORM and the menu those sixteen never change.
        {s_kit, false, "OCT LEFT", "the STEP keys always mean the same"},
        {s_kit, false, "OCT RIGHT", "ditto"},
        {s_perform, false, "OCT LEFT", "ditto"}, {s_perform, false, "OCT RIGHT", "ditto"},
        // The cursor starts on SAVE, which has no value for LEFT / RIGHT or B to change.
        {s_menu, false, "LEFT", "SAVE has no value"}, {s_menu, false, "RIGHT", "ditto"},
        {s_menu, false, "B+", "ditto"}, {s_menu, true, "B+", "ditto"},
        // FUNC + UP / DOWN is copy / paste on the editing screens and read / store on the card
        // lists. PERFORM's auto-fill and PROJECT's chain and sample reload moved to the menu.
        {s_perform, true, "UP", "auto-fill is in the FUNC menu"},
        {s_perform, true, "DOWN", "ditto"},
        {s_project, true, "UP", "chain and sample reload are in the FUNC menu"},
        {s_project, true, "DOWN", "ditto"},
        {s_chain, true, "UP", "CHAIN on / off is in the FUNC menu"},
        {s_chain, true, "DOWN", "ditto"},
        {s_pattern, true, "UP", "ditto"}, {s_pattern, true, "DOWN", "ditto"},
        {s_setup, true, "UP", "SETUP only shows"}, {s_setup, true, "DOWN", "ditto"},
        {s_menu, true, "UP", "no copy / paste in a list"}, {s_menu, true, "DOWN", "ditto"},
        {s_mixer, false, "OCT LEFT", "already step page 1"},
        {s_projects, false, "DOWN", "the cursor is on the last project"},
        {s_projects, false, "RIGHT", "ditto"},
        {s_projects, false, "A+", "ditto"},
        {s_projects, true, "A+", "ditto"},
        {s_resample, false, "OCT LEFT", "already step page 1"},
        {s_project, false, "UP", "the slot cursor is on the top row"},
        {s_pattern, false, "LEFT", "the cursor is on the first field"},
        {s_chain, false, "UP", "an empty chain has no entry to move to"},
        {s_chain, false, "DOWN", "an empty chain has no entry to move to"},
        // docs/CHEATSHEET.md marks these "--": no job in that screen.
        {s_grid, false, "OK", "no job (hold STEP + OK opens the step page)"},
        {s_keys, false, "OK", "no job"},
        {s_pattern, false, "OK", "no job"},
        {s_mixer, false, "UP", "no job"}, {s_mixer, false, "DOWN", "no job"},
        {s_mixer, false, "OK", "no job"},
        // Knobs a screen does not use. Each is stopped from falling through to the sound
        // parameters of a screen that is not showing, which is deliberate (app/app.hpp).
        {s_machine, false, "C+", "MACHINE uses A and B"}, {s_machine, false, "D+", "ditto"},
        {s_machine, true, "C+", "ditto"}, {s_machine, true, "D+", "ditto"},
        {s_project, false, "D+", "PROJECT uses A to C"}, {s_project, true, "D+", "ditto"},
        {s_chain, false, "C+", "CHAIN uses A and B"}, {s_chain, false, "D+", "ditto"},
        {s_chain, true, "C+", "ditto"}, {s_chain, true, "D+", "ditto"},
        {s_pattern, false, "D+", "PATTERN uses A to C"}, {s_pattern, true, "D+", "ditto"},
        {s_resample, false, "D+", "RESAMPLE uses A to C"}, {s_resample, true, "D+", "ditto"},
        {s_kit, false, "B+", "KIT uses A"}, {s_kit, false, "C+", "ditto"},
        {s_kit, false, "D+", "ditto"}, {s_kit, true, "B+", "ditto"},
        {s_kit, true, "C+", "ditto"}, {s_kit, true, "D+", "ditto"},
        {s_projects, false, "B+", "PROJECTS uses A"}, {s_projects, false, "C+", "ditto"},
        {s_projects, false, "D+", "ditto"}, {s_projects, true, "B+", "ditto"},
        {s_projects, true, "C+", "ditto"}, {s_projects, true, "D+", "ditto"},
        // SOUNDS opens with the cursor on the preset the track matches, or on AS WAS, the top
        // row, when it matches none -- as the test kit's kick does. A walks it; B to D have nothing.
        {s_sounds, false, "UP", "the cursor is on AS WAS"}, {s_sounds, false, "LEFT", "ditto"},
        {s_sounds, false, "A-", "ditto"}, {s_sounds, true, "A-", "ditto"},
        {s_sounds, false, "OCT LEFT", "the STEP keys always mean the same"},
        {s_sounds, false, "OCT RIGHT", "ditto"},
        {s_sounds, true, "UP", "no copy / paste in a list"}, {s_sounds, true, "DOWN", "ditto"},
        {s_sounds, false, "B+", "SOUNDS uses A"}, {s_sounds, false, "C+", "ditto"},
        {s_sounds, false, "D+", "ditto"}, {s_sounds, true, "B+", "ditto"},
        {s_sounds, true, "C+", "ditto"}, {s_sounds, true, "D+", "ditto"},
        // SETUP shows settings that live in the Enaga menu; only A (quantize) is STRATA's.
        {s_setup, false, "UP", "SETUP only shows"}, {s_setup, false, "DOWN", "ditto"},
        {s_setup, false, "LEFT", "ditto"}, {s_setup, false, "RIGHT", "ditto"},
        {s_setup, false, "OK", "ditto"},
        {s_setup, false, "B+", "ditto"}, {s_setup, false, "C+", "ditto"},
        {s_setup, false, "D+", "ditto"}, {s_setup, true, "B+", "ditto"},
        {s_setup, true, "C+", "ditto"}, {s_setup, true, "D+", "ditto"},
    };
    for (const auto &q : kQuiet) {
        if (q.screen == s && q.func == func && std::strcmp(q.name, p.name) == 0) return q.why;
    }
    return nullptr;
}

bool is_rule(const char *why) {
    return std::strncmp(why, "push", 4) == 0 || std::strncmp(why, "FUNC + LEFT", 11) == 0;
}

}  // namespace

TEST(ui_audit_every_input_in_every_screen_shows_it_did_something) {
    // First, that the screens hold still with no input at all: otherwise a meter or a clock
    // moving would pass for an answer and the audit would prove nothing.
    for (int s = 0; s < s_count; s++) {
        const Probe nothing = {in_none, 0, "nothing"};
        for (int func = 0; func < 2; func++) {
            const bool moved = visible(s, nothing, func != 0);
            if (moved) std::printf("    %s%s moves with no input\n", func ? "FUNC+" : "", screen_name(s));
            CHECK(!moved);
        }
    }
    for (int func = 0; func < 2; func++) {
        for (int s = 0; s < s_count; s++) {
            for (const Probe &p : kProbes) {
                const bool shows = visible(s, p, func != 0);
                const char *why = quiet_reason(s, p, func != 0);
                if (why != nullptr && is_rule(why) && shows) continue;
                if (!shows && why == nullptr) {
                    std::printf("    %s: %s%s changes nothing on screen or LEDs\n",
                                screen_name(s), func ? "FUNC+" : "", p.name);
                }
                if (shows && why != nullptr) {
                    std::printf("    %s: %s%s now shows something: drop it from the list\n",
                                screen_name(s), func ? "FUNC+" : "", p.name);
                }
                CHECK(shows == (why == nullptr));
            }
        }
    }
}

// --- MUTATE on the unit: upper pad 7 "did nothing" ------------------------------------------

namespace {

FakeHost::LedLevel pad_light(int pad) { return FakeHost::the().led(ENAGA_LED_PAD + pad); }

bool lit(int pad, uint8_t r, uint8_t g, uint8_t b) {
    const FakeHost::LedLevel level = pad_light(pad);
    return level.r == r && level.g == g && level.b == b;
}

// docs/CONTROLS.md: an upper pad is always lit in its own colour, at 2 idle and 10 while held or
// on, so the row reads as tools and each says which one it is.
bool lit_dim(int pad) {
    const Rgb c = kPerformColour[pad / 2];
    return lit(pad, uint8_t(c.r * 2 / 10), uint8_t(c.g * 2 / 10), uint8_t(c.b * 2 / 10));
}

bool lit_bright(int pad) {
    const Rgb c = kPerformColour[pad / 2];
    return lit(pad, c.r, c.g, c.b);
}

void fade_notice() {
    g_ms += 4000;
    pump(2);
}

}  // namespace

TEST(ui_mutate_says_what_it_did_and_waits_for_ok_or_clr_on_screen_and_on_the_pad) {
    Strata &app = fresh();
    for (int i = 0; i < 16; i += 4) {                 // trigs for the velocity mutation to move
        step_down(i);
        step_up(i);
    }
    pump(4);
    fade_notice();
    CHECK(lit_dim(13));                               // available: its colour at 2

    app.on_pad_press(13, 100);                        // upper 7
    app.on_pad_release(13);
    pump(4);
    // What it did and to which track -- the bare "VELOCITY" did not read as anything happening.
    CHECK(screen_has(app, "MUTATE T1 VELOCITY"));
    check_layout(app, "GRID, MUTATE notice");
    CHECK(lit_bright(13));                            // waiting for a verdict: its colour at 10

    // The notice fades; the question does not.
    fade_notice();
    CHECK(screen_has(app, "OK KEEPS CLR REVERTS"));
    check_layout(app, "GRID, MUTATE waiting");
    key(ENAGA_KEY_F8);                                // EDIT has it on its second line too
    pump(2);
    // A mode change settles a pending mutation (leave_mode), so the hint goes with it.
    CHECK(!screen_has(app, "OK KEEPS CLR REVERTS"));
    CHECK(lit_dim(13));

    // In EDIT: press, fade, and the hint is on the second line.
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    pump(4);
    fade_notice();
    CHECK(screen_has(app, "OK KEEPS CLR REVERTS"));
    check_layout(app, "EDIT, MUTATE waiting");
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(screen_has(app, "KEPT"));
    fade_notice();
    CHECK(!screen_has(app, "OK KEEPS CLR REVERTS"));
    CHECK(lit_dim(13));
}

TEST(ui_mutate_on_an_empty_track_says_there_was_nothing_to_mutate) {
    Strata &app = fresh();                            // track 1 is blank
    const uint8_t depth = undo_depth();
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    pump(4);
    CHECK(screen_has(app, "T1 NOTHING TO MUTATE"));
    check_layout(app, "GRID, nothing to mutate");
    CHECK(lit_dim(13));                               // nothing is waiting for a verdict
    fade_notice();
    CHECK(!screen_has(app, "OK KEEPS CLR REVERTS"));
    // So CLR is CLR again, not a "REVERTED" of a mutation that never happened.
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(!screen_has(app, "REVERTED"));
    (void)depth;
}

TEST(ui_the_upper_pads_light_dim_in_their_colour_and_bright_while_active) {
    Strata &app = fresh();
    pump(2);
    for (int pad = 1; pad <= 13; pad += 2) CHECK(lit_dim(pad));

    app.on_pad_press(1, 100);                         // FILL, held
    pump(2);
    CHECK(lit_bright(1));
    app.on_pad_release(1);
    pump(2);
    CHECK(lit_dim(1));

    app.on_pad_press(11, 100);                        // THROW, held
    pump(2);
    CHECK(lit_bright(11));
    app.on_pad_release(11);
    pump(2);
    CHECK(lit_dim(11));

    app.on_pad_press(5, 100);                         // MUTE MODE: a toggle, lit while on
    app.on_pad_release(5);
    pump(2);
    CHECK(lit_bright(5));
    app.on_pad_press(5, 100);
    app.on_pad_release(5);
    pump(2);
    CHECK(lit_dim(5));

    app.on_key(ENAGA_KEY_F1, true);                   // FUNC + FILL: NOTE RPT stays lit
    app.on_pad_press(1, 100);
    app.on_pad_release(1);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(lit_bright(1));
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(1, 100);
    app.on_pad_release(1);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(lit_dim(1));
}

// The other inputs the audit above found working with nothing on screen.
TEST(ui_inputs_that_worked_unseen_now_say_what_they_did) {
    Strata &app = fresh();

    // FUNC + pad-row key outside GRID: the track's length, live. (The speed is a FUNC menu row,
    // which shows its own value.)
    key(ENAGA_KEY_F8);
    shift_key(ENAGA_KEY_OCTAVE_LEFT);                 // a blank track is 64 long
    pump(2);
    CHECK(screen_has(app, "T1 LEN 63 X1"));           // the value AFTER the audio side applied it
    check_layout(app, "EDIT, length");
    shift_key(ENAGA_KEY_OCTAVE_RIGHT);                // put it back
    pump(2);
    CHECK(screen_has(app, "T1 LEN 64 X1"));

    // FUNC + a lower pad: the mute waits for the bar, so say it is armed.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(2, 100);
    app.on_pad_release(2);
    app.on_key(ENAGA_KEY_F1, false);
    pump(2);
    CHECK(screen_has(app, "T2 MUTE AT BAR"));
    check_layout(app, "EDIT, mute at bar");

    // KEYS has no knob slots: a turn says which knob and where it is now.
    open_keys();
    pump(2);
    app.on_encoder(0, +3);
    pump(2);
    CHECK(UiProbe::notice_showing(app));
    CHECK(std::strncmp(UiProbe::notice(app), "T1 ", 3) == 0);
    CHECK(std::strstr(UiProbe::notice(app), "LEN") == nullptr);
    CHECK(screen_has(app, UiProbe::notice(app)));
    check_layout(app, "KEYS, knob");

    // PERFORM: a macro turn names it, and says when it drives nothing.
    shift_key(ENAGA_KEY_F7);
    pump(2);
    while (UiProbe::macro_knob(app, 1) > 0) app.on_encoder(1, -64);   // the knob outlives fresh()
    app.on_encoder(1, +3);
    pump(2);
    CHECK(screen_has(app, "MACRO B 6"));               // three clicks of two
    check_layout(app, "PERFORM, macro");

    // CHAIN: the bank the pad-row keys pick, which CHAIN does not draw.
    key(ENAGA_KEY_F3);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_SELECT_LEFT);       // the slots, first
    key(ENAGA_KEY_SELECT_RIGHT);                                   // CHAIN
    for (int i = 0; i < 8 && UiProbe::shown_bank(app) != 0; i++) {
        key(ENAGA_KEY_OCTAVE_RIGHT);                               // it outlives fresh()
    }
    CHECK_EQ(UiProbe::shown_bank(app), 0);
    key(ENAGA_KEY_OCTAVE_RIGHT);
    pump(2);
    CHECK(screen_has(app, "BANK B"));
    key(ENAGA_KEY_OCTAVE_LEFT);

    // STOP on a stopped box arms the cut, and the second press cuts.
    key(ENAGA_KEY_F6);
    pump(2);
    CHECK(screen_has(app, "STOP") || screen_has(app, "TAILS CUT"));
    key(ENAGA_KEY_F6);
    pump(2);
    CHECK(screen_has(app, "STOP") || screen_has(app, "TAILS CUT"));
}

// --- screenshots for the README, the manual and the cheatsheet --------------------------------
//
// Not a check. With STRATA_SHOTS set to a folder, this draws every screen on the default beat
// and writes each one there as NAME.pgm (the canvas: every rect, greys 0-15) and NAME.txt (every
// string drawn, "x y grey text" a line). tools/screenshots.sh runs it alone, with --only, so the
// app is on its first init and still holds the default beat, then lays the text over the canvas
// in Enaga's own 5 x 7 font (read from third_party/enaga/firmware/hal/gfx.c) and writes PNGs.
// Without STRATA_SHOTS it returns at once, so the suite is unchanged. It sits last in the file
// because it uses the sample browser's card helpers above.

namespace {

// Blocks with the clock running at 130 BPM: 96 PPQN is 208 ticks a second, 0.277 a block. The
// fake clock delivers no ticks by itself, so without this the beat would stand still.
uint32_t g_shot_tick = 0;
float g_shot_due = 0.0f;

void play_blocks(int blocks) {
    FakeHost &host = FakeHost::the();
    constexpr float kTicksPerBlock = 130.0f / 60.0f * 96.0f * float(kBlk) / 48000.0f;
    for (int b = 0; b < blocks; b++) {
        g_shot_due += kTicksPerBlock;
        if (host.running() && g_shot_due >= 1.0f) {
            g_shot_due -= 1.0f;
            host.deliver_tick(g_shot_tick++, 0);
        }
        pump(1);
    }
}

// `settle` blocks are played first: by default past any notice (1.5 s) the last key left, fewer
// to catch the notice itself.
void write_shot(const char *folder, const char *name, int settle = 1600) {
    FakeHost &host = FakeHost::the();
    enaga::Canvas canvas(g_pixels);
    play_blocks(settle);
    host.clear_text();
    the_app().draw(canvas);

    char path[512];
    std::snprintf(path, sizeof path, "%s/%s.pgm", folder, name);
    FILE *pgm = std::fopen(path, "wb");
    CHECK(pgm != nullptr);
    if (pgm == nullptr) return;
    std::fprintf(pgm, "P5\n%d %d\n15\n", enaga::Canvas::width, enaga::Canvas::height);
    std::fwrite(g_pixels, 1, sizeof g_pixels, pgm);
    std::fclose(pgm);

    std::snprintf(path, sizeof path, "%s/%s.txt", folder, name);
    FILE *txt = std::fopen(path, "w");
    CHECK(txt != nullptr);
    if (txt == nullptr) return;
    for (int i = 0; i < host.text_count(); i++) {
        const FakeHost::TextDraw &d = host.text_draw(i);
        std::fprintf(txt, "%d %d %d %s\n", d.x, d.y, int(d.grey), d.text);
    }
    std::fclose(txt);
}

}  // namespace

TEST(ui_screenshots) {
    const char *folder = std::getenv("STRATA_SHOTS");
    if (folder == nullptr || folder[0] == '\0') return;

    // The app's first init, as on the unit: the default beat, not `fresh()`'s blank project.
    FakeHost &host = FakeHost::the();
    host.reset();
    host.set_device(ENAGA_DEVICE_SMPLTREK);
    host.set_audio_load(200);                   // 20.0%, near the emulator's cl6x figure for the beat
    Strata &app = the_app();
    CHECK(app.init());
    app.on_card(true);

    key(ENAGA_KEY_F4);                          // GRID, track 1
    app.on_pad_press(0, 100);
    app.on_pad_release(0);
    key(ENAGA_KEY_F5);                          // play, so the playhead shows
    play_blocks(900);
    write_shot(folder, "grid");

    // FUNC held alone: after 0.6 s the help over the screen (docs/CONTROLS.md, FUNC help).
    app.on_key(ENAGA_KEY_F1, true);
    write_shot(folder, "help", 800);
    CHECK(UiProbe::func_help(app));
    // The screen keys turn it: back three pages is FUNC + each function key, on every screen.
    for (int i = 0; i < 3; i++) key(ENAGA_KEY_SELECT_LEFT);
    write_shot(folder, "help_keys", 1);
    CHECK(UiProbe::func_help(app));
    app.on_key(ENAGA_KEY_F1, false);

    shift_key(ENAGA_KEY_F4);                    // the FUNC menu, cursor on SAVE
    write_shot(folder, "menu");
    CHECK_EQ(UiProbe::mode(app), int(Mode::menu));
    key(ENAGA_KEY_CLR);

    // MUTATE (upper pad 7) waits for OK or CLR and says so for as long as it waits.
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    write_shot(folder, "mutate", 200);
    key(ENAGA_KEY_CLR);                         // and reverts it, so the beat is the default again

    // SCENE A (upper pad 4) held: the notice says the STEP keys pick a scene.
    app.on_pad_press(7, 100);
    write_shot(folder, "scene", 40);
    app.on_pad_release(7);

    step_down(0);                               // the step page for track 1, step 1
    key(ENAGA_KEY_OK);
    step_up(0);
    write_shot(folder, "step");
    key(ENAGA_KEY_F4);

    key(ENAGA_KEY_F8);                          // the sound pages of the kick
    write_shot(folder, "edit");
    const char *const edit_pages[] = {"edit_fltr", "edit_amp", "edit_fx", "edit_lfo", "edit_arp"};
    for (const char *page : edit_pages) {
        key(ENAGA_KEY_SELECT_RIGHT);
        write_shot(folder, page);
    }
    // The ASR page on track 7 (VA POLY), set up as a pad; then everything back as it was, so the
    // shots after this one draw what they always drew.
    key(ENAGA_KEY_SELECT_RIGHT);
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_DOWN);
    const uint8_t pad[kAsrParams] = {72, 89, 90, 40};    // ~400 MS, 70%, ~1.9 S, 31%
    for (int i = 0; i < kAsrParams; i++) post_cmd_track_param(6, uint8_t(kAsrFirst + i), pad[i]);
    pump();
    write_shot(folder, "edit_asr");
    for (int i = 0; i < kAsrParams; i++) post_cmd_track_param(6, uint8_t(kAsrFirst + i), 0);
    // And the OSC page on the same track: a square, OSC 2 detuned, a little sub and drive.
    key(ENAGA_KEY_SELECT_RIGHT);
    const TrackParams before_osc = app_engine().params(6);
    const uint8_t osc[kOscParams] = {32, 30, 40, 30};    // SQR, D14, 40, 30
    for (int i = 0; i < kOscParams; i++) post_cmd_track_param(6, uint8_t(kOscFirst + i), osc[i]);
    pump();
    write_shot(folder, "edit_osc");
    for (int i = 0; i < kOscParams; i++) {
        post_cmd_track_param(6, uint8_t(kOscFirst + i), before_osc.v[kOscFirst + i]);
    }
    key(ENAGA_KEY_SELECT_LEFT);
    pump();
    for (int i = 0; i < 6; i++) key(ENAGA_KEY_UP);
    key(ENAGA_KEY_SELECT_LEFT);

    mixer_page(app, 0);
    write_shot(folder, "mixer");
    mixer_page(app, kMixerDelayPage);
    write_shot(folder, "mixer_dly");
    mixer_page(app, kMixerDuckDepthPage);
    write_shot(folder, "mixer_dck");
    mixer_page(app, kMixerDuckPage);
    write_shot(folder, "mixer_duck");
    mixer_page(app, kMixerPageCount - 2);
    write_shot(folder, "mixer_mst");
    mixer_page(app, kMixerEqPage);
    write_shot(folder, "mixer_eq");

    shift_key(ENAGA_KEY_F7);
    write_shot(folder, "perform");

    open_keys();
    write_shot(folder, "keys");

    shift_key(ENAGA_KEY_F8);
    write_shot(folder, "machine");

    shift_key(ENAGA_KEY_F2);                    // FUNC + REC: RESAMPLE
    write_shot(folder, "resample");
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));

    key(ENAGA_KEY_F3);
    write_shot(folder, "project");
    // A project on the card, so PROJECTS has one to list.
    CHECK(app_storage().request_save_as("TECHNO"));
    CHECK(pump_until_idle());
    const char *const project_pages[] = {"chain", "pattern", "setup", "projects"};
    for (int p = 0; p < 4; p++) {
        key(ENAGA_KEY_SELECT_RIGHT);
        for (int i = 0; i < 400 && app_storage().browsing(); i++) pump();
        CHECK_EQ(UiProbe::project_page(app), p + 1);
        write_shot(folder, project_pages[p]);
    }

    // The kit browser, with the factory kit stored in slot 1 so the screen has a kit to show.
    shift_key(ENAGA_KEY_F8);
    shift_key(ENAGA_KEY_F8);
    CHECK(pump_until_idle());
    shift_key(ENAGA_KEY_DOWN);
    for (int i = 0; i < 4000 && app_storage().kit_slot(0).state != Storage::SlotState::used; i++) {
        pump();
    }
    CHECK_EQ(int(app_storage().kit_slot(0).state), int(Storage::SlotState::used));
    pump(2100);                                 // let the SAVING KIT notice lapse
    write_shot(folder, "kit");

    // The sample browser on track 8, the default beat's SAMPLE track, with a few files on the card.
    seed_sample_card();
    key(ENAGA_KEY_F4);
    app.on_pad_press(14, 100);
    app.on_pad_release(14);
    key(ENAGA_KEY_F8);
    key(ENAGA_KEY_OK);
    for (int i = 0; i < 400 && app_storage().browsing(); i++) pump();
    CHECK_EQ(UiProbe::mode(app), int(Mode::samples));
    write_shot(folder, "samples");
    key(ENAGA_KEY_CLR);

    // The factory sounds on track 1, the default beat's kick: AS WAS, then 909 marked NOW.
    key(ENAGA_KEY_F4);
    app.on_pad_press(0, 100);
    app.on_pad_release(0);
    key(ENAGA_KEY_F8);
    key(ENAGA_KEY_OK);
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));
    write_shot(folder, "sounds");
    key(ENAGA_KEY_CLR);

    key(ENAGA_KEY_F6);
    key(ENAGA_KEY_F4);
    host.set_device(0);
    host.set_audio_load(0);
}

// --- KEYS on the pads: the two pressure-sensitive rows are a piano ---------------------------
//
// Roland on the unit: the STEP keys are not where you play. The two pad rows are, being
// pressure-sensitive and laid out like a keyboard, upper seven between lower eight.

TEST(ui_keys_plays_the_pads_as_a_piano) {
    Strata &app = fresh();
    open_keys();
    const int c = UiProbe::pad_note(app, 0);
    CHECK_EQ(c % 12, 0);                                   // lower 1 is a C
    // Lower row: the white keys C D E F G A B C.
    const int white[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    for (int i = 0; i < 8; i++) CHECK_EQ(UiProbe::pad_note(app, 2 * i) - c, white[i]);
    // Upper row, between them: C# D#, no key between E and F, F# G# A#, none after B.
    const int black[7] = {1, 3, -1, 6, 8, 10, -1};
    for (int i = 0; i < 7; i++) {
        const int note = UiProbe::pad_note(app, 2 * i + 1);
        CHECK_EQ(note < 0 ? -1 : note - c, black[i]);
    }

    // A pad holds its note until it comes up, on the selected track, and says which note.
    key(ENAGA_KEY_DOWN);                                   // track 2
    CHECK_EQ(UiProbe::selected(app), 1);
    app.on_pad_press(4, 90);                               // E
    CHECK_EQ(UiProbe::pad_held_note(app, 4), c + 4);
    CHECK_EQ(UiProbe::selected(app), 1);                   // a pad is a note, not a track
    CHECK(std::strncmp(UiProbe::notice(app), "T2 E", 4) == 0);
    pump(2);
    check_layout(app, "KEYS, a pad held");
    app.on_pad_release(4);
    CHECK_EQ(UiProbe::pad_held_note(app, 4), -1);

    // The two upper pads with no black key walk the octave; the rest of the upper row is notes,
    // not FILL or MUTATE.
    const int octave = UiProbe::keys_octave(app);
    app.on_pad_press(13, 100);
    app.on_pad_release(13);
    CHECK_EQ(UiProbe::keys_octave(app), octave + 1);
    CHECK_EQ(UiProbe::pad_note(app, 0), c + 12);
    app.on_pad_press(5, 100);
    app.on_pad_release(5);
    CHECK_EQ(UiProbe::keys_octave(app), octave);
    app.on_pad_press(1, 100);                              // C#, not FILL
    CHECK_EQ(UiProbe::pad_held_note(app, 1), c + 1);
    CHECK(!UiProbe::fill_held(app));
    // Leaving KEYS with a pad down lets the note go.
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::pad_held_note(app, 1), -1);
    app.on_pad_release(1);
    pump(2);
}

TEST(ui_keys_pads_record_at_their_own_velocity) {
    Strata &app = fresh();
    open_keys();
    FakeHost &host = FakeHost::the();
    host.set_running(true);
    host.set_position(96);
    key(ENAGA_KEY_F2);                                     // REC on
    const int note = UiProbe::pad_note(app, 8);            // G
    app.on_pad_press(8, 57);
    app.on_pad_release(8);
    pump(2);
    int trigs = 0, where = -1;
    for (int s = 0; s < kStepsPerTrack; s++) {
        if (trig_at(0, s)) { trigs++; where = s; }
    }
    CHECK_EQ(trigs, 1);
    CHECK(where >= 0);
    CHECK_EQ(int(step_of(0, where).note), note);
    CHECK_EQ(int(step_of(0, where).velocity), 57);        // the pad's, not a key's 100
    key(ENAGA_KEY_F2);
    host.set_running(false);
    (void)app;
}

TEST(ui_keys_lights_the_pads_as_a_keyboard_and_func_keeps_the_pad_jobs) {
    Strata &app = fresh();
    open_keys();
    pump(2);
    // Whatever key and scale the project has: the root bright, the scale's other notes dim, the
    // rest dark, the two octave pads dim white.
    const uint8_t root = app_engine().key_root();
    const int scale = int(app_engine().key_scale());
    int dark = 0;
    for (int pad = 0; pad < 15; pad++) {
        const int sum = UiProbe::pad_r(app, pad) + UiProbe::pad_g(app, pad) + UiProbe::pad_b(app, pad);
        const int note = UiProbe::pad_note(app, pad);
        if (note < 0) {
            CHECK_EQ(int(UiProbe::pad_r(app, pad)), 2);
            continue;
        }
        const bool in_scale = scale_fold(uint8_t(note), root, scale) == uint8_t(note);
        if (note % 12 == root) {
            CHECK(sum > UiProbe::pad_r(app, 5) * 3);
        } else if (in_scale) {
            CHECK(sum > 0);
        } else {
            CHECK_EQ(sum, 0);
            dark++;
        }
    }
    CHECK(scale == int(Scale::chromatic) || dark > 0);
    app.on_pad_press(2, 100);                              // held: white
    pump(2);
    CHECK_EQ(int(UiProbe::pad_r(app, 2)), 10);
    CHECK_EQ(int(UiProbe::pad_b(app, 2)), 10);
    app.on_pad_release(2);

    // FUNC + a lower pad is still the mute on the next bar.
    app.on_key(ENAGA_KEY_F1, true);
    app.on_pad_press(4, 100);
    app.on_pad_release(4);
    app.on_key(ENAGA_KEY_F1, false);
    CHECK_EQ(UiProbe::pad_held_note(app, 4), -1);
    CHECK(std::strstr(UiProbe::notice(app), "AT BAR") != nullptr);
    key(ENAGA_KEY_F4);
    pump(2);
}

// --- NOTE view: TUN and PIT as notes (core/tuning.hpp) ----------------------------------------

namespace {

// Track 1 as `machine`, on its EDIT page at `page`, with its tuning byte at `value`.
void tune_track_in_edit(MachineId machine, Page page, uint8_t value) {
    post_track(Cmd::set_machine, 0, uint8_t(machine));
    pump(4);
    Command command;
    command.cmd = Cmd::set_param;
    command.a = uint8_t(tune_param(uint8_t(machine)));
    command.b = value;
    app_engine().commands().post(command);
    pump(2);
    key(ENAGA_KEY_F4);
    key(ENAGA_KEY_F8);
    while (UiProbe::param_page(the_app()) != int(page)) key(ENAGA_KEY_SELECT_RIGHT);
}

}  // namespace

TEST(ui_tun_reads_as_a_note_and_turns_a_semitone_a_click) {
    Strata &app = fresh();
    CHECK(UiProbe::tune_notes(app));                   // on from the start
    tune_track_in_edit(MachineId::va_mono, Page::src, 64);
    CHECK(screen_has(app, "C-4"));                     // TUN 64: a C-4 step plays C-4

    app.on_encoder(0, +1);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 67);    // C#4, as near as the byte gets
    for (int i = 0; i < 6; i++) {
        app.on_encoder(0, +1);
        pump(2);
    }
    CHECK_EQ(int(app_engine().params(0).v[0]), 83);    // seven clicks: G-4, 12.5 cents sharp
    CHECK(screen_has(app, "G-4+"));
    app.on_encoder(0, +5);                             // a fast turn: five notes in one event
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 96);    // C-5 exactly
    check_layout(app, "EDIT SRC, MNO, TUN as a note");

    // The other knobs on the page are untouched by the view: CUT still moves one count.
    const uint8_t cut = app_engine().params(0).v[2];
    app.on_encoder(2, +1);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[2]), int(cut) + 1);
}

TEST(ui_pushing_the_tun_knob_switches_between_note_and_number) {
    Strata &app = fresh();
    tune_track_in_edit(MachineId::va_mono, Page::src, 64);

    key(ENAGA_KEY_PUSH_A);
    CHECK(!UiProbe::tune_notes(app));
    CHECK(std::strcmp(UiProbe::notice(app), "TUN AS NUMBERS") == 0);
    pump(2000);                                        // let the notice go
    CHECK(screen_has(app, "064"));
    CHECK(!screen_has(app, "C-4"));
    app.on_encoder(0, +1);                             // numbers: one count a click again
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 65);

    key(ENAGA_KEY_PUSH_A);
    CHECK(UiProbe::tune_notes(app));
    CHECK(std::strcmp(UiProbe::notice(app), "TUN AS NOTES") == 0);

    // Another knob's push does not switch it, nor does A on a page where A is not TUN.
    key(ENAGA_KEY_PUSH_B);
    CHECK(UiProbe::tune_notes(app));
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_PUSH_A);
    CHECK(UiProbe::tune_notes(app));
    key(ENAGA_KEY_SELECT_LEFT);

    // FUNC + push A is still the kill EQ.
    shift_key(ENAGA_KEY_PUSH_A);
    pump(2);
    CHECK(UiProbe::tune_notes(app));
    CHECK_EQ(int(app_engine().project().settings.eq_kills), 0x1);
    shift_key(ENAGA_KEY_PUSH_A);
    pump(2);

    // And on GRID, where the slots along the bottom show the same page.
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
    CHECK(screen_has(app, "C-4") || screen_has(app, "C#4"));
    key(ENAGA_KEY_PUSH_A);
    CHECK(!UiProbe::tune_notes(app));
    key(ENAGA_KEY_PUSH_A);
    CHECK(UiProbe::tune_notes(app));

    // And in KEYS, where A-D turn the EDIT page too and a turn echoes the note.
    key(ENAGA_KEY_F4);
    CHECK_EQ(UiProbe::mode(app), int(Mode::keys));
    app.on_encoder(0, +12);
    pump(2);
    CHECK_EQ(int(app_engine().params(0).v[0]), 96);
    CHECK(screen_has(app, "TUN C-5"));
    key(ENAGA_KEY_PUSH_A);
    CHECK(!UiProbe::tune_notes(app));
}

TEST(ui_a_held_step_still_takes_the_push_and_locks_tun_in_notes) {
    Strata &app = fresh();
    tune_track_in_edit(MachineId::va_mono, Page::src, 64);
    const int kStep = 4;

    step_down(kStep);
    app.on_encoder(0, +12);                            // an octave up, on this step only
    pump(2);
    uint8_t locked = 0;
    CHECK(pattern_get_lock(app_engine().pattern(), 0, kStep, 0, locked));
    CHECK_EQ(int(locked), 96);
    CHECK_EQ(int(app_engine().params(0).v[0]), 64);    // the kit did not move

    key(ENAGA_KEY_PUSH_A);                             // with the step held: clear the lock
    pump(2);
    CHECK(UiProbe::tune_notes(app));                   // not a switch of the view
    CHECK(!pattern_get_lock(app_engine().pattern(), 0, kStep, 0, locked));
    step_up(kStep);
    pump(2);
}

TEST(ui_grain_pit_is_on_amp_and_switches_there) {
    Strata &app = fresh();
    tune_track_in_edit(MachineId::grain, Page::amp, 64);
    CHECK(screen_has(app, "C-4"));
    app.on_encoder(0, -12);
    pump(2);
    CHECK_EQ(int(tune_note(uint8_t(MachineId::grain), app_engine().params(0).v[8])), 48);
    check_layout(app, "EDIT AMP, GRN, PIT as a note");
    key(ENAGA_KEY_PUSH_A);
    CHECK(!UiProbe::tune_notes(app));
    // On SRC, A is POS: no switch.
    key(ENAGA_KEY_SELECT_LEFT);
    key(ENAGA_KEY_SELECT_LEFT);
    CHECK_EQ(UiProbe::param_page(app), int(Page::src));
    key(ENAGA_KEY_PUSH_A);
    CHECK(!UiProbe::tune_notes(app));
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_SELECT_RIGHT);
    key(ENAGA_KEY_PUSH_A);
    CHECK(UiProbe::tune_notes(app));
}

TEST(ui_layout_tun_as_a_note_on_every_pitched_machine) {
    Strata &app = fresh();
    const MachineId machines[] = {MachineId::kick, MachineId::snare, MachineId::hat,
                                  MachineId::perc, MachineId::sample, MachineId::va_mono,
                                  MachineId::va_poly};
    for (MachineId m : machines) {
        static const uint8_t kValues[] = {0, 37, 83, 127};
        for (uint8_t v : kValues) {
            tune_track_in_edit(m, Page::src, v);
            check_layout(app, "EDIT SRC, TUN as a note");
            key(ENAGA_KEY_F4);
            check_layout(app, "GRID, TUN as a note");
        }
    }
    CHECK_EQ(int(app_engine().params(0).v[0]), 127);
    CHECK(screen_has(app, "C-6"));                     // PLY at 127: +23.6 from C-4
}

// The sidechain duck's three MIXER pages: a depth per track, then source, attack, hold, release.
TEST(ui_the_duck_pages_set_each_tracks_depth_the_source_and_the_times) {
    Strata &app = fresh();
    Engine &engine = app_engine();
    const Kit &kit = engine.project().kit;

    // DCK 5-8: C is track 7's depth, one percent a click.
    mixer_page(app, kMixerDuckDepthPage + 1);
    const int depth = int(kit.tracks[6].duck);
    app.on_encoder(2, +5);
    pump(2);
    CHECK_EQ(int(kit.tracks[6].duck), depth + 5);
    CHECK(app_storage().dirty());
    CHECK(std::strcmp(UiProbe::notice(app), "T7 DUCK DEPTH") == 0);
    CHECK(screen_has(app, "MIXER DCK 5-8"));
    app.on_encoder(2, -5);
    pump(2);
    CHECK_EQ(int(kit.tracks[6].duck), depth);

    // DUCK: A walks the source a track a click, D the release five milliseconds a click.
    mixer_page(app, kMixerDuckPage);
    const int source = int(kit.duck.source);
    const int release = int(kit.duck.release);
    app.on_encoder(0, +1);
    app.on_encoder(3, -2);
    pump(2);
    CHECK_EQ(int(kit.duck.source), source + 1);
    CHECK_EQ(engine.duck_source(), source + 1);
    CHECK_EQ(int(kit.duck.release), release - 2);
    CHECK(std::strcmp(UiProbe::notice(app), "DUCK RELEASE") == 0);
    CHECK(screen_has(app, "TIMES IN MS"));

    // Back on DCK 1-4 the new source says so instead of a depth.
    mixer_page(app, kMixerDuckDepthPage);
    CHECK(screen_has(app, "DUCKS UNDER T2"));
    app.on_encoder(1, +1);
    pump(2);
    CHECK(std::strcmp(UiProbe::notice(app), "T2 IS THE SOURCE") == 0);
    app.on_encoder(1, -1);

    mixer_page(app, kMixerDuckPage);
    app.on_encoder(0, -1);
    app.on_encoder(3, +2);
    pump(2);
    CHECK_EQ(int(kit.duck.source), source);
    CHECK_EQ(int(kit.duck.release), release);
}

// --- the STEP lights show velocity (Roland, 2026-11-23) -------------------------------------

namespace {

void step_field(int step, StepField field, int delta) {
    Command command;
    command.cmd = Cmd::step_field_delta;
    command.track = 0;
    command.step = uint8_t(step);
    command.a = uint8_t(field);
    command.b = uint8_t(int8_t(delta));
    app_engine().commands().post(command);
}

}  // namespace

TEST(ui_the_step_lights_show_velocity_and_blink_a_condition) {
    Strata &app = fresh();
    for (int i = 0; i < 5; i++) { step_down(i); step_up(i); }
    pump(4);
    // Velocities, from whatever a toggled trig gets: 20, 60, 95, 120, and an accented one.
    const int targets[4] = {20, 60, 95, 120};
    for (int i = 0; i < 4; i++) {
        step_field(i, StepField::velocity, targets[i] - int(step_of(0, i).velocity));
    }
    step_field(4, StepField::accent, 1);
    pump(4);
    for (int i = 0; i < 4; i++) CHECK_EQ(int(step_of(0, i).velocity), targets[i]);
    CHECK(step_of(0, 4).flags & kStepAccent);
    g_ms += 1;
    pump(2);
    CHECK_EQ(int(UiProbe::step_green(app, 0)), 1);       // soft
    CHECK_EQ(int(UiProbe::step_green(app, 1)), 3);       // medium
    CHECK_EQ(int(UiProbe::step_green(app, 2)), 6);       // loud
    CHECK_EQ(int(UiProbe::step_green(app, 3)), 10);      // accent by velocity
    CHECK_EQ(int(UiProbe::step_green(app, 4)), 10);      // accent by flag
    CHECK_EQ(int(UiProbe::step_green(app, 5)), 0);       // empty

    // A probability below 100 blinks the step: lit, then dark 200 ms later, then lit again.
    step_field(2, StepField::probability, -50);
    pump(2);
    CHECK(step_of(0, 2).probability < 100);
    int lit = 0, dark = 0;
    for (int t = 0; t < 4; t++) {
        // 199 + the 1 pump adds: exactly half a blink apart. At 201 the four reads drifted 3 ms,
        // and whenever an earlier test left g_ms within 3 ms of an edge two reads fell in one
        // half and the count depended on test order.
        g_ms += 199;
        pump(1);
        if (UiProbe::step_green(app, 2) == 6) lit++;
        if (UiProbe::step_green(app, 2) == 0) dark++;
    }
    CHECK_EQ(lit, 2);
    CHECK_EQ(dark, 2);
    CHECK_EQ(int(UiProbe::step_red(app, 2)), 0);
}

// --- macro assignment says what it did (Roland, 2026-11-23: "a bit unreliable") -------------
//
// A full macro refused in the engine while the screen said "D T1 DEC" as if it had worked, and
// macro D ships with all four slots used; pressing the same STEP again filled a second slot with
// a copy. Now: n/4 on every assignment, the same parameter again takes it off, and a full macro
// says so.

TEST(ui_macro_assignment_counts_toggles_and_says_full) {
    Strata &app = fresh();
    shift_key(ENAGA_KEY_F7);                               // PERFORM
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    for (int m = 0; m < kMacros; m++) post_cmd(Cmd::macro_clear, uint8_t(m));
    pump(2);
    while (UiProbe::macro_selected(app) != 0) key(ENAGA_KEY_LEFT);

    key(ENAGA_KEY_STEP1 + 4);                              // STEP 5: an FLTR parameter
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 1);
    CHECK(std::strstr(UiProbe::notice(app), " 1/4") != nullptr);
    check_layout(app, "PERFORM, macro assigned");

    key(ENAGA_KEY_STEP1 + 4);                              // again: off, not a copy
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 0);
    CHECK(std::strstr(UiProbe::notice(app), " OFF 0/4") != nullptr);

    for (int i = 0; i < 4; i++) {                          // a hand's pace: the engine applies
        key(ENAGA_KEY_STEP1 + i);                          // each before the next press
        pump(2);
    }
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 4);
    CHECK(std::strstr(UiProbe::notice(app), " 4/4") != nullptr);
    key(ENAGA_KEY_STEP1 + 8);                              // a fifth: refused, and it says so
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 4);
    CHECK_EQ(UiProbe::notice(app), "A FULL: CLR CLEARS");
    check_layout(app, "PERFORM, macro full");
    key(ENAGA_KEY_CLR);                                    // CLR clears the macro
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 0);
    key(ENAGA_KEY_F4);
}

// --- MACRO LEARN: push a macro's knob in PERFORM, then push parameter knobs in EDIT ----------

TEST(ui_macro_learn_assigns_by_pushing_knobs_in_edit) {
    Strata &app = fresh();
    for (int m = 0; m < kMacros; m++) post_cmd(Cmd::macro_clear, uint8_t(m));
    pump(2);
    shift_key(ENAGA_KEY_F7);                               // PERFORM
    key(ENAGA_KEY_PUSH_B);                                 // learn macro B: opens EDIT
    pump(2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    CHECK(std::strncmp(UiProbe::notice(app), "LEARN B 0/4", 11) == 0);
    check_layout(app, "EDIT, learn");

    key(ENAGA_KEY_SELECT_RIGHT);                           // another page
    const int page = UiProbe::param_page(app);
    key(ENAGA_KEY_PUSH_C);                                 // that page's third parameter
    pump(2);
    const Macro &b = app_engine().macros().macros[1];
    CHECK_EQ(macro_count(b), 1);
    CHECK_EQ(int(b.targets[0].param), page_first(Page(page)) + 2);
    CHECK_EQ(int(b.targets[0].track), UiProbe::selected(app));
    CHECK(std::strstr(UiProbe::notice(app), " 1/4") != nullptr);

    key(ENAGA_KEY_DOWN);                                   // another track, same knob
    key(ENAGA_KEY_PUSH_C);
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[1]), 2);
    key(ENAGA_KEY_PUSH_C);                                 // again: off
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[1]), 1);

    // The hint stays while learning, after the notice fades.
    g_ms += 4000;
    pump(2);
    CHECK(screen_has(app, "LEARN B 1/4 CLR: DONE"));
    check_layout(app, "EDIT, learning");

    key(ENAGA_KEY_CLR);                                    // done
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "KNOBS EDIT AGAIN");
    key(ENAGA_KEY_PUSH_C);                                 // a push is a push again
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[1]), 1);

    // Leaving for a screen without the EDIT knobs ends it too.
    shift_key(ENAGA_KEY_F7);
    key(ENAGA_KEY_PUSH_A);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_F7);                                     // MIXER
    key(ENAGA_KEY_F8);                                     // back to EDIT
    key(ENAGA_KEY_PUSH_A);
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[0]), 0);
    key(ENAGA_KEY_F4);
}

// --- an EDIT knob on a parameter a turned-up macro holds says so ----------------------------

TEST(ui_an_edit_knob_under_a_turned_up_macro_says_which_macro_holds_it) {
    Strata &app = fresh();
    for (int m = 0; m < kMacros; m++) {                    // empty and at 0, whatever ran before
        post_cmd(Cmd::macro_clear, uint8_t(m));
        post_cmd(Cmd::set_macro, uint8_t(m), 0);
    }
    pump(2);
    shift_key(ENAGA_KEY_F7);                               // PERFORM
    key(ENAGA_KEY_PUSH_C);                                 // learn C, EDIT opens
    key(ENAGA_KEY_PUSH_A);                                 // A's parameter on macro C
    key(ENAGA_KEY_CLR);                                    // learn done
    pump(2);
    CHECK_EQ(macro_count(app_engine().macros().macros[2]), 1);

    app.on_encoder(0, +3);                                 // macro at 0: the knob is heard
    pump(2);
    CHECK(std::strstr(UiProbe::notice(app), "HELD BY") == nullptr);

    shift_key(ENAGA_KEY_F7);                               // turn macro C up
    app.on_encoder(2, +10);
    pump(2);
    key(ENAGA_KEY_F8);
    app.on_encoder(0, +3);
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "HELD BY MACRO C");
    shift_key(ENAGA_KEY_F7);
    app.on_encoder(2, -127);
    post_cmd(Cmd::macro_clear, 2);
    pump(2);
    key(ENAGA_KEY_F4);
}

// On the unit LEARN felt inescapable and the CLR meant to leave it cleared the track (Roland,
// 2026-11-23). HOME ends it now, and a CLR right after the one that ended it clears nothing.
TEST(ui_learn_ends_on_home_and_a_second_clr_does_not_clear_the_track) {
    Strata &app = fresh();
    for (int i = 0; i < 16; i += 4) { step_down(i); step_up(i); }
    pump(2);
    shift_key(ENAGA_KEY_F7);
    key(ENAGA_KEY_PUSH_A);                                 // learn A
    CHECK_EQ(UiProbe::macro_learn(app), 0);
    key(ENAGA_KEY_F4);                                     // HOME: GRID, learn over
    CHECK_EQ(UiProbe::macro_learn(app), -1);
    CHECK_EQ(UiProbe::notice(app), "KNOBS EDIT AGAIN");

    key(ENAGA_KEY_F8);                                     // EDIT, learn again, CLR CLR
    shift_key(ENAGA_KEY_F7);
    key(ENAGA_KEY_PUSH_A);
    key(ENAGA_KEY_CLR);
    pump(2);
    key(ENAGA_KEY_CLR);
    pump(2);
    CHECK(trig_at(0, 0));                                  // the pattern is still there
    CHECK(trig_at(0, 12));
    // Later, CLR is CLR again.
    g_ms += 3000;
    pump(2);
    CHECK_EQ(UiProbe::macro_learn(app), -1);
    key(ENAGA_KEY_F4);
}

// --- a captured scene does not hold the EDIT knobs -----------------------------------------

TEST(ui_an_edit_knob_still_works_after_a_scene_capture) {
    // Roland, 2026-11-23: "the editing of sounds does not work anymore", and a p-lock on a held
    // step moved the sound while the knob alone did not. Pressing OK in PERFORM captures every
    // parameter into scene A, and the morph -- parked at 0, where it starts -- used to write
    // that capture over the kit on every rebuild. So the knob moved the kit and the screen, and
    // the sound stayed where the capture had left it. The morph now lets go at rest, as a macro
    // at 0 does (core/perform.cpp).
    Strata &app = fresh();
    for (int m = 0; m < kMacros; m++) {
        post_cmd(Cmd::macro_clear, uint8_t(m));
        post_cmd(Cmd::set_macro, uint8_t(m), 0);
    }
    post_cmd_morph(0);                                 // at rest: an earlier test left it up
    pump(2);

    shift_key(ENAGA_KEY_F7);                           // PERFORM
    key(ENAGA_KEY_OK);                                 // capture the whole kit into scene A
    pump(2);
    CHECK_EQ(UiProbe::notice(app), "SCENE CAPTURED");

    key(ENAGA_KEY_F8);                                 // EDIT
    const int track = UiProbe::selected(app);
    const int param = encoder_param(0);
    pump(2);
    const int before = int(app_engine().performed(track, param));
    app.on_encoder(0, +6);
    pump(2);
    CHECK(int(app_engine().performed(track, param)) != before);          // the sound moved
    CHECK_EQ(int(app_engine().performed(track, param)),
             int(app_engine().params(track).v[param]));                  // and follows the kit

    // Off the rest position the scene has it again, which is what the morph is for.
    post_cmd_morph(1);
    pump(2);
    CHECK_EQ(int(app_engine().performed(track, param)),
             int(app_engine().scenes().scenes[app_engine().scene_slot(0)].values[track][param]));
    post_cmd_morph(0);
    pump(2);
    key(ENAGA_KEY_F4);
}

// --- SOUNDS: the factory sounds for one track (core/sounds.hpp) -------------------------------

namespace {

// Puts `machine` on track 1, opens EDIT and then SOUNDS with OK.
void sounds_on(Strata &app, MachineId machine) {
    post_track(Cmd::set_machine, 0, uint8_t(machine));
    pump(4);
    key(ENAGA_KEY_F8);
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));
}

void sounds_cursor_to(int row) {
    for (int i = 0; i < 16; i++) key(ENAGA_KEY_UP);
    for (int i = 0; i < row; i++) key(ENAGA_KEY_DOWN);
}

bool same_params(const TrackParams &a, const TrackParams &b) {
    return std::memcmp(a.v, b.v, sizeof a.v) == 0;
}

}  // namespace

TEST(ui_layout_sounds_on_every_machine_that_has_them) {
    Strata &app = fresh();
    for (int machine = 0; machine < int(MachineId::count); machine++) {
        if (sound_preset_count(uint8_t(machine)) == 0) continue;
        sounds_on(app, MachineId(machine));
        check_layout(app, "SOUNDS");
        // The longest row: every preset marked as the one playing, in turn.
        for (int row = 1; row <= sound_preset_count(uint8_t(machine)); row++) {
            sounds_cursor_to(row);
            key(ENAGA_KEY_OK);
            pump(2);
            check_layout(app, machine_name(uint8_t(machine)));
        }
        key(ENAGA_KEY_CLR);
        key(ENAGA_KEY_F4);
    }
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
}

TEST(ui_sounds_a_list_longer_than_the_screen_scrolls_with_the_cursor) {
    Strata &app = fresh();
    sounds_on(app, MachineId::va_mono);
    const int count = sound_preset_count(uint8_t(MachineId::va_mono));
    CHECK(count >= 8);                                 // more rows than the screen's eight
    const char *last = sound_preset(uint8_t(MachineId::va_mono), count - 1)->name;
    sounds_cursor_to(0);
    CHECK(screen_has(app, "AS WAS"));
    CHECK(!screen_has(app, last));
    sounds_cursor_to(count);                           // the last preset
    CHECK(screen_has(app, last));
    CHECK(!screen_has(app, "AS WAS"));
    check_layout(app, "SOUNDS, scrolled");
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(sound_preset_matches(uint8_t(MachineId::va_mono), count - 1, app_engine().params(0).v));
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
}

TEST(ui_sounds_ok_loads_a_preset_into_the_selected_track_only_and_keeps_its_mix) {
    Strata &app = fresh();
    sounds_on(app, MachineId::kick);
    // Something of the user's own on the pages a preset must leave: pan, a send, the LFO.
    post_cmd(Cmd::set_param, uint8_t(page_first(Page::fx) + 1), 20);
    post_cmd(Cmd::set_param, uint8_t(page_first(Page::fx) + 3), 99);
    post_cmd(Cmd::set_param, uint8_t(kParams + 1), 77);
    pump(2);
    const TrackParams before = app_engine().params(0);
    TrackParams others[kTracks];
    for (int t = 0; t < kTracks; t++) others[t] = app_engine().params(t);

    sounds_cursor_to(2);                           // AS WAS, 909, 808
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));       // stays, for the next try
    const TrackParams &now = app_engine().params(0);
    CHECK(sound_preset_matches(uint8_t(MachineId::kick), 1, now.v));
    CHECK_EQ(int(now.v[page_first(Page::fx) + 1]), 20);
    CHECK_EQ(int(now.v[page_first(Page::fx) + 3]), 99);
    CHECK_EQ(int(now.v[kParams + 1]), 77);
    CHECK(!same_params(now, before));
    for (int t = 1; t < kTracks; t++) CHECK(same_params(app_engine().params(t), others[t]));
    // That it is what PLAYS is tests/test_sounds.cpp's, on an engine of its own.

    // AS WAS puts the sound back exactly.
    sounds_cursor_to(0);
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK(same_params(app_engine().params(0), before));

    // CLR goes back where the list came from.
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::edit));
    key(ENAGA_KEY_F4);
}

TEST(ui_sounds_the_selection_keys_walk_the_track_and_as_was_follows_it) {
    Strata &app = fresh();
    sounds_on(app, MachineId::kick);
    const TrackParams kick_before = app_engine().params(0);
    sounds_cursor_to(3);
    key(ENAGA_KEY_OK);                             // track 1 gets DEEP
    pump(2);
    const TrackParams kick_deep = app_engine().params(0);
    CHECK(!same_params(kick_deep, kick_before));

    key(ENAGA_KEY_SELECT_RIGHT);                   // track 2: its own list, its own AS WAS
    pump(2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));
    CHECK_EQ(UiProbe::selected(app), 1);
    const uint8_t machine = app_engine().machine_id(1);
    if (sound_preset_count(machine) > 1) {
        const TrackParams second_before = app_engine().params(1);
        sounds_cursor_to(2);
        key(ENAGA_KEY_OK);
        pump(2);
        CHECK(sound_preset_matches(machine, 1, app_engine().params(1).v));
        sounds_cursor_to(0);
        key(ENAGA_KEY_OK);
        pump(2);
        CHECK(same_params(app_engine().params(1), second_before));
    }
    // Track 1 kept the sound it was given.
    CHECK(same_params(app_engine().params(0), kick_deep));
    key(ENAGA_KEY_CLR);
    key(ENAGA_KEY_F4);
}

TEST(ui_sounds_machine_ok_opens_it_and_marks_the_sound_playing_now) {
    Strata &app = fresh();
    post_track(Cmd::set_machine, 0, uint8_t(MachineId::va_mono));
    pump(4);
    // Give the track ACID first, so the list opens on it.
    post_cmd(Cmd::sound_preset, uint8_t(MachineId::va_mono), 1);
    pump(2);
    shift_key(ENAGA_KEY_F8);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    CHECK(screen_has(app, "OK: SOUNDS"));
    key(ENAGA_KEY_OK);
    pump(2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::sounds));
    CHECK(screen_has(app, ">ACID    NOW"));
    key(ENAGA_KEY_CLR);
    CHECK_EQ(UiProbe::mode(app), int(Mode::machine));
    post_track(Cmd::set_machine, 0, 0);
    pump(4);
    key(ENAGA_KEY_F4);
}

// --- the performance mixer on VALUE (core/perffx.hpp, docs/CONTROLS.md) ----------------------

namespace {

void value_step(Strata &app, int raw) {      // push VALUE + a STEP tap
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    step_down(raw);
    step_up(raw);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
}

void value_key(Strata &app, int code) {      // push VALUE + a key tap
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    key(code);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
}

}  // namespace

TEST(ui_push_value_and_a_step_picks_an_effect_and_value_turns_its_amount) {
    Strata &app = fresh();
    const uint32_t tempo = UiProbe::tempo_tenths(app);
    value_step(app, 3);
    pump(2);
    CHECK(UiProbe::perf_picked(app) == PerfFx::echo_riser);
    CHECK(app_engine().perf_fx() == PerfFx::echo_riser);
    CHECK_EQ(int(app_engine().perf_amount()), 0);   // picked at 0: nothing changes yet
    CHECK(std::strcmp(UiProbe::notice(app), "4 ECHO RISER") == 0);
    CHECK(!trig_at(0, 3));                          // the STEP was the mixer's, not the grid's

    // VALUE is its amount now, two counts a click, and not the tempo.
    app.on_encoder(4, 5);
    pump(2);
    CHECK_EQ(int(app_engine().perf_amount()), 10);
    CHECK_EQ(UiProbe::tempo_tenths(app), tempo);
    CHECK(std::strcmp(UiProbe::notice(app), "ECHO RISER 10") == 0);
    // In every mode: PERFORM's morph waits too.
    shift_key(ENAGA_KEY_F7);
    CHECK_EQ(UiProbe::mode(app), int(Mode::perform));
    const uint16_t morph = app_engine().morph();
    app.on_encoder(4, 3);
    pump(2);
    CHECK_EQ(int(app_engine().perf_amount()), 16);
    CHECK_EQ(int(app_engine().morph()), int(morph));

    // A new effect replaces it, from 0; its own STEP again lets go and VALUE is VALUE again.
    value_step(app, 9);
    pump(2);
    CHECK(app_engine().perf_fx() == PerfFx::pump);
    CHECK_EQ(int(app_engine().perf_amount()), 0);
    value_step(app, 9);
    pump(2);
    CHECK(app_engine().perf_fx() == PerfFx::none);
    CHECK(UiProbe::perf_picked(app) == PerfFx::none);
    CHECK(std::strcmp(UiProbe::notice(app), "FX OFF: VALUE TEMPO") == 0);
    app.on_encoder(4, 3);
    pump(2);
    CHECK(app_engine().morph() != morph);           // PERFORM's VALUE is the morph again
}

TEST(ui_a_step_released_after_value_still_belongs_to_the_mixer) {
    Strata &app = fresh();
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    step_down(5);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);        // VALUE up first
    step_up(5);
    pump(2);
    CHECK(!trig_at(0, 5));
    CHECK(app_engine().perf_fx() == PerfFx::reverb_wash);
    // And the next plain tap is a trig, as ever.
    step_down(5);
    step_up(5);
    pump(2);
    CHECK(trig_at(0, 5));
}

TEST(ui_push_value_left_right_walks_the_effects_keeping_the_amount) {
    Strata &app = fresh();
    value_step(app, 1);                             // HP SWEEP
    app.on_encoder(4, 10);
    pump(2);
    CHECK_EQ(int(app_engine().perf_amount()), 20);
    value_key(app, ENAGA_KEY_RIGHT);
    pump(2);
    CHECK(app_engine().perf_fx() == PerfFx::hp_riser);
    CHECK_EQ(int(app_engine().perf_amount()), 20);  // a scene change on a crossfader left up
    value_key(app, ENAGA_KEY_LEFT);
    value_key(app, ENAGA_KEY_LEFT);
    pump(2);
    CHECK(app_engine().perf_fx() == PerfFx::lp_sweep);
    // Round to 16, the looper: picked, not recording, and the effect stays on under it.
    value_key(app, ENAGA_KEY_LEFT);
    pump(2);
    CHECK(UiProbe::perf_picked(app) == PerfFx::looper);
    CHECK(app_engine().perf_fx() == PerfFx::lp_sweep);
    CHECK_EQ(int(app_engine().status().looper_state.load(std::memory_order_relaxed)), 0);
    // The arrows still move the cursor without VALUE: GRID's LEFT is the step page's business.
    CHECK_EQ(UiProbe::mode(app), int(Mode::grid));
}

TEST(ui_step_16_records_a_loop_value_mixes_it_and_clr_clears_it) {
    Strata &app = fresh();
    post_cmd(Cmd::resample_cancel);                 // a take an earlier test left armed
    FakeHost::the().set_running(true);
    pump();
    value_step(app, 15);
    pump(2);
    CHECK(UiProbe::perf_picked(app) == PerfFx::looper);
    CHECK_EQ(int(app_engine().status().looper_state.load(std::memory_order_relaxed)), 1);
    CHECK_EQ(int(app_engine().status().looper_bars.load(std::memory_order_relaxed)), 4);
    CHECK(std::strcmp(UiProbe::notice(app), "LOOP 4 BAR: NEXT BAR") == 0);

    // VALUE is the loop's mix.
    app.on_encoder(4, 8);
    pump(2);
    CHECK_EQ(int(UiProbe::loop_mix(app)), 16);
    CHECK_EQ(int(app_engine().looper_mix()), 16);

    // RESAMPLE and FREEZE say why they cannot start while the loop holds their buffer.
    shift_key(ENAGA_KEY_F2);
    CHECK_EQ(UiProbe::mode(app), int(Mode::resample));
    key(ENAGA_KEY_OK);
    CHECK(std::strcmp(UiProbe::notice(app), "LOOP HOLDS BUFFER") == 0);
    pump(2);
    CHECK_EQ(int(app_engine().status().resample_state.load(std::memory_order_relaxed)), 0);
    key(ENAGA_KEY_F4);
    menu_run(kMenuFreeze);
    CHECK(std::strcmp(UiProbe::notice(app), "LOOP HOLDS BUFFER") == 0);

    // Push VALUE + CLR: everything off, the loop kept; again: the loop thrown away.
    value_key(app, ENAGA_KEY_CLR);
    pump(2);
    CHECK(UiProbe::perf_picked(app) == PerfFx::none);
    CHECK_EQ(int(app_engine().looper_mix()), 0);
    CHECK_EQ(int(app_engine().status().looper_state.load(std::memory_order_relaxed)), 1);
    value_key(app, ENAGA_KEY_CLR);
    pump(2);
    CHECK(std::strcmp(UiProbe::notice(app), "LOOP CLEARED") == 0);
    CHECK_EQ(int(app_engine().status().looper_state.load(std::memory_order_relaxed)), 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(ui_the_menu_loop_len_sets_the_looper_bars) {
    Strata &app = fresh();
    menu_to(kMenuLoopLen);
    CHECK(screen_has(app, "LOOP LEN"));
    CHECK(screen_has(app, "4 BAR"));
    key(ENAGA_KEY_RIGHT);
    CHECK(screen_has(app, "8 BAR"));
    key(ENAGA_KEY_CLR);                             // close the menu
    post_cmd(Cmd::resample_cancel);                 // a take an earlier test left armed
    FakeHost::the().set_running(true);
    pump();
    value_step(app, 15);
    pump(2);
    CHECK_EQ(int(app_engine().status().looper_bars.load(std::memory_order_relaxed)), 8);
    value_key(app, ENAGA_KEY_CLR);
    value_key(app, ENAGA_KEY_CLR);
    pump(2);
    FakeHost::the().set_running(false);
    // Back to 4 for the tests after this one: the app is shared.
    menu_to(kMenuLoopLen);
    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_RIGHT);
    key(ENAGA_KEY_RIGHT);
    CHECK(screen_has(app, "4 BAR"));
    key(ENAGA_KEY_CLR);
    pump();
}

TEST(ui_in_mute_mode_push_value_and_steps_1_8_still_store_mute_states) {
    Strata &app = fresh();
    app.on_pad_press(5, 100);                       // MUTE mode on
    app.on_pad_release(5);
    CHECK(UiProbe::mute_mode(app));
    value_step(app, 0);
    pump(2);
    CHECK(std::strcmp(UiProbe::notice(app), "STORED 1") == 0);
    CHECK(app_engine().perf_fx() == PerfFx::none);
}

TEST(ui_value_held_lights_the_step_keys_as_the_sixteen_effects) {
    Strata &app = fresh();
    value_step(app, 3);
    app.on_encoder(4, 10);
    value_step(app, 3);                             // let go...
    value_step(app, 6);                             // ...and pick STUTTER, still at 0
    app.on_key(ENAGA_KEY_PUSH_VALUE, true);
    pump(2);
    CHECK_EQ(int(UiProbe::step_green(app, 6)), 10); // the picked one, bright green
    CHECK_EQ(int(UiProbe::step_red(app, 6)), 0);
    CHECK_EQ(int(UiProbe::step_green(app, 0)), 2);  // the rest dim amber
    CHECK_EQ(int(UiProbe::step_red(app, 0)), 2);
    app.on_key(ENAGA_KEY_PUSH_VALUE, false);
    pump(2);
    CHECK_EQ(int(UiProbe::step_green(app, 0)), 0);  // the grid again: an empty step
}
