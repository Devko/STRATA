// STRATA: the Enaga app. M1 — a playable four-track beat.
//
// This file is the only place that talks to Enaga, which keeps the rest of the project portable
// and host-testable (CLAUDE.md). It owns the main context: keys, pads, encoders, the screen and
// the LEDs. Everything that makes sound lives in app/engine.cpp and runs in the audio callback.
//
// The split is strict, and it is the whole architecture: this side never writes pattern memory.
// Every edit is posted as a command and applied by the audio side at the start of its next
// block, so an edit can never land halfway through a step and a save can never race the
// transport. docs/SPEC.md, "Software architecture".
#include "engine.hpp"

#include <enaga/app.hpp>
#include <enaga/clock.hpp>

#include <cstdint>

namespace {

using namespace strata;

// --- text helpers (no printf, no libm, and the font is upper case and digits) ---

void append(char *&out, const char *text) {
    while (*text) *out++ = *text++;
}

void append_number(char *&out, uint32_t value) {
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
}

void append_tenths(char *&out, uint32_t tenths) {
    append_number(out, tenths / 10);
    *out++ = '.';
    *out++ = char('0' + tenths % 10);
}

void append_padded(char *&out, uint32_t value, int width) {
    uint32_t scale = 1;
    for (int i = 1; i < width; i++) scale *= 10;
    while (scale > 1 && value < scale) { *out++ = '0'; scale /= 10; }
    append_number(out, value);
}

const char *device_name(int device) {
    switch (device) {
        case ENAGA_DEVICE_SMPLTREK: return "SMPLTREK";
        case ENAGA_DEVICE_LOFI12XT: return "LOFI-12 XT";
        case ENAGA_DEVICE_CYDRUMS:  return "CYDRUMS";
        default:                    return "UNKNOWN";
    }
}

// --- the LED shadow -------------------------------------------------------
//
// The spec requires it: LEDs update from the main context only, and a command goes out only when
// a value actually changes. Without the shadow the panel MCU would be flooded every frame.

struct LedShadow {
    uint8_t step_red[16] = {};
    uint8_t step_green[16] = {};
    uint8_t pad_r[15] = {}, pad_g[15] = {}, pad_b[15] = {};
    uint8_t key_red[ENAGA_KEY_COUNT] = {};
    uint8_t key_green[ENAGA_KEY_COUNT] = {};
    bool primed = false;

    void step(int index, uint8_t red, uint8_t green) {
        if (primed && step_red[index] == red && step_green[index] == green) return;
        step_red[index] = red;
        step_green[index] = green;
        enaga::step_led(index + 1, red, green);
    }

    void pad(int index, uint8_t r, uint8_t g, uint8_t b) {
        if (primed && pad_r[index] == r && pad_g[index] == g && pad_b[index] == b) return;
        pad_r[index] = r; pad_g[index] = g; pad_b[index] = b;
        enaga::pad_led(index, r, g, b);
    }

    void key(int code, uint8_t red, uint8_t green) {
        if (code < 0 || code >= ENAGA_KEY_COUNT) return;
        if (primed && key_red[code] == red && key_green[code] == green) return;
        key_red[code] = red;
        key_green[code] = green;
        enaga::button_led(code, red, green);
    }
};

// Track colours for the pads, roughly following the kit: kick red, snare amber, hats cyan.
struct Rgb { uint8_t r, g, b; };
constexpr Rgb kTrackColour[kTracks] = {
    {10, 0, 0}, {10, 6, 0}, {0, 8, 10}, {0, 4, 10},
    {8, 0, 10}, {0, 10, 4}, {10, 10, 0}, {4, 4, 10},
};

// The 8 lower pads are the tracks: even ids 0, 2, 4 ... 14 (docs/SPEC.md, "Pads").
constexpr int pad_for_track(int track) { return track * 2; }
constexpr int track_for_pad(int pad) { return pad % 2 == 0 ? pad / 2 : -1; }

// The modes of docs/SPEC.md's function block. F4 = GRID, F7 = MIXER, F8 = EDIT, and the two
// selection keys under the screen move within whichever mode is showing -- the step page in
// GRID, the parameter page in EDIT, the bank of four tracks in MIXER.
enum class Mode : uint8_t { grid, edit, mixer };

const char *page_name(Page page) {
    switch (page) {
        case Page::src:  return "SRC";
        case Page::fltr: return "FLTR";
        case Page::amp:  return "AMP";
        case Page::fx:   return "FX";
        default:         return "?";
    }
}

class Strata : public enaga::App {
public:
    bool init() override {
        if (!engine_.init(float(ENAGA_SAMPLE_RATE))) return false;
        enaga::clock::set_tempo(130.0f);     // the spec's reference tempo
        enaga::log("strata m1 ready");
        return true;
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        engine_.process(in, out, frames);
    }

    // --- input ------------------------------------------------------------

    void on_key(int key, bool down) override {
        if (key == ENAGA_KEY_F1) { shift_ = down; return; }
        if (!down) return;

        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            const int step = key - ENAGA_KEY_STEP1 + page_ * 16;
            post(Cmd::toggle_trig, uint8_t(selected_), uint8_t(step));
            return;
        }

        switch (key) {
            case ENAGA_KEY_F5:
                // The transport is Enaga's and its calls are MAIN CONTEXT, so they happen here
                // rather than through the command ring. start() returns false under an external
                // clock source, where STRATA can only follow -- the screen shows EXT and the
                // refusal is simply recorded.
                clock_refused_ = !enaga::clock::start();
                stop_armed_ = false;
                post(Cmd::play);                       // for the audio side's trace only
                break;
            case ENAGA_KEY_F6:
                clock_refused_ = !enaga::clock::stop();
                // The second press cuts the tails. Only that part is the audio side's job.
                post(Cmd::stop, 0, 0, stop_armed_ ? 1 : 0);
                stop_armed_ = !stop_armed_;
                break;
            case ENAGA_KEY_F4:                         // HOME: the grid
                mode_ = Mode::grid;
                break;
            case ENAGA_KEY_F7:                         // MIXER
                mode_ = Mode::mixer;
                break;
            case ENAGA_KEY_F8:                         // EDIT: the sound pages
                mode_ = Mode::edit;
                break;

            // The octave keys are the step page in GRID, as the spec's table says.
            case ENAGA_KEY_OCTAVE_LEFT:
                if (page_ > 0) page_--;
                break;
            case ENAGA_KEY_OCTAVE_RIGHT:
                if (page_ < 3) page_++;
                break;

            // The selection keys move within the current mode.
            case ENAGA_KEY_SELECT_LEFT:
                select(-1);
                break;
            case ENAGA_KEY_SELECT_RIGHT:
                select(+1);
                break;
            case ENAGA_KEY_CLR:
                post(shift_ ? Cmd::clear_track : Cmd::clear_step,
                     uint8_t(selected_), uint8_t(cursor_step_));
                break;
            default:
                break;
        }
    }

    // What the two selection keys under the screen do depends on the mode.
    void select(int delta) {
        switch (mode_) {
            case Mode::grid:
                page_ = (page_ + delta + 4) % 4;                       // step page 1-4
                break;
            case Mode::edit:
                param_page_ = (param_page_ + delta + kPages) % kPages; // SRC/FLTR/AMP/FX
                break;
            case Mode::mixer:
                mixer_bank_ = (mixer_bank_ + delta + 2) % 2;           // tracks 1-4 or 5-8
                break;
        }
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder == 4) {
            tempo_tenths_ = uint32_t(int(tempo_tenths_) + delta);
            if (tempo_tenths_ < 200) tempo_tenths_ = 200;      // Enaga's clock allows 20-300
            if (tempo_tenths_ > 3000) tempo_tenths_ = 3000;
            // Main context again, and refused under an external clock.
            clock_refused_ = !enaga::clock::set_tempo(float(tempo_tenths_) * 0.1f);
            return;
        }

        // The ENCODER EVENT id is alphabetical (0-3 = A-D), unlike the PUSH-button key codes,
        // which run B, A, C, D. See docs/ENAGA_API.md -- confusing the two silently swaps A
        // and B, and nothing catches it.
        if (encoder < 0 || encoder > 3) return;

        if (mode_ == Mode::mixer) {
            // A-D are the levels of the four tracks in the current bank.
            const int track = mixer_bank_ * 4 + encoder;
            if (track >= kTracks) return;
            float level = engine_.level(track) + float(delta) * 0.02f;
            if (level < 0.0f) level = 0.0f;
            if (level > 1.5f) level = 1.5f;
            Command command;
            command.cmd = Cmd::set_track_level;
            command.track = uint8_t(track);
            command.f = level;
            engine_.commands().post(command);
            return;
        }

        // Otherwise A-D edit the four parameters of the current page on the selected track.
        const int index = page_first(Page(param_page_)) + encoder;
        const int value = int(engine_.params(selected_).v[index]) + delta;
        post(Cmd::set_param, uint8_t(selected_), 0,
             uint8_t(index), uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value)));
    }

    void on_pad_press(int pad, int velocity) override {
        const int track = track_for_pad(pad);
        if (track < 0 || track >= kTracks) return;

        if (shift_) {                                  // F1 + pad: mute, in any mode
            muted_[track] = !muted_[track];
            post(Cmd::set_mute, uint8_t(track), 0, muted_[track] ? 1 : 0);
            return;
        }

        selected_ = track;
        post(Cmd::trigger_note, uint8_t(track), 0,
             engine_.pattern().root, uint8_t(velocity));
    }

    void on_transport(int what, uint32_t position) override {
        // Under an external clock STRATA cannot start or stop anything; it follows. Resetting
        // the chain state here is what keeps conditions in step with an outside transport.
        (void)position;
        if (what == ENAGA_TRANSPORT_START || what == ENAGA_TRANSPORT_LOCATE) stop_armed_ = false;
    }

    // --- LEDs -------------------------------------------------------------

    void update(uint32_t ms) override {
        (void)ms;

        // Log transport and clock-source changes. Cheap (a handful of comparisons), only fires on
        // a change, and it is the one thing worth seeing in an emulator run or a unit log: it
        // says whether the clock is actually moving and who is driving it.
        {
            const enaga::clock::State clock = enaga::clock::state();
            const bool running = clock.running != 0;
            const int source = int(clock.source);
            if (running != logged_running_ || source != logged_source_) {
                logged_running_ = running;
                logged_source_ = source;
                enaga::log(running ? (source == ENAGA_CLOCK_INTERNAL ? "run int"
                                                                     : "run ext")
                                   : "stopped");
            }
            (void)logged_position_;
        }

        const Status &status = engine_.status();
        const Pattern &pattern = engine_.pattern();
        const uint8_t playhead = status.playhead[selected_].load(std::memory_order_relaxed);

        for (int i = 0; i < 16; i++) {
            const int step = i + page_ * 16;
            const Step &s = pattern.tracks[selected_].steps[step];
            const bool trig = (s.flags & kStepTrig) != 0;
            const bool has_locks = s.lock_count > 0;
            const bool conditional = s.probability < 100 || condition_kind(s.condition) != Condition::NONE;

            uint8_t green = 0, red = 0;
            if (trig) {
                green = conditional ? 2 : 6;          // the spec's LED language
                if (has_locks) red = 3;               // amber: a trig with locks
            }
            if (step == playhead && status.running.load(std::memory_order_relaxed)) red = 10;
            leds_.step(i, red, green);
        }

        for (int t = 0; t < kTracks; t++) {
            const Rgb colour = kTrackColour[t];
            const uint8_t level = muted_[t] ? 0 : (t == selected_ ? 10 : 2);
            const uint8_t scale = level == 0 ? 0 : level;
            leds_.pad(pad_for_track(t), uint8_t(colour.r * scale / 10),
                      uint8_t(colour.g * scale / 10), uint8_t(colour.b * scale / 10));
        }

        const bool running = status.running.load(std::memory_order_relaxed) != 0;
        leds_.key(ENAGA_KEY_F5, 0, running ? 8 : 0);
        leds_.key(ENAGA_KEY_F6, running ? 0 : 6, 0);

        // The mode keys light the mode you are in, as the spec's LED language asks.
        leds_.key(ENAGA_KEY_F4, 0, mode_ == Mode::grid ? 8 : 0);
        leds_.key(ENAGA_KEY_F7, 0, mode_ == Mode::mixer ? 8 : 0);
        leds_.key(ENAGA_KEY_F8, 0, mode_ == Mode::edit ? 8 : 0);

        leds_.primed = true;
    }

    // --- screen -----------------------------------------------------------

    void draw(enaga::Canvas &canvas) override {
        char line[40];
        char *out = line;

        canvas.clear();

        const Status &status = engine_.status();
        const enaga::clock::State clock = enaga::clock::state();
        const bool running = clock.running != 0;

        // Top line on every screen: pattern, tempo, play state, audio load.
        out = line;
        append(out, "A01 ");
        append_tenths(out, uint32_t(clock.tempo * 10.0f + 0.5f));
        append(out, running ? " > " : " . ");
        append_tenths(out, enaga::audio_load());
        *out++ = '%';
        *out = 0;
        canvas.text(0, 0, line, 14);

        if (int(clock.source) != ENAGA_CLOCK_INTERNAL) {
            // The tempo is not ours to set under an external source, so say so rather than
            // showing a number the user cannot change.
            canvas.text(0, 9, int(clock.source) == ENAGA_CLOCK_MIDI ? "EXT MIDI" : "EXT SYNC", 10);
        } else if (clock_refused_) {
            // Internal source and still refused: worth seeing rather than silently ignoring.
            canvas.text(0, 9, "CLOCK REFUSED", 10);
        }

        switch (mode_) {
            case Mode::grid:  draw_grid(canvas, running); break;
            case Mode::edit:  draw_edit(canvas); break;
            case Mode::mixer: draw_mixer(canvas); break;
        }

        // The bottom line on every mode: voices, the device, and any dropped edits. A dropped
        // command should be visible rather than mysterious (app/commands.hpp).
        out = line;
        append(out, "V");
        append_number(out, status.voices.load(std::memory_order_relaxed));
        append(out, " ");
        append(out, device_name(enaga::system::device()));
        const uint32_t dropped = engine_.commands().dropped();
        if (dropped) { append(out, " DROP "); append_number(out, dropped); }
        *out = 0;
        canvas.text(0, 120, line, 7);
    }

    // --- GRID: all eight tracks at a glance ------------------------------

    void draw_grid(enaga::Canvas &canvas, bool running) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        const int grid_top = 20;
        for (int t = 0; t < kTracks; t++) {
            const int y = grid_top + t * 8;
            const uint8_t head = status.playhead[t].load(std::memory_order_relaxed);
            for (int i = 0; i < 16; i++) {
                const int step = i + page_ * 16;
                const Step &s = engine_.pattern().tracks[t].steps[step];
                const bool trig = (s.flags & kStepTrig) != 0;
                const bool on_head = running && step == head;

                uint8_t grey = 2;
                if (trig) grey = (s.flags & kStepAccent) ? 15 : 11;
                if (on_head) grey = uint8_t(grey < 8 ? 7 : 15);

                if (trig || on_head) {
                    canvas.rect(i * 8 + 1, y + 1, 6, 6, grey);
                } else {
                    canvas.rect(i * 8 + 1, y + 3, 6, 2, grey);   // an empty step: a thin dash
                }
            }
            if (t == selected_) canvas.rect(0, y, 1, 8, 12);     // the selected track's marker
            if (muted_[t]) canvas.rect(0, y + 3, 128, 1, 4);     // a muted track is struck out
        }

        out = line;
        append(out, "TRK ");
        append_number(out, uint32_t(selected_ + 1));
        append(out, muted_[selected_] ? " MUTE" : "     ");
        append(out, " PG ");
        append_number(out, uint32_t(page_ + 1));
        *out = 0;
        canvas.text(0, 88, line, 10);

        draw_param_slots(canvas);
    }

    // --- EDIT: the selected track's sound, four parameters at a time -----

    void draw_edit(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;

        out = line;
        append(out, "EDIT TRK ");
        append_number(out, uint32_t(selected_ + 1));
        append(out, "  ");
        append(out, page_name(Page(param_page_)));
        *out = 0;
        canvas.text(0, 20, line, 14);

        // Which of the four pages is showing, as a row of boxes: the spec's selection keys
        // flip between them, so showing where you are matters more than labels.
        for (int p = 0; p < kPages; p++) {
            const uint8_t grey = p == param_page_ ? 14 : 4;
            canvas.rect(p * 32 + 2, 32, 28, 3, grey);
        }

        // The four parameters in large type: name, value and a bar each, stacked so they are
        // readable at a glance rather than squeezed onto one line.
        const int base = page_first(Page(param_page_));
        for (int slot = 0; slot < 4; slot++) {
            const int index = base + slot;
            const int y = 42 + slot * 12;
            const uint8_t value = engine_.params(selected_).v[index];

            out = line;
            append(out, param_name_for(index));
            *out = 0;
            canvas.text(0, y, line, 13);

            out = line;
            append_padded(out, value, 3);
            *out = 0;
            canvas.text(34, y, line, 15);

            const int bar = int(value) * 70 / 127;
            canvas.rect(56, y + 2, bar < 1 ? 1 : bar, 4, 9);
        }

        // Which encoder is which, since EDIT is where that matters most.
        canvas.text(0, 96, "A    B    C    D", 7);
        draw_param_slots(canvas, 106);
    }

    // --- MIXER: four levels at a time, with meters -----------------------

    void draw_mixer(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;

        out = line;
        append(out, "MIXER TRK ");
        append_number(out, uint32_t(mixer_bank_ * 4 + 1));
        *out++ = '-';
        append_number(out, uint32_t(mixer_bank_ * 4 + 4));
        *out = 0;
        canvas.text(0, 20, line, 14);

        const Status &status = engine_.status();
        const int peak_milli = int(status.peak_milli.load(std::memory_order_relaxed));

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

            // The machine driving it, so the mixer says what it is mixing.
            const Machine *machine = engine_.machine(track);
            canvas.text(x + 14, 46, machine ? "ON" : "--", machine ? 10 : 4);

            out = line;
            append_padded(out, uint32_t(level * 100.0f + 0.5f), 3);
            *out = 0;
            canvas.text(x, 98, line, 11);
        }

        // The master peak, as one bar: the only number that says whether it is clipping.
        out = line;
        append(out, "PEAK ");
        append_tenths(out, uint32_t(peak_milli));
        *out = 0;
        canvas.text(0, 110, line, 11);
        const int peak_bar = peak_milli * 60 / 1000;
        canvas.rect(60, 112, peak_bar > 60 ? 60 : (peak_bar < 1 ? 1 : peak_bar), 4,
                    peak_milli >= 1000 ? 15 : 8);
    }

    // --- shared pieces ----------------------------------------------------

    // The machine names SRC and AMP; the insert chain names FLTR and FX.
    const char *param_name_for(int index) const {
        if (param_page_ == int(Page::fltr) || param_page_ == int(Page::fx)) {
            return InsertChain::param_name(index);
        }
        const Machine *machine = engine_.machine(selected_);
        return machine ? machine->param_name(index) : "---";
    }

    // The four encoder slots along the bottom: a three-letter name, a value and a small bar.
    void draw_param_slots(enaga::Canvas &canvas, int y = 100) {
        char line[40];
        const int base = page_first(Page(param_page_));
        for (int slot = 0; slot < 4; slot++) {
            const int index = base + slot;
            char *out = line;
            append(out, param_name_for(index));
            *out++ = ' ';
            append_padded(out, engine_.params(selected_).v[index], 3);
            *out = 0;
            canvas.text(slot * 32, y, line, 12);

            const int bar = int(engine_.params(selected_).v[index]) * 28 / 127;
            canvas.rect(slot * 32, y + 12, bar < 1 ? 1 : bar, 3, 8);
        }
    }

private:
    void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0) {
        Command command;
        command.cmd = cmd;
        command.track = track;
        command.step = step;
        command.a = a;
        command.b = b;
        engine_.commands().post(command);
    }

    Engine engine_;

    Mode mode_ = Mode::grid;
    int selected_ = 0;
    int page_ = 0;            // step page 1-4, for 64-step tracks
    int param_page_ = int(Page::src);
    int mixer_bank_ = 0;      // 0 = tracks 1-4, 1 = tracks 5-8
    int cursor_step_ = 0;
    bool shift_ = false;
    bool stop_armed_ = false;
    bool clock_refused_ = false;   // the last transport call was refused: an external source
    bool muted_[kTracks] = {};

    LedShadow leds_;
    uint32_t tempo_tenths_ = 1300;

    bool logged_running_ = false;
    int logged_source_ = -1;
    uint32_t logged_position_ = 0xFFFFFFFFu;
};

}  // namespace

ENAGA_APP(Strata)
