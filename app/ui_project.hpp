// PROJECT: banks and slots, the pattern menu, CHAIN, PATTERN and SETUP.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- PROJECT: banks, patterns and the card ---------------------------

    // True when the key belonged to this mode. OK saves, F1 + OK reads the card again, F1 + CLR
    // starts an empty project, CLR cancels a queued change and UP toggles the chain.
    //
    // There is no file browser: a project's name comes from the settings and a track's sample
    // path comes from its kit, which is what docs/SPEC.md's "reopens the last project" and
    // "loaded whole into RAM at project open" need. Picking a different project or a different
    // sample from the card is a browser, and a browser is not in M4.
    bool on_project_key(int key) {
        if (project_page_ == ProjectPage::chain && on_chain_key(key)) return true;
        if (project_page_ == ProjectPage::pattern && on_pattern_page_key(key)) return true;
        if (project_page_ == ProjectPage::files && on_projects_key(key)) return true;
        switch (key) {
            case ENAGA_KEY_OK:
                // F1 + OK is redo here as everywhere (docs/SPEC.md); the shared handler has it.
                if (shift_) return false;
                // On the slots OK is on_slot_menu_key's: a tap queues the slot under the cursor
                // on its release, as OK picks in every Enaga list; a hold opens the pattern menu.
                return true;

            case ENAGA_KEY_CLR:
                // F1 + CLR is undo, likewise.
                if (shift_) return false;
                if (project_page_ != ProjectPage::slots) {
                    project_page_ = ProjectPage::slots;   // back, from any other page
                    notice("PROJECT");
                } else {
                    post(Cmd::cancel_queue);
                    notice("QUEUE OFF");
                }
                return true;

            // The arrows move the slot cursor. With FUNC, on the slots, they are the pattern
            // under the cursor (backlog [2]), as FUNC + UP / DOWN copy and paste on the editing
            // screens: UP copies it, DOWN pastes over it, LEFT clears it. Paste and clear throw
            // a pattern away, so each takes its key twice. Chain on/off, the sample reload,
            // RELOAD and NEW are in the FUNC menu (docs/CONTROLS.md).
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (!shift_) {
                    move_slot_cursor(key == ENAGA_KEY_UP ? -4 : +4);
                } else if (project_page_ == ProjectPage::slots) {
                    if (key == ENAGA_KEY_UP) slot_copy(); else slot_paste();
                }
                return true;

            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (!shift_) {
                    move_slot_cursor(key == ENAGA_KEY_LEFT ? -1 : +1);
                } else if (key == ENAGA_KEY_LEFT && project_page_ == ProjectPage::slots) {
                    slot_clear();
                }
                return true;

            default:
                return false;
        }
    }

    // The FUNC menu's OK-twice rows. `clear` is not a card action -- it empties the playing
    // pattern -- but it is armed the same way, so it shares the one arm.
    enum class CardAction : uint8_t { none, reload, fresh, clear, demo };

    // True on the second press, when the action has been asked for.
    bool arm_card_action(CardAction action) {
        if (card_armed_ == action && uint32_t(last_ms_ - card_armed_at_ms_) < kSaveArmMs) {
            card_armed_ = CardAction::none;
            if (action == CardAction::reload) {
                storage_.request_reload();
                notice("RELOADING");
            } else if (action == CardAction::clear) {
                clear_pattern();
            } else if (action == CardAction::demo) {
                storage_.request_new(false);
                notice("DEMO PROJECT");
            } else {
                storage_.request_new(true);
                notice("NEW PROJECT");
            }
            return true;
        }
        card_armed_ = action;
        card_armed_at_ms_ = last_ms_;
        notice(action == CardAction::reload  ? "OK AGAIN TO RELOAD"
               : action == CardAction::clear ? "OK AGAIN TO CLEAR"
               : action == CardAction::demo  ? "OK AGAIN FOR DEMO"
                                             : "OK AGAIN FOR NEW");
        return false;
    }

    // Every track's trigs and locks in the playing pattern, as one undo level: CLR once per
    // track was the only way before. The kit (the sounds), and each track's length, speed and
    // swing stay, as a track CLR keeps them (core/editops.hpp, pattern_erase_steps).
    void clear_pattern() {
        mark(GestureKind::erase, selected_, 0);
        post(Cmd::pattern_erase);
        end_gesture();
        notice("CLEARED FUNC+CLR UNDO");
    }

    // --- PROJECT's slots: copy, paste and clear a whole pattern -----------------------------

    enum class SlotAction : uint8_t { none, paste, clear, twice };

    // "A03" for the slot under the cursor in the shown bank.
    void append_slot_name(char *&out, int bank, int slot) {
        *out++ = bank_letter(bank);
        append_padded(out, uint32_t(slot) + 1, 2);
    }

    // The second press of the same action on the same slot within kSaveArmMs.
    bool arm_slot_action(SlotAction action) {
        const bool again = slot_armed_ == action && slot_armed_bank_ == shown_bank_ &&
                           slot_armed_slot_ == slot_cursor_ &&
                           uint32_t(last_ms_ - slot_armed_at_ms_) < kSaveArmMs;
        slot_armed_ = again ? SlotAction::none : action;
        slot_armed_bank_ = shown_bank_;
        slot_armed_slot_ = slot_cursor_;
        slot_armed_at_ms_ = last_ms_;
        return again;
    }

    void slot_copy() {
        clip_bank_ = shown_bank_;
        clip_slot_ = slot_cursor_;
        char *out = notice_buffer_;
        append_slot_name(out, clip_bank_, clip_slot_);
        append(out, " COPIED");
        *out = 0;
        notice(notice_buffer_);
    }

    // True when it is finished (the second press, or nothing to do), so the menu can close.
    bool slot_paste() {
        if (clip_slot_ < 0) {
            notice("COPY A PATTERN FIRST");
            return true;
        }
        if (clip_bank_ == shown_bank_ && clip_slot_ == slot_cursor_) {
            notice("SAME PATTERN");
            return true;
        }
        char *out = notice_buffer_;
        const bool run = arm_slot_action(SlotAction::paste);
        if (!run) {
            append(out, "AGAIN: PASTE TO ");
            append_slot_name(out, shown_bank_, slot_cursor_);
        } else {
            Command command;
            command.cmd = Cmd::pattern_copy;
            command.track = uint8_t(shown_bank_);
            command.step = uint8_t(slot_cursor_);
            command.a = uint8_t(clip_bank_);
            command.b = uint8_t(clip_slot_);
            post_command(command);
            append_slot_name(out, clip_bank_, clip_slot_);
            append(out, " PASTED TO ");
            append_slot_name(out, shown_bank_, slot_cursor_);
        }
        *out = 0;
        notice(notice_buffer_);
        return run;
    }

    bool slot_clear() {
        char *out = notice_buffer_;
        const bool run = arm_slot_action(SlotAction::clear);
        if (!run) {
            append(out, "AGAIN: CLEAR ");
            append_slot_name(out, shown_bank_, slot_cursor_);
        } else {
            post(Cmd::pattern_clear, uint8_t(shown_bank_), uint8_t(slot_cursor_));
            append_slot_name(out, shown_bank_, slot_cursor_);
            append(out, " CLEARED");
        }
        *out = 0;
        notice(notice_buffer_);
        return run;
    }

    // DOUBLE: every track twice as long (up to 64), its steps copied into the new half.
    bool slot_double() {
        char *out = notice_buffer_;
        const bool run = arm_slot_action(SlotAction::twice);
        if (!run) {
            append(out, "AGAIN: DOUBLE ");
            append_slot_name(out, shown_bank_, slot_cursor_);
        } else {
            post(Cmd::pattern_double, uint8_t(shown_bank_), uint8_t(slot_cursor_));
            append_slot_name(out, shown_bank_, slot_cursor_);
            append(out, " DOUBLED");
        }
        *out = 0;
        notice(notice_buffer_);
        return run;
    }

    // --- the pattern menu: hold OK on a slot ------------------------------------------------
    //
    // A short list for the pattern under the cursor (Roland, 2026-11-25): COPY, PASTE, CLEAR,
    // DOUBLE. UP/DOWN pick, OK runs (PASTE, CLEAR and DOUBLE take OK twice), CLR closes. A tap
    // on OK still queues the pattern: the list waits for a hold of kSlotMenuHoldMs, and a tap
    // queues on its release. FUNC + UP / DOWN / LEFT stay as the shortcuts.
    static constexpr uint32_t kSlotMenuHoldMs = 500;
    static constexpr int kSlotMenuRows = 4;
    static const char *slot_menu_name(int row) {
        switch (row) {
            case 0: return "COPY";
            case 1: return "PASTE";
            case 2: return "CLEAR";
            default: return "DOUBLE";
        }
    }

    bool slot_ok_target() const {
        return mode_ == Mode::project && project_page_ == ProjectPage::slots && !shift_;
    }

    // True when the key was the menu's, or the OK hold's.
    bool on_slot_menu_key(int key, bool down) {
        if (slot_menu_) {
            const bool mine = key == ENAGA_KEY_OK || key == ENAGA_KEY_UP ||
                              key == ENAGA_KEY_DOWN || key == ENAGA_KEY_CLR;
            if (!down || !mine) return mine;
            if (key == ENAGA_KEY_CLR) {
                slot_menu_ = false;
                notice("PROJECT");
                return true;
            }
            if (key != ENAGA_KEY_OK) {
                slot_menu_cursor_ = (slot_menu_cursor_ + (key == ENAGA_KEY_UP ? -1 : 1) +
                                     kSlotMenuRows) % kSlotMenuRows;
                slot_armed_ = SlotAction::none;
                return true;
            }
            bool done = true;
            switch (slot_menu_cursor_) {
                case 0: slot_copy(); break;
                case 1: done = slot_paste(); break;
                case 2: done = slot_clear(); break;
                default: done = slot_double(); break;
            }
            if (done) slot_menu_ = false;
            return true;
        }
        if (key != ENAGA_KEY_OK || !slot_ok_target()) return false;
        if (down) {
            slot_ok_held_ = true;
            slot_ok_down_ms_ = last_ms_;
        } else if (slot_ok_held_) {
            slot_ok_held_ = false;
            queue_shown_pattern(slot_cursor_);   // a tap: what OK always did here
        }
        return true;
    }

    // update(): a held OK opens the menu.
    void service_slot_menu() {
        if (slot_menu_ && (mode_ != Mode::project || project_page_ != ProjectPage::slots)) {
            slot_menu_ = false;
        }
        if (!slot_ok_held_) return;
        if (!slot_ok_target()) {
            slot_ok_held_ = false;
            return;
        }
        if (uint32_t(last_ms_ - slot_ok_down_ms_) < kSlotMenuHoldMs) return;
        slot_ok_held_ = false;
        slot_menu_ = true;
        slot_menu_cursor_ = 0;
        slot_armed_ = SlotAction::none;
        char *out = notice_buffer_;
        append(out, "PATTERN ");
        append_slot_name(out, shown_bank_, slot_cursor_);
        *out = 0;
        notice(notice_buffer_);
    }

    void draw_slot_menu(enaga::Canvas &canvas) {
        char line[24];
        char *out = line;
        append(out, "PATTERN ");
        append_slot_name(out, shown_bank_, slot_cursor_);
        *out = 0;
        canvas.text(0, 18, line, 14);
        for (int row = 0; row < kSlotMenuRows; row++) {
            const bool here = row == slot_menu_cursor_;
            out = line;
            *out++ = here ? '>' : ' ';
            append(out, slot_menu_name(row));
            if (row == 1 && clip_slot_ >= 0) {
                append(out, " ");
                append_slot_name(out, clip_bank_, clip_slot_);
            }
            *out = 0;
            canvas.text(0, 29 + row * 9, line, here ? 15 : 9);
        }
        canvas.text(0, 103, "OK: RUN   CLR: BACK", 7);
    }

    // The cursor over PROJECT's sixteen slots, drawn four by four: LEFT and RIGHT step through
    // them in reading order, UP and DOWN by a row. It stops at the ends rather than wrapping,
    // because the bank is on the selection keys and wrapping would hide where the edge is.
    void move_slot_cursor(int delta) {
        if (project_page_ != ProjectPage::slots) return;
        const int next = slot_cursor_ + delta;
        if (next >= 0 && next < kPatternsPerBank) slot_cursor_ = next;
    }

    void queue_shown_pattern(int slot) {
        if (!project_index_valid(shown_bank_, slot)) return;
        // The audio side decides when it lands, from the tick the sequencer is actually on. With
        // the transport stopped it switches at once, because there is no end of a pass to wait
        // for (core/queue.hpp, and Engine::apply's queue_pattern).
        Command command;
        command.cmd = Cmd::queue_pattern;
        command.a = uint8_t(shown_bank_);
        command.b = uint8_t(slot);
        command.c = uint8_t(queue_mode_);
        post_command(command);

        char *out = notice_buffer_;
        *out++ = bank_letter(shown_bank_);
        append_padded(out, uint32_t(slot) + 1, 2);
        append(out, " ");
        append(out, queue_mode_name(queue_mode_));
        *out = 0;
        notice(notice_buffer_);
    }

    // --- PROJECT's CHAIN page ---------------------------------------------
    //
    // The chain as a list, one entry per row: UP and DOWN move through the entries, LEFT and
    // RIGHT step the entry's pattern through the 128, a STEP key sets it to that slot of the
    // shown bank, VALUE (or A) sets how many times it repeats. OK adds an entry after the cursor
    // -- a copy of the one there, or the playing pattern when the chain is empty -- and F1 + LEFT
    // removes the one under the cursor. CLR goes back. F1 + UP is still CHAIN ON/OFF.
    // Song mode: B sets the entry's mute recall (`--`, or M1-M8, a stored mute state recalled as
    // the entry starts), and F1 + RIGHT switches SONG (play once, then stop) and LOOP.
    //
    // The entries are the audio side's (it plays them), so every edit is a command, and the
    // knob and arrow edits are deltas applied there. Chain edits are not undoable: core/undo
    // snapshots the pattern, and the chain is not in a pattern.

    int chain_entries() const { return chain_length(engine_.project()); }

    void chain_clamp_cursor() {
        const int length = chain_entries();
        if (chain_cursor_ >= length) chain_cursor_ = length - 1;
        if (chain_cursor_ < 0) chain_cursor_ = 0;
    }

    void post_chain(ChainEdit edit, int at, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0,
                    uint8_t e = 0) {
        Command command;
        command.cmd = Cmd::chain_edit;
        command.step = uint8_t(at);
        command.a = uint8_t(edit);
        command.b = b;
        command.c = c;
        command.d = d;
        command.e = e;
        post_command(command);
    }

    void chain_delta(ChainEdit edit, int delta) {
        if (chain_entries() == 0) {
            notice("CHAIN EMPTY");
            return;
        }
        chain_clamp_cursor();
        post_chain(edit, chain_cursor_, uint8_t(int8_t(clamp_delta(delta))));
    }

    void chain_put_slot(int slot) {
        const int length = chain_entries();
        if (length == 0) {
            post_chain(ChainEdit::insert, 0, uint8_t(shown_bank_), uint8_t(slot), 1);
            chain_cursor_ = 0;
        } else {
            chain_clamp_cursor();
            const ChainEntry &entry = engine_.project().chain[chain_cursor_];
            post_chain(ChainEdit::set, chain_cursor_, uint8_t(shown_bank_), uint8_t(slot),
                       entry.repeats, entry.mutes);
        }
        char *out = notice_buffer_;
        *out++ = bank_letter(shown_bank_);
        append_padded(out, uint32_t(slot) + 1, 2);
        *out = 0;
        notice(notice_buffer_);
    }

    bool on_chain_key(int key) {
        const int length = chain_entries();
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;                 // F1 + OK is redo
                if (length >= kChainEntries) {
                    notice("CHAIN FULL");
                    return true;
                }
                if (length == 0) {
                    const Status &status = engine_.status();
                    post_chain(ChainEdit::insert, 0,
                               status.live_bank.load(std::memory_order_relaxed),
                               status.live_slot.load(std::memory_order_relaxed), 1);
                    chain_cursor_ = 0;
                } else {
                    chain_clamp_cursor();
                    const ChainEntry entry = engine_.project().chain[chain_cursor_];
                    post_chain(ChainEdit::insert, chain_cursor_ + 1, entry.bank, entry.slot,
                               entry.repeats, entry.mutes);
                    chain_cursor_++;
                }
                notice("ENTRY ADDED");
                return true;
            }
            case ENAGA_KEY_CLR: {
                if (shift_) return false;                 // F1 + CLR is undo
                // CLR clears, as it clears a step or a track: here the entry under the cursor.
                // An empty chain has nothing to clear, so CLR is "back" there as on the other
                // PROJECT pages. A removal is not undoable, so it takes a second press within
                // kSaveArmMs on the same entry (Roland, 2026-11-22).
                if (length == 0) return false;
                chain_clamp_cursor();
                if (chain_remove_armed_ != chain_cursor_ ||
                    uint32_t(last_ms_ - chain_remove_armed_at_ms_) >= kSaveArmMs) {
                    chain_remove_armed_ = chain_cursor_;
                    chain_remove_armed_at_ms_ = last_ms_;
                    notice("AGAIN TO REMOVE");
                    return true;
                }
                chain_remove_armed_ = -1;
                post_chain(ChainEdit::remove, chain_cursor_);
                if (chain_cursor_ > 0 && chain_cursor_ >= length - 1) chain_cursor_--;
                notice("ENTRY REMOVED");
                return true;
            }
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (!shift_) {
                    chain_cursor_ += key == ENAGA_KEY_UP ? -1 : 1;
                    chain_clamp_cursor();
                }
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                // FUNC + LEFT / RIGHT were remove and SONG/LOOP: CLR twice and the FUNC menu now.
                if (!shift_) chain_delta(ChainEdit::pattern_delta, key == ENAGA_KEY_LEFT ? -1 : 1);
                return true;
            default:
                return false;
        }
    }

    void draw_chain(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Project &project = engine_.project();
        const int length = chain_length(project);
        const ChainPlayer &player = engine_.chain();

        append(out, "CHAIN ");
        append_number(out, uint32_t(length));
        append(out, "/64");
        append(out, player.active ? " ON" : " OFF");
        append(out, project.settings.chain_song ? " SONG" : " LOOP");
        *out = 0;
        canvas.text(0, 18, line, 15);

        if (length == 0) {
            canvas.text(0, 40, "EMPTY", 11);
            canvas.text(0, 52, "OK ADDS THE PLAYING", 9);
            canvas.text(0, 61, "PATTERN. STEP KEYS", 9);
            canvas.text(0, 70, "ADD FROM THIS BANK", 9);
        } else {
            // Seven rows, scrolled so the cursor stays on screen.
            constexpr int kRows = 7;
            int first = chain_cursor_ - kRows / 2;
            if (first > length - kRows) first = length - kRows;
            if (first < 0) first = 0;
            for (int row = 0; row < kRows && first + row < length; row++) {
                const int i = first + row;
                const ChainEntry &entry = project.chain[i];
                out = line;
                *out++ = i == chain_cursor_ ? '>' : ' ';
                append_padded(out, uint32_t(i) + 1, 2);
                *out++ = ' ';
                *out++ = bank_letter(int(entry.bank));
                append_padded(out, uint32_t(entry.slot) + 1, 2);
                append(out, " X");
                append_number(out, uint32_t(entry.repeats));
                if (entry.mutes != 0) {
                    append(out, " M");
                    append_number(out, uint32_t(entry.mutes));
                }
                if (player.active && !player.orphaned && int(player.entry) == i) {
                    append(out, " PLAY");
                }
                *out = 0;
                canvas.text(0, 30 + row * 10, line, i == chain_cursor_ ? 15 : 10);
            }
        }

        canvas.text(0, 103, "OK ADD  CLR X2 REMOVE", 7);
        canvas.text(0, 112, "A:REPEATS B:MUTES", 7);
    }

    // --- PROJECT's PATTERN page: the master length and the swing ----------

    // The fields, in the order LEFT and RIGHT walk them: LEN, SWG, then each track's own swing.
    static constexpr int kPatternFieldFeel = 2;
    static constexpr int kPatternFieldTrackSwing = 3;
    static constexpr int kPatternFields = kPatternFieldTrackSwing + kTracks;

    void pattern_field_delta(int field, int delta) {
        if (field == 0) {
            mark(GestureKind::length, 0, 0);
            post(Cmd::set_master_length, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
            notice("LEN");
        } else if (field == 1) {
            mark(GestureKind::length, 0, 0);
            post(Cmd::set_pattern_swing, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
            notice("SWING");
        } else if (field == kPatternFieldFeel) {
            // FEEL (core/timing.hpp): OFF, DRIFT 1-6, WAVE 1-6 along one line, a pattern setting
            // like SWG, so one gesture is one undo level.
            mark(GestureKind::length, 0, 0);
            post(Cmd::set_pattern_feel, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
            // Where it lands, worked out from the byte the screen has: the engine applies the
            // same delta to the same byte. Logged, for the emulator, which cannot read the page.
            const uint8_t feel = feel_step(engine_.pattern().feel, delta);
            char *out = notice_buffer_;
            append(out, "FEEL ");
            append(out, feel_mode_name(feel));
            if (feel_depth(feel) != 0) {
                *out++ = ' ';
                append_number(out, uint32_t(feel_depth(feel)));
            }
            *out = 0;
            notice_logged(notice_buffer_);
        } else if (field < kPatternFields) {
            // A track's own swing (docs/SPEC.md, "an optional per-track override"). PAT, then
            // 50 to 75; the engine walks the delta (core/pattern.hpp, `track_swing_step`). One
            // gesture per track, so a sweep is one undo level, as LEN and SWG are.
            const int track = field - kPatternFieldTrackSwing;
            mark(GestureKind::length, track, 0);
            post(Cmd::set_track_swing, uint8_t(track), 0, 0, clamp_delta(delta), kDeltaFlag);
            notice("TRACK SWING");
        }
    }

    bool on_pattern_page_key(int key) {
        switch (key) {
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (shift_) return true;                  // F1 + LEFT/RIGHT: free
                // Sideways through LEN, SWG and the eight track swings, stopping at the ends.
                pattern_field_ = clamp_int(pattern_field_ + (key == ENAGA_KEY_LEFT ? -1 : 1), 0,
                                           kPatternFields - 1);
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) return true;                  // F1 + UP/DOWN: nothing to copy here
                pattern_field_delta(pattern_field_, key == ENAGA_KEY_UP ? 1 : -1);
                end_gesture();
                return true;
            case ENAGA_KEY_OK:
                return !shift_;                           // nothing to pick here
            default:
                return false;
        }
    }

    void draw_pattern_page(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();
        const Pattern &pattern = engine_.pattern();

        append(out, "PATTERN ");
        *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        append_padded(out, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
        *out = 0;
        canvas.text(0, 18, line, 15);

        out = line;
        *out++ = pattern_field_ == 0 ? '>' : ' ';
        append(out, "LEN ");
        append_number(out, uint32_t(pattern.master_length));
        append(out, " STEPS");
        *out = 0;
        canvas.text(0, 29, line, pattern_field_ == 0 ? 15 : 10);

        out = line;
        *out++ = pattern_field_ == 1 ? '>' : ' ';
        append(out, "SWG ");
        append_number(out, uint32_t(pattern.swing));
        *out++ = '%';
        *out = 0;
        canvas.text(0, 38, line, pattern_field_ == 1 ? 15 : 10);

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
        canvas.text(0, 47, line,
                    pattern_field_ == kPatternFieldFeel ? 15 : (pattern.feel == 0 ? 8 : 10));

        // Each track's own swing, two to a row: PAT follows SWG above, a number overrides it.
        canvas.text(0, 57, "TRACK SWING", 8);
        for (int t = 0; t < kTracks; t++) {
            const int field = kPatternFieldTrackSwing + t;
            const uint8_t swing = pattern.tracks[t].swing;
            out = line;
            *out++ = pattern_field_ == field ? '>' : ' ';
            append_number(out, uint32_t(t + 1));
            *out++ = ' ';
            if (swing == kSwingInherit) {
                append(out, "PAT");
            } else {
                append_number(out, uint32_t(swing));
                *out++ = '%';
            }
            *out = 0;
            canvas.text((t & 1) * 60, 66 + (t >> 1) * 9, line,
                        pattern_field_ == field ? 15 : (swing == kSwingInherit ? 8 : 12));
        }

        canvas.text(0, 103, "LEFT RIGHT PICK", 7);
        canvas.text(0, 112, "UP DOWN VALUE A-D", 7);
    }

    // --- PROJECT's SETUP page: the clock, MIDI and the record quantize ----

    static const char *clock_source_name(int source) {
        switch (source) {
            case ENAGA_CLOCK_INTERNAL: return "INTERNAL";
            case ENAGA_CLOCK_MIDI:     return "MIDI";
            case ENAGA_CLOCK_SYNC:     return "SYNC IN";
            default:                   return "?";
        }
    }

    void draw_setup(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const enaga::clock::State clock = enaga::clock::state();

        canvas.text(0, 20, "SETUP", 14);
        // What a frame costs to draw, averaged (count_draw_time): "DRW 0.85MS" ends at x 123.
        out = line;
        append(out, "DRW ");
        append_hundredths(out, draw_hundredths_);
        append(out, "MS");
        *out = 0;
        canvas.text(64, 20, line, 7);

        // CLOCK SRC, read from the clock itself rather than from anything STRATA remembers: it
        // is the user's setting and it can change under us at any moment.
        out = line;
        append(out, "CLOCK SRC ");
        append(out, clock_source_name(int(clock.source)));
        *out = 0;
        canvas.text(0, 34, line, 13);

        // The tempo, and whose it is. Under an outside source STRATA does not own it, so the
        // number is shown as a measurement rather than as a setting (docs/SPEC.md, and
        // CLAUDE.md: "STRATA does not own the tempo").
        out = line;
        append(out, "TEMPO ");
        append_tenths(out, uint32_t(clock.tempo * 10.0f + 0.5f));
        append(out, int(clock.source) == ENAGA_CLOCK_INTERNAL ? " OURS" : " EXT");
        *out = 0;
        canvas.text(0, 46, line, int(clock.source) == ENAGA_CLOCK_INTERNAL ? 13 : 10);

        // MIDI CH, also the user's.
        out = line;
        append(out, "MIDI CH ");
        append_number(out, uint32_t(enaga::midi::channel() + 1));
        *out = 0;
        canvas.text(0, 58, line, 13);

        // The record quantize amount, which IS ours and is on encoder A.
        out = line;
        append(out, "QUANTIZE ");
        append_number(out, uint32_t(quantize()));
        *out = 0;
        canvas.text(0, 70, line, 15);
        const int bar = quantize() * 60 / 100;
        canvas.rect(0, 80, 60, 5, 3);
        if (bar > 0) canvas.rect(0, 80, bar, 5, 12);

        // What the frame's bottom line said before the tabs took it: the tracks sounding, the
        // held CPU maximum (since the last PLAY) and the device.
        out = line;
        append(out, "TRK");
        append_number(out, engine_.status().voices.load(std::memory_order_relaxed));
        append(out, " MAX");
        append_tenths(out, cpu_max_);
        append(out, " ");
        append(out, device_name(enaga::system::device()));
        *out = 0;
        canvas.text(0, 86, line, 9);

        // And where the three settings above are changed, because they are not changed here.
        // CLOCK OUT and SYNC OUT are in the same menu and Enaga exposes no way to read them,
        // so they are named rather than shown with a state this screen would be guessing at.
        // No commas: the font has none, and each drew as '?'.
        canvas.text(0, 94, "CLK SRC/OUT SYNC OUT", 7);
        canvas.text(0, 103, "MIDI CH: SET IN", 7);
        canvas.text(0, 112, "FUNC+POWER > SETTINGS", 7);
    }

    void draw_project(enaga::Canvas &canvas) {
        if (project_setup()) {
            draw_setup(canvas);
            return;
        }
        if (project_page_ == ProjectPage::chain) {
            draw_chain(canvas);
            return;
        }
        if (project_page_ == ProjectPage::pattern) {
            draw_pattern_page(canvas);
            return;
        }
        if (project_page_ == ProjectPage::files) {
            draw_projects(canvas);
            return;
        }
        if (slot_menu_) {
            draw_slot_menu(canvas);
            return;
        }
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        // The project, and whether RAM differs from the card. An unsaved project that looked
        // saved is the worst outcome this screen can produce, so the marker is on the name.
        //
        // The mark is "UNSAVED" on the status line below rather than a '*' after the name: the
        // font has no '*', so it drew as '?'. The name is the card's, so it is cut to fit and
        // shown in characters the font has.
        const bool usable = storage_.card_usable();
        out = line;
        append_shown(out, storage_.project_name(), usable ? kLineChars : kLineChars - 8);
        if (!usable) append(out, " NO CARD");
        *out = 0;
        canvas.text(0, 18, line, 15);

        // What the card side is doing, or what went wrong last. Both matter: a load that failed
        // silently is the least diagnosable thing a sampler can do.
        out = line;
        const char *busy = storage_.busy_text();
        if (busy != nullptr) {
            append(out, busy);
            append(out, " ");
            append_number(out, uint32_t(storage_.progress()));
            *out++ = '%';
        } else if (storage_.last_error() != nullptr) {
            append_shown(out, storage_.last_error(), kLineChars);
        } else if (storage_.dirty()) {
            append(out, "UNSAVED");
        }
        *out = 0;
        canvas.text(0, 27, line, busy != nullptr || storage_.dirty() ? 12 : 9);

        // The global key and scale, on encoders B and C. Drawn from the live PATTERN rather than
        // from the project settings, because the pattern's copy is what the sequencer and every
        // mutation actually fold into -- a pattern loaded in its own key must read as that key.
        out = line;
        append(out, "KEY ");
        append(out, note_name(engine_.key_root()));
        *out++ = ' ';
        append(out, scale_name(int(engine_.key_scale())));
        *out = 0;
        canvas.text(0, 103, line, 11);   // below the slots: at 94 it ran through their last row

        // The bank being browsed, which is not necessarily the one playing.
        out = line;
        append(out, "BANK ");
        *out++ = bank_letter(shown_bank_);
        append(out, "   ");
        append(out, queue_mode_name(queue_mode_));
        if (engine_.chain().active) append(out, " CHAIN");
        *out = 0;
        canvas.text(0, 40, line, 12);

        // The bank's 16 slots, four by four. Filled = the pattern has trigs; the live one is
        // boxed and a queued one is marked, so "what is playing" and "what will play" are both
        // on screen at once.
        const int live_bank = int(status.live_bank.load(std::memory_order_relaxed));
        const int live_slot = int(status.live_slot.load(std::memory_order_relaxed));
        const Queue &queue = engine_.queue();
        const Project &project = engine_.project();

        for (int slot = 0; slot < kPatternsPerBank; slot++) {
            const int x = (slot % 4) * 30 + 2;
            const int y = (slot / 4) * 13 + 51;

            int trigs = 0;
            const Pattern &pattern = project.patterns[shown_bank_][slot];
            for (int t = 0; t < kTracks && trigs == 0; t++) {
                for (int i = 0; i < int(pattern.tracks[t].length); i++) {
                    if (pattern.tracks[t].steps[i].flags & (kStepTrig | kStepTrigless)) {
                        trigs = 1;
                        break;
                    }
                }
            }

            const bool is_live = shown_bank_ == live_bank && slot == live_slot;
            const bool is_queued = queue.pending && int(queue.bank) == shown_bank_ &&
                                   int(queue.slot) == slot;
            if (slot == slot_cursor_) {
                // The cursor: a frame round the slot, in the gap between slots, so it never
                // touches the number inside.
                canvas.rect(x - 1, y - 1, 28, 13, 15);
                canvas.rect(x, y, 26, 11, 0);
            }
            canvas.rect(x, y, 26, 11, is_live ? 10 : (trigs ? 5 : 2));
            out = line;
            append_padded(out, uint32_t(slot) + 1, 2);
            if (is_queued) *out++ = '>';
            *out = 0;
            canvas.text(x + 3, y + 2, line, is_live ? 0 : 15);
        }

        // The sample pool: what each track is doing and how much of the budget is gone. The pool
        // lives on this side with the loader, so this is read directly and not through a status.
        const SamplePool &pool = storage_.samples();
        out = line;
        append(out, "SMP ");
        for (int t = 0; t < kTracks; t++) {
            const SampleState state = pool.slot(t).state;
            // S: ready, and streamed from the card rather than held whole.
            *out++ = state == SampleState::ready && storage_.streaming(t) ? 'S'
                     : state == SampleState::ready  ? 'O'
                     : state == SampleState::loading ? '.'
                     : state == SampleState::failed  ? 'X'
                                                     : '-';
        }
        append(out, " ");
        append_number(out, pool.used() >> 20);
        *out++ = '/';
        append_number(out, pool.budget() >> 20);
        append(out, "M");
        *out = 0;
        canvas.text(0, 112, line, 10);
    }
