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
//
// ## Where the rest of the class is
//
// The class body is split over the `app/ui_*.hpp` files, which this file includes INSIDE the
// class, in the order the text had when it was one 8,700-line file: input, the editing
// gestures, MIDI in and the LEDs, the screen, EDIT, MIXER, KEYS and the lists, the FUNC menu
// and the performance mixer, PERFORM, PROJECT. Here are what comes before them (the helpers and
// tables, `init`, `stop`, `audio`) and after them (the private helpers and all the state). A
// fragment is not a header of its own; a new function goes in the fragment for its screen.
#pragma once

#include "arp.hpp"          // the ARP page's names and clicks
#include "engine.hpp"
#include "help.hpp"         // the FUNC help's pages, one set per screen
#include "energy.hpp"       // ENERGY: FUNC + C in PERFORM
#include "liverand.hpp"     // LIVE RND: FUNC + D in PERFORM
#include "paint.hpp"        // the drawing kit: lines, arcs, the small font, afterglow
#include "spectrum.hpp"     // the output's spectrum for MIXER's master pages
#include "record.hpp"       // live recording's stamping and quantizing, used from here only
#include "sounds.hpp"       // SOUNDS: the factory sounds for one track
#include "arppresets.hpp"   // ARPS: the factory arps for one track
#include "stepnotes.hpp"    // a chord step's extra notes: counted on the step page, not as locks
#include "storage.hpp"      // projects, autosave and the sample loader: the card side
#include "tuning.hpp"       // TUN and PIT as notes: the NOTE view

#include <enaga/app.hpp>
#include <enaga/clock.hpp>
#include <enaga/midi.hpp>
#include <enaga/ui.hpp>

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

// A signed amount with its sign, "+12", "-6" or "0" (REL ALL).
void append_signed(char *&out, int value) {
    if (value != 0) *out++ = value < 0 ? '-' : '+';
    append_number(out, uint32_t(value < 0 ? -value : value));
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
// filter, so this is a DEVIATION, recorded in docs/archive/REVIEW.md: fourteen settings that reach a
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
    // The sidechain duck (core/project.hpp, KitDuck): how far each track ducks, four at a time,
    // then which track fires it and its envelope. Kit bytes, handled on their own like the
    // faders: `field` is unused.
    {"DCK 1-4", false, {-1, -1, -1, -1}},
    {"DCK 5-8", false, {-1, -1, -1, -1}},
    {"DUCK", false, {-1, -1, -1, -1}},
    // The stereo width of each POLY track (KitTrack::width, core/width.hpp): kit bytes, handled on
    // their own like the duck's depths.
    {"WID 1-4", false, {-1, -1, -1, -1}},
    {"WID 5-8", false, {-1, -1, -1, -1}},
    {"DLY", false, {int8_t(SendField::delay_seconds), int8_t(SendField::delay_feedback),
                    int8_t(SendField::delay_tone), int8_t(SendField::delay_level)}},
    // The ambient modes (dsp/ambient.hpp), each beside its send: TYP switches the mode, and the
    // rest only sound once it is on.
    {"DLY+", false, {int8_t(SendField::delay_mode), int8_t(SendField::delay_multiplier),
                     int8_t(SendField::delay_wow), int8_t(SendField::delay_diffusion)}},
    {"REV", false, {int8_t(SendField::reverb_decay_s), int8_t(SendField::reverb_damping),
                    int8_t(SendField::reverb_high_pass_hz), int8_t(SendField::reverb_level)}},
    {"REV+", false, {int8_t(SendField::reverb_mode), int8_t(SendField::reverb_size),
                     int8_t(SendField::reverb_mod), int8_t(SendField::reverb_shimmer)}},
    {"ROUTE", false, {int8_t(SendField::delay_ping_pong), int8_t(SendField::delay_to_reverb),
                      int8_t(SendField::delay_low_cut_hz), -1}},
    // The chorus, send C: how much of each track goes to it, four at a time (kit bytes, handled
    // on their own like the duck's depths), then the chorus itself.
    {"CHO 1-4", false, {-1, -1, -1, -1}},
    {"CHO 5-8", false, {-1, -1, -1, -1}},
    {"CHO", false, {int8_t(SendField::chorus_rate), int8_t(SendField::chorus_depth),
                    int8_t(SendField::chorus_level), int8_t(SendField::chorus_to_reverb)}},
    {"MST", true, {int8_t(MasterField::dj), int8_t(MasterField::compressor),
                   int8_t(MasterField::limit), int8_t(MasterField::level)}},
    // The kill EQ, next to the master it sits in. Push A, B or C to throw that band's kill
    // switch; FUNC + push does the same from any mode.
    {"EQ", true, {int8_t(MasterField::eq_low), int8_t(MasterField::eq_mid),
                  int8_t(MasterField::eq_high), -1}},
};


constexpr int kMixerPageCount = int(sizeof(kMixerPages) / sizeof(kMixerPages[0]));

// Which MIXER page is the kill EQ's, for the plain-push kill switches.
constexpr int kMixerEqPage = kMixerPageCount - 1;

// The duck's pages: two of depths, then the source and the envelope.
constexpr int kMixerDuckDepthPage = kMixerTrackPages;
constexpr int kMixerDuckPage = kMixerDuckDepthPage + 2;
static_assert(kMixerPages[kMixerDuckPage].name[0] == 'D' && kMixerPages[kMixerDuckPage].name[1] == 'U',
              "kMixerDuckPage no longer names the duck's page");

// The widths' two pages, after the duck's.
constexpr int kMixerWidthPage = kMixerDuckPage + 1;
static_assert(kMixerPages[kMixerWidthPage].name[0] == 'W' && kMixerPages[kMixerWidthPage].name[4] == '1',
              "kMixerWidthPage no longer names WID 1-4");
constexpr bool mixer_width_page(int page) {
    return page == kMixerWidthPage || page == kMixerWidthPage + 1;
}

// The delay's two pages, which show the delay's time in milliseconds.
constexpr int kMixerDelayPage = kMixerWidthPage + 2;

// The ambient modes' pages, whose push A throws the mode switch (TYP) like turning it does.
constexpr int kMixerLongPage = kMixerDelayPage + 1;
constexpr int kMixerHallPage = kMixerDelayPage + 3;
static_assert(kMixerPages[kMixerDelayPage].field[0] == int8_t(SendField::delay_seconds),
              "kMixerDelayPage no longer names the delay's page");
static_assert(kMixerPages[kMixerLongPage].field[0] == int8_t(SendField::delay_mode) &&
                  kMixerPages[kMixerHallPage].field[0] == int8_t(SendField::reverb_mode),
              "the ambient pages moved: push A would throw the wrong switch");

// The chorus's pages: two of sends, then the chorus. After ROUTE, so the pages the scenarios
// count to (MST, EQ) are three further on than they were.
constexpr int kMixerChorusSendPage = kMixerHallPage + 2;
constexpr int kMixerChorusPage = kMixerChorusSendPage + 2;
static_assert(kMixerPages[kMixerChorusSendPage].name[0] == 'C' &&
                  kMixerPages[kMixerChorusSendPage].name[4] == '1' &&
                  kMixerPages[kMixerChorusPage].field[0] == int8_t(SendField::chorus_rate),
              "the chorus's pages moved");
constexpr bool mixer_chorus_send_page(int page) {
    return page == kMixerChorusSendPage || page == kMixerChorusSendPage + 1;
}

static_assert(kMixerPages[kMixerEqPage].master &&
                  kMixerPages[kMixerEqPage].field[0] == int8_t(MasterField::eq_low),
              "kMixerEqPage no longer names the kill EQ's page");

// Every send and master field has a slot on some page. Without this a field added to
// dsp/buses.hpp would be saved, loaded and completely unreachable -- which is the bug this whole
// change exists to fix, so it is worth a compile-time check rather than a comment.
constexpr bool mixer_pages_cover_every_field() {
    for (int f = 0; f < int(SendField::count); f++) {
        bool found = false;
        for (int p = kMixerDelayPage; p < kMixerPageCount; p++) {
            if (kMixerPages[p].master) continue;
            for (int slot = 0; slot < 4; slot++) found = found || kMixerPages[p].field[slot] == f;
        }
        if (!found) return false;
    }
    for (int f = 0; f < int(MasterField::count); f++) {
        bool found = false;
        for (int p = kMixerDelayPage; p < kMixerPageCount; p++) {
            if (!kMixerPages[p].master) continue;
            for (int slot = 0; slot < 4; slot++) found = found || kMixerPages[p].field[slot] == f;
        }
        if (!found) return false;
    }
    return true;
}

static_assert(mixer_pages_cover_every_field(),
              "a send or master field has no MIXER slot, so nothing can reach it");

// A time for the ASR page, at most five characters: whole milliseconds under a second ("3MS",
// "250MS"), tenths of a second above ("1.2S", "8.0S").
void append_seconds(char *&out, float seconds) {
    const uint32_t ms = uint32_t(seconds * 1000.0f + 0.5f);
    if (ms < 1000) {
        append_number(out, ms);
        append(out, "MS");
    } else {
        append_tenths(out, (ms + 50) / 100);
        *out++ = 'S';
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

// docs/SPEC.md's performance row: the seven UPPER pads, which the panel controller numbers with
// the odd ids. Fixed, in every mode -- that is what makes it a performance row rather than
// another page.
enum class PerformPad : uint8_t { fill = 1, repeat = 3, mute = 5, scene_a = 7, scene_b = 9,
                                  throw_fx = 11, mutate = 13 };

constexpr bool is_perform_pad(int pad) { return pad % 2 == 1 && pad >= 1 && pad <= 13; }

// The seven, left to right, with the colour each is lit in (docs/CONTROLS.md). The print on
// the panel says TRK 1-7, so the light and the screen have to say what the pads are instead:
// FILL white, REPEAT cyan, MUTE red, SCENE A green, SCENE B blue, THROW amber, MUTATE violet.
constexpr int kPerformPads = 7;
constexpr int kPerformPadIds[kPerformPads] = {1, 3, 5, 7, 9, 11, 13};
constexpr Rgb kPerformColour[kPerformPads] = {
    {8, 8, 8}, {0, 8, 8}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}, {10, 5, 0}, {8, 0, 10},
};
// The STEP keys' green for a trig, by velocity: four bands the eye can tell apart on the unit's
// 0-10 LED levels (soft, medium, loud, accent). A blank kit's 100 is "loud".
constexpr uint8_t step_velocity_level(uint8_t velocity) {
    return velocity >= 110 ? 10 : (velocity >= 80 ? 6 : (velocity >= 40 ? 3 : 1));
}
constexpr uint32_t kConditionBlinkMs = 200;     // half a blink: on 200 ms, off 200 ms
constexpr uint32_t kLearnClrGuardMs = 2000;     // CLR after the CLR that ended LEARN: swallowed

constexpr const char *kPerformPadNames[kPerformPads] = {
    "FILL", "REPEAT", "MUTE", "SCENE A", "SCENE B", "THROW", "MUTATE",
};

// KEYS plays the two pad rows as a piano (docs/CONTROLS.md): they are the pressure-sensitive
// ones, and the upper seven sit between the lower eight like black keys between white. Semitones
// above the octave's C, by pad id; kPadOctaveDown / kPadOctaveUp are the two upper pads with no
// black key under them (between E and F, and after B), which walk the octave instead.
constexpr int kPadOctaveDown = -1;
constexpr int kPadOctaveUp = -2;
constexpr int kPianoPads = 15;
constexpr int kPianoSemitone[kPianoPads] = {
    0,  1,  2,  3,  4, kPadOctaveDown, 5, 6, 7, 8, 9, 10, 11, kPadOctaveUp, 12,
};

// What a live notice shows (Strata::echo): nothing, the selected track's length and speed, or a
// sound parameter's value.
enum class Echo : uint8_t { none, layout, param };

// What a MUTATE waiting for its verdict shows until OK or CLR answers it.
constexpr const char *kVerdictHint = "OK KEEPS CLR REVERTS";

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// The octave the pads play the root in. The pattern's root is a pitch class (core/pattern.hpp),
// so it needs one to become a note.
constexpr uint8_t kPadOctaveNote = 60;

// The velocity a STEP key plays at. Keys have none of their own, and 100 is what the default
// kit's trigs use, so a part played in sounds like a part programmed in.
constexpr uint8_t kKeyVelocity = 100;

// The modes of docs/SPEC.md's function block. F4 = GRID, F7 = MIXER, F8 = EDIT, and the two
// selection keys under the screen move within whichever mode is showing -- the step page in
// GRID, the parameter page in EDIT, the bank of four tracks in MIXER.
enum class Mode : uint8_t { grid, edit, mixer, project, perform, keys, resample, machine, kit, samples,
                            menu, sounds };

// How long a take is. docs/SPEC.md: "1, 2, 4 or 8 bars", so these four and nothing between --
// a resample is a loop and a loop is a power of two bars.
constexpr uint8_t kTakeBars[] = {1, 2, 4, 8};
constexpr int kTakeBarChoices = int(sizeof kTakeBars / sizeof kTakeBars[0]);

// Where a take comes from: the finished master, or one of the audio input's routes as Enaga
// names them (<enaga/abi.h>, enaga_input_source). docs/SPEC.md: "Resample with a chosen input
// source"; Enaga's `system::input` is the call, main context only.
//
// Or one TRACK alone (2026-11-23, a bounce: docs/SPEC.md's "Record the master or one track"):
// the track's own sound after drive, filter and lo-fi, before its duck, fader, pan and sends,
// mono (Engine::capture_track_). Its sample can then be sliced, reversed or put on GRAIN.
struct TakeSource {
    const char *name;     // 7 characters at most: "FROM DYN MIC HIGH" is 17
    int input;            // the enaga_input_source, or -1 for the master or a track
    int track = -1;       // 0-7 for one track alone, else -1
};
constexpr TakeSource kTakeSources[] = {
    {"MASTER", -1},
    {"LINE", ENAGA_INPUT_LINE_STEREO},
    {"LINE L", ENAGA_INPUT_LINE_MONO},
    {"MIC", ENAGA_INPUT_MIC},
    {"DYN MIC", ENAGA_INPUT_DYNAMIC_MIC},
    {"HI-Z", ENAGA_INPUT_HIZ},
    {"TRK 1", -1, 0},
    {"TRK 2", -1, 1},
    {"TRK 3", -1, 2},
    {"TRK 4", -1, 3},
    {"TRK 5", -1, 4},
    {"TRK 6", -1, 5},
    {"TRK 7", -1, 6},
    {"TRK 8", -1, 7},
};
constexpr int kTakeSourceChoices = int(sizeof kTakeSources / sizeof kTakeSources[0]);
constexpr const char *kInputLevelNames[] = {"LOW", "NORM", "HIGH"};   // enaga_input_level

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
        case Mode::samples:  return "SAMPLES";
        case Mode::menu:     return "MENU";
        case Mode::sounds:   return "SOUNDS";
        default:             return "?";
    }
}

// SAMPLE's SLC, the parameter SLC AUTO goes with (dsp/sample.cpp's parameter table).
constexpr int kSlcParam = 3;

const char *page_name(Page page) {
    switch (page) {
        case Page::src:  return "SRC";
        case Page::fltr: return "FLTR";
        case Page::amp:  return "AMP";
        case Page::fx:   return "FX";
        case Page::lfo:  return "LFO";
        case Page::arp:  return "ARP";
        case Page::asr:  return "ENV";      // the whole envelope (Roland, 2026-11-26)
        case Page::osc:  return "OSC";
        default:         return "?";
    }
}

// The ASR page's encoder names (dsp/va.cpp's table). Only the VAs read the page; on any other
// machine its four slots show "---".
constexpr const char *kAsrNames[kAsrParams] = {"ATK", "SUS", "REL", "FSU"};
// And their OSC page's (dsp/va.cpp): OSC 1's wave, what OSC 2 does, the sub, the drive.
constexpr const char *kOscNames[kOscParams] = {"WAV", "OS2", "SUB", "DRV"};

// EDIT's pages: the four of sound, then the track's LFO (dsp/lfo.hpp), its arpeggiator
// (core/arp.hpp), the VA machines' note envelope, ASR, and their oscillators, OSC (dsp/va.cpp).
// The selection keys walk all eight; everywhere else a page is still four encoders over four
// parameters.
constexpr int kEditPages = kPages + 4;

// Encoder counts per click: one zone on the arp's MOD RAT OCT, one shape or destination on the
// LFO's WAV and DST, one count everywhere else (ASR and OSC included).
constexpr int param_click(int index) {
    return index >= kAsrFirst ? 1
                              : (index >= kArpFirst ? arp_click(index - kArpFirst) : lfo_click(index));
}
static_assert(page_first(Page(kEditPages - 1)) + kParamsPerPage == kTrackParams,
              "EDIT's last page is the last four track parameters");
// The FUNC help (core/help.hpp) has a knob page for each EDIT page and a page for each MIXER
// page, in these orders.
static_assert(kEditPages == kHelpEditPages, "an EDIT page the help knows nothing about");
static_assert(kMixerPageCount == kHelpMixerPages, "a MIXER page the help knows nothing about");

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
        // MIDI first: a note left sounding on outboard gear would hang there, and the save can
        // take most of the budget. Enaga has already removed the audio callback, so the main
        // context may end the notes itself.
        engine_.end_midi_for_shutdown();
        storage_.save_for_shutdown();
        enaga::log("strata stopped");
    }

    void on_card(bool usable) override {
        // Every file handle opened before a removal is already dead; Storage abandons whatever
        // was in flight and writes nothing until a card is back.
        storage_.on_card(usable);
        // The take on offer is a file on the card that went: nothing left to put on a track.
        if (!usable) take_offer_ = -1;
        notice(usable ? "CARD IN" : "CARD OUT");
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        engine_.process(in, out, frames);
    }

#include "ui_input.hpp"

#include "ui_gestures.hpp"

#include "ui_lights.hpp"

#include "ui_screen.hpp"

#include "ui_edit.hpp"
#include "ui_editview.hpp"

#include "ui_mixer.hpp"
#include "ui_mixview.hpp"

#include "ui_browsers.hpp"

#include "ui_perfmix.hpp"

#include "ui_perform.hpp"

#include "ui_modslots.hpp"

#include "ui_live.hpp"

#include "ui_project.hpp"
#include "ui_trackmenu.hpp"
#include "ui_pickview.hpp"
#include "ui_listview.hpp"

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
            command.cmd, int(engine_.status().live_bank.load(std::memory_order_relaxed)),
            int(command.track));
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
        notice_screen_ = screen_key();
    }

    // Which screen and page is up, as one number: a notice's toast belongs to the screen it was
    // said on, and a screen change takes it away (the review: RESAMPLE's band over PROJECT).
    uint32_t screen_key() const {
        uint32_t page = 0;
        if (mode_ == Mode::edit) page = uint32_t(param_page_);
        else if (mode_ == Mode::mixer) page = uint32_t(mixer_bank_);
        else if (mode_ == Mode::project) page = uint32_t(project_page_);
        return uint32_t(mode_) * 1000u + page + (step_page_ ? 500u : 0u);
    }

    // A notice the emulator's scenarios look for in the log, since they cannot read the screen.
    void notice_logged(const char *text) {
        notice(text);
        enaga::log(text);
    }

    bool notice_showing() const {
        return notice_ != nullptr && uint32_t(last_ms_ - notice_at_ms_) < 1500u;
    }

    Engine engine_;
    uint32_t stream_late_seen_ = 0;    // Status::stream_late as last noticed
    uint8_t song_end_seen_ = 0;        // Status::song_end as last acted on
    uint32_t stream_late_said_ms_ = 0;

    // The card side: projects, autosave and the sample loader, all in this context. It owns the
    // sample region and the pool, so no pool state is shared across the ring at all
    // (app/storage.hpp).
    Storage storage_;

    Mode mode_ = Mode::grid;
    int selected_ = 0;
    int shown_bank_ = 0;                        // PROJECT mode browses a bank without switching
    QueueMode queue_mode_ = QueueMode::end_of_pattern;
    // Hold PROJECT + STEP queues a pattern from any screen (app/ui_input.hpp, on_project_hold).
    // `project_down_` follows the key itself; `project_open_` is a plain press waiting for its
    // release to open PROJECT, which a STEP pressed meanwhile cancels. `project_steps_` are the
    // STEP keys the chord took, whose releases it swallows too.
    bool project_down_ = false;
    bool project_open_ = false;
    uint16_t project_steps_ = 0;
    uint32_t jam_lost_seen_ = 0;               // JAM REC's dropped frames last said
    uint32_t jam_lost_ms_ = 0;
    char notice_buffer_[24] = {};               // notice() keeps the pointer, so dynamic text
                                                // needs somewhere of its own to live

    // --- the performance layer's UI state (M5) ---
    // All of it is which control is selected or held, never what the layer produces: the engine
    // owns that, and the UI reads it back through the accessors.
    bool fill_held_ = false;                    // FILL held (its LED)
    bool throw_held_ = false;                   // THROW held (its LED)
    bool mute_mode_ = false;                    // the upper MUTE pad: lower pads become mutes
    bool note_repeat_mode_ = false;             // FUNC + FILL: a held lower pad rolls
    // A notice whose text is the LIVE value rather than the one at the press: a delta the audio
    // side applies (a track's length, a knob) does not exist on this side until the command has
    // been drained, so the text is rebuilt on every draw from what the engine now holds. Any
    // other notice replaces it, because notice() points notice_ elsewhere.
    Echo echo_ = Echo::none;
    int echo_param_ = 0;
    char echo_buffer_[24] = {};
    uint8_t perform_held_ = 0;                  // upper pads down, bit = pad id / 2
    int mutate_track_ = 0;                      // the track the last MUTATE went to
    uint8_t mutate_unchanged_seen_ = 0;         // Status::mutate_unchanged as last noticed
    int freeze_bars_index_ = 0;                 // FUNC menu FREEZE's length, into kTakeBars
    int loop_bars_index_ = 2;                   // FUNC menu LOOP LEN, into kTakeBars: 4 bars
    // The performance mixer (core/perffx.hpp): the slot VALUE turns, the effect that is up and
    // its amount, the looper's mix, and the STEP keys a pick took (their releases are its too).
    PerfFx perf_picked_ = PerfFx::none;
    PerfFx perf_fx_ = PerfFx::none;
    uint8_t perf_amount_ = 0;
    // FX GLIDE (the FUNC menu row) and its rides: the effect's, and the loop mix's.
    PerfGlide perf_glide_ = kPerfGlideDefault;
    // The FUNC menu's CLICK and COUNT-IN (dsp/click.hpp). Not saved, as FX GLIDE is not. The
    // count-in is used by a PLAY from stop with REC on, under the internal clock.
    uint8_t roll_note_[kTracks] = {};           // a NOTE RPT pad's note, for its recorded LEN
    bool click_on_ = false;
    uint8_t count_in_bars_ = 0;
    uint8_t count_in_shown_ = 0;                // the beat the count-in notice last said
    // A count-in's length in ticks as this side armed it at PLAY. The audio side publishes its own
    // (Status::count_in_ticks) only at the end of its next block, and a hit in between read 0
    // there and recorded during the count-in. The published figure wins once it is there.
    uint32_t count_in_armed_ = 0;
    uint32_t count_in_lead() const {
        const uint32_t published = engine_.status().count_in_ticks.load(std::memory_order_relaxed);
        return published != 0 ? published : count_in_armed_;
    }
    PerfRide perf_ride_{};
    bool perf_riding_ = false;
    uint32_t perf_ride_ms_ = 0;
    PerfRide loop_ride_{};
    bool loop_riding_ = false;
    uint32_t loop_ride_ms_ = 0;
    // The momentary push: the STEP held with VALUE pushed, since when, and whether it is up.
    static constexpr uint32_t kPerfHoldMs = 300;
    int perf_hold_step_ = -1;
    uint32_t perf_hold_ms_ = 0;
    bool perf_hold_on_ = false;
    bool perf_release_lets_go_ = false;
    // LOCK: FUNC + MUTATE held this long stores (the concept's half second).
    static constexpr uint32_t kLockHoldMs = 500;
    bool lock_press_ = false;
    bool lock_press_stored_ = false;
    uint32_t lock_press_ms_ = 0;
    uint8_t lock_event_seen_ = 0;
    uint8_t loop_mix_ = 0;
    uint16_t perf_step_keys_ = 0;
    bool value_alone_ = false;     // VALUE pushed with nothing else since: its release resets
    uint32_t value_down_ms_ = 0;   // when it was pushed: only a tap resets
    static constexpr uint32_t kValueTapMs = 500;
    // FUNC + LEN- / LEN+ held repeats (backlog [4]): after kLenRepeatDelayMs, a step every
    // kLenRepeatMs, so 16 to 64 is about two seconds rather than 48 presses.
    static constexpr uint32_t kLenRepeatDelayMs = 400;
    static constexpr uint32_t kLenRepeatMs = 50;
    int len_repeat_key_ = -1;      // the octave key held with FUNC, or -1
    uint32_t len_repeat_due_ms_ = 0;
    uint8_t frozen_seen_ = 0;                   // Status::frozen as update() last saw it
    const char *freeze_error_seen_ = nullptr;   // and Storage::freeze_error
    uint16_t rolling_pads_ = 0;                 // per track: its pad is rolling (Cmd::note_repeat)
    uint8_t roll_velocity_[kTracks] = {};       // the last velocity sent for each roll
    bool mutation_pending_ = false;             // a MUTATE waiting for OK (keep) or CLR (revert)
    uint8_t step_edit_pads_ = 0;                // upper pads (by pad / 2) that edited a held step
    int16_t pad_held_note_[kPianoPads] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                          -1, -1, -1, -1, -1, -1, -1};   // KEYS: piano pads
    uint8_t pad_held_track_[kPianoPads] = {};
    int macro_learn_ = -1;                      // MACRO LEARN: the macro EDIT's pushes assign to
    bool learn_ended_ = false;                  // and when it ended, for the CLR guard
    uint32_t learn_ended_at_ms_ = 0;
    int scene_holding_ = -1;                    // a scene pad held: -1, 0 for A, 1 for B
    int macro_selected_ = 0;

    // The pad currently bending macro 1, and the last value sent for it. -1 for none; see
    // on_pad_pressure on why one pad at a time and why only changes are posted.
    // Resampling's two settings: how many bars, and where the take lands (-1 = the file only).
    int take_bars_index_ = 0;
    int resample_track_ = -1;
    // Bounce to track: once a take asked for TO FILE is whole on the card, the screen offers to
    // put it on a track with one OK (take_offer_, -1 when nothing is offered). The track the take
    // came from (-1: the master or the input), so the offer can prefer another track.
    int take_offer_ = -1;
    int take_from_track_ = -1;
    uint32_t file_takes_seen_ = 0;
    // And where it comes from: an index into kTakeSources (0, the master, by default), the input
    // level (enaga_input_level, NORMAL as Enaga starts), and whether the route on screen has been
    // applied. Not saved: Enaga puts the input back to LINE, NORMAL when the app stops anyway.
    int take_source_ = 0;
    int take_level_ = ENAGA_INPUT_NORMAL;
    bool input_applied_ = false;
    int input_result_ = 0;

    int pressure_pad_ = -1;
    uint8_t pressure_sent_ = 0;
    uint8_t macro_position_[kMacros] = {};
    // REL ALL and LEN ALL as the knobs left them (core/moveall.hpp); posted absolute, like a
    // macro, and read back from the engine after a LOCK jump.
    int8_t rel_all_ = 0;
    int8_t len_all_ = 0;
    // LIVE RND (core/liverand.hpp): the unit FUNC + D left (0 OFF) and the tracks FUNC + push D
    // armed. Posted absolute, like REL ALL, and read back from the engine after a LOCK jump.
    uint8_t rnd_unit_ = kLiveRndOff;
    uint8_t rnd_armed_ = kLiveRndAllTracks;
    // ENERGY (core/energy.hpp): where FUNC + C left it, posted absolute like REL ALL and read
    // back from the engine after a LOCK jump.
    int8_t energy_ = 0;
    uint16_t morph_ = 0;
    bool repeat_held_ = false;
    RepeatLength repeat_length_ = RepeatLength::sixteenth;
    bool repeat_audio_ = false;      // the audio repeat is on: REPEAT held and no stutter yet
    bool repeat_pitch_ = false;      // FUNC was down when REPEAT went down: pitched down
    // Step stutter: the STEP keys taken by it (their releases are its), the one playing, or -1,
    // and its tracks (0xFF all; FUNC + STEP the selected one).
    uint16_t stutter_keys_ = 0;
    int stutter_raw_ = -1;
    int stutter_step_ = 0;           // the step it plays, the STEP page included
    uint8_t stutter_tracks_ = 0xFF;
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
    enum class ProjectPage : uint8_t { slots = 0, chain, pattern, setup, files, count };
    ProjectPage project_page_ = ProjectPage::slots;
    bool project_setup() const { return project_page_ == ProjectPage::setup; }
    int chain_cursor_ = 0;         // the CHAIN page's entry
    int pattern_field_ = 0;        // the PATTERN page's field: 0 LEN, 1 SWG, 2 FEEL, 3-10 swings
    static const char *project_page_name(ProjectPage page) {
        switch (page) {
            case ProjectPage::slots:   return "PROJECT";
            case ProjectPage::chain:   return "CHAIN";
            case ProjectPage::pattern: return "PATTERN";
            case ProjectPage::files:   return "PROJECTS";
            default:                   return "SETUP";
        }
    }
    // The PROJECTS page: the card's projects, the cursor on them, and the name being typed.
    int project_cursor_ = 0;
    bool project_seek_ = false;    // put the cursor on the open project once the list is in
    bool projects_reread_ = false; // list again once a SAVE AS has landed
    int open_armed_ = -1;          // OK once on this entry; the second within kSaveArmMs opens
    uint32_t open_armed_at_ms_ = 0;
    enaga::ui::TextEntry name_entry_;
    // What the name entry is for: SAVE AS (FUNC+DOWN, the menu) or a rename (push B), and the
    // project the rename is of.
    enum class EntryFor : uint8_t { save_as, rename };
    EntryFor entry_for_ = EntryFor::save_as;
    char rename_from_[Storage::kNameBytes] = {};
    bool projects_op_reread_ = false;  // list again once a rename or delete is done
    int delete_armed_ = -1;        // push C on this entry; OK twice within kSaveArmMs deletes it
    int delete_oks_ = 0;
    uint32_t delete_ok_at_ms_ = 0;
    int slot_cursor_ = 0;          // PROJECT's slot cursor, on the arrows; OK queues it
    bool slot_menu_ = false;       // the pattern menu (hold OK on a slot) is open
    int slot_menu_cursor_ = 0;
    bool slot_ok_held_ = false;    // OK down on a slot: a tap queues, a hold opens the menu
    uint32_t slot_ok_down_ms_ = 0;
    // The track menu (hold OK in GRID, app/ui_trackmenu.hpp): GEN's knobs and EVOLVE.
    bool track_menu_ = false;
    int track_menu_cursor_ = 0;
    bool track_ok_held_ = false;
    uint32_t track_ok_down_ms_ = 0;
    int gen_density_ = 50;
    int gen_range_ = 1;
    int gen_leap_ = 30;
    int gen_what_ = 0;                          // GenWhat
    uint32_t gen_variant_ = 1;
    EvolveMode evolve_mode_ui_ = EvolveMode::orbit;
    uint8_t evolve_turns_seen_ = 0;             // Status::evolve_turns as last noticed
    uint8_t evolve_loaded_seen_ = 0;            // Status::evolve_loaded as last noticed
    int clip_bank_ = 0;            // COPY on the slots: the pattern copied, slot -1 for none
    int clip_slot_ = -1;
    SlotAction slot_armed_ = SlotAction::none;   // a paste or clear waiting for its second press
    int slot_armed_bank_ = 0;
    int slot_armed_slot_ = 0;
    uint32_t slot_armed_at_ms_ = 0;
    Mode menu_return_ = Mode::grid;   // the screen the FUNC menu opened over
    int menu_cursor_ = 0;             // the FUNC menu's row; SAVE when it opens
    bool shift_alone_ = false;        // FUNC down and nothing else pressed since
    int help_page_ = 0;               // the FUNC help's page: 0 = the screen's own
    uint32_t shift_at_ms_ = 0;        // when FUNC went down
    int chain_remove_armed_ = -1;                 // CHAIN's F1 + LEFT, armed on this entry
    uint32_t chain_remove_armed_at_ms_ = 0;
    CardAction card_armed_ = CardAction::none;   // the FUNC menu's OK-twice rows, armed once
    uint32_t card_armed_at_ms_ = 0;
    int kit_cursor_ = 0;                        // KIT: the slot under the cursor, 0-15
    int sample_cursor_ = 0;                     // SAMPLES: the entry under the cursor
    bool sample_seek_ = false;                  // put the cursor on a name once the list is in
    char sample_seek_name_[SampleList::kEntryNameBytes] = {};
    Mode samples_return_ = Mode::edit;          // where CLR at the top goes back to
    int audition_row_ = -1;                     // SAMPLES: the entry being heard (push A), or -1
    // SAMPLES with the transport running: push A once on this entry; the second within
    // kSaveArmMs hears it.
    int audition_armed_ = -1;
    uint32_t audition_armed_at_ms_ = 0;
    bool audition_sounding_seen_ = false;       // Storage::audition_sounding() as update() saw it
    int sound_cursor_ = 0;                      // SOUNDS: 0 is AS WAS, then the presets
    int sound_track_ = -1;                      // the track whose sound the engine remembers
    Mode sounds_return_ = Mode::edit;           // where CLR goes back to
    bool sounds_arps_ = false;                  // the list is ARPS (core/arppresets.hpp)
    KitAction kit_armed_ = KitAction::none;     // KIT: OK or F1 + DOWN pressed once
    uint32_t kit_armed_at_ms_ = 0;
    static constexpr uint32_t kSaveArmMs = 2000;
    static constexpr uint32_t kDoubleTapMs = 400;   // HOME, HOME: KEYS
    bool home_tapped_ = false;
    uint32_t home_tapped_at_ms_ = 0;

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

    // MIDI IN's held notes: per channel and note, the track it started on plus 1, 0 for none.
    // 2 KiB, so a note off finds its track even after the selected track has moved.
    uint8_t midi_in_held_[16][128] = {};

    // Notes recorded while held, waiting for their key to come up so the step gets its LEN
    // (record_note / record_note_up). Sixteen is more fingers than a hand has, plus a MIDI
    // keyboard's sustain of a chord or two; past it the oldest press keeps the step's LEN.
    struct RecHold {
        int8_t track = -1;          // -1: a free slot
        uint8_t note = 0;
        uint8_t step = 0;
        Stamp down{};
    };
    static constexpr int kRecHolds = 16;
    RecHold rec_holds_[kRecHolds] = {};
    int rec_hold_next_ = 0;
    // On a chord track, the pass each step was last recorded in, plus 1 (0: not this take).
    // A note landing on a step already recorded in its own pass is another note of the chord
    // being played and joins it; the first one of a new pass replaces under REC MODE REPLACE.
    // Cleared when REC goes on and when the transport starts, because the pass count starts
    // again at 0 and an old mark would read as this pass. 2 KiB.
    uint32_t rec_pass_[kTracks][kStepsPerTrack] = {};
    void clear_rec_passes() {
        for (auto &track : rec_pass_) {
            for (uint32_t &pass : track) pass = 0;
        }
    }
    // And when the pattern playing changes -- a chain entry, a queued or NOW switch -- for the
    // same reason: the pass count is the new pattern's, from 0 at its origin, and a mark the last
    // pattern left on a step would read as this pass and join instead of replace. The origin and
    // the live pattern the marks were made against (Status), checked by every chord hit.
    uint32_t rec_pass_origin_ = 0;
    uint16_t rec_pass_pattern_ = 0;
    void follow_rec_pattern(uint32_t origin) {
        const Status &status = engine_.status();
        const uint16_t pattern =
            uint16_t(uint16_t(status.live_bank.load(std::memory_order_relaxed)) << 8 |
                     status.live_slot.load(std::memory_order_relaxed));
        if (origin == rec_pass_origin_ && pattern == rec_pass_pattern_) return;
        clear_rec_passes();
        rec_pass_origin_ = origin;
        rec_pass_pattern_ = pattern;
    }
    bool rec_add() const { return engine_.project().settings.rec_add; }
    // CC 16-31 are the track's sixteen parameters: the "General Purpose" 16-19 and the
    // undefined 20-31, which no common controller sends by default for anything else.
    static constexpr int kMidiInFirstParamCc = 16;
    // The live-record quantize amount is a PROJECT field and the project is the only copy:
    // there is no `quantize_` member, because a second copy here is exactly how the value came
    // to be settable and unsaveable. The engine owns the write, through `Cmd::set_quantize`.
    int quantize() const { return int(engine_.project().settings.quantize); }
    int held_step_ = -1;           // the STEP key being held, absolute 0-63; -1 for none
    // The parameter last locked (or cleared) on the held step, for the "S5 CUT 64>90" line.
    // Shown only while that same step of that same track is still held.
    int lock_readout_index_ = -1;
    int lock_readout_step_ = -1;
    int lock_readout_track_ = -1;
    int held_key_ = -1;            // and the PHYSICAL key it came from, 0-15, so the release
                                   // matches even if the page moved under it
    bool held_used_ = false;       // it was used as a modifier, so its release is not a tap
    int len_step_ = -1;            // held on a note + a later STEP: LEN to set on release
    uint8_t len_index_ = 0;
    int len_applied_step_ = -1;    // and the step it was set on, while the selection lives
    uint8_t len_applied_old_ = 0;  // with the LEN it had before
    int len_applied_track_ = 0;
    int range_first_ = -1;         // two STEP keys held: the selection, inclusive
    int range_last_ = -1;
    bool step_page_ = false;       // the step page is open
    int page_step_ = 0;            // and on this step
    int field_page_ = 0;           // four of the step's fields at a time
    uint8_t gesture_seq_ = 0;
    bool value_pushed_ = false;    // VALUE held down: swing rather than tempo
    // NOTE view (push the TUN or PIT knob on EDIT, GRID or KEYS): TUN and PIT read as the note
    // they put C-4 at and turn a semitone a click. On by default, since STRATA plays in a key; not
    // saved, so every start shows notes.
    bool tune_notes_ = true;

    const char *notice_ = nullptr;
    uint32_t notice_at_ms_ = 0;
    uint32_t notice_screen_ = 0;    // screen_key() when the notice was said
    static constexpr uint32_t kWaveSliceMs = 2;   // the WAVE build's share of a pass (update)
    int waves_logged_ = 0;        // tables built, as last logged (update)
    uint32_t last_ms_ = 0;
    // The frame's meter: the held reading and when it was taken, and the limiter's last count.
    uint32_t meter_hold_ = 0;
    uint32_t meter_hold_ms_ = 0;
    uint8_t limit_seen_ = 0;
    uint32_t limit_ms_ = 0;
    bool limit_lit_ = false;
    // The live views (app/ui_live.hpp): the newest output, drained from the engine's ring every
    // frame; the scope's afterglow; GRID's fading outlines and the trigger counts they follow.
    ScopeHistory scope_history_;
    paint::Glow<124, 35> scope_glow_;
    uint8_t grid_glow_[kTracks][16] = {};
    uint8_t grid_seen_[kTracks] = {};
    // EDIT's LFO page: the last 40 frames of the LFO's output, and how many have arrived.
    uint8_t lfo_strip_[40] = {};
    uint32_t lfo_strip_at_ = 0;
    uint32_t lfo_strip_ms_ = 0;      // when the strip last took a sample
    // GRAIN's SRC page: the cloud's afterglow (app/ui_editview.hpp, kCloudRows rows).
    paint::Glow<124, 26> grain_glow_;
    // EDIT's pictures that play a note through (app/ui_editview.hpp): each track's trigger count
    // as last seen, when it last moved, and whether it has since the app started; FLTR's spectrum
    // behind the curve, each band's shown rows.
    uint8_t edit_trig_seen_[kTracks] = {};
    uint32_t edit_trig_ms_[kTracks] = {};
    bool edit_trig_live_[kTracks] = {};
    uint32_t edit_trig_checked_ms_ = 0;
    // WAVE's waterfall, drawn once a table (app/ui_editview.hpp, build_waterfall): each pixel's
    // grey at full brightness and the frame that drew it, and the table they are of.
    uint8_t wf_cover_[66 * 124] = {};
    uint8_t wf_frame_[66 * 124] = {};
    int wf_table_ = -1;
    uint8_t flt_band_rows_[spectrum::kBands] = {};
    // MIXER's master pages: the FFT's scratch, each band's shown rows, its peak dot and how long
    // the dot holds, and the vectorscope's afterglow.
    spectrum::Work spectrum_work_;
    uint8_t band_rows_[spectrum::kBands] = {};
    uint8_t band_peak_[spectrum::kBands] = {};
    uint8_t band_hold_[spectrum::kBands] = {};
    // MIXER (app/ui_mixview.hpp): the desk's held peaks and when each was taken, MST's
    // vectorscope afterglow, and the loudness meter's state (K filters, mean square, the
    // history's count last read, the last reading in tenths).
    uint8_t desk_hold_[kTracks] = {};
    uint32_t desk_hold_ms_[kTracks] = {};
    paint::Glow<40, 40> master_glow_;
    float loud_z_[2][4] = {};
    float loud_ms_ = 0.0f;
    uint32_t loud_seen_ = 0;
    int loud_tenths_ = -999;
    // PROJECT's slots (app/ui_pickview.hpp): the shown bank's patterns as sixteen columns of
    // track bits; every bank's "holds a pattern" bits and which bank is read next; the frames
    // drawn and the last one the slots drew, so entering the screen reads every bank.
    uint8_t slot_thumb_[kPatternsPerBank][16] = {};
    // RESAMPLE (app/ui_listview.hpp): the source's level over the last 120 frames, rolling, and
    // the take as it was recorded, a column a 120th of it, in rows of the picture's half height.
    uint8_t level_history_[120] = {};
    uint8_t take_wave_[120] = {};
    int take_wave_cols_ = 0;
    uint8_t take_seen_state_ = 0;
    uint32_t history_level_ = 0;       // the reading the history last rolled on
    // KITS: each machine's picture drawn once (build_kit_icons), and the strip it is drawn in.
    static constexpr int kIconStripRows = 24;
    uint8_t icon_strip_[128 * kIconStripRows] = {};
    uint8_t kit_icons_[int(MachineId::count)][14 * 8] = {};
    bool kit_icons_built_ = false;
    // CHAIN: the cursor entry's pattern as PROJECT's tiles draw one, and which pattern it is.
    uint8_t chain_thumb_[16] = {};
    int chain_thumb_key_ = -1;
    uint16_t bank_used_[kBanks] = {};
    int used_next_ = 0;
    uint32_t draw_frame_ = 0;
    uint32_t slots_frame_ = 0;
    // draw()'s own cost (count_draw_time): the window's sum, its frames, the last average.
    uint32_t draw_ms_sum_ = 0;
    uint32_t draw_count_ = 0;
    uint32_t draw_hundredths_ = 0;
    bool draw_logged_ = false;
    uint8_t logged_guard_ = 0;     // the CPU guard level last logged
    uint32_t cpu_max_ = 0;         // per mille, the highest block cost since the last start
    static constexpr uint8_t kLoadBurst = 4;       // `ld` lines (bars) after a freeze or unfreeze
    uint8_t load_skip_ = 0;        // bar lines to pass before the burst's first bar
    uint32_t load_bar_ = 0;        // the bar the last poll was in
    uint8_t load_burst_ = 0;       // `ld` lines still to log in this burst
    uint8_t load_frozen_ = 0;      // the frozen mask, last seen
    uint32_t window_sum_ = 0;      // per mille, summed over the current bar
    uint32_t window_max_ = 0;
    uint32_t window_polls_ = 0;

    LedShadow leds_;
    uint32_t tempo_tenths_ = 1300;

    bool logged_running_ = false;
    int logged_source_ = -1;
    uint32_t logged_position_ = 0xFFFFFFFFu;
    uint32_t logged_mask_ = 0xFFFFFFFFu;   // deliberately impossible, so the first one logs
    uint8_t logged_undo_ = 0xFFu;
};

}  // namespace
