// app/engine and app/storage, on the host.
//
// Until tests/fakehost.cpp there were no tests here at all: `host/Makefile` compiled `core/` and
// `dsp/` only, so `app/` -- the engine, the card protocols and every gesture the instrument has
// -- was verified by two things, that it compiles for the C674x and that four emulator scenarios
// pass, and `emulate.py` can press one key and nothing else. A review of `app/` found ten bugs.
// Three of them are in this file because a mechanical check of `effective_` after each command
// would have caught them on the day they were written, and one more is here because the first
// thing this harness did, before a single assertion existed, was expose it.
//
// ## One Engine, shared
//
// `Engine::init` takes about 85 MiB -- a 64 MiB sample region, 18 MiB of loader staging and the
// 2.93 MiB repeat ring -- and allocates it with `new`, once, as CLAUDE.md requires. So there is
// ONE engine for the whole file and `init` is called once; `fresh()` returns it to a known state
// the way the app itself would, with a new project and a drained ring.
//
// ## Both contexts, by hand
//
// The two sides meet at a 16-byte SPSC ring and a single-writer Status. `Storage::service()`
// WAITS while the ring is non-empty -- that is how a save is deferred past every pending edit
// (app/storage.cpp) -- and only `Engine::process()` drains it. A test that calls one without the
// other therefore waits forever, which is what `pump()` exists to prevent. Driving both by hand
// is also the point: the emulator runs them in lockstep at real speed and cannot hold one still.
#include "check.hpp"
#include "fakehost.hpp"

#include <cstring>

#include <enaga/abi.h>
#include <enaga/clock.hpp>

#include "engine.hpp"
#include "storage.hpp"

using namespace strata;
using strata::test::FakeHost;

namespace {

constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kStereo = 2 * kBlk;

// 85 MiB of it, so these are file-static and `init` runs once (see the header).
Engine g_engine;
Storage g_storage;
float g_out[kStereo];
bool g_ready = false;

// Run `blocks` audio blocks and service the card between each, which is the order App::audio and
// App::draw really run in. Returns the loudest sample seen, so a caller that cares whether
// something sounded does not need its own loop.
float pump(int blocks = 1) {
    float peak = 0.0f;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < kStereo; i++) g_out[i] = 0.0f;
        g_engine.process(nullptr, g_out, uint32_t(kBlk));
        for (int i = 0; i < kStereo; i++) {
            const float a = g_out[i] < 0.0f ? -g_out[i] : g_out[i];
            if (a > peak) peak = a;
        }
        g_storage.service();
        FakeHost::the().complete_loads();
    }
    return peak;
}

// Pump until the card is idle, or give up. The bound is generous because a save writes one file
// per pass and there are ten of them, each deferred behind the ring.
//
// It waits for a job to START and then finish, rather than for "not busy": `request_save` only
// sets a flag, and the job does not begin until `service()` finds the ring drained -- so a check
// made first sees "idle", returns true, and the test then asserts about a save that never ran.
//
// False means no job ever ran, which is a real answer and not only a timeout: with no usable
// card `service()` returns before it starts anything, and the save stays pending.
bool pump_until_idle(int limit = 4000) {
    bool started = false;
    for (int i = 0; i < limit; i++) {
        pump();
        if (g_storage.busy_text() != nullptr) {
            started = true;
        } else if (started && FakeHost::the().pending_loads() == 0) {
            return true;
        }
    }
    return false;
}

// A known state: a blank card, an internal clock at 130 BPM, a new project in RAM and an empty
// ring. `Engine::init` runs once, on the first call.
//
// The project is BLANK, not the default beat. `init` loads the default beat once and a test that
// inherited it would also inherit whatever the test before it did to it, so every test that wants
// a note puts its own trig down. That is more typing and it is the only way the assertions mean
// what they say.
Engine &fresh() {
    FakeHost &host = FakeHost::the();
    host.reset();
    if (!g_ready) {
        g_ready = g_engine.init(float(ENAGA_SAMPLE_RATE)) && g_storage.init(g_engine);
    }
    // A card SWAP, not just "a card is there". Storage remembers whether the project is
    // complete on the card, and the fake's reset() wiped the card without telling it -- which is
    // exactly what pulling an SD card does on the unit. Doing it as a transition keeps the tests
    // honest about a state the app really reaches.
    g_storage.on_card(false);
    g_storage.on_card(true);

    // The app's own way back to a clean project, rather than a second init(): project_init into
    // the load buffer and tell the audio side. `request_new` would do the same and also rename.
    Project &incoming = g_engine.project_for_load();
    project_init(incoming);

    // And a kit that can make a sound. `project_init` zeroes the kit, so LVL is 0 on every
    // track and the whole instrument is silent -- which is correct for a blank project and
    // useless as a starting point for a test about whether anything was heard. Mid values with
    // the level up is the same patch tests/test_va.cpp and the others start from.
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) incoming.kit.tracks[t].params[i] = 64;
        incoming.kit.tracks[t].params[11] = 127;     // LVL on every machine (dsp/track.hpp)
    }

    Command command;
    command.cmd = Cmd::project_loaded;
    g_engine.commands().post(command);

    // And cut any tails the test before left ringing. A kick decays for well over a block, so
    // without this a test that measures "silence before frame 40" measures the previous test's
    // note. `Cmd::stop` with c = 1 is the app's own second STOP press, which is what it is for.
    Command hush;
    hush.cmd = Cmd::stop;
    hush.c = 1;
    g_engine.commands().post(hush);
    pump(4);
    return g_engine;
}

void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
          uint8_t c = 0) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.step = step;
    command.a = a;
    command.b = b;
    command.c = c;
    g_engine.commands().post(command);
}

// What the machine on `track` would be prepared with right now: the parameters the engine hands
// it every block. This is `effective_`, which is private, so it is read the way the machine reads
// it -- by what the engine last prepared. `Machine::param` is not a thing, so the test instead
// asks the engine for the value it publishes for the UI and the value it plays, which must agree.
uint8_t kit_value(int track, int param) { return g_engine.params(track).v[param]; }

// A trig on step 0 of track 0, so a tick can make something happen.
void put_trig(int track, int step, uint8_t note = 60) {
    post(Cmd::toggle_trig, uint8_t(track), uint8_t(step));
    pump();
    (void)note;
}

}  // namespace

// --- the harness itself ---------------------------------------------------------------------
//
// If these fail, nothing below them means anything.

TEST(app_the_engine_starts_and_plays_a_tick) {
    Engine &engine = fresh();
    put_trig(0, 0);

    // Nothing before a tick: the trig is down and it must wait for the clock.
    CHECK_EQ(pump(2), 0.0f);

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    const float peak = pump();
    CHECK(peak > 0.01f);                       // the default beat's downbeat kick
    CHECK(peak < 2.0f);                        // and the master limiter is holding it
    CHECK_EQ(FakeHost::the().ticks_pending(), 0);   // the engine consumed it
    CHECK(engine.status().live_bank.load(std::memory_order_relaxed) == 0);
}

TEST(app_a_tick_lands_on_the_frame_the_clock_says) {
    // The whole reason the fake clock is programmable: a tick carries the frame it falls on, and
    // a step has to start on that exact sample. The emulator has one real clock and no way to
    // steer it, so this is the first time the promise has been checked at all.
    fresh();
    put_trig(0, 0);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 40);

    for (int i = 0; i < kStereo; i++) g_out[i] = 0.0f;
    g_engine.process(nullptr, g_out, uint32_t(kBlk));

    float before = 0.0f;
    for (int i = 0; i < 2 * 40; i++) {
        const float a = g_out[i] < 0.0f ? -g_out[i] : g_out[i];
        if (a > before) before = a;
    }
    float after = 0.0f;
    for (int i = 2 * 40; i < kStereo; i++) {
        const float a = g_out[i] < 0.0f ? -g_out[i] : g_out[i];
        if (a > after) after = a;
    }
    // Not "silence then sound": the master bus has a reverb, and its tail from whatever ran
    // before outlives anything the rig can reset -- `Cmd::stop` cuts the voices, not the send.
    // So the assertion is that the note's onset is AT the tick's frame: what comes before it is
    // a quiet decaying tail, and what comes after is more than an order of magnitude louder. A
    // note starting on frame 0 fails this, which is the bug it is here to catch.
    CHECK(before < 0.05f);
    CHECK(after > 0.001f);
    CHECK(after > 10.0f * before);
}

// --- the three the review found -------------------------------------------------------------
//
// M5 added a per-block copy of `performed_` into `effective_` so that a morph could be heard
// between steps. It reverts anything ELSE that writes `effective_`, and three commands did.

TEST(app_a_knob_turn_in_edit_mode_is_heard_in_the_same_block) {
    // `Cmd::set_param` mirrored into `effective_` and did not set `perform_dirty_`, so
    // `apply_performance` returned early, `performed_` still held the old kit value, and the copy
    // put it back before a sample was rendered. Turning a knob was inaudible until some unrelated
    // performance control happened to set the flag.
    //
    // The audible consequence is checked rather than the flag: a track's level parameter at 0 and
    // at 127 cannot produce the same block.
    fresh();
    FakeHost::the().set_running(true);

    // Track 0 is KICK and parameter 11 is LVL on every machine (dsp/track.hpp).
    constexpr int kLvl = 11;
    put_trig(0, 0);
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 127);
    pump();
    CHECK_EQ(int(kit_value(0, kLvl)), 127);

    FakeHost::the().deliver_tick(0, 0);
    const float loud = pump(8);

    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 0);
    pump();
    CHECK_EQ(int(kit_value(0, kLvl)), 0);

    FakeHost::the().deliver_tick(0, 0);
    const float quiet = pump(8);

    CHECK(loud > 0.01f);
    CHECK(quiet < loud);          // the turn reached the machine, in the block it was made
}

TEST(app_a_loaded_project_is_heard_and_not_only_drawn) {
    // The same mechanism through `params_from_kit()`: `performed_` kept the PREVIOUS project's
    // kit, so the screen showed the new one and the machines played the old one.
    fresh();
    constexpr int kLvl = 11;

    // Put a distinctive kit value in the project that is about to be loaded.
    Project &incoming = g_engine.project_for_load();
    project_init(incoming);
    incoming.kit.tracks[0].params[kLvl] = 7;
    post(Cmd::project_loaded);
    pump(2);

    // The engine's own copy followed the load...
    CHECK_EQ(int(kit_value(0, kLvl)), 7);

    // ...and so did what it plays. A block later the per-block copy has run; if `performed_` had
    // been left stale it would have written the old 127 back over it by now.
    pump(4);
    CHECK_EQ(int(kit_value(0, kLvl)), 7);
}

TEST(app_a_motion_recorded_lock_survives_the_block_copy) {
    // `lock_delta` mirrors its value into `effective_` so a motion-recorded knob is heard on the
    // pass it is recorded. It did not set the `locked_` bit, and the per-block copy rewrites
    // every parameter the mask does not claim -- so it was undone before a sample was rendered,
    // which is exactly the silence the comment above it says it prevents.
    fresh();
    FakeHost::the().set_running(true);
    constexpr int kLvl = 11;

    put_trig(0, 0);
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 127);
    pump();

    // Start the step so the playhead is on it, which is the condition the mirror is guarded by.
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(g_engine.status().playhead[0].load(std::memory_order_relaxed)), 0);

    // Now record a lock on that step, taking the level right down.
    post(Cmd::lock_delta, 0, 0, uint8_t(kLvl), uint8_t(int8_t(-120)));
    const float after_lock = pump(6);

    // The lock is in the pattern...
    const LockView locks = pattern_step_locks(g_engine.pattern(), 0, 0);
    bool found = false;
    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        if (lock->param == kLvl) found = true;
    }
    CHECK(found);

    // ...and it was heard: the same six blocks without the lock are louder.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 127);
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    const float unlocked = pump(6);
    CHECK(unlocked > 0.01f);
    CHECK(after_lock < unlocked);
}

// --- the transport, and the refusal CLAUDE.md requires --------------------------------------

TEST(app_the_transport_is_refused_under_an_external_clock) {
    // CLAUDE.md: "STRATA does not own the tempo. CLOCK SRC is the user's setting;
    // start/stop/set_tempo return false under external sync. Always handle the refusal." Nothing
    // had ever exercised that path -- the emulator has one clock and it is internal.
    fresh();
    FakeHost &host = FakeHost::the();
    host.set_source(ENAGA_CLOCK_MIDI);

    CHECK(!enaga::clock::start());
    CHECK(!enaga::clock::stop());
    CHECK(!host.running());                    // and the refusal left the clock alone

    // The tempo is the exception: it is still settable, because under MIDI it is a measurement
    // and the call is how the app reports what it wants, not what happens.
    CHECK(enaga::clock::set_tempo(140.0f));
    CHECK_NEAR(host.tempo(), 140.0f, 0.01);

    // And back on the internal clock it all works, so the test above is about the SOURCE and not
    // about a fake that refuses everything.
    host.set_source(ENAGA_CLOCK_INTERNAL);
    CHECK(enaga::clock::start());
    CHECK(host.running());
    CHECK(enaga::clock::stop());
    CHECK(!host.running());
}

// --- the card -------------------------------------------------------------------------------

TEST(app_the_first_save_of_a_project_writes_all_ten_files) {
    // The bug this harness found before it had a single assertion in it.
    //
    // `request_save` marks the KIT dirty and nothing else, and `step_save` skips every file whose
    // dirty flag is clear. The flags describe how RAM differs from a project ON THE CARD, so for
    // a project that was never written there is nothing to differ from -- and the first save
    // stored KIT.BIN alone. The nine others never existed, so reloading the project the user had
    // just saved failed with NOT FOUND: their own work, reported as saved, and gone.
    fresh();
    FakeHost &host = FakeHost::the();

    g_storage.request_save();
    CHECK(pump_until_idle());
    CHECK(g_storage.last_error() == nullptr);
    CHECK(host.logged("saved"));

    const char *files[] = {"PROJECT.BIN", "KIT.BIN",   "PERFORM.BIN", "BANK_A.BIN", "BANK_B.BIN",
                           "BANK_C.BIN",  "BANK_D.BIN", "BANK_E.BIN", "BANK_F.BIN", "BANK_G.BIN",
                           "BANK_H.BIN"};
    char path[ENAGA_FILE_PATH_MAX + 1];
    for (const char *file : files) {
        std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/");
        std::strcat(path, file);
        CHECK(host.has_file(path));
    }

    // The sizes tests/test_project.cpp pins, reached through the real card protocol this time.
    std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/PROJECT.BIN");
    CHECK_EQ(host.file_bytes(path), 251);
    std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/KIT.BIN");
    CHECK_EQ(host.file_bytes(path), 1076);
    std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/PERFORM.BIN");
    CHECK_EQ(host.file_bytes(path), 2424);
}

TEST(app_a_saved_project_reloads) {
    // The other half of it: what was written can be read back. An incomplete save passed every
    // check above except this one, which is why it is here and not left to the sizes.
    fresh();
    constexpr int kLvl = 11;

    post(Cmd::set_param, 3, 0, uint8_t(kLvl), 42);
    post(Cmd::toggle_trig, 2, 5);
    pump(2);
    CHECK_EQ(int(kit_value(3, kLvl)), 42);

    g_storage.request_save();
    CHECK(pump_until_idle());
    CHECK(g_storage.last_error() == nullptr);

    // Move RAM away from what was saved, then read the card again.
    post(Cmd::set_param, 3, 0, uint8_t(kLvl), 99);
    pump();
    CHECK_EQ(int(kit_value(3, kLvl)), 99);

    g_storage.request_reload();
    CHECK(pump_until_idle());
    CHECK(g_storage.last_error() == nullptr);
    CHECK(FakeHost::the().logged("loaded"));

    // The load finishing is not the same as the engine having taken it: the last thing a load
    // does is post Cmd::project_loaded, and the audio side applies that on its next block.
    pump(4);

    CHECK_EQ(int(kit_value(3, kLvl)), 42);                           // the kit came back
    CHECK((g_engine.pattern().tracks[2].steps[5].flags & kStepTrig) != 0);   // and the trig
}

TEST(app_a_save_asked_for_with_no_card_lands_when_one_arrives) {
    // A card that is not there, which on hardware means pulling an SD card at the right moment.
    //
    // `service()` returns before it starts anything while the card is unusable, so a save asked
    // for now does not fail -- it STAYS PENDING, nothing is written, and it completes by itself
    // when a card appears. The work is not lost and the user does not have to ask twice.
    //
    // The OTHER order is not the same, and the test below this one is that one.
    fresh();
    FakeHost &host = FakeHost::the();
    constexpr int kLvl = 11;

    post(Cmd::set_param, 5, 0, uint8_t(kLvl), 77);
    pump(2);

    host.set_card(false);
    g_storage.on_card(false);
    g_storage.request_save();
    CHECK(!pump_until_idle(200));                 // no job ever started
    CHECK_EQ(g_storage.last_error(), "NO CARD");  // and the screen says why
    CHECK_EQ(host.file_count(), 0);
    CHECK(!host.logged("saved"));

    // The edit is NOT lost: it is in RAM and the project still reads as dirty, so the screen
    // keeps its unsaved-changes mark.
    CHECK_EQ(int(kit_value(5, kLvl)), 77);
    CHECK(g_storage.dirty());

    // The card arrives and the save completes on its own, carrying the edit made while it was
    // out. `on_card(true)` clears the error first, so the screen stops saying NO CARD.
    host.set_card(true);
    g_storage.on_card(true);
    CHECK(pump_until_idle());
    CHECK(g_storage.last_error() == nullptr);
    CHECK(host.logged("saved"));
    CHECK(!g_storage.dirty());

    char path[ENAGA_FILE_PATH_MAX + 1];
    std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/KIT.BIN");
    CHECK(host.has_file(path));

    // And it was a FULL save, because the card swap made `project_complete_` a claim about a
    // card nobody had looked at.
    std::strcpy(path, "/ENAGA/DATA/STRATA/PROJECTS/DEFAULT/BANK_H.BIN");
    CHECK(host.has_file(path));
}

TEST(app_a_save_already_asked_for_is_cancelled_by_pulling_the_card) {
    // The other order, and it is NOT the same. `on_card(false)` clears `save_pending_` along
    // with any job in flight, so a save asked for BEFORE the card went out is abandoned: "press
    // SAVE, pull the card, put it back" leaves the project unsaved with the error as the only
    // sign. Nothing is lost -- the edit is in RAM and the project still reads dirty -- and the
    // user has to ask again.
    //
    // Whether it ought to resume by itself is a judgement for the unit, where someone can see
    // how it feels; docs/UNIT_CHECKLIST.md carries it as a question. This test pins what the
    // code does today so the answer is a decision rather than a surprise.
    fresh();
    FakeHost &host = FakeHost::the();
    constexpr int kLvl = 11;

    post(Cmd::set_param, 6, 0, uint8_t(kLvl), 31);
    pump(2);
    g_storage.request_save();

    // Out it comes, before any pass of service() could start the job.
    host.set_card(false);
    g_storage.on_card(false);
    CHECK_EQ(g_storage.last_error(), "NO CARD");

    host.set_card(true);
    g_storage.on_card(true);
    pump(400);
    CHECK(!host.logged("saved"));
    CHECK_EQ(host.file_count(), 0);
    CHECK(g_storage.dirty());                 // so the screen still shows the unsaved mark
    CHECK_EQ(int(kit_value(6, kLvl)), 31);    // and the edit itself is untouched

    // Asking again saves it.
    g_storage.request_save();
    CHECK(pump_until_idle());
    CHECK(host.logged("saved"));
    CHECK(!g_storage.dirty());
}

TEST(app_a_project_load_is_refused_under_an_external_clock) {
    // CLAUDE.md's rule again, on the other side: a load stops the transport, and under an
    // external clock STRATA cannot stop anything -- so it must refuse the load rather than
    // rewrite pattern memory under a running sequencer.
    fresh();
    FakeHost &host = FakeHost::the();

    g_storage.request_save();
    CHECK(pump_until_idle());
    CHECK(g_storage.last_error() == nullptr);

    host.set_source(ENAGA_CLOCK_MIDI);
    host.set_running(true);
    const int loads_before = host.read_alls();
    g_storage.request_reload();
    pump(40);
    CHECK_EQ(host.read_alls(), loads_before);   // it did not read a byte
    CHECK(host.running());                      // and it did not stop the clock either
}
