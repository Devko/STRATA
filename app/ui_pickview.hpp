// PROJECT's slots and the MACHINE picker, in the drafts' language (docs/SCREENS.md, "PROJECT"
// and "MACHINE"): pictures over text. PROJECT maps all 128 patterns and draws each of the shown
// bank's sixteen as its steps; MACHINE draws the kit and every machine as a picture of its sound.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, like the other ui_*.hpp parts: no #pragma once and no includes, and it sees every
// member the class has.

    // --- PROJECT's slots ---------------------------------------------------------------------
    //
    // Rows: the caption at 18 (the project, and whether RAM differs from the card), the key and
    // the queue at 25, the sample pool and the bank's count at 32, the map's bank letters at 39
    // and its cells 46-56, the shown bank's sixteen tiles 61-115.

    static constexpr int kMapY = 46;        // the map's first row of cells
    static constexpr int kTileY = 61;       // the first row of tiles
    static constexpr int kTilePitch = 14;

    // A pattern's steps as sixteen columns of eight track bits: a step with a trig (or a
    // trigless lock) sets its track's bit in its column. A track longer than 16 steps folds
    // into the sixteen, so a 64-step track shows four steps a column.
    static void pattern_columns(const Pattern &pattern, uint8_t *columns) {
        for (int c = 0; c < 16; c++) columns[c] = 0;
        for (int t = 0; t < kTracks; t++) {
            const TrackPattern &track = pattern.tracks[t];
            const int length = track.length > kStepsPerTrack ? kStepsPerTrack : int(track.length);
            // Steps per column in Q4: 16 for 16 steps or fewer (one a column), 64 for 64.
            const int per = length > 16 ? length : 16;
            for (int i = 0; i < length; i++) {
                if (track.steps[i].flags & (kStepTrig | kStepTrigless)) {
                    columns[(i * 16) / per] |= uint8_t(1u << t);
                }
            }
        }
    }

    // Does the pattern hold a trig (or a trigless lock) on any track? Stops at the first.
    static bool pattern_used(const Pattern &pattern) {
        for (int t = 0; t < kTracks; t++) {
            const TrackPattern &track = pattern.tracks[t];
            const int length = track.length > kStepsPerTrack ? kStepsPerTrack : int(track.length);
            for (int i = 0; i < length; i++) {
                if (track.steps[i].flags & (kStepTrig | kStepTrigless)) return true;
            }
        }
        return false;
    }

    static uint16_t bank_bits(const Project &project, int bank) {
        uint16_t bits = 0;
        for (int s = 0; s < kPatternsPerBank; s++) {
            if (pattern_used(project.patterns[bank][s])) bits = uint16_t(bits | (1u << s));
        }
        return bits;
    }

    // The cache the map and the tiles draw from. The shown bank's sixteen are read whole every
    // frame (they are what the tiles draw). The other banks' "holds a pattern" bits are read one
    // bank a frame, 400 ms for all seven, and all of them in the first frame the screen is up
    // after another: a map that changed while something else was on screen is right at once,
    // and a screen nobody touches holds still (tests/test_ui.cpp's input audit).
    void refresh_slot_cache() {
        const Project &project = engine_.project();
        const bool entered = slots_frame_ + 1u != draw_frame_;
        slots_frame_ = draw_frame_;
        uint16_t used = 0;
        for (int s = 0; s < kPatternsPerBank; s++) {
            pattern_columns(project.patterns[shown_bank_][s], slot_thumb_[s]);
            uint8_t any = 0;
            for (int c = 0; c < 16; c++) any |= slot_thumb_[s][c];
            if (any) used = uint16_t(used | (1u << s));
        }
        bank_used_[shown_bank_] = used;
        if (entered) {
            for (int b = 0; b < kBanks; b++) {
                if (b != shown_bank_) bank_used_[b] = bank_bits(project, b);
            }
            return;
        }
        used_next_ = (used_next_ + 1) % kBanks;
        if (used_next_ == shown_bank_) used_next_ = (used_next_ + 1) % kBanks;
        bank_used_[used_next_] = bank_bits(project, used_next_);
    }

    // What the queue blinks on: eighth notes of the running clock, so a stopped screen holds
    // still (and with the transport stopped a queued change lands at once anyway).
    bool queue_blink_on() const {
        const Status &status = engine_.status();
        if (status.running.load(std::memory_order_relaxed) == 0) return true;
        return ((status.tick.load(std::memory_order_relaxed) / 48u) & 1u) == 0;
    }

    void draw_slots_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Status &status = engine_.status();
        const int live_bank = int(status.live_bank.load(std::memory_order_relaxed));
        const int live_slot = int(status.live_slot.load(std::memory_order_relaxed));
        const Queue &queue = engine_.queue();
        const bool queued = queue.pending;
        const int queued_bank = int(queue.bank);
        const int queued_slot = int(queue.slot);
        const bool blink = queue_blink_on();
        refresh_slot_cache();

        // The caption: the project at the left; at the right what the card side is doing, what
        // went wrong last, or UNSAVED. An unsaved project that looked saved is the worst outcome
        // this screen can produce, so the mark is always on this line when RAM differs.
        const bool usable = storage_.card_usable();
        out = line;
        const char *busy = storage_.busy_text();
        if (busy != nullptr) {
            append(out, busy);
            append(out, " ");
            append_number(out, uint32_t(storage_.progress()));
            *out++ = '%';
        } else if (!usable) {
            append(out, "NO CARD");
        } else if (storage_.last_error() != nullptr) {
            append_shown(out, storage_.last_error(), 16);
        } else if (storage_.dirty()) {
            append(out, "UNSAVED");
        }
        *out = 0;
        const int state_w = line[0] != 0 ? paint::small_width(line) : 0;
        if (state_w > 0) paint::small_text(px, 125 - state_w, 18, line, busy != nullptr ? 15 : 12);
        char name[40];
        out = name;
        const int name_chars = (125 - state_w - (state_w > 0 ? 4 : 0) + 1) / paint::kSmallAdvance;
        append_shown(out, storage_.project_name(), name_chars < kLineChars ? name_chars : kLineChars);
        *out = 0;
        paint::small_text(px, 0, 18, name, 15);

        // The key and scale, from the live pattern's settings (as before); the queue mode and
        // the chain at the right.
        out = line;
        append(out, note_name(engine_.key_root()));
        *out++ = ' ';
        append(out, scale_name(int(engine_.key_scale())));
        *out = 0;
        paint::small_text(px, 0, 25, "KEY", 6);
        paint::small_text(px, 15, 25, line, 13);
        out = line;
        append(out, queue_mode_name(queue_mode_));
        if (engine_.chain().active) append(out, " CHAIN");
        *out = 0;
        const int queue_w = paint::small_width(line);
        paint::small_text(px, 125 - queue_w, 25, line, 13);
        paint::small_text(px, 125 - queue_w - 4 - paint::small_width("Q"), 25, "Q", 6);

        // The sample pool: a mark a track (bright: loaded; a hollow one: streamed from the card;
        // mid: loading; a cross: failed; dim: none), then the budget used. The pool lives on
        // this side with the loader, so it is read directly and not through a status.
        const SamplePool &pool = storage_.samples();
        paint::small_text(px, 0, 32, "SMP", 6);
        for (int t = 0; t < kTracks; t++) {
            const SampleState state = pool.slot(t).state;
            const int x = 15 + t * 4;
            if (state == SampleState::ready && storage_.streaming(t)) {
                canvas.rect(x, 33, 3, 3, 13);
                canvas.rect(x + 1, 34, 1, 1, 0);
            } else if (state == SampleState::ready) {
                canvas.rect(x, 33, 3, 3, 13);
            } else if (state == SampleState::loading) {
                canvas.rect(x, 34, 3, 1, 9);
            } else if (state == SampleState::failed) {
                paint::plot(px, x, 33, 15); paint::plot(px, x + 2, 33, 15);
                paint::plot(px, x + 1, 34, 15);
                paint::plot(px, x, 35, 15); paint::plot(px, x + 2, 35, 15);
            } else {
                canvas.rect(x + 1, 34, 1, 1, 4);
            }
        }
        out = line;
        append_number(out, pool.used() >> 20);
        *out++ = '/';
        append_number(out, pool.budget() >> 20);
        append(out, "M");
        *out = 0;
        paint::small_text(px, 49, 32, line, 13);
        int used_count = 0;
        for (int s = 0; s < kPatternsPerBank; s++) used_count += (bank_used_[shown_bank_] >> s) & 1;
        out = line;
        append(out, "BANK ");
        *out++ = bank_letter(shown_bank_);
        *out++ = ' ';
        append_number(out, uint32_t(used_count));
        append(out, "/16");
        *out = 0;
        paint::small_text(px, 125 - paint::small_width(line), 32, line, 10);

        // The map: every bank as a block of 4 x 4 cells laid out as its tiles below are. Dim:
        // empty; grey: holds a pattern; white: playing; blinking: queued. The shown bank's
        // letter is lit.
        for (int b = 0; b < kBanks; b++) {
            const int bx = b * 15 + 1;
            const char letter[2] = {bank_letter(b), 0};
            if (b == shown_bank_) {
                paint::small_badge(px, bx + 4, 39, letter);
            } else {
                paint::small_text(px, bx + 4, 39, letter, bank_used_[b] != 0 ? 9 : 4);
            }
            for (int s = 0; s < kPatternsPerBank; s++) {
                const int x = bx + (s % 4) * 3;
                const int y = kMapY + (s / 4) * 3;
                const bool has = (bank_used_[b] >> s) & 1;
                int grey = has ? (b == shown_bank_ ? 9 : 7) : 2;
                if (b == live_bank && s == live_slot) grey = 15;
                if (queued && b == queued_bank && s == queued_slot) grey = blink ? 15 : 4;
                canvas.rect(x, y, 2, 2, uint8_t(grey));
            }
        }

        // The shown bank's sixteen: each a tile with its number and its steps (a row a track,
        // sixteen columns). The playing one's number lit and its playhead a bright column; the
        // queued one's underline blinking; the cursor a white frame.
        const int ph_track = selected_;
        const int ph_length = int(engine_.pattern().tracks[ph_track].length);
        const int ph_step = int(status.playhead[ph_track].load(std::memory_order_relaxed));
        const bool running = status.running.load(std::memory_order_relaxed) != 0;
        const int ph_col = ph_length > 16 ? (ph_step * 16) / ph_length : ph_step;
        for (int s = 0; s < kPatternsPerBank; s++) {
            const int x = (s % 4) * 30;
            const int y = kTileY + (s / 4) * kTilePitch;
            const bool has = (bank_used_[shown_bank_] >> s) & 1;
            const bool live = shown_bank_ == live_bank && s == live_slot;
            const bool is_queued = queued && shown_bank_ == queued_bank && s == queued_slot;
            if (s == slot_cursor_) {
                frame_rect(canvas, x, y, 29, 13, 15);
            } else if (has || live) {
                frame_rect(canvas, x, y, 29, 13, 4);
            } else {
                paint::dotted_h(px, x, x + 28, y, 3);
                paint::dotted_h(px, x, x + 28, y + 12, 3);
            }
            char num[12];   // append_number writes up to 10 digits, as tic6x GCC knows
            out = num;
            append_padded(out, uint32_t(s) + 1, 2);
            *out = 0;
            if (live) {
                paint::small_badge(px, x + 3, y + 3, num);
            } else {
                paint::small_text(px, x + 3, y + 3, num, has ? 15 : 5);
            }
            if (is_queued) canvas.rect(x + 2, y + 10, 9, 1, blink ? 15 : 4);
            if (!has) {
                if (!live) paint::small_text(px, x + 15, y + 4, "--", 3);
                continue;
            }
            const uint8_t *columns = slot_thumb_[s];
            for (int c = 0; c < 16; c++) {
                const bool head = live && running && c == ph_col;
                for (int t = 0; t < kTracks; t++) {
                    const bool on = (columns[c] >> t) & 1;
                    const int grey = on ? (head ? 15 : (live ? 11 : 8)) : (head ? 4 : 0);
                    if (grey != 0) paint::plot(px, x + 12 + c, y + 3 + t, grey);
                }
            }
        }
    }

    // A one-pixel frame.
    static void frame_rect(enaga::Canvas &canvas, int x, int y, int w, int h, int grey) {
        canvas.rect(x, y, w, 1, uint8_t(grey));
        canvas.rect(x, y + h - 1, w, 1, uint8_t(grey));
        canvas.rect(x, y, 1, h, uint8_t(grey));
        canvas.rect(x + w - 1, y, 1, h, uint8_t(grey));
    }

    // --- MACHINE: the kit, the machines as pictures, and what the track's is for ---------------
    //
    // Rows: the caption at 18 (the track, MIDI OUT on B at the right), the kit 24-38 (a cell a
    // track: its machine's picture and name), the machines 42-94 (columns of three, as the arrows
    // walk them: UP and DOWN by one, LEFT and RIGHT by three), what the track's machine is for
    // at 98 and 105, the hints at 113.

    struct MachineInfo {
        const char *what;     // what it is and what it is for
        const char *cost;     // voices and CPU
    };

    static MachineInfo machine_info(uint8_t id) {
        static const MachineInfo info[] = {
            {"KICK: SINE, PITCH SWEEP, CLICK", "DRUM, 2 VOICES, LIGHT ON CPU"},
            {"SNARE OR CLAP: TONE AND NOISE", "DRUM, 2 VOICES, LIGHT ON CPU"},
            {"HATS: SIX SQUARES AND NOISE", "DRUM, 2 VOICES, T3 CHOKES T4"},
            {"PERC: 2-OP FM, TOMS TO BELLS", "DRUM, 2 VOICES, LIGHT ON CPU"},
            {"SAMPLE: ONE-SHOT, SLICES, LOOP", "4 VOICES, A WAV FROM THE CARD"},
            {"GRAIN: CLOUDS FROM A SAMPLE", "1 VOICE, UP TO 8 GRAINS"},
            {"VA MONO: BASS AND LEADS", "1 VOICE, GLIDE, SLIDE, ACCENT"},
            {"VA POLY: CHORDS AND PADS", "6 VOICES OF THE SHARED 12"},
            {"MIDI: NOTES AND CCS OUT", "NO SOUND, NO CPU"},
            {"WAVETABLE MONO: MOVING LEADS", "1 VOICE, 16 TABLES"},
            {"WAVETABLE POLY: MOVING PADS", "6 VOICES OF THE SHARED 12"},
            {"FM 4-OP: KEYS, BELLS, BASS", "6 VOICES OF THE SHARED 12"},
        };
        static_assert(int(sizeof(info) / sizeof(info[0])) == int(MachineId::count),
                      "every machine says what it is for");
        return id < uint8_t(MachineId::count) ? info[id] : MachineInfo{"", ""};
    }

    // A machine's picture, w x h at (x, y): what its sound looks like. Drawn from integer
    // shapes, no timer: a stopped screen holds still (docs/SCREENS.md). `w` 12-25, `h` 6-9.
    static void machine_icon(uint8_t *px, uint8_t id, int x, int y, int w, int h, int grey) {
        const int half_q4 = (h * paint::kSub) / 2 - paint::kSub / 2;   // the amplitude, Q4
        const int mid_q4 = y * paint::kSub + (h * paint::kSub) / 2 - paint::kSub / 2;
        int16_t xs[32];
        int16_t ys[32];
        const int n = w < 32 ? w : 32;
        uint32_t seed = 0x9E3779B9u * (id + 1u);
        const auto noise = [&seed](int range) {      // -range .. range, a fixed sequence
            seed = seed * 1664525u + 1013904223u;
            return int((seed >> 16) % uint32_t(2 * range + 1)) - range;
        };
        const int dim = grey * 2 / 3;
        switch (MachineId(id)) {
            case MachineId::kick: {
                // A sine sweeping down and dying away.
                int angle = 0;
                for (int i = 0; i < n; i++) {
                    const int left = n - i;
                    const int amp = (half_q4 * left * left) / (n * n);
                    xs[i] = int16_t((x + i) * paint::kSub);
                    ys[i] = int16_t(mid_q4 - (amp * paint::sin_q12(angle)) / 4096);
                    angle += 12 + (48 * left) / n;
                }
                paint::polyline(px, xs, ys, n, grey);
                break;
            }
            case MachineId::perc: {
                // A ring of FM: a sine bent by another, dying away.
                for (int i = 0; i < n; i++) {
                    const int left = n - i;
                    const int amp = (half_q4 * left * left) / (n * n);
                    const int angle = i * 54 + (paint::sin_q12(i * 27) * 40) / 4096;
                    xs[i] = int16_t((x + i) * paint::kSub);
                    ys[i] = int16_t(mid_q4 - (amp * paint::sin_q12(angle)) / 4096);
                }
                paint::polyline(px, xs, ys, n, grey);
                break;
            }
            case MachineId::snare: {
                // A short tone, then a burst of noise falling away.
                const int tone = n / 4;
                for (int i = 0; i < tone; i++) {
                    xs[i] = int16_t((x + i) * paint::kSub);
                    ys[i] = int16_t(mid_q4 - (half_q4 * paint::sin_q12(i * 50)) / 4096);
                }
                paint::polyline(px, xs, ys, tone, grey);
                for (int i = 0; i < n; i++) {
                    const int amp = ((h / 2) * (n - i)) / n;
                    paint::plot_max(px, x + i, y + h / 2 + noise(amp), grey);
                }
                break;
            }
            case MachineId::hat: {
                // Two ticks of noise: a closed hat and an open one.
                const int first = n / 5 + 1;
                for (int i = 0; i < n; i++) {
                    const bool open = i >= n / 2;
                    const int at = open ? i - n / 2 : i;
                    const int length = open ? n - n / 2 : first;
                    if (at >= length) continue;
                    const int amp = ((h / 2) * (length - at)) / length;
                    paint::plot_max(px, x + i, y + h / 2 + noise(amp), grey);
                    paint::plot_max(px, x + i, y + h / 2 + noise(amp), dim);
                }
                break;
            }
            case MachineId::sample: {
                // A recording: bars of a waveform, louder and quieter.
                for (int i = 0; i < n; i++) {
                    const int env = 4096 + (paint::sin_q12(i * 21) < 0 ? -paint::sin_q12(i * 21) : paint::sin_q12(i * 21));
                    int amp = ((h / 2) * env) / 8192 + (noise(1) > 0 ? 1 : 0);
                    if (amp < 1) amp = 1;
                    const int top = y + h / 2 - amp;
                    for (int j = 0; j < 2 * amp && top + j < y + h; j++) {
                        paint::plot(px, x + i, top + j, (i & 1) ? dim : grey);
                    }
                }
                break;
            }
            case MachineId::grain: {
                // A cloud of short grains.
                const int grains = (w * h) / 16;
                for (int k = 0; k < grains; k++) {
                    const int gx = x + int((seed = seed * 1664525u + 1013904223u) >> 16) % (w - 2);
                    const int gy = y + int((seed = seed * 1664525u + 1013904223u) >> 16) % h;
                    const int g = (k % 3 == 0) ? grey : dim;
                    paint::plot_max(px, gx, gy, g);
                    paint::plot_max(px, gx + 1, gy, g);
                }
                break;
            }
            case MachineId::va_mono:
            case MachineId::va_poly: {
                // A saw; POLY's three stacked, a chord.
                const int layers = MachineId(id) == MachineId::va_poly ? 3 : 1;
                const int period = layers == 1 ? 8 : 10;
                const int rise = layers == 1 ? h - 1 : (h * 3) / 5;
                for (int k = layers - 1; k >= 0; k--) {
                    const int g = k == 0 ? grey : dim;
                    const int base = y + h - 1 - (k * (h - rise)) / (layers > 1 ? layers - 1 : 1);
                    for (int i = 0; i < n; i++) {
                        const int phase = (i + k * 3) % period;
                        paint::plot_max(px, x + i, base - (phase * rise) / (period - 1), g);
                        if (phase == period - 1 && layers == 1) {
                            for (int j = 1; j < rise; j++) paint::plot_max(px, x + i, base - j, dim);
                        }
                    }
                }
                break;
            }
            case MachineId::midi: {
                // A five-pin socket.
                const int cx = x + w / 2;
                const int cy = y + h / 2;
                const int r = h / 2;
                for (int a = 0; a < paint::kTurn; a += 8) {
                    const int dx = (paint::cos_q12(a) * r + (paint::cos_q12(a) > 0 ? 2048 : -2048)) / 4096;
                    const int dy = (paint::sin_q12(a) * r + (paint::sin_q12(a) > 0 ? 2048 : -2048)) / 4096;
                    if (dy == r) continue;                      // the notch at the bottom
                    paint::plot_max(px, cx + dx, cy + dy, grey);
                }
                if (r >= 3) {
                    paint::plot(px, cx - r + 2, cy, grey);
                    paint::plot(px, cx + r - 2, cy, grey);
                    paint::plot(px, cx, cy - r + 2, grey);
                    paint::plot(px, cx - r / 2, cy - r / 2, grey);
                    paint::plot(px, cx + r / 2, cy - r / 2, grey);
                } else {
                    paint::plot(px, cx, cy, grey);
                }
                break;
            }
            case MachineId::wave_mono:
            case MachineId::wave_poly: {
                // Frames of a table, a sine turning square, stepped back like a waterfall.
                const int frames = MachineId(id) == MachineId::wave_poly ? 4 : 3;
                const int len = n - 2 * (frames - 1);
                for (int k = 0; k < frames; k++) {
                    const int g = k == frames - 1 ? grey : (grey * (k + 2)) / (frames + 2);
                    for (int i = 0; i < len; i++) {
                        const int s = paint::sin_q12((i * paint::kTurn) / (len - 1));
                        int v = s * 4;                          // the square end of the morph
                        v = v > 4096 ? 4096 : (v < -4096 ? -4096 : v);
                        v = (s * (frames - 1 - k) + v * k) / (frames - 1);
                        xs[i] = int16_t((x + i + 2 * k) * paint::kSub);
                        ys[i] = int16_t(mid_q4 + paint::kSub * (frames - 1) / 2 - k * paint::kSub -
                                        ((half_q4 - (frames - 1) * paint::kSub / 2) * v) / 4096);
                    }
                    paint::polyline(px, xs, ys, len, g);
                }
                break;
            }
            case MachineId::fm: {
                // Four operators, a stack.
                const int box = (w - 3) / 4 < h - 2 ? (w - 3) / 4 : h - 2;
                const int top = y + (h - box) / 2;
                for (int k = 0; k < 4; k++) {
                    const int bx = x + k * (box + 1);
                    for (int j = 0; j < box; j++) {
                        paint::plot(px, bx + j, top, grey);
                        paint::plot(px, bx + j, top + box - 1, grey);
                        paint::plot(px, bx, top + j, grey);
                        paint::plot(px, bx + box - 1, top + j, grey);
                    }
                }
                // Operator 4's feedback: a loop over the last box.
                const int fx = x + 3 * (box + 1);
                for (int j = 0; j < box; j++) paint::plot(px, fx + j, top - 2, dim);
                paint::plot(px, fx, top - 1, dim);
                paint::plot(px, fx + box - 1, top - 1, dim);
                break;
            }
            default:
                break;
        }
    }

    void draw_machine(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const uint8_t current = engine_.machine_id(selected_);

        append(out, "MACHINE T");
        append_number(out, uint32_t(selected_) + 1);
        *out = 0;
        paint::small_text(px, 0, 18, line, 15);

        // The track's MIDI OUT channel, on encoder B: OFF, or the channel its notes are sent on
        // (core/midiout.hpp). Shown for every machine because every machine can send; on MID it
        // is the whole point, so a MID track with it OFF says so lit.
        const uint8_t channel = engine_.midi_out(selected_);
        out = line;
        append(out, "MIDI OUT ");
        if (channel == kMidiOutOff) {
            append(out, "OFF");
        } else {
            append(out, "CH ");
            append_number(out, uint32_t(channel));
        }
        *out = 0;
        const bool silent = channel == kMidiOutOff && current == uint8_t(MachineId::midi);
        const int midi_w = paint::small_width(line);
        if (silent) {
            paint::small_badge(px, 124 - midi_w, 18, line);
        } else {
            paint::small_text(px, 125 - midi_w, 18, line, channel == kMidiOutOff ? 7 : 13);
        }
        paint::small_text(px, 125 - midi_w - (silent ? 1 : 0) - 4 - paint::small_width("B"), 18, "B", 6);

        // The kit: every track's machine, the selected one lit. The question anyone in this mode
        // is asking is "what is this kit", not only "what is this track".
        for (int t = 0; t < kTracks; t++) {
            const int x = t * 15;
            const uint8_t id = engine_.machine_id(t);
            const bool on = t == selected_;
            if (on) {
                frame_rect(canvas, x, 24, 14, 15, 15);
                machine_icon(px, id, x + 1, 25, 12, 6, 15);
                paint::small_badge(px, x + 1, 32, machine_name(id));
            } else {
                machine_icon(px, id, x + 1, 25, 12, 6, 8);
                paint::small_text(px, x + 1, 32, machine_name(id), 8);
            }
        }

        // The machines, each a picture of its sound and its name; the track's lit. Columns of
        // three, read down, as UP and DOWN (one) and LEFT and RIGHT (three) walk them.
        static_assert(int(MachineId::count) <= 12, "the MACHINE grid holds four columns of three");
        for (int m = 0; m < int(MachineId::count); m++) {
            const int x = (m / 3) * 30;
            const int y = 42 + (m % 3) * 18;
            const bool on = m == int(current);
            const char *name = machine_name(uint8_t(m));
            const int name_x = x + 15 - (paint::small_width(name) + 1) / 2;
            if (on) {
                frame_rect(canvas, x, y, 29, 17, 15);
                canvas.rect(x + 1, y + 11, 27, 6, 15);
                paint::small_text(px, name_x, y + 12, name, 0);
            } else {
                frame_rect(canvas, x, y, 29, 17, 3);
                paint::small_text(px, name_x, y + 12, name, 7);
            }
            machine_icon(px, uint8_t(m), x + 2, y + 2, 25, 8, on ? 15 : 7);
        }

        const MachineInfo info = machine_info(current);
        paint::small_text(px, 0, 98, info.what, 15);
        paint::small_text(px, 0, 105, info.cost, 8);

        paint::small_text(px, 0, 113,
                          plays_files(selected_)
                              ? "OK: PICK SAMPLE"
                              : (sound_preset_count(current) > 0 ? "OK: SOUNDS" : "ARROWS: MACHINE"),
                          10);
        paint::small_text(px, 125 - paint::small_width("FUNC+EDIT: KITS"), 113, "FUNC+EDIT: KITS", 6);
    }
