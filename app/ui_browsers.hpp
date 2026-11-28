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

    // ARPS is the same list over core/arppresets.hpp, opened by OK on the ARP page: the same
    // keys, AS WAS first, NOW on the arp the track has; `sounds_arps_` says which it is.
    void open_sounds(bool arps = false) {
        sounds_return_ = mode_;
        sounds_arps_ = arps;
        mode_ = Mode::sounds;
        leave_mode();
        sound_track_ = -1;
        retarget_sounds();
        notice(arps ? "ARPS" : "SOUNDS");
    }

    // The list belongs to the selected track: remember its sound and put the cursor on the
    // preset it matches, if any. A no-op while the track is the one already remembered.
    void retarget_sounds() {
        if (sound_track_ == selected_) return;
        sound_track_ = selected_;
        sound_cursor_ = 0;
        if (sounds_arps_) {
            post(Cmd::arp_preset, uint8_t(selected_), 0, 0, kSoundPresetKeep);
            for (int i = 0; i < arp_preset_count(); i++) {
                if (arp_preset_now(i)) {
                    sound_cursor_ = i + 1;
                    break;
                }
            }
            return;
        }
        post(Cmd::sound_preset, uint8_t(selected_), 0, 0, kSoundPresetKeep);
        sound_delete_armed_ = -1;
        const uint8_t machine = engine_.machine_id(selected_);
        storage_.want_sound_bank(machine);
        for (int i = 0; i < sound_preset_count(machine); i++) {
            if (sound_preset_matches(machine, i, engine_.params(selected_).v)) {
                sound_cursor_ = sound_factory_first() + i;
                break;
            }
        }
    }

    // --- the rows of SOUNDS (not ARPS) ---
    //
    //   AS WAS               the track as it was when the list opened
    //   PASTE T1 KCK         the copied sound (FUNC + UP), while there is one
    //   the factory sounds   core/sounds.hpp, the machine's
    //   your own sounds      core/usersounds.hpp, the machine's, from the card
    //
    // Your own sounds come last so that their arriving from the card, a pass after the list
    // opens, moves no row above them.
    enum class SoundRow : uint8_t { as_was, paste, factory, user };

    int sound_paste_rows() const { return sound_clip_.has ? 1 : 0; }
    int sound_factory_first() const { return 1 + sound_paste_rows(); }
    int sound_user_first() const {
        return sound_factory_first() + sound_preset_count(engine_.machine_id(selected_));
    }
    // The user sounds the list shows: the machine's bank once it is read.
    int sound_user_rows() const {
        const uint8_t machine = engine_.machine_id(selected_);
        return storage_.sound_bank_state(machine) == Storage::SoundBankState::ready
                   ? user_bank_count(storage_.sound_bank())
                   : 0;
    }

    // What row `row` is, and in `index` which factory sound (0-based) or which bank SLOT.
    SoundRow sound_row_kind(int row, int *index) const {
        *index = 0;
        if (row <= 0) return SoundRow::as_was;
        if (sound_clip_.has && row == 1) return SoundRow::paste;
        const int user_first = sound_user_first();
        if (row < user_first) {
            *index = row - sound_factory_first();
            return SoundRow::factory;
        }
        *index = user_bank_nth(storage_.sound_bank(), row - user_first);
        return *index >= 0 ? SoundRow::user : SoundRow::as_was;
    }

    // The user sound on row `row`, or nullptr.
    const UserSound *sound_row_user(int row) const {
        int index = 0;
        if (sound_row_kind(row, &index) != SoundRow::user) return nullptr;
        return &storage_.sound_bank().slots[index];
    }

    // The row bank slot `slot` is drawn on, or -1.
    int sound_row_of_slot(int slot) const {
        const UserSoundBank &bank = storage_.sound_bank();
        if (slot < 0 || slot >= kUserSoundSlots || !bank.slots[slot].used) return -1;
        int rank = 0;
        for (int s = 0; s < slot; s++) rank += bank.slots[s].used ? 1 : 0;
        return sound_user_first() + rank;
    }

    // A sound into a track: its parameters eight a command (Cmd::sound_bytes), then its LFO
    // TRIG, mod slots and arp settings by the commands their pages post, all together so they
    // land in one block. Only the bytes a sound owns are written on the audio side. The last
    // bytes command is short (44 is not a multiple of eight): its spare bytes go as 0, past
    // every parameter, and the engine skips them.
    void post_sound(int track, uint8_t machine, const uint8_t *params, const UserSoundExtras &extras) {
        for (int first = 0; first < kKitParams; first += 8) {
            uint8_t b[8] = {};
            for (int i = 0; i < 8 && first + i < kKitParams; i++) b[i] = params[first + i];
            Command command;
            command.cmd = Cmd::sound_bytes;
            command.track = uint8_t(track);
            command.step = uint8_t(first);
            command.a = machine;
            command.b = b[0];
            command.c = b[1];
            command.d = b[2];
            command.e = b[3];
            command.u = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
            post_command(command);
        }
        const uint8_t t = uint8_t(track);
        post(Cmd::set_lfo_trig, t, 0, extras.lfo_trig);
        for (int m = 0; m < kModSlots; m++) {
            Command slot;
            slot.cmd = Cmd::set_mod_slot;
            slot.track = t;
            slot.a = uint8_t(m);
            slot.b = extras.mod[m].source;
            slot.c = extras.mod[m].dest;
            slot.d = uint8_t(extras.mod[m].depth);
            post_command(slot);
        }
        for (int f = 0; f < kArpExtFields; f++) {
            post(Cmd::set_arp_ext, t, 0, uint8_t(f), arp_ext_get(extras.arp, f));
        }
    }

    // The selected track's LFO TRIG, mod slots and arp settings, as a sound would carry them.
    UserSoundExtras selected_extras() const {
        return user_sound_extras_of(engine_.project().kit.tracks[selected_]);
    }

    // `snd <what>` in the log: the emulator can press keys but not read the screen
    // (tests/scenario_usersounds.sh).
    static void log_sound(const char *what, const char *name) {
        char line[32];
        char *out = line;
        append(out, "snd ");
        append(out, what);
        if (name != nullptr) {
            append(out, " ");
            append(out, name);
        }
        *out = 0;
        enaga::log(line);
    }

    // FUNC + UP: the selected track's sound, machine and all, to the clipboard. The PASTE row
    // appears under AS WAS, on every track, until the next copy.
    void copy_sound() {
        const uint8_t machine = engine_.machine_id(selected_);
        if (!user_sounds_supported(machine)) {
            notice("NO SOUND TO COPY");
            return;
        }
        if (!sound_clip_.has && sound_cursor_ >= 1) sound_cursor_++;   // the new row above it
        sound_clip_.has = true;
        sound_clip_.machine = machine;
        sound_clip_.track = uint8_t(selected_);
        const uint8_t *now = engine_.params(selected_).v;
        for (int p = 0; p < kKitParams; p++) sound_clip_.params[p] = now[p];
        sound_clip_.extras = selected_extras();
        char *out = notice_buffer_;
        append(out, "COPIED T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");
        append(out, machine_name(machine));
        *out = 0;
        notice(notice_buffer_);
        log_sound("copy", machine_name(machine));
    }

    // OK on PASTE: the copied sound into the selected track. A track with another machine is
    // switched to the copied one first (Cmd::set_machine, which loads that machine's defaults
    // under it), so the bytes mean there what they meant where they came from. AS WAS takes
    // both back.
    void paste_sound() {
        const uint8_t machine = engine_.machine_id(selected_);
        if (machine != sound_clip_.machine) {
            post(Cmd::set_machine, uint8_t(selected_), 0, sound_clip_.machine);
        }
        post_sound(selected_, sound_clip_.machine, sound_clip_.params, sound_clip_.extras);
        char *out = notice_buffer_;
        append(out, "PASTED TO T");
        append_number(out, uint32_t(selected_) + 1);
        *out = 0;
        notice(notice_buffer_);
        log_sound("paste", machine_name(sound_clip_.machine));
    }

    // FUNC + DOWN: save the track's sound as one of your own -- into the slot under the cursor
    // when it is one of yours (REPLACE), else into the first empty one. Names it first.
    void begin_sound_save() {
        const uint8_t machine = engine_.machine_id(selected_);
        if (!user_sounds_supported(machine)) {
            notice("NO SOUNDS HERE");
            return;
        }
        if (!storage_.card_usable()) {
            notice("NO CARD");
            return;
        }
        const Storage::SoundBankState state = storage_.sound_bank_state(machine);
        if (state == Storage::SoundBankState::failed) {
            notice("SOUND FILE BAD");
            return;
        }
        if (state != Storage::SoundBankState::ready) {
            notice("READING CARD");
            return;
        }
        int index = 0;
        const bool replace = sound_row_kind(sound_cursor_, &index) == SoundRow::user;
        const int slot = replace ? index : user_bank_free_slot(storage_.sound_bank());
        if (slot < 0) {
            notice("16 SAVED: FULL");
            return;
        }
        sound_entry_slot_ = slot;
        sound_entry_machine_ = machine;
        const uint8_t *now = engine_.params(selected_).v;
        for (int p = 0; p < kKitParams; p++) sound_entry_params_[p] = now[p];
        sound_entry_extras_ = selected_extras();
        char name[kUserSoundNameChars + 1];
        if (replace) {
            const char *had = storage_.sound_bank().slots[slot].name;
            int n = 0;
            for (; had[n] != '\0' && n < kUserSoundNameChars; n++) name[n] = had[n];
            name[n] = '\0';
        } else {
            user_sound_default_name(storage_.sound_bank(), nullptr, name);
        }
        entry_for_ = replace ? EntryFor::sound_replace : EntryFor::sound_save;
        name_entry_.open(replace ? "REPLACE" : "SAVE SOUND", name, kUserSoundNameChars);
    }

    // The name typed, without trailing spaces; false (and a notice) for one that cannot be used,
    // or one another of the machine's sounds already has.
    bool typed_sound_name(char *name, int keep_slot) {
        const char *typed = name_entry_.text();
        int n = 0;
        for (; typed[n] != '\0' && n < kUserSoundNameChars; n++) name[n] = typed[n];
        name[n] = '\0';
        int lead = 0;
        while (name[lead] == ' ') lead++;
        if (lead > 0) {
            int i = 0;
            for (; name[lead + i] != '\0'; i++) name[i] = name[lead + i];
            name[i] = '\0';
        }
        if (name[0] == '\0') {
            notice("NO NAME");
            return false;
        }
        if (!user_sound_name_valid(name)) {
            notice("NAME NOT USABLE");
            return false;
        }
        const int at = user_bank_find(storage_.sound_bank(), name);
        if (at >= 0 && at != keep_slot) {
            notice("NAME TAKEN");
            return false;
        }
        return true;
    }

    // OK on the name entry from FUNC + DOWN.
    void finish_sound_save() {
        char name[kUserSoundNameChars + 1];
        if (!typed_sound_name(name, sound_entry_slot_)) return;
        if (!storage_.request_sound_store(sound_entry_machine_, sound_entry_slot_, name,
                                          sound_entry_params_, sound_entry_extras_)) {
            notice(storage_.card_usable() ? "CARD BUSY" : "NO CARD");
            return;
        }
        const int row = sound_row_of_slot(sound_entry_slot_);
        if (row >= 0 && engine_.machine_id(selected_) == sound_entry_machine_) sound_cursor_ = row;
        char *out = notice_buffer_;
        append(out, "SAVED ");
        append(out, name);
        *out = 0;
        notice(notice_buffer_);
        log_sound("saved", name);
    }

    // OK on the name entry from push B.
    void finish_sound_rename() {
        char name[kUserSoundNameChars + 1];
        if (!typed_sound_name(name, sound_entry_slot_)) return;
        if (!storage_.request_sound_rename(sound_entry_machine_, sound_entry_slot_, name)) {
            notice(storage_.card_usable() ? "CARD BUSY" : "NO CARD");
            return;
        }
        notice("RENAMED");
        log_sound("renamed", name);
    }

    // Is arp preset `index` what the selected track has now?
    bool arp_preset_now(int index) const {
        return arp_preset_matches(index, &engine_.params(selected_).v[kArpFirst],
                                  engine_.project().kit.tracks[selected_].arp);
    }

    int sound_rows() const {
        if (sounds_arps_) return 1 + arp_preset_count();
        return sound_user_first() + sound_user_rows();
    }

    // The screen's name: SOUNDS, or ARPS for the arp list.
    const char *screen_mode_name() const {
        return mode_ == Mode::sounds && sounds_arps_ ? "ARPS" : mode_name(mode_);
    }

    void move_sound_cursor_to(int row) {
        const int last = sound_rows() - 1;
        sound_cursor_ = row < 0 ? 0 : (row > last ? last : row);
    }

    bool on_sounds_key(int key) {
        // A STEP held makes OK, CLR and the arrows its own chords (step page, clear, copy).
        if (held_step_ >= 0) return false;
        if (!sounds_arps_) {
            // A pad may have picked another track since the list last looked.
            retarget_sounds();
            // A delete asked for (push C) takes OK twice within kSaveArmMs, as on PROJECTS. CLR
            // keeps the sound; any other key lets the question go and does its own job.
            if (sound_delete_armed_ >= 0 && !shift_) {
                if (key == ENAGA_KEY_OK) {
                    if (sound_delete_oks_ > 0 &&
                        uint32_t(last_ms_ - sound_delete_ok_at_ms_) < kSaveArmMs) {
                        const int slot = sound_delete_armed_;
                        sound_delete_armed_ = -1;
                        char name[kUserSoundNameChars + 1] = {};
                        const char *had = storage_.sound_bank().slots[slot].name;
                        for (int i = 0; i < kUserSoundNameChars && had[i] != '\0'; i++) name[i] = had[i];
                        if (!storage_.request_sound_delete(engine_.machine_id(selected_), slot)) {
                            notice(storage_.card_usable() ? "CARD BUSY" : "NO CARD");
                            return true;
                        }
                        move_sound_cursor_to(sound_cursor_);   // the list is a row shorter
                        notice("DELETED");
                        log_sound("deleted", name);
                        return true;
                    }
                    sound_delete_oks_ = 1;
                    sound_delete_ok_at_ms_ = last_ms_;
                    notice("OK AGAIN: DELETE");
                    return true;
                }
                sound_delete_armed_ = -1;
                if (key == ENAGA_KEY_CLR) {
                    notice("DELETE OFF");
                    return true;
                }
            }
        }
        switch (key) {
            case ENAGA_KEY_PUSH_B:
            case ENAGA_KEY_PUSH_C: {
                // Push B renames and push C deletes the sound under the cursor, as on PROJECTS:
                // your own only, the factory's are part of the build.
                if (sounds_arps_ || shift_) return false;
                int index = 0;
                if (sound_row_kind(sound_cursor_, &index) != SoundRow::user) {
                    notice("YOUR OWN SOUNDS ONLY");
                    return true;
                }
                if (!storage_.card_usable()) {
                    notice("NO CARD");
                    return true;
                }
                if (key == ENAGA_KEY_PUSH_B) {
                    sound_entry_slot_ = index;
                    sound_entry_machine_ = engine_.machine_id(selected_);
                    entry_for_ = EntryFor::sound_rename;
                    name_entry_.open("RENAME SOUND", storage_.sound_bank().slots[index].name,
                                     kUserSoundNameChars);
                } else {
                    sound_delete_armed_ = index;
                    sound_delete_oks_ = 0;
                    notice("OK TWICE: DELETE");
                }
                return true;
            }
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                if (sounds_arps_) {
                    const char *name = sound_cursor_ == 0 ? "AS WAS" : arp_preset(sound_cursor_ - 1)->name;
                    post(Cmd::arp_preset, uint8_t(selected_), 0, 0,
                         sound_cursor_ == 0 ? kSoundPresetBack : uint8_t(sound_cursor_ - 1));
                    notice(name);
                    // Logged too: the emulator can press OK but not read the screen
                    // (tests/scenario_arp.sh).
                    char line[16];
                    char *out = line;
                    append(out, "arp ");
                    append(out, name);
                    *out = 0;
                    enaga::log(line);
                    return true;
                }
                const uint8_t machine = engine_.machine_id(selected_);
                int index = 0;
                const SoundRow kind = sound_row_kind(sound_cursor_, &index);
                if (kind == SoundRow::paste) {
                    paste_sound();
                    return true;
                }
                if (sound_rows() == 1) {
                    notice("NO SOUNDS HERE");
                    return true;
                }
                const char *name = "AS WAS";
                if (kind == SoundRow::user) {
                    const UserSound &sound = storage_.sound_bank().slots[index];
                    post_sound(selected_, machine, sound.params, sound.extras);
                    name = sound.name;
                } else {
                    if (kind == SoundRow::factory) name = sound_preset(machine, index)->name;
                    post(Cmd::sound_preset, uint8_t(selected_), 0, machine,
                         kind == SoundRow::factory ? uint8_t(index) : kSoundPresetBack);
                }
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
                sounds_arps_ = false;
                leave_mode();
                notice(mode_name(mode_));
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) {
                    // FUNC + UP / DOWN: copy the track's sound / save it as your own -- copy, and
                    // "store here" as on the KIT and PROJECTS lists.
                    if (sounds_arps_) {
                        notice("ARPS: NOT SAVED");
                    } else if (key == ENAGA_KEY_UP) {
                        copy_sound();
                    } else {
                        begin_sound_save();
                    }
                    return true;
                }
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
        if (!machine_has_osc_page(id)) return "---";
        if (machine_is_kick(id)) {
            static const char *const kick[kOscParams] = {"CHK", "---", "---", "---"};
            return kick[slot];
        }
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
        audition_armed_ = -1;
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
        if (sample_cursor_ != was) audition_armed_ = -1;   // moving on lets a first push go
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
        if (storage_.browsing()) return;
        if (!sample_seek_) {
            // FUNC+UP reads the folder again and keeps the cursor; a list that came back shorter
            // (a card swapped, a file gone) must not leave it past the end, where the screen said
            // 9/4 over an empty page and OK said PATH TOO LONG.
            const int count = storage_.browse_list().count();
            if (sample_cursor_ >= count) sample_cursor_ = count > 0 ? count - 1 : 0;
            return;
        }
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
                        stop_audition();        // its row may hold another file next
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
                } else if (enaga::clock::running() &&
                           !(audition_armed_ == sample_cursor_ &&
                             uint32_t(last_ms_ - audition_armed_at_ms_) < kSaveArmMs)) {
                    // There is no cue output: the audition goes to the same output as the beat,
                    // so in a set the room would hear it. With the transport running it takes a
                    // second push within kSaveArmMs, as opening a project does. (FUNC + push A
                    // is the kill EQ's, so FUNC cannot be the way past.)
                    audition_armed_ = sample_cursor_;
                    audition_armed_at_ms_ = last_ms_;
                    notice_logged("A AGAIN: HEAR");   // tests/scenario_auditionplay.sh
                } else if (audition_cursor()) {
                    audition_armed_ = -1;
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
                notice("OK AGAIN: DELETE");
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
                    notice("OK TWICE: DELETE");
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
                notice("OK AGAIN: OPEN");
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
        // SOUNDS' names are eight characters (core/usersounds.hpp), a project's sixteen.
        const bool sound = entry_for_ == EntryFor::sound_save ||
                           entry_for_ == EntryFor::sound_replace ||
                           entry_for_ == EntryFor::sound_rename;
        const int kChars = sound ? kUserSoundNameChars : enaga::ui::TextEntry::max_chars;
        const int kX = (128 - kChars * 6) / 2;
        const bool rename = entry_for_ == EntryFor::rename || entry_for_ == EntryFor::sound_rename;
        const char *title = entry_for_ == EntryFor::sound_save      ? "SAVE SOUND"
                            : entry_for_ == EntryFor::sound_replace ? "REPLACE SOUND"
                            : entry_for_ == EntryFor::sound_rename  ? "RENAME SOUND"
                            : rename                                ? "RENAME"
                                                                    : "SAVE AS";
        paint::small_text(canvas.data(), 0, 19, title, 15);
        if (sound) {
            // Which machine's list it goes in, and where it came from.
            char line[40];
            char *out = line;
            append(out, "T");
            append_number(out, uint32_t(selected_) + 1);
            append(out, " ");
            append(out, machine_name(sound_entry_machine_));
            if (entry_for_ != EntryFor::sound_save) {
                append(out, " ");
                append_shown(out, storage_.sound_bank().slots[sound_entry_slot_ < 0 ? 0 : sound_entry_slot_].name,
                             kUserSoundNameChars);
            }
            *out = 0;
            canvas.text(0, 30, line, 9);
        } else if (rename) {
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
            append(out, delete_oks_ > 0 ? "OK AGAIN: DELETE" : "OK TWICE: DELETE");
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
