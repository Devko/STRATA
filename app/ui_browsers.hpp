// KEYS, and the lists: SOUNDS, SAMPLES, PROJECTS and KIT.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- KEYS: the pads as a piano ---------------------------------------

    // The note a piano pad plays: C of the current octave plus its semitone, -1 for the two
    // octave pads. Chromatic, as a keyboard is; the LEDs show which notes are in the scale.
    int pad_note(int pad) const {
        if (pad < 0 || pad >= kPianoPads || kPianoSemitone[pad] < 0) return -1;
        return clamp_int(kPadOctaveNote + keys_octave_ * 12 + kPianoSemitone[pad], 0, 127);
    }

    void release_pad(int pad) {
        if (pad < 0 || pad >= kPianoPads || pad_held_note_[pad] < 0) return;
        post(Cmd::release_note, uint8_t(pad_held_track_[pad]), 0, uint8_t(pad_held_note_[pad]));
        mod_pressure_release(pad_held_track_[pad]);
        record_note_up(pad_held_track_[pad], uint8_t(pad_held_note_[pad]));
        pad_held_note_[pad] = -1;
        // The macro bend this pad's pressure made goes back to where the knob left it.
        if (pressure_pad_ == pad) {
            pressure_pad_ = -1;
            post(Cmd::set_macro, 0, 0, 0, macro_position_[0]);
        }
    }

    // A piano pad played: the note held until the pad comes up, at the pad's own velocity, on
    // the selected track, recorded like a STEP key in KEYS. The two pads with no black key walk
    // the octave.
    void play_pad(int pad, int velocity) {
        if (kPianoSemitone[pad] == kPadOctaveDown || kPianoSemitone[pad] == kPadOctaveUp) {
            walk_keys_octave(kPianoSemitone[pad] == kPadOctaveDown ? -1 : +1);
            return;
        }
        const uint8_t note = uint8_t(pad_note(pad));
        const uint8_t vel = uint8_t(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
        release_pad(pad);
        pad_held_note_[pad] = int16_t(note);
        pad_held_track_[pad] = uint8_t(selected_);
        post(Cmd::trigger_note, uint8_t(selected_), 0, note, vel, 1);
        record_note(selected_, note, vel);
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");
        append(out, note_name(note));
        append_number(out, uint32_t(note_octave(note) < 0 ? 0 : note_octave(note)));
        *out = 0;
        notice(notice_buffer_);
    }

    // KEYS lights the pads as a keyboard: the root bright, the scale's other notes dim in the
    // selected track's colour, the rest dark, a held pad white; the two octave pads dim white.
    void light_piano_pads() {
        const uint8_t root = engine_.key_root();
        const int scale = int(engine_.key_scale());
        const Rgb colour = kTrackColour[selected_];
        for (int pad = 0; pad < kPianoPads; pad++) {
            const int note = pad_note(pad);
            if (note < 0) { leds_.pad(pad, 2, 2, 2); continue; }
            if (pad_held_note_[pad] >= 0) { leds_.pad(pad, 10, 10, 10); continue; }
            const bool at_root = note % 12 == root;
            const bool in_scale = scale_fold(uint8_t(note), root, scale) == uint8_t(note);
            const uint8_t level = at_root ? 10 : (in_scale ? 2 : 0);
            leds_.pad(pad, uint8_t(colour.r * level / 10), uint8_t(colour.g * level / 10),
                      uint8_t(colour.b * level / 10));
        }
    }

    // MACHINE is drawn in app/ui_pickview.hpp.

    // --- SOUNDS: the factory sounds (OK on a drum, synth or GRAIN track) ---
    //
    // core/sounds.hpp's presets for the selected track's machine, as a list: UP and DOWN (or A)
    // move the cursor, OK loads the one under it into the track -- with the transport running,
    // and staying in the list, so the next OK tries the next one, as the sample browser does --
    // and CLR goes back. The first row, AS WAS, is the track as it sounded when the list opened
    // (the engine remembers it, Cmd::sound_preset with kSoundPresetKeep), so trying sounds can
    // never lose the one you had. The selection keys walk the track, as in MACHINE; a new track
    // is remembered afresh. NOW marks the preset the track matches as it is.

    void open_sounds() {
        sounds_return_ = mode_;
        mode_ = Mode::sounds;
        leave_mode();
        sound_track_ = -1;
        retarget_sounds();
        notice("SOUNDS");
    }

    // The list belongs to the selected track: remember its sound and put the cursor on the
    // preset it matches, if any. A no-op while the track is the one already remembered.
    void retarget_sounds() {
        if (sound_track_ == selected_) return;
        sound_track_ = selected_;
        post(Cmd::sound_preset, uint8_t(selected_), 0, 0, kSoundPresetKeep);
        sound_cursor_ = 0;
        const uint8_t machine = engine_.machine_id(selected_);
        for (int i = 0; i < sound_preset_count(machine); i++) {
            if (sound_preset_matches(machine, i, engine_.params(selected_).v)) {
                sound_cursor_ = i + 1;
                break;
            }
        }
    }

    int sound_rows() const { return 1 + sound_preset_count(engine_.machine_id(selected_)); }

    void move_sound_cursor_to(int row) {
        const int last = sound_rows() - 1;
        sound_cursor_ = row < 0 ? 0 : (row > last ? last : row);
    }

    bool on_sounds_key(int key) {
        // A STEP held makes OK, CLR and the arrows its own chords (step page, clear, copy).
        if (held_step_ >= 0) return false;
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                const uint8_t machine = engine_.machine_id(selected_);
                if (sound_preset_count(machine) == 0) {
                    notice("NO SOUNDS HERE");
                    return true;
                }
                const char *name = sound_cursor_ == 0 ? "AS WAS"
                                                      : sound_preset(machine, sound_cursor_ - 1)->name;
                post(Cmd::sound_preset, uint8_t(selected_), 0, machine,
                     sound_cursor_ == 0 ? kSoundPresetBack : uint8_t(sound_cursor_ - 1));
                notice(name);
                // Logged too: the emulator can press OK but not read the screen
                // (tests/scenario_sounds.sh).
                char line[16];
                char *out = line;
                append(out, "snd ");
                append(out, name);
                *out = 0;
                enaga::log(line);
                return true;
            }
            case ENAGA_KEY_CLR:
                if (shift_) return false;               // F1 + CLR is undo
                mode_ = sounds_return_;
                leave_mode();
                notice(mode_name(mode_));
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) return true;
                move_sound_cursor_to(sound_cursor_ + (key == ENAGA_KEY_UP ? -1 : +1));
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                // A page, as in SAMPLES: eight rows up or down, held at the ends.
                if (!shift_) {
                    move_sound_cursor_to(sound_cursor_ +
                                         (key == ENAGA_KEY_LEFT ? -kSoundRows : kSoundRows));
                }
                return true;
            case ENAGA_KEY_OCTAVE_LEFT:                 // no steps behind this list
            case ENAGA_KEY_OCTAVE_RIGHT:
                return !shift_;
            default:
                return false;
        }
    }

    // --- SAMPLES: the sample browser (OK on a SAMPLE or GRAIN track) -------
    //
    // One folder of the card as a list (core/browse.hpp, filled by app/storage.cpp). The arrows
    // work as in every Enaga list: UP and DOWN move the cursor, LEFT and RIGHT turn a page, OK
    // opens a folder or loads the file into the track, CLR goes up a folder and, from the top,
    // back to where the browser was opened. One OK loads, with the transport running: picking
    // samples by ear is what this is for, so the cursor stays where it is and the next OK tries
    // the next file. Nothing is lost that cannot be picked again, which is why it takes one
    // press and the kit browser's recall takes two.
    //
    // Push A HEARS the file under the cursor without loading it (dsp/audition.hpp): streamed
    // from the card, whole, from its start, straight to the output. Push A again stops it.
    // While one is heard, UP/DOWN, A and the pages hear the next file the cursor lands on, so a
    // folder can be listened through before anything changes in the track. OK, CLR, another
    // folder or leaving SAMPLES stops it.

    static constexpr int kSampleRows = 8;
    // SAMPLES shows six: the preview of what is heard or played sits under the list.
    static constexpr int kSampleListRows = 6;

    static bool is_file_machine(uint8_t id) {
        return id == uint8_t(MachineId::sample) || id == uint8_t(MachineId::grain);
    }
    bool plays_files(int track) const { return is_file_machine(engine_.machine_id(track)); }
    // Does this track's machine read the ASR page? The VAs, the WAVE pair, FM4 and GRAIN
    // (core/project.hpp).
    bool has_asr(int track) const { return machine_reads_asr(engine_.machine_id(track)); }
    // The OSC page's names: the VAs' WAV OS2 SUB DRV, the WAVE pair's WRP DET SUB and then
    // GLD on WAVE MONO, DRV on WAVE POLY, FM4's DET FIN BRT CAR, or GRAIN's SCN PSP.
    const char *osc_param_name(int slot) const {
        const uint8_t id = engine_.machine_id(selected_);
        if (!machine_reads_osc(id)) return "---";
        if (machine_is_fm(id)) {
            static const char *const fm[kOscParams] = {"DET", "FIN", "BRT", "CAR"};
            return fm[slot];
        }
        if (machine_is_grain(id)) {
            // Scan and pitch spray (dsp/grain.hpp); the last two are free.
            static const char *const grain[kOscParams] = {"SCN", "PSP", "---", "---"};
            return grain[slot];
        }
        if (!machine_is_wave(id)) return kOscNames[slot];
        static const char *const wave[kOscParams] = {"WRP", "DET", "SUB", "DRV"};
        return slot == 3 && id == uint8_t(MachineId::wave_mono) ? "GLD" : wave[slot];
    }
    // And is its SUS up as it sounds now (kit, scene, macro and LFO in), so a note on it holds?
    bool holds_notes(int track) const {
        return has_asr(track) &&
               engine_.status().heard_sus[track].load(std::memory_order_relaxed) > 0;
    }

    void open_samples() {
        samples_return_ = mode_;
        mode_ = Mode::samples;
        leave_mode();
        // Where the track's file is, when it is somewhere the browser shows; else SAMPLES, or
        // the SmplTrek's sample pool on a card that has that and no /SAMPLES.
        const char *current = engine_.project().kit.tracks[selected_].sample;
        char folder[SampleList::kFolderBytes];
        if (!SampleList::in_browsable_folder(current) ||
            !SampleList::folder_of(current, folder, int(sizeof folder))) {
            const char *top = SampleList::root(SampleList::kSamplesRoot).path;
            const char *pool = SampleList::root(SampleList::kPoolRoot).path;
            if (!storage_.folder_exists(top) && storage_.folder_exists(pool)) top = pool;
            int i = 0;
            for (; top[i] != '\0'; i++) folder[i] = top[i];
            folder[i] = '\0';
        }
        browse_to(folder);
        notice(storage_.card_usable() ? "SAMPLES" : "NO CARD");
    }

    // List `folder` and put the cursor on `select` once the list is in ("" for the top, or for
    // the track's own file when the folder is the one it lives in).
    void browse_to(const char *folder) {
        stop_audition();
        storage_.request_browse(folder);
        sample_cursor_ = 0;
        sample_seek_ = true;
        // What the folder we came OUT of was called, so CLR lands the cursor back on it.
        sample_seek_name_[0] = '\0';
    }

    void move_sample_cursor_to(int index) {
        const int count = storage_.browse_list().count();
        const int was = sample_cursor_;
        sample_cursor_ = index < 0 ? 0 : (index >= count ? (count > 0 ? count - 1 : 0) : index);
        // Listening through a folder: the next entry is heard as the cursor lands on it.
        if (sample_cursor_ != was && storage_.auditioning()) audition_cursor();
    }

    // Hear the entry under the cursor: a file is streamed (Storage::request_audition), a folder
    // stops whatever plays. False when there is nothing to hear there.
    bool audition_cursor() {
        const SampleList &list = storage_.browse_list();
        if (storage_.browsing() || list.count() == 0 || list.is_folder(sample_cursor_)) {
            stop_audition();
            return false;
        }
        char path[kSamplePathBytes];
        if (!list.path_of(sample_cursor_, path, int(sizeof path)) ||
            !storage_.request_audition(path)) {
            stop_audition();
            notice("PATH TOO LONG");
            return false;
        }
        audition_row_ = sample_cursor_;
        return true;
    }

    void stop_audition() {
        storage_.stop_audition();
        audition_row_ = -1;
    }

    // Once a listing is in, the cursor goes to what we were looking for: the folder we came up
    // out of, or the file the track plays.
    void settle_sample_cursor() {
        if (!sample_seek_ || storage_.browsing()) return;
        sample_seek_ = false;
        const SampleList &list = storage_.browse_list();
        const char *want = sample_seek_name_[0] != '\0'
                               ? sample_seek_name_
                               : SampleList::name_of(engine_.project().kit.tracks[selected_].sample);
        const int at = list.find(want);
        sample_cursor_ = at < 0 ? 0 : at;
    }

    bool on_samples_key(int key) {
        // A STEP held makes OK, CLR and the arrows its own chords (step page, clear, copy).
        if (held_step_ >= 0) return false;
        const SampleList &list = storage_.browse_list();
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                if (storage_.browsing() || list.count() == 0) return true;
                if (list.is_folder(sample_cursor_)) {
                    char folder[SampleList::kFolderBytes];
                    // At the root, path_of gives the folder the entry opens (POOL is
                    // /SmplTrek/Pool/Audio); below it, folder + "/" + name.
                    if (!list.path_of(sample_cursor_, folder, int(sizeof folder))) {
                        notice("PATH TOO LONG");
                        return true;
                    }
                    browse_to(folder);
                    return true;
                }
                char path[kSamplePathBytes];
                if (!list.path_of(sample_cursor_, path, int(sizeof path))) {
                    notice("PATH TOO LONG");
                    return true;
                }
                // The track has it now: the audition makes way.
                stop_audition();
                if (!plays_files(selected_)) {
                    notice("SMP OR GRN ONLY");
                } else if (!storage_.card_usable()) {
                    notice("NO CARD");
                } else if (storage_.request_sample_file(selected_, path)) {
                    // A new file on a SAMPLE track plays whole (core/project.hpp,
                    // kSamplePickResets). Posted after the request, so the knobs and the file
                    // change together; the old sample's voices are stopped by the swap anyway.
                    if (engine_.machine_id(selected_) == uint8_t(MachineId::sample)) {
                        for (const ParamReset &r : kSamplePickResets) {
                            post(Cmd::set_param, uint8_t(selected_), 0, r.slot, r.value);
                        }
                    }
                    notice("LOADING");
                } else {
                    notice("PATH TOO LONG");
                }
                return true;
            }

            case ENAGA_KEY_CLR: {
                if (shift_) return false;               // F1 + CLR is undo
                stop_audition();
                if (list.at_root()) {
                    mode_ = samples_return_;            // back, where the browser came from
                    leave_mode();
                    notice(mode_name(mode_));
                    return true;
                }
                // Up a folder, with the cursor on the one we left.
                char parent[SampleList::kFolderBytes];
                SampleList::parent_of(list.folder(), parent, int(sizeof parent));
                const char *left = SampleList::name_of(list.folder());
                int i = 0;
                for (; left[i] != '\0' && i < SampleList::kEntryNameBytes - 1; i++) {
                    sample_seek_name_[i] = left[i];
                }
                sample_seek_name_[i] = '\0';
                storage_.request_browse(parent);
                sample_cursor_ = 0;
                sample_seek_ = true;
                return true;
            }

            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) {
                    // FUNC+UP reads the folder again (a card swapped, a take just written).
                    if (key == ENAGA_KEY_UP) {
                        char again[SampleList::kFolderBytes];
                        const char *f = list.folder();
                        int i = 0;
                        for (; f[i] != '\0'; i++) again[i] = f[i];
                        again[i] = '\0';
                        storage_.request_browse(again);
                        sample_seek_ = false;
                        notice("READING");
                    }
                    return true;
                }
                move_sample_cursor_to(sample_cursor_ + (key == ENAGA_KEY_UP ? -1 : +1));
                return true;

            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (!shift_) {
                    move_sample_cursor_to(sample_cursor_ + (key == ENAGA_KEY_LEFT
                                                                ? -kSampleListRows
                                                                : kSampleListRows));
                }
                return true;

            case ENAGA_KEY_OCTAVE_LEFT:                 // no steps behind this list
            case ENAGA_KEY_OCTAVE_RIGHT:
                return !shift_;                         // FUNC: the track's length, as anywhere

            // Push A: hear the file under the cursor, or stop hearing it. FUNC + push A is the
            // kill EQ's, taken before this (ui_input.hpp).
            case ENAGA_KEY_PUSH_A:
                if (shift_) return false;
                if (storage_.auditioning()) {
                    stop_audition();
                    notice("STOPPED");
                } else if (storage_.browsing() || list.count() == 0) {
                    // Nothing listed yet.
                } else if (list.is_folder(sample_cursor_)) {
                    notice("A HEARS FILES");
                } else if (!storage_.card_usable()) {
                    notice("NO CARD");
                } else if (audition_cursor()) {
                    notice("HEARING");
                }
                return true;

            default:
                return false;
        }
    }

    // --- PROJECT's PROJECTS page: open another project, save under a new name ----

    // The card's projects: the folders in PROJECTS/, listed into the same table the sample
    // browser uses (the two are never on screen together).
    void list_projects() {
        char folder[SampleList::kFolderBytes];
        if (!storage_.projects_folder(folder, sizeof folder)) return;
        storage_.request_browse(folder);
        project_cursor_ = 0;
        project_seek_ = true;
        open_armed_ = -1;
        delete_armed_ = -1;
    }

    void move_project_cursor_to(int index) {
        const int count = storage_.browse_list().count();
        project_cursor_ = index < 0 ? 0 : (index >= count ? (count > 0 ? count - 1 : 0) : index);
        open_armed_ = -1;
        delete_armed_ = -1;
    }

    void settle_project_cursor() {
        // A SAVE AS made a folder the list does not have yet: read it again once it is written.
        if (projects_reread_ && storage_.busy_text() == nullptr && !storage_.dirty() &&
            !storage_.browsing()) {
            projects_reread_ = false;
            list_projects();
        }
        // A rename or a delete changed the folders: read them again once it is done, whatever
        // the open project's own state (a dirty project would hold the SAVE AS reread for ever).
        if (projects_op_reread_ && !storage_.project_op_pending() && !storage_.browsing()) {
            projects_op_reread_ = false;
            list_projects();
        }
        if (!project_seek_ || storage_.browsing()) return;
        project_seek_ = false;
        const int at = storage_.browse_list().find(storage_.project_name());
        project_cursor_ = at < 0 ? 0 : at;
    }

    bool on_projects_key(int key) {
        if (held_step_ >= 0) return false;
        const SampleList &list = storage_.browse_list();
        // A delete asked for (push C) takes OK twice within kSaveArmMs. CLR keeps the project;
        // any other key lets the question go and does its own job.
        if (delete_armed_ >= 0 && !shift_) {
            if (key == ENAGA_KEY_OK) {
                if (delete_oks_ > 0 && uint32_t(last_ms_ - delete_ok_at_ms_) < kSaveArmMs) {
                    const char *name = list.name(delete_armed_);
                    delete_armed_ = -1;
                    if (!storage_.request_delete(name)) {
                        notice("CARD BUSY");
                        return true;
                    }
                    projects_op_reread_ = true;
                    notice("DELETING");
                    return true;
                }
                delete_oks_ = 1;
                delete_ok_at_ms_ = last_ms_;
                notice("OK AGAIN TO DELETE");
                return true;
            }
            delete_armed_ = -1;
            if (key == ENAGA_KEY_CLR) {
                notice("DELETE OFF");
                return true;
            }
        }
        switch (key) {
            case ENAGA_KEY_PUSH_B:
            case ENAGA_KEY_PUSH_C: {
                // Push B renames and push C deletes the project under the cursor: the two card
                // actions a list of projects needs, on the knobs the page leaves free.
                if (storage_.browsing() || list.count() == 0) return true;
                const char *name = list.name(project_cursor_);
                const bool rename = key == ENAGA_KEY_PUSH_B;
                if (!storage_.card_usable()) {
                    notice("NO CARD");
                } else if (!list.is_folder(project_cursor_)) {
                    notice("NOT A PROJECT");
                } else if (!Storage::valid_name(name)) {
                    notice("NAME NOT USABLE");
                } else if (storage_.project_op_pending()) {
                    notice("CARD BUSY");
                } else if (rename) {
                    int n = 0;
                    for (; name[n] != '\0' && n < Storage::kNameBytes - 1; n++) {
                        rename_from_[n] = name[n];
                    }
                    rename_from_[n] = '\0';
                    entry_for_ = EntryFor::rename;
                    name_entry_.open("RENAME", name);
                } else if (SampleList::same_name(name, storage_.project_name())) {
                    // Deleting what is in RAM would leave a project whose files are gone, and
                    // the next save would bring them back.
                    notice("PROJECT IS OPEN");
                } else {
                    delete_armed_ = project_cursor_;
                    delete_oks_ = 0;
                    open_armed_ = -1;
                    notice("DELETE? OK TWICE");
                }
                return true;
            }
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo
                if (storage_.browsing() || list.count() == 0) return true;
                const char *name = list.name(project_cursor_);
                if (!list.is_folder(project_cursor_)) {
                    notice("NOT A PROJECT");
                    return true;
                }
                if (SampleList::same_name(name, storage_.project_name())) {
                    notice("ALREADY OPEN");
                    return true;
                }
                if (!Storage::valid_name(name)) {
                    notice("NAME NOT USABLE");
                    return true;
                }
                // Opening throws away what is unsaved and stops the transport, so it takes a
                // second press within kSaveArmMs, as a reload does.
                if (open_armed_ == project_cursor_ &&
                    uint32_t(last_ms_ - open_armed_at_ms_) < kSaveArmMs) {
                    open_armed_ = -1;
                    storage_.request_open(name);
                    project_page_ = ProjectPage::slots;
                    notice("OPENING");
                    return true;
                }
                open_armed_ = project_cursor_;
                open_armed_at_ms_ = last_ms_;
                notice("AGAIN TO OPEN");
                return true;
            }
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) {
                    if (key == ENAGA_KEY_UP) {
                        list_projects();                // FUNC+UP reads the card again
                        notice("READING");
                    } else if (!storage_.card_usable()) {
                        notice("NO CARD");
                    } else {
                        // FUNC+DOWN: SAVE AS, starting from the name it has.
                        entry_for_ = EntryFor::save_as;
                        name_entry_.open("SAVE AS", storage_.project_name());
                    }
                    return true;
                }
                move_project_cursor_to(project_cursor_ + (key == ENAGA_KEY_UP ? -1 : +1));
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (shift_) return false;               // reload and new, as on the slots
                move_project_cursor_to(project_cursor_ +
                                       (key == ENAGA_KEY_LEFT ? -kSampleRows : kSampleRows));
                return true;
            default:
                return false;
        }
    }

    // OK on the name entry.
    void finish_save_as() {
        char name[Storage::kNameBytes];
        const char *typed = name_entry_.text();
        int n = 0;
        for (; typed[n] != '\0' && n < Storage::kNameBytes - 1; n++) name[n] = typed[n];
        name[n] = '\0';
        if (name[0] == '\0') {
            notice("NO NAME");
            return;
        }
        // Another project's folder is not overwritten by a SAVE AS: it is that project.
        const SampleList &list = storage_.browse_list();
        const int at = list.find(name);
        if (at >= 0 && list.is_folder(at) && !SampleList::same_name(name, storage_.project_name())) {
            notice("NAME TAKEN");
            return;
        }
        if (!storage_.request_save_as(name)) {
            notice("NAME NOT USABLE");
            return;
        }
        projects_reread_ = true;
        notice("SAVING");
    }

    // OK on the name entry opened by push B.
    void finish_rename() {
        char name[Storage::kNameBytes];
        const char *typed = name_entry_.text();
        int n = 0;
        for (; typed[n] != '\0' && n < Storage::kNameBytes - 1; n++) name[n] = typed[n];
        name[n] = '\0';
        if (name[0] == '\0') {
            notice("NO NAME");
            return;
        }
        if (SampleList::same_name(name, rename_from_)) {
            notice("NAME KEPT");
            return;
        }
        // The list is the card's folders: a name in it is taken. Storage checks the card again
        // when the rename runs, so a folder made since is refused there (NAME TAKEN) too.
        const int at = storage_.browse_list().find(name);
        if (at >= 0) {
            notice("NAME TAKEN");
            return;
        }
        if (!storage_.request_rename(rename_from_, name)) {
            notice(Storage::valid_name(name) ? "CARD BUSY" : "NAME NOT USABLE");
            return;
        }
        projects_op_reread_ = true;
        notice("RENAMING");
    }

    // The SAVE AS entry. Enaga's TextEntry does the editing; its own draw() puts each character
    // in a box 7 px from the next, which reads as separate words beside the rest of STRATA's
    // screens, so the name is drawn here as one string with a line under each place and a bright
    // one under the cursor.
    void draw_name_entry(enaga::Canvas &canvas) {
        constexpr int kChars = enaga::ui::TextEntry::max_chars;
        constexpr int kX = (128 - kChars * 6) / 2;
        const bool rename = entry_for_ == EntryFor::rename;
        paint::small_text(canvas.data(), 0, 19, rename ? "RENAME" : "SAVE AS", 15);
        if (rename) {
            char line[40];
            char *out = line;
            append(out, "FROM ");
            append_shown(out, rename_from_, kLineChars - 5);
            *out = 0;
            canvas.text(0, 30, line, 9);
        }
        canvas.text(kX, 48, name_entry_.text(), 15);
        for (int i = 0; i < kChars; i++) {
            canvas.rect(kX + i * 6, 57, 5, 1, i == name_entry_.cursor() ? 15 : 4);
        }
        paint::small_text(canvas.data(), 0, 89, "A / LEFT RIGHT: MOVE", 10);
        paint::small_text(canvas.data(), 0, 97, "B / VALUE: CHARACTER", 10);
        paint::small_text(canvas.data(), 0, 105, rename ? "OK: RENAME CLR: BACK" : "OK: SAVE  CLR: CANCEL", 10);
    }

    void draw_projects(enaga::Canvas &canvas) {
        if (name_entry_.active()) {
            draw_name_entry(canvas);
            return;
        }
        const SampleList &list = storage_.browse_list();
        char line[40];
        char *out = line;
        // The caption (the tab says PROJS): the open project, or what the card is doing; where
        // the cursor is in the list at the right.
        uint8_t *px = canvas.data();
        const int count = list.count();
        out = line;
        if (count > 0) {
            append_number(out, uint32_t(project_cursor_ + 1));
            *out++ = '/';
            append_number(out, uint32_t(count));
        }
        *out = 0;
        const int room = caption_right(px, line, 10);
        out = line;
        const char *busy = storage_.busy_text();
        const int chars = (room + 1) / paint::kSmallAdvance;
        const bool asking = delete_armed_ >= 0;
        if (asking) {
            append(out, delete_oks_ > 0 ? "OK AGAIN: DELETE" : "DELETE? OK TWICE");
        } else if (storage_.browsing()) {
            append(out, "READING CARD");
        } else if (storage_.browse_error() != nullptr) {
            append_shown(out, storage_.browse_error(), chars);
        } else if (busy != nullptr) {
            append_shown(out, busy, chars);
        } else {
            append(out, "NOW ");
            append_shown(out, storage_.project_name(), chars - 4);
            if (storage_.dirty() && int(out - line) + 8 <= chars) append(out, " UNSAVED");
        }
        *out = 0;
        if (asking) {
            paint::small_badge(px, 1, 18, line);
        } else {
            paint::small_text(px, 0, 18, line, 13);
        }

        const int first = (project_cursor_ / kSampleRows) * kSampleRows;
        if (count == 0 && !storage_.browsing()) canvas.text(0, 26, "NO PROJECTS YET", 9);
        for (int i = 0; i < kSampleRows && first + i < count; i++) {
            const int at = first + i;
            out = line;
            *out++ = at == project_cursor_ ? '>' : ' ';
            append_shown(out, list.name(at), kLineChars - 6);
            *out = 0;
            const int y = 26 + i * 9;
            const bool open = SampleList::same_name(list.name(at), storage_.project_name());
            list_row(canvas, 0, y, line, at == project_cursor_, open ? 12 : (list.is_folder(at) ? 10 : 5));
            // The open one says so at the row's right.
            if (open) {
                paint::small_text(px, 123 - paint::small_width("OPEN"), y + 1, "OPEN",
                                  at == project_cursor_ ? 0 : 9);
            }
        }

        // SAVE AS (FUNC+DOWN) is in the FUNC menu as well; the knobs' two jobs are only here.
        paint::small_text(px, 0, 105, asking ? "CLR KEEPS IT" : "PUSH B RENAME  C DELETE", 10);
        paint::small_text(px, 0, 113, "OK OPEN  CLR BACK", 10);
    }

    // --- KIT: the kit library (FUNC+EDIT twice) ---------------------------
    //
    // Sixteen kits on the card, shared by every project (app/storage.hpp, "the kit library").
    // The list is the arrows' rows, as every Enaga list is: UP and DOWN move the cursor, LEFT and
    // RIGHT flip the two pages of eight, OK picks -- twice, because a recall replaces the sound
    // and is not undoable -- and CLR goes back to MACHINE. F1 + DOWN puts the kit down into the
    // slot, the same direction it pastes in every editing mode; over a kit already there it
    // takes a second press. F1 + UP reads the card again.

    static const char *kit_state_name(Storage::SlotState state) {
        switch (state) {
            case Storage::SlotState::empty: return "EMPTY";
            case Storage::SlotState::used:  return "KIT";
            case Storage::SlotState::bad:   return "UNREADABLE";
            default:                        return "...";
        }
    }
