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
#include "record.hpp"       // live recording's stamping and quantizing, used from here only
#include "storage.hpp"      // projects, autosave and the sample loader: the card side

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

void append_hex4(char *&out, uint32_t value) {
    for (int shift = 12; shift >= 0; shift -= 4) {
        const uint32_t nibble = (value >> shift) & 0xFu;
        *out++ = char(nibble < 10 ? '0' + nibble : 'A' + (nibble - 10));
    }
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
enum class Mode : uint8_t { grid, edit, mixer, project };

// The step page shows four of a step's fields at a time, so the selection keys page through
// them. Derived rather than written down: core/stepedit.hpp owns how many fields there are.
constexpr int kFieldPages = (kStepFields + 3) / 4;

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

        // Storage takes the sample region and the staging buffers and hands the region's base to
        // the engine. Everything slow -- files, the pool, the loader -- lives on this side of the
        // ring (app/storage.hpp).
        if (!storage_.init(engine_)) return false;

        enaga::clock::set_tempo(130.0f);     // the spec's reference tempo

        // docs/SPEC.md: "On start, STRATA reopens the last project and offers the autosave if it
        // is newer." Nothing on the card is not an error -- it is a first run, and the engine's
        // built-in default pattern stays.
        if (storage_.open_last()) notice("OPENING");

        enaga::log("strata ready");
        return true;
    }

    void stop() override {
        // Destructors never run on this platform (CLAUDE.md), so this is the last chance. The
        // budget is 3 s including the save.
        storage_.save_for_shutdown();
        enaga::log("strata stopped");
    }

    void on_card(bool usable) override {
        // Every file handle opened before a removal is already dead; Storage abandons whatever
        // was in flight and writes nothing until a card is back.
        storage_.on_card(usable);
        notice(usable ? "CARD IN" : "CARD OUT");
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        engine_.process(in, out, frames);
    }

    // --- input ------------------------------------------------------------

    void on_key(int key, bool down) override {
        if (key == ENAGA_KEY_F1) { shift_ = down; return; }
        if (key == ENAGA_KEY_PUSH_VALUE) { value_pushed_ = down; return; }

        // In PROJECT mode a STEP key queues a pattern change rather than toggling a trig, so it
        // is taken here, before the editing path, and only on the press -- there is no modifier
        // gesture to decide on the release.
        if (mode_ == Mode::project && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) queue_shown_pattern(key - ENAGA_KEY_STEP1);
            return;
        }

        // A STEP key needs its release as well as its press. Holding one is a modifier -- it
        // locks a parameter, opens the step page or starts a range -- and a step held as a
        // modifier must NOT toggle its trig when it comes up. That can only be decided on the
        // release: at the moment of the press there is no way to know which it was yet.
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            on_step_key(key - ENAGA_KEY_STEP1 + page_ * 16, down);
            return;
        }

        if (!down) return;

        // PROJECT mode's own keys. Taken before the switch so the editing meanings of OK, CLR
        // and UP cannot fire while the card side is what is on screen.
        if (mode_ == Mode::project && on_project_key(key)) return;

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

            case ENAGA_KEY_F2:                         // REC: live and motion recording
                rec_ = !rec_;
                // A whole recording pass is one undo level, so the gesture ends when recording
                // does -- and the next pass must not coalesce into the last one.
                end_gesture();
                notice(rec_ ? "REC ON" : "REC OFF");
                break;

            case ENAGA_KEY_F3:                         // PROJECT: banks, patterns and the card
                mode_ = Mode::project;
                step_page_ = false;
                break;
            case ENAGA_KEY_F4:                         // HOME: the grid
                mode_ = Mode::grid;
                step_page_ = false;
                break;
            case ENAGA_KEY_F7:                         // MIXER
                mode_ = Mode::mixer;
                step_page_ = false;
                break;
            case ENAGA_KEY_F8:                         // EDIT: the sound pages
                mode_ = Mode::edit;
                step_page_ = false;
                break;

            case ENAGA_KEY_OK:
                if (held_step_ >= 0) {
                    // Hold STEP + OK: the step page, on that step (docs/SPEC.md, "Step editing").
                    step_page_ = true;
                    page_step_ = held_step_;
                    field_page_ = 0;
                    held_used_ = true;                 // so the release does not toggle the trig
                    mode_ = Mode::grid;
                } else if (shift_) {
                    post(Cmd::redo);                   // F1 + OK = redo
                    end_gesture();
                    notice("REDO");
                } else if (step_page_) {
                    step_page_ = false;                // OK again closes it
                }
                break;

            case ENAGA_KEY_CLR:
                if (shift_) {
                    post(Cmd::undo);                   // F1 + CLR = undo
                    end_gesture();
                    notice("UNDO");
                } else {
                    erase_selection();
                }
                break;

            // Copy and paste. docs/SPEC.md asks for them for steps, tracks and patterns but
            // maps no keys, and the arrow keys are the only unused pair on the panel. UP takes
            // the selection, DOWN puts it down again.
            case ENAGA_KEY_UP:
                copy_selection();
                break;
            case ENAGA_KEY_DOWN:
                paste_selection();
                break;

            // The octave keys are the step page in GRID; with F1 they are the track's length,
            // which is what makes polymeter reachable without a menu. The spec's key table.
            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT: {
                const int delta = key == ENAGA_KEY_OCTAVE_LEFT ? -1 : +1;
                if (shift_) {
                    const int length = int(engine_.pattern().tracks[selected_].length) + delta;
                    mark(GestureKind::length, selected_, 0);
                    post(Cmd::set_track_length, uint8_t(selected_), 0,
                         uint8_t(length < 1 ? 1
                                            : (length > kStepsPerTrack ? kStepsPerTrack : length)));
                    end_gesture();
                } else {
                    const int next = page_ + delta;
                    page_ = next < 0 ? 0 : (next > 3 ? 3 : next);
                }
                break;
            }

            // The selection keys move within the current mode; with F1 they walk the selected
            // track's speed, the other half of polymeter.
            case ENAGA_KEY_SELECT_LEFT:
                if (shift_) walk_speed(-1); else select(-1);
                break;
            case ENAGA_KEY_SELECT_RIGHT:
                if (shift_) walk_speed(+1); else select(+1);
                break;
            default:
                break;
        }
    }

    // --- STEP keys: tap, hold, and a pair for a range --------------------

    void on_step_key(int step, bool down) {
        if (down) {
            if (held_step_ >= 0 && held_step_ != step) {
                // A second STEP held makes the pair a selection, for CLR, copy, paste and the
                // range edits on A and B (docs/SPEC.md, "Step editing").
                range_first_ = held_step_ < step ? held_step_ : step;
                range_last_ = held_step_ < step ? step : held_step_;
                held_used_ = true;      // the first key made a selection; it was not a tap
                notice("RANGE");
                return;
            }
            // A fresh single press drops any previous selection. Without this, a range made
            // minutes ago would still be what the next CLR erases, which is the sort of thing
            // that loses someone's pattern.
            range_first_ = range_last_ = -1;
            held_step_ = step;
            held_used_ = false;
            return;
        }

        if (step != held_step_) return;        // the second key of a range coming back up

        if (!held_used_) {
            // A tap. With F1 it toggles the trigless bit instead: a step that applies its locks
            // without sounding a note.
            mark(GestureKind::trig, selected_, step);
            post(Cmd::toggle_trig, uint8_t(selected_), uint8_t(step), shift_ ? 1 : 0);
        }
        held_step_ = -1;
        end_gesture();
    }

    // --- what the selection keys do depends on where you are -------------

    void select(int delta) {
        if (step_page_) {
            field_page_ = (field_page_ + delta + kFieldPages) % kFieldPages;
            return;
        }
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
            case Mode::project:
                // The bank being SHOWN, which is not the bank playing: a queued change is armed
                // from the shown bank, so browsing must not switch anything by itself.
                shown_bank_ = (shown_bank_ + delta + kBanks) % kBanks;
                break;
        }
    }

    // The speed encoder walks kSpeedDisplayOrder (fastest to slowest), not the enum's own
    // numbering -- core/timing.hpp numbers 1x as 0 so that zeroed memory means normal speed,
    // which is deliberately not the order anyone wants to scroll through.
    void walk_speed(int delta) {
        const uint8_t current = engine_.pattern().tracks[selected_].speed;
        int index = 0;
        for (int i = 0; i < int(Speed::count); i++) {
            if (uint8_t(kSpeedDisplayOrder[i]) == current) index = i;
        }
        index += delta;
        if (index < 0) index = 0;
        if (index >= int(Speed::count)) index = int(Speed::count) - 1;
        mark(GestureKind::length, selected_, 0);
        post(Cmd::set_track_speed, uint8_t(selected_), 0, uint8_t(kSpeedDisplayOrder[index]));
        end_gesture();
    }

    // --- CLR, copy and paste ---------------------------------------------

    // What CLR erases is the narrowest thing selected: a held step, then a range, then the whole
    // track. A bare CLR wiping a track is a large gesture to leave unguarded, and it would be
    // reckless without undo -- it is acceptable here because F1 + CLR brings it straight back,
    // which is exactly what this milestone adds.
    void erase_selection() {
        if (held_step_ >= 0) {
            mark(GestureKind::erase, selected_, held_step_);
            post(Cmd::range_erase, uint8_t(selected_), uint8_t(held_step_), 1);
            held_used_ = true;
            notice("STEP CLEARED");
        } else if (range_first_ >= 0) {
            mark(GestureKind::erase, selected_, range_first_);
            post(Cmd::range_erase, uint8_t(selected_), uint8_t(range_first_),
                 uint8_t(range_last_ - range_first_ + 1));
            notice("RANGE CLEARED");
        } else {
            mark(GestureKind::erase, selected_, 0);
            post(Cmd::track_erase, uint8_t(selected_));
            notice("TRACK CLEARED  F1+CLR UNDO");
        }
        end_gesture();
    }

    void copy_selection() {
        int first = 0;
        int count = kStepsPerTrack;
        if (held_step_ >= 0) {
            first = held_step_;
            count = 1;
            held_used_ = true;
        } else if (range_first_ >= 0) {
            first = range_first_;
            count = range_last_ - range_first_ + 1;
        }
        post(Cmd::range_copy, uint8_t(selected_), uint8_t(first), uint8_t(count));
        notice(count == 1 ? "STEP COPIED" : "COPIED");
    }

    void paste_selection() {
        const int first = held_step_ >= 0 ? held_step_ : (range_first_ >= 0 ? range_first_ : 0);
        if (held_step_ >= 0) held_used_ = true;
        mark(GestureKind::paste, selected_, first);
        post(Cmd::range_paste, uint8_t(selected_), uint8_t(first));
        end_gesture();
        notice("PASTED");
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder == 4) {
            if (value_pushed_) {
                // VALUE pushed and turned is the pattern's swing, as the spec's key table says.
                const int swing = int(engine_.pattern().swing) + delta;
                mark(GestureKind::length, 0, 0);
                post(Cmd::set_pattern_swing, 0, 0,
                     uint8_t(swing < kSwingMin ? kSwingMin
                                               : (swing > kSwingMax ? kSwingMax : swing)));
                return;
            }
            if (shift_) {
                // The record quantize amount. The spec gives F1 + VALUE to the master DJ filter,
                // which is M5's performance layer and does not exist yet; quantize has nowhere
                // else to live and is useless once recording starts if it cannot be reached
                // while recording. Whichever of the two moves, it moves then, not now.
                const int next = quantize_ + delta * 5;
                quantize_ = next < 0 ? 0 : (next > 100 ? 100 : next);
                notice("QUANTIZE");
                return;
            }
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

        // PROJECT mode's own encoders: A picks when a queued change lands, B the key and C the
        // scale. A is only ever read when a pattern is queued, so it changes nothing by itself;
        // B and C apply at once, because the global key is what the melodic tracks and every
        // mutation fold into and there is nothing to confirm.
        if (mode_ == Mode::project) {
            if (encoder == 0) {
                const int next = int(queue_mode_) + delta;
                queue_mode_ = QueueMode(next < 0 ? 0
                                                 : (next >= int(QueueMode::count)
                                                        ? int(QueueMode::count) - 1
                                                        : next));
                notice(queue_mode_name(queue_mode_));
                return;
            }
            if (encoder == 1) {
                // The key wraps: twelve pitch classes in a circle is what they are, and walking
                // off one end to be stopped there would be wrong rather than merely annoying.
                const int root = (int(engine_.pattern().root) + delta % 12 + 12) % 12;
                post(Cmd::set_root, 0, 0, uint8_t(root));
                notice(note_name(uint8_t(root)));
                return;
            }
            if (encoder == 2) {
                // The scale does NOT wrap, and it is clamped to what this build has. An id a
                // later build wrote survives a load (core/project.cpp leaves it alone), so the
                // user can be looking at a scale the encoder cannot reach -- walking from there
                // lands on the nearest one this build knows rather than jumping to chromatic.
                const int current = int(engine_.pattern().scale);
                int next = (current < kScaleCount ? current : kScaleCount - 1) + delta;
                next = next < 0 ? 0 : (next >= kScaleCount ? kScaleCount - 1 : next);
                post(Cmd::set_scale, 0, 0, uint8_t(next));
                notice(scale_name(next));
                return;
            }
        }
        if (encoder < 0 || encoder > 3) return;

        // 1. A held STEP key: lock this page's parameter on that step. The single most
        //    important gesture in the whole app (docs/SPEC.md, "Step editing") -- and the
        //    reason the step's release has to know the key was used as a modifier.
        if (held_step_ >= 0) {
            held_used_ = true;
            const int index = page_first(Page(param_page_)) + encoder;
            mark(GestureKind::lock, selected_, held_step_);
            post_delta(Cmd::lock_delta, selected_, held_step_, uint8_t(index), delta);
            return;
        }

        // 2. The step page: A-D are four of the step's own fields.
        if (step_page_) {
            const int field = field_page_ * 4 + encoder;
            if (field >= kStepFields) return;
            mark(GestureKind::field, selected_, page_step_);
            post_delta(Cmd::step_field_delta, selected_, page_step_, uint8_t(field), delta);
            return;
        }

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

        // 3. A selection with no step held: A transposes it, B nudges its micro-timing. The
        //    only two edits that are genuinely about a span rather than a step.
        if (range_first_ >= 0 && encoder < 2) {
            const int count = range_last_ - range_first_ + 1;
            mark(GestureKind::field, selected_, range_first_);
            post_delta(encoder == 0 ? Cmd::range_transpose : Cmd::range_nudge,
                       selected_, range_first_, uint8_t(count), delta);
            return;
        }

        // 4. Recording, with the transport running: motion recording. The knob writes a lock on
        //    the step the track is playing right now, so a sweep lands across the steps it
        //    passes over (docs/SPEC.md, "Recording"). The gesture carries step 0 rather than the
        //    playhead so that the whole pass is ONE undo level instead of one per step.
        if (rec_ && engine_.status().running.load(std::memory_order_relaxed)) {
            const int step = int(engine_.status().playhead[selected_].load(std::memory_order_relaxed));
            const int index = page_first(Page(param_page_)) + encoder;
            mark(GestureKind::motion, selected_, 0);
            post_delta(Cmd::lock_delta, selected_, step, uint8_t(index), delta);
            return;
        }

        // 5. Otherwise A-D edit the four parameters of the current page on the selected track.
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

        if (track != selected_) end_gesture();         // a different track is a different edit
        selected_ = track;
        const uint8_t note = engine_.pattern().root;
        post(Cmd::trigger_note, uint8_t(track), 0, note, uint8_t(velocity));
        record_note(track, note, uint8_t(velocity));
    }

    // MIDI notes take exactly the same path as pad hits, which is the point: `on_midi` is main
    // context and carries no timestamp of its own, so it has nothing better to be stamped with
    // than the clock state read the moment it arrives -- the same thing a pad hit gets
    // (docs/REVIEW.md). One mechanism, one set of bugs.
    //
    // The channel picks the track, 1-8 for tracks 1-8, anything else playing the selected one.
    // docs/SPEC.md does not map this; MIDI channel is the obvious choice and a note-for-note
    // omni mapping would make an 8-track box unplayable from a keyboard.
    void on_midi(const enaga::MidiMessage &message) override {
        if (message.type() != enaga::MidiMessage::note_on) return;
        const int channel = message.channel();
        const int track = channel < kTracks ? channel : selected_;
        const uint8_t note = uint8_t(message.note());
        const uint8_t velocity = uint8_t(message.velocity() < 1 ? 1 : message.velocity());
        post(Cmd::trigger_note, uint8_t(track), 0, note, velocity);
        record_note(track, note, velocity);
    }

    // --- live recording ---------------------------------------------------

    // Stamp a hit, undo the pad latency, quantize it, and post the step it belongs on.
    //
    // The stamp is `clock::state()` read here, in the main context, at the moment the event
    // arrived: whole ticks plus the fraction into the next. core/record.hpp explains why that is
    // a better timestamp than a sample counter would be. Everything after that is arithmetic in
    // core/, where it is host-tested -- this function only gathers the inputs.
    void record_note(int track, uint8_t note, uint8_t velocity) {
        if (!rec_) return;
        const enaga::clock::State clock = enaga::clock::state();
        if (!clock.running) return;          // nothing to record onto

        Stamp now;
        now.tick = clock.position;
        now.fraction = clock.fraction;
        const Stamp hit = stamp_back(now, kPadLatencyMs, clock.tempo);

        // The track's timing has to be resolved exactly as core/scheduler does it, including
        // swing inheritance -- if the two disagree about where a step sits, a hit played dead on
        // the beat records off it.
        const Pattern &pattern = engine_.pattern();
        const TrackPattern &source = pattern.tracks[track];
        TrackTiming timing;
        timing.length = source.length;
        timing.speed = Speed(source.speed);
        timing.swing = source.swing == kSwingInherit ? pattern.swing : source.swing;

        const Landing landing = quantize_hit(hit, timing, quantize_);

        mark(GestureKind::record, track, 0);     // step 0: one undo level for the whole pass
        Command command;
        command.cmd = Cmd::record_hit;
        command.track = uint8_t(track);
        command.step = landing.step;
        command.a = note;
        command.b = velocity;
        command.c = uint8_t(landing.micro);
        engine_.commands().post(command);

        if (landing.clamped) notice("OFF GRID");   // it could not be stored where it was played
    }

    void on_transport(int what, uint32_t position) override {
        // Under an external clock STRATA cannot start or stop anything; it follows. Resetting
        // the chain state here is what keeps conditions in step with an outside transport.
        (void)position;
        if (what == ENAGA_TRANSPORT_START || what == ENAGA_TRANSPORT_LOCATE) stop_armed_ = false;

        // docs/SPEC.md: "Autosave to AUTOSAVE/ whenever the transport stops." It follows the
        // transport rather than the key, so a stop arriving from MIDI or SYNC autosaves too --
        // which is the case where the user never pressed anything and would least expect to
        // lose work.
        if (what == ENAGA_TRANSPORT_STOP) storage_.request_autosave();
    }

    // --- LEDs -------------------------------------------------------------

    void update(uint32_t ms) override {
        last_ms_ = ms;

        // One bounded slice of whatever the card side has in flight. It never blocks and never
        // waits on the audio side: a command ring that has not drained simply means "not this
        // pass" (app/storage.hpp).
        storage_.service();

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

        // A compact signature of what is being edited, logged only when it changes.
        //
        // This is the only way a scripted emulator run can see that an edit actually landed.
        // There is no way to read pattern memory from outside the app, and the screen comes back
        // as a bitmap; without this, a scenario can prove the app did not crash and nothing
        // more. With it, "toggle a step, undo, and check the grid came back" is testable without
        // a unit -- which matters because app/ is the one part of this project that has no host
        // tests at all.
        //
        // Enaga's log ring holds sixteen lines, so this has to stay sparse: one line per edit,
        // never one per block. Logging only on a change is what makes that true.
        {
            uint32_t mask = 0;
            for (int i = 0; i < 16; i++) {
                const Step &s = pattern.tracks[selected_].steps[i + page_ * 16];
                if (s.flags & (kStepTrig | kStepTrigless)) mask |= 1u << i;
            }
            const uint8_t depth = status.undo_depth.load(std::memory_order_relaxed);
            if (mask != logged_mask_ || depth != logged_undo_) {
                logged_mask_ = mask;
                logged_undo_ = depth;
                char line[24];
                char *out = line;
                append(out, "g");
                append_hex4(out, mask);
                append(out, " u");
                append_number(out, depth);
                *out = 0;
                enaga::log(line);
            }
        }

        for (int i = 0; i < 16; i++) {
            const int step = i + page_ * 16;
            const Step &s = pattern.tracks[selected_].steps[step];
            const bool trig = (s.flags & kStepTrig) != 0;
            const bool trigless = (s.flags & kStepTrigless) != 0;
            const bool has_locks = s.lock_count > 0;
            const bool conditional = s.probability < 100 || condition_kind(s.condition) != Condition::NONE;

            uint8_t green = 0, red = 0;
            if (trig) {
                green = conditional ? 2 : 6;          // the spec's LED language
                if (has_locks) red = 3;               // amber: a trig with locks
            } else if (trigless) {
                // Locks with no note: red alone, so it reads as "something happens here but it
                // does not hit". The spec's LED table has no entry for this; a trigless step has
                // to be distinguishable from an empty one or the feature is invisible.
                red = 4;
            }
            // A step in the selection, or the one the step page is editing, is always visible
            // even if it is empty: these are modes you can forget you are in.
            if (range_first_ >= 0 && step >= range_first_ && step <= range_last_ && !trig) green = 1;
            if (step_page_ && step == page_step_) { green = 8; red = 8; }
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
        leds_.key(ENAGA_KEY_F2, rec_ ? 8 : 0, 0);        // REC is red while recording

        // CLR and OK light when undo and redo have somewhere to go, which is the only way to
        // know from the panel that F1 + CLR will do anything at all.
        leds_.key(ENAGA_KEY_CLR, status.undo_depth.load(std::memory_order_relaxed) ? 4 : 0, 0);
        leds_.key(ENAGA_KEY_OK, 0, status.redo_depth.load(std::memory_order_relaxed) ? 4 : 0);

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

        // Top line on every screen: the live pattern, tempo, play state, audio load. The pattern
        // comes from the status rather than from the project, because a chain or a queued change
        // moves it on the audio side.
        out = line;
        *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        append_padded(out, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
        *out++ = ' ';
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

        if (step_page_) {
            draw_step_page(canvas);
        } else {
            switch (mode_) {
                case Mode::grid:    draw_grid(canvas, running); break;
                case Mode::edit:    draw_edit(canvas); break;
                case Mode::mixer:   draw_mixer(canvas); break;
                case Mode::project: draw_project(canvas); break;
            }
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

        // The track's own layout: length and speed are per track (polymeter), so they belong on
        // screen next to the track number rather than behind a menu.
        const TrackPattern &track = engine_.pattern().tracks[selected_];
        out = line;
        append(out, "TRK ");
        append_number(out, uint32_t(selected_ + 1));
        append(out, muted_[selected_] ? " MUTE" : "     ");
        append(out, " PG");
        append_number(out, uint32_t(page_ + 1));
        append(out, " L");
        append_number(out, track.length);
        append(out, " X");
        append(out, speed_name(Speed(track.speed)));
        *out = 0;
        canvas.text(0, 84, line, 10);

        draw_edit_state(canvas, 92);
        draw_param_slots(canvas, 102);
    }

    // The line that says what editing mode you are in: recording, the quantize amount, the
    // selection, and how much undo there is. Without it, REC and a held selection are invisible
    // states that change what every other key does.
    void draw_edit_state(enaga::Canvas &canvas, int y) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        if (notice_showing()) {
            canvas.text(0, y, notice_, 15);
            return;
        }

        out = line;
        if (rec_) {
            append(out, "REC Q");
            append_number(out, uint32_t(quantize_));
            append(out, " ");
        }
        if (range_first_ >= 0) {
            append(out, "SEL ");
            append_number(out, uint32_t(range_first_ + 1));
            *out++ = '-';
            append_number(out, uint32_t(range_last_ + 1));
            append(out, " ");
        }
        append(out, "U");
        append_number(out, status.undo_depth.load(std::memory_order_relaxed));
        const uint8_t redo = status.redo_depth.load(std::memory_order_relaxed);
        if (redo) {
            append(out, "/R");
            append_number(out, redo);
        }
        const uint32_t refused = status.refused.load(std::memory_order_relaxed);
        if (refused) {
            // A refused edit is almost always a full lock pool. Saying nothing would leave
            // someone repeating an edit that cannot work.
            append(out, " FULL ");
            append_number(out, refused);
        }
        *out = 0;
        canvas.text(0, y, line, rec_ ? 15 : 9);
    }

    // --- the step page: one step's own fields ------------------------------

    void draw_step_page(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Step &step = engine_.pattern().tracks[selected_].steps[page_step_];

        out = line;
        append(out, "STEP ");
        append_number(out, uint32_t(page_step_ + 1));
        append(out, "  TRK ");
        append_number(out, uint32_t(selected_ + 1));
        *out = 0;
        canvas.text(0, 20, line, 14);

        // Which of the field pages is showing. Same language as EDIT's parameter pages.
        for (int p = 0; p < kFieldPages; p++) {
            canvas.rect(p * 32 + 2, 32, 28, 3, p == field_page_ ? 14 : 4);
        }

        // Four fields, each a name and a value. No bars here: a condition or a note name is not
        // a quantity, and a bar under "NOT PRE" would only suggest it was one.
        for (int slot = 0; slot < 4; slot++) {
            const int field = field_page_ * 4 + slot;
            const int y = 42 + slot * 14;
            if (field >= kStepFields) {
                canvas.text(0, y, "---", 4);
                continue;
            }

            canvas.text(0, y, step_field_name(StepField(field)), 13);

            // Sized from core/stepedit.hpp rather than guessed: the longest label is 8
            // characters and this is the one place a wrong guess would be a stack smash the
            // host build never sees.
            char text[kStepFieldTextMax];
            step_field_text(step, StepField(field), text, int(sizeof text));
            canvas.text(44, y, text, 15);
        }

        // What the step actually is, under the fields: a step that is off says so, because
        // every field above it is then inert.
        out = line;
        if (step.flags & kStepTrig) {
            append(out, "TRIG");
        } else if (step.flags & kStepTrigless) {
            append(out, "TRIGLESS");
        } else {
            append(out, "OFF");
        }
        if (step.lock_count) {
            append(out, "  LOCKS ");
            append_number(out, step.lock_count);
        }
        *out = 0;
        canvas.text(0, 100, line, 11);

        canvas.text(0, 112, "A    B    C    D", 7);
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

    // --- PROJECT: banks, patterns and the card ---------------------------

    // True when the key belonged to this mode. OK saves, F1 + OK reads the card again, F1 + CLR
    // starts an empty project, CLR cancels a queued change and UP toggles the chain.
    //
    // There is no file browser: a project's name comes from the settings and a track's sample
    // path comes from its kit, which is what docs/SPEC.md's "reopens the last project" and
    // "loaded whole into RAM at project open" need. Picking a different project or a different
    // sample from the card is a browser, and a browser is not in M4.
    bool on_project_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK:
                if (shift_) {
                    // Reading the card again throws away everything unsaved, so it says what it
                    // is doing rather than looking like nothing happened.
                    storage_.request_reload();
                    notice("RELOADING");
                } else {
                    storage_.request_save();
                    notice("SAVING");
                }
                return true;

            case ENAGA_KEY_CLR:
                if (shift_) {
                    storage_.request_new();
                    notice("NEW PROJECT");
                } else {
                    post(Cmd::cancel_queue);
                    notice("QUEUE OFF");
                }
                return true;

            case ENAGA_KEY_UP: {
                // The chain follows the project's own entries; starting one jumps to its first.
                const bool on = engine_.chain().active;
                post(Cmd::set_chain, 0, 0, on ? 0 : 1);
                notice(on ? "CHAIN OFF" : "CHAIN ON");
                return true;
            }

            case ENAGA_KEY_DOWN:
                // Load the selected track's sample again. The one thing a user can do about a
                // FAIL, and the only way back after a card has been out.
                if (storage_.request_sample(selected_)) notice("LOADING");
                else notice("NO SAMPLE");
                return true;

            default:
                return false;
        }
    }

    void queue_shown_pattern(int slot) {
        if (!project_index_valid(shown_bank_, slot)) return;
        // The audio side decides when it lands, from the tick the sequencer is actually on. With
        // the transport stopped it switches at once, because there is no end of a pass to wait
        // for (core/queue.hpp, and Engine::apply's queue_pattern).
        Command command;
        command.cmd = Cmd::queue_pattern;
        command.a = uint8_t(shown_bank_);
        command.b = uint8_t(slot);
        command.c = uint8_t(queue_mode_);
        engine_.commands().post(command);
        storage_.note_command(Cmd::queue_pattern,
                              int(engine_.status().live_bank.load(std::memory_order_relaxed)));

        char *out = notice_buffer_;
        *out++ = bank_letter(shown_bank_);
        append_padded(out, uint32_t(slot) + 1, 2);
        append(out, " ");
        append(out, queue_mode_name(queue_mode_));
        *out = 0;
        notice(notice_buffer_);
    }

    void draw_project(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        // The project, and whether RAM differs from the card. An unsaved project that looked
        // saved is the worst outcome this screen can produce, so the marker is on the name.
        out = line;
        append(out, storage_.project_name());
        if (storage_.dirty()) append(out, " *");
        if (!storage_.card_usable()) append(out, " NO CARD");
        *out = 0;
        canvas.text(0, 18, line, 15);

        // What the card side is doing, or what went wrong last. Both matter: a load that failed
        // silently is the least diagnosable thing a sampler can do.
        out = line;
        const char *busy = storage_.busy_text();
        if (busy != nullptr) {
            append(out, busy);
            append(out, " ");
            append_number(out, uint32_t(storage_.progress()));
            *out++ = '%';
        } else if (storage_.last_error() != nullptr) {
            append(out, storage_.last_error());
        }
        *out = 0;
        canvas.text(0, 27, line, busy != nullptr ? 12 : 9);

        // The global key and scale, on encoders B and C. Drawn from the live PATTERN rather than
        // from the project settings, because the pattern's copy is what the sequencer and every
        // mutation actually fold into -- a pattern loaded in its own key must read as that key.
        out = line;
        append(out, "KEY ");
        append(out, note_name(engine_.pattern().root));
        *out++ = ' ';
        append(out, scale_name(int(engine_.pattern().scale)));
        *out = 0;
        canvas.text(0, 94, line, 11);

        // The bank being browsed, which is not necessarily the one playing.
        out = line;
        append(out, "BANK ");
        *out++ = bank_letter(shown_bank_);
        append(out, "   ");
        append(out, queue_mode_name(queue_mode_));
        if (engine_.chain().active) append(out, " CHAIN");
        *out = 0;
        canvas.text(0, 40, line, 12);

        // The bank's 16 slots, four by four. Filled = the pattern has trigs; the live one is
        // boxed and a queued one is marked, so "what is playing" and "what will play" are both
        // on screen at once.
        const int live_bank = int(status.live_bank.load(std::memory_order_relaxed));
        const int live_slot = int(status.live_slot.load(std::memory_order_relaxed));
        const Queue &queue = engine_.queue();
        const Project &project = engine_.project();

        for (int slot = 0; slot < kPatternsPerBank; slot++) {
            const int x = (slot % 4) * 30 + 2;
            const int y = (slot / 4) * 13 + 51;

            int trigs = 0;
            const Pattern &pattern = project.patterns[shown_bank_][slot];
            for (int t = 0; t < kTracks && trigs == 0; t++) {
                for (int i = 0; i < int(pattern.tracks[t].length); i++) {
                    if (pattern.tracks[t].steps[i].flags & (kStepTrig | kStepTrigless)) {
                        trigs = 1;
                        break;
                    }
                }
            }

            const bool is_live = shown_bank_ == live_bank && slot == live_slot;
            const bool is_queued = queue.pending && int(queue.bank) == shown_bank_ &&
                                   int(queue.slot) == slot;
            canvas.rect(x, y, 26, 11, is_live ? 10 : (trigs ? 5 : 2));
            out = line;
            append_padded(out, uint32_t(slot) + 1, 2);
            if (is_queued) *out++ = '>';
            *out = 0;
            canvas.text(x + 3, y + 2, line, is_live ? 0 : 15);
        }

        // The sample pool: what each track is doing and how much of the budget is gone. The pool
        // lives on this side with the loader, so this is read directly and not through a status.
        const SamplePool &pool = storage_.samples();
        out = line;
        append(out, "SMP ");
        for (int t = 0; t < kTracks; t++) {
            const SampleState state = pool.slot(t).state;
            *out++ = state == SampleState::ready    ? 'O'
                     : state == SampleState::loading ? '.'
                     : state == SampleState::failed  ? 'X'
                                                     : '-';
        }
        append(out, " ");
        append_number(out, pool.used() >> 20);
        *out++ = '/';
        append_number(out, pool.budget() >> 20);
        append(out, "M");
        *out = 0;
        canvas.text(0, 105, line, 10);
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

        // Which of a project's ten files this edit will have changed. The live bank is read from
        // the status rather than assumed, because a chain may have moved the live pattern without
        // the UI asking (app/storage.hpp says why the flags live on this side).
        storage_.note_command(
            cmd, int(engine_.status().live_bank.load(std::memory_order_relaxed)));
    }

    // An encoder delta, as a signed byte. The audio side applies it to the authoritative step,
    // which is why these carry a delta and not a value (app/commands.hpp explains the choice).
    void post_delta(Cmd cmd, int track, int step, uint8_t a, int delta) {
        const int clamped = delta < -120 ? -120 : (delta > 120 ? 120 : delta);
        post(cmd, uint8_t(track), uint8_t(step), a, uint8_t(int8_t(clamped)));
    }

    // --- undo gestures ----------------------------------------------------
    //
    // Every edit is preceded by a mark, and the mark says which gesture it belongs to. The audio
    // side coalesces marks that carry the same gesture (core/undo.hpp), so a knob swept through
    // forty clicks is one undo level rather than forty -- which matters, because there are only
    // sixteen and the fortieth click would otherwise have thrown away everything else.
    //
    // `gesture_seq_` is what ends a gesture. Bumping it is this side's whole say in where one
    // undoable edit stops and the next starts: a step key coming up, recording stopping, the
    // selected track changing. Nothing else distinguishes "the same sweep" from "the same knob,
    // a minute later".
    void mark(GestureKind kind, int track, int step) {
        post(Cmd::undo_mark, uint8_t(track), uint8_t(step), uint8_t(kind), gesture_seq_);
    }

    void end_gesture() { gesture_seq_ = uint8_t(gesture_seq_ + 1); }

    // A short message on the top of the grid: what just happened, for about a second and a half.
    // Edits that are invisible on screen (a copy, a clear, an undo) need to say so or they look
    // like nothing happened.
    void notice(const char *text) {
        notice_ = text;
        notice_at_ms_ = last_ms_;
    }

    bool notice_showing() const {
        return notice_ != nullptr && uint32_t(last_ms_ - notice_at_ms_) < 1500u;
    }

    Engine engine_;

    // The card side: projects, autosave and the sample loader, all in this context. It owns the
    // sample region and the pool, so no pool state is shared across the ring at all
    // (app/storage.hpp).
    Storage storage_;

    Mode mode_ = Mode::grid;
    int selected_ = 0;
    int shown_bank_ = 0;                        // PROJECT mode browses a bank without switching
    QueueMode queue_mode_ = QueueMode::end_of_pattern;
    char notice_buffer_[24] = {};               // notice() keeps the pointer, so dynamic text
                                                // needs somewhere of its own to live
    int page_ = 0;            // step page 1-4, for 64-step tracks
    int param_page_ = int(Page::src);
    int mixer_bank_ = 0;      // 0 = tracks 1-4, 1 = tracks 5-8
    bool shift_ = false;
    bool stop_armed_ = false;
    bool clock_refused_ = false;   // the last transport call was refused: an external source
    bool muted_[kTracks] = {};

    // --- M3 edit state ----------------------------------------------------
    bool rec_ = false;             // F2: live and motion recording
    int quantize_ = 100;           // 0-100%, how hard a recorded hit snaps to its step
    int held_step_ = -1;           // the STEP key being held, absolute 0-63; -1 for none
    bool held_used_ = false;       // it was used as a modifier, so its release is not a tap
    int range_first_ = -1;         // two STEP keys held: the selection, inclusive
    int range_last_ = -1;
    bool step_page_ = false;       // the step page is open
    int page_step_ = 0;            // and on this step
    int field_page_ = 0;           // four of the step's fields at a time
    uint8_t gesture_seq_ = 0;
    bool value_pushed_ = false;    // VALUE held down: swing rather than tempo

    const char *notice_ = nullptr;
    uint32_t notice_at_ms_ = 0;
    uint32_t last_ms_ = 0;

    LedShadow leds_;
    uint32_t tempo_tenths_ = 1300;

    bool logged_running_ = false;
    int logged_source_ = -1;
    uint32_t logged_position_ = 0xFFFFFFFFu;
    uint32_t logged_mask_ = 0xFFFFFFFFu;   // deliberately impossible, so the first one logs
    uint8_t logged_undo_ = 0xFFu;
};

}  // namespace

ENAGA_APP(Strata)
