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

    // The chorus sends (KitTrack::chorus) on CHO 1-4 and CHO 5-8: an encoder is a track's send,
    // one click two counts as a send knob on EDIT's FX page is, and said as "T3 CHORUS 64".
    void edit_chorus_send(int page, int slot, int delta) {
        if (!mixer_chorus_send_page(page) || slot < 0 || slot >= 4) return;
        const int track = (page - kMixerChorusSendPage) * 4 + slot;
        post_delta(Cmd::set_chorus, track, 0, 0, delta * 2);
        const int now = int(engine_.project().kit.tracks[track].chorus) + delta * 2;
        chorus_send_notice(track, now < 0 ? 0 : (now > int(kChorusSendMax) ? int(kChorusSendMax) : now));
    }

    void toggle_chorus_send(int track) {
        if (track < 0 || track >= kTracks) return;
        const bool off = engine_.project().kit.tracks[track].chorus == 0;
        post(Cmd::set_chorus, uint8_t(track), 0, 0, 0, 1);
        chorus_send_notice(track, off ? int(kChorusSendToggle) : 0);
    }

    // "T3 CHORUS 64", or "T3 CHORUS OFF". Logged too: the emulator cannot read the screen
    // (tests/scenario_chorus.sh).
    void chorus_send_notice(int track, int value) {
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(track + 1));
        append(out, " CHORUS ");
        if (value == 0) append(out, "OFF");
        else append_number(out, uint32_t(value));
        *out = 0;
        notice(notice_buffer_);
        enaga::log(notice_buffer_);
    }

    // The widths (KitTrack::width) on WID 1-4 and WID 5-8: an encoder is a track's WID, one count
    // a click so the zone edge at 64 can be found, said as "T7 WIDTH 40" or "T7 UNISON 96".
    void edit_width(int page, int slot, int delta) {
        if (!mixer_width_page(page) || slot < 0 || slot >= 4) return;
        const int track = (page - kMixerWidthPage) * 4 + slot;
        post_delta(Cmd::set_width, track, 0, 0, delta);
        const int now = int(engine_.project().kit.tracks[track].width) + delta;
        width_notice(track, now < 0 ? 0 : (now > int(kWidthMax) ? int(kWidthMax) : now));
    }

    void step_width(int track) {
        if (track < 0 || track >= kTracks) return;
        const uint8_t now = engine_.project().kit.tracks[track].width;
        post(Cmd::set_width, uint8_t(track), 0, 0, 0, 1);
        width_notice(track, now < kWidthSpreadStep ? kWidthSpreadStep
                                                   : (now < kWidthUnisonStep ? kWidthUnisonStep : 0));
    }

    // "T7 WIDTH 40", "T7 UNISON 96" or "T7 WIDTH OFF"; "T1 WIDTH: POLY ONLY" on a track whose
    // machine does not read it (the byte is kept for when it does). Logged too: the emulator
    // cannot read the screen (tests/scenario_width.sh).
    void width_notice(int track, int value) {
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(track + 1));
        if (!machine_is_poly(engine_.machine_id(track))) {
            append(out, " WIDTH: POLY ONLY");
        } else if (value == 0) {
            append(out, " WIDTH OFF");
        } else {
            append(out, value >= int(kWidthUnison) ? " UNISON " : " WIDTH ");
            append_number(out, uint32_t(value));
        }
        *out = 0;
        notice(notice_buffer_);
        enaga::log(notice_buffer_);
    }

    // --- the line input's strip (IN and IN+) -------------------------------------------------

    uint8_t input_byte(InputField field) const {
        return engine_.project().settings.input[int(field)];
    }

    // One encoder on IN or IN+: a DELTA, for the reason edit_mixer_field gives. Two counts a
    // click on the knobs, as a send knob; one on the duck (a percent) and the jack (a choice).
    void edit_input(int page, int slot, int delta) {
        if (!mixer_input_page(page) || slot < 0 || slot >= 4) return;
        const InputField field = kInputPageFields[page - kMixerInputPage][slot];
        const int click = field == InputField::duck || field == InputField::source ? 1 : 2;
        post_delta(Cmd::set_input, 0, 0, uint8_t(field), delta * click);
        if (field == InputField::source) {
            const int now = int(input_byte(field)) + delta;
            input_source_notice(now < 0 ? 0 : (now >= int(kInputSources) ? int(kInputSources) - 1 : now));
            return;
        }
        static const char *const names[kInputFields] = {"IN", "IN LEVEL", "IN PAN", "IN DELAY",
                                                        "IN REVERB", "IN CHORUS", "IN DUCK",
                                                        "IN TONE", "IN SOURCE", "IN GAIN"};
        notice(names[int(field)]);
    }

    // Push A on IN or IN+: the input on or off, said as what it will be. Logged: the emulator
    // cannot read the screen (tests/scenario_linein.sh).
    void toggle_input() {
        const bool on = input_byte(InputField::on) != 0;
        post(Cmd::set_input, 0, 0, uint8_t(InputField::on), 0, 1);
        notice_logged(on ? "IN OFF" : "IN ON");
    }

    // Push D on IN+: the jack's front-end level one on, LOW NORM HIGH, round.
    void step_input_gain() {
        const uint8_t now = input_byte(InputField::gain);
        const uint8_t next = uint8_t(now + 1 < kInputGains ? now + 1 : 0);
        post(Cmd::set_input, 0, 0, uint8_t(InputField::gain), 0, 1);
        char *out = notice_buffer_;
        append(out, "IN GAIN ");
        append(out, kInputLevelNames[next]);
        *out = 0;
        notice_logged(notice_buffer_);
    }

    void input_source_notice(int source) {
        char *out = notice_buffer_;
        append(out, "IN ");
        append(out, kInputRoutes[source].name);
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // The input's route on the codec, as the strip says, whenever the strip is on and the route
    // it asks for is not the one last applied (a turn of SRC or a push of GAIN, a project opened
    // with the input on, the app started). Main context only: enaga::system::input talks to the
    // codec and leaves the input quiet for about 10 ms. RESAMPLE's own input sources route it
    // while that screen shows one (apply_input), and leaving RESAMPLE gives it back to the strip.
    void service_input_route() {
        if (input_byte(InputField::on) == 0 || mode_ == Mode::resample) return;
        const int source = int(input_byte(InputField::source));
        const int gain = int(input_byte(InputField::gain));
        const int want = source * int(kInputGains) + gain;
        if (want == input_route_applied_) return;
        input_route_applied_ = want;
        const int result = enaga::system::input(kInputRoutes[source < int(kInputSources) ? source : 0].route,
                                                gain < int(kInputGains) ? gain : int(kInputGainNormal));
        if (result != 0) notice_logged("INPUT REFUSED");
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

    // A track's choke group, one step on: OFF, 1-4, then OFF again. A STEP, not the next group:
    // the group lives in the kit on the audio side, so a UI that computed its own would read the
    // same stale byte for two fast pushes. The notice says what one step from the kit makes.
    void step_choke(int track) {
        if (track < 0 || track >= kTracks) return;
        const uint8_t now = engine_.project().kit.tracks[track].choke;
        const uint8_t next = uint8_t(now >= kChokeGroups ? 0 : now + 1);
        post(Cmd::set_choke, uint8_t(track), 0, 0, 0, 1);
        char *out = notice_buffer_;
        append(out, "T");
        append_number(out, uint32_t(track + 1));
        if (next == 0) {
            append(out, " CHOKE OFF");
        } else {
            append(out, " CHOKE GROUP ");
            append_number(out, uint32_t(next));
        }
        *out = 0;
        notice(notice_buffer_);
        enaga::log(notice_buffer_);
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
        if (!kMixerPages[page].master && field == int8_t(SendField::chorus_rate)) {
            // The sweep in hertz, two decimals: 0.05 to 5.00.
            const uint32_t centi = uint32_t(send_value_from_byte(uint8_t(field), byte, 0.5f) * 100.0f + 0.5f);
            append_number(out, centi / 100u);
            append(out, ".");
            append_padded(out, centi % 100u, 2);
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

    // MIXER's screen is app/ui_mixview.hpp.

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
        uint8_t top = 127;       // its highest value: 127, or WAV's and DST's last number
    };

    ShownParam shown_param(int index) const {
        ShownParam shown;
        shown.base = engine_.params(selected_).v[index];
        shown.heard = shown.base;
        shown.top = kit_param_max(index);
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
        const int top = shown.top > 0 ? int(shown.top) : 127;
        const int bar = int(shown.heard) * width / top;
        canvas.rect(x, y, bar < 1 ? 1 : bar, height, grey);
        // A random range: a dim line under the bar over the values the step can land on.
        if (shown.range != 0) {
            const int low = int(shown.heard) - int(shown.range);
            const int high = int(shown.heard) + int(shown.range);
            const int from = x + (low < 0 ? 0 : low) * (width - 1) / top;
            const int to = x + (high > top ? top : high) * (width - 1) / top;
            canvas.rect(from, y + height, to - from + 1, 1, 7);
        }
        if (!shown.locked || shown.heard == shown.base) return;
        const int tick = x + int(shown.base) * (width - 1) / top;
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
    bool draw_lock_readout(enaga::Canvas &canvas, int y, int grey = 15) const {
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
        canvas.text(0, y, line, uint8_t(grey));
        return true;
    }
