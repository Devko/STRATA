// MOD slots: two per track, learnt on the LFO page (core/modslot.hpp, docs/CONTROLS.md "MOD
// slots").
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has.
//
// The gesture is MACRO LEARN's (D11), one slot and one knob at a time: on the LFO page push C
// (slot 1) or D (slot 2), on EDIT, GRID or KEYS; EDIT opens if it was not up. Push the same knob
// again to walk the source, LFO ENV VEL PRS RND OFF. Then turn the knob that should move, on
// any EDIT page of the same track: where it is left, less where it started, is the depth, and
// it goes back to its start. OK or CLR ends it, and so does leaving EDIT. With no knob turned,
// the end keeps the slot's knob and depth and takes the new source, so a push walks the source
// of a slot already set; OFF clears it.

    // --- MOD slots --------------------------------------------------------

    int mod_learn_ = -1;                        // the slot being learnt, 0 or 1, or -1
    int mod_learn_track_ = 0;
    uint8_t mod_learn_source_ = kModSrcLfo;
    // Every knob turned while learning, with the byte it started from: all go back at the end,
    // and the last one turned that moved is the slot's.
    struct ModTouch {
        uint8_t param;
        uint8_t start;
    };
    static constexpr int kModTouches = 8;
    ModTouch mod_touches_[kModTouches] = {};
    int mod_touch_count_ = 0;
    int mod_touch_last_ = -1;
    // PRS: the pressure last sent for each track, so a held pad posts changes only.
    uint8_t mod_pressure_sent_[kTracks] = {};

    const KitModSlot &mod_slot_of(int track, int slot) const {
        return engine_.project().kit.tracks[track].mod[slot];
    }

    bool track_reads_pressure(int track) const {
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = mod_slot_of(track, s);
            if (slot.on() && slot.source == kModSrcPrs) return true;
        }
        return false;
    }

    void post_mod_slot(int track, int slot, uint8_t source, uint8_t dest, int8_t depth) {
        Command command;
        command.cmd = Cmd::set_mod_slot;
        command.track = uint8_t(track);
        command.a = uint8_t(slot);
        command.b = source;
        command.c = dest;
        command.d = uint8_t(depth);
        post_command(command);
    }

    // "M1 VEL>CUT +40", or "M1 OFF".
    void append_mod_slot(char *&out, int slot, uint8_t source, int dest, int depth) const {
        *out++ = 'M';
        append_number(out, uint32_t(slot) + 1);
        *out++ = ' ';
        append(out, mod_source_name(source));
        if (source == kModSrcOff || depth == 0) return;
        *out++ = '>';
        append(out, track_param_name(dest));
        *out++ = ' ';
        append_signed(out, depth);
    }

    // A push of C or D on the LFO page. `slot` 0 or 1.
    void mod_push(int slot) {
        if (mod_learn_ == slot && mod_learn_track_ == selected_) {
            mod_learn_source_ = mod_source_next(mod_learn_source_);
            mod_learn_notice();
            return;
        }
        if (mod_learn_ >= 0) end_mod_learn();
        if (mode_ != Mode::edit) {
            mode_ = Mode::edit;
            leave_mode();
        }
        mod_learn_ = slot;
        mod_learn_track_ = selected_;
        const KitModSlot &now = mod_slot_of(selected_, slot);
        mod_learn_source_ = now.on() ? now.source : kModSrcLfo;
        mod_touch_count_ = 0;
        mod_touch_last_ = -1;
        mod_learn_notice();
    }

    // A knob turned in EDIT while learning; called with the turn's delta still in the ring, so
    // the kit's byte is the value before it.
    void mod_learn_touch(int param) {
        if (selected_ != mod_learn_track_) return;
        if (!mod_dest_allowed(param)) {
            notice("MOD: NOT LFO OR ARP");
            return;
        }
        // Any notice gives way, so the learn line (draw) shows the knob and depth as they move.
        notice_ = nullptr;
        for (int i = 0; i < mod_touch_count_; i++) {
            if (mod_touches_[i].param == param) {
                mod_touch_last_ = i;
                return;
            }
        }
        if (mod_touch_count_ >= kModTouches) return;
        mod_touches_[mod_touch_count_] = {uint8_t(param),
                                          engine_.params(mod_learn_track_).v[param]};
        mod_touch_last_ = mod_touch_count_++;
    }

    // The knob the slot takes: the last one turned, if it moved, else the latest that did.
    int mod_learnt_touch() const {
        const auto moved = [this](int i) {
            return engine_.params(mod_learn_track_).v[mod_touches_[i].param] !=
                   mod_touches_[i].start;
        };
        if (mod_touch_last_ >= 0 && moved(mod_touch_last_)) return mod_touch_last_;
        for (int i = mod_touch_count_ - 1; i >= 0; i--) {
            if (moved(i)) return i;
        }
        return -1;
    }

    void end_mod_learn() {
        if (mod_learn_ < 0) return;
        const int track = mod_learn_track_;
        const int slot = mod_learn_;
        const KitModSlot now = mod_slot_of(track, slot);
        const int learnt = mod_learnt_touch();
        uint8_t dest = now.dest;
        int8_t depth = now.depth;
        if (learnt >= 0) {
            dest = mod_touches_[learnt].param;
            depth = mod_depth_from_turn(mod_touches_[learnt].start,
                                        engine_.params(track).v[dest]);
        }
        char *out = notice_buffer_;
        if (mod_learn_source_ == kModSrcOff) {
            post_mod_slot(track, slot, kModSrcOff, 0, 0);
            append_mod_slot(out, slot, kModSrcOff, 0, 0);
        } else if (learnt >= 0 || now.on()) {
            post_mod_slot(track, slot, mod_learn_source_, dest, depth);
            append_mod_slot(out, slot, mod_learn_source_, dest, depth);
        } else {
            append(out, "NO KNOB: M");
            append_number(out, uint32_t(slot) + 1);
            append(out, " STAYS OFF");
        }
        *out = 0;
        // Every knob back where it started: the slot is the move, the knob the sound at rest.
        for (int i = 0; i < mod_touch_count_; i++) {
            if (engine_.params(track).v[mod_touches_[i].param] != mod_touches_[i].start) {
                post(Cmd::set_param, uint8_t(track), 0, mod_touches_[i].param,
                     mod_touches_[i].start, 0);
            }
        }
        notice(notice_buffer_);
        // Logged too: the emulator can push and turn but not read the screen
        // (tests/scenario_modslots.sh).
        enaga::log(notice_buffer_);
        mod_learn_ = -1;
        mod_touch_count_ = 0;
        mod_touch_last_ = -1;
        learn_ended_ = true;                    // the CLR guard MACRO LEARN has, for the same reason
        learn_ended_at_ms_ = last_ms_;
    }

    // "MOD1 VEL: TURN A KNOB", then "MOD1 VEL>CUT +40" once one has moved: the line LEARN
    // keeps on screen while it is on.
    void mod_learn_line(char *line) const {
        char *out = line;
        append(out, "MOD");
        append_number(out, uint32_t(mod_learn_) + 1);
        *out++ = ' ';
        append(out, mod_source_name(mod_learn_source_));
        const int learnt = mod_learnt_touch();
        if (mod_learn_source_ == kModSrcOff) {
            append(out, ": OK CLEARS");
        } else if (learnt >= 0) {
            const uint8_t param = mod_touches_[learnt].param;
            *out++ = '>';
            append(out, track_param_name(param));
            *out++ = ' ';
            append_signed(out, mod_depth_from_turn(mod_touches_[learnt].start,
                                                   engine_.params(mod_learn_track_).v[param]));
        } else {
            append(out, ": TURN A KNOB");
        }
        *out = 0;
    }

    void mod_learn_notice() {
        mod_learn_line(notice_buffer_);
        notice(notice_buffer_);
        enaga::log(notice_buffer_);
    }

    // PRS: a pad's pressure to its track, for a slot on PRS. Change-filtered like the macro
    // bend; nothing at all for a track with no slot on PRS.
    void mod_pressure(int track, int pressure) {
        if (track < 0 || track >= kTracks || !track_reads_pressure(track)) return;
        const uint8_t value = uint8_t(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
        if (value == mod_pressure_sent_[track]) return;
        mod_pressure_sent_[track] = value;
        post(Cmd::set_pressure, uint8_t(track), 0, value);
    }

    // The pad is up: PRS back to 0, whatever the slots say by now.
    void mod_pressure_release(int track) {
        if (track < 0 || track >= kTracks || mod_pressure_sent_[track] == 0) return;
        mod_pressure_sent_[track] = 0;
        post(Cmd::set_pressure, uint8_t(track), 0, 0);
    }

    // On EDIT's bars: what a slot does to the parameter. A dim line under the bar across the
    // range the slot can move it (both ways for LFO and RND, one way for the rest), and a bright
    // pixel on it where the parameter is heard now. The font has no ~, so this is the mark.
    void draw_mod_marks(enaga::Canvas &canvas, int index, int x, int y, int width,
                        uint8_t base) const {
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = mod_slot_of(selected_, s);
            if (!slot.on() || slot.dest != index) continue;
            const int depth = slot.depth;
            const int size = depth < 0 ? -depth : depth;
            const bool both = slot.source == kModSrcLfo || slot.source == kModSrcRnd;
            int lo = int(base) - (both ? size : (depth < 0 ? size : 0));
            int hi = int(base) + (both ? size : (depth > 0 ? size : 0));
            lo = lo < 0 ? 0 : lo;
            hi = hi > 127 ? 127 : hi;
            const int x0 = x + lo * (width - 1) / 127;
            const int x1 = x + hi * (width - 1) / 127;
            canvas.rect(x0, y, x1 - x0 + 1, 1, 6);
            // From Status (Engine::publish_heard): `modulated_` is the audio side's.
            const uint8_t heard =
                engine_.status().mod_heard[selected_][s].load(std::memory_order_relaxed);
            if (heard == Status::kNotHeard) continue;
            canvas.pixel(x + int(heard) * (width - 1) / 127, y, 15);
        }
    }
