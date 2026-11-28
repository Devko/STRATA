// PROJECT's LAUNCH page: Track Launch (core/launch.hpp, docs/CONTROLS.md "LAUNCH").
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has.
//
// The page is PROJECT's slot tiles turned into a matrix: 8 lane rows by the 16 slots of the
// shown bank. The selected track is the lane cursor (a lower pad selects one silently), the slot
// cursor is SLOT's. STEP launches the selected lane, or every lane whose pad is held, from that
// slot of the shown bank at the queue mode; the same STEP again while it is queued drops it. OK
// launches the cell under the cursor, a held OK CAPTUREs what plays into the first free slot of
// the shown bank. CLR queues the lane back to its own part, and only that (U2: it no longer
// doubles as "back"; the screen keys leave). Push B pins the lane. FUNC + STEP is a
// whole-pattern change, as STEP on SLOT.
//
// The audio side arms and lands every launch (Engine, Cmd::launch); this side posts, shows and
// says. It reads the lane sources from the project and the queues from the engine, as SLOT
// reads the pattern queue: one-byte reads a block stale at worst.

    bool launch_page() const {
        return mode_ == Mode::project && project_page_ == ProjectPage::launch;
    }

    // "B07", or "OWN" for a lane's own part.
    static void append_source(char *&out, uint8_t source) {
        if (source == kLaneOwn || !lane_source_valid(source)) {
            append(out, "OWN");
            return;
        }
        *out++ = bank_letter(lane_source_bank(source));
        append_padded(out, uint32_t(lane_source_slot(source)) + 1, 2);
    }

    // The lanes a launch is for: the held lower pads, or the selected lane.
    uint8_t launch_target_lanes() const {
        return launch_held_ != 0 ? launch_held_ : uint8_t(1u << selected_);
    }

    // Post a launch of `source` on `lanes` at the queue mode, and say it: "T3 < B07 AT END".
    void launch_lanes(uint8_t lanes, uint8_t source) {
        Command command;
        command.cmd = Cmd::launch;
        command.a = lanes;
        command.b = source;
        command.c = uint8_t(queue_mode_);
        post_command(command);

        char *out = notice_buffer_;
        int count = 0;
        int first = 0;
        for (int t = kTracks - 1; t >= 0; t--) {
            if ((lanes >> t & 1u) == 0) continue;
            count++;
            first = t;
        }
        if (count == 1) {
            *out++ = 'T';
            append_number(out, uint32_t(first) + 1);
        } else {
            append_number(out, uint32_t(count));
            append(out, " LANES");
        }
        append(out, " < ");
        append_source(out, source);
        const bool now = queue_mode_ == QueueMode::immediate ||
                         engine_.status().running.load(std::memory_order_relaxed) == 0;
        if (!now) {
            append(out, " AT ");
            append(out, queue_mode_name(queue_mode_));
        }
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // STEP `slot` on LAUNCH: launch, or drop the launch already queued for that slot.
    void launch_step(int slot) {
        if (!project_index_valid(shown_bank_, slot)) return;
        slot_cursor_ = slot;
        const uint8_t source = lane_source(shown_bank_, slot);
        const uint8_t lanes = launch_target_lanes();
        // The same STEP again while it is queued (on the selected lane, or every held one) drops
        // it, as a second press of a queued pattern does on SLOT.
        bool all_queued = true;
        for (int t = 0; t < kTracks; t++) {
            if ((lanes >> t & 1u) == 0) continue;
            const LaneQueue &queued = engine_.lane_queue(t);
            all_queued = all_queued && queued.queue.pending && queued.source == source;
        }
        if (all_queued) {
            post(Cmd::launch_cancel, 0, 0, lanes);
            notice_logged("LAUNCH OFF");
            return;
        }
        launch_lanes(lanes, source);
    }

    // CLR: the lane back to its own part. Only that (U2, 2026-11-28): a CLR that left for SLOT
    // when the lane already played its own took the page away from a hand pressing it twice.
    void launch_clear() {
        const bool queued = engine_.lane_queue(selected_).queue.pending;
        if (!lane_borrowed(engine_.project(), selected_) && !queued) {
            char *out = notice_buffer_;
            *out++ = 'T';
            append_number(out, uint32_t(selected_) + 1);
            append(out, " PLAYS ITS OWN");
            *out = 0;
            notice(notice_buffer_);
            return;
        }
        if (!lane_borrowed(engine_.project(), selected_)) {
            post(Cmd::launch_cancel, 0, 0, uint8_t(1u << selected_));
            notice_logged("LAUNCH OFF");
            return;
        }
        launch_lanes(uint8_t(1u << selected_), kLaneOwn);
    }

    void launch_pin() {
        const bool pinned = (engine_.project().settings.lane_pinned >> selected_ & 1u) != 0;
        post(Cmd::launch_pin, uint8_t(selected_));
        char *out = notice_buffer_;
        *out++ = 'T';
        append_number(out, uint32_t(selected_) + 1);
        append(out, pinned ? " UNPINNED" : " PINNED");
        *out = 0;
        notice_logged(notice_buffer_);
    }

    void launch_capture() {
        post(Cmd::launch_capture, uint8_t(shown_bank_));
        notice_logged("CAPTURING");
    }

    // True when the key was LAUNCH's. Before the shared handler, so STEP, OK, CLR, the arrows
    // and push B mean what this page says; everything else (the screen keys, the bank keys,
    // FUNC + OK redo, PLAY) falls through.
    bool on_launch_key(int key, bool down) {
        if (!launch_page()) {
            launch_ok_held_ = false;
            return false;
        }
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (!down) return true;
            const int slot = key - ENAGA_KEY_STEP1;
            if (shift_) {
                // FUNC + STEP: a whole-pattern change, every lane at once.
                slot_cursor_ = slot;
                queue_shown_pattern(slot);
            } else {
                launch_step(slot);
            }
            return true;
        }
        if (shift_) return false;    // FUNC + OK is redo, FUNC + CLR undo, as everywhere
        switch (key) {
            case ENAGA_KEY_OK:
                // A tap launches the cell under the cursor on its release; a hold CAPTUREs
                // (service_launch), so a held OK never also launches.
                if (down) {
                    launch_ok_held_ = true;
                    launch_ok_down_ms_ = last_ms_;
                } else if (launch_ok_held_) {
                    launch_ok_held_ = false;
                    launch_step(slot_cursor_);
                }
                return true;
            case ENAGA_KEY_CLR:
                if (down) launch_clear();
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (down) {
                    selected_ = (selected_ + (key == ENAGA_KEY_UP ? -1 : 1) + kTracks) % kTracks;
                    end_gesture();
                    drop_range();
                }
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (down) {
                    const int next = slot_cursor_ + (key == ENAGA_KEY_LEFT ? -1 : 1);
                    if (next >= 0 && next < kPatternsPerBank) slot_cursor_ = next;
                }
                return true;
            case ENAGA_KEY_PUSH_B:
                if (down) launch_pin();
                return true;
            default:
                return false;
        }
    }

    // A lower pad on LAUNCH selects its lane and is held for a launch of several: no sound, so a
    // launch gesture makes no stray hit. True when the pad was LAUNCH's.
    bool launch_pad(int track, bool down) {
        if (down) {
            if (!launch_page() || shift_ || mute_mode_) return false;
            launch_held_ = uint8_t(launch_held_ | (1u << track));
            // The first pad on a visit says why it made no sound (U2).
            if (!launch_pad_said_) {
                launch_pad_said_ = true;
                notice("PADS PICK LANES");
            }
            if (track != selected_) {
                end_gesture();
                drop_range();
            }
            selected_ = track;
            return true;
        }
        const bool was = (launch_held_ >> track & 1u) != 0;
        launch_held_ = uint8_t(launch_held_ & ~(1u << track));
        return was;
    }

    // update(): the held OK, what the audio side reported, and the launches that landed.
    void service_launch() {
        if (launch_ok_held_ && !launch_page()) launch_ok_held_ = false;
        if (launch_ok_held_ && uint32_t(last_ms_ - launch_ok_down_ms_) >= kSlotMenuHoldMs) {
            launch_ok_held_ = false;
            launch_capture();
        }
        if (!launch_page()) {
            launch_held_ = 0;
            launch_pad_said_ = false;
        }

        const Status &status = engine_.status();
        const uint8_t event = status.launch_event.load(std::memory_order_acquire);
        if (event != launch_event_seen_) {
            launch_event_seen_ = event;
            const LaunchEvent last = LaunchEvent(status.launch_last.load(std::memory_order_relaxed));
            const int arg = int(status.launch_arg.load(std::memory_order_relaxed));
            bool say_event = true;    // false: the case said it itself
            char *out = notice_buffer_;
            switch (last) {
                case LaunchEvent::read_only:
                    *out++ = 'T';
                    append_number(out, uint32_t(arg) + 1);
                    *out++ = ' ';                    // "T3 B07 READ ONLY": 16 of the 23
                    append_source(out, engine_.project().settings.lane_source[arg & 7]);
                    append(out, " READ ONLY");
                    // And the way to edit it (U2): on GRID and KEYS the track menu's CAPTURE
                    // makes what plays a pattern of its own.
                    if (track_menu_screen()) {
                        *out = 0;
                        notice_alternating(notice_buffer_, "HOLD OK: CAPTURE");
                        enaga::log(notice_buffer_);
                        say_event = false;
                    }
                    break;
                case LaunchEvent::frozen:
                    *out++ = 'T';
                    append_number(out, uint32_t(arg) + 1);
                    append(out, " FROZEN");
                    break;
                case LaunchEvent::captured:
                    append(out, "CAPTURED ");
                    *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
                    append_padded(out, uint32_t(arg) + 1, 2);
                    break;
                case LaunchEvent::no_slot:
                    append(out, "NO FREE SLOT IN BANK");
                    break;
                case LaunchEvent::locks_full:
                    append(out, "LOCKS FULL");
                    break;
            }
            *out = 0;
            if (say_event) notice_logged(notice_buffer_);
        }

        // A launch that landed: a lane's source moved while the playing pattern did not (a
        // pattern change or CAPTURE moves lanes too, and says so itself). "T3 B07", "T3 OWN".
        const Project &project = engine_.project();
        const uint8_t live = lane_live_source(project);
        for (int t = 0; t < kTracks; t++) {
            const uint8_t source = project.settings.lane_source[t];
            if (source == launch_seen_[t]) continue;
            launch_seen_[t] = source;
            if (live != launch_seen_live_) continue;
            char *out = notice_buffer_;
            *out++ = 'T';
            append_number(out, uint32_t(t) + 1);
            *out++ = ' ';
            append_source(out, source);
            *out = 0;
            notice_logged(notice_buffer_);
        }
        launch_seen_live_ = live;
    }

    // The STEP keys on LAUNCH, for the selected lane: off, the slot has no part for it; dim, it
    // has one; bright, it plays; blinking, it is queued. Green, as SLOT's tiles are drawn white.
    void light_launch_steps() {
        const Project &project = engine_.project();
        const uint8_t playing = lane_playing_source(project, selected_);
        const LaneQueue &queued = engine_.lane_queue(selected_);
        const uint8_t queued_source = queued.source == kLaneOwn ? lane_live_source(project)
                                                                : queued.source;
        const bool blink = queue_blink_on();
        for (int s = 0; s < kPatternsPerBank; s++) {
            const uint8_t here = lane_source(shown_bank_, s);
            const bool has = lane_has_part(s, selected_);
            uint8_t green = has ? 2 : 0;
            if (here == playing) green = 10;
            if (queued.queue.pending && here == queued_source) green = blink ? 10 : 0;
            leds_.step(s, here == playing ? 4 : 0, green);
        }
    }

    // Whether slot `slot` of the shown bank has steps on lane `track`, from SLOT's thumbnail
    // cache (refresh_slot_cache): a bit per track in each of 16 columns.
    bool lane_has_part(int slot, int track) const {
        uint8_t any = 0;
        for (int c = 0; c < 16; c++) any = uint8_t(any | slot_thumb_[slot][c]);
        return (any >> track & 1u) != 0;
    }

    // The page (docs/SCREENS.md, LAUNCH): the caption at 18, the slot numbers at 25, eight lane
    // rows from 31 (a lane's number, its source, sixteen cells), the cursor's lane at 104 and the
    // hints at 112. Cells: a dot, no part; grey, a part; white, playing; a blinking outline,
    // queued; a frame, the cursor.
    static constexpr int kLaunchRowY = 31;
    static constexpr int kLaunchRowPitch = 9;
    static constexpr int kLaunchCellX = 28;
    static constexpr int kLaunchCellPitch = 6;

    void draw_launch(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Project &project = engine_.project();
        refresh_slot_cache();
        const bool blink = queue_blink_on();

        append(out, "LAUNCH  BANK ");
        *out++ = bank_letter(shown_bank_);
        *out = 0;
        paint::small_text(px, 0, 18, line, 13);
        out = line;
        append(out, "Q ");
        append(out, queue_mode_name(queue_mode_));
        *out = 0;
        paint::small_text(px, 125 - paint::small_width(line), 18, line, 10);
        for (int c = 0; c < 16; c += 4) {
            out = line;
            append_number(out, uint32_t(c) + 1);
            *out = 0;
            paint::small_text(px, kLaunchCellX + c * kLaunchCellPitch, 25, line, 6);
        }

        const uint8_t live = lane_live_source(project);
        for (int t = 0; t < kTracks; t++) {
            const int y = kLaunchRowY + t * kLaunchRowPitch;
            const bool borrowed = lane_borrowed(project, t);
            const bool pinned = (project.settings.lane_pinned >> t & 1u) != 0;
            const uint8_t playing = lane_playing_source(project, t);
            const LaneQueue &queued = engine_.lane_queue(t);
            const uint8_t queued_source =
                queued.source == kLaneOwn ? live : queued.source;
            const char number[2] = {char('1' + t), 0};
            paint::small_text(px, 0, y, number, t == selected_ ? 15 : 9);
            out = line;
            append_source(out, playing);
            if (pinned) *out++ = 'P';
            *out = 0;
            paint::small_text(px, 6, y, line, borrowed ? 15 : 6);
            for (int s = 0; s < kPatternsPerBank; s++) {
                const int x = kLaunchCellX + s * kLaunchCellPitch;
                const uint8_t here = lane_source(shown_bank_, s);
                if (here == playing) {
                    canvas.rect(x, y - 1, 5, 6, 15);
                } else if (lane_has_part(s, t)) {
                    canvas.rect(x + 1, y, 3, 4, 7);
                } else {
                    paint::plot(px, x + 2, y + 2, 4);
                }
                if (queued.queue.pending && here == queued_source && blink) {
                    frame_rect(canvas, x, y - 1, 5, 6, 15);
                }
            }
        }
        // The cursor: the selected lane on the slot cursor.
        frame_rect(canvas, kLaunchCellX + slot_cursor_ * kLaunchCellPitch - 2,
                   kLaunchRowY + selected_ * kLaunchRowPitch - 3, 9, 10, 15);

        // The cursor's lane: what it plays and what is queued for it.
        out = line;
        *out++ = 'T';
        append_number(out, uint32_t(selected_) + 1);
        *out++ = ' ';
        append(out, machine_name(engine_.machine_id(selected_)));
        *out++ = ' ';
        append_source(out, lane_playing_source(project, selected_));
        const LaneQueue &mine = engine_.lane_queue(selected_);
        if (mine.queue.pending) {
            append(out, " < ");
            append_source(out, mine.source);
        }
        if (launch_held_ != 0) append(out, " +PADS");
        *out = 0;
        paint::small_text(px, 0, 104, line, 12);
        paint::small_text(px, 0, 112, "OK LAUNCH  CLR OWN  B PIN", 7);
    }
