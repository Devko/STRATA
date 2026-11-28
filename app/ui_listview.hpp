// The screens that were still text lists after the redesign (#174, #175), in the drafts'
// language (docs/SCREENS.md, "The other screens"): RESAMPLE with its meter, take bar and the
// take drawn as it is recorded; SAMPLES and SOUNDS with a preview under a shorter list; CHAIN
// as blocks on a timeline; KITS with each slot's machines as pictures; KEYS with the held notes
// and the knobs; PATTERN's track swing as bars.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, like the other ui_*.hpp parts: no #pragma once and no includes, and it sees every
// member the class has.

    // A caption at y 18 with something at its right, the right end at x 125. Returns the x the
    // left text may run to (4 px clear of the right one).
    int caption_right(uint8_t *px, const char *text, int grey) {
        if (text == nullptr || text[0] == 0) return 125;
        const int w = paint::small_width(text);
        paint::small_text(px, 125 - w, 18, text, grey);
        return 125 - w - 4;
    }

    // --- RESAMPLE ----------------------------------------------------------------------------
    //
    // Rows: where the take goes (18, its state at the right), where it comes from and how long
    // (25), the source's level as a segmented meter 31-37 with its dB at the right and the scale
    // at 40, the take 46-88 (drawn as it is recorded, column by column, from the source's level;
    // before a take the source's last six seconds roll by dim), the take's length as a bar of
    // its bars 92-95, the last take or what went wrong at 99, the keys at 105 and 113.

    static constexpr int kTakeCols = 120;
    static constexpr int kTakeMid = 67;
    static constexpr int kTakeAmp = 20;

    // The level the take is made of: the input's, one track's, or the master's.
    uint32_t take_source_level() const {
        const Status &status = engine_.status();
        const TakeSource &source = kTakeSources[take_source_];
        if (source.input >= 0) return status.input_peak.load(std::memory_order_relaxed);
        if (source.track >= 0) return status.track_milli[source.track].load(std::memory_order_relaxed);
        return status.meter_milli.load(std::memory_order_relaxed);
    }

    // Once a frame on RESAMPLE: the rolling history moves on a column, and a take being recorded
    // fills its columns up to where it is. Both from the level the audio side publishes, so a
    // silent source draws a flat line and a stopped screen holds still.
    void update_take_picture(uint8_t state, uint32_t frames, uint32_t take_frames) {
        const uint32_t level = take_source_level();
        const int rows = paint::level_rows(level, kTakeAmp, 48);
        // The history rolls while the beat plays or the reading moves; a stopped source whose
        // reading stands still (silence) holds it still.
        const bool running = engine_.status().running.load(std::memory_order_relaxed) != 0;
        const bool moved = level != history_level_;
        history_level_ = level;
        if (running || moved) {
            for (int i = 0; i + 1 < kTakeCols; i++) level_history_[i] = level_history_[i + 1];
            level_history_[kTakeCols - 1] = uint8_t(rows);
        }
        if (state == 2u && take_seen_state_ != 2u) {
            for (int i = 0; i < kTakeCols; i++) take_wave_[i] = 0;
            take_wave_cols_ = 0;
        }
        take_seen_state_ = state;
        if (state != 2u || take_frames == 0u) return;
        int col = int(uint64_t(frames) * kTakeCols / take_frames);
        if (col > kTakeCols) col = kTakeCols;
        // Every column since the last frame takes this frame's level: a short take moves several
        // columns a frame, and a gap would read as silence.
        for (int c = take_wave_cols_ > 0 ? take_wave_cols_ - 1 : 0; c < col; c++) {
            if (rows > take_wave_[c] || c >= take_wave_cols_) take_wave_[c] = uint8_t(rows);
        }
        if (col > take_wave_cols_) take_wave_cols_ = col;
    }

    void draw_resample(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Status &status = engine_.status();
        const uint8_t state = status.resample_state.load(std::memory_order_relaxed);
        const uint32_t frames = status.resample_frames.load(std::memory_order_relaxed);
        const int bars = int(kTakeBars[take_bars_index_]);
        // A bar's frames at the tempo: 48000 x 60 / 96 frames a beat's tick over the BPM.
        const float tempo = enaga::clock::state().tempo;
        const float tick_frames = tempo > 1.0f ? 30000.0f / tempo : 230.77f;
        const uint32_t take_frames = uint32_t(float(bars) * float(kTicksPerBar) * tick_frames);
        update_take_picture(state, frames, take_frames);

        // Where it goes, and what the take is doing at the right.
        out = line;
        switch (state) {
            case 1u: append(out, "ARMED"); break;
            case 2u: {
                append(out, "REC ");
                const uint32_t percent = take_frames == 0u ? 0u : frames * 100u / take_frames;
                append_number(out, percent > 100u ? 100u : percent);
                *out++ = '%';
                break;
            }
            case 3u: append(out, "WRITING"); break;
            default: break;
        }
        *out = 0;
        int left_end = 125;
        if (line[0] != 0) {
            const int w = paint::small_width(line);
            if (state == 2u) {
                paint::small_badge(px, 124 - w, 18, line);
            } else {
                paint::small_text(px, 125 - w, 18, line, state == 1u && !queue_blink_on() ? 6 : 15);
            }
            left_end = 125 - w - 5;
        }
        out = line;
        if (take_offer_ >= 0 && state == 0u) {
            append(out, "PUT ON TRK ");
            append_number(out, uint32_t(take_offer_) + 1);
            append(out, " ");
            append(out, plays_files(take_offer_) ? machine_name(engine_.machine_id(take_offer_))
                                                 : "-");
        } else if (resample_track_ < 0) {
            append(out, "TO FILE");
        } else {
            append(out, "TO TRK ");
            append_number(out, uint32_t(resample_track_) + 1);
            append(out, " ");
            append(out, machine_name(engine_.machine_id(resample_track_)));
        }
        *out = 0;
        const int fits = (left_end + 1) / paint::kSmallAdvance;    // never under the state
        if (fits >= 0 && fits < int(sizeof line)) line[fits] = 0;
        paint::small_text(px, 0, 18, line, 15);

        // Where it comes from (B), the input's level (C) and how long (A).
        const TakeSource &source = kTakeSources[take_source_];
        paint::small_text(px, 0, 25, "B", 6);
        int x = paint::small_text(px, 6, 25, source.name, 13);
        if (source.input >= 0) {
            paint::small_text(px, x + 5, 25, "C", 6);
            paint::small_text(px, x + 11, 25, kInputLevelNames[take_level_], 13);
        }
        out = line;
        append_number(out, uint32_t(bars));
        append(out, " BAR");
        *out = 0;
        const int bars_w = paint::small_width(line);
        paint::small_text(px, 125 - bars_w, 25, line, 13);
        paint::small_text(px, 125 - bars_w - 4 - paint::small_width("A"), 25, "A", 6);

        // The meter: 26 segments of 4 px over the top 48 dB, the top quarter brightest; the
        // reading in dB at the right, or CLIP lit.
        const uint32_t level = take_source_level();
        constexpr int kSegments = 26;
        const int lit = paint::level_rows(level, kSegments, 48);
        for (int s = 0; s < kSegments; s++) {
            const bool on = s < lit;
            const uint8_t g = !on ? 2 : (s >= kSegments - 3 ? 15 : (s >= kSegments * 3 / 4 ? 12 : 9));
            canvas.rect(s * 4, 31, 3, 7, g);
        }
        if (level >= 990u) {
            paint::small_badge(px, 107, 32, "CLIP");
        } else {
            out = line;
            const int db = paint::level_db_q8(level) / 256;
            if (db <= -paint::kDbFloor) {
                append(out, "-INF");
            } else {
                if (db < 0) *out++ = '-';
                append_number(out, uint32_t(db < 0 ? -db : db));
            }
            *out = 0;
            paint::small_text(px, 125 - paint::small_width(line), 32, line, level == 0u ? 5 : 12);
        }
        // The scale under it: where -48, -24, -12, -6 and 0 dB fall on the segments.
        paint::small_text(px, 0, 40, "-48", 5);
        paint::small_text(px, 50, 40, "-24", 5);
        paint::small_text(px, 74, 40, "-12", 5);
        paint::small_text(px, 88, 40, "-6", 5);
        paint::small_text(px, 100, 40, "0", 5);

        // The take: bar lines dotted, the centre dotted, and the columns as they were recorded
        // (bright while recording, grey once written). No take yet: the source rolls by, dim.
        const int top = kTakeMid - kTakeAmp - 1;
        const int bottom = kTakeMid + kTakeAmp + 1;
        paint::dotted_h(px, 0, kTakeCols - 1, kTakeMid, 3, 3);
        for (int b = 1; b < bars; b++) paint::dotted_v(px, b * kTakeCols / bars, top, bottom, 4, 2);
        if (take_wave_cols_ > 0) {
            const bool live = state == 2u;
            for (int c = 0; c < take_wave_cols_ && c < kTakeCols; c++) {
                const int a = take_wave_[c];
                if (a > 0) canvas.rect(c, kTakeMid - a, 1, 2 * a + 1, live ? 13 : 8);
            }
            if (live && take_wave_cols_ < kTakeCols) {
                canvas.rect(take_wave_cols_, top, 1, bottom - top + 1, 15);
            }
        } else {
            for (int c = 0; c < kTakeCols; c++) {
                const int a = level_history_[c];
                if (a > 0) canvas.rect(c, kTakeMid - a, 1, 2 * a + 1, 5);
            }
            if (state == 1u) {
                const char *wait = "STARTS ON THE NEXT BAR";
                const int w = paint::small_width(wait);
                canvas.rect(60 - w / 2 - 2, kTakeMid - 4, w + 4, 9, 0);
                paint::small_text(px, 60 - w / 2, kTakeMid - 2, wait, 15);
            }
        }

        // The take's length: one block a bar, filled as far as the take has got.
        constexpr int kBarY = 92;
        int filled = 0;
        if (state == 2u && take_frames > 0u) {
            filled = int(uint64_t(frames > take_frames ? take_frames : frames) * kTakeCols / take_frames);
        } else if (state == 3u || (state == 0u && take_wave_cols_ >= kTakeCols)) {
            filled = kTakeCols;
        }
        for (int b = 0; b < bars; b++) {
            const int x0 = b * kTakeCols / bars;
            const int x1 = (b + 1) * kTakeCols / bars - 2;
            canvas.rect(x0, kBarY, x1 - x0 + 1, 4, 3);
            const int f = filled > x1 + 1 ? x1 + 1 : filled;
            if (f > x0) canvas.rect(x0, kBarY, f - x0, 4, state == 2u ? 15 : 9);
        }

        // The last take, the clamp, or what is wrong with the input.
        const char *why = nullptr;
        if (source.input >= 0 && input_result_ != 0) {
            why = "INPUT REFUSED";
        } else if (source.input == ENAGA_INPUT_MIC && !enaga::system::headphones()) {
            // Enaga keeps the built-in mic silent without headphones, against feedback.
            why = "MIC NEEDS HEADPHONES";
        }
        out = line;
        if (why != nullptr) {
            append(out, why);
        } else if (status.resample_clamped.load(std::memory_order_relaxed) != 0u) {
            append(out, "SHORTENED TO 30S");
        } else if (storage_.last_take_name()[0] != 0) {
            append(out, "LAST ");
            append_shown(out, storage_.last_take_name(), 26);
        }
        *out = 0;
        if (line[0] != 0) paint::small_text(px, 0, 99, line, why != nullptr ? 15 : 9);
        out = line;
        append_tenths(out, uint32_t(float(take_frames) * (10.0f / 48000.0f) + 0.5f));
        *out++ = 'S';
        *out = 0;
        paint::small_text(px, 125 - paint::small_width(line), 99, line, 9);

        paint::small_text(px, 0, 105,
                          state == 0u ? (take_offer_ >= 0 ? "OK PUTS  CLR KEEPS" : "OK ARMS  CLR CANCELS")
                                      : "CLR CANCELS",
                          10);
        // JAM REC, FUNC + REC again here: how long it has run, at the right of the last line, and
        // the key that stops it in place of the arrows' hint; JAM OFF, dim, when none runs.
        if (storage_.jam_running()) {
            paint::small_text(px, 0, 113, "FUNC+REC: STOP JAM", 10);
            out = line;
            append_clock(out, storage_.jam_seconds());
            *out = 0;
            paint::small_badge(px, 124 - paint::small_width(line), 113, line);
        } else {
            paint::small_text(px, 0, 113, "LEFT RIGHT: WHERE TO", 8);
            paint::small_text(px, 125 - paint::small_width("JAM OFF"), 113, "JAM OFF", 6);
        }
    }

    // --- SAMPLES -----------------------------------------------------------------------------
    //
    // Rows: the folder and where the cursor is (18), six entries in Enaga's font (26-71), the
    // preview 76-110: what is heard (the file, its time and rate, how far through, as a tape
    // running under a playhead), else what the track plays (its file's picture with the window
    // and the voices, its length and rate), and the keys at 113.

    static constexpr int kSampleListY = 26;

    void draw_samples(enaga::Canvas &canvas) {
        settle_sample_cursor();
        uint8_t *px = canvas.data();
        const SampleList &list = storage_.browse_list();
        char line[40];
        char *out = line;
        const int count = list.count();

        // Where the cursor is in the folder, at the right; the folder, or what is happening.
        out = line;
        if (count > 0) {
            append_number(out, uint32_t(sample_cursor_ + 1));
            *out++ = '/';
            append_number(out, uint32_t(count));
            if (list.skipped() > 0) {
                append(out, " +");
                append_number(out, uint32_t(list.skipped()));
            }
        }
        *out = 0;
        const int room = caption_right(px, line, 10);
        out = line;
        const char *busy = storage_.busy_text();
        const int chars = (room + 1) / paint::kSmallAdvance;
        if (storage_.browsing()) {
            append(out, "READING FOLDER");
        } else if (storage_.browse_error() != nullptr) {
            append_shown(out, storage_.browse_error(), chars);
        } else if (busy != nullptr && storage_.loading_track() == selected_) {
            append_shown(out, busy, chars);
        } else if (list.at_root()) {
            append(out, "CARD");
        } else {
            // The folder's tail: its end is the part that tells two folders apart.
            const char *folder = list.folder();
            int n = 0;
            while (folder[n] != '\0') n++;
            append_shown(out, n > chars ? folder + (n - chars) : folder, chars);
        }
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);

        const int first = (sample_cursor_ / kSampleListRows) * kSampleListRows;
        if (count == 0 && !storage_.browsing()) {
            canvas.text(0, kSampleListY, list.skipped() > 0 ? "NOTHING THAT FITS" : "NO WAV FILES HERE", 9);
        }
        for (int i = 0; i < kSampleListRows && first + i < count; i++) {
            const int at = first + i;
            out = line;
            *out++ = at == sample_cursor_ ? '>' : ' ';
            const bool folder = list.is_folder(at);
            append_shown(out, list.name(at), kLineChars - 2);
            if (folder) *out++ = '/';
            *out = 0;
            const int y = kSampleListY + i * 8;
            list_row(canvas, 0, y, line, at == sample_cursor_, folder ? 11 : 9);
            // The entry being heard: a mark at the row's right.
            if (at == audition_row_ && storage_.auditioning() && at != sample_cursor_) {
                canvas.rect(121, y + 1, 3, 5, 13);
            }
        }

        // The preview.
        constexpr int kTop = 76;
        paint::dotted_h(px, 0, 123, kTop - 2, 3, 2);
        if (storage_.auditioning()) {
            draw_heard_preview(canvas, kTop);
        } else {
            draw_track_file_preview(canvas, kTop);
        }

        paint::small_text(px, 0, 113, storage_.auditioning() ? "A STOP  OK LOAD  CLR UP" : "A HEAR  OK LOAD  CLR UP",
                          10);
    }

    // What is heard: the name lit, the time and rate at the right, and the file as a tape of
    // seconds running under a playhead, the heard part lit.
    void draw_heard_preview(enaga::Canvas &canvas, int top) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const bool sounding = storage_.audition_sounding();
        out = line;
        if (sounding) {
            append_number(out, storage_.audition_elapsed());
            *out++ = '/';
            append_number(out, storage_.audition_seconds());
            *out++ = 'S';
            const uint32_t rate = storage_.audition_rate();
            if (rate > 0u) {
                *out++ = ' ';
                append_tenths(out, (rate + 50u) / 100u);
                *out++ = 'K';
            }
        } else {
            append(out, "OPENING");
        }
        *out = 0;
        const int right_w = paint::small_width(line);
        paint::small_text(px, 125 - right_w, top, line, 12);
        const int x = paint::small_badge(px, 0, top, "HEAR");
        out = line;
        const int chars = (125 - right_w - 4 - (x + 3)) / paint::kSmallAdvance;
        append_shown(out, SampleList::name_of(storage_.audition_path()), chars);
        *out = 0;
        paint::small_text(px, x + 3, top, line, 15);

        // The tape: a tick a second (every fifth taller), the heard part lit, the head bright.
        constexpr int kY = 88, kH = 12;
        const uint32_t total = storage_.audition_frames();
        const uint32_t now = engine_.status().audition_frame.load(std::memory_order_relaxed);
        const int head = !sounding || total == 0u ? 0 : int(uint64_t(now > total ? total : now) * 119u / total);
        canvas.rect(0, kY, 120, kH, 2);
        if (head > 0) canvas.rect(0, kY, head, kH, 7);
        const uint32_t seconds = storage_.audition_seconds();
        if (seconds > 0u && seconds < 120u) {
            for (uint32_t s = 1; s < seconds; s++) {
                const int tx = int(s * 119u / seconds);
                const int len = s % 5u == 0u ? 5 : 2;
                canvas.rect(tx, kY + kH - len, 1, len, tx < head ? 2 : 6);
            }
        }
        canvas.rect(head, kY - 3, 1, kH + 6, 15);
        paint::dot(px, head, kY - 3, 15);
    }

    // What the track plays: its file's picture with START..END bright and the voices, the name,
    // and its length and rate.
    void draw_track_file_preview(enaga::Canvas &canvas, int top) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        out = line;
        if (storage_.has_overview(selected_)) {
            append_number(out, storage_.file_ms(selected_) / 1000u);
            *out++ = '.';
            append_padded(out, storage_.file_ms(selected_) % 1000u / 10u, 2);
            *out++ = 'S';
            const uint32_t rate = storage_.file_rate(selected_);
            if (rate > 0u) {
                *out++ = ' ';
                append_tenths(out, (rate + 50u) / 100u);
                *out++ = 'K';
            }
        }
        *out = 0;
        const int right_w = line[0] != 0 ? paint::small_width(line) : 0;
        if (right_w > 0) paint::small_text(px, 125 - right_w, top, line, 12);
        out = line;
        *out++ = 'T';
        append_number(out, uint32_t(selected_) + 1);
        *out = 0;
        const int x = paint::small_text(px, 0, top, line, 6);
        out = line;
        const char *current = engine_.project().kit.tracks[selected_].sample;
        const int chars = (125 - right_w - 4 - (x + 3)) / paint::kSmallAdvance;
        append_shown(out, current[0] != '\0' ? SampleList::name_of(current) : "LOOP", chars);
        *out = 0;
        paint::small_text(px, x + 3, top, line, 15);

        constexpr int kMid = 95, kAmp = 10;
        const WaveView view = engine_.status().wave[selected_].read();
        if (!plays_files(selected_)) {
            draw_overview_band(canvas, kMid, kAmp, kWaveColumns, kWaveColumns, 6, 6);
            return;
        }
        draw_overview_band(canvas, kMid, kAmp, view.lo, view.hi, 10, 4);
        for (int i = 0; i < WaveView::kMarks; i++) {
            if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
            const int g = view.glow[i] > 15 ? 15 : view.glow[i];
            canvas.rect(view.mark[i], kMid - kAmp - 1, 1, 2 * kAmp + 3, uint8_t(g));
        }
        if (view.lo < kWaveColumns && view.hi < kWaveColumns) {
            canvas.rect(view.lo, kMid - kAmp - 2, 1, 2 * kAmp + 5, 15);
            canvas.rect(view.hi, kMid - kAmp - 2, 1, 2 * kAmp + 5, 15);
        }
        if (!storage_.has_overview(selected_)) {
            const char *why = storage_.loading_track() == selected_ ? "LOADING" : "NO FILE";
            const int w = paint::small_width(why);
            canvas.rect(60 - w / 2 - 2, kMid - 4, w + 4, 9, 0);
            paint::small_text(px, 60 - w / 2, kMid - 2, why, 15);
        }
    }

    // --- SOUNDS ------------------------------------------------------------------------------
    //
    // Rows: which track it goes into (18, the cursor's place at the right), six presets in
    // Enaga's font (26-71), the preset under the cursor as bars (76-111): its twenty knobs by
    // page, SRC FLT AMP ASR OSC, each as tall as the preset sets it with the track's own value
    // as a tick, so what a preset changes reads at a glance; the keys at 113.

    static constexpr int kSoundRows = 6;

    void draw_sounds(enaga::Canvas &canvas) {
        if (sounds_arps_) {
            draw_arps(canvas);
            return;
        }
        if (name_entry_.active()) {
            draw_name_entry(canvas);
            return;
        }
        // The machine's own sounds, read from the card the first time the list shows them.
        storage_.want_sound_bank(engine_.machine_id(selected_));
        uint8_t *px = canvas.data();
        const uint8_t machine = engine_.machine_id(selected_);
        char line[40];
        char *out = line;
        const int rows = sound_rows();

        out = line;
        append_number(out, uint32_t(sound_cursor_ + 1));
        *out++ = '/';
        append_number(out, uint32_t(rows));
        *out = 0;
        caption_right(px, line, 10);
        out = line;
        if (sound_delete_armed_ >= 0) {
            append(out, sound_delete_oks_ > 0 ? "OK AGAIN: DELETE" : "OK TWICE: DELETE");
        } else if (storage_.sound_bank_state(machine) == Storage::SoundBankState::failed) {
            append(out, "SOUND FILE: ");
            append_shown(out, storage_.sound_bank_error() != nullptr ? storage_.sound_bank_error() : "BAD", 8);
        } else {
            append(out, "INTO T");
            append_number(out, uint32_t(selected_) + 1);
            append(out, " ");
            append(out, machine_name(machine));
            append(out, " ONLY");
        }
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);

        // The VAs' lists are longer than the screen: it scrolls to keep the cursor on it.
        const uint8_t *now = engine_.params(selected_).v;
        const int first = sound_cursor_ >= kSoundRows ? sound_cursor_ - kSoundRows + 1 : 0;
        for (int row = first; row < rows && row < first + kSoundRows; row++) {
            out = line;
            const bool on = row == sound_cursor_;
            *out++ = on ? '>' : ' ';
            int index = 0;
            const SoundRow kind = sound_row_kind(row, &index);
            const char *name = "AS WAS";
            bool matches = false;
            int width = kSoundNameChars;
            if (kind == SoundRow::paste) {
                append(out, "PASTE T");
                append_number(out, uint32_t(sound_clip_.track) + 1);
                append(out, " ");
                name = machine_name(sound_clip_.machine);
            } else if (kind == SoundRow::factory) {
                name = sound_preset(machine, index)->name;
                matches = sound_preset_matches(machine, index, now);
            } else if (kind == SoundRow::user) {
                // Your own: a '+' in front, so they read apart from the factory's.
                const UserSound &sound = storage_.sound_bank().slots[index];
                *out++ = '+';
                name = sound.name;
                matches = user_sound_matches(machine, sound, now, user_sound_extras_of(engine_.project().kit.tracks[selected_]));
                width = kUserSoundNameChars;
            }
            append(out, name);
            if (matches) {
                int length = 0;
                while (name[length] != '\0') length++;
                for (int pad = width - length; pad > 0; pad--) *out++ = ' ';
                append(out, "  NOW");
            }
            *out = 0;
            list_row(canvas, 0, 26 + (row - first) * 8, line, on, 9);
        }
        if (rows == 1) {
            canvas.text(0, 34, "NO SOUNDS HERE", 9);
        } else {
            draw_sound_bars(canvas, machine, sound_cursor_);
        }
        paint::small_text(px, 0, 113, "OK LOAD  F+DOWN SAVE", 10);
    }

    // ARPS: the same rows over the factory arps (core/arppresets.hpp), and under them the arp
    // under the cursor in words: MOD RAT OCT GAT on one line, the kit settings on the next.
    void draw_arps(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const int rows = sound_rows();
        out = line;
        append_number(out, uint32_t(sound_cursor_ + 1));
        *out++ = '/';
        append_number(out, uint32_t(rows));
        *out = 0;
        caption_right(px, line, 10);
        out = line;
        append(out, "INTO T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ARP ONLY");
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);

        const int first = sound_cursor_ >= kSoundRows ? sound_cursor_ - kSoundRows + 1 : 0;
        for (int row = first; row < rows && row < first + kSoundRows; row++) {
            out = line;
            const bool on = row == sound_cursor_;
            *out++ = on ? '>' : ' ';
            if (row == 0) {
                append(out, "AS WAS");
            } else {
                const ArpPreset *a = arp_preset(row - 1);
                append(out, a->name);
                if (arp_preset_now(row - 1)) {
                    int length = 0;
                    while (a->name[length] != '\0') length++;
                    for (int pad = kArpPresetNameChars - length; pad > 0; pad--) *out++ = ' ';
                    append(out, "  NOW");
                }
            }
            *out = 0;
            list_row(canvas, 0, 26 + (row - first) * 8, line, on, 9);
        }

        // The cursor's arp, or the track's own for AS WAS.
        const ArpPreset *a = sound_cursor_ == 0 ? nullptr : arp_preset(sound_cursor_ - 1);
        const uint8_t *bytes = a != nullptr ? a->bytes : &engine_.params(selected_).v[kArpFirst];
        const KitArp ext = a != nullptr ? a->ext : engine_.project().kit.tracks[selected_].arp;
        paint::small_text(px, 0, 77, a != nullptr ? a->name : "AS WAS", 15);
        const char *note = a == nullptr ? "THE ARP BEFORE" : (arp_preset_now(sound_cursor_ - 1) ? "PLAYING NOW" : "");
        paint::small_text(px, 125 - paint::small_width(note), 77, note, 7);
        out = line;
        append(out, arp_mode_name(bytes[0]));
        append(out, " ");
        append(out, arp_rate_name(bytes[1]));
        append(out, " X");
        append_number(out, uint32_t(arp_octaves(bytes[2])));
        append(out, " GAT ");
        append_number(out, uint32_t(arp_gate_percent(bytes[3])));
        *out = 0;
        paint::small_text(px, 0, 87, line, 11);
        for (int f = 0; f < kArpExtFields; f++) {
            const int x = (f % 2) * 64;
            const int y = 96 + (f / 2) * 8;
            out = line;
            append(out, arp_ext_label(f));
            append(out, " ");
            append(out, arp_ext_name(f, arp_ext_get(ext, f)));
            *out = 0;
            paint::small_text(px, x, y, line, arp_ext_get(ext, f) == 0 ? 6 : 11);
        }
        paint::small_text(px, 0, 113, "OK LOAD  CLR BACK", 10);
    }

    // The row's knobs as bars, five pages of four; a page the machine does not read dotted.
    // AS WAS: the sound the track had before the browser, drawn from the track. A factory sound,
    // one of your own and the copied one draw alike, from their bytes.
    void draw_sound_bars(enaga::Canvas &canvas, uint8_t machine, int row) {
        uint8_t *px = canvas.data();
        constexpr int kBottom = 104, kTall = 24;
        static const char *const kGroups[5] = {"SRC", "FLT", "AMP", "ASR", "OSC"};
        const int firsts[5] = {0, 4, 8, kSoundAsrFirst, kSoundOscFirst};
        const uint8_t *values[5] = {};
        const uint8_t *now = engine_.params(selected_).v;
        int index = 0;
        const SoundRow kind = sound_row_kind(row, &index);
        const char *title = "AS WAS";
        bool playing = false;
        if (kind == SoundRow::factory) {
            const SoundPreset *preset = sound_preset(machine, index);
            values[0] = preset->src;
            values[1] = preset->fltr;
            values[2] = preset->amp;
            values[3] = preset->asr;
            values[4] = preset->osc;
            title = preset->name;
            playing = sound_preset_matches(machine, index, now);
        } else if (kind == SoundRow::user || kind == SoundRow::paste) {
            const uint8_t *bytes = kind == SoundRow::user ? storage_.sound_bank().slots[index].params
                                                          : sound_clip_.params;
            for (int g = 0; g < 5; g++) values[g] = bytes + firsts[g];
            if (kind == SoundRow::user) {
                title = storage_.sound_bank().slots[index].name;
                playing = user_sound_matches(machine, storage_.sound_bank().slots[index], now,
                                             user_sound_extras_of(engine_.project().kit.tracks[selected_]));
            } else {
                title = "PASTE";
                // The copied sound draws in the machine it is for, which a paste switches to.
                machine = sound_clip_.machine;
            }
        }
        const bool before = kind == SoundRow::as_was;
        paint::dotted_h(px, 0, 123, 74, 3, 2);
        for (int g = 0; g < 5; g++) {
            const int gx = g * 25;
            const bool reads = g == 3 ? machine_has_asr_page(machine) : (g == 4 ? machine_reads_osc(machine) : true);
            for (int k = 0; k < 4; k++) {
                const int x = gx + k * 5;
                if (!reads) {
                    paint::dotted_v(px, x + 1, kBottom - kTall, kBottom, 2, 3);
                    continue;
                }
                const int current = now[firsts[g] + k];
                const int value = values[g] != nullptr ? values[g][k] : current;
                const int h = 1 + value * (kTall - 1) / 127;
                canvas.rect(x, kBottom - h + 1, 4, h, before ? 8 : 13);
                const int t = kBottom - current * (kTall - 1) / 127;
                if (current != value) canvas.rect(x, t, 4, 1, uint8_t(current > value ? 6 : 0));
            }
            paint::small_text(px, gx + 4, 106, kGroups[g], reads ? 8 : 3);
        }
        paint::small_text(px, 0, 77, title, 15);
        const char *note = before    ? "THE SOUND BEFORE"
                           : playing ? "PLAYING NOW"
                           : kind == SoundRow::paste && sound_clip_.machine != engine_.machine_id(selected_)
                               ? "SWITCHES MACHINE"
                               : "TICKS: THE TRACK NOW";
        paint::small_text(px, 125 - paint::small_width(note), 77, note, 7);
    }

    // --- CHAIN -------------------------------------------------------------------------------
    //
    // Rows: how many entries and SONG or LOOP (18; ON lit while the chain plays), the whole song
    // as a strip (24-26), the song as blocks on a lane (31-60: each entry as long as its passes,
    // its pattern and repeats in it, the passes dotted, the cursor's framed bright, the playing
    // one filled as far as it has got with the playhead), the cursor's entry (66: number,
    // pattern, repeats; its pattern's steps; the mute state it recalls as eight boxes), the keys
    // at 105 and 113.

    static constexpr int kLaneY = 31;
    static constexpr int kLaneH = 30;

    // One pass of an entry's pattern in pixels on the lane: longer patterns longer.
    int chain_pass_width(const ChainEntry &entry) const {
        const Pattern &p = engine_.project().patterns[entry.bank][entry.slot];
        const int steps = p.master_length < 1 ? 16 : int(p.master_length);
        return 6 + steps / 4;
    }

    int chain_block_width(const ChainEntry &entry) const {
        const int w = chain_pass_width(entry) * int(entry.repeats);
        return w < 15 ? 15 : (w > 120 ? 120 : w);
    }

    // How far the chain's playing entry is, in 1/256 of the entry.
    int chain_play_fraction(const ChainEntry &entry) const {
        const ChainPlayer &player = engine_.chain();
        const Status &status = engine_.status();
        const uint32_t into = status.pattern_into.load(std::memory_order_relaxed);
        const uint32_t loop = status.pattern_loop.load(std::memory_order_relaxed);
        const int repeats = entry.repeats < 1 ? 1 : int(entry.repeats);
        const int pass = loop == 0u ? 0 : int(uint64_t(into > loop ? loop : into) * 256u / loop);
        int played = int(player.played);
        if (played >= repeats) played = repeats - 1;
        return (played * 256 + pass) / repeats;
    }

    void draw_chain(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Project &project = engine_.project();
        const int length = chain_length(project);
        const ChainPlayer &player = engine_.chain();
        const bool playing_chain = player.active && !player.orphaned;
        const bool running = engine_.status().running.load(std::memory_order_relaxed) != 0;

        // SONG or LOOP, and whether it plays.
        if (player.active) {
            paint::small_badge(px, 124 - paint::small_width("ON"), 18, "ON");
        } else {
            paint::small_text(px, 125 - paint::small_width("OFF"), 18, "OFF", 6);
        }
        out = line;
        append_number(out, uint32_t(length));
        append(out, "/64 ");
        append(out, project.settings.chain_song ? "SONG" : "LOOP");
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);

        if (length == 0) {
            paint::dotted_h(px, 0, 123, kLaneY, 4, 2);
            paint::dotted_h(px, 0, 123, kLaneY + kLaneH - 1, 4, 2);
            canvas.text(0, kLaneY + 11, "EMPTY", 11);
            paint::small_text(px, 0, 72, "OK ADDS THE PLAYING PATTERN", 9);
            paint::small_text(px, 0, 80, "STEP KEYS ADD FROM THIS BANK", 9);
            paint::small_text(px, 0, 113, "A:REPEATS B:MUTES", 10);
            return;
        }

        // The whole song on one strip, as long as each entry's passes.
        int total = 0;
        for (int i = 0; i < length; i++) total += chain_block_width(project.chain[i]);
        {
            int acc = 0;
            for (int i = 0; i < length; i++) {
                const int x0 = acc * 124 / total;
                acc += chain_block_width(project.chain[i]);
                const int x1 = acc * 124 / total - 1;
                const bool here = i == chain_cursor_;
                const bool now = playing_chain && int(player.entry) == i;
                canvas.rect(x0, 24, x1 - x0 < 1 ? 1 : x1 - x0, 3, uint8_t(now ? 15 : (here ? 10 : 4)));
            }
        }

        // The lane: from the entry that keeps the cursor's block on screen, a third in.
        int first = chain_cursor_;
        {
            int room = 124 - chain_block_width(project.chain[chain_cursor_]);
            int lead = 40;
            while (first > 0) {
                const int w = chain_block_width(project.chain[first - 1]) + 2;
                if (w > room || w > lead + 30) break;
                room -= w;
                lead -= w;
                first--;
                if (lead <= 0) break;
            }
        }
        int x = 0;
        for (int i = first; i < length && x < 124; i++) {
            const ChainEntry &entry = project.chain[i];
            const int w = chain_block_width(entry);
            const bool here = i == chain_cursor_;
            const bool now = playing_chain && int(player.entry) == i;
            const int right = x + w - 1 > 123 ? 123 : x + w - 1;
            const int bw = right - x + 1;
            canvas.rect(x, kLaneY, bw, kLaneH, uint8_t(here ? 4 : 2));
            if (now) {
                const int head = x + chain_play_fraction(entry) * w / 256;
                if (running && head > x) canvas.rect(x, kLaneY, (head > right ? right : head) - x, kLaneH, 6);
                if (head <= right) {
                    canvas.rect(head, kLaneY - 2, 1, kLaneH + 4, 15);
                    paint::dot(px, head, kLaneY - 2, 15);
                }
            }
            // Each pass after the first, dotted.
            const int pass_w = w / (entry.repeats < 1 ? 1 : entry.repeats);
            for (int r = 1; r < entry.repeats && x + r * pass_w < right; r++) {
                paint::dotted_v(px, x + r * pass_w, kLaneY + 1, kLaneY + kLaneH - 2, 8, 2);
            }
            frame_rect(canvas, x, kLaneY, bw, kLaneH, here ? 15 : (now ? 11 : 6));
            char name[12];
            out = name;
            *out++ = bank_letter(int(entry.bank));
            append_padded(out, uint32_t(entry.slot) + 1, 2);
            *out = 0;
            if (bw >= 14) {
                if (now) {
                    paint::small_badge(px, x + 2, kLaneY + 3, name);
                } else {
                    paint::small_text(px, x + 2, kLaneY + 3, name, here ? 15 : 11);
                }
                out = name;
                *out++ = 'X';
                append_number(out, uint32_t(entry.repeats));
                *out = 0;
                paint::small_text(px, x + 2, kLaneY + 11, name, here ? 13 : 8);
                // The mute state it sets, and `+` when the section has fields of its own.
                const SectionExtra &extra = project.chain_extra[i];
                out = name;
                if (extra.cap != 0) {
                    *out++ = 'C';
                } else if (entry.mutes != 0) {
                    *out++ = 'M';
                    append_number(out, uint32_t(entry.mutes));
                }
                SectionExtra rest = extra;
                rest.cap = 0;
                if (!section_empty(rest)) *out++ = '+';
                *out = 0;
                if (name[0] != 0) paint::small_text(px, x + 2, kLaneY + 19, name, here ? 13 : 8);
            }
            x += w + 2;
        }
        if (x < 124) {
            // Nothing after the last: the song's end, a double bar.
            const int end = x - 2;
            if (end + 3 < 124) {
                canvas.rect(end + 1, kLaneY, 1, kLaneH, 9);
                canvas.rect(end + 3, kLaneY, 1, kLaneH, 9);
            }
        }

        // The cursor's entry: its place, pattern and repeats; its steps; the mutes it recalls.
        const ChainEntry &entry = project.chain[chain_cursor_];
        out = line;
        append_padded(out, uint32_t(chain_cursor_) + 1, 2);
        *out = 0;
        paint::small_badge(px, 0, 67, line);
        out = line;
        *out++ = bank_letter(int(entry.bank));
        append_padded(out, uint32_t(entry.slot) + 1, 2);
        append(out, " X");
        append_number(out, uint32_t(entry.repeats));
        *out = 0;
        canvas.text(14, 66, line, 15);
        refresh_chain_thumb(entry);
        for (int c = 0; c < 16; c++) {
            for (int t = 0; t < kTracks; t++) {
                const bool on = (chain_thumb_[c] >> t) & 1u;
                canvas.rect(c * 3, 77 + t * 2, 2, 1, on ? 13 : (c % 4 == 0 ? 3 : 1));
            }
        }
        paint::small_text(px, 54, 77, "MUTES", 6);
        const SectionExtra &extra = project.chain_extra[chain_cursor_];
        if (entry.mutes == 0 && extra.cap == 0) {
            paint::small_text(px, 54, 85, "AS THEY ARE", 9);
        } else {
            const uint8_t mask = extra.cap != 0 ? extra.mask
                                                : project.settings.mute_states[entry.mutes - 1];
            out = line;
            if (extra.cap != 0) {
                append(out, "CAP");
            } else {
                *out++ = 'M';
                append_number(out, uint32_t(entry.mutes));
            }
            *out = 0;
            paint::small_text(px, 78, 77, line, 13);
            for (int t = 0; t < kTracks; t++) {
                const int bx = 54 + t * 9;
                if ((mask >> t) & 1u) {
                    frame_rect(canvas, bx, 85, 7, 7, 6);
                } else {
                    canvas.rect(bx, 85, 7, 7, 12);
                }
            }
        }

        // The section's own fields (app/ui_section.hpp), "S4 SNAP E+32 132BPM".
        section_summary(extra, line);
        if (line[0] != 0) paint::small_text(px, 0, 96, line, 12);

        paint::small_text(px, 0, 105, "OK ADD  CLR X2 REMOVE", 10);
        paint::small_text(px, 0, 113, "A:REPEATS PUSH A:SECT", 10);
    }

    // The cursor entry's steps, read once when the cursor's pattern changes (a pattern is a few
    // KiB: docs/SCREENS.md's rule against walking patterns in draw()). An edit to that pattern
    // shows the next time the cursor comes to it.
    void refresh_chain_thumb(const ChainEntry &entry) {
        const int key = int(entry.bank) * kPatternsPerBank + int(entry.slot);
        if (key == chain_thumb_key_) return;
        chain_thumb_key_ = key;
        pattern_columns(engine_.project().patterns[entry.bank][entry.slot], chain_thumb_);
    }

    // --- KITS --------------------------------------------------------------------------------
    //
    // Rows: the page (18; what the card is doing at the right), eight slots 11 px apart from 25:
    // each its number and its eight machines as MACHINE's pictures; the cursor's row lit with the
    // machines' names; an empty slot its number and a dotted line; the keys at 113.

    // KITS draws up to 64 machine pictures a frame; drawn from their shapes that would cost
    // more than MACHINE's whole screen. They never change, so each is drawn once, at 12 x 6 in
    // KITS' grey, into a strip off the canvas (with room for the lines' anti-aliased edges) and
    // kept as 14 x 8 greys; KITS lays the copies down, brighter winning.
    static constexpr int kIconW = 14, kIconH = 8;

    void build_kit_icons() {
        if (kit_icons_built_) return;
        kit_icons_built_ = true;
        for (int m = 0; m < int(MachineId::count); m++) {
            for (int i = 0; i < 128 * kIconStripRows; i++) icon_strip_[i] = 0;
            // At (1, 9) in a 24-row strip: nine rows of slack on either side of the 6, far more than
            // any picture strays (a line's edge, a burst of noise); paint clips only to 128 rows.
            machine_icon(icon_strip_, uint8_t(m), 1, 9, 12, 6, 10);
            for (int y = 0; y < kIconH; y++) {
                for (int x = 0; x < kIconW; x++) kit_icons_[m][y * kIconW + x] = icon_strip_[(y + 8) * 128 + x];
            }
        }
    }

    void blit_kit_icon(uint8_t *px, uint8_t machine, int x, int y) const {
        if (machine >= uint8_t(MachineId::count)) return;
        for (int j = 0; j < kIconH; j++) {
            for (int i = 0; i < kIconW; i++) {
                const uint8_t g = kit_icons_[machine][j * kIconW + i];
                if (g != 0) paint::plot_max(px, x - 1 + i, y - 1 + j, g);
            }
        }
    }

    void draw_kit(enaga::Canvas &canvas) {
        build_kit_icons();
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const int first = (kit_cursor_ / 8) * 8;

        out = line;
        const char *busy = storage_.busy_text();
        if (busy != nullptr) {
            append(out, busy);
        } else if (!storage_.card_usable()) {
            append(out, "NO CARD");
        } else if (storage_.last_error() != nullptr) {
            append_shown(out, storage_.last_error(), 18);
        }
        *out = 0;
        caption_right(px, line, busy != nullptr ? 15 : 12);
        out = line;
        append(out, "SLOTS ");
        append_padded(out, uint32_t(first + 1), 2);
        *out++ = '-';
        append_padded(out, uint32_t(first + 8), 2);
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);

        for (int i = 0; i < 8; i++) {
            const int slot = first + i;
            const Storage::KitSlot &info = storage_.kit_slot(slot);
            const int y = 25 + i * 11;
            const bool here = slot == kit_cursor_;
            char num[12];
            out = num;
            append_padded(out, uint32_t(slot + 1), 2);
            *out = 0;
            if (here) canvas.rect(0, y, 124, 10, 15);
            paint::small_text(px, 1, y + 3, num, here ? 0 : 10);
            switch (info.state) {
                case Storage::SlotState::used:
                    for (int t = 0; t < kTracks; t++) {
                        const int x = 12 + t * 14;
                        if (here) {
                            paint::small_text(px, x, y + 3, machine_name(info.machines[t]), 0);
                        } else {
                            blit_kit_icon(px, info.machines[t], x, y + 2);
                        }
                    }
                    break;
                case Storage::SlotState::empty:
                    if (here) {
                        paint::small_text(px, 12, y + 3, "EMPTY: FUNC+DOWN SAVES HERE", 0);
                    } else {
                        paint::dotted_h(px, 12, 120, y + 5, 3, 3);
                    }
                    break;
                case Storage::SlotState::bad:
                    paint::small_text(px, 12, y + 3, "UNREADABLE", here ? 0 : 12);
                    break;
                default:
                    paint::small_text(px, 12, y + 3, "...", here ? 0 : 6);
                    break;
            }
        }
        paint::small_text(px, 0, 113, "OK TWICE: LOAD  FUNC+DOWN: SAVE", 10);
    }

    // --- KEYS --------------------------------------------------------------------------------
    //
    // Rows: the track, the key and the octave (18), the pads as a keyboard (24-55: the upper
    // row's black keys over the lower row's white ones), the notes held and the chord they make
    // (60-68), the EDIT page's four knobs as gauges (75-104) under the page's name, which STEP
    // keys are steps (113).

    // The chord the held pitch classes make, from its root: a name, or null.
    static const char *chord_name(unsigned classes, int &root) {
        struct Shape { unsigned bits; const char *name; };
        static const Shape kShapes[] = {
            {0x891u, "MAJ7"}, {0x491u, "7"}, {0x489u, "MIN7"}, {0x449u, "M7B5"},
            {0x91u, "MAJ"}, {0x89u, "MIN"}, {0x49u, "DIM"}, {0x111u, "AUG"},
            {0xA1u, "SUS4"}, {0x85u, "SUS2"}, {0x81u, "5"},
        };
        for (const Shape &shape : kShapes) {
            for (int r = 0; r < 12; r++) {
                if (((classes >> r) & 1u) == 0) continue;
                const unsigned turned = ((classes >> r) | (classes << (12 - r))) & 0xFFFu;
                if (turned == shape.bits) {
                    root = r;
                    return shape.name;
                }
            }
        }
        return nullptr;
    }

    void draw_keys(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const uint8_t root = engine_.key_root();
        const int scale = int(engine_.key_scale());

        out = line;
        append(out, "OCT ");
        append_number(out, uint32_t(keys_octave_ + 3));
        *out = 0;
        caption_right(px, line, 13);
        out = line;
        append(out, note_name(root));
        *out++ = ' ';
        append(out, scale_name(scale));
        *out = 0;
        paint::small_text(px, 0, 18, "KEY", 6);
        paint::small_text(px, 15, 18, line, 13);

        // The pads as the keyboard they are, in the current octave: the upper row's black keys
        // between the lower row's white ones, the two octave pads as - and +. The root brightest
        // and the scale's other notes bright, as the pads light, and a held pad filled.
        for (int pad = 0; pad < kPianoPads; pad++) {
            const bool upper = (pad & 1) != 0;
            const int x = (pad / 2) * 16 + (upper ? 8 : 0);
            const int y = upper ? 25 : 41;
            const int note = pad_note(pad);
            out = line;
            if (note < 0) {
                append(out, kPianoSemitone[pad] == kPadOctaveDown ? "-" : "+");
            } else {
                append(out, note_name(uint8_t(note)));
            }
            *out = 0;
            const bool at_root = note >= 0 && note % 12 == root;
            const bool in_scale = note >= 0 && scale_fold(uint8_t(note), root, scale) == uint8_t(note);
            const bool held = pad_held_note_[pad] >= 0;
            canvas.rect(x + 1, y, 14, 14, held ? 15 : (at_root ? 6 : (in_scale ? 3 : 1)));
            canvas.text(x + 3, y + 4, line, held ? 0 : (in_scale ? 15 : 8));
        }

        // What is held, lowest first, and the chord it makes.
        unsigned classes = 0;
        int held_notes[kPianoPads] = {};
        int held = 0;
        for (int pad = 0; pad < kPianoPads; pad++) {
            if (pad_held_note_[pad] < 0) continue;
            const int n = pad_held_note_[pad];
            int at = held++;
            while (at > 0 && held_notes[at - 1] > n) {
                held_notes[at] = held_notes[at - 1];
                at--;
            }
            held_notes[at] = n;
            classes |= 1u << (n % 12);
        }
        paint::small_text(px, 0, 61, "HELD", 6);
        if (held == 0) {
            paint::small_text(px, 20, 61, "-", 5);
        } else {
            int x = 20;
            for (int i = 0; i < held && x < 100; i++) {
                out = line;
                append(out, note_name(uint8_t(held_notes[i])));
                const int octave = note_octave(uint8_t(held_notes[i]));
                append_number(out, uint32_t(octave < 0 ? 0 : octave));
                *out = 0;
                x = paint::small_text(px, x, 61, line, 15) + 4;
            }
            int chord_root = 0;
            const char *chord = held >= 2 ? chord_name(classes, chord_root) : nullptr;
            if (chord != nullptr) {
                out = line;
                append(out, note_name(uint8_t(chord_root)));
                *out++ = ' ';
                append(out, chord);
                *out = 0;
                const int w = paint::small_width(line);
                if (x <= 124 - w - 2) paint::small_badge(px, 124 - w, 61, line);
            }
        }

        // The knobs A-D turn here: the EDIT page's, as gauges, with the page named.
        paint::dotted_h(px, 0, 123, 69, 3, 2);
        const Page page = Page(param_page_);
        paint::small_text(px, 0, 72, page_name_on(page, engine_.machine_id(selected_)), 8);
        for (int slot = 0; slot < 4; slot++) {
            const int cx = 15 + slot * 31;
            const int index = edit_index(slot);
            const char *name = index < 0 ? "---" : param_name_for(index);
            const bool empty = index < 0 || name[0] == '-';
            char text[24];
            out = text;
            *out++ = char('A' + slot);
            *out++ = ' ';
            append(out, name);
            *out = 0;
            const int tw = paint::small_width(text);
            paint::small_text(px, cx - tw / 2, 100, text, empty ? 4 : 10);
            if (empty) {
                paint::arc(px, cx, 87, 9, kGaugeStart, kGaugeSpan, 2);
                continue;
            }
            const ShownParam shown = shown_param(index);
            draw_gauge(canvas, cx, 87, 9, shown.heard * 127 / shown.top, -1, true, false);
            out = text;
            append_param_value(out, index, shown.heard);
            *out = 0;
            const int vw = paint::small_width(text);
            paint::small_text(px, cx - vw / 2, 107, text, 15);
        }

        out = line;
        append(out, "STEP KEYS ");
        append_number(out, uint32_t(page_ * 16 + 1));
        *out++ = '-';
        append_number(out, uint32_t(page_ * 16 + 16));
        *out = 0;
        paint::small_text(px, 0, 113, line, 8);
    }

    // --- PATTERN -----------------------------------------------------------------------------
    //
    // Rows: the live pattern (18), LEN, SWG and FEEL as rows in Enaga's font (25, 34, 43), SWG
    // drawn at the right as a beat of sixteenths with the off-beats late by the swing, the
    // tracks' own swing as eight bars 56-102 (50% at the left, 75% at the right, the pattern's
    // swing a dotted line; a track that follows it dim with PAT), the keys at 113.

    // Where a swing of `swing` % falls on the bars' 50-75 scale, x 22-104.
    static int swing_x(int swing) {
        const int s = swing < 50 ? 50 : (swing > 75 ? 75 : swing);
        return 22 + (s - 50) * 82 / 25;
    }

    void draw_pattern_page(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Status &status = engine_.status();
        const Pattern &pattern = engine_.pattern();

        out = line;
        append(out, "PATTERN ");
        *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        append_padded(out, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);
        paint::small_text(px, 125 - paint::small_width("LEFT RIGHT PICK"), 18, "LEFT RIGHT PICK", 7);

        out = line;
        *out++ = pattern_field_ == 0 ? '>' : ' ';
        append(out, "LEN ");
        append_number(out, uint32_t(pattern.master_length));
        append(out, " STEPS");
        *out = 0;
        list_row(canvas, 0, 26, line, pattern_field_ == 0, 10);

        out = line;
        *out++ = pattern_field_ == 1 ? '>' : ' ';
        append(out, "SWG ");
        append_number(out, uint32_t(pattern.swing));
        *out++ = '%';
        *out = 0;
        list_row(canvas, 0, 35, line, pattern_field_ == 1, 10);
        // A beat of sixteenths: the on-beats where they are, the off-beats late by the swing.
        {
            const bool on = pattern_field_ == 1;
            constexpr int kX = 70, kStep = 12;
            for (int i = 0; i < 4; i++) {
                const int late = (i & 1) ? (int(pattern.swing) - 50) * 2 * kStep / 50 : 0;
                const int bx = kX + i * kStep + late;
                canvas.rect(bx, 35, 2, (i & 1) ? 4 : 6, on ? 0 : uint8_t((i & 1) ? 13 : 9));
            }
        }

        // FEEL: the drift over the swing, OFF or its mode and depth in ticks.
        out = line;
        *out++ = pattern_field_ == kPatternFieldFeel ? '>' : ' ';
        append(out, "FEEL ");
        append(out, feel_mode_name(pattern.feel));
        if (feel_depth(pattern.feel) != 0) {
            *out++ = ' ';
            append_number(out, uint32_t(feel_depth(pattern.feel)));
        }
        *out = 0;
        list_row(canvas, 0, 44, line, pattern_field_ == kPatternFieldFeel, pattern.feel == 0 ? 8 : 10);

        // Each track's own swing as a bar from 50%; PAT follows SWG above, drawn dim at it.
        paint::small_text(px, 0, 55, "TRACK SWING", 8);
        paint::small_text(px, swing_x(75) - 7, 55, "75%", 5);
        const int pat_x = swing_x(int(pattern.swing));
        paint::dotted_v(px, pat_x, 61, 109, 5, 2);
        for (int t = 0; t < kTracks; t++) {
            const int field = kPatternFieldTrackSwing + t;
            const bool here = pattern_field_ == field;
            const uint8_t swing = pattern.tracks[t].swing;
            const bool inherit = swing == kSwingInherit;
            const int y = 62 + t * 6;
            if (here) canvas.rect(0, y - 1, 124, 7, 15);
            char num[4] = {char('1' + t), 0, 0, 0};
            paint::small_text(px, 1, y, num, here ? 0 : 8);
            paint::small_text(px, 6, y, machine_name(engine_.machine_id(t)), here ? 0 : 6);
            const int to = swing_x(inherit ? int(pattern.swing) : int(swing));
            canvas.rect(swing_x(50), y + 1, to - swing_x(50) + 1, 3,
                        uint8_t(here ? 0 : (inherit ? 4 : 12)));
            if (!here) paint::plot(px, pat_x, y + 4, 7);
            out = line;
            if (inherit) {
                append(out, "PAT");
            } else {
                append_number(out, uint32_t(swing));
                *out++ = '%';
            }
            *out = 0;
            paint::small_text(px, 123 - paint::small_width(line), y, line,
                              here ? 0 : (inherit ? 6 : 13));
        }

        paint::small_text(px, 0, 113, "UP DOWN VALUE A-D", 10);
    }
