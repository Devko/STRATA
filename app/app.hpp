// STRATA's main context: every mode, every gesture, the screen and the LEDs.
//
// This is the only place that talks to Enaga, which keeps the rest of the project portable
// and host-testable (CLAUDE.md). It owns the main context: keys, pads, encoders, the screen and
// the LEDs. Everything that makes sound lives in app/engine.cpp and runs in the audio callback.
//
// The split is strict, and it is the whole architecture: this side never writes pattern memory.
// Every edit is posted as a command and applied by the audio side at the start of its next
// block, so an edit can never land halfway through a step and a save can never race the
// transport. docs/SPEC.md, "Software architecture".
//
// ## Why the class is in a header
//
// `ENAGA_APP(Strata)` defines the package's entry points, which belong to a device build, so
// `host/Makefile` cannot compile the translation unit that holds it -- and for as long as the
// class lived there too, every mode and every gesture was outside the host suite: about 2,000
// lines reachable only by a device build and four emulator scenarios that can press one key.
// The class is therefore here and `app/app.cpp` is the entry point and nothing else.
// `tests/test_ui.cpp` includes this file and drives the same class the unit runs, with no
// `#ifdef` anywhere in it.
//
// Nothing else includes this: `app/app.cpp` and `tests/test_ui.cpp` are its two readers, and
// the anonymous namespace below keeps everything in it internal to each of them.
#pragma once

#include "engine.hpp"
#include "record.hpp"       // live recording's stamping and quantizing, used from here only
#include "storage.hpp"      // projects, autosave and the sample loader: the card side

#include <enaga/app.hpp>
#include <enaga/clock.hpp>
#include <enaga/midi.hpp>

#include <cstdint>

// The one thing in here that is not also in a device build: tests/test_ui.cpp reads the UI state
// a gesture leaves behind -- the held step, the selection, the mode -- and the Engine and Storage
// this object owns, none of which has an accessor because nothing in the app needs one. A single
// friend declaration is the whole mechanism: no test-only member function, no #ifdef, and the
// class below is otherwise exactly the text app/app.cpp held.
namespace strata::test {
struct UiProbe;
}  // namespace strata::test

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

// The screen holds 21 characters a line: 128 pixels at the font's 6-pixel advance
// (<enaga/ui.hpp>'s line_chars). A longer string is cut off at the right edge on the unit.
constexpr int kLineChars = 21;

// What Enaga's font can draw (third_party/enaga/firmware/hal/gfx.c, font_chars). Anything else
// comes out as '?'. tests/test_ui.cpp holds every screen to the same list.
bool font_has(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    for (const char *p = " !#+-./:=?%>"; *p; p++) {
        if (*p == c) return true;
    }
    return false;
}

// Text that did not come from this file -- a project or take name off the card -- as the font
// can draw it: lower case raised, anything else the font lacks shown as '-', at most `max`
// characters. "strata_1.wav" would otherwise read "STRATA?1.WAV".
void append_shown(char *&out, const char *text, int max) {
    for (int n = 0; text != nullptr && text[n] != '\0' && n < max; n++) {
        char c = text[n];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        *out++ = font_has(c) ? c : '-';
    }
}

// Append `part` only if the line still fits on screen afterwards. For a status line built from
// optional pieces in order of importance: what does not fit is left out whole, rather than
// running off the edge half-drawn.
void append_if_fits(char *&out, const char *line, const char *part) {
    int used = int(out - line), extra = 0;
    while (part[extra] != '\0') extra++;
    if (used + extra <= kLineChars) append(out, part);
}

void append_hex4(char *&out, uint32_t value) {
    for (int shift = 12; shift >= 0; shift -= 4) {
        const uint32_t nibble = (value >> shift) & 0xFu;
        *out++ = char(nibble < 10 ? '0' + nibble : 'A' + (nibble - 10));
    }
}

// --- the MIXER's pages ----------------------------------------------------------------------
//
// Two pages of track faders and then the sends and the master, four encoders at a time.
// docs/SPEC.md gives the sends no gesture of their own and the master only F1 + VALUE for the DJ
// filter, so this is a DEVIATION, recorded in docs/REVIEW.md: fourteen settings that reach a
// file have to be reachable from the panel, and MIXER is the mode that is already about the
// buses. The DJ filter is on F1 + VALUE as well, as the spec requires.
//
// `field` indexes dsp/buses.hpp's SendField, or MasterField on the page marked `master`. -1 is
// an empty slot, which the routing page has two of: ten send fields do not divide into four.
struct MixerPage {
    const char *name;
    bool master;
    int8_t field[4];
};

constexpr int kMixerTrackPages = 2;

constexpr MixerPage kMixerPages[] = {
    {"TRK 1-4", false, {-1, -1, -1, -1}},        // the faders, handled on their own
    {"TRK 5-8", false, {-1, -1, -1, -1}},
    {"DLY", false, {int8_t(SendField::delay_seconds), int8_t(SendField::delay_feedback),
                    int8_t(SendField::delay_tone), int8_t(SendField::delay_level)}},
    {"REV", false, {int8_t(SendField::reverb_decay_s), int8_t(SendField::reverb_damping),
                    int8_t(SendField::reverb_high_pass_hz), int8_t(SendField::reverb_level)}},
    {"ROUTE", false, {int8_t(SendField::delay_ping_pong), int8_t(SendField::delay_to_reverb),
                      -1, -1}},
    {"MST", true, {int8_t(MasterField::dj), int8_t(MasterField::compressor),
                   int8_t(MasterField::limit), int8_t(MasterField::level)}},
};

constexpr int kMixerPageCount = int(sizeof(kMixerPages) / sizeof(kMixerPages[0]));

// Every send and master field has a slot on some page. Without this a field added to
// dsp/buses.hpp would be saved, loaded and completely unreachable -- which is the bug this whole
// change exists to fix, so it is worth a compile-time check rather than a comment.
constexpr bool mixer_pages_cover_every_field() {
    for (int f = 0; f < int(SendField::count); f++) {
        bool found = false;
        for (int p = kMixerTrackPages; p < kMixerPageCount; p++) {
            if (kMixerPages[p].master) continue;
            for (int slot = 0; slot < 4; slot++) found = found || kMixerPages[p].field[slot] == f;
        }
        if (!found) return false;
    }
    for (int f = 0; f < int(MasterField::count); f++) {
        bool found = false;
        for (int p = kMixerTrackPages; p < kMixerPageCount; p++) {
            if (!kMixerPages[p].master) continue;
            for (int slot = 0; slot < 4; slot++) found = found || kMixerPages[p].field[slot] == f;
        }
        if (!found) return false;
    }
    return true;
}

static_assert(mixer_pages_cover_every_field(),
              "a send or master field has no MIXER slot, so nothing can reach it");

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

// docs/SPEC.md's performance row: the seven UPPER pads, which the panel controller numbers with
// the odd ids. Fixed, in every mode -- that is what makes it a performance row rather than
// another page.
enum class PerformPad : uint8_t { fill = 1, repeat = 3, mute = 5, scene_a = 7, scene_b = 9,
                                  throw_fx = 11, mutate = 13 };

constexpr bool is_perform_pad(int pad) { return pad % 2 == 1 && pad >= 1 && pad <= 13; }

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// The octave the pads play the root in. The pattern's root is a pitch class (core/pattern.hpp),
// so it needs one to become a note.
constexpr uint8_t kPadOctaveNote = 60;

// PERFORM's macro bars, in pixels: tall enough to read, short enough to end above the hints.
constexpr int kMacroBar = 18;

// The velocity a STEP key plays at. Keys have none of their own, and 100 is what the default
// kit's trigs use, so a part played in sounds like a part programmed in.
constexpr uint8_t kKeyVelocity = 100;

// The modes of docs/SPEC.md's function block. F4 = GRID, F7 = MIXER, F8 = EDIT, and the two
// selection keys under the screen move within whichever mode is showing -- the step page in
// GRID, the parameter page in EDIT, the bank of four tracks in MIXER.
enum class Mode : uint8_t { grid, edit, mixer, project, perform, keys, resample, machine, kit };

// How long a take is. docs/SPEC.md: "1, 2, 4 or 8 bars", so these four and nothing between --
// a resample is a loop and a loop is a power of two bars.
constexpr uint8_t kTakeBars[] = {1, 2, 4, 8};
constexpr int kTakeBarChoices = int(sizeof kTakeBars / sizeof kTakeBars[0]);

// The step page shows four of a step's fields at a time, so the selection keys page through
// them. Derived rather than written down: core/stepedit.hpp owns how many fields there are.
constexpr int kFieldPages = (kStepFields + 3) / 4;

// The mode's name, for the screen-change log line. Upper case, like everything the device font
// can draw, and short: a log line is 23 characters.
const char *mode_name(Mode mode) {
    switch (mode) {
        case Mode::grid:     return "GRID";
        case Mode::edit:     return "EDIT";
        case Mode::mixer:    return "MIXER";
        case Mode::project:  return "PROJECT";
        case Mode::perform:  return "PERFORM";
        case Mode::keys:     return "KEYS";
        case Mode::resample: return "RESAMPLE";
        case Mode::machine:  return "MACHINE";
        case Mode::kit:      return "KIT";
        default:             return "?";
    }
}

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
    friend struct strata::test::UiProbe;    // tests/test_ui.cpp only; see the note above

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

        // Hold STEP + push A-D: take that parameter's lock off the held step -- the undo of
        // "hold STEP + turn A-D", on the same knob. Without it a lock could be turned but never
        // removed short of erasing the whole step. The pushes are numbered B, A, C, D in the ABI
        // (docs/ENAGA_API.md), so they are mapped by name rather than by arithmetic.
        if (held_step_ >= 0 && (key == ENAGA_KEY_PUSH_A || key == ENAGA_KEY_PUSH_B ||
                                key == ENAGA_KEY_PUSH_C || key == ENAGA_KEY_PUSH_D)) {
            if (!down) return;
            const int encoder = key == ENAGA_KEY_PUSH_A   ? 0
                                : key == ENAGA_KEY_PUSH_B ? 1
                                : key == ENAGA_KEY_PUSH_C ? 2
                                                          : 3;
            const int index = page_first(Page(param_page_)) + encoder;
            held_used_ = true;
            mark(GestureKind::erase, selected_, held_step_);
            post(Cmd::clear_lock, uint8_t(selected_), uint8_t(held_step_), uint8_t(index));
            end_gesture();
            notice("LOCK CLEARED");
            return;
        }

        // MUTE mode turns STEP 1-8 into the eight stored mute states: recall on a tap, store
        // with VALUE pushed (F1 is taken: it makes a recall wait for the bar). docs/SPEC.md says they are "recalled from the upper row", but the upper row is
        // seven FIXED performance pads and there are eight states, so that cannot be taken
        // literally. The STEP keys are the only run of eight free while MUTE mode is on, and
        // MUTE mode is a mode, so repurposing them there is the smallest deviation available.
        // A recall with F1 held lands on the bar, like a mute does.
        if (mute_mode_ && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 8) {
            if (!down) return;
            const int slot = key - ENAGA_KEY_STEP1;
            char *out = notice_buffer_;
            if (value_pushed_) {
                post(Cmd::mute_store, 0, 0, uint8_t(slot));
                append(out, "STORED ");
            } else {
                post(Cmd::mute_recall, 0, 0, uint8_t(slot), shift_ ? 1 : 0);
                append(out, shift_ ? "NEXT BAR " : "RECALL ");
            }
            append_number(out, uint32_t(slot) + 1);
            *out = 0;
            notice(notice_buffer_);
            return;
        }

        // A scene pad held turns the STEP keys into the sixteen scenes, in whatever mode is on
        // screen: docs/SPEC.md gives "hold + STEP picks the scene" and the performance row is
        // deliberately not modal.
        if (scene_holding_ >= 0 && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) {
                const int scene = key - ENAGA_KEY_STEP1;
                post(Cmd::scene_select, 0, 0, uint8_t(scene_holding_), uint8_t(scene));
                char *out = notice_buffer_;
                append(out, scene_holding_ == 0 ? "A " : "B ");
                append_number(out, uint32_t(scene) + 1);
                *out = 0;
                notice(notice_buffer_);
            }
            return;
        }

        // In KIT mode the sixteen STEP keys are the sixteen kit slots: a press puts the cursor
        // there, and OK (twice) recalls it. Only the cursor -- a slot key that loaded at once
        // would throw the sound away on a stray press.
        if (mode_ == Mode::kit && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) move_kit_cursor_to(key - ENAGA_KEY_STEP1);
            return;
        }

        // In PROJECT mode a STEP key queues a pattern change rather than toggling a trig, so it
        // is taken here, before the editing path, and only on the press -- there is no modifier
        // gesture to decide on the release.
        if (mode_ == Mode::project && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (!down) return;
            // On the CHAIN page a STEP key is that slot of the shown bank, written into the
            // entry under the cursor -- or the first entry, when the chain is empty.
            if (project_page_ == ProjectPage::chain) {
                chain_put_slot(key - ENAGA_KEY_STEP1);
                return;
            }
            slot_cursor_ = key - ENAGA_KEY_STEP1;       // the cursor follows, so OK repeats it
            queue_shown_pattern(slot_cursor_);
            return;
        }

        // In KEYS the STEP keys are an instrument: sixteen degrees of the project's scale from
        // the root up, which is what docs/SPEC.md means by "STEP keys play notes in scale".
        // The press plays the note and the release lets it go (play_key, below).
        if (mode_ == Mode::keys && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) play_key(key - ENAGA_KEY_STEP1);
            else release_key(key - ENAGA_KEY_STEP1);
            return;
        }

        // And in PERFORM they are the sixteen parameters of the selected track, assigned to the
        // selected macro. The STEP keys are already a sixteen-wide selector in the step page, so
        // this is the gesture they have; without it a macro could never be set up at all, and
        // "4 macros driving up to 4 parameters" would be a feature with no way in.
        if (mode_ == Mode::perform && key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            if (down) assign_macro(key - ENAGA_KEY_STEP1);
            return;
        }

        // A STEP key needs its release as well as its press. Holding one is a modifier -- it
        // locks a parameter, opens the step page or starts a range -- and a step held as a
        // modifier must NOT toggle its trig when it comes up. That can only be decided on the
        // release: at the moment of the press there is no way to know which it was yet.
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            on_step_key(key - ENAGA_KEY_STEP1, down);
            return;
        }

        if (!down) return;

        // PROJECT and PERFORM mode's own keys. Taken before the switch so the editing meanings
        // of OK, CLR and the arrows cannot fire while one of them is what is on screen.
        if (mode_ == Mode::project && on_project_key(key)) return;
        if (mode_ == Mode::kit && on_kit_key(key)) return;
        // A mutation waiting for its verdict takes OK and CLR first, in whatever mode: OK keeps
        // it, CLR reverts it. Only the plain keys; F1 + CLR and F1 + OK stay undo and redo.
        if (mutation_pending_ && !shift_ && (key == ENAGA_KEY_OK || key == ENAGA_KEY_CLR)) {
            mutation_pending_ = false;
            if (key == ENAGA_KEY_CLR) {
                post(Cmd::undo);
                end_gesture();
                notice("REVERTED");
            } else {
                notice("KEPT");
            }
            return;
        }
        if (mode_ == Mode::perform && on_perform_key(key)) return;

        switch (key) {
            case ENAGA_KEY_F5:
                // Not while a project is loading. `step_load` rewrites pattern memory a pass at
                // a time, and the audio side would play a pattern half of which belongs to the
                // project being replaced. The load itself stops the transport; this is the gap
                // where someone starts it again before the load has finished. A SAMPLE load is
                // different and is allowed: playing through one is the point of loading in the
                // background, and the pool's epoch handshake makes it safe.
                if (storage_.loading_project()) {
                    notice("LOADING");
                    break;
                }
                // The transport is Enaga's and its calls are MAIN CONTEXT, so they happen here
                // rather than through the command ring. start() returns false under an external
                // clock source, where STRATA can only follow -- the screen shows EXT and the
                // refusal is simply recorded.
                if (shift_) {
                    // F1 + F5: restart from step 1 (docs/SPEC.md), playing or not. Running, the
                    // clock is located back to 0 and keeps going; stopped, START already begins
                    // at 0. Either way the audio side resets what it measures from the position.
                    // An outside clock refuses both, and then nothing is reset: the playheads
                    // must stay with the master.
                    const bool ok = enaga::clock::running() ? enaga::clock::locate(0)
                                                            : enaga::clock::start();
                    clock_refused_ = !ok;
                    if (!ok) {
                        notice("EXT CLOCK");
                        break;
                    }
                    stop_armed_ = false;
                    post(Cmd::restart);
                    notice("RESTART");
                    break;
                }
                clock_refused_ = !enaga::clock::start();
                stop_armed_ = false;
                post(Cmd::play);                       // for the audio side's trace only
                break;
            case ENAGA_KEY_F6:
                if (shift_) {
                    // F1 + F6: stop and reset all tracks (docs/SPEC.md). Everything a plain STOP
                    // does, and the second press's cut, in one: the tails are cut, a queued
                    // pattern change is dropped and the clock goes back to 0, so the next PLAY
                    // or CONTINUE starts at step 1 with nothing pending. Refused under an
                    // outside clock that keeps running, exactly as plain F6 is.
                    clock_refused_ = !enaga::clock::stop();
                    if (clock_refused_ && enaga::clock::running()) {
                        notice("EXT CLOCK");
                        break;
                    }
                    enaga::clock::locate(0);
                    post(Cmd::stop, 0, 0, 1);
                    post(Cmd::cancel_queue);
                    stop_armed_ = false;
                    notice("RESET");
                    break;
                }
                clock_refused_ = !enaga::clock::stop();
                // Refused while an outside clock keeps running: nothing stopped, so nothing is
                // released or reset either -- doing so cut every voice and sent the playheads back
                // to step 1 while the clock carried on, and the pattern then played out of step
                // with the master. STRATA follows the outside STOP when it comes (on_transport).
                if (clock_refused_ && enaga::clock::running()) {
                    notice("EXT CLOCK");
                    break;
                }
                // The second press cuts the tails. Only that part is the audio side's job.
                post(Cmd::stop, 0, 0, stop_armed_ ? 1 : 0);
                stop_armed_ = !stop_armed_;
                break;

            case ENAGA_KEY_F2:                         // REC, or F1 + F2 for resample setup
                if (shift_) {
                    // docs/SPEC.md's key table gives F1 + F2 to "Resample setup". A mode rather
                    // than a one-shot, because a take has two settings to choose -- how many
                    // bars and where it lands -- and OK is what commits it.
                    mode_ = Mode::resample;
                    leave_mode();
                    notice("RESAMPLE");
                    break;
                }
                rec_ = !rec_;
                // A whole recording pass is one undo level, so the gesture ends when recording
                // does -- and the next pass must not coalesce into the last one.
                end_gesture();
                notice(rec_ ? "REC ON" : "REC OFF");
                break;

            // Every one of these ends a STEP hold as well. KEYS, PERFORM and the mute pads
            // swallow the STEP keys ahead of on_step_key, so a key held across a mode change
            // never delivers its release and the hold would last until the app was restarted --
            // with the next encoder move writing a lock onto it.
            case ENAGA_KEY_F3:                         // PROJECT: banks, patterns and the card
                mode_ = Mode::project;
                leave_mode();
                if (shift_) {
                    // docs/SPEC.md's key table: "F1 + F3 twice = save project". Twice, so a
                    // save is never one stray press; within two seconds, so an old first press
                    // does not arm a save minutes later. OK used to save here, but OK is what
                    // picks the slot under the cursor now, as it picks in every Enaga list.
                    if (save_armed_ && uint32_t(last_ms_ - save_armed_at_ms_) < kSaveArmMs) {
                        save_armed_ = false;
                        storage_.request_save();
                        notice("SAVING");
                    } else {
                        save_armed_ = true;
                        save_armed_at_ms_ = last_ms_;
                        notice("FUNC+PROJECT TO SAVE");
                    }
                }
                break;
            case ENAGA_KEY_F4:                         // HOME: the grid, or KEYS with F1
                mode_ = shift_ ? Mode::keys : Mode::grid;
                leave_mode();
                break;
            case ENAGA_KEY_F7:                         // MIXER, or PERFORM with F1
                mode_ = shift_ ? Mode::perform : Mode::mixer;
                leave_mode();
                break;
            case ENAGA_KEY_F8:                         // EDIT: the sound pages, or machine select
                if (shift_) {
                    // docs/SPEC.md's key table: F1 + F8 is "Kit and machine select". The first
                    // press is the machine select; pressed again from there it is the kit
                    // browser, and from the browser it goes back -- one key for both halves, as
                    // the spec gives it, and MACHINE's screen says so.
                    if (mode_ == Mode::machine) {
                        open_kits();
                    } else {
                        mode_ = Mode::machine;
                        leave_mode();
                        notice("MACHINE");
                    }
                    break;
                }
                mode_ = Mode::edit;
                leave_mode();
                break;

            case ENAGA_KEY_OK:
                // RESAMPLE mode first: OK is what commits a take, and there is no step to hold.
                if (mode_ == Mode::resample) {
                    post(Cmd::resample_arm, 0, 0, kTakeBars[take_bars_index_]);
                    // And the card side, asked for now so the write happens the moment the take
                    // finishes rather than needing a second press at exactly the right time.
                    storage_.request_resample_write(resample_track_);
                    notice(enaga::clock::running() ? "ARMED" : "ARMED: PRESS PLAY");
                    break;
                }
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
                } else if (step_page_ && held_step_ < 0 && range_first_ < 0) {
                    // CLR is "back" on Enaga's lists, and the step page is a page you opened.
                    // It used to fall through to erase_selection, which with nothing selected
                    // erases the WHOLE TRACK -- from a screen showing one step.
                    step_page_ = false;
                } else if (mode_ == Mode::resample) {
                    // In RESAMPLE, CLR cancels the take rather than erasing a step: there is no
                    // selection in this mode and a take is the only thing there is to cancel.
                    post(Cmd::resample_cancel);
                    notice("CANCELLED");
                } else {
                    erase_selection();
                }
                break;

            // The arrow keys navigate, as they do in Enaga's own menu and SETTINGS page: UP and
            // DOWN move through the rows on screen, LEFT and RIGHT move sideways (see
            // `vertical` and `horizontal`). PROJECT and PERFORM take theirs in their own key
            // handlers above.
            //
            // Copy and paste moved to F1 + UP and F1 + DOWN -- and stay on plain UP and DOWN
            // while a STEP is held, because a key held with a step is already a chord on that
            // step and cannot be navigation. UP takes the selection, DOWN puts it down again.
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_ || held_step_ >= 0) {
                    if (key == ENAGA_KEY_UP) copy_selection(); else paste_selection();
                } else {
                    vertical(key == ENAGA_KEY_UP ? -1 : +1);
                }
                break;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (!shift_) horizontal(key == ENAGA_KEY_LEFT ? -1 : +1);
                break;

            // The octave keys are the step page in GRID; with F1 they are the track's length,
            // which is what makes polymeter reachable without a menu. The spec's key table.
            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT: {
                const int delta = key == ENAGA_KEY_OCTAVE_LEFT ? -1 : +1;
                if (mode_ == Mode::keys) {
                    // "Step page 1-4 in GRID; octave in KEYS" (docs/SPEC.md's key table).
                    walk_keys_octave(delta);
                    break;
                }
                if (mode_ == Mode::project) {
                    // PROJECT mode's SETUP page: the unit's own settings, and the one project
                    // preference that has nowhere else to live now that F1 + VALUE is the DJ
                    // filter the spec asks for. A DEVIATION, recorded in docs/REVIEW.md: every
                    // key in docs/SPEC.md's table is taken, plain and with F1, and the octave
                    // keys do nothing in this mode -- they were setting GRID's step page, which
                    // is not on screen here.
                    // Four pages now: the slots, CHAIN, PATTERN and SETUP, walked in that order
                    // and stopping at the ends.
                    const int next = int(project_page_) + delta;
                    const int last = int(ProjectPage::count) - 1;
                    project_page_ = ProjectPage(next < 0 ? 0 : (next > last ? last : next));
                    notice(project_page_name(project_page_));
                    break;
                }
                if (shift_) {
                    // A delta, applied by the audio side (app/commands.hpp, kDeltaFlag).
                    mark(GestureKind::length, selected_, 0);
                    post(Cmd::set_track_length, uint8_t(selected_), 0, 0, clamp_delta(delta),
                         kDeltaFlag);
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

    // --- the arrow keys -----------------------------------------------------
    //
    // One model for every mode, Enaga's own (its menu and SETTINGS page): UP and DOWN move the
    // cursor through the rows on screen, LEFT and RIGHT move sideways -- the page, where the
    // screen has pages, exactly as the selection keys do. OK picks and CLR goes back. F1 + an
    // arrow is the mode's second meaning (copy and paste in the editing modes).

    void vertical(int delta) {
        switch (mode_) {
            case Mode::grid:
            case Mode::edit:
            case Mode::keys:
                // The tracks are the rows: GRID draws them top to bottom. Silent, unlike a pad,
                // which plays the track it selects.
                walk_track(delta);
                break;
            case Mode::machine:
                // The machine list, applied at once like encoder A.
                walk_machine(delta);
                break;
            case Mode::resample:
                // The take's length, A's other way in.
                take_bars_index_ = (take_bars_index_ + delta + kTakeBarChoices) % kTakeBarChoices;
                break;
            case Mode::mixer:                   // nothing is selected by row on the mixer
            case Mode::project:                 // PROJECT, PERFORM and KIT have their own handlers
            case Mode::perform:
            case Mode::kit:
                break;
        }
    }

    void horizontal(int delta) {
        if (step_page_) {
            // The step page walks along the track, so a run of steps is edited without going
            // back to the grid for each one.
            const int length = int(engine_.pattern().tracks[selected_].length);
            page_step_ = (page_step_ + delta + length) % (length < 1 ? 1 : length);
            return;
        }
        if (mode_ == Mode::keys) {
            walk_keys_octave(delta);
            return;
        }
        select(delta);                          // the page, or what the selection keys walk
    }

    void walk_track(int delta) {
        const int next = (selected_ + delta + kTracks) % kTracks;
        if (next != selected_) end_gesture();   // a different track is a different edit
        selected_ = next;
    }

    void walk_machine(int delta) {
        const int count = int(MachineId::count);
        const int next = (int(engine_.machine_id(selected_)) + delta) % count;
        post(Cmd::set_machine, uint8_t(selected_), 0, uint8_t(next < 0 ? next + count : next));
    }

    void walk_keys_octave(int delta) {
        keys_octave_ = clamp_int(keys_octave_ + delta, -3, 4);
        char *out = notice_buffer_;
        append(out, "OCT ");
        append_number(out, uint32_t(keys_octave_ + 3));
        *out = 0;
        notice(notice_buffer_);
    }

    // --- STEP keys: tap, hold, and a pair for a range --------------------

    // `raw` is the physical key, 0-15. The ABSOLUTE step it means depends on the page, and the
    // page can move while a key is held -- the UP and DOWN keys are not blocked by a hold -- so
    // the page is resolved on the PRESS and remembered. Matching the release on the raw key
    // rather than on a recomputed absolute step is the point: the key you let go of is the key
    // you pressed, whatever the page says by then. Resolving it twice stranded `held_step_` at
    // the old page's value, and a stranded hold turns the next encoder move into a parameter
    // lock on a step nobody is touching.
    // Forget the STEP key being held, and the range it was building. Called on the release and
    // on anything that makes the hold unanswerable.
    void release_hold() {
        held_step_ = -1;
        held_key_ = -1;
        held_used_ = false;
    }

    // What every mode change does: close the step page and drop any hold.
    // Drop a STEP hold that can no longer be answered, and end its gesture. Separate from
    // `leave_mode` because the performance pads deliberately do NOT change the mode -- the row
    // works in every one of them -- so they need the hold dropped without the rest.
    void drop_hold() {
        if (held_step_ < 0) return;
        release_hold();
        end_gesture();
    }

    void leave_mode() {
        step_page_ = false;
        mutation_pending_ = false;
        // A KEYS key held across a mode change: its release goes to the next mode, so the note
        // is let go here or it would hang on a sustaining machine.
        for (int i = 0; i < 16; i++) release_key(i);
        if (held_step_ >= 0) {
            release_hold();
            end_gesture();
        }
    }

    void on_step_key(int raw, bool down) {
        const int step = raw + page_ * 16;
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
            held_key_ = raw;
            held_used_ = false;
            return;
        }

        if (raw != held_key_) return;          // the second key of a range coming back up

        if (!held_used_) {
            // A tap. With F1 it toggles the trigless bit instead: a step that applies its locks
            // without sounding a note.
            mark(GestureKind::trig, selected_, step);
            // On the step the hold STARTED on, not on what this key would mean now.
            post(Cmd::toggle_trig, uint8_t(selected_), uint8_t(held_step_), shift_ ? 1 : 0);
        }
        release_hold();
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
                // Faders, then the delay, the reverb, the routing and the master.
                mixer_bank_ =
                    (mixer_bank_ + delta + kMixerPageCount * 2) % kMixerPageCount;
                break;
            case Mode::project:
                // The bank being SHOWN, which is not the bank playing: a queued change is armed
                // from the shown bank, so browsing must not switch anything by itself.
                shown_bank_ = (shown_bank_ + delta + kBanks) % kBanks;
                break;
            case Mode::keys:
                // The selected track, so a part can be played in on one track and then another
                // without leaving the mode.
                selected_ = (selected_ + delta + kTracks) % kTracks;
                end_gesture();
                break;
            case Mode::machine:
                // The selection keys walk the TRACK, so one hand can go along the eight of them
                // without leaving the mode -- the same thing they do in KEYS.
                selected_ = (selected_ + delta + kTracks) % kTracks;
                end_gesture();
                break;
            case Mode::resample:
                // The selection keys walk the DESTINATION: -1 is "the file only", then the eight
                // tracks. A track that hosts no Sampler is still offered, because which machine
                // a track hosts becomes switchable in M6/M7 and the kit's path is what the
                // loader reads either way.
                resample_track_ = resample_track_ + delta;
                if (resample_track_ < -1) resample_track_ = kTracks - 1;
                if (resample_track_ >= kTracks) resample_track_ = -1;
                break;
            case Mode::perform:
                // How hard a mutation mutates. The one number in this mode with no encoder of
                // its own, all four being macros.
                mutate_amount_ = (mutate_amount_ + delta * 10 + 110) % 110;
                if (mutate_amount_ > 100) mutate_amount_ = 100;
                break;
            case Mode::kit:
                // The selection keys flip the browser's two pages of eight, like LEFT and RIGHT.
                move_kit_cursor_to(kit_cursor_ + delta * 8);
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
            notice("CLEARED FUNC+CLR UNDO");    // 21 characters at most
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
            // VALUE picks the beat-repeat length while that pad is held, which is what
            // docs/SPEC.md gives it, and it has to come before the tempo.
            if (repeat_held_) {
                const int next = int(repeat_length_) + delta;
                repeat_length_ = RepeatLength(
                    next < 0 ? 0
                             : (next >= int(RepeatLength::count) ? int(RepeatLength::count) - 1
                                                                 : next));
                post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), 1, shift_ ? 1 : 0);
                notice(repeat_length_name(repeat_length_));
                return;
            }
            // A held STEP key: VALUE sets that step's note (docs/SPEC.md, "Step editing": hold
            // STEP + VALUE). Before the tempo, which it used to turn instead -- with a finger
            // on a step, the tempo is the last thing the hand means. The same field and the
            // same undo coalescing as the step page's NOTE, and the key counts as used, so its
            // release does not toggle the trig.
            if (held_step_ >= 0) {
                held_used_ = true;
                mark(GestureKind::field, selected_, held_step_);
                post_delta(Cmd::step_field_delta, selected_, held_step_,
                           uint8_t(StepField::note), delta);
                notice("NOTE");
                return;
            }
            // PROJECT's CHAIN and PATTERN pages: VALUE edits the thing under the cursor -- the
            // entry's repeat count, or the highlighted field. Not with VALUE pushed (swing) or F1
            // (the DJ filter), which keep their meanings everywhere.
            if (mode_ == Mode::project && !value_pushed_ && !shift_) {
                if (project_page_ == ProjectPage::chain) {
                    chain_delta(ChainEdit::repeats_delta, delta);
                    return;
                }
                if (project_page_ == ProjectPage::pattern) {
                    pattern_field_delta(pattern_field_, delta);
                    return;
                }
            }
            // In PERFORM, VALUE is the morph: the one continuous control the layer has.
            if (mode_ == Mode::perform && !value_pushed_ && !shift_) {
                const int next = int(morph_) + delta * (int(kMorphScale) / 64);
                morph_ = uint16_t(next < 0 ? 0 : (next > int(kMorphScale) ? int(kMorphScale)
                                                                         : next));
                Command command;
                command.cmd = Cmd::set_morph;
                command.u = morph_;
                post_command(command);
                return;
            }
            if (value_pushed_) {
                // VALUE pushed and turned is the pattern's swing, as the spec's key table says.
                mark(GestureKind::length, 0, 0);
                post(Cmd::set_pattern_swing, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
                return;
            }
            if (shift_) {
                // The MASTER DJ FILTER, which is what docs/SPEC.md's key table gives to
                // F1 + VALUE. It held the record quantize amount until the master had any
                // settable field at all; quantize moved to PROJECT mode's SETUP page, where
                // the rest of the unit's preferences are.
                //
                // A delta, for the same reason as every other encoder gesture. The notice shows
                // the byte BEFORE this click, because the one after it does not exist on this
                // side until the audio side has applied the command -- about 1.3 ms.
                post_delta(Cmd::set_master, 0, 0, uint8_t(MasterField::dj),
                           delta * kMasterFieldClick);
                char *out = notice_buffer_;
                append(out, "DJ ");
                const int signed_value =
                    int(engine_.project().settings.master[int(MasterField::dj)]) - 64;
                if (signed_value == 0) {
                    append(out, "OFF");
                } else {
                    append(out, signed_value < 0 ? "LP" : "HP");
                    append_number(out, uint32_t(signed_value < 0 ? -signed_value : signed_value));
                }
                *out = 0;
                notice(notice_buffer_);
                return;
            }
            // REFUSED under an external clock, rather than moved and then ignored. docs/SPEC.md:
            // VALUE "shows EXT and refuses edits when CLOCK SRC is not INTERNAL". Before this
            // the knob moved `tempo_tenths_` and Enaga threw the call away, so the app's idea of
            // the tempo and the unit's drifted apart with every click.
            if (enaga::clock::source() != ENAGA_CLOCK_INTERNAL) {
                clock_refused_ = true;
                notice("EXT CLOCK");
                return;
            }
            // Clamped as a SIGNED value, before the cast. `tempo_tenths_` is unsigned, so a
            // delta large enough to take the sum below zero used to wrap to about four billion
            // and then clamp to 3000 -- 300 BPM -- when the user was turning the knob DOWN. Not
            // reachable from the panel, where an encoder event carries a click or two, but it is
            // one line either way and the wrong end of the range is a bad place to arrive.
            int tenths = int(tempo_tenths_) + delta;
            if (tenths < 200) tenths = 200;                    // Enaga's clock allows 20-300
            if (tenths > 3000) tenths = 3000;
            tempo_tenths_ = uint32_t(tenths);
            // Main context again.
            clock_refused_ = !enaga::clock::set_tempo(float(tempo_tenths_) * 0.1f);
            // And the project, which is where the tempo is SAVED. Nothing posted this before, so
            // `ProjectSettings::tempo_centi_bpm` round-tripped through the serializer while
            // holding the default for ever: a project saved at 142 reopened at 130. The engine
            // also re-derives the tempo-synced delay from it.
            post_tempo(float(tempo_tenths_) * 0.1f);
            adopted_tempo_centi_ = uint16_t(tempo_tenths_ * 10);
            return;
        }

        // The ENCODER EVENT id is alphabetical (0-3 = A-D), unlike the PUSH-button key codes,
        // which run B, A, C, D. See docs/ENAGA_API.md -- confusing the two silently swaps A
        // and B, and nothing catches it.

        // A to D are the four macros in PERFORM mode, which is what they are for.
        if (mode_ == Mode::perform && encoder >= 0 && encoder < kMacros) {
            const int next = int(macro_position_[encoder]) + delta * 2;
            macro_position_[encoder] =
                uint8_t(next < 0 ? 0 : (next > 127 ? 127 : next));
            post(Cmd::set_macro, 0, 0, uint8_t(encoder), macro_position_[encoder]);
            return;
        }

        // MACHINE mode's one encoder: A walks the machines this build has. The change is posted
        // on every click rather than on an OK, because hearing it is the point -- a machine
        // select you have to confirm is a menu, and docs/SPEC.md's whole argument for this box is
        // that a change is one gesture.
        if (mode_ == Mode::machine && encoder == 0) {
            walk_machine(delta);
            return;
        }

        // KIT mode: A walks the slot cursor, as UP and DOWN do. B to D have nothing to set here,
        // and must not fall through to the sound parameters of a screen that is not showing.
        if (mode_ == Mode::kit && encoder >= 0 && encoder < 4) {
            if (encoder == 0) move_kit_cursor_to(kit_cursor_ + delta);
            return;
        }

        // RESAMPLE mode's one encoder: A walks the four lengths docs/SPEC.md allows.
        if (mode_ == Mode::resample && encoder == 0) {
            const int next = (take_bars_index_ + delta) % kTakeBarChoices;
            take_bars_index_ = next < 0 ? next + kTakeBarChoices : next;
            return;
        }

        // The SETUP page: A is the record quantize amount. The other three settings on it --
        // CLOCK SRC, CLOCK OUT / SYNC OUT and MIDI CH -- are the USER'S, set in the Enaga menu;
        // <enaga/clock.hpp> and <enaga/midi.hpp> expose no way to write them, so STRATA shows
        // them and says where they live, which is all docs/SPEC.md's "STRATA only exposes the
        // settings" can mean.
        // CHAIN: A is the entry's repeat count. PATTERN: A is LEN and B is SWG.
        if (mode_ == Mode::project && project_page_ == ProjectPage::chain) {
            if (encoder == 0) chain_delta(ChainEdit::repeats_delta, delta);
            return;
        }
        if (mode_ == Mode::project && project_page_ == ProjectPage::pattern) {
            if (encoder == 0 || encoder == 1) {
                pattern_field_ = encoder;
                pattern_field_delta(encoder, delta);
            }
            return;
        }
        if (mode_ == Mode::project && project_setup()) {
            if (encoder != 0) return;
            post_delta(Cmd::set_quantize, 0, 0, 0, delta * 5);
            notice("QUANTIZE");
            return;
        }

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
                const int root = (int(engine_.key_root()) + delta % 12 + 12) % 12;
                post(Cmd::set_root, 0, 0, uint8_t(root));
                notice(note_name(uint8_t(root)));
                return;
            }
            if (encoder == 2) {
                // The scale does NOT wrap, and it is clamped to what this build has. An id a
                // later build wrote survives a load (core/project.cpp leaves it alone), so the
                // user can be looking at a scale the encoder cannot reach -- walking from there
                // lands on the nearest one this build knows rather than jumping to chromatic.
                const int current = int(engine_.key_scale());
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
            if (mixer_bank_ < kMixerTrackPages) {
                // A-D are the levels of the four tracks in the current bank.
                const int track = mixer_bank_ * 4 + encoder;
                if (track >= kTracks) return;
                Command command;
                command.cmd = Cmd::set_track_level;
                command.track = uint8_t(track);
                command.c = kDeltaFlag;            // the engine adds it and clamps at 1.5
                command.f = float(delta) * 0.02f;
                post_command(command);
                return;
            }
            edit_mixer_field(mixer_bank_, encoder, delta);
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

        // 4b. F1 + A, B or C in GRID: the Euclidean generator's three numbers, applied live.
        //     docs/SPEC.md asks for "hits, steps, rotation" and these are them. Case 5 below
        //     never looked at F1, so this gesture was free.
        //
        //     Each click is one edit and they share a gesture, so core/undo coalesces a whole
        //     sweep into a single level -- the same arrangement the step-field encoders use, and
        //     the reason a knob turned forty times does not throw away the undo history.
        if (shift_ && mode_ == Mode::grid && encoder < 3) {
            if (encoder == 0) {
                euclid_hits_ = clamp_int(euclid_hits_ + delta, 0, euclid_steps_);
            } else if (encoder == 1) {
                euclid_steps_ = clamp_int(euclid_steps_ + delta, 1, kStepsPerTrack);
                if (euclid_hits_ > euclid_steps_) euclid_hits_ = euclid_steps_;
            } else {
                // Rotation wraps: a figure rotated by its whole cycle is the same figure, so
                // there is nothing to stop at.
                euclid_rotation_ = (euclid_rotation_ + delta) % euclid_steps_;
            }

            mark(GestureKind::length, selected_, 0);
            post(Cmd::euclid, uint8_t(selected_), 0, uint8_t(euclid_hits_),
                 uint8_t(euclid_steps_), uint8_t(int8_t(euclid_rotation_)));

            char *out = notice_buffer_;
            append(out, "EUC ");
            append_number(out, uint32_t(euclid_hits_));
            *out++ = '/';
            append_number(out, uint32_t(euclid_steps_));
            if (euclid_rotation_ != 0) {
                append(out, " R");
                append_number(out, uint32_t(euclid_rotation_ < 0 ? -euclid_rotation_
                                                                 : euclid_rotation_));
            }
            *out = 0;
            notice(notice_buffer_);
            return;
        }

        // 5. Otherwise A-D edit the four parameters of the current page on the selected track.
        const int index = page_first(Page(param_page_)) + encoder;
        post(Cmd::set_param, uint8_t(selected_), 0, uint8_t(index), clamp_delta(delta),
             kDeltaFlag);
    }

    void on_pad_press(int pad, int velocity) override {
        // The performance row first: it is the same seven pads in every mode, which is what
        // docs/SPEC.md means by a fixed row.
        if (is_perform_pad(pad)) {
            on_perform_pad(pad, true);
            return;
        }

        const int track = track_for_pad(pad);
        if (track < 0 || track >= kTracks) return;

        // F1 + pad mutes in any mode, and MUTE mode makes the lower pads mutes outright. With
        // F1 the mute waits for the bar: a held F1 is the performer asking for it to land in
        // time rather than under their finger.
        if (shift_ || mute_mode_) {
            post(Cmd::set_mute, uint8_t(track), 0, engine_.muted(track) ? 0 : 1,
                 shift_ ? 1 : 0);
            return;
        }

        if (track != selected_) end_gesture();         // a different track is a different edit
        selected_ = track;
        // The root in a playable octave: the pattern's root is a pitch class (core/pattern.hpp).
        const uint8_t note = uint8_t(kPadOctaveNote + engine_.key_root());
        post(Cmd::trigger_note, uint8_t(track), 0, note, uint8_t(velocity));
        record_note(track, note, uint8_t(velocity));
    }

    void on_pad_release(int pad) override {
        if (is_perform_pad(pad)) {
            on_perform_pad(pad, false);
            return;
        }

        // A pad that was bending macro 1 lets it go. The macro returns to where the ENCODER
        // left it, not to zero: the knob's position is the setting and the pressure is a bend on
        // top of it, so letting go must not throw the setting away. Without this, pressing a pad
        // once would leave macro 1 wherever the finger happened to lift.
        if (pressure_pad_ == pad) {
            pressure_pad_ = -1;
            post(Cmd::set_macro, 0, 0, 0, macro_position_[0]);
        }
    }

    // docs/SPEC.md: "Pad pressure modulates the selected track's macro 1 while a pad is held."
    //
    // Enaga's event is already 0-127 through its own cube curve, and the spec's `[rev]` note is
    // explicit that the app-side smoothing and change-filtering this document originally planned
    // over raw readings is no longer needed -- so this does no filtering, and adding any would be
    // undoing Enaga's shaping with a second guess at it.
    //
    // The pressure takes the macro only UPWARD from where the encoder left it. A pad that bent
    // the macro DOWN would mean resting a finger quietly on a pad changed the sound away from
    // what the knob said, and a performer leaning on a pad expects more of something rather than
    // less. So the macro goes to the larger of the knob's position and the pressure, and
    // `on_pad_release` puts the knob's position back.
    //
    // Only the pad the press came from, and only one at a time: with two pads held, the first
    // owns the macro until it is released. Pressure events arrive about 262 times a second per
    // pad (docs/SPEC.md), which would otherwise be two streams fighting over one value.
    void on_pad_pressure(int pad, int pressure) override {
        // `track_for_pad` is `pad % 2 == 0 ? pad / 2 : -1`, so this one test does three jobs:
        // it rejects the performance row (the odd pads, which are tools and not keys -- leaning
        // on FILL must not change the sound), it rejects anything out of range, and because each
        // track has exactly ONE pad there can never be a second pad competing for this macro.
        //
        // Guards for the second and third of those were written here and then removed: both
        // survived mutation testing, because this line already covers them. A guard that cannot
        // fire is a claim about a situation that cannot arise.
        const int track = track_for_pad(pad);
        if (track < 0 || track >= kTracks) return;
        if (track != selected_) return;                   // "the SELECTED track's macro 1"

        const uint8_t clamped = uint8_t(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
        const uint8_t floor = macro_position_[0];
        const uint8_t value = clamped > floor ? clamped : floor;

        // One command per CHANGE, not per event. 262 events a second per pad against a ring the
        // audio side drains at a bounded rate per block is a waste of both; a pad held steady
        // produces the same number repeatedly and the first of them is enough.
        if (pressure_pad_ == pad && value == pressure_sent_) return;
        pressure_pad_ = pad;
        pressure_sent_ = value;
        post(Cmd::set_macro, 0, 0, 0, value);
    }

    // The seven upper pads. Four of them are HELD rather than toggled -- fill, repeat, throw and
    // the two scene selectors -- so each one's release matters as much as its press.
    void on_perform_pad(int pad, bool down) {
        switch (PerformPad(pad)) {
            case PerformPad::fill:
                post(Cmd::set_fill, 0, 0, down ? 1 : 0);
                break;

            case PerformPad::repeat:
                // VALUE picks the length while it is held (docs/SPEC.md), which on_encoder
                // handles; F1 is the pitch-down.
                repeat_held_ = down;
                post(Cmd::set_repeat, 0, 0, uint8_t(repeat_length_), down ? 1 : 0,
                     shift_ ? 1 : 0);
                break;

            case PerformPad::mute:
                if (down) {
                    mute_mode_ = !mute_mode_;
                    notice(mute_mode_ ? "MUTE MODE" : "PADS PLAY");
                    // MUTE mode takes the lower STEP keys, and its branch in `on_key` returns on
                    // the release as well as the press -- so a STEP key held when this pad is
                    // tapped never delivers its release, and the hold would outlive its key.
                    // The next encoder turn would then write a parameter lock onto a step nobody
                    // was touching. Same reasoning as `leave_mode`, and the third of three ways
                    // this could happen: the other two were a page change and a mode change.
                    drop_hold();
                }
                break;

            case PerformPad::scene_a:
            case PerformPad::scene_b:
                // Held + STEP picks the scene; on its own it just shows which is assigned.
                // Deliberately does NOT change the mode. The performance row works in every
                // mode, and a hold that switched the screen and did not switch it back would be
                // a surprise in the middle of a take.
                scene_holding_ = down ? (PerformPad(pad) == PerformPad::scene_a ? 0 : 1) : -1;
                // And a scene hold takes all sixteen STEP keys, with the same swallowed release.
                if (down) drop_hold();
                break;

            case PerformPad::throw_fx:
                // Delay, or reverb with F1. Released, it goes back to the stored send by itself
                // -- the engine never wrote over `params_`.
                post(Cmd::set_throw, uint8_t(selected_), 0,
                     down ? uint8_t(shift_ ? 2 : 1) : uint8_t(0));
                break;

            case PerformPad::mutate:
                if (!down) break;
                // One undo level, then the mutation. CLR reverts it because CLR is the undo that
                // already exists -- core/mutate.hpp's whole argument for having no commit step.
                mark(GestureKind::paste, selected_, 0);
                post_mutate();
                end_gesture();
                // docs/SPEC.md: "OK keeps, CLR reverts". The mutation is already one undo level,
                // so keeping is just forgetting it is pending and reverting is that undo.
                mutation_pending_ = true;
                break;

            default:
                break;
        }
    }

    void post_mutate() {
        Command command;
        command.cmd = Cmd::mutate;
        command.track = uint8_t(selected_);
        command.step = 0;
        command.a = uint8_t(mutation_);
        command.b = uint8_t(mutate_amount_);
        command.c = 0;                       // the whole track
        command.u = mutate_variant_++;       // a new result each press
        post_command(command);
        char *out = notice_buffer_;
        append(out, mutation_name(mutation_));
        *out = 0;
        notice(notice_buffer_);
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

        const Landing landing = quantize_hit(hit, timing, quantize());

        mark(GestureKind::record, track, 0);     // step 0: one undo level for the whole pass
        Command command;
        command.cmd = Cmd::record_hit;
        command.track = uint8_t(track);
        command.step = landing.step;
        command.a = note;
        command.b = velocity;
        command.c = uint8_t(landing.micro);
        post_command(command);

        if (landing.clamped) notice("OFF GRID");   // it could not be stored where it was played
    }

    void on_transport(int what, uint32_t position) override {
        // Under an external clock STRATA cannot start or stop anything; it follows. Enaga sends
        // this for every transport change whoever made it (docs/ENAGA_API.md), so a STOP from
        // MIDI or SYNC is followed exactly as F6 is: voices released and the sequencer reset,
        // which is what keeps the conditions and the playheads in step with the outside
        // transport. A STOP that F6 made arrives here too; releasing twice is harmless. The next
        // F6 press is then the one that cuts the tails.
        (void)position;
        if (what == ENAGA_TRANSPORT_START || what == ENAGA_TRANSPORT_LOCATE) stop_armed_ = false;
        if (what == ENAGA_TRANSPORT_STOP) {
            post(Cmd::stop, 0, 0, 0);
            stop_armed_ = true;
        }

        // docs/SPEC.md: "Autosave to AUTOSAVE/ whenever the transport stops." It follows the
        // transport rather than the key, so a stop arriving from MIDI or SYNC autosaves too --
        // which is the case where the user never pressed anything and would least expect to
        // lose work.
        if (what == ENAGA_TRANSPORT_STOP) storage_.request_autosave();
    }

    // --- LEDs -------------------------------------------------------------

    void update(uint32_t ms) override {
        last_ms_ = ms;

        // The mutes, read back from the engine. The UI keeps a shadow so a press shows at once,
        // but a recall or a mute that landed on the bar happened on the audio side, and without
        // this the shadow would drift from what is actually muted.
        for (int t = 0; t < kTracks; t++) muted_[t] = engine_.muted(t);

        // One bounded slice of whatever the card side has in flight. It never blocks and never
        // waits on the audio side: a command ring that has not drained simply means "not this
        // pass" (app/storage.hpp).
        storage_.service();

        // The tempo a LOADED project brought with it. PROJECT.BIN has always had the field and
        // nothing read it, so every project opened at whatever the clock happened to be set to
        // -- a project saved at 142 came back at 130. Watched here rather than hooked to the end
        // of a load because a load finishes on the card side, and this is one comparison on a
        // pass that already does several.
        //
        // `adopted_tempo_centi_` is what stops this fighting the knob: a click posts the new
        // tempo AND records it here, so the engine's copy coming back is not read as a change.
        {
            const uint16_t project_centi = engine_.project().settings.tempo_centi_bpm;
            if (project_centi != adopted_tempo_centi_) {
                adopted_tempo_centi_ = project_centi;
                tempo_tenths_ = uint32_t(project_centi) / 10;
                // Refused under an outside clock, which is not an error: the project's tempo is
                // simply not what is playing, and the screen already says EXT.
                clock_refused_ = !enaga::clock::set_tempo(float(project_centi) * 0.01f);
            }
        }

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

        // Which SCREEN is up, logged on a change for the same reason as the two above: in an
        // emulator run or a unit log it is the difference between "the app is wedged" and "the
        // app is on a page I did not expect", and it is what lets a scenario reach a screen
        // that the emulator cannot otherwise observe -- it exposes key input and no encoders,
        // so a page walked with the selection keys is all it can see of MIXER and SETUP.
        {
            const int screen = int(mode_) * 16 + (mode_ == Mode::mixer ? mixer_bank_
                                                                       : (mode_ == Mode::project
                                                                              ? int(project_page_)
                                                                              : 0));
            if (screen != logged_screen_) {
                logged_screen_ = screen;
                char line[24];
                char *out = line;
                append(out, "scr ");
                append(out, mode_name(mode_));
                if (mode_ == Mode::mixer) {
                    *out++ = ' ';
                    append(out, kMixerPages[mixer_bank_].name);
                } else if (mode_ == Mode::project && project_page_ != ProjectPage::slots) {
                    *out++ = ' ';
                    append(out, project_page_name(project_page_));
                }
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

        if ((step_page_ || mode_ != Mode::grid) && notice_showing()) {
            // What just happened, on the second line. GRID shows it in its edit-state line; every
            // other screen had nowhere for it, so a gesture there (F1 + F3's "again to save", an
            // octave, a scene captured) answered with nothing at all.
            out = line;
            append_shown(out, notice_, kLineChars);
            *out = 0;
            canvas.text(0, 9, line, 15);
        } else if (int(clock.source) != ENAGA_CLOCK_INTERNAL) {
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
                case Mode::perform: draw_perform(canvas); break;
                case Mode::keys:    draw_keys(canvas); break;
                case Mode::resample: draw_resample(canvas); break;
                case Mode::machine: draw_machine(canvas); break;
                case Mode::kit:     draw_kit(canvas); break;
            }
        }

        // The bottom line on every mode: tracks sounding, the device, and any dropped edits. A
        // dropped command should be visible rather than mysterious (app/commands.hpp). It said
        // "V" for voices, but the count is of tracks (Status::voices), so a chord read as one.
        out = line;
        append(out, "TRK");
        append_number(out, status.voices.load(std::memory_order_relaxed));
        append(out, " ");
        // A dropped edit takes the device's place: "V12 LOFI-12 XT DROP 99" is 22 characters,
        // one past the edge, and the drop count is the part that matters.
        const uint32_t dropped = engine_.commands().dropped();
        if (dropped) { append(out, "DROP "); append_number(out, dropped); }
        else append(out, device_name(enaga::system::device()));
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
        // "T1", not "TRK 1": with a three-character speed and a two-digit length the long form
        // was 22 characters, and the speed ran off the right edge.
        append(out, "T");
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
        draw_param_slots(canvas, 101);
    }

    // The line that says what editing mode you are in: recording, the quantize amount, the
    // selection, and how much undo there is. Without it, REC and a held selection are invisible
    // states that change what every other key does.
    void draw_edit_state(enaga::Canvas &canvas, int y) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        if (notice_showing()) {
            // Cut to the line: a notice is built in places all over this file, and one that ran
            // off the edge would lose its end, which is usually the part that says what to do.
            out = line;
            append_shown(out, notice_, kLineChars);
            *out = 0;
            canvas.text(0, y, line, 15);
            return;
        }

        // Built from pieces in order of importance, each left out whole if it would not fit:
        // all of them at once ("REC Q100 SEL 1-64 U16/R16 FULL 255") is 35 characters. Undo and
        // redo are also on the CLR and OK LEDs, so they are the first to go.
        char part[24];
        char *p = part;
        out = line;
        if (rec_) {
            p = part;
            append(p, "REC Q");
            append_number(p, uint32_t(quantize()));
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        if (range_first_ >= 0) {
            p = part;
            append(p, "SEL ");
            append_number(p, uint32_t(range_first_ + 1));
            *p++ = '-';
            append_number(p, uint32_t(range_last_ + 1));
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        const uint32_t refused = status.refused.load(std::memory_order_relaxed);
        if (refused) {
            // A refused edit is almost always a full lock pool. Saying nothing would leave
            // someone repeating an edit that cannot work.
            p = part;
            append(p, "FULL ");
            append_number(p, refused);
            append(p, " ");
            *p = 0;
            append_if_fits(out, line, part);
        }
        p = part;
        append(p, "U");
        append_number(p, status.undo_depth.load(std::memory_order_relaxed));
        const uint8_t redo = status.redo_depth.load(std::memory_order_relaxed);
        if (redo) {
            append(p, "/R");
            append_number(p, redo);
        }
        *p = 0;
        append_if_fits(out, line, part);
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
        append(out, "EDIT T");
        append_number(out, uint32_t(selected_ + 1));
        append(out, " ");
        append(out, machine_name(engine_.machine_id(selected_)));
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

        // The four parameters in large type, one row per encoder: its letter, the name, the
        // value and a bar. The letter is on the row because the encoder-slot strip GRID has at
        // the bottom would only repeat these four, and with names and values side by side it
        // did not fit four to a line (7 characters is 41 pixels, in a 32-pixel slot).
        const int base = page_first(Page(param_page_));
        for (int slot = 0; slot < 4; slot++) {
            const int index = base + slot;
            const int y = 42 + slot * 12;
            const uint8_t value = engine_.params(selected_).v[index];

            out = line;
            *out++ = char('A' + slot);
            *out = 0;
            canvas.text(0, y, line, 7);

            out = line;
            append(out, param_name_for(index));
            *out = 0;
            canvas.text(10, y, line, 13);

            out = line;
            append_padded(out, value, 3);
            *out = 0;
            canvas.text(34, y, line, 15);

            const int bar = int(value) * 70 / 127;
            canvas.rect(56, y + 2, bar < 1 ? 1 : bar, 4, 9);
        }

        // The machine is changed somewhere else, and nothing said where.
        canvas.text(0, 96, "FUNC+EDIT: MACHINE", 7);
    }

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
        append_number(out, byte);
    }

    void draw_mixer_fields(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;

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

        // What the delay is actually doing, in milliseconds, which is the one thing a division
        // does not tell you -- and the number that says whether the tempo-sync is working.
        if (mixer_bank_ == 2) {
            out = line;
            append(out, "TIME ");
            append_number(out, uint32_t(engine_.sends().delay_seconds * 1000.0f + 0.5f));
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
        if (mixer_bank_ >= kMixerTrackPages) {
            draw_mixer_fields(canvas);
            return;
        }
        char line[40];
        char *out = line;

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

    // The machine names SRC and AMP; the insert chain names FLTR and FX.
    const char *param_name_for(int index) const {
        if (param_page_ == int(Page::fltr) || param_page_ == int(Page::fx)) {
            return InsertChain::param_name(index);
        }
        const Machine *machine = engine_.machine(selected_);
        return machine ? machine->param_name(index) : "---";
    }

    // The four encoder slots along the bottom: a three-letter name, the value under it and a
    // small bar, 18 pixels tall from `y`.
    void draw_param_slots(enaga::Canvas &canvas, int y) {
        char line[40];
        const int base = page_first(Page(param_page_));
        for (int slot = 0; slot < 4; slot++) {
            const int index = base + slot;
            // Name over value: "CUT 064" on one line is 41 pixels and a slot is 32, so each
            // slot ran into the next one (and the fourth off the screen).
            char *out = line;
            append(out, param_name_for(index));
            *out = 0;
            canvas.text(slot * 32, y, line, 12);

            out = line;
            append_padded(out, engine_.params(selected_).v[index], 3);
            *out = 0;
            canvas.text(slot * 32, y + 8, line, 15);

            const int bar = int(engine_.params(selected_).v[index]) * 28 / 127;
            canvas.rect(slot * 32, y + 16, bar < 1 ? 1 : bar, 2, 8);
        }
    }

    // --- KEYS: the STEP keys as an instrument ----------------------------

    // The note STEP `step` plays: `step` degrees of the project's scale above the root, in the
    // current octave. By DEGREES rather than semitones, which is the whole reason core/scale
    // exists -- sixteen keys then span two octaves of a seven-note scale and stay in the key.
    uint8_t keys_note(int step) const {
        const uint8_t root = engine_.key_root();
        const int scale = int(engine_.key_scale());
        const int base = clamp_int(kPadOctaveNote + keys_octave_ * 12 + int(root), 0, 127);
        return scale_step(scale_fold(uint8_t(base), root, scale), root, scale, step);
    }

    // A STEP key played, and its release. The note is held while the key is down: the press
    // triggers with no gate and the release lets that note go (dsp/track.hpp, `release`), so a
    // pad or a chord on VA, VA POLY or SAMPLE sustains for as long as it is held. The note is
    // remembered per key, so an octave change while a key is down still releases what it
    // started rather than a note it never played.
    void release_key(int step) {
        if (step < 0 || step >= 16 || keys_held_note_[step] < 0) return;
        post(Cmd::release_note, uint8_t(keys_held_track_[step]), 0,
             uint8_t(keys_held_note_[step]));
        keys_held_note_[step] = -1;
    }

    void play_key(int step) {
        const uint8_t note = keys_note(step);
        release_key(step);                       // a press without its release: let that go first
        keys_held_note_[step] = int16_t(note);
        keys_held_track_[step] = uint8_t(selected_);
        // A key has no velocity. 100 is what the default kit's own trigs use, so a part played
        // in and a part programmed in sound the same.
        post(Cmd::trigger_note, uint8_t(selected_), 0, note, kKeyVelocity);
        record_note(selected_, note, kKeyVelocity);
    }

    void draw_machine(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        append(out, "MACHINE T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, "  ");
        append(out, machine_name(engine_.machine_id(selected_)));
        *out = 0;
        canvas.text(0, 18, line, 15);

        // All eight tracks at once, because the question anyone in this mode is asking is "what
        // is this kit", not "what is this track". Four to a line: eight names are 31 characters.
        for (int half = 0; half < 2; half++) {
            out = line;
            append_number(out, uint32_t(half * 4 + 1));
            *out++ = '-';
            append_number(out, uint32_t(half * 4 + 4));
            for (int t = half * 4; t < half * 4 + 4; t++) {
                *out++ = ' ';
                append(out, machine_name(engine_.machine_id(t)));
            }
            *out = 0;
            canvas.text(0, 30 + half * 9, line, 10);
        }

        // The machines as a list, the selected track's lit: UP and DOWN (or A) walk it, which is
        // the cursor the arrows move everywhere else. Two columns of four, read down.
        const int current = int(engine_.machine_id(selected_));
        for (int m = 0; m < int(MachineId::count); m++) {
            const int x = (m / 4) * 64;
            const int y = 52 + (m % 4) * 12;
            const bool on = m == current;
            canvas.rect(x + 1, y, 60, 11, on ? 10 : 2);
            canvas.text(x + 4, y + 2, machine_name(uint8_t(m)), on ? 0 : 11);
        }

        canvas.text(0, 103, "ARROWS: MACHINE TRACK", 7);
        canvas.text(0, 112, "F1+EDIT AGAIN: KITS", 7);
    }

    // --- KIT: the kit library (FUNC+EDIT twice) ---------------------------
    //
    // Sixteen kits on the card, shared by every project (app/storage.hpp, "the kit library").
    // The list is the arrows' rows, as every Enaga list is: UP and DOWN move the cursor, LEFT and
    // RIGHT flip the two pages of eight, OK picks -- twice, because a recall replaces the sound
    // and is not undoable -- and CLR goes back to MACHINE. F1 + DOWN puts the kit down into the
    // slot, the same direction it pastes in every editing mode; over a kit already there it
    // takes a second press. F1 + UP reads the card again.

    void draw_kit(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const int first = (kit_cursor_ / 8) * 8;
        append(out, "KITS ");
        append_padded(out, uint32_t(first + 1), 2);
        *out++ = '-';
        append_padded(out, uint32_t(first + 8), 2);
        if (!storage_.card_usable()) append(out, "  NO CARD");
        *out = 0;
        canvas.text(0, 18, line, 15);

        // What the card is doing, what went wrong, or what the keys do.
        out = line;
        const char *busy = storage_.busy_text();
        if (busy != nullptr) {
            append(out, busy);
        } else if (storage_.last_error() != nullptr) {
            append_shown(out, storage_.last_error(), kLineChars);
        } else {
            append(out, "OK LOAD  F1+DOWN SAVE");
        }
        *out = 0;
        canvas.text(0, 27, line, busy != nullptr ? 12 : 9);

        for (int i = 0; i < 8; i++) {
            const int slot = first + i;
            const Storage::KitSlot &info = storage_.kit_slot(slot);
            out = line;
            *out++ = slot == kit_cursor_ ? '>' : ' ';
            append_padded(out, uint32_t(slot + 1), 2);
            *out++ = ' ';
            append(out, kit_state_name(info.state));
            *out = 0;
            canvas.text(0, 37 + i * 8, line, slot == kit_cursor_ ? 15 : 9);
        }

        // The slot under the cursor, as MACHINE draws the kit in play: two lines of four.
        const Storage::KitSlot &info = storage_.kit_slot(kit_cursor_);
        if (info.state == Storage::SlotState::used) {
            for (int half = 0; half < 2; half++) {
                out = line;
                append_number(out, uint32_t(half * 4 + 1));
                *out++ = '-';
                append_number(out, uint32_t(half * 4 + 4));
                for (int t = half * 4; t < half * 4 + 4; t++) {
                    *out++ = ' ';
                    append(out, machine_name(info.machines[t]));
                }
                *out = 0;
                canvas.text(0, 103 + half * 9, line, 11);
            }
        }
    }

    static const char *kit_state_name(Storage::SlotState state) {
        switch (state) {
            case Storage::SlotState::empty: return "EMPTY";
            case Storage::SlotState::used:  return "KIT";
            case Storage::SlotState::bad:   return "UNREADABLE";
            default:                        return "...";
        }
    }

    void open_kits() {
        mode_ = Mode::kit;
        leave_mode();
        kit_armed_ = KitAction::none;
        storage_.request_kit_scan();
        notice(storage_.card_usable() ? "KITS" : "NO CARD");
    }

    void move_kit_cursor_to(int slot) {
        const int next = slot < 0 ? 0 : (slot >= Storage::kKitSlots ? Storage::kKitSlots - 1 : slot);
        if (next != kit_cursor_) kit_armed_ = KitAction::none;   // an arm is for one slot
        kit_cursor_ = next;
    }

    enum class KitAction : uint8_t { none, load, save };

    // A second press of the same key on the same slot within kSaveArmMs, as a project save,
    // reload and NEW take. True when this press is that second one.
    bool kit_second_press(KitAction action) {
        if (kit_armed_ == action && uint32_t(last_ms_ - kit_armed_at_ms_) < kSaveArmMs) {
            kit_armed_ = KitAction::none;
            return true;
        }
        kit_armed_ = action;
        kit_armed_at_ms_ = last_ms_;
        return false;
    }

    bool on_kit_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;               // F1 + OK is redo, as everywhere
                const Storage::SlotState state = storage_.kit_slot(kit_cursor_).state;
                if (state == Storage::SlotState::empty) {
                    kit_armed_ = KitAction::none;
                    notice("EMPTY SLOT");
                } else if (!storage_.card_usable()) {
                    notice("NO CARD");
                } else if (kit_second_press(KitAction::load)) {
                    storage_.request_kit_load(kit_cursor_);
                    notice("LOADING KIT");
                } else {
                    notice("OK AGAIN TO LOAD");
                }
                return true;
            }

            case ENAGA_KEY_CLR:
                if (shift_) return false;               // F1 + CLR is undo
                mode_ = Mode::machine;                  // back, where the browser came from
                leave_mode();
                notice("MACHINE");
                return true;

            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (!shift_) {
                    move_kit_cursor_to(kit_cursor_ + (key == ENAGA_KEY_UP ? -1 : +1));
                } else if (key == ENAGA_KEY_UP) {
                    storage_.request_kit_scan();
                    notice(storage_.card_usable() ? "READING KITS" : "NO CARD");
                } else {
                    // Straight into an empty slot; over a kit, or a slot not yet read, only on
                    // the second press -- the kit already there is not undoable either.
                    const bool empty =
                        storage_.kit_slot(kit_cursor_).state == Storage::SlotState::empty;
                    if (!storage_.card_usable()) {
                        notice("NO CARD");
                    } else if (empty || kit_second_press(KitAction::save)) {
                        kit_armed_ = KitAction::none;
                        storage_.request_kit_save(kit_cursor_);
                        notice("SAVING KIT");
                    } else {
                        notice("AGAIN TO OVERWRITE");
                    }
                }
                return true;

            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                // The page of eight; F1 + LEFT / RIGHT are free here, and kept free.
                if (!shift_) move_kit_cursor_to(kit_cursor_ + (key == ENAGA_KEY_LEFT ? -8 : +8));
                return true;

            default:
                return false;
        }
    }

    void draw_resample(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const uint8_t state = engine_.status().resample_state.load(std::memory_order_relaxed);

        append(out, "RESAMPLE ");
        append_number(out, uint32_t(kTakeBars[take_bars_index_]));
        append(out, state == 1u ? " BAR  WAIT" : " BAR");
        *out = 0;
        canvas.text(0, 18, line, 15);

        out = line;
        append(out, "TO ");
        if (resample_track_ < 0) {
            append(out, "FILE");
        } else {
            append(out, "TRK ");
            append_number(out, uint32_t(resample_track_) + 1);
        }
        *out = 0;
        canvas.text(0, 28, line, 10);

        // What the take is doing, from the two words the audio side publishes. A bar at 130 BPM
        // is 88,615 frames, so a percentage is the only figure that fits and the only one anyone
        // reads while recording.
        out = line;
        const uint32_t frames = engine_.status().resample_frames.load(std::memory_order_relaxed);
        switch (state) {
            case 1u: append(out, "ARMED"); break;
            case 2u: {
                append(out, "REC ");
                const uint32_t bar_frames =
                    uint32_t(float(kTakeBars[take_bars_index_]) * float(kTicksPerBar) * 230.77f);
                const uint32_t percent = bar_frames == 0u ? 0u : frames * 100u / bar_frames;
                append_number(out, percent > 100u ? 100u : percent);
                *out++ = '%';
                break;
            }
            case 3u: append(out, "WRITING"); break;
            default: append(out, "OK ARMS  CLR CANCELS"); break;
        }
        *out = 0;
        canvas.text(0, 40, line, state == 2u ? 15 : 10);

        if (engine_.status().resample_clamped.load(std::memory_order_relaxed) != 0u) {
            canvas.text(0, 50, "SHORTENED TO 30S", 10);
        } else if (storage_.last_take_name()[0] != 0) {
            out = line;
            append(out, "LAST ");
            append_shown(out, storage_.last_take_name(), kLineChars - 5);
            *out = 0;
            canvas.text(0, 50, line, 10);
        }

        canvas.text(0, 103, "UP DOWN OR A: BARS", 7);
        canvas.text(0, 112, "LEFT RIGHT: WHERE TO", 7);
    }

    void draw_keys(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const uint8_t root = engine_.key_root();
        const int scale = int(engine_.key_scale());

        out = line;
        append(out, "KEYS T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");                    // one space: "KEYS T1 C# MIXOLYDIAN" is exactly 21
        append(out, note_name(root));
        *out++ = ' ';
        append(out, scale_name(scale));
        *out = 0;
        canvas.text(0, 18, line, 15);

        out = line;
        append(out, "OCT ");
        append_number(out, uint32_t(keys_octave_ + 3));
        append(out, "   ");
        append_number(out, uint32_t(scale_size(scale)));
        append(out, " PER OCTAVE");
        *out = 0;
        canvas.text(0, 27, line, 11);

        // What each of the sixteen keys will play. The point of the screen in this mode: a
        // performer needs to see the notes under their fingers, not work them out.
        for (int i = 0; i < 16; i++) {
            const int x = (i % 4) * 32;
            const int y = (i / 4) * 16 + 44;
            const uint8_t note = keys_note(i);
            out = line;
            append(out, note_name(note));
            append_number(out, uint32_t(note_octave(note) < 0 ? 0 : note_octave(note)));
            *out = 0;
            // The root of each octave brighter, so the shape of the scale is readable.
            const bool at_root = (note % 12) == root;
            canvas.rect(x + 1, y, 29, 13, at_root ? 6 : 3);
            canvas.text(x + 3, y + 3, line, at_root ? 15 : 11);
        }
    }

    // --- PERFORM: scenes, the morph, macros and mutation -----------------

    // True when the key belonged to this mode. OK captures a scene, F1 + OK erases one, UP and
    // DOWN walk which mutation MUTATE will do, CLR clears the selected macro.
    bool on_perform_key(int key) {
        switch (key) {
            case ENAGA_KEY_OK: {
                // F1 + OK is redo here as everywhere (docs/SPEC.md), so it falls through to
                // the shared handler.
                if (shift_) return false;
                // Into whichever slot is held, or slot A's scene. The selected track only when a
                // scene pad is held: capturing one track is what makes "a scene stores parameter
                // values for any tracks" usable.
                const int slot = scene_holding_ >= 0 ? scene_holding_ : 0;
                const int scene = engine_.scene_slot(slot);
                post(Cmd::scene_capture, uint8_t(selected_), 0, uint8_t(scene),
                     scene_holding_ >= 0 ? 1 : 0);
                notice(scene_holding_ >= 0 ? "TRACK CAPTURED" : "SCENE CAPTURED");
                return true;
            }

            case ENAGA_KEY_CLR:
                // F1 + CLR is undo, which is how a MUTATE is taken back without leaving PERFORM.
                if (shift_) return false;
                // A scene pad held + CLR erases that slot's scene: a scene you cannot empty is
                // one you can only overwrite. (It was F1 + OK, which the spec gives to redo.)
                if (scene_holding_ >= 0) {
                    post(Cmd::scene_erase, 0, 0, uint8_t(engine_.scene_slot(scene_holding_)));
                    notice("SCENE ERASED");
                    return true;
                }
                post(Cmd::macro_clear, 0, 0, uint8_t(macro_selected_));
                notice("MACRO CLEARED");
                return true;

            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN: {
                const int delta = key == ENAGA_KEY_UP ? -1 : 1;
                if (shift_) {
                    // The auto-fill period: off, 4, 8 or 16 bars. It wraps, since "off" is one
                    // of the four rather than an end stop.
                    const int next = (int(auto_fill_) + delta + int(AutoFill::count)) %
                                     int(AutoFill::count);
                    auto_fill_ = AutoFill(next);
                    post(Cmd::set_auto_fill, 0, 0, uint8_t(auto_fill_));
                    char *out = notice_buffer_;
                    append(out, "AUTOFILL ");
                    append(out, auto_fill_name(auto_fill_));
                    *out = 0;
                    notice(notice_buffer_);
                    return true;
                }
                // Which mutation MUTATE performs. Walked here rather than on an encoder because
                // all four encoders are macros in this mode.
                const int next = int(mutation_) + delta;
                mutation_ = Mutation(next < 0 ? int(Mutation::count) - 1
                                              : (next >= int(Mutation::count) ? 0 : next));
                notice(mutation_name(mutation_));
                return true;
            }

            case ENAGA_KEY_OCTAVE_LEFT:
            case ENAGA_KEY_OCTAVE_RIGHT:
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT: {
                // Which macro CLR clears and F1 + a parameter assigns to. The arrows too: the
                // macros are the row of four across the screen.
                const int delta = key == ENAGA_KEY_OCTAVE_LEFT || key == ENAGA_KEY_LEFT ? -1 : 1;
                macro_selected_ = (macro_selected_ + delta + kMacros) % kMacros;
                return true;
            }

            default:
                return false;
        }
    }

    // Assign one parameter of the selected track to the selected macro.
    //
    // The range runs from where the parameter is NOW to whichever end is further away, so the
    // knob always has somewhere to go: a cutoff already wide open gets a macro that closes it
    // rather than one that does nothing. That is a guess at intent, and the point of guessing is
    // that a macro assigned in one press is usable in the next -- the ranges are stored, so a
    // richer editor can refine them later without changing the format.
    void assign_macro(int param) {
        if (param < 0 || param >= kParams) return;
        const uint8_t now = engine_.performed(selected_, param);
        const uint8_t far = now > 63 ? uint8_t(0) : uint8_t(127);
        Command command;
        command.cmd = Cmd::macro_assign;
        command.track = uint8_t(selected_);
        command.a = uint8_t(macro_selected_);
        command.b = uint8_t(param);
        command.c = now;
        command.d = far;
        post_command(command);

        char *out = notice_buffer_;
        *out++ = char('A' + macro_selected_);
        append(out, " T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");
        append(out, engine_.machine(selected_) != nullptr
                        ? engine_.machine(selected_)->param_name(param)
                        : "---");
        *out = 0;
        notice(notice_buffer_);
    }

    void draw_perform(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;

        // The two scenes and where the morph sits between them. The bar is the one thing in this
        // mode that has to be readable at a glance from across a room.
        // "A16 128P": the scene and how many parameters it holds. Short, because both have to fit
        // on one line at any count -- "B16  12 PRM" at x = 72 ran three pixels off the screen.
        for (int side = 0; side < 2; side++) {
            out = line;
            *out++ = side == 0 ? 'A' : 'B';
            append_number(out, uint32_t(engine_.scene_slot(side)) + 1);
            *out++ = ' ';
            append_number(out, uint32_t(scene_count(
                                   engine_.scenes().scenes[engine_.scene_slot(side)])));
            *out++ = 'P';
            *out = 0;
            canvas.text(side == 0 ? 0 : 72, 18, line, scene_holding_ == side ? 15 : 11);
        }

        const int filled = int(uint32_t(engine_.morph()) * 124u / kMorphScale);
        canvas.rect(2, 28, 124, 5, 3);
        canvas.rect(2, 28, filled < 1 ? 1 : filled, 5, 12);

        // The four macros, with how many parameters each drives. An empty macro says so rather
        // than looking like one set to zero.
        for (int m = 0; m < kMacros; m++) {
            const int x = m * 32;
            // The selected macro is marked with '>' before its letter. The font has no '*', so
            // "A*" drew as "A?" on the unit.
            out = line;
            if (m == macro_selected_) *out++ = '>';
            *out++ = char('A' + m);
            *out = 0;
            canvas.text(x + 2, 40, line, m == macro_selected_ ? 15 : 10);

            const int targets = macro_count(engine_.macros().macros[m]);
            out = line;
            if (targets == 0) {
                append(out, "---");
            } else {
                append_number(out, uint32_t(targets));
                append(out, " PRM");
            }
            *out = 0;
            canvas.text(x + 2, 49, line, targets == 0 ? 6 : 11);

            // 18 pixels tall, ending above the hints. At 40 the bars ran down through both
            // hint lines.
            const int height = int(engine_.macro_position(m)) * kMacroBar / 127;
            canvas.rect(x + 2, 58, 12, kMacroBar, 2);
            canvas.rect(x + 2, 58 + (kMacroBar - height), 12, height < 1 ? 1 : height,
                        targets == 0 ? 5 : 12);
        }

        // The gesture that is not otherwise discoverable.
        canvas.text(0, 78, "STEP ASSIGNS TO MACRO", 7);
        canvas.text(0, 87, "FUNC+UP AUTOFILL", 7);

        // What MUTATE will do, and to which track.
        out = line;
        append(out, "MUT ");
        append(out, mutation_name(mutation_));
        append(out, " T");
        append_number(out, uint32_t(selected_) + 1);
        append(out, " ");
        append_number(out, uint32_t(mutate_amount_));
        *out = 0;
        canvas.text(0, 96, line, 11);

        // The live performance state: fill, the repeat and its length, a throw, the bar.
        out = line;
        if (engine_.fill()) append(out, "FILL ");
        if (engine_.auto_fill() != AutoFill::off) {
            append(out, "AF");
            append(out, auto_fill_name(engine_.auto_fill()));
            append(out, " ");
        }
        if (engine_.repeat().active()) {
            append(out, repeat_length_name(repeat_length_));
            append(out, engine_.repeat().pitch_down() ? "- " : " ");
        }
        const int throwing = engine_.throwing();
        if (throwing == 1) append(out, "DLY");
        if (throwing == 2) append(out, "REV");
        *out = 0;
        canvas.text(0, 104, line, 9);

        // The bar on a line of its own: with everything above on, "FILL AF16 1/16T- DLY BAR
        // 1000" is 29 characters.
        out = line;
        append(out, "BAR ");
        append_number(out, engine_.bar() % 1000u + 1u);
        *out = 0;
        canvas.text(0, 112, line, 9);
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
        if (project_page_ == ProjectPage::chain && on_chain_key(key)) return true;
        if (project_page_ == ProjectPage::pattern && on_pattern_page_key(key)) return true;
        switch (key) {
            case ENAGA_KEY_OK:
                // F1 + OK is redo here as everywhere (docs/SPEC.md); the shared handler has it.
                if (shift_) return false;
                if (project_page_ == ProjectPage::slots) {
                    // OK picks the slot under the cursor, as OK picks in every Enaga list.
                    // Saving is F1 + F3 twice (docs/SPEC.md's key table).
                    queue_shown_pattern(slot_cursor_);
                }
                return true;

            case ENAGA_KEY_CLR:
                // F1 + CLR is undo, likewise.
                if (shift_) return false;
                if (project_page_ != ProjectPage::slots) {
                    project_page_ = ProjectPage::slots;   // back, from any other page
                    notice("PROJECT");
                } else {
                    post(Cmd::cancel_queue);
                    notice("QUEUE OFF");
                }
                return true;

            case ENAGA_KEY_UP:
                if (shift_) {
                    // The chain follows the project's own entries; starting one jumps to its
                    // first.
                    const bool on = engine_.chain().active;
                    post(Cmd::set_chain, 0, 0, on ? 0 : 1);
                    notice(on ? "CHAIN OFF" : "CHAIN ON");
                } else {
                    move_slot_cursor(-4);
                }
                return true;

            case ENAGA_KEY_DOWN:
                if (shift_) {
                    // Load the selected track's sample again. The one thing a user can do about
                    // a FAIL, and the only way back after a card has been out.
                    if (storage_.request_sample(selected_)) notice("LOADING");
                    else notice("NO SAMPLE");
                } else {
                    move_slot_cursor(+4);
                }
                return true;

            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (shift_) {
                    // The card's two whole-project actions, PROJECT's F1 + arrow (the mode's
                    // second meaning, as copy and paste are in GRID): F1 + LEFT reads the
                    // project from the card again, F1 + RIGHT starts a new one. Both throw away
                    // what is unsaved, so each takes a second press within kSaveArmMs, as a save
                    // does. They were F1 + OK and F1 + CLR, which the spec gives to redo and undo.
                    arm_card_action(key == ENAGA_KEY_LEFT ? CardAction::reload : CardAction::fresh);
                } else {
                    move_slot_cursor(key == ENAGA_KEY_LEFT ? -1 : +1);
                }
                return true;

            default:
                return false;
        }
    }

    enum class CardAction : uint8_t { none, reload, fresh };

    void arm_card_action(CardAction action) {
        if (card_armed_ == action && uint32_t(last_ms_ - card_armed_at_ms_) < kSaveArmMs) {
            card_armed_ = CardAction::none;
            if (action == CardAction::reload) {
                storage_.request_reload();
                notice("RELOADING");
            } else {
                storage_.request_new();
                notice("NEW PROJECT");
            }
            return;
        }
        card_armed_ = action;
        card_armed_at_ms_ = last_ms_;
        notice(action == CardAction::reload ? "AGAIN TO RELOAD" : "AGAIN FOR NEW");
    }

    // The cursor over PROJECT's sixteen slots, drawn four by four: LEFT and RIGHT step through
    // them in reading order, UP and DOWN by a row. It stops at the ends rather than wrapping,
    // because the bank is on the selection keys and wrapping would hide where the edge is.
    void move_slot_cursor(int delta) {
        if (project_page_ != ProjectPage::slots) return;
        const int next = slot_cursor_ + delta;
        if (next >= 0 && next < kPatternsPerBank) slot_cursor_ = next;
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
        post_command(command);

        char *out = notice_buffer_;
        *out++ = bank_letter(shown_bank_);
        append_padded(out, uint32_t(slot) + 1, 2);
        append(out, " ");
        append(out, queue_mode_name(queue_mode_));
        *out = 0;
        notice(notice_buffer_);
    }

    // --- PROJECT's CHAIN page ---------------------------------------------
    //
    // The chain as a list, one entry per row: UP and DOWN move through the entries, LEFT and
    // RIGHT step the entry's pattern through the 128, a STEP key sets it to that slot of the
    // shown bank, VALUE (or A) sets how many times it repeats. OK adds an entry after the cursor
    // -- a copy of the one there, or the playing pattern when the chain is empty -- and F1 + LEFT
    // removes the one under the cursor. CLR goes back. F1 + UP is still CHAIN ON/OFF.
    //
    // The entries are the audio side's (it plays them), so every edit is a command, and the
    // knob and arrow edits are deltas applied there. Chain edits are not undoable: core/undo
    // snapshots the pattern, and the chain is not in a pattern.

    int chain_entries() const { return chain_length(engine_.project()); }

    void chain_clamp_cursor() {
        const int length = chain_entries();
        if (chain_cursor_ >= length) chain_cursor_ = length - 1;
        if (chain_cursor_ < 0) chain_cursor_ = 0;
    }

    void post_chain(ChainEdit edit, int at, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) {
        Command command;
        command.cmd = Cmd::chain_edit;
        command.step = uint8_t(at);
        command.a = uint8_t(edit);
        command.b = b;
        command.c = c;
        command.d = d;
        post_command(command);
    }

    void chain_delta(ChainEdit edit, int delta) {
        if (chain_entries() == 0) {
            notice("CHAIN EMPTY");
            return;
        }
        chain_clamp_cursor();
        post_chain(edit, chain_cursor_, uint8_t(int8_t(clamp_delta(delta))));
    }

    void chain_put_slot(int slot) {
        const int length = chain_entries();
        if (length == 0) {
            post_chain(ChainEdit::insert, 0, uint8_t(shown_bank_), uint8_t(slot), 1);
            chain_cursor_ = 0;
        } else {
            chain_clamp_cursor();
            const uint8_t repeats = engine_.project().chain[chain_cursor_].repeats;
            post_chain(ChainEdit::set, chain_cursor_, uint8_t(shown_bank_), uint8_t(slot),
                       repeats);
        }
        char *out = notice_buffer_;
        *out++ = bank_letter(shown_bank_);
        append_padded(out, uint32_t(slot) + 1, 2);
        *out = 0;
        notice(notice_buffer_);
    }

    bool on_chain_key(int key) {
        const int length = chain_entries();
        switch (key) {
            case ENAGA_KEY_OK: {
                if (shift_) return false;                 // F1 + OK: reload, as on the slots
                if (length >= kChainEntries) {
                    notice("CHAIN FULL");
                    return true;
                }
                if (length == 0) {
                    const Status &status = engine_.status();
                    post_chain(ChainEdit::insert, 0,
                               status.live_bank.load(std::memory_order_relaxed),
                               status.live_slot.load(std::memory_order_relaxed), 1);
                    chain_cursor_ = 0;
                } else {
                    chain_clamp_cursor();
                    const ChainEntry entry = engine_.project().chain[chain_cursor_];
                    post_chain(ChainEdit::insert, chain_cursor_ + 1, entry.bank, entry.slot,
                               entry.repeats);
                    chain_cursor_++;
                }
                notice("ENTRY ADDED");
                return true;
            }
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) return false;                 // chain on/off, sample reload
                chain_cursor_ += key == ENAGA_KEY_UP ? -1 : 1;
                chain_clamp_cursor();
                return true;
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (shift_) {
                    if (key == ENAGA_KEY_RIGHT) return true;   // free
                    if (length == 0) return true;
                    chain_clamp_cursor();
                    post_chain(ChainEdit::remove, chain_cursor_);
                    if (chain_cursor_ > 0 && chain_cursor_ >= length - 1) chain_cursor_--;
                    notice("ENTRY REMOVED");
                    return true;
                }
                chain_delta(ChainEdit::pattern_delta, key == ENAGA_KEY_LEFT ? -1 : 1);
                return true;
            default:
                return false;
        }
    }

    void draw_chain(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Project &project = engine_.project();
        const int length = chain_length(project);
        const ChainPlayer &player = engine_.chain();

        append(out, "CHAIN ");
        append_number(out, uint32_t(length));
        append(out, "/64");
        append(out, player.active ? " ON" : " OFF");
        *out = 0;
        canvas.text(0, 18, line, 15);

        if (length == 0) {
            canvas.text(0, 40, "EMPTY", 11);
            canvas.text(0, 52, "OK ADDS THE PLAYING", 9);
            canvas.text(0, 61, "PATTERN. STEP KEYS", 9);
            canvas.text(0, 70, "ADD FROM THIS BANK", 9);
        } else {
            // Seven rows, scrolled so the cursor stays on screen.
            constexpr int kRows = 7;
            int first = chain_cursor_ - kRows / 2;
            if (first > length - kRows) first = length - kRows;
            if (first < 0) first = 0;
            for (int row = 0; row < kRows && first + row < length; row++) {
                const int i = first + row;
                const ChainEntry &entry = project.chain[i];
                out = line;
                *out++ = i == chain_cursor_ ? '>' : ' ';
                append_padded(out, uint32_t(i) + 1, 2);
                *out++ = ' ';
                *out++ = bank_letter(int(entry.bank));
                append_padded(out, uint32_t(entry.slot) + 1, 2);
                append(out, " X");
                append_number(out, uint32_t(entry.repeats));
                if (player.active && int(player.entry) == i) append(out, " PLAY");
                *out = 0;
                canvas.text(0, 30 + row * 10, line, i == chain_cursor_ ? 15 : 10);
            }
        }

        canvas.text(0, 103, "OK ADD  FUNC+LEFT DEL", 7);
        canvas.text(0, 112, "VALUE REPEATS", 7);
    }

    // --- PROJECT's PATTERN page: the master length and the swing ----------

    void pattern_field_delta(int field, int delta) {
        if (field == 0) {
            mark(GestureKind::length, 0, 0);
            post(Cmd::set_master_length, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
            notice("LEN");
        } else {
            mark(GestureKind::length, 0, 0);
            post(Cmd::set_pattern_swing, 0, 0, 0, clamp_delta(delta), kDeltaFlag);
            notice("SWING");
        }
    }

    bool on_pattern_page_key(int key) {
        switch (key) {
            case ENAGA_KEY_LEFT:
            case ENAGA_KEY_RIGHT:
                if (shift_) return true;                  // F1 + LEFT/RIGHT: free
                pattern_field_ = key == ENAGA_KEY_LEFT ? 0 : 1;
                return true;
            case ENAGA_KEY_UP:
            case ENAGA_KEY_DOWN:
                if (shift_) return false;
                pattern_field_delta(pattern_field_, key == ENAGA_KEY_UP ? 1 : -1);
                end_gesture();
                return true;
            case ENAGA_KEY_OK:
                return !shift_;                           // nothing to pick here
            default:
                return false;
        }
    }

    void draw_pattern_page(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const Status &status = engine_.status();
        const Pattern &pattern = engine_.pattern();

        append(out, "PATTERN ");
        *out++ = bank_letter(int(status.live_bank.load(std::memory_order_relaxed)));
        append_padded(out, uint32_t(status.live_slot.load(std::memory_order_relaxed)) + 1, 2);
        *out = 0;
        canvas.text(0, 18, line, 15);

        out = line;
        *out++ = pattern_field_ == 0 ? '>' : ' ';
        append(out, "LEN ");
        append_number(out, uint32_t(pattern.master_length));
        append(out, " STEPS");
        *out = 0;
        canvas.text(0, 40, line, pattern_field_ == 0 ? 15 : 10);

        out = line;
        *out++ = pattern_field_ == 1 ? '>' : ' ';
        append(out, "SWG ");
        append_number(out, uint32_t(pattern.swing));
        *out++ = '%';
        *out = 0;
        canvas.text(0, 52, line, pattern_field_ == 1 ? 15 : 10);

        canvas.text(0, 76, "LEN IS WHAT A QUEUED", 8);
        canvas.text(0, 85, "CHANGE AND A CHAIN", 8);
        canvas.text(0, 94, "WAIT FOR", 8);
        canvas.text(0, 112, "UP DOWN VALUE OR A B", 7);
    }

    // --- PROJECT's SETUP page: the clock, MIDI and the record quantize ----

    static const char *clock_source_name(int source) {
        switch (source) {
            case ENAGA_CLOCK_INTERNAL: return "INTERNAL";
            case ENAGA_CLOCK_MIDI:     return "MIDI";
            case ENAGA_CLOCK_SYNC:     return "SYNC IN";
            default:                   return "?";
        }
    }

    void draw_setup(enaga::Canvas &canvas) {
        char line[40];
        char *out = line;
        const enaga::clock::State clock = enaga::clock::state();

        canvas.text(0, 20, "SETUP", 14);

        // CLOCK SRC, read from the clock itself rather than from anything STRATA remembers: it
        // is the user's setting and it can change under us at any moment.
        out = line;
        append(out, "CLOCK SRC ");
        append(out, clock_source_name(int(clock.source)));
        *out = 0;
        canvas.text(0, 34, line, 13);

        // The tempo, and whose it is. Under an outside source STRATA does not own it, so the
        // number is shown as a measurement rather than as a setting (docs/SPEC.md, and
        // CLAUDE.md: "STRATA does not own the tempo").
        out = line;
        append(out, "TEMPO ");
        append_tenths(out, uint32_t(clock.tempo * 10.0f + 0.5f));
        append(out, int(clock.source) == ENAGA_CLOCK_INTERNAL ? " OURS" : " EXT");
        *out = 0;
        canvas.text(0, 46, line, int(clock.source) == ENAGA_CLOCK_INTERNAL ? 13 : 10);

        // MIDI CH, also the user's.
        out = line;
        append(out, "MIDI CH ");
        append_number(out, uint32_t(enaga::midi::channel() + 1));
        *out = 0;
        canvas.text(0, 58, line, 13);

        // The record quantize amount, which IS ours and is on encoder A.
        out = line;
        append(out, "QUANTIZE ");
        append_number(out, uint32_t(quantize()));
        *out = 0;
        canvas.text(0, 70, line, 15);
        const int bar = quantize() * 60 / 100;
        canvas.rect(0, 80, 60, 5, 3);
        if (bar > 0) canvas.rect(0, 80, bar, 5, 12);

        // And where the three settings above are changed, because they are not changed here.
        // CLOCK OUT and SYNC OUT are in the same menu and Enaga exposes no way to read them,
        // so they are named rather than shown with a state this screen would be guessing at.
        // No commas: the font has none, and each drew as '?'.
        canvas.text(0, 94, "CLK SRC/OUT SYNC OUT", 7);
        canvas.text(0, 103, "MIDI CH: SET IN", 7);
        canvas.text(0, 112, "FUNC+POWER > SETTINGS", 7);
    }

    void draw_project(enaga::Canvas &canvas) {
        if (project_setup()) {
            draw_setup(canvas);
            return;
        }
        if (project_page_ == ProjectPage::chain) {
            draw_chain(canvas);
            return;
        }
        if (project_page_ == ProjectPage::pattern) {
            draw_pattern_page(canvas);
            return;
        }
        char line[40];
        char *out = line;
        const Status &status = engine_.status();

        // The project, and whether RAM differs from the card. An unsaved project that looked
        // saved is the worst outcome this screen can produce, so the marker is on the name.
        //
        // The mark is "UNSAVED" on the status line below rather than a '*' after the name: the
        // font has no '*', so it drew as '?'. The name is the card's, so it is cut to fit and
        // shown in characters the font has.
        const bool usable = storage_.card_usable();
        out = line;
        append_shown(out, storage_.project_name(), usable ? kLineChars : kLineChars - 8);
        if (!usable) append(out, " NO CARD");
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
            append_shown(out, storage_.last_error(), kLineChars);
        } else if (storage_.dirty()) {
            append(out, "UNSAVED");
        }
        *out = 0;
        canvas.text(0, 27, line, busy != nullptr || storage_.dirty() ? 12 : 9);

        // The global key and scale, on encoders B and C. Drawn from the live PATTERN rather than
        // from the project settings, because the pattern's copy is what the sequencer and every
        // mutation actually fold into -- a pattern loaded in its own key must read as that key.
        out = line;
        append(out, "KEY ");
        append(out, note_name(engine_.key_root()));
        *out++ = ' ';
        append(out, scale_name(int(engine_.key_scale())));
        *out = 0;
        canvas.text(0, 103, line, 11);   // below the slots: at 94 it ran through their last row

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
            if (slot == slot_cursor_) {
                // The cursor: a frame round the slot, in the gap between slots, so it never
                // touches the number inside.
                canvas.rect(x - 1, y - 1, 28, 13, 15);
                canvas.rect(x, y, 26, 11, 0);
            }
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
        canvas.text(0, 112, line, 10);
    }

private:
    void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
              uint8_t c = 0) {
        Command command;
        command.cmd = cmd;
        command.track = track;
        command.step = step;
        command.a = a;
        command.b = b;
        command.c = c;
        post_command(command);
    }

    // The same, for a command that carries a float or a wide value and so has to be built by
    // hand rather than from `post`'s byte arguments.
    //
    // **Every post goes through here.** Six gestures used to call `engine_.commands().post`
    // directly and so skipped `note_command` altogether -- a mutate, a live-recorded hit, a
    // queued pattern, a macro assignment, a morph move and a mixer level each changed a file
    // while the screen went on claiming the project was saved. The ring and the dirty flags have
    // to be fed by one function or they drift apart again the next time someone needs a float.
    void post_command(const Command &command) {
        engine_.commands().post(command);

        // Which of a project's ten files this edit will have changed. The live bank is read from
        // the status rather than assumed, because a chain may have moved the live pattern without
        // the UI asking (app/storage.hpp says why the flags live on this side).
        storage_.note_command(
            command.cmd, int(engine_.status().live_bank.load(std::memory_order_relaxed)));
    }

    // The tempo, as the engine wants it: BPM in `f` (app/commands.hpp). The engine stores it in
    // the project -- which is what makes it SAVE -- and re-derives the tempo-synced delay from
    // it. `enaga::clock::set_tempo` is a separate call, because the clock is Enaga's and the
    // project is ours; both have to be told, and only one of them can refuse.
    void post_tempo(float bpm) {
        Command command;
        command.cmd = Cmd::set_tempo;
        command.f = bpm;
        post_command(command);
    }

    // An encoder delta, as a signed byte. The audio side applies it to the authoritative step,
    // which is why these carry a delta and not a value (app/commands.hpp explains the choice).
    void post_delta(Cmd cmd, int track, int step, uint8_t a, int delta) {
        post(cmd, uint8_t(track), uint8_t(step), a, clamp_delta(delta));
    }

    // An encoder delta as the signed byte a command carries.
    static uint8_t clamp_delta(int delta) {
        const int clamped = delta < -120 ? -120 : (delta > 120 ? 120 : delta);
        return uint8_t(int8_t(clamped));
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
        // Any new edit settles a mutation still waiting for OK or CLR: CLR would otherwise undo
        // the newer edit and say it reverted the mutation.
        mutation_pending_ = false;
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

    // --- the performance layer's UI state (M5) ---
    // All of it is which control is selected or held, never what the layer produces: the engine
    // owns that, and the UI reads it back through the accessors.
    bool mute_mode_ = false;                    // the upper MUTE pad: lower pads become mutes
    bool mutation_pending_ = false;             // a MUTATE waiting for OK (keep) or CLR (revert)
    int16_t keys_held_note_[16] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                   -1, -1, -1, -1, -1, -1, -1, -1};   // KEYS: what each key holds
    uint8_t keys_held_track_[16] = {};          // and on which track
    int scene_holding_ = -1;                    // a scene pad held: -1, 0 for A, 1 for B
    int macro_selected_ = 0;

    // The pad currently bending macro 1, and the last value sent for it. -1 for none; see
    // on_pad_pressure on why one pad at a time and why only changes are posted.
    // Resampling's two settings: how many bars, and where the take lands (-1 = the file only).
    int take_bars_index_ = 0;
    int resample_track_ = -1;

    int pressure_pad_ = -1;
    uint8_t pressure_sent_ = 0;
    uint8_t macro_position_[kMacros] = {};
    uint16_t morph_ = 0;
    bool repeat_held_ = false;
    RepeatLength repeat_length_ = RepeatLength::sixteenth;
    AutoFill auto_fill_ = AutoFill::off;
    Mutation mutation_ = Mutation::velocity;
    int mutate_amount_ = 50;
    uint32_t mutate_variant_ = 1;

    // The Euclidean generator's three numbers (F1 + A, B, C in GRID). They live here rather than
    // in the pattern because the pattern stores the RESULT: a generated rhythm is then edited by
    // hand, and keeping the numbers would make the screen claim a figure the steps no longer are.
    // KEYS mode's octave, 0 being the one the pads play in.
    int keys_octave_ = 0;

    int euclid_hits_ = 4;
    int euclid_steps_ = 16;
    int euclid_rotation_ = 0;
    int page_ = 0;            // step page 1-4, for 64-step tracks
    int param_page_ = int(Page::src);
    int mixer_bank_ = 0;      // which kMixerPages entry: the faders, then the sends and master
    // PROJECT mode's pages, on the octave keys: the slots, the chain, the pattern's own
    // settings and SETUP.
    enum class ProjectPage : uint8_t { slots = 0, chain, pattern, setup, count };
    ProjectPage project_page_ = ProjectPage::slots;
    bool project_setup() const { return project_page_ == ProjectPage::setup; }
    int chain_cursor_ = 0;         // the CHAIN page's entry
    int pattern_field_ = 0;        // the PATTERN page's field: 0 LEN, 1 SWG
    static const char *project_page_name(ProjectPage page) {
        switch (page) {
            case ProjectPage::slots:   return "PROJECT";
            case ProjectPage::chain:   return "CHAIN";
            case ProjectPage::pattern: return "PATTERN";
            default:                   return "SETUP";
        }
    }
    int slot_cursor_ = 0;          // PROJECT's slot cursor, on the arrows; OK queues it
    bool save_armed_ = false;      // F1 + F3 once; the second press within kSaveArmMs saves
    uint32_t save_armed_at_ms_ = 0;
    CardAction card_armed_ = CardAction::none;   // F1 + LEFT / RIGHT in PROJECT, armed once
    uint32_t card_armed_at_ms_ = 0;
    int kit_cursor_ = 0;                        // KIT: the slot under the cursor, 0-15
    KitAction kit_armed_ = KitAction::none;     // KIT: OK or F1 + DOWN pressed once
    uint32_t kit_armed_at_ms_ = 0;
    static constexpr uint32_t kSaveArmMs = 2000;

    // The project tempo this side has already reconciled with the clock, so that a LOAD is
    // adopted and the engine's echo of our own knob move is not.
    uint16_t adopted_tempo_centi_ = 13000;

    // The screen last logged, as mode * 16 + page. -1 so the first pass logs whatever is up.
    int logged_screen_ = -1;
    bool shift_ = false;
    bool stop_armed_ = false;
    bool clock_refused_ = false;   // the last transport call was refused: an external source
    bool muted_[kTracks] = {};

    // --- M3 edit state ----------------------------------------------------
    bool rec_ = false;             // F2: live and motion recording
    // The live-record quantize amount is a PROJECT field and the project is the only copy:
    // there is no `quantize_` member, because a second copy here is exactly how the value came
    // to be settable and unsaveable. The engine owns the write, through `Cmd::set_quantize`.
    int quantize() const { return int(engine_.project().settings.quantize); }
    int held_step_ = -1;           // the STEP key being held, absolute 0-63; -1 for none
    int held_key_ = -1;            // and the PHYSICAL key it came from, 0-15, so the release
                                   // matches even if the page moved under it
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
