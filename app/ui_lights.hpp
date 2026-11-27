// MIDI in, live recording and the LEDs.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- MIDI in -----------------------------------------------------------
    //
    // MIDI notes take exactly the same path as pad hits, which is the point: `on_midi` is main
    // context and carries no timestamp of its own, so it has nothing better to be stamped with
    // than the clock state read the moment it arrives -- the same thing a pad hit gets
    // (docs/archive/REVIEW.md). One mechanism, one set of bugs.
    //
    // The channel picks the track, 1-8 for tracks 1-8, anything else playing the selected one.
    // docs/SPEC.md does not map this; MIDI channel is the obvious choice and a note-for-note
    // omni mapping would make an 8-track box unplayable from a keyboard.
    //
    // What arrives (docs/SPEC.md, "MIDI in"):
    //   - note on: plays the track and records like a pad, HELD until its note off;
    //   - note off (and a note on at velocity 0, which Enaga delivers as one): lets go of it on
    //     the track it STARTED on, which matters for channels 9-16 when the selected track has
    //     moved in between;
    //   - CC 16-31: the track's sixteen parameters in page order (SRC, FLTR, AMP, FX), set to
    //     the value, as a knob would; CC 120 and 123 let go of every note this MIDI IN holds on
    //     that track (Enaga sends 123 itself when active sensing stops);
    //   - program change on the user's MIDI CH: queues pattern 1-128 (A01-H16) in the queue
    //     mode the PROJECT page has set, as pressing it there would.
    void on_midi(const enaga::MidiMessage &message) override {
        const int channel = message.channel();
        const int track = channel < kTracks ? channel : selected_;
        switch (message.type()) {
            case enaga::MidiMessage::note_on: {
                if (message.velocity() == 0) {             // the running-status note off
                    midi_note_off(channel, message.note());
                    break;
                }
                const uint8_t note = uint8_t(message.note() & 0x7F);
                const uint8_t velocity = uint8_t(message.velocity());
                // A second note on for a note still held here: let the first go, so its off
                // cannot later end this one on another track.
                midi_note_off(channel, note);
                midi_in_held_[channel][note] = uint8_t(track + 1);
                post(Cmd::trigger_note, uint8_t(track), 0, note, velocity, 1);
                record_note(track, note, velocity);
                break;
            }
            case enaga::MidiMessage::note_off:
                midi_note_off(channel, message.note());
                break;
            case enaga::MidiMessage::control_change: {
                const int cc = message.controller();
                if (cc >= kMidiInFirstParamCc && cc < kMidiInFirstParamCc + kParams) {
                    post(Cmd::set_param, uint8_t(track), 0, uint8_t(cc - kMidiInFirstParamCc),
                         uint8_t(message.value() & 0x7F));
                } else if (cc == 120 || cc == 123) {
                    for (int note = 0; note < 128; note++) {
                        if (midi_in_held_[channel][note] != 0) midi_note_off(channel, note);
                    }
                }
                break;
            }
            case enaga::MidiMessage::program_change:
                if (channel != enaga::midi::channel()) break;
                shown_bank_ = message.program() / kPatternsPerBank;
                queue_shown_pattern(message.program() % kPatternsPerBank);
                break;
            default:
                break;
        }
    }

    // A note off from MIDI IN: the track the note started on lets go of it.
    void midi_note_off(int channel, int note) {
        if (channel < 0 || channel >= 16 || note < 0 || note > 127) return;
        const uint8_t held = midi_in_held_[channel][note];
        if (held == 0) return;                       // never started here, or already let go
        midi_in_held_[channel][note] = 0;
        post(Cmd::release_note, uint8_t(held - 1), 0, uint8_t(note));
        record_note_up(held - 1, uint8_t(note));
    }

    // --- live recording ---------------------------------------------------

    // Stamp a hit, undo the pad latency, quantize it, and post the step it belongs on.
    //
    // The stamp is `clock::state()` read here, in the main context, at the moment the event
    // arrived: whole ticks plus the fraction into the next. core/record.hpp explains why that is
    // a better timestamp than a sample counter would be. Everything after that is arithmetic in
    // core/, where it is host-tested -- this function only gathers the inputs.
    // `held`: the note has a key-up still to come (a piano pad, a STEP key in KEYS, a MIDI note),
    // so its length is measured. A performance pad hit has none and keeps the step's LEN.
    // `roll`: a ratchet byte for the step, a held NOTE RPT pad's roll (0 for a plain hit). Its
    // pad's release gives the roll its LEN, on any machine.
    void record_note(int track, uint8_t note, uint8_t velocity, bool held = true,
                     uint8_t roll = 0) {
        if (!rec_) return;
        const enaga::clock::State clock = enaga::clock::state();
        if (!clock.running) return;          // nothing to record onto
        // A count-in is the player's lead-in, not the pattern: nothing records until it ends.
        const Status &status = engine_.status();
        const uint32_t lead = status.count_in_ticks.load(std::memory_order_relaxed);
        if (clock.position < lead) return;

        // The pattern's position, as the scheduler's: the clock less a count-in, less where the
        // pattern started (a chain entry, a queued switch), which is what steps and passes count
        // from (Engine::resolve_tick hands the scheduler `tick - origin_`).
        const uint32_t origin = status.pattern_origin.load(std::memory_order_relaxed);
        if (clock.position - lead < origin) return;   // a switch the audio side has not made yet
        Stamp now;
        now.tick = clock.position - lead - origin;
        now.fraction = clock.fraction;
        const Stamp hit = stamp_back(now, kPadLatencyMs, clock.tempo);

        // The track's timing has to be resolved exactly as core/scheduler does it, including
        // swing inheritance -- if the two disagree about where a step sits, a hit played dead on
        // the beat records off it.
        const Pattern &pattern = engine_.pattern();
        const TrackPattern &source = pattern.tracks[track];
        TrackTiming timing;
        timing.length = source.length;
        timing.speed = Speed(source.speed);
        timing.swing = source.swing == kSwingInherit ? pattern.swing : source.swing;
        // LIVE RND too (core/liverand.hpp): a hit goes to the step that was sounding.
        timing.play = live_play_mode(source.play, engine_.live_rnd_unit(),
                                     engine_.live_rnd_armed(), track);
        timing.play_seed = play_seed(pattern.seed, track);

        // quantize_hit finds the time SLOT the hit belongs to; under REV, PING or RND the note
        // goes to the step that plays in that slot on that pass, so it plays back where it was
        // played. Its micro is relative to the slot, which is where that step sounds then.
        Landing landing = quantize_hit(hit, timing, quantize());
        landing.step = play_step(int(landing.step), landing.loop, timing);

        mark(GestureKind::record, track, 0);     // step 0: one undo level for the whole pass
        Command command;
        command.cmd = Cmd::record_hit;
        command.track = uint8_t(track);
        command.step = landing.step;
        command.a = note;
        command.b = velocity;
        command.c = uint8_t(landing.micro);
        command.e = roll;
        // On a chord track several notes can share a step (core/stepnotes.hpp). The first on a
        // step this pass replaces what it held, unless REC MODE is ADD; any after it join.
        command.d = uint8_t(RecWrite::note);
        if (records_chords(track)) {
            uint32_t &pass = rec_pass_[track][landing.step];
            const bool again = pass == landing.loop + 1u;
            command.d = uint8_t(again || rec_add() ? RecWrite::join : RecWrite::replace);
            pass = landing.loop + 1u;
        }
        post_command(command);

        if (landing.clamped) notice("OFF GRID");   // it could not be stored where it was played

        // And remember the press, so the key coming up can give the step its length. Only on a
        // machine that holds a note -- a drum ignores LEN, and a tap there would rewrite the
        // step's LEN for nothing. The stamp is `now`, not `hit`: the pad latency is the same at
        // both ends, so the hold is the difference of the raw stamps.
        if (!held || (roll == 0 && engine_.machine_id(track) < uint8_t(MachineId::sample))) return;
        RecHold *slot = nullptr;
        for (RecHold &h : rec_holds_) {
            if (h.track == track && h.note == note) slot = &h;   // the same note again: replace
        }
        for (RecHold &h : rec_holds_) {
            if (slot == nullptr && h.track < 0) slot = &h;
        }
        if (slot == nullptr) {
            // More held notes than slots: the oldest press gives up its length (it keeps LEN).
            slot = &rec_holds_[rec_hold_next_];
            rec_hold_next_ = rec_hold_next_ + 1 >= kRecHolds ? 0 : rec_hold_next_ + 1;
        }
        slot->track = int8_t(track);
        slot->note = note;
        slot->step = landing.step;
        // On the clock less the count-in, as `up` is: a chain entry or a queued switch that
        // moves the pattern's origin while the key is held must not shorten the hold.
        slot->down = now;
        slot->down.tick = now.tick + origin;
    }

    // The machines whose steps play several notes: the POLY three, and MID, which sends a
    // chord to the synth it drives. Only these gather a chord played into one step.
    bool records_chords(int track) const {
        const uint8_t id = engine_.machine_id(track);
        return id == uint8_t(MachineId::va_poly) || id == uint8_t(MachineId::wave_poly) ||
               id == uint8_t(MachineId::fm) || id == uint8_t(MachineId::midi);
    }

    // A recorded note's key came up: its hold, quantized to the note-length table
    // (core/record.hpp, held_length_index), becomes the step's LEN. Nothing if it was not
    // recorded, if REC went off in between or the clock stopped -- there is nothing to measure.
    void record_note_up(int track, uint8_t note) {
        RecHold *hold = nullptr;
        for (RecHold &h : rec_holds_) {
            if (h.track == track && h.note == note) hold = &h;
        }
        if (hold == nullptr) return;
        hold->track = -1;
        if (!rec_) return;
        const enaga::clock::State clock = enaga::clock::state();
        if (!clock.running) return;
        const uint32_t lead = engine_.status().count_in_ticks.load(std::memory_order_relaxed);
        Stamp up;
        up.tick = clock.position > lead ? clock.position - lead : 0u;
        up.fraction = clock.fraction;

        const TrackPattern &source = engine_.pattern().tracks[track];
        TrackTiming timing;
        timing.length = source.length;
        timing.speed = Speed(source.speed);
        Command command;
        command.cmd = Cmd::record_length;
        command.track = uint8_t(track);
        command.step = hold->step;
        command.a = note;
        command.b = held_length_index(hold->down, up, timing);
        post_command(command);
    }

    void on_transport(int what, uint32_t position) override {
        // Under an external clock STRATA cannot start or stop anything; it follows. Enaga sends
        // this for every transport change whoever made it (docs/ENAGA_API.md), so a STOP from
        // MIDI or SYNC is followed exactly as F6 is: voices released and the sequencer reset,
        // which is what keeps the conditions and the playheads in step with the outside
        // transport. A STOP that F6 made arrives here too; releasing twice is harmless. The next
        // F6 press is then the one that cuts the tails.
        (void)position;
        if (what == ENAGA_TRANSPORT_START || what == ENAGA_TRANSPORT_LOCATE) {
            stop_armed_ = false;
            clear_rec_passes();          // the pass count starts again: old marks would match
        }
        if (what == ENAGA_TRANSPORT_STOP) {
            post(Cmd::stop, 0, 0, 0);
            stop_armed_ = true;
        }

        // docs/SPEC.md: "Autosave to AUTOSAVE/ whenever the transport stops." It follows the
        // transport rather than the key, so a stop arriving from MIDI or SYNC autosaves too --
        // which is the case where the user never pressed anything and would least expect to
        // lose work.
        if (what == ENAGA_TRANSPORT_STOP) storage_.request_autosave();
    }

    // FUNC + LEN- / LEN+: one step of the selected track's length, as a delta the audio side
    // applies (app/commands.hpp, kDeltaFlag).
    void step_track_length(int delta) {
        mark(GestureKind::length, selected_, 0);
        post(Cmd::set_track_length, uint8_t(selected_), 0, 0, clamp_delta(delta), kDeltaFlag);
        end_gesture();
        // Only GRID drew the length, so in EDIT or MACHINE the press changed the pattern and
        // nothing on screen.
        echo(Echo::layout);
    }

    // --- LEDs -------------------------------------------------------------

    void update(uint32_t ms) override {
        last_ms_ = ms;

        service_slot_menu();
        service_track_menu();

        // EVOLVE changed a track on a pass boundary (Engine::evolve_passes): no command did it,
        // so the bank is marked here, and the log says so for the scenarios.
        {
            const uint8_t turns = engine_.status().evolve_turns.load(std::memory_order_acquire);
            if (turns != evolve_turns_seen_) {
                evolve_turns_seen_ = turns;
                storage_.note_command(
                    Cmd::evolve_home,
                    int(engine_.status().live_bank.load(std::memory_order_relaxed)));
                enaga::log("EVOLVE TURN");
            }
            // A load set EVOLVE from the file: the screens' copies follow (Engine::
            // evolve_from_settings). The mutation and amount are PERFORM's MUTATE setting too.
            const uint8_t loaded = engine_.status().evolve_loaded.load(std::memory_order_acquire);
            if (loaded != evolve_loaded_seen_) {
                evolve_loaded_seen_ = loaded;
                mutation_ = engine_.evolve_kind();
                mutate_amount_ = engine_.evolve_amount();
                evolve_mode_ui_ = engine_.evolve_mode();
            }
        }

        // A count-in says its beats as it clicks them (`COUNT-IN 4` .. `1`), logged for
        // tests/scenario_click.sh.
        const uint8_t counting = engine_.status().count_in_beats.load(std::memory_order_relaxed);
        if (counting != count_in_shown_) {
            count_in_shown_ = counting;
            if (counting != 0) {
                char *out = notice_buffer_;
                append(out, "COUNT-IN ");
                append_number(out, uint32_t(counting));
                *out = 0;
                notice_logged(notice_buffer_);
            }
        }

        // A LEN key held with FUNC repeats. Late passes catch up one step at a time, never in
        // a burst: a 900 ms stall in a save is one step, not eighteen.
        if (len_repeat_key_ >= 0 && int32_t(ms - len_repeat_due_ms_) >= 0) {
            step_track_length(len_repeat_key_ == ENAGA_KEY_OCTAVE_LEFT ? -1 : +1);
            len_repeat_due_ms_ = ms + kLenRepeatMs;
        }

        // The mutes, read back from the engine. The UI keeps a shadow so a press shows at once,
        // but a recall or a mute that landed on the bar happened on the audio side, and without
        // this the shadow would drift from what is actually muted.
        for (int t = 0; t < kTracks; t++) muted_[t] = engine_.muted(t);

        // The WAVE tables the tracks sound, built on demand a few small pieces a pass
        // (Engine::step_waves). Pieces only while the pass is under kWaveSliceMs old, so the
        // loop comes back to poll the keys (every 6 ms; the emulator holds a press 54 ms) long
        // before a press can come and go unseen. `wav <TBL>` for each table built, for the
        // scenarios and a unit log.
        {
            const uint32_t start = enaga::ms();
            while (engine_.step_waves() && enaga::ms() - start < kWaveSliceMs) {
            }
            const int built = engine_.waves().builds();
            if (built != waves_logged_) {
                waves_logged_ = built;
                char line[16];
                char *out = line;
                append(out, "wav ");
                append(out, WaveLibrary::short_name(engine_.waves().last_built()));
                *out = 0;
                enaga::log(line);
            }
        }

        // One bounded slice of whatever the card side has in flight. It never blocks and never
        // waits on the audio side: a command ring that has not drained simply means "not this
        // pass" (app/storage.hpp).
        storage_.service();

        // The sample browser's audition (push A in SAMPLES): only while SAMPLES is up, whatever
        // took the screen away; why one did not play, said once; and `aud play` / `aud end` in
        // the log, for the emulator scenario (tests/scenario_audition.sh) and a unit log.
        if (mode_ != Mode::samples && storage_.auditioning()) storage_.stop_audition();
        if (!storage_.auditioning()) audition_row_ = -1;
        if (const char *why = storage_.take_audition_error()) {
            notice(why);
            char line[24];
            char *out = line;
            append(out, "aud ");
            append_shown(out, why, 16);
            *out = 0;
            enaga::log(line);
        }
        if (storage_.audition_sounding() != audition_sounding_seen_) {
            audition_sounding_seen_ = storage_.audition_sounding();
            enaga::log(audition_sounding_seen_ ? "aud play" : "aud end");
        }

        // A take asked for TO FILE is whole on the card: offer it to a track (bounce to track).
        if (storage_.file_takes() != file_takes_seen_) {
            file_takes_seen_ = storage_.file_takes();
            take_offer_ = offer_track(take_from_track_);
            char line[16];
            char *out = line;
            append(out, "offer T");
            append_number(out, uint32_t(take_offer_) + 1);
            *out = 0;
            enaga::log(line);
            if (mode_ == Mode::resample) notice("TAKE ON CARD");
        }

        // The PROJECTS page's list, once the card has answered: the cursor onto the open
        // project, and a fresh read after a SAVE AS has landed.
        if (mode_ == Mode::project && project_page_ == ProjectPage::files) settle_project_cursor();

        // Freeze (dsp/frozen.hpp): a track that has just frozen or come back, on the screen and
        // as `frz <mask>` for the emulator scenario and a unit log; and a freeze the pool had no
        // room for, said once.
        {
            const uint8_t mask = engine_.status().frozen.load(std::memory_order_relaxed);
            if (mask != frozen_seen_) {
                const uint8_t gained = uint8_t(mask & ~frozen_seen_);
                frozen_seen_ = mask;
                char line[16];
                char *out = line;
                append(out, "frz ");
                append_number(out, uint32_t(mask));
                *out = 0;
                enaga::log(line);
                if (gained != 0) notice("FROZEN");
            }
            const char *why = storage_.freeze_error();
            if (why != freeze_error_seen_) {
                freeze_error_seen_ = why;
                if (why != nullptr) notice(why);
            }
        }

        // A streamed file the card could not keep up with (app/storage.hpp, "streaming"): the
        // audio side played silence where frames were missing. Said once, then not again for a
        // couple of seconds, so a slow card is visible without the screen saying nothing else.
        {
            const uint32_t late = engine_.status().stream_late.load(std::memory_order_relaxed);
            if (late != stream_late_seen_) {
                stream_late_seen_ = late;
                if (uint32_t(ms - stream_late_said_ms_) > 2000u) {
                    stream_late_said_ms_ = ms;
                    notice("STREAM LATE");
                }
            }
        }

        // LOCK: a FUNC + MUTATE held long enough stores, once per press; and what the audio
        // side did with a store or a tap, said once it has done it.
        if (lock_press_ && !lock_press_stored_ && uint32_t(ms - lock_press_ms_) >= kLockHoldMs) {
            lock_press_stored_ = true;
            post(Cmd::lock_store);
        }
        service_lock_event();

        // FX GLIDE's rides, and a STEP held with VALUE pushed long enough to push its effect in.
        service_perf(ms);

        // A MUTATE that changed nothing (Engine, Cmd::mutate): say so, and there is no verdict
        // to wait for. Before this the screen named the mutation anyway, and an empty track or
        // a rotation of identical steps was a pad press that did nothing at all.
        {
            const uint8_t none = engine_.status().mutate_unchanged.load(std::memory_order_acquire);
            if (none != mutate_unchanged_seen_) {
                mutate_unchanged_seen_ = none;
                mutation_pending_ = false;
                char *out = notice_buffer_;
                append(out, "T");
                append_number(out, uint32_t(mutate_track_ + 1));
                append(out, " NOTHING TO MUTATE");
                *out = 0;
                notice(notice_buffer_);
            }
        }

        // Song mode: the chain played to its end (Engine::cross_boundary). Only this context may
        // stop the clock. Back to 0 as well, so PLAY starts the song from the top -- the engine
        // is already on entry 1. Under an outside clock that keeps running the stop is refused;
        // the engine stays silent until PLAY or the outside STOP.
        {
            const uint8_t ends = engine_.status().song_end.load(std::memory_order_acquire);
            if (ends != song_end_seen_) {
                song_end_seen_ = ends;
                clock_refused_ = !enaga::clock::stop();
                if (!clock_refused_ || !enaga::clock::running()) enaga::clock::locate(0);
                notice("SONG END");
            }
        }

        // The tempo a LOADED project brought with it. PROJECT.BIN has always had the field and
        // nothing read it, so every project opened at whatever the clock happened to be set to
        // -- a project saved at 142 came back at 130. Watched here rather than hooked to the end
        // of a load because a load finishes on the card side, and this is one comparison on a
        // pass that already does several.
        //
        // `adopted_tempo_centi_` is what stops this fighting the knob: a click posts the new
        // tempo AND records it here, so the engine's copy coming back is not read as a change.
        {
            const uint16_t project_centi = engine_.project().settings.tempo_centi_bpm;
            if (project_centi != adopted_tempo_centi_) {
                adopted_tempo_centi_ = project_centi;
                tempo_tenths_ = uint32_t(project_centi) / 10;
                // Refused under an outside clock, which is not an error: the project's tempo is
                // simply not what is playing, and the screen already says EXT.
                clock_refused_ = !enaga::clock::set_tempo(float(project_centi) * 0.01f);
            }
        }

        // Log transport and clock-source changes. Cheap (a handful of comparisons), only fires on
        // a change, and it is the one thing worth seeing in an emulator run or a unit log: it
        // says whether the clock is actually moving and who is driving it.
        {
            const enaga::clock::State clock = enaga::clock::state();
            const bool running = clock.running != 0;
            const int source = int(clock.source);
            if (running != logged_running_ || source != logged_source_) {
                // A start begins a fresh MAX, so the figure on screen belongs to what is playing
                // now and not to a load or an earlier pattern. Here rather than in on_transport
                // because this edge is seen whoever started the clock.
                if (running && !logged_running_) cpu_max_ = 0;
                logged_running_ = running;
                logged_source_ = source;
                enaga::log(running ? (source == ENAGA_CLOCK_INTERNAL ? "run int"
                                                                     : "run ext")
                                   : "stopped");
            }
            (void)logged_position_;
        }

        // The held CPU maximum for the bottom line. The top line's figure is the LAST block, which
        // changes 750 times a second and cannot be read on the unit. update() runs after every
        // block, so polling here sees each block's cost. Enaga's own audio_load_peak() is held
        // since the app started, which includes init and every load; this one restarts on PLAY.
        {
            const uint32_t load = enaga::audio_load();
            cpu_max_ = load > cpu_max_ ? load : cpu_max_;

            // Windowed figures for scenarios and unit logs: `ld <avg> <max>` in percent, one
            // line per BAR. One block is too noisy to compare (which drum hits land in it moves it
            // by points), and the emulator's report holds only the peak since boot
            // (tests/scenario_freeze.sh). Enaga's log ring keeps sixteen lines and most scenarios
            // check lines from early in the run, so the bars come only in one burst of kLoadBurst
            // after a track freezes or comes back, while the transport runs.
            //
            // Bars, not half seconds, so a frozen run and a live one compare the same music: four
            // bars are one pass of a four-bar pattern (A05 changes chord every bar, and its
            // chords hold different numbers of voices). The part bar the change lands in and one
            // whole bar after it are skipped: an unfrozen machine starts silent and comes back on
            // the next trigger, without the release tails a live chord track carries over.
            const uint8_t frozen = engine_.status().frozen.load(std::memory_order_relaxed);
            const uint32_t bar = engine_.status().tick.load(std::memory_order_relaxed) / kTicksPerBar;
            if (frozen != load_frozen_) {
                load_frozen_ = frozen;
                load_burst_ = kLoadBurst;
                load_skip_ = 2;
                load_bar_ = bar;
            }
            if (!logged_running_) load_burst_ = 0;
            if (load_burst_ != 0 && bar != load_bar_) {
                load_bar_ = bar;
                if (load_skip_ != 0) {
                    load_skip_--;
                } else if (window_polls_ != 0) {
                    char line[24];
                    char *out = line;
                    append(out, "ld ");
                    append_tenths(out, (window_sum_ + window_polls_ / 2) / window_polls_);
                    append(out, " ");
                    append_tenths(out, window_max_);
                    *out = '\0';
                    enaga::log(line);
                    load_burst_--;
                }
                window_sum_ = window_max_ = window_polls_ = 0;
            }
            if (load_burst_ != 0 && load_skip_ == 0) {
                window_sum_ += load;
                window_max_ = load > window_max_ ? load : window_max_;
                window_polls_++;
            } else {
                window_sum_ = window_max_ = window_polls_ = 0;
            }
        }

        // The CPU guard's level, logged when it moves: on the unit and in a scenario this is how
        // a run says the guard rose (core/cpuguard.hpp). It moves at most ten times a second, so
        // the sixteen-line log ring is not at risk.
        {
            const uint8_t guard = engine_.status().cpu_guard.load(std::memory_order_relaxed);
            if (guard != logged_guard_) {
                logged_guard_ = guard;
                char text[8] = "guard 0";
                text[6] = char('0' + (guard > 9 ? 9 : guard));
                enaga::log(text);
            }
        }

        const Status &status = engine_.status();
        const Pattern &pattern = engine_.pattern();
        const uint8_t playhead = status.playhead[selected_].load(std::memory_order_relaxed);

        // A compact signature of what is being edited, logged only when it changes.
        //
        // This is the only way a scripted emulator run can see that an edit actually landed.
        // There is no way to read pattern memory from outside the app, and the screen comes back
        // as a bitmap; without this, a scenario can prove the app did not crash and nothing
        // more. With it, "toggle a step, undo, and check the grid came back" is testable without
        // a unit -- which matters because app/ is the one part of this project that has no host
        // tests at all.
        //
        // Enaga's log ring holds sixteen lines, so this has to stay sparse: one line per edit,
        // never one per block. Logging only on a change is what makes that true.
        {
            uint32_t mask = 0;
            for (int i = 0; i < 16; i++) {
                const Step &s = pattern.tracks[selected_].steps[i + page_ * 16];
                if (s.flags & (kStepTrig | kStepTrigless)) mask |= 1u << i;
            }
            const uint8_t depth = status.undo_depth.load(std::memory_order_relaxed);
            if (mask != logged_mask_ || depth != logged_undo_) {
                logged_mask_ = mask;
                logged_undo_ = depth;
                char line[24];
                char *out = line;
                append(out, "g");
                append_hex4(out, mask);
                append(out, " u");
                append_number(out, depth);
                *out = 0;
                enaga::log(line);
            }
        }

        // Which SCREEN is up, logged on a change for the same reason as the two above: in an
        // emulator run or a unit log it is the difference between "the app is wedged" and "the
        // app is on a page I did not expect", and it is what lets a scenario reach a screen
        // that the emulator cannot otherwise observe: it cannot read the screen, so a page walked
        // with the selection keys is all it can see of MIXER and SETUP.
        {
            const int screen = int(mode_) * 16 + (mode_ == Mode::mixer ? mixer_bank_
                                                  : mode_ == Mode::project ? int(project_page_)
                                                  : mode_ == Mode::edit    ? param_page_
                                                                           : 0);
            if (screen != logged_screen_) {
                logged_screen_ = screen;
                char line[24];
                char *out = line;
                append(out, "scr ");
                append(out, screen_mode_name());
                if (mode_ == Mode::mixer) {
                    *out++ = ' ';
                    append(out, kMixerPages[mixer_bank_].name);
                } else if (mode_ == Mode::project && project_page_ != ProjectPage::slots) {
                    *out++ = ' ';
                    append(out, project_page_name(project_page_));
                } else if (mode_ == Mode::edit) {
                    // EDIT's page, so a scenario can reach the LFO page with the selection keys.
                    *out++ = ' ';
                    append(out, page_name(Page(param_page_)));
                }
                *out = 0;
                enaga::log(line);
            }
        }

        for (int i = 0; i < 16; i++) {
            const int step = i + page_ * 16;
            const Step &s = pattern.tracks[selected_].steps[step];
            const bool trig = (s.flags & kStepTrig) != 0;
            const bool trigless = (s.flags & kStepTrigless) != 0;
            // Parameter locks: a chord's extra notes are locks in the pool, not on the keys.
            const bool has_locks =
                s.lock_count > 0 && step_param_lock_count(pattern, selected_, step) > 0;
            const bool conditional = s.probability < 100 || condition_kind(s.condition) != Condition::NONE;

            uint8_t green = 0, red = 0;
            if (trig) {
                // The green is the step's velocity in four bands, so the groove's accents read
                // on the keys (Roland, 2026-11-23). A condition or probability blinks it, about
                // 2.5 times a second, instead of the dim green it used to be -- dim now means soft.
                // An accented step is an accent whatever its velocity says.
                green = (s.flags & kStepAccent) != 0 ? 10 : step_velocity_level(s.velocity);
                if (conditional && ((last_ms_ / kConditionBlinkMs) & 1u) != 0) green = 0;
                if (has_locks) red = 3;               // amber: a trig with locks
            } else if (trigless) {
                // Locks with no note: red alone, so it reads as "something happens here but it
                // does not hit". The spec's LED table has no entry for this; a trigless step has
                // to be distinguishable from an empty one or the feature is invisible.
                red = 4;
            }
            // A step in the selection, or the one the step page is editing, is always visible
            // even if it is empty: these are modes you can forget you are in.
            if (range_first_ >= 0 && step >= range_first_ && step <= range_last_ && !trig) green = 1;
            if (step_page_ && step == page_step_) { green = 8; red = 8; }
            if (step == playhead && status.running.load(std::memory_order_relaxed)) red = 10;
            leds_.step(i, red, green);
        }
        if (value_pushed_ && !mute_mode_ && mode_ != Mode::menu) light_perf_steps();

        for (int t = 0; t < kTracks && mode_ != Mode::keys; t++) {
            const Rgb colour = kTrackColour[t];
            const uint8_t level = muted_[t] ? 0 : (t == selected_ ? 10 : 2);
            const uint8_t scale = level == 0 ? 0 : level;
            leds_.pad(pad_for_track(t), uint8_t(colour.r * scale / 10),
                      uint8_t(colour.g * scale / 10), uint8_t(colour.b * scale / 10));
        }

        // The performance pads (the upper row, printed TRK 1-7 on the SmplTrek): always lit dim
        // in their own colour, so the row reads as tools rather than tracks, and bright while
        // the pad is held or its mode is on (docs/CONTROLS.md).
        if (mode_ == Mode::keys) light_piano_pads();
        for (int i = 0; i < kPerformPads && mode_ != Mode::keys; i++) {
            const Rgb colour = kPerformColour[i];
            const bool held = (perform_held_ >> (kPerformPadIds[i] / 2) & 1u) != 0;
            // MUTATE glows brighter while the playing pattern has a LOCK to jump to.
            const bool lit = held || perform_pad_active(PerformPad(kPerformPadIds[i]));
            const bool locked = PerformPad(kPerformPadIds[i]) == PerformPad::mutate &&
                                (lock_state() & kLockHas) != 0;
            const uint8_t level = lit ? 10 : (locked ? 5 : 2);
            leds_.pad(kPerformPadIds[i], uint8_t(colour.r * level / 10),
                      uint8_t(colour.g * level / 10), uint8_t(colour.b * level / 10));
        }

        const bool running = status.running.load(std::memory_order_relaxed) != 0;
        leds_.key(ENAGA_KEY_F5, 0, running ? 8 : 0);
        leds_.key(ENAGA_KEY_F6, running ? 0 : 6, 0);

        // The mode keys light where you are (docs/CONTROLS.md): green on the key's own screen,
        // red on its FUNC screen -- the one its second printed label names. HOME is amber in
        // KEYS (its second press) and red in the FUNC menu ("system").
        leds_.key(ENAGA_KEY_F4, mode_ == Mode::keys || mode_ == Mode::menu ? 8 : 0,
                  mode_ == Mode::grid || mode_ == Mode::keys ? 8 : 0);
        leds_.key(ENAGA_KEY_F3, mode_ == Mode::samples ? 8 : 0, mode_ == Mode::project ? 8 : 0);
        leds_.key(ENAGA_KEY_F7, mode_ == Mode::perform ? 8 : 0, mode_ == Mode::mixer ? 8 : 0);
        leds_.key(ENAGA_KEY_F8, mode_ == Mode::machine || mode_ == Mode::kit ? 8 : 0,
                  mode_ == Mode::edit ? 8 : 0);
        // REC is red while recording, and green on RESAMPLE ("sampling").
        leds_.key(ENAGA_KEY_F2, rec_ ? 8 : 0, mode_ == Mode::resample ? 8 : 0);
        // FUNC lights while it is held, so a chord shows it is being read as one.
        leds_.key(ENAGA_KEY_F1, 0, shift_ ? 8 : 0);

        // CLR and OK light when undo and redo have somewhere to go, which is the only way to
        // know from the panel that F1 + CLR will do anything at all.
        leds_.key(ENAGA_KEY_CLR, status.undo_depth.load(std::memory_order_relaxed) ? 4 : 0, 0);
        leds_.key(ENAGA_KEY_OK, 0, status.redo_depth.load(std::memory_order_relaxed) ? 4 : 0);

        leds_.primed = true;
    }
