// The editing gestures: CLR, copy and paste, the knobs turned, locks, the performance pads,
// the step stutter and MUTATE.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- CLR, copy and paste ---------------------------------------------

    // What CLR erases is the narrowest thing selected: a held step, then a range, then the whole
    // track. A bare CLR wiping a track is a large gesture to leave unguarded, and it would be
    // reckless without undo -- it is acceptable here because F1 + CLR brings it straight back,
    // which is exactly what this milestone adds.
    void erase_selection() {
        len_step_ = -1;                        // the selection was for this, not a length
        len_applied_step_ = -1;                // and an erase takes the length with the step
        if (held_step_ >= 0) {
            mark(GestureKind::erase, selected_, held_step_);
            post(Cmd::range_erase, uint8_t(selected_), uint8_t(held_step_), 1);
            held_used_ = true;
            notice("STEP CLEARED");
        } else if (range_first_ >= 0) {
            mark(GestureKind::erase, selected_, range_first_);
            post(Cmd::range_erase, uint8_t(selected_), uint8_t(range_first_),
                 uint8_t(range_last_ - range_first_ + 1));
            notice("RANGE CLEARED");
        } else {
            mark(GestureKind::erase, selected_, 0);
            post(Cmd::track_erase, uint8_t(selected_));
            notice("CLEARED FUNC+CLR UNDO");    // 21 characters at most
        }
        end_gesture();
    }

    void copy_selection() {
        len_step_ = -1;
        undo_applied_length();
        int first = 0;
        int count = kStepsPerTrack;
        if (held_step_ >= 0) {
            first = held_step_;
            count = 1;
            held_used_ = true;
        } else if (range_first_ >= 0) {
            first = range_first_;
            count = range_last_ - range_first_ + 1;
        }
        post(Cmd::range_copy, uint8_t(selected_), uint8_t(first), uint8_t(count));
        notice(count == 1 ? "STEP COPIED" : "COPIED");
    }

    void paste_selection() {
        len_step_ = -1;
        len_applied_step_ = -1;                // the paste writes over that step anyway
        const int first = held_step_ >= 0 ? held_step_ : (range_first_ >= 0 ? range_first_ : 0);
        if (held_step_ >= 0) held_used_ = true;
        mark(GestureKind::paste, selected_, first);
        post(Cmd::range_paste, uint8_t(selected_), uint8_t(first));
        end_gesture();
        notice("PASTED");
    }

    void on_encoder(int encoder, int delta) override {
        shift_alone_ = false;                   // FUNC + a knob: the hand knew; no help
        value_alone_ = false;                   // push VALUE + any turn is a chord, not a reset
        if (name_entry_.active()) {
            name_entry_.encoder(encoder, delta);      // A moves, B and VALUE pick the character
            return;
        }
        if (encoder == 4) {
            // VALUE picks the beat-repeat length while that pad is held, which is what
            // docs/SPEC.md gives it, and it has to come before the tempo.
            if (repeat_held_) {
                const int next = int(repeat_length_) + delta;
                repeat_length_ = RepeatLength(
                    next < 0 ? 0
                             : (next >= int(RepeatLength::count) ? int(RepeatLength::count) - 1
                                                                 : next));
                post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), repeat_audio_ ? 1 : 0,
                     shift_ ? 1 : 0);
                notice(repeat_length_name(repeat_length_));
                return;
            }
            // A held STEP key: VALUE sets that step's note (docs/SPEC.md, "Step editing": hold
            // STEP + VALUE). Before the tempo, which it used to turn instead -- with a finger
            // on a step, the tempo is the last thing the hand means. The same field and the
            // same undo coalescing as the step page's NOTE, and the key counts as used, so its
            // release does not toggle the trig.
            if (held_step_ >= 0) {
                held_used_ = true;
                mark(GestureKind::field, selected_, held_step_);
                post_delta(Cmd::step_field_delta, selected_, held_step_,
                           uint8_t(StepField::note), delta);
                notice("NOTE");
                return;
            }
            // A picked effect owns VALUE (core/perffx.hpp): its amount, the looper's mix or the
            // morph, in every mode, until it is let go (push VALUE + its STEP again, or + CLR).
            // Before the fields and the morph VALUE turns otherwise; push + turn keeps its own.
            if (perf_picked_ != PerfFx::none && !value_pushed_ && !shift_) {
                perf_turn(delta);
                return;
            }
            // PROJECT's CHAIN and PATTERN pages: VALUE edits the thing under the cursor -- the
            // entry's repeat count, or the highlighted field. Not with VALUE pushed (swing), which
            // keeps its meaning everywhere, or with F1 (below).
            if (mode_ == Mode::project && !value_pushed_ && !shift_) {
                if (project_page_ == ProjectPage::chain) {
                    chain_delta(ChainEdit::repeats_delta, delta);
                    return;
                }
                if (project_page_ == ProjectPage::pattern) {
                    pattern_field_delta(pattern_field_, delta);
                    return;
                }
            }
            // In PERFORM, VALUE is the morph: the one continuous control the layer has.
            if (mode_ == Mode::perform && !value_pushed_ && !shift_) {
                morph_set(int(morph_) + delta * (int(kMorphScale) / 64));
                return;
            }
            if (value_pushed_ && shift_) {
                // FUNC + push VALUE + turn is the tempo nudge (core/taptempo.hpp): each detent
                // holds the tempo 4% faster or slower for a quarter of a second, then it goes
                // back, so the beat moves against a source that sends no clock and the tempo
                // stays where it was set. Not saved. Beside swing (push + turn), the other
                // timing move; FUNC + turn VALUE is VariSpeed (Roland, 2026-11-28).
                if (enaga::clock::source() != ENAGA_CLOCK_INTERNAL) {
                    clock_refused_ = true;
                    notice("EXT CLOCK");
                    return;
                }
                // Times VariSpeed's ratio, as every clock tempo is (set_clock_tempo), but straight
                // to the clock: through set_clock_tempo a nudge near 300 BPM would pull
                // VariSpeed back for good, where it only has to clamp for a quarter second.
                nudge_turn(nudge_, delta, last_ms_);
                float bpm = nudged_bpm(tempo_tenths_, nudge_.direction) * vari_ratio(vari_);
                if (bpm < float(kTempoTenthsMin) * 0.1f) bpm = float(kTempoTenthsMin) * 0.1f;
                if (bpm > float(kTempoTenthsMax) * 0.1f) bpm = float(kTempoTenthsMax) * 0.1f;
                clock_refused_ = !enaga::clock::set_tempo(bpm);
                notice_logged(nudge_.direction > 0 ? "NUDGE FASTER" : "NUDGE SLOWER");
                return;
            }
            if (value_pushed_) {
                // VALUE pushed and turned is the pattern's swing, as the spec's key table says.
                mark(GestureKind::length, 0, 0);
                post(Cmd::set_pattern_swing, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
                return;
            }
            if (shift_) {
                // FUNC + VALUE is VariSpeed (core/varispeed.hpp): VALUE is the tempo, and with
                // FUNC the pitch moves with it, like a record's speed. It was the master DJ
                // filter until 2026-11-25 (backlog D1), whose live home is the mixer's slots 1
                // and 2, and then free, naming those slots.
                vari_turn_by(delta);
                return;
            }
            // REFUSED under an external clock, rather than moved and then ignored. docs/SPEC.md:
            // VALUE "shows EXT and refuses edits when CLOCK SRC is not INTERNAL". Before this
            // the knob moved `tempo_tenths_` and Enaga threw the call away, so the app's idea of
            // the tempo and the unit's drifted apart with every click.
            if (enaga::clock::source() != ENAGA_CLOCK_INTERNAL) {
                clock_refused_ = true;
                notice("EXT CLOCK");
                return;
            }
            // Clamped as a SIGNED value, before the cast. `tempo_tenths_` is unsigned, so a
            // delta large enough to take the sum below zero used to wrap to about four billion
            // and then clamp to 3000 -- 300 BPM -- when the user was turning the knob DOWN. Not
            // reachable from the panel, where an encoder event carries a click or two, but it is
            // one line either way and the wrong end of the range is a bad place to arrive.
            int tenths = int(tempo_tenths_) + delta;
            if (tenths < 200) tenths = 200;                    // Enaga's clock allows 20-300
            if (tenths > 3000) tenths = 3000;
            tempo_tenths_ = uint32_t(tenths);
            // Main context again.
            clock_refused_ = !set_clock_tempo(tempo_tenths_ * 10u);      // times VariSpeed's
            // And the project, which is where the tempo is SAVED. Nothing posted this before, so
            // `ProjectSettings::tempo_centi_bpm` round-tripped through the serializer while
            // holding the default for ever: a project saved at 142 reopened at 130. The engine
            // also re-derives the tempo-synced delay from it.
            post_tempo(float(tempo_tenths_) * 0.1f);
            adopted_tempo_centi_ = uint16_t(tempo_tenths_ * 10);
            return;
        }

        // The ENCODER EVENT id is alphabetical (0-3 = A-D), unlike the PUSH-button key codes,
        // which run B, A, C, D. See docs/ENAGA_API.md -- confusing the two silently swaps A
        // and B, and nothing catches it.

        // FUNC + turn A / B in PERFORM: REL ALL and LEN ALL (core/moveall.hpp), the Moves layer's
        // two global controls beside the macros. Before the macros, which FUNC + turn only
        // duplicated. 0 is a detent and rest: the programmed sound to the bit.
        if (mode_ == Mode::perform && shift_ && (encoder == 0 || encoder == 1)) {
            int8_t &amount = encoder == 0 ? rel_all_ : len_all_;
            amount = all_turn(amount, delta);
            post(Cmd::set_move_all, 0, 0, uint8_t(encoder), uint8_t(amount));
            move_all_notice(encoder);
            return;
        }

        // FUNC + turn C in PERFORM: ENERGY (core/energy.hpp), from a sparse breakdown at -64 to
        // the peak at +64. 0 is a detent and rest: the programmed part to the bit. Before the
        // macros, which FUNC + turn only duplicated.
        if (mode_ == Mode::perform && shift_ && encoder == 2) {
            energy_ = energy_turn(energy_, delta);
            post(Cmd::set_energy, 0, 0, uint8_t(energy_));
            energy_notice();
            return;
        }

        // FUNC + turn D in PERFORM: LIVE RND (core/liverand.hpp), the unit for every armed track,
        // OFF 1 2 4 8 16 and stopping at both ends.
        if (mode_ == Mode::perform && shift_ && encoder == 3) {
            const uint8_t was = rnd_unit_;
            rnd_unit_ = live_rnd_turn(rnd_unit_, delta);
            // LIVE RND starts with the kick out (Roland, 2026-11-28, D3): a kick drawn out of
            // order takes the floor away. Every track but those playing KICK, each time it comes
            // on from OFF, until a FUNC + lower pad (or a LOCK) has chosen the set by hand.
            if (was == kLiveRndOff && rnd_unit_ != kLiveRndOff && !rnd_armed_chosen_) {
                rnd_armed_ = kLiveRndAllTracks;
                for (int t = 0; t < kTracks; t++) {
                    if (engine_.machine_id(t) == uint8_t(MachineId::kick)) {
                        rnd_armed_ = uint8_t(rnd_armed_ & ~(1u << t));
                    }
                }
            }
            post(Cmd::set_live_rnd, 0, 0, rnd_unit_, rnd_armed_);
            live_rnd_notice();
            return;
        }

        // A to D are the four macros in PERFORM mode, which is what they are for.
        if (mode_ == Mode::perform && encoder >= 0 && encoder < kMacros) {
            const int next = int(macro_position_[encoder]) + delta * 2;
            macro_position_[encoder] =
                uint8_t(next < 0 ? 0 : (next > 127 ? 127 : next));
            post(Cmd::set_macro, 0, 0, uint8_t(encoder), macro_position_[encoder]);
            // The bars move a pixel per four or five clicks, and a macro with nothing assigned
            // moves nothing at all, so a turn says where the macro is -- or that it is empty.
            {
                char *out = notice_buffer_;
                append(out, "MACRO ");
                *out++ = char('A' + encoder);
                if (macro_count(engine_.macros().macros[encoder]) == 0) {
                    append(out, " EMPTY");
                } else {
                    *out++ = ' ';
                    append_number(out, macro_position_[encoder]);
                }
                *out = 0;
                notice(notice_buffer_);
            }
            return;
        }

        // MACHINE mode's one encoder: A walks the machines this build has. The change is posted
        // on every click rather than on an OK, because hearing it is the point -- a machine
        // select you have to confirm is a menu, and docs/SPEC.md's whole argument for this box is
        // that a change is one gesture.
        if (mode_ == Mode::machine && encoder == 0) {
            walk_machine(delta);
            return;
        }
        // And B the track's MIDI OUT channel: OFF, 1-16, stopping at the ends. A delta, applied
        // on the audio side, like every knob. C and D set nothing here, and must not fall through
        // to the sound parameters of a screen that is not showing.
        if (mode_ == Mode::machine && encoder >= 1 && encoder < 4) {
            if (encoder == 1) {
                post(Cmd::set_midi_out, uint8_t(selected_), 0, 0, clamp_delta(delta), kDeltaFlag);
            }
            return;
        }

        // The FUNC menu: A scrolls it, as UP and DOWN do, and B changes the value of a setting
        // row, as LEFT and RIGHT do. C and D have nothing to set here.
        // The track menu in GRID and KEYS, the same way: A the row, B the value.
        if (track_menu_ && track_menu_screen() && held_step_ < 0 && encoder >= 0 &&
            encoder < 4) {
            if (encoder == 0) {
                track_menu_cursor_ = step_wrap(track_menu_cursor_, delta > 0 ? 1 : -1, 0,
                                               kTrackRows - 1);
            }
            if (encoder == 1) track_row_change(delta > 0 ? 1 : -1, false);
            return;
        }

        if (mode_ == Mode::menu && encoder >= 0 && encoder < 4) {
            if (encoder == 0) move_menu_cursor(delta);
            if (encoder == 1) menu_value(delta > 0 ? 1 : -1, false);
            return;
        }

        // KIT mode: A walks the slot cursor, as UP and DOWN do. B to D have nothing to set here,
        // and must not fall through to the sound parameters of a screen that is not showing.
        if (mode_ == Mode::kit && encoder >= 0 && encoder < 4) {
            if (encoder == 0) move_kit_cursor_to(kit_cursor_ + delta);
            return;
        }

        // SOUNDS: A walks the list, as UP and DOWN do; nothing loads until OK. B to D have
        // nothing to set here.
        if (mode_ == Mode::sounds && encoder >= 0 && encoder < 4) {
            if (encoder == 0) move_sound_cursor_to(sound_cursor_ + delta);
            return;
        }

        // SAMPLES: A scrolls the list, as UP and DOWN do. B to D have nothing to set here.
        if (mode_ == Mode::samples && encoder >= 0 && encoder < 4) {
            if (encoder == 0) move_sample_cursor_to(sample_cursor_ + delta);
            return;
        }

        // RESAMPLE mode's encoders: A walks the four lengths docs/SPEC.md allows, B the source
        // (the master or an input route), C the input's level. D has nothing to set here, and
        // must not fall through to the sound parameters of a screen that is not showing.
        if (mode_ == Mode::resample && encoder >= 0 && encoder < 4) {
            if (encoder == 0) {
                const int next = (take_bars_index_ + delta) % kTakeBarChoices;
                take_bars_index_ = next < 0 ? next + kTakeBarChoices : next;
            } else if (encoder == 1) {
                take_source_ = clamp_int(take_source_ + (delta > 0 ? 1 : -1), 0,
                                         kTakeSourceChoices - 1);
                if (kTakeSources[take_source_].input >= 0) apply_input();
            } else if (encoder == 2 && kTakeSources[take_source_].input >= 0) {
                take_level_ = clamp_int(take_level_ + (delta > 0 ? 1 : -1), 0, 2);
                apply_input();
            }
            return;
        }

        // The SETUP page: A is the record quantize amount. The other three settings on it --
        // CLOCK SRC, CLOCK OUT / SYNC OUT and MIDI CH -- are the USER'S, set in the Enaga menu;
        // <enaga/clock.hpp> and <enaga/midi.hpp> expose no way to write them, so STRATA shows
        // them and says where they live, which is all docs/SPEC.md's "STRATA only exposes the
        // settings" can mean.
        // CHAIN: A is the entry's repeat count. PATTERN: A is LEN, B is SWG and C the swing of
        // the track under the cursor (the selected track's, when the cursor is on LEN or SWG).
        if (section_view()) {
            section_turn(encoder, delta);         // app/ui_section.hpp
            return;
        }
        if (mode_ == Mode::project && project_page_ == ProjectPage::chain) {
            if (encoder == 0) chain_delta(ChainEdit::repeats_delta, delta);
            if (encoder == 1) chain_delta(ChainEdit::mutes_delta, delta);
            return;
        }
        if (mode_ == Mode::project && project_page_ == ProjectPage::pattern) {
            if (encoder == 0 || encoder == 1) {
                pattern_field_ = encoder;
                pattern_field_delta(encoder, delta);
            } else if (encoder == 3) {
                pattern_field_ = kPatternFieldFeel;
                pattern_field_delta(kPatternFieldFeel, delta);
            } else if (encoder == 2) {
                if (pattern_field_ < kPatternFieldTrackSwing) {
                    pattern_field_ = kPatternFieldTrackSwing + selected_;
                }
                pattern_field_delta(pattern_field_, delta);
            }
            return;
        }
        if (mode_ == Mode::project && project_page_ == ProjectPage::files) {
            if (encoder == 0) move_project_cursor_to(project_cursor_ + delta);
            return;
        }
        if (mode_ == Mode::project && project_setup()) {
            if (encoder != 0) return;
            post_delta(Cmd::set_quantize, 0, 0, 0, delta * 5);
            notice("QUANTIZE");
            return;
        }

        // PROJECT mode's own encoders: A picks when a queued change lands, B the key and C the
        // scale. A is only ever read when a pattern is queued, so it changes nothing by itself;
        // B and C apply at once, because the global key is what the melodic tracks and every
        // mutation fold into and there is nothing to confirm.
        if (mode_ == Mode::project) {
            if (encoder == 0) {
                const int next = int(queue_mode_) + delta;
                queue_mode_ = QueueMode(next < 0 ? 0
                                                 : (next >= int(QueueMode::count)
                                                        ? int(QueueMode::count) - 1
                                                        : next));
                notice(queue_mode_name(queue_mode_));
                return;
            }
            if (encoder == 1) {
                // The key wraps: twelve pitch classes in a circle is what they are, and walking
                // off one end to be stopped there would be wrong rather than merely annoying.
                const int root = (int(engine_.key_root()) + delta % 12 + 12) % 12;
                post(Cmd::set_root, 0, 0, uint8_t(root));
                notice(note_name(uint8_t(root)));
                return;
            }
            if (encoder == 2) {
                // The scale does NOT wrap, and it is clamped to what this build has. An id a
                // later build wrote survives a load (core/project.cpp leaves it alone), so the
                // user can be looking at a scale the encoder cannot reach -- walking from there
                // lands on the nearest one this build knows rather than jumping to chromatic.
                const int current = int(engine_.key_scale());
                int next = (current < kScaleCount ? current : kScaleCount - 1) + delta;
                next = next < 0 ? 0 : (next >= kScaleCount ? kScaleCount - 1 : next);
                post(Cmd::set_scale, 0, 0, uint8_t(next));
                notice(scale_name(next));
                return;
            }
        }
        if (encoder < 0 || encoder > 3) return;
        len_step_ = -1;                        // a knob turned under a selection: not a length

        // 1. The step page: A-D are four of the step's own fields. Before a held STEP key: the
        //    page is opened by holding STEP + OK, and a hand still on that STEP key while it
        //    turns C means LEN on the page it is looking at, not a lock on CUT.
        if (step_page_) {
            if (held_step_ >= 0) held_used_ = true;      // the release must not toggle the trig
            const int field = field_page_ * 4 + encoder;
            if (field >= kStepFields) return;
            mark(GestureKind::field, selected_, page_step_);
            post_delta(Cmd::step_field_delta, selected_, page_step_, uint8_t(field), delta);
            return;
        }

        // 2. A held STEP key: lock this page's parameter on that step. The single most
        //    important gesture in the whole app (docs/SPEC.md, "Step editing") -- and the
        //    reason the step's release has to know the key was used as a modifier.
        //    With FUNC it is the parameter's RANDOM RANGE on the step instead (Phase 5): each
        //    pass the step plays the lock, or the kit value, moved by a draw within the width.
        if (held_step_ >= 0) {
            held_used_ = true;
            const int index = edit_index(encoder);
            mark(GestureKind::lock, selected_, held_step_);
            if (shift_) {
                post_delta(Cmd::lock_range_delta, selected_, held_step_, uint8_t(index),
                           delta * param_click(index));
            } else {
                post_param_lock(held_step_, index, delta);
            }
            show_lock_readout(index);
            return;
        }


        if (mode_ == Mode::mixer) {
            if (mixer_bank_ < kMixerTrackPages) {
                // A-D are the levels of the four tracks in the current bank.
                const int track = mixer_bank_ * 4 + encoder;
                if (track >= kTracks) return;
                Command command;
                command.cmd = Cmd::set_track_level;
                command.track = uint8_t(track);
                command.c = kDeltaFlag;            // the engine adds it and clamps at 1.5
                command.f = float(delta) * 0.02f;
                post_command(command);
                return;
            }
            if (mixer_width_page(mixer_bank_)) {
                edit_width(mixer_bank_, encoder, delta);
                return;
            }
            if (mixer_bank_ < kMixerDelayPage) {
                edit_duck(mixer_bank_, encoder, delta);
                return;
            }
            if (mixer_chorus_send_page(mixer_bank_)) {
                edit_chorus_send(mixer_bank_, encoder, delta);
                return;
            }
            if (mixer_input_page(mixer_bank_)) {
                edit_input(mixer_bank_, encoder, delta);
                return;
            }
            // LEARN takes the kill EQ's bands here (D11): before the delta is posted, so the
            // touch keeps the band's value from before the turn.
            if (macro_learn_ >= 0 && mixer_bank_ == kMixerEqPage && encoder < kMasterBands) {
                learn_touch(kMacroMasterTrack, uint8_t(encoder));
            }
            edit_mixer_field(mixer_bank_, encoder, delta);
            return;
        }

        // 3. A selection with no step held: A transposes it, B nudges its micro-timing. The
        //    only two edits that are genuinely about a span rather than a step.
        if (range_first_ >= 0 && encoder < 2) {
            undo_applied_length();
            const int count = range_last_ - range_first_ + 1;
            mark(GestureKind::field, selected_, range_first_);
            post_delta(encoder == 0 ? Cmd::range_transpose : Cmd::range_nudge,
                       selected_, range_first_, uint8_t(count), delta);
            return;
        }

        // 4. Recording, with the transport running: motion recording. The knob writes a lock on
        //    the step the track is playing right now, so a sweep lands across the steps it
        //    passes over (docs/SPEC.md, "Recording"). The gesture carries step 0 rather than the
        //    playhead so that the whole pass is ONE undo level instead of one per step.
        if (rec_ && engine_.status().running.load(std::memory_order_relaxed)) {
            const int step = int(engine_.status().playhead[selected_].load(std::memory_order_relaxed));
            const int index = edit_index(encoder);
            mark(GestureKind::motion, selected_, 0);
            post_param_lock(step, index, delta);
            return;
        }

        // 4b. F1 + A, B or C in GRID: the Euclidean generator's three numbers, applied live.
        //     docs/SPEC.md asks for "hits, steps, rotation" and these are them. Case 5 below
        //     never looked at F1, so this gesture was free.
        //
        //     Each click is one edit and they share a gesture, so core/undo coalesces a whole
        //     sweep into a single level -- the same arrangement the step-field encoders use, and
        //     the reason a knob turned forty times does not throw away the undo history.
        if (shift_ && mode_ == Mode::grid && encoder < 3) {
            if (encoder == 0) {
                euclid_hits_ = clamp_int(euclid_hits_ + delta, 0, euclid_steps_);
            } else if (encoder == 1) {
                euclid_steps_ = clamp_int(euclid_steps_ + delta, 1, kStepsPerTrack);
                if (euclid_hits_ > euclid_steps_) euclid_hits_ = euclid_steps_;
            } else {
                // Rotation wraps: a figure rotated by its whole cycle is the same figure, so
                // there is nothing to stop at.
                euclid_rotation_ = (euclid_rotation_ + delta) % euclid_steps_;
            }

            mark(GestureKind::length, selected_, 0);
            post(Cmd::euclid, uint8_t(selected_), 0, uint8_t(euclid_hits_),
                 uint8_t(euclid_steps_), uint8_t(int8_t(euclid_rotation_)));

            char *out = notice_buffer_;
            append(out, "EUC ");
            append_number(out, uint32_t(euclid_hits_));
            *out++ = '/';
            append_number(out, uint32_t(euclid_steps_));
            if (euclid_rotation_ != 0) {
                append(out, " R");
                append_number(out, uint32_t(euclid_rotation_ < 0 ? -euclid_rotation_
                                                                 : euclid_rotation_));
            }
            *out = 0;
            notice(notice_buffer_);
            return;
        }

        // 5. Otherwise A-D edit the four parameters of the current page on the selected track.
        // One click is one shape or one destination on the LFO's WAV and DST (dsp/lfo.hpp,
        // `lfo_click`), and one count everywhere else.
        const int index = edit_index(encoder);
        // A knob with nothing on it on this machine (a drum's ENV and OSC, an empty slot) moves
        // nothing and says so, rather than writing a byte no machine reads.
        if (knob_dead(index)) {
            notice("NOTHING ON THIS KNOB");
            return;
        }
        // In NOTE view a click on TUN or PIT is a semitone (core/tuning.hpp, `tune_step`).
        post(Cmd::set_param, uint8_t(selected_), 0, uint8_t(index),
             clamp_delta(shows_note(index) ? delta : delta * param_click(index)),
             shows_note(index) ? kSemitoneFlag : kDeltaFlag);
        // KEYS has no knob slots on screen (its sixteen notes fill it), so a turn there changed
        // the sound and showed nothing. GRID and EDIT draw the slots and need no notice.
        if (mode_ == Mode::keys) echo(Echo::param, index);
        if (macro_learn_ >= 0 && mode_ == Mode::edit) learn_touch(uint8_t(selected_), uint8_t(index));
        if (mod_learn_ >= 0 && mode_ == Mode::edit) mod_learn_touch(index);
        // A macro turned up owns its parameters (a macro at 0 lets go): the knob moves the
        // setting underneath, but the sound follows the macro. Say which one, or the knob
        // looks broken (Roland, 2026-11-23: "editing of sounds does not work anymore").
        const int owner = macro_holding(selected_, index);
        if (owner >= 0) {
            char *out = notice_buffer_;
            append(out, "HELD BY MACRO ");
            *out++ = char('A' + owner);
            *out = 0;
            notice(notice_buffer_);
        }
    }

    // A parameter this machine does not have: it shows as "---".
    bool knob_dead(int index) const {
        return index < 0 || param_name_for(index)[0] == '-';
    }

    // An EDIT page with something on it for this machine: one of its four knobs is live.
    bool page_live(int page) const {
        for (int knob = 0; knob < 4; knob++) {
            if (!knob_dead(edit_slot(engine_.machine_id(selected_), page, knob))) return true;
        }
        return false;
    }

    // The macro, turned up from 0, that has `param` of `track` among its targets, or -1.
    int macro_holding(int track, int param) const {
        for (int m = 0; m < kMacros; m++) {
            if (engine_.macro_position(m) == 0) continue;
            const Macro &macro = engine_.macros().macros[m];
            for (int i = 0; i < kMacroTargets; i++) {
                const MacroTarget &t = macro.targets[i];
                if (t.enabled != 0 && t.track == track && t.param == param) return m;
            }
        }
        return -1;
    }

    // A knob turned into a lock on `step` of the selected track: counts, or semitones on TUN or
    // PIT in NOTE view. The audio side starts from the step's lock, or the kit value if it has
    // none, either way.
    void post_param_lock(int step, int index, int delta) {
        if (knob_dead(index)) {                 // no lock on a knob with nothing on it
            notice("NOTHING ON THIS KNOB");
            return;
        }
        Command command;
        command.cmd = Cmd::lock_delta;
        command.track = uint8_t(selected_);
        command.step = uint8_t(step);
        command.a = uint8_t(index);
        command.b = clamp_delta(shows_note(index) ? delta : delta * param_click(index));
        command.c = shows_note(index) ? kSemitoneFlag : 0;
        post_command(command);
    }

    void on_pad_press(int pad, int velocity) override {
        shift_alone_ = false;                   // FUNC + a pad: the hand knew; no help
        value_alone_ = false;
        // KEYS: both rows are a piano. FUNC + a pad keeps its usual job (mute on the bar, the
        // performance moves), so a mute is still a reach away while playing.
        if (mode_ == Mode::keys && !shift_ && pad >= 0 && pad < kPianoPads) {
            play_pad(pad, velocity);
            return;
        }
        // D6, "STEP first edits, pad first performs": with a STEP held, FILL, REPEAT, MUTE and
        // MUTATE step one field of that step through a short cycle instead of performing
        // (core/stepedit.hpp); THROW locks the step's delay send, or with FUNC its reverb send.
        // Otherwise not with FUNC, whose pad chords stay, and not in KEYS (above), where the
        // upper pads are the piano's black keys.
        if (held_step_ >= 0 && is_perform_pad(pad) &&
            (!shift_ || PerformPad(pad) == PerformPad::throw_fx) && edit_held_step(pad)) {
            return;
        }
        // The performance row first: it is the same seven pads in every mode, which is what
        // docs/SPEC.md means by a fixed row.
        if (is_perform_pad(pad)) {
            on_perform_pad(pad, true);
            return;
        }

        const int track = track_for_pad(pad);
        if (track < 0 || track >= kTracks) return;
        // LAUNCH: the pad picks the lane, silently (app/ui_launch.hpp).
        if (launch_pad(track, true)) return;

        // FUNC + lower pad in PERFORM: that track in or out of LIVE RND (core/liverand.hpp),
        // heard at once while a unit is on (Roland, 2026-11-28, D2: FUNC + push D is the kill EQ's
        // on every screen). MUTE mode keeps the pads mutes.
        if (shift_ && mode_ == Mode::perform && !mute_mode_) {
            live_rnd_arm(track);
            return;
        }

        // F1 + pad mutes in any mode, and MUTE mode makes the lower pads mutes outright. With
        // F1 the mute waits for the bar: a held F1 is the performer asking for it to land in
        // time rather than under their finger.
        if (shift_ || mute_mode_) {
            // On a stopped box the bar is not coming: FUNC + pad mutes at once there, as the
            // performer would otherwise press and hear nothing happen until PLAY.
            const bool at_bar = shift_ && enaga::clock::running();
            post(Cmd::set_mute, uint8_t(track), 0, engine_.muted(track) ? 0 : 1,
                 at_bar ? 1 : 0);
            // The bar-quantized one waits, so until the bar nothing on screen moved. Say what
            // is armed, or on a stopped box what was done (KEYS shows no mutes of its own).
            if (shift_) {
                char *out = notice_buffer_;
                append(out, "T");
                append_number(out, uint32_t(track + 1));
                append(out, engine_.muted(track) ? " UNMUTE" : " MUTE");
                if (at_bar) append(out, " AT BAR");
                *out = 0;
                notice(notice_buffer_);
            }
            return;
        }

        if (track != selected_) {
            end_gesture();                             // a different track is a different edit
            drop_range();
        }
        selected_ = track;
        // The root in a playable octave: the pattern's root is a pitch class (core/pattern.hpp).
        const uint8_t note = uint8_t(kPadOctaveNote + engine_.key_root());

        // NOTE RPT: the pad rolls for as long as it is held. With REC on it is recorded as ONE
        // step, a roll over LEN at the repeat rate (CyDrums P3): the press is the step, the
        // release its LEN. A rate the step byte cannot hold (1/4T) records the plain hit.
        if (note_repeat_mode_) {
            rolling_pads_ = uint16_t(rolling_pads_ | (1u << track));
            roll_velocity_[track] = uint8_t(velocity < 1 ? 1 : velocity);
            roll_note_[track] = note;
            post(Cmd::note_repeat, uint8_t(track), 0, note, roll_velocity_[track], 1);
            const uint8_t rate = ratchet_rate_for_ticks(repeat_length_ticks(repeat_length_));
            const uint8_t roll = rate < kRatchetRates ? ratchet_pack_span(rate, RatchetRamp::none)
                                                      : uint8_t(0);
            record_note(track, note, roll_velocity_[track], roll != 0, roll);
            return;
        }

        // A VA whose ASR page holds notes (SUS up): the pad is a key, held until it comes up,
        // exactly as a piano pad in KEYS -- so a pad sound plays as long as the finger stays.
        // At SUS 0 it stays a hit, so a long DEC is not cut short by lifting the finger.
        if (holds_notes(track)) {
            release_pad(pad);
            pad_held_note_[pad] = int16_t(note);
            pad_held_track_[pad] = uint8_t(track);
            post(Cmd::trigger_note, uint8_t(track), 0, note, uint8_t(velocity), 1);
            record_note(track, note, uint8_t(velocity));
            return;
        }
        post(Cmd::trigger_note, uint8_t(track), 0, note, uint8_t(velocity));
        record_note(track, note, uint8_t(velocity), false);   // a hit: nothing to let go of
    }

    void on_pad_release(int pad) override {
        // A piano pad lets its note go whatever the mode is by now (leave_mode already did, if
        // the mode changed under it).
        if (pad >= 0 && pad < kPianoPads && pad_held_note_[pad] >= 0) {
            release_pad(pad);
            return;
        }
        // A pad that edited a held step comes up with nothing to let go.
        if (is_perform_pad(pad) && (step_edit_pads_ >> (pad / 2) & 1u) != 0) {
            step_edit_pads_ = uint8_t(step_edit_pads_ & ~(1u << (pad / 2)));
            return;
        }
        if (is_perform_pad(pad)) {
            on_perform_pad(pad, false);
            return;
        }

        // A rolling pad stops on its own release, whatever the mode is by now: NOTE RPT turned
        // off under a held pad must not leave the roll running.
        const int track = track_for_pad(pad);
        if (track >= 0 && track < kTracks && launch_pad(track, false)) return;
        mod_pressure_release(track);           // PRS back to 0 (app/ui_modslots.hpp)
        if (track >= 0 && track < kTracks && (rolling_pads_ >> track & 1u) != 0) {
            rolling_pads_ = uint16_t(rolling_pads_ & ~(1u << track));
            post(Cmd::note_repeat, uint8_t(track), 0, 0, 0, 0);
            record_note_up(track, roll_note_[track]);   // a recorded roll's LEN
            return;
        }

        // A pad that was bending macro 1 lets it go. The macro returns to where the ENCODER
        // left it, not to zero: the knob's position is the setting and the pressure is a bend on
        // top of it, so letting go must not throw the setting away. Without this, pressing a pad
        // once would leave macro 1 wherever the finger happened to lift.
        if (pressure_pad_ == pad) {
            pressure_pad_ = -1;
            post(Cmd::set_macro, 0, 0, 0, macro_position_[0]);
        }
    }

    // docs/SPEC.md: "Pad pressure modulates the selected track's macro 1 while a pad is held."
    //
    // Enaga's event is already 0-127 through its own cube curve, and the spec's `[rev]` note is
    // explicit that the app-side smoothing and change-filtering this document originally planned
    // over raw readings is no longer needed -- so this does no filtering, and adding any would be
    // undoing Enaga's shaping with a second guess at it.
    //
    // The pressure takes the macro only UPWARD from where the encoder left it. A pad that bent
    // the macro DOWN would mean resting a finger quietly on a pad changed the sound away from
    // what the knob said, and a performer leaning on a pad expects more of something rather than
    // less. So the macro goes to the larger of the knob's position and the pressure, and
    // `on_pad_release` puts the knob's position back.
    //
    // Only the pad the press came from, and only one at a time: with two pads held, the first
    // owns the macro until it is released. Pressure events arrive about 262 times a second per
    // pad (docs/SPEC.md), which would otherwise be two streams fighting over one value.
    void on_pad_pressure(int pad, int pressure) override {
        // `track_for_pad` is `pad % 2 == 0 ? pad / 2 : -1`, so this one test does three jobs:
        // it rejects the performance row (the odd pads, which are tools and not keys -- leaning
        // on FILL must not change the sound), it rejects anything out of range, and because each
        // track has exactly ONE pad there can never be a second pad competing for this macro.
        //
        // Guards for the second and third of those were written here and then removed: both
        // survived mutation testing, because this line already covers them. A guard that cannot
        // fire is a claim about a situation that cannot arise.
        // A held piano pad in KEYS bends the selected track's macro 1 like a track pad does.
        const bool piano = pad >= 0 && pad < kPianoPads && pad_held_note_[pad] >= 0;
        const int track = piano ? selected_ : track_for_pad(pad);
        if (track < 0 || track >= kTracks) return;
        // A slot on PRS hears every pad of its track, rolling or not, selected or not.
        mod_pressure(track, pressure);

        // A rolling pad's pressure is the roll's velocity instead: lean in and the roll swells.
        // Change-filtered like the macro, and never below 1, which is silence on most machines.
        if (!piano && (rolling_pads_ >> track & 1u) != 0) {
            const uint8_t velocity =
                uint8_t(pressure < 1 ? 1 : (pressure > 127 ? 127 : pressure));
            if (velocity == roll_velocity_[track]) return;
            roll_velocity_[track] = velocity;
            post(Cmd::note_repeat, uint8_t(track), 0, 0, velocity, 2);
            return;
        }

        if (track != selected_) return;                   // "the SELECTED track's macro 1"

        const uint8_t clamped = uint8_t(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
        const uint8_t floor = macro_position_[0];
        const uint8_t value = clamped > floor ? clamped : floor;

        // One command per CHANGE, not per event. 262 events a second per pad against a ring the
        // audio side drains at a bounded rate per block is a waste of both; a pad held steady
        // produces the same number repeatedly and the first of them is enough.
        if (pressure_pad_ == pad && value == pressure_sent_) return;
        pressure_pad_ = pad;
        pressure_sent_ = value;
        post(Cmd::set_macro, 0, 0, 0, value);
    }

    // The seven upper pads. Four of them are HELD rather than toggled -- fill, repeat, throw and
    // the two scene selectors -- so each one's release matters as much as its press.
    // Hold STEP + an upper pad (D6): the held step's condition, ratchet or probability one
    // press further round its cycle, said as `STEP 5 PROB 50%`. False for a pad with no cycle
    // (the scenes, THROW), which then performs as usual. The release counts as a use, so the
    // STEP does not toggle its trig when it comes up.
    // Step stutter (Cmd::step_stutter): REPEAT held, then STEP n. Every track loops its step n
    // at the rate; FUNC + STEP only the selected track. The audio repeat the pad started gives
    // way for the rest of the hold: the stutter replays live sound, so filters, LFOs and tails
    // keep moving. A second STEP moves the stutter there; letting it go goes back to the one
    // still held, if any.
    void stutter_press(int raw) {
        stutter_keys_ = uint16_t(stutter_keys_ | (1u << raw));
        stutter_raw_ = raw;
        stutter_tracks_ = shift_ ? uint8_t(1u << selected_) : uint8_t(0xFF);
        if (repeat_audio_) {
            repeat_audio_ = false;
            post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), 0, repeat_pitch_ ? 1 : 0);
        }
        stutter_post();
    }

    void stutter_release(int raw) {
        stutter_keys_ = uint16_t(stutter_keys_ & ~(1u << raw));
        if (raw != stutter_raw_) return;       // an earlier STEP coming up: the latest plays on
        if (stutter_keys_ != 0) {
            int next = 0;
            while ((stutter_keys_ >> next & 1u) == 0) next++;
            stutter_raw_ = next;
            stutter_post();
            return;
        }
        stutter_raw_ = -1;
        post(Cmd::step_stutter, 0, 0, 0, 0, 0);
        notice(repeat_held_ ? "STUTTER OFF" : "PADS PLAY");
    }

    void stutter_post() {
        const int step = stutter_raw_ + page_ * 16;
        stutter_step_ = step;
        post(Cmd::step_stutter, 0, 0, uint8_t(step), 1, stutter_tracks_);
        char *out = notice_buffer_;
        if (stutter_tracks_ != 0xFF) {
            append(out, "T");
            append_number(out, uint32_t(selected_) + 1);
            *out++ = ' ';
        }
        append(out, "STUTTER ");
        append_number(out, uint32_t(step) + 1);
        *out++ = ' ';
        append(out, repeat_length_name(repeat_length_));
        *out = 0;
        notice(notice_buffer_);
    }

    // REPEAT + LEFT/RIGHT, or LEFT/RIGHT under a rolling pad: the rate one step slower or
    // faster (dsp/repeat.hpp's repeat_length_step), heard at once by the repeat, the roll and
    // the stutter alike, and said on screen.
    void step_repeat_rate(int direction) {
        repeat_length_ = repeat_length_step(repeat_length_, direction);
        post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), repeat_audio_ ? 1 : 0,
             repeat_pitch_ ? 1 : 0);
        char *out = notice_buffer_;
        append(out, stutter_raw_ >= 0 ? "STUTTER " : (repeat_held_ ? "REPEAT " : "ROLL "));
        append(out, repeat_length_name(repeat_length_));
        *out = 0;
        notice(notice_buffer_);
    }

    bool edit_held_step(int pad) {
        if (PerformPad(pad) == PerformPad::throw_fx) return throw_lock_held_step(pad);
        StepCycle cycle;
        switch (PerformPad(pad)) {
            case PerformPad::fill:   cycle = StepCycle::fill; break;
            case PerformPad::repeat: cycle = StepCycle::ratchet; break;
            case PerformPad::mute:   cycle = StepCycle::a_b; break;
            case PerformPad::mutate: cycle = StepCycle::probability; break;
            default:                 return false;
        }
        const StepField field = step_cycle_field(cycle);
        Step shown = engine_.pattern().tracks[selected_].steps[held_step_];
        const int delta = step_cycle_next(shown, cycle) - step_field_value(shown, field);
        step_field_apply(shown, field, delta);
        held_used_ = true;
        step_edit_pads_ = uint8_t(step_edit_pads_ | (1u << (pad / 2)));
        mark(GestureKind::field, selected_, held_step_);
        post_delta(Cmd::step_field_delta, selected_, held_step_, uint8_t(field), delta);

        char value[kStepFieldTextMax];
        if (field == StepField::ratchet_count && !ratchet_spans(shown.ratchet) &&
            ratchet_count(shown.ratchet) == 1) {
            value[0] = 'O'; value[1] = 'F'; value[2] = 'F'; value[3] = 0;
        } else {
            step_field_text(shown, field, value, int(sizeof value));
        }
        char *out = notice_buffer_;
        append(out, "STEP ");
        append_number(out, uint32_t(held_step_) + 1);
        *out++ = ' ';
        append(out, step_field_name(field));
        *out++ = ' ';
        append(out, value);
        *out = 0;
        notice(notice_buffer_);
        return true;
    }

    // Hold STEP + THROW: the throw written into the step, as a lock on the selected track's
    // delay send (FUNC: reverb send) at full; again takes the lock off. The cycle is "full,
    // off", as the concept's table has it; a lock at some other level (turned on the FX page)
    // goes to full first. Unlike the held throw it is part of the pattern: saved and undone.
    bool throw_lock_held_step(int pad) {
        const int param = page_first(Page::fx) + (shift_ ? 3 : 2);
        uint8_t value = 0;
        const bool full = pattern_get_lock(engine_.pattern(), selected_, held_step_, param, value) &&
                          value == 127u;
        held_used_ = true;
        step_edit_pads_ = uint8_t(step_edit_pads_ | (1u << (pad / 2)));
        if (full) {
            mark(GestureKind::erase, selected_, held_step_);
            post(Cmd::clear_lock, uint8_t(selected_), uint8_t(held_step_), uint8_t(param));
        } else {
            mark(GestureKind::field, selected_, held_step_);
            // +127 from any base lands on 127 (the audio side clamps); post_delta would cap
            // the delta at 120 and leave a lock from 0 at 120.
            post(Cmd::lock_delta, uint8_t(selected_), uint8_t(held_step_), uint8_t(param),
                 uint8_t(127));
        }
        end_gesture();
        char *out = notice_buffer_;
        append(out, "STEP ");
        append_number(out, uint32_t(held_step_) + 1);
        append(out, shift_ ? " REV " : " DLY ");
        append(out, full ? "OFF" : "FULL");
        *out = 0;
        notice(notice_buffer_);
        return true;
    }

    void on_perform_pad(int pad, bool down) {
        // Which of the row is held, for its LED. Every pad, toggles too: a held pad is lit
        // while the finger is on it, which is the first sign the press arrived at all.
        const uint8_t bit = uint8_t(1u << (pad / 2));
        perform_held_ = uint8_t(down ? (perform_held_ | bit) : (perform_held_ & ~bit));
        switch (PerformPad(pad)) {
            case PerformPad::fill:
                // FUNC + FILL turns NOTE RPT on and off; the fill itself is untouched. A FILL
                // released with FUNC still down after an ordinary press still lets the fill go.
                if (down && shift_) {
                    note_repeat_mode_ = !note_repeat_mode_;
                    notice(note_repeat_mode_ ? "NOTE RPT" : "PADS PLAY");
                    break;
                }
                fill_held_ = down;
                post(Cmd::set_fill, 0, 0, down ? 1 : 0);
                if (down) notice("FILL");
                break;

            case PerformPad::repeat:
                // VALUE picks the length while it is held (docs/SPEC.md), which on_encoder
                // handles; F1 is the pitch-down.
                repeat_held_ = down;
                repeat_audio_ = down;
                if (down) repeat_pitch_ = shift_;
                post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), down ? 1 : 0,
                     shift_ ? 1 : 0);
                // A stutter is REPEAT's gesture: it ends with the pad, held STEP or not. The
                // STEP keys' releases are still swallowed (stutter_keys_).
                if (!down && stutter_raw_ >= 0) {
                    stutter_raw_ = -1;
                    post(Cmd::step_stutter, 0, 0, 0, 0, 0);
                }
                if (down) {
                    char *out = notice_buffer_;
                    append(out, "REPEAT ");
                    append(out, repeat_length_name(repeat_length_));
                    *out = 0;
                    notice(notice_buffer_);
                }
                break;

            case PerformPad::mute:
                if (down) {
                    mute_mode_ = !mute_mode_;
                    notice(mute_mode_ ? "MUTE MODE" : "PADS PLAY");
                    // MUTE mode takes the lower STEP keys, and its branch in `on_key` returns on
                    // the release as well as the press -- so a STEP key held when this pad is
                    // tapped never delivers its release, and the hold would outlive its key.
                    // The next encoder turn would then write a parameter lock onto a step nobody
                    // was touching. Same reasoning as `leave_mode`, and the third of three ways
                    // this could happen: the other two were a page change and a mode change.
                    drop_hold();
                }
                break;

            case PerformPad::scene_a:
            case PerformPad::scene_b:
                // Held + STEP picks the scene; on its own it just shows which is assigned.
                // Deliberately does NOT change the mode. The performance row works in every
                // mode, and a hold that switched the screen and did not switch it back would be
                // a surprise in the middle of a take.
                scene_holding_ = down ? (PerformPad(pad) == PerformPad::scene_a ? 0 : 1) : -1;
                // And a scene hold takes all sixteen STEP keys, with the same swallowed release.
                if (down) {
                    drop_hold();
                    notice(scene_holding_ == 0 ? "SCENE A: STEP PICKS" : "SCENE B: STEP PICKS");
                }
                break;

            case PerformPad::throw_fx:
                // Delay, or reverb with F1. Released, it goes back to the stored send by itself
                // -- the engine never wrote over `params_`.
                post(Cmd::set_throw, uint8_t(selected_), 0,
                     down ? uint8_t(shift_ ? 2 : 1) : uint8_t(0));
                throw_held_ = down;
                if (down) notice(shift_ ? "THROW REVERB" : "THROW DELAY");
                break;

            case PerformPad::mutate:
                // FUNC + MUTATE is LOCK (core/lockstore.hpp, backlog D3 D4 D13 D14): held for
                // kLockHoldMs it stores a lock for the playing pattern (update), tapped it jumps
                // to it or swaps back. The release decides, whether FUNC is still down or not:
                // a chord let go in either order is still the chord. Until 2026-11-25 FUNC +
                // MUTATE was the held delay freeze, which is the mixer's slot 5 now.
                if (down && shift_) {
                    lock_press_ = true;
                    lock_press_stored_ = false;
                    lock_press_ms_ = last_ms_;
                    break;
                }
                if (!down && lock_press_) {
                    lock_press_ = false;
                    if (!lock_press_stored_) lock_tap();
                    break;
                }
                if (!down) break;
                // One undo level, then the mutation. CLR reverts it because CLR is the undo that
                // already exists -- core/mutate.hpp's whole argument for having no commit step.
                mark(GestureKind::paste, selected_, 0);
                post_mutate();
                end_gesture();
                {
                    // Say what happened, and to which track: a mutation of an empty track, or a
                    // small one, can be hard to hear, and the pad's print (TRK 7) says nothing.
                    char *out = notice_buffer_;
                    append(out, "MUTATE T");
                    append_number(out, uint32_t(selected_) + 1);
                    *out++ = ' ';
                    append(out, mutation_name(mutation_));
                    *out = 0;
                    notice(notice_buffer_);
                }
                // docs/SPEC.md: "OK keeps, CLR reverts". The mutation is already one undo level,
                // so keeping is just forgetting it is pending and reverting is that undo.
                mutation_pending_ = true;
                break;

            default:
                break;
        }
    }

    // Whether a performance pad is lit bright: held, or its mode on.
    bool perform_pad_active(PerformPad pad) const {
        switch (pad) {
            case PerformPad::fill:     return fill_held_ || note_repeat_mode_;
            case PerformPad::repeat:   return repeat_held_;
            case PerformPad::mute:     return mute_mode_;
            case PerformPad::scene_a:  return scene_holding_ == 0;
            case PerformPad::scene_b:  return scene_holding_ == 1;
            case PerformPad::throw_fx: return throw_held_;
            case PerformPad::mutate:   return mutation_pending_;
            default:                   return false;
        }
    }

    void post_mutate() {
        Command command;
        command.cmd = Cmd::mutate;
        command.track = uint8_t(selected_);
        command.step = 0;
        command.a = uint8_t(mutation_);
        command.b = uint8_t(mutate_amount_);
        command.c = 0;                       // the whole track
        command.u = mutate_variant_++;       // a new result each press
        post_command(command);
        // "MUTATE T1 VELOCITY": what was done and to which track. The bare type name alone
        // ("VELOCITY") did not read as anything having happened, and a velocity mutation is
        // invisible on the grid. At most 21 characters: "MUTATE T8 " is 10, the longest
        // name ("ROT RIGHT", "DENSITY +") 9.
        char *out = notice_buffer_;
        append(out, "MUTATE T");
        append_number(out, uint32_t(selected_ + 1));
        *out++ = ' ';
        append(out, mutation_name(mutation_));
        *out = 0;
        notice(notice_buffer_);
        mutate_track_ = selected_;
    }

    // Where OK and CLR are the verdict on a pending MUTATE (on_key): not PROJECT, KIT, SAMPLES,
    // RESAMPLE or the FUNC menu, whose own OK and CLR come first (in the menu OK is SAVE), and
    // not while a STEP, a range or a scene pad is held. The screen only offers the verdict where the keys will give it.
    bool verdict_keys_free() const {
        return mutation_pending_ && held_step_ < 0 && range_first_ < 0 && scene_holding_ < 0 &&
               mode_ != Mode::project && mode_ != Mode::kit && mode_ != Mode::samples &&
               mode_ != Mode::resample && mode_ != Mode::menu && mode_ != Mode::sounds;
    }

    // Show a live value as the notice (see Echo). GRID draws the track's length and speed on
    // its own line, so a layout echo is only needed elsewhere -- but it is harmless there.
    void echo(Echo kind, int param = 0) {
        echo_ = kind;
        echo_param_ = param;
        refresh_echo();
        notice(echo_buffer_);
    }

    void refresh_echo() {
        char *out = echo_buffer_;
        if (echo_ == Echo::layout) {
            // "T1 LEN 16 X1/2": the polymeter pair, as GRID's line spells them.
            const TrackPattern &track = engine_.pattern().tracks[selected_];
            append(out, "T");
            append_number(out, uint32_t(selected_ + 1));
            append(out, " LEN ");
            append_number(out, track.length);
            append(out, " X");
            append(out, speed_name(Speed(track.speed)));
            // And the play mode, only when it is not FWD: "T1 LEN 64 X3/4 RND16" is 20 wide.
            if (track.play != uint8_t(PlayMode::fwd) && track.play < uint8_t(PlayMode::count)) {
                *out++ = ' ';
                append(out, play_mode_name(PlayMode(track.play)));
            }
        } else if (echo_ == Echo::param) {
            // "T1 CUT 064": the knob's name and where it is now.
            append(out, "T");
            append_number(out, uint32_t(selected_ + 1));
            *out++ = ' ';
            append(out, param_name_for(echo_param_));
            *out++ = ' ';
            append_param_value(out, echo_param_, engine_.params(selected_).v[echo_param_]);
        }
        *out = 0;
    }
