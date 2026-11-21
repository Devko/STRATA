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

    // And cut any tails the test before left ringing. A kick decays for well over a block, so
    // without this a test that measures "silence before frame 40" measures the previous test's
    // note. `Cmd::stop` with c = 1 is the app's own second STOP press, which is what it is for.
    //
    // It does NOT silence the master reverb, whose tail outlives anything here: a test that
    // needs to know where in a block a note started measures the onset against that tail rather
    // than demanding exact silence.
    post(Cmd::stop, 0, 0, 0, 0, 1);
    pump(4);
    return g_engine;
}

}  // namespace strata::test
