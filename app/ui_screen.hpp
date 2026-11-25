// The screen: the frame, the master meter and live strip, the FUNC help, GRID and the step
// page.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- screen -----------------------------------------------------------

    void draw(enaga::Canvas &canvas) override {
        const uint32_t draw_start = enaga::ms();
        char line[40];
        char *out = line;

        canvas.clear();
        // Every frame on every screen, so the ring never fills while a scope is not up.
        scope_history_.drain(engine_.scope());
        if (echo_ != Echo::none && notice_ == echo_buffer_) refresh_echo();

        const Status &status = engine_.status();
        const enaga::clock::State clock = enaga::clock::state();
        const bool running = clock.running != 0;

        // --- the frame (docs/CONTROLS.md, "The screen frame") ---------------------------------
        // The same four things in the same place on every screen, drawn from words the audio
        // side published: nothing here walks a pattern or a buffer.
        //
        // The top bar, as the mockups draw it (tools/visuals_mockup.py): the live pattern as a lit
        // badge, bar.beat beside it, then the tempo (or EXT under an outside clock), the play state
        // and the load in the small font, the LOCK mark, and the selected track as a lit badge at
        // the right. The pattern comes from the status rather than from the project, because a
        // chain or a queued change moves it on the audio side. Load in whole percent; SETUP keeps
        // the held MAX.
        const uint32_t into = status.pattern_into.load(std::memory_order_relaxed);
        const uint32_t loop = status.pattern_loop.load(std::memory_order_relaxed);
        uint8_t *px = canvas.data();
        out = line;
        *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        append_padded(out, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
        *out = 0;
        canvas.rect(0, 0, 21, 8, 15);
        canvas.text(2, 0, line, 0);
        out = line;
        append_number(out, into / kTicksPerBar % 10u + 1u);
        *out++ = '.';
        append_number(out, into % kTicksPerBar / uint32_t(kPpqn) + 1u);
        *out = 0;
        canvas.text(24, 0, line, 15);
        out = line;
        if (int(clock.source) != ENAGA_CLOCK_INTERNAL) {
            // Not ours to set under an outside source: say whose it is.
            append(out, int(clock.source) == ENAGA_CLOCK_MIDI ? "MIDI" : "SYNC");
        } else {
            append_tenths(out, uint32_t(clock.tempo * 10.0f + 0.5f));
        }
        append(out, running ? " >" : "");
        *out = 0;
        paint::small_text(px, 44, 2, line, 10);
        out = line;
        append_number(out, (enaga::audio_load() + 5u) / 10u);
        *out++ = '%';
        *out = 0;
        paint::small_text(px, 80, 2, line, 6);
        if (lock_mark() != nullptr) paint::small_text(px, 96, 2, lock_mark(), 15);
        out = line;
        *out++ = 'T';
        append_number(out, uint32_t(selected_) + 1);
        *out = 0;
        canvas.rect(106, 0, 20, 8, 15);
        canvas.text(108, 0, line, 0);

        draw_progress(canvas, into, loop, running);

        // Under the progress line: a notice, LEARN or the MUTATE verdict as a lit toast while
        // there is one; else, only while something is off rest, the live strip in the small font;
        // else nothing (clock trouble the same way). GRID shows its notice in its own line.
        const int y2 = kFrameStripY;
        const auto toast = [&](const char *text) {
            canvas.rect(0, y2 - 1, 124, 9, 15);
            canvas.text(1, y2, text, 0);
        };
        if (func_help_showing()) {
            // The help's title takes this line (draw_func_help).
        } else if ((step_page_ || mode_ != Mode::grid) && notice_showing()) {
            // What just happened. GRID shows it in its edit-state line; every other screen had
            // nowhere for it, so a gesture there (F1 + F3's "again to save", an octave, a scene
            // captured) answered with nothing at all.
            out = line;
            append_shown(out, notice_, kLineChars - 1);
            *out = 0;
            toast(line);
        } else if ((step_page_ || mode_ != Mode::grid) && macro_learn_ >= 0) {
            // MACRO LEARN says so for as long as it is on: the knobs' pushes are not theirs.
            learn_line(line);
            toast(line);
        } else if ((step_page_ || mode_ != Mode::grid) && verdict_keys_free()) {
            // A MUTATE waiting for its verdict says so for as long as it waits: the notice
            // fades after a second and a half, and the keys it is waiting on do not.
            toast(kVerdictHint);
        } else if (live_strip(line)) {
            paint::small_text(px, 0, y2 + 1, line, 12);
        } else if (clock_refused_ && int(clock.source) == ENAGA_CLOCK_INTERNAL) {
            // Internal source and still refused: worth seeing rather than silently ignoring.
            paint::small_text(px, 0, y2 + 1, "CLOCK REFUSED", 10);
        }

        if (func_help_showing()) {
            draw_func_help(canvas);
        } else if (step_page_) {
            draw_step_page(canvas);
        } else {
            switch (mode_) {
                case Mode::grid:    draw_grid(canvas, running); break;
                case Mode::edit:    draw_edit(canvas); break;
                case Mode::mixer:   draw_mixer(canvas); break;
                case Mode::project: draw_project(canvas); break;
                case Mode::perform: draw_perform(canvas); break;
                case Mode::keys:    draw_keys(canvas); break;
                case Mode::resample: draw_resample(canvas); break;
                case Mode::machine: draw_machine(canvas); break;
                case Mode::kit:     draw_kit(canvas); break;
                case Mode::samples: draw_samples(canvas); break;
                case Mode::sounds: draw_sounds(canvas); break;
                case Mode::menu:    draw_menu(canvas); break;
            }
        }

        // The bottom line: tabs for the screen's pages, as the mockups draw them, the one showing
        // lit (draw_tabs). A dropped edit or the CPU guard takes the right end, because both are
        // things the user must see (app/commands.hpp: a dropped command should be visible rather
        // than mysterious; core/cpuguard.hpp: why a chord got thinner). The tracks sounding, the
        // held MAX and the device moved to SETUP.
        draw_tabs(canvas);
        const uint32_t dropped = engine_.commands().dropped();
        const uint8_t guard = status.cpu_guard.load(std::memory_order_relaxed);
        if (dropped || guard) {
            out = line;
            if (dropped) { append(out, "DROP "); append_number(out, dropped); }
            else { append(out, "GUARD"); append_number(out, guard); }
            *out = 0;
            int width = 0;
            while (line[width] != 0) width++;
            canvas.rect(124 - width * 6 - 2, 119, width * 6 + 2, 9, 0);
            canvas.text(124 - width * 6, 120, line, 15);
        }

        draw_frame_meter(canvas);
        count_draw_time(draw_start);
    }

    // The progress line under the top line: one pixel across the pattern's pass, lit from the
    // left on a dim track, its head brightest, and the beats (dim) and bars (brighter) marked on
    // the part still to come, so the line reads as the pattern's shape and not only a fraction.
    void draw_progress(enaga::Canvas &canvas, uint32_t into, uint32_t loop, bool running) {
        canvas.rect(0, kFrameProgressY, 128, 1, 2);
        const uint32_t progress = loop > 0 ? (into + 1u) * 128u / loop : 0u;
        const int lit = running ? int(progress > 128u ? 128u : progress) : 0;
        if (lit > 0) canvas.rect(0, kFrameProgressY, lit, 1, 11);
        if (loop > 0) {
            // Beats only when they are at least three pixels apart, bars at least two.
            const bool beats = uint32_t(kPpqn) * 128u >= 3u * loop;
            const bool bars = kTicksPerBar * 128u >= 2u * loop;
            for (uint32_t t = uint32_t(kPpqn); t < loop; t += uint32_t(kPpqn)) {
                const bool bar = t % kTicksPerBar == 0;
                if (!(bar ? bars : beats)) continue;
                const int x = int(t * 128u / loop);
                if (x >= lit) canvas.pixel(x, kFrameProgressY, bar ? 7 : 4);
            }
        }
        if (lit > 0) canvas.pixel(lit - 1, kFrameProgressY, 15);
    }

    // How long draw() takes, averaged over kDrawWindow frames: enaga::ms() has a millisecond's
    // resolution and a frame takes about one, so a single reading says little and the sum of
    // many says it to a hundredth. Shown on SETUP, and logged once for the first window that
    // ends past kDrawLogMs, so the emulator's scenario_visuals and the unit both report it
    // (docs/BUDGET.md, "The screen"). Once only: Enaga's log keeps 16 lines and other
    // scenarios read theirs.
    static constexpr uint32_t kDrawWindow = 64;
    static constexpr uint32_t kDrawLogMs = 5000;
    void count_draw_time(uint32_t start) {
        draw_ms_sum_ += enaga::ms() - start;
        if (++draw_count_ < kDrawWindow) return;
        const uint32_t hundredths = draw_ms_sum_ * 100u / kDrawWindow;
        draw_hundredths_ = hundredths > 999u ? 999u : hundredths;
        draw_ms_sum_ = 0;
        draw_count_ = 0;
        if (!draw_logged_ && enaga::ms() >= kDrawLogMs) {
            draw_logged_ = true;
            char text[24];
            char *out = text;
            append(out, "draw ");
            append_hundredths(out, draw_hundredths_);
            append(out, " ms");
            *out = 0;
            enaga::log(text);
        }
    }

    static void append_hundredths(char *&out, uint32_t hundredths) {
        append_number(out, hundredths / 100u);
        *out++ = '.';
        *out++ = char('0' + hundredths / 10u % 10u);
        *out++ = char('0' + hundredths % 10u);
    }

    // --- the frame's master meter and live strip ------------------------------------------
    //
    // Rows: top line 0-6, progress 8, second line (strip) 10-16, the screen's own 18-117,
    // bottom line 120. The meter is the two columns no text reaches: 21 characters end at x
    // 124 (the layout check holds every string to the screen), so x 126-127 is the frame's.
    static constexpr int kFrameProgressY = 8;
    static constexpr int kFrameStripY = 10;
    static constexpr int kFrameMeterX = 126;
    static constexpr int kFrameMeterTop = 10;
    static constexpr int kFrameMeterRows = 108;          // rows 10-117
    static constexpr int kFrameStripChars = 17;          // the strip stops at x 100 ...
    static constexpr int kFrameLockX = 114;              // ... and "L" or "L+" sits at 114-124
    static constexpr uint32_t kMeterHoldMs = 1000;

    // The LOCK indicator: "L" while the playing pattern has a lock, "L+" once the live set has
    // changed since (the concept's "L*": Enaga's font has no '*'). nullptr draws nothing.
    const char *lock_mark() const {
        const uint8_t state = lock_state();
        if ((state & kLockHas) == 0) return nullptr;
        return (state & kLockChanged) != 0 ? "L+" : "L";
    }

    uint8_t lock_state() const {
        return engine_.status().lock_state.load(std::memory_order_relaxed);
    }

    // What is off rest, shortest first-read first: kills, the performance effect, the morph,
    // the macros away from zero, the looper. "K:LMH FX12 S100 M:ABCD LP" is 25 characters, so
    // whatever does not fit in kFrameStripChars is left out whole. False when all is at rest.
    bool live_strip(char *line) {
        char *out = line;
        char part[12];
        char *p;
        const auto add = [&](const char *text) {
            const int used = int(out - line);
            int extra = 0;
            while (text[extra] != '\0') extra++;
            const int gap = used > 0 ? 1 : 0;
            if (used + gap + extra > kFrameStripChars) return;
            if (gap) *out++ = ' ';
            append(out, text);
        };

        // Where the hand is now, first: the stutter's step and the rate under REPEAT or a roll.
        if (stutter_raw_ >= 0) {
            p = part;
            append(p, "ST");
            append_number(p, uint32_t(stutter_step_) + 1);
            *p = 0;
            add(part);
        }
        if (repeat_held_ || rolling_pads_ != 0) {
            p = part;
            *p++ = 'R';
            append(p, repeat_length_name(repeat_length_));
            *p = 0;
            add(part);
        }
        const uint8_t kills = engine_.project().settings.eq_kills;
        if (kills & 7u) {
            p = part;
            append(p, "K:");
            if (kills & 1u) *p++ = 'L';
            if (kills & 2u) *p++ = 'M';
            if (kills & 4u) *p++ = 'H';
            *p = 0;
            add(part);
        }
        if (engine_.perf_fx() != PerfFx::none) {
            p = part;
            append(p, "FX");
            append_number(p, uint32_t(engine_.perf_fx()));
            *p = 0;
            add(part);
        }
        const uint32_t morph = engine_.morph();
        if (morph != 0) {
            p = part;
            *p++ = 'S';
            append_number(p, (morph * 100u + kMorphScale / 2) / kMorphScale);
            *p = 0;
            add(part);
        }
        p = part;
        append(p, "M:");
        for (int m = 0; m < kMacros; m++) {
            if (engine_.macro_position(m) != 0) *p++ = char('A' + m);
        }
        *p = 0;
        if (p - part > 2) add(part);
        // REL ALL and LEN ALL away from rest, and which way: RL- shorter tails, LN+ longer
        // notes. Two letters, so they cannot be read as the repeat rate's R1/16.
        if (engine_.rel_all() != 0) add(engine_.rel_all() < 0 ? "RL-" : "RL+");
        if (engine_.len_all() != 0) add(engine_.len_all() < 0 ? "LN-" : "LN+");
        if (engine_.status().looper_state.load(std::memory_order_relaxed) != 0) add("LP");
        *out = 0;
        return out != line;
    }

    // Two columns down the right edge: the master level (the audio side's falling peak), a
    // bright dot at the highest reading of the last second, and the top pixel lit for a second
    // after any block that reached full scale.
    void draw_frame_meter(enaga::Canvas &canvas) {
        const Status &status = engine_.status();
        const uint32_t level = status.meter_milli.load(std::memory_order_relaxed);
        if (level >= meter_hold_ || uint32_t(last_ms_ - meter_hold_ms_) >= kMeterHoldMs) {
            meter_hold_ = level;
            meter_hold_ms_ = last_ms_;
        }
        const uint8_t limits = status.limit_seen.load(std::memory_order_relaxed);
        if (limits != limit_seen_) {
            limit_seen_ = limits;
            limit_ms_ = last_ms_;
            limit_lit_ = true;
        }
        if (limit_lit_ && uint32_t(last_ms_ - limit_ms_) >= kMeterHoldMs) limit_lit_ = false;

        canvas.rect(kFrameMeterX, kFrameMeterTop, 2, kFrameMeterRows, 0);
        // Lit every other row, so it reads as segments: the top 6 dB brightest, the next 12 dB
        // bright, the rest dim, as on a desk.
        const int bar = meter_rows(level);
        const int bottom = kFrameMeterTop + kFrameMeterRows - 1;
        for (int r = 0; r < bar; r += 2) {
            const uint8_t grey = r >= kFrameMeterRows - kMeterRowsPer6Db ? 15
                               : (r >= kFrameMeterRows - 3 * kMeterRowsPer6Db ? 11 : 7);
            canvas.rect(kFrameMeterX, bottom - r, 2, 1, grey);
        }
        const int held = meter_rows(meter_hold_);
        if (held > 0) {
            canvas.rect(kFrameMeterX, kFrameMeterTop + kFrameMeterRows - held, 2, 1, 15);
        }
        if (limit_lit_) canvas.rect(kFrameMeterX, kFrameMeterTop, 2, 1, 15);
    }

    // Milli of full scale to rows, in decibels: the column shows the top kMeterRangeDb, so a
    // quiet mix moves the meter instead of sitting in its bottom rows as a linear scale left it.
    static constexpr int kMeterRangeDb = 48;
    static constexpr int kMeterRowsPer6Db = kFrameMeterRows * 6 / kMeterRangeDb;
    static int meter_rows(uint32_t milli) {
        return paint::level_rows(milli, kFrameMeterRows, kMeterRangeDb);
    }

    // --- the FUNC help: FUNC held on its own ------------------------------
    //
    // docs/CONTROLS.md, "FUNC help". Holding FUNC for a moment with nothing else shows help for
    // the screen that is up: its knobs and its gestures first (core/help.hpp), then, a screen key
    // at a time, FUNC + each function key, FUNC + a knob or pad, and the upper pads. The next
    // other key, knob or pad (or letting go of FUNC) takes it away, so it never sits in the way
    // of a chord.

    static constexpr uint32_t kFuncHelpMs = 600;

    bool func_help_showing() const {
        return shift_ && shift_alone_ && !name_entry_.active() &&
               uint32_t(last_ms_ - shift_at_ms_) >= kFuncHelpMs;
    }

    // Where the help is: the screen, the EDIT or MIXER page, and the knobs as the screen names
    // them, so the help says "B FMA FM AMOUNT" on a kick and "B SNP SNAPPY" on a snare.
    HelpPlace help_place() const {
        HelpPlace place;
        place.machine = engine_.machine_id(selected_);
        place.page = uint8_t(param_page_);
        for (int i = 0; i < 4; i++) place.knob[i] = track_param_name(edit_index(i));
        if (step_page_) {
            place.screen = HelpScreen::step;
            return place;
        }
        switch (mode_) {
            case Mode::grid:     place.screen = HelpScreen::grid; break;
            case Mode::edit:
                place.screen = macro_learn_ >= 0 ? HelpScreen::learn : HelpScreen::edit;
                break;
            case Mode::keys:     place.screen = HelpScreen::keys; break;
            case Mode::mixer:
                place.screen = HelpScreen::mixer;
                place.page = uint8_t(mixer_bank_);
                break;
            case Mode::perform:  place.screen = HelpScreen::perform; break;
            case Mode::project:
                switch (project_page_) {
                    case ProjectPage::chain:   place.screen = HelpScreen::chain; break;
                    case ProjectPage::pattern: place.screen = HelpScreen::pattern; break;
                    case ProjectPage::setup:   place.screen = HelpScreen::setup; break;
                    case ProjectPage::files:   place.screen = HelpScreen::projects; break;
                    default:                   place.screen = HelpScreen::project; break;
                }
                break;
            case Mode::resample: place.screen = HelpScreen::resample; break;
            case Mode::machine:  place.screen = HelpScreen::machine; break;
            case Mode::kit:      place.screen = HelpScreen::kit; break;
            case Mode::samples:  place.screen = HelpScreen::samples; break;
            case Mode::sounds:   place.screen = HelpScreen::sounds; break;
            case Mode::menu:     place.screen = HelpScreen::menu; break;
        }
        return place;
    }

    // Title on the second line, ten rows, and the page line: "SCREEN KEYS: PAGE 1/7", which is
    // also how anyone finds out there is more than one page.
    void draw_func_help(enaga::Canvas &canvas) {
        canvas.rect(0, 9, 128, 110, 0);
        const HelpPlace place = help_place();
        char title[kHelpChars + 1];
        char rows[kHelpRows][kHelpChars + 1];
        const int pages = help_page_count(place);
        const int count = help_page(place, help_page_, title, rows);
        canvas.text(0, 9, title, 15);
        for (int r = 0; r < count; r++) {
            int length = 0;
            while (rows[r][length] != 0) length++;
            const bool heading = length > 0 && rows[r][length - 1] == ':';
            canvas.text(0, 18 + r * 9, rows[r], heading ? 15 : 12);
        }
        char line[40];
        char *out = line;
        append(out, pages < 10 ? "SCREEN KEYS: PAGE " : "PAGE ");
        append_number(out, uint32_t(help_page_ % pages + 1));
        *out++ = '/';
        append_number(out, uint32_t(pages));
        *out = 0;
        canvas.text(0, 108, line, 7);
    }

    // --- GRID: all eight tracks at a glance ------------------------------

    void draw_grid(enaga::Canvas &canvas, bool running) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        // The mockup's grid (tools/visuals_mockup.py): a gutter with each track's number and
        // live level, then sixteen 5 x 6 cells seven pixels apart from x 9 (kGridX), the playhead's
        // column lit behind them while the beat plays, and the trigs that just played glowing
        // (app/ui_live.hpp).
        const int grid_top = kGridTop;
        uint8_t *px = canvas.data();
        if (running) {
            const int head = status.playhead[selected_].load(std::memory_order_relaxed);
            if (head / 16 == page_) {
                canvas.rect(kGridX + (head % 16) * kGridPitch - 1, grid_top, 7, kTracks * kGridRow, 2);
            }
        }
        for (int t = 0; t < kTracks; t++) {
            const int y = grid_top + t * kGridRow;
            const uint8_t head = status.playhead[t].load(std::memory_order_relaxed);
            const char number[2] = {char('1' + t), 0};
            paint::small_text(px, 2, y + 2, number, t == selected_ ? 15 : (muted_[t] ? 3 : 7));
            if (running) {
                const uint32_t level = status.track_milli[t].load(std::memory_order_relaxed);
                const int lit = paint::level_rows(level, 6, 36);
                if (lit > 0) canvas.rect(6, y + 7 - lit, 2, lit, muted_[t] ? 4 : 10);
            }
            for (int i = 0; i < 16; i++) {
                const int step = i + page_ * 16;
                const Step &s = engine_.pattern().tracks[t].steps[step];
                const bool trig = (s.flags & kStepTrig) != 0;
                const bool on_head = running && step == head;

                uint8_t grey = 2;
                if (trig) grey = (s.flags & kStepAccent) ? 15 : 11;
                if (on_head) grey = uint8_t(grey < 8 ? 7 : 15);

                const int x = kGridX + i * kGridPitch;           // step 16 ends at x 118
                if (trig || on_head) {
                    canvas.rect(x, y + 1, 5, 6, grey);
                    if (trig) draw_step_glyph(canvas, x, y + 1, s);
                } else if ((s.flags & kStepTrigless) != 0) {
                    draw_trigless_cell(canvas, x, y + 1);
                } else {
                    // An empty step: a dot, a beat's first a little brighter.
                    canvas.rect(x + 2, y + 3, 1, 2, i % 4 == 0 ? 5 : 3);
                }
            }
            if (t == selected_) canvas.rect(0, y, 1, kGridRow - 1, 12);   // the selected track's marker
            if (muted_[t]) canvas.rect(0, y + 3, 128, 1, 4);     // a muted track is struck out
        }
        update_grid_glow(running);
        draw_grid_glow(canvas, grid_top);

        // The track's own layout: length and speed are per track (polymeter), so they belong on
        // screen next to the track number rather than behind a menu.
        const TrackPattern &track = engine_.pattern().tracks[selected_];
        out = line;
        // "T1", not "TRK 1": with a three-character speed and a two-digit length the long form
        // was 22 characters, and the speed ran off the right edge.
        append(out, "T");
        append_number(out, uint32_t(selected_ + 1));
        append(out, muted_[selected_] ? " MUTE" : "     ");
        append(out, " PG");
        append_number(out, uint32_t(page_ + 1));
        append(out, " L");
        append_number(out, track.length);
        append(out, " X");
        append(out, speed_name(Speed(track.speed)));
        *out = 0;
        // In the small font under the grid, as the mockup has it, with the track's machine.
        {
            char info[48];
            char *o = info;
            append(o, line);
            append(o, "  ");
            append(o, machine_name(engine_.machine_id(selected_)));
            *o = 0;
            paint::small_text(px, 0, 92, info, 10);
        }

        draw_edit_state(canvas, 100);
        draw_small_slots(canvas, 109);
    }

    // What a trig carries, cut dark into its 5 x 6 cell (docs/CONTROLS.md, "Locks on screen"):
    //
    //     C . . . P      C  a condition (FILL, A:B, PRE ...): the top-left corner is cut
    //     . . . . .      P  a probability under 100%: the top-right corner is cut
    //     . . L . .      L  parameter locks: a dot in the middle, two pixels tall
    //     . . L . .      R  a ratchet of two hits or more: two teeth cut in the bottom edge
    //     . . . . .
    //     . R . R .
    //
    // Four marks that never overlap, so any mix of them reads. Cheap: the fields are already in
    // the step the cell was drawn from, and each mark is one or two pixels.
    static void draw_step_glyph(enaga::Canvas &canvas, int x, int y, const Step &s) {
        if (condition_kind(s.condition) != Condition::NONE) canvas.pixel(x, y, 0);
        if (s.probability < 100) canvas.pixel(x + 4, y, 0);
        if (s.lock_count > 0) canvas.rect(x + 2, y + 2, 1, 2, 0);
        if (ratchet_count(s.ratchet) > 1) {
            canvas.pixel(x + 1, y + 5, 0);
            canvas.pixel(x + 3, y + 5, 0);
        }
    }

    // A trigless step -- locks with no note -- as the outline of a cell: something happens here,
    // but nothing hits. It used to draw as an empty step on GRID.
    static void draw_trigless_cell(enaga::Canvas &canvas, int x, int y) {
        canvas.rect(x, y, 5, 1, 6);
        canvas.rect(x, y + 5, 5, 1, 6);
        canvas.rect(x, y + 1, 1, 4, 6);
        canvas.rect(x + 4, y + 1, 1, 4, 6);
    }

    // The line that says what editing mode you are in: recording, the quantize amount, the
    // selection, and how much undo there is. Without it, REC and a held selection are invisible
    // states that change what every other key does.
    void draw_edit_state(enaga::Canvas &canvas, int y) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        if (notice_showing()) {
            // Cut to the line: a notice is built in places all over this file, and one that ran
            // off the edge would lose its end, which is usually the part that says what to do.
            out = line;
            append_shown(out, notice_, kLineChars);
            *out = 0;
            canvas.text(0, y, line, 15);
            return;
        }
        if (draw_lock_readout(canvas, y)) return;
        if (macro_learn_ >= 0) {                         // see draw(): the same hint, in GRID
            learn_line(line);
            canvas.text(0, y, line, 15);
            return;
        }
        if (verdict_keys_free()) {                       // see draw(): the same hint, in GRID
            canvas.text(0, y, kVerdictHint, 15);
            return;
        }

        // Built from pieces in order of importance, each left out whole if it would not fit:
        // all of them at once ("REC Q100 SEL 1-64 U16/R16 FULL 255") is 35 characters. Undo and
        // redo are also on the CLR and OK LEDs, so they are the first to go.
        char part[24];
        char *p = part;
        out = line;
        if (rec_) {
            p = part;
            append(p, "REC Q");
            append_number(p, uint32_t(quantize()));
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        if (range_first_ >= 0) {
            p = part;
            append(p, "SEL ");
            append_number(p, uint32_t(range_first_ + 1));
            *p++ = '-';
            append_number(p, uint32_t(range_last_ + 1));
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        const uint32_t refused = status.refused.load(std::memory_order_relaxed);
        if (refused) {
            // A refused edit is almost always a full lock pool. Saying nothing would leave
            // someone repeating an edit that cannot work.
            p = part;
            append(p, "FULL ");
            append_number(p, refused);
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        p = part;
        append(p, "U");
        append_number(p, status.undo_depth.load(std::memory_order_relaxed));
        const uint8_t redo = status.redo_depth.load(std::memory_order_relaxed);
        if (redo) {
            append(p, "/R");
            append_number(p, redo);
        }
        *p = 0;
        append_if_fits(out, line, part);
        *out = 0;
        canvas.text(0, y, line, rec_ ? 15 : 9);
    }

    // --- the step page: one step's own fields ------------------------------

    void draw_step_page(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Step &step = engine_.pattern().tracks[selected_].steps[page_step_];

        out = line;
        append(out, "STEP ");
        append_number(out, uint32_t(page_step_ + 1));
        append(out, "  TRK ");
        append_number(out, uint32_t(selected_ + 1));
        *out = 0;
        canvas.text(0, 20, line, 14);

        // Which of the field pages is showing. Same language as EDIT's parameter pages.
        for (int p = 0; p < kFieldPages; p++) {
            canvas.rect(p * 32 + 2, 32, 28, 3, p == field_page_ ? 14 : 4);
        }

        // Four fields, each a name and a value. No bars here: a condition or a note name is not
        // a quantity, and a bar under "NOT PRE" would only suggest it was one.
        for (int slot = 0; slot < 4; slot++) {
            const int field = field_page_ * 4 + slot;
            const int y = 42 + slot * 14;
            if (field >= kStepFields) {
                canvas.text(0, y, "---", 4);
                continue;
            }

            canvas.text(0, y, step_field_name(StepField(field)), 13);

            // Sized from core/stepedit.hpp rather than guessed: the longest label is 8
            // characters and this is the one place a wrong guess would be a stack smash the
            // host build never sees.
            char text[kStepFieldTextMax];
            step_field_text(step, StepField(field), text, int(sizeof text));
            canvas.text(44, y, text, 15);
        }

        // What the step actually is, under the fields: a step that is off says so, because
        // every field above it is then inert.
        out = line;
        if (step.flags & kStepTrig) {
            append(out, "TRIG");
        } else if (step.flags & kStepTrigless) {
            append(out, "TRIGLESS");
        } else {
            append(out, "OFF");
        }
        if (step.lock_count) {
            append(out, "  LOCKS ");
            append_number(out, step.lock_count);
        }
        *out = 0;
        canvas.text(0, 100, line, 11);

        canvas.text(0, 112, "A    B    C    D", 7);
    }
