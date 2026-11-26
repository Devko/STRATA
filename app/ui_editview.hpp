// EDIT as the drafts draw it (tools/visuals_mockup.py; docs/SCREENS.md has the pixel spec):
// one line of knobs in the small font at the top, a picture that takes the screen, and a footer
// with the fourth knob, what moves the page's knobs, and the page's hint.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, like the other ui_*.hpp parts: no #pragma once and no includes, and it sees every
// member the class has.
//
// Rows (docs/SCREENS.md, "EDIT"): the frame's top line 0-8 and progress 8, a toast 9-17 over
// everything, the knob line 18-22 (A B C), the picture 26-101, the footer at 105 (D, and the
// page's state at the right) and 113 (modulation, or the hint), the tabs 121-127. Columns 0-123;
// 126-127 are the frame's meter.
//
// Everything moving here moves from what the audio side publishes (Status) or from the running
// transport; stopped, a page is drawn from its knobs alone and holds still, so a picture never
// passes for an answer to an input (tests/test_ui.cpp, the control audit).

    static constexpr int kKnobY = 18;
    static constexpr int kPicTop = 26;
    static constexpr int kPicBottom = 101;
    static constexpr int kPicRight = 123;
    static constexpr int kFootY = 105;
    static constexpr int kFoot2Y = 113;
    static constexpr int kKnobPitch = 42;           // the drafts' A B C at x 0, 42, 84

    void draw_edit(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const Page page = Page(param_page_);
        const uint8_t id = engine_.machine_id(selected_);
        const EditView view = edit_view();

        // The knob line: A B C, and D in the footer, as every draft has it.
        draw_knob_line(canvas, kKnobY, 0x7u);

        if (frozen(selected_)) {
            // A frozen track plays its loop, not these knobs (dsp/frozen.hpp).
            draw_knob_line(canvas, kFootY, 0x8u);
            canvas.rect(8, 54, 108, 13, 15);
            canvas.text(11, 57, "FROZEN: FUNC+HOME", 0);
            return;
        }

        switch (view) {
            case EditView::sample:   draw_sample_view(canvas); break;
            case EditView::grain:    draw_grain_view(canvas); break;
            case EditView::table:    draw_table_view(canvas); break;
            case EditView::hit:      draw_hit_view(canvas); break;
            case EditView::osc:      draw_osc_view(canvas); break;
            case EditView::fm:       draw_fm_view(canvas); break;
            case EditView::ratios:   draw_ratio_view(canvas); break;
            case EditView::scan:     draw_scan_view(canvas); break;
            case EditView::envelope: draw_env_view(canvas, envelope_for(page == Page::fltr ? EditPicture::filter : EditPicture::amp), page != Page::fltr); break;
            case EditView::chain_filter: draw_chain_filter_view(canvas); break;
            case EditView::voice_filter: draw_voice_filter_view(canvas); break;
            case EditView::lfo:      draw_lfo_view(canvas); break;
            case EditView::fx:       draw_fx_view(canvas); break;
            case EditView::arp:      draw_arp_view(canvas); break;
            case EditView::bars:     draw_bars_view(canvas); break;
        }
        (void)id;

        // Holding a step and turning a knob: what the lock is, lit, over the footer's first line.
        // Else D and the page's state.
        if (!draw_lock_badge(canvas)) {
            const int end = draw_knob_line(canvas, kFootY, 0x8u);
            char state[32];
            edit_state_text(state);
            const int w = paint::small_width(state);
            if (w > 0 && kPicRight - w >= end + 5) paint::small_text(px, kPicRight - w + 1, kFootY, state, 8);
        }
        char line[48];
        if (!edit_mod_text(line)) edit_hint_text(line);
        if (line[0] != 0) paint::small_text(px, 0, kFoot2Y, line, 6);
    }

    // --- which picture ------------------------------------------------------------------------

    enum class EditView : uint8_t {
        sample, grain, table, hit, osc, fm, ratios, scan, envelope, chain_filter, voice_filter,
        lfo, fx, arp, bars,
    };

    EditView edit_view() const {
        const uint8_t id = engine_.machine_id(selected_);
        const MachineId m = MachineId(id);
        const bool midi = m == MachineId::midi;
        switch (Page(param_page_)) {
            case Page::src:
                if (midi) return EditView::bars;
                if (m == MachineId::sample) return EditView::sample;
                if (machine_is_grain(id)) return EditView::grain;
                if (machine_is_wave(id)) return EditView::table;
                if (machine_is_fm(id)) return EditView::fm;
                if (m == MachineId::va_mono || m == MachineId::va_poly) return EditView::osc;
                return EditView::hit;
            case Page::fltr:
                if (midi) return EditView::bars;
                return machine_has_voice_filter(id) ? EditView::voice_filter : EditView::chain_filter;
            case Page::amp:
                return midi ? EditView::bars : EditView::envelope;
            case Page::fx:  return midi ? EditView::bars : EditView::fx;
            case Page::lfo: return EditView::lfo;
            case Page::arp: return EditView::arp;
            case Page::asr: return has_asr(selected_) ? EditView::envelope : EditView::bars;
            case Page::osc:
                if (machine_is_fm(id)) return EditView::ratios;
                if (machine_is_grain(id)) return EditView::scan;
                if (machine_is_wave(id) || m == MachineId::va_mono || m == MachineId::va_poly) {
                    return EditView::osc;
                }
                return EditView::bars;
        }
        return EditView::bars;
    }

    // --- the knob line ------------------------------------------------------------------------
    //
    // "A ATK 004": the knob's letter dim, the name grey, the value bright, in the small font. A
    // value a held step locks (or gives a random range) is drawn lit. Laid out at the drafts'
    // x 0, 42, 84, each moved right only as far as the one before it needs; if the three still
    // run past the picture the letters lose their space ("ATUN G-1+"), and that is all.

    struct KnobText {
        char text[28];
        int letter = 0;          // characters of the letter and its space
        int name = 0;            // characters of the name
        bool marked = false;     // a held step's lock or range on it
        bool empty = false;      // no parameter on this knob
    };

    KnobText knob_text(int slot, bool compact) {
        KnobText k;
        if (mode_ == Mode::mixer) {
            char name[8];
            char value[16];
            bool empty = false;
            mixer_knob_parts(slot, name, value, empty);
            char *out = k.text;
            *out++ = char('A' + slot);
            if (!compact) *out++ = ' ';
            k.letter = int(out - k.text);
            append(out, name);
            k.name = int(out - k.text) - k.letter;
            k.empty = empty;
            if (!empty) { *out++ = ' '; append(out, value); }
            *out = 0;
            return k;
        }
        const int index = edit_index(slot);
        const ShownParam shown = shown_param(index);
        char *out = k.text;
        *out++ = char('A' + slot);
        if (!compact) *out++ = ' ';
        k.letter = int(out - k.text);
        const char *name = param_name_for(index);
        append(out, name);
        k.name = int(out - k.text) - k.letter;
        k.empty = index < 0 || name[0] == '-';
        if (!k.empty) {
            *out++ = ' ';
            append_param_value(out, index, shown.heard);
        }
        *out = 0;
        k.marked = !k.empty && (shown.locked || shown.range != 0);
        return k;
    }

    void draw_knob_text(uint8_t *px, int x, int y, const KnobText &k) {
        if (k.empty && k.letter > 0 && k.text[0] == 'D') return;     // the footer's D: nothing
        if (k.empty) {
            paint::small_text(px, x, y, k.text, 4);
            return;
        }
        paint::small_text(px, x, y, k.text, 15);
        char part[8];
        int n = 0;
        for (; n < k.letter && n < 7; n++) part[n] = k.text[n];
        part[n] = 0;
        paint::small_glyphs(px, x, y, part, 6);
        for (n = 0; n < k.name && n < 7; n++) part[n] = k.text[k.letter + n];
        part[n] = 0;
        paint::small_glyphs(px, x + k.letter * paint::kSmallAdvance, y, part, 10);
        if (k.marked) {
            const int vx = x + (k.letter + k.name + 1) * paint::kSmallAdvance;
            const int vw = paint::small_width(k.text + k.letter + k.name + 1);
            for (int j = -1; j <= paint::kSmallHeight; j++) {
                for (int i = -1; i <= vw; i++) paint::plot(px, vx + i, y + j, 15);
            }
            paint::small_glyphs(px, vx, y, k.text + k.letter + k.name + 1, 0);
        }
    }

    // The knobs in `mask` (bit 0 A ... bit 3 D) on one line at `y`. Returns the x after the last.
    // Three blank columns between two entries at the least (a letter's own gap makes it four);
    // past the meter's edge the longest entry, then all of them, lose the letter's space.
    int draw_knob_line(enaga::Canvas &canvas, int y, unsigned mask, int x0 = 0) {
        uint8_t *px = canvas.data();
        KnobText k[4];
        int xs[4] = {};
        int end = 0;
        int longest = -1;
        for (int pass = 0; pass < 3; pass++) {
            end = 0;
            int at = 0;
            for (int slot = 0; slot < 4; slot++) {
                if (((mask >> slot) & 1u) == 0) continue;
                k[slot] = knob_text(slot, pass == 2 || (pass == 1 && slot == longest));
                const int nominal = x0 + at * kKnobPitch;
                xs[slot] = end > 0 && nominal < end + 4 ? end + 4 : nominal;
                end = xs[slot] + paint::small_width(k[slot].text);
                at++;
            }
            if (end <= kPicRight + 2) break;
            int most = -1;
            for (int slot = 0; slot < 4; slot++) {
                if (((mask >> slot) & 1u) == 0) continue;
                const int w = paint::small_width(k[slot].text);
                if (w > most) { most = w; longest = slot; }
            }
        }
        for (int slot = 0; slot < 4; slot++) {
            if (((mask >> slot) & 1u) != 0) draw_knob_text(px, xs[slot], y, k[slot]);
        }
        return end;
    }

    // Turning a lock with a STEP held: "S5 CUT 64>90" lit across the footer, in Enaga's font, until
    // the STEP is let go (app/ui_mixer.hpp, draw_lock_readout).
    bool draw_lock_badge(enaga::Canvas &canvas) {
        if (held_step_ < 0 || lock_readout_index_ < 0 || lock_readout_step_ != held_step_ ||
            lock_readout_track_ != selected_) {
            return false;
        }
        canvas.rect(0, kFootY - 2, 125, 9, 15);
        return draw_lock_readout(canvas, kFootY - 1, 0);
    }

    // --- the footer ---------------------------------------------------------------------------

    // What the page is doing, at the footer's right: the filter's mode, the LFO's TRIG, the arp's
    // run, SAMPLE's slices. Empty when the knobs say it all.
    void edit_state_text(char *out_text) const {
        char *out = out_text;
        const uint8_t id = engine_.machine_id(selected_);
        const TrackParams &kit = engine_.params(selected_);
        switch (Page(param_page_)) {
            case Page::lfo:
                append(out, "TRIG ");
                append(out, lfo_trig_name(lfo_trig_of(selected_)));
                break;
            case Page::arp:
                if (arp_mode_index(kit.v[kArpFirst]) != 0) {
                    append(out, "X");
                    append_number(out, uint32_t(arp_octaves(kit.v[kArpFirst + 2])));
                    append(out, " OCT");
                }
                break;
            case Page::src:
                if (id == uint8_t(MachineId::sample)) {
                    if (slices_auto(selected_)) {
                        const uint8_t hits = engine_.status().slice_points[selected_].load(std::memory_order_relaxed);
                        append(out, "AUTO ");
                        append_number(out, hits);
                    }
                }
                break;
            default: break;
        }
        *out = 0;
    }

    // What moves this page's knobs: the LFO ("LFO>CUT 40") and the MOD slots ("VEL>RES +20"),
    // as the drafts' "ENV>CUT +32  LFO>CUT 20". False when nothing does.
    bool edit_mod_text(char *line) const {
        char *out = line;
        const TrackParams &kit = engine_.params(selected_);
        const auto on_page = [&](int index) {
            for (int s = 0; s < 4; s++) {
                if (edit_index(s) == index) return true;
            }
            return false;
        };
        const auto add = [&](const char *part) {
            int n = 0;
            while (part[n] != 0) n++;
            const int used = int(out - line);
            if (used + (used ? 2 : 0) + n > 30) return;
            if (used) { *out++ = ' '; *out++ = ' '; }
            append(out, part);
        };
        char part[20];
        const int dest = lfo_dest(kit.v[kLfoDest]);
        if (dest >= 0 && kit.v[kLfoDepth] > 0 && on_page(dest)) {
            char *p = part;
            append(p, "LFO>");
            append(p, track_param_name(dest));
            *p++ = ' ';
            append_number(p, kit.v[kLfoDepth]);
            *p = 0;
            add(part);
        }
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = mod_slot_of(selected_, s);
            if (!slot.on() || !on_page(slot.dest)) continue;
            char *p = part;
            append(p, mod_source_name(slot.source));
            *p++ = '>';
            append(p, track_param_name(slot.dest));
            *p++ = ' ';
            append_signed(p, slot.depth);
            *p = 0;
            add(part);
        }
        *out = 0;
        return out != line;
    }

    // The page's hint, the line it had before the drafts: what a push does here, or why nothing
    // does.
    void edit_hint_text(char *line) const {
        char *out = line;
        const uint8_t id = engine_.machine_id(selected_);
        const TrackParams &kit = engine_.params(selected_);
        switch (Page(param_page_)) {
            case Page::src:
                if (id == uint8_t(MachineId::sample)) {
                    if (!slices_auto(selected_)) {
                        append(out, "PUSH SLC: AUTO SLICE");
                    } else {
                        const uint8_t hits = engine_.status().slice_points[selected_].load(std::memory_order_relaxed);
                        append(out, "SLC AUTO: ");
                        if (hits == 0) append(out, "NO HITS");
                        else { append_number(out, hits); append(out, hits == 1 ? " HIT" : " HITS"); }
                    }
                } else if (machine_is_grain(id)) {
                    append(out, "OK: SOUNDS");
                } else if (id == uint8_t(MachineId::midi)) {
                    append(out, "MIDI OUT: NOTES AND CCS");
                } else {
                    append(out, "FUNC+EDIT: MACHINE");
                }
                break;
            case Page::fltr:
                if (machine_has_voice_filter(id)) {
                    append(out, kit.v[kSlotFdec] == 0 ? "FDE 0: 0.55 X DEC" : "TON ON SRC: CHAIN FILTER");
                } else {
                    append(out, "FUNC+EDIT: MACHINE");
                }
                break;
            case Page::amp:
                if (id == uint8_t(MachineId::sample)) append(out, "OK: PICK SAMPLE");
                else if (machine_is_grain(id)) append(out, "OK: SOUNDS");
                else append(out, "FUNC+EDIT: MACHINE");
                break;
            case Page::fx:
                append(out, "SNA DELAY  SNB REVERB");
                break;
            case Page::lfo: {
                // The page the MOD slots are set from lists them both, or says how.
                const bool any = mod_slot_of(selected_, 0).on() || mod_slot_of(selected_, 1).on();
                if (!any) {
                    append(out, "PUSH C D: MOD SLOTS");
                    break;
                }
                for (int s = 0; s < kModSlots; s++) {
                    const KitModSlot &slot = mod_slot_of(selected_, s);
                    if (s > 0) append(out, "  ");
                    append_mod_slot(out, s, slot.on() ? slot.source : kModSrcOff, slot.dest,
                                    slot.on() ? slot.depth : 0);
                }
                break;
            }
            case Page::arp:
                if (arp_mode_index(kit.v[kArpFirst]) == 0) append(out, "ARP OFF: TURN MOD");
                break;
            case Page::asr:
                if (!has_asr(selected_)) {
                    append(out, "NOT ON THIS MACHINE");
                } else {
                    const bool sustains = kit.v[kAsrFirst + 1] > 0;
                    append(out, sustains ? "HOLDS: PAD OR LEN"
                                         : (machine_is_grain(id) ? "SUS 0: DEC DECIDES" : "SUS 0: ONE-SHOT"));
                }
                break;
            case Page::osc:
                append(out, machine_is_fm(id)     ? "FIN NUDGES RAT"
                            : machine_is_wave(id) ? "WRP BENDS  DET 2 OSCS"
                            : machine_is_grain(id) ? "PSP: DET 1-63 OCT 64+"
                            : has_asr(selected_) ? "OS2: DET 1-63 SYN 64+"
                                                 : "NOT ON THIS MACHINE");
                break;
        }
        *out = 0;
    }

    // --- shared pieces ------------------------------------------------------------------------

    // What a parameter sounds at now: a MOD slot's or the LFO's result when one moves it, else
    // the kit (or the held step's lock).
    uint8_t live_value(int index) const {
        if (index < 0) return 0;
        const Status &status = engine_.status();
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = mod_slot_of(selected_, s);
            if (!slot.on() || slot.dest != index) continue;
            const uint8_t v = status.mod_heard[selected_][s].load(std::memory_order_relaxed);
            if (v != Status::kNotHeard) return v;
        }
        const uint8_t shown = status.lfo_shown[selected_].load(std::memory_order_relaxed);
        if (shown != Status::kNotHeard && lfo_dest(engine_.params(selected_).v[kLfoDest]) == index) {
            return shown;
        }
        return shown_param(index).heard;
    }

    // Each track's last note, as the UI saw it arrive (Status::triggered): when, in ms. A picture
    // that plays the note through (the drum's hit, the filter's sweep) starts from here. Only the
    // audio side moves the counts, so a stopped box holds still.
    void update_track_triggers() {
        const Status &status = engine_.status();
        // A count that moved while nothing was drawn for a while (Enaga's menu was up) is an old
        // note, not one that just came: it is taken in without playing it through.
        const bool fresh = uint32_t(last_ms_ - edit_trig_checked_ms_) <= 200u;
        edit_trig_checked_ms_ = last_ms_;
        for (int t = 0; t < kTracks; t++) {
            const uint8_t count = status.triggered[t].load(std::memory_order_relaxed);
            if (count != edit_trig_seen_[t]) {
                edit_trig_seen_[t] = count;
                edit_trig_ms_[t] = last_ms_;
                edit_trig_live_[t] = fresh;
            }
        }
    }

    // Milliseconds since the selected track's last note, or -1 when there has been none since the
    // page came up (or it was long ago).
    int ms_since_trigger(uint32_t longest) const {
        if (!edit_trig_live_[selected_]) return -1;
        const uint32_t age = last_ms_ - edit_trig_ms_[selected_];
        return age > longest ? -1 : int(age);
    }

    // A knob byte as 0..1.
    static float unit(uint8_t v) { return float(v) * (1.0f / 127.0f); }

    // The drafts' dotted guide grid over a picture.
    static void picture_grid(uint8_t *px, int top, int bottom, int every_x) {
        for (int x = 0; x <= 119; x += every_x) paint::dotted_v(px, x, top, bottom, 4, 2);
    }

    // --- AMP and ASR (and a synth's filter envelope): the envelope, as the draft draws it -------
    //
    // The curve over a dotted grid with a dim fill, the knobs' letters lit over the points they
    // move. While the track's note sounds, the part it has travelled is bright with its fill, and a
    // dot rides the curve (Status::env_stage, env_level). Widths go by the knob: every time here is
    // exponential in its knob, so this is a log-time drawing (2 ms and 5 s share the width).
    void draw_env_view(enaga::Canvas &canvas, const EnvelopePicture &e, bool ride) {
        uint8_t *px = canvas.data();
        const int top = kPicTop + 8;              // room for the letters over the peak
        const int bottom = kPicBottom - 1;
        const int span = bottom - top;
        // A note that holds has four segments to share the width; one that does not, two, so a
        // drum's decay is not squeezed into the left third.
        const int atk_max = e.holds ? 25 : 30, dec_max = e.holds ? 36 : 82;
        const int xa = 2 + e.atk * atk_max / 127;
        const int xd = xa + 2 + e.dec * dec_max / 127;
        const int xs = xd + (e.holds ? 14 : 0);
        const int xr = xs + (e.holds ? 2 + e.rel * 36 / 127 : 0);
        const float settle = e.hold ? 1.0f : (e.holds ? float(e.sus) * (1.0f / 127.0f) : 0.0f);
        const float peak = float(e.peak) * (1.0f / 127.0f);

        picture_grid(px, top, bottom, 8);
        for (int q = 1; q <= 3; q++) paint::dotted_h(px, 0, 119, bottom - span * q / 4, 4, 2);

        float curve[kWaveColumns] = {};
        int16_t xs_q[kWaveColumns];
        int16_t ys_q[kWaveColumns];
        int n = 0;
        for (int x = 0; x <= xr && x < kWaveColumns; x++) {
            float level;
            if (x < 2) level = 0.0f;
            else if (x <= xa) level = xa == 2 ? 1.0f : float(x - 2) / float(xa - 2);
            else if (x <= xd) {
                const float u = 1.0f - float(x - xa) / float(xd - xa);
                level = e.hold ? 1.0f : settle + (1.0f - settle) * u * u * u;
            } else if (x <= xs) level = settle;
            else {
                const float u = 1.0f - float(x - xs) / float(xr - xs);
                level = settle * u * u * u;
            }
            curve[x] = level;
            const int y = bottom - int(level * peak * float(span) + 0.5f);
            for (int fy = y + 2; fy < bottom; fy += 2) paint::plot_max(px, x, fy, 2);
            xs_q[n] = int16_t(paint::q4(x));
            ys_q[n] = int16_t(paint::q4(y));
            n++;
        }
        paint::polyline(px, xs_q, ys_q, n, 9);

        if (ride) {
            const int head = envelope_head(e, curve, xa, xd, xs, xr);
            if (head > 0 && head < n) {
                for (int x = 0; x < head; x++) {
                    for (int fy = (ys_q[x] >> 4) + 2; fy < bottom; fy += 2) paint::plot_max(px, x, fy, 4);
                }
                paint::polyline(px, xs_q, ys_q, head + 1, 15);
                paint::dot(px, head, ys_q[head] >> 4, 15);
            }
        }

        // The letters over the points, lit, as the draft has them; never one on another.
        const int sus_y = bottom - int(settle * peak * float(span) + 0.5f);
        const int top_y = bottom - int(peak * float(span) + 0.5f);
        struct Handle { int x, y, param; };
        const Handle handles[] = {
            {xa, top_y, e.atk_param}, {xa, top_y, e.peak_param}, {xd, sus_y, e.dec_param},
            {(xd + xs) / 2, sus_y, e.sus_param}, {xr, bottom, e.rel_param},
        };
        int placed[5];
        int count = 0;
        for (const Handle &h : handles) {
            const char letter = knob_letter(h.param);
            if (letter == 0) continue;
            int lx = h.x - 1 < 1 ? 1 : h.x - 1;
            bool clash = true;
            for (int tries = 0; tries < 6 && clash; tries++) {
                clash = false;
                for (int i = 0; i < count; i++) {
                    if (lx < placed[i] + 9 && placed[i] < lx + 9) clash = true;
                }
                if (clash) lx += 9;
            }
            if (clash || lx > 117) continue;
            const char text[2] = {letter, 0};
            int ly = h.y - 9;
            if (ly < kPicTop + 1) ly = kPicTop + 1;
            paint::small_badge(px, lx, ly, text);
            placed[count++] = lx;
        }
        canvas.rect(0, bottom, 120, 1, 6);
    }

    // Where the track's last note is along the curve: its stage, then along that stage's columns
    // the first whose level is at or below the note's. 0 when nothing sounds.
    int envelope_head(const EnvelopePicture &e, const float *curve, int xa, int xd, int xs, int xr) const {
        const Status &status = engine_.status();
        const uint8_t stage = status.env_stage[selected_].load(std::memory_order_relaxed);
        const float level = float(status.env_level[selected_].load(std::memory_order_relaxed)) * (1.0f / 255.0f);
        if (stage == 0 || level < 0.004f) return 0;
        const auto falling = [&](int from, int to) {
            for (int i = from; i <= to && i < kWaveColumns; i++) {
                if (curve[i] <= level) return i;
            }
            return to;
        };
        switch (stage) {
            case 1: return 2 + int(float(xa - 2) * level + 0.5f);
            case 2: return e.hold ? (xa + xd) / 2 : falling(xa, xd);
            case 3: return e.holds ? (xd + xs) / 2 : (e.hold ? (xa + xd) / 2 : falling(xa, xd));
            default: return e.holds ? falling(xs, xr) : falling(xa, xd);
        }
    }

    // --- FLTR: the filter's response over the track's spectrum, as the draft draws it ----------
    //
    // 30 Hz to 12 kHz across, 0 dB 22 rows under the top, 1.1 rows a decibel; behind it, dim, the
    // output's spectrum in thin bars while the beat plays; the curve bright with a faint fill, a
    // line at the cutoff with its knob's letter. The magnitudes are the textbook state-variable
    // ones in decibels from log2, no root.
    static constexpr int kRespTop = kPicTop;
    static constexpr int kRespBottom = 96;

    static int resp_x(float hz) {
        namespace ed = enaga::dsp;
        return 1 + int(ed::log2(hz * (1.0f / 30.0f)) * (118.0f / 8.6439f));
    }
    static float resp_hz(int x) {
        namespace ed = enaga::dsp;
        return 30.0f * ed::exp2(float(x - 1) * (8.6439f / 118.0f));
    }

    // mode: 0 off, 1 LP12, 2 LP24, 3 BP12, 4 BP24, 5 HP12, 6 HP24.
    static int resp_row(float hz, float cut_hz, float inv_q, int mode) {
        namespace ed = enaga::dsp;
        float db = 0.0f;
        if (mode != 0) {
            const float w = hz / cut_hz;
            const float w2 = w * w;
            const float a = 1.0f - w2;
            const float b = w * inv_q;
            const float den = a * a + b * b;
            const float num = mode <= 2 ? 1.0f : (mode <= 4 ? b * b : w2 * w2);
            db = 3.0103f * ed::log2((num + 1e-9f) / (den + 1e-9f));
            if (mode % 2 == 0) db *= 2.0f;
        }
        int y = kRespTop + 22 - int(db * 1.1f);
        return y < kRespTop ? kRespTop : (y > kRespBottom ? kRespBottom : y);
    }

    void draw_response_curve(uint8_t *px, float cut_hz, float q, int mode, int grey, bool fill) {
        const float inv_q = 1.0f / q;
        int16_t xs[62];
        int16_t ys[62];
        int n = 0;
        for (int x = 1; x < 121 && n < 62; x += 2) {
            const int y = resp_row(resp_hz(x), cut_hz, inv_q, mode);
            if (fill) {
                for (int fy = y + 2; fy < kRespBottom; fy += 3) paint::plot_max(px, x, fy, 2);
            }
            xs[n] = int16_t(paint::q4(x));
            ys[n] = int16_t(paint::q4(y));
            n++;
        }
        paint::polyline(px, xs, ys, n, grey);
    }

    // The response's frame: the guides, the spectrum behind (while the beat plays), the axis.
    void draw_response_frame(uint8_t *px) {
        const int f100 = resp_x(100.0f), f1k = resp_x(1000.0f), f10k = resp_x(10000.0f);
        paint::dotted_v(px, f100, kRespTop, kRespBottom, 4, 2);
        paint::dotted_v(px, f1k, kRespTop, kRespBottom, 4, 2);
        paint::dotted_v(px, f10k, kRespTop, kRespBottom, 4, 2);
        paint::small_text(px, f100 - 5, kRespBottom + 2, "100", 5);
        paint::small_text(px, f1k - 3, kRespBottom + 2, "1K", 5);
        paint::small_text(px, f10k - 5, kRespBottom + 2, "10K", 5);
        const bool running = enaga::clock::state().running != 0;
        if (running) {
            uint16_t milli[spectrum::kBands];
            spectrum::analyse(scope_history_, spectrum_work_, milli);
            for (int b = 0; b < spectrum::kBands; b++) {
                const int rows = paint::level_rows(uint32_t(milli[b]) * 2u, (kRespBottom - kRespTop) * 9 / 10, 60);
                const int fallen = flt_band_rows_[b] - (flt_band_rows_[b] + 4) / 5;
                flt_band_rows_[b] = uint8_t(rows > fallen ? rows : fallen);
            }
        } else {
            for (int b = 0; b < spectrum::kBands; b++) flt_band_rows_[b] = 0;
        }
        for (int x = 1; x < 120; x += 2) {
            const float bin = resp_hz(x) * (1.0f / 93.75f);
            int b = 0;
            while (b < spectrum::kBands - 1 && float(spectrum::kEdges[b + 1]) <= bin) b++;
            if (bin < float(spectrum::kEdges[0])) continue;
            const int h = flt_band_rows_[b];
            if (h > 0) {
                for (int y = kRespBottom - h; y < kRespBottom; y++) paint::plot(px, x, y, 3);
            }
        }
    }

    void draw_cut_marker(enaga::Canvas &canvas, float cut_hz, int letter) {
        const int cx = resp_x(cut_hz);
        if (cx < 0 || cx > 119) return;
        canvas.rect(cx, kRespTop, 1, kRespBottom - kRespTop, 6);
        if (letter != 0) {
            const char text[2] = {char(letter), 0};
            paint::small_badge(canvas.data(), cx - 1 < 1 ? 1 : (cx - 1 > 117 ? 117 : cx - 1), kRespBottom - 7, text);
        }
    }

    static constexpr const char *kChainModes[7] = {"OFF", "LP12", "LP24", "BP12", "BP24", "HP12", "HP24"};
    static constexpr const char *kVoiceModes[4] = {"LP24", "LP12", "BP12", "HP12"};
    static int voice_filter_mode(uint8_t mod) {         // dsp/va.cpp, slot 1: four unequal zones
        return mod < 64 ? 0 : (mod < 96 ? 1 : (mod < 112 ? 2 : 3));
    }

    // The insert chain's filter (drums, SAMPLE, GRAIN): DRV CUT RES MOD. What the track plays,
    // from Status (Engine::publish_heard), so an LFO on CUT moves the line.
    void draw_chain_filter_view(enaga::Canvas &canvas) {
        namespace ed = enaga::dsp;
        uint8_t *px = canvas.data();
        const auto heard = [&](int k) {
            return engine_.status().heard_filter[selected_][k].load(std::memory_order_relaxed);
        };
        const int mode = int(heard(2)) * 7 / 128;
        const float cut_hz = 20.0f * ed::exp2(float(heard(0)) * (9.814f / 127.0f));
        const float q = 0.5f + float(heard(1)) * (0.95f / 127.0f) * 8.0f;
        draw_response_frame(px);
        draw_response_curve(px, cut_hz, q, mode, mode == 0 ? 6 : 15, true);
        if (mode != 0) draw_cut_marker(canvas, cut_hz, knob_letter(edit_index(1)));
        paint::small_text(px, 120 - paint::small_width(kChainModes[mode]), kRespTop + 1, kChainModes[mode], 9);
        canvas.rect(0, kRespBottom, 120, 1, 6);
    }

    // A synth's own filter (VA, WAVE): CUT RES ENV FDE, its mode on SRC's MOD. The curve at the
    // knob's cutoff, a dim one where the filter envelope opens it to (ENV), and after each note
    // the cutoff sweeping back down from there over the envelope's fall (FDE, or 0.55 x DEC).
    void draw_voice_filter_view(enaga::Canvas &canvas) {
        namespace ed = enaga::dsp;
        uint8_t *px = canvas.data();
        const TrackParams &kit = engine_.params(selected_);
        static constexpr int kVoiceToChain[4] = {2, 1, 3, 5};
        const int shown_mode = kVoiceToChain[voice_filter_mode(kit.v[1])];
        const float cut_hz = 35.0f * ed::exp2(8.42f * unit(live_value(edit_index(0))));
        const float q = 0.5f + unit(live_value(edit_index(1))) * 6.0f;
        const float env_semis = 66.0f * unit(live_value(edit_index(2)));
        const float open_hz = cut_hz * ed::exp2(env_semis * (1.0f / 12.0f));
        draw_response_frame(px);
        if (env_semis > 0.5f) draw_response_curve(px, open_hz > 12000.0f ? 12000.0f : open_hz, q, shown_mode, 5, false);
        // The sweep: from the open curve back to the knob's over the envelope's fall.
        float now_hz = cut_hz;
        const int fde = live_value(edit_index(3));
        const uint8_t dec = kit.v[8];
        const float fall_s = fde == 0 ? 0.55f * 0.015f * ed::exp2(7.32f * unit(dec))
                                      : 0.015f * ed::exp2(7.32f * unit(uint8_t(fde)));
        const int age = ms_since_trigger(4000);
        if (age >= 0 && env_semis > 0.5f) {
            const float t = float(age) * 0.001f;
            const float left = ed::exp2(-1.4427f * t / (fall_s * 0.4f + 0.001f));
            now_hz = cut_hz * ed::exp2(env_semis * left * (1.0f / 12.0f));
        }
        draw_response_curve(px, now_hz > 12000.0f ? 12000.0f : now_hz, q, shown_mode, 15, true);
        draw_cut_marker(canvas, cut_hz, knob_letter(edit_index(0)));
        paint::small_text(px, 120 - paint::small_width(kChainModes[shown_mode]), kRespTop + 1, kChainModes[shown_mode], 9);
        canvas.rect(0, kRespBottom, 120, 1, 6);
    }

    // --- LFO: the shape with the LFO riding it, its output over time, and where it has its
    // destination, as the draft draws them ---------------------------------------------------------
    static constexpr int kLfoX0 = 2, kLfoX1 = 76, kLfoMid = 58, kLfoAmp = 26;
    static constexpr int kLfoStrip = 40;

    // The shape at `phase` (0-255 a cycle) in Q12, -4096..4096. Drawn for reading, from the table
    // sine and straight lines; the sound's own are dsp/lfo.hpp's.
    static int lfo_shape_q12(LfoWave wave, int phase) {
        phase &= 255;
        switch (wave) {
            case LfoWave::sine:     return paint::sin_q12(phase);
            case LfoWave::triangle: return phase < 64 ? phase * 64
                                         : (phase < 192 ? (128 - phase) * 64 : (phase - 256) * 64);
            case LfoWave::ramp:     return -4096 + phase * 32;
            case LfoWave::saw:      return 4096 - phase * 32;
            case LfoWave::square:   return phase < 128 ? 4096 : -4096;
            default: {
                static constexpr int16_t kSteps[8] = {2600, -1400, 3900, -3300, 600, -2600, 1800, -400};
                return kSteps[phase >> 5];
            }
        }
    }

    // A one-pixel outline.
    static void frame_rect(uint8_t *px, int x, int y, int w, int h, int grey) {
        for (int i = 0; i < w; i++) { paint::plot(px, x + i, y, grey); paint::plot(px, x + i, y + h - 1, grey); }
        for (int j = 0; j < h; j++) { paint::plot(px, x, y + j, grey); paint::plot(px, x + w - 1, y + j, grey); }
    }

    void draw_lfo_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const TrackParams &kit = engine_.params(selected_);
        const LfoWave wave = lfo_wave(live_value(kLfoWave));
        const Status &status = engine_.status();
        const uint8_t shown = status.lfo_shown[selected_].load(std::memory_order_relaxed);
        const bool running = shown != Status::kNotHeard;
        const int top = kLfoMid - kLfoAmp - 4;
        const int rows = 2 * kLfoAmp + 9;

        paint::dotted_h(px, kLfoX0, kLfoX1, kLfoMid, 3, 4);
        frame_rect(px, kLfoX0 - 2, top, kLfoX1 - kLfoX0 + 4, rows, 3);
        int16_t xs[65];
        int16_t ys[65];
        for (int i = 0; i <= 64; i++) {
            xs[i] = int16_t(paint::q4(kLfoX0) + i * (kLfoX1 - kLfoX0) * paint::kSub / 64);
            ys[i] = int16_t(paint::q4(kLfoMid) - lfo_shape_q12(wave, i * 4) * kLfoAmp * paint::kSub / 4096);
        }
        paint::polyline(px, xs, ys, 65, running ? 8 : 5);

        // The strip: the output over the last frames, newest at the right.
        frame_rect(px, 82, top, 38, rows, 3);
        const int phase = status.lfo_phase[selected_].load(std::memory_order_relaxed);
        if (running) {
            const int k = (phase / 4) & 63;
            for (int j = 9; j >= 1; j--) {
                const int kk = ((k - j * 2) % 64 + 64) % 64;
                paint::dot(px, xs[kk] >> 4, ys[kk] >> 4, 12 - j > 2 ? 12 - j : 2);
            }
            paint::dotted_v(px, xs[k] >> 4, kLfoMid - kLfoAmp - 3, kLfoMid + kLfoAmp + 4, 6, 2);
            paint::dot(px, xs[k] >> 4, ys[k] >> 4, 15);
            const int v = lfo_shape_q12(wave, phase);
            lfo_strip_[lfo_strip_at_ % uint32_t(kLfoStrip)] = uint8_t((v + 4096) * 254 / 8192);
            lfo_strip_at_++;
            int prev = -1;
            for (int i = 0; i < 36 && uint32_t(i) < lfo_strip_at_; i++) {
                const uint8_t s = lfo_strip_[(lfo_strip_at_ - 1u - uint32_t(i)) % uint32_t(kLfoStrip)];
                const int y = kLfoMid - (int(s) - 127) * kLfoAmp / 127;
                const int x = 118 - i;
                if (prev >= 0) paint::line(px, paint::q4(x + 1), paint::q4(prev), paint::q4(x), paint::q4(y), 12);
                prev = y;
            }
        } else {
            lfo_strip_at_ = 0;
        }
        paint::small_text(px, 84, top + rows + 2, "OUT", 6);

        // Where the destination is: the bar lit to what it sounds at, a mark at its knob. Off,
        // what turns it on.
        const int dest = lfo_dest(kit.v[kLfoDest]);
        const int bar_y = top + rows + 3;
        if (!running) {
            paint::small_text(px, 0, bar_y, "LFO OFF: SET DEP+DST", 7);
            return;
        }
        canvas.rect(0, bar_y, 79, 5, 2);
        if (dest >= 0) {
            const int now = running ? int(shown) : int(shown_param(dest).heard);
            if (now > 0) canvas.rect(0, bar_y, now * 79 / 127, 5, running ? 12 : 6);
            const int base = shown_param(dest).heard * 78 / 127;
            canvas.rect(base, bar_y - 1, 1, 7, 15);
        }
    }

    // --- WAVE's SRC page: the table as a waterfall, as the draft draws it ---------------------
    //
    // Sixteen frames back to front, up and to the right, each 40 points of the table's smoothest
    // level; the frame POS plays brightest and its neighbours brighter than the rest, each frame
    // blacking out what is behind it under its curve. Under it, LIVE: the frame POS plays, between
    // its two neighbours as the oscillator reads it. What the track plays comes from Status
    // (Engine::publish_heard); the frames from the WaveLibrary the main context built.
    void draw_table_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const Status &status = engine_.status();
        const uint8_t heard_table = status.heard_table[selected_].load(std::memory_order_relaxed);
        const int table = heard_table == Status::kNotHeard ? -1 : int(heard_table);
        const WaveLibrary &waves = engine_.waves();
        if (table < 0 || !waves.resident(table)) {
            canvas.text(38, 56, "BUILDING", 7);
            return;
        }
        const int pos = int(status.heard_pos[selected_].load(std::memory_order_relaxed)) *
                        (kWaveFrames - 1) * 16 / 127;                       // in 1/16 frames
        const int mip = kWaveMipLevels - 1;
        const int length = wave_mip_length(mip);
        constexpr int kPoints = 40;
        for (int f = kWaveFrames - 1; f >= 0; f--) {
            const int16_t *samples = waves.frame(table, f, mip);
            const int ox = paint::q4(6) + f * 54;                // 3.4 px a frame to the right
            const int oy = paint::q4(74) - f * 42;               // and 2.6 up
            int16_t xs[kPoints];
            int16_t ys[kPoints];
            for (int i = 0; i < kPoints; i++) {
                const int v = samples[i * length / kPoints];
                xs[i] = int16_t(ox + i * 26);                    // 1.6 px a point
                ys[i] = int16_t(oy - v * 11 * paint::kSub / 32768);
                const int x = xs[i] >> 4;
                const int floor = (oy >> 4) + 12;
                for (int y = (ys[i] >> 4) + 1; y <= floor; y++) {
                    paint::plot(px, x, y, 0);
                    paint::plot(px, x + 1, y, 0);
                }
            }
            const int d = f * 16 - pos < 0 ? pos - f * 16 : f * 16 - pos;
            paint::polyline(px, xs, ys, kPoints, d < 8 ? 15 : (d < 24 ? 8 : 4));
        }
        canvas.rect(0, 87, 120, 1, 3);
        paint::small_text(px, 0, 90, "LIVE", 6);
        const int f0 = pos / 16;
        const int f1 = f0 + 1 < kWaveFrames ? f0 + 1 : f0;
        const int mix = pos % 16;
        const int16_t *a = waves.frame(table, f0, mip);
        const int16_t *b = waves.frame(table, f1, mip);
        int16_t lx[64];
        int16_t ly[64];
        for (int i = 0; i < 64; i++) {
            const int at = i * length / 64;
            const int v = (int(a[at]) * (16 - mix) + int(b[at]) * mix) / 16;
            lx[i] = int16_t(paint::q4(20) + i * 99 * paint::kSub / 63);
            ly[i] = int16_t(paint::q4(95) - v * 5 * paint::kSub / 32768);
        }
        paint::polyline(px, lx, ly, 64, 13);
    }

    // --- GRAIN's SRC page: the sample, the window the grains start in, and the cloud ----------
    //
    // As the draft: the file's overview with the bracket bright and the rest dim, each grain in
    // the air a tick at its read position above and below the waveform, and under it the cloud:
    // each grain a spark that sinks as it fades, with an afterglow.
    static constexpr int kGrainMid = 46, kGrainAmp = 20, kCloudTop = 78, kCloudRows = 24;

    void draw_grain_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const WaveView view = engine_.status().wave[selected_].read();
        draw_overview_band(canvas, kGrainMid, kGrainAmp, view.lo, view.hi, 9, 4);
        if (view.lo < kWaveColumns && view.hi < kWaveColumns) {
            const int a = view.lo;
            const int b = view.hi > view.lo ? int(view.hi) : a + 1;
            const int y0 = kGrainMid - kGrainAmp - 3;
            const int h = 2 * kGrainAmp + 7;
            canvas.rect(a, y0, 1, h, 15);
            canvas.rect(b, y0, 1, h, 15);
            canvas.rect(a, y0, b - a, 1, 15);
            canvas.rect(a, y0 + h - 1, b - a, 1, 15);
        }
        grain_glow_.fade(184);
        int alive = 0;
        for (int i = 0; i < WaveView::kMarks; i++) {
            if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
            alive++;
            const int g = view.glow[i] > 15 ? 15 : view.glow[i];
            paint::plot_max(px, view.mark[i], kGrainMid - kGrainAmp - 1, g);
            paint::plot_max(px, view.mark[i], kGrainMid + kGrainAmp + 1, g);
            const int y = 2 + (i * 7) % 16 + (15 - g) / 3;
            grain_glow_.plot(view.mark[i], y, g);
        }
        canvas.rect(0, kCloudTop - 6, 120, 1, 2);
        char line[16];
        char *out = line;
        append(out, "GRAINS ");
        append_number(out, uint32_t(alive));
        append(out, "/8");
        *out = 0;
        paint::small_text(px, 0, kCloudTop - 4, line, 6);
        for (int y = 0; y < kCloudRows; y++) {
            for (int x = 0; x < 120; x++) {
                const int v = grain_glow_.level[y * 124 + x];
                if (v > 10) paint::dot(px, x, kCloudTop + y, v);
                else if (v > 1) paint::plot_max(px, x, kCloudTop + y, v);
            }
        }
        draw_no_file(canvas, kGrainMid);
    }

    // The overview as the draft draws it: each column's range about `mid`, the loudest filling
    // `amp`, columns lo..hi in `inside` and the rest in `outside` (all inside when there is none).
    void draw_overview_band(enaga::Canvas &canvas, int mid, int amp, int lo, int hi, int inside, int outside) {
        const WaveOverview &overview = storage_.overview(selected_);
        const int filled = overview.filled > kWaveColumns ? kWaveColumns : int(overview.filled);
        int peak = 8;
        for (int x = 0; x < filled; x++) {
            peak = -int(overview.lo[x]) > peak ? -int(overview.lo[x]) : peak;
            peak = int(overview.hi[x]) > peak ? int(overview.hi[x]) : peak;
        }
        const bool windowed = lo < kWaveColumns && hi < kWaveColumns;
        for (int x = 0; x < 120; x++) {
            if (x >= filled) {
                if (x % 3 == 0) canvas.pixel(x, mid, 3);
                continue;
            }
            const int up = int(overview.hi[x]) > 0 ? int(overview.hi[x]) * amp / peak : 0;
            const int down = int(overview.lo[x]) < 0 ? -int(overview.lo[x]) * amp / peak : 0;
            const bool in = !windowed || (x >= lo && x <= hi);
            canvas.rect(x, mid - up, 1, up + down + 1, uint8_t(in ? inside : outside));
        }
    }

    // No file of its own: it plays the placeholder, and says why.
    void draw_no_file(enaga::Canvas &canvas, int mid) {
        if (storage_.has_overview(selected_)) return;
        const char *why = storage_.loading_track() == selected_ ? "LOADING" : "NO FILE";
        canvas.rect(36, mid - 5, 47, 11, 0);
        canvas.text(39, mid - 3, why, 15);
    }

    // --- SAMPLE's SRC page: the file, the START..END window, the slices and the voices -------
    static constexpr int kSampleMid = 62, kSampleAmp = 28;

    void draw_sample_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const WaveView view = engine_.status().wave[selected_].read();
        draw_overview_band(canvas, kSampleMid, kSampleAmp, view.lo, view.hi, 9, 4);
        const int top = kSampleMid - kSampleAmp - 3;
        const int bottom = kSampleMid + kSampleAmp + 3;
        for (int x = 0; x < 120; x++) {
            if (!wave_has_cut(view.cuts, x)) continue;
            paint::dotted_v(px, x, top, bottom, 8, 2);
        }
        for (int i = 0; i < WaveView::kMarks; i++) {
            if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
            const int g = view.glow[i] > 15 ? 15 : view.glow[i];
            canvas.rect(view.mark[i], top, 1, bottom - top + 1, uint8_t(g));
            paint::dot(px, view.mark[i], kSampleMid, g);
        }
        if (view.lo < kWaveColumns && view.hi < kWaveColumns) {
            canvas.rect(view.lo, top, 1, bottom - top + 1, 15);
            canvas.rect(view.hi, top, 1, bottom - top + 1, 15);
            const char sta[2] = {knob_letter(edit_index(1)), 0};
            const char len[2] = {knob_letter(edit_index(2)), 0};
            if (sta[0] != 0) paint::small_badge(px, view.lo + 2 > 116 ? 116 : view.lo + 2, top, sta);
            if (len[0] != 0 && view.hi - 7 > view.lo + 8) paint::small_badge(px, view.hi - 5, top, len);
        }
        canvas.rect(0, bottom + 2, 120, 1, 3);
        draw_no_file(canvas, kSampleMid);
    }

    // --- the drums' SRC page: one hit, drawn from the knobs ------------------------------------
    //
    // What the machine plays for one trig, as a scope would show it, worked out from the knobs
    // with the machines' own curves (dsp/kick.cpp, snare.cpp, hat.cpp, perc.cpp), loosely: the
    // kick's pitch sweep, the snare's tone and noise and the clap's bursts, the hat's metal and
    // noise, the FM percussion's index falling. Its envelope dotted round it. After each trig the
    // part the note has played through lights up, in real time.
    void draw_hit_view(enaga::Canvas &canvas) {
        namespace ed = enaga::dsp;
        uint8_t *px = canvas.data();
        const MachineId m = MachineId(engine_.machine_id(selected_));
        const auto u = [&](int index) { return unit(live_value(index)); };
        constexpr int kMid = 62, kAmp = 30, kCols = 120, kSub = 4;
        constexpr float kTau = 6.2831853f;

        float f0 = 55.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, tau = 0.1f, window = 0.1f;
        switch (m) {
            case MachineId::kick:
                f0 = 25.0f * ed::exp2(2.263f * u(0));
                a1 = u(1);                                            // FMA
                a2 = 0.008f * ed::exp2(4.0f * u(2));                  // FMD
                a3 = u(3);                                            // CLK
                tau = 0.02f * ed::exp2(5.0f * u(8) * u(8));
                window = tau * 4.0f;
                break;
            case MachineId::snare: {
                f0 = 90.0f * ed::exp2(2.286f * u(0));
                a1 = u(1);                                            // SNP
                const uint8_t clp = live_value(3);
                a2 = clp < 43 ? 0.0f : (clp < 85 ? 3.0f : 4.0f);      // bursts
                a3 = 0.002f + 0.038f * u(9);                          // SPR
                tau = 0.015f * ed::exp2(6.0f * u(8) + u(10));
                window = tau * 4.0f + a2 * a3;
                break;
            }
            case MachineId::hat:
                f0 = 180.0f * ed::exp2(2.385f * u(0));
                a1 = u(2);                                            // NOI
                a2 = 0.25f + 1.5f * u(3);                             // DTN
                tau = 0.007f * ed::exp2(7.0f * u(8));
                window = tau * 4.0f;
                break;
            default:                                                  // PRC
                f0 = 48.0f * ed::exp2(5.246f * u(0));
                a1 = 0.5f + 7.5f * u(1);                              // RAT
                a2 = 12.566f * u(2) * u(2);                           // IDX, radians
                a3 = 0.002f * ed::exp2(7.64f * u(3));                 // IDD
                tau = 0.004f * ed::exp2(7.23f * u(8));
                window = tau * 4.0f;
                break;
        }
        window = window < 0.03f ? 0.03f : (window > 0.5f ? 0.5f : window);
        const float dt = window / float(kCols * kSub);
        const int age = ms_since_trigger(uint32_t(window * 1000.0f) + 50u);
        const int head = age < 0 ? -1 : int(float(age) * 0.001f / window * float(kCols));

        paint::dotted_h(px, 0, 119, kMid, 3, 3);
        float ph = 0.0f, ph2 = 0.0f;
        uint32_t noise = 0x2545F491u;
        static constexpr float kHatRatios[6] = {1.0f, 1.304f, 1.466f, 1.787f, 1.932f, 2.536f};
        float hat_ph[6] = {};
        for (int c = 0; c < kCols; c++) {
            float lo = 0.0f, hi = 0.0f, env_peak = 0.0f;
            for (int s = 0; s < kSub; s++) {
                const float t = float(c * kSub + s) * dt;
                noise = noise * 1664525u + 1013904223u;
                const float n = float(int32_t(noise) >> 16) * (1.0f / 32768.0f);
                float env = ed::exp2(-1.4427f * t / tau);
                float v = 0.0f;
                switch (m) {
                    case MachineId::kick: {
                        const float sweep = ed::exp2(-1.4427f * t / a2);
                        ph += f0 * (1.0f + 3.5f * a1 * sweep) * dt;
                        v = float(paint::sin_q12(int(ph * 256.0f) & 255)) * (1.0f / 4096.0f) * env;
                        if (t < 0.0015f) v += a3 * n;
                        break;
                    }
                    case MachineId::snare: {
                        const float start = a2 * a3;
                        float burst = 0.0f;
                        for (int k = 0; k < int(a2); k++) {
                            const float bt = t - float(k) * a3;
                            if (bt >= 0.0f) burst += ed::exp2(-1.4427f * bt / 0.006f);
                        }
                        const float body_t = t - start;
                        env = body_t < 0.0f ? 0.0f : ed::exp2(-1.4427f * body_t / tau);
                        ph += f0 * dt;
                        const float tone = float(paint::sin_q12(int(ph * 256.0f) & 255)) * (1.0f / 4096.0f);
                        const float tone_env = body_t < 0.0f ? 0.0f : ed::exp2(-1.4427f * body_t / (tau * 0.6f));
                        v = (1.0f - 0.7f * a1) * tone * tone_env + (0.3f + 0.7f * a1) * n * env + n * burst * 0.8f;
                        env = env > burst ? env : (burst > 1.0f ? 1.0f : burst);
                        break;
                    }
                    case MachineId::hat: {
                        float metal = 0.0f;
                        for (int k = 0; k < 6; k++) {
                            hat_ph[k] += f0 * (1.0f + (kHatRatios[k] - 1.0f) * a2) * dt;
                            metal += (hat_ph[k] - float(int(hat_ph[k]))) < 0.5f ? 1.0f : -1.0f;
                        }
                        v = ((1.0f - a1) * metal * (1.0f / 6.0f) + a1 * n) * env;
                        break;
                    }
                    default: {
                        const float idx = a2 * ed::exp2(-1.4427f * t / a3);
                        ph2 += f0 * a1 * dt;
                        ph += f0 * dt;
                        const float mod = float(paint::sin_q12(int(ph2 * 256.0f) & 255)) * (1.0f / 4096.0f);
                        const float turns = ph + idx * mod * (1.0f / kTau);
                        v = float(paint::sin_q12(int(turns * 256.0f) & 255)) * (1.0f / 4096.0f) * env;
                        break;
                    }
                }
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                if (s == 0 || v < lo) lo = v;
                if (s == 0 || v > hi) hi = v;
                if (env > env_peak) env_peak = env;
            }
            const int y0 = kMid - int(hi * float(kAmp) + 0.5f);
            const int y1 = kMid - int(lo * float(kAmp) - 0.5f);
            const int grey = head < 0 ? 11 : (c < head ? 15 : 6);
            canvas.rect(c, y0, 1, y1 - y0 + 1, uint8_t(grey));
            if (c % 2 == 0) {
                const int e = int(env_peak * float(kAmp) + 0.5f);
                paint::plot_max(px, c, kMid - e - 1, 5);
                paint::plot_max(px, c, kMid + e + 1, 5);
            }
        }
        if (head >= 0 && head < kCols) canvas.rect(head, kMid - kAmp - 2, 1, 2 * kAmp + 5, 9);
        char text[12];
        char *out = text;
        append_number(out, uint32_t(window * 1000.0f + 0.5f));
        append(out, "MS");
        *out = 0;
        paint::small_text(px, 120 - paint::small_width(text), kMid + kAmp + 3, text, 5);
    }

    // --- a synth's oscillators: VA's SRC and OSC pages, WAVE's OSC page ------------------------
    //
    // Two cycles of what the oscillators make before the filter, bright, over its parts dim: the
    // first oscillator (VA: the saw, or a pulse as WAV narrows it; WAVE: the frame POS plays, bent
    // by WRP), the second (OS2 or DET: a detuned copy that drifts against the first while the beat
    // plays, or OS2's synced pulse), the sub an octave down, all through the drive's soft clip.
    static constexpr int kOscMid = 62, kOscAmp = 28;

    static float frac(float x) { return x - float(int(x)) + (x < 0.0f ? 1.0f : 0.0f); }
    static float saw(float p) { return 2.0f * frac(p) - 1.0f; }

    float osc_first(float p, bool wave, uint8_t wav, uint8_t wrp, const int16_t *a, const int16_t *b,
                    int mix, int length) const {
        if (!wave) {
            if (wav < 32) return saw(p);
            const float pw = 0.5f - float(wav - 32) * (0.42f / 95.0f);
            return frac(p) < pw ? 1.0f : -1.0f;
        }
        float q = frac(p);
        const float bend = unit(wrp);
        q = (1.0f - bend) * q + bend * q * q * (3.0f - 2.0f * q);
        const int at = int(q * float(length)) % length;
        return (float(a[at]) * float(16 - mix) + float(b[at]) * float(mix)) * (1.0f / (16.0f * 32768.0f));
    }

    void draw_osc_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const uint8_t id = engine_.machine_id(selected_);
        const bool wave = machine_is_wave(id);
        const uint8_t b0 = live_value(kOscFirst + 0), b1 = live_value(kOscFirst + 1);
        const uint8_t sub = live_value(kOscFirst + 2);
        const uint8_t drv = wave ? uint8_t(0) : live_value(kOscFirst + 3);
        const int16_t *fa = nullptr;
        const int16_t *fb = nullptr;
        int mix = 0;
        int length = 1;
        if (wave) {
            const Status &status = engine_.status();
            const uint8_t table = status.heard_table[selected_].load(std::memory_order_relaxed);
            const WaveLibrary &waves = engine_.waves();
            if (table == Status::kNotHeard || !waves.resident(table)) {
                canvas.text(38, 56, "BUILDING", 7);
                return;
            }
            const int pos = int(status.heard_pos[selected_].load(std::memory_order_relaxed)) * (kWaveFrames - 1) * 16 / 127;
            const int mip = kWaveMipLevels - 1;
            length = wave_mip_length(mip);
            fa = waves.frame(table, pos / 16, mip);
            fb = waves.frame(table, pos / 16 + 1 < kWaveFrames ? pos / 16 + 1 : pos / 16, mip);
            mix = pos % 16;
        }
        // The second oscillator: WAVE's DET (0 off, else cents), VA's OS2 (1-63 cents, 64+ sync).
        const uint8_t second = b1;
        const bool sync = !wave && second >= 64;
        const float cents = sync ? 0.0f : float(second) * (60.0f / 63.0f);
        const float ratio = sync ? enaga::dsp::exp2(float(second - 64) * (2.0f / 63.0f)) : 1.0f;
        // The drift: a detuned copy slides against the first while the beat plays; still, a
        // fixed offset shows the two.
        const bool running = enaga::clock::state().running != 0;
        const float drift = running ? frac(float(last_ms_) * cents * 0.00004f) : 0.3f;
        const float sub_level = unit(sub);
        const float drive = unit(drv);

        paint::dotted_h(px, 0, 119, kOscMid, 3, 3);
        paint::dotted_v(px, 61, kOscMid - kOscAmp - 2, kOscMid + kOscAmp + 2, 3, 3);
        int16_t xs[121], y1[121], y2[121], y3[121], ym[121];
        for (int i = 0; i <= 120; i++) {
            const float p = float(i) * (1.0f / 60.0f);               // two cycles across
            const float o1 = osc_first(p, wave, b0, b0, fa, fb, mix, length);
            float o2 = 0.0f;
            if (sync) o2 = saw(frac(p) * ratio);
            else if (second > 0) o2 = wave ? osc_first(p + drift, true, b0, b0, fa, fb, mix, length) : saw(p + drift);
            const float o3 = frac(p * 0.5f) < 0.5f ? 1.0f : -1.0f;
            const float weight = 1.0f + (second > 0 ? 0.8f : 0.0f) + 0.8f * sub_level;
            float m = (o1 + (second > 0 ? 0.8f * o2 : 0.0f) + 0.8f * sub_level * o3) / weight;
            if (drive > 0.0f) {
                const float k = 3.0f * drive;
                const float a = m < 0.0f ? -m : m;
                m = m * (1.0f + k) / (1.0f + a * k);
            }
            xs[i] = int16_t(paint::q4(i));
            y1[i] = int16_t(paint::q4(kOscMid) - int(o1 * float(kOscAmp) * 0.5f * 16.0f));
            y2[i] = int16_t(paint::q4(kOscMid) - int(o2 * float(kOscAmp) * 0.5f * 16.0f));
            y3[i] = int16_t(paint::q4(kOscMid) - int(o3 * sub_level * float(kOscAmp) * 0.5f * 16.0f));
            ym[i] = int16_t(paint::q4(kOscMid) - int(m * float(kOscAmp) * 16.0f));
        }
        paint::polyline(px, xs, y1, 121, 4);
        if (second > 0) paint::polyline(px, xs, y2, 121, 4);
        if (sub_level > 0.0f) paint::polyline(px, xs, y3, 121, 3);
        paint::polyline(px, xs, ym, 121, 15);
        // What is in it, under the picture.
        char line[32];
        char *out = line;
        append(out, wave ? "FRAME" : (b0 < 32 ? "SAW" : "PULSE"));
        if (second > 0) append(out, sync ? " + SYNC" : " + DETUNE");
        if (sub_level > 0.0f) append(out, " + SUB");
        if (drive > 0.0f) append(out, " DRIVEN");
        *out = 0;
        paint::small_text(px, 0, kOscMid + kOscAmp + 4, line, 7);
    }

    // --- FM4's SRC page: the algorithm, and what it plays ---------------------------------------
    //
    // The operators as boxes wired as ALG wires them (dsp/fm.cpp's eight), carriers on the bus at
    // the bottom, op 4's feedback a loop when FBK is up; beside it two cycles of a carrier under
    // its modulator at IDX and RAT. After each note the index falls as IEN lets it, and the
    // modulators glow with it.
    struct FmLayout {
        uint8_t links;           // bits: 0 m12, 1 m13, 2 m14, 3 m23, 4 m24, 5 m34 (B into A)
        uint8_t carriers;        // bit k: op k + 1 is heard
        int8_t x[4], y[4];       // each op's box, top-left
    };
    static constexpr FmLayout kFmLayouts[8] = {
        {0x01 | 0x08 | 0x20, 0x1, {20, 20, 20, 20}, {72, 58, 44, 30}},
        {0x01 | 0x08 | 0x10, 0x1, {20, 20, 6, 34}, {72, 58, 44, 44}},
        {0x01 | 0x04 | 0x08, 0x1, {20, 6, 6, 34}, {72, 58, 44, 58}},
        {0x01 | 0x02 | 0x20, 0x1, {20, 34, 6, 6}, {72, 58, 58, 44}},
        {0x01 | 0x20, 0x5, {6, 6, 34, 34}, {72, 58, 72, 58}},
        {0x04 | 0x10 | 0x20, 0x7, {2, 20, 38, 20}, {72, 72, 72, 58}},
        {0x20, 0x7, {2, 20, 38, 38}, {72, 72, 72, 58}},
        {0x00, 0xF, {0, 13, 26, 39}, {72, 72, 72, 72}},
    };

    void draw_fm_view(enaga::Canvas &canvas) {
        namespace ed = enaga::dsp;
        uint8_t *px = canvas.data();
        const int alg = int(live_value(edit_index(0))) * 8 / 128;
        const FmLayout &l = kFmLayouts[alg < 0 ? 0 : (alg > 7 ? 7 : alg)];
        const float idx = unit(live_value(edit_index(2)));
        const float fbk = unit(live_value(edit_index(3)));
        const int age = ms_since_trigger(1500);
        const float fall = age < 0 ? 1.0f : 0.25f + 0.75f * ed::exp2(-float(age) * (1.4427f / 180.0f));
        const float now = idx * (age < 0 ? 1.0f : fall);
        // Wires first, then boxes over their ends.
        static constexpr int kFrom[6] = {1, 2, 3, 2, 3, 3};          // modulator (0-based)
        static constexpr int kTo[6] = {0, 0, 0, 1, 1, 2};            // and what it modulates
        for (int k = 0; k < 6; k++) {
            if (((l.links >> k) & 1u) == 0) continue;
            const int a = kFrom[k], b = kTo[k];
            paint::line(px, paint::q4(l.x[a] + 5), paint::q4(l.y[a] + 9), paint::q4(l.x[b] + 5), paint::q4(l.y[b]), 8);
        }
        int bus_lo = 120, bus_hi = 0;
        for (int op = 0; op < 4; op++) {
            if (((l.carriers >> op) & 1u) == 0) continue;
            canvas.rect(l.x[op] + 5, l.y[op] + 9, 1, 4, 8);
            bus_lo = l.x[op] + 5 < bus_lo ? l.x[op] + 5 : bus_lo;
            bus_hi = l.x[op] + 5 > bus_hi ? l.x[op] + 5 : bus_hi;
        }
        canvas.rect(bus_lo, l.y[0] + 13, bus_hi - bus_lo + 1, 1, 8);
        paint::small_text(px, bus_lo, l.y[0] + 15, "OUT", 6);
        for (int op = 0; op < 4; op++) {
            const bool carrier = ((l.carriers >> op) & 1u) != 0;
            const int glow = carrier ? 12 : 4 + int(now * 11.0f);
            frame_rect(px, l.x[op], l.y[op], 11, 9, glow > 15 ? 15 : glow);
            const char n[2] = {char('1' + op), 0};
            paint::small_text(px, l.x[op] + 4, l.y[op] + 2, n, carrier ? 15 : 9);
        }
        if (fbk > 0.0f) {                                              // op 4 into itself
            const int x = l.x[3] + 11, y = l.y[3] + 4;
            const int g = 4 + int(fbk * 11.0f);
            canvas.rect(x, y, 3, 1, uint8_t(g));
            canvas.rect(x + 2, y - 7, 1, 7, uint8_t(g));
            canvas.rect(l.x[3] + 5, y - 7, x - l.x[3] - 3, 1, uint8_t(g));
            canvas.rect(l.x[3] + 5, y - 7, 1, 3, uint8_t(g));
        }

        // What it plays: two cycles of the carrier under one modulator at RAT, the index now.
        constexpr int kX0 = 60, kMid = 58, kAmp = 24;
        const int rat = int(live_value(edit_index(1))) * 9 / 128;
        static constexpr float kRatios[9] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        const float r = kRatios[rat < 0 ? 0 : (rat > 8 ? 8 : rat)];
        paint::dotted_h(px, kX0, 119, kMid, 3, 3);
        int16_t xs[61], ys[61];
        for (int i = 0; i <= 60; i++) {
            const float p = float(i) * (1.0f / 30.0f);
            const float mod = float(paint::sin_q12(int(p * r * 256.0f) & 255)) * (1.0f / 4096.0f);
            const float turns = p + now * 0.9f * mod;
            const float v = float(paint::sin_q12(int(turns * 256.0f) & 255)) * (1.0f / 4096.0f);
            xs[i] = int16_t(paint::q4(kX0 + i));
            ys[i] = int16_t(paint::q4(kMid) - int(v * float(kAmp) * 16.0f));
        }
        paint::polyline(px, xs, ys, 61, 15);
    }

    // --- FM4's OSC page: the ratios on a harmonic axis ----------------------------------------
    //
    // Harmonics 0-8 across; the carrier at CAR tall and bright, the modulator at RAT nudged by
    // FIN, the detuned pair's second carrier a hair to its right (DET, drawn ten times wider than
    // it is), and the sidebands the index puts either side of the carrier, falling away.
    void draw_ratio_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const TrackParams &kit = engine_.params(selected_);
        constexpr int kBase = 92, kTall = 56;
        const auto x_of = [](float h) { return 4 + int(h * 14.0f + 0.5f); };
        for (int h = 0; h <= 8; h++) {
            canvas.rect(x_of(float(h)), kBase + 1, 1, 2, 5);
            const char n[2] = {char('0' + h), 0};
            paint::small_text(px, x_of(float(h)) - 1, kBase + 4, n, 5);
        }
        canvas.rect(0, kBase, 120, 1, 6);
        static constexpr float kCar[5] = {1.0f, 0.5f, 2.0f, 3.0f, 4.0f};
        const int car_zone = int(live_value(kOscFirst + 3)) * 5 / 128;
        const float car = kCar[car_zone < 0 ? 0 : (car_zone > 4 ? 4 : car_zone)];
        const int rat = int(kit.v[1]) * 9 / 128;
        static constexpr float kRatios[9] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        const float fin = (float(live_value(kOscFirst + 1)) - 64.0f) * (0.5f / 64.0f);
        const float mod = kRatios[rat < 0 ? 0 : (rat > 8 ? 8 : rat)] + fin;
        const float idx = unit(kit.v[2]);
        // Sidebands at car +- k mod, heights falling as idx^k / k!.
        float h = 1.0f;
        for (int k = 1; k <= 4; k++) {
            h = h * idx * 1.6f / float(k);
            const int rows = int(h * float(kTall) * 0.8f);
            if (rows < 1) break;
            const float up = car + float(k) * mod, down = car - float(k) * mod;
            if (up <= 8.4f) canvas.rect(x_of(up), kBase - rows, 1, rows, 6);
            if (down >= 0.0f) canvas.rect(x_of(down), kBase - rows, 1, rows, 6);
        }
        canvas.rect(x_of(car), kBase - kTall, 2, kTall, 15);
        const float det = float(live_value(kOscFirst + 0)) * (25.0f / 127.0f) * 0.006f * 10.0f;
        if (det > 0.0f) canvas.rect(x_of(car + det) + 2, kBase - kTall + 6, 1, kTall - 6, 9);
        canvas.rect(x_of(mod), kBase - kTall * 2 / 3, 2, kTall * 2 / 3, 10);
        paint::small_text(px, x_of(car) + 4, kBase - kTall, "CAR", 15);
        paint::small_text(px, x_of(mod) + 4 > 108 ? x_of(mod) - 14 : x_of(mod) + 4, kBase - kTall * 2 / 3, "MOD", 10);
    }

    // --- GRAIN's OSC page: the scan and the pitch spray -----------------------------------------
    //
    // The file small, the grains' window in it with an arrow as far as SCN carries it; under it
    // the spray: a fan from the note out to the pitches PSP scatters grains over (cents, or
    // octaves past the middle), the grains in the air sparking on it.
    void draw_scan_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const WaveView view = engine_.status().wave[selected_].read();
        constexpr int kMid = 42, kAmp = 14;
        draw_overview_band(canvas, kMid, kAmp, view.lo, view.hi, 9, 4);
        const int scn = live_value(kOscFirst + 0);
        if (view.lo < kWaveColumns) {
            const int from = view.lo;
            const int reach = (scn - 64) * 50 / 64;                    // either way from the middle
            const int to = from + reach < 0 ? 0 : (from + reach > 119 ? 119 : from + reach);
            const int y = kMid + kAmp + 5;
            if (to != from) {
                canvas.rect(to > from ? from : to, y, (to > from ? to - from : from - to) + 1, 1, 12);
                const int dir = to > from ? -1 : 1;
                paint::plot(px, to + dir, y - 1, 12);
                paint::plot(px, to + dir, y + 1, 12);
                paint::plot(px, to + 2 * dir, y - 2, 9);
                paint::plot(px, to + 2 * dir, y + 2, 9);
            }
            canvas.rect(from, kMid - kAmp - 2, 1, 2 * kAmp + 5, 15);
        }
        // The spray fan.
        constexpr int kFanY = 84, kFanX = 8;
        const int psp = live_value(kOscFirst + 1);
        const int spread = psp < 64 ? psp * 6 / 63 : 6 + (psp - 64) * 10 / 63;   // rows either way
        paint::dotted_h(px, 0, 119, kFanY, 4, 2);
        for (int k = -2; k <= 2; k++) {
            if (k == 0 || spread == 0) continue;
            paint::line(px, paint::q4(kFanX), paint::q4(kFanY), paint::q4(119), paint::q4(kFanY + k * spread / 2), 5);
        }
        for (int i = 0; i < WaveView::kMarks; i++) {
            if (view.mark[i] >= kWaveColumns || view.glow[i] == 0) continue;
            const int lane = (i * 5) % 5 - 2;
            const int x = view.mark[i] < kFanX ? kFanX : view.mark[i];
            const int y = kFanY + lane * spread * (x - kFanX) / (2 * (119 - kFanX));
            paint::dot(px, x, y, view.glow[i] > 15 ? 15 : view.glow[i]);
        }
        paint::small_text(px, 0, kFanY - 9, "PITCH", 5);
        draw_no_file(canvas, kMid);
    }

    // --- FX: the insert chain's lo-fi, the pan and the two sends --------------------------------
    //
    // A sine and what LOF makes of it (fewer levels, held longer); the stereo field as an arc with
    // the track a dot where PAN puts it, swelling with the track's level; SNA and SNB as two
    // columns into the delay and the reverb, their tops lit while the track sounds.
    void draw_fx_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const float lof = unit(live_value(edit_index(0)));
        const int pan = live_value(edit_index(1));
        const int sna = live_value(edit_index(2));
        const int snb = live_value(edit_index(3));
        const bool running = enaga::clock::state().running != 0;
        const uint32_t level = running ? engine_.status().track_milli[selected_].load(std::memory_order_relaxed) : 0u;
        const int lit = paint::level_rows(level, 8, 36);

        // Lo-fi, rows 28-58.
        constexpr int kMid = 43, kAmp = 13;
        const int levels = 2 + int((1.0f - lof) * (1.0f - lof) * 30.0f);
        const int hold = 1 + int(lof * 7.0f);
        paint::dotted_h(px, 0, 119, kMid, 3, 4);
        int16_t xs[120], clean[120], crushed[120];
        for (int x = 0; x < 120; x++) {
            const int held = x - x % hold;
            const int a = paint::sin_q12(x * 256 / 60 & 255);
            const int b = paint::sin_q12(held * 256 / 60 & 255);
            const int q = lof <= 0.0f ? b : (b * levels / 4096) * 4096 / levels;
            xs[x] = int16_t(paint::q4(x));
            clean[x] = int16_t(paint::q4(kMid) - a * kAmp * paint::kSub / 4096);
            crushed[x] = int16_t(paint::q4(kMid) - q * kAmp * paint::kSub / 4096);
        }
        paint::polyline(px, xs, clean, 120, 4);
        if (lof > 0.0f) {
            for (int x = 1; x < 120; x++) {
                const int y0 = crushed[x - 1] >> 4, y1 = crushed[x] >> 4;
                canvas.rect(x, y0 < y1 ? y0 : y1, 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, 13);
            }
        } else {
            paint::polyline(px, xs, crushed, 120, 13);
        }

        // The pan field, centre (36, 96), radius 24.
        constexpr int kCx = 36, kCy = 96, kR = 24;
        paint::arc(px, kCx, kCy, kR, 128, 128, 4);
        for (int k = 0; k <= 4; k++) {
            const int at = 128 + k * 32;
            paint::plot(px, kCx + ((kR + 2) * paint::cos_q12(at) >> 12), kCy + ((kR + 2) * paint::sin_q12(at) >> 12), k == 2 ? 9 : 5);
        }
        const int at = 128 + pan * 128 / 127;
        const int dx = kCx + (kR * paint::cos_q12(at) >> 12);
        const int dy = kCy + (kR * paint::sin_q12(at) >> 12);
        paint::line(px, paint::q4(kCx), paint::q4(kCy), paint::q4(dx), paint::q4(dy), 8);
        paint::dot(px, dx, dy, 15);
        if (lit > 3) {
            for (int k = 0; k < 256; k += 32) {
                paint::plot_max(px, dx + (3 * paint::cos_q12(k) >> 12), dy + (3 * paint::sin_q12(k) >> 12), 4 + lit);
            }
        }
        paint::small_text(px, kCx - kR - 6, kCy - 4, "L", 5);
        paint::small_text(px, kCx + kR + 4, kCy - 4, "R", 5);

        // The sends.
        constexpr int kTop = 66, kRows = 30;
        static const char *const kNames[2] = {"DLY", "REV"};
        const int sends[2] = {sna, snb};
        for (int s = 0; s < 2; s++) {
            const int x = 82 + s * 20;
            frame_rect(px, x, kTop, 13, kRows + 2, 3);
            const int rows = sends[s] * kRows / 127;
            if (rows > 0) canvas.rect(x + 2, kTop + 1 + kRows - rows, 9, rows, 9);
            if (rows > 0 && lit > 0) canvas.rect(x + 2, kTop + 1 + kRows - rows, 9, rows * lit / 8 > 0 ? (rows * lit / 8 < rows ? rows * lit / 8 : rows) : 1, 15);
            paint::small_text(px, x + 1, kTop + kRows + 4, kNames[s], 6);
        }
    }

    // --- ARP: the run as a piano roll ----------------------------------------------------------
    //
    // Sixteen notes of the run the arp would play over a three-note chord, MOD's order through
    // OCT octaves, each as long as GAT makes it; while the beat plays, the note it is on lit.
    void draw_arp_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const TrackParams &kit = engine_.params(selected_);
        const uint8_t *arp = &kit.v[kArpFirst];
        const int mode = arp_mode_index(arp[0]);
        const int octaves = arp_octaves(arp[2]);
        const int gate = arp_gate_percent(arp[3]);
        const int notes = 3 * octaves;
        constexpr int kTop = 30, kBottom = 92, kCell = 7, kX0 = 4;
        // The run, as indexes into the chord's notes, low to high.
        int run[24];
        int n = 0;
        switch (mode) {
            case 2: for (int i = notes - 1; i >= 0; i--) run[n++] = i; break;               // DN
            case 3:                                                                         // U/D
                for (int i = 0; i < notes; i++) run[n++] = i;
                for (int i = notes - 2; i > 0; i--) run[n++] = i;
                break;
            case 5: {                                                                       // RND
                uint32_t seed = 0x9E3779B9u;
                for (int i = 0; i < 16; i++) { seed = seed * 1664525u + 1013904223u; run[n++] = int(seed >> 16) % notes; }
                break;
            }
            default: for (int i = 0; i < notes; i++) run[n++] = i; break;                   // UP, ORD, OFF
        }
        if (n == 0) { run[0] = 0; n = 1; }
        for (int i = 0; i < 16; i += 4) paint::dotted_v(px, kX0 + i * kCell - 1, kTop, kBottom, 3, 2);
        for (int o = 1; o < octaves; o++) {
            paint::dotted_h(px, 0, 119, kBottom - o * 3 * (kBottom - kTop) / (notes > 1 ? notes - 1 : 1), 3, 3);
        }
        int current = -1;
        if (mode != 0 && enaga::clock::state().running != 0) {
            const uint32_t ticks = arp_rate_ticks(arp_rate_index(arp[1]));
            const uint32_t into = engine_.status().pattern_into.load(std::memory_order_relaxed);
            if (ticks > 0) current = int(into / ticks % 16u);
        }
        const int width = (kCell - 1) * gate / 100 < 1 ? 1 : (kCell - 1) * gate / 100;
        for (int i = 0; i < 16; i++) {
            const int k = run[i % n];
            const int y = kBottom - 2 - k * (kBottom - kTop - 4) / (notes > 1 ? notes - 1 : 1);
            const int grey = mode == 0 ? 3 : (i == current ? 15 : 9);
            canvas.rect(kX0 + i * kCell, y, width + 1, 3, uint8_t(grey));
            if (i == current) paint::dot(px, kX0 + i * kCell + width / 2, y + 1, 15);
        }
        canvas.rect(0, kBottom + 1, 120, 1, 6);
        paint::small_text(px, 0, kBottom + 4, mode == 0 ? "OFF" : arp_rate_name(arp[1]), mode == 0 ? 5 : 9);
    }

    // --- a page with no picture: the four knobs as long bars ------------------------------------
    void draw_bars_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        bool any = false;
        for (int slot = 0; slot < 4; slot++) {
            const int index = edit_index(slot);
            any = any || (index >= 0 && param_name_for(index)[0] != '-');
        }
        if (!any) {
            // A page this machine does not have: its name, and which machines do.
            const bool asr = Page(param_page_) == Page::asr;
            canvas.text(44, 52, asr ? "NO ASR" : "NO OSC", 6);
            paint::small_text(px, 18, 66, "ON VA, WAVE, FM4, GRAIN", 5);
            return;
        }
        for (int slot = 0; slot < 4; slot++) {
            const int index = edit_index(slot);
            const ShownParam shown = shown_param(index);
            const int y = 32 + slot * 16;
            const char letter[2] = {char('A' + slot), 0};
            paint::small_text(px, 0, y, letter, 6);
            const bool empty = index < 0 || param_name_for(index)[0] == '-';
            canvas.rect(8, y + 1, 112, 3, 2);
            if (!empty && shown.heard > 0) canvas.rect(8, y + 1, shown.heard * 112 / 127, 3, shown.locked ? 15 : 11);
        }
    }
