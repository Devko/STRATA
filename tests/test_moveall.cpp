// REL ALL and LEN ALL's arithmetic (core/moveall.hpp). The engine's use of it, rest included, is
// in tests/test_engine.cpp, with each machine's own DEC name; the gesture in tests/test_ui.cpp.
#include "check.hpp"

#include "moveall.hpp"
#include "project.hpp"
#include "track.hpp"     // dsp's page numbering, which core/moveall.cpp states again

using namespace strata;

TEST(all_turn_moves_two_a_click_holds_the_range_and_stops_on_zero) {
    CHECK_EQ(int(all_turn(0, 1)), 2);
    CHECK_EQ(int(all_turn(0, -3)), -6);
    CHECK_EQ(int(all_turn(60, 5)), kAllRange);
    CHECK_EQ(int(all_turn(-60, -5)), -kAllRange);
    // The detent: a turn through 0 lands on it, from either side, however fast.
    CHECK_EQ(int(all_turn(2, -1)), 0);
    CHECK_EQ(int(all_turn(4, -10)), 0);
    CHECK_EQ(int(all_turn(-4, 10)), 0);
    CHECK_EQ(int(all_turn(-2, 1)), 0);
    CHECK_EQ(int(all_turn(0, -1)), -2);           // and leaves it on the next click
    CHECK_EQ(int(all_turn(int8_t(100), 0)), kAllRange);
}

TEST(rel_all_is_exact_at_rest_and_clamps_the_byte) {
    for (int v = 0; v < 128; v++) CHECK_EQ(int(rel_all_byte(uint8_t(v), 0)), v);
    CHECK_EQ(int(rel_all_byte(40, -20)), 20);
    CHECK_EQ(int(rel_all_byte(40, 20)), 60);
    CHECK_EQ(int(rel_all_byte(10, -64)), 0);
    CHECK_EQ(int(rel_all_byte(100, 64)), 127);
}

TEST(len_all_is_exact_at_rest_an_octave_every_32_and_never_makes_a_gate_vanish) {
    const uint32_t gates[5] = {0u, 1u, 77u, 5512u, 4000000u};
    for (uint32_t f : gates) CHECK_EQ(len_all_frames(f, 0), f);
    CHECK_EQ(len_all_frames(0, -64), 0u);         // no gate stays no gate
    CHECK_EQ(len_all_frames(1000, 32), 2000u);
    CHECK_EQ(len_all_frames(1000, 64), 4000u);
    CHECK_EQ(len_all_frames(1000, -32), 500u);
    CHECK_EQ(len_all_frames(1000, -64), 250u);
    CHECK_EQ(len_all_frames(1, -64), 1u);
    CHECK_EQ(len_all_frames(1000, 16), 1414u);
    CHECK_EQ(len_all_frames(1000, -16), 707u);
    // Monotonic across the whole range, and past it held to the ends.
    for (int a = -kAllRange; a < kAllRange; a++) {
        CHECK(len_all_frames(48000, int8_t(a)) <= len_all_frames(48000, int8_t(a + 1)));
    }
    CHECK_EQ(len_all_frames(1000, 100), 4000u);
    CHECK_EQ(len_all_frames(1000, -100), 250u);
    CHECK_EQ(len_all_percent(0), 100);
    CHECK_EQ(len_all_percent(-64), 25);
    CHECK_EQ(len_all_percent(64), 400);
    CHECK_EQ(len_all_percent(-32), 50);
}

// Which bytes REL ALL moves. That each is the machine's own DEC is held by the engine tests,
// which can ask a machine its names.
TEST(release_params_are_dec_and_where_the_machine_reads_asr_rel) {
    static_assert(page_first(Page::amp) == 8 && kAsrFirst + 2 == 26,
                  "core/moveall.cpp's page numbers have drifted from dsp/track.hpp");
    for (int id = 0; id < int(MachineId::count); id++) {
        uint8_t params[2] = {255, 255};
        const int n = machine_release_params(uint8_t(id), params);
        if (id == int(MachineId::midi)) {
            CHECK_EQ(n, 0);
            continue;
        }
        CHECK_EQ(n, machine_reads_asr(uint8_t(id)) ? 2 : 1);
        CHECK(params[0] >= 8 && params[0] < 12);  // on the AMP page
        if (n == 2) CHECK_EQ(int(params[1]), kAsrFirst + 2);
    }
    uint8_t params[2] = {};
    CHECK_EQ(machine_release_params(uint8_t(MachineId::count), params), 0);
    CHECK_EQ(machine_release_params(uint8_t(MachineId::sample), params), 2);   // DEC, and REL
    CHECK_EQ(int(params[0]), 9);
    CHECK_EQ(machine_release_params(uint8_t(MachineId::grain), params), 2);
    CHECK_EQ(int(params[0]), 10);
}

// One step each side of rest: REL ALL and LEN ALL already move there, and a turn from just off
// zero still lands on the detent.
TEST(all_amounts_one_off_rest_already_move) {
    CHECK_EQ(int(rel_all_byte(60, 1)), 61);
    CHECK_EQ(int(rel_all_byte(60, -1)), 59);
    CHECK(len_all_frames(48000, 1) > 48000u);
    CHECK(len_all_frames(48000, -1) < 48000u);
    CHECK_EQ(int(all_turn(1, -1)), 0);
    CHECK_EQ(int(all_turn(-1, 1)), 0);
}
