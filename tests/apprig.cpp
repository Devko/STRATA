// The shared app/ test rig. tests/apprig.hpp says why it is shared and why both contexts have
// to be driven by hand.
#include "apprig.hpp"

#include "fakehost.hpp"
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

Engine &fresh() {
    FakeHost &host = FakeHost::the();
    host.reset();
    if (!g_ready) {
        g_ready = g_engine.init(float(ENAGA_SAMPLE_RATE)) && g_storage.init(g_engine);
    }

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

    // And a kit that can make a sound. `project_init` zeroes the kit, so LVL is 0 on every track
    // and the whole instrument is silent -- correct for a blank project and useless as a
    // starting point for a test about whether anything was heard. Mid values with the level up is
    // the patch tests/test_va.cpp and the others start from.
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) incoming.kit.tracks[t].params[i] = 64;
        incoming.kit.tracks[t].params[11] = 127;     // LVL on every machine (dsp/track.hpp)
    }

    // Zeroing parameters 12-15 to get a DRY kit was tried and reverted: that page is the insert
    // chain's, not a pair of send levels, so zeroing it changes the signal path and it broke two
    // passing tests. The master reverb's tail therefore outlives `fresh()`, and a test that wants
    // to know WHERE in a block a note started has to measure it relative to that tail rather
    // than against silence.

    post(Cmd::project_loaded);

    // The PERFORMANCE layer, which `Cmd::project_loaded` does not touch: macros, the morph, the
    // throws, the mutes and auto-fill all live in the engine rather than in the project, so
    // without this they survive from one test to the next.
    //
    // It is not tidiness. `apply_performance` builds what the machines read as kit -> morph ->
    // macros, so a macro left ASSIGNED and at full position by an earlier test overrides the
    // kit -- and a test that then sets a kit parameter and listens finds its knob does nothing.
    // That cost real time to find: it looked like the per-block-copy bug coming back.
    for (int m = 0; m < kMacros; m++) post(Cmd::macro_clear, 0, 0, uint8_t(m));
    post(Cmd::set_morph);
    post(Cmd::set_auto_fill);

    // The scene SLOTS are deliberately not reset. `Cmd::scene_select` refuses a scene index
    // outside 0..kScenes-1, so there is no "unassign" to post -- and none is needed: the scenes
    // themselves come back empty with the re-initialised project, and core/perform.hpp's rule is
    // that an empty scene behaves as the kit. Posting an out-of-range index would only bump the
    // refusal counter, which a test may be about to read.
    for (int t = 0; t < kTracks; t++) {
        post(Cmd::set_throw, uint8_t(t));                   // a = 0: off
        post(Cmd::set_mute, uint8_t(t), 0, 0);              // unmuted, immediately
    }
    pump(4);

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
    return g_engine;
}

}  // namespace strata::test
