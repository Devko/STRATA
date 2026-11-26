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
    // (kGridX + i * kGridPitch, row + 1). A glow above 10 is a solid ring, so a trig that has
    // just played swells by a pixel all round before it fades (the mockup's "pop").
    static constexpr int kGridX = 9;
    static constexpr int kGridPitch = 7;
    static constexpr int kGridTop = 18;
    static constexpr int kGridRow = 9;           // rows 18-89: a pixel more each than before
    void draw_grid_glow(enaga::Canvas &canvas, int grid_top) {
        uint8_t *px = canvas.data();
        for (int t = 0; t < kTracks; t++) {
            const int y = grid_top + t * kGridRow;
            for (int i = 0; i < 16; i++) {
                const int g = grid_glow_[t][i];
                if (g < 2) continue;
                const int x = kGridX + i * kGridPitch - 1;
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
    static constexpr int kScopeRows = 35;
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
        // Each column the mean of its three frames, then smoothed into the last column by half:
        // a picture of the waveform's shape, not of every hat's grain, as the mockup draws it.
        int smooth = 0;
        for (int c = 0; c < kScopeWidth; c++) {
            const uint32_t ago = start - uint32_t(c) * kScopeStride;
            int sum = 0;
            for (uint32_t k = 0; k < kScopeStride; k++) {
                sum += scope_history_.left(ago - k) + scope_history_.right(ago - k);
            }
            const int v = sum / int(kScopeStride);                // -65536..65534
            smooth = c == 0 ? v : (smooth + v) / 2;
            int row = mid - (smooth * (kScopeRows / 2 - 1)) / 32768;   // +6 dB: a mix fills it
            row = row < 0 ? 0 : (row >= kScopeRows ? kScopeRows - 1 : row);
            scope_glow_.span(c, c == 0 ? row : prev, row, grey);
            prev = row;
        }
        paint::dotted_h(canvas.data(), x, x + kScopeWidth - 1, y + mid, 3, 4);
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
        paint::small_text(px, cx - 5, cy + r / 2 + 1, digits, live ? 15 : 6);
    }

    // --- every track's level as a column ------------------------------------------------------
    //
    // Eight bars, 15 pixels a track, `rows` tall from `y`, in segments: Status::track_milli in
    // decibels (the top 36), the top quarter brightest; a muted track's bar dim. Under each its
    // machine's short name in the small font, the selected track's bright. The bars only while
    // the beat plays (a stopped screen holds still); the names always.
    // `rnd_marked`: the tracks LIVE RND moves (bit t), each marked by a dot under its name.
    void draw_track_columns(enaga::Canvas &canvas, int y, int rows, uint8_t rnd_marked = 0) {
        uint8_t *px = canvas.data();
        const Status &status = engine_.status();
        const bool running = enaga::clock::state().running != 0;
        const int base = y + rows - 1;
        for (int t = 0; t < kTracks; t++) {
            const int x = t * 15 + 1;
            canvas.rect(x, base + 1, 11, 1, 3);
            if (running) {
                const uint32_t level = status.track_milli[t].load(std::memory_order_relaxed);
                const int lit = paint::level_rows(level, rows, 36);
                for (int r = 0; r < lit; r += 2) {
                    const uint8_t g = muted_[t] ? 4 : (r >= rows - rows / 4 ? 15 : 9);
                    canvas.rect(x, base - r, 11, 1, g);
                }
            }
            paint::small_text(px, x, base + 3, machine_name(engine_.machine_id(t)),
                              muted_[t] ? 4 : (t == selected_ ? 15 : 8));
            if (((rnd_marked >> t) & 1u) != 0) canvas.rect(x + 13, base + 5, 2, 2, 13);
        }
    }

    // --- the bottom line: the screen's pages as tabs ----------------------------------------
    //
    // As the mockups draw it: each page of the screen that is up in the small font, the one
    // showing lit (a white tab, black letters), at y 121-127. EDIT's eight pages, three of
    // MIXER's twelve around the one showing, GRID's pattern pages, PERFORM with what is live
    // beside it (it has no pages), and every other screen its own name.
    // A list's row, as the drafts mark a choice: the cursor's row a lit bar with its text dark,
    // the others plain at `grey`. The bar runs to the meter's edge, so a value drawn at the
    // right of the row sits on it too.
    static void list_row(enaga::Canvas &canvas, int x, int y, const char *text, bool cursor, int grey) {
        if (cursor) {
            canvas.rect(0, y - 1, 126, 9, 15);
            canvas.text(x, y, text, 0);
        } else {
            canvas.text(x, y, text, uint8_t(grey));
        }
    }

    int draw_tab(enaga::Canvas &canvas, int x, const char *text, bool on) {
        const int w = paint::small_width(text);
        if (on) canvas.rect(x, 121, w + 4, 7, 15);
        paint::small_text(canvas.data(), x + 2, 122, text, on ? 0 : 6);
        return x + w + 4;                  // eight of EDIT's pages end inside the meter's edge
    }

    void draw_tabs(enaga::Canvas &canvas) {
        int x = 0;
        if (func_help_showing()) {
            x = draw_tab(canvas, x, "HELP", true);
            return;
        }
        if (step_page_) {
            draw_tab(canvas, x, "STEP", true);
            return;
        }
        switch (mode_) {
            case Mode::grid: {
                const int length = engine_.pattern().tracks[selected_].length;
                const int pages = length > 16 ? (length + 15) / 16 : 1;
                for (int p = 0; p < pages || p <= page_; p++) {
                    const char name[4] = {'P', 'G', char('1' + p), 0};
                    x = draw_tab(canvas, x, name, p == page_);
                }
                break;
            }
            case Mode::edit:
                for (int p = 0; p < kEditPages; p++) {
                    const Page page = Page(p);
                    x = draw_tab(canvas, x, page == Page::fltr ? "FLT" : page_name(page),
                                 p == param_page_);
                }
                break;
            case Mode::mixer: {
                const int first = mixer_bank_ > 0 ? mixer_bank_ - 1 : 0;
                for (int p = first; p < kMixerPageCount && p <= first + 2; p++) {
                    x = draw_tab(canvas, x, kMixerPages[p].name, p == mixer_bank_);
                }
                break;
            }
            case Mode::perform:
                x = draw_tab(canvas, x, "PERF", true);
                if (engine_.fill()) x = draw_tab(canvas, x, "FILL", false);
                if (engine_.status().looper_state.load(std::memory_order_relaxed) != 0) {
                    x = draw_tab(canvas, x, "LOOP", false);
                }
                if (engine_.delay_frozen()) x = draw_tab(canvas, x, "FRZ", false);
                if (note_repeat_mode_) x = draw_tab(canvas, x, "NRPT", false);
                break;
            default:
                x = draw_tab(canvas, x, mode_name(mode_), true);
                break;
        }
    }

    // --- GRID's four knobs, compact --------------------------------------------------------
    //
    // The four parameters encoders A-D turn on GRID, in the small font as name and value
    // ("TUN" dim, "G-1+" bright), the heard value with a lock's hollow tick on a bar under each (the same
    // ShownParam and draw_value_bar as EDIT), four slots of 31 pixels at `y`.
    void draw_small_slots(enaga::Canvas &canvas, int y) {
        char line[24];
        for (int slot = 0; slot < 4; slot++) {
            const int index = edit_index(slot);
            const ShownParam shown = shown_param(index);
            char *out = line;
            append(out, param_name_for(index));
            *out = 0;
            paint::small_text(canvas.data(), slot * 31, y, line, 7);
            out = line;
            append_param_value(out, index, shown.heard);
            *out = 0;
            paint::small_text(canvas.data(), slot * 31 + 13, y, line, 15);
            draw_value_bar(canvas, slot * 31, y + 7, 28, 2, shown, 8);
        }
    }
