// MIXER (levels, sends, master, the kill EQ), the pieces the screens share, and what a
// parameter is.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- MIXER: four levels at a time, with meters -----------------------

    // --- MIXER's send and master pages ------------------------------------

    // The byte a slot is currently showing, straight out of the project, which is where the
    // fourteen of them live (core/project.hpp). There is no app-side copy to drift.
    uint8_t mixer_byte(int page, int slot) const {
        const int8_t field = kMixerPages[page].field[slot];
        if (field < 0) return 0;
        const ProjectSettings &settings = engine_.project().settings;
        return kMixerPages[page].master ? settings.master[field] : settings.sends[field];
    }

    void edit_mixer_field(int page, int slot, int delta) {
        if (page < 0 || page >= kMixerPageCount || slot < 0 || slot >= 4) return;
        const int8_t field = kMixerPages[page].field[slot];
        if (field < 0) return;                       // an empty slot on the routing page
        const bool master = kMixerPages[page].master;

        // A DELTA, not the next byte: the byte lives in the project on the audio side, so a UI
        // that computed its own would read the same stale value for every click in a sweep and
        // only the last one would land. dsp/buses.cpp says what one click is worth here -- two
        // counts for a knob, one division for the delay's time, a sign for the switch.
        const int scaled = delta * (master ? kMasterFieldClick : send_field_click(uint8_t(field)));
        post_delta(master ? Cmd::set_master : Cmd::set_sends, 0, 0, uint8_t(field), scaled);
        notice(master ? master_field_name(uint8_t(field)) : send_field_name(uint8_t(field)));
    }

    // The duck's pages: on DCK 1-4 and DCK 5-8 an encoder is a track's depth, on DUCK the four
    // are the source, attack, hold and release. One click is one step of each: a percent, a
    // track, a millisecond, five milliseconds.
    void edit_duck(int page, int slot, int delta) {
        if (page < kMixerDuckDepthPage || page > kMixerDuckPage || slot < 0 || slot >= 4) return;
        if (page < kMixerDuckPage) {
            const int track = (page - kMixerDuckDepthPage) * 4 + slot;
            post_delta(Cmd::set_duck, track, 0, uint8_t(DuckField::depth), delta);
            char *out = notice_buffer_;
            append(out, "T");
            append_number(out, uint32_t(track + 1));
            append(out, track == int(engine_.project().kit.duck.source) ? " IS THE SOURCE"
                                                                        : " DUCK DEPTH");
            *out = 0;
            notice(notice_buffer_);
            return;
        }
        static const DuckField fields[4] = {DuckField::source, DuckField::attack, DuckField::hold,
                                            DuckField::release};
        static const char *const names[4] = {"DUCK SOURCE", "DUCK ATTACK", "DUCK HOLD",
                                             "DUCK RELEASE"};
        post_delta(Cmd::set_duck, 0, 0, uint8_t(fields[slot]), delta);
        notice(names[slot]);
    }

    void draw_duck(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Kit &kit = engine_.project().kit;
        const int source = kit.duck.source < kTracks ? int(kit.duck.source) : 0;

        append(out, "MIXER ");
        append(out, kMixerPages[mixer_bank_].name);
        *out = 0;
        canvas.text(0, 20, line, 14);

        if (mixer_bank_ < kMixerDuckPage) {
            for (int slot = 0; slot < 4; slot++) {
                const int track = (mixer_bank_ - kMixerDuckDepthPage) * 4 + slot;
                const int x = slot * 32;
                const uint8_t depth = kit.tracks[track].duck;
                out = line;
                append(out, "T");
                append_number(out, uint32_t(track + 1));
                *out = 0;
                canvas.text(x, 46, line, 13);

                // The source never ducks under itself, whatever its own depth says.
                out = line;
                if (track == source) {
                    append(out, "SRC");
                } else if (depth == 0) {
                    append(out, "OFF");
                } else {
                    append_number(out, depth);
                    append(out, "%");
                }
                *out = 0;
                canvas.text(x, 60, line, track == source ? 10 : 15);

                const int bar = track == source ? 0 : int(depth) * 28 / int(kDuckDepthMax);
                canvas.rect(x + 2, 76, 28, 6, 3);
                if (bar > 0) canvas.rect(x + 2, 76, bar, 6, 12);
            }
            out = line;
            append(out, "DUCKS UNDER T");
            append_number(out, uint32_t(source + 1));
            *out = 0;
            canvas.text(0, 98, line, 10);
        } else {
            static const char *const names[4] = {"SRC", "ATK", "HLD", "REL"};
            const uint32_t values[4] = {
                uint32_t(source + 1), kit.duck.attack_ms,
                uint32_t(kit.duck.hold) * uint32_t(kDuckTimeStepMs),
                uint32_t(kit.duck.release) * uint32_t(kDuckTimeStepMs)};
            const int bars[4] = {source * 28 / (kTracks - 1),
                                 int(kit.duck.attack_ms) * 28 / int(kDuckAttackMax),
                                 int(kit.duck.hold) * 28 / int(kDuckHoldMax),
                                 int(kit.duck.release) * 28 / int(kDuckReleaseMax)};
            for (int slot = 0; slot < 4; slot++) {
                const int x = slot * 32;
                canvas.text(x, 46, names[slot], 13);
                out = line;
                if (slot == 0) append(out, "T");
                append_number(out, values[slot]);
                *out = 0;
                canvas.text(x, 60, line, 15);
                canvas.rect(x + 2, 76, 28, 6, 3);
                if (bars[slot] > 0) canvas.rect(x + 2, 76, bars[slot], 6, 12);
            }
            canvas.text(0, 98, "TIMES IN MS", 10);
        }

        draw_master_peak(canvas);
    }

    // Throw one kill switch. The notice says what it will be once the audio side has applied it,
    // which is the opposite of what the project says now.
    void toggle_kill(int band) {
        if (band < 0 || band >= kKillBands) return;
        post(Cmd::eq_kill, 0, 0, uint8_t(band), 2);
        const bool was = (engine_.project().settings.eq_kills >> band & 1u) != 0;
        static const char *const killed[kKillBands] = {"KILL LOW", "KILL MID", "KILL HI"};
        static const char *const back[kKillBands] = {"LOW BACK", "MID BACK", "HI BACK"};
        notice(was ? back[band] : killed[band]);
        // Logged as well: a kill is a performance move whose effect the emulator cannot hear,
        // so tests/scenario_killeq.sh reads it here.
        enaga::log(was ? back[band] : killed[band]);
    }

    // What a slot's byte MEANS, as four characters at most: a division, a switch, or the byte.
    void append_mixer_value(char *&out, int page, int slot) {
        const int8_t field = kMixerPages[page].field[slot];
        const uint8_t byte = mixer_byte(page, slot);
        if (!kMixerPages[page].master && field == int8_t(SendField::delay_seconds)) {
            append(out, delay_division_name(byte));
            return;
        }
        if (!kMixerPages[page].master && field == int8_t(SendField::delay_ping_pong)) {
            append(out, byte >= 64 ? "ON" : "OFF");
            return;
        }
        if (!kMixerPages[page].master && field == int8_t(SendField::delay_mode)) {
            append(out, byte >= 64 ? "LONG" : "CLAS");
            return;
        }
        if (!kMixerPages[page].master && field == int8_t(SendField::reverb_mode)) {
            append(out, byte >= 64 ? "HALL" : "ROOM");
            return;
        }
        if (!kMixerPages[page].master && field == int8_t(SendField::delay_multiplier)) {
            append(out, "X");
            append_number(out, uint32_t(delay_multiplier_value(byte)));
            return;
        }
        if (kMixerPages[page].master && field == int8_t(MasterField::dj)) {
            // Signed, and the one field whose centre means "off". Shown as the number the knob
            // is at so that finding the flat centre again is possible.
            const int signed_value = int(byte) - 64;
            if (signed_value == 0) {
                append(out, "OFF");
                return;
            }
            if (signed_value < 0) {
                append(out, "LP");
                append_number(out, uint32_t(-signed_value));
            } else {
                append(out, "HP");
                append_number(out, uint32_t(signed_value));
            }
            return;
        }
        if (kMixerPages[page].master && field >= int8_t(MasterField::eq_low) &&
            field <= int8_t(MasterField::eq_high)) {
            // Centred like the DJ filter: 0 is flat, and a switched band or a knob at the
            // bottom is a KILL.
            const int band = field - int8_t(MasterField::eq_low);
            const bool switched = (engine_.project().settings.eq_kills >> band & 1u) != 0;
            const int signed_value = int(byte) - 64;
            if (switched || byte == 0) {
                append(out, "KILL");
            } else if (signed_value == 0) {
                append(out, "0");
            } else {
                append(out, signed_value < 0 ? "-" : "+");
                append_number(out, uint32_t(signed_value < 0 ? -signed_value : signed_value));
            }
            return;
        }
        append_number(out, byte);
    }

    void draw_mixer_fields(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        // MST and EQ are the mockup's master page (app/ui_live.hpp).
        if (kMixerPages[mixer_bank_].master) {
            draw_master_page(canvas);
            return;
        }

        append(out, "MIXER ");
        append(out, kMixerPages[mixer_bank_].name);
        *out = 0;
        canvas.text(0, 20, line, 14);

        for (int slot = 0; slot < 4; slot++) {
            const int8_t field = kMixerPages[mixer_bank_].field[slot];
            const int x = slot * 32;
            if (field < 0) {
                canvas.text(x, 46, "---", 4);
                continue;
            }
            out = line;
            append(out, kMixerPages[mixer_bank_].master ? master_field_name(uint8_t(field))
                                                        : send_field_name(uint8_t(field)));
            *out = 0;
            canvas.text(x, 46, line, 13);

            out = line;
            append_mixer_value(out, mixer_bank_, slot);
            *out = 0;
            canvas.text(x, 60, line, 15);

            // A bar, so a page of numbers still reads at a glance.
            const int bar = int(mixer_byte(mixer_bank_, slot)) * 28 / 127;
            canvas.rect(x + 2, 76, 28, 6, 3);
            if (bar > 0) canvas.rect(x + 2, 76, bar, 6, 12);
        }

        // MST and EQ: what the master is putting out, while the beat plays (app/ui_live.hpp).
        if (kMixerPages[mixer_bank_].master) {
            draw_master_live(canvas, enaga::clock::state().running != 0);
        }

        // What the delay is actually doing, in milliseconds, which is the one thing a division
        // does not tell you -- and the number that says whether the tempo-sync is working.
        // On the LONG page too: there it is the division times the multiplier, up to 12 s.
        if (mixer_bank_ == kMixerDelayPage || mixer_bank_ == kMixerLongPage) {
            out = line;
            append(out, "TIME ");
            append_number(out,
                          uint32_t(effective_delay_seconds(engine_.sends()) * 1000.0f + 0.5f));
            append(out, " MS");
            *out = 0;
            canvas.text(0, 98, line, 10);
        }

        draw_master_peak(canvas);
    }

    void draw_master_peak(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const int peak_milli =
            int(engine_.status().peak_milli.load(std::memory_order_relaxed));
        append(out, "PEAK ");
        append_tenths(out, uint32_t(peak_milli));
        *out = 0;
        canvas.text(0, 110, line, 11);
        const int peak_bar = peak_milli * 60 / 1000;
        canvas.rect(60, 112, peak_bar > 60 ? 60 : (peak_bar < 1 ? 1 : peak_bar), 4,
                    peak_milli >= 1000 ? 15 : 8);
    }

    void draw_mixer(enaga::Canvas &canvas) {
        if (mixer_bank_ >= kMixerDelayPage) {
            draw_mixer_fields(canvas);
            return;
        }
        if (mixer_bank_ >= kMixerDuckDepthPage) {
            draw_duck(canvas);
            return;
        }
        char line[40];
        char *out = line;
        const bool running = enaga::clock::state().running != 0;

        out = line;
        append(out, "MIXER TRK ");
        append_number(out, uint32_t(mixer_bank_ * 4 + 1));
        *out++ = '-';
        append_number(out, uint32_t(mixer_bank_ * 4 + 4));
        *out = 0;
        canvas.text(0, 20, line, 14);

        for (int slot = 0; slot < 4; slot++) {
            const int track = mixer_bank_ * 4 + slot;
            if (track >= kTracks) break;

            const int x = slot * 32;
            const float level = engine_.level(track);
            const int bar = int(level * 40.0f);

            out = line;
            append(out, "T");
            append_number(out, uint32_t(track + 1));
            if (muted_[track]) append(out, " M");
            *out = 0;
            canvas.text(x, 34, line, muted_[track] ? 6 : 13);

            // A vertical fader per track, filled to its level.
            canvas.rect(x + 2, 46, 10, 48, 3);
            const int filled = bar > 48 ? 48 : (bar < 0 ? 0 : bar);
            if (filled > 0) {
                canvas.rect(x + 2, 46 + (48 - filled), 10, filled, muted_[track] ? 5 : 12);
            }
            // What the track is playing, beside its fader (app/ui_live.hpp).
            draw_fader_meter(canvas, track, x + 12, running);

            // The machine driving it, so the mixer says what it is mixing.
            const Machine *machine = engine_.machine(track);
            canvas.text(x + 14, 46, machine ? "ON" : "--", machine ? 10 : 4);

            out = line;
            append_padded(out, uint32_t(level * 100.0f + 0.5f), 3);
            *out = 0;
            canvas.text(x, 98, line, 11);
        }

        // The master peak, as one bar: the only number that says whether it is clipping.
        draw_master_peak(canvas);
    }

    // --- shared pieces ----------------------------------------------------

    // The machine names SRC and AMP; the insert chain names FLTR and FX; the LFO its own page.
    const char *param_name_for(int index) const { return track_param_name(index); }

    // The parameter behind encoder `knob` on the page on screen. A VA or WAVE track shows its
    // filter section on FLTR (CUT RES ENV FDE), so its first three pages are a different order
    // over the same slots (core/project.hpp, edit_slot, D8).
    int edit_index(int knob) const {
        return edit_slot(engine_.machine_id(selected_), param_page_, knob);
    }

    // By the parameter's own page rather than the page on screen, so a macro assignment or the
    // LFO's destination can name a parameter from any page.
    const char *track_param_name(int index) const { return track_param_name_on(selected_, index); }

    // The same for any track: a macro's target names its parameter on its own track's machine.
    const char *track_param_name_on(int track, int index) const {
        if (index < 0 || index >= kTrackParams) return "---";
        if (index >= kOscFirst) return osc_param_name(index - kOscFirst);
        if (index >= kAsrFirst) {
            if (!machine_reads_asr_slot(engine_.machine_id(track), index - kAsrFirst)) return "---";
            // FM4's fourth ASR knob holds the index envelope, not a filter's: ISU. GRAIN has no
            // filter envelope, so it has no fourth; SAMPLE reads SUS and REL alone.
            if (index == kAsrFirst + 3 && machine_is_fm(engine_.machine_id(track))) return "ISU";
            if (index == kAsrFirst + 3 && machine_is_grain(engine_.machine_id(track))) return "---";
            return kAsrNames[index - kAsrFirst];
        }
        if (index >= kArpFirst) return arp_param_name(index - kArpFirst);
        if (index >= kParams) return lfo_param_name(index);
        const int page = index / kParamsPerPage;
        // A VA's or WAVE's slots 4-7 are FDE and TON, its own names (D8); the FX page is the
        // chain's everywhere.
        const bool voice_filter = machine_has_voice_filter(engine_.machine_id(track));
        if (page == int(Page::fx) || (page == int(Page::fltr) && !voice_filter)) {
            return InsertChain::param_name(index);
        }
        const Machine *machine = engine_.machine(track);
        return machine ? machine->param_name(index) : "---";
    }

    // A parameter's value as the screen shows it: the byte, except on the LFO's page, where it
    // is a shape, a rate (Hz free, a note length synced), a depth, and a destination. At most
    // five characters.
    //
    // The WAVE pair's own knobs, where they differ from the VA's: TBL by its table's name, MOT
    // signed (frames at the envelope's top), WRP a plain value and DET "OFF" or cents ("D12").
    // False for every other knob, which reads as on the VAs.
    static bool append_wave_value(char *&out, int index, uint8_t value) {
        switch (index) {
            case 0:
                append(out, WaveLibrary::short_name(va_wave_table(value)));
                return true;
            case 10: {
                const float frames = va_wave_motion(value);
                const int whole = frames >= 0.0f ? int(frames + 0.5f) : -int(-frames + 0.5f);
                if (whole == 0) {
                    append(out, "OFF");
                } else {
                    *out++ = whole > 0 ? '+' : '-';
                    append_number(out, uint32_t(whole > 0 ? whole : -whole));
                }
                return true;
            }
            case kOscFirst + 0:
                append_padded(out, value, 3);
                return true;
            case kOscFirst + 1:
                if (value == 0) {
                    append(out, "OFF");
                } else {
                    *out++ = 'D';
                    append_number(out, uint32_t(va_wave_detune_cents(value) + 0.5f));
                }
                return true;
            default:
                return false;
        }
    }

    // FM4's own knobs: ALG as A1-A8, RAT and CAR as the ratio they pick, FIN signed around 64,
    // DET "OFF" or cents. False for the rest, which read as plain bytes (dsp/fm.hpp maps them).
    static void append_ratio(char *&out, float ratio) {
        const uint32_t tenths = uint32_t(ratio * 10.0f + 0.5f);
        if (tenths % 10 == 0) {
            append_number(out, tenths / 10);
        } else {
            append_tenths(out, tenths);
        }
    }
    // GRAIN's knobs in their units (dsp/grain.cpp has the curves): POS as a share of the sample,
    // SIZ and SPR in milliseconds, DEN as grains in the air ("X2.5"), SHP as a share, DEC as a
    // time or HOLD, ATK and REL as times, SCN as a share of real time, PSP as "OFF", a detune
    // ("D12", cents either way) or an octave share ("OCT25"). PIT and LVL read as numbers (PIT
    // as a note in NOTE view, above). False for every other knob.
    static bool append_grain_value(char *&out, int index, uint8_t value) {
        switch (index) {
            case 0:
            case 9:
                append_number(out, uint32_t((int(value) * 100 + 63) / 127));
                *out++ = '%';
                return true;
            case 1:
                append_seconds(out, grain_size_seconds(value));
                return true;
            case 2: {
                // Tenths below 10: "X0.3" to "X8.0".
                *out++ = 'X';
                append_tenths(out, uint32_t(grain_overlap(value) * 10.0f + 0.5f));
                return true;
            }
            case 3:
                append_seconds(out, grain_spray_seconds(value));
                return true;
            case 10:
                if (value >= kGrainDecayHold) {
                    append(out, "HOLD");
                } else {
                    append_seconds(out, grain_decay_seconds(value));
                }
                return true;
            case kAsrFirst + 0:
                append_seconds(out, grain_attack_seconds(value));
                return true;
            case kAsrFirst + 2:
                append_seconds(out, grain_release_seconds(value));
                return true;
            case kAsrFirst + 3:
            case kOscFirst + 2:
            case kOscFirst + 3:
                append(out, "---");
                return true;
            case kOscFirst + 0:
                if (value == 0) {
                    append(out, "OFF");
                } else {
                    append_number(out, uint32_t(grain_scan_rate(value) * 100.0f + 0.5f));
                    *out++ = '%';
                }
                return true;
            case kOscFirst + 1:
                switch (grain_pitch_spray_mode(value)) {
                    case GrainSpray::off:
                        append(out, "OFF");
                        break;
                    case GrainSpray::detune:
                        *out++ = 'D';
                        append_number(out, uint32_t(grain_detune_cents(value) + 0.5f));
                        break;
                    case GrainSpray::octave:
                        append(out, "OCT");
                        append_number(out, uint32_t(grain_octave_chance(value) * 100.0f + 0.5f));
                        break;
                }
                return true;
            default:
                return false;
        }
    }

    static bool append_fm_value(char *&out, int index, uint8_t value) {
        switch (index) {
            case 0:
                *out++ = 'A';
                append_number(out, uint32_t(fm_algorithm(value) + 1));
                return true;
            case 1:
                append_ratio(out, fm_ratio(value));
                return true;
            case kOscFirst + 0:
                if (value == 0) {
                    append(out, "OFF");
                } else {
                    *out++ = 'D';
                    append_number(out, uint32_t(fm_detune_cents(value) + 0.5f));
                }
                return true;
            case kOscFirst + 1:
                if (value == 64) {
                    append(out, "0");
                } else {
                    *out++ = value > 64 ? '+' : '-';
                    append_number(out, uint32_t(value > 64 ? value - 64 : 64 - value));
                }
                return true;
            case kOscFirst + 3:
                append_ratio(out, fm_carrier_ratio(value));
                return true;
            default:
                return false;
        }
    }

    void append_param_value(char *&out, int index, uint8_t value) const {
        // TUN or PIT in NOTE view: where it puts C-4 ("G-4", "A-1+"), core/tuning.hpp.
        if (shows_note(index)) {
            char text[kTuneTextMax];
            tune_text(engine_.machine_id(selected_), value, text, kTuneTextMax);
            append(out, text);
            return;
        }
        if (machine_is_wave(engine_.machine_id(selected_)) && append_wave_value(out, index, value)) return;
        if (machine_is_fm(engine_.machine_id(selected_)) && append_fm_value(out, index, value)) return;
        if (machine_is_grain(engine_.machine_id(selected_)) && append_grain_value(out, index, value)) return;
        if (engine_.machine_id(selected_) == uint8_t(MachineId::sample)) {
            // SAMPLE's ASR page: SUS and REL alone, REL on its own curve (dsp/sample.cpp).
            if (index == kAsrFirst + 0 || index == kAsrFirst + 3) {
                append(out, "---");
                return;
            }
            if (index == kAsrFirst + 2) {
                append_seconds(out, sample_release_seconds(value));
                return;
            }
        }
        switch (index) {
            case kLfoWave:
                append(out, lfo_wave_name(lfo_wave(value)));
                return;
            case kLfoRate:
                if (value >= kLfoSyncFirst) {
                    append(out, lfo_sync_name(value));
                } else {
                    // Hundredths below 1 Hz ("0.06"), tenths below 10 ("2.5"), then whole.
                    const uint32_t centi = lfo_free_centi_hz(value);
                    if (centi < 100) {
                        append(out, "0.");
                        append_padded(out, centi, 2);
                    } else if (centi < 1000) {
                        append_tenths(out, (centi + 5) / 10);
                    } else {
                        append_number(out, (centi + 50) / 100);
                    }
                }
                return;
            case kLfoDest: {
                const int dest = lfo_dest(value);
                append(out, dest < 0 ? "OFF" : track_param_name(dest));
                return;
            }
            case kArpFirst + 0:
                append(out, arp_mode_name(value));
                return;
            case kArpFirst + 1:
                append(out, arp_rate_name(value));
                return;
            case kArpFirst + 2:
                *out++ = 'X';
                append_number(out, uint32_t(arp_octaves(value)));
                return;
            case kArpFirst + 3:
                append_number(out, uint32_t(arp_gate_percent(value)));
                *out++ = '%';
                return;
            case kAsrFirst + 0:
                append_seconds(out, va_attack_seconds(value));
                return;
            case kAsrFirst + 2:
                append_seconds(out, va_release_seconds(value));
                return;
            case kAsrFirst + 1:
            case kAsrFirst + 3:
                append_number(out, uint32_t((int(value) * 100 + 63) / 127));
                *out++ = '%';
                return;
            case kOscFirst + 0: {
                // "SAW", "SQR" for a 50% pulse, else the pulse's width: "P30".
                const int width = va_pulse_percent(value);
                if (width == 0) {
                    append(out, "SAW");
                } else if (width == 50) {
                    append(out, "SQR");
                } else {
                    *out++ = 'P';
                    append_number(out, uint32_t(width));
                }
                return;
            }
            case kOscFirst + 1:
                // "OFF", a detune in cents ("D12"), or a sync interval in semitones ("S+7").
                switch (va_osc2_mode(value)) {
                    case VaOsc2::off:
                        append(out, "OFF");
                        break;
                    case VaOsc2::detune:
                        *out++ = 'D';
                        append_number(out, uint32_t(va_osc2_detune_cents(value) + 0.5f));
                        break;
                    case VaOsc2::sync:
                        append(out, "S+");
                        append_number(out, uint32_t(va_osc2_sync_semitones(value) + 0.5f));
                        break;
                }
                return;
            default:
                append_padded(out, value, 3);
                return;
        }
    }

    // Is parameter `index` the selected track's TUN or PIT, shown as a note? NOTE view is one
    // setting for every track (push the knob that turns TUN or PIT, on EDIT, GRID or KEYS), so a track
    // never reads one way and its neighbour the other.
    bool is_tune(int index) const { return index == tune_param(engine_.machine_id(selected_)); }
    bool shows_note(int index) const { return tune_notes_ && is_tune(index); }

    // The four encoder slots along the bottom: a three-letter name, the value under it and a
    // small bar, 18 pixels tall from `y`.
    void draw_param_slots(enaga::Canvas &canvas, int y) {
        char line[40];
        for (int slot = 0; slot < 4; slot++) {
            const int index = edit_index(slot);
            // Name over value: "CUT 064" on one line is 41 pixels and a slot is 32, so each
            // slot ran into the next one (and the fourth off the screen).
            char *out = line;
            append(out, param_name_for(index));
            *out = 0;
            canvas.text(slot * 32, y, line, 12);

            // The value you hear: the held step's lock when it has one, else the kit's.
            const ShownParam shown = shown_param(index);
            out = line;
            append_param_value(out, index, shown.heard);
            *out = 0;
            canvas.text(slot * 32, y + 8, line, 15);

            draw_value_bar(canvas, slot * 32, y + 16, 28, 2, shown, 8);
        }
    }

    // --- what a parameter is, and what plays ------------------------------

    // A parameter as the screen shows it: `base` is the kit value (the knob), `heard` what the
    // held step plays -- its lock if it has one. Without a held step the two are the same. Read
    // from the pattern the grid already reads; one walk of at most eight locks per slot.
    struct ShownParam {
        uint8_t base = 0;
        uint8_t heard = 0;
        bool locked = false;
        uint8_t range = 0;       // the held step's random range on it: 0 none, else the width
        bool slides = false;     // the held step's lock on it slides
    };

    ShownParam shown_param(int index) const {
        ShownParam shown;
        shown.base = engine_.params(selected_).v[index];
        shown.heard = shown.base;
        if (held_step_ >= 0 && index >= 0 && index < kTrackParams) {
            shown.locked = pattern_get_lock(engine_.pattern(), selected_, held_step_,
                                            uint8_t(index), shown.heard);
            shown.range = pattern_get_range(engine_.pattern(), selected_, held_step_,
                                            uint8_t(index));
            shown.slides = shown.locked && pattern_get_slide(engine_.pattern(), selected_,
                                                             held_step_, uint8_t(index));
        }
        return shown;
    }

    // A value bar: solid up to what plays. When a lock moves that away from the kit value, a
    // hollow tick marks the kit value: one bright pixel above and one below the bar, dark
    // between them inside the solid part and dim outside it, so the gap between the two values
    // reads at a glance (docs/CONTROLS.md, "Locks on screen").
    static void draw_value_bar(enaga::Canvas &canvas, int x, int y, int width, int height,
                               const ShownParam &shown, uint8_t grey) {
        const int bar = int(shown.heard) * width / 127;
        canvas.rect(x, y, bar < 1 ? 1 : bar, height, grey);
        // A random range: a dim line under the bar over the values the step can land on.
        if (shown.range != 0) {
            const int low = int(shown.heard) - int(shown.range);
            const int high = int(shown.heard) + int(shown.range);
            const int from = x + (low < 0 ? 0 : low) * (width - 1) / 127;
            const int to = x + (high > 127 ? 127 : high) * (width - 1) / 127;
            canvas.rect(from, y + height, to - from + 1, 1, 7);
        }
        if (!shown.locked || shown.heard == shown.base) return;
        const int tick = x + int(shown.base) * (width - 1) / 127;
        canvas.pixel(tick, y - 1, 15);
        canvas.pixel(tick, y + height, 15);
        canvas.rect(tick, y, 1, height, tick < x + bar ? 0 : 3);
    }

    void show_lock_readout(int index) {
        lock_readout_index_ = index;
        lock_readout_step_ = held_step_;
        lock_readout_track_ = selected_;
    }

    // While a STEP is held and a knob has locked (or cleared) a parameter on it, one line that
    // says what changed: "S5 CUT 64>90", the kit value and then the lock, read live from the
    // pattern so the second number follows the knob. "S5 CUT 64 KIT" once the lock is gone.
    // False when there is nothing to say, and the caller draws its usual line.
    bool draw_lock_readout(enaga::Canvas &canvas, int y) const {
        if (held_step_ < 0 || lock_readout_index_ < 0 || lock_readout_step_ != held_step_ ||
            lock_readout_track_ != selected_) {
            return false;
        }
        const ShownParam shown = shown_param(lock_readout_index_);
        char part[48];
        char *p = part;
        *p++ = 'S';
        append_number(p, uint32_t(held_step_) + 1);
        *p++ = ' ';
        append(p, param_name_for(lock_readout_index_));
        *p++ = ' ';
        append_param_value(p, lock_readout_index_, shown.base);
        if (shown.locked) {
            *p++ = '>';
            append_param_value(p, lock_readout_index_, shown.heard);
        } else if (shown.range == 0) {
            append(p, " KIT");
        }
        // The random range as R and its width, a slide as '/' (the font has no tilde).
        if (shown.range != 0) {
            append(p, " R");
            append_number(p, shown.range);
        }
        if (shown.slides) *p++ = '/';
        *p = 0;
        char line[kLineChars + 1];
        char *out = line;
        append_shown(out, part, kLineChars);
        *out = 0;
        canvas.text(0, y, line, 15);
        return true;
    }
