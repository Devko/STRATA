// The shared app/ test rig. tests/apprig.hpp says why it is shared and why both contexts have
// to be driven by hand.
#include "apprig.hpp"

#include "fakehost.hpp"
#include "factory.hpp"
#include "liverand.hpp"
#include "project.hpp"

namespace strata::test {
namespace {

Engine g_engine;
Storage g_storage;
float g_out[kRigStereo];
bool g_ready = false;

}  // namespace

Engine &engine() { return g_engine; }
Storage &storage() { return g_storage; }
const float *last_block() { return g_out; }

float peak_of(const float *x, int n) {
    float best = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > best) best = a;
    }
    return best;
}

float pump(int blocks) {
    float peak = 0.0f;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < kRigStereo; i++) g_out[i] = 0.0f;
        g_engine.process(nullptr, g_out, uint32_t(kRigBlock));
        const float block_peak = peak_of(g_out, kRigStereo);
        if (block_peak > peak) peak = block_peak;
        g_storage.service();
        FakeHost::the().complete_loads();
    }
    return peak;
}

bool settle(int quiet_blocks, int limit) {
    int quiet = 0;
    for (int i = 0; i < limit; i++) {
        pump();
        // 1e-6 rather than 0: the reverb's tail decays geometrically and a float never quite
        // reaches zero, so an exact test would never finish.
        if (peak_of(g_out, kRigStereo) < 1e-6f) {
            if (++quiet >= quiet_blocks) return true;
        } else {
            quiet = 0;
        }
    }
    return false;
}

bool pump_until_idle(int limit) {
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

void post(Cmd cmd, uint8_t track, uint8_t step, uint8_t a, uint8_t b, uint8_t c) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.step = step;
    command.a = a;
    command.b = b;
    command.c = c;
    g_engine.commands().post(command);
}

void post_delta(Cmd cmd, int track, int step, uint8_t a, int delta) {
    const int clamped = delta < -120 ? -120 : (delta > 120 ? 120 : delta);
    post(cmd, uint8_t(track), uint8_t(step), a, uint8_t(int8_t(clamped)));
}

uint8_t kit_value(int track, int param) { return g_engine.params(track).v[param]; }

void put_trig(int track, int step) {
    post(Cmd::toggle_trig, uint8_t(track), uint8_t(step));
    pump();
}

void set_send_byte(uint8_t field, uint8_t target) {
    const uint8_t now = g_engine.project().settings.sends[field];
    // In the FIELD'S own units, which is what the command carries: divisions for the delay's
    // time, a sign for the ping-pong switch, counts for a knob (dsp/buses.hpp).
    int delta;
    if (field == uint8_t(SendField::delay_seconds)) {
        delta = delay_division_index(target) - delay_division_index(now);
    } else if (field == uint8_t(SendField::delay_ping_pong)) {
        delta = target >= 64 ? +1 : -1;
    } else {
        delta = int(target) - int(now);
    }
    Command command;
    command.cmd = Cmd::set_sends;
    command.a = field;
    command.b = uint8_t(int8_t(delta));
    g_engine.commands().post(command);
    pump();
}

void set_master_byte(uint8_t field, uint8_t target) {
    const int now = int(g_engine.project().settings.master[field]);
    Command command;
    command.cmd = Cmd::set_master;
    command.a = field;
    command.b = uint8_t(int8_t(int(target) - now));
    g_engine.commands().post(command);
    pump();
}

SendSettings default_sends() {
    const ProjectSettings defaults;
    SendSettings sends;
    sends_from_bytes(sends, defaults.sends,
                     6000.0f / float(defaults.tempo_centi_bpm));
    return sends;
}

MasterSettings default_master() {
    const ProjectSettings defaults;
    MasterSettings master;
    master_from_bytes(master, defaults.master);
    return master;
}

namespace {
Project g_factory;
SceneSet g_factory_scenes;
MacroSet g_factory_macros;
bool g_factory_ready = false;

void build_factory() {
    if (g_factory_ready) return;
    factory_project(g_factory, g_factory_scenes, g_factory_macros);
    g_factory_ready = true;
}
}  // namespace

const Project &factory() { build_factory(); return g_factory; }
const SceneSet &factory_scenes() { build_factory(); return g_factory_scenes; }
const MacroSet &factory_macros() { build_factory(); return g_factory_macros; }

Engine &fresh() {
    FakeHost &host = FakeHost::the();
    // init() before the reset, not after: init reads the settings and the card, and those reads
    // belong to whichever test happened to run first in the process. Counted after the reset,
    // they made that test's own read counts wrong -- a test that passed in the full suite failed
    // with --only, or first in a shard.
    if (!g_ready) {
        g_ready = g_engine.init(float(ENAGA_SAMPLE_RATE)) && g_storage.init(g_engine);
    }
    host.reset();

    // A card SWAP, not just "a card is there". Storage remembers whether the project is complete
    // on the card, and the fake's reset() wiped the card without telling it -- which is exactly
    // what pulling an SD card does on the unit. Doing it as a transition keeps the tests honest
    // about a state the app really reaches, and it is what clears that flag.
    g_storage.on_card(false);
    g_storage.on_card(true);

    // The app's own way back to a clean project, rather than a second init(): project_init into
    // the load buffer and tell the audio side. `request_new` would do the same and also rename.
    Project &incoming = g_engine.project_for_load();
    project_init(incoming);
    // The scenes and the macros are not in `Project`: they load into their own sets beside it
    // (Engine::scenes_for_load), so `project_init` leaves them as the last test left them. A
    // scene captured by one test went on moving the next test's sound whenever its morph was off
    // rest -- found by running the suite in a shuffled order. Blank, as Storage's NEW makes them.
    perform_init(g_engine.scenes_for_load(), g_engine.macros_for_load());

    // And a kit that can make a sound. `project_init` zeroes the kit, so LVL is 0 on every track
    // and the whole instrument is silent -- correct for a blank project and useless as a
    // starting point for a test about whether anything was heard. Mid values with the level up is
    // the patch tests/test_va.cpp and the others start from.
    for (int t = 0; t < kTracks; t++) {
        // The SOUND parameters only: the LFO's four bytes stay 0, which is OFF (dsp/lfo.hpp).
        // At 64 they would be a sine at 0.25 Hz moving parameter 8 on every track.
        for (int i = 0; i < kKitSoundParams; i++) incoming.kit.tracks[t].params[i] = 64;
        incoming.kit.tracks[t].params[11] = 127;     // LVL on every machine (dsp/track.hpp)
    }

    // Zeroing parameters 12-15 to get a DRY kit was tried and reverted: that page is the insert
    // chain's, not a pair of send levels, so zeroing it changes the signal path and it broke two
    // passing tests. The master reverb's tail therefore outlives `fresh()`, and a test that wants
    // to know WHERE in a block a note started has to measure it relative to that tail rather
    // than against silence.

    post(Cmd::project_loaded);

    // The TRANSPORT's derived state and any take, which nothing above reaches: the engine keeps
    // its bar count, pass origin and pending ratchets across a fake-host reset, and a take left
    // armed or waiting for the card refuses the next arm. With them carried over, a test passed
    // or failed depending on which test ran before it -- invisible in one serial run, found the
    // first time the suite was split into shards (tools/ci_local.sh). A restart is what the
    // engine is told when the clock goes back to 0, and cancel plus release empty the capture.
    post(Cmd::restart);
    post(Cmd::resample_cancel);
    post(Cmd::resample_release);

    // The PERFORMANCE layer, which `Cmd::project_loaded` does not touch: macros, the morph, the
    // throws, the mutes and auto-fill all live in the engine rather than in the project, so
    // without this they survive from one test to the next.
    //
    // It is not tidiness. `apply_performance` builds what the machines read as kit -> morph ->
    // macros, so a macro left ASSIGNED and at full position by an earlier test overrides the
    // kit -- and a test that then sets a kit parameter and listens finds its knob does nothing.
    // That cost real time to find: it looked like the per-block-copy bug coming back.
    for (int m = 0; m < kMacros; m++) {
        post(Cmd::macro_clear, 0, 0, uint8_t(m));
        post(Cmd::set_macro, 0, 0, uint8_t(m), 0);       // and its knob at rest: clear keeps it
    }
    post(Cmd::set_morph);
    post(Cmd::set_auto_fill);
    post(Cmd::set_move_all, 0, 0, 0, 0);                    // REL ALL to rest
    post(Cmd::set_move_all, 0, 0, 1, 0);                    // LEN ALL to rest
    post(Cmd::set_live_rnd, 0, 0, 0, kLiveRndAllTracks);    // LIVE RND off, every track armed
    post(Cmd::set_energy, 0, 0, 0);                         // ENERGY to rest
    post(Cmd::step_stutter);                                // b = 0: let go
    post(Cmd::set_click, 0, 0, 0);                         // the metronome off: it is the
                                                            // engine's, not the project's

    // The SEND and MASTER settings come back with `Cmd::project_loaded` above, because
    // `project_init` reset the fourteen bytes they are derived from and the engine re-derives
    // them on a load. Nothing to post here -- but it is worth saying, because before those
    // bytes existed nothing put them back at all, and a test that lengthened the reverb handed
    // the next test a 10-second tail that `settle()` cannot wait out inside its block limit.

    // The scene SLOTS need nothing here: `Cmd::project_loaded` puts A and B back from the blank
    // project's settings, and the scenes themselves were blanked with `perform_init` above, so
    // core/perform.hpp's rule that an empty scene behaves as the kit holds.
    for (int t = 0; t < kTracks; t++) {
        post(Cmd::set_throw, uint8_t(t));                   // a = 0: off
        post(Cmd::set_mute, uint8_t(t), 0, 0);              // unmuted, immediately
    }
    pump(4);

    // The card swap again, now that the blank project is the engine's. The first one, above, ran
    // while the engine still held the PREVIOUS test's kit, and a swap re-requests every sample
    // the kit names -- so the previous test's samples were queued against an empty card, and the
    // next test inherited the read and the failure ("NO SUCH FILE" in last_error()). Found by
    // running the suite in shards (tools/ci_local.sh), where a test's predecessor changes.
    g_storage.on_card(false);
    g_storage.on_card(true);

    // And cut any tails the test before left ringing. A kick decays for well over a block, so
    // without this a test that measures "silence before frame 40" measures the previous test's
    // note.
    //
    // The flag is `a`, not `c`. This was written as `c = 1` and so asked for an ordinary STOP:
    // a RELEASE with a 60 ms tail rather than the kill its own comment described.
    // app/commands.hpp documents the field ("a: 1 = cut all voices") and app/engine.cpp reads
    // `command.a`, and the mistake cost real time -- it is most of why two onset tests in
    // tests/test_app.cpp fought a tail that should not have been there.
    //
    // Even with it right, this does NOT silence the master reverb, whose tail outlives anything
    // here: a test that needs to know where in a block a note started still measures the onset
    // against that tail rather than demanding exact silence.
    post(Cmd::stop, 0, 0, 1);
    pump(4);

    // And wait for genuine silence before handing the engine over.
    //
    // This is what makes an audio measurement in one test independent of the test before it. The
    // kill above takes the voices; it cannot take the master delay and reverb, which ring for
    // seconds on whatever was playing -- and on the click the kill itself makes. Every test that
    // asked "how loud was this block" without settling was measuring a share of the previous
    // test's tail, and several of them were tuned around it before the cause was understood.
    //
    // It costs a few hundred silent blocks per test, which is microseconds: the whole suite runs
    // in about a second.
    settle();

    // A PROJECT error left by the test before ("PATTERN LOST" from a damaged-file test) is kept
    // by Storage until the next save or load begins -- correct on the unit, where it is still the
    // last thing that happened to the project, and a leak here: the next test's
    // `last_error() == nullptr` failed whenever a damaged-file test ran before it, which only a
    // sharded run showed. A save is what clears it on the unit, so the rig does the same and then
    // wipes the card again, so the test starts from an empty one with zero counts.
    if (g_storage.last_error() != nullptr) {
        g_storage.request_save();
        pump_until_idle(2000);
        host.reset();
        g_storage.on_card(false);
        g_storage.on_card(true);
        pump(4);
    }
    return g_engine;
}

}  // namespace strata::test
