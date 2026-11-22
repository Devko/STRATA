// The card side of STRATA: projects on disk, autosave, and background sample loading.
//
// This is the MAIN context's half of M4. `app/engine.cpp` is the audio side and never touches a
// file; everything slow lives here, and the two meet only through the command ring and the
// published Status, exactly as docs/SPEC.md's "Software architecture" requires.
//
// ## One job at a time, a slice per pass
//
// `service()` is called from `App::update`, which runs at least 750 times a second, and it does
// a bounded amount of work per call: one file, or one slice of a conversion. Nothing here spins,
// waits on the audio side, or writes more than about 130 KiB in one go -- a bank, which
// docs/SPEC.md budgets at "tens of milliseconds of the UI, never of the audio".
//
// There is deliberately only ONE job in flight. Enaga allows 8 open files, but a second
// concurrent load would double the staging memory and make the sample-swap protocol below race
// with itself for no benefit a person could hear. Requests queue instead.
//
// ## The two protocols this file exists to get right
//
// **Saving must not read a torn step.** A save runs here while the audio side may be applying an
// edit to the live pattern. So a save request posts `Cmd::kit_store` and then WAITS for the
// command ring to drain. Posting makes the ring non-empty, which defers the save by at least one
// pass, and by the time it is empty the audio side has applied every pending edit and is writing
// nothing. The UI is single-threaded, so once the ring is empty and the UI is the one saving,
// nothing is writing pattern memory at all. That holds for ONE pass: a save is a file per pass
// and the UI posts between passes, so every pass of a save or a load waits for the ring again.
//
// **A load additionally requires the transport stopped.** With the clock stopped
// `clock::ticks()` yields nothing, so the scheduler never reads the pattern and 1.03 MiB can be
// written into it from here. `Engine::project_for_load()` is the one hole in "the main context
// never writes pattern memory" and it is deliberate: a project cannot travel through a 16-byte
// command. Under an external clock `clock::stop()` returns false and the load is REFUSED rather
// than done anyway -- CLAUDE.md's rule about handling the refusal.
//
// **The sample swap.** `SamplePool::release` makes memory reusable immediately and `core/`
// deliberately cannot tell whether a voice is still reading it. So the order is
// `Cmd::clear_sample` -> wait for `Status::sample_epoch[track]` to change -> release the range ->
// load -> `Cmd::set_sample`. Releasing before the epoch confirms means writing over memory a
// voice is reading, which sounds like a broken sampler rather than a broken lifetime.
//
// ## Why the dirty flags are here and not in Project
//
// `core/project.hpp` has a `bank_dirty[]`, described as "set by an edit and cleared by a save".
// An edit happens on the AUDIO side and a save happens here, so that field would be a plain bool
// written by one context and cleared by the other -- precisely the cross-context state CLAUDE.md
// forbids outside a single-writer atomic or an SPSC ring. The UI knows which commands it posts,
// so it tracks dirtiness itself, in one context, and `Project::bank_dirty` is left to
// `core/project.cpp`'s own use (`bank_read` clearing it after a load).
//
// The cost is that a bank is marked dirty even if the audio side refuses the edit. That is one
// unnecessary 130 KiB write in a case that does not otherwise happen, which is the cheaper
// mistake by a wide margin.
#pragma once

#include "commands.hpp"
#include "pcm.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "samplepool.hpp"

#include <enaga/files.hpp>
#include <enaga/settings.hpp>
#include <enaga/wav.hpp>

#include <cstdint>

namespace strata {

class Engine;

// How long a sample may be and still be loaded whole into RAM. docs/SPEC.md puts streams at
// "loops longer than 30 s", so 30 seconds is the design limit, and it is what sizes the staging
// buffers below rather than a number picked to make them fit.
//
// Thirty seconds OF THE FILE, at its own rate, up to kMaxSampleSourceRate. The buffers used to
// be sized in 48 kHz frames, so a 96 kHz file got 15 s and a 30 s one was refused TOO LONG
// (Roland, 2026-11-22: the limit should mean 30 s of the file's own length). A lower rate gets
// more than 30 s out of the same frames -- 44.1 kHz about 65 s -- which costs nothing and is not
// refused; a higher one (176.4 or 192 kHz) gets half. What a long file costs once LOADED is the
// sample pool's to refuse (NO ROOM), as it always was.
constexpr uint32_t kMaxSampleSeconds = 30;
constexpr uint32_t kMaxSampleSourceRate = 96000;

class Storage {
public:
    // A project folder name. Settings values are 31 bytes, and the name has to fit in one with
    // room to spare, so this is the binding limit rather than the file system's.
    static constexpr int kNameBytes = 24;

    // App::init() only. Takes the sample region and the staging buffers from the heap -- every
    // allocation in this file happens here, as CLAUDE.md requires -- and hands the region's base
    // to the engine. False when the heap cannot provide them, which `App::init` reports.
    bool init(Engine &engine);

    // One bounded slice of whatever is in flight. Call from App::update.
    void service();

    // --- projects ---------------------------------------------------------

    // The user asked to save. Posts `Cmd::kit_store` and defers until the ring drains.
    void request_save();

    // The transport stopped, or App::stop is about to. Same protocol, written to AUTOSAVE/.
    void request_autosave();

    // Read the current project off the card again, discarding what is in RAM. Refused, with a
    // reason on screen, under an external clock that will not stop.
    void request_reload();

    // An empty project, in RAM only: nothing is written until a save. Queued like a load, and
    // for the same reason: it rewrites all of pattern memory from the main context, so it waits
    // for the command ring to drain. It needs no card, so it runs without one.
    void request_new();

    // App::init: the last project named in the settings, or the autosave if it is newer. False
    // when there was nothing to open, which is not an error -- it means a first run, and the
    // engine's built-in default pattern stays.
    bool open_last();

    // App::stop(), whose budget is 3 s including saves. Blocking and bounded: it drains the ring
    // for a few milliseconds and then writes, because destructors never run on this platform and
    // this is the last chance to keep the user's work.
    void save_for_shutdown();

    // --- dirtiness --------------------------------------------------------

    // Every command the UI posts passes through here, so that a save knows which of the ten
    // files actually changed. `live_bank` is Status::live_bank at the moment of the post: a chain
    // may have moved the live pattern without the UI asking.
    void note_command(Cmd cmd, int live_bank);

    bool dirty() const;

    // --- samples ----------------------------------------------------------

    // Load the sample named in the project's kit for this track. Queued; `service()` runs it.
    // False for a track with no path, or an index that is not a track.
    bool request_sample(int track);

    // --- resampling -------------------------------------------------------

    // Write the finished take to /RECORD/STRATA_<n>.WAV, and -- when `into_track` is a track --
    // put it in that track's kit and load it straight back.
    //
    // Queued, like everything else: `service()` does the work when the take is ready, which is
    // when `Status::resample_state` reads 3. False only for an index that is not a track and is
    // not -1, because the take's own readiness is the audio side's to report and may not have
    // happened yet when the key is pressed.
    //
    // docs/SPEC.md asks for the whole thing "in one action", which is why the load-back is a
    // parameter here rather than a second gesture.
    bool request_resample_write(int into_track = -1);

    // The name the last take was written under, for the screen; empty before the first.
    const char *last_take_name() const { return take_name_; }

    // Queue every track whose kit names a file. What a project open does.
    void request_all_samples();

    // --- the kit library ---------------------------------------------------
    //
    // docs/SPEC.md's "Kit and machine select" (FUNC+EDIT, twice for the kits). Sixteen kits on
    // the card, shared by every project: `<data folder>/KITS/KIT_01.BIN` .. `KIT_16.BIN`, each in
    // KIT.BIN's own format, so a kit is exactly what a project's KIT.BIN holds -- machines,
    // parameters, faders and sample paths.
    //
    // A save is a project save's protocol in small: post `Cmd::kit_store`, wait for the ring to
    // drain, write one file. A load is a project load's in small, minus the stopped transport:
    // read the file into `Engine::kit_for_recall()` while the ring is empty, put the recalled
    // sample paths in place, post `Cmd::kit_recall` with a = 1, and send every track whose path
    // CHANGED through the ordinary sample swap. A track whose path did not change keeps playing.

    static constexpr int kKitSlots = 16;

    // What the browser shows of one slot. `unknown` until a scan has read it.
    enum class SlotState : uint8_t { unknown, empty, used, bad };
    struct KitSlot {
        SlotState state = SlotState::unknown;
        uint8_t machines[kTracks] = {};
    };

    // Read all sixteen slots, a file per pass, so the browser can say which hold a kit and what
    // is in it. Queued like everything else.
    void request_kit_scan();

    // Store the kit as it sounds now into a slot, or recall one. Queued; false for a slot that
    // does not exist or with no usable card.
    bool request_kit_save(int slot);
    bool request_kit_load(int slot);

    // A slot outside 0..15 reads as unknown.
    const KitSlot &kit_slot(int slot) const;

    // --- the card ---------------------------------------------------------

    // Enaga's on_card. On a removal every open handle is already dead, so the job in flight is
    // abandoned and nothing is written until a card comes back.
    void on_card(bool usable);

    bool card_usable() const { return card_usable_; }

    // --- for the screen ---------------------------------------------------

    const char *project_name() const { return name_; }

    // What is happening, in words the 5x7 font can draw, or nullptr when idle.
    const char *busy_text() const;

    // 0..100 through the job in flight; 0 when idle.
    int progress() const;

    // The last failure, or nullptr. Sticky until the next job starts, because a load that failed
    // silently is the least diagnosable thing a sampler can do.
    //
    // Two slots behind one accessor. A load ends by queueing eight sample swaps, so a reason
    // kept only in `error_` was wiped by the first of them within a millisecond -- invisible on
    // a screen that draws at 20 fps. A save's or a load's reason therefore also goes in
    // `project_error_`, which only another save or load clears, and this prefers whichever is
    // newer: a sample failure while one is in flight, the project's reason otherwise.
    const char *last_error() const { return error_ != nullptr ? error_ : project_error_; }

    const SamplePool &samples() const { return pool_; }

    // Which track the loader is working on, or -1.
    int loading_track() const;

    // True while a PROJECT load is in flight -- the job that rewrites pattern memory a pass at a
    // time. The transport must not be started under one: the audio side would play a pattern
    // half of which belongs to the project being replaced. A SAMPLE load is deliberately NOT
    // included; running the transport through one is the whole point of loading in the
    // background, and the pool's epoch handshake is what makes that safe.
    bool loading_project() const { return job_ == Job::load; }

private:
    // The states a job passes through. One at a time; see the header comment.
    enum class Job : uint8_t {
        idle,
        save,              // writing the ten files, one per pass
        load,              // reading them back
        sample_epoch,      // posted clear_sample, waiting for the audio side to confirm
        sample_read,       // files::load in flight
        sample_convert,    // pcm_to_s16, in slices
        sample_resample,   // pcm_resample, in slices
        take_write,        // a resample's WAV, header then payload
        kit_scan,          // the kit library's sixteen slots, one file per pass
        kit_save,          // one kit into its slot: started and finished in one pass
        kit_load,          // one kit out of its slot, likewise
    };

    // --- paths ---
    // "<data folder>/PROJECTS/<name>/<file>", or "<data folder>/AUTOSAVE/<file>". False when it
    // would not fit, rather than a truncated path that names the wrong file.
    bool project_path(char *out, uint32_t max, bool autosave, const char *file) const;
    bool folder_path(char *out, uint32_t max, bool autosave) const;

    // --- saving and loading ---
    void begin_job(Job job);
    void finish_job(const char *error);
    void step_save();
    void step_load();
    void step_take_write();
    void step_kit_scan();
    void run_kit_save();
    void run_kit_load();
    bool kit_path(char *out, uint32_t max, int slot, bool folder_only) const;

    // --- samples ---
    void step_sample_epoch();
    void step_sample_convert();
    void step_sample_resample();
    void start_next_sample();
    void start_new();
    bool flush_backlog();          // true when nothing is left waiting
    void sample_loaded(int32_t result);      // the files::load callback, main context
    void hand_sample_over();

    // One command, the way app/app.cpp posts them. A full Command for the sample handover,
    // which carries an offset, a frame count and a channel count.
    void post(Cmd cmd);
    void post(const Command &command);

    Engine *engine_ = nullptr;
    enaga::Settings settings_;

    char name_[kNameBytes] = {};
    bool card_usable_ = true;
    // Resampling. `take_pending_` is a request waiting for the audio side to finish the take;
    // `take_track_` is the track to load it into afterwards, or -1 for "just the file".
    bool take_pending_ = false;
    int take_track_ = -1;
    static constexpr uint32_t kTakeNameBytes = 24;
    char take_name_[kTakeNameBytes] = {};
    char take_path_[ENAGA_FILE_PATH_MAX + 1] = {};

    const char *error_ = nullptr;
    const char *project_error_ = nullptr;   // a save's or a load's reason; see last_error()

    Job job_ = Job::idle;
    int step_ = 0;                 // which file, or which slice
    bool autosave_ = false;        // the job in flight is writing AUTOSAVE/
    bool save_pending_ = false;    // requested, waiting for the ring to drain
    bool autosave_pending_ = false;
    bool load_pending_ = false;
    bool load_autosave_ = false;   // which folder the pending load reads; copied into autosave_
                                   // only when it starts, so a running save keeps its own
    static constexpr int kBacklog = 4;
    Command backlog_[kBacklog] = {};   // posts that met a full ring, sent first by service()
    int backlog_n_ = 0;
    bool new_pending_ = false;     // request_new, waiting for the ring like a load
    uint32_t save_seq_ = 0;        // monotonic, so a start can tell which of the two is newer

    bool project_dirty_ = false;
    // Does the PROJECT folder hold a complete set of the ten files?
    //
    // It decides whether an explicit save writes everything or only what is dirty, and it has to
    // exist because the dirty flags describe how RAM differs from a project ON THE CARD. For a
    // project that was never written there is nothing to differ from: `request_save` marks the
    // kit and nothing else, so a first save stored KIT.BIN alone and the reload of it failed
    // with NOT FOUND -- the user's own work, reported as saved and gone.
    //
    // False at init (the default beat lives in RAM and has no files) and after `request_new`,
    // which deliberately takes a new name and so names a folder that does not exist yet. True
    // after a save that wrote every file, and after a successful load, which could not have
    // happened unless the files were there.
    bool project_complete_ = false;

    // The kit library. -1 when nothing is asked for.
    int kit_save_slot_ = -1;
    int kit_load_slot_ = -1;
    bool kit_scan_pending_ = false;
    KitSlot kit_slots_[kKitSlots] = {};
    Kit kit_scratch_;              // a scan reads each slot into this, never into the engine

    bool perform_dirty_ = false;   // the scenes and macro setup, in PERFORM.BIN
    bool kit_dirty_ = false;
    bool bank_dirty_[kBanks] = {};

    // --- the sample loader ---
    uint8_t sample_queue_ = 0;     // one bit per track, waiting to be loaded
    uint32_t wanted_budget_ = 0;   // a loaded project's budget, applied once nothing is reserved
    int sample_track_ = -1;        // the track being loaded, or -1
    uint8_t epoch_before_ = 0;     // Status::sample_epoch[track] when clear_sample was posted
    uint32_t epoch_waited_ = 0;    // passes spent waiting, so a lost epoch cannot wait for ever

    bool read_in_flight_ = false;  // a files::load owns raw_ until its callback
    bool read_done_ = false;
    int32_t read_result_ = 0;

    enaga::wav::Format format_;
    PcmFormat encoding_ = PcmFormat::unknown;
    uint32_t src_frames_ = 0;      // frames in the file
    uint32_t src_done_ = 0;        // converted so far
    uint32_t out_frames_ = 0;      // frames the sampler will get
    uint32_t out_done_ = 0;
    uint32_t out_offset_ = 0;      // the pool's byte offset for this slot
    Resampler cursor_;
    Antialias antialias_;          // a downsample's low-pass, carried across slices

    // The buffers, all taken in init().
    //
    //   region_   the 64 MiB sample region the pool carves up, and the only one the audio side
    //             ever reads. The pool and the loader live here, so no pool state crosses the
    //             ring (app/engine.hpp's `sample_region_`).
    //   raw_      one whole file as it is on the card, for files::load. docs/SPEC.md asks for
    //             files::load by name, and it wants a buffer for the whole file.
    //   src_      the file converted to int16 at ITS rate, which a resample reads from. Only
    //             needed when the file is not at 48 kHz, but allocated once either way: there is
    //             no allocating later (CLAUDE.md).
    //   file_     one project file at a time. A bank is the biggest at about 129 KiB.
    //
    // raw_ and src_ are a fixed cost, not a per-sample one, and they are sized from
    // kMaxSampleSeconds at kMaxSampleSourceRate rather than chosen to fit: 30 s of a 96 kHz
    // stereo 24-bit file (plus room for its header and the chunks a WAV carries after its data)
    // is the largest file the design loads whole, and 30 s of 96 kHz stereo int16 is the largest
    // thing a resample reads. A 96 kHz 32-bit float file is 23 MB at 30 s and gets about 22 s
    // (files::load refuses a longer one as TOO BIG). tests/test_memory.cpp holds the total to
    // the heap.
    int16_t *region_ = nullptr;
    uint8_t *raw_ = nullptr;
    int16_t *src_ = nullptr;
    uint8_t *file_ = nullptr;

    SamplePool pool_;

    static constexpr uint32_t kRegionBytes = kSampleBudgetMaxMib << 20;
    // Header and trailing chunks: a LIST or a cue is a few hundred bytes, embedded artwork in an
    // id3 chunk can be tens of KiB.
    static constexpr uint32_t kRawSlackBytes = 64u * 1024u;
    static constexpr uint32_t kRawBytes =
        kMaxSampleSeconds * kMaxSampleSourceRate * 2u * 3u + kRawSlackBytes;     // stereo s24
    static constexpr uint32_t kSrcFrames = kMaxSampleSeconds * kMaxSampleSourceRate;
    static constexpr uint32_t kSrcBytes = kSrcFrames * 2u * sizeof(int16_t);      // stereo s16
    static constexpr uint32_t kFileBytes = kBankMaxBytes;

    // How much of a conversion or a resample one pass does. 48,000 frames is a second of audio
    // and a few hundred microseconds of work, so the UI's 20 fps is never at risk and a 30 s
    // file still finishes in well under a second of wall time.
    static constexpr uint32_t kSliceFrames = 48000;

    // How many passes to wait for a sample epoch before giving up. update() runs at least 750
    // times a second, so this is a fraction of a second -- long enough for any real block, short
    // enough that a track whose epoch never moves does not hang the loader for ever.
    static constexpr uint32_t kEpochPasses = 512;
};

}  // namespace strata
