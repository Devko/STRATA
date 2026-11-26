// Input: keys, pads and encoders as they arrive, the arrow keys, the STEP keys (tap, hold, a
// range) and the screen keys.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- input ------------------------------------------------------------

    void on_key(int key, bool down) override {
        // FUNC held on its own for a moment shows what FUNC does (draw_func_help). Any other
        // key, knob or pad while it is down means the hand knew, and the help stays away.
        // A held LEN key repeats until it, or FUNC, comes up (update).
        if (!down && key == len_repeat_key_) len_repeat_key_ = -1;
        if (key == ENAGA_KEY_F1) {
            shift_ = down;
            if (!down) len_repeat_key_ = -1;
            if (down) slot_ok_held_ = false;    // FUNC + OK is redo, never a queue
            if (down) {
                shift_at_ms_ = last_ms_;
                shift_alone_ = true;
                help_page_ = 0;                 // the help opens on the screen's own page
            }
            return;
        }
        // While the help shows, the screen keys (or LEFT/RIGHT) turn its pages, as they turn the
        // screen's own. FUNC + these four does nothing anywhere else (docs/CONTROLS.md), and
        // only once the help is up, so no chord changes meaning.
        if (func_help_showing() &&
            (key == ENAGA_KEY_SELECT_LEFT || key == ENAGA_KEY_SELECT_RIGHT ||
             key == ENAGA_KEY_LEFT || key == ENAGA_KEY_RIGHT)) {
            if (down) {
                const int pages = help_page_count(help_place());
                const int delta = key == ENAGA_KEY_SELECT_LEFT || key == ENAGA_KEY_LEFT ? -1 : 1;
                help_page_ = (help_page_ + delta + pages) % pages;
            }
            return;
        }
        if (down) shift_alone_ = false;
        // A press of VALUE on its own, let go with nothing turned or pressed in between, resets
        // what VALUE is on to 0 (Roland, 2026-11-23; value_reset). Held, it stays the modifier
        // for swing, STEP, LEFT/RIGHT and CLR, which is why it acts on the RELEASE.
        //
        // Only a TAP (under kValueTapMs, Roland 2026-11-23): a long hold let go without anything
        // else is a chord changed mid-reach, and must not throw away the amount.
        if (key == ENAGA_KEY_PUSH_VALUE) {
            value_pushed_ = down;
            if (down) {
                value_alone_ = true;
                value_down_ms_ = last_ms_;
            } else if (value_alone_) {
                value_alone_ = false;
                if (uint32_t(last_ms_ - value_down_ms_) < kValueTapMs) value_reset();
            }
            return;
        }
        if (down) value_alone_ = false;

        // Typing a project's name (PROJECTS page, FUNC+DOWN): the entry takes every key until OK
        // or CLR closes it (<enaga/ui.hpp>). F1 and VALUE's push are tracked above, so a FUNC
        // still held from opening it is let go of properly.
        if (name_entry_.active()) {
            const enaga::ui::Action action = name_entry_.key(key, down);
            if (action == enaga::ui::Action::chosen) {
                if (entry_for_ == EntryFor::rename) {
                    finish_rename();
                } else {
                    finish_save_as();
                }
            }
            if (action == enaga::ui::Action::no) notice("NAME KEPT");
            return;
        }

        // PROJECT's slots: a held OK opens the pattern menu, which then takes its keys.
        if (on_slot_menu_key(key, down)) return;

        // FUNC + push D was the held tape stop until 2026-11-25 (backlog D5): one home per job,
        // and the tape stop's is the mixer's slot 8. The chord says where it went rather than
        // falling through to a page's push, which would do something nobody meant. Not while a
        // STEP is held: then FUNC + push D toggles the slide of that step's D lock, as FUNC +
        // push A-C do theirs -- every push with a step held is the step's.
        //
        // In PERFORM it is LIVE RND's other half (core/liverand.hpp): the selected track in or out
        // of the armed set, heard at once while a unit is on.
        if (key == ENAGA_KEY_PUSH_D && shift_ && held_step_ < 0 && mode_ == Mode::perform) {
            if (down) live_rnd_arm(selected_);
            return;
        }
        if (key == ENAGA_KEY_PUSH_D && shift_ && held_step_ < 0) {
            if (down) notice_logged("TAPE STOP: SLOT 8");
            return;
        }

        // Hold STEP + push A-D: take that parameter's lock off the held step -- the undo of
        // "hold STEP + turn A-D", on the same knob. Without it a lock could be turned but never
        // removed short of erasing the whole step. The pushes are numbered B, A, C, D in the ABI
        // (docs/ENAGA_API.md), so they are mapped by name rather than by arithmetic.
        if (held_step_ >= 0 && (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B ||
                                key == ENAGA_KEY_PUSH_C || key == ENAGA_KEY_PUSH_D)) {
            if (!down) return;
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                                          : 3;
            const int index = edit_index(encoder);
            held_used_ = true;
            // With FUNC: the parameter's SLIDE on the step (Phase 5), toggled. It glides from the
            // step's lock to what the next step that plays holds. The screen reads the pattern a
            // block late, so the notice says what the toggle is about to make it.
            if (shift_) {
                const bool slides = pattern_get_slide(engine_.pattern(), selected_, held_step_,
                                                      uint8_t(index));
                mark(GestureKind::field, selected_, held_step_);
                post(Cmd::lock_slide, uint8_t(selected_), uint8_t(held_step_), uint8_t(index));
                end_gesture();
                show_lock_readout(index);
                notice(slides ? "SLIDE OFF" : "SLIDE ON");
                return;
            }
            // The whole parameter goes: its value, its slide and its random range.
            mark(GestureKind::erase, selected_, held_step_);
            post(Cmd::clear_lock, uint8_t(selected_), uint8_t(held_step_), uint8_t(index));
            end_gesture();
            show_lock_readout(index);
            notice("LOCK CLEARED");
            return;
        }

        // MACRO LEARN (Roland, 2026-11-23; by turning since D11): in PERFORM, push a macro's
        // knob to learn it, which opens EDIT; there, every knob turned goes on the macro, on any
        // page and any track, and so do the kill EQ's bands turned on MIXER's EQ page. OK, CLR,
        // or leaving for any screen but EDIT and MIXER ends it. Before SLC AUTO and the note
        // view, which also take a plain push.
        {
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                : key == ENAGA_KEY_PUSH_D ? 3
                                                          : -1;
            if (encoder >= 0 && !shift_ && mode_ == Mode::perform) {
                if (!down) return;
                macro_selected_ = encoder;
                mode_ = Mode::edit;
                leave_mode();
                macro_learn_ = encoder;
                learn_notice();
                return;
            }
            if (encoder >= 0 && !shift_ && macro_learn_ >= 0 && !step_page_ &&
                mode_ == Mode::edit) {
                if (!down) return;
                // Learning is by turning (D11). A push takes a parameter the macro already holds
                // off it, which is the one thing turning cannot say.
                const uint8_t param = uint8_t(edit_index(encoder));
                if (macro_holds(macro_learn_, uint8_t(selected_), param)) {
                    take_off_macro(macro_learn_, uint8_t(selected_), param);
                } else {
                    notice("TURN A KNOB TO LEARN");
                }
                return;
            }
            // LEARN goes on onto MIXER's EQ page, where LOW, MID and HI are learnt by turning
            // like any EDIT knob. A push there on a band the macro holds takes it off; any
            // other push throws the kill switch, below, as it always does.
            if (encoder >= 0 && encoder < kMasterBands && !shift_ && macro_learn_ >= 0 &&
                mode_ == Mode::mixer && mixer_bank_ == kMixerEqPage &&
                macro_holds(macro_learn_, kMacroMasterTrack, uint8_t(encoder))) {
                if (down) take_off_macro(macro_learn_, kMacroMasterTrack, uint8_t(encoder));
                return;
            }
            if ((key == ENAGA_KEY_CLR || key == ENAGA_KEY_OK) && down && !shift_ &&
                macro_learn_ >= 0 && range_first_ < 0 && held_step_ < 0) {
                end_learn();
                return;
            }
            // And a MOD slot's learn the same way (app/ui_modslots.hpp).
            if ((key == ENAGA_KEY_CLR || key == ENAGA_KEY_OK) && down && !shift_ &&
                mod_learn_ >= 0 && range_first_ < 0 && held_step_ < 0) {
                end_mod_learn();
                return;
            }
            // A second CLR straight after the one that ended LEARN is the same "get me out"
            // press, not a clear of the track: on the unit it wiped the pattern (Roland,
            // 2026-11-23). FUNC + CLR (undo) is untouched.
            if (key == ENAGA_KEY_CLR && down && !shift_ && learn_ended_ &&
                last_ms_ - learn_ended_at_ms_ < kLearnClrGuardMs) {
                notice("KNOBS EDIT AGAIN");
                return;
            }
        }

        // SLC AUTO: on EDIT, a SAMPLE track, push the knob that turns SLC. Cuts at the hits found
        // in the sample (core/onsets.hpp) rather than evenly; push again for even. Before the
        // kill switches so FUNC + push stays theirs: this one is without FUNC.
        if (mode_ == Mode::edit && !shift_ &&
            (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B || key == ENAGA_KEY_PUSH_C ||
             key == ENAGA_KEY_PUSH_D) &&
            engine_.machine_id(selected_) == uint8_t(MachineId::sample)) {
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                                          : 3;
            if (edit_index(encoder) == kSlcParam) {
                if (!down) return;
                const bool on = !slices_auto(selected_);
                post(Cmd::set_slice_mode, uint8_t(selected_), 0, on ? kSliceAuto : kSliceEqual);
                notice(on ? "SLICES AUTO" : "SLICES EVEN");
                return;
            }
        }

        // LFO TRIG: on the LFO page, push RAT's knob for FREE, RETRIG, ONE-SHOT and round again
        // (dsp/lfo.hpp, "TRIG"). A kit setting, like SLC AUTO, and without FUNC for the same
        // reason; a held STEP took its push above.
        if ((mode_ == Mode::edit || mode_ == Mode::grid || mode_ == Mode::keys) && !shift_ &&
            !step_page_ && param_page_ == int(Page::lfo) &&
            (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B || key == ENAGA_KEY_PUSH_C ||
             key == ENAGA_KEY_PUSH_D)) {
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                                          : 3;
            // MOD slots (core/modslot.hpp): push DEP for slot 1, DST for slot 2, again for
            // the next source; then turn the knob the slot should move (app/ui_modslots.hpp).
            if (edit_index(encoder) == kLfoDepth || edit_index(encoder) == kLfoDest) {
                if (down) mod_push(edit_index(encoder) == kLfoDepth ? 0 : 1);
                return;
            }
            if (edit_index(encoder) == kLfoRate) {
                if (!down) return;
                const uint8_t next = uint8_t((int(lfo_trig_of(selected_)) + 1) % kLfoTrigs);
                post(Cmd::set_lfo_trig, uint8_t(selected_), 0, next);
                char *out = notice_buffer_;
                append(out, "LFO TRIG ");
                append(out, lfo_trig_name(LfoTrig(next)));
                *out = 0;
                notice(notice_buffer_);
                // Logged too: the emulator can push a knob but not read the screen
                // (tests/scenario_lfo.sh).
                enaga::log(notice_buffer_);
                return;
            }
        }

        // The arp's kit settings (core/arp.hpp): on the ARP page, push MOD's knob for VAR, RAT's
        // for RHYTHM, OCT's for ACCENT and GAT's for RATCHET, each round its table and back to
        // OFF. Kit settings like LFO TRIG, and without FUNC for the same reason.
        if ((mode_ == Mode::edit || mode_ == Mode::grid || mode_ == Mode::keys) && !shift_ &&
            !step_page_ && param_page_ == int(Page::arp) &&
            (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B || key == ENAGA_KEY_PUSH_C ||
             key == ENAGA_KEY_PUSH_D)) {
            if (!down) return;
            const int field = key == ENAGA_KEY_PUSH_A   ? 0
                              : key == ENAGA_KEY_PUSH_B ? 1
                              : key == ENAGA_KEY_PUSH_C ? 2
                                                        : 3;
            const KitArp &now = engine_.project().kit.tracks[selected_].arp;
            const uint8_t next =
                uint8_t((int(arp_ext_get(now, field)) + 1) % arp_ext_count(field));
            post(Cmd::set_arp_ext, uint8_t(selected_), 0, uint8_t(field), next);
            char *out = notice_buffer_;
            append(out, "ARP ");
            append(out, arp_ext_label(field));
            append(out, " ");
            append(out, arp_ext_name(field, next));
            *out = 0;
            notice(notice_buffer_);
            // Logged too: the emulator can push a knob but not read the screen
            // (tests/scenario_arp.sh).
            enaga::log(notice_buffer_);
            return;
        }

        // NOTE view: on EDIT, GRID or KEYS (wherever A-D turn the EDIT page), push the knob that
        // turns TUN (PIT on SAMPLE and GRAIN) to show it as a note, where it puts C-4, and turn
        // it a semitone a click; push again for the number and counts. One setting for every
        // track. Without FUNC, as SLC AUTO, so FUNC + push stays the kill EQ and the tape stop;
        // a held STEP took its push above.
        if ((mode_ == Mode::edit || mode_ == Mode::grid || mode_ == Mode::keys) && !shift_ &&
            !step_page_ &&
            (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B || key == ENAGA_KEY_PUSH_C ||
             key == ENAGA_KEY_PUSH_D)) {
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                                          : 3;
            const int index = edit_index(encoder);
            if (is_tune(index)) {
                if (!down) return;
                tune_notes_ = !tune_notes_;
                char *out = notice_buffer_;
                append(out, param_name_for(index));
                append(out, tune_notes_ ? " AS NOTES" : " AS NUMBERS");
                *out = 0;
                notice(notice_buffer_);
                // Logged too: the emulator can push a knob but not read the screen
                // (tests/scenario_tune.sh).
                enaga::log(notice_buffer_);
                return;
            }
        }

        // The master kill EQ's kill switches: push A, B or C on the MIXER's EQ page, or FUNC +
        // push A, B or C from anywhere, PERFORM included, so the cut is under one hand. A switch latches: push to
        // take the lows out for the build, push again on the one.
        if (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B || key == ENAGA_KEY_PUSH_C) {
            const bool on_eq_page = mode_ == Mode::mixer && mixer_bank_ == kMixerEqPage;
            if (shift_ || on_eq_page) {
                if (down) toggle_kill(key == ENAGA_KEY_PUSH_A ? 0 : (key == ENAGA_KEY_PUSH_B ? 1 : 2));
                return;
            }
        }

        // The ambient modes: push A on DLY+ or REV+ throws that send's mode, CLAS to LONG or
        // ROOM to HALL and back. A switch is a push as much as a turn, and a push is what the
        // emulator can press (tests/scenario_ambient.sh).
        if (key == ENAGA_KEY_PUSH_A && mode_ == Mode::mixer &&
            (mixer_bank_ == kMixerLongPage || mixer_bank_ == kMixerHallPage)) {
            if (!down) return;
            const bool long_page = mixer_bank_ == kMixerLongPage;
            const SendField field = long_page ? SendField::delay_mode : SendField::reverb_mode;
            const bool on = engine_.project().settings.sends[int(field)] >= 64;
            post_delta(Cmd::set_sends, 0, 0, uint8_t(field), on ? -1 : +1);
            static const char *const names[2][2] = {{"REV ROOM", "REV HALL"},
                                                    {"DLY CLASSIC", "DLY LONG"}};
            notice(names[long_page ? 1 : 0][on ? 0 : 1]);
            enaga::log(names[long_page ? 1 : 0][on ? 0 : 1]);
            return;
        }

        // Hold REPEAT, then a STEP: step stutter (CyDrums' P2, docs/CONTROLS.md "REPEAT"). Pad
        // first performs, so this comes before MUTE mode's, the mixer's and the scenes' STEP
        // keys; VALUE pushed first keeps the mixer's. A STEP taken here keeps its release even
        // if REPEAT came up first: a release reaching the grid would toggle a trig.
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            const int raw = key - ENAGA_KEY_STEP1;
            if (down && repeat_held_ && !value_pushed_) {
                stutter_press(raw);
                return;
            }
            if (!down && (stutter_keys_ >> raw & 1u) != 0) {
                stutter_release(raw);
                return;
            }
            // A press that is not a stutter owns its own release, whatever came before.
            if (down) stutter_keys_ = uint16_t(stutter_keys_ & ~(1u << raw));
        }
        // REPEAT held, or a pad rolling: LEFT/RIGHT walk the rate (CyDrums' P3), which beat
        // repeat, the roll and the stutter share. The release is swallowed with the press.
        if ((key == ENAGA_KEY_LEFT || key == ENAGA_KEY_RIGHT) && !shift_ && !value_pushed_ &&
            (repeat_held_ || rolling_pads_ != 0)) {
            if (down) step_repeat_rate(key == ENAGA_KEY_LEFT ? -1 : +1);
            return;
        }

        // MUTE mode turns STEP 1-8 into the eight stored mute states: recall on a tap, store
        // with VALUE pushed (F1 is taken: it makes a recall wait for the bar). docs/SPEC.md says they are "recalled from the upper row", but the upper row is
        // seven FIXED performance pads and there are eight states, so that cannot be taken
        // literally. The STEP keys are the only run of eight free while MUTE mode is on, and
        // MUTE mode is a mode, so repurposing them there is the smallest deviation available.
        // A recall with F1 held lands on the bar, like a mute does.
        if (mute_mode_ && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 8) {
            if (!down) return;
            const int slot = key - ENAGA_KEY_STEP1;
            char *out = notice_buffer_;
            if (value_pushed_) {
                post(Cmd::mute_store, 0, 0, uint8_t(slot));
                append(out, "STORED ");
            } else {
                post(Cmd::mute_recall, 0, 0, uint8_t(slot), shift_ ? 1 : 0);
                append(out, shift_ ? "NEXT BAR " : "RECALL ");
            }
            append_number(out, uint32_t(slot) + 1);
            *out = 0;
            notice(notice_buffer_);
            return;
        }

        // The performance mixer (core/perffx.hpp, docs/CONTROLS.md): with VALUE pushed, a STEP
        // picks one of the sixteen effects (16 the looper), LEFT/RIGHT walk them, CLR turns them
        // all off. In every mode but the menu, after MUTE mode, which keeps push VALUE + STEP 1-8
        // for storing a mute state. A STEP taken here keeps its release too, even if VALUE came
        // up first: a release reaching the grid would toggle a trig.
        if (value_pushed_ && mode_ != Mode::menu) {
            if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
                const int index = key - ENAGA_KEY_STEP1;
                if (down) {
                    perf_step_keys_ = uint16_t(perf_step_keys_ | (1u << index));
                    perf_step_press(index);
                } else {
                    perf_step_keys_ = uint16_t(perf_step_keys_ & ~(1u << index));
                    perf_step_release(index);
                }
                return;
            }
            if ((key == ENAGA_KEY_LEFT || key == ENAGA_KEY_RIGHT) && !shift_) {
                if (down) perf_walk(key == ENAGA_KEY_LEFT ? -1 : 1);
                return;
            }
            if (key == ENAGA_KEY_CLR && !shift_) {
                if (down) perf_clear();
                return;
            }
        }
        if (!down && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16 &&
            (perf_step_keys_ >> (key - ENAGA_KEY_STEP1) & 1u) != 0) {
            perf_step_keys_ = uint16_t(perf_step_keys_ & ~(1u << (key - ENAGA_KEY_STEP1)));
            perf_step_release(key - ENAGA_KEY_STEP1);
            return;
        }

        // A scene pad held turns the STEP keys into the sixteen scenes, in whatever mode is on
        // screen: docs/SPEC.md gives "hold + STEP picks the scene" and the performance row is
        // deliberately not modal.
        if (scene_holding_ >= 0 && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) {
                const int scene = key - ENAGA_KEY_STEP1;
                post(Cmd::scene_select, 0, 0, uint8_t(scene_holding_), uint8_t(scene));
                char *out = notice_buffer_;
                append(out, scene_holding_ == 0 ? "A " : "B ");
                append_number(out, uint32_t(scene) + 1);
                *out = 0;
                notice(notice_buffer_);
            }
            return;
        }

        // In KIT mode the sixteen STEP keys are the sixteen kit slots: a press puts the cursor
        // there, and OK (twice) recalls it. Only the cursor -- a slot key that loaded at once
        // would throw the sound away on a stray press.
        if (mode_ == Mode::kit && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) move_kit_cursor_to(key - ENAGA_KEY_STEP1);
            return;
        }

        // The FUNC menu is a list over whatever was on screen: a STEP key there would toggle a
        // trig on a track nobody is looking at.
        if (mode_ == Mode::menu && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) return;

        // In PROJECT mode a STEP key queues a pattern change rather than toggling a trig, so it
        // is taken here, before the editing path, and only on the press -- there is no modifier
        // gesture to decide on the release.
        if (mode_ == Mode::project && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (!down) return;
            // On the CHAIN page a STEP key is that slot of the shown bank, written into the
            // entry under the cursor -- or the first entry, when the chain is empty.
            if (project_page_ == ProjectPage::chain) {
                chain_put_slot(key - ENAGA_KEY_STEP1);
                return;
            }
            slot_cursor_ = key - ENAGA_KEY_STEP1;       // the cursor follows, so OK repeats it
            queue_shown_pattern(slot_cursor_);
            return;
        }

        // The STEP keys are steps on every playing screen, PERFORM and KEYS included (D12):
        // KEYS plays on the pads, so the hands can play and edit at once. MUTE MODE and the list
        // screens, above, keep theirs. Macros are set by learning (D11), the kill EQ's bands
        // included, so no STEP key assigns one any more.
        // A STEP key needs its release as well as its press. Holding one is a modifier -- it
        // locks a parameter, opens the step page or starts a range -- and a step held as a
        // modifier must NOT toggle its trig when it comes up. That can only be decided on the
        // release: at the moment of the press there is no way to know which it was yet.
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            on_step_key(key - ENAGA_KEY_STEP1, down);
            return;
        }

        if (!down) return;

        // PROJECT and PERFORM mode's own keys. Taken before the switch so the editing meanings
        // of OK, CLR and the arrows cannot fire while one of them is what is on screen.
        if (mode_ == Mode::project && on_project_key(key)) return;
        if (mode_ == Mode::kit && on_kit_key(key)) return;
        if (mode_ == Mode::menu && on_menu_key(key)) return;
        if (mode_ == Mode::samples && on_samples_key(key)) return;
        if (mode_ == Mode::sounds && on_sounds_key(key)) return;
        // A mutation waiting for its verdict takes OK and CLR first, in whatever mode: OK keeps
        // it, CLR reverts it. Only the plain keys; F1 + CLR and F1 + OK stay undo and redo. And
        // not where OK and CLR already mean something more specific: a held STEP (its page, its
        // clear), a range (its clear), a held scene pad (capture, erase) and RESAMPLE (arm,
        // cancel). There the verdict took the key, and a held step's release then toggled its
        // trig as well, because nothing had marked the hold as used.
        if (mutation_pending_ && !shift_ && (key == ENAGA_KEY_OK || key == ENAGA_KEY_CLR) &&
            held_step_ < 0 && range_first_ < 0 && scene_holding_ < 0 &&
            mode_ != Mode::resample) {
            mutation_pending_ = false;
            if (key == ENAGA_KEY_CLR) {
                post(Cmd::undo);
                end_gesture();
                notice("REVERTED");
            } else {
                notice("KEPT");
            }
            return;
        }
        if (mode_ == Mode::perform && on_perform_key(key)) return;

        switch (key) {
            case ENAGA_KEY_F5:
                // Not while a project is loading. `step_load` rewrites pattern memory a pass at
                // a time, and the audio side would play a pattern half of which belongs to the
                // project being replaced. The load itself stops the transport; this is the gap
                // where someone starts it again before the load has finished. A SAMPLE load is
                // different and is allowed: playing through one is the point of loading in the
                // background, and the pool's epoch handshake makes it safe.
                if (storage_.loading_project()) {
                    notice("LOADING");
                    break;
                }
                // The transport is Enaga's and its calls are MAIN CONTEXT, so they happen here
                // rather than through the command ring. start() returns false under an external
                // clock source, where STRATA can only follow -- the screen shows EXT and the
                // refusal is simply recorded.
                if (shift_) {
                    // F1 + F5: restart from step 1 (docs/SPEC.md), playing or not. Running, the
                    // clock is located back to 0 and keeps going; stopped, START already begins
                    // at 0. Either way the audio side resets what it measures from the position.
                    // An outside clock refuses both, and then nothing is reset: the playheads
                    // must stay with the master.
                    const bool ok = enaga::clock::running() ? enaga::clock::locate(0)
                                                            : enaga::clock::start();
                    clock_refused_ = !ok;
                    if (!ok) {
                        notice("EXT CLOCK");
                        break;
                    }
                    stop_armed_ = false;
                    post(Cmd::restart);
                    notice("RESTART");
                    break;
                }
                clock_refused_ = !enaga::clock::start();
                stop_armed_ = false;
                post(Cmd::play);                       // for the audio side's trace only
                break;
            case ENAGA_KEY_F6:
                if (shift_) {
                    // F1 + F6: stop and reset all tracks (docs/SPEC.md). Everything a plain STOP
                    // does, and the second press's cut, in one: the tails are cut, a queued
                    // pattern change is dropped and the clock goes back to 0, so the next PLAY
                    // or CONTINUE starts at step 1 with nothing pending -- and a running chain
                    // goes back to its first entry, so a whole set starts again from the top
                    // (Roland, 2026-11-22). Refused under an outside clock that keeps running,
                    // exactly as plain F6 is.
                    clock_refused_ = !enaga::clock::stop();
                    if (clock_refused_ && enaga::clock::running()) {
                        notice("EXT CLOCK");
                        break;
                    }
                    enaga::clock::locate(0);
                    post(Cmd::stop, 0, 0, 1);
                    post(Cmd::cancel_queue);
                    // Starting the chain again is what puts it on entry 1 and that entry's
                    // pattern live; with the clock stopped the switch is immediate.
                    if (engine_.chain().active) post(Cmd::set_chain, 0, 0, 1);
                    stop_armed_ = false;
                    notice("RESET");
                    break;
                }
                clock_refused_ = !enaga::clock::stop();
                // Refused while an outside clock keeps running: nothing stopped, so nothing is
                // released or reset either -- doing so cut every voice and sent the playheads back
                // to step 1 while the clock carried on, and the pattern then played out of step
                // with the master. STRATA follows the outside STOP when it comes (on_transport).
                if (clock_refused_ && enaga::clock::running()) {
                    notice("EXT CLOCK");
                    break;
                }
                // The second press cuts the tails. Only that part is the audio side's job.
                post(Cmd::stop, 0, 0, stop_armed_ ? 1 : 0);
                // Stopping shows on the top line; the second press's cut was silent, and a STOP
                // on a stopped box (which arms that cut) looked like a key that does nothing.
                notice(stop_armed_ ? "TAILS CUT" : "STOP");
                stop_armed_ = !stop_armed_;
                break;

            case ENAGA_KEY_F2:                         // REC, or F1 + F2 for resample setup
                if (shift_) {
                    // docs/SPEC.md's key table gives F1 + F2 to "Resample setup". A mode rather
                    // than a one-shot, because a take has two settings to choose -- how many
                    // bars and where it lands -- and OK is what commits it.
                    mode_ = Mode::resample;
                    leave_mode();
                    notice("RESAMPLE");
                    break;
                }
                rec_ = !rec_;
                // A whole recording pass is one undo level, so the gesture ends when recording
                // does -- and the next pass must not coalesce into the last one.
                end_gesture();
                notice(rec_ ? "REC ON" : "REC OFF");
                break;

            // Every one of these ends a STEP hold as well. KEYS, PERFORM and the mute pads
            // swallow the STEP keys ahead of on_step_key, so a key held across a mode change
            // never delivers its release and the hold would last until the app was restarted --
            // with the next encoder move writing a lock onto it.
            case ENAGA_KEY_F3:                         // PROJECT, or FUNC: "pool", the samples
                if (shift_) {
                    // FUNC + PROJECT is the sample browser: the key's second label is "pool",
                    // the SmplTrek's own name for its samples (docs/CONTROLS.md). Saving moved
                    // to the FUNC menu (FUNC + HOME, OK).
                    open_samples();
                    break;
                }
                mode_ = Mode::project;
                leave_mode();
                // Back on PROJECTS: the list it shares with the sample browser may hold samples
                // now, so it is read again.
                if (project_page_ == ProjectPage::files) list_projects();
                break;
            case ENAGA_KEY_F4:                         // HOME: GRID and KEYS; FUNC: "system", the menu
                if (shift_) {
                    // The key's second label is "system": the FUNC menu, where the setup actions
                    // live (docs/CONTROLS.md). Pressed again from the menu, it closes it.
                    if (mode_ == Mode::menu) close_menu();
                    else open_menu();
                    break;
                }
                // HOME is GRID, always, so a reflex press gets you home. Double-tapped (the
                // second press within kDoubleTapMs, already on GRID) it is KEYS.
                {
                    const bool double_tap = mode_ == Mode::grid && !step_page_ &&
                                            home_tapped_ &&
                                            uint32_t(last_ms_ - home_tapped_at_ms_) < kDoubleTapMs;
                    home_tapped_ = !double_tap;
                    home_tapped_at_ms_ = last_ms_;
                    mode_ = double_tap ? Mode::keys : Mode::grid;
                    leave_mode();
                    if (double_tap) notice("KEYS");
                }
                break;
            case ENAGA_KEY_F7:                         // MIXER, or PERFORM with F1
                mode_ = shift_ ? Mode::perform : Mode::mixer;
                leave_mode();
                break;
            case ENAGA_KEY_F8:                         // EDIT: the sound pages, or machine select
                if (shift_) {
                    // docs/SPEC.md's key table: F1 + F8 is "Kit and machine select". The first
                    // press is the machine select; pressed again from there it is the kit
                    // browser, and from the browser it goes back -- one key for both halves, as
                    // the spec gives it, and MACHINE's screen says so.
                    if (mode_ == Mode::machine) {
                        open_kits();
                    } else {
                        mode_ = Mode::machine;
                        leave_mode();
                        notice("MACHINE");
                    }
                    break;
                }
                mode_ = Mode::edit;
                leave_mode();
                break;

            case ENAGA_KEY_OK:
                // RESAMPLE mode first: OK is what commits a take, and there is no step to hold.
                if (mode_ == Mode::resample && take_offer_ >= 0) {
                    put_take_on_track();
                    break;
                }
                if (mode_ == Mode::resample) {
                    // Not while a take is armed, recording or still going to the card: the
                    // engine would refuse the arm anyway, but the write request below would
                    // retarget the take in flight -- a take asked for TO FILE landed on the track
                    // now on screen, over that track's sample, and the screen said ARMED.
                    const uint8_t state =
                        engine_.status().resample_state.load(std::memory_order_relaxed);
                    if (state != 0u || storage_.take_busy()) {
                        notice(state == 3u || storage_.take_busy() ? "TAKE STILL WRITING"
                                                                   : "TAKE RUNNING");
                        break;
                    }
                    // The performance mixer's looper holds the same buffer (dsp/looper.hpp).
                    if (looper_state() != 0u) {
                        notice("LOOP HOLDS BUFFER");
                        break;
                    }
                    // From the input: make sure the route is the one on screen before the take.
                    if (kTakeSources[take_source_].input >= 0 && !input_applied_) apply_input();
                    // A take on offer is put on a track with OK (above) or let go with CLR first;
                    // here there is none, so this is a new take.
                    {
                        const TakeSource &source = kTakeSources[take_source_];
                        take_from_track_ = source.track;
                        post(Cmd::resample_arm, uint8_t(source.track >= 0 ? source.track : 0), 0,
                             kTakeBars[take_bars_index_],
                             source.input >= 0 ? 1 : (source.track >= 0 ? 2 : 0));
                    }
                    // And the card side, asked for now so the write happens the moment the take
                    // finishes rather than needing a second press at exactly the right time.
                    storage_.request_resample_write(resample_track_);
                    notice(enaga::clock::running() ? "ARMED" : "ARMED: PRESS PLAY");
                    break;
                }
                if (held_step_ >= 0) {
                    len_step_ = -1;
                    // Hold STEP + OK: the step page, on that step (docs/SPEC.md, "Step editing").
                    step_page_ = true;
                    page_step_ = held_step_;
                    field_page_ = 0;
                    held_used_ = true;                 // so the release does not toggle the trig
                    mode_ = Mode::grid;
                } else if (shift_) {
                    mutation_pending_ = false;         // settled, or CLR would undo a 2nd level
                    post(Cmd::redo);                   // F1 + OK = redo
                    end_gesture();
                    notice("REDO");
                } else if (step_page_) {
                    step_page_ = false;                // OK again closes it
                } else if (mode_ == Mode::edit && param_page_ == int(Page::arp)) {
                    // On the ARP page OK opens that page's list, the factory arps, on any
                    // machine: the arp runs on every track (core/arppresets.hpp).
                    open_sounds(true);
                } else if (mode_ == Mode::edit && machine_is_grain(engine_.machine_id(selected_))) {
                    // GRAIN has a sample AND factory sounds, so the two screens split them: OK on
                    // EDIT opens its SOUNDS, as on every synth, and OK on MACHINE (below) the
                    // sample browser, which keeps "pick the machine, then its sample" in order.
                    open_sounds();
                } else if ((mode_ == Mode::edit || mode_ == Mode::machine) &&
                           plays_files(selected_)) {
                    // OK picks, as in every Enaga list: on a SAMPLE or GRAIN track, from the
                    // sound pages or the machine list, it opens the sample browser on that track.
                    open_samples();
                } else if ((mode_ == Mode::edit || mode_ == Mode::machine) &&
                           sound_preset_count(engine_.machine_id(selected_)) > 0) {
                    // And on the synths and drums, the factory sounds (core/sounds.hpp).
                    open_sounds();
                }
                break;

            case ENAGA_KEY_CLR:
                if (shift_) {
                    // An undo settles a pending MUTATE: it has either just been taken back or was
                    // never the newest edit. Left pending, the next plain CLR undid ANOTHER level
                    // -- the edit before the mutation -- and said REVERTED.
                    mutation_pending_ = false;
                    post(Cmd::undo);                   // F1 + CLR = undo
                    end_gesture();
                    notice("UNDO");
                } else if (step_page_ && held_step_ < 0 && range_first_ < 0) {
                    // CLR is "back" on Enaga's lists, and the step page is a page you opened.
                    // It used to fall through to erase_selection, which with nothing selected
                    // erases the WHOLE TRACK -- from a screen showing one step.
                    step_page_ = false;
                } else if (mode_ == Mode::resample && take_offer_ >= 0) {
                    // A take on offer: CLR leaves it a file, and the next OK arms a new take.
                    take_offer_ = -1;
                    notice("TAKE STAYS A FILE");
                } else if (mode_ == Mode::resample) {
                    // In RESAMPLE, CLR cancels the take rather than erasing a step: there is no
                    // selection in this mode and a take is the only thing there is to cancel.
                    post(Cmd::resample_cancel);
                    notice("CANCELLED");
                } else {
                    erase_selection();
                }
                break;

            // The arrow keys navigate, as they do in Enaga's own menu and SETTINGS page: UP and
            // DOWN move through the rows on screen, LEFT and RIGHT move sideways (see
            // `vertical` and `horizontal`). PROJECT and PERFORM take theirs in their own key
            // handlers above.
            //
            // Copy and paste moved to F1 + UP and F1 + DOWN -- and stay on plain UP and DOWN
            // while a STEP is held, because a key held with a step is already a chord on that
            // step and cannot be navigation. UP takes the selection, DOWN puts it down again.
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_ || held_step_ >= 0) {
                    if (key == ENAGA_KEY_UP) copy_selection(); else paste_selection();
                } else {
                    vertical(key == ENAGA_KEY_UP ? -1 : +1);
                }
                break;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (!shift_) horizontal(key == ENAGA_KEY_LEFT ? -1 : +1);
                break;

            // The octave keys are the step page in GRID; with F1 they are the track's length,
            // which is what makes polymeter reachable without a menu. The spec's key table.
            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT: {
                const int delta = key == ENAGA_KEY_OCTAVE_LEFT ? -1 : +1;
                if (mode_ == Mode::project && !shift_) {
                    // The pad-row keys say which sixteen the STEP keys mean (docs/CONTROLS.md):
                    // in PROJECT and CHAIN the STEP keys are a bank's sixteen slots, so these
                    // walk the bank being SHOWN, which is not the bank playing -- a queued change
                    // is armed from the shown bank, so browsing switches nothing by itself.
                    shown_bank_ = (shown_bank_ + delta + kBanks) % kBanks;
                    char *out = notice_buffer_;
                    append(out, "BANK ");
                    *out++ = bank_letter(shown_bank_);
                    *out = 0;
                    notice(notice_buffer_);
                    break;
                }
                if (mode_ == Mode::menu) break;
                if (shift_) {
                    step_track_length(delta);
                    len_repeat_key_ = key;
                    len_repeat_due_ms_ = last_ms_ + kLenRepeatDelayMs;
                } else {
                    // Round the four, as LEFT/RIGHT walk the same page on GRID and the pad-row
                    // keys walk PROJECT's banks: page 4 to 1 is one press, not three.
                    page_ = (page_ + delta + 4) % 4;
                }
                break;
            }

            // The screen keys turn the screen's pages, in every mode (docs/CONTROLS.md). The
            // track's speed, which FUNC + these used to walk, is in the FUNC menu.
            case ENAGA_KEY_SELECT_LEFT:
                if (!shift_) select(-1);
                break;
            case ENAGA_KEY_SELECT_RIGHT:
                if (!shift_) select(+1);
                break;
            default:
                break;
        }
    }

    // --- the arrow keys -----------------------------------------------------
    //
    // One model for every mode, Enaga's own (its menu and SETTINGS page): UP and DOWN move the
    // cursor through the rows on screen, LEFT and RIGHT move sideways -- the page, where the
    // screen has pages, exactly as the selection keys do. OK picks and CLR goes back. F1 + an
    // arrow is the mode's second meaning (copy and paste in the editing modes).

    void vertical(int delta) {
        switch (mode_) {
            case Mode::grid:
            case Mode::edit:
            case Mode::keys:
                // The tracks are the rows: GRID draws them top to bottom. Silent, unlike a pad,
                // which plays the track it selects.
                walk_track(delta);
                break;
            case Mode::machine:
                // The machine list, applied at once like encoder A.
                walk_machine(delta);
                break;
            case Mode::resample:
                // The take's length, A's other way in.
                take_bars_index_ = (take_bars_index_ + delta + kTakeBarChoices) % kTakeBarChoices;
                break;
            case Mode::mixer:                   // nothing is selected by row on the mixer
            case Mode::project:                 // PROJECT, PERFORM and KIT have their own handlers
            case Mode::perform:
            case Mode::kit:
            case Mode::samples:
            case Mode::menu:
            case Mode::sounds:                  // on_sounds_key has the arrows
                break;
        }
    }

    void horizontal(int delta) {
        if (step_page_) {
            // The step page walks along the track, so a run of steps is edited without going
            // back to the grid for each one.
            const int length = int(engine_.pattern().tracks[selected_].length);
            page_step_ = (page_step_ + delta + length) % (length < 1 ? 1 : length);
            return;
        }
        if (mode_ == Mode::keys) {
            walk_keys_octave(delta);
            return;
        }
        if (mode_ == Mode::grid) {
            // Sideways along the steps: GRID's step page, as the pad-row keys turn it. The
            // screen keys are GRID's knob page (the EDIT page A-D work on), not this.
            page_ = (page_ + delta + 4) % 4;
            return;
        }
        if (mode_ == Mode::machine) {
            // Sideways across the machine grid: a column of three. The arrows are the cursor on
            // what is drawn, and MACHINE draws machines; sideways used to walk the track, and
            // Roland (2026-11-23) turned it expecting the next machine and changed track 7. The
            // track is on the screen keys and the pads, as in SOUNDS.
            walk_machine(delta * 3);
            return;
        }
        select(delta);                          // the page, or what the screen keys walk
    }

    void walk_track(int delta) {
        const int next = (selected_ + delta + kTracks) % kTracks;
        if (next != selected_) end_gesture();   // a different track is a different edit
        selected_ = next;
    }

    void walk_machine(int delta) {
        const int count = int(MachineId::count);
        int next = (int(engine_.machine_id(selected_)) + delta) % count;
        next = next < 0 ? next + count : next;
        post(Cmd::set_machine, uint8_t(selected_), 0, uint8_t(next));
        // `mch <track> <machine>`: one line per change, for the emulator scenarios and a unit
        // log, where nothing else says which machine a track was given.
        char line[16];
        char *out = line;
        append(out, "mch ");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");
        append(out, machine_name(uint8_t(next)));
        *out = 0;
        enaga::log(line);
    }

    void walk_keys_octave(int delta) {
        keys_octave_ = clamp_int(keys_octave_ + delta, -3, 4);
        char *out = notice_buffer_;
        append(out, "OCT ");
        append_number(out, uint32_t(keys_octave_ + 3));
        *out = 0;
        notice(notice_buffer_);
    }

    // --- STEP keys: tap, hold, and a pair for a range --------------------

    // `raw` is the physical key, 0-15. The ABSOLUTE step it means depends on the page, and the
    // page can move while a key is held -- the UP and DOWN keys are not blocked by a hold -- so
    // the page is resolved on the PRESS and remembered. Matching the release on the raw key
    // rather than on a recomputed absolute step is the point: the key you let go of is the key
    // you pressed, whatever the page says by then. Resolving it twice stranded `held_step_` at
    // the old page's value, and a stranded hold turns the next encoder move into a parameter
    // lock on a step nobody is touching.
    // Forget the STEP key being held, and the range it was building. Called on the release and
    // on anything that makes the hold unanswerable.
    // A selection made by "hold a note, press a later step" set that note's LEN when the keys
    // came up. Used afterwards for copy, transpose or nudge, it was a selection after all: the
    // old LEN goes back before the edit, so the edit is the only change.
    void undo_applied_length() {
        if (len_applied_step_ < 0) return;
        post(Cmd::set_step_length, uint8_t(len_applied_track_), uint8_t(len_applied_step_),
             len_applied_old_);
        len_applied_step_ = -1;
    }

    void release_hold() {
        len_step_ = -1;
        held_step_ = -1;
        held_key_ = -1;
        held_used_ = false;
    }

    // What every mode change does: close the step page and drop any hold.
    // Drop a STEP hold that can no longer be answered, and end its gesture. Separate from
    // `leave_mode` because the performance pads deliberately do NOT change the mode -- the row
    // works in every one of them -- so they need the hold dropped without the rest.
    void drop_hold() {
        if (held_step_ < 0) return;
        release_hold();
        end_gesture();
    }

    void leave_mode() {
        step_page_ = false;
        slot_menu_ = false;
        slot_ok_held_ = false;
        // An audition belongs to SAMPLES (dsp/audition.hpp).
        if (mode_ != Mode::samples) stop_audition();
        // LEARN is an EDIT thing, and MIXER's for the kill EQ's bands: any other screen, HOME's
        // GRID included, ends it.
        if (mode_ != Mode::edit && mode_ != Mode::mixer && macro_learn_ >= 0) end_learn();
        // A MOD slot learns from EDIT's knobs only.
        if (mode_ != Mode::edit && mod_learn_ >= 0) end_mod_learn();
        mutation_pending_ = false;
        // A KEYS pad held across a mode change: its release goes to the next mode, so the note
        // is let go here or it would hang on a sustaining machine.
        for (int i = 0; i < kPianoPads; i++) release_pad(i);
        if (held_step_ >= 0) {
            release_hold();
            end_gesture();
        }
    }

    void on_step_key(int raw, bool down) {
        const int step = raw + page_ * 16;
        if (down) {
            if (held_step_ >= 0 && held_step_ != step) {
                // A second STEP held makes the pair a selection, for CLR, copy, paste and the
                // range edits on A and B (docs/SPEC.md, "Step editing").
                range_first_ = held_step_ < step ? held_step_ : step;
                range_last_ = held_step_ < step ? step : held_step_;
                held_used_ = true;      // the first key made a selection; it was not a tap
                len_applied_step_ = -1;
                // Held on a note and pressed LATER: the note's length, up to the end of the step
                // pressed, set when the first key comes up -- unless the selection is used for
                // something first (CLR, copy, paste, a knob), which cancels it.
                len_step_ = -1;
                const Step &first = engine_.pattern().tracks[selected_].steps[held_step_];
                if (held_step_ < step && (first.flags & kStepTrig) != 0) {
                    len_step_ = held_step_;
                    len_index_ = length_index_within_steps(step - held_step_ + 1);
                    Step shown = first;
                    shown.length = len_index_;
                    char value[16];
                    step_field_text(shown, StepField::length, value, int(sizeof value));
                    char *out = notice_buffer_;
                    append(out, "LEN ");
                    append(out, value);
                    *out = 0;
                    notice(notice_buffer_);
                } else {
                    notice("RANGE");
                }
                return;
            }
            // A fresh single press drops any previous selection. Without this, a range made
            // minutes ago would still be what the next CLR erases, which is the sort of thing
            // that loses someone's pattern.
            range_first_ = range_last_ = -1;
            len_applied_step_ = -1;
            held_step_ = step;
            held_key_ = raw;
            held_used_ = false;
            return;
        }

        if (raw != held_key_) return;          // the second key of a range coming back up

        if (len_step_ >= 0) {
            mark(GestureKind::field, selected_, len_step_);
            post(Cmd::set_step_length, uint8_t(selected_), uint8_t(len_step_), len_index_);
            // Remembered while the selection lives, so using it later for something that is
            // not a length (copy, transpose, nudge) puts the old LEN back first.
            len_applied_step_ = len_step_;
            len_applied_track_ = selected_;
            len_applied_old_ = engine_.pattern().tracks[selected_].steps[len_step_].length;
            len_step_ = -1;
        }

        if (!held_used_) {
            // A tap. With F1 it toggles the trigless bit instead: a step that applies its locks
            // without sounding a note.
            mark(GestureKind::trig, selected_, step);
            // On the step the hold STARTED on, not on what this key would mean now. With FILL
            // held, a FILL trig (COND FILL) goes on or comes off.
            if (fill_held_ && !shift_) {
                const Step &was = engine_.pattern().tracks[selected_].steps[held_step_];
                const bool is_fill = (was.flags & kStepTrig) != 0 &&
                                     condition_kind(was.condition) == Condition::FILL;
                post(Cmd::toggle_trig, uint8_t(selected_), uint8_t(held_step_), 2);
                notice(is_fill ? "FILL TRIG OFF" : "FILL TRIG");
            } else {
                post(Cmd::toggle_trig, uint8_t(selected_), uint8_t(held_step_), shift_ ? 1 : 0);
            }
        }
        release_hold();
        end_gesture();
    }

    // --- what the selection keys do depends on where you are -------------

    void select(int delta) {
        if (step_page_) {
            field_page_ = (field_page_ + delta + kFieldPages) % kFieldPages;
            return;
        }
        switch (mode_) {
            case Mode::grid:
            case Mode::keys:
            case Mode::edit:
                // GRID and KEYS turn A-D on the current EDIT page, so their screen pages are
                // EDIT's: SRC .. ASR. One page set for the knobs wherever they are.
                // Pages this machine does not have (a drum's ENV and OSC) are stepped over.
                for (int tries = 0; tries < kEditPages; tries++) {
                    param_page_ = (param_page_ + delta + kEditPages) % kEditPages;
                    if (page_live(param_page_)) break;
                }
                if (mode_ != Mode::edit) notice(page_name(Page(param_page_)));
                break;
            case Mode::mixer:
                // Faders, then the delay, the reverb, the routing and the master.
                mixer_bank_ =
                    (mixer_bank_ + delta + kMixerPageCount * 2) % kMixerPageCount;
                break;
            case Mode::project: {
                // PROJECT's pages: the slots, CHAIN, PATTERN, SETUP and PROJECTS, walked in
                // that order and stopping at the ends. On the screen keys since the controls
                // concept (docs/CONTROLS.md): they were on the pad-row keys, at the bottom of the
                // unit, far from the screen they turned.
                const int next = int(project_page_) + delta;
                const int last = int(ProjectPage::count) - 1;
                const ProjectPage was = project_page_;
                project_page_ = ProjectPage(next < 0 ? 0 : (next > last ? last : next));
                if (project_page_ == ProjectPage::files && was != ProjectPage::files) {
                    list_projects();
                }
                notice(project_page_name(project_page_));
                break;
            }
            case Mode::machine: {
                // The selection keys walk the TRACK, so one hand can go along the eight of them
                // without leaving the mode -- the same thing they do in KEYS. Said out loud: a
                // machine change lands on whatever track this leaves selected.
                selected_ = (selected_ + delta + kTracks) % kTracks;
                end_gesture();
                char *out = notice_buffer_;
                append(out, "TRACK ");
                append_number(out, uint32_t(selected_) + 1);
                *out = 0;
                notice(notice_buffer_);
                break;
            }
            case Mode::resample:
                // With a take on offer they walk the track it would go on, the eight round.
                if (take_offer_ >= 0) {
                    take_offer_ = (take_offer_ + delta % kTracks + kTracks) % kTracks;
                    break;
                }
                // The selection keys walk the DESTINATION: -1 is "the file only", then the eight
                // tracks. A track that hosts no Sampler is still offered, because which machine
                // a track hosts becomes switchable in M6/M7 and the kit's path is what the
                // loader reads either way.
                resample_track_ = resample_track_ + delta;
                if (resample_track_ < -1) resample_track_ = kTracks - 1;
                if (resample_track_ >= kTracks) resample_track_ = -1;
                break;
            case Mode::perform:
                // How hard a mutation mutates. The one number in this mode with no encoder of
                // its own, all four being macros.
                mutate_amount_ = (mutate_amount_ + delta * 10 + 110) % 110;
                if (mutate_amount_ > 100) mutate_amount_ = 100;
                {
                    // Said in a toast: PERFORM's screen has no MUT line since the rebuild.
                    char *out = notice_buffer_;
                    append(out, "MUT ");
                    append(out, mutation_name(mutation_));
                    *out++ = ' ';
                    append_number(out, uint32_t(mutate_amount_));
                    *out = 0;
                    notice(notice_buffer_);
                }
                break;
            case Mode::kit:
                // The selection keys flip the browser's two pages of eight, like LEFT and RIGHT.
                move_kit_cursor_to(kit_cursor_ + delta * 8);
                break;
            case Mode::samples:
                // A page of the list, like LEFT and RIGHT.
                move_sample_cursor_to(sample_cursor_ + delta * kSampleListRows);
                break;
            case Mode::sounds:
                // The track, as in MACHINE, so one hand can try a sound on each of the eight.
                selected_ = (selected_ + delta + kTracks) % kTracks;
                end_gesture();
                retarget_sounds();
                break;
            case Mode::menu:                    // one page
                break;
        }
    }

    // The speed encoder walks kSpeedDisplayOrder (fastest to slowest), not the enum's own
    // numbering -- core/timing.hpp numbers 1x as 0 so that zeroed memory means normal speed,
    // which is deliberately not the order anyone wants to scroll through.
    void walk_speed(int delta) {
        const uint8_t current = engine_.pattern().tracks[selected_].speed;
        int index = 0;
        for (int i = 0; i < int(Speed::count); i++) {
            if (uint8_t(kSpeedDisplayOrder[i]) == current) index = i;
        }
        index += delta;
        if (index < 0) index = 0;
        if (index >= int(Speed::count)) index = int(Speed::count) - 1;
        mark(GestureKind::length, selected_, 0);
        post(Cmd::set_track_speed, uint8_t(selected_), 0, uint8_t(kSpeedDisplayOrder[index]));
        end_gesture();
        echo(Echo::layout);                    // as the length: GRID was the only screen with it
    }

    // The play mode (core/timing.hpp): FWD, REV, PING, RND1 .. RND16, in the enum's own order,
    // which is the order the menu walks. The arrows stop at the ends; OK's step wraps.
    void walk_play(int delta, bool wrap) {
        const int count = int(PlayMode::count);
        int next = int(engine_.pattern().tracks[selected_].play) + delta;
        if (wrap) next = (next + count) % count;
        next = clamp_int(next, 0, count - 1);
        mark(GestureKind::length, selected_, 0);
        post(Cmd::set_track_play, uint8_t(selected_), 0, uint8_t(next));
        end_gesture();
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(selected_ + 1));
        append(out, " PLAY ");
        append(out, play_mode_name(PlayMode(next)));
        *out = 0;
        notice(notice_buffer_);
        // Logged too, one line a press: the emulator can press keys but not read the screen
        // (tests/scenario_playmode.sh).
        enaga::log(notice_buffer_);
    }
