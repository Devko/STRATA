// The live visuals' taps on the audio side (part 2): each track's meter, the master's samples
// for the scopes (core/scope.hpp), and each LFO's phase. What the screens will draw from.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include "lfo.hpp"
#include "scope.hpp"
#include "track.hpp"

#include <atomic>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

namespace {

int16_t g_left[ScopeRing::kFrames], g_right[ScopeRing::kFrames];

uint16_t track_level(int t) {
    return strata::test::engine().status().track_milli[t].load(std::memory_order_relaxed);
}

// Empty the scope ring; returns the frames taken and the loudest of them.
uint32_t drain(Engine &engine, int &loudest) {
    uint32_t total = 0;
    loudest = 0;
    for (;;) {
        const uint32_t got = engine.scope().take(g_left, g_right, ScopeRing::kFrames);
        for (uint32_t i = 0; i < got; i++) {
            const int a = g_left[i] < 0 ? -g_left[i] : g_left[i];
            if (a > loudest) loudest = a;
        }
        total += got;
        if (got == 0) return total;
    }
}

}  // namespace

TEST(taps_a_sounding_track_moves_its_meter_and_only_its_own) {
    Engine &engine = fresh();
    pump(400);                                   // whatever fresh() left ringing falls away
    int loudest = 0;
    drain(engine, loudest);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(track_level(t)), 0);

    FakeHost::the().set_running(true);
    put_trig(0, 0);                              // the kick
    FakeHost::the().deliver_tick(0, 0);
    pump(4);
    const uint16_t hit = track_level(0);
    CHECK(hit > 100);                            // a kick's first blocks: well up
    CHECK(hit <= 1000);
    for (int t = 1; t < kTracks; t++) CHECK_EQ(int(track_level(t)), 0);

    // The scope got the output: every second frame of every block, and it is not silence.
    const uint32_t frames = drain(engine, loudest);
    CHECK(frames >= 4u * ENAGA_BLOCK_FRAMES / 2u);
    CHECK(loudest > 1000);

    // Stopped, the meter falls: about 20 dB in a third of a second, like the master's.
    // Nobody taking meanwhile, so the ring fills and the rest is dropped, never waited for.
    FakeHost::the().set_running(false);
    const uint32_t dropped = engine.scope().dropped();
    pump(1200);
    CHECK(track_level(0) < hit / 8);
    CHECK_EQ(engine.scope().dropped() - dropped, 1200u * ENAGA_BLOCK_FRAMES / 2u - ScopeRing::kFrames);
    drain(engine, loudest);
}

TEST(taps_an_lfo_publishes_its_phase) {
    Engine &engine = fresh();
    const int track = 1;
    const auto set = [&](int param, uint8_t value) {
        post(Cmd::set_param, uint8_t(track), 0, uint8_t(param), value);
    };
    set(kLfoWave, lfo_wave_byte(LfoWave::triangle));
    set(kLfoRate, 40);
    set(kLfoDepth, 100);
    set(kLfoDest, lfo_dest_byte(2));
    pump();
    CHECK(engine.lfo_on(track));
    int moves = 0;
    uint8_t last = engine.status().lfo_phase[track].load(std::memory_order_relaxed);
    for (int i = 0; i < 400; i++) {
        pump();
        const uint8_t now = engine.status().lfo_phase[track].load(std::memory_order_relaxed);
        moves += now != last;
        last = now;
    }
    CHECK(moves > 20);
    set(kLfoDepth, 0);
    pump();
    int loudest = 0;
    drain(engine, loudest);
}
