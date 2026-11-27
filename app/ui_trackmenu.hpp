// (Included inside class StrataApp; see app/app.hpp.)
//
    // --- the track menu: hold OK in GRID ------------------------------------------------------
    //
    // GEN and EVOLVE for the selected track (Roland, 2026-11-27), opened the way the pattern menu
    // opens on PROJECT's slots: a hold of kTrackMenuHoldMs on OK in GRID with nothing held. A
    // tap does what OK in GRID always did, which is nothing, so the hold costs no gesture.
    //
    // UP/DOWN or A pick a row, LEFT/RIGHT or B change it, OK runs an action row (and steps a
    // setting row forward, wrapping), CLR closes. The selection keys still change the track, so
    // one menu walks GEN over a whole kit.
    //
    //   GENERATE      OK: a new part, one undo level (core/generate.hpp)
    //   DENSITY       0-100 in tens
    //   RANGE         1-3 octaves; a melodic track's
    //   LEAP          0-100 in tens: how often the melody jumps
    //   WHAT          BOTH, RHYTHM, NOTES
    //   EVOLVE        OFF, 1, 2, 4, 8 loops: this track's (core/evolve.hpp)
    //   MODE          ORBIT or DRIFT, for every track
    //   MUTATION      which mutation EVOLVE plays: PERFORM's MUTATE setting, shared
    //   AMOUNT        and how hard, also shared
    //   KEEP          OK: what plays becomes HOME, EVOLVE stops
    //   HOME          OK: back to HOME (one undo level), EVOLVE stops
    static constexpr uint32_t kTrackMenuHoldMs = 500;
    enum class TrackRow : uint8_t { generate, density, range, leap, what, evolve, mode, kind,
                                    amount, keep, home, count };
    static constexpr int kTrackRows = int(TrackRow::count);
    static constexpr int kTrackRowsShown = 9;

    static const char *track_row_name(TrackRow row) {
        switch (row) {
            case TrackRow::generate: return "GENERATE";
            case TrackRow::density:  return "DENSITY";
            case TrackRow::range:    return "RANGE";
            case TrackRow::leap:     return "LEAP";
            case TrackRow::what:     return "WHAT";
            case TrackRow::evolve:   return "EVOLVE";
            case TrackRow::mode:     return "MODE";
            case TrackRow::kind:     return "MUTATION";
            case TrackRow::amount:   return "AMOUNT";
            case TrackRow::keep:     return "KEEP";
            default:                 return "HOME";
        }
    }

    bool track_ok_target() const {
        return mode_ == Mode::grid && !step_page_ && held_step_ < 0 && range_first_ < 0 &&
               !shift_ && !mutation_pending_;
    }

    void track_row_value(TrackRow row, char *out) {
        switch (row) {
            case TrackRow::density: append_number(out, uint32_t(gen_density_)); break;
            case TrackRow::range:
                append_number(out, uint32_t(gen_range_));
                append(out, " OCT");
                break;
            case TrackRow::leap:    append_number(out, uint32_t(gen_leap_)); break;
            case TrackRow::what:    append(out, gen_what_name(GenWhat(gen_what_))); break;
            case TrackRow::evolve:
                append(out, evolve_period_name(int(engine_.evolve_period(selected_))));
                break;
            case TrackRow::mode:    append(out, evolve_mode_name(evolve_mode_ui_)); break;
            case TrackRow::kind:    append(out, mutation_name(mutation_)); break;
            case TrackRow::amount:  append_number(out, uint32_t(mutate_amount_)); break;
            default: break;
        }
        *out = 0;
    }

    // EVOLVE's shared settings to the engine: on every change here, and when PERFORM changes
    // the mutation or its amount, so EVOLVE always plays what the screens say.
    void post_evolve_config() {
        post(Cmd::evolve_config, 0, 0, uint8_t(evolve_mode_ui_), uint8_t(mutation_),
             uint8_t(mutate_amount_));
    }

    static int step_wrap(int value, int delta, int low, int high) {
        const int span = high - low + 1;
        return low + ((value - low + delta) % span + span) % span;
    }

    static int step_stop(int value, int delta, int low, int high) {
        const int next = value + delta;
        return next < low ? low : (next > high ? high : next);
    }

    // A setting row by `delta`; `wrap` is OK's forward step, which goes round.
    void track_row_change(int delta, bool wrap) {
        const TrackRow row = TrackRow(track_menu_cursor_);
        auto walk = [&](int value, int by, int low, int high) {
            return wrap ? step_wrap(value, by, low, high) : step_stop(value, by, low, high);
        };
        switch (row) {
            case TrackRow::density: gen_density_ = walk(gen_density_, delta * 10, 0, 100); break;
            case TrackRow::range:   gen_range_ = walk(gen_range_, delta, kGenRangeMin, kGenRangeMax); break;
            case TrackRow::leap:    gen_leap_ = walk(gen_leap_, delta * 10, 0, 100); break;
            case TrackRow::what:
                gen_what_ = step_wrap(gen_what_, delta, 0, int(GenWhat::count) - 1);
                break;
            case TrackRow::evolve: {
                const int now = int(engine_.evolve_period(selected_));
                const int next = walk(now, delta, 0, kEvolvePeriods - 1);
                post_evolve_config();
                post(Cmd::evolve_set, uint8_t(selected_), 0, uint8_t(next));
                char *out = notice_buffer_;
                append(out, "EVOLVE T");
                append_number(out, uint32_t(selected_ + 1));
                *out++ = ' ';
                append(out, evolve_period_name(next));
                *out = 0;
                notice_logged(notice_buffer_);
                break;
            }
            case TrackRow::mode:
                evolve_mode_ui_ = EvolveMode(step_wrap(int(evolve_mode_ui_), delta, 0,
                                                       int(EvolveMode::count) - 1));
                post_evolve_config();
                break;
            case TrackRow::kind:
                mutation_ = Mutation(step_wrap(int(mutation_), delta, 0, int(Mutation::count) - 1));
                post_evolve_config();
                break;
            case TrackRow::amount:
                mutate_amount_ = walk(mutate_amount_, delta * 10, 0, 100);
                post_evolve_config();
                break;
            default:
                break;
        }
    }

    void track_generate() {
        mark(GestureKind::paste, selected_, 0);
        Command command;
        command.cmd = Cmd::generate;
        command.track = uint8_t(selected_);
        command.a = uint8_t(gen_density_);
        command.b = uint8_t(gen_range_);
        command.c = uint8_t(gen_leap_);
        command.d = uint8_t(gen_what_);
        command.u = gen_variant_++;          // a new part each press
        post_command(command);
        end_gesture();
        mutate_track_ = selected_;           // NOTHING TO MUTATE names this track if it did nothing
        char *out = notice_buffer_;
        append(out, "GEN T");
        append_number(out, uint32_t(selected_ + 1));
        *out++ = ' ';
        append(out, gen_what_name(GenWhat(gen_what_)));
        *out = 0;
        notice_logged(notice_buffer_);
    }

    void track_row_run() {
        switch (TrackRow(track_menu_cursor_)) {
            case TrackRow::generate:
                track_generate();
                break;
            case TrackRow::keep:
                post(Cmd::evolve_keep, uint8_t(selected_));
                notice_logged(engine_.evolve_period(selected_) != 0 ? "KEPT: EVOLVE OFF"
                                                                    : "EVOLVE IS OFF");
                break;
            case TrackRow::home:
                if (engine_.evolve_period(selected_) == 0) {
                    notice_logged("EVOLVE IS OFF");
                    break;
                }
                mark(GestureKind::paste, selected_, 0);
                post(Cmd::evolve_home, uint8_t(selected_));
                end_gesture();
                notice_logged("HOME: EVOLVE OFF");
                break;
            default:
                track_row_change(+1, true);
                break;
        }
    }

    // True when the key was the menu's, or the OK hold's.
    bool on_track_menu_key(int key, bool down) {
        if (track_menu_) {
            const bool mine = key == ENAGA_KEY_OK || key == ENAGA_KEY_UP ||
                              key == ENAGA_KEY_DOWN || key == ENAGA_KEY_CLR ||
                              key == ENAGA_KEY_LEFT || key == ENAGA_KEY_RIGHT;
            if (!mine || shift_) return false;  // F1 + OK / CLR stay redo and undo
            if (!down) return true;
            switch (key) {
                case ENAGA_KEY_CLR:
                    track_menu_ = false;
                    notice("GRID");
                    break;
                case ENAGA_KEY_UP:
                case ENAGA_KEY_DOWN:
                    track_menu_cursor_ = step_wrap(track_menu_cursor_,
                                                   key == ENAGA_KEY_UP ? -1 : 1, 0, kTrackRows - 1);
                    break;
                case ENAGA_KEY_LEFT:
                case ENAGA_KEY_RIGHT:
                    track_row_change(key == ENAGA_KEY_LEFT ? -1 : 1, false);
                    break;
                default:
                    track_row_run();
                    break;
            }
            return true;
        }
        if (key != ENAGA_KEY_OK) return false;
        if (down) {
            if (!track_ok_target()) return false;
            track_ok_held_ = true;
            track_ok_down_ms_ = last_ms_;
            return true;
        }
        if (track_ok_held_) {
            track_ok_held_ = false;          // a tap: OK in GRID does nothing else
            return true;
        }
        return false;
    }

    // update(): a held OK opens the menu, and leaving GRID closes it.
    void service_track_menu() {
        if (track_menu_ && (mode_ != Mode::grid || step_page_)) track_menu_ = false;
        if (!track_ok_held_) return;
        if (!track_ok_target()) {
            track_ok_held_ = false;
            return;
        }
        if (uint32_t(last_ms_ - track_ok_down_ms_) < kTrackMenuHoldMs) return;
        track_ok_held_ = false;
        track_menu_ = true;
        post_evolve_config();
        char *out = notice_buffer_;
        append(out, "TRACK T");
        append_number(out, uint32_t(selected_ + 1));
        *out = 0;
        notice_logged(notice_buffer_);
    }

    void draw_track_menu(enaga::Canvas &canvas) {
        char line[40];
        char value[24];
        int first = track_menu_cursor_ - kTrackRowsShown / 2;
        if (first > kTrackRows - kTrackRowsShown) first = kTrackRows - kTrackRowsShown;
        if (first < 0) first = 0;
        for (int row = 0; row < kTrackRowsShown && first + row < kTrackRows; row++) {
            const int i = first + row;
            const bool here = i == track_menu_cursor_;
            char *out = line;
            *out++ = here ? '>' : ' ';
            append(out, track_row_name(TrackRow(i)));
            *out = 0;
            const int y = 20 + row * 9;
            list_row(canvas, 0, y, line, here, 9);
            track_row_value(TrackRow(i), value);
            int length = 0;
            while (value[length] != '\0') length++;
            if (length > 0) canvas.text(kFrameMeterX - length * 6, y, value, here ? 0 : 9);
        }
        char *out = line;
        append(out, "T");
        append_number(out, uint32_t(selected_ + 1));
        append(out, "  OK: RUN  CLR: BACK");
        *out = 0;
        paint::small_text(canvas.data(), 0, 105, line, 10);
        paint::small_text(canvas.data(), 0, 113, "LEFT/RIGHT: CHANGE", 10);
    }
