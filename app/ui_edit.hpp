// EDIT: the selected track's sound four parameters at a time, and its pictures (waveforms and
// envelopes).
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // --- EDIT: the selected track's sound, four parameters at a time -----

    void draw_edit(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;

        // As the mockups draw EDIT: the page is a tab and the track a badge in the frame, so the
        // four knobs take two lines, two to a line (letter, name and value, and the heard value's
        // bar with a lock's tick under it), and the picture gets the rest of the screen.
        const bool lfo_page = param_page_ == int(Page::lfo);
        const bool arp_page = param_page_ == int(Page::arp);
        const bool asr_page = param_page_ == int(Page::asr);
        const bool osc_page = param_page_ == int(Page::osc);
        uint8_t *px = canvas.data();

        for (int slot = 0; slot < 4; slot++) {
            const int index = edit_index(slot);
            const int x = (slot % 2) * 62;
            const int y = kEditRowsY + (slot / 2) * 11;
            const ShownParam shown = shown_param(index);
            const char letter[2] = {char('A' + slot), 0};
            paint::small_text(px, x, y + 1, letter, 7);
            out = line;
            append(out, param_name_for(index));
            *out++ = ' ';
            append_param_value(out, index, shown.heard);   // the held step's lock, if it has one
            *out = 0;
            canvas.text(x + 5, y, line, 15);
            draw_value_bar(canvas, x + 5, y + 8, 54, 1, shown, 9);
            // A MOD slot on this parameter: its range under the bar (app/ui_modslots.hpp).
            draw_mod_marks(canvas, index, x + 5, y + 9, 54, shown.heard);
        }

        // A waveform or an envelope under the knobs (docs/CONTROLS.md, "Waveforms and
        // envelopes"), with one line of text above it at kEditLineY instead of the two at kEditTextY.
        const EditPicture picture = edit_picture();

        // Turning a lock: "S5 CUT 64>90" where the page's own line goes, until the STEP is let go.
        // Over a picture it takes the line above it, and the picture draws the lock it is turning.
        const bool readout = draw_lock_readout(canvas, picture != EditPicture::none ? kEditLineY : kEditTextY);
        if (readout && picture == EditPicture::none) return;

        // A frozen track plays its loop, not these knobs (dsp/frozen.hpp): say so on every page,
        // and where to undo it.
        if (frozen(selected_)) {
            if (!readout) canvas.text(0, kEditTextY, "FROZEN: FUNC+HOME", 15);
            return;
        }

        if (picture != EditPicture::none) {
            if (!readout) draw_picture_line(canvas, picture);
            if (picture == EditPicture::wave) draw_wave(canvas);
            else if (picture == EditPicture::table) draw_table_picture(canvas);
            else if (picture == EditPicture::response) draw_response_picture(canvas);
            else draw_envelope(canvas, envelope_for(picture), picture == EditPicture::amp);
            return;
        }

        if (lfo_page) {
            // What the LFO is doing to its destination right now, so a moving number says it runs;
            // while it runs, over its picture (app/ui_live.hpp) at kEditLineY, as the other pictures'.
            // With a MOD slot set, the two slots take the picture's place, a line each, and the
            // LFO's line goes to 87 whether it runs or not.
            const bool slots = mod_slot_of(selected_, 0).on() || mod_slot_of(selected_, 1).on();
            const uint8_t shown =
                engine_.status().lfo_shown[selected_].load(std::memory_order_relaxed);
            const bool lfo_running = shown != Status::kNotHeard;
            out = line;
            if (lfo_running) {
                const int dest = lfo_dest(engine_.params(selected_).v[kLfoDest]);
                append(out, "NOW ");
                append(out, track_param_name(dest));
                *out++ = ' ';
                append_padded(out, shown, 3);
                // TRIG, when it is not FREE: "NOW CUT 064 ONE-SHOT" is 20 characters.
                const LfoTrig trig = lfo_trig_of(selected_);
                if (trig != LfoTrig::free) {
                    *out++ = ' ';
                    append(out, lfo_trig_name(trig));
                }
            } else {
                append(out, "LFO OFF: SET DEP+DST");
            }
            *out = 0;
            canvas.text(0, lfo_running || slots ? kEditLineY : kEditTextY, line, 7);
            if (slots) {
                for (int s = 0; s < kModSlots; s++) {
                    const KitModSlot &slot = mod_slot_of(selected_, s);
                    out = line;
                    append_mod_slot(out, s, slot.on() ? slot.source : kModSrcOff, slot.dest,
                                    slot.on() ? slot.depth : 0);
                    *out = 0;
                    canvas.text(0, kEditTextY + s * 9, line, 7);
                }
            } else if (lfo_running) {
                draw_lfo_picture(canvas, shown);
            } else {
                canvas.text(0, kEditTextY + 9, "PUSH C D: MOD SLOTS", 7);
            }
            return;
        }

        if (arp_page) {
            // What the arp will do, in a line: "UP 1/16 X2 75%", or how to start one.
            const uint8_t *arp = &engine_.params(selected_).v[kArpFirst];
            out = line;
            if (arp_mode_index(arp[0]) == 0) {
                append(out, "ARP OFF: TURN MOD");
            } else {
                append(out, arp_mode_name(arp[0]));
                *out++ = ' ';
                append(out, arp_rate_name(arp[1]));
                append(out, " X");
                append_number(out, uint32_t(arp_octaves(arp[2])));
                *out++ = ' ';
                append_number(out, uint32_t(arp_gate_percent(arp[3])));
                *out++ = '%';
            }
            *out = 0;
            canvas.text(0, kEditTextY, line, 7);
            return;
        }

        if (asr_page) {
            // A machine that reads ASR draws its envelope (edit_picture); here it does nothing.
            canvas.text(0, kEditTextY, "NOT ON THIS MACHINE", 7);
            return;
        }

        if (osc_page) {
            const uint8_t id = engine_.machine_id(selected_);
            canvas.text(0, kEditTextY,
                        machine_is_fm(id)     ? "FIN NUDGES RAT"
                        : machine_is_wave(id) ? "WRP BENDS  DET 2 OSCS"
                        : machine_is_grain(id) ? "PSP: DET 1-63 OCT 64+"
                        : machine_reads_osc(id) ? "OS2: DET 1-63 SYN 64+"
                                             : "NOT ON THIS MACHINE",
                        7);
            return;
        }

        // The machine is changed somewhere else, and nothing said where.
        canvas.text(0, kEditTextY, "FUNC+EDIT: MACHINE", 7);
        // And a sample track's file is picked from here -- on GRAIN from MACHINE, because here
        // OK opens its SOUNDS.
        if (machine_is_grain(engine_.machine_id(selected_))) {
            canvas.text(0, kEditTextY + 9, "OK: SOUNDS", 7);
        } else if (plays_files(selected_)) {
            canvas.text(0, kEditTextY + 9, "OK: PICK SAMPLE", 7);
        }
    }

    // --- EDIT's pictures: the waveform and the envelopes -------------------
    //
    // docs/CONTROLS.md, "Waveforms and envelopes". Under the four knobs, rows 50-117, x 0-123
    // (the frame's meter has 126-127): SAMPLE's and GRAIN's sample on their SRC page, and the
    // envelope a page's knobs shape on AMP, ASR and a synth's FLTR. Nothing here walks sample
    // memory: the waveform comes from the loader's overview (core/overview.hpp, 250 bytes) and
    // the audio side's WaveView in Status, the envelopes from the knobs alone. One pass of at
    // most kWaveColumns columns a frame.

    enum class EditPicture : uint8_t { none, wave, amp, filter, table, response };

    // EDIT's rows since the live visuals' rebuild (the mockups give the graph the screen): the
    // four knobs two to a line at 18 and 29, a picture's line at 41 and the picture under it,
    // rows 50-117, three times what it was; a page with no picture puts its lines at 50 and 59.
    static constexpr int kEditRowsY = 18;
    static constexpr int kEditLineY = 41;
    static constexpr int kEditTextY = 50;
    static constexpr int kPictureTop = 50;
    static constexpr int kPictureRows = 68;              // rows 50-117: the tabs are at 121

    EditPicture edit_picture() const {
        const uint8_t id = engine_.machine_id(selected_);
        const Page page = Page(param_page_);
        if (page == Page::src && (id == uint8_t(MachineId::sample) || machine_is_grain(id))) {
            return EditPicture::wave;
        }
        if (page == Page::src && machine_is_wave(id)) return EditPicture::table;
        if (page == Page::amp && id != uint8_t(MachineId::midi)) return EditPicture::amp;
        if (page == Page::asr && has_asr(selected_)) return EditPicture::amp;
        if (page == Page::fltr && machine_has_voice_filter(id)) return EditPicture::filter;
        if (page == Page::fltr && id != uint8_t(MachineId::midi)) return EditPicture::response;
        return EditPicture::none;
    }

    // The one line over a picture, at kEditLineY: what used to sit at kEditTextY on these pages,
    // shortened to the part that is not already drawn.
    void draw_picture_line(enaga::Canvas &canvas, EditPicture picture) {
        char line[40];
        char *out = line;
        const uint8_t id = engine_.machine_id(selected_);
        const TrackParams &kit = engine_.params(selected_);
        if (picture == EditPicture::wave && id == uint8_t(MachineId::sample)) {
            // How SAMPLE's SLC cuts, on the page SLC is on.
            if (!slices_auto(selected_)) {
                append(out, "PUSH SLC: AUTO SLICE");
            } else {
                const uint8_t hits =
                    engine_.status().slice_points[selected_].load(std::memory_order_relaxed);
                append(out, "SLC AUTO: ");
                if (hits == 0) {
                    append(out, "NO HITS");
                } else {
                    append_number(out, hits);
                    append(out, hits == 1 ? " HIT" : " HITS");
                }
            }
        } else if (picture == EditPicture::wave) {
            append(out, "OK: SOUNDS");                    // GRAIN: OK here opens its SOUNDS
        } else if (Page(param_page_) == Page::asr) {
            // Whether the note envelope holds, and if not what decides its length.
            const bool sustains = kit.v[kAsrFirst + 1] > 0;
            append(out, sustains          ? "HOLDS: PAD OR LEN"
                        : machine_is_grain(id) || id == uint8_t(MachineId::sample)
                            ? "SUS 0: DEC DECIDES"
                                               : "SUS 0: ONE-SHOT");
        } else if (picture == EditPicture::filter) {
            // FDE at 0 is tied to DEC, which the envelope then draws from.
            append(out, kit.v[kSlotFdec] == 0 ? "FDE 0: 0.55 X DEC" : "FUNC+EDIT: MACHINE");
        } else if (id == uint8_t(MachineId::sample)) {
            append(out, "OK: PICK SAMPLE");
        } else if (machine_is_grain(id)) {
            append(out, "OK: SOUNDS");
        } else {
            append(out, "FUNC+EDIT: MACHINE");
        }
        *out = 0;
        canvas.text(0, kEditLineY, line, 7);
    }

    // SAMPLE's or GRAIN's sample: each column's range as a bar about the middle row, scaled so
    // the loudest column fills the height. SAMPLE adds its START..END window (bright, the rest
    // dim), the window's two ends, a dotted tick at each slice start, and a column for each
    // playing voice; GRAIN the bracket its next grain starts in and a tick for each grain in the
    // air, each as bright as that grain is loud.
    void draw_wave(enaga::Canvas &canvas) {
        const WaveOverview &overview = storage_.overview(selected_);
        const WaveView view = engine_.status().wave[selected_].read();
        const bool grain = machine_is_grain(engine_.machine_id(selected_));
        // GRAIN's sample takes the top 36 rows: the grain cloud has the rest (app/ui_live.hpp).
        const int picture_rows = grain ? kGrainWaveRows : kPictureRows;
        const int half = (picture_rows - 2) / 2;
        const int middle = kPictureTop + 1 + half;
        const int filled = overview.filled > kWaveColumns ? kWaveColumns : int(overview.filled);

        int peak = 8;            // at least this, so near-silence is not blown up into noise
        for (int x = 0; x < filled; x++) {
            const int lo = -int(overview.lo[x]);
            const int hi = int(overview.hi[x]);
            peak = lo > peak ? lo : peak;
            peak = hi > peak ? hi : peak;
        }
        const bool windowed = !grain && view.lo < kWaveColumns && view.hi < kWaveColumns;
        for (int x = 0; x < kWaveColumns; x++) {
            if (x >= filled) {
                // A streamed file past its head: never read at load, so a dotted line.
                if (x % 3 == 0) canvas.pixel(x, middle, 3);
                continue;
            }
            const int up = int(overview.hi[x]) > 0 ? int(overview.hi[x]) * half / peak : 0;
            const int down = int(overview.lo[x]) < 0 ? -int(overview.lo[x]) * half / peak : 0;
            const bool inside = !windowed || (x >= view.lo && x <= view.hi);
            canvas.rect(x, middle - up, 1, up + down + 1, inside ? 9 : 3);
        }

        if (grain) {
            if (view.lo < kWaveColumns && view.hi < kWaveColumns) {
                // The bracket the next grain starts in: corners top and bottom, SIZ wide.
                const int x0 = view.lo;
                const int x1 = view.hi > view.lo ? int(view.hi) : x0 + 1;
                const int top = kPictureTop;
                const int bottom = kPictureTop + picture_rows - 1;
                canvas.rect(x0, top, x1 - x0 + 1, 1, 13);
                canvas.rect(x0, bottom, x1 - x0 + 1, 1, 13);
                canvas.rect(x0, top, 1, 3, 13);
                canvas.rect(x0, bottom - 2, 1, 3, 13);
                canvas.rect(x1, top, 1, 3, 13);
                canvas.rect(x1, bottom - 2, 1, 3, 13);
            }
            draw_grain_sparks(canvas, view, middle);        // app/ui_live.hpp
            for (int i = 0; i < WaveView::kMarks; i++) {
                if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
                canvas.rect(view.mark[i], middle - 6, 1, 13, view.glow[i] > 15 ? 15 : view.glow[i]);
            }
        } else {
            for (int x = 0; x < kWaveColumns; x++) {
                if (!wave_has_cut(view.cuts, x)) continue;
                for (int y = kPictureTop; y < kPictureTop + kPictureRows; y += 2) {
                    canvas.pixel(x, y, 12);
                }
            }
            if (windowed) {
                canvas.rect(view.lo, kPictureTop, 1, kPictureRows, 15);
                canvas.rect(view.hi, kPictureTop, 1, kPictureRows, 15);
            }
            for (int i = 0; i < WaveView::kMarks; i++) {
                if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
                canvas.rect(view.mark[i], kPictureTop, 1, kPictureRows,
                            view.glow[i] > 15 ? 15 : view.glow[i]);
            }
        }

        // No file of its own: it plays the placeholder, and says why.
        if (!storage_.has_overview(selected_)) {
            const char *why = storage_.loading_track() == selected_ ? "LOADING" : "NO FILE";
            const int x = (kWaveColumns - 6 * 7) / 2;
            canvas.rect(x - 2, middle - 4, 6 * 7 + 3, 9, 0);
            canvas.text(x, middle - 3, why, 15);
        }
    }

    // An overview as a plain strip, `rows` tall from `top`: each column's range about the middle,
    // the loudest filling the height. The sample browser's picture of what the track plays.
    static void draw_overview_strip(enaga::Canvas &canvas, const WaveOverview &overview, int top,
                                    int rows) {
        const int half = (rows - 1) / 2;
        const int middle = top + half;
        const int filled = overview.filled > kWaveColumns ? kWaveColumns : int(overview.filled);
        int peak = 8;
        for (int x = 0; x < filled; x++) {
            peak = -int(overview.lo[x]) > peak ? -int(overview.lo[x]) : peak;
            peak = int(overview.hi[x]) > peak ? int(overview.hi[x]) : peak;
        }
        for (int x = 0; x < kWaveColumns; x++) {
            if (x >= filled) {
                if (x % 3 == 0) canvas.pixel(x, middle, 3);
                continue;
            }
            const int up = int(overview.hi[x]) > 0 ? int(overview.hi[x]) * half / peak : 0;
            const int down = int(overview.lo[x]) < 0 ? -int(overview.lo[x]) * half / peak : 0;
            canvas.rect(x, middle - up, 1, up + down + 1, 6);
        }
    }

    // An envelope as the knobs shape it. Widths go by the knob, not the time it makes: every
    // time curve here is exponential in its knob, so this is a log-time drawing, which is what
    // lets 2 ms and 5 s share 124 pixels.
    struct EnvelopePicture {
        int atk = 0;             // knob bytes, 0-127: the segments' widths
        int dec = 0;
        int rel = 0;
        int sus = 0;             // 0-127: the level the decay falls to, when the note holds
        int peak = 127;          // 0-127: the top (ENV, on the filter's)
        bool hold = false;       // the decay does not fall: DEC at its top (SAMPLE, GRAIN)
        bool holds = false;      // a plateau, then a release: the note sustains
        // The parameter behind each handle, -1 for none: its knob's letter is drawn there.
        int atk_param = -1, dec_param = -1, sus_param = -1, rel_param = -1, peak_param = -1;
    };

    uint8_t heard(int index) const { return shown_param(index).heard; }

    EnvelopePicture envelope_for(EditPicture picture) const {
        EnvelopePicture e;
        const uint8_t id = engine_.machine_id(selected_);
        constexpr int kAtk = kAsrFirst, kSus = kAsrFirst + 1, kRel = kAsrFirst + 2;
        constexpr int kFsu = kAsrFirst + 3;
        if (id == uint8_t(MachineId::sample)) {
            // ATK DEC on AMP; DEC at the top holds (dsp/sample.cpp). SUS and REL on ASR, as
            // GRAIN's: SUS up holds the note while gated, then REL.
            e.atk = heard(8);
            e.dec = heard(9);
            e.atk_param = 8;
            e.dec_param = 9;
            e.sus = heard(kSus);
            e.holds = e.sus > 0;
            e.hold = e.dec >= 127 && !e.holds;
            if (e.holds) {
                e.rel = heard(kRel);
                e.sus_param = kSus;
                e.rel_param = kRel;
            }
            return e;
        }
        if (!has_asr(selected_)) {
            // The drums: no attack, DEC to silence.
            e.dec = heard(8);
            e.dec_param = 8;
            return e;
        }
        e.atk = heard(kAtk);
        e.sus = heard(kSus);
        e.rel = heard(kRel);
        e.holds = e.sus > 0;
        e.atk_param = kAtk;
        e.sus_param = kSus;
        e.rel_param = kRel;
        if (machine_is_grain(id)) {
            // GRAIN's DEC is on AMP's third knob; at the top it holds (dsp/grain.cpp), and the
            // cloud then fades over REL when the note ends.
            e.dec = heard(10);
            e.dec_param = 10;
            e.hold = e.dec >= kGrainDecayHold && !e.holds;
            e.holds = e.holds || e.hold;
            return e;
        }
        e.dec = heard(8);                                 // DEC, slot 8 on the VAs and FM4
        e.dec_param = 8;
        if (picture == EditPicture::filter) {
            // The filter's: ATK shared, FDE (or 0.55 x DEC, tied), FSU, ENV as its height.
            const int fde = heard(kSlotFdec);
            e.dec = fde == 0 ? e.dec * 55 / 100 : fde;
            e.dec_param = kSlotFdec;
            e.sus = heard(kFsu);
            e.sus_param = kFsu;
            e.peak = heard(9);
            e.peak_param = 9;
        }
        return e;
    }

    // The knob letter for a parameter on the page on screen, or 0 when no knob here turns it.
    char knob_letter(int param) const {
        if (param < 0) return 0;
        for (int slot = 0; slot < 4; slot++) {
            if (edit_index(slot) == param) return char('A' + slot);
        }
        return 0;
    }

    // `ride`: also the dot where the track's last note is on the curve (Status::env_stage),
    // on AMP and ASR, which draw the amplitude envelope the dot belongs to.
    void draw_envelope(enaga::Canvas &canvas, const EnvelopePicture &e, bool ride) {
        const int bottom = kPictureTop + kPictureRows - 1;
        const int span = kPictureRows - 1;
        // Segment ends: attack up to 26 px, decay 2-40, a 14 px plateau and a 2-40 px release
        // when the note holds. 1 + 26 + 40 + 14 + 40 = 121 < kWaveColumns.
        const int xa = 1 + e.atk * 26 / 127;
        const int xd = xa + 2 + e.dec * 38 / 127;
        const int xs = xd + (e.holds ? 14 : 0);
        const int xr = xs + (e.holds ? 2 + e.rel * 38 / 127 : 0);
        const float settle = e.hold ? 1.0f : (e.holds ? float(e.sus) * (1.0f / 127.0f) : 0.0f);
        const float top = float(e.peak) * (1.0f / 127.0f);

        // The floor, dim, across the width.
        canvas.rect(0, bottom, kWaveColumns, 1, 2);

        // The curve as a function of x, each column joined to the last so a steep fall has no
        // gaps. An exponential fall to 1% is drawn as (1 - t)^3, near enough to read.
        float curve[kWaveColumns] = {};
        int last = bottom;
        for (int x = 0; x <= xr; x++) {
            float level;
            if (x <= 1) {
                level = xa == 1 ? 1.0f : 0.0f;
            } else if (x <= xa) {
                level = float(x - 1) / float(xa - 1);
            } else if (x <= xd) {
                const float u = 1.0f - float(x - xa) / float(xd - xa);
                level = e.hold ? 1.0f : settle + (1.0f - settle) * u * u * u;
            } else if (x <= xs) {
                level = settle;
            } else {
                const float u = 1.0f - float(x - xs) / float(xr - xs);
                level = settle * u * u * u;
            }
            curve[x] = level;
            const int y = bottom - int(level * top * float(span) + 0.5f);
            for (int fy = y + 2; fy < bottom; fy += 2) canvas.pixel(x, fy, 2);   // a dim fill
            const int from = y < last ? y : last;
            const int to = y < last ? last : y;
            canvas.rect(x, from, 1, to - from + 1, 13);
            last = y;
        }

        if (ride) draw_envelope_dot(canvas, e, curve, xa, xd, xs, xr, settle);

        // The handles a knob on this page moves, with its letter, as CyDrums draws them.
        const int sus_y = bottom - int(settle * top * float(span) + 0.5f);
        const int top_y = bottom - int(top * float(span) + 0.5f);
        struct Handle { int x, y, param; };
        const Handle handles[] = {
            {xa, top_y, e.atk_param},  {xa, top_y, e.peak_param}, {xd, sus_y, e.dec_param},
            {(xd + xs) / 2, sus_y, e.sus_param}, {xr, bottom, e.rel_param},
        };
        int placed_x[5];
        int placed_y[5];
        int placed = 0;
        for (const Handle &h : handles) {
            const char letter = knob_letter(h.param);
            if (letter == 0) continue;
            const int hy = h.y - 1 < kPictureTop ? kPictureTop
                           : (h.y + 1 > bottom ? bottom - 2 : h.y - 1);
            canvas.rect(h.x - 1 < 0 ? 0 : h.x - 1, hy, 3, 3, 15);
            // Above the handle when there is room, else beside it; never on another letter.
            int ly = h.y - 10 >= kPictureTop ? h.y - 10 : kPictureTop;
            const auto clashes = [&](int lx) {
                for (int i = 0; i < placed; i++) {
                    // A glyph is 5 x 7 and the layout check wants 4 px between strings.
                    if (lx < placed_x[i] + 9 && placed_x[i] < lx + 9 && ly < placed_y[i] + 11 &&
                        placed_y[i] < ly + 11) {
                        return true;
                    }
                }
                return false;
            };
            const int right = kWaveColumns - 6;
            int lx = h.x + 3 > right ? right : h.x + 3;
            while (lx < right && clashes(lx)) lx = lx + 9 > right ? right : lx + 9;
            for (int left = h.x + 3 - 9; clashes(lx) && left >= 1; left -= 9) lx = left;
            if (clashes(lx)) continue;                    // no room: the handle alone
            const char text[2] = {letter, 0};
            canvas.rect(lx - 1, ly - 1, 7, 9, 0);
            canvas.text(lx, ly, text, 11);
            placed_x[placed] = lx;
            placed_y[placed] = ly;
            placed++;
        }
    }

    // Where the track's last note is on its envelope: a hollow square on the curve, found by its
    // stage and then, along that stage's columns, the first whose level is at or below the
    // note's. Nothing when the machine does not say or the note is over.
    void draw_envelope_dot(enaga::Canvas &canvas, const EnvelopePicture &e, const float *curve,
                           int xa, int xd, int xs, int xr, float settle) {
        const Status &status = engine_.status();
        const uint8_t stage = status.env_stage[selected_].load(std::memory_order_relaxed);
        const float level =
            float(status.env_level[selected_].load(std::memory_order_relaxed)) * (1.0f / 255.0f);
        if (stage == 0 || level < 0.004f) return;
        int x = -1;
        const auto find_falling = [&](int from, int to) {
            for (int i = from; i <= to && i < kWaveColumns; i++) {
                if (curve[i] <= level) return i;
            }
            return to;
        };
        switch (stage) {
            case 1:                                       // attack: rising to the peak
                x = 1 + int(float(xa - 1) * level + 0.5f);
                break;
            case 2:                                       // decay
                x = e.hold ? (xa + xd) / 2 : find_falling(xa, xd);
                break;
            case 3:                                       // sustain: the plateau, or a parked 0
                x = e.holds ? (xd + xs) / 2 : (e.hold ? (xa + xd) / 2 : find_falling(xa, xd));
                break;
            default:                                      // release
                x = e.holds ? find_falling(xs, xr) : find_falling(xa, xd);
                break;
        }
        (void)settle;
        if (x < 0 || x >= kWaveColumns) return;
        const int bottom = kPictureTop + kPictureRows - 1;
        // The part of the curve the note has travelled, its fill lit (app/ui_live.hpp's views
        // move the same way): where the sound has been, brighter than where it is going.
        for (int i = 0; i < x; i++) {
            const int cy = bottom - int(curve[i] * float(e.peak) * (1.0f / 127.0f) *
                                        float(kPictureRows - 1) + 0.5f);
            for (int fy = cy + 2; fy < bottom; fy += 2) canvas.pixel(i, fy, 5);
        }
        const int y = bottom - int(curve[x] * float(e.peak) * (1.0f / 127.0f) *
                                   float(kPictureRows - 1) + 0.5f);
        const int x0 = x - 1 < 0 ? 0 : x - 1;
        const int y0 = y - 1 < kPictureTop ? kPictureTop : (y + 1 > bottom ? bottom - 2 : y - 1);
        canvas.rect(x0, y0, 3, 3, 15);
        canvas.pixel(x0 + 1, y0 + 1, 0);
    }

    // LFO TRIG on this track: the kit's byte, which only the audio side writes, by command.
    LfoTrig lfo_trig_of(int track) const {
        return lfo_trig(engine_.project().kit.tracks[track].lfo_trig);
    }

    // SLC AUTO on this track: the kit's byte, which only the audio side writes, by command.
    bool slices_auto(int track) const {
        return engine_.project().kit.tracks[track].slice_mode == kSliceAuto;
    }
