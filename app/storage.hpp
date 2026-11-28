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

#include "browse.hpp"
#include "commands.hpp"
#include "onsets.hpp"
#include "overview.hpp"
#include "pcm.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "samplepool.hpp"
#include "usersounds.hpp"

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
//
// [2026-11-22] The limit now binds only files at OTHER rates, through src_. A file bigger than raw_
// is read in chunks (below, "a file bigger than raw_"), so a 48 kHz file may be as long as the
// pool and Cmd::set_sample's 24-bit frame count allow.
constexpr uint32_t kMaxSampleSeconds = 30;
constexpr uint32_t kMaxSampleSourceRate = 96000;

class Storage {
public:
    // A project folder name. Settings values are 31 bytes, and the name has to fit in one with
    // room to spare, so this is the binding limit rather than the file system's.
    static constexpr int kNameBytes = 24;

    // The sample budget a project gets, in bytes: what it stored, but never less than
    // kSampleBudgetDefaultMib. Every v0.1 project stored the old default of 32 MiB and nothing in
    // the UI sets another value, so the stored byte is read as a floor (2026-11-23, the default
    // became the 64 MiB ceiling). The region is reserved whole in init(), so this costs nothing.
    static constexpr uint32_t project_budget(uint8_t stored_mib) {
        return (stored_mib > kSampleBudgetDefaultMib ? uint32_t(stored_mib)
                                                     : kSampleBudgetDefaultMib) << 20;
    }

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

    // A new project, in RAM only: nothing is written until a save. Queued like a load, and
    // for the same reason: it rewrites all of pattern memory from the main context, so it waits
    // for the command ring to drain. It needs no card, so it runs without one. `blank`: NEW
    // PROJECT's empty one (core/factory.hpp, blank_project); otherwise LOAD DEMO, the factory
    // project.
    void request_new(bool blank = false);

    // App::init: the last project named in the settings, or the autosave if it is newer. False
    // when there was nothing to open, which is not an error -- it means a first run, and the
    // engine's built-in default pattern stays.
    bool open_last();

    // --- the project browser (PROJECT mode's PROJECTS page) ---

    // Open another project from the card: `name` is a folder in PROJECTS/. Queued like a reload
    // and with its rules -- the transport stops, unsaved work is thrown away (the UI asks twice),
    // and an external clock that will not stop refuses it. The name becomes the project's only
    // once the load has read its PROJECT.BIN: a folder that is not a project leaves the current
    // one as it was, name and all. A project opened this way is what the next start opens.
    // False for a name that could not be a project's (valid_name).
    bool request_open(const char *name);

    // Save under a new name: a new folder with all ten files, the old one left as it is. The
    // project is called `name` from here on. False for a name that could not be one.
    bool request_save_as(const char *name);

    // Rename a project on the card: the folder PROJECTS/`from` becomes PROJECTS/`to`. Enaga has
    // no rename call (<enaga/files.hpp>: save, remove, make_folder), so it is a copy and a
    // delete, ordered so that a card pulled at any moment loses nothing:
    //
    //   1. `to` must not exist (NAME TAKEN) and `from` must hold a PROJECT.BIN (NOT A PROJECT).
    //   2. Every file is copied into `to`, PROJECT.BIN LAST. Until it lands, `to` is a folder
    //      that opens as NOT FOUND and the original is untouched; from then on both are whole.
    //   3. If `from` is the open project, the project takes the new name and the settings are
    //      saved with it, so the next start opens `to`.
    //   4. `from` is deleted as request_delete does it (PROJECT.BIN first).
    //
    // A failed copy deletes what it made of `to` and reports the reason. One rename or delete is
    // queued at a time: false when one already is, for names that are not usable, for the same
    // name twice and with no card.
    bool request_rename(const char *from, const char *to);

    // Delete the project folder PROJECTS/`name` and everything in it. PROJECT.BIN goes first
    // (and the PROJECT.BIN.TMP an interrupted save may have left, which Enaga's reads fall back
    // to), so a card pulled midway leaves a folder that is no longer a project -- it opens as
    // NOT FOUND, never as a project with its banks missing -- and deleting it again finishes the
    // job. Then the folder is listed and emptied a file per pass, and removed. Refused (false)
    // for the open project: RAM would keep a project whose files are gone, and the next save
    // would quietly bring it back.
    bool request_delete(const char *name);

    // A rename or delete asked for and not finished.
    bool project_op_pending() const {
        return op_pending_ != Op::none || job_ == Job::project_rename ||
               job_ == Job::project_delete;
    }

    // A name a project folder can have: 1 to kNameBytes-1 characters of the panel's text entry
    // (A-Z 0-9 space - + . # !), not starting with a space or a dot.
    static bool valid_name(const char *name);

    // "<data folder>/PROJECTS", for the browser's listing.
    bool projects_folder(char *out, uint32_t max) const;

    // App::stop(), whose budget is 3 s including saves. Blocking and bounded: it drains the ring
    // for a few milliseconds and then writes, because destructors never run on this platform and
    // this is the last chance to keep the user's work.
    void save_for_shutdown();

    // --- dirtiness --------------------------------------------------------

    // Every command the UI posts passes through here, so that a save knows which of the ten
    // files actually changed. `live_bank` is Status::live_bank at the moment of the post: a chain
    // may have moved the live pattern without the UI asking. `target_bank`: the bank a
    // pattern_copy or pattern_clear writes (its `track`), which need not be the live one; -1
    // falls back to the live bank.
    void note_command(Cmd cmd, int live_bank, int target_bank = -1);

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
    // A finished take not yet all on the card: its write is running, or waiting to start. A
    // second request then would retarget it, so request_resample_write refuses.
    bool take_busy() const;

    // The name the last take was written under, for the screen; empty before the first.
    const char *last_take_name() const { return take_name_; }
    // Its whole path on the card (/RECORD/STRATA_<n>.WAV), for putting it on a track later.
    const char *last_take_path() const { return take_path_; }
    // Bumped each time a take asked for TO FILE alone is all on the card. The RESAMPLE screen
    // watches it to offer "put on track n" (one OK) once the write has finished.
    uint32_t file_takes() const { return file_takes_; }

    // --- freeze (dsp/frozen.hpp), RAM only: no card needed, nothing written ---
    //
    // `request_freeze` arms a take of `track` alone, before its insert chain, for `bars` bars
    // (1, 2, 4 or 8) from the next bar line. When the take is done it is copied into the track's
    // freeze slot of the sample pool (core/samplepool.hpp) and the engine plays it in place of
    // the machine. False, changing nothing, while any take is armed, running or unwritten, while
    // the track is frozen or still giving its last loop back, or for a MID track (no audio).
    bool request_freeze(int track, int bars);
    // Stop playing the loop; the slot is given back once the audio side has let go of it. False
    // for a track that is not frozen. Cancels a freeze take still on its way for that track.
    bool request_unfreeze(int track);
    // Every frozen track, and any freeze take on its way. A project load, NEW and a kit recall
    // do this: a frozen loop is the old sound.
    void unfreeze_all();
    // A freeze take is armed, recording or waiting to be copied, and for which track (-1: none).
    int freezing_track() const { return freeze_track_; }
    // Why the last freeze failed ("NO ROOM"), or nullptr. Cleared by the next request.
    const char *freeze_error() const { return freeze_error_; }

    // Queue every track whose kit names a file. What a project open does.
    void request_all_samples();

    // --- the sample browser ------------------------------------------------
    //
    // One folder of the card at a time, listed with `files::list` into `browse_list()`
    // (core/browse.hpp says what is kept). The listing runs beside the job machine rather than
    // in it: it touches none of the loader's buffers, Enaga runs requests in the order they are
    // made, and a browser that waited for a 30 s sample to convert before it could show a folder
    // would be no browser. Only one listing is in flight; asking for another meanwhile queues it,
    // because the entries of the first would otherwise land in the second's table.

    // Is there a folder at `path` on a usable card? One immediate files::info: main context, and
    // not in a loop.
    bool folder_exists(const char *path) const;

    // Show `folder` ("" is the browser's own root: SAMPLES, RECORD and, on a card with the stock
    // firmware's /SmplTrek, POOL and SMPLTREK; core/browse.hpp). The root is filled at
    // once; a folder on the card is listed in the background. False for a folder too long to
    // show, with nothing changed.
    bool request_browse(const char *folder);

    // True until the folder asked for last has been listed.
    bool browsing() const { return list_in_flight_ || browse_pending_; }
    const SampleList &browse_list() const { return list_; }
    // Why the last listing failed (NOT FOUND, NO CARD...), or nullptr.
    const char *browse_error() const { return browse_error_; }

    // --- sample audition (dsp/audition.hpp): push A in SAMPLES ---
    //
    // Hear a file without loading it: it is streamed from the card, whole and from its start,
    // straight to the output. Beside the job machine, like the browser's listing and the tracks'
    // streams, so a sample loading or a project saving does not hold it up, and it touches none of
    // the loader's buffers. One at a time: a second request stops the first and plays the new one.
    //
    // The protocol, all in service_audition():
    //   1. The file's first kAuditionHeadBytes are read (a files::File read) and its WAV header
    //      parsed, for its format and where its data starts.
    //   2. An Enaga files::Stream (Engine::audition_stream) is opened at the data, into
    //      `audition_ring_`. Once it says it is filled, Engine::audition_for_load is written and
    //      Cmd::audition_start posted.
    //   3. The audio side reads and plays it to its end, or until Cmd::audition_stop's fade, and
    //      then bumps Status::audition_epoch. Only after seeing that is the stream closed.

    // Play the WAV at `path`. False, changing nothing, for an empty path or one too long. Queued:
    // a request while another plays stops that one first.
    bool request_audition(const char *path);
    // Stop the audition, or the one on its way.
    void stop_audition();
    // An audition asked for and not yet over: opening, playing or fading out.
    bool auditioning() const { return audition_want_ || audition_state_ != AuditionState::idle; }
    // The file it is, or is about to be ("" when none).
    const char *audition_path() const { return audition_want_ ? audition_next_ : audition_path_; }
    // Opened and sounding (step 3), rather than still on its way.
    bool audition_sounding() const { return audition_state_ == AuditionState::playing; }
    // The file's length in seconds and where it is, for the screen: rounded down.
    uint32_t audition_seconds() const;
    uint32_t audition_elapsed() const;
    // Its length in file frames (Status::audition_frame counts the same), 0 before it is known.
    uint32_t audition_frames() const { return audition_total_; }
    // Its rate in Hz, 0 before it is known.
    uint32_t audition_rate() const { return audition_rate_; }
    // Why the last audition did not play (NOT A WAV, NOT FOUND ...), once: it reads and clears.
    const char *take_audition_error() {
        const char *e = audition_error_;
        audition_error_ = nullptr;
        return e;
    }

    // --- jam recording: JAM REC, the FUNC menu's second row ---
    //
    // The whole jam to the card: the finished master (what RESAMPLE's MASTER take hears, so never
    // the click or the audition) to /RECORD/JAM_<n>.WAV, 48 kHz 24-bit stereo, for as long as it
    // runs. About 1 GB an hour. Beside the job machine, like the audition, so a save or a sample
    // load does not hold it up, and it shares no buffer with anything: RESAMPLE, freeze and the
    // looper still work while it runs.
    //
    // The protocol, all in service_jam():
    //   1. request_jam_start() creates the file through an Enaga recording stream
    //      (Engine::jam_writer, a wav::Writer, into `jam_ring_`) and posts Cmd::jam_start.
    //   2. The audio side writes every block into it. Enaga moves the ring to the card in its main
    //      loop; when the ring is full, the frames of a block that do not fit are dropped and
    //      counted (jam_lost_frames), and the ones that fit are kept.
    //   3. A stop (the user, a full card, kJamMaxBytes, the card going) posts Cmd::jam_stop. The
    //      audio side stops writing and bumps Status::jam_epoch; only after seeing that is the
    //      file closed, which writes what the ring still holds and then the header.
    //
    // Numbered across restarts like RESAMPLE's takes (JAMSEQ in the settings), so a jam never
    // overwrites an earlier one.

    // Start a jam. False when it cannot, with the reason in take_jam_message (NO CARD, JAM
    // BUSY while the last one is still closing, or the card's own answer).
    bool request_jam_start();
    // Stop it; the file is closed on a later pass, once the audio side lets go of it.
    void request_jam_stop();
    // Started and not yet closed: recording, or stopping.
    bool jam_running() const { return jam_state_ != JamState::idle; }
    // Recording, not yet asked to stop.
    bool jam_recording() const { return jam_state_ == JamState::recording; }
    // How long it is so far, in seconds, rounded down; the last jam's length once it has closed.
    uint32_t jam_seconds() const { return jam_frames_ / uint32_t(ENAGA_SAMPLE_RATE); }
    // Frames dropped because the card fell behind: 0 on a healthy card.
    uint32_t jam_lost_frames() const;
    // The file's name (JAM_3.WAV), the running jam's or the last one's; "" before the first.
    const char *jam_name() const { return jam_name_; }
    // What became of the last start or stop (SAVED JAM 3, CARD FULL ...), once: reads and clears.
    const char *take_jam_message() {
        const char *m = jam_message_;
        jam_message_ = nullptr;
        return m;
    }
    // App::stop: the audio side is already gone, so the file is closed at once, header and all.
    void close_jam_for_shutdown();
    // A FAT32 file stops short of 4 GiB, and so does a WAV's size field: a jam is closed at this
    // many bytes of audio, 3 h 51 min. Settable for the tests, which cannot fill 4 GB.
    static constexpr uint32_t kJamMaxBytes = 4000000000u;
    void set_jam_limit(uint32_t bytes) { jam_limit_ = bytes; }

    // --- streaming (below, "streaming: a file too big for the pool") ---

    // Does this track play a streamed file? For the screen's sample line and for tests.
    bool streaming(int track) const {
        return track >= 0 && track < kTracks && streams_[track].on;
    }

    // Put a file in a track's kit and load it, through the ordinary swap -- the same thing a
    // resample into a track does at its end. False for a track that is not one, or a path that
    // is empty or would not fit the kit's kSamplePathBytes. The kit becomes UNSAVED.
    bool request_sample_file(int track, const char *path);

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

    // --- your own sounds (core/usersounds.hpp) -----------------------------
    //
    // One machine's sixteen at a time, the machine of the track SOUNDS is open on:
    // `<data folder>/SOUNDS/<MCH>.BIN`, shared by every project like the kit library. Each read
    // and write is one small file, done in the pass that starts it, as a kit save is. A save, a
    // rename or a delete changes the bank held here at once (the list shows it) and then writes
    // the whole file; files::save goes through a .TMP, so a pulled card leaves the old file or
    // the new. Never touched by a project save or load: projects open exactly as before.
    //
    // A file that cannot be read (damaged, or from a newer build) is never written over: the
    // bank stays `failed` and saving into it is refused, so whatever is in it is not lost.
    enum class SoundBankState : uint8_t { none, reading, ready, failed };

    // Ask for `machine`'s bank. Nothing happens if it is held already, being read, or failed
    // (until the card comes back); called on every frame SOUNDS draws, so it must be cheap.
    void want_sound_bank(uint8_t machine);

    // The state of `machine`'s bank: `none` when the bank held is another machine's.
    SoundBankState sound_bank_state(uint8_t machine) const;
    const UserSoundBank &sound_bank() const { return sound_bank_; }
    // Why the last read or write failed, or nullptr.
    const char *sound_bank_error() const { return sound_bank_error_; }

    // Change the held bank and queue its write. False, nothing changed, when the bank held is not
    // `machine`'s or not ready, with no usable card, or when core/usersounds.hpp refuses (a slot
    // that does not exist, an empty one for rename or delete, a name it cannot hold).
    bool request_sound_store(uint8_t machine, int slot, const char *name, const uint8_t *params);
    bool request_sound_rename(uint8_t machine, int slot, const char *name);
    bool request_sound_delete(uint8_t machine, int slot);

    // A write asked for and not yet done.
    bool sound_write_pending() const { return sound_write_pending_; }

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

    // The picture of what a track plays, for EDIT's waveform (core/overview.hpp): its file's,
    // made while the loader wrote it into the pool, or -- while there is none, a load is on its
    // way or a load failed -- the placeholder's, which is what Cmd::clear_sample leaves playing.
    // A streamed file's covers only its head (`filled` columns). Main context.
    const WaveOverview &overview(int track) const {
        if (track >= 0 && track < kTracks && overviews_[track].ready) return overviews_[track];
        return placeholder_overview_;
    }
    // Whether that picture is the track's own file rather than the placeholder.
    bool has_overview(int track) const {
        return track >= 0 && track < kTracks && overviews_[track].ready;
    }

    // What EDIT's file line says about the track's file (the drafts' "LOOP.WAV 2.41S 44.1K"): its
    // length in ms as the track plays it and the file's own rate, 0 when nothing is loaded.
    uint32_t file_ms(int track) const { return has_overview(track) ? file_ms_[track] : 0u; }
    uint32_t file_rate(int track) const { return has_overview(track) ? file_rate_[track] : 0u; }
    void note_file_info(int track, uint32_t frames, uint32_t frames_rate, uint32_t file_rate) {
        if (track < 0 || track >= kTracks || frames_rate == 0) return;
        // 32-bit only (a 64-bit divide drags libgcc's unwinder into the link, see progress()).
        file_ms_[track] = frames / frames_rate * 1000u + frames % frames_rate * 1000u / frames_rate;
        file_rate_[track] = file_rate;
    }

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
        sample_chunk,      // a file bigger than raw_: one chunk converted, in slices (below)
        take_write,        // a resample's WAV, header then payload
        kit_scan,          // the kit library's sixteen slots, one file per pass
        kit_save,          // one kit into its slot: started and finished in one pass
        kit_load,          // one kit out of its slot, likewise
        project_rename,    // copying a project folder's files into its new name, one per pass
        project_delete,    // removing a project folder's files, one per pass, then the folder
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
    bool sound_bank_path(char *out, uint32_t max, uint8_t machine, bool folder_only) const;
    void run_sound_read();
    void run_sound_write();
    bool sound_bank_editable(uint8_t machine) const;
    void run_kit_save();
    void run_kit_load();
    bool kit_path(char *out, uint32_t max, int slot, bool folder_only) const;

    // --- samples ---
    void step_sample_epoch();
    void step_sample_convert();
    void step_sample_resample();
    void step_sample_chunk();
    bool start_chunked(const char *path, uint32_t file_bytes);
    bool read_next_chunk();
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
    // request_open: the name the pending load reads, and the one to go back to if it fails.
    char open_name_[kNameBytes] = {};
    char kept_name_[kNameBytes] = {};
    bool open_pending_ = false;    // the pending load is an open of open_name_
    bool opening_ = false;         // the load in flight is one
    bool card_usable_ = true;
    // Resampling. `take_pending_` is a request waiting for the audio side to finish the take;
    // `take_track_` is the track to load it into afterwards, or -1 for "just the file".
    bool take_pending_ = false;
    int take_track_ = -1;
    static constexpr uint32_t kTakeNameBytes = 24;
    char take_name_[kTakeNameBytes] = {};
    char take_path_[ENAGA_FILE_PATH_MAX + 1] = {};
    uint32_t file_takes_ = 0;
    // The take goes to the card a chunk per pass, one write in flight at a time (Roland,
    // 2026-11-22): one files::save of 5.76 MiB held the main loop ~3 s, so the UI froze and
    // streamed tracks, which Enaga refills only between main-loop file slices, ran dry.
    static constexpr uint32_t kTakeChunkBytes = 32u * 1024u;
    enaga::files::File take_file_;
    uint32_t take_bytes_ = 0;          // the whole file, header included
    uint32_t take_written_ = 0;        // bytes of it on the card so far
    bool take_write_in_flight_ = false;
    bool take_write_done_ = false;
    int32_t take_write_result_ = 0;
    // Freeze: the take on its way (-1 none), the bars it was asked for, the tracks whose slot
    // waits for the audio side to bump freeze_epoch (and the epoch each saw at the clear).
    int freeze_track_ = -1;
    uint8_t freeze_bars_ = 1;
    uint8_t unfreeze_wait_ = 0;
    uint8_t unfreeze_epoch_[kTracks] = {};
    const char *freeze_error_ = nullptr;
    uint8_t freeze_refused_seen_ = 0;  // Status::freeze_refused when the take was asked for
    bool service_freeze();             // true if it posted, which ends the pass
    void take_chunk_written(int32_t result);   // the File::write callback, main context
    bool write_next_take_chunk();
    void end_take_write(const char *error);
    void finish_take_write();

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
    // 16: unfreeze_all posts up to ten (a cancel, a release and eight clears) beside a load's own.
    static constexpr int kBacklog = 16;
    Command backlog_[kBacklog] = {};   // posts that met a full ring, sent first by service()
    int backlog_n_ = 0;
    bool new_pending_ = false;     // request_new, waiting for the ring like a load
    bool new_blank_ = false;       // and which project it makes
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

    // Your own sounds: the one bank held, whose machine, and what is asked of the card.
    UserSoundBank sound_bank_;
    uint8_t sound_bank_machine_ = 0;
    SoundBankState sound_bank_state_ = SoundBankState::none;
    bool sound_read_pending_ = false;
    bool sound_write_pending_ = false;
    const char *sound_bank_error_ = nullptr;

    bool perform_dirty_ = false;   // the scenes and macro setup, in PERFORM.BIN
    bool kit_dirty_ = false;
    bool bank_dirty_[kBanks] = {};

    // --- renaming and deleting projects (request_rename, request_delete) ---
    enum class Op : uint8_t { none, rename, remove };
    Op op_pending_ = Op::none;
    char op_from_[kNameBytes] = {};    // the folder renamed or deleted
    char op_to_[kNameBytes] = {};      // a rename's new name
    void start_op();
    void step_rename();
    void step_delete();
    void begin_delete(const char *name, bool for_rename, const char *carried);
    bool named_path(char *out, uint32_t max, const char *name, const char *file) const;
    void delete_entry(const enaga::files::Info &entry);   // files::list callbacks, main context
    void delete_listed(int32_t result);
    // The delete in flight: which folder, and whether it is a rename's tail (the old name, after
    // the copy) or its cleanup (the new name, after a failed copy, carrying the copy's reason).
    char del_name_[kNameBytes] = {};
    bool del_for_rename_ = false;
    const char *del_carried_ = nullptr;
    // The folder's entries, from one files::list. A folder holding more than kDelEntries is
    // emptied in rounds: listed, removed, listed again, for as long as a round removes something.
    static constexpr int kDelEntries = 32;
    static constexpr int kDelNameBytes = 64;
    char del_names_[kDelEntries][kDelNameBytes] = {};
    int del_count_ = 0;
    int del_next_ = 0;
    int del_removed_ = 0;              // files removed in this round
    bool del_overflow_ = false;        // the listing had more than the table holds, or a long name
    bool del_listing_ = false;         // a files::list owns the callbacks until its done()
    bool del_listed_ = false;          // ...and it has answered, with:
    int32_t del_list_result_ = 0;

    // --- the sample browser ---
    void start_browse();
    void list_entry(const enaga::files::Info &entry);   // files::list callbacks, main context
    void list_done(int32_t result);
    SampleList list_;
    char browse_wanted_[SampleList::kFolderBytes] = {};
    bool browse_pending_ = false;  // asked for, not yet started (one is in flight)
    bool list_in_flight_ = false;  // a files::list owns the callbacks until its done()
    const char *browse_error_ = nullptr;

    // --- the sample loader ---
    uint8_t sample_queue_ = 0;     // one bit per track, waiting to be loaded
    uint32_t wanted_budget_ = 0;   // a loaded project's budget, applied once nothing is reserved
    int sample_track_ = -1;        // the track being loaded, or -1
    uint8_t epoch_before_ = 0;     // Status::sample_epoch[track] when clear_sample was posted
    uint32_t epoch_waited_ = 0;    // passes spent waiting, so a lost epoch cannot wait for ever

    bool read_in_flight_ = false;  // a files::load owns raw_ until its callback
    bool read_done_ = false;
    int32_t read_result_ = 0;

    // --- a file bigger than raw_, read a chunk at a time ---
    //
    // docs/SPEC.md planned streams for loops over 30 s. Roland's call (2026-11-22) was to load
    // them whole instead, so they keep pitch, slices and GRAIN: a file that does not fit raw_ is
    // opened and read into it a chunk at a time, each chunk converted straight into the pool (a
    // 48 kHz file, any length the pool and Cmd::set_sample's 24-bit frame count allow) or into
    // src_ (any other rate, up to kSrcFrames of the file, as before). raw_ is the chunk buffer,
    // so this costs no memory. Short files keep the one files::load docs/SPEC.md names.
    enaga::files::File chunk_file_;
    bool chunked_ = false;         // the sample job in flight reads chunks
    uint32_t chunk_file_bytes_ = 0;  // the file's size
    uint32_t chunk_file_pos_ = 0;  // bytes of it read so far
    uint32_t chunk_have_ = 0;      // valid bytes in raw_
    uint32_t chunk_pos_ = 0;       // the next unconverted byte in raw_
    uint32_t chunk_carry_ = 0;     // part of a frame kept at raw_'s start for the next read

    // --- sample audition ---
    enum class AuditionState : uint8_t { idle, header, filling, playing, closing };
    void service_audition();
    void begin_audition();               // idle -> header, for audition_next_
    void audition_header_read();         // header -> filling, or idle with a reason
    void audition_read(int32_t result);  // the File::read callback, main context
    void fail_audition(const char *why);
    AuditionState audition_state_ = AuditionState::idle;
    bool audition_want_ = false;         // audition_next_ is asked for and not yet begun
    char audition_next_[kSamplePathBytes] = {};
    char audition_path_[kSamplePathBytes] = {};
    uint32_t file_ms_[kTracks] = {};
    uint32_t file_rate_[kTracks] = {};
    enaga::files::File audition_file_;
    bool audition_read_in_flight_ = false;   // a read owns audition_head_ until its callback
    bool audition_read_done_ = false;
    bool audition_cancelled_ = false;    // stopped while the header was on its way
    int32_t audition_read_result_ = 0;
    uint32_t audition_file_bytes_ = 0;
    uint32_t audition_total_ = 0;        // frames
    uint32_t audition_rate_ = 0;
    uint8_t audition_epoch_ = 0;         // Status::audition_epoch when the start was posted
    const char *audition_error_ = nullptr;
    // A WAV's header and the chunks before its data: fmt, and often LIST, bext, iXML or JUNK,
    // which run to a few KiB. 32 KiB leaves room for a big one without reading audio for nothing.
    static constexpr uint32_t kAuditionHeadBytes = 32u << 10;
    // The stream's ring: as a track's (kStreamRingBytes), 1.36 s of 48 kHz stereo 16-bit.
    static constexpr uint32_t kAuditionRingBytes = 256u << 10;
    uint8_t *audition_head_ = nullptr;   // both taken in init()
    uint8_t *audition_ring_ = nullptr;

    // --- jam recording ---
    enum class JamState : uint8_t { idle, recording, closing };
    void service_jam();
    void stop_jam(const char *why);      // recording -> closing; `why` the message once closed
    void close_jam();                    // closing -> idle, the file closed
    JamState jam_state_ = JamState::idle;
    uint8_t jam_epoch_ = 0;              // Status::jam_epoch when the jam began
    uint32_t jam_frames_ = 0;            // the jam's length, kept once it has closed
    uint32_t jam_limit_ = kJamMaxBytes;
    const char *jam_stop_reason_ = nullptr;   // what the stop says once closed; null: SAVED
    const char *jam_message_ = nullptr;
    char jam_name_[24] = {};
    char jam_path_[40] = {};
    char jam_text_[32] = {};
    // The stream's ring: 3.6 s of 24-bit stereo, for the card's slow moments (a FAT allocation, a
    // sample stream's read at the same time). Taken in init().
    static constexpr uint32_t kJamRingBytes = 1u << 20;
    uint8_t *jam_ring_ = nullptr;

    // --- streaming: a file too big for the pool ---
    //
    // Roland's call (2026-11-22), after chunked loading: a file the pool cannot hold is STREAMED
    // rather than refused. Its first kStreamHeadSeconds are loaded into the pool as a HEAD,
    // converted like any sample but not resampled (a stream plays at the file's own rate), and
    // the rest is read from the card while it plays, through an Enaga `files::Stream` that
    // belongs to the engine (Engine::stream). The head is what makes a restart instant: a
    // trigger or a loop wrap plays the head from RAM while this side closes the stream and opens
    // it again at the head's end, which an Enaga stream needs because it only reads forward.
    //
    // The protocol, per track:
    //   1. The chunk job decides the file streams (begin_stream), converts the head into the
    //      pool, fills Engine::stream_for_load and posts Cmd::set_stream (hand_stream_over).
    //   2. The audio side puts the Sampler in stream mode and bumps Status::stream_want.
    //   3. service_streams sees the bump, closes the stream, opens it at the head's end and,
    //      once Enaga says it is filled, posts Cmd::stream_attach with the value it answered.
    //   4. The audio side reads it into the track's fifo every block until the voice goes back
    //      to frame 0, when it stops reading and bumps stream_want again: back to 3.
    //   5. A sample swap's clear_sample stops the reading before its epoch, so once the epoch
    //      moves (step_sample_epoch) end_stream closes the stream and frees the ring.
    // The card side never touches the stream between an attach and the next bump, and the audio
    // side never reads it outside that window, so the Stream object has one user at a time.
    //
    // A card pulled mid-stream makes reads come back short (silence, STREAM LATE); the card
    // coming back reloads every track (on_card), which reopens it.
    struct StreamPlan {
        bool on = false;           // the track plays a streamed file
        bool attached = false;     // the attach for `seq` is posted
        bool failed = false;       // the open for `seq` failed: wait for the next restart
        int8_t ring = -1;
        uint16_t seq = 0;          // the Status::stream_want value last acted on
        uint32_t offset = 0;       // the file byte where the tail (after the head) begins
        char path[kSamplePathBytes] = {};
    };
    void service_streams();
    const char *begin_stream(int track, const char *whole);   // nullptr: it streams
    void hand_stream_over();
    void end_stream(int track);
    StreamPlan streams_[kTracks];
    bool streaming_ = false;       // the sample job in flight converts a stream's head
    uint32_t stream_total_ = 0;    // that file's frames
    int8_t stream_ring_ = -1;      // and the ring it will use
    // Four streamed tracks at once, each with a ring of 256 KiB: 1.36 s of 48 kHz stereo 16-bit
    // and 0.34 s of the heaviest file allowed (96 kHz stereo float), against card reads that
    // take milliseconds. 1 MiB in all, from docs/SPEC.md's "recording ring and stream buffers".
    static constexpr int kStreamRings = 4;
    static constexpr uint32_t kStreamRingBytes = 256u << 10;
    // How much of a streamed file is resident: the time a restart has to reopen the stream.
    static constexpr uint32_t kStreamHeadSeconds = 2;
    uint8_t *rings_[kStreamRings] = {};

    enaga::wav::Format format_;
    PcmFormat encoding_ = PcmFormat::unknown;
    uint32_t src_frames_ = 0;      // frames in the file
    uint32_t src_done_ = 0;        // converted so far
    uint32_t out_frames_ = 0;      // frames the sampler will get
    uint32_t out_done_ = 0;
    uint32_t out_offset_ = 0;      // the pool's byte offset for this slot
    Resampler cursor_;
    Antialias antialias_;          // a downsample's low-pass, carried across slices
    OnsetFinder onsets_;           // the slice points of the sample being loaded, as it lands
    // And its picture for the screen, the same way: 250 bytes a track, built beside onsets_ from
    // the same slices, so a load reads nothing twice.
    OverviewBuilder overview_builder_;
    WaveOverview overviews_[kTracks];
    WaveOverview placeholder_overview_;   // built once in init(), from the engine's placeholder

    // The buffers, all taken in init().
    //
    //   region_   the 64 MiB sample region the pool carves up, and the only one the audio side
    //             ever reads. The pool and the loader live here, so no pool state crosses the
    //             ring (app/engine.hpp's `sample_region_`).
    //   raw_      a small file whole, for files::load (docs/SPEC.md asks for files::load by
    //             name), or one chunk of a bigger one: kChunkBytes, 2 MiB.
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
    //
    // [2026-11-23, Roland] raw_ is one chunk, 2 MiB, not a whole 30 s file (16.5 MiB): every file
    // bigger than it was already read in chunks and converted straight into the pool or src_,
    // so the result is the same and 14.5 MiB of heap comes back. Files up to 2 MiB (about 11 s
    // of 48 kHz stereo 16-bit) still take the one files::load docs/SPEC.md names.
    static constexpr uint32_t kChunkBytes = 2u << 20;
    static constexpr uint32_t kRawBytes = kChunkBytes;
    static constexpr uint32_t kSrcFrames = kMaxSampleSeconds * kMaxSampleSourceRate;
    static constexpr uint32_t kSrcBytes = kSrcFrames * 2u * sizeof(int16_t);      // stereo s16
    static constexpr uint32_t kFileBytes = kBankMaxBytes;

    // How much of a conversion or a resample one pass does. 48,000 frames is a second of audio
    // and a few hundred microseconds of work, so the UI's 20 fps is never at risk and a 30 s
    // file still finishes in well under a second of wall time.
    static constexpr uint32_t kSliceFrames = 48000;

    // kChunkBytes (above, with raw_) is how much of a long file one read asks for. Enaga works a
    // read in the main loop a slice per pass whatever its size, so it only sets how often a read
    // is queued: 2 MiB is about a quarter of a second of the card at 7.6 MB/s.

    // How many passes to wait for a sample epoch before giving up. update() runs at least 750
    // times a second, so this is a fraction of a second -- long enough for any real block, short
    // enough that a track whose epoch never moves does not hang the loader for ever.
    static constexpr uint32_t kEpochPasses = 512;
};

}  // namespace strata
