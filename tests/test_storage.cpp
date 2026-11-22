// app/storage.cpp: the card side -- the ten files a project is, the save and load protocols,
// autosave, the WAV loader, the background sample swap, and the dirty flags.
//
// ## What this file is for
//
// `app/storage.cpp` is 1,037 lines and until two commits ago nothing tested it. tests/test_app.cpp
// has a handful of card tests (a first save, a reload, two card-removal orders, the external-clock
// refusal); this is the thorough pass over everything else.
//
// The highest-value test here is the first one: a mechanical walk over EVERY value of `Cmd`
// asserting which of a project's files that command dirties. Two commands were missing from those
// lists until a review caught them -- `Cmd::euclid` (a generated rhythm left its bank clean, so an
// explicit save skipped the bank file and the rhythm was gone on reopen) and `set_root`/`set_scale`
// (which write BOTH the project's key and the live pattern's copy of it, and marking only the
// project saved half the change). A review found those. A table does not need a reviewer.
//
// ## Both contexts, by hand, and one of them deliberately held
//
// The rig is tests/apprig.hpp: one Engine, one Storage, `pump()` to run a block of audio and then
// a pass of the card. Two things here go underneath it:
//
//   * `pump_held()` runs both contexts WITHOUT firing the fake's queued `files::load`
//     completions, which is how a load is held in flight and how the sample-swap protocol can be
//     inspected one state at a time. `pump()` completes loads every pass, which is right for
//     everything else and fatal to a test about what the app refuses mid-load.
//   * `settle()` waits for the card side to go quiet and STAY quiet. `pump_until_idle()` returns
//     at the first idle pass, and a project load is followed by eight queued sample swaps with
//     one idle pass between each, so it comes back in the middle of the sequence.
//
// ## What cannot be checked on the host, and is therefore not pretended
//
//   * The conversion arithmetic itself, which is tests/test_pcm.cpp's. What IS checked here is
//     that the loader put the right bytes at the offset it handed over: `Storage::region_` is
//     private, but the Sampler was given a pointer into it and dsp/sample.hpp exposes that for
//     tests, so a file of distinguishable frames can be compared where it landed.
//   * `files::save`'s crash safety ("NAME.TMP then replace"). The fake writes in one step, so a
//     card pulled between the two cannot be produced here. What is checked is the consequence the
//     app is responsible for: a failed or interrupted save leaves the project on the card readable
//     and does not mark it complete.
//   * That `App::stop` really runs with the audio callback gone (Enaga's apps_unload clears it
//     first). `save_for_shutdown` relies on it to apply the ring from the main context; the
//     host runs both sides on one thread, so it cannot tell the difference.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include "pcm.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "serialize.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <enaga/settings.hpp>

#include <enaga/abi.h>
#include <enaga/clock.hpp>
#include <enaga/wav.hpp>

using namespace strata;
using strata::test::FakeHost;

namespace {

using strata::test::engine;
using strata::test::fresh;
using strata::test::kit_value;
using strata::test::peak_of;
using strata::test::post;
using strata::test::pump;
using strata::test::pump_until_idle;
using strata::test::storage;

constexpr int kBlk = strata::test::kRigBlock;
constexpr int kStereo = strata::test::kRigStereo;

// The eleven files a project folder holds. app/storage.cpp calls them "the ten files" throughout;
// there are three fixed ones and eight banks, so eleven is the count the code actually uses
// (`kFileCount` is `kFileFirstBank + kBanks`). The order is the order they are written and read in.
constexpr const char *kFiles[] = {
    "PROJECT.BIN", "KIT.BIN",    "PERFORM.BIN", "BANK_A.BIN", "BANK_B.BIN", "BANK_C.BIN",
    "BANK_D.BIN",  "BANK_E.BIN", "BANK_F.BIN",  "BANK_G.BIN", "BANK_H.BIN",
};
constexpr int kFileN = int(sizeof kFiles / sizeof kFiles[0]);
static_assert(kFileN == 3 + kBanks, "the file list and the bank count have diverged");

constexpr int kIndexProject = 0;
constexpr int kIndexKit = 1;
constexpr int kIndexPerform = 2;
constexpr int kIndexFirstBank = 3;

// LVL on every machine (dsp/track.hpp), which is what `fresh()` turns up and what most of these
// tests move around as a value they can recognise again after a round trip.
constexpr int kLvl = 11;

// Big enough for the largest project file (a bank, about 129 KiB) and for the longest test WAV
// (70,000 mono frames, which is the one that needs all 24 bits of the handover's frame count).
// File scope rather than locals: CLAUDE.md caps a stack frame at 16 KiB.
constexpr uint32_t kScratchBytes = kBankMaxBytes > 160000u ? kBankMaxBytes : 160000u;
uint8_t g_scratch[kScratchBytes];
uint8_t g_pristine[kScratchBytes];
uint32_t g_pristine_bytes = 0;

float g_block[kStereo];

// --- driving the two contexts ---------------------------------------------

// One pass of each context, WITHOUT completing a held `files::load`. tests/apprig.hpp's `pump()`
// calls `complete_loads()` every pass, which is what almost every test wants and exactly what a
// test about a load in flight must not do. Returns the loudest sample seen, so a caller can also
// ask whether the transport is still producing audio while the card side waits.
float pump_held(int blocks = 1) {
    float peak = 0.0f;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < kStereo; i++) g_block[i] = 0.0f;
        engine().process(nullptr, g_block, uint32_t(kBlk));
        const float block_peak = peak_of(g_block, kStereo);
        if (block_peak > peak) peak = block_peak;
        storage().service();
    }
    return peak;
}

// Pump until the card side has been idle, with nothing in flight and an empty ring, for `quiet`
// passes in a row.
//
// `pump_until_idle()` returns at the FIRST idle pass, which is not the end of the work: a project
// load finishes by queueing eight sample swaps, and between one swap and the next there is a pass
// with no job at all. A test that stopped there would assert about half a sequence.
bool settle(int quiet = 24, int limit = 8000) {
    int still = 0;
    for (int i = 0; i < limit; i++) {
        pump();
        const bool idle = storage().busy_text() == nullptr &&
                          FakeHost::the().pending_loads() == 0 && engine().commands().empty();
        still = idle ? still + 1 : 0;
        if (still >= quiet) return true;
    }
    return false;
}

// Read the card again and stop at the pass the load finished on.
//
// `last_error()` is sticky only "until the next job starts" (app/storage.hpp), and `begin_job`
// clears it -- so the reason a load refused a file has to be read before anything else begins. A
// project load ENDS by queueing a sample swap for all eight tracks, which is exactly such a job,
// so `settle()` wipes the message this is here to read. tests/apprig.hpp's `pump_until_idle`
// returns at the first idle pass, which is the one the load finished on.
//
// `last_error()` now survives that (`storage_the_reason_a_load_refused_a_file_survives_the_sample_
// swaps` below), but reading at the finishing pass still says which file the reason came from
// before any swap could add one of its own.
bool reload_now() {
    storage().request_reload();
    return pump_until_idle();
}

// What the screen would say, with a diagnostic that names the case. Null-safe, which matters:
// `last_error()` being null IS one of the failures this file is looking for.
void check_error(const char *what, const char *want) {
    const char *got = storage().last_error();
    if (got == nullptr || std::strcmp(got, want) != 0) {
        std::printf("    %s reported %s, wanted %s\n", what, got == nullptr ? "nothing" : got,
                    want);
    }
    CHECK_EQ(got, want);
}

// `fresh()` and then quiet. The rig's `fresh()` restores the card with an `on_card` transition,
// which re-offers every sample the PREVIOUS test's kit named, so a test can inherit a swap job
// that is still running. Starting from quiet means a `pump_until_idle()` below is waiting for this
// test's job and not the last one's.
Engine &clean() {
    Engine &e = fresh();
    settle(4);
    return e;
}

// --- paths ----------------------------------------------------------------

constexpr const char *kDataFolder = "/ENAGA/DATA/STRATA";

// A file in the project folder, or in AUTOSAVE/. The project NAME is read from Storage rather
// than hard-coded, because `request_new()` renames the project for the rest of the process and
// there is no way to name it back (nothing but `init()` reads the setting).
const char *card_path(const char *file, bool autosave = false) {
    static char path[ENAGA_FILE_PATH_MAX + 1];
    std::strcpy(path, kDataFolder);
    if (autosave) {
        std::strcat(path, "/AUTOSAVE/");
    } else {
        std::strcat(path, "/PROJECTS/");
        std::strcat(path, storage().project_name());
        std::strcat(path, "/");
    }
    std::strcat(path, file);
    return path;
}

// --- reading and damaging what is on the card -----------------------------

uint32_t fetch(const char *path) {
    const int32_t got = FakeHost::the().read_file(path, g_scratch, uint32_t(sizeof g_scratch));
    CHECK(got > 0);
    return got > 0 ? uint32_t(got) : 0u;
}

void store(const char *path, uint32_t bytes) {
    CHECK(FakeHost::the().write_file(path, g_scratch, bytes));
}

void keep_pristine(const char *path) { g_pristine_bytes = fetch(path);
    std::memcpy(g_pristine, g_scratch, g_pristine_bytes);
}

void restore_pristine(const char *path) {
    std::memcpy(g_scratch, g_pristine, g_pristine_bytes);
    store(path, g_pristine_bytes);
}

uint32_t u16_at(uint32_t at) {
    return uint32_t(g_scratch[at]) | uint32_t(g_scratch[at + 1]) << 8;
}

void put_u16_at(uint32_t at, uint32_t value) {
    g_scratch[at] = uint8_t(value);
    g_scratch[at + 1] = uint8_t(value >> 8);
}

uint32_t u32_at(uint32_t at) { return u16_at(at) | u16_at(at + 2) << 16; }

// Make the CRC-32 agree with the payload again, so that a test can damage the STRUCTURE of a file
// and reach the checks that come after the checksum. core/serialize.hpp puts the CRC over the
// payload only, which is why damaging the header's own fields needs no help from this.
void refresh_crc() {
    const uint32_t payload = u32_at(8);
    const uint32_t crc = crc32(g_scratch + kHeaderBytes, payload);
    g_scratch[kCrcOffset + 0] = uint8_t(crc);
    g_scratch[kCrcOffset + 1] = uint8_t(crc >> 8);
    g_scratch[kCrcOffset + 2] = uint8_t(crc >> 16);
    g_scratch[kCrcOffset + 3] = uint8_t(crc >> 24);
}

// --- the file-set probe ---------------------------------------------------
//
// "Which files did that save write?" The dirty flags are private and the right observable is the
// card, so every file is first overwritten with ONE byte and a file that comes back a different
// size was written. Marking rather than removing keeps the fake's arena from growing: its entry
// capacity is a high-water mark, so a 129 KiB bank rewritten over its own 1-byte marker lands in
// the space it already had.

void mark_files(bool autosave = false) {
    for (int i = 0; i < kFileN; i++) FakeHost::the().write_file(card_path(kFiles[i], autosave), "x", 1);
}

bool was_written(int index, bool autosave = false) {
    return FakeHost::the().file_bytes(card_path(kFiles[index], autosave)) != 1;
}

// --- building a WAV on the card -------------------------------------------

void put_sample(uint8_t *out, int bits, bool floating, int32_t value) {
    if (floating) {
        const float f = float(value) / 32768.0f;
        std::memcpy(out, &f, sizeof f);
        return;
    }
    switch (bits) {
        case 16:
            out[0] = uint8_t(value);
            out[1] = uint8_t(value >> 8);
            break;
        case 24: {
            const int32_t wide = value << 8;      // the same level, three bytes of it
            out[0] = uint8_t(wide);
            out[1] = uint8_t(wide >> 8);
            out[2] = uint8_t(wide >> 16);
            break;
        }
        default:
            out[0] = uint8_t(value >> 8) ^ 0x80u;  // 8-bit PCM is unsigned, offset by 128
            break;
    }
}

// A WAV on the fake card: `enaga::wav::header` for the 44 bytes (the same function a recording
// uses), then `frames` frames of a loud square wave, then an optional chunk AFTER the data, which
// is what a LIST, a cue or an id3 looks like in a real file.
//
// `declare_data` false writes a data chunk whose size field is 0, which <enaga/wav.hpp> says a
// header still being written may report.
// `ramp` makes every frame distinguishable from every other one -- frame f is f + 1 on the left
// and -(f + 1) on the right -- which is what lets a test check that the loader put the file in the
// region at the offset it handed over, and not one frame off it.
bool g_ramp = false;

uint32_t make_wav(const char *path, uint32_t rate, int channels, int bits, uint32_t frames,
                  const char *extra_tag = nullptr, uint32_t extra_bytes = 0,
                  bool floating = false, bool declare_data = true, uint32_t stray_bytes = 0) {
    enaga::wav::Format format;
    format.encoding = floating ? uint16_t(enaga::wav::floating) : uint16_t(enaga::wav::pcm);
    format.channels = uint16_t(channels);
    format.rate = rate;
    format.bits = uint16_t(bits);

    const uint32_t sample_bytes = uint32_t(bits) / 8u;
    const uint32_t frame_bytes = format.frame_bytes();
    const uint32_t data_bytes = frames * frame_bytes;
    // A file bigger than the scratch buffer is the TEST's mistake, and writing past the buffer
    // corrupts whatever the linker put next -- which was another test file's App, failing a test
    // nowhere near the one at fault. Refuse it loudly instead.
    if (44u + data_bytes + extra_bytes + 8u + stray_bytes > kScratchBytes) {
        std::printf("    make_wav: %s does not fit the %u-byte scratch buffer\n", path,
                    unsigned(kScratchBytes));
        CHECK(false);
        return 0;
    }
    enaga::wav::header(g_scratch, format, declare_data ? data_bytes : 0u);

    uint32_t at = 44;
    for (uint32_t f = 0; f < frames; f++) {
        // A square wave 32 frames long: loud, and obviously not silence.
        const int32_t value = (f / 32u) % 2u == 0u ? 12000 : -12000;
        for (int c = 0; c < channels; c++) {
            const int32_t sample = g_ramp ? (c == 0 ? int32_t(f + 1) : -int32_t(f + 1)) : value;
            put_sample(g_scratch + at, bits, floating, sample);
            at += sample_bytes;
        }
    }
    for (uint32_t i = 0; i < stray_bytes; i++) g_scratch[at++] = 0x11u;

    if (extra_tag != nullptr) {
        for (int i = 0; i < 4; i++) g_scratch[at + uint32_t(i)] = uint8_t(extra_tag[i]);
        g_scratch[at + 4] = uint8_t(extra_bytes);
        g_scratch[at + 5] = uint8_t(extra_bytes >> 8);
        g_scratch[at + 6] = uint8_t(extra_bytes >> 16);
        g_scratch[at + 7] = uint8_t(extra_bytes >> 24);
        at += 8;
        for (uint32_t i = 0; i < extra_bytes; i++) g_scratch[at++] = 0x55u;
    }

    store(path, at);
    return at;
}

void kit_sample(int track, const char *path) {
    KitTrack &t = engine().project_for_load().kit.tracks[track];
    std::memset(t.sample, 0, sizeof t.sample);
    std::strcpy(t.sample, path);
}

// Track 7 is the only SAMPLE machine in the default kit (app/engine.cpp), so it is the only track
// whose `Cmd::set_sample` the audio side accepts. Everything about the loader is checked there.
constexpr int kLoopTrack = 7;

const SampleSlot &slot_of(int track) { return storage().samples().slot(track); }

// Load whatever the kit names for a track and wait for the whole sequence.
bool load_sample(int track) {
    if (!storage().request_sample(track)) return false;
    return settle();
}

}  // namespace

// =============================================================================================
// 1. The dirty-flag table
// =============================================================================================
//
// `mutates_pattern`, `mutates_kit`, `mutates_project` and `mutates_perform` in app/storage.cpp
// decide which of a project's files each command dirties, and a dirty flag that is missing is
// data loss: an explicit save skips the file, and the edit is gone on the next open.
//
// The expectation for each command is what app/engine.cpp's `apply` actually writes, checked
// against what each file holds (core/project.cpp's `project_write` and `kit_write`,
// core/perform.cpp's `perform_write`, core/serialize.cpp's `pattern_write`). Where the flag is
// CONSERVATIVE -- set for a command that does not in fact change the file -- the comment says so
// and why the extra write is the cheaper mistake; app/storage.hpp makes that trade explicitly.

namespace {

enum : unsigned {
    kDirtyProject = 1u << 0,   // PROJECT.BIN
    kDirtyKit = 1u << 1,       // KIT.BIN
    kDirtyPerform = 1u << 2,   // PERFORM.BIN
    kDirtyBank = 1u << 3,      // BANK_<live>.BIN
};

struct Expect {
    Cmd cmd;
    unsigned files;
    const char *name;
};

const Expect kTable[] = {
    // --- nothing at all ---------------------------------------------------
    {Cmd::none, 0u, "none"},
    // The transport is MAIN context (<enaga/clock.hpp>), so these two carry intent for a trace
    // and the audio side only cuts or releases voices. Nothing a file holds moves.
    {Cmd::play, 0u, "play"},
    {Cmd::stop, 0u, "stop"},

    // PROJECT.BIN holds `tempo_centi_bpm`, so a tempo change belongs to it. Nothing writes that
    // field today (app/app.cpp sets the CLOCK and never posts this command) and nothing applies
    // it after a load either, so the flag is right for the day the tempo is stored and the tempo
    // itself does not survive a reopen. Reported with the commit that added this file.
    {Cmd::set_tempo, kDirtyProject, "set_tempo"},

    // --- the live pattern, and therefore its bank's file -------------------
    {Cmd::toggle_trig, kDirtyBank, "toggle_trig"},
    {Cmd::clear_lock, kDirtyBank, "clear_lock"},
    // Length, speed and swing are all TrackPattern/Pattern fields, written by `pattern_write`.
    {Cmd::set_track_length, kDirtyBank, "set_track_length"},
    {Cmd::set_track_speed, kDirtyBank, "set_track_speed"},
    {Cmd::set_pattern_swing, kDirtyBank, "set_pattern_swing"},
    {Cmd::set_track_swing, kDirtyBank, "set_track_swing"},
    {Cmd::set_master_length, kDirtyBank, "set_master_length"},
    {Cmd::step_field_delta, kDirtyBank, "step_field_delta"},
    // A lock lands in the pattern's shared pool, which the bank file carries.
    {Cmd::lock_delta, kDirtyBank, "lock_delta"},
    // The Euclidean generator writes trig bits. It was MISSING from the pattern list, so a
    // generated rhythm left its bank clean and an explicit save skipped the bank file entirely.
    {Cmd::euclid, kDirtyBank, "euclid"},

    // An undo MARK copies the pattern into the undo history and changes nothing in it. The
    // history is not serialized, so there is nothing to save.
    {Cmd::undo_mark, 0u, "undo_mark"},
    {Cmd::undo, kDirtyBank, "undo"},
    {Cmd::redo, kDirtyBank, "redo"},

    // A copy fills the clipboard, which is engine state and is not serialized.
    {Cmd::range_copy, 0u, "range_copy"},
    {Cmd::range_paste, kDirtyBank, "range_paste"},
    {Cmd::range_erase, kDirtyBank, "range_erase"},
    {Cmd::track_erase, kDirtyBank, "track_erase"},
    {Cmd::range_transpose, kDirtyBank, "range_transpose"},
    {Cmd::range_nudge, kDirtyBank, "range_nudge"},
    {Cmd::record_hit, kDirtyBank, "record_hit"},

    // --- PROJECT.BIN ------------------------------------------------------
    //
    // A pattern change moves `project_.bank` and `project_.slot`, which `project_write` stores --
    // so reopening the project comes back to the pattern that was playing. The pattern itself is
    // untouched, so no bank is dirtied.
    {Cmd::queue_pattern, kDirtyProject, "queue_pattern"},
    // Cancelling only clears the armed change, which lives in core/queue.hpp's Queue and is not
    // serialized at all.
    {Cmd::cancel_queue, 0u, "cancel_queue"},
    // Starting a chain jumps to its first entry, which is the same `bank`/`slot` move as above.
    // (`ProjectSettings::chain_enabled` is serialized but nothing writes it -- see the report.)
    {Cmd::set_chain, kDirtyProject, "set_chain"},

    // --- KIT.BIN ----------------------------------------------------------
    //
    // `kit_store` copies `params_` and the mixer levels into `project_.kit`, which is exactly
    // what KIT.BIN holds. It is also what `request_save` posts, so an explicit save always
    // stores the kit whatever else is dirty.
    {Cmd::kit_store, kDirtyKit, "kit_store"},
    // The other direction: the kit the FILE holds is copied into the machines. Nothing a file
    // holds changes, so nothing is dirtied.
    {Cmd::kit_recall, 0u, "kit_recall"},

    // Both stores: `project_.settings.root`/`scale` for PROJECT.BIN and `live().root`/`scale`,
    // which core/serialize puts in the BANK file and which core/scale actually reads. Marking
    // only the project saved half the change, and the bank's untouched copy won the next load --
    // so the key appeared to revert.
    {Cmd::set_root, kDirtyProject | kDirtyBank, "set_root"},
    {Cmd::set_scale, kDirtyProject | kDirtyBank, "set_scale"},

    // A load is the opposite of an edit: 1.03 MiB arrived through `project_for_load` and whoever
    // put it there owns the flags (`step_load` clears them all, `request_new` sets them all).
    {Cmd::project_loaded, 0u, "project_loaded"},

    // The two halves of the sample swap. Neither writes `KitTrack::sample` today -- M4 has no
    // file browser, so a path only ever arrives with a project -- but the path a track plays is
    // what KIT.BIN holds for it, so the pair that changes a track's sample dirties the kit. The
    // cost of being wrong the other way is a lost sample assignment; the cost of this is one
    // 1,076-byte write.
    {Cmd::clear_sample, kDirtyKit, "clear_sample"},
    {Cmd::set_sample, kDirtyKit, "set_sample"},
    // A streamed file is the same swap's second half. Attaching its stream changes nothing on
    // the card: it is the card side telling the audio side the stream is open.
    {Cmd::set_stream, kDirtyKit, "set_stream"},
    {Cmd::stream_attach, 0u, "stream_attach"},
    {Cmd::set_slice_mode, kDirtyKit, "set_slice_mode"},

    {Cmd::set_param, kDirtyKit, "set_param"},
    // The mixer fader, which `kit_from_params` copies into `KitTrack::level`.
    {Cmd::set_track_level, kDirtyKit, "set_track_level"},

    // FIXED. The send and master settings used to be in no file -- `SendSettings` and
    // `MasterSettings` had no section in PROJECT.BIN -- and no handler and no caller either, so
    // every delay and reverb setting was frozen at its compile-time default for the life of the
    // app. PROJECT.BIN's third section now holds the fourteen encoder bytes they are derived
    // from, so both dirty the project.
    {Cmd::set_sends, kDirtyProject, "set_sends"},
    {Cmd::set_master, kDirtyProject, "set_master"},
    // The kill EQ's switches, in the same section as the master bytes.
    {Cmd::eq_kill, kDirtyProject, "eq_kill"},
    // The live-record quantize amount. PROJECT.BIN has had a field for it since M4 and nothing
    // wrote it: the value lived in `app/` alone and went back to 100 on every restart.
    {Cmd::set_quantize, kDirtyProject, "set_quantize"},
    {Cmd::restart, 0u, "restart"},
    // The chain's entries are PROJECT.BIN's second section.
    {Cmd::chain_edit, kDirtyProject, "chain_edit"},

    // --- the performance layer --------------------------------------------
    //
    // PROJECT.BIN's live section holds the mutes as they stand (core/project.hpp).
    {Cmd::set_mute, kDirtyProject, "set_mute"},
    // The fill pad is where a hand is, not part of the project.
    {Cmd::set_fill, 0u, "set_fill"},
    // One undoable variation of a track's range: a pattern edit like any other.
    {Cmd::mutate, kDirtyBank, "mutate"},

    // PERFORM.BIN holds the scenes and the macro setup, and nothing else.
    {Cmd::scene_capture, kDirtyPerform, "scene_capture"},
    {Cmd::scene_erase, kDirtyPerform, "scene_erase"},
    // WHICH scenes are in slots A and B is in PROJECT.BIN's live section, restored on open.
    {Cmd::scene_select, kDirtyProject, "scene_select"},
    // The morph position and a macro's position are where the performer's hands are. docs/SPEC.md
    // is explicit that those do not save.
    {Cmd::set_morph, 0u, "set_morph"},
    {Cmd::set_macro, 0u, "set_macro"},
    // The macro SETUP does save.
    {Cmd::macro_assign, kDirtyPerform, "macro_assign"},
    {Cmd::macro_clear, kDirtyPerform, "macro_clear"},

    // `mute_store` writes `project_.settings.mute_states[slot]`, which PROJECT.BIN holds.
    {Cmd::mute_store, kDirtyProject, "mute_store"},
    // A recall changes the live mutes, which PROJECT.BIN's live section holds.
    {Cmd::mute_recall, kDirtyProject, "mute_recall"},

    // Auto-fill, a throw and beat repeat are all live-only: no file has a field for any of them.
    {Cmd::set_auto_fill, 0u, "set_auto_fill"},
    {Cmd::set_throw, 0u, "set_throw"},
    {Cmd::set_repeat, 0u, "set_repeat"},

    // A pad hit sounds and is not recorded (core/record posts `record_hit` for that).
    {Cmd::trigger_note, 0u, "trigger_note"},
    {Cmd::release_note, 0u, "release_note"},
    {Cmd::note_repeat, 0u, "note_repeat"},   // a roll is played, not written
    {Cmd::delay_freeze, 0u, "delay_freeze"},
    {Cmd::tape_stop, 0u, "tape_stop"},

    // Resampling dirties no project file. The take goes to /RECORD as its own WAV -- which is
    // not one of a project's eleven files -- and the arm, the cancel and the release only move
    // the capture buffer's state, which is audio-side and in no file at all. Loading the result
    // back into a track is `Cmd::set_sample`, which dirties the KIT and has its own row above.
    {Cmd::resample_arm, 0u, "resample_arm"},
    {Cmd::resample_cancel, 0u, "resample_cancel"},
    {Cmd::resample_release, 0u, "resample_release"},

    // Which machine a track hosts is a KIT field -- KitTrack::machine, on disk since M4 -- which
    // is why machine switching needed no change to the file format.
    {Cmd::set_machine, kDirtyKit, "set_machine"},

    // MIDI OUT's channel per track is PROJECT.BIN's fifth section: cabling, not a sound, so not
    // the kit (core/project.hpp).
    {Cmd::set_midi_out, kDirtyProject, "set_midi_out"},
};

// A command added without an entry here does not compile, which is the whole point of the table:
// the two commands a review had to find were both "added later and not added to the list".
static_assert(sizeof kTable / sizeof kTable[0] == size_t(Cmd::count),
              "a Cmd was added or removed: give it a row in kTable saying which of a project's "
              "files it dirties, and check mutates_* in app/storage.cpp agrees");

unsigned bit_for_file(int index, int live_bank) {
    if (index == kIndexProject) return kDirtyProject;
    if (index == kIndexKit) return kDirtyKit;
    if (index == kIndexPerform) return kDirtyPerform;
    return index - kIndexFirstBank == live_bank ? kDirtyBank : 0u;
}

// Which files a save wrote, against which it should have.
void check_file_set(const char *what, unsigned want, int live_bank) {
    for (int i = 0; i < kFileN; i++) {
        const bool written = was_written(i);
        const bool expected = (want & bit_for_file(i, live_bank)) != 0u;
        if (written != expected) {
            std::printf("    %s: %s was %s\n", what, kFiles[i],
                        written ? "written" : "left alone");
        }
        CHECK_EQ(written, expected);
    }
}

}  // namespace

TEST(storage_every_command_dirties_exactly_the_files_it_changes) {
    // Mechanically, over every value of Cmd. `note_command` is what app/app.cpp's `post_command`
    // calls for every edit the UI posts, so this drives it directly and does not post the command
    // to the engine: the subject is the flag table, not what the audio side does with it.
    clean();
    FakeHost &host = FakeHost::the();

    // The baseline: one FULL save, which clears every flag and sets `project_complete_`. From here
    // a save writes only what is dirty, which is what makes the set of files observable.
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(!storage().dirty());

    for (const Expect &e : kTable) {
        mark_files();
        CHECK(!storage().dirty());                      // every round starts from a saved project

        storage().note_command(e.cmd, 0);               // bank A is live
        CHECK_EQ(storage().dirty(), e.files != 0u);

        storage().request_save();
        CHECK(pump_until_idle());
        CHECK(storage().last_error() == nullptr);

        // `request_save` marks the kit itself -- the machines' parameters have to be copied into
        // the kit a file holds before it is written -- so KIT.BIN is written whatever the command
        // dirtied. Everything else is the command's own doing.
        check_file_set(e.name, e.files | kDirtyKit, 0);

        // And the save cleared what it wrote, so the next round's precondition means something.
        CHECK(!storage().dirty());
    }

    CHECK_EQ(host.file_count(), kFileN + 2);   // the eleven, the folder, and SETTINGS.TXT
}

TEST(storage_the_bank_a_command_dirties_is_the_live_one) {
    // `note_command` takes the live bank from Status rather than assuming bank A, because a chain
    // can move the live pattern without the UI asking. A trig edited while bank D plays belongs in
    // BANK_D.BIN, and an out-of-range bank must dirty no bank at all rather than index past the
    // array (`project_index_valid` guards it).
    clean();

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(!storage().dirty());

    mark_files();
    storage().note_command(Cmd::toggle_trig, 3);        // bank D
    CHECK(storage().dirty());
    storage().request_save();
    CHECK(pump_until_idle());
    check_file_set("toggle_trig in bank D", kDirtyKit | kDirtyBank, 3);

    mark_files();
    CHECK(!storage().dirty());
    storage().note_command(Cmd::toggle_trig, kBanks);   // one past the last bank
    storage().note_command(Cmd::toggle_trig, -1);
    CHECK(!storage().dirty());                          // refused, not folded onto bank A
    storage().request_save();
    CHECK(pump_until_idle());
    check_file_set("toggle_trig in no bank", kDirtyKit, 0);
}

// =============================================================================================
// 2. The round trip of every file
// =============================================================================================

TEST(storage_every_file_of_a_project_round_trips_through_the_card) {
    clean();
    FakeHost &host = FakeHost::the();

    // --- what the project is going to be ---------------------------------
    //
    // The patterns of all eight banks and the project's own settings are written through
    // `project_for_load()`, the one hole app/engine.hpp allows in "the main context never writes
    // pattern memory": only the live pattern is reachable through the command ring, and the point
    // here is that all 128 patterns and every field survive.
    Project &p = engine().project_for_load();
    for (int b = 0; b < kBanks; b++) {
        for (int s = 0; s < kPatternsPerBank; s++) {
            Pattern &pattern = p.patterns[b][s];
            const int step = (b * 7 + s) % kStepsPerTrack;
            Step &trig = pattern.tracks[b % kTracks].steps[step];
            trig.flags = uint8_t(trig.flags | kStepTrig);
            trig.note = uint8_t(36 + b * 2 + s % 3);
            trig.velocity = uint8_t(40 + s);
            pattern.tracks[b % kTracks].length = uint8_t(1 + (b * 3 + s) % kStepsPerTrack);
            pattern.swing = uint8_t(kSwingMin + (s % 5));
        }
    }
    p.settings.root = 64;
    p.settings.scale = 3;
    p.settings.quantize = 70;
    p.settings.chain_enabled = true;
    p.settings.chain_length = 3;
    p.chain[0] = ChainEntry{1, 2, 3};
    p.chain[1] = ChainEntry{7, 15, 1};
    p.chain[2] = ChainEntry{0, 0, 8};
    for (int i = 0; i < 8; i++) p.settings.mute_states[i] = uint8_t(0x81u + i);
    p.bank = 2;
    p.slot = 11;
    for (int t = 0; t < kTracks; t++) {
        p.kit.tracks[t].machine = uint8_t(t % kMachines);
    }
    std::strcpy(p.kit.tracks[4].sample, "/SAMPLES/ROUND TRIP.WAV");
    post(Cmd::project_loaded);
    pump(2);

    // The kit and the mixer, through the ring, as a knob and a fader really are.
    post(Cmd::set_param, 3, 0, uint8_t(kLvl), 42);
    post(Cmd::set_param, 6, 0, 2, 117);
    Command fader;
    fader.cmd = Cmd::set_track_level;
    fader.track = 2;
    fader.f = 0.5f;
    engine().commands().post(fader);

    // The scenes and the macro setup, which are PERFORM.BIN.
    //
    // `Cmd::kit_store` FIRST, and that is not incidental: `scene_capture` reads `project_.kit` --
    // the kit a file holds -- and only `kit_store` refreshes that from `params_`, the kit a knob
    // actually turns. Without the store the scene captures the sound as of the last save. Nothing
    // in app/app.cpp posts one before a capture, which is a bug in the capture and not in
    // PERFORM.BIN; reported with the commit that added this file.
    post(Cmd::kit_store);
    post(Cmd::scene_capture, 0, 0, 5, 0);                 // scene 5, every track
    // A macro assignment carries five values, one more than the rig's `post` takes.
    Command assign;
    assign.cmd = Cmd::macro_assign;
    assign.track = 1;
    assign.a = 2;                                         // macro 2
    assign.b = 9;                                         // its target parameter
    assign.c = 20;                                        // low
    assign.d = 110;                                       // high
    engine().commands().post(assign);
    pump(4);

    CHECK_EQ(int(kit_value(3, kLvl)), 42);
    CHECK(scene_count(engine().scenes().scenes[5]) > 0);
    CHECK_EQ(int(engine().macros().macros[2].targets[0].enabled), 1);

    // --- save ------------------------------------------------------------
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("saved"));
    CHECK(!storage().dirty());

    // The sizes tests/test_project.cpp pins from the serializer, reached here through the real
    // card protocol: a file that came out a different size means the two have drifted apart.
    CHECK_EQ(host.file_bytes(card_path("PROJECT.BIN")), 402);
    CHECK_EQ(project_bytes(), 402u);
    CHECK_EQ(host.file_bytes(card_path("KIT.BIN")), 1184);
    CHECK_EQ(kit_bytes(), 1184u);
    CHECK_EQ(host.file_bytes(card_path("PERFORM.BIN")), 3804);
    CHECK_EQ(perform_bytes(), 3804u);
    for (int b = 0; b < kBanks; b++) {
        char name[12];
        CHECK(bank_file_name(b, name, int(sizeof name)));
        // Through a `const char *`, or CHECK_EQ compares two ADDRESSES: tests/check.hpp's
        // non-template overload is the one that compares content, and an array argument does not
        // reach it.
        const char *made = name;
        CHECK_EQ(made, kFiles[kIndexFirstBank + b]);      // the names this test assumes
        CHECK_EQ(uint32_t(host.file_bytes(card_path(name))), bank_bytes(engine().project(), b));
    }

    // --- move RAM away ---------------------------------------------------
    Project &wipe = engine().project_for_load();
    project_init(wipe);
    perform_init(engine().scenes_for_load(), engine().macros_for_load());
    engine().performance_changed();
    post(Cmd::project_loaded);
    pump(4);
    CHECK_EQ(int(kit_value(3, kLvl)), 0);                 // a blank project: nothing left to find
    CHECK_EQ(scene_count(engine().scenes().scenes[5]), 0);

    // And some unsaved edits on top, so that the load has flags to clear: reading the card again
    // throws away what is in RAM, and the screen's unsaved mark has to go with it.
    storage().note_command(Cmd::toggle_trig, 0);
    storage().note_command(Cmd::mute_store, 0);
    storage().note_command(Cmd::scene_erase, 0);
    storage().note_command(Cmd::set_param, 0);
    CHECK(storage().dirty());

    // --- read it back ----------------------------------------------------
    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK(FakeHost::the().logged("loaded"));
    pump(4);                                              // the load's Cmd::project_loaded lands

    const Project &back = engine().project();

    // PROJECT.BIN
    CHECK_EQ(int(back.settings.root), 64);
    CHECK_EQ(int(back.settings.scale), 3);
    CHECK_EQ(int(back.settings.quantize), 70);
    CHECK(back.settings.chain_enabled);
    // And the chain is FOLLOWED again, not just remembered: the flag used to be read by nothing.
    // The saved live pattern (C12, below) is not an entry, so it plays out its pass first.
    CHECK(engine().chain().active);
    CHECK(engine().chain().orphaned);
    CHECK_EQ(int(engine().chain().entry), 0);
    CHECK_EQ(int(back.settings.chain_length), 3);
    CHECK_EQ(int(back.chain[0].bank), 1);
    CHECK_EQ(int(back.chain[0].slot), 2);
    CHECK_EQ(int(back.chain[0].repeats), 3);
    CHECK_EQ(int(back.chain[1].bank), 7);
    CHECK_EQ(int(back.chain[1].slot), 15);
    CHECK_EQ(int(back.chain[2].repeats), 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(int(back.settings.mute_states[i]), int(0x81u + i));
    CHECK_EQ(int(back.bank), 2);
    CHECK_EQ(int(back.slot), 11);
    CHECK_EQ(int(engine().status().live_bank.load(std::memory_order_relaxed)), 2);
    CHECK_EQ(int(engine().status().live_slot.load(std::memory_order_relaxed)), 11);

    // KIT.BIN
    CHECK_EQ(int(kit_value(3, kLvl)), 42);
    CHECK_EQ(int(kit_value(6, 2)), 117);
    CHECK_EQ(int(back.kit.tracks[3].params[kLvl]), 42);
    CHECK_NEAR(back.kit.tracks[2].level, 0.5f, 0.002);
    CHECK_NEAR(engine().level(2), 0.5f, 0.002);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(back.kit.tracks[t].machine), t % kMachines);
    CHECK_EQ(back.kit.tracks[4].sample, "/SAMPLES/ROUND TRIP.WAV");

    // PERFORM.BIN
    CHECK(scene_count(engine().scenes().scenes[5]) > 0);
    CHECK(scene_has(engine().scenes().scenes[5], 3, kLvl));
    CHECK_EQ(int(engine().scenes().scenes[5].values[3][kLvl]), 42);
    const MacroTarget &target = engine().macros().macros[2].targets[0];
    CHECK_EQ(int(target.enabled), 1);
    CHECK_EQ(int(target.track), 1);
    CHECK_EQ(int(target.param), 9);
    CHECK_EQ(int(target.low), 20);
    CHECK_EQ(int(target.high), 110);

    // BANK_A.BIN .. BANK_H.BIN: all 128 patterns, not just the live one.
    for (int b = 0; b < kBanks; b++) {
        for (int s = 0; s < kPatternsPerBank; s++) {
            const Pattern &pattern = back.patterns[b][s];
            const int step = (b * 7 + s) % kStepsPerTrack;
            const Step &trig = pattern.tracks[b % kTracks].steps[step];
            CHECK((trig.flags & kStepTrig) != 0);
            CHECK_EQ(int(trig.note), 36 + b * 2 + s % 3);
            CHECK_EQ(int(trig.velocity), 40 + s);
            CHECK_EQ(int(pattern.tracks[b % kTracks].length),
                     1 + (b * 3 + s) % kStepsPerTrack);
            CHECK_EQ(int(pattern.swing), int(kSwingMin + (s % 5)));
        }
    }

    // And nothing is dirty after a load: what is in RAM is what is on the card.
    CHECK(!storage().dirty());
}

TEST(storage_the_live_mutes_and_scene_choice_come_back_with_the_project) {
    // A set left with tracks muted and a scene picked into slot B reopens that way, rather than
    // all unmuted on scenes 1 and 2.
    clean();
    post(Cmd::set_mute, 2, 0, 1, 0);
    post(Cmd::set_mute, 5, 0, 1, 0);
    post(Cmd::scene_select, 0, 0, 1, 6);
    pump(2);
    CHECK(engine().muted(2));
    CHECK(engine().muted(5));
    CHECK_EQ(engine().scene_slot(1), 6);

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);

    // Away: a blank project is all unmuted on scenes 1 and 2.
    project_init(engine().project_for_load());
    post(Cmd::project_loaded);
    pump(4);
    CHECK(!engine().muted(2));
    CHECK_EQ(engine().scene_slot(1), 1);

    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK(engine().muted(2));
    CHECK(engine().muted(5));
    CHECK(!engine().muted(0));
    CHECK_EQ(engine().scene_slot(0), 0);
    CHECK_EQ(engine().scene_slot(1), 6);

    post(Cmd::set_mute, 2, 0, 0, 0);
    post(Cmd::set_mute, 5, 0, 0, 0);
    pump(2);
}

TEST(storage_a_save_waits_for_the_command_ring_to_drain) {
    // The first of app/storage.hpp's two protocols. A save reads pattern memory from the MAIN
    // context while the audio side may be applying an edit to it, so `request_save` posts
    // `Cmd::kit_store` and `service()` refuses to start anything while the ring is non-empty. By
    // the time it is empty the audio side has applied every pending edit and is writing nothing.
    clean();
    FakeHost &host = FakeHost::the();

    // More edits than the audio side applies in one block (Commands::kMaxPerBlock is 64), so the
    // ring stays non-empty across two blocks and the wait is a state a test can see.
    for (int s = 0; s < 60; s++) post(Cmd::toggle_trig, 0, uint8_t(s));
    for (int s = 0; s < 10; s++) post(Cmd::toggle_trig, 1, uint8_t(s));
    storage().request_save();
    CHECK(!engine().commands().empty());

    // The card side alone, as many passes as it likes. Nothing starts, and nothing spins either:
    // an un-drained ring simply means "not this pass".
    for (int i = 0; i < 200; i++) storage().service();
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(host.saves(), 0);
    CHECK_EQ(host.folders_made(), 0);

    // Now the audio side drains it and the save runs -- including the LAST of those edits, which
    // is the whole reason for deferring.
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK((engine().pattern().tracks[1].steps[9].flags & kStepTrig) != 0);

    Project &wipe = engine().project_for_load();
    project_init(wipe);
    post(Cmd::project_loaded);
    pump(2);

    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    for (int s = 0; s < 60; s++) {
        CHECK((engine().pattern().tracks[0].steps[s].flags & kStepTrig) != 0);
    }
    CHECK((engine().pattern().tracks[1].steps[9].flags & kStepTrig) != 0);
}

TEST(storage_a_save_already_running_waits_for_the_ring_between_files) {
    // The same protocol, one step further. A save is fourteen passes, one file each, and the UI
    // runs between them: a knob turned while an autosave is going -- and an autosave starts the
    // moment the transport stops -- posts an edit between two banks. On the unit the audio side
    // can apply it in the MIDDLE of the next bank's serialization, because audio() preempts the
    // main context, and the file gets a step half old and half new. The check before the job
    // started does not cover this: it ran once, fourteen passes ago.
    //
    // So every pass of a save waits for the ring as the first one did. On the host nothing
    // preempts anything, so what is checked is the condition itself: with an edit pending, a
    // pass writes nothing.
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    pump(4);
    CHECK_EQ(storage().busy_text(), "SAVING");
    const int written = host.saves();
    CHECK(written > 0);

    post(Cmd::toggle_trig, 2, 5);
    CHECK(!engine().commands().empty());
    for (int i = 0; i < 50; i++) storage().service();
    CHECK_EQ(host.saves(), written);
    CHECK_EQ(storage().busy_text(), "SAVING");

    // Drained, it carries on from where it stopped and finishes.
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.saves() > written);
}

TEST(storage_a_load_already_running_waits_for_the_ring_between_files) {
    // The load's version, where it is worse: `step_load` WRITES pattern memory from the main
    // context, so an edit the audio side applies during a pass is two contexts writing the same
    // bytes. The transport being stopped keeps the scheduler out; it does nothing about the
    // command ring, which the audio side drains whether or not the clock runs.
    clean();
    FakeHost &host = FakeHost::the();
    storage().request_save();
    CHECK(settle());

    storage().request_reload();
    pump(4);
    CHECK(storage().loading_project());
    const int read = host.read_alls();
    CHECK(read > 0);

    post(Cmd::toggle_trig, 2, 5);
    for (int i = 0; i < 50; i++) storage().service();
    CHECK_EQ(host.read_alls(), read);
    CHECK(storage().loading_project());

    CHECK(pump_until_idle());
    CHECK(host.read_alls() > read);
}

TEST(storage_busy_text_and_progress_follow_the_job) {
    // What the screen draws while the card is working. Not cosmetic: `busy_text()` is what
    // tests/apprig.hpp waits on, and `progress()` does its percentage in 32-bit arithmetic on
    // purpose (a 64-bit divide here once broke the LINK -- app/storage.cpp says why).
    clean();

    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(storage().progress(), 0);

    storage().request_save();
    pump();
    CHECK_EQ(storage().busy_text(), "SAVING");
    CHECK_EQ(storage().progress(), 0);
    pump(3);
    CHECK_EQ(storage().busy_text(), "SAVING");
    CHECK(storage().progress() > 0);
    CHECK(storage().progress() <= 100);
    CHECK(pump_until_idle());
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(storage().progress(), 0);

    storage().request_reload();
    pump(2);
    CHECK_EQ(storage().busy_text(), "LOADING");
    CHECK(storage().loading_project());        // so the UI refuses PLAY under it
    CHECK(settle());
    CHECK(!storage().loading_project());
    CHECK(storage().busy_text() == nullptr);
}

// =============================================================================================
// 3. The refusal order
// =============================================================================================
//
// core/serialize.hpp checks a damaged container in a FIXED order -- the buffer, the magic, the
// payload against the buffer, the CRC, the version, then the section table -- and core/project.cpp
// repeats it for the project, kit and bank files. The order is what makes the message on screen
// the same for the same damage whichever file it happened to.

TEST(storage_damage_is_reported_in_one_fixed_order) {
    clean();
    const char *kit = card_path("KIT.BIN");

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    keep_pristine(kit);
    CHECK_EQ(g_pristine_bytes, 1184u);

    // A sanity pass first: the pristine file loads.
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);

    // The magic beats everything that follows it.
    restore_pristine(kit);
    g_scratch[0] = 'X';
    g_scratch[kHeaderBytes] = uint8_t(g_scratch[kHeaderBytes] ^ 0xFFu);   // and a bad CRC with it
    put_u16_at(kVersionOffset, 99);                                      // and a bad version
    store(kit, g_pristine_bytes);
    CHECK(reload_now());
    check_error("a bad magic with bad everything else", "NOT A STRATA FILE");

    // A payload longer than the bytes that are there beats the CRC and the version: with the
    // buffer short the CRC cannot even be computed over what the header claims.
    restore_pristine(kit);
    put_u16_at(kVersionOffset, 99);
    store(kit, 40);
    CHECK(reload_now());
    check_error("a truncated file from a newer build", "FILE TRUNCATED");

    // The CRC beats the version. The header is not covered by the checksum (core/serialize.hpp
    // says so and why), so these two kinds of damage really are independent.
    restore_pristine(kit);
    g_scratch[kHeaderBytes + 4] = uint8_t(g_scratch[kHeaderBytes + 4] ^ 0x01u);
    put_u16_at(kVersionOffset, 99);
    store(kit, g_pristine_bytes);
    CHECK(reload_now());
    check_error("a bad CRC and a bad version", "BAD CHECKSUM");

    // The version beats the section table.
    restore_pristine(kit);
    put_u16_at(kVersionOffset, 99);
    put_u16_at(6, 0);                        // no sections, which is malformed
    store(kit, g_pristine_bytes);
    CHECK(reload_now());
    check_error("a bad version and no sections", "UNKNOWN VERSION");

    // And with everything above it sound, the section table is what is reported. The message
    // says PATTERN for every file type, which is core/serialize.cpp's one name for `malformed`.
    restore_pristine(kit);
    put_u16_at(6, 0);
    store(kit, g_pristine_bytes);
    CHECK(reload_now());
    check_error("no sections", "DAMAGED PATTERN");

    // A damaged section OFFSET, with the checksum made to agree, is refused the same way -- so
    // the structure checks are not reachable only through a header field.
    restore_pristine(kit);
    g_scratch[kHeaderBytes + 4] = 0xFFu;     // the first section's offset, out of the payload
    refresh_crc();
    store(kit, g_pristine_bytes);
    CHECK(reload_now());
    check_error("a section outside the payload", "DAMAGED PATTERN");

    restore_pristine(kit);
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);
    CHECK(settle());
}

TEST(storage_the_reason_a_load_refused_a_file_survives_the_sample_swaps) {
    // docs/SPEC.md: "An unknown version or a bad CRC is refused with a message on screen."
    // app/storage.hpp: the last failure is "sticky until the next job starts, because a load that
    // failed silently is the least diagnosable thing a sampler can do."
    //
    // A project load's LAST act is to queue a sample swap for all eight tracks, and the next pass
    // of `service()` starts one, and `begin_job` clears `error_`. `App::update` runs at least 750
    // times a second and the screen is drawn at 20 fps, so a reason kept only there is gone
    // before anything could draw it.
    //
    // This test used to pin that as a known bug. `project_error_` was then added to fix it, but
    // only caught a reason passed to `finish_job` -- and a load that refuses one file does not
    // finish with it: it records it and carries on with the rest. So this exact case still lost
    // its message, and this test still passed asserting that it did.
    clean();

    storage().request_save();
    CHECK(pump_until_idle());

    const char *kit = card_path("KIT.BIN");
    const uint32_t bytes = fetch(kit);
    g_scratch[kHeaderBytes] = uint8_t(g_scratch[kHeaderBytes] ^ 0xFFu);
    store(kit, bytes);

    CHECK(reload_now());
    check_error("at the pass the load finished on", "BAD CHECKSUM");

    pump(3);
    check_error("after the sample swaps began", "BAD CHECKSUM");
    CHECK(settle());
    check_error("once everything settled", "BAD CHECKSUM");

    // And the next save or load is what clears it.
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
}

TEST(storage_the_same_damage_gives_the_same_reason_in_every_file_type) {
    // One bit flipped in the payload of each of the four file types. All four must say BAD
    // CHECKSUM: the container is shared, and a user looking at the screen should not have to know
    // which file the app happened to be reading.
    clean();

    storage().request_save();
    CHECK(pump_until_idle());

    const char *files[] = {"PROJECT.BIN", "KIT.BIN", "PERFORM.BIN", "BANK_C.BIN"};
    for (const char *file : files) {
        const char *path = card_path(file);
        keep_pristine(path);                   // which also leaves the file in the scratch buffer
        CHECK(g_pristine_bytes > kHeaderBytes);
        g_scratch[kHeaderBytes] = uint8_t(g_scratch[kHeaderBytes] ^ 0xFFu);
        store(path, g_pristine_bytes);

        CHECK(reload_now());
        check_error(file, "BAD CHECKSUM");

        // Put it back from the copy, rather than by saving: a save after a load writes only what
        // is dirty, and a load leaves nothing dirty -- so the next round would damage a second
        // file while this one was still broken.
        restore_pristine(path);
        CHECK(settle());
        CHECK(reload_now());
        CHECK(storage().last_error() == nullptr);
        CHECK(settle());
    }
}

TEST(storage_one_damaged_bank_costs_one_bank_and_not_the_project) {
    // The reason PROJECT.BIN is written and read first, and the reason a bank file holds one bank:
    // damage has to be contained. A broken BANK_B.BIN must not cost the other seven banks, the
    // kit, or the project's settings.
    clean();

    Project &p = engine().project_for_load();
    for (int b = 0; b < kBanks; b++) {
        for (int s = 0; s < kPatternsPerBank; s++) {
            p.patterns[b][s].tracks[0].steps[b].flags = uint8_t(kStepTrig);
        }
    }
    p.settings.root = 67;
    post(Cmd::project_loaded);
    pump(2);
    post(Cmd::set_param, 5, 0, uint8_t(kLvl), 88);
    pump(2);

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);

    const char *bank_b = card_path("BANK_B.BIN");
    const uint32_t bytes = fetch(bank_b);
    g_scratch[kHeaderBytes + 32] = uint8_t(g_scratch[kHeaderBytes + 32] ^ 0xFFu);
    store(bank_b, bytes);

    Project &wipe = engine().project_for_load();
    project_init(wipe);
    post(Cmd::project_loaded);
    pump(2);

    CHECK(reload_now());
    check_error("a damaged BANK_B.BIN", "BAD CHECKSUM");
    pump(4);

    const Project &back = engine().project();
    // Bank B is empty -- core/project.cpp leaves all 16 slots cleared when the FILE fails, rather
    // than half-reading it.
    for (int s = 0; s < kPatternsPerBank; s++) {
        CHECK_EQ(int(back.patterns[1][s].tracks[0].steps[1].flags), 0);
    }
    // Everything else arrived.
    for (int b = 0; b < kBanks; b++) {
        if (b == 1) continue;
        for (int s = 0; s < kPatternsPerBank; s++) {
            CHECK((back.patterns[b][s].tracks[0].steps[b].flags & kStepTrig) != 0);
        }
    }
    CHECK_EQ(int(back.settings.root), 67);
    CHECK_EQ(int(kit_value(5, kLvl)), 88);
}

TEST(storage_one_damaged_pattern_inside_a_bank_costs_one_pattern) {
    // A bank file is sixteen pattern containers back to back, each with its own CRC. A bank whose
    // own container is sound but which holds one damaged pattern loads the other fifteen, leaves
    // that slot empty, and says PATTERN LOST rather than pretending the bank was clean
    // (core/project.hpp). The bank's checksum is recomputed here so the damage really is inside
    // one pattern and not in the file around it.
    clean();

    Project &p = engine().project_for_load();
    for (int s = 0; s < kPatternsPerBank; s++) {
        p.patterns[0][s].tracks[0].steps[s].flags = uint8_t(kStepTrig);
    }
    post(Cmd::project_loaded);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());

    const char *bank = card_path("BANK_A.BIN");
    const uint32_t bytes = fetch(bank);
    g_scratch[bytes / 2u] = uint8_t(g_scratch[bytes / 2u] ^ 0xFFu);   // somewhere in the middle
    refresh_crc();
    store(bank, bytes);

    Project &wipe = engine().project_for_load();
    project_init(wipe);
    post(Cmd::project_loaded);
    pump(2);

    CHECK(reload_now());
    check_error("one damaged pattern in a bank", "PATTERN LOST");
    pump(4);

    int intact = 0;
    int lost = 0;
    for (int s = 0; s < kPatternsPerBank; s++) {
        const bool trig =
            (engine().project().patterns[0][s].tracks[0].steps[s].flags & kStepTrig) != 0;
        intact += trig ? 1 : 0;
        lost += trig ? 0 : 1;
    }
    CHECK_EQ(lost, 1);
    CHECK_EQ(intact, kPatternsPerBank - 1);
    CHECK(settle());
}

TEST(storage_a_damaged_project_file_does_not_stop_the_banks_loading) {
    // The other half of the containment rule: 251 damaged bytes must not cost 128 patterns.
    // `project_read` leaves the settings at their defaults and deliberately does not touch a
    // pattern, so the load reports the reason and carries on.
    clean();

    Project &p = engine().project_for_load();
    p.patterns[0][0].tracks[2].steps[6].flags = uint8_t(kStepTrig);
    p.settings.root = 70;
    post(Cmd::project_loaded);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());

    const char *project = card_path("PROJECT.BIN");
    const uint32_t bytes = fetch(project);
    g_scratch[kHeaderBytes + 8] = uint8_t(g_scratch[kHeaderBytes + 8] ^ 0xFFu);
    store(project, bytes);

    Project &wipe = engine().project_for_load();
    project_init(wipe);
    post(Cmd::project_loaded);
    pump(2);

    CHECK(reload_now());
    check_error("a damaged PROJECT.BIN", "BAD CHECKSUM");
    CHECK(FakeHost::the().logged("loaded"));         // it ran to the end rather than stopping
    pump(4);

    // The pattern came back from BANK_A.BIN...
    CHECK((engine().project().patterns[0][0].tracks[2].steps[6].flags & kStepTrig) != 0);
    // ...and the project's own settings did not, which is what a refused PROJECT.BIN means.
    CHECK_EQ(int(engine().project().settings.root), 60);
}

// =============================================================================================
// 4. Failure injection
// =============================================================================================

TEST(storage_a_full_card_fails_a_save_and_corrupts_nothing) {
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(kit_value(2, kLvl)), 127);          // what is on the card now

    // An edit, and then no room for it.
    post(Cmd::set_param, 2, 0, uint8_t(kLvl), 55);
    post(Cmd::toggle_trig, 1, 9);
    pump(2);
    storage().note_command(Cmd::set_param, 0);
    storage().note_command(Cmd::toggle_trig, 0);
    CHECK(storage().dirty());

    host.set_full(true);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK_EQ(storage().last_error(), "CARD FULL");
    CHECK(host.logged("save failed CARD FULL"));

    // The work is still in RAM and still marked unsaved, so the screen keeps the unsaved mark and
    // the user can try again rather than being told it is safe.
    CHECK(storage().dirty());
    CHECK_EQ(int(kit_value(2, kLvl)), 55);

    // And the card is not corrupted: what was there before still reads, with the OLD values.
    host.set_full(false);
    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK_EQ(int(kit_value(2, kLvl)), 127);
    CHECK_EQ(int(engine().pattern().tracks[1].steps[9].flags), 0);
}

TEST(storage_a_card_that_fills_at_the_banks_stops_there_and_says_so) {
    // The other end of a save. A save writes one file per pass, so filling the card after the
    // three fixed files are down puts the failure on BANK_A.BIN -- the branch that writes 129 KiB
    // and has its own error path, which the failure above (on KIT.BIN) never reaches.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 4);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);      // so bank A has something to lose
    CHECK(storage().dirty());

    storage().request_save();
    pump(4);                                      // begin, then PROJECT.BIN, KIT.BIN, PERFORM.BIN
    CHECK_EQ(storage().busy_text(), "SAVING");
    CHECK_EQ(storage().progress(), 3 * 100 / (kFileN));   // three files down, the banks next
    CHECK(host.has_file(card_path("PERFORM.BIN")));
    CHECK(!host.has_file(card_path("BANK_A.BIN")));

    // The failure lands inside the pass below, so this does not wait for a job to finish: by the
    // time the pass is over the job has already ended.
    host.set_full(true);
    pump(2);
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(storage().last_error(), "CARD FULL");
    CHECK(host.logged("save failed CARD FULL"));
    CHECK(!host.logged("saved"));
    CHECK(!host.has_file(card_path("BANK_A.BIN")));    // it stopped, rather than skipping on
    CHECK(!host.has_file(card_path("BANK_B.BIN")));
    CHECK(storage().dirty());

    // Room again, and the next save writes EVERY file -- the interrupted one never marked the
    // folder complete.
    host.set_full(false);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    for (int i = 0; i < kFileN; i++) CHECK(host.has_file(card_path(kFiles[i])));
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);
    CHECK(settle());
}

TEST(storage_a_read_error_on_the_first_file_abandons_the_load) {
    // A card that fails its reads. PROJECT.BIN is read first and a failure there ENDS the job --
    // which is the right way round: nothing has been written into pattern memory yet, so the
    // project in RAM is still whole and the user has lost nothing but the attempt.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 3);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);

    const int reads_before = host.read_alls();
    host.set_read_error(true);
    CHECK(reload_now());
    check_error("a card that failed every read", "CARD ERROR");
    CHECK(host.logged("load failed CARD ERROR"));
    CHECK(!host.logged("loaded"));
    CHECK_EQ(host.read_alls(), reads_before + 1);   // it stopped at the first file

    // And RAM is untouched: the edit is still there to be saved again.
    pump(4);
    CHECK((engine().pattern().tracks[0].steps[3].flags & kStepTrig) != 0);

    // The card itself is intact: reading again, without the injected error, works.
    host.set_read_error(false);
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("loaded"));
    CHECK(settle());
    pump(4);
    CHECK((engine().pattern().tracks[0].steps[3].flags & kStepTrig) != 0);
}

TEST(storage_a_file_missing_from_the_project_folder_is_not_damage) {
    // Not every file has to be there. A bank that was never touched has no file, and a project
    // written by an M4 build has no PERFORM.BIN -- both mean "empty", not "damaged", and the load
    // carries on. KIT.BIN is deliberately NOT in that set: a project with no kit is a project
    // whose sound is gone, and the screen says so.
    clean();
    FakeHost &host = FakeHost::the();

    Project &p = engine().project_for_load();
    p.patterns[2][0].tracks[0].steps[5].flags = uint8_t(kStepTrig);   // bank C
    p.patterns[0][0].tracks[0].steps[1].flags = uint8_t(kStepTrig);   // bank A
    post(Cmd::project_loaded);
    pump(2);
    post(Cmd::scene_capture, 0, 0, 3, 0);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(scene_count(engine().scenes().scenes[3]) > 0);

    CHECK(host.remove_file(card_path("BANK_C.BIN")));
    CHECK(host.remove_file(card_path("PERFORM.BIN")));
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);          // neither one is an error
    CHECK(host.logged("loaded"));
    CHECK(settle());
    pump(4);

    CHECK_EQ(int(engine().project().patterns[2][0].tracks[0].steps[5].flags), 0);   // empty bank
    CHECK((engine().project().patterns[0][0].tracks[0].steps[1].flags & kStepTrig) != 0);
    CHECK_EQ(scene_count(engine().scenes().scenes[3]), 0);                          // no scenes

    // A missing KIT.BIN is reported.
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(host.remove_file(card_path("KIT.BIN")));
    CHECK(reload_now());
    check_error("a missing KIT.BIN", "NOT FOUND");
    CHECK(settle());
}

TEST(storage_a_load_asked_for_with_no_card_lands_when_one_arrives) {
    // The mirror of tests/test_app.cpp's save: `service()` returns before it starts anything while
    // the card is unusable, so the request STAYS PENDING rather than failing, and it completes by
    // itself when a card appears.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 7);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    post(Cmd::track_erase, 0);
    pump(2);
    CHECK_EQ(int(engine().pattern().tracks[0].steps[7].flags), 0);

    host.set_card(false);
    storage().on_card(false);
    CHECK(!storage().card_usable());
    CHECK_EQ(storage().last_error(), "NO CARD");

    const int reads = host.read_alls();
    storage().request_reload();
    CHECK(!pump_until_idle(200));                 // no job ever started
    CHECK_EQ(host.read_alls(), reads);            // and not a byte was read
    CHECK(!host.logged("loaded"));

    host.set_card(true);
    storage().on_card(true);
    CHECK(settle());
    CHECK(host.logged("loaded"));
    CHECK(host.read_alls() > reads);
    pump(4);
    CHECK((engine().pattern().tracks[0].steps[7].flags & kStepTrig) != 0);
}

TEST(storage_a_load_already_asked_for_is_cancelled_by_pulling_the_card) {
    // The other order, and it is not the same -- `on_card(false)` clears `load_pending_` along
    // with any job in flight. tests/test_app.cpp pins the same asymmetry for a save and carries
    // the question of whether it ought to resume by itself; this is the load half of it.
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    CHECK(pump_until_idle());

    storage().request_reload();                   // asked for FIRST...
    host.set_card(false);
    storage().on_card(false);                     // ...and then the card goes
    CHECK_EQ(storage().last_error(), "NO CARD");

    const int reads = host.read_alls();
    host.set_card(true);
    storage().on_card(true);
    CHECK(settle());
    CHECK(!host.logged("loaded"));                // it did not resume
    CHECK_EQ(host.read_alls(), reads);

    // Asking again reads it.
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("loaded"));
    CHECK(settle());
}

TEST(storage_a_file_call_from_the_audio_context_is_refused_not_retried) {
    // Every Enaga file call is main context; from audio() it returns ENAGA_FILE_WRONG_CONTEXT
    // (<enaga/abi.h>). Nothing in app/storage.cpp should ever reach a file call from the audio
    // side, so this is a guard rather than a path -- and what matters is that the refusal is
    // reported and the job ends, rather than being retried for ever.
    clean();
    FakeHost &host = FakeHost::the();

    host.set_audio_context(true);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK_EQ(storage().last_error(), "WRONG CONTEXT");
    CHECK(host.logged("save failed WRONG CONTEXT"));
    CHECK_EQ(host.file_count(), 0);               // not even the folder was made
    CHECK_EQ(host.folders_made(), 0);
    CHECK(storage().dirty());

    host.set_audio_context(false);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("saved"));
    CHECK(!storage().dirty());
}

TEST(storage_a_save_interrupted_by_a_card_removal_is_not_a_complete_project) {
    // A save that did not finish must not leave `project_complete_` set: the folder holds some of
    // the eleven files and the next save has to write them all again. Getting this wrong writes
    // only the dirty files into a half-written folder, which is the data loss that flag exists to
    // prevent, arriving by another route.
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    pump(3);                                      // started, and a file or two down
    CHECK_EQ(storage().busy_text(), "SAVING");
    const int part = host.file_count();
    CHECK(part > 1);
    CHECK(part < kFileN + 2);

    host.set_card(false);
    storage().on_card(false);
    CHECK(storage().busy_text() == nullptr);      // abandoned where it stood
    CHECK_EQ(storage().last_error(), "NO CARD");
    CHECK_EQ(host.file_count(), part);            // and nothing was written after the removal
    pump(20);
    CHECK_EQ(host.file_count(), part);

    host.set_card(true);
    storage().on_card(true);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    for (int i = 0; i < kFileN; i++) CHECK(host.has_file(card_path(kFiles[i])));

    // And it really is a project: it reads back.
    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
}

// =============================================================================================
// 5. Autosave, and which copy is opened
// =============================================================================================

TEST(storage_an_autosave_writes_every_file_and_leaves_the_project_dirty) {
    // docs/SPEC.md: "Autosave to AUTOSAVE/ whenever the transport stops". It is a SNAPSHOT, not a
    // diff: the dirty flags describe how RAM differs from the PROJECT, so they say nothing about
    // what is in the autosave folder and every file goes down. For the same reason an autosave
    // does not clear them -- the project on the card is still out of date and the screen must keep
    // saying so.
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(!storage().dirty());

    post(Cmd::toggle_trig, 4, 11);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);
    CHECK(storage().dirty());

    const int saves_before = host.saves();
    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("autosaved"));

    for (int i = 0; i < kFileN; i++) {
        if (!host.has_file(card_path(kFiles[i], true))) {
            std::printf("    AUTOSAVE/%s is missing\n", kFiles[i]);
        }
        CHECK(host.has_file(card_path(kFiles[i], true)));
    }
    CHECK_EQ(host.file_bytes(card_path("PROJECT.BIN", true)), 402);
    CHECK_EQ(host.file_bytes(card_path("KIT.BIN", true)), 1184);
    CHECK_EQ(host.file_bytes(card_path("PERFORM.BIN", true)), 3804);

    // Eleven files and SETTINGS.TXT, which carries the counter a later start compares.
    CHECK_EQ(host.saves() - saves_before, kFileN + 1);
    CHECK(host.has_file("/ENAGA/DATA/STRATA/SETTINGS.TXT"));

    // Still dirty: the project folder has not seen this edit.
    CHECK(storage().dirty());

    // And a second autosave with nothing dirty writes everything again, because a snapshot has
    // nothing to compare itself with.
    mark_files(true);
    storage().request_autosave();
    CHECK(pump_until_idle());
    for (int i = 0; i < kFileN; i++) {
        if (!was_written(i, true)) std::printf("    AUTOSAVE/%s was left alone\n", kFiles[i]);
        CHECK(was_written(i, true));
    }
}

TEST(storage_an_autosave_does_not_make_the_project_folder_complete) {
    // The transport stops, which autosaves, and then the user presses SAVE. An autosave says
    // nothing about what is in the PROJECT folder -- it may hold an older project, or nothing --
    // so that save has to write all eleven files. This is what `finish_job` means by setting
    // `project_complete_` only for a save that is not an autosave.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 2);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);

    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(host.logged("autosaved"));
    for (int i = 0; i < kFileN; i++) CHECK(!host.has_file(card_path(kFiles[i])));

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    for (int i = 0; i < kFileN; i++) {
        if (!host.has_file(card_path(kFiles[i]))) {
            std::printf("    %s was not written by the save after an autosave\n", kFiles[i]);
        }
        CHECK(host.has_file(card_path(kFiles[i])));
    }

    // And it is a project, not eleven files: it reads back.
    CHECK(reload_now());
    CHECK(storage().last_error() == nullptr);
    CHECK(settle());
    pump(4);
    CHECK((engine().pattern().tracks[0].steps[2].flags & kStepTrig) != 0);
}

TEST(storage_a_reload_asked_for_during_an_autosave_leaves_that_autosave_in_its_folder) {
    // `request_reload` used to set `autosave_ = false` on the spot. That flag also names the folder
    // a running save writes, so an autosave interrupted by RELOAD wrote its remaining files into
    // PROJECT/ and was then counted as a deliberate save. The load now picks its folder when it
    // starts, after the autosave has finished.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 5);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);

    storage().request_autosave();
    pump(3);                                   // the autosave is under way, a file per pass
    CHECK_EQ(storage().busy_text(), "AUTOSAVING");
    storage().request_reload();
    CHECK(pump_until_idle());
    CHECK(host.logged("autosaved"));

    for (int i = 0; i < kFileN; i++) {
        if (host.has_file(card_path(kFiles[i]))) {
            std::printf("    PROJECT/%s was written by an autosave\n", kFiles[i]);
        }
        CHECK(!host.has_file(card_path(kFiles[i])));
        CHECK(host.has_file(card_path(kFiles[i], true)));
    }
    // And no deliberate save happened, so the project is still unsaved.
    CHECK(storage().dirty());
}

TEST(storage_a_command_the_card_side_posts_into_a_full_ring_is_sent_later_not_dropped) {
    // `request_save` posts `kit_store` from the key press, and the UI may have filled the ring in
    // the same pass. The ring refuses a post when full, and Storage used to ignore the refusal: the
    // save then serialized the kit as of the last store and a knob turn was lost from the file.
    clean();

    post(Cmd::set_param, 3, 0, uint8_t(kLvl), 42);
    pump(2);
    CHECK_EQ(int(kit_value(3, kLvl)), 42);

    // The ring full of an idempotent edit, so only the store is in question.
    Command same;
    same.cmd = Cmd::set_param;
    same.track = 3;
    same.a = uint8_t(kLvl);
    same.b = 42;
    int posted = 0;
    while (engine().commands().post(same)) posted++;
    CHECK(posted > 100);

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(engine().project().kit.tracks[3].params[kLvl]), 42);

    // And it is in the file, not just in RAM.
    post(Cmd::set_param, 3, 0, uint8_t(kLvl), 7);
    pump(2);
    CHECK(reload_now());
    CHECK(settle());
    pump(4);
    CHECK_EQ(int(kit_value(3, kLvl)), 42);
}

TEST(storage_a_save_already_asked_for_beats_an_autosave) {
    // Both arrive before either can start -- the transport stops at the same moment the user
    // presses SAVE. `request_autosave` stands down while a save is pending, and `service()` takes
    // the save, so the deliberate one wins and the project folder is the one that is written.
    clean();
    FakeHost &host = FakeHost::the();

    storage().request_save();
    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("saved"));
    CHECK(!host.logged("autosaved"));
    CHECK(host.has_file(card_path("KIT.BIN")));
    CHECK(!host.has_file(card_path("KIT.BIN", true)));

    // And it does not happen afterwards either: the autosave was stood down, not deferred.
    CHECK(settle());
    CHECK(!host.logged("autosaved"));
    CHECK(!host.has_file(card_path("KIT.BIN", true)));
}

TEST(storage_open_last_prefers_whichever_copy_is_newer) {
    // There is no timestamp in `files::Info` -- it carries a name, a size and flags -- so "newer"
    // is a counter STRATA keeps in its own settings and bumps on every save of either kind.
    clean();

    // The project, with one value...
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 21);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());

    // ...and then the autosave, with another. The autosave is now the newer of the two.
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 99);
    pump(2);
    storage().request_autosave();
    CHECK(pump_until_idle());

    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 7);
    pump(2);
    CHECK(storage().open_last());
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 99);        // the autosave was offered and taken

    // The other way round: a deliberate save after the autosave makes the project folder newer.
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 33);
    pump(2);
    storage().note_command(Cmd::set_param, 0);
    storage().request_save();
    CHECK(pump_until_idle());

    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 7);
    pump(2);
    CHECK(storage().open_last());
    CHECK(settle());
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 33);
}

TEST(storage_open_last_is_false_when_the_card_holds_nothing) {
    // A first run, which is not an error: the engine's built-in default pattern stays and nothing
    // is reported on screen.
    clean();
    FakeHost &host = FakeHost::the();

    CHECK_EQ(host.file_count(), 0);
    CHECK(!storage().open_last());
    CHECK(storage().last_error() == nullptr);
    CHECK(!pump_until_idle(100));                 // and no load was queued by the attempt
    CHECK_EQ(host.read_alls(), 0);
}

TEST(storage_save_for_shutdown_writes_the_autosave_in_one_call) {
    // App::stop has 3 s including saves and destructors never run on this platform, so this is the
    // last chance to keep the user's work. It is the one blocking path in app/storage.cpp: no
    // passes, no slices.
    //
    // An empty ring here; the next test has one with edits still in it.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 64);
    pump(4);
    CHECK(engine().commands().empty());

    const int saves_before = host.saves();
    storage().save_for_shutdown();

    CHECK(host.logged("autosaved"));
    CHECK(storage().busy_text() == nullptr);      // it ran to the end inside the one call
    for (int i = 0; i < kFileN; i++) CHECK(host.has_file(card_path(kFiles[i], true)));
    CHECK_EQ(host.saves() - saves_before, kFileN + 1);

    // With no card it does nothing at all, rather than failing eleven times on the way out.
    host.set_card(false);
    storage().on_card(false);
    const int after = host.saves();
    storage().save_for_shutdown();
    CHECK_EQ(host.saves(), after);
}

TEST(storage_save_for_shutdown_applies_the_edits_still_in_the_ring) {
    // On the unit nothing drains the ring during App::stop: Enaga removes the audio callback
    // before it calls stop(). The shutdown save posts its own kit_store, and it used to spin
    // 100 ms waiting for an audio side that never came, then save a kit without it -- the knob
    // turned just before switching app was not in the autosave.
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 77);  // posted, and no block runs after it
    CHECK(!engine().commands().empty());

    storage().save_for_shutdown();
    CHECK(engine().commands().empty());

    static Kit kit;
    const int32_t got = host.read_file(card_path("KIT.BIN", true), g_scratch, kScratchBytes);
    CHECK(got > 0);
    CHECK(kit_read(kit, g_scratch, uint32_t(got)) == SerializeResult::ok);
    CHECK_EQ(int(kit.tracks[0].params[kLvl]), 77);
}

// =============================================================================================
// 6. The WAV loader
// =============================================================================================

TEST(storage_a_wav_at_48k_loads_in_mono_and_in_stereo) {
    clean();

    make_wav("/SAMPLES/M48.WAV", 48000, 1, 16, 2400);
    kit_sample(kLoopTrack, "/SAMPLES/M48.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 2400u);
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 1);
    CHECK_EQ(slot_of(kLoopTrack).rate, uint32_t(ENAGA_SAMPLE_RATE));
    CHECK_EQ(slot_of(kLoopTrack).bytes, 2400u * 2u);
    CHECK_EQ(slot_of(kLoopTrack).offset % 2u, 0u);     // an odd offset is refused by the audio side
    CHECK_EQ(storage().loading_track(), -1);

    make_wav("/SAMPLES/S48.WAV", 48000, 2, 16, 1800);
    kit_sample(kLoopTrack, "/SAMPLES/S48.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 1800u);
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 2);
    CHECK_EQ(slot_of(kLoopTrack).bytes, 1800u * 2u * 2u);   // interleaved, two bytes a sample
}

TEST(storage_a_loaded_wav_is_heard) {
    // The one end-to-end check that the bytes really arrived in the region the audio side reads:
    // the converted int16 is private to Storage, and the handover is an offset, a frame count and
    // a channel count that `Cmd::set_sample` checks against the region before forming a pointer.
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/HEARD.WAV", 48000, 1, 16, 9600);     // 200 ms, comfortably longer than a block
    kit_sample(kLoopTrack, "/SAMPLES/HEARD.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    // The SAMPLE machine's own parameters (dsp/track.hpp: SRC is PIT, STA, LEN, SLC; AMP is ATK,
    // DEC, PLY, LVL). `fresh()` leaves every knob at 64, which starts playback half way in and
    // cuts it into nine slices; one slice from the start is what this wants to hear.
    post(Cmd::set_param, kLoopTrack, 0, 1, 0);        // STA: the beginning
    post(Cmd::set_param, kLoopTrack, 0, 2, 127);      // LEN: all of it
    post(Cmd::set_param, kLoopTrack, 0, 3, 0);        // SLC: one slice
    post(Cmd::set_param, kLoopTrack, 0, 8, 0);        // ATK: instant
    post(Cmd::set_param, kLoopTrack, 0, 9, 110);      // DEC: long
    post(Cmd::set_param, kLoopTrack, 0, 10, 0);       // PLY: forward, one shot
    // And the sends closed. `fresh()` leaves SNA and SNB at 64, so a loud sample feeds the delay
    // and the reverb and they go on repeating it for hundreds of milliseconds -- which is a fine
    // thing for the instrument and useless for a test that wants to know when the track is silent.
    post(Cmd::set_param, kLoopTrack, 0, 14, 0);       // SNA: the delay send
    post(Cmd::set_param, kLoopTrack, 0, 15, 0);       // SNB: the reverb send
    post(Cmd::toggle_trig, uint8_t(kLoopTrack), 0);
    pump(2);

    host.set_running(true);
    host.deliver_tick(0, 0);
    using strata::test::kRigStereo;
    using strata::test::last_block;
    static float heard[kRigStereo];
    float peak = pump(1);
    for (int i = 0; i < kRigStereo; i++) heard[i] = last_block()[i];
    peak = std::fmax(peak, pump(5));
    CHECK(peak > 0.01f);

    // And with the sample taken away the SAME step plays something else: a track with no file
    // falls back to the placeholder every Sampler had before a card was read (app/engine.cpp,
    // Cmd::clear_sample), so what was heard above was the file and not that. The step has to
    // fire for this to mean anything, so the trig counter is checked too, and the master reverb
    // is given time to decay first.
    post(Cmd::clear_sample, uint8_t(kLoopTrack));
    pump(40);
    CHECK(engine().machine(kLoopTrack) != nullptr);

    const uint8_t fired = engine().status().triggered[kLoopTrack].load(std::memory_order_relaxed);
    host.deliver_tick(24u * 64u, 0);                  // tick 1536 is step 0 again
    pump(1);
    float differs = 0.0f;
    for (int i = 0; i < kRigStereo; i++) {
        differs = std::fmax(differs, std::fabs(last_block()[i] - heard[i]));
    }
    pump(5);

    CHECK(engine().status().triggered[kLoopTrack].load(std::memory_order_relaxed) != fired);
    CHECK(differs > 0.01f);                           // not the file any more
}

TEST(storage_a_wav_at_another_rate_is_resampled_once_at_load) {
    // docs/SPEC.md: "Files not at 48 kHz are resampled once at load, in main-loop slices." The
    // frame count a track ends up with is the resampler's own answer, and 24 kHz to 48 kHz is an
    // exact doubling, so it can be predicted by hand as well as by core/pcm.
    clean();

    make_wav("/SAMPLES/UP24.WAV", 24000, 1, 16, 1000);
    kit_sample(kLoopTrack, "/SAMPLES/UP24.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 2000u);
    CHECK_EQ(slot_of(kLoopTrack).frames, Resampler::make(24000, 48000).output_frames(1000));
    // The track is handed the ENGINE's rate, not the file's: the resample already happened.
    CHECK_EQ(slot_of(kLoopTrack).rate, uint32_t(ENAGA_SAMPLE_RATE));
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 1);

    // And downwards, in stereo.
    make_wav("/SAMPLES/DN96.WAV", 96000, 2, 16, 1000);
    kit_sample(kLoopTrack, "/SAMPLES/DN96.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 500u);
    CHECK_EQ(slot_of(kLoopTrack).frames, Resampler::make(96000, 48000).output_frames(1000));
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 2);
    CHECK_EQ(slot_of(kLoopTrack).rate, uint32_t(ENAGA_SAMPLE_RATE));

    // 44.1 kHz, the one that actually turns up, through the same path.
    make_wav("/SAMPLES/CD441.WAV", 44100, 2, 16, 1000);
    kit_sample(kLoopTrack, "/SAMPLES/CD441.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, Resampler::make(44100, 48000).output_frames(1000));
    CHECK(slot_of(kLoopTrack).frames > 1000u);
}

// A WAV too big for g_scratch: 30 s of 96 kHz mono 8-bit is 2.88 MB. Its own buffer, file scope
// (the frame rule), sized for one frame past 30 s.
constexpr uint32_t kLong96Frames = kMaxSampleSeconds * 96000u;
uint8_t g_long_wav[44u + kLong96Frames + 1u];

uint32_t make_long_wav(const char *path, uint32_t frames) {
    enaga::wav::Format format;
    format.encoding = uint16_t(enaga::wav::pcm);
    format.channels = 1;
    format.rate = 96000;
    format.bits = 8;
    enaga::wav::header(g_long_wav, format, frames);
    for (uint32_t f = 0; f < frames; f++) g_long_wav[44u + f] = (f / 64u) % 2u ? 0x20u : 0xE0u;
    CHECK(FakeHost::the().write_file(path, g_long_wav, 44u + frames));
    return 44u + frames;
}

TEST(storage_thirty_seconds_of_a_96k_file_loads) {
    // Roland, 2026-11-22: the 30 s limit is 30 s of the FILE. The loader's resample buffer used
    // to count 30 s of 48 kHz frames, so a 96 kHz file was refused TOO LONG past 15 s. Mono
    // 8-bit keeps the file inside the fake card; the frame count is what the limit reads.
    clean();
    make_long_wav("/SAMPLES/LONG96.WAV", kLong96Frames + 1u);     // one frame past 30 s
    kit_sample(kLoopTrack, "/SAMPLES/LONG96.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() != nullptr);
    CHECK_EQ(storage().last_error(), "TOO LONG");
    CHECK(slot_of(kLoopTrack).state != SampleState::ready);

    make_long_wav("/SAMPLES/LONG96.WAV", kLong96Frames);          // exactly 30 s, in place
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, Resampler::make(96000, 48000).output_frames(kLong96Frames));
    CHECK_EQ(slot_of(kLoopTrack).frames, kMaxSampleSeconds * 48000u);   // 30 s at 48 kHz
    FakeHost::the().reset();                                     // give the arena back
}

TEST(storage_a_downsampled_wav_is_low_passed_before_it_is_resampled) {
    // The loader's bytes are what core/pcm's low-pass then resample give for the same square
    // wave, and not what the resample alone gives: the filter is wired into the convert step.
    // That the result does not depend on the slicing is core/pcm's test.
    clean();
    constexpr uint32_t kFrames = 4800;
    make_wav("/SAMPLES/AA96.WAV", 96000, 2, 16, kFrames);
    kit_sample(kLoopTrack, "/SAMPLES/AA96.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    pump(2);

    static int16_t source[kFrames * 2];
    static int16_t filtered[kFrames * 2];
    static int16_t plain[kFrames * 2];
    for (uint32_t f = 0; f < kFrames; f++) {
        source[f * 2] = source[f * 2 + 1] = (f / 32u) % 2u == 0u ? 12000 : -12000;
    }
    Resampler cursor = Resampler::make(96000, 48000);
    const uint32_t n_plain = pcm_resample(source, kFrames, 2, cursor, plain, kFrames);
    Antialias lowpass = Antialias::make(96000, 48000);
    pcm_antialias(source, kFrames, 2, lowpass);
    cursor = Resampler::make(96000, 48000);
    const uint32_t n = pcm_resample(source, kFrames, 2, cursor, filtered, kFrames);
    CHECK_EQ(n, n_plain);

    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK_EQ(uint32_t(loop->sample_frames()), n);
    const int16_t *pcm = loop->sample_data();
    CHECK(pcm != nullptr);
    if (pcm != nullptr && uint32_t(loop->sample_frames()) == n) {
        CHECK(std::memcmp(pcm, filtered, n * 2 * sizeof(int16_t)) == 0);
        CHECK(std::memcmp(pcm, plain, n * 2 * sizeof(int16_t)) != 0);
    }
}

TEST(storage_a_chunk_after_the_data_is_not_counted_as_audio) {
    // A WAV may carry chunks AFTER its data -- LIST, cue and id3 are all common -- and those bytes
    // are not audio: played, they are a burst of noise on the end of the sample. The frame count
    // has to come from the data chunk's own length and not from "everything after the header",
    // which is the bug this pins. The trailing chunk here is 400 bytes, which at two bytes a mono
    // frame would add 200 frames to a 500-frame file.
    clean();

    make_wav("/SAMPLES/LIST.WAV", 48000, 1, 16, 500, "LIST", 400);
    kit_sample(kLoopTrack, "/SAMPLES/LIST.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 500u);

    make_wav("/SAMPLES/CUE.WAV", 48000, 2, 16, 300, "cue ", 120);
    kit_sample(kLoopTrack, "/SAMPLES/CUE.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 300u);
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 2);

    // The fallback, and the reason it is a fallback: <enaga/wav.hpp> notes that a header still
    // being written may report a data size of 0, and then the rest of the file IS the audio.
    make_wav("/SAMPLES/OPEN.WAV", 48000, 1, 16, 250, nullptr, 0, false, false);
    kit_sample(kLoopTrack, "/SAMPLES/OPEN.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 250u);
}

TEST(storage_a_24_bit_and_a_float_wav_load) {
    // The formats docs/SPEC.md lists for user samples ("WAV 16/24/32-bit"). core/pcm does the
    // arithmetic and tests/test_pcm.cpp checks it; what is checked here is that the loader picks
    // the right one from the header and sizes the reservation from the ENGINE's format and not
    // the file's.
    clean();

    make_wav("/SAMPLES/B24.WAV", 48000, 2, 24, 700);
    kit_sample(kLoopTrack, "/SAMPLES/B24.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 700u);
    CHECK_EQ(slot_of(kLoopTrack).bytes, 700u * 2u * 2u);    // int16 in the region, not 24-bit

    make_wav("/SAMPLES/F32.WAV", 48000, 1, 32, 600, nullptr, 0, true);
    kit_sample(kLoopTrack, "/SAMPLES/F32.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 600u);
    CHECK_EQ(slot_of(kLoopTrack).bytes, 600u * 2u);
}

TEST(storage_a_file_that_is_not_a_wav_is_refused) {
    clean();
    FakeHost &host = FakeHost::the();

    for (int i = 0; i < 64; i++) g_scratch[i] = uint8_t(0xA5u ^ i);
    store("/SAMPLES/NOT.WAV", 64);
    kit_sample(kLoopTrack, "/SAMPLES/NOT.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "NOT A WAV");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(storage().loading_track(), -1);
    CHECK_EQ(slot_of(kLoopTrack).bytes, 0u);        // nothing of the budget is held by a failure

    // A failed track does not stop the next one: the loader takes one job at a time and a refusal
    // ends that job, not the queue.
    make_wav("/SAMPLES/GOOD.WAV", 48000, 1, 16, 400);
    kit_sample(kLoopTrack, "/SAMPLES/GOOD.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 400u);
    CHECK(host.loads() >= 2);
}

TEST(storage_more_than_two_channels_is_refused) {
    // dsp/sample.cpp plays mono and stereo. More channels are a refusable file rather than a guess
    // at which two of them the user meant.
    clean();

    make_wav("/SAMPLES/CH3.WAV", 48000, 3, 16, 300);
    kit_sample(kLoopTrack, "/SAMPLES/CH3.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "TOO MANY CH");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));

    make_wav("/SAMPLES/CH6.WAV", 48000, 6, 16, 200);
    kit_sample(kLoopTrack, "/SAMPLES/CH6.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "TOO MANY CH");

    // Two is still fine, so the refusal is about the count and not about the parser.
    make_wav("/SAMPLES/CH2.WAV", 48000, 2, 16, 200);
    kit_sample(kLoopTrack, "/SAMPLES/CH2.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).channels), 2);
}

TEST(storage_a_wav_with_no_samples_in_it_is_refused) {
    clean();

    // A header and nothing after it. The data chunk starts where the file ends, which the loader
    // catches before it can compute a frame count from it.
    make_wav("/SAMPLES/EMPTY.WAV", 48000, 2, 16, 0);
    kit_sample(kLoopTrack, "/SAMPLES/EMPTY.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "BAD FORMAT");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));

    // Less than one whole frame of data, which is a different branch: there are bytes, and they do
    // not add up to a frame.
    make_wav("/SAMPLES/SHORT.WAV", 48000, 2, 16, 0, nullptr, 0, false, false, 3);
    kit_sample(kLoopTrack, "/SAMPLES/SHORT.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "EMPTY");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
}

TEST(storage_request_sample_refuses_what_it_cannot_load) {
    clean();

    // A track whose kit names no file is not a job: `request_all_samples` leans on this, and so
    // does the UI, which has nothing to say about a track with no sample.
    CHECK(!storage().request_sample(0));
    CHECK(!storage().request_sample(kLoopTrack));
    // Not a track at all.
    CHECK(!storage().request_sample(-1));
    CHECK(!storage().request_sample(kTracks));
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(FakeHost::the().loads(), 0);

    // A named file that is not there IS a job, and it fails with the host's own reason.
    kit_sample(kLoopTrack, "/SAMPLES/GONE.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle());
    CHECK_EQ(storage().last_error(), "NOT FOUND");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
}

// =============================================================================================
// 7. The sample swap: the epoch handshake
// =============================================================================================

TEST(storage_a_sample_swap_releases_the_memory_only_after_the_epoch_moves) {
    // docs/SPEC.md's generation-counter handoff, one state at a time. `SamplePool::release` makes
    // memory reusable immediately and core/ deliberately cannot tell whether a voice is still
    // reading it, so the order is: post `clear_sample`, WAIT for Status::sample_epoch to change,
    // release, load, hand over. Releasing before the epoch confirms means writing over memory a
    // voice is reading.
    clean();

    make_wav("/SAMPLES/SWAP1.WAV", 48000, 1, 16, 300);
    make_wav("/SAMPLES/SWAP2.WAV", 48000, 1, 16, 500);
    kit_sample(kLoopTrack, "/SAMPLES/SWAP1.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 300u);

    kit_sample(kLoopTrack, "/SAMPLES/SWAP2.WAV");
    const uint8_t epoch_before =
        engine().status().sample_epoch[kLoopTrack].load(std::memory_order_relaxed);
    CHECK(storage().request_sample(kLoopTrack));

    // Pass one, card side only: `clear_sample` is posted and the epoch it is waiting on is
    // remembered. The audio side has not run since, so nothing has been confirmed...
    storage().service();
    CHECK_EQ(storage().busy_text(), "SWAPPING");
    CHECK_EQ(storage().loading_track(), kLoopTrack);
    CHECK_EQ(int(engine().status().sample_epoch[kLoopTrack].load(std::memory_order_relaxed)),
             int(epoch_before));
    // ...and the memory is still the track's, with the old sample still in it.
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 300u);

    // Any number of card passes change nothing: the wait is for the AUDIO side.
    for (int i = 0; i < 20; i++) storage().service();
    CHECK_EQ(storage().busy_text(), "SWAPPING");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(FakeHost::the().loads(), 1);          // and no read has been started

    // One pass of both: the audio side applies `clear_sample` and bumps the epoch, and the card
    // side sees the change, releases the range and starts the read. The slot is EMPTY in that
    // window -- released, not yet reserved -- which is the proof that the release waited.
    pump_held();
    CHECK(int(engine().status().sample_epoch[kLoopTrack].load(std::memory_order_relaxed)) !=
          int(epoch_before));
    CHECK_EQ(storage().busy_text(), "READING");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::empty));
    CHECK_EQ(slot_of(kLoopTrack).bytes, 0u);
    CHECK_EQ(FakeHost::the().pending_loads(), 1);
    CHECK_EQ(FakeHost::the().loads(), 2);

    // The read completes, the conversion runs, and the new sample is handed over.
    FakeHost::the().complete_loads();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 500u);
}

TEST(storage_the_transport_runs_through_a_held_sample_load) {
    // The whole point of loading in the background: a sample arriving must not stop the music.
    // `loading_project()` is deliberately false for a sample job -- it is only true for the job
    // that rewrites pattern memory -- so PLAY stays available, and the epoch handshake is what
    // makes that safe.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 0);                  // a kick to listen for
    pump(2);

    make_wav("/SAMPLES/HELD.WAV", 48000, 2, 16, 4800);
    kit_sample(kLoopTrack, "/SAMPLES/HELD.WAV");
    CHECK(storage().request_sample(kLoopTrack));

    host.set_running(true);
    pump_held(3);                                  // start the swap, see the epoch, begin the read
    CHECK_EQ(storage().busy_text(), "READING");
    CHECK_EQ(host.pending_loads(), 1);
    CHECK(!storage().loading_project());

    const uint32_t blocks_before = engine().status().blocks.load(std::memory_order_relaxed);
    host.deliver_tick(0, 0);
    const float peak = pump_held(4);
    CHECK(peak > 0.01f);                           // the sequencer played, mid-load
    CHECK(engine().status().blocks.load(std::memory_order_relaxed) > blocks_before);
    CHECK_EQ(host.pending_loads(), 1);             // and the load really was still in flight

    host.complete_loads();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 4800u);
}

TEST(storage_a_request_made_while_a_job_runs_is_queued_and_not_dropped) {
    // One job at a time (app/storage.hpp), and EVERY request now queues rather than vanishing.
    //
    // This test pinned the opposite, and it was a real bug: `request_save`, `request_autosave`,
    // `request_reload` and `request_new` each returned early and silently while a job was in
    // flight -- and a background sample load is the normal state for a second or two after every
    // project open, while app/app.cpp announces SAVING, RELOADING or NEW PROJECT whatever the
    // call does. So pressing SAVE just after opening a project did nothing and said it had
    // worked. app/storage.hpp claimed requests queue; now they do, and `service()` serialises
    // them because it starts a job only when it is idle.
    //
    // A deliberate SAVE still supersedes an autosave, which is the one ordering rule left.
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/BUSY.WAV", 48000, 1, 16, 2400);
    kit_sample(kLoopTrack, "/SAMPLES/BUSY.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    pump_held(3);
    CHECK_EQ(storage().busy_text(), "READING");

    const int saves_before = host.saves();
    const int reads_before = host.read_alls();
    char name_bytes[Storage::kNameBytes];
    std::strcpy(name_bytes, storage().project_name());
    const char *name_before = name_bytes;    // a pointer, so CHECK_EQ compares the CONTENT

    // A save asked for NOW, while the read is in flight.
    storage().request_save();
    CHECK_EQ(storage().busy_text(), "READING");    // it did not displace the job
    CHECK_EQ(host.saves(), saves_before);          // and nothing is written yet

    // The read finishes, and the save that was waiting runs by itself.
    host.complete_loads();
    CHECK(pump_until_idle());
    CHECK(host.saves() > saves_before);
    CHECK(host.logged("saved"));
    CHECK(storage().last_error() == nullptr);

    // The sample arrived too: queueing the save did not cost the read.
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    // And the project was not renamed by anything above -- `request_new` was not asked for here,
    // because it is destructive and this test is about a save surviving a wait.
    CHECK_EQ(storage().project_name(), name_before);
    CHECK(host.read_alls() >= reads_before);
}

TEST(storage_a_card_sample_reaches_both_sample_machines_of_a_track) {
    // Every track owns a Sampler and a Grain, and `clear_sample` / `set_sample` used to reach only
    // the one the track hosted. So GRAIN never got a card sample, and a track switched away from
    // SAMPLE kept a pointer into a range the loader then gave to another file.
    clean();
    make_wav("/SAMPLES/ONE.WAV", 48000, 1, 16, 2400);
    kit_sample(kLoopTrack, "/SAMPLES/ONE.WAV");
    CHECK(load_sample(kLoopTrack));
    pump(2);
    const Sampler *sampler = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    const int16_t *first = sampler->sample_data();
    CHECK(first != nullptr);
    CHECK_EQ(sampler->sample_frames(), 2400);

    // To GRAIN: it plays the same sample, not the placeholder.
    post(Cmd::set_machine, uint8_t(kLoopTrack), 0, uint8_t(MachineId::grain));
    pump(2);
    const Granular *grain = static_cast<const Granular *>(engine().machine(kLoopTrack));
    CHECK(grain->sample_data() == first);
    CHECK_EQ(grain->sample_frames(), 2400);

    // A new file while the track hosts GRAIN, then back to SAMPLE: the Sampler has the new one.
    make_wav("/SAMPLES/TWO.WAV", 48000, 1, 16, 1200);
    kit_sample(kLoopTrack, "/SAMPLES/TWO.WAV");
    CHECK(load_sample(kLoopTrack));
    pump(2);
    CHECK_EQ(grain->sample_frames(), 1200);
    post(Cmd::set_machine, uint8_t(kLoopTrack), 0, uint8_t(MachineId::sample));
    pump(2);
    CHECK(engine().machine(kLoopTrack) == sampler);
    CHECK_EQ(sampler->sample_frames(), 1200);
    CHECK(sampler->sample_data() == grain->sample_data());
}

TEST(storage_the_handover_carries_the_frame_count_and_the_channels_intact) {
    // The only three things that cross from the loader to the audio side: a byte offset, a frame
    // count in 24 bits and a channel count (app/commands.hpp). A truncated frame count would play
    // a fraction of the file and look like a decode bug, and a channel count of 1 for a stereo
    // file would play it at half speed -- so both are checked against what the Sampler ended up
    // holding rather than against the pool, which is the loader's own bookkeeping.
    clean();

    // 70,000 frames is 0x011170: it needs all three of the command's frame bytes, and more than
    // one conversion slice (`kSliceFrames` is 48,000).
    make_wav("/SAMPLES/LONG.WAV", 48000, 1, 16, 70000);
    kit_sample(kLoopTrack, "/SAMPLES/LONG.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(slot_of(kLoopTrack).frames, 70000u);
    pump(2);                                        // the handover command lands on the audio side

    // Track 7 hosts SAMPLE in the default kit, so `machines_[7]` is its Sampler (app/engine.cpp)
    // and the cast is to the type the engine put there. No dynamic_cast: this build has no RTTI.
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK(loop->has_sample());
    CHECK_EQ(loop->sample_frames(), 70000);
    CHECK_EQ(loop->sample_channels(), 1);
    CHECK(loop->sample_data() != nullptr);

    // In stereo, where a channel count of 1 would play the file at half speed -- and with every
    // frame distinguishable, so the bytes themselves can be checked where they landed. This is
    // the one place the converted PCM is reachable: `Storage::region_` is private, but the
    // Sampler was handed a pointer into it and dsp/sample.hpp exposes it for tests.
    g_ramp = true;
    make_wav("/SAMPLES/ST2.WAV", 48000, 2, 16, 1500);
    g_ramp = false;
    kit_sample(kLoopTrack, "/SAMPLES/ST2.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    pump(2);
    CHECK_EQ(loop->sample_frames(), 1500);
    CHECK_EQ(loop->sample_channels(), 2);

    const int16_t *pcm = loop->sample_data();
    CHECK(pcm != nullptr);
    if (pcm != nullptr) {
        // Frame f is f + 1 on the left and -(f + 1) on the right, interleaved. One frame off --
        // an offset handed over two bytes out, or a conversion that started at the wrong place --
        // shows here and nowhere else.
        CHECK_EQ(int(pcm[0]), 1);
        CHECK_EQ(int(pcm[1]), -1);
        CHECK_EQ(int(pcm[2]), 2);
        CHECK_EQ(int(pcm[3]), -2);
        CHECK_EQ(int(pcm[2 * 999]), 1000);
        CHECK_EQ(int(pcm[2 * 1499]), 1500);
        CHECK_EQ(int(pcm[2 * 1499 + 1]), -1500);
    }

    // And a swap away from it leaves the track holding the placeholder, outside the region,
    // rather than a pointer into memory that is about to be reused.
    post(Cmd::clear_sample, uint8_t(kLoopTrack));
    pump(2);
    CHECK(loop->sample_data() == engine().placeholder());
}

TEST(storage_a_read_that_fails_after_it_was_queued_fails_that_track) {
    // `files::load` reports its result through the callback, so a card that goes bad after the
    // read was accepted comes back as an error on the completion rather than from the call. The
    // loader has to read that result: a sample whose read failed must leave the track `failed` and
    // say so, not convert whatever happened to be in the staging buffer.
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/BADREAD.WAV", 48000, 1, 16, 900);
    kit_sample(kLoopTrack, "/SAMPLES/BADREAD.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    pump_held(3);
    CHECK_EQ(storage().busy_text(), "READING");
    CHECK_EQ(host.pending_loads(), 1);

    host.fail_pending_loads(ENAGA_FILE_CARD_ERROR);
    CHECK(settle());
    check_error("a read that failed after it was queued", "CARD ERROR");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(slot_of(kLoopTrack).bytes, 0u);
    CHECK_EQ(storage().loading_track(), -1);

    // And the loader is usable again: the next request reads and converts normally.
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    // Track 0 the same way. `sample_track_ >= 0` became `> 0` without a test noticing, because
    // every loader test used track 7: track 0's slot was left LOADING for ever.
    make_wav("/SAMPLES/BADREAD0.WAV", 48000, 1, 16, 900);
    kit_sample(0, "/SAMPLES/BADREAD0.WAV");
    CHECK(storage().request_sample(0));
    pump_held(3);
    CHECK_EQ(storage().loading_track(), 0);
    host.fail_pending_loads(ENAGA_FILE_CARD_ERROR);
    CHECK(settle());
    CHECK_EQ(int(slot_of(0).state), int(SampleState::failed));
    kit_sample(0, "");
    CHECK_EQ(slot_of(kLoopTrack).frames, 900u);
}

TEST(storage_a_sample_swap_gives_up_if_the_epoch_never_moves) {
    // A track whose epoch never changes must not hang the loader for ever. `kEpochPasses` is 512
    // and `update()` runs at least 750 times a second, so the wait is a fraction of a second --
    // long enough for any real block, short enough that the queue moves on.
    //
    // The audio side is simply never run here, which is the only way to hold the epoch still.
    clean();

    make_wav("/SAMPLES/STUCK.WAV", 48000, 1, 16, 300);
    kit_sample(kLoopTrack, "/SAMPLES/STUCK.WAV");
    CHECK(storage().request_sample(kLoopTrack));

    storage().service();                           // the pass that starts the job
    CHECK_EQ(storage().busy_text(), "SWAPPING");
    for (int i = 0; i < 511; i++) storage().service();
    CHECK_EQ(storage().busy_text(), "SWAPPING");   // 511 waits is still waiting
    storage().service();                           // the 512th gives up
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(storage().last_error(), "NO SWAP");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(storage().loading_track(), -1);
    CHECK_EQ(FakeHost::the().loads(), 0);          // the file was never opened

    // And the loader is usable again afterwards.
    pump(4);
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
}

TEST(storage_after_no_swap_nothing_new_starts_until_the_clear_is_applied) {
    // NO SWAP gives the track's range back WITHOUT the epoch's confirmation, which on its face
    // is the one thing the swap protocol forbids: a voice may still be reading it. What makes it
    // safe is that the `clear_sample` it posted is still in the ring, and the card side starts
    // nothing -- no load, no save, no other swap -- while the ring is non-empty. So the audio side
    // applies the clear, and kills the voice, before anything can write that memory again. This
    // test pins that ordering, because removing either half would bring the bug back silently.
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/STUCK.WAV", 48000, 1, 16, 300);
    make_wav("/SAMPLES/NEXT.WAV", 48000, 1, 16, 300);
    kit_sample(kLoopTrack, "/SAMPLES/STUCK.WAV");
    kit_sample(kLoopTrack - 1, "/SAMPLES/NEXT.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    for (int i = 0; i < 513; i++) storage().service();
    CHECK_EQ(storage().last_error(), "NO SWAP");
    CHECK(!engine().commands().empty());             // the clear is still waiting

    // Another track asks. The card side alone, as long as it likes: nothing starts.
    CHECK(storage().request_sample(kLoopTrack - 1));
    for (int i = 0; i < 600; i++) storage().service();
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(host.loads(), 0);

    // The audio side drains the ring -- the stuck track's clear included -- and only then does
    // the next swap run.
    CHECK(settle());
    CHECK(host.loads() > 0);
    CHECK_EQ(int(slot_of(kLoopTrack - 1).state), int(SampleState::ready));
}

TEST(storage_a_card_pulled_mid_read_holds_the_buffer_until_the_callback) {
    // `raw_` belongs to Enaga until the callback, which still arrives after a removal -- with an
    // error. So the job is abandoned and the track fails, but nothing may start another read until
    // that callback has come back, or the second read would be writing into a buffer the first one
    // still owns.
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/PULL.WAV", 48000, 1, 16, 1200);
    kit_sample(kLoopTrack, "/SAMPLES/PULL.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    pump_held(3);
    CHECK_EQ(storage().busy_text(), "READING");
    CHECK_EQ(host.pending_loads(), 1);

    host.set_card(false);
    storage().on_card(false);
    CHECK(storage().busy_text() == nullptr);
    CHECK_EQ(storage().last_error(), "NO CARD");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(storage().loading_track(), -1);

    // The card comes back, which re-offers every sample the kit names...
    const int loads_before = host.loads();
    host.set_card(true);
    storage().on_card(true);
    for (int i = 0; i < 40; i++) pump_held();
    // ...and the loader waits, because the old read still owns the staging buffer.
    CHECK_EQ(host.loads(), loads_before);
    CHECK_EQ(host.pending_loads(), 1);
    CHECK(storage().busy_text() == nullptr);

    // The stale callback arrives and the new read can start.
    host.complete_loads();
    CHECK(settle());
    CHECK_EQ(host.loads(), loads_before + 1);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 1200u);
}

TEST(storage_a_project_load_takes_every_track_through_the_swap) {
    // A load replaces the kit, so every track's sample has to come off it -- not just the ones the
    // new kit names a file for. `pool_.reset()` would be wrong: it forgets every reservation while
    // the samplers are still holding pointers into those ranges. So each track goes through
    // clear -> epoch -> release in turn, and a track whose new kit names no file stops there.
    clean();

    make_wav("/SAMPLES/BEFORE.WAV", 48000, 1, 16, 800);
    kit_sample(kLoopTrack, "/SAMPLES/BEFORE.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    // The project that is about to be saved names NO sample, so the one in RAM has to come off
    // the track when it is loaded. `Cmd::kit_store` deliberately leaves `KitTrack::sample` alone
    // (it is not derived from anything the engine owns), so clearing it here is what the kit a
    // file holds really says.
    kit_sample(kLoopTrack, "");
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));   // still loaded in RAM

    uint8_t epochs[kTracks];
    for (int t = 0; t < kTracks; t++) {
        epochs[t] = engine().status().sample_epoch[t].load(std::memory_order_relaxed);
    }

    // Every track should end up empty rather than still holding its old range.
    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);

    for (int t = 0; t < kTracks; t++) {
        const uint8_t now = engine().status().sample_epoch[t].load(std::memory_order_relaxed);
        if (now == epochs[t]) std::printf("    track %d never went through the swap\n", t);
        CHECK(now != epochs[t]);
    }
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::empty));
    CHECK_EQ(storage().samples().count_in(SampleState::ready), 0);
    CHECK_EQ(storage().samples().used(), 0u);
}

TEST(storage_a_loaded_project_applies_its_sample_budget) {
    // docs/SPEC.md: 32 MiB by default, "raisable per project to the 64 MiB the memory budget
    // allows". The new budget cannot be applied while the load is running --
    // `SamplePool::set_budget` refuses to shrink below what is already reserved rather than
    // stranding a loaded sample -- so it waits until every track has been through the swap above.
    clean();
    // Whatever it is now, rather than the default: the pool is not reset between tests, and a
    // budget this test did not choose is not this test's business.
    const uint32_t before = storage().samples().budget();

    Project &p = engine().project_for_load();
    p.settings.sample_budget_mib = 48;
    post(Cmd::project_loaded);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK_EQ(storage().samples().budget(), before);        // saving changes nothing

    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(engine().project().settings.sample_budget_mib), 48);
    CHECK_EQ(storage().samples().budget(), 48u << 20);

    // And down again, because the swap released everything before the budget was applied. (Any
    // PROJECT.BIN command marks the file dirty; the table at the top of this file is what pins
    // which ones they are.)
    Project &back = engine().project_for_load();
    back.settings.sample_budget_mib = uint8_t(kSampleBudgetDefaultMib);
    storage().note_command(Cmd::mute_store, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    storage().request_reload();
    CHECK(settle());
    CHECK_EQ(storage().samples().budget(), kSampleBudgetDefaultMib << 20);
}

// =============================================================================================
// 7b. What mutation testing found nothing holding
// =============================================================================================
//
// Each test here exists because a mutant of app/storage.cpp survived the whole suite. The
// comment on each says which line it pins.

TEST(storage_every_save_makes_its_folder_before_writing_into_it) {
    // step_save makes the project folder on pass 0 and only then; `step_ != 0` survived, because
    // the fake let a file be written into a folder nobody made. Strict here, as FAT is.
    clean();
    FakeHost &host = FakeHost::the();
    host.set_require_folders(true);

    // A project folder nobody has made yet, without renaming the project (request_new would, and
    // the tests after this one build their paths from the name): take the folder away.
    char folder[ENAGA_FILE_PATH_MAX + 1];
    std::strcpy(folder, kDataFolder);
    std::strcat(folder, "/PROJECTS/");
    std::strcat(folder, storage().project_name());
    for (int i = host.file_count() - 1; i >= 0; i--) {
        const char *name = host.file_name(i);
        if (std::strncmp(name, folder, std::strlen(folder)) == 0) enaga::files::remove(name);
    }
    CHECK(!host.has_file(folder));
    CHECK(!host.has_file(card_path("PROJECT.BIN")));
    post(Cmd::toggle_trig, 0, 3);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.has_file(card_path("PROJECT.BIN")));

    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.has_file(card_path("PROJECT.BIN", true)));
}

TEST(storage_open_last_falls_back_when_the_newer_copy_is_missing) {
    // The autosave is newer but its PROJECT.BIN is gone. open_last must check the AUTOSAVE path
    // before choosing it -- checking the project's (a survived mutant) chooses a folder that
    // then fails to load.
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 21);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 99);
    pump(2);
    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK_EQ(int(enaga::files::remove(card_path("PROJECT.BIN", true))), int(ENAGA_FILE_OK));
    CHECK(!host.has_file(card_path("PROJECT.BIN", true)));

    CHECK(storage().open_last());
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 21);         // the project, the copy that is there
}

TEST(storage_open_last_sets_which_folder_it_chose_every_time) {
    // `autosave_` is left from whatever ran last, so open_last must SET it on both branches.
    // Deleting either assignment survived, because every earlier test happened to arrive with it
    // already right.
    clean();
    FakeHost &host = FakeHost::the();

    // (1) The autosave is newer, but the last job was a reload of the PROJECT, so autosave_ is
    // false on the way in.
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 21);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 99);
    pump(2);
    storage().request_autosave();
    CHECK(pump_until_idle());
    storage().request_reload();
    CHECK(settle());
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 21);
    CHECK(storage().open_last());
    CHECK(settle());
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 99);         // the autosave, as the counters say

    // (2) The project is newer, but the last job was an AUTOSAVE that failed -- so autosave_ is
    // true on the way in and the autosave's counter never moved.
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 33);
    pump(2);
    storage().note_command(Cmd::set_param, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 77);
    pump(2);
    host.set_full(true);
    // A full card refuses the folder only when it has to be made, so take PROJECT.BIN's slot
    // away instead: every file write fails.
    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() != nullptr);
    host.set_full(false);

    CHECK(storage().open_last());
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 33);         // the project, not the older autosave
}

TEST(storage_a_load_under_an_external_clock_runs_stopped_and_is_refused_running) {
    // CLAUDE.md: stop() returns false under external sync, and the refusal must be handled. The
    // load path had no test for either half: a STOPPED transport under MIDI clock loads (`||`
    // for `&&` survived and refused it), and a RUNNING one is refused once, with a reason, and
    // not retried every pass (deleting `load_pending_ = false` or the reason survived).
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 44);
    pump(2);
    storage().request_save();
    CHECK(pump_until_idle());

    host.set_source(ENAGA_CLOCK_MIDI);
    host.set_running(false);
    post(Cmd::set_param, 1, 0, uint8_t(kLvl), 5);
    pump(2);
    storage().request_reload();
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    pump(4);
    CHECK_EQ(int(kit_value(1, kLvl)), 44);

    host.set_running(true);
    const int reads = host.read_alls();
    storage().request_reload();
    pump(2);
    CHECK_EQ(storage().last_error(), "EXT CLOCK");
    CHECK(host.running());
    const int stops = host.control_calls(ENAGA_CLOCK_STOP);
    CHECK(settle());
    CHECK_EQ(host.read_alls(), reads);             // refused, and not tried again:
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_STOP), stops);   // no stop asked for on every pass
    CHECK(!storage().loading_project());
}

TEST(storage_a_failed_project_write_keeps_the_project_dirty) {
    // Deleting the finish_job after a failed PROJECT.BIN write survived: the job then carried on
    // and cleared `project_dirty_` for a file it never wrote, so the NEXT save skipped it and the
    // change was lost with no error anywhere.
    clean();
    FakeHost &host = FakeHost::the();
    storage().request_save();
    CHECK(pump_until_idle());                      // the folder exists from here on

    post(Cmd::set_tempo, 0, 0, 0, 0);
    Command tempo;
    tempo.cmd = Cmd::set_tempo;
    tempo.f = 141.0f;
    engine().commands().post(tempo);
    pump(2);
    storage().note_command(Cmd::set_tempo, 0);
    host.set_full(true);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() != nullptr);
    CHECK(storage().dirty());

    host.set_full(false);
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);

    Project &wipe = engine().project_for_load();
    project_init(wipe);
    post(Cmd::project_loaded);
    pump(2);
    storage().request_reload();
    CHECK(settle());
    CHECK_EQ(int(engine().project().settings.tempo_centi_bpm), 14100);
}

TEST(storage_a_card_pulled_during_a_swap_fails_track_zero_too) {
    // on_card abandons a sample job and fails its track -- `sample_track_ >= 0`, and `> 0`
    // survived because every test used track 7. Track 0 is a track.
    clean();
    FakeHost &host = FakeHost::the();
    make_wav("/SAMPLES/ZERO.WAV", 48000, 1, 16, 1200);
    kit_sample(0, "/SAMPLES/ZERO.WAV");
    CHECK(storage().request_sample(0));
    pump_held(3);
    CHECK_EQ(storage().busy_text(), "READING");
    CHECK_EQ(storage().loading_track(), 0);

    host.set_card(false);
    storage().on_card(false);
    CHECK_EQ(int(slot_of(0).state), int(SampleState::failed));
    CHECK_EQ(storage().loading_track(), -1);

    host.complete_loads();
    host.set_card(true);
    storage().on_card(true);
    kit_sample(0, "");
    CHECK(settle());
}

TEST(storage_progress_moves_through_a_long_convert_and_resample) {
    // progress() for the two sliced jobs had no test: `== 0` for `!= 0` survived in both. A
    // file of three slices shows a fraction between the ends on the way through.
    clean();
    // 8-bit, so three slices of frames fit the scratch buffer.
    make_wav("/SAMPLES/LONG.WAV", 48000, 1, 8, 3 * 48000);
    kit_sample(kLoopTrack, "/SAMPLES/LONG.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    bool between = false;
    for (int i = 0; i < 400 && !between; i++) {
        pump();
        const char *busy = storage().busy_text();
        if (busy != nullptr && std::strcmp(busy, "CONVERTING") == 0) {
            const int p = storage().progress();
            between = p > 0 && p < 100;
        }
    }
    CHECK(between);
    CHECK(settle());

    make_wav("/SAMPLES/LONG24.WAV", 24000, 1, 8, 3 * 48000 / 2);   // three slices once doubled
    kit_sample(kLoopTrack, "/SAMPLES/LONG24.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    between = false;
    for (int i = 0; i < 400 && !between; i++) {
        pump();
        const char *busy = storage().busy_text();
        if (busy != nullptr && std::strcmp(busy, "RESAMPLING") == 0) {
            const int p = storage().progress();
            between = p > 0 && p < 100;
        }
    }
    CHECK(between);
    CHECK(settle());
}

TEST(storage_an_empty_file_is_not_a_wav) {
    // A read of zero bytes is a file, not a failure, and goes to the header parser like any
    // other: `read_result_ <= 0` survived, which reported it as the read error name for 0.
    clean();
    FakeHost &host = FakeHost::the();
    CHECK(host.write_file("/SAMPLES/EMPTY.WAV", "", 0));
    kit_sample(kLoopTrack, "/SAMPLES/EMPTY.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(pump_until_idle());
    CHECK_EQ(storage().last_error(), "NOT A WAV");
}

// =============================================================================================
// 8. A new project
// =============================================================================================
//
// These come last on purpose. `request_new` renames the project for the rest of the process --
// `name_` is written here and read from the settings only in `init()` -- so a test that ran after
// one of these and expected "DEFAULT" would be looking in the wrong folder. Every test above
// builds its paths from `Storage::project_name()` for the same reason.

namespace {

// "NEW12" -> 12, and -1 for anything else.
int new_name_number(const char *name) {
    if (name[0] != 'N' || name[1] != 'E' || name[2] != 'W' || name[3] == '\0') return -1;
    int value = 0;
    for (const char *p = name + 3; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10 + (*p - '0');
    }
    return value;
}

}  // namespace

TEST(storage_request_new_is_refused_under_an_external_clock) {
    // CLAUDE.md: start/stop/set_tempo return false under external sync, and the refusal has to be
    // handled. A new project replaces everything in RAM, so it needs the transport stopped -- and
    // under a MIDI clock STRATA cannot stop anything.
    clean();
    FakeHost &host = FakeHost::the();

    post(Cmd::toggle_trig, 0, 5);
    pump(2);
    char before_bytes[Storage::kNameBytes];
    std::strcpy(before_bytes, storage().project_name());
    const char *before = before_bytes;       // a pointer, so CHECK_EQ compares the CONTENT

    host.set_source(ENAGA_CLOCK_MIDI);
    host.set_running(true);
    storage().request_new();
    storage().service();                                 // it runs from the card side's pass

    CHECK_EQ(storage().last_error(), "EXT CLOCK");
    CHECK_EQ(storage().project_name(), before);          // not renamed
    CHECK(host.running());                               // and the clock was left alone
    pump(2);
    CHECK((engine().pattern().tracks[0].steps[5].flags & kStepTrig) != 0);   // nothing was wiped
    CHECK_EQ(host.file_count(), 0);
}

TEST(storage_request_new_is_ram_only_and_takes_a_fresh_name) {
    clean();
    FakeHost &host = FakeHost::the();

    // A project on the card to lose, if this went wrong.
    post(Cmd::toggle_trig, 0, 1);
    post(Cmd::set_param, 0, 0, uint8_t(kLvl), 73);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);
    storage().note_command(Cmd::set_param, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    const int files_before = host.file_count();
    const int saves_before = host.saves();
    char old_name[Storage::kNameBytes];
    std::strcpy(old_name, storage().project_name());

    storage().request_new();
    CHECK(settle());

    // RAM only: nothing is written until a save, so a new project cannot lose the one on the card
    // by itself.
    CHECK_EQ(host.saves(), saves_before);
    CHECK_EQ(host.file_count(), files_before);
    CHECK(storage().last_error() == nullptr);

    // A NEW NAME, from a counter in the settings. Keeping the old one would make the next save
    // overwrite a project the user never asked to touch.
    const int number = new_name_number(storage().project_name());
    if (number < 0) std::printf("    the new project is called %s\n", storage().project_name());
    CHECK(number > 0);
    CHECK(std::strcmp(storage().project_name(), old_name) != 0);

    // The project in RAM is the FACTORY one (core/factory.hpp) -- the edits above are gone and the
    // kit is playable, where a blank kit had LVL 0 on every track -- every file is dirty, and the
    // first save of it writes all eleven: a new name names a folder with nothing in it to be a
    // diff against.
    pump(4);
    CHECK_EQ(int(engine().pattern().tracks[0].steps[1].flags), 0);
    CHECK_EQ(int(kit_value(0, kLvl)), int(strata::test::factory().kit.tracks[0].params[kLvl]));
    CHECK(kit_value(0, kLvl) > 0);
    CHECK(storage().dirty());
    CHECK_EQ(scene_count(engine().scenes().scenes[0]), 0);

    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    for (int i = 0; i < kFileN; i++) {
        if (!host.has_file(card_path(kFiles[i]))) {
            std::printf("    the new project's %s is missing\n", kFiles[i]);
        }
        CHECK(host.has_file(card_path(kFiles[i])));
    }

    // And the project that was there is untouched, with its own trig still in it.
    char path[ENAGA_FILE_PATH_MAX + 1];
    std::strcpy(path, kDataFolder);
    std::strcat(path, "/PROJECTS/");
    std::strcat(path, old_name);
    std::strcat(path, "/BANK_A.BIN");
    CHECK(host.has_file(path));

    // The counter keeps counting, so two new projects in a row cannot name the same folder.
    storage().request_new();
    CHECK(settle());
    CHECK_EQ(new_name_number(storage().project_name()), number + 1);
}

TEST(storage_request_new_takes_everything_of_the_old_project_away) {
    // What the two tests above do not look at: everything a new project has to undo besides the
    // patterns and the name. Each line below is one assignment in `request_new` that mutation
    // testing showed could be deleted with the whole suite still passing.
    clean();
    FakeHost &host = FakeHost::the();

    // A sample on a track and a raised budget, by the real route: saved, then read back.
    make_wav("/SAMPLES/OLD.WAV", 48000, 1, 16, 600);
    kit_sample(kLoopTrack, "/SAMPLES/OLD.WAV");
    engine().project_for_load().settings.sample_budget_mib = 48;
    post(Cmd::project_loaded);
    pump(2);
    storage().note_command(Cmd::mute_store, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    storage().request_reload();
    CHECK(settle());
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(storage().samples().budget(), 48u << 20);

    // A scene and a macro, which live in PERFORM.BIN and not in the project.
    post(Cmd::kit_store);
    post(Cmd::scene_capture, 0, 0, 5, 0);
    Command assign;
    assign.cmd = Cmd::macro_assign;
    assign.track = 1;
    assign.a = 2;
    assign.b = 9;
    assign.c = 20;
    assign.d = 110;
    engine().commands().post(assign);
    pump(4);
    CHECK(scene_count(engine().scenes().scenes[5]) > 0);
    CHECK_EQ(int(engine().macros().macros[2].targets[0].enabled), 1);

    // A reason still on screen from something that failed earlier.
    kit_sample(kLoopTrack - 1, "/SAMPLES/MISSING.WAV");
    CHECK(storage().request_sample(kLoopTrack - 1));
    CHECK(settle());
    CHECK(storage().last_error() != nullptr);

    // And the transport running on the INTERNAL clock, which a new project may stop. Only an
    // outside clock refuses (the test above); this one must not.
    host.set_running(true);
    storage().request_new();
    storage().service();
    CHECK(!host.running());
    CHECK(storage().last_error() == nullptr);      // the old reason does not outlive its project

    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::empty));   // off its track
    CHECK_EQ(storage().samples().budget(), kSampleBudgetDefaultMib << 20);
    CHECK_EQ(scene_count(engine().scenes().scenes[5]), 0);
    // The macro is the factory's again, not the one assigned above.
    const MacroTarget &target = engine().macros().macros[2].targets[0];
    const MacroTarget &wanted = strata::test::factory_macros().macros[2].targets[0];
    CHECK_EQ(int(target.enabled), int(wanted.enabled));
    CHECK_EQ(int(target.track), int(wanted.track));
    CHECK_EQ(int(target.param), int(wanted.param));
    CHECK_EQ(int(target.low), int(wanted.low));
    CHECK_EQ(int(target.high), int(wanted.high));
}

TEST(storage_request_new_waits_for_the_command_ring) {
    // `project_init` rewrites all of pattern memory from the main context, and the audio side
    // applies pending edits to that memory whether or not the clock runs. So a new project waits
    // for the ring like a load does. It used to run inside the key press, on top of whatever that
    // press and the ones before it had left in the ring.
    clean();
    char before_bytes[Storage::kNameBytes];
    std::strcpy(before_bytes, storage().project_name());
    const char *before = before_bytes;

    post(Cmd::toggle_trig, 0, 6);
    storage().request_new();
    for (int i = 0; i < 50; i++) storage().service();
    CHECK_EQ(storage().project_name(), before);          // not yet
    CHECK(!engine().commands().empty());

    // Drained: the edit lands on the OLD project, and then the new one replaces it whole.
    CHECK(settle());
    CHECK(std::strcmp(storage().project_name(), before) != 0);
    CHECK_EQ(int(engine().pattern().tracks[0].steps[6].flags), 0);
}

TEST(storage_request_new_needs_no_card) {
    // RAM only, so pulling the card must not make NEW do nothing -- service() returns early
    // without a card, and the new project is handled before that.
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::toggle_trig, 0, 7);
    pump(2);
    host.set_card(false);
    storage().on_card(false);
    char before_bytes[Storage::kNameBytes];
    std::strcpy(before_bytes, storage().project_name());
    const char *before = before_bytes;

    storage().request_new();
    pump(4);
    CHECK(std::strcmp(storage().project_name(), before) != 0);
    CHECK_EQ(int(engine().pattern().tracks[0].steps[7].flags), 0);

    host.set_card(true);
    storage().on_card(true);
    CHECK(settle());
}

TEST(storage_a_deliberate_save_puts_the_project_name_in_the_settings) {
    // What init() reopens next time is the name in the settings, so a deliberate save has to
    // write it there. `if (autosave_)` for `if (!autosave_)` survived: no test read the settings
    // back after the name had changed.
    clean();
    storage().request_new();
    CHECK(settle());
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    enaga::Settings settings;
    settings.load();
    CHECK_EQ(settings.text("PROJECT", ""), storage().project_name());
}

// =============================================================================================
// 7c. What a line-by-line mutation run found (tools/mutation_test.py)
// =============================================================================================
//
// The tests above came from hand-picked mutants. tools/mutation_test.py mutates every line of
// app/storage.cpp; these close what it found. The survivors that change no behaviour are argued
// in app/storage.cpp and listed in docs/MUTATION.md.

namespace {

// A WAV bigger than g_scratch: 8-bit PCM, which is one byte a sample, so the converted int16 is
// twice the file -- the cheapest way to ask the pool for a lot of room.
// 3.5 MiB: the fake card's arena is 4 MiB (tests/fakehost.hpp), and the settings and the project
// files have to fit next to it.
uint8_t g_big[7u << 19];

uint32_t make_big_wav(const char *path, uint32_t rate, int channels, uint32_t frames) {
    enaga::wav::Format format;
    format.encoding = uint16_t(enaga::wav::pcm);
    format.channels = uint16_t(channels);
    format.rate = rate;
    format.bits = 8;
    const uint32_t data = frames * uint32_t(channels);
    CHECK(44u + data <= uint32_t(sizeof g_big));
    enaga::wav::header(g_big, format, data);
    for (uint32_t i = 0; i < data; i++) g_big[44u + i] = uint8_t(((i / 64u) & 1u) ? 0x20u : 0xE0u);
    CHECK(FakeHost::the().write_file(path, g_big, 44u + data));
    return 44u + data;
}

}  // namespace

TEST(storage_every_refused_sample_leaves_the_loader_idle) {
    // `sample_track_ = -1` on each refusal is what `loading_track()` reports, and the screen
    // draws "loading" from it. Only NOT A WAV was ever checked.
    clean();
    kit_sample(kLoopTrack, "/SAMPLES/NOWHERE.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "NOT FOUND");
    CHECK_EQ(storage().loading_track(), -1);

    make_wav("/SAMPLES/E0.WAV", 48000, 2, 16, 0);
    kit_sample(kLoopTrack, "/SAMPLES/E0.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "BAD FORMAT");
    CHECK_EQ(storage().loading_track(), -1);

    make_wav("/SAMPLES/C3.WAV", 48000, 3, 16, 100);
    kit_sample(kLoopTrack, "/SAMPLES/C3.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "TOO MANY CH");
    CHECK_EQ(storage().loading_track(), -1);

    make_wav("/SAMPLES/E1.WAV", 48000, 2, 16, 0, nullptr, 0, false, false, 3);
    kit_sample(kLoopTrack, "/SAMPLES/E1.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "EMPTY");
    CHECK_EQ(storage().loading_track(), -1);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
}

TEST(storage_a_file_to_resample_longer_than_the_source_buffer_is_refused) {
    // A file at another rate is converted whole into src_ before it is resampled, and src_ holds
    // 30 s at kMaxSampleSourceRate (96 kHz): 2,880,000 frames. One frame more is refused with a
    // reason rather than written past the buffer; exactly that many is fine. At 44.1 kHz that is
    // about 65 s, as app/storage.hpp says. 8-bit mono keeps the file under the fake card's 4 MiB,
    // and both files share one name so that only one is on the card at a time.
    constexpr uint32_t kSrcFrames = 30u * 96000u;
    clean();
    make_big_wav("/SAMPLES/LONG441.WAV", 44100, 1, kSrcFrames + 1u);
    kit_sample(kLoopTrack, "/SAMPLES/LONG441.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(storage().last_error(), "TOO LONG");
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(storage().loading_track(), -1);
    CHECK_EQ(slot_of(kLoopTrack).bytes, 0u);

    make_big_wav("/SAMPLES/LONG441.WAV", 44100, 1, kSrcFrames);
    kit_sample(kLoopTrack, "/SAMPLES/LONG441.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    storage().request_new();                    // and give the memory back
    CHECK(settle());
}

TEST(storage_a_sample_the_pool_has_no_room_for_is_refused_and_the_rest_play) {
    // 3.5 MiB of 8-bit stereo is 7 MiB once converted, so the budget holds `fit` of them and
    // the next is refused with NO ROOM and holds nothing, while the ones before it stay loaded.
    // The budget is whatever the tests before left it at (32 MiB unless one is mid-change), so
    // the count is worked out rather than assumed; anything up to 56 MiB leaves a track over.
    clean();
    const uint32_t frames = (uint32_t(sizeof g_big) - 44u) / 2u;
    make_big_wav("/SAMPLES/BIG.WAV", 48000, 2, frames);
    const uint32_t converted = frames * 2u * 2u;
    const uint32_t fit = storage().samples().budget() / converted;
    CHECK(fit >= 1u && fit + 1u <= uint32_t(kTracks));

    for (uint32_t t = 0; t <= fit; t++) kit_sample(int(t), "/SAMPLES/BIG.WAV");
    for (uint32_t t = 0; t <= fit; t++) {
        CHECK(storage().request_sample(int(t)));
        CHECK(settle(24, 40000));
    }
    for (uint32_t t = 0; t < fit; t++) {
        CHECK_EQ(int(slot_of(int(t)).state), int(SampleState::ready));
    }
    CHECK_EQ(storage().last_error(), "NO ROOM");
    CHECK_EQ(int(slot_of(int(fit)).state), int(SampleState::failed));
    CHECK_EQ(slot_of(int(fit)).bytes, 0u);
    CHECK_EQ(storage().loading_track(), -1);

    // NEW names no file for any track, so every track goes through the swap's "no file" branch:
    // the sample is taken away, the memory given back, and the loader ends idle.
    storage().request_new();
    CHECK(settle(24, 40000));
    for (uint32_t t = 0; t <= fit; t++) {
        CHECK_EQ(int(slot_of(int(t)).state), int(SampleState::empty));
        CHECK_EQ(slot_of(int(t)).bytes, 0u);
    }
    CHECK_EQ(storage().samples().used(), 0u);
    CHECK_EQ(storage().loading_track(), -1);
}

namespace {

// Stereo 16-bit with every frame distinguishable from its neighbours, written past g_scratch.
// The right channel is the left negated, so a channel stride of one -- which is what a slice
// offset computed with the channel count left out would use -- reads the wrong samples.
int16_t big_value(uint32_t f) { return int16_t(int32_t((f * 7919u) % 60000u) - 30000); }

void make_big_wav16(const char *path, uint32_t rate, uint32_t frames) {
    enaga::wav::Format format;
    format.encoding = uint16_t(enaga::wav::pcm);
    format.channels = 2;
    format.rate = rate;
    format.bits = 16;
    CHECK(44u + frames * 4u <= uint32_t(sizeof g_big));
    enaga::wav::header(g_big, format, frames * 4u);
    for (uint32_t f = 0; f < frames; f++) {
        const int16_t l = big_value(f);
        const int16_t r = int16_t(-l);
        g_big[44u + 4u * f] = uint8_t(uint16_t(l));
        g_big[45u + 4u * f] = uint8_t(uint16_t(l) >> 8);
        g_big[46u + 4u * f] = uint8_t(uint16_t(r));
        g_big[47u + 4u * f] = uint8_t(uint16_t(r) >> 8);
    }
    CHECK(FakeHost::the().write_file(path, g_big, 44u + frames * 4u));
}

int16_t g_expect[2 * 130000];
int16_t g_source[2 * 60000];

}  // namespace

TEST(storage_a_file_longer_than_one_slice_arrives_whole_and_in_order) {
    // The loader converts 48,000 frames a pass. Every file before this one fitted in a single
    // pass, where the slice offsets are all zero -- so a slice written at the wrong offset, or
    // read from the wrong byte, was invisible. 100,000 stereo frames is three passes.
    clean();
    constexpr uint32_t kFrames = 100000;
    make_big_wav16("/SAMPLES/LONG48.WAV", 48000, kFrames);
    kit_sample(kLoopTrack, "/SAMPLES/LONG48.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK_EQ(uint32_t(loop->sample_frames()), kFrames);
    CHECK_EQ(loop->sample_channels(), 2);
    const int16_t *pcm = loop->sample_data();
    CHECK(pcm != nullptr);
    if (pcm != nullptr && uint32_t(loop->sample_frames()) == kFrames) {
        uint32_t wrong = 0;
        for (uint32_t f = 0; f < kFrames; f++) {
            wrong += pcm[2 * f] != big_value(f) ? 1u : 0u;
            wrong += pcm[2 * f + 1] != int16_t(-big_value(f)) ? 1u : 0u;
        }
        CHECK_EQ(wrong, 0u);
    }
    storage().request_new();
    CHECK(settle());
}

TEST(storage_a_resample_longer_than_one_slice_matches_one_done_in_one_go) {
    // 60,000 stereo frames at 24 kHz is two passes of conversion and three of resampling
    // (120,000 out). The slices must join into exactly what one uninterrupted resample of the
    // whole file gives.
    clean();
    constexpr uint32_t kFrames = 60000;
    make_big_wav16("/SAMPLES/LONG24.WAV", 24000, kFrames);
    for (uint32_t f = 0; f < kFrames; f++) {
        g_source[2 * f] = big_value(f);
        g_source[2 * f + 1] = int16_t(-big_value(f));
    }
    Resampler cursor = Resampler::make(24000, 48000);
    const uint32_t n = pcm_resample(g_source, kFrames, 2, cursor, g_expect,
                                    uint32_t(sizeof g_expect / sizeof g_expect[0] / 2));
    CHECK(n > 96000u);

    kit_sample(kLoopTrack, "/SAMPLES/LONG24.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK_EQ(uint32_t(loop->sample_frames()), n);
    const int16_t *pcm = loop->sample_data();
    if (pcm != nullptr && uint32_t(loop->sample_frames()) == n) {
        CHECK(std::memcmp(pcm, g_expect, n * 2 * sizeof(int16_t)) == 0);
    }
    storage().request_new();
    CHECK(settle());
}

TEST(storage_a_save_asked_for_after_an_autosave_stands_the_autosave_down) {
    // The other order of storage_a_save_already_asked_for_beats_an_autosave: the transport
    // stops, then SAVE is pressed before the autosave could start. `request_save` clears the
    // pending autosave, so only the project folder is written.
    clean();
    FakeHost &host = FakeHost::the();
    storage().request_autosave();
    storage().request_save();
    CHECK(settle());
    CHECK(host.logged("saved"));
    CHECK(!host.logged("autosaved"));
    CHECK(!host.has_file(card_path("KIT.BIN", true)));
}

TEST(storage_the_later_of_new_and_reload_is_the_one_that_happens) {
    clean();
    storage().request_save();
    CHECK(pump_until_idle());
    char saved_name[Storage::kNameBytes];
    std::strcpy(saved_name, storage().project_name());

    // NEW, then RELOAD before either ran: the reload wins and the name stays.
    storage().request_new();
    storage().request_reload();
    CHECK(settle());
    CHECK_EQ(storage().project_name(), static_cast<const char *>(saved_name));

    // RELOAD, then NEW: the new project wins and takes a new name -- and the reload does not
    // run after it, which would look for the new name's folder on the card and fail.
    storage().request_reload();
    storage().request_new();
    CHECK(settle());
    CHECK(std::strcmp(storage().project_name(), saved_name) != 0);
    CHECK(storage().last_error() == nullptr);
}

TEST(storage_a_take_the_card_cannot_hold_is_reported_and_its_buffer_given_back) {
    // Two places a take write can fail: making /RECORD, and the save into it. Both report the
    // card's reason, and both give the buffer back to the audio side -- nothing else can, and a
    // take wedged in it would refuse every resample after it (dsp/capture.hpp). A failed save
    // must not land the take in a track either: the kit would name a file that is not there.
    Engine &e = clean();
    FakeHost &host = FakeHost::the();
    auto record_a_bar = [&](uint32_t bar_tick) {
        post(Cmd::resample_arm, 0, 0, 1);
        host.deliver_tick(bar_tick, 0);
        for (int i = 0; i < 1500; i++) {
            if (i % 8 == 0) host.deliver_tick(bar_tick + uint32_t(i), 0);
            pump();
            if (e.status().resample_state.load(std::memory_order_relaxed) == 3u) break;
        }
        CHECK_EQ(int(e.status().resample_state.load(std::memory_order_relaxed)), 3);
    };
    host.set_running(true);

    // No /RECORD and a full card: the folder cannot be made.
    record_a_bar(384);
    host.set_full(true);
    CHECK(storage().request_resample_write());
    CHECK(settle());
    CHECK_EQ(storage().last_error(), "CARD FULL");
    CHECK_EQ(int(e.status().resample_state.load(std::memory_order_relaxed)), 0);

    // /RECORD exists and the card is full: the save itself fails.
    host.set_full(false);
    CHECK(enaga::files::make_folder("/RECORD") >= 0);
    host.set_full(true);
    const char before = e.project().kit.tracks[kLoopTrack].sample[0];
    record_a_bar(768);
    CHECK(storage().request_resample_write(kLoopTrack));
    CHECK(settle());
    CHECK_EQ(storage().last_error(), "CARD FULL");
    CHECK_EQ(int(e.status().resample_state.load(std::memory_order_relaxed)), 0);
    CHECK_EQ(int(e.project().kit.tracks[kLoopTrack].sample[0]), int(before));
    host.set_full(false);
    host.set_running(false);
}

// =============================================================================================
// The kit library (FUNC+EDIT twice): sixteen kits in <data folder>/KITS, shared by projects
// =============================================================================================

namespace {

const char *kit_slot_path(int slot) {
    static char path[64];
    std::snprintf(path, sizeof path, "%s/KITS/KIT_%02d.BIN", kDataFolder, slot + 1);
    return path;
}

// Wait for the kit job asked for to run, and every sample swap it queued after it.
bool kit_settle() { return settle(); }

}  // namespace

TEST(storage_a_kit_saved_to_a_slot_comes_back_on_recall) {
    clean();
    FakeHost &host = FakeHost::the();

    // A sound worth keeping: track 1's first knob and its machine, track 2's fader.
    post(Cmd::set_param, 0, 0, 0, 101);
    post(Cmd::set_machine, 1, 0, uint8_t(MachineId::grain));
    pump(2);
    CHECK_EQ(int(kit_value(0, 0)), 101);

    CHECK(storage().request_kit_save(2));
    CHECK(kit_settle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.has_file(kit_slot_path(2)));
    CHECK_EQ(host.file_bytes(kit_slot_path(2)), int32_t(kit_bytes()));
    CHECK(host.logged("kit saved"));
    CHECK_EQ(int(storage().kit_slot(2).state), int(Storage::SlotState::used));
    CHECK_EQ(int(storage().kit_slot(2).machines[1]), int(MachineId::grain));

    // Change it, then recall: everything a sound is comes back.
    post(Cmd::set_param, 0, 0, 0, 7);
    post(Cmd::set_machine, 1, 0, uint8_t(MachineId::kick));
    pump(2);
    CHECK_EQ(int(kit_value(0, 0)), 7);

    storage().request_save();               // so "dirty" below is the recall's doing
    CHECK(kit_settle());
    CHECK(!storage().dirty());

    CHECK(storage().request_kit_load(2));
    CHECK(kit_settle());
    CHECK(storage().last_error() == nullptr);
    CHECK(host.logged("kit loaded"));
    CHECK_EQ(int(kit_value(0, 0)), 101);
    CHECK_EQ(int(engine().machine_id(1)), int(MachineId::grain));
    CHECK_EQ(int(engine().project().kit.tracks[1].machine), int(MachineId::grain));
    // RAM's kit is no longer the project's KIT.BIN, so a save has something to write.
    CHECK(storage().dirty());
}

TEST(storage_a_kit_recall_is_heard_and_plays_through_a_running_transport) {
    // A recall must not stop the clock (unlike a project load), and the recalled sound must be
    // what plays -- not only what is drawn: the per-block copy rebuilds `effective_` from
    // `performed_`, which only `perform_dirty_` refreshes.
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 0, 0, 3, 120);
    pump(2);
    CHECK(storage().request_kit_save(0));
    CHECK(kit_settle());

    post(Cmd::set_param, 0, 0, 3, 10);
    pump(2);
    host.set_running(true);
    CHECK(storage().request_kit_load(0));
    CHECK(kit_settle());
    CHECK(enaga::clock::running());
    CHECK_EQ(int(kit_value(0, 3)), 120);
    CHECK_EQ(int(engine().performed(0, 3)), 120);
    host.set_running(false);
}

TEST(storage_a_kit_recall_swaps_only_the_tracks_whose_sample_changed) {
    clean();
    FakeHost &host = FakeHost::the();

    make_wav("/SAMPLES/KA.WAV", 48000, 1, 16, 2400);
    make_wav("/SAMPLES/KB.WAV", 48000, 1, 16, 1200);
    kit_sample(kLoopTrack, "/SAMPLES/KA.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(slot_of(kLoopTrack).frames, 2400u);
    CHECK(storage().request_kit_save(0));       // slot 1 plays KA
    CHECK(kit_settle());

    // Recalling a kit with the same path reads no WAV: the loop keeps playing.
    const int loads = host.loads();
    CHECK(storage().request_kit_load(0));
    CHECK(kit_settle());
    CHECK_EQ(host.loads(), loads);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));

    // A kit naming another file swaps that track, through the ordinary loader.
    kit_sample(kLoopTrack, "/SAMPLES/KB.WAV");
    CHECK(storage().request_kit_save(1));       // slot 2 names KB (the path is the kit's)
    CHECK(kit_settle());
    CHECK(storage().request_kit_load(0));       // back to KA
    CHECK(kit_settle());
    CHECK_EQ(host.loads(), loads + 1);
    CHECK_EQ(std::strcmp(engine().project().kit.tracks[kLoopTrack].sample, "/SAMPLES/KA.WAV"), 0);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, 2400u);

    // And a kit naming no file takes the sample off the track.
    kit_sample(kLoopTrack, "");
    CHECK(storage().request_kit_save(2));
    CHECK(kit_settle());
    CHECK(storage().request_kit_load(0));
    CHECK(kit_settle());
    CHECK(storage().request_kit_load(2));
    CHECK(kit_settle());
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::empty));
}

TEST(storage_the_kit_scan_tells_empty_used_and_damaged_slots_apart) {
    clean();
    FakeHost &host = FakeHost::the();
    CHECK_EQ(int(storage().kit_slot(0).state), int(Storage::SlotState::unknown));
    CHECK_EQ(int(storage().kit_slot(-1).state), int(Storage::SlotState::unknown));
    CHECK_EQ(int(storage().kit_slot(16).state), int(Storage::SlotState::unknown));

    CHECK(storage().request_kit_save(3));
    CHECK(kit_settle());
    const char junk[] = "NOT A KIT AT ALL";
    CHECK(host.write_file(kit_slot_path(9), junk, sizeof junk));

    storage().request_kit_scan();
    CHECK(kit_settle());
    for (int slot = 0; slot < Storage::kKitSlots; slot++) {
        const Storage::SlotState want = slot == 3 ? Storage::SlotState::used
                                      : slot == 9 ? Storage::SlotState::bad
                                                  : Storage::SlotState::empty;
        CHECK_EQ(int(storage().kit_slot(slot).state), int(want));
    }
    // The default kit's machines, read back from the file.
    CHECK_EQ(int(storage().kit_slot(3).machines[0]), int(engine().machine_id(0)));
    CHECK_EQ(int(storage().kit_slot(3).machines[7]), int(engine().machine_id(7)));

    // A card swap forgets all of it: the slots are about a card that may be gone.
    storage().on_card(false);
    storage().on_card(true);
    CHECK_EQ(int(storage().kit_slot(3).state), int(Storage::SlotState::unknown));
    CHECK(settle());
}

TEST(storage_a_damaged_or_empty_kit_slot_changes_nothing) {
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 0, 0, 0, 33);
    pump(2);
    kit_sample(kLoopTrack, "/SAMPLES/KEEP.WAV");

    // A kit job starts and finishes in one pass, so there is no busy pass for pump_until_idle to
    // see; a few passes are enough for the ring to drain and the job to run.
    CHECK(storage().request_kit_load(5));       // nothing there
    pump(4);
    check_error("an empty slot", "EMPTY SLOT");
    CHECK(host.logged("kit load failed EMPTY SLOT"));
    CHECK_EQ(int(storage().kit_slot(5).state), int(Storage::SlotState::empty));

    // A kit whose CRC does not add up is refused whole: not one knob, not one path.
    CHECK(storage().request_kit_save(6));
    CHECK(kit_settle());
    const uint32_t bytes = fetch(kit_slot_path(6));
    g_scratch[bytes - 1] ^= 0x5Au;
    store(kit_slot_path(6), bytes);
    kit_sample(kLoopTrack, "/SAMPLES/OTHER.WAV");
    post(Cmd::set_param, 0, 0, 0, 44);
    pump(2);

    CHECK(storage().request_kit_load(6));
    pump(4);
    CHECK(storage().last_error() != nullptr);
    CHECK_EQ(int(storage().kit_slot(6).state), int(Storage::SlotState::bad));
    CHECK_EQ(int(kit_value(0, 0)), 44);
    CHECK_EQ(std::strcmp(engine().project().kit.tracks[kLoopTrack].sample, "/SAMPLES/OTHER.WAV"),
             0);
    kit_sample(kLoopTrack, "");
    CHECK(settle());
}

TEST(storage_kit_requests_need_a_slot_and_a_card) {
    clean();
    CHECK(!storage().request_kit_save(-1));
    CHECK(!storage().request_kit_save(Storage::kKitSlots));
    CHECK(!storage().request_kit_load(-1));
    CHECK(!storage().request_kit_load(Storage::kKitSlots));

    FakeHost::the().set_card(false);
    storage().on_card(false);
    CHECK(!storage().request_kit_save(0));
    CHECK(!storage().request_kit_load(0));
    storage().request_kit_scan();
    CHECK(!pump_until_idle(50));                // nothing ran
    FakeHost::the().set_card(true);
    storage().on_card(true);
    CHECK(settle());
}

TEST(storage_a_kit_save_stores_the_knob_turned_just_before_it) {
    // The `kit_store` a save posts copies `params_` into `project_.kit`, and the write waits for
    // the ring to drain -- so an edit posted in the same breath as the save is in the file.
    clean();
    post(Cmd::set_param, 2, 0, 5, 77);
    CHECK(storage().request_kit_save(11));
    CHECK(kit_settle());
    const uint32_t bytes = fetch(kit_slot_path(11));
    Kit kit;
    CHECK_EQ(int(kit_read(kit, g_scratch, bytes)), int(SerializeResult::ok));
    CHECK_EQ(int(kit.tracks[2].params[5]), 77);
}

// --- a file bigger than the loader's staging buffer, read in chunks ---------------------------
//
// Roland, 2026-11-22: loops over 30 s load whole, a chunk at a time, instead of streaming. A
// file bigger than raw_ (16.5 MiB) is opened and read 4 MiB at a time into raw_, each chunk
// converted straight into the pool. These files are built in place on the fake card.

namespace {

constexpr uint32_t kRawLimit = 30u * 96000u * 6u + 64u * 1024u;   // app/storage.hpp's kRawBytes

// A 48 kHz stereo 24-bit file whose frames are big_value(f) in the top 16 bits, so a frame read
// one byte off, or a carry across a chunk lost, shows as a wrong value. 6-byte frames do not
// divide a 4 MiB chunk, so every chunk boundary splits a frame.
uint32_t make_long24(const char *path, uint32_t frames, uint32_t declared_frames = 0) {
    const uint32_t bytes = 44u + frames * 6u;
    uint8_t *p = FakeHost::the().create_file(path, bytes);
    CHECK(p != nullptr);
    if (p == nullptr) return 0;
    enaga::wav::Format format;
    format.channels = 2;
    format.rate = 48000;
    format.bits = 24;
    enaga::wav::header(p, format, (declared_frames != 0 ? declared_frames : frames) * 6u);
    for (uint32_t f = 0; f < frames; f++) {
        const uint16_t l = uint16_t(big_value(f)), r = uint16_t(-big_value(f));
        uint8_t *q = p + 44u + 6u * f;
        q[0] = 0; q[1] = uint8_t(l); q[2] = uint8_t(l >> 8);
        q[3] = 0; q[4] = uint8_t(r); q[5] = uint8_t(r >> 8);
    }
    return bytes;
}

bool settle_long() { return settle(24, 40000); }

}  // namespace

TEST(storage_a_file_bigger_than_the_staging_buffer_loads_whole_in_chunks) {
    clean();
    // 3,000,000 frames: 62.5 s at 48 kHz, 18 MB on the card -- refused TOO BIG before.
    constexpr uint32_t kFrames = 3000000;
    CHECK(make_long24("/SAMPLES/STEM.WAV", kFrames) > kRawLimit);
    kit_sample(kLoopTrack, "/SAMPLES/STEM.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK_EQ(uint32_t(loop->sample_frames()), kFrames);
    const int16_t *pcm = loop->sample_data();
    CHECK(pcm != nullptr);
    if (pcm != nullptr && uint32_t(loop->sample_frames()) == kFrames) {
        uint32_t wrong = 0;
        for (uint32_t f = 0; f < kFrames; f++) {
            wrong += pcm[2 * f] != big_value(f) ? 1u : 0u;
            wrong += pcm[2 * f + 1] != int16_t(-big_value(f)) ? 1u : 0u;
        }
        CHECK_EQ(wrong, 0u);
    }
    // The file is closed again: all eight handles are free.
    for (int i = 0; i < int(ENAGA_FILES_OPEN); i++) {
        enaga::files::File probe;
        CHECK(probe.open("/SAMPLES/STEM.WAV", ENAGA_FILE_READ) == ENAGA_FILE_OK);
        probe.close();
    }
    storage().request_new();
    CHECK(settle_long());
}

TEST(storage_a_long_file_shorter_than_its_header_says_plays_what_is_there) {
    clean();
    constexpr uint32_t kFrames = 3000000;
    make_long24("/SAMPLES/CUT.WAV", kFrames, kFrames + 500000);
    kit_sample(kLoopTrack, "/SAMPLES/CUT.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().last_error() == nullptr);
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK_EQ(uint32_t(loop->sample_frames()), kFrames);
    storage().request_new();
    CHECK(settle_long());
}

TEST(storage_a_long_file_at_another_rate_is_resampled_or_streamed_past_the_source_buffer) {
    clean();
    // 32-bit float stereo at 44.1 kHz: 8-byte frames make 2.5 M frames (56.7 s) a 20 MB file,
    // past raw_, yet inside src_'s 2,880,000 frames -- so it is read in chunks and resampled.
    {
        constexpr uint32_t kFrames = 2500000;
        uint8_t *p = FakeHost::the().create_file("/SAMPLES/F441.WAV", 44u + kFrames * 8u);
        CHECK(p != nullptr);
        enaga::wav::Format format;
        format.encoding = uint16_t(enaga::wav::floating);
        format.channels = 2;
        format.rate = 44100;
        format.bits = 32;
        enaga::wav::header(p, format, kFrames * 8u);
        std::memset(p + 44, 0, kFrames * 8u);   // silence is 0.0f
        kit_sample(kLoopTrack, "/SAMPLES/F441.WAV");
        CHECK(storage().request_sample(kLoopTrack));
        CHECK(settle_long());
        CHECK(storage().last_error() == nullptr);
        CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
        const uint32_t expect = Resampler::make(44100, 48000).output_frames(kFrames);
        CHECK_EQ(slot_of(kLoopTrack).frames, expect);
        FakeHost::the().remove_file("/SAMPLES/F441.WAV");
    }
    // One past src_ cannot be resampled whole, so it streams, at its own rate: the head is two
    // seconds OF THE FILE, not resampled.
    {
        constexpr uint32_t kFrames = 30u * 96000u + 1u;
        uint8_t *p = FakeHost::the().create_file("/SAMPLES/F441.WAV", 44u + kFrames * 8u);
        CHECK(p != nullptr);
        enaga::wav::Format format;
        format.encoding = uint16_t(enaga::wav::floating);
        format.channels = 2;
        format.rate = 44100;
        format.bits = 32;
        enaga::wav::header(p, format, kFrames * 8u);
        kit_sample(kLoopTrack, "/SAMPLES/F441.WAV");
        CHECK(storage().request_sample(kLoopTrack));
        CHECK(settle_long());
        CHECK(storage().last_error() == nullptr);
        CHECK(storage().streaming(kLoopTrack));
        CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
        CHECK_EQ(slot_of(kLoopTrack).frames, 2u * 44100u);
        CHECK_EQ(slot_of(kLoopTrack).rate, 44100u);
        CHECK_EQ(FakeHost::the().last_stream_offset(), 44u + 2u * 44100u * 8u);
    }
    storage().request_new();
    CHECK(settle_long());
}

// --- a file too big for the pool, streamed -----------------------------------------------------
//
// Roland, 2026-11-22: a file the pool cannot hold streams. Its first two seconds load as a head,
// the rest is read from the card while it plays (app/storage.hpp, "streaming").

namespace {

// Mono 16-bit at 48 kHz, frame f = big_value(f): 2^24 frames is one past Cmd::set_sample's 24
// bits, so it can never be loaded whole, whatever the pool holds.
constexpr uint32_t kHugeFrames = (1u << 24) + 1000u;
constexpr uint32_t kHeadFrames = 2u * 48000u;

void make_huge(const char *path) {
    uint8_t *p = FakeHost::the().create_file(path, 44u + kHugeFrames * 2u);
    CHECK(p != nullptr);
    if (p == nullptr) return;
    enaga::wav::Format format;
    format.channels = 1;
    format.rate = 48000;
    format.bits = 16;
    enaga::wav::header(p, format, kHugeFrames * 2u);
    for (uint32_t f = 0; f < kHugeFrames; f++) {
        const uint16_t v = uint16_t(big_value(f));
        p[44u + 2u * f] = uint8_t(v);
        p[45u + 2u * f] = uint8_t(v >> 8);
    }
}

const Sampler &loop_sampler() {
    return *static_cast<const Sampler *>(engine().machine(kLoopTrack));
}

// The loop track as a plain forward one-shot that holds, sends closed, one trig on step 0.
void play_loop_once() {
    post(Cmd::set_param, kLoopTrack, 0, 8, 0);        // ATK: instant
    post(Cmd::set_param, kLoopTrack, 0, 9, 127);      // DEC: hold
    post(Cmd::set_param, kLoopTrack, 0, 10, 0);       // PLY: forward, one shot
    post(Cmd::set_param, kLoopTrack, 0, 14, 0);
    post(Cmd::set_param, kLoopTrack, 0, 15, 0);
    post(Cmd::toggle_trig, uint8_t(kLoopTrack), 0);
    pump(2);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);               // once: no tick comes again by itself
    pump(1);
}

}  // namespace

TEST(storage_a_file_past_the_24_bit_frame_count_streams) {
    clean();
    FakeHost &host = FakeHost::the();
    make_huge("/SAMPLES/HUGE.WAV");
    kit_sample(kLoopTrack, "/SAMPLES/HUGE.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().last_error() == nullptr);
    CHECK(storage().streaming(kLoopTrack));
    // Only the head is resident.
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    CHECK_EQ(slot_of(kLoopTrack).frames, kHeadFrames);
    CHECK_EQ(slot_of(kLoopTrack).bytes, kHeadFrames * 2u);
    const Sampler &loop = loop_sampler();
    CHECK(loop.streaming());
    CHECK_EQ(loop.stream_frames(), kHugeFrames);
    CHECK_EQ(int(loop.sample_data()[1234]), int(big_value(1234)));
    // The stream is open after the head and the audio side reads it.
    CHECK_EQ(host.stream_opens(), 1);
    CHECK_EQ(host.streams_open(), 1);
    CHECK_EQ(host.last_stream_offset(), 44u + kHeadFrames * 2u);
    CHECK(engine().stream_attached(kLoopTrack));
    // And what it read is the file, frame for frame, from the head's end.
    {
        const StreamFifo &fifo = engine().stream_fifo(kLoopTrack);
        CHECK(fifo.has(kHeadFrames));
        uint32_t wrong = 0, seen = 0;
        for (uint32_t f = kHeadFrames; f < kHeadFrames + 3000u && fifo.has(f); f++, seen++) {
            wrong += fifo.frame(f)[0] != big_value(f) ? 1u : 0u;
        }
        CHECK(seen >= 2048u);
        CHECK_EQ(wrong, 0u);
    }

    // Played past the head: the trigger reopens the stream (the fifo was filled for a voice
    // that was not playing yet, which is fine, and is thrown away), and the voice reads on.
    play_loop_once();
    CHECK(settle(4, 100));
    CHECK_EQ(host.stream_opens(), 2);
    CHECK_EQ(host.last_stream_offset(), 44u + kHeadFrames * 2u);
    pump(1600);                                       // 2.1 s
    CHECK(loop.stream_position() > kHeadFrames + 4000u);
    CHECK_EQ(loop.stream_late(), 0u);
    CHECK_EQ(engine().status().stream_late.load(), 0u);
    CHECK(loop.active());

    // The swap closes it: a new project takes every sample away.
    storage().request_new();
    CHECK(settle_long());
    CHECK(!storage().streaming(kLoopTrack));
    CHECK_EQ(host.streams_open(), 0);
    CHECK(!engine().stream_attached(kLoopTrack));
    host.remove_file("/SAMPLES/HUGE.WAV");
}

TEST(storage_a_stream_of_24_bit_frames_split_across_reads_arrives_whole) {
    // 44.1 kHz stereo 24-bit, past src_ so it streams: six-byte frames, read 1000 bytes at a
    // time, so nearly every read ends inside a frame and the engine has to carry the rest.
    clean();
    FakeHost &host = FakeHost::the();
    constexpr uint32_t kFrames = 2900000;
    const uint32_t bytes = 44u + kFrames * 6u;
    CHECK(bytes > kRawLimit);
    uint8_t *p = host.create_file("/SAMPLES/S24.WAV", bytes);
    CHECK(p != nullptr);
    if (p != nullptr) {
        enaga::wav::Format format;
        format.channels = 2;
        format.rate = 44100;
        format.bits = 24;
        enaga::wav::header(p, format, kFrames * 6u);
        for (uint32_t f = 0; f < kFrames; f++) {
            const uint16_t l = uint16_t(big_value(f)), r = uint16_t(-big_value(f));
            uint8_t *q = p + 44u + 6u * f;
            q[0] = 0; q[1] = uint8_t(l); q[2] = uint8_t(l >> 8);
            q[3] = 0; q[4] = uint8_t(r); q[5] = uint8_t(r >> 8);
        }
    }
    host.set_stream_read_limit(1000);
    kit_sample(kLoopTrack, "/SAMPLES/S24.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().streaming(kLoopTrack));
    const uint32_t head = 2u * 44100u;
    CHECK_EQ(host.last_stream_offset(), 44u + head * 6u);
    pump(20);
    const StreamFifo &fifo = engine().stream_fifo(kLoopTrack);
    uint32_t wrong = 0, seen = 0;
    for (uint32_t f = head; f < head + StreamFifo::kFrames && fifo.has(f); f++, seen++) {
        wrong += fifo.frame(f)[0] != big_value(f) ? 1u : 0u;
        wrong += fifo.frame(f)[1] != int16_t(-big_value(f)) ? 1u : 0u;
    }
    CHECK(seen >= 2000u);
    CHECK_EQ(wrong, 0u);
    host.set_stream_read_limit(0);
    storage().request_new();
    CHECK(settle_long());
    host.remove_file("/SAMPLES/S24.WAV");
}

TEST(storage_a_stream_that_falls_behind_plays_silence_then_catches_up) {
    clean();
    FakeHost &host = FakeHost::the();
    make_huge("/SAMPLES/HUGE.WAV");
    kit_sample(kLoopTrack, "/SAMPLES/HUGE.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    play_loop_once();
    CHECK(settle(4, 100));
    host.set_stream_stall(true);                      // the card stops delivering
    pump(1600);                                       // past the head
    const Sampler &loop = loop_sampler();
    CHECK(loop.stream_late() > 0u);
    CHECK(engine().status().stream_late.load() > 0u);
    const uint32_t at = loop.stream_position();
    CHECK(at > kHeadFrames);                          // it kept time
    host.set_stream_stall(false);
    pump(200);                                        // catching up, 448 frames a block
    const uint32_t late = loop.stream_late();
    pump(100);
    CHECK_EQ(loop.stream_late(), late);
    CHECK(loop.stream_position() > at + 300u * 64u - 64u);
    storage().request_new();
    CHECK(settle_long());
    host.remove_file("/SAMPLES/HUGE.WAV");
}

TEST(storage_a_stream_is_reopened_for_the_newest_restart_only) {
    clean();
    FakeHost &host = FakeHost::the();
    make_huge("/SAMPLES/HUGE.WAV");
    kit_sample(kLoopTrack, "/SAMPLES/HUGE.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK_EQ(host.stream_opens(), 1);
    // Each open now takes a while to fill, so restarts pile up before any attach.
    host.set_stream_fill_polls(1000);
    play_loop_once();
    pump(3);
    CHECK_EQ(host.stream_opens(), 2);
    CHECK(!engine().stream_attached(kLoopTrack));
    host.deliver_tick(24u * 64u, 0);                  // step 0 again: another restart
    pump(3);
    CHECK_EQ(host.stream_opens(), 3);                 // the filling one was dropped for it
    CHECK_EQ(host.streams_open(), 1);
    CHECK(!engine().stream_attached(kLoopTrack));
    host.set_stream_fill_polls(0);
    // A ring already counting down keeps its count: let the open finish filling.
    for (int i = 0; i < 1100 && !engine().stream_attached(kLoopTrack); i++) pump();
    CHECK(engine().stream_attached(kLoopTrack));
    CHECK_EQ(host.stream_opens(), 3);
    storage().request_new();
    CHECK(settle_long());
    host.remove_file("/SAMPLES/HUGE.WAV");
}

TEST(storage_four_tracks_stream_at_once_and_a_fifth_is_refused) {
    clean();
    FakeHost &host = FakeHost::the();
    make_huge("/SAMPLES/HUGE.WAV");
    for (int t = 0; t < 5; t++) kit_sample(t, "/SAMPLES/HUGE.WAV");
    for (int t = 0; t < 5; t++) CHECK(storage().request_sample(t));
    for (int t = 0; t < 4; t++) {
        // One at a time, so the fifth's refusal is the last word.
        CHECK(settle_long());
    }
    CHECK(settle_long());
    int streaming = 0;
    for (int t = 0; t < 5; t++) streaming += storage().streaming(t) ? 1 : 0;
    CHECK_EQ(streaming, 4);
    CHECK(!storage().streaming(4));
    CHECK_EQ(int(slot_of(4).state), int(SampleState::failed));
    CHECK_EQ(host.streams_open(), 4);
    for (int t = 0; t < 5; t++) kit_sample(t, "");
    storage().request_new();
    CHECK(settle_long());
    CHECK_EQ(host.streams_open(), 0);
    host.remove_file("/SAMPLES/HUGE.WAV");
}

TEST(storage_a_stream_comes_back_with_the_card) {
    clean();
    FakeHost &host = FakeHost::the();
    make_huge("/SAMPLES/HUGE.WAV");
    kit_sample(kLoopTrack, "/SAMPLES/HUGE.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().streaming(kLoopTrack));
    host.set_card(false);
    storage().on_card(false);
    pump(10);
    host.set_card(true);
    storage().on_card(true);                          // which reloads every track
    CHECK(settle_long());
    CHECK(storage().streaming(kLoopTrack));
    CHECK(engine().stream_attached(kLoopTrack));
    CHECK_EQ(host.streams_open(), 1);
    storage().request_new();
    CHECK(settle_long());
    host.remove_file("/SAMPLES/HUGE.WAV");
}

TEST(storage_a_card_pulled_or_failing_mid_chunk_fails_the_track_and_frees_the_handle) {
    clean();
    make_long24("/SAMPLES/STEM.WAV", 3000000);
    kit_sample(kLoopTrack, "/SAMPLES/STEM.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    // Until it is between chunks, a few passes in.
    for (int i = 0; i < 4000 && storage().progress() < 20; i++) pump();
    CHECK(storage().progress() >= 20);
    CHECK(storage().progress() < 100);
    FakeHost::the().set_read_error(true);
    CHECK(settle_long());
    CHECK(storage().last_error() != nullptr);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    CHECK_EQ(slot_of(kLoopTrack).bytes, 0u);
    FakeHost::the().set_read_error(false);

    // Again, and this time the card goes.
    CHECK(storage().request_sample(kLoopTrack));
    for (int i = 0; i < 4000 && storage().progress() < 20; i++) pump();
    FakeHost::the().set_card(false);
    storage().on_card(false);
    pump(20);
    CHECK_EQ(storage().loading_track(), -1);
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::failed));
    FakeHost::the().set_card(true);
    storage().on_card(true);                         // which asks for every track again
    CHECK(settle_long());
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    for (int i = 0; i < int(ENAGA_FILES_OPEN); i++) {
        enaga::files::File probe;
        CHECK(probe.open("/SAMPLES/STEM.WAV", ENAGA_FILE_READ) == ENAGA_FILE_OK);
        probe.close();
    }
    storage().request_new();
    CHECK(settle_long());
}

// --- the project browser: open another project, save under a new name --------------------------

namespace {

bool settings_say(const char *line) {
    static char text[2048];
    const int32_t got = FakeHost::the().read_prefix("/ENAGA/DATA/STRATA/SETTINGS.TXT", text,
                                                    sizeof text - 1);
    if (got < 0) return false;
    text[got] = '\0';
    return std::strstr(text, line) != nullptr;
}

}  // namespace

TEST(storage_save_as_writes_a_new_folder_and_leaves_the_old_one) {
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::toggle_trig, 0, 3);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);
    storage().request_save();
    CHECK(pump_until_idle());
    char old_name[Storage::kNameBytes];
    std::strcpy(old_name, storage().project_name());

    CHECK(storage().request_save_as("LIVE SET 2"));
    CHECK(storage().dirty());                       // nothing under the new name yet
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(std::strcmp(storage().project_name(), "LIVE SET 2"), 0);
    CHECK(!storage().dirty());
    for (int i = 0; i < kFileN; i++) CHECK(host.has_file(card_path(kFiles[i])));
    CHECK(settings_say("PROJECT=LIVE SET 2"));

    // The old folder is still there and still its own: a trig added now goes only to the new one.
    post(Cmd::toggle_trig, 0, 5);
    pump(2);
    storage().note_command(Cmd::toggle_trig, 0);
    storage().request_save();
    CHECK(pump_until_idle());

    CHECK(storage().request_open(old_name));
    CHECK(settle());
    CHECK(storage().last_error() == nullptr);
    CHECK_EQ(std::strcmp(storage().project_name(), old_name), 0);
    CHECK(engine().pattern().tracks[0].steps[3].flags != 0);
    CHECK_EQ(int(engine().pattern().tracks[0].steps[5].flags), 0);
    CHECK(settings_say(old_name));

    // And back again: the new one has both trigs.
    CHECK(storage().request_open("LIVE SET 2"));
    CHECK(settle());
    CHECK_EQ(std::strcmp(storage().project_name(), "LIVE SET 2"), 0);
    CHECK(engine().pattern().tracks[0].steps[5].flags != 0);
    CHECK(!storage().dirty());
}

TEST(storage_an_open_makes_the_project_the_one_the_next_start_opens) {
    clean();
    CHECK(storage().request_save_as("FIRST"));
    CHECK(pump_until_idle());
    CHECK(storage().request_save_as("OTHER"));
    CHECK(pump_until_idle());
    // An autosave of OTHER, newer than its save: what a stop leaves.
    storage().request_autosave();
    CHECK(pump_until_idle());
    CHECK(storage().request_open("FIRST"));
    CHECK(settle());
    CHECK_EQ(std::strcmp(storage().project_name(), "FIRST"), 0);
    // The open is counted newer than that autosave, so a start opens FIRST, not OTHER's
    // autosave under FIRST's name.
    int project_seq = -1, auto_seq = -1;
    {
        static char text[2048];
        const int32_t got = FakeHost::the().read_prefix("/ENAGA/DATA/STRATA/SETTINGS.TXT", text,
                                                        sizeof text - 1);
        CHECK(got > 0);
        text[got > 0 ? got : 0] = '\0';
        const char *p = std::strstr(text, "PROJSEQ=");
        const char *a = std::strstr(text, "AUTOSEQ=");
        if (p != nullptr) project_seq = std::atoi(p + 8);
        if (a != nullptr) auto_seq = std::atoi(a + 8);
    }
    CHECK(project_seq > auto_seq);
    CHECK(settings_say("PROJECT=FIRST"));
}

TEST(storage_opening_a_folder_that_is_not_a_project_changes_nothing) {
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::toggle_trig, 0, 7);
    pump(2);
    char name[Storage::kNameBytes];
    std::strcpy(name, storage().project_name());
    CHECK(host.write_file("/ENAGA/DATA/STRATA/PROJECTS/JUNK/NOTES.TXT", "x", 1));
    CHECK(storage().request_open("JUNK"));
    CHECK(pump_until_idle());
    check_error("a folder with no PROJECT.BIN", "NOT FOUND");
    CHECK_EQ(std::strcmp(storage().project_name(), name), 0);
    CHECK(engine().pattern().tracks[0].steps[7].flags != 0);
    CHECK(settle());
}

TEST(storage_project_names_are_what_the_panel_can_type) {
    CHECK(Storage::valid_name("A"));
    CHECK(Storage::valid_name("LIVE SET 2"));
    CHECK(Storage::valid_name("TECHNO-01+#!.X"));
    CHECK(Storage::valid_name("lower"));                    // a PC's folder: FAT finds it anyway
    CHECK(Storage::valid_name("ABCDEFGHIJKLMNOPQRSTUVW"));  // 23
    CHECK(!Storage::valid_name("ABCDEFGHIJKLMNOPQRSTUVWX"));
    CHECK(!Storage::valid_name(""));
    CHECK(!Storage::valid_name(nullptr));
    CHECK(!Storage::valid_name(" LEADING"));
    CHECK(!Storage::valid_name(".HIDDEN"));
    CHECK(!Storage::valid_name("A/B"));
    CHECK(!Storage::valid_name("A_B"));
    clean();
    CHECK(!storage().request_open("../X"));
    CHECK(!storage().request_save_as(""));
    CHECK(!pump_until_idle(20));                            // nothing was queued
}

// =============================================================================================
// SLC AUTO: a sample's slice points are found as it loads (core/onsets.hpp)
// =============================================================================================

namespace {

// Silence with drum hits: decaying noise bursts from `at[i]` at `peak[i]`, the same every run.
// `bits` 16 or 24; the file is written straight into the fake card.
int32_t hit_value(uint32_t f, const uint32_t *at, const int32_t *peak, int hits, uint32_t *seed) {
    int32_t v = 0;
    for (int i = 0; i < hits; i++) {
        if (f < at[i] || f - at[i] >= 2880u) continue;
        *seed = *seed * 1664525u + 1013904223u;
        const int32_t noise = int32_t(*seed >> 16) - 32768;
        const int32_t level = peak[i] - int32_t((f - at[i]) * uint32_t(peak[i]) / 2880u);
        v += noise * level >> 15;
    }
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

void make_hits_wav(const char *path, uint32_t rate, int channels, int bits, uint32_t frames,
                   const uint32_t *at, const int32_t *peak, int hits) {
    const uint32_t sample_bytes = uint32_t(bits) / 8u;
    const uint32_t frame_bytes = sample_bytes * uint32_t(channels);
    uint8_t *p = FakeHost::the().create_file(path, 44u + frames * frame_bytes);
    CHECK(p != nullptr);
    if (p == nullptr) return;
    enaga::wav::Format format;
    format.channels = uint16_t(channels);
    format.rate = rate;
    format.bits = uint16_t(bits);
    enaga::wav::header(p, format, frames * frame_bytes);
    uint32_t seed = 7;
    uint8_t *q = p + 44;
    for (uint32_t f = 0; f < frames; f++) {
        const int32_t v = hit_value(f, at, peak, hits, &seed);
        for (int c = 0; c < channels; c++) {
            if (bits == 24) *q++ = 0;
            *q++ = uint8_t(v);
            *q++ = uint8_t(v >> 8);
        }
    }
}

const uint32_t kHitAt[] = {10000, 30000, 50000, 61111};
const int32_t kHitPeak[] = {30000, 6000, 15000, 22000};

// The cut lands at most one hop (plus a resample's few frames) before the hit, never after it.
bool cut_near(uint32_t cut, uint32_t hit) {
    return cut <= hit + 2u && hit - cut < OnsetFinder::kHop + 4u;
}

}  // namespace

TEST(storage_a_sample_brings_its_slice_points_and_slc_auto_cuts_there) {
    clean();
    CHECK(settle());                 // the swaps the previous test's kit left behind, all of them
    make_hits_wav("/SAMPLES/BREAK.WAV", 48000, 1, 16, 72000, kHitAt, kHitPeak, 4);
    kit_sample(kLoopTrack, "/SAMPLES/BREAK.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK_EQ(int(slot_of(kLoopTrack).state), int(SampleState::ready));
    const Sampler &loop = loop_sampler();
    CHECK_EQ(loop.point_count(), 4);

    // AUTO (Cmd::set_slice_mode, a KIT.BIN change: the dirty table above) and SLC 3 over the
    // whole sample: the two strongest hits, 10000 (30000) and 61111 (22000), cut it.
    post(Cmd::set_slice_mode, kLoopTrack, 0, kSliceAuto);
    post(Cmd::set_param, kLoopTrack, 0, 1, 0);           // STA: the start
    post(Cmd::set_param, kLoopTrack, 0, 2, 127);         // LEN: all of it
    post(Cmd::set_param, kLoopTrack, 0, 3, 17);          // SLC 3
    play_loop_once();                                    // a trig: the machine reads its knobs
    FakeHost::the().set_running(false);
    pump(2);
    CHECK_EQ(int(engine().project().kit.tracks[kLoopTrack].slice_mode), int(kSliceAuto));
    CHECK(loop.slicing_by_points());
    CHECK_EQ(loop.slice_count(), 3);
    CHECK_EQ(loop.slice_start(0), 0);
    CHECK(cut_near(uint32_t(loop.slice_start(1)), 10000));
    CHECK(cut_near(uint32_t(loop.slice_start(2)), 61111));

    // Saved and reopened, the track is still AUTO, and the reloaded sample brings its points.
    storage().request_save();
    CHECK(pump_until_idle());
    CHECK(storage().last_error() == nullptr);
    storage().request_reload();
    CHECK(settle());
    CHECK_EQ(int(engine().project().kit.tracks[kLoopTrack].slice_mode), int(kSliceAuto));
    CHECK(loop_sampler().auto_slices());
    CHECK_EQ(loop_sampler().point_count(), 4);

    post(Cmd::set_slice_mode, kLoopTrack, 0, kSliceEqual);
    pump(2);
    CHECK(!loop_sampler().slicing_by_points());
}

TEST(storage_slice_points_survive_the_resample_and_the_chunked_load) {
    clean();
    // 44.1 kHz: the points are found in the resampled 48 kHz PCM the track plays, so they land
    // at 48/44.1 of the file's frame.
    make_hits_wav("/SAMPLES/BREAK44.WAV", 44100, 2, 16, 72000, kHitAt, kHitPeak, 4);
    kit_sample(kLoopTrack, "/SAMPLES/BREAK44.WAV");
    CHECK(load_sample(kLoopTrack));
    CHECK(storage().last_error() == nullptr);
    {
        const SlicePoints &points = engine().slices_for_load(kLoopTrack);
        CHECK_EQ(int(points.count), 4);
        for (int i = 0; i < 4 && i < points.count; i++) {
            CHECK(cut_near(points.at[i], uint32_t(uint64_t(kHitAt[i]) * 48000u / 44100u)));
        }
        CHECK_EQ(loop_sampler().point_count(), 4);
    }

    // Past the staging buffer, read in 4 MiB chunks whose ends split frames: the points are the
    // same as for a small file, one hit at the end of the first chunk included.
    const uint32_t frames = 3000000;                      // 62.5 s, 24-bit stereo, 18 MB
    const uint32_t chunk_frames = (4u << 20) / 6u;         // where the first chunk ends
    const uint32_t at[] = {100000, chunk_frames - 50, 2000000, 2999000};
    const int32_t peak[] = {20000, 25000, 9000, 15000};
    make_hits_wav("/SAMPLES/STEM.WAV", 48000, 2, 24, frames, at, peak, 4);
    kit_sample(kLoopTrack, "/SAMPLES/STEM.WAV");
    CHECK(storage().request_sample(kLoopTrack));
    CHECK(settle_long());
    CHECK(storage().last_error() == nullptr);
    const SlicePoints &points = engine().slices_for_load(kLoopTrack);
    CHECK_EQ(int(points.count), 4);
    for (int i = 0; i < 4 && i < points.count; i++) CHECK(cut_near(points.at[i], at[i]));
    CHECK_EQ(int(points.rank[1]), 0);
    storage().request_new();
    CHECK(settle_long());
}
