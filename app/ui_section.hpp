// PROJECT's CHAIN page, one entry as a song section: the SECTION view (push A on CHAIN,
// docs/CONTROLS.md "SECTION").
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has.
//
// The view edits the entry under CHAIN's cursor: two pages of four fields, SECT (PAT REP MUT
// SCN) and MOVE (MRP NRG BPM, D free), on the screen keys. A knob turns its field, a push puts it
// back to `--`, UP/DOWN walk the entries, OK's tap or CLR go back to the list and a held OK fills
// the fields from what plays. Every edit is a command the audio side applies to the authoritative
// record (Cmd::chain_edit, ChainEdit::section_delta and friends), a delta and never a value read
// back from here, so fast clicks are not lost to a record a block stale.

    bool section_view() const {
        return section_open_ && mode_ == Mode::project && project_page_ == ProjectPage::chain;
    }

    // The field a knob edits on a page: PAT and REP are the entry's own, the rest SectionFields.
    // -1 PAT, -2 REP, -3 none (MOVE's D).
    static int section_knob_field(int page, int knob) {
        static const int kFields[2][4] = {
            {-1, -2, int(SectionField::mut), int(SectionField::scn)},
            {int(SectionField::mrp), int(SectionField::nrg), int(SectionField::bpm), -3},
        };
        return kFields[page & 1][knob & 3];
    }

    static const char *section_field_name(int field) {
        switch (field) {
            case -1: return "PAT";
            case -2: return "REP";
            case int(SectionField::mut): return "MUT";
            case int(SectionField::scn): return "SCN";
            case int(SectionField::mrp): return "MRP";
            case int(SectionField::nrg): return "NRG";
            case int(SectionField::bpm): return "BPM";
            default: return "---";
        }
    }

    // The field's value as the view and the notices write it: "A05", "X2", "M3", "CAP", "4",
    // "SNAP", "+32", "132.5", and "--" for a field left alone.
    void section_field_text(int field, const ChainEntry &entry, const SectionExtra &e,
                            char *out) const {
        switch (field) {
            case -1:
                *out++ = bank_letter(int(entry.bank));
                append_padded(out, uint32_t(entry.slot) + 1, 2);
                break;
            case -2:
                *out++ = 'X';
                append_number(out, uint32_t(entry.repeats));
                break;
            case int(SectionField::mut):
                if (e.cap != 0) {
                    append(out, "CAP");
                } else if (entry.mutes != 0) {
                    *out++ = 'M';
                    append_number(out, uint32_t(entry.mutes));
                } else {
                    append(out, "--");
                }
                break;
            case int(SectionField::scn):
                if (e.scene == 0) append(out, "--");
                else append_number(out, uint32_t(e.scene));
                break;
            case int(SectionField::mrp): {
                static const char *const kMoves[] = {"--", "SNAP", "RIDE", "BACK", "SNAP A"};
                append(out, kMoves[e.morph <= kSectionMorphMax ? e.morph : 0]);
                break;
            }
            case int(SectionField::nrg):
                if (e.energy == 0) {
                    append(out, "--");
                } else {
                    append_signed(out, section_energy(e.energy));
                }
                break;
            case int(SectionField::bpm):
                if (e.bpm == 0) {
                    append(out, "--");
                } else {
                    append_number(out, uint32_t(e.bpm) / 100);
                    *out++ = '.';
                    append_number(out, uint32_t(e.bpm) / 10 % 10);
                }
                break;
            default:
                append(out, "---");
                break;
        }
        *out = 0;
    }

    void section_open() {
        if (chain_entries() == 0) {
            notice("CHAIN EMPTY");
            return;
        }
        chain_clamp_cursor();
        section_open_ = true;
        section_page_ = 0;
        section_ok_held_ = false;
        section_say_entry();
    }

    void section_close() {
        section_open_ = false;
        section_ok_held_ = false;
        notice_logged("CHAIN");
    }

    // "SECT 3/8": which entry the view is on, logged for the emulator, which cannot read it.
    void section_say_entry() {
        char *out = notice_buffer_;
        append(out, "SECT ");
        append_number(out, uint32_t(chain_cursor_) + 1);
        *out++ = '/';
        append_number(out, uint32_t(chain_entries()));
        *out = 0;
        notice_logged(notice_buffer_);
    }

    void section_post(ChainEdit edit, int field, int delta = 0) {
        chain_clamp_cursor();
        post_chain(edit, chain_cursor_, uint8_t(field), uint8_t(int8_t(clamp_delta(delta))));
    }

    // A knob turned in the view.
    void section_turn(int knob, int delta) {
        if (chain_entries() == 0) return;
        const int field = section_knob_field(section_page_, knob);
        if (field == -3) return;                  // MOVE's D: free (docs/CONTROLS.md)
        if (field == -1) {
            chain_delta(ChainEdit::pattern_delta, delta);
            return;
        }
        if (field == -2) {
            chain_delta(ChainEdit::repeats_delta, delta);
            return;
        }
        section_post(ChainEdit::section_delta, field, delta);
        notice(section_field_name(field));
    }

    // A knob pushed in the view: its field back to `--`.
    void section_push(int knob) {
        if (chain_entries() == 0) return;
        const int field = section_knob_field(section_page_, knob);
        if (field == -3) return;
        if (field < 0) {
            notice_logged(field == -1 ? "PAT HAS NO --" : "REP HAS NO --");
            return;
        }
        section_post(ChainEdit::section_clear, field);
        char *out = notice_buffer_;
        append(out, section_field_name(field));
        append(out, " --");
        *out = 0;
        notice_logged(notice_buffer_);
    }

    // True when the key was the view's. FUNC chords (FUNC + OK redo, FUNC + push A-C the kill
    // EQ), LEFT/RIGHT, the STEP keys, VALUE and the bank keys fall through to CHAIN's own, which
    // mean the same here.
    bool on_section_key(int key, bool down) {
        if (!section_view()) {
            section_ok_held_ = false;
            return false;
        }
        if (chain_entries() == 0) {               // the last entry removed under the view
            section_open_ = false;
            return false;
        }
        if (shift_) return false;
        switch (key) {
            case ENAGA_KEY_OK:
                // A tap goes back on its release; a hold fills (service_section), and then its
                // release does nothing.
                if (down) {
                    section_ok_held_ = true;
                    section_ok_down_ms_ = last_ms_;
                } else if (section_ok_held_) {
                    section_ok_held_ = false;
                    section_close();
                }
                return true;
            case ENAGA_KEY_CLR:
                if (down) section_close();
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (down) {
                    chain_cursor_ += key == ENAGA_KEY_UP ? -1 : 1;
                    chain_clamp_cursor();
                    section_say_entry();
                }
                return true;
            case ENAGA_KEY_SELECT_LEFT:
            case ENAGA_KEY_SELECT_RIGHT:
                if (down) {
                    section_page_ ^= 1;
                    notice_logged(section_page_ == 0 ? "SECT PAGE" : "MOVE PAGE");
                }
                return true;
            case ENAGA_KEY_PUSH_A:
            case ENAGA_KEY_PUSH_B:
            case ENAGA_KEY_PUSH_C:
            case ENAGA_KEY_PUSH_D:
                // The pushes are numbered B, A, C, D in the ABI (docs/ENAGA_API.md).
                if (down) {
                    section_push(key == ENAGA_KEY_PUSH_A   ? 0
                                 : key == ENAGA_KEY_PUSH_B ? 1
                                 : key == ENAGA_KEY_PUSH_C ? 2
                                                           : 3);
                }
                return true;
            default:
                return false;
        }
    }

    // update(): the held OK's quick fill.
    void service_section() {
        if (section_ok_held_ && !section_view()) section_ok_held_ = false;
        if (section_ok_held_ && uint32_t(last_ms_ - section_ok_down_ms_) >= kSlotMenuHoldMs) {
            section_ok_held_ = false;
            if (chain_entries() == 0) return;
            chain_clamp_cursor();
            post_chain(ChainEdit::fill, chain_cursor_);
            notice_logged("SECTION FILLED");
        }
    }

    // The view (docs/SCREENS.md, SECTION): the entry and the page at 18, the page tabs at 26,
    // four fields from 32 (a letter, a name, a value), the entry's mute mask at 80 when MUT is
    // a recall or CAP, the keys at 105 and 113.
    void draw_section(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        char line[40];
        char *out = line;
        const Project &project = engine_.project();
        const int length = chain_length(project);
        const int at = chain_cursor_ < length ? chain_cursor_ : (length > 0 ? length - 1 : 0);
        const ChainEntry &entry = project.chain[at];
        const SectionExtra &e = project.chain_extra[at];
        const ChainPlayer &player = engine_.chain();
        const bool now = player.active && !player.orphaned && int(player.entry) == at;

        append(out, "SECT ");
        append_number(out, uint32_t(at) + 1);
        *out++ = '/';
        append_number(out, uint32_t(length));
        *out = 0;
        if (now) {
            paint::small_badge(px, 0, 18, line);
        } else {
            paint::small_text(px, 0, 18, line, 13);
        }
        const char *page = section_page_ == 0 ? "SECT" : "MOVE";
        paint::small_text(px, 125 - paint::small_width(page), 18, page, 10);
        for (int p = 0; p < 2; p++) canvas.rect(p * 64 + 2, 26, 60, 2, p == section_page_ ? 14 : 4);

        for (int knob = 0; knob < 4; knob++) {
            const int field = section_knob_field(section_page_, knob);
            const int y = 32 + knob * 11;
            const char letter[2] = {char('A' + knob), 0};
            paint::small_text(px, 0, y + 1, letter, 6);
            if (field == -3) {
                canvas.text(8, y, "---", 4);
                continue;
            }
            canvas.text(8, y, section_field_name(field), 11);
            char text[12];
            section_field_text(field, entry, e, text);
            const bool left_alone = text[0] == '-';
            canvas.text(44, y, text, uint8_t(left_alone ? 6 : 15));
        }

        // The mutes the entry sets, as CHAIN draws them: a frame muted, a block playing.
        if (e.cap != 0 || entry.mutes != 0) {
            const uint8_t mask = e.cap != 0 ? e.mask : project.settings.mute_states[entry.mutes - 1];
            paint::small_text(px, 0, 82, e.cap != 0 ? "CAP" : "MUTES", 6);
            for (int t = 0; t < kTracks; t++) {
                const int bx = 24 + t * 9;
                if ((mask >> t) & 1u) {
                    frame_rect(canvas, bx, 81, 7, 7, 6);
                } else {
                    canvas.rect(bx, 81, 7, 7, 12);
                }
            }
        }

        paint::small_text(px, 0, 105, "HOLD OK FILL CLR BACK", 10);
        paint::small_text(px, 0, 113, "PUSH A-D: SET TO --", 10);
    }

    // The CHAIN list's summary of the cursor's section fields, "S4 SNAP E+32 132", or nothing
    // when every field is `--`.
    static void section_summary(const SectionExtra &e, char *out) {
        bool any = false;
        const auto gap = [&out, &any]() {
            if (any) *out++ = ' ';
            any = true;
        };
        if (e.scene != 0) {
            gap();
            *out++ = 'S';
            append_number(out, uint32_t(e.scene));
        }
        if (e.morph != kSectionMorphNone && e.morph <= kSectionMorphMax) {
            static const char *const kMoves[] = {"", "SNAP", "RIDE", "BACK", "SNAP A"};
            gap();
            append(out, kMoves[e.morph]);
        }
        if (e.energy != 0) {
            gap();
            *out++ = 'E';
            append_signed(out, section_energy(e.energy));
        }
        if (e.bpm != 0) {
            gap();
            append_number(out, uint32_t(e.bpm) / 100);
            append(out, "BPM");
        }
        *out = 0;
    }
