// A project: 8 banks of 16 patterns, the kit, the chains, and the files they live in.
//
// docs/SPEC.md, "Data model and storage". A project is one folder on the card and loads fully
// into RAM when it is opened:
//
//     PROJECT.BIN   tempo, root, scale, chains, mute states, the sample budget
//     KIT.BIN       per track: machine, 16 parameters, mixer level, sample path
//     BANK_A.BIN .. BANK_H.BIN   16 patterns each
//
// Three files rather than one because saving writes only what changed: a bank is about 130 KiB,
// which stalls the UI for tens of milliseconds and the audio for none, where one 1 MiB file
// would rewrite 127 untouched patterns every time a step is toggled.
//
// ## This is the live data, not a copy of it
//
// The sequencer plays a pattern *inside* this structure -- `live()` -- rather than a copy of it.
// So an edit lands in the pattern that will be saved, a pattern change is an index change with
// no 8 KiB copy in the audio path, and saving a bank reads exactly the memory the audio side
// plays from. The cost is that an undo history belongs to one pattern: `core/undo.hpp`'s
// `clear()` has to be called on a pattern change, which its own header already says.
//
// ## Why the kit is bytes here and not dsp/'s structs
//
// `core/` does not depend on `dsp/` (core/pattern.hpp says why), so this file cannot name
// `TrackParams`. The kit is therefore declared as what it is on disk -- 16 parameter bytes per
// track -- and `app/` copies between the two, with a static_assert there that the counts still
// agree. A format that followed a struct in another layer would change meaning the day that
// struct did.
//
// ## Where this runs
//
// Writing is a main-context operation over a caller-owned buffer, like everything in
// core/serialize.hpp, and never modifies the project. Reading fills a Project the caller owns.
// Nothing here allocates, and nothing here touches a file: `app/` is where this meets
// `enaga::files`.
#pragma once

#include "pattern.hpp"
#include "serialize.hpp"

#include <cstdint>

namespace strata {

// --- sizes ----------------------------------------------------------------

constexpr int kBanks = 8;                    // A..H
constexpr int kPatternsPerBank = 16;
constexpr int kProjectPatterns = kBanks * kPatternsPerBank;   // 128

// Parameters per track, mirroring dsp/track.hpp's kParams without depending on it. app/ asserts
// that the two agree.
constexpr int kKitParams = 16;

// How many machines a track can host, mirroring dsp/'s machine list the same way and for the
// same reason as kKitParams: docs/SPEC.md, "Machines (v1)" -- KICK, SNARE, HAT, PERC, SAMPLE,
// GRAIN, VA MONO and VA POLY. The file format needs the domain of KitTrack::machine, because a
// kit naming machine 12 has to become something this build can host rather than an index into
// app/'s machine pool. app/ asserts that the two agree.
constexpr int kMachines = 8;

// Which machine a track hosts, as the number KIT.BIN stores.
//
// **These ids are PERMANENT.** They are on disk the moment a kit is saved, so renumbering them
// would make every existing project play the wrong machines -- the same rule core/scale.hpp
// states for its scale ids, and for the same reason. New machines go on the END.
//
// The order is docs/SPEC.md's "Machines (v1)" list, read left to right, so a reader comparing
// the two sees the same sequence. A kit naming an id this build does not have plays NOTHING
// rather than being reinterpreted as an index into app/'s pool: a silent track is recoverable
// and a wrong pointer is not.
enum class MachineId : uint8_t {
    kick = 0,
    snare,
    hat,
    perc,
    sample,
    grain,
    va_mono,
    va_poly,
    count
};
static_assert(int(MachineId::count) == kMachines,
              "kMachines is the domain KIT.BIN stores: it has to match the id list exactly");

// The three-letter name for the screen, or "---" for an id this build does not have.
const char *machine_name(uint8_t id);

// A sample path, including its terminator. Enaga's paths are absolute and UTF-8
// ("/SAMPLES/Kick 01.wav"), and long names are supported, so this has to hold a real one rather
// than an 8.3 name.
constexpr int kSamplePathBytes = 112;

// docs/SPEC.md: "Chains: up to 64 entries per project, each a pattern plus a repeat count."
constexpr int kChainEntries = 64;

// --- the kit --------------------------------------------------------------

// One track's sound, as the project stores it.
struct KitTrack {
    uint8_t params[kKitParams] = {};
    uint8_t machine = 0;                        // which machine the track hosts
    float level = 1.0f;                         // the mixer fader
    char sample[kSamplePathBytes] = {};         // empty when the track plays no file
};

struct Kit {
    KitTrack tracks[kTracks];
};

// --- chains ---------------------------------------------------------------

// One entry of the chain: a pattern, played `repeats` times before the next entry.
//
// `repeats` of 0 means the entry is unused, which is also what zeroed memory means -- a chain
// of 64 zeroed entries is an empty chain rather than 64 infinite repeats of pattern A01.
struct ChainEntry {
    uint8_t bank = 0;
    uint8_t slot = 0;
    uint8_t repeats = 0;
};

// --- the project ----------------------------------------------------------

// The tempo's domain, in centi-BPM. Enaga's clock takes 20-300 BPM (docs/ENAGA_API.md), so a
// project cannot usefully hold anything else and a damaged byte is clamped into it. Here rather
// than in project.cpp because app/ sets the tempo from the same range: a UI that let the user
// past these would have the value quietly changed by the next load.
constexpr uint16_t kTempoCentiMin = 2000;     // 20.00 BPM
constexpr uint16_t kTempoCentiMax = 30000;    // 300.00 BPM

// How many send and master fields a file holds. Stated here rather than taken from
// dsp/buses.hpp's enums because core/ cannot include dsp/ -- dsp/buses.hpp reaches Enaga and
// CLAUDE.md says core/ never does. dsp/buses.hpp static_asserts that the two agree, so the day
// a field is added the build stops here rather than writing a short section.
constexpr int kSendFields = 10;
constexpr int kMasterFields = 4;

// The delay's default division: dotted 1/8, index 5 of dsp/buses.hpp's table, as the byte an
// encoder at that position produces. Named here because `ProjectSettings` needs it as an
// initialiser and a bare 64 next to nine other numbers says nothing.
constexpr uint8_t kDefaultDelayDivision = 64;

// Settings that belong to the project rather than to a pattern.
struct ProjectSettings {
    uint16_t tempo_centi_bpm = 13000;    // 130.00 BPM; Enaga's clock takes BPM x 100
    uint8_t root = 60;
    uint8_t scale = 0;
    uint8_t swing = uint8_t(kSwingMin);
    uint8_t quantize = 100;              // the live-record quantize amount, 0-100
    uint8_t mute_states[8] = {};         // 8 stored mute states, one bit per track

    // docs/SPEC.md: 32 MiB by default, raisable per project to the 64 MiB the budget allows.
    // Stored in MiB rather than bytes: a byte count would let a file ask for 4 GiB.
    uint8_t sample_budget_mib = 32;

    uint8_t chain_length = 0;            // how many chain entries are in use
    bool chain_enabled = false;

    // The two send effects and the master bus, as encoder bytes rather than as the floats the
    // mixer runs on. One byte per field, in dsp/buses.hpp's `SendField` and `MasterField`
    // order, which is why those two enums are declared permanent: these indices reach a file.
    //
    // Bytes, not floats, for three reasons. The encoder produces a byte, so a knob round-trips
    // through a save EXACTLY rather than coming back a quantisation step away. A byte cannot
    // hold a NaN or an infinity, so a damaged file cannot hand the mixer a coefficient that
    // poisons every later block. And the serializer has no float method, so storing one would
    // mean bit-casting through a buffer -- which is the thing CLAUDE.md forbids.
    //
    // dsp/buses.hpp owns the byte -> value mapping, because only it knows what each field
    // means; `delay_division` is not even a time until the tempo is known.
    // The bytes that reproduce dsp/buses.hpp's documented defaults through that mapping:
    // dotted 1/8, 40% feedback, mid tone, ping-pong on, full return, no delay into the reverb,
    // a 2 s reverb, mid damping, a 180 Hz input high-pass, full return.
    uint8_t sends[kSendFields] = {kDefaultDelayDivision, 53, 64, 127, 127, 0, 58, 64, 61, 127};
    // Centre (flat) DJ filter, the glue compressor at 0.3, the limiter at 0.8, unity out.
    uint8_t master[kMasterFields] = {64, 38, 101, 127};
};

struct Project {
    ProjectSettings settings;
    Kit kit;
    ChainEntry chain[kChainEntries];
    Pattern patterns[kBanks][kPatternsPerBank];

    // Which pattern is playing. The sequencer plays `live()`, in place.
    uint8_t bank = 0;
    uint8_t slot = 0;

    // Per bank, set by an edit and cleared by a save, so a save writes only what changed.
    // Not serialized: a freshly loaded project has nothing dirty.
    bool bank_dirty[kBanks] = {};

    Pattern &live() { return patterns[bank][slot]; }
    const Pattern &live() const { return patterns[bank][slot]; }
};

// About 1.03 MiB, which is a member of something long-lived and never a local: a stack frame is
// capped at 16 KiB (CLAUDE.md).
static_assert(sizeof(Pattern) * kProjectPatterns > 1000000,
              "if a Pattern shrank, the figures in docs/SPEC.md want revisiting");

// An empty project: every pattern cleared, the kit zeroed, no chain, bank A slot 1 live.
void project_init(Project &project);

// True when `bank` and `slot` name a pattern that exists. Used on every value read from a file
// and on every queued change, because an out-of-range index would read past the array.
//
// Inline, like `bank_letter` below, so that core/queue.cpp needs this HEADER and not
// core/project.cpp. The two modules are written independently and a link-time dependency
// between them would mean neither could be built until both were finished.
constexpr bool project_index_valid(int bank, int slot) {
    return bank >= 0 && bank < kBanks && slot >= 0 && slot < kPatternsPerBank;
}

// Bank letters for the screen and for file names: 'A'..'H'. '?' for an invalid index.
constexpr char bank_letter(int bank) {
    return bank >= 0 && bank < kBanks ? char('A' + bank) : '?';
}

// --- file names -----------------------------------------------------------

// Writes the bank's file name ("BANK_A.BIN") into `out`, NUL-terminated, at most `max` bytes
// including the terminator. False, with `out` untouched, for an invalid bank or too small a
// buffer. app/ joins it to the project folder; core/ does not know what a path is.
bool bank_file_name(int bank, char *out, int max);

// --- versions and sections ------------------------------------------------
//
// The container is core/serialize.hpp's (magic, version, section table, CRC-32 over the
// payload). Section ids are per FILE TYPE, so a project's ids and a pattern's ids are separate
// numbering spaces and these do not collide with kSectionPatternHeader and friends.
//
// Permanent, like the pattern's: a new section takes the next free number, and a section that
// goes away leaves its number burnt.

constexpr uint16_t kProjectVersion = 1;
constexpr uint16_t kSectionProjectSettings = 1;
constexpr uint16_t kSectionProjectChain = 2;
// The sends and the master. OPTIONAL on read: a PROJECT.BIN written before it existed has two
// sections and still loads, with the defaults above. That is what the format was built for --
// the section table is a lookup by id, so an older build reading a file with this section
// simply does not ask for it. The version therefore does NOT move: the meaning of no existing
// byte has changed, which is the only thing kProjectVersion tracks.
constexpr uint16_t kSectionProjectMixer = 3;

constexpr uint16_t kKitVersion = 1;
constexpr uint16_t kSectionKitTracks = 1;

constexpr uint16_t kBankVersion = 1;
constexpr uint16_t kSectionBankPatterns = 1;

// --- round trips ----------------------------------------------------------
//
// Each of the three returns and reports in the same style as `pattern_write` / `pattern_read`:
// a buffer the caller owns, a result enum, and the byte count. Loading is value-tolerant but
// structure-strict, exactly as docs/SPEC.md decided for patterns -- an out-of-range tempo or a
// chain entry naming a pattern that cannot exist is clamped or dropped, while a bad magic,
// an unknown version, a bad CRC or a malformed section table is refused.

// PROJECT.BIN: settings, the chain, and which pattern was live. `project_read` touches nothing
// else -- the patterns come from the bank files, and app/ reads this file first, so a damaged
// 277 bytes must not cost 128 patterns that are about to load.
uint32_t project_bytes();
SerializeResult project_write(const Project &project, uint8_t *buffer, uint32_t capacity,
                              uint32_t *written = nullptr);
SerializeResult project_read(Project &project, const uint8_t *buffer, uint32_t size);

// KIT.BIN: the eight tracks' sounds.
uint32_t kit_bytes();
SerializeResult kit_write(const Kit &kit, uint8_t *buffer, uint32_t capacity,
                          uint32_t *written = nullptr);
SerializeResult kit_read(Kit &kit, const uint8_t *buffer, uint32_t size);

// BANK_x.BIN: 16 patterns, each one `pattern_write`'s own container back to back, which is what
// `pattern_read`'s `consumed` was built for. A pattern that fails to read leaves that slot empty
// and the rest of the bank still loads -- one damaged pattern should cost one pattern.
//
// `kBankMaxBytes` sizes a static buffer that `bank_write` can never refuse.
constexpr uint32_t kBankMaxBytes =
    kHeaderBytes + kSectionEntryBytes + uint32_t(kPatternsPerBank) * kPatternMaxBytes;

uint32_t bank_bytes(const Project &project, int bank);
SerializeResult bank_write(const Project &project, int bank, uint8_t *buffer, uint32_t capacity,
                           uint32_t *written = nullptr);

// `refused` receives how many of the 16 patterns were damaged and left empty, which the UI shows
// rather than pretending the bank loaded cleanly. It is 0 on any result other than `ok`: the
// file itself failed rather than a pattern in it, and then all 16 slots are left empty.
//
// A successful read clears the bank's dirty flag, including for the slots it refused -- what is
// in RAM is what is in the file, and marking it dirty would let the next save overwrite a
// damaged file. Neither of these takes a bank index no pattern exists at: both report
// `malformed` for one, since SerializeResult has no value for "no such bank".
SerializeResult bank_read(Project &project, int bank, const uint8_t *buffer, uint32_t size,
                          int *refused = nullptr);

}  // namespace strata
