// The arpeggiator's note logic (core/arp.hpp): the bytes, the cycles, the hits.
#include "check.hpp"

#include "arp.hpp"

#include <cstring>

using namespace strata;

namespace {

ArpSettings settings(ArpMode mode, int octaves) {
    ArpSettings s;
    s.mode = mode;
    s.octaves = uint8_t(octaves);
    return s;
}

}  // namespace

TEST(arp_zeroed_bytes_are_off_and_sensible) {
    const uint8_t zero[kArpBytes] = {};
    const ArpSettings s = ArpSettings::make(zero);
    CHECK(!s.on());
    CHECK_EQ(int(s.rate_ticks), 96);
    CHECK_EQ(int(s.octaves), 1);
    CHECK_EQ(int(s.gate_percent), 50);
    uint8_t out[kArpMaxCycle];
    const uint8_t notes[3] = {60, 64, 67};
    CHECK_EQ(arp_cycle(notes, 3, s, out), 0);
}

TEST(arp_bytes_cover_every_zone_and_end_at_the_top) {
    // Every mode, rate and octave is reachable, in order, and 127 is the last of each.
    int last_mode = -1, last_rate = -1, last_oct = 0;
    for (int b = 0; b <= 127; b++) {
        const int m = arp_mode_index(uint8_t(b));
        const int r = arp_rate_index(uint8_t(b));
        const int o = arp_octaves(uint8_t(b));
        CHECK(m == last_mode || m == last_mode + 1);
        CHECK(r == last_rate || r == last_rate + 1);
        CHECK(o == last_oct || o == last_oct + 1);
        last_mode = m;
        last_rate = r;
        last_oct = o;
    }
    CHECK_EQ(last_mode, int(ArpMode::count) - 1);
    CHECK_EQ(last_rate, kArpRates - 1);
    CHECK_EQ(last_oct, kArpMaxOctaves);
    CHECK_EQ(arp_gate_percent(127), 99);
    // Slow to fast.
    for (int i = 1; i < kArpRates; i++) CHECK(arp_rate_ticks(i) < arp_rate_ticks(i - 1));
    CHECK(std::strcmp(arp_rate_name(0), "1/4") == 0);
    CHECK(std::strcmp(arp_rate_name(127), "1/32T") == 0);
    CHECK(std::strcmp(arp_mode_name(0), "OFF") == 0);
    CHECK(std::strcmp(arp_mode_name(127), "RND") == 0);
    // Out-of-range bytes clamp rather than read past a table.
    CHECK_EQ(arp_mode_index(255), int(ArpMode::count) - 1);
}

TEST(arp_cycles_up_down_updown_order_over_octaves) {
    const uint8_t played[3] = {67, 60, 64};     // played out of order
    uint8_t out[kArpMaxCycle];

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up, 1), out), 3);
    CHECK_EQ(int(out[0]), 60);
    CHECK_EQ(int(out[1]), 64);
    CHECK_EQ(int(out[2]), 67);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up, 2), out), 6);
    CHECK_EQ(int(out[3]), 72);
    CHECK_EQ(int(out[5]), 79);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::down, 1), out), 3);
    CHECK_EQ(int(out[0]), 67);
    CHECK_EQ(int(out[2]), 60);

    // Up and down without repeating either end: 60 64 67 64, then 60 again.
    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up_down, 1), out), 4);
    CHECK_EQ(int(out[3]), 64);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::order, 1), out), 3);
    CHECK_EQ(int(out[0]), 67);
    CHECK_EQ(int(out[1]), 60);

    // One note over four octaves: a classic octave arp.
    const uint8_t one[1] = {48};
    CHECK_EQ(arp_cycle(one, 1, settings(ArpMode::up_down, 4), out), 6);
    CHECK_EQ(int(out[0]), 48);
    CHECK_EQ(int(out[3]), 84);
    CHECK_EQ(int(out[4]), 72);
    CHECK_EQ(int(out[5]), 60);
    // A single note, one octave, up/down: just the note.
    CHECK_EQ(arp_cycle(one, 1, settings(ArpMode::up_down, 1), out), 1);

    // Notes past 127 are dropped, never wrapped.
    const uint8_t high[1] = {120};
    CHECK_EQ(arp_cycle(high, 1, settings(ArpMode::up, 4), out), 1);

    // More than eight inputs are capped, and the cycle fits.
    uint8_t many[12];
    for (int i = 0; i < 12; i++) many[i] = uint8_t(40 + i);
    CHECK_EQ(arp_cycle(many, 12, settings(ArpMode::up_down, 4), out), 2 * 32 - 2);
}

TEST(arp_hits_walk_the_cycle_and_random_repeats_per_seed) {
    const uint8_t cycle[4] = {60, 64, 67, 72};
    for (uint32_t h = 0; h < 12; h++) CHECK_EQ(int(arp_note(cycle, 4, ArpMode::up, h, 0)), int(cycle[h % 4]));

    // Random: always a note of the cycle, the same for the same seed and hit, and not one note.
    bool seen[4] = {};
    for (uint32_t h = 0; h < 64; h++) {
        const uint8_t a = arp_note(cycle, 4, ArpMode::random, h, 1234);
        CHECK_EQ(int(a), int(arp_note(cycle, 4, ArpMode::random, h, 1234)));
        for (int i = 0; i < 4; i++) seen[i] = seen[i] || a == cycle[i];
    }
    for (int i = 0; i < 4; i++) CHECK(seen[i]);
    int differ = 0;
    for (uint32_t h = 0; h < 32; h++) {
        differ += arp_note(cycle, 4, ArpMode::random, h, 1) != arp_note(cycle, 4, ArpMode::random, h, 2);
    }
    CHECK(differ > 8);
}
