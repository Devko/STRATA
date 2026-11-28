// The FUNC help's pages. See help.hpp.
//
// Every gesture named here is one the key handlers in app/app.hpp do; docs/CHEATSHEET.md is the
// long form of the same list. When a gesture changes, change it in docs/CONTROLS.md, the code,
// the cheatsheet and here together (docs/CONTROLS.md says so).
//
// Wording: the panel's printed names (FUNC, EDIT, the screen keys, the pad-row keys), the key
// on the left and what it does on the right, one gesture a row. The font has no lower case, no
// comma and no brackets, so "FUNC+STEP" and "OK  LOAD" rather than prose.
#include "help.hpp"

#include "project.hpp"
#include "tuning.hpp"

namespace strata {

namespace {

constexpr int kParamsPerHelpPage = 4;   // dsp/track.hpp's kParamsPerPage; core cannot include it
constexpr int kTrackParamCount = kHelpEditPages * kParamsPerHelpPage;

struct Text {
    const char *title;
    const char *rows[kHelpRows];
};

// --- the shared pages: the last five on every screen -------------------------------------------

constexpr Text kFuncKeys = {"FUNC + KEY:", {
    "REC    RESAMPLE  JAM",
    "PROJ   SAMPLES",
    "HOME   MENU",
    "PLAY   RESTART",
    "STOP   RESET",
    "MIXER  PERFORM",
    "EDIT   MACHINE  KIT",
    "OK/CLR REDO/UNDO",
    "UP/DN  COPY/PASTE",
    "PAD-ROW KEY  LENGTH",
}};

constexpr Text kFuncKnobs = {"FUNC + KNOB OR PAD:", {
    "PUSH A-C   KILL L/M/H",
    "LOWER PAD  BAR MUTE",
    "STEP       TRIGLESS",
    "TURN A-C  EUCLID GRID",
    "WITHOUT FUNC:",
    "VALUE      TEMPO",
    "PUSH+TURN VALUE SWING",
    "LOWER PAD  TRACK",
    "HOLD PROJECT+STEP",
    "  QUEUE THAT SLOT",
}};

constexpr Text kUpperPads = {"UPPER PADS TRK1-7:", {
    "1 FILL  FUNC NOTE RPT",
    "2 REPEAT FUNC PITCH",
    "3 MUTE MODE",
    "4 SCENE A +STEP PICKS",
    "5 SCENE B +STEP PICKS",
    "6 THROW  FUNC REVERB",
    "7 MUTATE  OK/CLR",
    "FUNC+7 HOLD  LOCK",
    "FUNC+7 TAP JUMP/BACK",
    "KEYS: PADS = PIANO",
}};

// "LOWER PAD  BAR MUTE" is 22 characters: the table above is held to 21 by the test, so
// rows that need the room are spelled out here instead of trimmed by eye.

// The performance mixer (core/perffx.hpp): VALUE as a crossfader into one of sixteen effects.
constexpr Text kValueMixer = {"VALUE FX MIXER:", {
    "PUSH VALUE+STEP  PICK",
    "  SAME STEP  LETS GO",
    "  HOLD STEP  PUSH IN",
    "VALUE  AMOUNT 0 = DRY",
    "TAP VALUE  RIDE TO 0",
    "FUNC+TAP VALUE  CUT",
    "PUSH+LEFT/RIGHT  NEXT",
    "PUSH+CLR  ALL OFF",
    "  AGAIN  CLEARS LOOP",
    "MENU FX GLIDE  RIDE",
}};

constexpr Text kValueSlots = {"VALUE FX STEPS:", {
    "1 LP SWEEP  9 BASS",
    "2 HP SWEEP 10 PUMP",
    "3 HP RISER 11 CRUSH",
    "4 ECHO     12 NOISE",
    "5 FREEZE   13 MORPH",
    "6 WASH     14 GATE",
    "7 MUTE BUS 15 BUILD",
    "8 TAPE     16 LOOPER",
    "16 RECORDS AGAIN OFF",
    "MENU LOOP LEN  BARS",
}};

constexpr const Text *kShared[] = {&kFuncKeys, &kFuncKnobs, &kUpperPads, &kValueMixer,
                                   &kValueSlots};
constexpr int kSharedPages = int(sizeof kShared / sizeof kShared[0]);

// --- step editing, shared by GRID and EDIT ------------------------------------------------------

constexpr Text kSteps = {"STEPS:", {
    "TAP STEP  TRIG ON/OFF",
    "STEP+KNB/PSH LOCK/OFF",
    "  +FUNC RANDOM/SLIDE",
    "STEP+VALUE NOTE",
    "STEP+OK    STEP PAGE",
    "STEP+LATER STEP  LEN",
    "STEP+CLR   CLEAR STEP",
    "STEP+UP/DN COPY/PASTE",
    "TWO STEPS  RANGE",
    "PAD-ROW KEYS STEP PG",
}};

// D6: STEP first edits the step, pad first performs (core/stepedit.hpp's cycles).
constexpr Text kStepPads = {"HOLD STEP + PAD:", {
    "FILL  FILL/NOT/NONE",
    "REPEAT RATCHET 2 3 4",
    "MUTE  1:2 2:2 1:4 4:4",
    "MUTATE PROB 75 50 25",
    "THROW DLY FULL/OFF",
    "FUNC+THROW REV",
    "AGAIN  NEXT THEN OFF",
    nullptr, nullptr, nullptr,
}};

// And the other order: a pad held first performs with the STEP.
constexpr Text kPadFirst = {"HOLD PAD + STEP:", {
    "FILL+STEP  FILL TRIG",
    "SCENE+STEP PICK",
    "REPEAT+STEP STUTTER",
    "REPEAT+LEFT/RT RATE",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
}};

constexpr Text kRange = {"RANGE AND TRACK:", {
    "RANGE+CLR  CLEAR",
    "RANGE+A    TRANSPOSE",
    "RANGE+B    NUDGE",
    "CLR       CLEAR TRACK",
    "FUNC+CLR   UNDO",
    "FUNC+UP/DN COPY/PASTE",
    "REC+KNOB   MOTION REC",
    "PAD-ROW+FUNC  LENGTH",
    "CLEAR PATTERN:",
    "FUNC+HOME  OK TWICE",
}};

constexpr Text kLights = {"STEP LIGHTS:", {
    "GREEN      TRIG",
    "BRIGHTER   LOUDER",
    "BLINK      COND/PROB",
    "AMBER      HAS LOCKS",
    "RED        TRIGLESS",
    "BRIGHT RED PLAYHEAD",
    // GRID's cells (docs/CONTROLS.md, "Locks on screen"): what a trig carries, cut into it.
    "CELL DOT   LOCKS",
    "CUT TOP-L  CONDITION",
    "CUT TOP-R  PROB",
    "CUT TEETH  RATCHET",
}};

// --- the screens' own pages --------------------------------------------------------------------

constexpr Text kEditKeys = {"EDIT KEYS:", {
    "LEFT/RIGHT PAGE",
    "UP/DOWN    TRACK",
    "OK  SAMPLES OR SOUNDS",
    "FUNC+EDIT  MACHINE",
    "STEP+PUSH  UNLOCK",
    "MACRO LEARN:",
    "PERFORM  PUSH A-D",
    "HELD BY MACRO:",
    "MACRO UP HOLDS IT",
    "MACRO AT 0 LETS GO",
}};

constexpr Text kStepPage = {"STEP PAGE:", {
    "A-D        FIELDS",
    "SCREEN KEYS  1/2/3",
    "1 NOTE VEL LEN MICR",
    "2 PROB COND RTCH RAMP",
    "3 ACNT SLID CHRD",
    "LEFT/RIGHT STEP -/+",
    "OK OR CLR  CLOSE",
    "OPEN  HOLD STEP+OK",
    nullptr, nullptr,
}};

constexpr Text kStepFields = {"STEP FIELDS:", {
    "VEL  VELOCITY",
    "LEN  1/16 TO 64 STP",
    "MICR MICRO-TIMING",
    "PROB PROBABILITY",
    "COND CONDITION",
    "RTCH RATCHETS",
    "RAMP RATCHET RAMP",
    "ACNT ACCENT",
    "SLID SLIDE",
    "CHRD CHORD",
}};

constexpr Text kLearn = {"MACRO LEARN:", {
    "TURN KNOBS TO WHERE",
    "THE MACRO TOP GOES",
    "PUSH  TAKES ONE OFF",
    "ANY PAGE OR TRACK",
    "LEFT/RIGHT PAGE",
    "UP/DOWN    TRACK",
    "MIXER EQ  LOW MID HI",
    "OK/CLR END KNOBS BACK",
    "START  PERFORM PUSH",
    "THE MACRO KNOB A-D",
}};

constexpr Text kKeysPads = {"KEYS: PADS = PIANO", {
    "LOWER PADS WHITE KEYS",
    "UPPER PADS BLACK KEYS",
    "UPPER 3/7  OCTAVE -/+",
    "PRESSURE   MACRO A",
    "STEP KEYS  STEPS",
    "LEFT/RIGHT OCTAVE",
    "UP/DOWN    TRACK",
    "SCREEN KEYS EDIT PAGE",
    "FUNC+PAD   USUAL JOB",
    "HOME TWICE  KEYS",
}};

constexpr Text kKeysLights = {"KEYS LIGHTS:", {
    "ROOT       BRIGHT",
    "IN SCALE   DIM",
    "HELD       WHITE",
    "OCTAVE PADS DIM WHITE",
    "SUS UP ON A VA:",
    "A HELD PAD HOLDS",
    "REC  HOLD BECOMES LEN",
    "POLY CHORDS ALL KEPT",
    "MENU REC MODE: ADD",
    nullptr,
}};

constexpr Text kPerform = {"PERFORM:", {
    "A-D        MACROS",
    "PUSH A-D   LEARN",
    "VALUE      A/B MORPH",
    "LEFT/RIGHT MACRO",
    "LEARN  TURN THE KNOBS",
    "LEARN EQ  ON MIXER EQ",
    "OK         CAPTURE A",
    "CLR       CLEAR MACRO",
    "UP/DOWN   MUTATE TYPE",
    "STEP KEYS  STEPS",
}};

constexpr Text kPerform2 = {"PERFORM MORE:", {
    "SCREEN KEYS  AMOUNT",
    "SCENE+OK   TRACK ONLY",
    "SCENE+CLR  ERASE",
    "MACRO AT 0 LETS GO",
    "4 PARAMS A MACRO",
    "PAD PRESSURE  MACRO A",
    "MUTATE PAD  TRY IT",
    "OK KEEPS CLR REVERTS",
    "AUTO-FILL  FUNC MENU",
    "FUNC+A/B REL/LEN ALL",
}};

constexpr Text kPerform3 = {"PERFORM LIVE:", {
    "FUNC+A     REL ALL",
    "FUNC+B     LEN ALL",
    "FUNC+C     ENERGY",
    "  - THINS  + FILLS",
    "FUNC+D     LIVE RND",
    "  OFF 1 2 4 8 16",
    "FUNC+PUSH D  TRACK",
    "  IN OR OUT OF RND",
    "PATTERN NOT CHANGED",
    "LOCK HOLDS THEM ALL",
}};

constexpr Text kProject = {"PROJECT:", {
    "STEP 1-16  QUEUE SLOT",
    "ARROWS SLOT  OK QUEUE",
    "HOLD OK  PATTERN MENU",
    "CLR        CANCEL",
    "PAD-ROW KEYS  BANK",
    "A  QUEUE MODE",
    "B  KEY     C  SCALE",
    "SCREEN KEYS  PAGES",
    "SAVE  FUNC+HOME OK",
    "CHAIN ON  FUNC MENU",
}};

constexpr Text kChain = {"CHAIN:", {
    "UP/DOWN    ENTRY",
    "LEFT/RIGHT PATTERN",
    "STEP       PUT SLOT",
    "A OR VALUE REPEATS",
    "B          MUTE STATE",
    "OK         ADD ENTRY",
    "CLR TWICE  REMOVE",
    "PAD-ROW KEYS  BANK",
    "ON/OFF+END FUNC MENU",
    "PUSH A     SECTION",
}};

constexpr Text kSection = {"SECTION:", {
    "A-D   THE PAGE FIELDS",
    "PUSH  FIELD TO --",
    "SCREEN KEYS SECT/MOVE",
    "UP/DOWN    ENTRY",
    "HOLD OK FILL FROM NOW",
    "OK OR CLR  BACK",
    "MUT --/M1-M8/CAP",
    "SCN  SCENE ON SIDE B",
    "MRP SNAP/RIDE/BACK",
    "NRG ENERGY  BPM TEMPO",
}};

constexpr Text kLaunch = {"LAUNCH:", {
    "STEP     LAUNCH LANE",
    "PADS+STEP  SEVERAL",
    "PAD      PICK LANE",
    "UP/DOWN  LANE",
    "OK       CURSOR SLOT",
    "HOLD OK  CAPTURE",
    "CLR      OWN PART",
    "PUSH B   PIN LANE",
    "FUNC+STEP  PATTERN",
    "A        QUEUE MODE",
}};

constexpr Text kPattern = {"PATTERN:", {
    "LEFT/RIGHT FIELD",
    "UP/DOWN    CHANGE",
    "VALUE      CHANGE",
    "A  LEN  MASTER LENGTH",
    "B  SWG  SWING",
    "C  TRACK SWING",
    "PAT  FOLLOWS SWG",
    "D  FEEL DRIFT/WAVE",
    "CLR        BACK",
    nullptr,
}};

constexpr Text kSetup = {"SETUP:", {
    "A          QUANTIZE",
    "CLOCK AND MIDI:",
    "FUNC+POWER ENAGA MENU",
    "CLOCK SRC  MIDI CH",
    "CLR        BACK",
    nullptr, nullptr, nullptr, nullptr, nullptr,
}};

constexpr Text kProjects = {"PROJECTS:", {
    "UP/DOWN/A  MOVE",
    "LEFT/RIGHT PAGE",
    "OK TWICE   OPEN",
    "PUSH B     RENAME",
    "PUSH C     DELETE",
    "  THEN OK TWICE",
    "FUNC+DOWN  SAVE AS",
    "FUNC+UP    READ AGAIN",
    "CLR        BACK",
    nullptr,
}};

constexpr Text kNaming = {"NAMING:", {
    "A/LEFT/RIGHT  MOVE",
    "B/VALUE/UP/DN LETTER",
    "OK         SAVE",
    "CLR        CANCEL",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
}};

constexpr Text kMachine = {"MACHINE:", {
    "UP/DOWN/A  MACHINE",
    "B         MIDI OUT CH",
    "LEFT/RIGHT TRACK",
    "SCREEN KEYS TRACK",
    "OK  SAMPLES OR SOUNDS",
    "FUNC+EDIT  KIT",
    nullptr, nullptr, nullptr, nullptr,
}};

constexpr Text kKit = {"KIT:", {
    "UP/DOWN/A  SLOT",
    "STEP 1-16  SLOT",
    "LEFT/RIGHT 8 SLOTS",
    "OK TWICE   RECALL",
    "FUNC+DOWN  SAVE HERE",
    "FUNC+UP    READ AGAIN",
    "CLR        MACHINE",
    "SHARED BY ALL",
    "PROJECTS ON THE CARD",
    nullptr,
}};

constexpr Text kSamples = {"SAMPLES:", {
    "UP/DOWN/A  ENTRY",
    "LEFT/RIGHT 8 ENTRIES",
    "OK  LOAD OR OPEN",
    "PUSH A  HEAR FIRST",
    "WHILE HEARD: UP/DOWN",
    "HEARS THE NEXT",
    "CLR       UP A FOLDER",
    "FUNC+UP    READ AGAIN",
    "POOL  SMPLTREK POOL",
    "SMPLTREK  ALL FOLDERS",
}};

constexpr Text kSounds = {"SOUNDS:", {
    "UP/DOWN/A  SOUND",
    "LEFT/RIGHT FIRST/LAST",
    "SCREEN KEYS TRACK",
    "OK  LOAD INTO TRACK",
    "AS WAS  YOURS BACK",
    "CLR        BACK",
    "ONE TRACK ONLY",
    nullptr, nullptr, nullptr,
}};

constexpr Text kResample = {"RESAMPLE:", {
    "A OR UP/DN BARS",
    "B          SOURCE",
    "C         INPUT LEVEL",
    "LEFT/RIGHT FILE/TRACK",
    "OK         ARM",
    "CLR        CANCEL",
    "FUNC+REC   JAM REC",
    "MIC NEEDS HEADPHONES",
    "DONE: OK PUTS ON TRK",
    "DONE: CLR KEEPS FILE",
}};

constexpr Text kMenu = {"FUNC MENU:", {
    "UP/DOWN/A  ROW",
    "OK         RUN",
    "LEFT/RIGHT/B CHANGE",
    "CLR/FUNC+HOME CLOSE",
    "OPENS ON SAVE:",
    "FUNC+HOME OK  SAVES",
    "NEW RELOAD CLEAR",
    "PATTERN  OK TWICE",
    "TRACK ROWS: HOLD OK",
    "FUNC+REC TWICE  JAM",
}};

// The MIXER's pages, in app/app.hpp's kMixerPages order (its static_assert holds the count).
constexpr Text kMixer[kHelpMixerPages] = {
    {"MIXER TRK 1-4:", {"A-D  TRACK 1-4 LEVEL", "PUSH  CHOKE GROUP", "OFF 1-4: A NOTE CUTS",
                        "ITS GROUP", nullptr}},
    {"MIXER TRK 5-8:", {"A-D  TRACK 5-8 LEVEL", "PUSH  CHOKE GROUP", "OFF 1-4: A NOTE CUTS",
                        "ITS GROUP", nullptr}},
    {"MIXER DCK 1-4:", {"A-D  DUCK DEPTH 1-4", "OFF TO 100%", "THE SOURCE NEVER",
                        "DUCKS  SET ON DUCK", nullptr}},
    {"MIXER DCK 5-8:", {"A-D  DUCK DEPTH 5-8", "OFF TO 100%", "THE SOURCE NEVER",
                        "DUCKS  SET ON DUCK", nullptr}},
    {"MIXER DUCK:", {"A SRC SOURCE TRACK", "B ATK ATTACK", "C HLD HOLD", "D REL RELEASE",
                     "SAVED IN THE KIT", nullptr}},
    {"MIXER WID 1-4:", {"A-D  WIDTH 1-4", "1-63 SPREAD VOICES", "64+  UNISON 2 VOICES",
                        "PUSH OFF SPREAD UNI", "PLY WPL FM4 ONLY", nullptr}},
    {"MIXER WID 5-8:", {"A-D  WIDTH 5-8", "1-63 SPREAD VOICES", "64+  UNISON 2 VOICES",
                        "PUSH OFF SPREAD UNI", "PLY WPL FM4 ONLY", nullptr}},
    {"MIXER DLY:", {"A TIM TIME", "B FBK FEEDBACK", "C TON TONE", "D LVL LEVEL",
                    "SEND  EDIT FX SNA", nullptr}},
    {"MIXER DLY+:", {"A TYP CLAS OR LONG", "B MUL LONG TIME X1-X8", "C WOW TAPE WOBBLE",
                     "D DIF DIFFUSION", "PUSH A  FLIPS TYP", nullptr}},
    {"MIXER REV:", {"A DEC DECAY", "B DMP DAMPING", "C HPF HIGH-PASS", "D LVL LEVEL",
                    "SEND  EDIT FX SNB", nullptr}},
    {"MIXER REV+:", {"A TYP ROOM OR HALL", "B SIZ SIZE", "C MOD SWEEP", "D SHM SHIMMER",
                     "PUSH A  FLIPS TYP", nullptr}},
    {"MIXER ROUTE:", {"A PNG PING-PONG", "B D>R DELAY TO REVERB", "C LCT LONG LOW CUT",
                      "D ---", nullptr}},
    {"MIXER CHO 1-4:", {"A-D  CHORUS SEND 1-4", "PUSH  OFF OR HALF", "RATE DEPTH ON CHO",
                        "SAVED IN THE KIT", nullptr}},
    {"MIXER CHO 5-8:", {"A-D  CHORUS SEND 5-8", "PUSH  OFF OR HALF", "RATE DEPTH ON CHO",
                        "SAVED IN THE KIT", nullptr}},
    {"MIXER CHO:", {"A RAT SWEEP RATE", "B DEP DEPTH", "C LVL LEVEL", "D C>R INTO REVERB",
                    "SENDS: CHO 1-4 5-8", nullptr}},
    {"MIXER MST:", {"A DJ  DJ FILTER", "B CMP COMPRESSOR", "C LIM LIMITER", "D LVL LEVEL",
                    nullptr}},
    {"MIXER EQ:", {"A LOW BELOW 250 HZ", "B MID MIDDLE", "C HI  ABOVE 2.5 KHZ", "D ---",
                   "PUSH A-C   KILL BAND", "MIDDLE = FLAT", "BOTTOM = KILL", nullptr}},
};

// Rows every MIXER page ends with, under its own.
constexpr const char *kMixerKeys[] = {"LEFT/RIGHT PAGE", "UP/DOWN    GROUP", "SCREEN KEYS PAGE",
                                      "STEP KEYS  TRIGS",
                                      "FUNC+MIXER PERFORM"};

// --- what each knob does ------------------------------------------------------------------------

// SRC and AMP belong to the machine (dsp/*.cpp, param_name): slots 0-3 and 8-11. Same order as
// the names there and as docs/CHEATSHEET.md's machine table.
constexpr const char *kMachineWords[int(MachineId::count)][8] = {
    {"TUNE", "FM AMOUNT", "FM DECAY", "CLICK", "DECAY", "TONE", "DIRT", "LEVEL"},           // KCK
    {"TUNE", "SNAPPY", "NOISE TONE", "CLAP MODE", "DECAY", "CLAP SPREAD", "BODY", "LEVEL"}, // SNR
    {"TUNE", "TONE", "NOISE", "DETUNE", "DECAY", "CHOKE TIME", "CHOKE GROUP", "LEVEL"},     // HAT
    {"TUNE", "FM RATIO", "FM INDEX", "INDEX DECAY", "DECAY", "PITCH SWEEP", "SWEEP TIME",
     "LEVEL"},                                                                               // PRC
    {"PITCH", "START", "LENGTH", "SLICES", "ATTACK", "DECAY", "PLAY MODE", "LEVEL"},       // SMP
    {"POSITION", "GRAIN SIZE", "DENSITY", "SPRAY", "PITCH", "GRAIN SHAPE", "DECAY",
     "LEVEL"},                                                                               // GRN
    {"TUNE", "FILTER MODE", "CUTOFF", "RESONANCE", "DECAY", "FILTER ENV", "GLIDE",
     "LEVEL"},                                                                               // MNO
    {"TUNE", "FILTER MODE", "CUTOFF", "RESONANCE", "DECAY", "FILTER ENV", "DETUNE",
     "LEVEL"},                                                                               // PLY
    {"SENDS CC 74", "SENDS CC 71", "SENDS CC 73", "SENDS CC 72", "SENDS CC 70", "SENDS CC 75",
     "SENDS CC 76", "SENDS CC 77"},                                                          // MID
    {"WAVETABLE", "POSITION", "CUTOFF LP24", "RESONANCE", "DECAY", "FILTER ENV", "ENV>POSITION",
     "LEVEL"},                                                                               // WMN
    {"WAVETABLE", "POSITION", "CUTOFF LP24", "RESONANCE", "DECAY", "FILTER ENV", "ENV>POSITION",
     "LEVEL"},                                                                               // WPL
    {"ALGORITHM 1-8", "MOD RATIO", "FM INDEX", "OP4 FEEDBACK", "DECAY", "INDEX ENV",
     "VEL>INDEX", "LEVEL"},                                                                  // FM4
};

// The rest are the same on every machine: the insert chain's FLTR and FX (dsp/chain.cpp), the
// LFO (dsp/lfo.cpp), the arpeggiator (core/arp.cpp) and the VAs' ASR and OSC (app/app.hpp).
constexpr const char *kFltrWords[4] = {"DRIVE", "CUTOFF", "RESONANCE", "FILTER MODE"};
constexpr const char *kVoiceFilterWords[4] = {"FILTER DECAY", "TONE LP-HP", "", ""};
constexpr const char *kFxWords[4] = {"LO-FI", "PAN", "SEND A DELAY", "SEND B REVERB"};
constexpr const char *kLfoWords[4] = {"SHAPE", "RATE", "DEPTH", "DESTINATION"};
constexpr const char *kArpWords[4] = {"MODE", "RATE", "OCTAVES", "GATE"};
constexpr const char *kAsrWords[4] = {"ATTACK", "SUSTAIN", "RELEASE", "FILTER SUSTAIN"};
constexpr const char *kOscWords[4] = {"SAW/SQR/PULSE", "OSC2 DET/SYNC", "SUB OSC", "DRIVE"};
constexpr const char *kWaveOscWords[4] = {"PHASE WARP", "DETUNE", "SUB OSC", "DRIVE"};
constexpr const char *kFmAsrWords[4] = {"ATTACK", "SUSTAIN", "RELEASE", "INDEX SUSTAIN"};
constexpr const char *kFmOscWords[4] = {"PAIR DETUNE", "RATIO FINE", "INDEX KEYTRACK",
                                        "CARRIER RATIO"};
// GRAIN's: the cloud's envelope, and the scan and pitch spray (dsp/grain.hpp).
constexpr const char *kGrainAsrWords[4] = {"ATTACK", "SUSTAIN", "RELEASE", ""};
// SAMPLE's: SUS and REL only, its attack being AMP's (dsp/sample.cpp).
constexpr const char *kSampleAsrWords[4] = {"", "SUSTAIN", "RELEASE", ""};
constexpr const char *kGrainOscWords[4] = {"SCAN SPEED", "PITCH SPRAY", "", ""};

const char *const kEditPageNames[kHelpEditPages] = {"SRC", "FLTR", "AMP", "FX",
                                                    "LFO", "ARP", "ENV", "OSC"};

// Does the machine read EDIT's page 6 (ASR) or 7 (OSC)?
bool reads_page(uint8_t machine, int page) {
    return page == 6 ? machine_reads_asr(machine) : machine_reads_osc(machine);
}

const char *osc_words(uint8_t machine, int slot) {
    if (!machine_reads_osc(machine)) return "";
    if (machine_is_fm(machine)) return kFmOscWords[slot];
    if (machine_is_grain(machine)) return kGrainOscWords[slot];
    if (!machine_is_wave(machine)) return kOscWords[slot];
    if (slot == 3 && machine == uint8_t(MachineId::wave_mono)) return "GLIDE";
    return kWaveOscWords[slot];
}

// --- writing a line -----------------------------------------------------------------------------

// Rows cut to fit, counted so that a test can fail on one instead of a row losing its end on
// the unit (help_cut_rows). Only ever written from the main context.
int g_cut_rows = 0;

struct Line {
    char *out;
    int used = 0;
    explicit Line(char *buffer) : out(buffer) { out[0] = 0; }
    Line &add(const char *text) {
        while (text != nullptr && *text != 0 && used < kHelpChars) out[used++] = *text++;
        if (text != nullptr && *text != 0) g_cut_rows++;
        out[used] = 0;
        return *this;
    }
    Line &pad_to(int column) {
        while (used < column && used < kHelpChars) out[used++] = ' ';
        out[used] = 0;
        return *this;
    }
};

void copy_text(const Text &text, char title[kHelpChars + 1], char rows[kHelpRows][kHelpChars + 1],
               int &count) {
    Line(title).add(text.title);
    for (int r = 0; r < kHelpRows && text.rows[r] != nullptr; r++) Line(rows[count++]).add(text.rows[r]);
}

void add_row(char rows[kHelpRows][kHelpChars + 1], int &count, const char *text) {
    if (count < kHelpRows) Line(rows[count++]).add(text);
}

// "A TUN TUNE": the knob, its name as the screen shows it, and what it does.
void knob_row(const HelpPlace &place, int slot, char row[kHelpChars + 1]) {
    const char *name = place.knob[slot] != nullptr ? place.knob[slot] : "---";
    Line line(row);
    const char letter[2] = {char('A' + slot), 0};
    line.add(letter).add(" ").add(name);
    if (name[0] == '-') return;          // an empty slot: nothing to say
    line.pad_to(5).add(" ").add(help_param_words(place.machine, edit_slot(place.machine, int(place.page), slot)));
}

// The four knobs of the EDIT page A-D turn, and what is special about the page: the pushes
// (NOTE view on TUN or PIT, SLC AUTO on SAMPLE's SLC, LFO TRIG on RAT) and how the page switches itself on.
void knob_rows(const HelpPlace &place, bool slc_auto, char rows[kHelpRows][kHelpChars + 1],
               int &count) {
    const int page = place.page < kHelpEditPages ? place.page : 0;
    if (page >= 6 && !reads_page(place.machine, page)) {
        add_row(rows, count, page == 6 ? "ASR: SYNTHS GRN SMP" : "OSC: VA WAVE FM GRN");
        return;
    }
    for (int slot = 0; slot < 4; slot++) knob_row(place, slot, rows[count++]);

    const int tune = tune_param(place.machine);
    if (tune >= page * kParamsPerHelpPage && tune < (page + 1) * kParamsPerHelpPage) {
        char row[kHelpChars + 1];
        Line line(row);
        const char letter[2] = {char('A' + tune - page * kParamsPerHelpPage), 0};
        line.add("PUSH ").add(letter).add("     NOTE VIEW");
        add_row(rows, count, row);
    }
    if (slc_auto && place.machine == uint8_t(MachineId::sample) && page == 0) {
        add_row(rows, count, "PUSH D     SLC AUTO");
        add_row(rows, count, "SLC 1      ONE-SHOT");
    }
    switch (page) {
        case 1: add_row(rows, count, "MOD  OFF LP BP HP"); break;
        case 3: add_row(rows, count, "SENDS  MIXER DLY/REV"); break;
        case 4:
            add_row(rows, count, "ON WITH DEP AND DST");
            add_row(rows, count, "PUSH B     TRIG MODE");
            add_row(rows, count, "FREE RETRIG ONE-SHOT");
            add_row(rows, count, "PUSH C D   MOD SLOTS");
            break;
        case 5:
            add_row(rows, count, "MOD OFF  NO ARP");
            add_row(rows, count, "PUSH A VAR  B RHYTHM");
            add_row(rows, count, "PUSH C ACC  D RATCHET");
            add_row(rows, count, "OK         ARPS");
            break;
        case 6:
            add_row(rows, count,
                    machine_is_grain(place.machine) || place.machine == uint8_t(MachineId::sample)
                        ? "SUS 0  DEC DECIDES"
                        : "SUS 0  ONE-SHOT");
            add_row(rows, count, "SUS UP  PAD/LEN HOLDS");
            break;
        case 7:
            if (machine_is_grain(place.machine)) {
                add_row(rows, count, "SCN 0  STILL  90 = 1X");
                add_row(rows, count, "PSP 1-63 DET 64+ OCT");
            } else if (machine_is_fm(place.machine)) {
                add_row(rows, count, "DET 0  NO DETUNE");
                add_row(rows, count, "BRT  DULLER UP HIGH");
            } else if (machine_is_wave(place.machine)) {
                add_row(rows, count, "WRP 0  NO BEND");
                add_row(rows, count, "DET 0  ONE OSC");
            } else {
                add_row(rows, count, "OS2 1-63 DETUNE");
                add_row(rows, count, "OS2 64+  SYNC");
            }
            break;
        default: break;
    }
    if (page == 0 && machine_is_wave(place.machine)) add_row(rows, count, "TBL 16 TABLES  8 EACH");
    if (page == 2 && machine_is_wave(place.machine)) add_row(rows, count, "MOT 64 OFF  +UP -DOWN");
    if (page == 0 && machine_is_fm(place.machine)) add_row(rows, count, "ALG 8 IS AN ORGAN");
    if (page == 2 && machine_is_fm(place.machine)) add_row(rows, count, "IEN 0  STEADY INDEX");
    if (page == 2 && machine_is_grain(place.machine)) add_row(rows, count, "DEC 127  HOLDS");
    if (page == 0 && machine_is_grain(place.machine)) add_row(rows, count, "DEN UNDER X1  GAPS");
    // EDIT's pictures (docs/CONTROLS.md, "Waveforms and envelopes"): what the marks are.
    if (page == 0 && place.machine == uint8_t(MachineId::sample)) {
        add_row(rows, count, "LINE STA END  DOT SLC");
    }
    if (page == 0 && machine_is_grain(place.machine)) add_row(rows, count, "BRACKET POS  TICK GRN");
    const bool curve = (page == 2 && place.machine != uint8_t(MachineId::midi)) ||
                       (page == 6 && machine_reads_asr(place.machine)) ||
                       (page == 1 && machine_has_voice_filter(place.machine));
    if (curve) add_row(rows, count, "CURVE  LETTER = KNOB");
}

// The title of a page that names the EDIT page: "EDIT SRC KCK".
void edit_title(const char *screen, const HelpPlace &place, char title[kHelpChars + 1]) {
    const int page = place.page < kHelpEditPages ? place.page : 0;
    Line(title).add(screen).add(" ").add(kEditPageNames[page]).add(" ").add(machine_name(place.machine));
}

// The screen's own pages, before the five shared ones. Indexed from 0; returns false past
// the last, so help_page_count can count them with the same code that writes them.
// GRID's and KEYS' track menu (app/ui_trackmenu.hpp): GEN, EVOLVE and the track's settings.
constexpr Text kTrackMenu = {"HOLD OK: TRACK MENU", {
    "GENERATE  NEW PART",
    "DENS RANGE LEAP WHAT",
    "EVOLVE 1 2 4 8 LOOPS",
    "ORBIT DRIFT KEEP HOME",
    "SPEED  2X TO 1/8",
    "PLAY FWD REV PING RND",
    "FREEZE  LOOP IN RAM",
    "RELOAD SAMPLE",
    "UP/DN A ROW  LR B SET",
    "OK RUN  CLR CLOSE",
}};

bool own_page(const HelpPlace &place, int page, char title[kHelpChars + 1],
              char rows[kHelpRows][kHelpChars + 1], int &count) {
    const Text *list[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int listed = 0;
    // A generated first page (the knobs) takes page 0; the Texts follow.
    bool knobs = false;
    switch (place.screen) {
        case HelpScreen::grid:
            knobs = true;
            list[listed++] = &kSteps;
            list[listed++] = &kStepPads;
            list[listed++] = &kPadFirst;
            list[listed++] = &kRange;
            list[listed++] = &kTrackMenu;
            list[listed++] = &kLights;
            break;
        case HelpScreen::edit:
            knobs = true;
            list[listed++] = &kEditKeys;
            list[listed++] = &kSteps;
            list[listed++] = &kStepPads;
            list[listed++] = &kPadFirst;
            list[listed++] = &kRange;
            break;
        case HelpScreen::learn:
            list[listed++] = &kLearn;
            knobs = true;           // after LEARN's own page: see below
            break;
        case HelpScreen::keys:
            list[listed++] = &kKeysPads;
            knobs = true;
            list[listed++] = &kKeysLights;
            list[listed++] = &kTrackMenu;
            break;
        case HelpScreen::step:
            list[listed++] = &kStepPage;
            list[listed++] = &kStepFields;
            break;
        case HelpScreen::mixer:     knobs = true; break;
        case HelpScreen::perform:
            list[listed++] = &kPerform;
            list[listed++] = &kPerform2;
            list[listed++] = &kPerform3;
            break;
        case HelpScreen::project:   list[listed++] = &kProject; break;
        case HelpScreen::launch:    list[listed++] = &kLaunch; break;
        case HelpScreen::chain:     list[listed++] = &kChain; break;
        case HelpScreen::section:   list[listed++] = &kSection; break;
        case HelpScreen::pattern:   list[listed++] = &kPattern; break;
        case HelpScreen::setup:     list[listed++] = &kSetup; break;
        case HelpScreen::projects:  list[listed++] = &kProjects; list[listed++] = &kNaming; break;
        case HelpScreen::machine:   list[listed++] = &kMachine; break;
        case HelpScreen::kit:       list[listed++] = &kKit; break;
        case HelpScreen::samples:   list[listed++] = &kSamples; break;
        case HelpScreen::sounds:    list[listed++] = &kSounds; break;
        case HelpScreen::resample:  list[listed++] = &kResample; break;
        case HelpScreen::menu:      list[listed++] = &kMenu; break;
        case HelpScreen::count:     break;
    }

    // Where the generated page sits: first on GRID, EDIT and MIXER (it IS the screen), second
    // on LEARN and KEYS, whose first page says what is different about them.
    const int knob_at = !knobs ? -1
                        : (place.screen == HelpScreen::learn || place.screen == HelpScreen::keys) ? 1
                                                                                                    : 0;
    if (page == knob_at) {
        if (title == nullptr) return true;
        if (place.screen == HelpScreen::mixer) {
            const int bank = place.page < kHelpMixerPages ? place.page : 0;
            copy_text(kMixer[bank], title, rows, count);
            for (const char *row : kMixerKeys) add_row(rows, count, row);
            return true;
        }
        switch (place.screen) {
            case HelpScreen::grid:
                edit_title("GRID", place, title);
                add_row(rows, count, "A-D  THE EDIT PAGE:");
                knob_rows(place, false, rows, count);
                add_row(rows, count, "SCREEN KEYS EDIT PAGE");
                add_row(rows, count, "UP/DOWN    TRACK");
                add_row(rows, count, "LEFT/RIGHT STEP PAGE");
                break;
            case HelpScreen::edit:
                edit_title("EDIT", place, title);
                knob_rows(place, true, rows, count);
                add_row(rows, count, "STEP+KNOB  LOCK");
                add_row(rows, count, "LEFT/RIGHT PAGE");
                break;
            case HelpScreen::learn:
                edit_title("LEARN", place, title);
                knob_rows(place, false, rows, count);
                add_row(rows, count, "PUSH  ON OR OFF MACRO");
                break;
            default:                        // KEYS
                edit_title("KEYS", place, title);
                add_row(rows, count, "A-D  THE EDIT PAGE:");
                knob_rows(place, false, rows, count);
                break;
        }
        return true;
    }
    const int text_index = knob_at >= 0 && page > knob_at ? page - 1 : page;
    if (text_index < 0 || text_index >= listed) return false;
    if (title != nullptr) copy_text(*list[text_index], title, rows, count);
    return true;
}

int own_page_count(const HelpPlace &place) {
    int pages = 0;
    char *no_title = nullptr;
    char unused[kHelpRows][kHelpChars + 1];
    int count = 0;
    while (pages < 8 && own_page(place, pages, no_title, unused, count)) pages++;
    return pages;
}

}  // namespace

const char *help_param_words(uint8_t machine, int index) {
    if (index < 0 || index >= kTrackParamCount) return "";
    const int page = index / kParamsPerHelpPage, slot = index % kParamsPerHelpPage;
    switch (page) {
        case 0:
        case 2:
            if (machine >= uint8_t(MachineId::count)) return "";
            return kMachineWords[machine][(page == 0 ? 0 : 4) + slot];
        case 1:
            // A VA's or WAVE's slots 4-7 are FDE and TON (D8); its FLTR page shows them with
            // CUT RES ENV (edit_slot), and the words follow the slot, not the page.
            if (machine_has_voice_filter(machine)) return kVoiceFilterWords[slot];
            return kFltrWords[slot];
        case 3: return kFxWords[slot];
        case 4: return kLfoWords[slot];
        case 5: return kArpWords[slot];
        case 6:
            if (!machine_reads_asr(machine)) return "";
            return machine_is_fm(machine)                    ? kFmAsrWords[slot]
                   : machine_is_grain(machine)               ? kGrainAsrWords[slot]
                   : machine == uint8_t(MachineId::sample)   ? kSampleAsrWords[slot]
                                                             : kAsrWords[slot];
        default: return osc_words(machine, slot);
    }
}

int help_cut_rows() { return g_cut_rows; }

int help_page_count(const HelpPlace &place) {
    return own_page_count(place) + kSharedPages;
}

int help_page(const HelpPlace &place, int page, char title[kHelpChars + 1],
              char rows[kHelpRows][kHelpChars + 1]) {
    const int total = help_page_count(place);
    page = ((page % total) + total) % total;
    for (int r = 0; r < kHelpRows; r++) rows[r][0] = 0;
    title[0] = 0;
    int count = 0;
    const int own = total - kSharedPages;
    if (page < own) {
        own_page(place, page, title, rows, count);
    } else {
        copy_text(*kShared[page - own], title, rows, count);
    }
    return count;
}

}  // namespace strata
