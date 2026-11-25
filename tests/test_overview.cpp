// core/overview: the screen's picture of a sample, made once at load.
#include "overview.hpp"

#include "check.hpp"

using namespace strata;

namespace {

constexpr uint32_t kMaxFrames = 48000 * 2;
int16_t g_pcm[kMaxFrames * 2];                 // file scope: no 384 KB stack frame

// A rising ramp across the whole sample, so each column's range is known exactly.
int16_t ramp(uint32_t f, uint32_t total) {
    return int16_t(int32_t(int64_t(f) * 65535 / int64_t(total)) - 32768);
}

void fill_ramp(uint32_t total, int channels) {
    for (uint32_t f = 0; f < total; f++) {
        for (int c = 0; c < channels; c++) {
            g_pcm[f * uint32_t(channels) + uint32_t(c)] = ramp(f, total);
        }
    }
}

WaveOverview build(uint32_t total, int channels, uint32_t piece) {
    WaveOverview out;
    OverviewBuilder builder;
    builder.begin(out, total);
    for (uint32_t first = 0; first < total; first += piece) {
        const uint32_t n = total - first < piece ? total - first : piece;
        builder.feed(g_pcm + first * uint32_t(channels), first, n, channels);
    }
    builder.finish(total);
    return out;
}

// The columns frame f falls in, by the header's definition.
uint32_t column_of(uint32_t f, uint32_t total) {
    return uint32_t(uint64_t(f) * uint64_t(kWaveColumns) / uint64_t(total));
}

}  // namespace

TEST(overview_holds_each_columns_lowest_and_highest_value) {
    const uint32_t total = 48000;
    fill_ramp(total, 1);
    const WaveOverview o = build(total, 1, total);
    CHECK(o.ready);
    CHECK_EQ(int(o.filled), kWaveColumns);
    // Each column's range is exactly the ramp over its own frames: the first and last frame in it.
    uint32_t first = 0;
    for (int c = 0; c < kWaveColumns; c++) {
        uint32_t last = first;
        while (last + 1 < total && column_of(last + 1, total) == uint32_t(c)) last++;
        CHECK_EQ(int(o.lo[c]), int(ramp(first, total)) >> 8);
        CHECK_EQ(int(o.hi[c]), int(ramp(last, total)) >> 8);
        first = last + 1;
    }
    CHECK_EQ(first, total);
}

TEST(overview_is_the_same_however_the_loader_slices_the_feed) {
    const uint32_t total = 77777;                  // not a multiple of anything here
    fill_ramp(total, 2);
    const WaveOverview whole = build(total, 2, total);
    const uint32_t pieces[] = {1, 3, 383, 48000};
    for (uint32_t piece : pieces) {
        const WaveOverview cut = build(total, 2, piece);
        CHECK(cut.ready);
        for (int c = 0; c < kWaveColumns; c++) {
            CHECK_EQ(int(cut.lo[c]), int(whole.lo[c]));
            CHECK_EQ(int(cut.hi[c]), int(whole.hi[c]));
        }
    }
}

TEST(overview_draws_a_stereo_pair_as_its_mid) {
    // L full up and R full down cancel: the machines play the mid, so the picture is flat.
    const uint32_t total = 4000;
    for (uint32_t f = 0; f < total; f++) {
        g_pcm[2 * f] = 32767;
        g_pcm[2 * f + 1] = -32767;
    }
    const WaveOverview o = build(total, 2, 1000);
    CHECK(o.ready);
    for (int c = 0; c < kWaveColumns; c++) {
        CHECK_EQ(int(o.lo[c]), 0);
        CHECK_EQ(int(o.hi[c]), 0);
    }
    // And one side alone is drawn at half height.
    for (uint32_t f = 0; f < total; f++) g_pcm[2 * f + 1] = 0;
    const WaveOverview half = build(total, 2, 1000);
    CHECK_EQ(int(half.hi[10]), 16383 >> 8);
}

TEST(overview_of_a_sample_shorter_than_the_screen_fills_every_column) {
    const uint32_t total = 10;
    for (uint32_t f = 0; f < total; f++) g_pcm[f] = int16_t(f * 1000);
    const WaveOverview o = build(total, 1, 3);
    CHECK(o.ready);
    CHECK_EQ(int(o.filled), kWaveColumns);
    // Each column shows the last frame at or before it: the largest f with f * K / total <= c.
    for (int c = 0; c < kWaveColumns; c++) {
        const int under = int((uint32_t(c + 1) * total - 1) / uint32_t(kWaveColumns));
        CHECK_EQ(int(o.hi[c]), (under * 1000) >> 8);
        CHECK_EQ(int(o.lo[c]), (under * 1000) >> 8);
    }
    CHECK_EQ(int(o.hi[kWaveColumns - 1]), (9 * 1000) >> 8);   // the last frame, to the edge
}

TEST(overview_refuses_frames_out_of_order) {
    const uint32_t total = 9000;
    fill_ramp(total, 1);
    WaveOverview out;
    OverviewBuilder builder;
    builder.begin(out, total);
    builder.feed(g_pcm, 0, 4000, 1);
    builder.feed(g_pcm + 4001, 4001, 4999, 1);     // a frame missing
    builder.finish(total);
    CHECK(!out.ready);

    // More frames than the sample has is no overview either, rather than a column past the end.
    builder.begin(out, 100);
    builder.feed(g_pcm, 0, 101, 1);
    builder.finish(101);
    CHECK(!out.ready);

    // And an empty sample is nothing to draw.
    builder.begin(out, 0);
    builder.finish(0);
    CHECK(!out.ready);
}

TEST(overview_of_a_file_shorter_than_its_header_lines_up_with_what_plays) {
    // The header promised 60,000 frames and 45,000 arrived: the sampler gets 45,000, so the
    // overview has to be the one a 45,000-frame sample makes, column for column.
    const uint32_t real = 45000;
    fill_ramp(real, 1);
    WaveOverview out;
    OverviewBuilder builder;
    builder.begin(out, 60000);
    builder.feed(g_pcm, 0, real, 1);
    builder.finish(real);
    CHECK(out.ready);
    CHECK_EQ(int(out.filled), kWaveColumns);
    const WaveOverview exact = build(real, 1, real);
    int off = 0;
    for (int c = 0; c < kWaveColumns; c++) {
        // Stretched from coarser columns, so within one column's worth of the ramp (2 steps of
        // the int8 scale), never somewhere else in the sample.
        const int d = int(out.hi[c]) - int(exact.hi[c]);
        off += d > 3 || d < -3 ? 1 : 0;
    }
    CHECK_EQ(off, 0);
    CHECK_EQ(int(out.lo[0]), int(exact.lo[0]));
    CHECK_EQ(int(out.hi[kWaveColumns - 1]), int(exact.hi[kWaveColumns - 1]));
}

TEST(overview_of_a_streamed_head_keeps_the_whole_files_scale) {
    // A 10 s file whose first 2 s are the head: a fifth of the columns hold data.
    const uint32_t total = 480000;
    const uint32_t head = 96000;
    fill_ramp(head, 1);
    WaveOverview out;
    OverviewBuilder builder;
    builder.begin(out, total);
    builder.feed(g_pcm, 0, head, 1);
    builder.finish_head();
    CHECK(out.ready);
    // Column 24 ends at ceil(25 * 480000 / 124) = 96775 > 96000, so 24 is the partial one.
    CHECK_EQ(int(out.filled), 25);
    CHECK_EQ(int(out.hi[30]), 0);                  // never read: nothing drawn there
}

TEST(overview_columns_are_the_columns_the_audio_side_puts_a_playhead_in) {
    // wave_column (float, audio side) and the builder (integer, main side) agree on every frame
    // but the few where float rounding lands on a boundary, and there by one column at most.
    const uint32_t totals[] = {4, 123, 124, 125, 48000, 1440000, 16777215};
    for (uint32_t total : totals) {
        const float scale = float(kWaveColumns) / float(total);
        int disagree = 0;
        int far = 0;
        const uint32_t step = total > 100000 ? 997 : 1;
        for (uint32_t f = 0; f < total; f += step) {
            const int a = int(column_of(f, total));
            const int b = int(wave_column(float(f), scale));
            disagree += a != b ? 1 : 0;
            far += (a - b > 1 || b - a > 1) ? 1 : 0;
        }
        CHECK_EQ(far, 0);
        CHECK(disagree * 100 <= int(total / step) + 1);   // under one in a hundred
    }
    CHECK_EQ(int(wave_column(-5.0f, 1.0f)), 0);
    CHECK_EQ(int(wave_column(1e9f, 1.0f)), kWaveColumns - 1);
}

TEST(overview_view_marks_slice_columns_as_bits) {
    WaveView view;
    wave_set_cut(view, 0);
    wave_set_cut(view, 31);
    wave_set_cut(view, 32);
    wave_set_cut(view, kWaveColumns - 1);
    wave_set_cut(view, WaveView::kNone);           // ignored
    CHECK(wave_has_cut(view.cuts, 0));
    CHECK(wave_has_cut(view.cuts, 31));
    CHECK(wave_has_cut(view.cuts, 32));
    CHECK(wave_has_cut(view.cuts, kWaveColumns - 1));
    CHECK(!wave_has_cut(view.cuts, 1));
    CHECK(!wave_has_cut(view.cuts, 33));
    CHECK(!wave_has_cut(view.cuts, -1));
    CHECK(!wave_has_cut(view.cuts, kWaveColumns));
    for (int i = 0; i < WaveView::kMarks; i++) CHECK_EQ(int(view.mark[i]), int(WaveView::kNone));
}
