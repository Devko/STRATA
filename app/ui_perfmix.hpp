// The FUNC menu, LOCK, the performance mixer and FX GLIDE, KIT keys, RESAMPLE and KEYS drawn.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- the FUNC menu (FUNC + HOME) -----------------------------------------
    //
    // docs/CONTROLS.md: the rare setup actions live here, named on the screen, instead of on
    // FUNC + arrow chords that changed from page to page. HOME's second printed label is
    // "system", which is what this is. UP/DOWN (or A) move, OK runs the row, LEFT/RIGHT (or B)
    // change a setting row, CLR or FUNC + HOME closes it and returns to the screen it opened
    // over. The cursor always starts on SAVE, so FUNC + HOME, OK saves.

    // Project-wide rows only, ordered by how often a jam reaches for them: the live toggles
    // first, the file actions at the bottom. The selected track's rows (SPEED, PLAY, FREEZE,
    // RELOAD SAMPLE) are in GRID's track menu (app/ui_trackmenu.hpp).
    enum class MenuItem : uint8_t { save, jam, click, count_in, rec_mode, quantize, autofill,
                                    glide, loop_len, chain, song, prog_out, save_as, open, kits, fresh,
                                    reload, clear, demo, count };
    static constexpr int kMenuItems = int(MenuItem::count);
    static constexpr int kMenuRows = 9;

    static const char *menu_item_name(MenuItem item) {
        switch (item) {
            case MenuItem::save:     return "SAVE";
            case MenuItem::save_as:  return "SAVE AS";
            case MenuItem::open:     return "OPEN PROJECT";
            case MenuItem::fresh:    return "NEW PROJECT";
            case MenuItem::reload:   return "RELOAD PROJECT";
            case MenuItem::clear:    return "CLEAR PATTERN";
            case MenuItem::kits:     return "KITS";
            case MenuItem::chain:    return "CHAIN";
            case MenuItem::song:     return "CHAIN END";
            case MenuItem::prog_out: return "PROG CHG OUT";
            case MenuItem::autofill: return "AUTO-FILL";
            case MenuItem::quantize: return "QUANTIZE";
            case MenuItem::rec_mode: return "REC MODE";
            case MenuItem::click:    return "CLICK";
            case MenuItem::count_in: return "COUNT-IN";
            case MenuItem::loop_len: return "LOOP LEN";
            case MenuItem::glide:    return "FX GLIDE";
            case MenuItem::demo:     return "LOAD DEMO";
            case MenuItem::jam:      return "JAM REC";
            default:                 return "?";
        }
    }

    // A setting row's value as the row shows it, or "" for an action row.
    void menu_item_value(MenuItem item, char *out) {
        switch (item) {
            case MenuItem::chain:
                append(out, engine_.chain().active ? "ON" : "OFF");
                break;
            case MenuItem::song:
                append(out, engine_.project().settings.chain_song ? "SONG" : "LOOP");
                break;
            case MenuItem::prog_out:
                append(out, engine_.project().settings.program_out ? "ON" : "OFF");
                break;
            case MenuItem::autofill:
                append(out, auto_fill_name(engine_.auto_fill()));
                break;
            case MenuItem::quantize:
                append_number(out, uint32_t(quantize()));
                break;
            case MenuItem::rec_mode:
                append(out, rec_add() ? "ADD" : "REPLACE");
                break;
            case MenuItem::loop_len:
                // How many bars push VALUE + STEP 16 records (dsp/looper.hpp).
                append_number(out, uint32_t(kTakeBars[loop_bars_index_]));
                append(out, " BAR");
                break;
            case MenuItem::click:
                append(out, click_on_ ? "ON" : "OFF");
                break;
            case MenuItem::count_in:
                if (count_in_bars_ == 0) {
                    append(out, "OFF");
                } else {
                    append_number(out, uint32_t(count_in_bars_));
                    append(out, " BAR");
                }
                break;
            case MenuItem::glide:
                // How long a button's jump in the performance mixer rides (core/perffx.hpp).
                append(out, perf_glide_name(perf_glide_));
                break;
            case MenuItem::jam:
                // OFF, or how long the jam has run, as a clock: 4:07, 1:02:33.
                if (storage_.jam_running()) append_clock(out, storage_.jam_seconds());
                else append(out, "OFF");
                break;
            default:
                break;
        }
        *out = 0;
    }

    // Seconds as M:SS, or H:MM:SS from an hour.
    static void append_clock(char *&out, uint32_t seconds) {
        const uint32_t hours = seconds / 3600u, minutes = seconds / 60u % 60u, secs = seconds % 60u;
        if (hours > 0u) {
            append_number(out, hours);
            *out++ = ':';
            append_padded(out, minutes, 2);
        } else {
            append_number(out, minutes);
        }
        *out++ = ':';
        append_padded(out, secs, 2);
    }

    // JAM REC's mark, the top bar's dot and the REC key's blink: lit for 600 ms of each second
    // while the jam records, steady while its file is being closed, dark otherwise.
    bool jam_mark_on() const {
        return storage_.jam_running() && (!storage_.jam_recording() || last_ms_ % 1000u < 600u);
    }

    // JAM REC, from the menu's row (OK) or FUNC + REC on RESAMPLE: start the jam, or stop it. From
    // the menu, the menu closes on a start, so the music is back on the screen with the REC dot in
    // the top bar; a stop's SAVED comes once the file is closed.
    void toggle_jam(bool from_menu) {
        if (storage_.jam_running()) {
            storage_.request_jam_stop();
            notice_logged("JAM STOPPING");
            return;
        }
        if (!storage_.request_jam_start()) return;     // the reason comes as a jam message
        if (from_menu) close_menu(false);
        char *out = notice_buffer_;
        append(out, "JAM REC ");
        const char *name = storage_.jam_name();
        for (; *name != '\0' && *name != '.'; name++) *out++ = *name == '_' ? ' ' : *name;   // no _ in the font
        *out = 0;
        notice_logged(notice_buffer_);
    }

    bool frozen(int track) const {
        return (engine_.status().frozen.load(std::memory_order_relaxed) >> track & 1u) != 0;
    }

    // FREEZE's OK: freeze the selected track for the bars on the row, or give it back. A
    // freeze is a take like a resample, so it waits for the next bar line with the transport
    // running and refuses while any take is busy.
    void toggle_freeze() {
        if (frozen(selected_) || storage_.freezing_track() == selected_) {
            storage_.request_unfreeze(selected_);
            notice("UNFROZEN");
            return;
        }
        if (engine_.machine_id(selected_) == uint8_t(MachineId::midi)) {
            notice("MID HAS NO AUDIO");
            return;
        }
        if (looper_state() != 0u) {             // the looper holds the take buffer
            notice("LOOP HOLDS BUFFER");
            return;
        }
        if (!storage_.request_freeze(selected_, kTakeBars[freeze_bars_index_])) {
            notice("TAKE RUNNING");
            return;
        }
        notice(enaga::clock::running() ? "FREEZE ARMED" : "ARMED: PRESS PLAY");
    }

    // --- LOCK (core/lockstore.hpp): FUNC + MUTATE ---------------------------------------------
    //
    // The audio side keeps the locks and does the jump; the UI times the hold, says what
    // happened and puts its own copies (the morph, the macro knobs, the mixer) where a jump
    // leaves them.

    // A tap. A pending MUTATE, the mixer and the knobs are dealt with once the engine has said
    // what the tap did (service_lock_event): with no lock it does nothing at all.
    void lock_tap() { post(Cmd::lock_tap); }

    void service_lock_event() {
        const Status &status = engine_.status();
        const uint8_t event = status.lock_event.load(std::memory_order_acquire);
        if (event == lock_event_seen_) return;
        lock_event_seen_ = event;
        const uint8_t last = status.lock_last.load(std::memory_order_relaxed);
        char name[4];
        name[0] = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        {
            char *p = name + 1;
            append_padded(p, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
            *p = 0;
        }
        char *out = notice_buffer_;
        if (last == kLockStored) {
            append(out, "LOCK ");
            append(out, name);
            append(out, " STORED");
        } else if (last == uint8_t(LockTap::empty)) {
            append(out, "NO LOCK: HOLD STORES");
        } else {
            append(out, last == uint8_t(LockTap::jumped) ? "BACK TO LOCK " : "SWAPPED BACK ");
            append(out, name);
            // The pattern under a pending MUTATE is gone: OK and CLR have nothing to answer,
            // and CLR's undo would put the mutated steps back over the jump.
            mutation_pending_ = false;
            // The knobs the UI shadows, where the jump left them.
            morph_ = engine_.morph();
            for (int m = 0; m < kMacros; m++) macro_position_[m] = engine_.macro_position(m);
            rel_all_ = engine_.rel_all();
            len_all_ = engine_.len_all();
            rnd_unit_ = engine_.live_rnd_unit();
            rnd_armed_ = engine_.live_rnd_armed();
            rnd_armed_chosen_ = true;              // the lock's set, kept as chosen
            energy_ = engine_.energy();
            // The mixer to rest over FX GLIDE (a lock holds no effect); a loop plays on.
            if (perf_target_fx() != PerfFx::none) perf_ride_to(perf_target_fx(), 0, false);
        }
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // --- the performance mixer (core/perffx.hpp, dsp/looper.hpp) ----------------------------
    //
    // The UI owns the picked slot, the effect's amount and the looper's mix, as it owns the
    // morph: the engine is told absolute values and keeps no history of them.

    uint8_t looper_state() const {
        return engine_.status().looper_state.load(std::memory_order_relaxed);
    }

    void perf_set(PerfFx fx, uint8_t amount) {
        perf_fx_ = fx;
        perf_amount_ = amount;
        post(Cmd::perf_fx, 0, 0, uint8_t(fx), amount);
    }

    // --- FX GLIDE (core/perffx.hpp) --------------------------------------------------------
    //
    // A button's jump rides to its target over the FUNC menu's FX GLIDE, a beat by default,
    // and back to rest (amount 0) over at most a 1/16 (perf_release_ms); `hard` (FUNC held)
    // cuts as before. `perf_fx_` / `perf_amount_` and `loop_mix_` stay what
    // the engine has NOW, so the lights and the strip show the ride; `perf_ride_` holds where it
    // is going. Turning VALUE takes over from wherever a ride has got to.

    uint32_t glide_ms(bool hard, uint8_t to_amount) const {
        if (hard) return 0;
        const float tempo = enaga::clock::state().tempo;
        return to_amount == 0 ? perf_release_ms(perf_glide_, tempo)
                              : perf_glide_ms(perf_glide_, tempo);
    }

    void perf_ride_to(PerfFx fx, uint8_t amount, bool hard) {
        PerfRide ride;
        ride.from_fx = perf_fx_;
        ride.from_amount = perf_amount_;
        ride.to_fx = fx;
        ride.to_amount = amount;
        // From 0 to 0 (a slot picked over nothing) there is nothing to ride: set at once.
        ride.length_ms = perf_amount_ == 0 && amount == 0 ? 0u : glide_ms(hard, amount);
        perf_riding_ = false;
        if (ride.length_ms == 0) {
            if (fx != perf_fx_ || amount != perf_amount_) perf_set(fx, amount);
            return;
        }
        perf_ride_ = ride;
        perf_ride_ms_ = last_ms_;
        perf_riding_ = true;
        perf_ride_step(last_ms_);
    }

    void perf_ride_step(uint32_t ms) {
        const uint32_t elapsed = ms - perf_ride_ms_;
        PerfFx fx = PerfFx::none;
        uint8_t amount = 0;
        perf_ride_at(perf_ride_, elapsed, fx, amount);
        if (fx != perf_fx_ || amount != perf_amount_) perf_set(fx, amount);
        if (elapsed >= perf_ride_.length_ms) perf_riding_ = false;
    }

    // Where the effect is heading: the ride's target while one runs.
    PerfFx perf_target_fx() const { return perf_riding_ ? perf_ride_.to_fx : perf_fx_; }
    uint8_t perf_target_amount() const {
        return perf_riding_ ? perf_ride_.to_amount : perf_amount_;
    }

    // VALUE turned while a ride runs: it stops where it is, on the effect it was heading for.
    void perf_ride_stop() {
        if (!perf_riding_) return;
        perf_riding_ = false;
        if (perf_ride_.to_fx != perf_fx_) perf_set(perf_ride_.to_fx, 0);
    }

    void loop_ride_to(uint8_t mix, bool hard) {
        loop_riding_ = false;
        const uint32_t length = glide_ms(hard, mix);
        if (length == 0 || loop_mix_ == mix) {
            loop_mix_ = mix;
            post(Cmd::looper, 0, 0, 0, loop_mix_);
            return;
        }
        loop_ride_.from_fx = PerfFx::looper;
        loop_ride_.to_fx = PerfFx::looper;
        loop_ride_.from_amount = loop_mix_;
        loop_ride_.to_amount = mix;
        loop_ride_.length_ms = length;
        loop_ride_ms_ = last_ms_;
        loop_riding_ = true;
    }

    // update(): the rides, and the momentary push (hold VALUE pushed + hold a STEP).
    void service_perf(uint32_t ms) {
        if (perf_riding_) perf_ride_step(ms);
        if (loop_riding_) {
            const uint32_t elapsed = ms - loop_ride_ms_;
            PerfFx fx = PerfFx::none;
            uint8_t mix = 0;
            perf_ride_at(loop_ride_, elapsed, fx, mix);
            if (mix != loop_mix_) {
                loop_mix_ = mix;
                post(Cmd::looper, 0, 0, 0, loop_mix_);
            }
            if (elapsed >= loop_ride_.length_ms) loop_riding_ = false;
        }
        if (perf_hold_step_ >= 0 && !perf_hold_on_ &&
            uint32_t(ms - perf_hold_ms_) >= kPerfHoldMs) {
            const PerfFx fx = perf_fx_for_step(perf_hold_step_);
            if (fx == perf_picked_) {
                perf_hold_on_ = true;
                perf_ride_to(fx, kPerfAmountMax, shift_);
                perf_notice(fx, false, kPerfAmountMax);
            }
        }
    }

    // Push VALUE + a STEP pressed. A new effect is picked on the press, as ever. The picked
    // effect's own STEP lets go on the RELEASE instead, because held it is the momentary push.
    void perf_step_press(int index) {
        const PerfFx fx = perf_fx_for_step(index);
        const bool effect = fx != PerfFx::none && !perf_fx_is_control(fx);
        perf_hold_step_ = -1;
        perf_hold_on_ = false;
        perf_release_lets_go_ = false;
        if (effect && fx == perf_picked_) {
            perf_release_lets_go_ = true;
        } else {
            perf_pick(index);
        }
        if (effect && fx == perf_picked_) {
            perf_hold_step_ = index;
            perf_hold_ms_ = last_ms_;
        }
    }

    // And let go: a push rides out (the slot stays picked, at 0); a tap on the picked effect's
    // STEP lets it go.
    void perf_step_release(int index) {
        if (index != perf_hold_step_) return;
        const PerfFx fx = perf_fx_for_step(index);
        perf_hold_step_ = -1;
        if (perf_hold_on_) {
            perf_hold_on_ = false;
            perf_ride_to(fx, 0, shift_);
            perf_notice(fx, false, 0);
            return;
        }
        if (perf_release_lets_go_ && fx == perf_picked_) {
            perf_release_lets_go_ = false;
            perf_ride_to(PerfFx::none, 0, shift_);
            perf_picked_ = PerfFx::none;
            notice_logged("FX OFF: VALUE TEMPO");   // tests/scenario_glide.sh reads it
        }
    }

    // The morph as a 0-127 amount, for the notice: the slot reads like every other.
    uint32_t morph_amount() const {
        return uint32_t(morph_) * kPerfAmountMax / kMorphScale;
    }

    // The morph from the mixer's slot 13 or PERFORM's VALUE: the same control seen from two
    // places (backlog D2, R6), so both move `morph_` and neither keeps a copy.
    void morph_set(int next) {
        morph_ = uint16_t(next < 0 ? 0 : (next > int(kMorphScale) ? int(kMorphScale) : next));
        Command command;
        command.cmd = Cmd::set_morph;
        command.u = morph_;
        post_command(command);
    }

    // "12 NOISE RISER" on a pick, "NOISE RISER 64" on a turn, "LOOP MIX 64" for the looper,
    // "MORPH 64" for the morph, picked or turned: it has no zero to start from.
    void perf_notice(PerfFx fx, bool picked, int amount = -1) {
        char *out = notice_buffer_;
        if (fx == PerfFx::morph) {
            append(out, "MORPH ");
            append_number(out, morph_amount());
        } else if (fx == PerfFx::looper) {
            const uint8_t state = looper_state();
            if (state == 1u) {
                append(out, enaga::clock::running() ? "LOOP WAITS FOR BAR" : "LOOP ARMED: PLAY");
            } else if (state == 2u) {
                append(out, "LOOP RECORDING");
            } else {
                append(out, "LOOP MIX ");
                append_number(out, loop_mix_);
            }
        } else if (picked) {
            append_number(out, uint32_t(fx));
            *out++ = ' ';
            append(out, perf_fx_name(fx));
        } else {
            append(out, perf_fx_name(fx));
            *out++ = ' ';
            append_number(out, uint32_t(amount >= 0 ? amount : int(perf_amount_)));
        }
        *out = 0;
        notice(notice_buffer_);
        // A pick is logged too: the emulator can press push VALUE + STEP but not read the
        // screen (tests/scenario_slots.sh). Turns are not, or the log would be nothing else.
        if (picked) enaga::log(notice_buffer_);
    }

    // Push VALUE + STEP. A new effect replaces the last at amount 0 (the old one fades out);
    // its own STEP again lets go and gives VALUE back. STEP 16 picks the looper and records
    // when there is no loop yet; again, it leaves the looper like any other effect (backlog
    // D10): a finished loop falls silent (mix 0) but is kept, so STEP 16 picks it back up and
    // VALUE + CLR throws it away for a new take; a take still armed or recording is dropped.
    void perf_pick(int index) {
        const PerfFx fx = perf_fx_for_step(index);
        if (fx == PerfFx::looper) {
            const uint8_t state = looper_state();
            if (perf_picked_ == PerfFx::looper && state != 0u) {
                const bool taking = state != 3u;
                // A playing loop rides out over FX GLIDE; a take still to come has no sound.
                if (taking) {
                    loop_riding_ = false;
                    loop_mix_ = 0;
                    post(Cmd::looper, 0, 0, 0, 0);
                    post(Cmd::looper, 0, 0, 2, 0);
                } else {
                    loop_ride_to(0, shift_);
                }
                perf_picked_ = PerfFx::none;
                notice(taking ? "LOOP CANCELLED" : "LOOP OFF: VALUE TEMPO");
                return;
            }
            perf_picked_ = PerfFx::looper;
            if (state == 0u) {
                perf_record();
                return;
            }
            perf_notice(fx, true);
            return;
        }
        if (fx == PerfFx::morph) {
            // The morph is a control, not an effect: picking it leaves the effect that was up
            // where it is (the A/B switch no longer waits for the mixer to be let go), and
            // letting go leaves the morph where it is, as leaving PERFORM does.
            if (perf_picked_ == PerfFx::morph) {
                perf_picked_ = PerfFx::none;
                notice_logged("MORPH KEPT: VALUE TEMPO");
                return;
            }
            perf_picked_ = PerfFx::morph;
            perf_notice(fx, true);
            return;
        }
        if (fx == perf_picked_) {
            perf_ride_to(PerfFx::none, 0, shift_);
            perf_picked_ = PerfFx::none;
            notice("FX OFF: VALUE TEMPO");
            return;
        }
        // The new effect replaces the last from 0: the old one rides out over FX GLIDE.
        perf_picked_ = fx;
        perf_ride_to(fx, 0, shift_);
        perf_notice(fx, true);
    }

    void perf_record() {
        // The take's buffer is RESAMPLE's (dsp/looper.hpp): refused while a take holds it.
        if (engine_.status().resample_state.load(std::memory_order_relaxed) != 0u) {
            notice("RESAMPLE BUSY");
            return;
        }
        post(Cmd::looper, 0, 0, 1, kTakeBars[loop_bars_index_]);
        char *out = notice_buffer_;
        if (enaga::clock::running()) {
            append(out, "LOOP ");
            append_number(out, uint32_t(kTakeBars[loop_bars_index_]));
            append(out, " BAR: NEXT BAR");
        } else {
            append(out, "LOOP ARMED: PLAY");
        }
        *out = 0;
        notice(notice_buffer_);
    }

    // Push VALUE + LEFT/RIGHT: the next or previous slot, round the sixteen, keeping the amount
    // -- a scene change on a crossfader that stays where it is. Reaching the looper picks it
    // without recording; only STEP 16 records. The morph and the looper are passed through
    // like the others, but being controls they leave the effect that was up as it is.
    void perf_walk(int delta) {
        const int from = perf_picked_ == PerfFx::none ? (delta > 0 ? 0 : kPerfSlots + 1)
                                                      : int(perf_picked_);
        const int slot = (from - 1 + delta + kPerfSlots * 2) % kPerfSlots + 1;
        const PerfFx fx = PerfFx(slot);
        perf_picked_ = fx;
        if (perf_fx_is_control(fx)) {
            perf_notice(fx, true);
            return;
        }
        // A crossfade at the amount the last was heading for, over FX GLIDE.
        perf_ride_to(fx, perf_target_amount(), shift_);
        perf_notice(fx, true);
    }

    // Push VALUE + CLR: every effect off and the loop's mix to 0, VALUE back to its own jobs.
    // With all of that already off, it throws the loop away, which frees RESAMPLE's buffer.
    void perf_clear() {
        // Counting where a ride is heading, so a second press during a ride-out clears the loop.
        const bool anything = perf_target_fx() != PerfFx::none || perf_picked_ != PerfFx::none ||
                              (loop_riding_ ? loop_ride_.to_amount : loop_mix_) != 0;
        if (anything) {
            perf_ride_to(PerfFx::none, 0, shift_);
            perf_picked_ = PerfFx::none;
            loop_ride_to(0, shift_);
            notice("FX OFF");
            return;
        }
        if (looper_state() != 0u) {
            loop_riding_ = false;
            loop_mix_ = 0;
            post(Cmd::looper, 0, 0, 2, 0);
            notice("LOOP CLEARED");
            return;
        }
        notice("FX OFF");
    }

    // A press of VALUE alone (on_key): what VALUE is on, back to 0. A picked effect's amount
    // (dry), the looper's mix (live), the morph, picked or on PERFORM (at rest: the knobs own
    // the sound). Not the tempo, swing or a field: 0 is no tempo, and a reset
    // there would be a trap rather than a shortcut. Not with a STEP held (VALUE is that step's
    // note, and hold STEP + push is a lock's removal on A-D) or while a name is typed.
    // The effect and the loop's mix ride to 0 over FX GLIDE; with FUNC held they cut. (FUNC +
    // VALUE reset the DJ filter until that left for the mixer's slots, backlog D1.)
    void value_reset() {
        if (held_step_ >= 0 || name_entry_.active() || mode_ == Mode::menu) return;
        if (perf_picked_ == PerfFx::looper) {
            loop_ride_to(0, shift_);
            notice("LOOP MIX 0");
            return;
        }
        if (perf_picked_ != PerfFx::none && perf_picked_ != PerfFx::morph) {
            perf_ride_to(perf_picked_, 0, shift_);
            perf_notice(perf_picked_, false, 0);
            return;
        }
        if (shift_) {
            // FUNC + tap with nothing to cut: VariSpeed snaps back to the project's tempo and
            // pitch (core/varispeed.hpp). Nothing at rest, so a stray tap says nothing.
            if (vari_ != 0) {
                vari_apply(0);
                vari_notice();
            }
            return;
        }
        if (mode_ == Mode::perform || perf_picked_ == PerfFx::morph) {
            morph_set(0);
            notice("MORPH 0");
            return;
        }
        if (value_on_tempo()) tap_value_tempo();
    }

    // Where a plain turn of VALUE turns the tempo: nothing picked, not PERFORM (the morph), not
    // PROJECT's CHAIN or PATTERN page (the field under the cursor), no beat repeat held.
    bool value_on_tempo() const {
        if (perf_picked_ != PerfFx::none || mode_ == Mode::perform || repeat_held_) return false;
        return !(mode_ == Mode::project && (project_page_ == ProjectPage::chain ||
                                            project_page_ == ProjectPage::pattern));
    }

    // Tap tempo (core/taptempo.hpp): a VALUE tap where VALUE turns the tempo is a beat, stamped
    // at its press. The first tap starts the count; from the second the tempo follows the taps,
    // set and saved as a turn of VALUE sets it. Refused under an outside clock, as the turn is.
    void tap_value_tempo() {
        if (enaga::clock::source() != ENAGA_CLOCK_INTERNAL) {
            clock_refused_ = true;
            notice("EXT CLOCK");
            return;
        }
        const uint32_t tenths = tap_tempo(tap_, value_down_ms_);
        if (tenths == 0) {
            notice_logged("TAP TEMPO");
            return;
        }
        tempo_tenths_ = tenths;
        adopted_tempo_centi_ = uint16_t(tenths * 10);
        clock_refused_ = !set_clock_tempo(tenths * 10u);      // times VariSpeed's
        post_tempo(float(tenths) * 0.1f);
        char *out = notice_buffer_;
        append(out, "TAP ");
        append_tenths(out, tenths);
        *out = 0;
        notice_logged(notice_buffer_);   // tests/scenario_tempo.sh reads it
    }

    // VALUE with an effect picked: its amount, two counts a click (the whole range in 64
    // clicks, as a macro); with the looper picked, its mix; with the morph, the morph, at
    // PERFORM's rate.
    void perf_turn(int delta) {
        if (perf_picked_ == PerfFx::morph) {
            morph_set(int(morph_) + delta * (int(kMorphScale) / 64));
            perf_notice(PerfFx::morph, false);
            return;
        }
        if (perf_picked_ == PerfFx::looper) {
            loop_riding_ = false;
            loop_mix_ = uint8_t(clamp_int(int(loop_mix_) + 2 * delta, 0, int(kPerfAmountMax)));
            post(Cmd::looper, 0, 0, 0, loop_mix_);
            perf_notice(PerfFx::looper, false);
            return;
        }
        perf_ride_stop();
        perf_set(perf_fx_,
                 uint8_t(clamp_int(int(perf_amount_) + 2 * delta, 0, int(kPerfAmountMax))));
        perf_notice(perf_fx_, false);
    }

    // The STEP keys while VALUE is pushed: the sixteen slots, dim amber; the effect that is up
    // bright amber, the picked one bright green. STEP 16 also says what the looper is doing:
    // red blinking armed, red recording, dim green holding a loop.
    void light_perf_steps() {
        const uint8_t state = looper_state();
        const bool blink = ((last_ms_ / kConditionBlinkMs) & 1u) != 0;
        for (int i = 0; i < kPerfSlots; i++) {
            const PerfFx fx = perf_fx_for_step(i);
            uint8_t red = 2, green = 2;
            if (fx == perf_fx_ && perf_amount_ > 0) red = green = 10;
            if (fx == PerfFx::morph && morph_ > 0) red = green = 10;
            if (fx == PerfFx::looper && state == 3u) { red = 0; green = loop_mix_ > 0 ? 10 : 4; }
            if (fx == perf_picked_) { red = 0; green = 10; }
            if (fx == PerfFx::looper && state == 1u) { red = blink ? 10 : 0; green = 0; }
            if (fx == PerfFx::looper && state == 2u) { red = 10; green = 0; }
            leds_.step(i, red, green);
        }
    }

    void open_menu() {
        if (mode_ == Mode::menu) return;
        menu_return_ = mode_;
        mode_ = Mode::menu;
        leave_mode();
        menu_cursor_ = 0;
        card_armed_ = CardAction::none;
        notice("MENU");
    }

    // `announce`: say where it went back to. Not after an action that has its own notice.
    void close_menu(bool announce = true) {
        if (mode_ != Mode::menu) return;
        mode_ = menu_return_;
        leave_mode();
        if (announce) notice(mode_name(mode_));
    }

    void move_menu_cursor(int delta) {
        menu_cursor_ = (menu_cursor_ + delta + kMenuItems * 4) % kMenuItems;
        card_armed_ = CardAction::none;                   // an arm is for one row
    }

    // Change a setting row by one, LEFT/RIGHT or B. `wrap` is OK's forward step, which goes round
    // so one key reaches every value; the arrows stop at the ends where the list has ends.
    void menu_value(int delta, bool wrap) {
        switch (MenuItem(menu_cursor_)) {
            case MenuItem::chain: {
                const bool on = engine_.chain().active;
                post(Cmd::set_chain, 0, 0, on ? 0 : 1);
                notice(on ? "CHAIN OFF" : "CHAIN ON");
                break;
            }
            case MenuItem::song: {
                const bool song = !engine_.project().settings.chain_song;
                post_chain(ChainEdit::song, 0, song ? 1 : 0);
                notice(song ? "SONG: PLAY ONCE" : "LOOP CHAIN");
                break;
            }
            case MenuItem::prog_out: {
                // PROG CHG OUT (core/project.hpp): pattern changes go out as program changes on
                // the MIDI CH, which the notice names as gear prints it.
                const bool on = !engine_.project().settings.program_out;
                post(Cmd::set_program_out, 0, 0, on ? 1 : 0);
                char *out = notice_buffer_;
                append(out, on ? "PROG OUT CH " : "PROG OUT OFF");
                if (on) append_number(out, uint32_t((enaga::midi::channel() & 0x0F) + 1));
                *out = 0;
                notice(notice_buffer_);
                break;
            }
            case MenuItem::autofill: {
                // Off, 4, 8 or 16 bars. It wraps, since "off" is one of the four rather than an
                // end stop.
                const int next = (int(auto_fill_) + delta + int(AutoFill::count)) %
                                 int(AutoFill::count);
                auto_fill_ = AutoFill(next);
                post(Cmd::set_auto_fill, 0, 0, uint8_t(auto_fill_));
                char *out = notice_buffer_;
                append(out, "AUTOFILL ");
                append(out, auto_fill_name(auto_fill_));
                *out = 0;
                notice(notice_buffer_);
                break;
            }
            case MenuItem::quantize:
                if (wrap && quantize() >= 100) post_delta(Cmd::set_quantize, 0, 0, 0, -100);
                else post_delta(Cmd::set_quantize, 0, 0, 0, delta * 5);
                break;
            case MenuItem::rec_mode: {
                // Two values, so any step is a toggle. What a note recorded onto a chord
                // track's step that already plays does: REPLACE takes the step over on the
                // first note of a pass, ADD adds to its notes (core/stepnotes.hpp).
                const bool add = !rec_add();
                post(Cmd::set_rec_mode, 0, 0, add ? 1 : 0);
                notice_logged(add ? "REC ADDS NOTES" : "REC REPLACES");
                break;
            }
            case MenuItem::loop_len: {
                const int next = (loop_bars_index_ + delta + kTakeBarChoices) % kTakeBarChoices;
                loop_bars_index_ = next;
                char *out = notice_buffer_;
                append(out, "LOOP ");
                append_number(out, uint32_t(kTakeBars[loop_bars_index_]));
                append(out, " BAR");
                *out = 0;
                notice(notice_buffer_);
                break;
            }
            case MenuItem::click: {
                // A switch: either arrow, B or OK flips it.
                click_on_ = !click_on_;
                post(Cmd::set_click, 0, 0, click_on_ ? 1 : 0);
                notice_logged(click_on_ ? "CLICK ON" : "CLICK OFF");
                break;
            }
            case MenuItem::count_in: {
                // OFF, 1 BAR, 2 BAR; it wraps like AUTO-FILL. Used by the next PLAY with REC on.
                count_in_bars_ = uint8_t((int(count_in_bars_) + delta + 3) % 3);
                char *out = notice_buffer_;
                append(out, "COUNT-IN ");
                if (count_in_bars_ == 0) {
                    append(out, "OFF");
                } else {
                    append_number(out, uint32_t(count_in_bars_));
                    append(out, " BAR");
                }
                *out = 0;
                notice_logged(notice_buffer_);
                break;
            }
            case MenuItem::glide: {
                // OFF, 1/16, 1 BEAT, 1 BAR; it wraps like AUTO-FILL.
                const int count = int(PerfGlide::count);
                perf_glide_ = PerfGlide((int(perf_glide_) + delta + count) % count);
                char *out = notice_buffer_;
                append(out, "FX GLIDE ");
                append(out, perf_glide_name(perf_glide_));
                *out = 0;
                notice_logged(notice_buffer_);       // tests/scenario_glide.sh reads it
                break;
            }
            default:
                break;
        }
    }

    void run_menu_item() {
        switch (MenuItem(menu_cursor_)) {
            case MenuItem::save:
                storage_.request_save();
                close_menu(false);
                notice("SAVING");
                break;
            case MenuItem::save_as:
                if (!storage_.card_usable()) {
                    notice("NO CARD");
                    break;
                }
                // On PROJECTS, so the list the new name is checked against is the card's.
                mode_ = Mode::project;
                leave_mode();
                project_page_ = ProjectPage::files;
                list_projects();
                entry_for_ = EntryFor::save_as;
                name_entry_.open("SAVE AS", storage_.project_name());
                break;
            case MenuItem::open:
                mode_ = Mode::project;
                leave_mode();
                project_page_ = ProjectPage::files;
                list_projects();
                notice("OK TWICE OPENS");
                break;
            case MenuItem::fresh:
            case MenuItem::demo:
            case MenuItem::reload:
            case MenuItem::clear: {
                // Each throws away work, so each takes a second OK within kSaveArmMs. CLEAR
                // PATTERN is undoable as well (FUNC + CLR), the others are not.
                const MenuItem item = MenuItem(menu_cursor_);
                const CardAction action = item == MenuItem::reload ? CardAction::reload
                                          : item == MenuItem::clear ? CardAction::clear
                                          : item == MenuItem::demo  ? CardAction::demo
                                                                    : CardAction::fresh;
                if (arm_card_action(action)) close_menu(false);
                break;
            }
            case MenuItem::kits:
                open_kits();
                break;
            case MenuItem::jam:
                toggle_jam(true);
                break;
            default:
                menu_value(+1, true);
                break;
        }
    }

    bool on_menu_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK:
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                run_menu_item();
                return true;
            case ENAGA_KEY_CLR:
                if (shift_) return false;               // F1 + CLR is undo
                close_menu();
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (!shift_) move_menu_cursor(key == ENAGA_KEY_UP ? -1 : +1);
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (!shift_) menu_value(key == ENAGA_KEY_LEFT ? -1 : +1, false);
                return true;
            case ENAGA_KEY_SELECT_LEFT:
            case ENAGA_KEY_SELECT_RIGHT:
            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT:
                return true;                            // one page; no steps behind it
            default:
                return false;
        }
    }

    void draw_menu(enaga::Canvas &canvas) {
        char line[40];
        char value[24];
        // No caption: the tab says MENU, and the rows start where it was.
        int first = menu_cursor_ - kMenuRows / 2;
        if (first > kMenuItems - kMenuRows) first = kMenuItems - kMenuRows;
        if (first < 0) first = 0;
        for (int row = 0; row < kMenuRows && first + row < kMenuItems; row++) {
            const int i = first + row;
            const bool here = i == menu_cursor_;
            char *out = line;
            *out++ = here ? '>' : ' ';
            append(out, menu_item_name(MenuItem(i)));
            *out = 0;
            const int y = 20 + row * 9;
            list_row(canvas, 0, y, line, here, 9);
            menu_item_value(MenuItem(i), value);
            int length = 0;
            while (value[length] != '\0') length++;
            if (length > 0) canvas.text(kFrameMeterX - length * 6, y, value, here ? 0 : 9);   // on the row's bar
        }
        paint::small_text(canvas.data(), 0, 105, "OK: RUN   CLR: BACK", 10);
        paint::small_text(canvas.data(), 0, 113, "LEFT/RIGHT: CHANGE", 10);
    }

    void open_kits() {
        mode_ = Mode::kit;
        leave_mode();
        kit_armed_ = KitAction::none;
        storage_.request_kit_scan();
        notice(storage_.card_usable() ? "KITS" : "NO CARD");
    }

    void move_kit_cursor_to(int slot) {
        const int next = slot < 0 ? 0 : (slot >= Storage::kKitSlots ? Storage::kKitSlots - 1 : slot);
        if (next != kit_cursor_) kit_armed_ = KitAction::none;   // an arm is for one slot
        kit_cursor_ = next;
    }

    enum class KitAction : uint8_t { none, load, save };

    // A second press of the same key on the same slot within kSaveArmMs, as a project save,
    // reload and NEW take. True when this press is that second one.
    bool kit_second_press(KitAction action) {
        if (kit_armed_ == action && uint32_t(last_ms_ - kit_armed_at_ms_) < kSaveArmMs) {
            kit_armed_ = KitAction::none;
            return true;
        }
        kit_armed_ = action;
        kit_armed_at_ms_ = last_ms_;
        return false;
    }

    bool on_kit_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                const Storage::SlotState state = storage_.kit_slot(kit_cursor_).state;
                if (state == Storage::SlotState::empty) {
                    kit_armed_ = KitAction::none;
                    notice("EMPTY SLOT");
                } else if (!storage_.card_usable()) {
                    notice("NO CARD");
                } else if (kit_second_press(KitAction::load)) {
                    storage_.request_kit_load(kit_cursor_);
                    notice("LOADING KIT");
                } else {
                    notice("OK AGAIN TO LOAD");
                }
                return true;
            }

            case ENAGA_KEY_CLR:
                if (shift_) return false;               // F1 + CLR is undo
                mode_ = Mode::machine;                  // back, where the browser came from
                leave_mode();
                notice("MACHINE");
                return true;

            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (!shift_) {
                    move_kit_cursor_to(kit_cursor_ + (key == ENAGA_KEY_UP ? -1 : +1));
                } else if (key == ENAGA_KEY_UP) {
                    storage_.request_kit_scan();
                    notice(storage_.card_usable() ? "READING KITS" : "NO CARD");
                } else {
                    // Straight into an empty slot; over a kit, or a slot not yet read, only on
                    // the second press -- the kit already there is not undoable either.
                    const bool empty =
                        storage_.kit_slot(kit_cursor_).state == Storage::SlotState::empty;
                    if (!storage_.card_usable()) {
                        notice("NO CARD");
                    } else if (empty || kit_second_press(KitAction::save)) {
                        kit_armed_ = KitAction::none;
                        storage_.request_kit_save(kit_cursor_);
                        notice("SAVING KIT");
                    } else {
                        notice("AGAIN TO OVERWRITE");
                    }
                }
                return true;

            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                // The page of eight; F1 + LEFT / RIGHT are free here, and kept free.
                if (!shift_) move_kit_cursor_to(kit_cursor_ + (key == ENAGA_KEY_LEFT ? -8 : +8));
                return true;

            // The STEP keys are always the sixteen slots here, so the pad-row keys, which pick
            // which sixteen they mean, have nothing to do (docs/CONTROLS.md). With FUNC they are
            // the selected track's length, as on every screen.
            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT:
                return !shift_;

            default:
                return false;
        }
    }

    // Where a finished take is offered first: a track that plays files (SAMPLE or GRAIN) other
    // than the one it was bounced from -- the selected one if it does, else the lowest -- so the
    // usual case is one OK. The bounced track itself next (re-bouncing a GRAIN onto itself is a
    // way to stack it), and the selected track when no track plays files: OK then says so.
    int offer_track(int from) const {
        if (plays_files(selected_) && selected_ != from) return selected_;
        for (int t = 0; t < kTracks; t++) {
            if (plays_files(t) && t != from) return t;
        }
        if (from >= 0 && plays_files(from)) return from;
        return selected_;
    }

    // OK with a take on offer: its file into the track's kit, loaded by the ordinary sample swap
    // (Storage::request_sample_file), as TO TRK does before a take. Only on a SAMPLE or GRAIN
    // track: anything else would keep a path it never plays.
    void put_take_on_track() {
        const int track = take_offer_;
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(track) + 1);
        if (!plays_files(track)) {
            append(out, " NOT SAMPLE/GRAIN");
            *out = 0;
            notice(notice_buffer_);
            return;
        }
        if (!storage_.request_sample_file(track, storage_.last_take_path())) {
            append(out, " REFUSED");
            *out = 0;
            notice(notice_buffer_);
            return;
        }
        take_offer_ = -1;
        append(out, " ");
        append(out, machine_name(engine_.machine_id(track)));
        append(out, ": TAKE ON");
        *out = 0;
        notice(notice_buffer_);
        char line[16];
        char *log_out = line;
        append(log_out, "take T");
        append_number(log_out, uint32_t(track) + 1);
        *log_out = 0;
        enaga::log(line);
    }

    // Route the input as the screen says. Enaga answers 0, -1 for a source or level it does not
    // know, -2 when the codec did not answer; the screen keeps the refusal until the next try.
    // The input is quiet for about 10 ms around the change, which is why this runs on a turn of
    // B or C and not on every arm.
    void apply_input() {
        const int source = kTakeSources[take_source_].input;
        if (source < 0) return;
        input_result_ = enaga::system::input(source, take_level_);
        input_applied_ = input_result_ == 0;
        input_route_applied_ = -1;      // the strip's route is no longer the codec's (service_input_route)
        notice(input_result_ == 0 ? kTakeSources[take_source_].name : "INPUT REFUSED");
    }

