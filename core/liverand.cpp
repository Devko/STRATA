#include "liverand.hpp"

namespace strata {

uint8_t live_rnd_turn(uint8_t current, int clicks) {
    const int from = current > kLiveRndMax ? 0 : int(current);
    const int to = from + clicks;
    return uint8_t(to < 0 ? 0 : (to > int(kLiveRndMax) ? int(kLiveRndMax) : to));
}

PlayMode live_play_mode(uint8_t pattern_play, uint8_t unit, uint8_t armed, int track) {
    const bool on = unit != kLiveRndOff && unit <= kLiveRndMax && track >= 0 && track < kTracks &&
                    ((armed >> track) & 1u) != 0;
    if (on) return PlayMode(uint8_t(PlayMode::rnd1) + unit - 1u);
    return pattern_play < uint8_t(PlayMode::count) ? PlayMode(pattern_play) : PlayMode::fwd;
}

const char *live_rnd_name(uint8_t unit) {
    return unit == kLiveRndOff || unit > kLiveRndMax
               ? "OFF"
               : play_mode_name(PlayMode(uint8_t(PlayMode::rnd1) + unit - 1u));
}

int live_rnd_count(uint8_t armed) {
    int count = 0;
    for (int t = 0; t < kTracks; t++) count += (armed >> t) & 1;
    return count;
}

}  // namespace strata
