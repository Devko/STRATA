// The one channel from the UI to the audio side.
//
// docs/SPEC.md, "Software architecture": the UI never writes pattern memory directly. It posts
// every edit as a command, which the audio side applies at the start of the next block. That is
// what lets a save run in the main context while the transport keeps playing, and it is why
// there is no lock anywhere in this app.
//
// Rules from the spec, enforced here:
//   - commands are fixed-size 16-byte structs
//   - the ring is a power of two, single-producer single-consumer
//   - the audio side applies at most 64 per block, so a burst cannot break the deadline
//
// ## Why this is safe without a lock
//
// One writer (the main context) owns `write_`; one reader (the audio context) owns `read_`. Each
// only ever publishes its own index, and reads the other's. The payload is written before
// `write_` advances and read after `read_` is seen to lag, so a release/acquire pair on the two
// indices is all the ordering needed. The C674x is a single core with no store buffer to worry
// about, but the atomics are still correct and cost nothing measurable.
//
// A full ring drops the command and says so. Dropping is the right failure: the alternative is
// blocking the UI thread or overwriting an edit the audio side has not applied yet. The UI shows
// the count so a dropped edit is visible rather than mysterious.
#pragma once

#include <atomic>
#include <cstdint>

namespace strata {

// What a command does. Serialized nowhere, so these may be renumbered freely.
enum class Cmd : uint8_t {
    none = 0,

    // --- transport and clock ---
    play,              // a:ignored
    stop,              // a: 1 = cut all voices (the second STOP press)
    set_tempo,         // f: BPM

    // --- pattern editing ---
    toggle_trig,       // track, step, a: 1 = the trigless bit instead of the trig
    clear_lock,        // track, step, a: param
    set_track_length,  // track, a: length; or c = 1: b is an int8 delta (kDeltaFlag below)
    set_track_speed,   // track, a: speed
    set_pattern_swing, // a: swing; or c = 1: b is an int8 delta

    // --- pattern editing, M3 -----------------------------------------------
    //
    // These carry a DELTA rather than a value, which is the opposite of the M2 commands above
    // and deliberate. An encoder sends several clicks per audio block, and the UI can only read
    // the pattern as it was a block ago, so computing "current + delta" on the UI side loses
    // every click after the first. Applying the delta on the audio side, to the authoritative
    // step, cannot lose one. The cost is that the screen shows the result one block late, which
    // at 1.33 ms is not a thing a person can see.
    step_field_delta,  // track, step, a: StepField, b: int8 delta
    lock_delta,        // track, step, a: param index, b: int8 delta

    // The Euclidean generator (docs/SPEC.md, "Musical defaults that help creativity"): a whole
    // track's rhythm from three numbers. One edit, so it carries an undo mark like any other and
    // a sweep of the knob coalesces into one level.
    euclid,            // track, a: hits, b: cycle steps, c: int8 rotation

    undo_mark,         // track, step, a: GestureKind, b: the UI's gesture counter
    undo,
    redo,

    range_copy,        // track, step: the first step, a: how many
    range_paste,       // track, step: the first step
    range_erase,       // track, step: the first step, a: how many
    track_erase,       // track
    range_transpose,   // track, step: the first step, a: how many, b: int8 semitones
    range_nudge,       // track, step: the first step, a: how many, b: int8 ticks

    // A live-recorded hit, already resolved to a step and a micro-timing value by core/record in
    // the main context -- where the clock state that stamped it was read.
    record_hit,        // track, step, a: note, b: velocity, c: int8 micro

    // --- projects and banks, M4 --------------------------------------------
    //
    // A queued pattern change is armed on the AUDIO side, which is why core/queue.hpp's Queue
    // needs no atomics: both the arming and the per-tick "is it due yet" happen in the same
    // context. The UI's part is posting this.
    queue_pattern,     // a: bank, b: slot, c: QueueMode
    cancel_queue,
    set_chain,         // a: 0 off, 1 on (starting one jumps to its first entry)

    // The kit the machines read (`params_`) and the kit a file holds (`project_.kit`) are
    // different structures in different layers, so they are copied across explicitly: `store`
    // before a save, `recall` after a load. Both run on the audio side, which owns both.
    kit_store,
    kit_recall,

    // The global key and scale (docs/SPEC.md, "Musical defaults that help creativity"). Both
    // apply to the PROJECT's settings and to the live pattern at once: the pattern's own copy is
    // what the sequencer and core/mutate read, so a pattern saved in its own key keeps it, and
    // the project's copy is the default a new pattern gets.
    set_root,          // a: pitch class 0-11
    set_scale,         // a: a core/scale.hpp Scale id

    // A project was loaded: 1.03 MiB arrived through `Engine::project_for_load` rather than
    // through this ring, so everything derived from the pattern is now stale. `kit_recall` alone
    // is not enough -- the scheduler's track lengths, speeds and swing all came from the pattern
    // that was replaced, and the undo history belonged to it too.
    project_loaded,

    // Sample swapping, in the order docs/SPEC.md requires. `clear_sample` takes the sample away
    // from a track and bumps that track's epoch in Status; only once the UI has SEEN the epoch
    // change is the audio side known to have stopped reading, and only then may the loader
    // release the pool range and write over it. `set_sample` hands the new one over.
    //
    // Doing it the other way round -- release, then load, then hand over -- overwrites memory a
    // voice is still reading, which sounds like a broken sampler rather than a broken lifetime.
    clear_sample,      // track
    // track, a: channels, u: byte offset into the sample region, c|d|e: frames (24 bits, which
    // is 16.7 M frames -- 349 s of mono at 48 kHz, well past the 30 s above which docs/SPEC.md
    // streams from the card instead of loading into RAM)
    set_sample,

    // --- kit and sound ---
    set_param,         // track, a: param index, b: value; or c = 1: b is an int8 delta
    set_track_level,   // track, f: level; or c = 1: f is a delta, the result clamped to 1.5
    // One send or master setting. `a` is the field -- dsp/buses.hpp's SendField or MasterField,
    // whose numbers are permanent because they reach PROJECT.BIN -- and `b` is an encoder DELTA,
    // as a signed byte, exactly like `set_param` and the step fields and for the same reason.
    //
    // What moves is a BYTE, 0-127, because that is what the project stores and what the encoder
    // produces, so the file, the screen and the sound cannot disagree; the engine turns it into
    // the value the mixer runs on, and for the delay's time it needs the tempo to do it.
    set_sends,         // a: SendField, b: int8 delta
    set_master,        // a: MasterField, b: int8 delta

    // --- performance, M5 ---------------------------------------------------
    //
    // Every one of these changes how the music sounds without touching a stored pattern, which
    // is docs/SPEC.md's whole point about the performance layer: "Eight tools change the music
    // live without touching the stored pattern unless you commit." The exceptions, which DO
    // commit and say so in the table there, are `mutate` (one undo level) and the scene and
    // macro setup (saved in PERFORM.BIN).
    set_mute,          // track, a: 0 or 1, b: 1 = at the next bar instead of now
    set_fill,          // a: 0 or 1

    // One undoable variation of one track's range. `u` is the variant: the same variant over the
    // same pattern reproduces the same mutation exactly, which is what "seeded" means in
    // core/mutate.hpp. "OK keeps it, CLR reverts" needs nothing here -- the UI marks an undo
    // level first and CLR is the undo that already exists.
    mutate,            // track, step: the first step, a: Mutation, b: amount 0-100, c: how many

    // Scenes. A capture takes the CURRENT kit, which is why it happens on this side: `params_`
    // is what the machines read and the UI only has a block-old copy of it.
    scene_capture,     // a: scene, b: 0 = every track, 1 = `track` only
    scene_erase,       // a: scene
    scene_select,      // a: 0 = slot A, 1 = slot B, b: scene
    set_morph,         // u: 0..kMorphScale

    // Macros. `macro_assign` takes the next free target of that macro rather than an index: the
    // UI assigns by pointing at a parameter, and which of the four slots it lands in is not
    // something a performer should have to think about.
    set_macro,         // a: macro, b: position 0-127
    macro_assign,      // a: macro, track, b: param, c: low, d: high
    macro_clear,       // a: macro

    // The eight stored mute states (core/perform.hpp). A recall may wait for the bar, like a
    // mute does.
    mute_store,        // a: slot 0-7
    mute_recall,       // a: slot 0-7, b: 1 = at the next bar

    set_auto_fill,     // a: AutoFill

    // A throw holds a track's delay or reverb send at full and puts it back on release. It is
    // not a parameter edit: `params_` is untouched, so nothing is saved and nothing is undone.
    set_throw,         // track, a: 0 = off, 1 = delay (send A), 2 = reverb (send B)

    // Beat repeat. The length is in note values and the engine turns it into frames, because
    // only the audio side knows the tempo in samples (dsp/repeat.hpp).
    set_repeat,        // a: RepeatLength, b: 1 = held, c: 1 = pitch down
    trigger_note,      // track, a: note, b: velocity -- a pad hit, played live
    release_note,      // track, a: note

    // Resampling, docs/SPEC.md's eighth performance tool. Three commands, because a take spans
    // both contexts: the audio side records it, the main context writes the file.
    //
    // `resample_arm` asks for `a` BARS (1, 2, 4 or 8). The engine turns bars into frames -- only
    // it knows the tempo in samples -- and begins on the next bar line, because a take that is
    // not bar-aligned does not loop, which is the whole point of recording a round number of
    // bars. `resample_release` is the main context saying it has finished with the buffer, which
    // is what lets the next take be armed (dsp/capture.hpp).
    resample_arm,      // a: bars 1-8
    resample_cancel,
    resample_release,

    // Which machine a track hosts (core/project.hpp's MachineId, which is what KIT.BIN stores).
    // The audio side swaps the pointer at a block boundary and kills the voices of the machine
    // being left, because a machine that is no longer rendered cannot release its own tail.
    set_machine,       // track, a: MachineId

    // The live-record quantize amount, 0-100 (docs/SPEC.md, "Recording"). It is a PROJECT
    // setting with a field in PROJECT.BIN, so the engine keeps it and `app/` reads it back
    // rather than holding a second copy that a save could not see.
    set_quantize,      // b: int8 delta, clamped to 0-100

    // F1 + F5 (docs/SPEC.md): every track back to its step 1 from the next tick, without
    // touching the voices. The main context has already located the clock to 0; this resets
    // what the audio side derives from the position -- the pattern's origin, its pass count and
    // the chaining state that 1ST and the conditions read.
    restart,

    // PROJECT's CHAIN page. step: the entry index, a: ChainEdit, b: bank, c: slot, d: repeats.
    // core/queue.hpp's chain_set / chain_insert / chain_remove, which keep the chain a prefix.
    chain_edit,

    // The pattern's master length in steps, 1-64: what a queued change and a chain wait for.
    // a: length; or c = 1: b is an int8 delta.
    set_master_length,

    count
};

// Exactly 16 bytes, as the spec requires. Everything an edit needs fits: a target, two small
// values and one float for the things that are genuinely continuous.
// `Command::c` set to this on set_param, set_track_length, set_pattern_swing and
// set_track_level makes the value a DELTA, applied on the audio side to the authoritative value.
// The encoders send these: the UI reads engine state a block late, so "current + delta" computed
// there loses every click after the first in a block -- the reason the M3 commands are deltas.
// An absolute value (c = 0) is still what a load, a test or a recall sends.
constexpr uint8_t kDeltaFlag = 1;

// `Cmd::chain_edit`'s operation, in `a`.
// set, insert and remove carry the entry whole; `pattern_delta` and `repeats_delta` move the
// entry's pattern (through the 128, bank by bank) or its repeat count (1-16) by the int8 in `b`,
// so knob clicks applied a block late are not lost.
enum class ChainEdit : uint8_t { set = 0, insert, remove, pattern_delta, repeats_delta };

struct Command {
    Cmd cmd = Cmd::none;
    uint8_t track = 0;
    uint8_t step = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    // Three more bytes, which used to be padding. They cost nothing -- the struct has to be 16
    // bytes and `f` has to stay 4-byte aligned either way -- and they are what lets a recorded
    // hit carry its note, velocity and micro-timing in one command instead of three.
    uint8_t c = 0;
    uint8_t d = 0;
    uint8_t e = 0;
    float f = 0.0f;
    // A second 32-bit field, which used to be padding like c/d/e. A sample's byte offset into
    // the 64 MiB region needs 26 bits, so it needs a word of its own: `f` is a float and the
    // three spare bytes are not enough. Nothing is punned through `f` -- reading a float's bits
    // as an integer would be undefined behaviour for the sake of saving four bytes that the
    // struct has to carry anyway.
    uint32_t u = 0;
};

static_assert(sizeof(Command) == 16, "the spec fixes commands at 16 bytes");
static_assert(alignof(Command) == 4, "a misaligned 32-bit read on this chip silently misreads");

// A single-producer single-consumer ring. Capacity must be a power of two so the wrap is a mask
// rather than a division -- there is no division in the audio path.
template <uint32_t kCapacity>
class CommandRing {
public:
    static_assert(kCapacity >= 2, "a ring needs room");
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    // The most commands the audio side applies in one block. The spec's figure: a burst of edits
    // cannot be allowed to break the 100 ms deadline, and 64 is far more than a human generates.
    static constexpr uint32_t kMaxPerBlock = 64;

    // Main context only. Returns false when the ring is full, having changed nothing.
    bool post(const Command &command) {
        const uint32_t write = write_.load(std::memory_order_relaxed);
        const uint32_t next = (write + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire)) {
            // Load-then-store, not fetch_add: only the main context ever writes this, and the
            // tic6x toolchain is built without libatomic, so an atomic read-modify-write would
            // need a __atomic_fetch_add helper that does not exist. Single-writer discipline
            // makes the plain pair correct, which is what docs/SPEC.md asks for anyway.
            dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return false;
        }
        slots_[write] = command;
        write_.store(next, std::memory_order_release);   // publishes the payload with it
        return true;
    }

    // Audio context only. Returns false when empty.
    bool take(Command &out) {
        const uint32_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        out = slots_[read];
        read_.store((read + 1) & kMask, std::memory_order_release);
        return true;
    }

    // How many edits the UI posted that did not fit. Shown on screen: a dropped edit should be
    // visible, not mysterious.
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    bool empty() const {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

private:
    static constexpr uint32_t kMask = kCapacity - 1;

    Command slots_[kCapacity];
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    std::atomic<uint32_t> dropped_{0};
};

// 256 slots is 4 KiB: four blocks' worth of the per-block limit, so even a motion-recording
// sweep with every encoder moving has room.
using Commands = CommandRing<256>;

// --- the other direction --------------------------------------------------

// What the audio side publishes every block for the UI to read. Single writer (audio), single
// reader (UI), and the UI only ever wants the newest value -- so no ring, just words that are
// each written by one side. A torn read here costs one frame of a slightly stale meter, which
// is why these are plain relaxed atomics and not a lock.
//
// NOTE: no atomic read-modify-write anywhere in here. The tic6x toolchain is built with
// --disable-libatomic, so fetch_add on a byte needs a __atomic_fetch_add_1 helper that does not
// exist and the app fails to LINK (it compiles fine, which makes it a confusing failure). Plain
// loads and stores of a single word are fine, and with one writer per field the load-then-store
// pair below is correct without an RMW.
struct Status {
    std::atomic<uint32_t> tick{0};             // the clock's position, for the playhead
    std::atomic<uint32_t> blocks{0};           // audio blocks processed, for liveness
    std::atomic<uint8_t> playhead[8] = {};     // per-track step, for the grid
    std::atomic<uint8_t> triggered[8] = {};    // bumped on each trig, for the pad LEDs
    std::atomic<uint16_t> peak_milli{0};       // master peak x 1000
    // How many TRACKS rendered last block (an active machine, not muted), 0-8. Not a voice
    // count: a six-note POLY chord is one. Shown as TRK on the status line.
    std::atomic<uint8_t> voices{0};
    std::atomic<uint8_t> running{0};

    // M3. How deep the undo history is, so the screen can say whether F1+CLR will do anything,
    // and a count of edits the audio side had to refuse -- a paste that did not fit in the lock
    // pool, almost always. A refused edit has to be visible: the same reasoning as the dropped
    // command count above, and the same failure if it is not (the user repeats the edit and
    // concludes the box is broken).
    std::atomic<uint8_t> undo_depth{0};
    std::atomic<uint8_t> redo_depth{0};
    std::atomic<uint32_t> refused{0};

    // M4. Per track, bumped by the audio side when it stops reading that track's sample. The
    // loader watches for the change before it releases the memory and loads over it -- the
    // generation counter docs/SPEC.md requires for a sample swap. A wrap is harmless: the UI
    // compares against the value it saw, it does not count.
    std::atomic<uint8_t> sample_epoch[8] = {};

    // M6. Resampling. `resample_state` is 0 idle, 1 armed (waiting for the bar line), 2
    // recording, 3 a finished take the card has not had yet; `resample_frames` is how much is in
    // it. The screen draws a progress bar from the two, and the card side watches for state 3 --
    // the same single-writer-word discipline as everything else here, because the take's buffer
    // belongs to the audio side until then (dsp/capture.hpp).
    std::atomic<uint8_t> resample_state{0};
    std::atomic<uint32_t> resample_frames{0};
    std::atomic<uint8_t> resample_clamped{0};   // the take was longer than the buffer holds

    // Which pattern is live, so the UI can draw it without reaching into the project.
    std::atomic<uint8_t> live_bank{0};
    std::atomic<uint8_t> live_slot{0};

    // Audio context only, so the load and the store cannot interleave with another writer.
    void bump_sample_epoch(int track) {
        const uint8_t next = uint8_t(sample_epoch[track].load(std::memory_order_relaxed) + 1);
        sample_epoch[track].store(next, std::memory_order_relaxed);
    }

    // Audio context only, so load-then-store is safe with one writer (see the note above).
    void bump_refused() {
        refused.store(refused.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void publish_playhead(int track, uint8_t step) {
        playhead[track].store(step, std::memory_order_relaxed);
    }

    // Audio context only, so the read and the write cannot interleave with another writer.
    // The UI watches this for a change rather than reading the count, so a wrap is harmless.
    void bump_triggered(int track) {
        const uint8_t next = uint8_t(triggered[track].load(std::memory_order_relaxed) + 1);
        triggered[track].store(next, std::memory_order_relaxed);
    }

    void bump_blocks() {
        blocks.store(blocks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

}  // namespace strata
