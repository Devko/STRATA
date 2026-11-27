// The FUNC help's pages (core/help.hpp): every row of every page of every screen fits the line
// and the font, and each screen's help says the things it is for.
#include "check.hpp"

#include <cstring>

#include "help.hpp"
#include "project.hpp"

using namespace strata;

namespace {

constexpr char kFont[] = " !#+-./0123456789:=?ABCDEFGHIJKLMNOPQRSTUVWXYZ%>";

bool in_font(const char *text) {
    for (; *text; text++) {
        if (std::strchr(kFont, *text) == nullptr) return false;
    }
    return true;
}

// Long enough names to stand for any machine's: the app passes the screen's own.
const char *const kNames[4] = {"TUN", "FMA", "FMD", "CLK"};

HelpPlace place_of(HelpScreen screen, int page, int machine) {
    HelpPlace place;
    place.screen = screen;
    place.page = uint8_t(page);
    place.machine = uint8_t(machine);
    for (int i = 0; i < 4; i++) place.knob[i] = kNames[i];
    return place;
}

int pages_of(HelpScreen screen) {
    return screen == HelpScreen::mixer ? kHelpMixerPages
           : (screen == HelpScreen::grid || screen == HelpScreen::edit ||
              screen == HelpScreen::learn || screen == HelpScreen::keys)
               ? kHelpEditPages
               : 1;
}

// Every row of every help page, through `visit(place, page, title, rows, count)`.
template <typename Visit>
void every_page(Visit visit) {
    for (int s = 0; s < int(HelpScreen::count); s++) {
        for (int sub = 0; sub < pages_of(HelpScreen(s)); sub++) {
            for (int m = 0; m < int(MachineId::count); m++) {
                const HelpPlace place = place_of(HelpScreen(s), sub, m);
                const int pages = help_page_count(place);
                for (int p = 0; p < pages; p++) {
                    char title[kHelpChars + 1];
                    char rows[kHelpRows][kHelpChars + 1];
                    const int count = help_page(place, p, title, rows);
                    visit(place, p, title, rows, count);
                }
            }
        }
    }
}

bool page_has(const HelpPlace &place, const char *text) {
    const int pages = help_page_count(place);
    for (int p = 0; p < pages; p++) {
        char title[kHelpChars + 1];
        char rows[kHelpRows][kHelpChars + 1];
        const int count = help_page(place, p, title, rows);
        if (std::strstr(title, text) != nullptr) return true;
        for (int r = 0; r < count; r++) {
            if (std::strstr(rows[r], text) != nullptr) return true;
        }
    }
    return false;
}

bool first_page_has(const HelpPlace &place, const char *text) {
    char title[kHelpChars + 1];
    char rows[kHelpRows][kHelpChars + 1];
    const int count = help_page(place, 0, title, rows);
    if (std::strstr(title, text) != nullptr) return true;
    for (int r = 0; r < count; r++) {
        if (std::strstr(rows[r], text) != nullptr) return true;
    }
    return false;
}

}  // namespace

TEST(help_every_row_fits_the_line_and_the_font) {
    const int cut_before = help_cut_rows();
    int bad = 0;
    every_page([&](const HelpPlace &, int, const char *title, char rows[][kHelpChars + 1],
                   int count) {
        if (title[0] == 0 || !in_font(title)) bad++;
        CHECK(count >= 1);
        CHECK(count <= kHelpRows);
        for (int r = 0; r < count; r++) {
            if (std::strlen(rows[r]) > size_t(kHelpChars) || rows[r][0] == 0 || !in_font(rows[r])) {
                std::printf("    bad help row: \"%s\" under \"%s\"\n", rows[r], title);
                bad++;
            }
        }
    });
    CHECK_EQ(bad, 0);
    CHECK_EQ(help_cut_rows() - cut_before, 0);       // nothing lost its end to the 21 columns
}

TEST(help_knob_words_fit_beside_a_three_letter_name) {
    // "A FMA " is six characters; what the knob does gets the other fifteen.
    for (int m = 0; m < int(MachineId::count); m++) {
        for (int i = 0; i < kHelpEditPages * 4; i++) {
            CHECK(std::strlen(help_param_words(uint8_t(m), i)) <= 15);
            CHECK(in_font(help_param_words(uint8_t(m), i)));
        }
    }
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::kick), 1), "FM AMOUNT") == 0);
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::sample), 3), "SLICES") == 0);
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::grain), 8), "PITCH") == 0);
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::va_poly), 24), "ATTACK") == 0);
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::kick), 24), "") == 0);  // no ASR
}

TEST(help_the_last_five_pages_are_the_same_everywhere) {
    // FUNC + a function key, FUNC + a knob or pad, the upper pads: what the one page was. Then
    // the performance mixer on VALUE: its gestures and its sixteen slots.
    every_page([&](const HelpPlace &place, int page, const char *title, char[][kHelpChars + 1],
                   int) {
        const int pages = help_page_count(place);
        if (page == pages - 5) CHECK(std::strcmp(title, "FUNC + KEY:") == 0);
        if (page == pages - 4) CHECK(std::strcmp(title, "FUNC + KNOB OR PAD:") == 0);
        if (page == pages - 3) CHECK(std::strcmp(title, "UPPER PADS TRK1-7:") == 0);
        if (page == pages - 2) CHECK(std::strcmp(title, "VALUE FX MIXER:") == 0);
        if (page == pages - 1) CHECK(std::strcmp(title, "VALUE FX STEPS:") == 0);
    });
    // And the page number wraps both ways, so the screen keys can go round.
    const HelpPlace grid = place_of(HelpScreen::grid, 0, 0);
    char a[kHelpChars + 1], b[kHelpChars + 1];
    char rows[kHelpRows][kHelpChars + 1];
    help_page(grid, -1, a, rows);
    help_page(grid, help_page_count(grid) - 1, b, rows);
    CHECK(std::strcmp(a, b) == 0);
    help_page(grid, help_page_count(grid), a, rows);
    help_page(grid, 0, b, rows);
    CHECK(std::strcmp(a, b) == 0);
}

TEST(help_starts_on_the_screens_own_page) {
    // The knobs of the EDIT page A-D turn, named as on screen and said in words.
    HelpPlace edit = place_of(HelpScreen::edit, 0, int(MachineId::kick));
    CHECK(first_page_has(edit, "EDIT SRC KCK"));
    CHECK(first_page_has(edit, "B FMA FM AMOUNT"));
    CHECK(first_page_has(edit, "PUSH A     NOTE VIEW"));           // TUN is A on SRC
    // GRAIN's PIT is on AMP, so NOTE view is pushed there; SAMPLE's SLC has SLC AUTO.
    CHECK(first_page_has(place_of(HelpScreen::edit, 2, int(MachineId::grain)), "PUSH A     NOTE VIEW"));
    CHECK(!first_page_has(place_of(HelpScreen::edit, 0, int(MachineId::grain)), "NOTE VIEW"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 0, int(MachineId::sample)), "SLC AUTO"));
    CHECK(!first_page_has(place_of(HelpScreen::grid, 0, int(MachineId::sample)), "SLC AUTO"));
    // ASR: what SUS does on a VA, and that it is a VA page elsewhere.
    CHECK(first_page_has(place_of(HelpScreen::edit, 6, int(MachineId::va_poly)), "SUS 0  ONE-SHOT"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 6, int(MachineId::kick)), "ASR: SYNTHS GRN SMP"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 6, int(MachineId::sample)), "SUS 0  DEC DECIDES"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 7, int(MachineId::sample)), "OSC: VA WAVE FM GRN"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 6, int(MachineId::grain)), "SUS 0  DEC DECIDES"));
    CHECK(first_page_has(place_of(HelpScreen::edit, 7, int(MachineId::grain)), "PSP 1-63 DET 64+ OCT"));
    // GRID: A-D are the EDIT page's, the screen keys turn it.
    const HelpPlace grid = place_of(HelpScreen::grid, 1, int(MachineId::snare));
    CHECK(first_page_has(grid, "GRID FLTR SNR"));
    CHECK(first_page_has(grid, "SCREEN KEYS EDIT PAGE"));
    // Each MIXER page its own knobs, the duck's included.
    CHECK(first_page_has(place_of(HelpScreen::mixer, 4, 0), "A SRC SOURCE TRACK"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 2, 0), "DUCK DEPTH 1-4"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 16, 0), "KILL BAND"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 5, 0), "64+  UNISON 2 VOICES"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 12, 0), "CHORUS SEND 1-4"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 14, 0), "D C>R INTO REVERB"));
    CHECK(first_page_has(place_of(HelpScreen::mixer, 8, 0), "PUSH A  FLIPS TYP"));
    // LEARN says what a push does now; KEYS that the pads are a piano.
    CHECK(first_page_has(place_of(HelpScreen::learn, 0, 0), "MACRO LEARN"));
    CHECK(first_page_has(place_of(HelpScreen::keys, 0, 0), "LOWER PADS WHITE KEYS"));
    CHECK(first_page_has(place_of(HelpScreen::step, 0, 0), "STEP PAGE"));
    CHECK(first_page_has(place_of(HelpScreen::perform, 0, 0), "PUSH A-D   LEARN"));
    CHECK(first_page_has(place_of(HelpScreen::projects, 0, 0), "PUSH B     RENAME"));
    CHECK(first_page_has(place_of(HelpScreen::projects, 0, 0), "PUSH C     DELETE"));
    CHECK(first_page_has(place_of(HelpScreen::sounds, 0, 0), "AS WAS"));
    CHECK(first_page_has(place_of(HelpScreen::menu, 0, 0), "FUNC MENU"));
}

TEST(help_covers_the_gestures_added_on_2026_11_23) {
    const HelpPlace grid = place_of(HelpScreen::grid, 0, 0);
    CHECK(page_has(grid, "STEP+LATER STEP  LEN"));          // note length from the STEP keys
    CHECK(page_has(grid, "STEP+OK    STEP PAGE"));
    CHECK(page_has(grid, "BRIGHTER   LOUDER"));             // velocity on the STEP lights
    CHECK(page_has(grid, "CLEAR PATTERN"));
    CHECK(page_has(grid, "PAD-ROW+FUNC  LENGTH"));
    CHECK(page_has(place_of(HelpScreen::edit, 0, 0), "MACRO LEARN"));
    CHECK(page_has(place_of(HelpScreen::keys, 0, 0), "UPPER 3/7  OCTAVE -/+"));
    CHECK(page_has(place_of(HelpScreen::keys, 0, 0), "ROOT       BRIGHT"));
    // D6: hold STEP + an upper pad, on GRID and EDIT.
    CHECK(page_has(grid, "MUTATE PROB 75 50 25"));
    CHECK(page_has(place_of(HelpScreen::edit, 0, 0), "REPEAT RATCHET 2 3 4"));
    // D12: the STEP keys are steps in KEYS and PERFORM too.
    CHECK(page_has(place_of(HelpScreen::keys, 0, 0), "STEP KEYS  STEPS"));
    CHECK(page_has(place_of(HelpScreen::perform, 0, 0), "STEP KEYS  STEPS"));
    // LIVE RND (core/liverand.hpp): FUNC + D in PERFORM, on its own page with REL/LEN ALL.
    CHECK(page_has(place_of(HelpScreen::perform, 0, 0), "FUNC+D     LIVE RND"));
    CHECK(page_has(place_of(HelpScreen::perform, 0, 0), "FUNC+PUSH D  TRACK"));
}

// 24 is the ASR page's first parameter, 28 the OSC page's (dsp/track.hpp).
TEST(help_names_fm4s_knobs) {
    const uint8_t fm = uint8_t(MachineId::fm);
    CHECK(std::strcmp(help_param_words(fm, 0), "ALGORITHM 1-8") == 0);
    CHECK(std::strcmp(help_param_words(fm, 9), "INDEX ENV") == 0);
    CHECK(std::strcmp(help_param_words(fm, 24 + 3), "INDEX SUSTAIN") == 0);
    CHECK(std::strcmp(help_param_words(fm, 28 + 0), "PAIR DETUNE") == 0);
    CHECK(std::strcmp(help_param_words(fm, 28 + 3), "CARRIER RATIO") == 0);
    // The VAs keep FSU as the filter's.
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::va_poly), 24 + 3), "FILTER SUSTAIN") == 0);
}

TEST(help_names_the_wave_pairs_knobs) {
    const uint8_t mono = uint8_t(MachineId::wave_mono), poly = uint8_t(MachineId::wave_poly);
    CHECK(std::strcmp(help_param_words(mono, 0), "WAVETABLE") == 0);
    CHECK(std::strcmp(help_param_words(mono, 10), "ENV>POSITION") == 0);
    CHECK(std::strcmp(help_param_words(mono, 28 + 0), "PHASE WARP") == 0);
    CHECK(std::strcmp(help_param_words(mono, 28 + 3), "GLIDE") == 0);
    CHECK(std::strcmp(help_param_words(poly, 28 + 3), "DRIVE") == 0);
    CHECK(std::strcmp(help_param_words(poly, 24), "ATTACK") == 0);
    // And the VAs keep theirs.
    CHECK(std::strcmp(help_param_words(uint8_t(MachineId::va_mono), 28 + 0), "SAW/SQR/PULSE") == 0);
}
