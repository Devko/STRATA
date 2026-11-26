// STRATA's drawing kit: what the live screens draw with, beyond Enaga's pixel, rect and text.
//
// Enaga's Canvas has clear, pixel, rect and a 5 x 7 text (docs/ENAGA_API.md). A screen that
// moves with the music needs more: lines that sit between pixels (16 greys make anti-aliasing
// cheap and it is what makes a curve read as a curve at 128 x 128), arcs for knob gauges, a
// smaller font for the labels that sit on a graph, soft dots that ride a curve, and an afterglow
// for scopes and particles. All of it is here, as plain functions on the canvas bytes
// (`Canvas::data()`, 128 x 128, row-major, one grey 0-15 a byte), so it builds and is tested on
// the host with no Enaga header (tests/test_paint.cpp).
//
// Cost. Everything here runs in draw(), in the main loop, never in audio(): Enaga calls draw()
// at most every 50 ms, and a frame costs the audio side nothing (docs/BUDGET.md, "The
// screen"). Per call the work is a few integer operations a pixel touched, one integer divide a
// line, and no float. No libm: the arcs use a 65-entry quarter sine table.
//
// Every write is clipped to the screen: a draw off the canvas is a hard fault on the unit.
#pragma once

#include <cstdint>

namespace strata::paint {

constexpr int kWidth = 128;
constexpr int kHeight = 128;

// Labels on the live screens: the 3 x 5 font here (true) or Enaga's 5 x 7 (false). Roland
// decides on the OLED; until then the small one is used where it fits, and this one switch puts
// every label back on the big font (app/app.hpp, `label()`).
inline constexpr bool kSmallLabels = true;

// One pixel, clipped. `plot` overwrites; `plot_max` keeps the brighter of the two, which is how
// lines, dots and the afterglow combine without one erasing another.
void plot(uint8_t *px, int x, int y, int grey);
void plot_max(uint8_t *px, int x, int y, int grey);

// Positions in sixteenths of a pixel (Q4), so a curve's points can fall between pixels.
constexpr int kSub = 16;
constexpr int q4(int pixels) { return pixels * kSub; }

// An anti-aliased line (Xiaolin Wu): along the line's long axis one column (or row) at a time,
// the grey shared between the two pixels it falls between. Brighter wins over what is there.
void line(uint8_t *px, int x0, int y0, int x1, int y1, int grey);

// Points joined by lines, in Q4. `n` below 2 draws nothing.
void polyline(uint8_t *px, const int16_t *x, const int16_t *y, int n, int grey);

// A soft 3 x 3 dot: the centre at `grey`, the four sides at two thirds, the corners at a third.
void dot(uint8_t *px, int x, int y, int grey);

// Dotted guide lines, a pixel every `step`, overwriting (grids are drawn first).
void dotted_h(uint8_t *px, int x0, int x1, int y, int grey, int step = 2);
void dotted_v(uint8_t *px, int x, int y0, int y1, int grey, int step = 2);

// Angles in 1/256 of a turn, 0 at three o'clock and growing clockwise (the screen's y grows
// down). Sine and cosine in Q12: 4096 is 1.
constexpr int kTurn = 256;
int sin_q12(int angle);
int cos_q12(int angle);

// An arc of radius `r` around (cx, cy), from `start` through `span` (both in 1/256 turn,
// clockwise), anti-aliased, `thick` rings deep inwards (1 or more).
void arc(uint8_t *px, int cx, int cy, int r, int start, int span, int grey, int thick = 1);

// The 3 x 5 font: upper case, digits and " !#'()*+,-./:=<>?[]_%". Four pixels a character; lower
// case draws as upper case and anything else as '?'. Returns the x after the text.
constexpr int kSmallAdvance = 4;
constexpr int kSmallHeight = 5;
int small_text(uint8_t *px, int x, int y, const char *text, int grey);
int small_width(const char *text);

// The same glyphs, not reported to the hook: recolouring part of a string small_text drew (a
// knob's letter dimmer than its value), so the string is found and checked once, whole.
int small_glyphs(uint8_t *px, int x, int y, const char *text, int grey);

// Tests only: when set, every small_text string is reported with where it was drawn, so the host
// tests find and lay out the small labels as they do Enaga's text (tests/test_ui.cpp,
// layout_faults). Null on the unit, where it costs one compare a string.
using SmallTextHook = void (*)(int x, int y, const char *text, int grey);
extern SmallTextHook small_text_hook;

// A lit badge in the small font: a grey-15 box one pixel round the text, the text black. The
// drafts' knob letters on a curve ("A" over the attack's peak). Returns the x after the box.
int small_badge(uint8_t *px, int x, int y, const char *text);

// A level to decibels for meters: `milli` is thousandths of full scale (Status::meter_milli),
// the answer is dB in Q8 (256 = 1 dB), 0 at full scale and above, -60 dB (-15360) at and below
// the floor. A table of 61 thresholds and one divide; no log.
constexpr int kDbFloor = 60;
int level_db_q8(uint32_t milli);

// How many of `rows` a level fills on a meter that shows the top `range_db` decibels.
int level_rows(uint32_t milli, int rows, int range_db);

// An afterglow: a W x H patch of its own greys that fades a little every frame and is laid over
// the canvas, brighter winning. What a scope, a vectorscope or the grain cloud draw into, so a
// trace leaves a tail like a phosphor screen. The patch is a member of the screen that owns it,
// allocated with the app (CLAUDE.md: nothing allocated after init).
template <int W, int H>
struct Glow {
    static_assert(W > 0 && H > 0 && W <= kWidth && H <= kHeight, "a glow fits on the screen");
    uint8_t level[W * H] = {};

    void clear() {
        for (int i = 0; i < W * H; i++) level[i] = 0;
    }
    // keep: how much of each grey survives a frame, in 1/256 (192 = three quarters).
    void fade(int keep) {
        for (int i = 0; i < W * H; i++) level[i] = uint8_t((level[i] * keep) >> 8);
    }
    void plot(int x, int y, int grey) {
        if (unsigned(x) < unsigned(W) && unsigned(y) < unsigned(H) && grey > level[y * W + x]) {
            level[y * W + x] = uint8_t(grey > 15 ? 15 : grey);
        }
    }
    // A vertical run from y0 to y1 in the patch, both ends in: a scope's trace between two
    // samples, so a steep edge stays joined.
    void span(int x, int y0, int y1, int grey) {
        if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
        for (int y = y0; y <= y1; y++) plot(x, y, grey);
    }
    // Onto the canvas with its top-left at (x, y), clipped, brighter winning.
    void blit(uint8_t *px, int x, int y) const {
        for (int j = 0; j < H; j++) {
            for (int i = 0; i < W; i++) {
                const uint8_t g = level[j * W + i];
                if (g != 0) plot_max(px, x + i, y + j, g);
            }
        }
    }
};

}  // namespace strata::paint
