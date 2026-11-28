// PERFORM: scenes, the morph, macros (MACRO LEARN) and mutation.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- PERFORM: scenes, the morph, macros and mutation -----------------

    // True when the key belonged to this mode. OK captures a scene, F1 + OK erases one, UP and
    // DOWN walk which mutation MUTATE will do, CLR clears the selected macro.
    bool on_perform_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK: {
                // F1 + OK is redo here as everywhere (docs/SPEC.md), so it falls through to
                // the shared handler.
                if (shift_) return false;
                // Into whichever slot is held, or slot A's scene. The selected track only when a
                // scene pad is held: capturing one track is what makes "a scene stores parameter
                // values for any tracks" usable.
                const int slot = scene_holding_ >= 0 ? scene_holding_ : 0;
                const int scene = engine_.scene_slot(slot);
                post(Cmd::scene_capture, uint8_t(selected_), 0, uint8_t(scene),
                     scene_holding_ >= 0 ? 1 : 0);
                notice(scene_holding_ >= 0 ? "TRACK CAPTURED" : "SCENE CAPTURED");
                return true;
            }

            case ENAGA_KEY_CLR:
                // F1 + CLR is undo, which is how a MUTATE is taken back without leaving PERFORM.
                if (shift_) return false;
                // A scene pad held + CLR erases that slot's scene: a scene you cannot empty is
                // one you can only overwrite. (It was F1 + OK, which the spec gives to redo.)
                if (scene_holding_ >= 0) {
                    post(Cmd::scene_erase, 0, 0, uint8_t(engine_.scene_slot(scene_holding_)));
                    notice("SCENE ERASED");
                    return true;
                }
                post(Cmd::macro_clear, 0, 0, uint8_t(macro_selected_));
                notice("MACRO CLEARED");
                return true;

            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN: {
                const int delta = key == ENAGA_KEY_UP ? -1 : 1;
                // FUNC + UP / DOWN walked the auto-fill period; it is in the FUNC menu now.
                if (shift_) return true;
                // Which mutation MUTATE performs. Walked here rather than on an encoder because
                // all four encoders are macros in this mode.
                const int next = int(mutation_) + delta;
                mutation_ = Mutation(next < 0 ? int(Mutation::count) - 1
                                              : (next >= int(Mutation::count) ? 0 : next));
                notice(mutation_name(mutation_));
                post_evolve_config();          // and EVOLVE the same mutation
                return true;
            }

            // The pad-row keys are the step page here too, as on GRID: the STEP keys are steps
            // in PERFORM (D12). They fall through to the common handler.
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT: {
                // Which macro CLR clears and a STEP key assigns to: the macros are the row of
                // four across the screen.
                if (shift_) return true;
                macro_selected_ = (macro_selected_ + (key == ENAGA_KEY_LEFT ? -1 : 1) + kMacros) %
                                  kMacros;
                return true;
            }

            default:
                return false;
        }
    }

    // Does macro `m` hold this target? `track` is a track, or kMacroMasterTrack for a kill EQ
    // band, whose `param` is then the band (core/perform.hpp).
    bool macro_holds(int m, uint8_t track, uint8_t param) const {
        const Macro &macro = engine_.macros().macros[m];
        for (int i = 0; i < kMacroTargets; i++) {
            const MacroTarget &t = macro.targets[i];
            if (t.enabled != 0 && t.track == track && t.param == param) return true;
        }
        return false;
    }

    // One Cmd::macro_assign. A target the macro holds comes off (`low` and `high` unread); any
    // other goes on from `low` to `high`. A kill EQ band travels past the track parameters in
    // `b`, because the engine refuses every command that names track 8.
    void post_macro_assign(int m, uint8_t track, uint8_t param, uint8_t low, uint8_t high) {
        const bool master = track == kMacroMasterTrack;
        Command command;
        command.cmd = Cmd::macro_assign;
        command.track = master ? uint8_t(0) : track;
        command.a = uint8_t(m);
        command.b = master ? uint8_t(kTrackParams + param) : param;
        command.c = low;
        command.d = high;
        post_command(command);
    }

    // "T6 CUT" or "EQ LOW": a target as the notices name it.
    void append_target(char *&out, uint8_t track, uint8_t param) const {
        if (track == kMacroMasterTrack) {
            append(out, "EQ ");
            append(out, master_field_name(uint8_t(kMasterEqFirst + param)));
            return;
        }
        *out++ = 'T';
        append_number(out, uint32_t(track) + 1);
        *out++ = ' ';
        append(out, track_param_name(param));
    }

    // A push while learning, on a target the macro holds: take it off ("B T6 CUT OFF 1/4").
    // Adding is by turning only (D11), so this is the one assignment a push still makes.
    void take_off_macro(int m, uint8_t track, uint8_t param) {
        char *out = notice_buffer_;
        *out++ = char('A' + m);
        *out++ = ' ';
        append_target(out, track, param);
        append(out, " OFF ");
        append_number(out, uint32_t(macro_count(engine_.macros().macros[m]) - 1));
        append(out, "/4");
        *out = 0;
        notice(notice_buffer_);
        post_macro_assign(m, track, param, 0, 0);
    }

    // LEARN BY TURNING (Roland, 2026-11-25, D11): while LEARN is on, every knob turned in EDIT
    // -- and LOW, MID and HI on MIXER's EQ page, the kill EQ's bands -- is remembered with the
    // value it started from. When LEARN ends, each goes on the macro with that start as its 0
    // and where the knob was left as its 127, and the knob goes back to its start -- so the
    // macro at 0 is the old sound and at 127 the sound just dialled.
    struct LearnTouch {
        uint8_t track;                          // a track, or kMacroMasterTrack: an EQ band
        uint8_t param;
        uint8_t start;
    };
    LearnTouch learn_touches_[kMacroTargets] = {};
    int learn_touch_count_ = 0;

    // The byte a learnt target reads now: the kit's, or the master EQ band's in the project.
    uint8_t learn_byte(uint8_t track, uint8_t param) const {
        return track == kMacroMasterTrack
                   ? engine_.project().settings.master[kMasterEqFirst + param]
                   : engine_.params(track).v[param];
    }

    // Called with the turn's delta still in the ring, so learn_byte reads the value before it.
    void learn_touch(uint8_t track, uint8_t param) {
        for (int i = 0; i < learn_touch_count_; i++) {
            if (learn_touches_[i].track == track && learn_touches_[i].param == param) return;
        }
        const bool already = macro_holds(macro_learn_, track, param);
        if (learn_touch_count_ >= kMacroTargets ||
            (!already && macro_count(engine_.macros().macros[macro_learn_]) +
                                 learn_new_targets() >= kMacroTargets)) {
            char *out = notice_buffer_;
            *out++ = char('A' + macro_learn_);
            append(out, " FULL: KNOB NOT LEARNT");
            *out = 0;
            notice(notice_buffer_);
            return;
        }
        learn_touches_[learn_touch_count_++] = {track, param, learn_byte(track, param)};
    }

    // Touched parameters the macro does not hold yet: the slots the commit will take.
    int learn_new_targets() const {
        int count = 0;
        for (int i = 0; i < learn_touch_count_; i++) {
            if (!macro_holds(macro_learn_, learn_touches_[i].track, learn_touches_[i].param)) {
                count++;
            }
        }
        return count;
    }

    void commit_learn() {
        for (int i = 0; i < learn_touch_count_; i++) {
            const LearnTouch &touch = learn_touches_[i];
            const uint8_t end = learn_byte(touch.track, touch.param);
            // Already on the macro: the first macro_assign takes it off, and the second puts it
            // back with the new range.
            if (macro_holds(macro_learn_, touch.track, touch.param)) {
                post_macro_assign(macro_learn_, touch.track, touch.param, 0, 0);
            }
            if (end != touch.start) {
                post_macro_assign(macro_learn_, touch.track, touch.param, touch.start, end);
            }
            // Back to the start. A master byte is commanded by a delta (HANDOFF, section 6);
            // two bytes of 0-127 apart always fit in its signed byte.
            if (touch.track == kMacroMasterTrack) {
                post(Cmd::set_master, 0, 0, uint8_t(kMasterEqFirst + touch.param),
                     uint8_t(int8_t(int(touch.start) - int(end))));
            } else {
                post(Cmd::set_param, touch.track, 0, touch.param, touch.start, 0);
            }
        }
    }

    void end_learn() {
        if (learn_touch_count_ > 0) {
            commit_learn();
            char *out = notice_buffer_;
            *out++ = char('A' + macro_learn_);
            append(out, ": ");
            append_number(out, uint32_t(learn_touch_count_));
            append(out, learn_touch_count_ == 1 ? " PARAM" : " PARAMS");
            *out = 0;
            notice(notice_buffer_);
        } else {
            notice("KNOBS EDIT AGAIN");
        }
        learn_touch_count_ = 0;
        macro_learn_ = -1;
        learn_ended_ = true;
        learn_ended_at_ms_ = last_ms_;
    }

    // "LEARN A 2/4 CLR: DONE", the line LEARN keeps on screen while it is on.
    void learn_line(char *line) const {
        char *out = line;
        append(out, "LEARN ");
        *out++ = char('A' + macro_learn_);
        *out++ = ' ';
        append_number(out, uint32_t(macro_count(engine_.macros().macros[macro_learn_])));
        append(out, "/4 CLR: DONE");
        *out = 0;
    }

    // What a turn of REL ALL or LEN ALL says: `REL ALL -12` (counts on every release byte) or
    // `LEN ALL 71%` (of each note's programmed length); at rest `REL ALL 0`, `LEN ALL 100%`.
    void move_all_notice(int which) {
        char *out = notice_buffer_;
        if (which == 0) {
            append(out, "REL ALL ");
            append_signed(out, rel_all_);
        } else {
            append(out, "LEN ALL ");
            append_number(out, uint32_t(len_all_percent(len_all_)));
            *out++ = '%';
        }
        *out = 0;
        // Logged too: tests/scenario_moveall.sh reads each click's amount off the log.
        notice_logged(notice_buffer_);
    }

    // ENERGY's notice, logged so a scenario can follow it: `ENERGY -12`, `ENERGY +40`, and at
    // rest `ENERGY 0`.
    void energy_notice() {
        char *out = notice_buffer_;
        append(out, "ENERGY ");
        append_signed(out, energy_);
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // TRANSPOSE (core/transpose.hpp): FUNC + SEL< / SEL> a step (a scale step, or a semitone
    // with the FUNC menu's row on SEMI), the other screen key held as well puts it back to 0.
    // Posted absolute with the mode; the notice is logged so a scenario can follow it:
    // `TRANSPOSE +2 IN KEY`, `TRANSPOSE +3 C` (SEMI: the key the parts are in), `TRANSPOSE 0`.
    void transpose_key(int delta) {
        const bool both = sel_left_down_ && sel_right_down_;
        transpose_ = both ? int8_t(0) : transpose_turn(transpose_, delta);
        post_transpose();
        transpose_text(transpose_, transpose_semis_, engine_.key_root(), notice_buffer_,
                       kTransposeTextMax);
        notice_logged(notice_buffer_);
    }

    void post_transpose() {
        post(Cmd::set_transpose, 0, 0, uint8_t(transpose_), transpose_semis_ ? 1 : 0);
    }

    // LIVE RND's notice, logged so a scenario can follow it: `LIVE RND4 8 TRK` (the unit and how
    // many tracks it moves), `LIVE RND OFF` at rest.
    void live_rnd_notice() {
        char *out = notice_buffer_;
        append(out, "LIVE ");
        if (rnd_unit_ == kLiveRndOff) {
            append(out, "RND OFF");
        } else {
            append(out, live_rnd_name(rnd_unit_));
            *out++ = ' ';
            append_number(out, uint32_t(live_rnd_count(rnd_armed_)));
            append(out, " TRK");
        }
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // FUNC + push D in PERFORM: track `track` in or out of LIVE RND (`T3 RND OUT 7 TRK`).
    void live_rnd_arm(int track) {
        rnd_armed_ = uint8_t(rnd_armed_ ^ (1u << track));
        post(Cmd::set_live_rnd, 0, 0, rnd_unit_, rnd_armed_);
        const bool in = ((rnd_armed_ >> track) & 1u) != 0;
        char *out = notice_buffer_;
        *out++ = 'T';
        append_number(out, uint32_t(track) + 1);
        append(out, in ? " RND IN " : " RND OUT ");
        append_number(out, uint32_t(live_rnd_count(rnd_armed_)));
        append(out, " TRK");
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // What MACRO LEARN shows while it waits: the macro, its count, and what to do.
    void learn_notice() {
        if (macro_learn_ < 0) return;
        char *out = notice_buffer_;
        append(out, "LEARN ");
        *out++ = char('A' + macro_learn_);
        *out++ = ' ';
        append_number(out, uint32_t(macro_count(engine_.macros().macros[macro_learn_])));
        append(out, "/4 TURN KNOBS");
        *out = 0;
        notice(notice_buffer_);
    }

    void draw_perform(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        uint8_t *px = canvas.data();
        const bool running = enaga::clock::state().running != 0;
        const uint8_t rnd = engine_.live_rnd_unit();

        // PERFORM as the mockup draws it (tools/visuals_mockup.py, docs/CONTROLS.md "PERFORM"):
        // rows 19-41 four macro gauges, 43-49 their letters and what they move, 52 OUT, 55-89 the
        // output's scope in an afterglow, 92-118 every track's level with its machine's name. The
        // morph is in the live strip (S40), a scene and MUTATE say themselves in a toast when
        // pressed, and the FUNC help has the gestures. Stopped, the scope's rows hold the lines a
        // hand needs before it plays.

        // The four macros: a gauge each, the value inside, the letter as a badge under it (the
        // selected one lit) and how many parameters it drives. An empty macro's gauge is dim.
        for (int m = 0; m < kMacros; m++) {
            const int x = m * 31;
            const int cx = x + 15;
            const int targets = macro_count(engine_.macros().macros[m]);
            draw_gauge(canvas, cx, 30, 11, int(engine_.macro_position(m)),
                       int(macro_position_[m]), targets != 0);
            const bool picked = m == macro_selected_;
            canvas.rect(cx - 11, 43, 7, 7, picked ? 15 : 4);
            const char letter[2] = {char('A' + m), 0};
            paint::small_text(px, cx - 9, 44, letter, picked ? 0 : 13);
            // What it moves: its first target's parameter, on that target's own track, and a
            // '+' when it moves more than one.
            const char *what = "---";
            for (int k = 0; k < kMacroTargets; k++) {
                const MacroTarget &target = engine_.macros().macros[m].targets[k];
                if (!macro_target_usable(target)) continue;
                what = track_param_name_on(target.track, target.param);
                break;
            }
            out = line;
            append(out, what);
            if (targets > 1) *out++ = '+';
            *out = 0;
            paint::small_text(px, cx - 2, 44, line, targets == 0 ? 5 : 11);
        }

        // The output, bright, in an afterglow, while the beat plays; and the little that changes
        // while it does, in the small font over its corner.
        if (running) {
            draw_scope(canvas, 0, 55, 15, running);
            paint::small_text(px, 0, 52, "OUT", 7);
            out = line;
            *out = 0;
            if (engine_.fill()) append(out, " FILL");
            if (engine_.repeat().active()) {
                *out++ = ' ';
                append(out, repeat_length_name(repeat_length_));
            }
            const int throwing = engine_.throwing();
            if (throwing == 1) append(out, " DLY");
            if (throwing == 2) append(out, " REV");
            if (note_repeat_mode_) append(out, " NRPT");
            if (engine_.delay_frozen()) append(out, " FRZ");
            if (engine_.energy() != 0) {
                append(out, " EN");
                append_signed(out, engine_.energy());
            }
            if (engine_.transpose() != 0) {
                append(out, " TR");
                append_signed(out, engine_.transpose());
            }
            *out = 0;
            paint::small_text(px, 16, 52, line, 9);
            if (engine_.rel_all() != 0 || engine_.len_all() != 0) {
                out = line;
                append(out, "REL ");
                append_signed(out, engine_.rel_all());
                append(out, " LEN ");
                append_number(out, uint32_t(len_all_percent(engine_.len_all())));
                *out++ = '%';
                *out = 0;
                paint::small_text(px, 123 - paint::small_width(line), 52, line, 12);
            }
        } else {
            draw_scope(canvas, 0, 55, 15, running);           // clears the glow
            // REL ALL, LEN ALL and LIVE RND where they are, or the gesture while all are at rest.
            // At most "REL -64 LEN 400% R16", 20 characters.
            if (engine_.rel_all() != 0 || engine_.len_all() != 0) {
                out = line;
                append(out, "REL ");
                append_signed(out, engine_.rel_all());
                append(out, " LEN ");
                append_number(out, uint32_t(len_all_percent(engine_.len_all())));
                *out++ = '%';
                if (rnd != kLiveRndOff) {
                    append(out, " R");
                    append(out, live_rnd_name(rnd) + 3);       // "RND4" -> "4"
                }
                *out = 0;
                canvas.text(0, 57, line, 12);
            } else if (rnd != kLiveRndOff) {
                out = line;
                append(out, "LIVE ");
                append(out, live_rnd_name(rnd));
                append(out, " FUNC+D");
                *out = 0;
                canvas.text(0, 57, line, 12);
            } else {
                canvas.text(0, 57, "FUNC+A/B REL/LEN ALL", 7);
            }
            // The fill and AUTO-FILL where they are on, or where AUTO-FILL lives.
            if (engine_.fill() || engine_.auto_fill() != AutoFill::off) {
                out = line;
                if (engine_.fill()) append(out, "FILL ");
                if (engine_.auto_fill() != AutoFill::off) {
                    append(out, "AF");
                    append(out, auto_fill_name(engine_.auto_fill()));
                }
                *out = 0;
                canvas.text(0, 66, line, 9);
            } else {
                canvas.text(0, 66, "AUTOFILL: FUNC MENU", 7);
            }
            // The bar, ENERGY and TRANSPOSE away from rest and the held modes: "BAR 1000 EN-64
            // TR-12" is 20 characters, so NRPT and FRZ go on only while they fit the line.
            out = line;
            append(out, "BAR ");
            append_number(out, engine_.bar() % 1000u + 1u);
            if (engine_.energy() != 0) {
                append(out, " EN");
                append_signed(out, engine_.energy());
            }
            if (engine_.transpose() != 0) {
                append(out, " TR");
                append_signed(out, engine_.transpose());
            }
            if (note_repeat_mode_ && out - line + 5 <= kLineChars) append(out, " NRPT");
            if (engine_.delay_frozen() && out - line + 4 <= kLineChars) append(out, " FRZ");
            *out = 0;
            canvas.text(0, 75, line, 9);
        }

        // Every track's level: eight bars, its machine's name under each.
        draw_track_columns(canvas, 92, 20, rnd != kLiveRndOff ? engine_.live_rnd_armed() : 0);
    }
