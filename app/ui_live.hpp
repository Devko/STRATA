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

    // --- EDIT's LFO page: the shape, where the LFO is on it, and what it has been doing -------
    //
    // Under the knobs, rows 50-117 like EDIT's other pictures: one cycle of the selected shape
    // (x 0-79) with a dot riding it at the phase the audio side publishes (Status::lfo_phase)
    // and a dotted line down through it, and beside it a strip of what the LFO put out over the
    // last 40 frames (two seconds), newest at the right. The shapes here are drawn for reading,
    // from the table sine and straight lines; the sound's own are dsp/lfo.hpp's.
    static constexpr int kLfoShapeWidth = 80;
    static constexpr int kLfoStripX = 84;
    static constexpr int kLfoStrip = 40;

    // The shape at `phase` (0-255 a cycle) in Q12, -4096..4096.
    static int lfo_shape_q12(LfoWave wave, int phase) {
        phase &= 255;
        switch (wave) {
            case LfoWave::sine:     return paint::sin_q12(phase);
            case LfoWave::triangle: return phase < 64 ? phase * 64
                                         : (phase < 192 ? (128 - phase) * 64 : (phase - 256) * 64);
            case LfoWave::ramp:     return -4096 + phase * 32;
            case LfoWave::saw:      return 4096 - phase * 32;
            case LfoWave::square:   return phase < 128 ? 4096 : -4096;
            default: {
                // S+H: eight held steps, the same every cycle so the picture holds still.
                static constexpr int16_t kSteps[8] = {2600, -1400, 3900, -3300, 600, -2600, 1800, -400};
                return kSteps[phase >> 5];
            }
        }
    }

    // `shown` is Status::lfo_shown as the caller read it, so the line and the strip agree.
    void draw_lfo_picture(enaga::Canvas &canvas, uint8_t shown) {
        uint8_t *px = canvas.data();
        const int top = kPictureTop;
        const int mid = kPictureTop + kPictureRows / 2;
        const int amp = kPictureRows / 2 - 2;
        const LfoWave wave = lfo_wave(engine_.params(selected_).v[kLfoWave]);

        paint::dotted_h(px, 0, kLfoShapeWidth - 1, mid, 3, 3);
        int16_t xs[kLfoShapeWidth / 2 + 1];
        int16_t ys[kLfoShapeWidth / 2 + 1];
        int n = 0;
        for (int x = 0; x <= kLfoShapeWidth - 2; x += 2) {
            const int v = lfo_shape_q12(wave, x * 256 / kLfoShapeWidth);
            xs[n] = int16_t(paint::q4(x));
            ys[n] = int16_t(paint::q4(mid) - v * amp * paint::kSub / 4096);
            n++;
        }
        // A square or a held step jumps: join it straight down, as the scope would.
        paint::polyline(px, xs, ys, n, 9);

        const int phase = engine_.status().lfo_phase[selected_].load(std::memory_order_relaxed);
        const int dot_x = phase * kLfoShapeWidth / 256;
        const int dot_y = mid - lfo_shape_q12(wave, phase) * amp / 4096;
        paint::dotted_v(px, dot_x, top, top + kPictureRows - 1, 5, 2);
        for (int j = 8; j >= 1; j--) {
            const int behind = phase - j * 5;
            const int bx = ((behind + 256) & 255) * kLfoShapeWidth / 256;
            const int by = mid - lfo_shape_q12(wave, behind & 255) * amp / 4096;
            paint::dot(px, bx, by, 12 - j);
        }
        paint::dot(px, dot_x, dot_y, 15);

        // The strip: this frame's output joins the history (UI side; one byte a frame).
        lfo_strip_[lfo_strip_at_ % uint32_t(kLfoStrip)] = shown;
        lfo_strip_at_++;
        canvas.rect(kLfoStripX - 2, top, 1, kPictureRows, 3);
        int prev_y = -1;
        for (int i = 0; i < kLfoStrip; i++) {
            if (uint32_t(i) >= lfo_strip_at_) break;
            const uint8_t v = lfo_strip_[(lfo_strip_at_ - 1u - uint32_t(i)) % uint32_t(kLfoStrip)];
            const int x = kLfoStripX + kLfoStrip - 1 - i;
            const int y = top + kPictureRows - 1 - int(v) * (kPictureRows - 1) / 127;
            if (prev_y < 0) paint::plot_max(px, x, y, 15);
            else paint::line(px, paint::q4(x + 1), paint::q4(prev_y), paint::q4(x), paint::q4(y), 12);
            prev_y = y;
        }
    }

    // --- WAVE's SRC page: the table as a waterfall ---------------------------------------------
    //
    // The track's table, its sixteen frames stacked back to front up and to the right, each a
    // line of 40 points from the table's smoothest level (one harmonic cycle of 256 samples), the
    // frame POS plays brightest and its neighbours brighter than the rest. Each frame blacks out
    // what is behind it under its curve, so the stack reads as a surface (CyDrums' OSC page).
    // The frames come from the WaveLibrary the main context itself built (read-only once a table
    // is resident); until it is, the page says so.
    static constexpr int kTablePoints = 40;

    void draw_table_picture(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        // What the track plays, from Status (Engine::publish_heard), never the audio side's
        // arrays: the table TBL plays and its POS byte.
        const Status &status = engine_.status();
        const uint8_t heard_table = status.heard_table[selected_].load(std::memory_order_relaxed);
        const int table = heard_table == Status::kNotHeard ? -1 : int(heard_table);
        const WaveLibrary &waves = engine_.waves();
        if (table < 0 || !waves.resident(table)) {
            canvas.text(30, kPictureTop + 7, "BUILDING", 7);
            return;
        }
        const int pos = int(status.heard_pos[selected_].load(std::memory_order_relaxed)) *
                        (kWaveFrames - 1) * 16 / 127;                       // in 1/16 frames
        const int mip = kWaveMipLevels - 1;
        const int length = wave_mip_length(mip);
        const int amp = 7;
        const int live_rows = 18;                         // the LIVE frame under the stack
        const int base_y = kPictureTop + kPictureRows - 1 - live_rows - amp;
        for (int f = kWaveFrames - 1; f >= 0; f--) {
            const int16_t *samples = waves.frame(table, f, mip);
            const int ox = 4 + f * 3;                     // back frames to the right ...
            const int oy = base_y - f * 2;                // ... and up
            int16_t xs[kTablePoints];
            int16_t ys[kTablePoints];
            for (int i = 0; i < kTablePoints; i++) {
                const int v = samples[i * length / kTablePoints];
                xs[i] = int16_t(paint::q4(ox) + i * 7 * paint::kSub / 4);   // 1.75 px a point
                ys[i] = int16_t(paint::q4(oy) - v * amp * paint::kSub / 32768);
                // Hidden lines: black from just under this curve to the floor of this frame.
                const int x = ox + i * 7 / 4;
                const int top = (ys[i] >> 4) + 1;
                for (int y = top; y <= oy + amp + 1; y++) {
                    paint::plot(px, x, y, 0);
                    paint::plot(px, x + 1, y, 0);
                }
            }
            const int distance = f * 16 - pos < 0 ? pos - f * 16 : f * 16 - pos;
            const int grey = distance < 8 ? 15 : (distance < 24 ? 9 : 4);
            paint::polyline(px, xs, ys, kTablePoints, grey);
        }

        // LIVE: the frame POS plays, between its two neighbours as the oscillator reads it,
        // across the width under the stack.
        const int top = kPictureTop + kPictureRows - live_rows;
        canvas.rect(0, top, 124, 1, 3);
        paint::small_text(px, 0, top + 2, "LIVE", 6);
        const int f0 = pos / 16;
        const int f1 = f0 + 1 < kWaveFrames ? f0 + 1 : f0;
        const int mix = pos % 16;                          // sixteenths of the way to f1
        const int16_t *a = waves.frame(table, f0, mip);
        const int16_t *b = waves.frame(table, f1, mip);
        const int mid = top + live_rows / 2 + 1;
        int16_t lx[64];
        int16_t ly[64];
        for (int i = 0; i < 64; i++) {
            const int at = i * length / 64;
            const int v = (int(a[at]) * (16 - mix) + int(b[at]) * mix) / 16;
            lx[i] = int16_t(paint::q4(18) + i * 105 * paint::kSub / 64);
            ly[i] = int16_t(paint::q4(mid) - v * (live_rows / 2 - 2) * paint::kSub / 32768);
        }
        paint::polyline(px, lx, ly, 64, 13);
    }

    // --- GRAIN's SRC page: the grains as sparks with a tail ------------------------------------
    //
    // Each grain in the air (Status::wave's marks, as bright as the grain is loud) also drops a
    // soft spark into an afterglow patch over the waveform, at a height of its own, so the cloud
    // reads as particles drifting through the sample rather than ticks. Kept in the middle rows,
    // clear of the bracket's corners.
    // GRAIN's sample takes rows 50-85; under it, rows 88-117, the grain cloud as the mockup
    // draws it: each grain in the air a soft spark at its read position, at a height of its own
    // (the grain's slot, so a grain keeps its row while it lives), fading with a tail.
    static constexpr int kGrainWaveRows = 36;
    static constexpr int kGrainCloudTop = 88;

    void draw_grain_sparks(enaga::Canvas &canvas, const WaveView &view, int middle) {
        (void)middle;
        grain_glow_.fade(190);
        for (int i = 0; i < WaveView::kMarks; i++) {
            if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
            const int g = view.glow[i] > 15 ? 15 : view.glow[i];
            const int y = 3 + (i * 7) % 24;
            grain_glow_.plot(view.mark[i], y, g);
            grain_glow_.plot(view.mark[i] - 1, y, g * 2 / 3);
            grain_glow_.plot(view.mark[i] + 1, y, g * 2 / 3);
            grain_glow_.plot(view.mark[i], y - 1, g / 2);
            grain_glow_.plot(view.mark[i], y + 1, g / 2);
        }
        canvas.rect(0, kGrainCloudTop - 2, 124, 1, 2);
        paint::small_text(canvas.data(), 0, kGrainCloudTop, "GRAINS", 5);
        grain_glow_.blit(canvas.data(), 0, kGrainCloudTop);
    }

    // --- MIXER's master pages (MST, EQ): the spectrum and a vectorscope -------------------------
    //
    // While the beat plays, in the rows between the bars and the PEAK line (86-107): the output's
    // spectrum in 24 bands (app/spectrum.hpp; 94 Hz to 12 kHz, 48 dB tall, read 12 dB hot), falling
    // two rows a frame with a peak dot held for half a second; and beside it a vectorscope, 22 x
    // 22, mid up and side across (a third of full scale reaches the edge), in an afterglow, so a
    // mono mix is a vertical line and a wide one a cloud. On the EQ page the bands a kill takes
    // out sink as it is thrown.
    static constexpr int kSpectrumTop = 86;
    static constexpr int kSpectrumRows = 22;
    static constexpr int kVectorX = 101;

    void draw_master_live(enaga::Canvas &canvas, bool running) {
        uint8_t *px = canvas.data();
        if (!running) {
            vector_glow_.clear();
            for (int b = 0; b < spectrum::kBands; b++) band_rows_[b] = band_peak_[b] = 0;
            return;
        }
        uint16_t milli[spectrum::kBands];
        spectrum::analyse(scope_history_, spectrum_work_, milli);
        const int bottom = kSpectrumTop + kSpectrumRows - 1;
        for (int b = 0; b < spectrum::kBands; b++) {
            // +12 dB: a band of a mix sits well under full scale; the top 48 dB from there.
            const uint32_t boosted = uint32_t(milli[b]) * 4u;
            const int rows = paint::level_rows(boosted, kSpectrumRows, 48);
            band_rows_[b] = uint8_t(rows > band_rows_[b] - 2 ? rows : band_rows_[b] - 2);
            if (rows >= band_peak_[b]) {
                band_peak_[b] = uint8_t(rows);
                band_hold_[b] = 10;
            } else if (band_hold_[b] > 0) {
                band_hold_[b]--;
            } else if (band_peak_[b] > 0) {
                band_peak_[b]--;
            }
            const int x = b * 4;
            for (int r = 0; r < band_rows_[b]; r += 2) {
                const uint8_t g = r >= kSpectrumRows - 6 ? 15 : (r >= kSpectrumRows - 12 ? 11 : 7);
                canvas.rect(x, bottom - r, 3, 1, g);
            }
            if (band_peak_[b] > 0) canvas.rect(x, bottom - band_peak_[b], 3, 1, 15);
        }
        canvas.rect(0, bottom + 1, 96, 1, 3);

        // The vectorscope: the newest 400 frames, every second one, mid = L + R up, side = L - R
        // across, scaled so full scale reaches the edge.
        vector_glow_.fade(140);
        for (uint32_t ago = 0; ago < 400; ago += 2) {
            const int l = scope_history_.left(ago);
            const int r = scope_history_.right(ago);
            const int mid = (l + r) * 30 / 65536;          // a third of full scale fills it
            const int side = (l - r) * 30 / 65536;
            vector_glow_.plot(11 + side, 11 - mid, 12);
        }
        paint::dotted_v(px, kVectorX + 11, kSpectrumTop, bottom, 3, 3);
        paint::dotted_h(px, kVectorX, kVectorX + 21, kSpectrumTop + 11, 3, 3);
        vector_glow_.blit(px, kVectorX, kSpectrumTop);
    }

    // MIXER's fader pages: each track's live level, a thin column beside its fader, while the
    // beat plays (Status::track_milli in decibels, the fader's 48 rows showing the top 48 dB).
    void draw_fader_meter(enaga::Canvas &canvas, int track, int x, bool running) {
        if (!running) return;
        const uint32_t level = engine_.status().track_milli[track].load(std::memory_order_relaxed);
        const int lit = paint::level_rows(level, 48, 48);
        for (int r = 0; r < lit; r += 2) {
            canvas.rect(x, 46 + 47 - r, 1, 1, r >= 40 ? 15 : 9);
        }
    }

    // --- FLTR on the drums and SAMPLE: the insert filter's response over the output's spectrum ---
    //
    // The chain's filter (dsp/chain.hpp: CUT 20 Hz-18 kHz exponential, RES 0-0.95, MOD OFF, LP,
    // BP, HP in 12 or 24 dB) as a curve on a log-frequency axis, 30 Hz to 15 kHz across the
    // picture, +18 dB at the top to -36 dB at the bottom, with a dim fill under it; behind it,
    // dim, the output's spectrum (app/spectrum.hpp) while the beat plays, so a sweep is seen
    // carving it; a line at the cutoff with knob B's badge; the mode in the small font. The
    // magnitudes are the textbook state-variable ones, worked in decibels from log2, no root.
    void draw_response_picture(enaga::Canvas &canvas) {
        namespace ed = enaga::dsp;
        uint8_t *px = canvas.data();
        // What the track plays, from Status (Engine::publish_heard): CUT, RES, MOD.
        const auto heard = [&](int k) {
            return engine_.status().heard_filter[selected_][k].load(std::memory_order_relaxed);
        };
        const int mode = int(heard(2)) * 7 / 128;               // 0 OFF, 1-2 LP, 3-4 BP, 5-6 HP
        const float cut_hz = 20.0f * ed::exp2(float(heard(0)) * (9.814f / 127.0f));  // x900
        const float res = float(heard(1)) * (0.95f / 127.0f);
        const float q = 0.5f + res * 8.0f;
        const float inv_q = 1.0f / q;
        const int top = kPictureTop;
        const int rows = kPictureRows - 1;
        const int bottom = top + rows;
        const auto row_of_db = [&](float db) {
            const float clamped = db > 18.0f ? 18.0f : (db < -36.0f ? -36.0f : db);
            return top + int((18.0f - clamped) * float(rows) * (1.0f / 54.0f) + 0.5f);
        };
        // Frequency of column x: 30 Hz x 500^(x / 123).
        const auto hz_of = [](int x) { return 30.0f * ed::exp2(float(x) * (8.9658f / 123.0f)); };
        const auto x_of = [](float hz) { return int(ed::log2(hz * (1.0f / 30.0f)) * (123.0f / 8.9658f)); };

        // The grid: 100 Hz, 1 kHz, 10 kHz, and the 0 dB line.
        paint::dotted_v(px, x_of(100.0f), top, bottom, 3, 2);
        paint::dotted_v(px, x_of(1000.0f), top, bottom, 3, 2);
        paint::dotted_v(px, x_of(10000.0f), top, bottom, 3, 2);
        paint::dotted_h(px, 0, 123, row_of_db(0.0f), 3, 4);
        paint::small_text(px, x_of(100.0f) - 5, bottom - 5, "100", 4);
        paint::small_text(px, x_of(1000.0f) - 3, bottom - 5, "1K", 4);
        paint::small_text(px, x_of(10000.0f) - 9, bottom - 5, "10K", 4);

        // The output's spectrum behind, dim: each band a column across its bins' frequencies.
        if (enaga::clock::state().running != 0) {
            uint16_t milli[spectrum::kBands];
            spectrum::analyse(scope_history_, spectrum_work_, milli);
            for (int b = 0; b < spectrum::kBands; b++) {
                const int x0 = x_of(float(spectrum::kEdges[b]) * 93.75f);
                const int x1 = x_of(float(spectrum::kEdges[b + 1]) * 93.75f);
                const int lit = paint::level_rows(uint32_t(milli[b]) * 2u, rows * 3 / 4, 48);
                // Thin dim bars every other column, as the mockup draws the spectrum behind.
                for (int x = (x0 < 0 ? 0 : x0) | 1; x < x1 && x < 124; x += 2) {
                    canvas.rect(x, bottom - lit + 1, 1, lit, 4);
                }
            }
        }

        // The response, column by column, joined as an anti-aliased line.
        int16_t xs[62];
        int16_t ys[62];
        int n = 0;
        for (int x = 0; x < 124; x += 2) {
            float db = 0.0f;
            if (mode != 0) {
                const float w = hz_of(x) / cut_hz;
                const float w2 = w * w;
                const float a = 1.0f - w2;
                const float b = w * inv_q;
                const float den = a * a + b * b;                  // |H|^2 = num / den
                const float num = mode <= 2 ? 1.0f : (mode <= 4 ? b * b : w2 * w2);
                db = 3.0103f * ed::log2((num + 1e-9f) / (den + 1e-9f));
                if (mode % 2 == 0) db *= 2.0f;                    // the 24 dB modes: two stages
            }
            const int y = row_of_db(db);
            for (int fy = y + 3; fy < bottom; fy += 3) canvas.pixel(x, fy, 2);
            xs[n] = int16_t(paint::q4(x));
            ys[n] = int16_t(paint::q4(y));
            n++;
        }
        paint::polyline(px, xs, ys, n, mode == 0 ? 6 : 15);

        if (mode != 0) {
            const int cx = x_of(cut_hz);
            if (cx >= 0 && cx < 124) {
                canvas.rect(cx, top, 1, rows + 1, 6);
                canvas.rect(cx - 3 < 0 ? 0 : cx - 3, bottom - 7, 7, 7, 15);
                paint::small_text(px, (cx - 3 < 0 ? 0 : cx - 3) + 2, bottom - 6, "B", 0);
            }
        }
        static const char *const kModes[7] = {"OFF", "LP12", "LP24", "BP12", "BP24", "HP12", "HP24"};
        paint::small_text(px, 123 - paint::small_width(kModes[mode]), top + 1, kModes[mode], 9);
    }

    // --- the bottom line: the screen's pages as tabs ----------------------------------------
    //
    // As the mockups draw it: each page of the screen that is up in the small font, the one
    // showing lit (a white tab, black letters), at y 121-127. EDIT's eight pages, three of
    // MIXER's twelve around the one showing, GRID's pattern pages, PERFORM with what is live
    // beside it (it has no pages), and every other screen its own name.
    int draw_tab(enaga::Canvas &canvas, int x, const char *text, bool on) {
        const int w = paint::small_width(text);
        if (on) canvas.rect(x, 121, w + 4, 7, 15);
        paint::small_text(canvas.data(), x + 2, 122, text, on ? 0 : 6);
        return x + w + 6;
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

    // --- MIXER's master pages, as the mockup draws them ---------------------------------------
    //
    // The page's knobs two to a line at the top (name and value, a bar under each), then while
    // the beat plays the output's spectrum large (rows 41-80, peak dots, 100 / 1K / 10K under it),
    // and under that a vectorscope (30 x 30, afterglow), the glue compressor's gain reduction
    // and the limiter as two bars falling from the top, the reduction in decibels and the peak.
    // Stopped, the analysers' rows say so and hold still.
    void draw_master_page(enaga::Canvas &canvas) {
        char line[24];
        uint8_t *px = canvas.data();
        const bool running = enaga::clock::state().running != 0;
        for (int slot = 0; slot < 4; slot++) {
            const int8_t field = kMixerPages[mixer_bank_].field[slot];
            const int x = (slot % 2) * 62;
            const int y = 18 + (slot / 2) * 11;
            char *out = line;
            if (field < 0) {
                append(out, "---");
            } else {
                append(out, master_field_name(uint8_t(field)));
                *out++ = ' ';
                append_mixer_value(out, mixer_bank_, slot);
            }
            *out = 0;
            canvas.text(x, y, line, field < 0 ? 4 : 15);
            if (field >= 0) {
                const int bar = int(mixer_byte(mixer_bank_, slot)) * 58 / 127;
                canvas.rect(x, y + 8, 58, 1, 3);
                if (bar > 0) canvas.rect(x, y + 8, bar, 1, 12);
            }
        }
        if (!running) {
            vector_glow_.clear();
            for (int b = 0; b < spectrum::kBands; b++) band_rows_[b] = band_peak_[b] = 0;
            paint::small_text(px, 0, 60, "PLAY: SPECTRUM AND SCOPE", 5);
            return;
        }

        // The spectrum, 24 bands of 5 pixels (4 lit), 40 rows, read 12 dB hot over 48 dB.
        uint16_t milli[spectrum::kBands];
        spectrum::analyse(scope_history_, spectrum_work_, milli);
        const int rows = 40, bottom = 41 + rows - 1;
        for (int b = 0; b < spectrum::kBands; b++) {
            const int lit = paint::level_rows(uint32_t(milli[b]) * 4u, rows, 48);
            band_rows_[b] = uint8_t(lit > band_rows_[b] - 3 ? lit : band_rows_[b] - 3);
            if (lit >= band_peak_[b]) { band_peak_[b] = uint8_t(lit); band_hold_[b] = 12; }
            else if (band_hold_[b] > 0) band_hold_[b]--;
            else if (band_peak_[b] > 0) band_peak_[b]--;
            const int x = b * 5;
            for (int r = 0; r < band_rows_[b]; r += 2) {
                const uint8_t g = r >= rows - 10 ? 15 : (r >= rows - 22 ? 11 : 7);
                canvas.rect(x, bottom - r, 4, 1, g);
            }
            if (band_peak_[b] > 0) canvas.rect(x, bottom - band_peak_[b], 4, 1, 15);
        }
        canvas.rect(0, bottom + 1, 120, 1, 4);
        paint::small_text(px, 9, bottom + 3, "100", 5);
        paint::small_text(px, 52, bottom + 3, "1K", 5);
        paint::small_text(px, 98, bottom + 3, "10K", 5);

        // The vectorscope, 30 x 30 at (0, 88), in a frame with its axes dotted.
        vector_glow_.fade(130);
        for (uint32_t ago = 0; ago < 400; ago += 2) {
            const int l = scope_history_.left(ago);
            const int r = scope_history_.right(ago);
            vector_glow_.plot(11 + (l - r) * 30 / 65536, 11 - (l + r) * 30 / 65536, 13);
        }
        canvas.rect(0, 89, 30, 1, 3);
        canvas.rect(0, 116, 30, 1, 3);
        paint::dotted_v(px, 15, 90, 115, 3, 3);
        paint::dotted_h(px, 1, 29, 102, 3, 3);
        vector_glow_.blit(px, 4, 91);

        // The compressor's reduction and the limiter, falling from the top of two bars.
        const uint32_t gain = engine_.status().comp_gain_milli.load(std::memory_order_relaxed);
        const int reduction_db = -paint::level_db_q8(gain) / 256;              // 0..60
        const int cmp = reduction_db * 2 > 25 ? 25 : reduction_db * 2;           // 2 rows a dB
        canvas.rect(36, 89, 8, 25, 2);
        if (cmp > 0) canvas.rect(36, 89, 8, cmp, 13);
        canvas.rect(48, 89, 8, 25, 2);
        if (limit_lit_) canvas.rect(48, 89, 8, 25, 13);
        paint::small_text(px, 34, 116, "CMP", 6);
        paint::small_text(px, 48, 116, "LIM", 6);
        paint::small_text(px, 64, 90, "GR", 6);
        char *out = line;
        if (reduction_db > 0) *out++ = '-';
        append_number(out, uint32_t(reduction_db));
        append(out, " DB");
        *out = 0;
        canvas.text(64, 97, line, 15);
        out = line;
        const int peak_milli = int(engine_.status().peak_milli.load(std::memory_order_relaxed));
        append(out, "PEAK ");
        append_tenths(out, uint32_t(peak_milli > 9999 ? 9999 : peak_milli));
        *out = 0;
        paint::small_text(px, 64, 113, line, peak_milli >= 1000 ? 15 : 9);
    }
