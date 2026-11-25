// app/paint: the drawing kit the live screens draw with.
#include "paint.hpp"

#include "check.hpp"

using namespace strata;
using namespace strata::paint;

namespace {

// The canvas with guard bytes on both sides: a write off the screen lands in a guard.
constexpr int kGuard = 512;
uint8_t g_mem[kGuard + kWidth * kHeight + kGuard];
uint8_t *const g_px = g_mem + kGuard;

void wipe() {
    for (uint8_t &b : g_mem) b = 0;
}

bool guards_clean() {
    for (int i = 0; i < kGuard; i++) {
        if (g_mem[i] != 0 || g_mem[kGuard + kWidth * kHeight + i] != 0) return false;
    }
    return true;
}

int at(int x, int y) { return g_px[y * kWidth + x]; }

int lit_count() {
    int n = 0;
    for (int i = 0; i < kWidth * kHeight; i++) n += g_px[i] != 0;
    return n;
}

}  // namespace

TEST(paint_plot_clips_to_the_screen_and_plot_max_keeps_the_brighter) {
    wipe();
    plot(g_px, -1, 0, 15);
    plot(g_px, 128, 5, 15);
    plot(g_px, 3, -1, 15);
    plot(g_px, 3, 128, 15);
    CHECK(guards_clean());
    CHECK_EQ(lit_count(), 0);
    plot(g_px, 4, 4, 20);                      // greys are 0-15
    CHECK_EQ(at(4, 4), 15);
    plot_max(g_px, 4, 4, 3);
    CHECK_EQ(at(4, 4), 15);
    plot(g_px, 4, 4, 3);
    CHECK_EQ(at(4, 4), 3);
    plot_max(g_px, 4, 4, 9);
    CHECK_EQ(at(4, 4), 9);
}

TEST(paint_a_line_on_a_whole_row_is_one_pixel_deep_and_end_to_end) {
    wipe();
    line(g_px, q4(10), q4(20), q4(40), q4(20), 12);
    for (int x = 10; x <= 40; x++) {
        CHECK_EQ(at(x, 20), 12);
        CHECK_EQ(at(x, 19), 0);
        CHECK_EQ(at(x, 21), 0);
    }
    CHECK_EQ(at(9, 20), 0);
    CHECK_EQ(at(41, 20), 0);
    CHECK_EQ(lit_count(), 31);
}

TEST(paint_a_line_between_two_rows_shares_its_grey_between_them) {
    wipe();
    line(g_px, q4(10), q4(20) + kSub / 2, q4(30), q4(20) + kSub / 2, 14);   // y 20.5
    for (int x = 10; x <= 30; x++) {
        CHECK_EQ(at(x, 20), 7);
        CHECK_EQ(at(x, 21), 7);
    }
    // A quarter of the way down: three quarters above.
    wipe();
    line(g_px, q4(10), q4(50) + kSub / 4, q4(30), q4(50) + kSub / 4, 12);
    CHECK_EQ(at(20, 50), 9);
    CHECK_EQ(at(20, 51), 3);
}

TEST(paint_a_steep_line_lights_every_row_once_and_either_direction_is_the_same) {
    wipe();
    line(g_px, q4(60), q4(10), q4(64), q4(90), 15);
    for (int y = 10; y <= 90; y++) {
        int sum = 0;
        for (int x = 55; x < 70; x++) sum += at(x, y);
        CHECK(sum >= 14 && sum <= 16);         // the grey, shared between two columns
    }
    uint8_t first[kWidth * kHeight];
    for (int i = 0; i < kWidth * kHeight; i++) first[i] = g_px[i];
    wipe();
    line(g_px, q4(64), q4(90), q4(60), q4(10), 15);
    int differ = 0;
    for (int i = 0; i < kWidth * kHeight; i++) differ += first[i] != g_px[i];
    CHECK_EQ(differ, 0);
}

TEST(paint_lines_off_the_screen_write_nothing_outside_it) {
    wipe();
    line(g_px, q4(-200), q4(-50), q4(400), q4(300), 15);
    line(g_px, q4(-10), q4(127), q4(140), q4(127), 15);
    line(g_px, q4(127), q4(-30), q4(127), q4(200), 15);
    line(g_px, q4(5), q4(5), q4(5), q4(5), 15);   // a point
    CHECK(guards_clean());
    CHECK_EQ(at(5, 5), 15);
    CHECK_EQ(at(0, 127), 15);
    CHECK_EQ(at(127, 0), 15);
}

TEST(paint_polyline_joins_its_points) {
    wipe();
    const int16_t xs[3] = {int16_t(q4(0)), int16_t(q4(10)), int16_t(q4(10))};
    const int16_t ys[3] = {int16_t(q4(0)), int16_t(q4(0)), int16_t(q4(10))};
    polyline(g_px, xs, ys, 3, 10);
    CHECK_EQ(at(5, 0), 10);
    CHECK_EQ(at(10, 5), 10);
    CHECK_EQ(at(10, 10), 10);
    CHECK_EQ(lit_count(), 21);
    polyline(g_px, xs, ys, 1, 15);             // one point: nothing
    CHECK_EQ(at(0, 0), 10);
}

TEST(paint_a_dot_is_bright_in_the_middle_and_softer_round_it) {
    wipe();
    dot(g_px, 20, 20, 15);
    CHECK_EQ(at(20, 20), 15);
    CHECK_EQ(at(19, 20), 10);
    CHECK_EQ(at(20, 21), 10);
    CHECK_EQ(at(21, 21), 5);
    CHECK_EQ(at(22, 20), 0);
    dot(g_px, 0, 0, 15);                       // in the corner, clipped
    CHECK(guards_clean());
}

TEST(paint_dotted_guides_skip_every_other_pixel) {
    wipe();
    dotted_h(g_px, 0, 9, 3, 4);
    dotted_v(g_px, 50, 0, 9, 4, 3);
    CHECK_EQ(at(0, 3), 4);
    CHECK_EQ(at(1, 3), 0);
    CHECK_EQ(at(8, 3), 4);
    CHECK_EQ(at(50, 0), 4);
    CHECK_EQ(at(50, 1), 0);
    CHECK_EQ(at(50, 9), 4);
    CHECK_EQ(lit_count(), 5 + 4);
}

TEST(paint_sine_and_cosine_go_round_a_turn_of_256) {
    CHECK_EQ(sin_q12(0), 0);
    CHECK_EQ(sin_q12(64), 4096);
    CHECK_EQ(sin_q12(128), 0);
    CHECK_EQ(sin_q12(192), -4096);
    CHECK_EQ(sin_q12(256), 0);
    CHECK_EQ(sin_q12(-64), -4096);
    CHECK_EQ(cos_q12(0), 4096);
    CHECK_EQ(cos_q12(128), -4096);
    CHECK_EQ(sin_q12(32), 2896);               // 45 degrees: 4096 / sqrt 2
    CHECK_EQ(sin_q12(96), 2896);
    CHECK_EQ(sin_q12(160), -2896);
    CHECK_EQ(sin_q12(224), -2896);
}

TEST(paint_an_arc_runs_clockwise_from_its_start_and_stops_at_its_span) {
    wipe();
    arc(g_px, 60, 60, 10, 0, 64, 15);          // three o'clock round to six
    CHECK(at(70, 60) >= 12);
    CHECK(at(60, 70) >= 12);
    CHECK(at(67, 67) + at(68, 67) + at(67, 68) >= 12);   // 45 degrees, about (67.1, 67.1)
    CHECK_EQ(at(50, 60), 0);                   // nine o'clock: not on it
    CHECK_EQ(at(60, 50), 0);                   // twelve: not on it
    wipe();
    arc(g_px, 60, 60, 10, 0, 64, 15, 3);       // three rings deep
    CHECK(at(68, 60) >= 12);
    CHECK_EQ(at(66, 60), 0);
    wipe();
    arc(g_px, 2, 2, 20, 0, kTurn, 15, 2);      // off the corner, clipped
    CHECK(guards_clean());
}

TEST(paint_the_small_font_draws_three_by_five_with_a_four_pixel_advance) {
    wipe();
    const int end = small_text(g_px, 10, 10, "A1", 13);
    CHECK_EQ(end, 18);
    // A:  .X.  X.X  XXX  X.X  X.X
    CHECK_EQ(at(10, 10), 0);
    CHECK_EQ(at(11, 10), 13);
    CHECK_EQ(at(10, 12), 13);
    CHECK_EQ(at(11, 12), 13);
    CHECK_EQ(at(12, 12), 13);
    CHECK_EQ(at(11, 13), 0);
    CHECK_EQ(at(12, 14), 13);
    CHECK_EQ(at(13, 12), 0);                   // the gap column
    // 1:  .X.  XX.  .X.  .X.  XXX
    CHECK_EQ(at(15, 10), 13);
    CHECK_EQ(at(14, 11), 13);
    CHECK_EQ(at(16, 14), 13);
    CHECK_EQ(small_width("A1"), 7);
    CHECK_EQ(small_width(""), 0);

    // Lower case is upper case; a character it lacks is '?'.
    uint8_t upper[kWidth * 8];
    wipe();
    small_text(g_px, 0, 0, "CUT", 15);
    for (int i = 0; i < kWidth * 8; i++) upper[i] = g_px[i];
    wipe();
    small_text(g_px, 0, 0, "cut", 15);
    int differ = 0;
    for (int i = 0; i < kWidth * 8; i++) differ += upper[i] != g_px[i];
    CHECK_EQ(differ, 0);
    wipe();
    small_text(g_px, 0, 0, "?", 15);
    for (int i = 0; i < kWidth * 8; i++) upper[i] = g_px[i];
    wipe();
    small_text(g_px, 0, 0, "@", 15);
    differ = 0;
    for (int i = 0; i < kWidth * 8; i++) differ += upper[i] != g_px[i];
    CHECK_EQ(differ, 0);
    small_text(g_px, 126, 126, "WW", 15);      // past the corner: clipped
    CHECK(guards_clean());
}

TEST(paint_every_small_glyph_stays_in_its_three_by_five_cell) {
    const char all[] = " !#'()*+,-./0123456789:=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_%<";
    for (int i = 0; all[i] != '\0'; i++) {
        wipe();
        const char one[2] = {all[i], '\0'};
        small_text(g_px, 20, 20, one, 15);
        for (int y = 15; y < 30; y++) {
            for (int x = 15; x < 30; x++) {
                const bool inside = x >= 20 && x < 23 && y >= 20 && y < 25;
                if (!inside) CHECK_EQ(at(x, y), 0);
            }
        }
        if (all[i] != ' ') CHECK(lit_count() > 0);
    }
}

TEST(paint_levels_in_decibels_from_thousandths_of_full_scale) {
    CHECK_EQ(level_db_q8(1000), 0);
    CHECK_EQ(level_db_q8(5000), 0);            // above full scale reads full scale
    CHECK_EQ(level_db_q8(0), -kDbFloor * 256);
    CHECK_EQ(level_db_q8(1), -kDbFloor * 256); // -60 dB is the floor
    const int half = level_db_q8(500);         // -6.02 dB
    CHECK(half <= -6 * 256 + 10 && half >= -7 * 256 + 230);
    const int tenth = level_db_q8(100);        // -20 dB
    CHECK(tenth <= -20 * 256 + 20 && tenth >= -20 * 256 - 20);
    // Louder is never fewer decibels.
    int prev = level_db_q8(0);
    for (uint32_t m = 1; m <= 1000; m++) {
        const int db = level_db_q8(m);
        CHECK(db >= prev);
        prev = db;
    }
}

TEST(paint_level_rows_fill_a_meter_by_decibels) {
    CHECK_EQ(level_rows(1000, 100, 48), 100);
    CHECK_EQ(level_rows(0, 100, 48), 0);
    CHECK_EQ(level_rows(4, 100, 48), 0);       // -48 dB: the bottom
    const int half = level_rows(63, 100, 48);  // -24 dB: half way up
    CHECK(half >= 49 && half <= 51);
    CHECK_EQ(level_rows(1000, 0, 48), 0);
    CHECK_EQ(level_rows(1000, 50, 0), 0);
}

TEST(paint_a_glow_fades_and_lays_over_the_canvas_brighter_winning) {
    static Glow<8, 4> glow;
    glow.clear();
    glow.plot(1, 1, 15);
    glow.plot(-1, 0, 15);                      // outside the patch: ignored
    glow.plot(8, 0, 15);
    glow.span(5, 3, 0, 8);                     // either way round
    CHECK_EQ(int(glow.level[1 * 8 + 1]), 15);
    CHECK_EQ(int(glow.level[0 * 8 + 5]), 8);
    CHECK_EQ(int(glow.level[3 * 8 + 5]), 8);
    glow.fade(128);
    CHECK_EQ(int(glow.level[1 * 8 + 1]), 7);
    CHECK_EQ(int(glow.level[2 * 8 + 5]), 4);

    wipe();
    plot(g_px, 21, 31, 12);                    // brighter than the glow there: stays
    glow.blit(g_px, 20, 30);
    CHECK_EQ(at(21, 31), 12);
    CHECK_EQ(at(25, 30), 4);
    CHECK_EQ(at(20, 30), 0);
    glow.blit(g_px, 124, 126);                 // half off the screen: clipped
    CHECK(guards_clean());
    for (int i = 0; i < 8; i++) glow.fade(128);
    int left = 0;
    for (const uint8_t g : glow.level) left += g;
    CHECK_EQ(left, 0);                         // it does go dark
}
