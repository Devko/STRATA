// What a just-initialised Engine is, before any project reaches it.
//
// Every other app/ test goes through tests/apprig.hpp's `fresh()`, which posts a blank project and
// so rewires every track from that project's kit. That hides `Engine::init`'s own wiring
// completely: mutation testing deleted the default machine IDs, `rewire_machines()`, the
// per-machine `init` calls, the hat choke and the placeholder sample one at a time, and the
// whole suite passed every time. On the unit that state is not a corner: with no card, or a
// first run, it is what the instrument IS until the user loads something.
//
// So this file owns an Engine nobody has touched. File scope, as everywhere: it is far over the
// 16 KiB frame rule.
//
// The default PATTERN's notes, accents and locks (app/engine.cpp, load_default_pattern) are
// content, not behaviour -- a different beat is not a bug -- so they are deliberately not pinned
// note by note. What is pinned is that it exists and plays.
#include "check.hpp"
#include "fakehost.hpp"

#include "engine.hpp"
#include "project.hpp"

#include <cmath>

#include <enaga/abi.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

Engine g_boot;
bool g_ready = false;
constexpr int kBlock = int(ENAGA_BLOCK_FRAMES);
float g_out[2 * kBlock];

Engine &boot() {
    if (!g_ready) {
        FakeHost::the().reset();
        g_ready = g_boot.init(float(ENAGA_SAMPLE_RATE));
    }
    return g_boot;
}

// Energy over `blocks` blocks, and whether every sample was finite.
double render(int blocks, bool *finite = nullptr) {
    double energy = 0.0;
    bool ok = true;
    for (int b = 0; b < blocks; b++) {
        for (float &x : g_out) x = 0.0f;
        g_boot.process(nullptr, g_out, uint32_t(kBlock));
        for (float x : g_out) {
            if (!std::isfinite(x)) ok = false;
            energy += double(x) * double(x);
        }
    }
    if (finite) *finite = ok;
    return energy;
}

void hit(int track, uint8_t note = 60, uint8_t velocity = 110) {
    Command command;
    command.cmd = Cmd::trigger_note;
    command.track = uint8_t(track);
    command.a = note;
    command.b = velocity;
    g_boot.commands().post(command);
}

// Silence first: nothing of one test's hits rings into the next.
void quiet() {
    FakeHost::the().set_running(false);
    for (int i = 0; i < 2000; i++) {
        if (render(1) < 1e-12) return;
    }
}

}  // namespace

TEST(boot_the_default_kit_is_the_spec_kit) {
    // docs/SPEC.md: KICK, SNARE, HAT, HAT, PERC, VA MONO, VA POLY, SAMPLE -- in the engine's
    // wiring and in the kit a first save would write.
    Engine &e = boot();
    CHECK(g_ready);
    const MachineId want[kTracks] = {MachineId::kick,  MachineId::snare,   MachineId::hat,
                                     MachineId::hat,   MachineId::perc,    MachineId::va_mono,
                                     MachineId::va_poly, MachineId::sample};
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(e.machine_id(t)), int(want[t]));
        CHECK_EQ(int(e.project().kit.tracks[t].machine), int(want[t]));
        CHECK(e.machine(t) != nullptr);
    }
    // Two tracks hosting the same machine type must still be two machines.
    CHECK(e.machine(2) != e.machine(3));
}

TEST(boot_every_default_track_sounds_and_stays_finite) {
    // A hit on each track alone, on the engine as init() left it. An uninitialised machine is
    // silent or NaN; the SAMPLE track has no file yet and plays the placeholder.
    boot();
    for (int t = 0; t < kTracks; t++) {
        quiet();
        hit(t);
        bool finite = false;
        const double energy = render(60, &finite);
        if (!(energy > 1e-3)) std::printf("    track %d is silent after a hit (%g)\n", t, energy);
        CHECK(energy > 1e-3);
        CHECK(finite);
    }
}

TEST(boot_the_closed_hat_chokes_the_open_one) {
    // Tracks 2 and 3 are both HAT, closed and open, and the closed one cuts the open one off --
    // the 909 behaviour the default kit is voiced for. Measured as the open hat's tail long after
    // the closed one's own short decay is over.
    boot();
    quiet();
    hit(3);
    render(4);
    const double alone = render(120);

    quiet();
    hit(3);
    render(2);
    hit(2);
    render(2);
    const double choked = render(120);

    CHECK(alone > 1e-4);
    if (!(choked < alone * 0.5)) std::printf("    open tail %g, choked %g\n", alone, choked);
    CHECK(choked < alone * 0.5);
}

TEST(boot_the_default_pattern_plays_when_the_clock_runs) {
    // No card, first run: PLAY must make a beat, not silence.
    Engine &e = boot();
    quiet();
    FakeHost &host = FakeHost::the();
    host.set_running(true);
    double energy = 0.0;
    for (int i = 0; i < 96; i++) {
        host.deliver_tick(uint32_t(i), 0);
        energy += render(1);
    }
    host.set_running(false);
    CHECK(energy > 1e-2);
    int trigs = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < 16; s++) trigs += (e.pattern().tracks[t].steps[s].flags & kStepTrig) != 0;
    }
    CHECK(trigs > 8);
}
