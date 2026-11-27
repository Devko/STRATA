// The FUNC help: what the keys and knobs do on the screen that is up.
//
// docs/CONTROLS.md, "FUNC help". FUNC held alone for 0.6 s shows help over the current screen.
// It used to be one page, the same everywhere: FUNC + each function key and the upper pads.
// Roland (2026-11-23): "if I am in a specific module I get the help for this module". So the
// help now starts on the screen's own page -- which knob does what there and the gestures that
// work there -- and the screen keys (or LEFT/RIGHT), pressed while FUNC is still held, turn to
// its further pages. The last five pages are the same on every screen: FUNC + a function key,
// FUNC + a knob or pad, and the upper pads, which is what the one page used to be, then the
// performance mixer on VALUE (core/perffx.hpp): its gestures and its sixteen slots.
//
// Plain text and plain data, so it lives in core/ and is tested on the host without the app:
// tests/test_help.cpp holds every row of every page of every screen to the font and the line
// (21 characters of " !#+-./0-9:=?A-Z%>"), and tests/test_ui.cpp draws each one through the
// layout check. The app tells it where it is (HelpPlace) and draws the rows it writes.
#pragma once

#include <cstdint>

namespace strata {

// Which screen the help is for. EDIT's seven pages and MIXER's twelve are told apart by
// HelpPlace::page rather than by a screen each.
enum class HelpScreen : uint8_t {
    grid,
    step,         // the step page (hold STEP + OK), over GRID
    edit,
    learn,        // EDIT while MACRO LEARN is on: the pushes are the macro's
    keys,
    mixer,
    perform,
    project,      // PROJECT's slots
    chain,
    pattern,
    setup,
    projects,
    machine,
    kit,
    samples,
    sounds,
    resample,
    menu,
    count
};

constexpr int kHelpEditPages = 8;      // SRC FLTR AMP FX LFO ARP ASR OSC, as app/app.hpp's kEditPages
constexpr int kHelpMixerPages = 15;    // as app/app.hpp's kMixerPageCount
constexpr int kHelpRows = 10;          // rows under the title, above the page line
constexpr int kHelpChars = 21;         // one line of Enaga's font on the 128-pixel screen

struct HelpPlace {
    HelpScreen screen = HelpScreen::grid;
    // The EDIT page A-D turn (GRID, EDIT, LEARN, KEYS), 0-6; or the MIXER page, 0-11.
    uint8_t page = 0;
    uint8_t machine = 0;                 // MachineId of the selected track
    // The short names of the four knobs on that EDIT page, exactly as the screen shows them
    // (a machine's own names on SRC and AMP, the chain's on FLTR and FX). Null reads "---".
    const char *knob[4] = {nullptr, nullptr, nullptr, nullptr};
};

// How many pages the help has on this screen: its own, then the five shared ones.
int help_page_count(const HelpPlace &place);

// Page `page` (0 = the screen's own; wraps): its title into `title`, its rows into `rows`.
// Returns how many rows it used (0-kHelpRows). Every string is at most kHelpChars long and
// terminated. A row that ends in ':' is a heading.
int help_page(const HelpPlace &place, int page, char title[kHelpChars + 1],
              char rows[kHelpRows][kHelpChars + 1]);

// What a knob does, in a few words: "FM AMOUNT" for KCK's FMA. At most 15 characters, so that
// "A FMA FM AMOUNT" fits a line. `index` is the track parameter, 0-27 (dsp/track.hpp).
const char *help_param_words(uint8_t machine, int index);

// How many rows have been cut to kHelpChars so far. Always 0 unless a row is too long: the
// tests fail on any.
int help_cut_rows();

}  // namespace strata
