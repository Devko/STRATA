// The live views: what moves with the music on GRID and PERFORM (live visuals, part 3).
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, like the other ui_*.hpp parts: no #pragma once and no includes, and it sees every
// member the class has.
//
// Everything here draws from what the audio side publishes (app/commands.hpp Status, and the
// output in core/scope.hpp's ring, drained once a frame into scope_history_), with the drawing
// kit in app/paint.hpp. Nothing here walks a pattern or a sample.
//
// The views move only while the transport runs. Stopped, they hold still or step aside for the
// hint they replace: a screen that moves with no input would hide whether an input did
// something (tests/test_ui.cpp's input audit), and a stopped box has nothing to show anyway.

    // --- GRID: the trigs that just played glow and fade -----------------------------------
    //
    // Each track's trigger count (Status::triggered) is compared with the last frame's; a change
    // lights an outline round the cell under that track's playhead, which fades over about a
    // third of a second. So the steps leave a trail behind the playhead, brightest on the beat
    // that just played. The outline sits in the three-pixel gap round each 5 x 6 cell, so the
    // cell, its marks and the dash of an empty step are untouched.
    static constexpr int kGridGlowKeep = 160;             // in 1/256 a frame: 15 to 0 in ~6

    void update_grid_glow(bool running) {
        const Status &status = engine_.status();
        for (int t = 0; t < kTracks; t++) {
            const uint8_t count = status.triggered[t].load(std::memory_order_relaxed);
            const uint8_t head = status.playhead[t].load(std::memory_order_relaxed);
            for (int i = 0; i < 16; i++) {
                grid_glow_[t][i] = uint8_t((grid_glow_[t][i] * kGridGlowKeep) >> 8);
                if (!running) grid_glow_[t][i] = 0;
            }
            if (running && count != grid_seen_[t] && head / 16 == page_) grid_glow_[t][head % 16] = 15;
            grid_seen_[t] = count;
        }
    }

    // After the cells: grid_top is the first track's row, rows eight pixels apart, cells at
    // (i * 8 + 1, row + 1).
    void draw_grid_glow(enaga::Canvas &canvas, int grid_top) {
        uint8_t *px = canvas.data();
        for (int t = 0; t < kTracks; t++) {
            const int y = grid_top + t * 8;
            for (int i = 0; i < 16; i++) {
                const int g = grid_glow_[t][i];
                if (g < 2) continue;
                const int x = i * 8;
                for (int k = 0; k < 7; k++) {
                    paint::plot_max(px, x + k, y, g);              // above the cell
                    paint::plot_max(px, x + k, y + 7, g);          // below it
                }
                for (int k = 1; k < 7; k++) {
                    paint::plot_max(px, x, y + k, g);              // left of it
                    paint::plot_max(px, x + 6, y + k, g);          // right of it
                }
            }
        }
    }

    // --- the output's scope, with an afterglow ---------------------------------------------
    //
    // The newest output from scope_history_, three frames a column (124 columns, 15.5 ms),
    // started on a rising zero crossing so a steady tone stands still, drawn into a glow patch
    // that fades a little every frame like a phosphor screen. `grey` is the trace's brightness:
    // dim when it sits behind something, as on PERFORM.
    static constexpr int kScopeWidth = 124;
    static constexpr int kScopeRows = 20;
    static constexpr uint32_t kScopeStride = 3;
    static constexpr uint32_t kScopeSpan = uint32_t(kScopeWidth) * kScopeStride;
    static constexpr uint32_t kScopeSearch = 600;          // how far back to look for a crossing

    void draw_scope(enaga::Canvas &canvas, int x, int y, int grey, bool running) {
        if (!running) {
            scope_glow_.clear();
            return;
        }
        scope_glow_.fade(150);
        // The trigger: the newest rising crossing that leaves a whole span after it.
        uint32_t start = kScopeSpan;
        for (uint32_t ago = kScopeSpan; ago < kScopeSearch; ago++) {
            const int before = scope_history_.left(ago + 1) + scope_history_.right(ago + 1);
            const int now = scope_history_.left(ago) + scope_history_.right(ago);
            if (before < 0 && now >= 0) { start = ago; break; }
        }
        const int mid = kScopeRows / 2;
        int prev = mid;
        for (int c = 0; c < kScopeWidth; c++) {
            const uint32_t ago = start - uint32_t(c) * kScopeStride;
            const int v = scope_history_.left(ago) + scope_history_.right(ago);   // -65536..65534
            int row = mid - (v * (kScopeRows / 2 - 1)) / 65536;
            row = row < 0 ? 0 : (row >= kScopeRows ? kScopeRows - 1 : row);
            scope_glow_.span(c, c == 0 ? row : prev, row, grey);
            prev = row;
        }
        scope_glow_.blit(canvas.data(), x, y);
    }

    // --- a knob as an arc gauge -------------------------------------------------------------
    //
    // 270 degrees from bottom-left round to bottom-right, dim along the whole way and bright up
    // to `value` (0-127), a needle to the value, and the value in the small font in the gap at
    // the bottom. `knob` (0-127, or -1 for none) marks where the knob is when that is not what
    // is heard (a pad's pressure on a macro): a bright dot just outside the ring.
    static constexpr int kGaugeStart = 96;                 // 135 degrees: bottom-left
    static constexpr int kGaugeSpan = 192;                 // 270 degrees

    static void draw_gauge(enaga::Canvas &canvas, int cx, int cy, int r, int value, int knob,
                           bool live) {
        uint8_t *px = canvas.data();
        paint::arc(px, cx, cy, r, kGaugeStart, kGaugeSpan, 3);
        const int span = value * kGaugeSpan / 127;
        if (span > 0) paint::arc(px, cx, cy, r, kGaugeStart, span, live ? 15 : 6, 2);
        const int angle = kGaugeStart + span;
        const int tip_x = paint::q4(cx) + ((r - 2) * paint::cos_q12(angle) * paint::kSub >> 12);
        const int tip_y = paint::q4(cy) + ((r - 2) * paint::sin_q12(angle) * paint::kSub >> 12);
        paint::line(px, paint::q4(cx), paint::q4(cy), tip_x, tip_y, live ? 12 : 5);
        if (knob >= 0 && knob != value) {
            const int at = kGaugeStart + knob * kGaugeSpan / 127;
            paint::dot(px, cx + ((r + 2) * paint::cos_q12(at) >> 12),
                       cy + ((r + 2) * paint::sin_q12(at) >> 12), 15);
        }
        char digits[12];                                   // append_padded's widest
        char *out = digits;
        append_padded(out, uint32_t(value), 3);
        *out = 0;
        paint::small_text(px, cx - 5, cy + r - 3, digits, live ? 13 : 6);
    }

    // --- every track's level in a row --------------------------------------------------------
    //
    // Eight short bars, 13 pixels a track, from Status::track_milli in decibels (the top 36),
    // the selected track's number bright, a muted track's bar dim. Three rows of bar and the
    // track number under it in the small font.
    void draw_track_meters(enaga::Canvas &canvas, int y) {
        uint8_t *px = canvas.data();
        const Status &status = engine_.status();
        for (int t = 0; t < kTracks; t++) {
            const int x = t * 15;
            const uint32_t level = status.track_milli[t].load(std::memory_order_relaxed);
            const int lit = paint::level_rows(level, 12, 36);
            canvas.rect(x, y, 12, 3, 2);
            for (int k = 0; k < lit; k++) {
                const uint8_t g = muted_[t] ? 5 : (k >= 10 ? 15 : (k >= 6 ? 12 : 9));
                canvas.rect(x + k, y, 1, 3, g);
            }
            char digit[2] = {char('1' + t), '\0'};
            paint::small_text(px, x + 4, y + 4, digit, t == selected_ ? 15 : 6);
        }
    }
