// MIXER as the drafts draw their screens (docs/SCREENS.md, "MIXER"): the faders as an eight-strip
// desk, the duck as bars that dip with the source, the sends as pictures of what they do (the
// delay's echoes, the reverb's tail, the routing), MST as the draft's master page, and the kill
// EQ as a curve over the output's spectrum.
//
// Part of class Strata. app/app.hpp includes this file INSIDE the class body, in the public
// section, like the other ui_*.hpp parts: no #pragma once and no includes, and it sees every
// member the class has.
//
// Rows as EDIT's (app/ui_editview.hpp): the knob line at 18 (A B C), the picture 26-101, the
// footer at 105 (D) and 113. The desk and the duck's depths have no knob line: each strip
// carries its knob's letter.

    void draw_mixer(enaga::Canvas &canvas) {
        if (mixer_bank_ < kMixerTrackPages) { draw_desk(canvas, DeskKind::faders); return; }
        if (mixer_bank_ < kMixerDuckPage) { draw_desk(canvas, DeskKind::duck); return; }
        if (mixer_width_page(mixer_bank_)) { draw_desk(canvas, DeskKind::width); return; }
        if (mixer_chorus_send_page(mixer_bank_)) { draw_desk(canvas, DeskKind::chorus); return; }
        if (kMixerPages[mixer_bank_].master && mixer_bank_ != kMixerEqPage) { draw_master_view(canvas); return; }

        draw_knob_line(canvas, kKnobY, 0x7u);
        if (mixer_bank_ == kMixerDuckPage) draw_duck_view(canvas);
        else if (mixer_bank_ == kMixerDelayPage || mixer_bank_ == kMixerLongPage) draw_echo_view(canvas, mixer_bank_ == kMixerLongPage);
        else if (mixer_bank_ == kMixerEqPage) draw_eq_view(canvas);
        else if (mixer_bank_ == kMixerChorusPage) draw_chorus_view(canvas);
        else if (kMixerPages[mixer_bank_].name[0] == 'R' && kMixerPages[mixer_bank_].name[1] == 'E') {
            draw_reverb_view(canvas, kMixerPages[mixer_bank_].name[3] == '+');
        }
        else draw_route_view(canvas);
        const int end = draw_knob_line(canvas, kFootY, 0x8u);
        char line[32];
        mixer_state_text(line);
        const int w = paint::small_width(line);
        if (w > 0 && kPicRight - w >= end + 5) paint::small_text(canvas.data(), kPicRight - w + 1, kFootY, line, 8);
        mixer_hint_text(line);
        if (line[0] != 0) paint::small_text(canvas.data(), 0, kFoot2Y, line, 10);
    }

    // A MIXER knob's name and value for the knob line (knob_text in app/ui_editview.hpp).
    void mixer_knob_parts(int slot, char *name, char *value, bool &empty) {
        char *n = name;
        char *v = value;
        empty = false;
        if (mixer_bank_ == kMixerDuckPage) {
            static const char *const names[4] = {"SRC", "ATK", "HLD", "REL"};
            const KitDuck &duck = engine_.project().kit.duck;
            append(n, names[slot]);
            if (slot == 0) { *v++ = 'T'; append_number(v, uint32_t(duck.source) + 1u); }
            else {
                append_number(v, slot == 1 ? uint32_t(duck.attack_ms)
                                           : uint32_t(slot == 2 ? duck.hold : duck.release) * uint32_t(kDuckTimeStepMs));
                append(v, "MS");
            }
        } else {
            const int8_t field = kMixerPages[mixer_bank_].field[slot];
            if (field < 0) {
                append(n, "---");
                empty = true;
            } else {
                append(n, kMixerPages[mixer_bank_].master ? master_field_name(uint8_t(field))
                                                          : send_field_name(uint8_t(field)));
                append_mixer_value(v, mixer_bank_, slot);
            }
        }
        *n = 0;
        *v = 0;
    }

    void mixer_state_text(char *line) {
        char *out = line;
        if (mixer_bank_ == kMixerDuckPage) {
            append(out, "DUCKS UNDER T");
            append_number(out, uint32_t(engine_.project().kit.duck.source) + 1u);
        }
        *out = 0;
    }

    void mixer_hint_text(char *line) {
        char *out = line;
        const SendSettings &sends = engine_.sends();
        if (mixer_bank_ == kMixerEqPage) append(out, "PUSH A B C: KILL");
        else if (mixer_bank_ == kMixerLongPage && !sends.delay_long) append(out, "PUSH A: LONG DELAY");
        else if (mixer_bank_ == kMixerDuckPage) append(out, "TRACKS DUCK ON DCK 1-4 5-8");
        else if (mixer_bank_ == kMixerChorusPage) append(out, "TRACKS SEND ON CHO 1-4 5-8");
        else if (kMixerPages[mixer_bank_].name[0] == 'R' && kMixerPages[mixer_bank_].name[3] == '+' && !sends.reverb_hall) {
            append(out, "PUSH A: HALL");
        }
        *out = 0;
    }

    // --- the desk: eight strips --------------------------------------------------------------
    //
    // Fifteen pixels a track: its knob's letter lit over the four the page's knobs turn (the
    // others' numbers dim), the pan as a tick on a short line, the two sends as two thin bars,
    // then the meter (Status::track_milli, the top 36 dB, a peak held a second) beside the fader's
    // slot and its cap at the level, and under them the level and the machine. A muted track's
    // strip is dim and its cap hollow. DeskKind::duck: DCK 1-4 / 5-8, where each strip's bar is
    // how far the track ducks, hanging from the top, and dips with the source while the beat
    // plays. DeskKind::chorus: CHO 1-4 / 5-8, where it is the track's chorus send, standing up
    // from the bottom as a fader does.
    static constexpr int kDeskTop = 36, kDeskRows = 62;
    enum class DeskKind : uint8_t { faders, duck, chorus, width };

    void draw_desk(enaga::Canvas &canvas, DeskKind kind) {
        uint8_t *px = canvas.data();
        const Status &status = engine_.status();
        const bool running = enaga::clock::state().running != 0;
        const bool duck = kind == DeskKind::duck;
        const int bank = duck ? mixer_bank_ - kMixerDuckDepthPage
                              : (kind == DeskKind::chorus ? mixer_bank_ - kMixerChorusSendPage
                                 : (kind == DeskKind::width ? mixer_bank_ - kMixerWidthPage
                                                            : mixer_bank_));
        const Kit &kit = engine_.project().kit;
        const int source = kit.duck.source < kTracks ? int(kit.duck.source) : 0;
        const float dip = duck ? duck_gain_now(source) : 0.0f;
        const int bottom = kDeskTop + kDeskRows - 1;
        for (int t = 0; t < kTracks; t++) {
            const int x = t * 15;
            const bool active = t / 4 == bank;
            const bool muted = muted_[t];
            char text[12];
            if (active) {
                const char letter[2] = {char('A' + t % 4), 0};
                paint::small_badge(px, x + 5, 20, letter);
            } else {
                const char n[2] = {char('1' + t), 0};
                paint::small_text(px, x + 5, 20, n, 7);
            }
            const TrackParams &p = engine_.params(t);
            const int fx = page_first(Page::fx);
            // The pan: a tick on a line.
            canvas.rect(x + 1, 28, 11, 1, 3);
            canvas.rect(x + 1 + p.v[fx + 1] * 10 / 127, 27, 1, 3, uint8_t(active ? 13 : 7));
            // The sends: two thin bars.
            canvas.rect(x + 1, 31, 11, 1, 2);
            canvas.rect(x + 1, 33, 11, 1, 2);
            if (p.v[fx + 2] > 0) canvas.rect(x + 1, 31, 1 + p.v[fx + 2] * 10 / 127, 1, 8);
            if (p.v[fx + 3] > 0) canvas.rect(x + 1, 33, 1 + p.v[fx + 3] * 10 / 127, 1, 8);

            // The meter, x + 1 .. x + 5, every other row.
            const uint32_t level = running ? status.track_milli[t].load(std::memory_order_relaxed) : 0u;
            const int lit = paint::level_rows(level, kDeskRows, 36);
            if (!running || lit >= desk_hold_[t] || uint32_t(last_ms_ - desk_hold_ms_[t]) >= 1000u) {
                desk_hold_[t] = uint8_t(lit);
                desk_hold_ms_[t] = last_ms_;
            }
            for (int r = 0; r < lit; r += 2) {
                const uint8_t g = muted ? 4 : (r >= kDeskRows - kDeskRows / 4 ? 15 : 9);
                canvas.rect(x + 1, bottom - r, 5, 1, g);
            }
            if (desk_hold_[t] > 1) canvas.rect(x + 1, bottom - desk_hold_[t] + 1, 5, 1, muted ? 5 : 15);
            canvas.rect(x + 1, bottom + 1, 5, 1, 3);

            if (kind == DeskKind::width) {
                // The width, standing up from the bottom; unison's half above a tick at 64 lit
                // brighter. A track whose machine does not read it shows dashes.
                const bool poly = machine_is_poly(engine_.machine_id(t));
                const int width = int(width_sanitized(kit.tracks[t].width));
                const int rows = width * kDeskRows / int(kWidthMax);
                const int zone = int(kWidthUnison) * kDeskRows / int(kWidthMax);
                canvas.rect(x + 8, kDeskTop, 5, kDeskRows, 2);
                canvas.rect(x + 7, bottom - zone + 1, 7, 1, 6);
                if (poly && rows > 0) {
                    const int low = rows < zone ? rows : zone;
                    canvas.rect(x + 8, bottom - low + 1, 5, low, uint8_t(active ? 9 : 5));
                    if (rows > zone) {
                        canvas.rect(x + 8, bottom - rows + 1, 5, rows - zone, uint8_t(active ? 15 : 7));
                    }
                }
                char *out = text;
                if (!poly) append(out, "---");
                else if (width == 0) append(out, "OFF");
                else if (width >= int(kWidthUnison)) append(out, "UNI");
                else append_number(out, uint32_t(width));
                *out = 0;
            } else if (kind == DeskKind::chorus) {
                // The chorus send, standing up from the bottom.
                const int send = int(chorus_send_sanitized(kit.tracks[t].chorus));
                const int rows = send * kDeskRows / int(kChorusSendMax);
                canvas.rect(x + 8, kDeskTop, 5, kDeskRows, 2);
                if (rows > 0) canvas.rect(x + 8, bottom - rows + 1, 5, rows, uint8_t(active ? 12 : 6));
                char *out = text;
                if (send == 0) append(out, "OFF");
                else append_number(out, uint32_t(send));
                *out = 0;
            } else if (!duck) {
                // The fader: its slot and the cap at the level (0-1.2 over the rows).
                const float gain = engine_.level(t);
                int at = int(gain * float(kDeskRows - 4) / 1.2f);
                at = at < 0 ? 0 : (at > kDeskRows - 4 ? kDeskRows - 4 : at);
                canvas.rect(x + 10, kDeskTop, 1, kDeskRows, 3);
                const int cy = bottom - at - 2;
                canvas.rect(x + 8, cy, 5, 3, uint8_t(active ? 15 : 8));
                if (muted) canvas.rect(x + 9, cy + 1, 3, 1, 0);
                char *out = text;
                append_padded(out, uint32_t(gain * 100.0f + 0.5f), 3);
                *out = 0;
            } else {
                // The duck's depth, hanging from the top; while the source plays, the part it is
                // ducking now lit over it.
                const int depth = t == source ? 0 : int(kit.tracks[t].duck);
                const int rows = depth * kDeskRows / int(kDuckDepthMax);
                canvas.rect(x + 8, kDeskTop, 5, kDeskRows, 2);
                if (rows > 0) canvas.rect(x + 8, kDeskTop, 5, rows, uint8_t(active ? 7 : 4));
                const int now = int(float(rows) * dip + 0.5f);
                if (now > 0) canvas.rect(x + 8, kDeskTop, 5, now, 15);
                char *out = text;
                if (t == source) append(out, "SRC");
                else if (depth == 0) append(out, "OFF");
                else append_number(out, uint32_t(depth));
                *out = 0;
                if (t == source) {
                    const int age = edit_trig_live_[t] ? int(last_ms_ - edit_trig_ms_[t]) : 9999;
                    if (running && age < 120) canvas.rect(x + 8, kDeskTop, 5, kDeskRows, 12);
                }
            }
            paint::small_text(px, x + 1, 102, text, muted ? 4 : (active ? 15 : 7));
            paint::small_text(px, x + 1, 110, machine_name(engine_.machine_id(t)),
                              muted ? 3 : (t == selected_ ? 15 : 8));
            if (muted) canvas.rect(x + 1, 116, 11, 1, 6);
            // The choke group under the machine on the fader pages (push A-D steps it).
            const uint8_t choke = kit.tracks[t].choke;
            if (kind == DeskKind::faders && choke != 0) {
                const char group[3] = {'C', char('0' + choke), 0};
                paint::small_text(px, x + 1, 119, group, active ? 12 : 6);
            }
        }
    }

    // How far the duck is pulling down now, 0..1 of each track's depth: the source's last trig
    // through the attack, the hold and the release (core/project.hpp KitDuck), from when the UI
    // saw it. 0 when stopped or long since.
    float duck_gain_now(int source) const {
        if (enaga::clock::state().running == 0 || !edit_trig_live_[source]) return 0.0f;
        const KitDuck &duck = engine_.project().kit.duck;
        const float age = float(last_ms_ - edit_trig_ms_[source]);
        const float atk = float(duck.attack_ms);
        const float hold = float(duck.hold) * float(kDuckTimeStepMs);
        const float rel = float(duck.release) * float(kDuckTimeStepMs);
        if (age < atk) return age / (atk > 0.0f ? atk : 1.0f);
        if (age < atk + hold) return 1.0f;
        if (age < atk + hold + rel) return 1.0f - (age - atk - hold) / rel;
        return 0.0f;
    }

    // --- DUCK: the envelope a source's trig pulls the other tracks down by --------------------
    void draw_duck_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const KitDuck &duck = engine_.project().kit.duck;
        const int source = duck.source < kTracks ? int(duck.source) : 0;
        constexpr int kTop = 34, kBottom = 92;
        const float atk = float(duck.attack_ms);
        const float hold = float(duck.hold) * float(kDuckTimeStepMs);
        const float rel = float(duck.release) * float(kDuckTimeStepMs);
        // The whole envelope and half as much again across the width, at least 200 ms.
        const float total = atk + hold + rel;
        const float span = total * 1.5f < 200.0f ? 200.0f : total * 1.5f;
        const auto x_of = [&](float ms) { return 4 + int(ms * 112.0f / span); };
        const int xa = x_of(atk), xh = x_of(atk + hold), xr = x_of(atk + hold + rel);
        picture_grid(px, kTop, kBottom, 8);
        paint::dotted_h(px, 0, 119, kTop, 4, 2);
        // The gain: full at the top, down to the floor through the attack, held, back up.
        int16_t xs[5] = {int16_t(paint::q4(0)), int16_t(paint::q4(4)), int16_t(paint::q4(xa)),
                         int16_t(paint::q4(xh)), int16_t(paint::q4(xr))};
        int16_t ys[5] = {int16_t(paint::q4(kTop)), int16_t(paint::q4(kTop)), int16_t(paint::q4(kBottom)),
                         int16_t(paint::q4(kBottom)), int16_t(paint::q4(kTop))};
        for (int x = 4; x <= xr && x < 120; x++) {
            int y;
            if (x <= xa) y = kTop + (kBottom - kTop) * (x - 4) / (xa > 4 ? xa - 4 : 1);
            else if (x <= xh) y = kBottom;
            else y = kBottom - (kBottom - kTop) * (x - xh) / (xr > xh ? xr - xh : 1);
            for (int fy = kTop + 2; fy < y; fy += 2) paint::plot_max(px, x, fy, 2);
        }
        paint::polyline(px, xs, ys, 5, 12);
        canvas.rect(xr, kTop, 120 - xr, 1, 12);
        const float g = duck_gain_now(source);
        if (g > 0.0f) {
            const float age = float(last_ms_ - edit_trig_ms_[source]);
            const int hx = x_of(age);
            const int hy = kTop + int(float(kBottom - kTop) * g);
            if (hx < 120) paint::dot(px, hx, hy, 15);
        }
        const char *letters[3] = {"B", "C", "D"};
        paint::small_badge(px, xa - 2 < 1 ? 1 : xa - 2, kBottom - 8, letters[0]);
        if (xh - xa > 8) paint::small_badge(px, xh - 2, kBottom - 8, letters[1]);
        if (xr < 116 && xr - xh > 8) paint::small_badge(px, xr - 2, kTop + 2, letters[2]);
        canvas.rect(0, kBottom + 1, 120, 1, 6);
        paint::small_text(px, 0, kBottom + 4, "0", 8);
        char text[12];
        char *o = text;
        append_number(o, uint32_t(span + 0.5f));
        append(o, "MS");
        *o = 0;
        paint::small_text(px, 120 - paint::small_width(text), kBottom + 4, text, 8);
    }

    // --- DLY and DLY+: the echoes ----------------------------------------------------------
    //
    // One hit and its repeats along two seconds (eight for the LONG delay), each as tall as the
    // feedback leaves it, softer as TON darkens; ping-pong alternates them up and down; WOW wobbles
    // them and DIF smears each into a cluster. While the beat plays, a head sweeps the time in step
    // with the transport and lights the echoes it passes.
    void draw_echo_view(enaga::Canvas &canvas, bool plus) {
        uint8_t *px = canvas.data();
        const SendSettings &s = engine_.sends();
        // DLY+ is the LONG delay's page: the tape's wobble (WOW) over the echoes, and all of it
        // dim while the classic delay is the one playing (push A switches).
        const bool dim = plus && !s.delay_long;
        const int kMid = plus ? 70 : 64, kAmp = plus ? 24 : 30;
        char time[16];
        char *t_out = time;
        append_number(t_out, uint32_t(effective_delay_seconds(s) * 1000.0f + 0.5f));
        append(t_out, "MS");
        *t_out = 0;
        paint::small_text(px, 0, 27, time, 12);
        if (plus) {
            const bool running_now = enaga::clock::state().running != 0;
            int16_t wx[56], wy[56];
            for (int i = 0; i < 56; i++) {
                const int ph = (i * 13 + (running_now ? int(last_ms_ / 12u) : 0)) & 255;
                wx[i] = int16_t(paint::q4(36) + i * 24);
                wy[i] = int16_t(paint::q4(31) - int(s.delay_wow * 4.0f * float(paint::sin_q12(ph)) / 4096.0f * 16.0f));
            }
            paint::polyline(px, wx, wy, 56, dim ? 4 : 10);
            paint::small_text(px, 30, 29, "W", dim ? 4 : 8);
        }
        const float delay = effective_delay_seconds(s);
        const float window = s.delay_long ? 8.0f : 2.0f;
        const auto x_of = [&](float sec) { return 2 + int(sec * 116.0f / window); };
        paint::dotted_h(px, 0, 119, kMid, 3, 3);
        for (int k = 1; k < 8; k++) paint::dotted_v(px, x_of(window * float(k) / 8.0f), kMid - kAmp, kMid + kAmp, 3, 3);
        const bool running = enaga::clock::state().running != 0;
        float head = -1.0f;
        if (running) {
            const uint32_t into = engine_.status().pattern_into.load(std::memory_order_relaxed);
            const float beat_s = 60.0f / (enaga::clock::state().tempo > 1.0f ? enaga::clock::state().tempo : 120.0f);
            head = float(into % uint32_t(kPpqn * 4)) / float(kPpqn) * beat_s;
            while (head > window) head -= window;
        }
        float amp = 1.0f;
        const int soft = 1 + int((1.0f - s.delay_tone) * 3.0f);
        for (int k = 0; k < 64 && amp > 0.03f; k++) {
            float t = float(k) * delay;
            if (s.delay_long && k > 0) {
                const int wob = paint::sin_q12((k * 37 + int(last_ms_ / 8u) * (running ? 1 : 0)) & 255);
                t += s.delay_wow * 0.04f * float(wob) / 4096.0f;
            }
            if (t > window) break;
            const int x = x_of(t);
            const int rows = int(amp * float(kAmp));
            const bool up = !s.delay_ping_pong || k % 2 == 0;
            const bool passed = head >= 0.0f && t <= head;
            const int grey = dim ? (k == 0 ? 6 : 4) : (k == 0 ? 15 : (passed ? 15 : 9));
            const int spread = s.delay_long ? int(s.delay_diffusion * 4.0f) : 0;
            for (int d = -spread; d <= spread; d++) {
                const int r = rows * (spread + 1 - (d < 0 ? -d : d)) / (spread + 1);
                if (r <= 0) continue;
                for (int w = 0; w < soft; w++) {
                    const int rr = r * (soft - w) / soft;
                    if (up) canvas.rect(x + d * 2 + w, kMid - rr, 1, rr, uint8_t(w == 0 ? grey : grey / 2));
                    else canvas.rect(x + d * 2 + w, kMid + 1, 1, rr, uint8_t(w == 0 ? grey : grey / 2));
                }
            }
            amp *= k == 0 ? 0.8f : s.delay_feedback;
        }
        if (head >= 0.0f) canvas.rect(x_of(head), kMid - kAmp - 2, 1, 2 * kAmp + 5, 6);
        if (s.delay_ping_pong) {
            paint::small_text(px, 113, kMid - kAmp, "L", 8);
            paint::small_text(px, 113, kMid + kAmp - 4, "R", 8);
        }
        canvas.rect(0, kMid + kAmp + 3, 120, 1, 6);
        paint::small_text(px, 0, kMid + kAmp + 5, "0", 8);
        paint::small_text(px, 104, kMid + kAmp + 5, s.delay_long ? "8S" : "2S", 8);
        paint::small_text(px, 48, kMid + kAmp + 5, s.delay_long ? "LONG" : "CLASSIC", 7);
    }

    // --- REV and REV+: the tail ------------------------------------------------------------
    //
    // A hit and the reverb's tail after it over four seconds (logarithmic in time, so 0.2 s and
    // 30 s both read): early reflections as spikes spaced by SIZ, then the dense tail falling over
    // DEC, its later part smoother as DMP darkens it, HALL's sweep wobbling it, and the shimmer's
    // octave-up sparks above it. While the beat plays the tail's grain moves.
    void draw_reverb_view(enaga::Canvas &canvas, bool plus) {
        uint8_t *px = canvas.data();
        const SendSettings &s = engine_.sends();
        // REV+ is the HALL's page: the room as a box as big as SIZ makes it, wobbling with MOD,
        // over the tail; all of it dim while the classic room plays (push A switches).
        const bool dim = plus && !s.reverb_hall;
        if (plus) {
            const bool running_now = enaga::clock::state().running != 0;
            const int wob = running_now ? int(s.reverb_mod * 2.0f * float(paint::sin_q12(int(last_ms_ / 16u) & 255)) / 4096.0f) : 0;
            const int w = 14 + int(s.reverb_size * 22.0f) + wob;
            const int h = 8 + int(s.reverb_size * 8.0f);
            frame_rect(px, 112 - w, 34, w, h, dim ? 4 : 11);
            paint::small_text(px, 112 - w + 2, 36, "SIZ", dim ? 4 : 8);
        }
        constexpr int kBottom = 96, kTall = 62;
        const bool running = enaga::clock::state().running != 0;
        uint32_t seed = 0x1234567u + (running ? last_ms_ / 50u : 0u);
        const float decay = s.reverb_decay_s;
        picture_grid(px, kBottom - kTall, kBottom, 8);
        // Column x is t = 0.01 * 2^(x / 15) s: 10 ms to about 2.5 minutes, but drawn to 120.
        for (int x = 0; x < 120; x++) {
            const float t = 0.01f * enaga::dsp::exp2(float(x) * (1.0f / 15.0f)) - 0.01f;
            const float env = enaga::dsp::exp2(-t * 6.9f / (decay > 0.05f ? decay : 0.05f) * 1.4427f * 0.5f);
            seed = seed * 1664525u + 1013904223u;
            const float n = float(seed >> 24) * (1.0f / 255.0f);
            const float smooth = s.reverb_damping * float(x) / 120.0f;
            const float grain = n * (1.0f - smooth) + 0.5f * smooth;
            float wob = 1.0f;
            if (s.reverb_hall) wob += s.reverb_mod * 0.25f * float(paint::sin_q12((x * 9 + int(running ? last_ms_ / 20u : 0u)) & 255)) / 4096.0f;
            const int rows = int(env * float(kTall) * (0.45f + 0.55f * grain) * wob);
            if (rows > 0) canvas.rect(x, kBottom - rows, 1, rows, uint8_t(dim ? 4 : (x < 20 ? 12 : 8)));
        }
        // Early reflections.
        const float size = s.reverb_hall ? s.reverb_size : 1.0f;
        for (int k = 1; k <= 5; k++) {
            const float t = 0.012f * size * float(k) * (1.0f + 0.3f * float(k % 2));
            const int x = int(15.0f * enaga::dsp::log2((t + 0.01f) * 100.0f));
            if (x >= 0 && x < 120) canvas.rect(x, kBottom - kTall / 2 + k * 4, 1, kTall / 2 - k * 4, 13);
        }
        canvas.rect(0, kBottom - kTall, 1, kTall, 15);                  // the hit
        if (s.reverb_hall && s.reverb_shimmer > 0.0f) {
            for (int k = 0; k < int(s.reverb_shimmer * 24.0f); k++) {
                seed = seed * 1664525u + 1013904223u;
                const int x = 20 + int(seed >> 25);
                const int y = kBottom - kTall + int((seed >> 8) & 15u);
                if (x < 120) paint::dot(px, x, y, 10);
            }
        }
        canvas.rect(0, kBottom + 1, 120, 1, 6);
        paint::small_text(px, 0, kBottom + 3, "10MS", 8);
        paint::small_text(px, 52, kBottom + 3, "1S", 8);
        paint::small_text(px, 100, kBottom + 3, "30S", 8);
        paint::small_text(px, 120 - paint::small_width(s.reverb_hall ? "HALL" : "ROOM"), kBottom - kTall, s.reverb_hall ? "HALL" : "ROOM", 9);
    }

    // --- CHO: the chorus's two sweeps ----------------------------------------------------------
    //
    // The delay each side reads at, across about two seconds: two sines a quarter turn apart, as
    // deep as DEP swings them and as many as RAT turns in that time, L above the centre line's
    // dotted 8 ms and R a shade dimmer. While the beat plays they run. Under them, how much of
    // the chorus goes into the reverb (C>R) as a bar.
    void draw_chorus_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const SendSettings &s = engine_.sends();
        constexpr int kMid = 62, kAmp = 26;
        constexpr float kWindowS = 2.0f;
        const bool running = enaga::clock::state().running != 0;
        picture_grid(px, kMid - kAmp - 4, kMid + kAmp + 4, 8);
        paint::dotted_h(px, 0, 119, kMid, 3, 3);
        // Turns of the sweep per pixel; and where it is now, in 256ths of a turn.
        const float turns_per_px = s.chorus_rate_hz * kWindowS * (1.0f / 120.0f);
        const int now = running ? int(float(last_ms_) * 0.001f * s.chorus_rate_hz * 256.0f) : 0;
        const float depth = s.chorus_depth * float(kAmp);
        for (int side = 0; side < 2; side++) {
            int16_t xs[61], ys[61];
            for (int i = 0; i <= 60; i++) {
                const int x = i * 2;
                const int phase = (int(float(x) * turns_per_px * 256.0f) + now + side * 64) & 255;
                xs[i] = int16_t(paint::q4(x));
                ys[i] = int16_t(paint::q4(kMid) - int(depth * float(paint::sin_q12(phase)) / 4096.0f * 16.0f));
            }
            paint::polyline(px, xs, ys, 61, side == 0 ? 14 : 9);
        }
        paint::small_text(px, 113, kMid - kAmp, "L", 14);
        paint::small_text(px, 113, kMid + kAmp - 4, "R", 9);
        canvas.rect(0, kMid + kAmp + 6, 120, 1, 6);
        paint::small_text(px, 0, kMid + kAmp + 8, "8MS", 8);
        // C>R: the chorus into the reverb.
        const int into = int(s.chorus_to_reverb * 40.0f + 0.5f);
        canvas.rect(78, kMid + kAmp + 9, 40, 2, 3);
        if (into > 0) canvas.rect(78, kMid + kAmp + 9, into, 2, 11);
        paint::small_text(px, 62, kMid + kAmp + 8, "REV", 8);
    }

    // --- ROUTE: where the sends go -------------------------------------------------------------
    //
    // The tracks into the delay and the reverb, both into the output; the delay into the reverb
    // as bright as D>R sends; ping-pong as the delay's return zigzagging left and right; and while
    // the beat plays, dots running along every wire that carries something.
    void draw_route_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const SendSettings &s = engine_.sends();
        const bool running = enaga::clock::state().running != 0;
        const int phase = running ? int(last_ms_ / 40u % 8u) : 0;
        struct Box { int x, y; const char *name; };
        const Box trk{2, 56, "TRK"}, dly{42, 36, "DLY"}, rev{42, 78, "REV"}, out{90, 56, "OUT"};
        const auto wire = [&](int x0, int y0, int x1, int y1, int grey) {
            paint::line(px, paint::q4(x0), paint::q4(y0), paint::q4(x1), paint::q4(y1), grey);
            if (running && grey > 4) {
                for (int k = 0; k < 4; k++) {
                    const int f = (k * 8 + phase * 4) % 32;
                    paint::dot(px, x0 + (x1 - x0) * f / 32, y0 + (y1 - y0) * f / 32, grey);
                }
            }
        };
        wire(trk.x + 22, trk.y + 5, dly.x, dly.y + 5, 9);
        wire(trk.x + 22, trk.y + 5, rev.x, rev.y + 5, 9);
        wire(dly.x + 22, dly.y + 5, out.x, out.y + 5, 9);
        wire(rev.x + 22, rev.y + 5, out.x, out.y + 5, 9);
        const int dr = int(s.delay_to_reverb * 11.0f);
        if (dr > 0) wire(dly.x + 11, dly.y + 11, rev.x + 11, rev.y - 1, 3 + dr);
        const Box boxes[4] = {trk, dly, rev, out};
        for (const Box &b : boxes) {
            frame_rect(px, b.x, b.y, 23, 11, 12);
            paint::small_text(px, b.x + 6, b.y + 3, b.name, 15);
        }
        if (s.delay_ping_pong) {                                       // the return, left and right
            for (int k = 0; k < 4; k++) {
                const int x = dly.x + 26 + k * 4;
                canvas.rect(x, dly.y - 4 + (k % 2) * 4, 3, 1, 10);
            }
            paint::small_text(px, dly.x + 26, dly.y - 10, "L R", 8);
        }
        if (s.delay_long) {                                            // the LONG delay's low cut
            char text[12];
            char *o = text;
            append(o, "LC ");
            append_number(o, uint32_t(s.delay_low_cut_hz + 0.5f));
            *o = 0;
            paint::small_text(px, dly.x, dly.y + 13, text, 7);
        }
    }

    // --- MST: the draft's master page ---------------------------------------------------------
    //
    // The output's spectrum across the top (24 bands, peaks held), 100 / 1K / 10K under it; then
    // the vectorscope (mid up, side across, an afterglow), the glue compressor's reduction and the
    // limiter as two bars falling from the top, and beside them GR and the momentary loudness in
    // LUFS (K-weighted, 400 ms; mixer_loudness), and LVL. Stopped, the analysers sit at rest.
    void draw_master_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        const bool running = enaga::clock::state().running != 0;
        draw_knob_line(canvas, kKnobY, 0x7u);
        constexpr int kTop = 26, kRows = 38, kBottom = kTop + kRows;
        if (running) {
            uint16_t milli[spectrum::kBands];
            spectrum::analyse(scope_history_, spectrum_work_, milli);
            for (int b = 0; b < spectrum::kBands; b++) {
                const int lit = paint::level_rows(uint32_t(milli[b]) * 4u, kRows, 54);
                band_rows_[b] = uint8_t(lit > band_rows_[b] - 3 ? lit : band_rows_[b] - 3);
                if (lit >= band_peak_[b]) { band_peak_[b] = uint8_t(lit); band_hold_[b] = 12; }
                else if (band_hold_[b] > 0) band_hold_[b]--;
                else if (band_peak_[b] > 0) band_peak_[b]--;
            }
        } else {
            for (int b = 0; b < spectrum::kBands; b++) band_rows_[b] = band_peak_[b] = band_hold_[b] = 0;
        }
        for (int b = 0; b < spectrum::kBands; b++) {
            const int x = b * 5;
            for (int r = 0; r < band_rows_[b]; r += 2) {
                const uint8_t g = r >= kRows - 10 ? 15 : (r >= kRows - 22 ? 11 : 7);
                canvas.rect(x, kBottom - 1 - r, 4, 1, g);
            }
            canvas.rect(x, kBottom - 1 - band_peak_[b], 4, 1, band_peak_[b] > 0 ? 15 : 3);
        }
        canvas.rect(0, kBottom + 1, 120, 1, 4);
        paint::small_text(px, 8, kBottom + 3, "100", 8);
        paint::small_text(px, 50, kBottom + 3, "1K", 8);
        paint::small_text(px, 98, kBottom + 3, "10K", 8);

        // The vectorscope, 42 x 42 at (0, 72).
        constexpr int kVx = 0, kVy = 72;
        frame_rect(px, kVx, kVy, 42, 42, 3);
        paint::dotted_v(px, kVx + 21, kVy + 1, kVy + 41, 4, 3);
        paint::dotted_h(px, kVx + 1, kVx + 41, kVy + 21, 4, 3);
        if (running) {
            master_glow_.fade(128);
            for (uint32_t ago = 0; ago < 600; ago += 3) {
                const int l = scope_history_.left(ago);
                const int r = scope_history_.right(ago);
                master_glow_.plot(20 + (l - r) * 18 / 46341, 20 - (l + r) * 18 / 46341, 15);
            }
        } else {
            master_glow_.clear();
            master_glow_.plot(20, 20, 9);
        }
        master_glow_.blit(px, kVx + 1, kVy + 1);
        paint::small_text(px, kVx + 1, kVy + 43, "M/S", 8);

        // The compressor's reduction and the limiter, falling from the top.
        const uint32_t gain = engine_.status().comp_gain_milli.load(std::memory_order_relaxed);
        const int gr_tenths = -paint::level_db_q8(gain) * 10 / 256;                  // 0..600
        const int cmp = running ? (gr_tenths * 38 / 60 > 38 ? 38 : gr_tenths * 38 / 60) : 0;   // 6 dB fills
        canvas.rect(50, 74, 8, 38, 2);
        if (cmp > 0) canvas.rect(50, 74, 8, cmp, 13);
        canvas.rect(68, 74, 8, 38, 2);
        if (running && limit_lit_) canvas.rect(68, 74, 8, 15, 13);
        paint::small_text(px, 48, 115, "CMP", 8);
        paint::small_text(px, 66, 115, "LIM", 8);

        char line[16];
        char *out = line;
        paint::small_text(px, 88, 74, "GR", 8);
        if (running && gr_tenths > 0) *out++ = '-';
        append_tenths(out, uint32_t(running ? gr_tenths : 0));
        *out = 0;
        paint::small_text(px, 88, 81, line, 15);
        paint::small_text(px, 88, 91, "LUFS", 8);
        out = line;
        const int lufs = running ? mixer_loudness() : -999;
        if (lufs <= -600) append(out, "---");
        else { *out++ = '-'; append_tenths(out, uint32_t(-lufs)); }
        *out = 0;
        paint::small_text(px, 88, 98, line, 15);
        if (!running) loudness_reset();
        draw_knob_line(canvas, 108, 0x8u, 88);
    }

    // The output's momentary loudness (ITU-R BS.1770: K-weighted, both channels, 400 ms), in
    // tenths of LUFS, from every frame core/scope.hpp's history took since the last frame (24 kHz;
    // a frame that came late loses what the history no longer holds). The K filters' two stages
    // are designed for 24 kHz: a high shelf of +4 dB from 1.7 kHz and a high-pass at 38 Hz.
    int mixer_loudness() {
        uint32_t fresh = scope_history_.arrived() - loud_seen_;
        loud_seen_ = scope_history_.arrived();
        if (fresh > ScopeHistory::kHistory) fresh = ScopeHistory::kHistory;
        if (fresh == 0) return loud_tenths_;
        static constexpr float kS[5] = {1.4772469f, -2.1462167f, 0.8468185f, -1.3213797f, 0.4992284f};
        static constexpr float kH[5] = {0.9900967f, -1.9801935f, 0.9900967f, -1.9801441f, 0.9802428f};
        float sum = 0.0f;
        for (uint32_t ago = fresh; ago-- > 0;) {
            for (int c = 0; c < 2; c++) {
                const float x = float(c == 0 ? scope_history_.left(ago) : scope_history_.right(ago)) * (1.0f / 32768.0f);
                float *z = loud_z_[c];
                const float y1 = kS[0] * x + z[0];
                z[0] = kS[1] * x - kS[3] * y1 + z[1];
                z[1] = kS[2] * x - kS[4] * y1;
                const float y2 = kH[0] * y1 + z[2];
                z[2] = kH[1] * y1 - kH[3] * y2 + z[3];
                z[3] = kH[2] * y1 - kH[4] * y2;
                sum += y2 * y2;
            }
        }
        const float block = sum / float(fresh);
        const float share = float(fresh) * (1.0f / (24000.0f * 0.4f));
        loud_ms_ += (block - loud_ms_) * (share > 1.0f ? 1.0f : share);
        if (loud_ms_ < 1e-7f) { loud_tenths_ = -999; return loud_tenths_; }
        const float lufs = -0.691f + 3.0103f * enaga::dsp::log2(loud_ms_);
        loud_tenths_ = int(lufs * 10.0f - 0.5f);
        return loud_tenths_;
    }

    void loudness_reset() {
        loud_ms_ = 0.0f;
        loud_tenths_ = -999;
        loud_seen_ = scope_history_.arrived();
        for (auto &z : loud_z_) z[0] = z[1] = z[2] = z[3] = 0.0f;
    }

    // --- EQ: the kill EQ as a curve over the output's spectrum ---------------------------------
    //
    // FLT's frame (the same axis and spectrum, app/ui_editview.hpp): the three bands' gains as one
    // curve (a shelf under 250 Hz, a bell in the middle, a shelf over 2.5 kHz; KILL drawn 30 dB
    // down), each band's letter on it, and a lit KILL over a band its switch has thrown.
    void draw_eq_view(enaga::Canvas &canvas) {
        uint8_t *px = canvas.data();
        draw_response_frame(px);
        const uint8_t kills = engine_.project().settings.eq_kills;
        float db[3];
        for (int b = 0; b < 3; b++) {
            const uint8_t byte = engine_.project().settings.master[int(MasterField::eq_low) + b];
            const bool killed = ((kills >> b) & 1u) != 0 || byte == 0;
            db[b] = killed ? -30.0f : float(int(byte) - 64) * (6.0f / 63.0f);
        }
        int16_t xs[60], ys[60];
        int n = 0;
        for (int x = 1; x < 121 && n < 60; x += 2) {
            const float hz = resp_hz(x);
            const float lo = 1.0f / (1.0f + (hz / 250.0f) * (hz / 250.0f));
            const float hi = 1.0f / (1.0f + (2500.0f / hz) * (2500.0f / hz));
            const float mid = 1.0f - lo - hi < 0.0f ? 0.0f : 1.0f - lo - hi;
            const float g = db[0] * lo + db[1] * mid + db[2] * hi;
            int y = kRespTop + 22 - int(g * 1.1f);
            y = y < kRespTop ? kRespTop : (y > kRespBottom ? kRespBottom : y);
            for (int fy = y + 2; fy < kRespBottom; fy += 3) paint::plot_max(px, x, fy, 2);
            xs[n] = int16_t(paint::q4(x));
            ys[n] = int16_t(paint::q4(y));
            n++;
        }
        paint::polyline(px, xs, ys, n, 15);
        static const int kCentres[3] = {resp_x(90.0f), resp_x(800.0f), resp_x(6000.0f)};
        for (int b = 0; b < 3; b++) {
            const char letter[2] = {char('A' + b), 0};
            const int x = kCentres[b];
            const int y = kRespTop + 22 - int(db[b] * 1.1f);
            paint::small_badge(px, x - 1, (y > kRespBottom - 8 ? kRespBottom - 8 : (y < kRespTop + 1 ? kRespTop + 1 : y)) - 3, letter);
            if (((kills >> b) & 1u) != 0) paint::small_badge(px, x - 7, kRespTop + 3, "KILL");
        }
        canvas.rect(0, kRespBottom, 120, 1, 6);
    }
