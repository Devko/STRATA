// LIVE RND's arithmetic (core/liverand.hpp). The scheduler under it is in tests/test_scheduler.cpp,
// the engine and LOCK in tests/test_lockstore.cpp, the gesture in tests/test_ui.cpp.
#include "check.hpp"

#include "liverand.hpp"

#include <cstring>

using namespace strata;

TEST(live_rnd_turn_walks_one_unit_a_click_and_stops_at_both_ends) {
    CHECK_EQ(int(live_rnd_turn(0, 1)), 1);
    CHECK_EQ(int(live_rnd_turn(1, 2)), 3);
    CHECK_EQ(int(live_rnd_turn(3, -1)), 2);
    CHECK_EQ(int(live_rnd_turn(0, -1)), 0);
    CHECK_EQ(int(live_rnd_turn(4, 9)), int(kLiveRndMax));
    CHECK_EQ(int(live_rnd_turn(5, -40)), 0);
    CHECK_EQ(int(live_rnd_turn(200, 0)), 0);       // a damaged byte is OFF
    CHECK_EQ(int(live_rnd_turn(200, 1)), 1);
}

TEST(live_play_mode_overrides_only_an_armed_track_while_the_unit_is_on) {
    // On and armed: the unit's RND mode, whatever the pattern says.
    CHECK(live_play_mode(uint8_t(PlayMode::fwd), 1, 0xFF, 0) == PlayMode::rnd1);
    CHECK(live_play_mode(uint8_t(PlayMode::rev), 3, 0xFF, 4) == PlayMode::rnd4);
    CHECK(live_play_mode(uint8_t(PlayMode::rnd2), 5, 0x80, 7) == PlayMode::rnd16);
    // Off, or not armed: the pattern's own mode.
    CHECK(live_play_mode(uint8_t(PlayMode::ping), 0, 0xFF, 2) == PlayMode::ping);
    CHECK(live_play_mode(uint8_t(PlayMode::rev), 2, 0xFB, 2) == PlayMode::rev);
    CHECK(live_play_mode(uint8_t(PlayMode::fwd), 2, 0x00, 0) == PlayMode::fwd);
    // A byte this build does not know plays forward, and a damaged unit is off.
    CHECK(live_play_mode(uint8_t(PlayMode::count), 0, 0xFF, 0) == PlayMode::fwd);
    CHECK(live_play_mode(uint8_t(PlayMode::rev), uint8_t(kLiveRndMax + 1), 0xFF, 0) ==
          PlayMode::rev);
    // A track outside 0..7 is never armed.
    CHECK(live_play_mode(uint8_t(PlayMode::fwd), 1, 0xFF, -1) == PlayMode::fwd);
    CHECK(live_play_mode(uint8_t(PlayMode::fwd), 1, 0xFF, kTracks) == PlayMode::fwd);
    // Every unit maps to the RND mode of its size, in order.
    for (uint8_t u = 1; u <= kLiveRndMax; u++) {
        CHECK_EQ(int(live_play_mode(0, u, 0x01, 0)), int(PlayMode::rnd1) + u - 1);
    }
}

TEST(live_rnd_names_and_counts) {
    CHECK(std::strcmp(live_rnd_name(0), "OFF") == 0);
    CHECK(std::strcmp(live_rnd_name(1), "RND1") == 0);
    CHECK(std::strcmp(live_rnd_name(3), "RND4") == 0);
    CHECK(std::strcmp(live_rnd_name(5), "RND16") == 0);
    CHECK(std::strcmp(live_rnd_name(9), "OFF") == 0);
    CHECK_EQ(live_rnd_count(0x00), 0);
    CHECK_EQ(live_rnd_count(0xFF), 8);
    CHECK_EQ(live_rnd_count(0x81), 2);
}
