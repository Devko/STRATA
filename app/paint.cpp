// The drawing kit (app/paint.hpp). Integer only; every write goes through plot or plot_max.
#include "paint.hpp"

namespace strata::paint {

namespace {

// sin(i * 90 / 64 degrees) in Q12, i = 0..64.
constexpr int16_t kQuarterSine[65] = {
    0,    101,  201,  301,  401,  501,  601,  700,  799,  897,  995,  1092, 1189,
    1285, 1380, 1474, 1567, 1660, 1751, 1842, 1931, 2019, 2106, 2191, 2276, 2359,
    2440, 2520, 2598, 2675, 2751, 2824, 2896, 2967, 3035, 3102, 3166, 3229, 3290,
    3349, 3406, 3461, 3513, 3564, 3612, 3659, 3703, 3745, 3784, 3822, 3857, 3889,
    3920, 3948, 3973, 3996, 4017, 4036, 4052, 4065, 4076, 4085, 4091, 4095, 4096,
};

// 10^(-d/20) in Q16, d = 0..60 dB.
constexpr int32_t kDbQ16[kDbFloor + 1] = {
    65536, 58409, 52057, 46396, 41350, 36854, 32846, 29274, 26090, 23253, 20724, 18471, 16462,
    14672, 13076, 11654, 10387, 9257,  8250,  7353,  6554,  5841,  5206,  4640,  4135,  3685,
    3285,  2927,  2609,  2325,  2072,  1847,  1646,  1467,  1308,  1165,  1039,  926,   825,
    735,   655,   584,   521,   464,   414,   369,   328,   293,   261,   233,   207,   185,
    165,   147,   131,   117,   104,   93,    83,    74,    66,
};

// The 3 x 5 glyphs: 15 bits, row-major from the top-left at bit 14. kSmallChars[i] is
// kSmallGlyphs[i]; drawn from tools/visuals_mockup.py's table, where they were tried at the
// OLED's size.
constexpr char kSmallChars[] = " !#'()*+,-./0123456789:=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_%<";
constexpr uint16_t kSmallGlyphs[] = {
    0x0000, 0x2482, 0x5F7D, 0x2400, 0x2922, 0x224A, 0x0AA8, 0x05D0, 0x0014, 0x01C0,
    0x0002, 0x12A4, 0x7B6F, 0x2C97, 0x62A7, 0x628E, 0x5BC9, 0x798E, 0x39EF, 0x7292,
    0x7BEF, 0x7BCE, 0x0410, 0x0E38, 0x4454, 0x7282, 0x2BED, 0x6BAE, 0x3923, 0x6B6E,
    0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED, 0x6B6D,
    0x2B6A, 0x6BA4, 0x2B73, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD,
    0x5A92, 0x72A7, 0x6926, 0x324B, 0x0007, 0x52A5, 0x1511,
};
static_assert(sizeof kSmallChars - 1 == sizeof kSmallGlyphs / sizeof kSmallGlyphs[0],
              "one glyph a character");

uint16_t small_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (int i = 0; kSmallChars[i] != '\0'; i++) {
        if (kSmallChars[i] == c) return kSmallGlyphs[i];
    }
    return 0x7282;   // '?'
}

int clamp_grey(int grey) { return grey < 0 ? 0 : (grey > 15 ? 15 : grey); }

}  // namespace

void plot(uint8_t *px, int x, int y, int grey) {
    if (unsigned(x) < unsigned(kWidth) && unsigned(y) < unsigned(kHeight)) {
        px[y * kWidth + x] = uint8_t(clamp_grey(grey));
    }
}

void plot_max(uint8_t *px, int x, int y, int grey) {
    if (unsigned(x) < unsigned(kWidth) && unsigned(y) < unsigned(kHeight)) {
        const uint8_t g = uint8_t(clamp_grey(grey));
        if (g > px[y * kWidth + x]) px[y * kWidth + x] = g;
    }
}

// Wu's line in fixed point. Internally Q8 (the Q4 input shifted up), the slope in Q16, the
// position along the minor axis at each major-axis pixel centre computed from the start rather
// than accumulated, so a long line does not drift. One divide.
void line(uint8_t *px, int x0, int y0, int x1, int y1, int grey) {
    grey = clamp_grey(grey);
    int ax0 = x0 * 16, ay0 = y0 * 16, ax1 = x1 * 16, ay1 = y1 * 16;   // Q8
    const int dxa = ax1 > ax0 ? ax1 - ax0 : ax0 - ax1;
    const int dya = ay1 > ay0 ? ay1 - ay0 : ay0 - ay1;
    const bool steep = dya > dxa;
    if (steep) {
        int t = ax0; ax0 = ay0; ay0 = t;
        t = ax1; ax1 = ay1; ay1 = t;
    }
    if (ax0 > ax1) {
        int t = ax0; ax0 = ax1; ax1 = t;
        t = ay0; ay0 = ay1; ay1 = t;
    }
    const int dx = ax1 - ax0;
    const int dy = ay1 - ay0;
    const int64_t slope = dx > 0 ? (int64_t(dy) << 16) / dx : 0;   // Q16
    const int first = (ax0 + 128) >> 8;
    const int last = (ax1 + 128) >> 8;
    for (int i = first; i <= last; i++) {
        const int64_t along = int64_t(i) * 256 - ax0;                // Q8
        const int64_t minor = int64_t(ay0) + ((along * slope) >> 16);  // Q8
        const int whole = int(minor >> 8);
        const int frac = int(minor & 255);
        const int lit = (grey * (256 - frac) + 128) >> 8;
        const int rest = (grey * frac + 128) >> 8;
        if (steep) {
            plot_max(px, whole, i, lit);
            plot_max(px, whole + 1, i, rest);
        } else {
            plot_max(px, i, whole, lit);
            plot_max(px, i, whole + 1, rest);
        }
    }
}

void polyline(uint8_t *px, const int16_t *x, const int16_t *y, int n, int grey) {
    for (int i = 1; i < n; i++) line(px, x[i - 1], y[i - 1], x[i], y[i], grey);
}

void dot(uint8_t *px, int x, int y, int grey) {
    grey = clamp_grey(grey);
    const int side = grey * 2 / 3;
    const int corner = grey / 3;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int g = (dx == 0 && dy == 0) ? grey : ((dx == 0 || dy == 0) ? side : corner);
            plot_max(px, x + dx, y + dy, g);
        }
    }
}

void dotted_h(uint8_t *px, int x0, int x1, int y, int grey, int step) {
    if (step < 1) step = 1;
    for (int x = x0; x <= x1; x += step) plot(px, x, y, grey);
}

void dotted_v(uint8_t *px, int x, int y0, int y1, int grey, int step) {
    if (step < 1) step = 1;
    for (int y = y0; y <= y1; y += step) plot(px, x, y, grey);
}

int sin_q12(int angle) {
    angle &= kTurn - 1;
    const int quarter = kTurn / 4;
    if (angle < quarter) return kQuarterSine[angle];
    if (angle < 2 * quarter) return kQuarterSine[2 * quarter - angle];
    if (angle < 3 * quarter) return -kQuarterSine[angle - 2 * quarter];
    return -kQuarterSine[kTurn - angle];
}

int cos_q12(int angle) { return sin_q12(angle + kTurn / 4); }

void arc(uint8_t *px, int cx, int cy, int r, int start, int span, int grey, int thick) {
    if (r < 1 || span <= 0) return;
    if (thick < 1) thick = 1;
    // Segments about two pixels long: a step of 1/256 turn is 2 pi r / 256 pixels.
    int step = 80 / r;
    if (step < 1) step = 1;
    for (int k = 0; k < thick && r - k >= 1; k++) {
        const int rr = r - k;
        int prev_x = q4(cx) + (rr * cos_q12(start) * kSub >> 12);
        int prev_y = q4(cy) + (rr * sin_q12(start) * kSub >> 12);
        for (int a = step; ; a += step) {
            if (a > span) a = span;
            const int x = q4(cx) + (rr * cos_q12(start + a) * kSub >> 12);
            const int y = q4(cy) + (rr * sin_q12(start + a) * kSub >> 12);
            line(px, prev_x, prev_y, x, y, grey);
            prev_x = x;
            prev_y = y;
            if (a == span) break;
        }
    }
}

SmallTextHook small_text_hook = nullptr;

int small_text(uint8_t *px, int x, int y, const char *text, int grey) {
    if (small_text_hook != nullptr) small_text_hook(x, y, text, grey);
    return small_glyphs(px, x, y, text, grey);
}

int small_badge(uint8_t *px, int x, int y, const char *text) {
    const int w = small_width(text);
    for (int j = -1; j <= kSmallHeight; j++) {
        for (int i = -1; i <= w; i++) plot(px, x + i, y + j, 15);
    }
    small_text(px, x, y, text, 0);
    return x + w + 2;
}

int small_glyphs(uint8_t *px, int x, int y, const char *text, int grey) {
    for (; *text != '\0'; text++) {
        const uint16_t bits = small_glyph(*text);
        for (int i = 0; i < 15; i++) {
            if ((bits >> (14 - i)) & 1u) plot(px, x + i % 3, y + i / 3, grey);
        }
        x += kSmallAdvance;
    }
    return x;
}

int small_width(const char *text) {
    int n = 0;
    while (text[n] != '\0') n++;
    return n > 0 ? n * kSmallAdvance - 1 : 0;
}

int level_db_q8(uint32_t milli) {
    if (milli >= 1000u) return 0;
    const int32_t amp = int32_t(milli * 65536u / 1000u);    // Q16 of full scale
    if (amp <= kDbQ16[kDbFloor]) return -kDbFloor * 256;
    int d = 1;
    while (kDbQ16[d] > amp) d++;                              // kDbQ16[d] <= amp < kDbQ16[d - 1]
    // Linear between the two thresholds: close enough inside one decibel.
    const int32_t above = kDbQ16[d - 1] - amp;
    const int32_t width = kDbQ16[d - 1] - kDbQ16[d];
    return -((d - 1) * 256 + int(above * 256 / width));
}

int level_rows(uint32_t milli, int rows, int range_db) {
    if (rows <= 0 || range_db <= 0) return 0;
    const int db = level_db_q8(milli) + range_db * 256;      // 0 at the bottom of the range
    if (db <= 0) return 0;
    const int r = db * rows / (range_db * 256);
    return r > rows ? rows : r;
}

}  // namespace strata::paint
