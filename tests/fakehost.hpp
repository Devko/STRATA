// A fake Enaga, so app/ can be host-tested.
//
// ## Why this exists
//
// `app/` is about 5,000 lines -- the engine, the card protocols and every gesture the instrument
// has -- and until this file it had NO tests at all. `host/Makefile` compiled `core/` and `dsp/`
// only, so the whole of `app/` was verified by two things: that it compiles for the C674x, and
// four emulator scenarios that can press one key (`emulate.py` exposes `--key` and nothing else:
// no pads, no encoders). A review of `app/` found ten bugs, three of which a mechanical test of
// `effective_` after each command would have caught on the day they were written.
//
// ## Why it is small
//
// Enaga's C++ API is header-only: every call in <enaga/app.hpp>, <enaga/clock.hpp> and
// <enaga/files.hpp> is an inline function over `enaga::host()`, which returns a reference to one
// plain C struct of 38 function pointers (<enaga/abi.h>) and is "defined by the app runtime".
// `host/Makefile` does not link Enaga's `runtime.cpp`, so this file can define it instead and
// nothing clashes. No #ifdef reaches production code, and `app/` is compiled for the tests
// exactly as it is compiled for the device.
//
// Of the 38 pointers, twelve do real work here -- the clock, six file calls, the log -- and the
// rest are safe no-ops. That is the entire surface `app/` touches.
//
// ## What it gives that the emulator cannot
//
// **A programmable clock.** `deliver_tick(24, 37)` says "tick 24 falls on frame 37 of the next
// block", which is how micro-timing, ratchets and swing are actually specified, and an assertion
// can then be exact instead of a listen. The emulator has one real clock and no way to steer it.
//
// **Held asynchrony.** A `files::load` does not complete until `complete_loads()` is called, so a
// test can hold a project load in flight and check what the app refuses meanwhile. That is the
// shape of a bug this review found (PLAY during a load reads pattern memory mid-rewrite) and
// there is no way to produce it in the emulator on purpose.
//
// **Failure injection.** No card, a full card, a missing file, a card that fails mid-read. Every
// refusal path in `app/storage.cpp` is reachable, which on real hardware needs someone pulling an
// SD card at the right moment.
//
// ## The rules it keeps
//
// It is a FAKE, not a mock: it has working behaviour (a card that stores bytes, a clock that
// advances) rather than recorded expectations. Tests assert on what the app did to it.
//
// It obeys the ABI's contracts, because a fake that is more forgiving than the real thing tests
// nothing: paths are rejected the way <enaga/abi.h> says (not absolute, "." or "..", too long),
// at most ENAGA_FILES_OPEN handles exist at once, `file_read_all` returns ENAGA_FILE_TOO_BIG
// rather than truncating, and a file call made from the audio context returns
// ENAGA_FILE_WRONG_CONTEXT.
#pragma once

#include <cstdint>

#include <enaga/abi.h>

namespace strata::test {

// The fake. One instance, because `enaga::host()` takes no argument and the real one is a
// singleton too; `reset()` is what separates one test from the next.
class FakeHost {
public:
    static FakeHost &the();

    // Forget everything: the card, the clock, the queued ticks, the counters. Every test that
    // touches app/ starts with this, or it inherits the last one's card.
    void reset();

    // --- the clock --------------------------------------------------------
    //
    // `source` is ENAGA_CLOCK_INTERNAL, _MIDI or _SYNC. Under MIDI and SYNC the transport calls
    // are REFUSED, exactly as the real clock refuses them, which is the behaviour CLAUDE.md
    // requires STRATA to handle and which nothing has ever exercised.

    void set_source(uint32_t source);
    void set_tempo(float bpm);                 // whatever the source, as a measured tempo would be
    void set_running(bool running);            // without going through clock_control
    void set_position(uint32_t ticks, float fraction = 0.0f);

    bool running() const { return clock_.running != 0; }
    float tempo() const { return clock_.tempo; }
    uint32_t position() const { return clock_.position; }

    // Queue a tick for the NEXT block. `clock_ticks` hands the queue out once and empties it, so
    // a block that is not given ticks reports none -- which is the common case and the one a
    // scheduler bug hides in.
    void deliver_tick(uint32_t tick, uint32_t frame);

    // Queue `count` ticks `step` apart starting at `first`, spread over a block of `frames`.
    // The shorthand for "play normally for a block".
    void deliver_ticks(uint32_t first, uint32_t count, uint32_t step, uint32_t frames);

    int ticks_pending() const { return tick_count_; }

    // How many times the app asked the transport to do something: ENAGA_CLOCK_START, _STOP,
    // _CONTINUE, _LOCATE, _TEMPO. `control_value` is the last value passed with that command.
    int control_calls(uint32_t command) const;
    int32_t control_value(uint32_t command) const;

    // --- the card ---------------------------------------------------------

    void set_card(bool present);               // false: every file call gives ENAGA_FILE_NO_CARD
    void set_full(bool full);                  // true: writes give ENAGA_FILE_FULL
    void set_read_error(bool fail);            // true: reads give ENAGA_FILE_CARD_ERROR
    void set_audio_context(bool in_audio);     // true: file calls give ENAGA_FILE_WRONG_CONTEXT
    // true: `files::save` into a folder nobody made gives ENAGA_FILE_NOT_FOUND, as a FAT driver
    // does (FatFs answers FR_NO_PATH). Off by default, because the ABI comment does not say what
    // Enaga's own save does with a missing parent -- so this is the strict reading, opted into by
    // the tests that check STRATA makes its folders before it writes into them. The data folder
    // counts as existing: Enaga names it, and STRATA's settings live there.
    void set_require_folders(bool require);

    // Seed or inspect the card directly, without going through the app.
    bool write_file(const char *path, const void *data, uint32_t bytes);
    // A file of `bytes`, to be filled in place through the pointer returned (nullptr when the
    // card has no room). For files too big for any buffer a test could hold.
    uint8_t *create_file(const char *path, uint32_t bytes);
    int32_t read_file(const char *path, void *out, uint32_t max) const;

    // The first `max` bytes, however big the file is. `read_file` mirrors the ABI and refuses a
    // buffer smaller than the file (ENAGA_FILE_TOO_BIG), which is right for the app and useless
    // for a test that wants to look at a 44-byte header on a 5 MiB take.
    int32_t read_prefix(const char *path, void *out, uint32_t max) const;
    bool has_file(const char *path) const;
    int32_t file_bytes(const char *path) const;   // -1 when there is no such file
    bool remove_file(const char *path);
    int file_count() const { return count_; }
    const char *file_name(int index) const;       // for "which files did a save write?"

    // How many times each file call was made, so a test can assert that a save wrote TEN files
    // and not nine, or that a clean project wrote none at all.
    int saves() const { return saves_; }
    int loads() const { return loads_; }
    int read_alls() const { return read_alls_; }
    int folders_made() const { return folders_made_; }

    // --- the audio input (enaga::system::input) and headphones ----------------
    //
    // The route last chosen, -1 before any; `set_input_result` makes the next calls answer as the
    // codec would (-1 unknown source or level, -2 no answer). Headphones are out by default.
    int input_source() const { return input_source_; }
    int input_level() const { return input_level_; }
    int input_calls() const { return input_calls_; }
    void set_input_result(int32_t result) { input_result_ = result; }
    void set_headphones(bool in) { headphones_ = in; }

    // --- held asynchrony --------------------------------------------------
    //
    // `file_load` queues its completion instead of calling `done` straight away, so a load is in
    // flight until a test says otherwise. The real host completes from its main loop, which is
    // the same ordering: never inside the call.

    int pending_loads() const { return pending_; }
    void complete_loads();                     // fire every queued completion, oldest first
    void fail_pending_loads(int32_t error);    // fire them with an error instead

    // --- the panel --------------------------------------------------------

    const char *last_log() const { return log_[0] == '\0' ? nullptr : log_; }
    int log_lines() const { return log_lines_; }
    bool logged(const char *text) const;       // did any line equal this?

    int leds_set() const { return leds_; }
    void set_audio_load(uint32_t per_mille);   // what enaga::audio_load() reports
    void set_device(uint32_t device);          // what enaga::system::device() reports; 0 after reset

    // Every `Canvas::text` call since the last `clear_text()`, in order. The real host draws the
    // string with Enaga's 5 x 7 font; this one only records it, so a test can check where text
    // lands (tests/test_ui.cpp's layout checks) without a copy of the font's bitmaps.
    struct TextDraw {
        int x, y;
        uint8_t grey;
        int length;                            // the whole string's, even past `text`'s room
        char text[48];
    };
    static constexpr int kTextDraws = 64;
    void clear_text() { text_count_ = 0; text_dropped_ = 0; }
    int text_count() const { return text_count_; }
    int text_dropped() const { return text_dropped_; }   // calls past kTextDraws
    const TextDraw &text_draw(int index) const { return text_[index]; }

    // The struct the app sees. `enaga::host()` returns this.
    const enaga_host_t &abi() const { return abi_; }

private:
    FakeHost();
    void install();                            // fill abi_ with the trampolines below

    // --- the card's storage ---
    //
    // One arena and a table, both fixed: 4 MiB holds a whole project (a bank file is about
    // 129 KiB and there are eight) with room for a WAV or two, and there is no allocation here
    // for the same reason there is none in the app.
    // ENAGA_FILES_OPEN is an unsigned macro and this build is -Werror=sign-compare, so the
    // handle count gets a signed name rather than a cast at each of its six comparisons.
    static constexpr int kHandles = int(ENAGA_FILES_OPEN);

    static constexpr int kMaxFiles = 48;
    // 48 MiB: a project or two, and the long WAVs the chunked loader is tested with (a file
    // bigger than the loader's 16.5 MiB staging buffer, and one past Cmd::set_sample's 24 bits).
    static constexpr uint32_t kArenaBytes = 48u << 20;
    static constexpr uint32_t kPathMax = ENAGA_FILE_PATH_MAX + 1u;

    struct Entry {
        char path[kPathMax];
        uint32_t offset;
        uint32_t bytes;
        uint32_t capacity;                     // so a rewrite in place does not move the arena
        bool folder;
    };

    struct Handle {
        bool open;
        int entry;
        uint32_t position;
        uint32_t mode;
    };

    struct Pending {
        enaga_file_done_t done;
        void *user;
        int32_t result;
    };

    int find(const char *path) const;
    int add(const char *path, uint32_t bytes, bool folder);
    int32_t path_check(const char *path) const;   // ENAGA_FILE_OK or the ABI's refusal
    int32_t card_check() const;                   // no card, wrong context

    // --- the trampolines the ABI needs ---
    //
    // The ABI is C function pointers, so each one is a static that forwards to `the()`. Written
    // out rather than generated by a macro: a reader comparing this file with <enaga/abi.h>
    // should be able to see every one of the 38 at a glance.
    static void t_log(const char *text);
    static uint32_t t_ms();
    static void t_fatal(const char *reason);
    static void t_text(uint8_t *canvas, int x, int y, const char *text, uint8_t grey);
    static uint32_t t_audio_load();
    static uint32_t t_audio_load_peak();
    static void t_led(uint32_t led, uint32_t red, uint32_t green, uint32_t blue);
    static int32_t t_file_open(const char *path, uint32_t mode);
    static int32_t t_file_close(int32_t file);
    static int32_t t_file_size(int32_t file);
    static int32_t t_file_seek(int32_t file, uint32_t position);
    static int32_t t_file_info(const char *path, enaga_file_info_t *info);
    static int32_t t_file_remove(const char *path);
    static int32_t t_file_make_folder(const char *path);
    static int32_t t_file_read_all(const char *path, void *buffer, uint32_t max);
    static int32_t t_file_save(const char *path, const void *data, uint32_t bytes);
    static int32_t t_file_read(int32_t file, void *buffer, uint32_t bytes, enaga_file_done_t done,
                               void *user);
    static int32_t t_file_write(int32_t file, const void *data, uint32_t bytes,
                                enaga_file_done_t done, void *user);
    static int32_t t_file_load(const char *path, void *buffer, uint32_t max,
                               enaga_file_done_t done, void *user);
    static int32_t t_file_list(const char *path, enaga_file_entry_t entry, enaga_file_done_t done,
                               void *user);
    static const char *t_file_data_folder();
    static int32_t t_stream_open(const char *path, uint32_t offset, void *ring,
                                 uint32_t ring_bytes);
    static int32_t t_stream_create(const char *path, uint32_t reserve, void *ring,
                                   uint32_t ring_bytes);
    static uint32_t t_stream_read(int32_t stream, void *out, uint32_t bytes);
    static uint32_t t_stream_write(int32_t stream, const void *data, uint32_t bytes);
    static int32_t t_stream_status(int32_t stream, enaga_stream_status_t *status);
    static int32_t t_stream_close(int32_t stream, const void *header, uint32_t header_bytes);
    static void t_open_menu();
    static void t_set_busy(uint32_t busy);
    static void t_speaker(uint32_t on);
    static int32_t t_headphones();
    static uint32_t t_device();
    static int32_t t_input(uint32_t source, uint32_t level);
    static int32_t t_midi_send(uint32_t ports, const uint8_t *data, uint32_t bytes);
    static uint32_t t_midi_channel();
    static int32_t t_clock_control(uint32_t command, int32_t value);
    static void t_clock_state(enaga_clock_state_t *state);
    static uint32_t t_clock_ticks(enaga_clock_tick_t *ticks, uint32_t max);

    enaga_host_t abi_{};

    // --- the clock ---
    enaga_clock_state_t clock_{};
    static constexpr int kMaxTicks = 32;
    enaga_clock_tick_t ticks_[kMaxTicks]{};
    int tick_count_ = 0;
    int control_[8]{};                        // indexed by ENAGA_CLOCK_* command, 1-5
    int32_t control_value_[8]{};

    // --- the card ---
    uint8_t *arena_ = nullptr;
    uint32_t used_ = 0;
    Entry files_[kMaxFiles]{};
    int count_ = 0;
    Handle handles_[kHandles]{};
    static constexpr int kMaxPending = 16;
    Pending pending_queue_[kMaxPending]{};
    int pending_ = 0;

    bool card_ = true;
    bool full_ = false;
    bool require_folders_ = false;
    bool parent_exists(const char *path) const;
    bool read_error_ = false;
    bool in_audio_ = false;

    int input_source_ = -1;
    int input_level_ = -1;
    int input_calls_ = 0;
    int32_t input_result_ = 0;
    bool headphones_ = false;
    int saves_ = 0;
    int loads_ = 0;
    int read_alls_ = 0;
    int folders_made_ = 0;

    // --- the panel ---
    static constexpr int kLogMax = 128;
    char log_[kLogMax]{};                     // the most recent line
    static constexpr int kLogHistory = 32;
    char log_history_[kLogHistory][kLogMax]{};
    int log_lines_ = 0;
    TextDraw text_[kTextDraws]{};
    int text_count_ = 0;
    int text_dropped_ = 0;
    int leds_ = 0;
    uint32_t audio_load_ = 0;
    uint32_t device_ = 0;
    uint32_t ms_ = 0;
};

}  // namespace strata::test
