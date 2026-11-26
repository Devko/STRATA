// EDIT: the selected track's sound four parameters at a time, and its pictures (waveforms and
// envelopes).
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, so it is not a header of its own: no #pragma once and no includes, and it sees
// every member the class has. Split out of app/app.hpp without changing a line of it.

    // EDIT's screen is app/ui_editview.hpp; what is here are the pieces it draws from.

    // --- EDIT's pictures: the waveform and the envelopes -------------------
    //
    // docs/CONTROLS.md, "Waveforms and envelopes". Under the four knobs, rows 50-117, x 0-123
    // (the frame's meter has 126-127): SAMPLE's and GRAIN's sample on their SRC page, and the
    // envelope a page's knobs shape on AMP, ASR and a synth's FLTR. Nothing here walks sample
    // memory: the waveform comes from the loader's overview (core/overview.hpp, 250 bytes) and
    // the audio side's WaveView in Status, the envelopes from the knobs alone. One pass of at
    // most kWaveColumns columns a frame.

    enum class EditPicture : uint8_t { none, wave, amp, filter, table, response };

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

    // LFO TRIG on this track: the kit's byte, which only the audio side writes, by command.
    LfoTrig lfo_trig_of(int track) const {
        return lfo_trig(engine_.project().kit.tracks[track].lfo_trig);
    }

    // SLC AUTO on this track: the kit's byte, which only the audio side writes, by command.
    bool slices_auto(int track) const {
        return engine_.project().kit.tracks[track].slice_mode == kSliceAuto;
    }
