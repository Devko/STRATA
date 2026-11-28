// core/onsets: the hits in a sample, for SLC AUTO.
#include "onsets.hpp"

#include "check.hpp"

using namespace strata;

namespace {

constexpr uint32_t kFrames = 48000 * 4;        // four seconds at 48 kHz
int16_t g_pcm[kFrames * 2];                    // file scope: 768 KB is no stack frame

uint32_t g_seed = 1;
int32_t noise() {                              // -32768..32767, the same every run
    g_seed = g_seed * 1664525u + 1013904223u;
    return int32_t(g_seed >> 16) - 32768;
}

// Hiss at about -50 dBFS everywhere: under the floor, so it never counts on its own.
void hiss(int channels) {
    g_seed = 1;
    for (uint32_t i = 0; i < kFrames * uint32_t(channels); i++) g_pcm[i] = int16_t(noise() >> 8);
}

// A drum hit: noise at `peak`, decaying over about 60 ms, added from frame `at`.
void hit(uint32_t at, int32_t peak, int channels) {
    int32_t level = peak;
    for (uint32_t f = at; f < kFrames && level > 0; f++) {
        for (int c = 0; c < channels; c++) {
            int32_t v = g_pcm[f * uint32_t(channels) + uint32_t(c)] + (noise() * level >> 15);
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            g_pcm[f * uint32_t(channels) + uint32_t(c)] = int16_t(v);
        }
        level -= peak / 2880 + 1;
    }
}

SlicePoints find(uint32_t frames, int channels, uint32_t piece = kFrames) {
    OnsetFinder finder;
    for (uint32_t first = 0; first < frames; first += piece) {
        const uint32_t n = frames - first < piece ? frames - first : piece;
        finder.feed(g_pcm + first * uint32_t(channels), first, n, channels);
    }
    SlicePoints out;
    finder.finish(frames, out);
    return out;
}

// The cut is at the start of the hop the hit begins in: at most one hop early, never late.
bool cut_for(uint32_t cut, uint32_t hit_at) {
    return cut <= hit_at && hit_at - cut < OnsetFinder::kHop;
}

}  // namespace

TEST(onsets_finds_each_hit_in_order_and_ranks_them_by_strength) {
    hiss(1);
    const uint32_t at[] = {9000, 31111, 52345, 70000, 95001};
    const int32_t peak[] = {8000, 30000, 4000, 16000, 12000};
    for (int i = 0; i < 5; i++) hit(at[i], peak[i], 1);
    const SlicePoints points = find(kFrames, 1);
    CHECK_EQ(int(points.count), 5);
    for (int i = 0; i < 5; i++) CHECK(cut_for(points.at[i], at[i]));
    // 30000 > 16000 > 12000 > 8000 > 4000.
    CHECK_EQ(int(points.rank[1]), 0);
    CHECK_EQ(int(points.rank[3]), 1);
    CHECK_EQ(int(points.rank[4]), 2);
    CHECK_EQ(int(points.rank[0]), 3);
    CHECK_EQ(int(points.rank[2]), 4);
}

TEST(onsets_hiss_and_a_steady_tone_are_not_hits) {
    hiss(1);
    CHECK_EQ(int(find(kFrames, 1).count), 0);
    // A loud square wave from the first frame: one rise, at frame 0, which is never a point.
    for (uint32_t f = 0; f < kFrames; f++) g_pcm[f] = int16_t((f / 50) % 2 ? 12000 : -12000);
    CHECK_EQ(int(find(kFrames, 1).count), 0);
}

TEST(onsets_a_flam_is_one_hit_cut_at_its_first_stroke) {
    hiss(1);
    hit(20000, 6000, 1);
    hit(20000 + 1200, 25000, 1);                 // 25 ms later and louder: the same hit
    hit(60000, 10000, 1);
    const SlicePoints points = find(kFrames, 1);
    CHECK_EQ(int(points.count), 2);
    CHECK(cut_for(points.at[0], 20000));
    CHECK(cut_for(points.at[1], 60000));
    CHECK_EQ(int(points.rank[0]), 0);            // the flam's strength is its loud stroke
}

TEST(onsets_keeps_the_fifteen_strongest_of_more) {
    hiss(1);
    // 24 hits, 7000 frames apart; the even ones loud, the odd ones quiet.
    for (int i = 0; i < 24; i++) hit(4000 + uint32_t(i) * 7000, i % 2 == 0 ? 20000 : 3000, 1);
    const SlicePoints points = find(kFrames, 1);
    CHECK_EQ(int(points.count), 15);
    int loud = 0;
    bool ranks_seen[15] = {};
    for (int i = 0; i < 15; i++) {
        const uint32_t index = (points.at[i] + OnsetFinder::kHop - 4000) / 7000;
        loud += index % 2 == 0 ? 1 : 0;
        if (i > 0) CHECK(points.at[i] > points.at[i - 1]);
        CHECK(points.rank[i] < 15);
        ranks_seen[points.rank[i]] = true;
    }
    CHECK_EQ(loud, 12);                          // all twelve loud ones, and three quiet ones
    for (bool seen : ranks_seen) CHECK(seen);    // every rank exactly once
}

TEST(onsets_the_same_points_however_the_load_is_sliced) {
    hiss(2);
    hit(12345, 20000, 2);
    hit(40000, 9000, 2);
    hit(77777, 15000, 2);
    const SlicePoints whole = find(kFrames, 2);
    CHECK_EQ(int(whole.count), 3);
    const uint32_t pieces[] = {255, 256, 1000, 4097};
    for (uint32_t piece : pieces) {
        const SlicePoints sliced = find(kFrames, 2, piece);
        CHECK_EQ(int(sliced.count), int(whole.count));
        for (int i = 0; i < whole.count; i++) {
            CHECK_EQ(sliced.at[i], whole.at[i]);
            CHECK_EQ(int(sliced.rank[i]), int(whole.rank[i]));
        }
    }
}

TEST(onsets_a_gap_or_a_wrong_total_gives_no_points_rather_than_wrong_ones) {
    hiss(1);
    hit(30000, 20000, 1);
    OnsetFinder finder;
    SlicePoints out;
    finder.feed(g_pcm, 0, 1000, 1);
    finder.feed(g_pcm + 2000, 2000, kFrames - 2000, 1);     // frames 1000-1999 never came
    finder.finish(kFrames, out);
    CHECK_EQ(int(out.count), 0);

    finder.feed(g_pcm, 0, kFrames, 1);                       // first == 0 starts over
    finder.finish(kFrames - 1, out);
    CHECK_EQ(int(out.count), 0);

    finder.feed(g_pcm, 0, kFrames, 1);
    finder.finish(kFrames, out);
    CHECK_EQ(int(out.count), 1);
}

namespace {

// Silence, and square bursts a whole number of hops long: every hop's energy is exact.
void silence() {
    for (int16_t &s : g_pcm) s = 0;
}
void burst(uint32_t hop, uint32_t hops, int16_t level, int channels, int only_channel = -1) {
    for (uint32_t f = hop * OnsetFinder::kHop; f < (hop + hops) * OnsetFinder::kHop; f++) {
        for (int c = 0; c < channels; c++) {
            if (only_channel < 0 || c == only_channel) g_pcm[f * uint32_t(channels) + uint32_t(c)] = level;
        }
    }
}
uint32_t at_hop(uint32_t hop) { return hop * OnsetFinder::kHop; }

}  // namespace

TEST(onsets_the_floor_is_exact_for_mono_and_stereo) {
    const int16_t floor = int16_t(OnsetFinder::kFloorPerFrame);
    for (int channels = 1; channels <= 2; channels++) {
        silence();
        burst(10, 1, floor, channels);
        CHECK_EQ(int(find(kFrames, channels).count), 1);
        silence();
        burst(10, 1, int16_t(floor - 1), channels);
        CHECK_EQ(int(find(kFrames, channels).count), 0);
    }
    // A hit in the right channel alone is a hit.
    silence();
    burst(10, 1, 20000, 2, 1);
    CHECK_EQ(int(find(kFrames, 2).count), 1);
}

TEST(onsets_a_gap_of_exactly_the_minimum_is_two_hits) {
    silence();
    burst(20, 1, 20000, 1);
    burst(20 + OnsetFinder::kMinGapHops, 1, 20000, 1);
    CHECK_EQ(int(find(kFrames, 1).count), 2);
    silence();
    burst(20, 1, 20000, 1);
    burst(20 + OnsetFinder::kMinGapHops - 1, 1, 20000, 1);
    CHECK_EQ(int(find(kFrames, 1).count), 1);
}

// Strength is the rise above what came before: the same burst over a steady bed ranks below
// one over silence.
TEST(onsets_strength_is_the_rise_above_the_recent_mean) {
    silence();
    burst(20, 1, 4000, 1);
    burst(100, 200, 1000, 1);                    // a bed from hop 100 (itself an onset)
    burst(200, 1, 4000, 1);
    const SlicePoints p = find(kFrames, 1);
    CHECK_EQ(int(p.count), 3);
    CHECK_EQ(p.at[0], at_hop(20));
    CHECK_EQ(p.at[1], at_hop(100));
    CHECK_EQ(p.at[2], at_hop(200));
    CHECK_EQ(int(p.rank[0]), 0);
    CHECK_EQ(int(p.rank[2]), 1);
    CHECK_EQ(int(p.rank[1]), 2);
}

// Full: a sixteenth hit takes the weakest's place -- the first of two equally weak -- only when
// it is stronger, and the points come out in order however the table was filled.
TEST(onsets_a_full_table_swaps_out_exactly_the_weakest) {
    struct Case { int weak_a, weak_b; int16_t last; int replaced; };   // -1: nothing replaced
    const Case cases[] = {
        {0, -1, 1500, 0},
        {1, -1, 1500, 1},
        {4, -1, 1000, -1},                       // the same as the weakest: the earlier stays
        {0, 3, 1500, 0},                         // a tie: the first of them goes
    };
    for (const Case &c : cases) {
        silence();
        for (int i = 0; i < 15; i++) {
            const bool weak = i == c.weak_a || i == c.weak_b;
            burst(uint32_t(20 + 20 * i), 1, weak ? 1000 : 2000, 1);
        }
        burst(20 + 20 * 15, 1, c.last, 1);
        const SlicePoints p = find(kFrames, 1);
        CHECK_EQ(int(p.count), 15);
        int k = 0;
        for (int i = 0; i < 16; i++) {
            const bool gone = c.replaced < 0 ? i == 15 : i == c.replaced;
            if (gone) continue;
            CHECK_EQ(p.at[k], at_hop(uint32_t(20 + 20 * i)));
            k++;
        }
    }
}

// Broken stays broken until a feed from frame 0: a gap, a null block or a channel change, then
// the right frames after it, still give no points.
TEST(onsets_once_broken_later_feeds_do_not_mend_it) {
    silence();
    burst(20, 1, 20000, 1);
    for (int how = 0; how < 3; how++) {
        OnsetFinder finder;
        SlicePoints out;
        finder.feed(g_pcm, 0, 1000, 1);
        if (how == 0) finder.feed(g_pcm + 5000, 5000, 1000, 1);
        if (how == 1) finder.feed(nullptr, 1000, 1000, 1);
        if (how == 2) finder.feed(g_pcm + 1000, 1000, 1000, 2);
        finder.feed(g_pcm + 1000, 1000, kFrames - 1000, 1);
        finder.finish(kFrames, out);
        CHECK_EQ(int(out.count), 0);
    }
}
