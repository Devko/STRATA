// The card side of STRATA. app/storage.hpp has the design and the three protocols.
#include "storage.hpp"

#include "factory.hpp"

#include "engine.hpp"

#include <enaga/app.hpp>
#include <enaga/clock.hpp>
#include <enaga/wav.hpp>

namespace strata {
namespace {

// The settings keys. Up to 23 bytes each and at most 32 keys in the whole file
// (<enaga/settings.hpp>), so they are short and there are three.
constexpr const char *kKeyProject = "PROJECT";
constexpr const char *kKeyProjectSeq = "PROJSEQ";
constexpr const char *kKeyAutoSeq = "AUTOSEQ";
constexpr const char *kKeyNewSeq = "NEWSEQ";
// Resampled takes are numbered across restarts, like NEW projects are, so a take never
// overwrites an earlier one: docs/SPEC.md's /RECORD/STRATA_<n>.WAV.
constexpr const char *kKeyTakeSeq = "TAKESEQ";

constexpr const char *kDefaultName = "DEFAULT";

// The ten files a project is. The order is the order they are written and read in, and
// PROJECT.BIN is first on purpose: it is the one that says which pattern was live, and
// core/project.cpp's `project_read` deliberately does not touch a pattern, so a damaged 292
// bytes cannot cost the banks that follow it.
constexpr int kFileProject = 0;
constexpr int kFileKit = 1;
// PERFORM.BIN (M5): the scenes and the macros. Last of the fixed-size files and read last, so
// that a project written before it existed loads everything else and simply has no scenes --
// which is why a missing PERFORM.BIN is not an error here.
constexpr int kFilePerform = 2;
constexpr int kFileFirstBank = 3;
constexpr int kFileCount = kFileFirstBank + kBanks;

void append_text(char *&out, const char *text, const char *end) {
    while (*text != '\0' && out < end) *out++ = *text++;
}

void append_number(char *&out, uint32_t value, const char *end) {
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value != 0);
    while (n > 0 && out < end) *out++ = digits[--n];
}

// True when `cmd` changes the live pattern, and therefore its bank's file.
bool mutates_pattern(Cmd cmd) {
    switch (cmd) {
        case Cmd::toggle_trig:
        case Cmd::clear_lock:
        case Cmd::set_track_length:
        case Cmd::set_track_speed:
        case Cmd::set_pattern_swing:
        case Cmd::set_track_swing:
        case Cmd::set_master_length:
        case Cmd::step_field_delta:
        case Cmd::lock_delta:
        case Cmd::undo:
        case Cmd::redo:
        case Cmd::range_paste:
        case Cmd::range_erase:
        case Cmd::range_transpose:
        case Cmd::range_nudge:
        case Cmd::track_erase:
        case Cmd::pattern_erase:
        case Cmd::pattern_copy:      // the destination's bank (note_command's target_bank)
        case Cmd::pattern_clear:
        case Cmd::record_hit:
        case Cmd::record_length:
        case Cmd::set_step_length:
        case Cmd::mutate:
        // The Euclidean generator writes trig bits, so it is as much a pattern edit as
        // `toggle_trig` is. It was missing here, which meant a generated rhythm left its bank
        // clean: an explicit save then skipped the bank file entirely and the rhythm was gone on
        // reopen. `Cmd::mutate` was remembered and `Cmd::euclid`, added later, was not.
        case Cmd::euclid:
        // And these two write BOTH stores. `project_.settings` holds the key and scale for
        // PROJECT.BIN, but `Engine` also writes `live().root` and `live().scale`, which
        // core/serialize puts in the BANK file (serialize.cpp:235) and which is the copy
        // core/scale actually reads. Marking only the project dirty saved half the change, and
        // the bank's untouched copy won on the next load -- so the key appeared to revert.
        case Cmd::set_root:
        case Cmd::set_scale:
            return true;
        default:
            return false;
    }
}

// True when `cmd` changes the kit, and therefore KIT.BIN. A sample command counts: the path a
// track plays is part of the kit even though the audio side only ever sees a pointer.
bool mutates_kit(Cmd cmd) {
    switch (cmd) {
        case Cmd::set_param:
        // Which machine a track hosts is a kit field (KitTrack::machine) and has been since M4,
        // which is the whole reason machine switching needed no file-format change.
        case Cmd::set_machine:
        // A factory sound writes a track's parameters, as a knob does.
        case Cmd::sound_preset:
        case Cmd::set_track_level:
        case Cmd::set_slice_mode:
        case Cmd::set_duck:
        case Cmd::clear_sample:
        case Cmd::set_sample:
        case Cmd::set_stream:
        case Cmd::kit_store:
            return true;
        default:
            return false;
    }
}

// True when `cmd` changes something PROJECT.BIN holds: the chain, the queue's idea of which
// pattern is live, the tempo, the mutes.
bool mutates_project(Cmd cmd) {
    switch (cmd) {
        case Cmd::set_tempo:
        case Cmd::set_chain:
        case Cmd::chain_edit:
        case Cmd::queue_pattern:
        case Cmd::set_mute:
        case Cmd::set_root:
        case Cmd::set_scale:
        // The eight stored mute states live in PROJECT.BIN (core/project.hpp).
        case Cmd::mute_store:
        // And so do the live mutes and which scenes are in slots A and B, so a recall and a
        // scene pick change the file too.
        case Cmd::mute_recall:
        case Cmd::scene_select:
        // The fourteen send and master bytes, which PROJECT.BIN's third section holds. Before
        // that section existed these were in no file at all, so the flag was 0 and a delay
        // setting was lost the moment the app restarted.
        case Cmd::set_sends:
        case Cmd::set_master:
        // And the kill EQ's switches, which sit in the same section.
        case Cmd::eq_kill:
        // The live-record quantize amount, which has a field in PROJECT.BIN and until now was
        // kept only in `app/` -- settable, saved nowhere.
        case Cmd::set_quantize:
        // Each track's MIDI OUT channel, PROJECT.BIN's fifth section.
        case Cmd::set_midi_out:
            return true;
        default:
            return false;
    }
}

// True when `cmd` changes PERFORM.BIN: the scenes and the macro setup, which docs/SPEC.md says
// save. The morph POSITION and a macro's position do not -- they are where the performer's hands
// are, not part of the project.
bool mutates_perform(Cmd cmd) {
    switch (cmd) {
        case Cmd::scene_capture:
        case Cmd::scene_erase:
        case Cmd::macro_assign:
        case Cmd::macro_clear:
            return true;
        default:
            return false;
    }
}

}  // namespace

void Storage::post(Cmd cmd) {
    Command command;
    command.cmd = cmd;
    post(command);
}

// Every command this file posts is one the protocols depend on -- a `clear_sample` the epoch wait
// is waiting for, the `set_sample` that ends a swap, the `kit_store` a save serializes, the
// `project_loaded` that tells the audio side its derived state is stale. So none may be dropped
// on a full ring. One that does not fit waits here, in order, and service() sends it before it
// does anything else; while any waits, nothing new starts, exactly as for a non-empty ring.
void Storage::post(const Command &command) {
    if (backlog_n_ == 0 && engine_->commands().post(command)) return;
    if (backlog_n_ < kBacklog) {
        backlog_[backlog_n_++] = command;
        return;
    }
    // Unreachable: no pass posts more than a dozen (unfreeze_all's ten and a load's own), and a
    // backlog holds every pass back. Counted on
    // the ring's own drop counter if it ever happens, so it is visible rather than silent.
    engine_->commands().post(command);
}

bool Storage::flush_backlog() {
    int sent = 0;
    while (sent < backlog_n_ && engine_->commands().post(backlog_[sent])) sent++;
    for (int i = sent; i < backlog_n_; i++) backlog_[i - sent] = backlog_[i];
    backlog_n_ -= sent;
    return backlog_n_ == 0;
}

// --- setting up -----------------------------------------------------------

bool Storage::init(Engine &engine) {
    engine_ = &engine;

    // Every allocation in this file, here and nowhere else. The CEILING of the sample region is
    // reserved rather than a project's budget, because a project may raise its budget long after
    // init() is over and allocating then would break CLAUDE.md's rule.
    region_ = new int16_t[kRegionBytes / sizeof(int16_t)];
    raw_ = new uint8_t[kRawBytes];
    src_ = new int16_t[kSrcBytes / sizeof(int16_t)];
    file_ = new uint8_t[kFileBytes];
    if (region_ == nullptr || raw_ == nullptr || src_ == nullptr || file_ == nullptr) return false;
    for (int r = 0; r < kStreamRings; r++) {
        rings_[r] = new uint8_t[kStreamRingBytes];
        if (rings_[r] == nullptr) return false;
    }

    pool_.init(kSampleBudgetDefaultMib << 20);
    engine_->set_sample_region(region_, kRegionBytes);

    // A missing settings file is empty settings, not an error: that is a first run.
    settings_.load();
    const char *last = settings_.text(kKeyProject, kDefaultName);
    char *out = name_;
    append_text(out, last, name_ + kNameBytes - 1);
    *out = '\0';
    if (name_[0] == '\0') {
        out = name_;
        append_text(out, kDefaultName, name_ + kNameBytes - 1);
        *out = '\0';
    }

    const int32_t project_seq = settings_.get(kKeyProjectSeq, 0);
    const int32_t auto_seq = settings_.get(kKeyAutoSeq, 0);
    save_seq_ = uint32_t(project_seq > auto_seq ? project_seq : auto_seq);
    return true;
}

// --- paths ----------------------------------------------------------------

// Mutation testing note: the path buffers in this file are ENAGA_FILE_PATH_MAX + 1 bytes and every
// BAD PATH below is unreachable today -- the longest path STRATA builds is the data folder, 10
// bytes of "/PROJECTS/", a 23-byte name and "/PERFORM.BIN", well under 80 of the ABI's 255. So a
// buffer two bytes smaller, or a BAD PATH branch deleted, survives the suite, and that is an
// equivalence rather than a missing test. The checks stay: the data folder is Enaga's to name.
bool Storage::folder_path(char *out, uint32_t max, bool autosave) const {
    if (max == 0) return false;
    char *p = out;
    const char *end = out + max - 1;
    append_text(p, enaga::files::data_folder(), end);
    if (autosave) {
        append_text(p, "/AUTOSAVE", end);
    } else {
        append_text(p, "/PROJECTS/", end);
        append_text(p, name_, end);
    }
    if (p >= end) return false;        // truncated: it would name the wrong folder
    *p = '\0';
    return true;
}

bool Storage::project_path(char *out, uint32_t max, bool autosave, const char *file) const {
    if (!folder_path(out, max, autosave)) return false;
    char *p = out;
    const char *end = out + max - 1;
    while (*p != '\0') p++;
    append_text(p, "/", end);
    append_text(p, file, end);
    if (p >= end) return false;
    *p = '\0';
    return true;
}

// --- requests -------------------------------------------------------------

void Storage::request_save() {
    // QUEUED, not dropped. This used to return when a job was in flight -- and a background
    // sample load is the normal state for a second or two after every project open, while
    // app/app.cpp says "SAVING" on the screen regardless. So pressing SAVE just after opening a
    // project did nothing at all and said it had worked. app/storage.hpp claimed requests queue;
    // now they do, and `service()` is what serialises them because it starts a job only when it
    // is idle.
    // The kit the machines read and the kit a file holds are different structures, so the audio
    // side is asked to copy one into the other. Posting it is also what defers the save: the ring
    // is now non-empty, so `service()` waits, and by the time it drains the audio side has
    // applied this and every edit before it.
    post(Cmd::kit_store);
    kit_dirty_ = true;
    save_pending_ = true;
    // Mutation-equivalent (docs/MUTATION.md): service() clears both flags when the save it
    // starts is the full one, so an autosave left pending here never runs on its own. Kept
    // because it states the rule: an explicit save stands an autosave down.
    autosave_pending_ = false;
}

void Storage::request_autosave() {
    if (save_pending_) return;      // a deliberate save is pending: it supersedes an autosave
    post(Cmd::kit_store);
    autosave_pending_ = true;
}

void Storage::request_reload() {
    // The project, not the autosave, even if this session opened the autosave: "read the card
    // again" means the last deliberate save. The UI says RELOADING first, because this throws
    // away everything unsaved.
    //
    // The folder is chosen when the load STARTS, not here: `autosave_` also says which folder a
    // save job in progress is writing, and flipping it under a running autosave sent the rest of
    // that snapshot into PROJECT/ as if it were a deliberate save -- the opposite of discarding.
    load_autosave_ = false;
    load_pending_ = true;
    new_pending_ = false;
    open_pending_ = false;         // the latest ask wins: this one reads the current name
}

namespace {

bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '+' ||
           c == '.' || c == '#' || c == '!';
}

void copy_name(char *out, const char *name, int max) {
    int i = 0;
    for (; name[i] != '\0' && i < max - 1; i++) out[i] = name[i];
    out[i] = '\0';
}

bool same_name(const char *a, const char *b) {
    // FAT compares names without regard to case, so two names differing only in case are one
    // folder.
    for (int i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = char(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = char(y - 'a' + 'A');
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

}  // namespace

bool Storage::valid_name(const char *name) {
    if (name == nullptr || name[0] == '\0' || name[0] == ' ' || name[0] == '.') return false;
    int n = 0;
    for (; name[n] != '\0'; n++) {
        // Lower case too: a folder a PC made, which FAT will find whatever its case.
        char c = name[n];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (!name_char(c)) return false;
    }
    return n < kNameBytes;
}

bool Storage::projects_folder(char *out, uint32_t max) const {
    if (max == 0) return false;
    char *p = out;
    const char *end = out + max - 1;
    append_text(p, enaga::files::data_folder(), end);
    append_text(p, "/PROJECTS", end);
    if (p >= end) return false;
    *p = '\0';
    return true;
}

bool Storage::request_open(const char *name) {
    if (!valid_name(name)) return false;
    copy_name(open_name_, name, kNameBytes);
    open_pending_ = true;
    load_autosave_ = false;
    load_pending_ = true;
    new_pending_ = false;
    return true;
}

bool Storage::request_save_as(const char *name) {
    if (!valid_name(name)) return false;
    if (!same_name(name, name_)) {
        // A new folder: every file is written (`project_complete_`), and until that save lands
        // the screen says UNSAVED, because nothing of this project is under this name yet.
        copy_name(name_, name, kNameBytes);
        project_complete_ = false;
        project_dirty_ = kit_dirty_ = perform_dirty_ = true;
        for (int b = 0; b < kBanks; b++) bank_dirty_[b] = true;
    }
    request_save();
    return true;
}

// --- renaming and deleting projects ---------------------------------------------
//
// app/storage.hpp (request_rename, request_delete) has the order the files go in and why a card
// pulled at any point loses nothing. Neither touches the project in RAM, so neither waits for the
// transport or the ring the way a load does; they are queued behind any save asked for first,
// which is what makes renaming the open project with a save pending rename the saved state.

bool Storage::named_path(char *out, uint32_t max, const char *name, const char *file) const {
    if (max == 0) return false;
    char *p = out;
    const char *end = out + max - 1;
    append_text(p, enaga::files::data_folder(), end);
    append_text(p, "/PROJECTS/", end);
    append_text(p, name, end);
    if (file != nullptr) {
        append_text(p, "/", end);
        append_text(p, file, end);
    }
    if (p >= end) return false;
    *p = '\0';
    return true;
}

bool Storage::request_rename(const char *from, const char *to) {
    if (!valid_name(from) || !valid_name(to) || same_name(from, to)) return false;
    if (!card_usable_ || op_pending_ != Op::none) return false;
    copy_name(op_from_, from, kNameBytes);
    copy_name(op_to_, to, kNameBytes);
    op_pending_ = Op::rename;
    return true;
}

bool Storage::request_delete(const char *name) {
    if (!valid_name(name) || !card_usable_ || op_pending_ != Op::none) return false;
    // The open project, and one an open is about to make the open project.
    if (same_name(name, name_)) return false;
    if (open_pending_ && same_name(name, open_name_)) return false;
    copy_name(op_from_, name, kNameBytes);
    op_pending_ = Op::remove;
    return true;
}

void Storage::start_op() {
    const Op op = op_pending_;
    op_pending_ = Op::none;
    if (op == Op::remove) {
        begin_delete(op_from_, false, nullptr);
    } else {
        begin_job(Job::project_rename);
    }
}

void Storage::begin_delete(const char *name, bool for_rename, const char *carried) {
    copy_name(del_name_, name, kNameBytes);
    del_for_rename_ = for_rename;
    del_carried_ = carried;
    del_count_ = del_next_ = del_removed_ = 0;
    del_overflow_ = false;
    del_listed_ = false;
    // begin_job, not a bare assignment, so error_ starts clear -- but a rename's tail keeps the
    // job it continues counted as one: step_ restarts for the delete's own phases.
    begin_job(Job::project_delete);
}

void Storage::step_rename() {
    char from[ENAGA_FILE_PATH_MAX + 1];
    char to[ENAGA_FILE_PATH_MAX + 1];
    const int index = step_;
    step_++;

    if (index == 0) {
        // The source must be a project: its PROJECT.BIN (or the .TMP an interrupted save left,
        // which read_all falls back to) is what makes the copy worth anything.
        if (!named_path(from, sizeof from, op_from_, "PROJECT.BIN") ||
            !named_path(to, sizeof to, op_to_, nullptr)) {
            finish_job("BAD PATH");
            return;
        }
        const int32_t got = enaga::files::read_all(from, file_, kFileBytes);
        if (got < 0) {
            finish_job(got == ENAGA_FILE_NOT_FOUND ? "NOT A PROJECT"
                                                   : enaga::files::error_name(got));
            return;
        }
        // Another folder under the new name is another project, or what is left of one: never
        // written into. (FAT compares without case, and so does files::info.)
        enaga::files::Info info;
        const int32_t there = enaga::files::info(to, info);
        if (there == ENAGA_FILE_OK) {
            finish_job("NAME TAKEN");
            return;
        }
        if (there != ENAGA_FILE_NOT_FOUND) {
            finish_job(enaga::files::error_name(there));
            return;
        }
        const int32_t made = enaga::files::make_folder(to);
        if (made < 0) {
            finish_job(enaga::files::error_name(made));
            return;
        }
        return;
    }

    // Steps 1 .. kFileCount: KIT.BIN, PERFORM.BIN and the banks, then PROJECT.BIN last.
    if (index <= kFileCount) {
        int file = index;                    // 1 kit, 2 perform, 3.. banks
        if (index == kFileCount) file = kFileProject;
        char name[12];
        const char *leaf = nullptr;
        if (file == kFileProject) {
            leaf = "PROJECT.BIN";
        } else if (file == kFileKit) {
            leaf = "KIT.BIN";
        } else if (file == kFilePerform) {
            leaf = "PERFORM.BIN";
        } else {
            if (!bank_file_name(file - kFileFirstBank, name, int(sizeof name))) return;
            leaf = name;
        }
        if (!named_path(from, sizeof from, op_from_, leaf) ||
            !named_path(to, sizeof to, op_to_, leaf)) {
            begin_delete(op_to_, true, "BAD PATH");
            return;
        }
        const int32_t got = enaga::files::read_all(from, file_, kFileBytes);
        if (got == ENAGA_FILE_NOT_FOUND && file != kFileProject) {
            // A bank never written, or a project from before PERFORM.BIN: absent there, absent
            // here, and it loads the same (step_load treats both as empty).
            return;
        }
        if (got < 0) {
            begin_delete(op_to_, true, got == ENAGA_FILE_NOT_FOUND
                                           ? "NOT A PROJECT"
                                           : enaga::files::error_name(got));
            return;
        }
        const int32_t wrote = enaga::files::save(to, file_, uint32_t(got));
        if (wrote < 0) {
            begin_delete(op_to_, true, enaga::files::error_name(wrote));
            return;
        }
        if (file != kFileProject) return;

        // Both folders are whole now. If this was the open project it is called by the new name
        // from here on, and the settings say so BEFORE the old folder starts to go: a card pulled
        // after this opens the new name, and one pulled before it the old, both complete.
        if (same_name(op_from_, name_)) {
            copy_name(name_, op_to_, kNameBytes);
            settings_.set(kKeyProject, name_);
            const int32_t saved = settings_.save();
            if (saved < 0) {
                // The settings still name the old folder, so it stays: two whole copies, and the
                // next save (which writes the settings again) puts that right.
                finish_job(enaga::files::error_name(saved));
                return;
            }
        }
        begin_delete(op_from_, true, nullptr);
    }
}

void Storage::delete_entry(const enaga::files::Info &entry) {
    // Main context, inside the listing step_delete asked for. A name too long for the table
    // cannot be removed from it; the folder's own removal then fails, and says so.
    if (del_count_ >= kDelEntries) {
        del_overflow_ = true;
        return;
    }
    int n = 0;
    while (entry.name[n] != '\0' && n < kDelNameBytes - 1) n++;
    if (entry.name[n] != '\0') {
        del_overflow_ = true;
        return;
    }
    for (int i = 0; i <= n; i++) del_names_[del_count_][i] = entry.name[i];
    del_count_++;
}

void Storage::delete_listed(int32_t result) {
    del_listing_ = false;
    del_listed_ = true;
    del_list_result_ = result;
}

void Storage::step_delete() {
    char path[ENAGA_FILE_PATH_MAX + 1];
    // Phases: 0 PROJECT.BIN, 1 list the folder, 2 waiting for the list, 3 one entry per pass,
    // 4 the folder itself.
    if (step_ == 0) {
        // The .TMP before the file it stands in for: Enaga's reads fall back to PROJECT.BIN.TMP
        // when PROJECT.BIN is missing, so the other order could leave a folder that still opens.
        for (int i = 0; i < 2; i++) {
            if (!named_path(path, sizeof path, del_name_,
                            i == 0 ? "PROJECT.BIN.TMP" : "PROJECT.BIN")) {
                finish_job("BAD PATH");
                return;
            }
            const int32_t gone = enaga::files::remove(path);
            if (gone < 0 && gone != ENAGA_FILE_NOT_FOUND) {
                finish_job(enaga::files::error_name(gone));
                return;
            }
        }
        step_ = 1;
        return;
    }
    if (step_ == 1) {
        // A listing from a job the card took away may still be answering: let it finish first.
        if (del_listing_) return;
        if (!named_path(path, sizeof path, del_name_, nullptr)) {
            finish_job("BAD PATH");
            return;
        }
        del_count_ = del_next_ = del_removed_ = 0;
        del_overflow_ = false;
        del_listed_ = false;
        del_listing_ = true;
        const int32_t queued = enaga::files::list<&Storage::delete_entry, &Storage::delete_listed>(
            path, this);
        if (queued < 0) {
            del_listing_ = false;
            finish_job(enaga::files::error_name(queued));
            return;
        }
        step_ = 2;
        return;
    }
    if (step_ == 2) {
        if (!del_listed_) return;
        del_listed_ = false;
        if (del_list_result_ == ENAGA_FILE_NOT_FOUND) {
            finish_job(nullptr);             // no folder at all: nothing left to delete
            return;
        }
        if (del_list_result_ < 0) {
            finish_job(enaga::files::error_name(del_list_result_));
            return;
        }
        step_ = 3;
        return;
    }
    if (step_ == 3) {
        if (del_next_ < del_count_) {
            const char *leaf = del_names_[del_next_++];
            if (!named_path(path, sizeof path, del_name_, leaf)) {
                del_overflow_ = true;        // cannot name it: the folder will refuse to go
                return;
            }
            // A file, or an empty folder. A folder with something in it is refused (DENIED) and
            // left, and the folder's own removal below reports it: STRATA never made one.
            const int32_t gone = enaga::files::remove(path);
            if (gone == ENAGA_FILE_OK) {
                del_removed_++;
            } else if (gone != ENAGA_FILE_NOT_FOUND && gone != ENAGA_FILE_DENIED) {
                finish_job(enaga::files::error_name(gone));
            }
            return;
        }
        // More than the table held: list again, for as long as a round gets somewhere.
        step_ = del_overflow_ && del_removed_ > 0 ? 1 : 4;
        return;
    }
    if (!named_path(path, sizeof path, del_name_, nullptr)) {
        finish_job("BAD PATH");
        return;
    }
    const int32_t gone = enaga::files::remove(path);
    if (gone < 0 && gone != ENAGA_FILE_NOT_FOUND) {
        finish_job(gone == ENAGA_FILE_DENIED ? "FOLDER NOT EMPTY" : enaga::files::error_name(gone));
        return;
    }
    finish_job(nullptr);
}

void Storage::request_new(bool blank) {
    // Queued, not done here. It used to run on the spot, and the UI calls this straight from a
    // key press -- with whatever edits that same press and the ones before it left in the ring.
    // `project_init` rewrites 1.03 MiB of pattern memory from the main context, and the audio
    // side applies pending edits to that memory whether or not the clock runs: two contexts
    // writing the same bytes, which is what app/storage.hpp's first protocol exists to rule out.
    // service() starts it once the ring is empty, as it does a load.
    //
    // The latest of a new project and a reload is the one the user meant.
    new_pending_ = true;
    new_blank_ = blank;
    load_pending_ = false;
    open_pending_ = false;
}

void Storage::start_new() {
    // RAM only: nothing is written until a save, so a new project cannot lose the one on the
    // card by itself.
    if (!enaga::clock::running() || enaga::clock::stop()) {
        // The scenes and macros FIRST, then the project, then the post. `Cmd::project_loaded` is
        // what tells the audio side that everything it derives is stale -- the performance layer
        // included (app/engine.cpp sets its own `perform_dirty_` there) -- so it has to come
        // after every write it describes. This used to post first and then call
        // `performance_changed()`, a plain bool the audio side also writes, from this context.
        //
        // The FACTORY project, not a blank one (core/factory.hpp): `project_init` alone leaves
        // LVL 0 on every track, so NEW used to give a silent instrument with nothing in it.
        if (new_blank_) {
            blank_project(engine_->project_for_load(), engine_->scenes_for_load(),
                          engine_->macros_for_load());
        } else {
            factory_project(engine_->project_for_load(), engine_->scenes_for_load(),
                            engine_->macros_for_load());
        }
        post(Cmd::project_loaded);

        // A new project takes a NEW NAME. Keeping the old one would make the next save overwrite
        // a project the user never asked to touch, which is the one unrecoverable mistake this
        // screen could make. The counter lives in the settings so the names stay distinct across
        // restarts; there is no file browser in M4 to offer anything better.
        //
        // A new name names a folder with nothing in it, so the next save has to write all ten:
        // `project_complete_` is what makes a save write every file. The dirty flags below are a
        // second, separate fact -- that RAM differs from the card, which the screen shows -- and
        // either one alone would get this save right, which is why deleting one of them is a
        // mutation no test can kill. Both are kept because each answers its own question.
        project_complete_ = false;

        const int32_t next = settings_.get(kKeyNewSeq, 0) + 1;
        settings_.set(kKeyNewSeq, next);
        char *out = name_;
        const char *end = name_ + kNameBytes - 1;
        append_text(out, "NEW", end);
        append_number(out, uint32_t(next), end);
        *out = '\0';
        // And it is the name the next start uses, so an autosave of this project is not opened
        // under the old one -- whose folder the next save would then overwrite with it. Written
        // to the card with the next save or autosave, which both save the settings.
        settings_.set(kKeyProject, name_);

        // Every track goes through the swap, so the previous project's samples come off their
        // tracks without anything writing over memory a voice is reading. Not pool_.reset(),
        // for the reason step_load gives.
        unfreeze_all();         // a frozen loop is the old project's sound
        sample_queue_ = 0xFFu;
        wanted_budget_ = uint32_t(kSampleBudgetDefaultMib) << 20;
        project_dirty_ = kit_dirty_ = perform_dirty_ = true;
        for (int b = 0; b < kBanks; b++) bank_dirty_[b] = true;
        error_ = nullptr;
        project_error_ = nullptr;
    } else {
        error_ = "EXT CLOCK";
    }
}

bool Storage::open_last() {
    // The autosave is offered when it is newer, which docs/SPEC.md asks for. There is no
    // timestamp in `files::Info` -- it carries a name, a size and flags and nothing else -- so
    // "newer" is a counter STRATA keeps itself, bumped on every save of either kind.
    const int32_t project_seq = settings_.get(kKeyProjectSeq, 0);
    const int32_t auto_seq = settings_.get(kKeyAutoSeq, 0);

    char path[ENAGA_FILE_PATH_MAX + 1];
    // `>` and `>=` differ only on a tie, and the two counters cannot tie once either exists:
    // they are written from one `save_seq_` that every save bumps first. Both 0 is a card with
    // neither, where the choice does not matter.
    const bool prefer_auto = auto_seq > project_seq;
    if (prefer_auto && project_path(path, sizeof path, true, "PROJECT.BIN")) {
        enaga::files::Info info;
        if (enaga::files::info(path, info) == ENAGA_FILE_OK) {
            load_autosave_ = true;
            load_pending_ = true;
            return true;
        }
    }
    if (project_path(path, sizeof path, false, "PROJECT.BIN")) {
        enaga::files::Info info;
        if (enaga::files::info(path, info) == ENAGA_FILE_OK) {
            load_autosave_ = false;
            load_pending_ = true;
            return true;
        }
    }
    // Nothing on the card. Not an error: a first run keeps the engine's built-in default pattern.
    return false;
}

void Storage::note_command(Cmd cmd, int live_bank, int target_bank) {
    // PROJECT's slot copy and clear write the bank under the cursor, not the live one.
    const bool slot_edit = cmd == Cmd::pattern_copy || cmd == Cmd::pattern_clear;
    const int bank = slot_edit && target_bank >= 0 ? target_bank : live_bank;
    if (mutates_pattern(cmd) && project_index_valid(bank, 0)) {
        bank_dirty_[bank] = true;
    }
    if (mutates_kit(cmd)) kit_dirty_ = true;
    if (mutates_project(cmd)) project_dirty_ = true;
    if (mutates_perform(cmd)) perform_dirty_ = true;
}

bool Storage::dirty() const {
    if (project_dirty_ || kit_dirty_ || perform_dirty_) return true;
    for (int b = 0; b < kBanks; b++) {
        if (bank_dirty_[b]) return true;
    }
    return false;
}

// --- the card -------------------------------------------------------------

void Storage::on_card(bool usable) {
    // A card that came or went may hold anything, so forget whether the project is complete on
    // it. The flag says "all ten files are there", and after a swap that is a claim about a card
    // nobody has looked at: the one left behind might hold a different project, an older copy of
    // this one, or nothing. Getting it wrong writes only the dirty files onto a card with none,
    // which is the data-loss `project_complete_` exists to prevent, arriving by another route.
    //
    // On the transition only, so a redundant on_card(true) does not force a full save every time.
    if (usable != card_usable_) project_complete_ = false;

    // Whatever the kit library said is about a card that may no longer be in the slot.
    for (int i = 0; i < kKitSlots; i++) kit_slots_[i] = KitSlot{};
    kit_save_slot_ = kit_load_slot_ = -1;

    card_usable_ = usable;
    if (usable) {
        error_ = nullptr;
        // Whatever the kit names, offered again. Every track that was loading or loaded when the
        // card went out was failed below, so without this they stay silent until the user asks
        // track by track. Each one still goes through the full swap, so this is safe even while
        // something is playing.
        request_all_samples();
        return;
    }
    // Every handle opened before a removal is already dead (<enaga/app.hpp>), and a load in
    // flight owns `raw_` until its callback -- which will still arrive, with an error. So the job
    // is abandoned here but `read_in_flight_` is left alone: nothing may reuse that buffer until
    // Enaga has finished with it.
    //
    // (`sample_track_` is -1 whenever no sample job runs, so the job test is belt and braces, and
    // the `sample_queue_ = 0` below is re-filled by the request_all_samples a returning card
    // makes. Both survive mutation for that reason.)
    // A long file's handle is dead with the card; closing it ends its pending read CANCELLED.
    chunk_file_.close();
    chunked_ = false;
    // A take half written: its handle is dead too, and the buffer goes back to the audio side,
    // or the next RESAMPLE could never arm. The file left on the card is cut short.
    if (job_ == Job::take_write) {
        if (take_file_.is_open()) take_file_.close();
        take_write_done_ = false;
        take_track_ = -1;
        post(Cmd::resample_release);
    }
    if (job_ == Job::sample_epoch || job_ == Job::sample_read || job_ == Job::sample_convert ||
        job_ == Job::sample_resample || job_ == Job::sample_chunk) {
        if (sample_track_ >= 0) pool_.fail(sample_track_);
        sample_track_ = -1;
    }
    job_ = Job::idle;
    save_pending_ = autosave_pending_ = load_pending_ = open_pending_ = opening_ = false;
    kit_scan_pending_ = false;
    // A rename or delete stops where it was; the order its files go in is what makes that safe.
    // A listing it had in flight still answers, and step_delete waits for that before another.
    op_pending_ = Op::none;
    sample_queue_ = 0;
    error_ = "NO CARD";
}

// --- the job in flight ----------------------------------------------------

void Storage::begin_job(Job job) {
    job_ = job;
    step_ = 0;      // finish_job zeroes it too; either alone suffices, and each survives deletion

    error_ = nullptr;

    // A SAVE or a LOAD also clears the PROJECT error, which the sample jobs do not.
    //
    // Why there are two. `step_load` ends by queueing the sample swaps, so the very next
    // `service()` pass used to start one and wipe the reason the load had just refused a file --
    // and `update()` runs over 750 times a second against a screen that draws at 20 fps, so a
    // damaged file was SILENT, against docs/SPEC.md's "refused with a message on screen".
    //
    // Making `error_` survive a sample job instead would have broken its own documented
    // contract, "sticky until the next job starts", which three tests rightly hold it to. So the
    // project's reason gets a slot of its own, and `last_error()` prefers the newer of the two.
    // A rename or a delete is a project job too: its reason is about a project, and it is what
    // the screen should say over a stale one from before it.
    if (job == Job::save || job == Job::load || job == Job::project_rename ||
        job == Job::project_delete) {
        project_error_ = nullptr;
    }
}

void Storage::finish_job(const char *error) {
    // One log line per finished SAVE or LOAD, and only those. It is the only way an emulator
    // scenario can see whether a save worked: pattern memory cannot be read from outside the
    // app and the screen comes back as a bitmap, which is the same reason app/app.cpp logs its
    // `g.... u..` edit signature (tests/scenario_m3.sh explains it at length).
    //
    // Deliberately NOT logged for the sample jobs. Enaga's log ring holds sixteen lines, and a
    // project open queues eight sample jobs -- which would push every other line out of the ring
    // and break the scenarios that read it.
    if (job_ == Job::save || job_ == Job::load) {
        const bool saving = job_ == Job::save;
        if (error != nullptr) {
            char line[40];
            char *out = line;
            const char *what = saving ? "save failed " : "load failed ";
            while (*what != '\0') *out++ = *what++;
            const char *why = error;
            // 12 + 24 + NUL fits 40 with room to spare, so `<=` here is not an overflow either.
            while (*why != '\0' && out < line + 36) *out++ = *why++;
            *out = '\0';
            enaga::log(line);
        } else {
            enaga::log(saving ? (autosave_ ? "autosaved" : "saved") : "loaded");

            // The PROJECT folder now holds all ten files: a save that got here wrote every one
            // it was asked for, and a load that got here found every one it needed. Only for the
            // project folder -- an autosave says nothing about what is in the project's own.
            // A failed save leaves this alone, so the next one writes everything again.
            if (!autosave_) project_complete_ = true;
        }
    }

    // An open that could not read the project's PROJECT.BIN changed nothing in RAM (step_load
    // stops there), so the project keeps its own name. One that got through is now the project
    // the next start opens: named in the settings and counted newer than any autosave, which
    // still holds the project that was left.
    if (job_ == Job::load && opening_) {
        opening_ = false;
        if (error != nullptr) {
            copy_name(name_, kept_name_, kNameBytes);
        } else {
            save_seq_++;
            settings_.set(kKeyProjectSeq, int32_t(save_seq_));
            settings_.set(kKeyProject, name_);
            settings_.save();
        }
    }

    // The kit library's two writes and reads get a line too, for the same reason: a scenario can
    // see nothing else of them. The scan does not -- sixteen passes of it would be noise.
    if (job_ == Job::kit_save || job_ == Job::kit_load) {
        char line[40];
        char *out = line;
        const char *end = line + 36;
        append_text(out, job_ == Job::kit_save ? "kit save" : "kit load", end);
        append_text(out, error != nullptr ? " failed " : (job_ == Job::kit_save ? "d" : "ed"),
                    end);
        if (error != nullptr) append_text(out, error, end);
        *out = '\0';
        enaga::log(line);
    }

    // A rename or a delete gets a line too: the scenarios see nothing else of it. A rename's
    // reason travels through the delete of what it had made (del_carried_).
    if (job_ == Job::project_rename || job_ == Job::project_delete) {
        const bool renaming = job_ == Job::project_rename || del_for_rename_;
        if (job_ == Job::project_delete && error == nullptr) error = del_carried_;
        char line[40];
        char *out = line;
        const char *end = line + 36;
        append_text(out, renaming ? "rename" : "delete", end);
        append_text(out, error != nullptr ? " failed " : "d", end);
        if (error != nullptr) append_text(out, error, end);
        *out = '\0';
        enaga::log(line);
        del_for_rename_ = false;
        del_carried_ = nullptr;
    }

    // A long file read in chunks is finished with whichever way its job ended, and so is a
    // stream's head (a stream that got as far as hand_stream_over has its own plan by now).
    if (chunk_file_.is_open()) chunk_file_.close();
    chunked_ = false;
    streaming_ = false;

    const bool project_job = job_ == Job::save || job_ == Job::load ||
                             job_ == Job::project_rename || job_ == Job::project_delete;
    job_ = Job::idle;
    step_ = 0;
    if (error != nullptr) error_ = error;
    // And kept where the sample swaps that follow a load cannot reach it. From `error_`, not only
    // from `error`: a load that refuses one file carries on with the rest -- step_load records
    // the reason in `error_` and finishes with nullptr -- and that reason is the one the screen
    // most needs. Taking it from `error` alone left it in the slot the very next sample job
    // clears, which is the bug `project_error_` was added to fix.
    if (project_job && error_ != nullptr) project_error_ = error_;
}

const char *Storage::busy_text() const {
    switch (job_) {
        case Job::save: return autosave_ ? "AUTOSAVING" : "SAVING";
        case Job::load: return "LOADING";
        case Job::sample_epoch: return "SWAPPING";
        case Job::sample_read: return "READING";
        case Job::sample_convert: return "CONVERTING";
        case Job::sample_chunk: return "LOADING LONG";
        case Job::sample_resample: return "RESAMPLING";
        case Job::take_write: return "WRITING";
        case Job::kit_scan: return "READING KITS";
        case Job::kit_save: return "SAVING KIT";
        case Job::kit_load: return "LOADING KIT";
        case Job::project_rename: return "RENAMING";
        case Job::project_delete: return del_for_rename_ ? "RENAMING" : "DELETING";
        default: return nullptr;
    }
}

int Storage::progress() const {
    switch (job_) {
        case Job::save:
        case Job::load:
            return step_ * 100 / kFileCount;
        // 32-bit arithmetic, deliberately. A 64-bit divide here pulled libgcc's
        // __c6xabi_divull into the link, and on this toolchain that object also drags in the
        // ARM-style unwinder, whose __exidx_start / __exidx_end the app's linker script does not
        // define -- so the whole app failed to LINK for the sake of a percentage on the screen.
        // (That is the same failure seawaffle/enaga#1 fixes from the other side, and
        // core/pcm.cpp hand-writes div_u64 rather than divide 64-bit for the same reason.)
        //
        // Both products fit: frames are capped at kSrcFrames (2,880,000) and at the 24 bits
        // Cmd::set_sample carries (16,777,215), so times 100 they stay inside a uint32_t.
        case Job::sample_convert:
        case Job::sample_chunk:
            return src_frames_ == 0 ? 0 : int(src_done_ * 100u / src_frames_);
        case Job::sample_read:
            // Between two chunks of a long file: how far it has got, not 0.
            return chunked_ && src_frames_ != 0 ? int(src_done_ * 100u / src_frames_) : 0;
        case Job::sample_resample:
            return out_frames_ == 0 ? 0 : int(out_done_ * 100u / out_frames_);
        case Job::kit_scan:
            return step_ * 100 / kKitSlots;
        case Job::project_rename:
            return step_ * 100 / (kFileCount + 1);
        default:
            return 0;
    }
}

int Storage::loading_track() const { return sample_track_; }

void Storage::service() {
    // A command that did not fit in the ring goes first, and until it has, nothing moves on.
    if (!flush_backlog()) return;

    // Streams before anything else, every pass and whatever job runs: a restart has the head's
    // two seconds to be reopened, and a 2 MiB chunk read must not stand in its way.
    service_streams();

    // Freeze next, whatever job runs and with or without a card: it is RAM only, and a loop
    // whose slot is waiting to be given back should not wait behind a save.
    if (service_freeze()) return;

    // A new project first, and before the card check: it is RAM only, so it needs no card. It
    // waits for an idle loader and an empty ring like a load, and for a save asked for before
    // it, which belongs to the project being replaced.
    if (new_pending_ && job_ == Job::idle && engine_->commands().empty() &&
        !(card_usable_ && (save_pending_ || autosave_pending_))) {
        new_pending_ = false;
        start_new();
        return;
    }

    if (!card_usable_) return;

    // A save or a load is one file per pass, and the UI runs between passes. The ring was empty
    // when the job STARTED (below), but an edit posted since can be applied by the audio side in
    // the middle of this pass -- audio() preempts the main context -- so a save would serialize
    // a step half old and half new, and a load would have two contexts writing the same pattern
    // bytes. So every pass waits for the ring exactly as the first one did. A knob turned without
    // pause holds the job back until it stops; that is the same trade the start already makes.
    if ((job_ == Job::save || job_ == Job::load) && !engine_->commands().empty()) return;

    switch (job_) {
        case Job::save: step_save(); return;
        case Job::load: step_load(); return;
        case Job::sample_epoch: step_sample_epoch(); return;
        case Job::sample_read: 
            // Nothing to do but wait: Enaga works the load in the main loop between callbacks.
            if (read_done_) {
                read_done_ = false;   // and again before every read starts, so equivalent
                if (read_result_ < 0) {
                    if (sample_track_ >= 0) pool_.fail(sample_track_);
                    sample_track_ = -1;
                    finish_job(enaga::files::error_name(read_result_));
                } else if (chunked_) {
                    // The chunk is in raw_ after whatever part of a frame was carried over.
                    chunk_have_ = chunk_carry_ + uint32_t(read_result_);
                    chunk_file_pos_ += uint32_t(read_result_);
                    chunk_pos_ = 0;
                    begin_job(Job::sample_chunk);
                    step_sample_chunk();
                } else {
                    // The job has to MOVE ON here. Leaving it at sample_read would stall after
                    // this first slice: the next pass would find read_done_ false and do nothing.
                    begin_job(Job::sample_convert);
                    step_sample_convert();   // the header is parsed there, on the first slice
                }
            }
            return;
        case Job::sample_convert: step_sample_convert(); return;
        case Job::sample_resample: step_sample_resample(); return;
        case Job::sample_chunk: step_sample_chunk(); return;
        case Job::take_write: step_take_write(); return;
        case Job::kit_scan: step_kit_scan(); return;
        case Job::project_rename: step_rename(); return;
        case Job::project_delete: step_delete(); return;
        // Both run start to finish in the pass that begins them, so neither is ever found here.
        case Job::kit_save:
        case Job::kit_load:
        case Job::idle: break;
    }

    // Idle. A save or a load may only start once the ring is empty: see the protocols in
    // app/storage.hpp. Nothing here spins -- an un-drained ring simply means "not this pass".
    if (!engine_->commands().empty()) return;

    if (load_pending_) {
        // A load writes 1.03 MiB into pattern memory, so the transport has to be stopped first.
        // Under an external clock `stop()` returns false and the load is refused, which is
        // CLAUDE.md's rule about handling the refusal rather than loading anyway.
        if (enaga::clock::running() && !enaga::clock::stop()) {
            load_pending_ = open_pending_ = false;
            error_ = "EXT CLOCK";
            return;
        }
        load_pending_ = false;
        autosave_ = load_autosave_;
        // An open reads the other project's folder: the name changes now, for project_path, and
        // goes back in finish_job if that folder turns out not to hold a project.
        opening_ = open_pending_;
        open_pending_ = false;
        if (opening_) {
            copy_name(kept_name_, name_, kNameBytes);
            copy_name(name_, open_name_, kNameBytes);
        }
        begin_job(Job::load);
        return;
    }
    if (save_pending_ || autosave_pending_) {
        autosave_ = !save_pending_;
        save_pending_ = autosave_pending_ = false;
        begin_job(Job::save);
        return;
    }
    // A rename or a delete: after a save, which belongs to the project as it was asked for.
    if (op_pending_ != Op::none) {
        start_op();
        return;
    }
    // The kit library. After a project save or load -- which may be about to replace the kit a
    // save would store -- and before the samples, because a recall queues swaps of its own.
    // Each is one file, done in this pass: the ring is empty (checked above), so the audio side
    // has applied every edit and the `kit_store` a save posted, and is writing nothing.
    if (kit_save_slot_ >= 0) {
        run_kit_save();
        return;
    }
    if (kit_load_slot_ >= 0 && !read_in_flight_) {
        run_kit_load();
        return;
    }
    if (kit_scan_pending_) {
        kit_scan_pending_ = false;
        begin_job(Job::kit_scan);
        return;
    }

    // A finished take, before the sample queue: the whole point is that it is written and then
    // loaded back, and the load-back goes through that same queue.
    if (take_pending_ &&
        engine_->status().resample_state.load(std::memory_order_relaxed) == 3u) {
        take_pending_ = false;
        begin_job(Job::take_write);
        return;
    }

    if (sample_queue_ != 0 && !read_in_flight_) {
        start_next_sample();
        return;
    }

    // A project's budget, applied once every track has been through the swap above. It cannot be
    // applied earlier: SamplePool::set_budget refuses to shrink below what is already reserved
    // rather than stranding a loaded sample, and until the clears have run the previous project's
    // ranges are still reserved. By here nothing is, so this cannot fail -- and if it ever does,
    // the budget stays higher than the project asked, which only ever allows more.
    if (wanted_budget_ != 0 && !read_in_flight_) {
        const uint32_t wanted = wanted_budget_;
        wanted_budget_ = 0;     // set_budget is idempotent, so re-applying it is harmless
        if (!pool_.set_budget(wanted)) error_ = "BUDGET BUSY";
    }
}

// --- saving ---------------------------------------------------------------

void Storage::step_save() {
    char path[ENAGA_FILE_PATH_MAX + 1];

    // The folder first. make_folder makes parents too and is OK if it already exists.
    if (step_ == 0) {
        if (!folder_path(path, sizeof path, autosave_)) {
            finish_job("BAD PATH");
            return;
        }
        const int32_t made = enaga::files::make_folder(path);
        if (made < 0) {
            finish_job(enaga::files::error_name(made));
            return;
        }
    }

    // One file per pass. A bank is about 129 KiB, which docs/SPEC.md budgets at tens of
    // milliseconds of the UI and none of the audio.
    const int index = step_;
    step_++;

    // An autosave writes everything: it is a snapshot, and the dirty flags describe how RAM
    // differs from the PROJECT, not from the autosave.
    //
    // And so does the FIRST save of a project, for the same reason turned around: there is no
    // file on the card for a flag to differ from, so "only what changed" is every file. Without
    // this, a first save wrote KIT.BIN alone -- `request_save` is what marks the kit -- and the
    // nine others never existed, so reloading the project failed with NOT FOUND.
    const bool all = autosave_ || !project_complete_;
    uint32_t bytes = 0;
    const char *file = nullptr;
    SerializeResult res = SerializeResult::ok;
    const Project &project = engine_->project();

    if (index == kFileProject) {
        if (!all && !project_dirty_) return;
        file = "PROJECT.BIN";
        res = project_write(project, file_, kFileBytes, &bytes);
    } else if (index == kFileKit) {
        if (!all && !kit_dirty_) return;
        file = "KIT.BIN";
        res = kit_write(project.kit, file_, kFileBytes, &bytes);
    } else if (index == kFilePerform) {
        if (!all && !perform_dirty_) return;
        file = "PERFORM.BIN";
        res = perform_write(engine_->scenes(), engine_->macros(), file_, kFileBytes, &bytes);
    } else if (index < kFileCount) {
        // (`<=` survives: index kFileCount names bank kBanks, bank_file_name refuses it, and the
        // pass is wasted before the next one finishes the job. Equivalent, one pass slower.)
        const int bank = index - kFileFirstBank;
        if (!all && !bank_dirty_[bank]) return;
        char name[12];
        if (!bank_file_name(bank, name, int(sizeof name))) return;
        if (!project_path(path, sizeof path, autosave_, name)) {
            finish_job("BAD PATH");
            return;
        }
        // A serializer fails only for a buffer too small, and `file_` is kBankMaxBytes, the
        // largest file there is -- so these refusals cannot happen and deleting their
        // finish_job survives. Kept, because the sizes are another file's to change.
        res = bank_write(project, bank, file_, kFileBytes, &bytes);
        if (res != SerializeResult::ok) {
            finish_job(serialize_result_name(res));
            return;
        }
        const int32_t wrote = enaga::files::save(path, file_, bytes);
        if (wrote < 0) {
            finish_job(enaga::files::error_name(wrote));
            return;
        }
        if (!autosave_) bank_dirty_[bank] = false;
        return;
    } else {
        // Every file down. The sequence counter is what a later start compares, since
        // `files::Info` carries no timestamp.
        save_seq_++;
        settings_.set(autosave_ ? kKeyAutoSeq : kKeyProjectSeq, int32_t(save_seq_));
        if (!autosave_) settings_.set(kKeyProject, name_);
        const int32_t saved = settings_.save();
        finish_job(saved < 0 ? enaga::files::error_name(saved) : nullptr);
        return;
    }

    if (res != SerializeResult::ok) {
        finish_job(serialize_result_name(res));
        return;
    }
    if (!project_path(path, sizeof path, autosave_, file)) {
        finish_job("BAD PATH");
        return;
    }
    // files::save writes NAME.TMP and then replaces NAME with it, so a card pulled or power lost
    // mid-save leaves either the old file or the new one -- which is what M4's exit criterion
    // ("pulling the card during a save never corrupts the project") rests on.
    const int32_t wrote = enaga::files::save(path, file_, bytes);
    if (wrote < 0) {
        finish_job(enaga::files::error_name(wrote));
        return;
    }
    if (!autosave_) {
        // `!=` survives: request_save marks the kit dirty every time, so KIT.BIN is written by
        // every deliberate save, and a clear moved onto that write ends in the same state.
        if (index == kFileProject) project_dirty_ = false;
        if (index == kFileKit) kit_dirty_ = false;
        if (index == kFilePerform) perform_dirty_ = false;
    }
}

// --- loading --------------------------------------------------------------

void Storage::step_load() {
    char path[ENAGA_FILE_PATH_MAX + 1];
    const int index = step_;
    step_++;

    // The transport was stopped before this job began, so writing here is safe: with the clock
    // still, `clock::ticks()` yields nothing and the scheduler never reads the pattern.
    Project &project = engine_->project_for_load();

    if (index == kFileProject) {
        if (!project_path(path, sizeof path, autosave_, "PROJECT.BIN")) {
            finish_job("BAD PATH");
            return;
        }
        const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
        if (got < 0) {
            finish_job(enaga::files::error_name(got));
            return;
        }
        const SerializeResult res = project_read(project, file_, uint32_t(got));
        // A refused PROJECT.BIN is reported but does not stop the load: core/project.cpp leaves
        // the settings at their defaults and deliberately does not touch a pattern, so the banks
        // that follow still load. One damaged 292-byte file must not cost 128 patterns.
        if (res != SerializeResult::ok) error_ = serialize_result_name(res);
        return;
    }

    if (index == kFileKit) {
        if (!project_path(path, sizeof path, autosave_, "KIT.BIN")) return;
        const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
        if (got < 0) {
            error_ = enaga::files::error_name(got);
            return;
        }
        const SerializeResult res = kit_read(project.kit, file_, uint32_t(got));
        if (res != SerializeResult::ok) error_ = serialize_result_name(res);
        return;
    }

    if (index == kFilePerform) {
        if (!project_path(path, sizeof path, autosave_, "PERFORM.BIN")) return;
        const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
        if (got < 0) {
            // A project saved by an M4 build has no PERFORM.BIN. That is not damage: it means no
            // scenes and no macros, which is what an empty set is.
            if (got != ENAGA_FILE_NOT_FOUND) error_ = enaga::files::error_name(got);
            perform_init(engine_->scenes_for_load(), engine_->macros_for_load());
            engine_->performance_changed();
            return;
        }
        const SerializeResult res = perform_read(engine_->scenes_for_load(),
                                                 engine_->macros_for_load(), file_,
                                                 uint32_t(got));
        if (res != SerializeResult::ok) error_ = serialize_result_name(res);
        engine_->performance_changed();
        return;
    }

    if (index < kFileCount) {
        const int bank = index - kFileFirstBank;
        char name[12];
        if (!bank_file_name(bank, name, int(sizeof name))) return;
        if (!project_path(path, sizeof path, autosave_, name)) return;
        const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
        if (got < 0) {
            // A missing bank file is an empty bank, not a failure: a project saved before a bank
            // was ever touched has no file for it.
            if (got != ENAGA_FILE_NOT_FOUND) error_ = enaga::files::error_name(got);
            for (int slot = 0; slot < kPatternsPerBank; slot++) {
                pattern_init(project.patterns[bank][slot]);
            }
            return;
        }
        int refused = 0;
        const SerializeResult res = bank_read(project, bank, file_, uint32_t(got), &refused);
        if (res != SerializeResult::ok) {
            error_ = serialize_result_name(res);
        } else if (refused > 0) {
            // One damaged pattern costs one pattern, and the UI says so rather than pretending
            // the bank loaded cleanly (core/project.hpp).
            error_ = "PATTERN LOST";
        }
        return;
    }

    // Everything in. Hand it to the audio side, which re-derives the kit, the scheduler and the
    // undo history from what just arrived.
    post(Cmd::project_loaded);

    // What is in RAM is what is on the card.
    project_dirty_ = kit_dirty_ = perform_dirty_ = false;
    for (int b = 0; b < kBanks; b++) bank_dirty_[b] = false;

    // Every track, not just the ones the new kit names a file for. `pool_.reset()` would be
    // wrong here: it forgets every reservation, and its own contract says the caller must have
    // silenced every track first -- which nothing has. The samplers are still holding pointers
    // into those ranges, so the next load would write over memory a voice is reading, which is
    // exactly what the epoch protocol exists to prevent.
    //
    // So each track goes through clear -> wait for the epoch -> release in turn, and a track
    // whose new kit names no file simply stops there. Until its turn comes it keeps playing the
    // old project's sample, which is a second or two of the wrong sound rather than a corrupted
    // read.
    unfreeze_all();         // a frozen loop is the old project's sound
    sample_queue_ = 0xFFu;
    wanted_budget_ = project_budget(project.settings.sample_budget_mib);

    finish_job(nullptr);
}

// --- the kit library --------------------------------------------------------
//
// app/storage.hpp has the design. Sixteen files in one folder beside PROJECTS/, shared by every
// project, each in KIT.BIN's format.

bool Storage::kit_path(char *out, uint32_t max, int slot, bool folder_only) const {
    if (max == 0 || slot < 0 || slot >= kKitSlots) return false;
    char *p = out;
    const char *end = out + max - 1;
    append_text(p, enaga::files::data_folder(), end);
    append_text(p, "/KITS", end);
    if (!folder_only) {
        append_text(p, "/KIT_", end);
        if (p < end) *p++ = char('0' + (slot + 1) / 10);
        if (p < end) *p++ = char('0' + (slot + 1) % 10);
        append_text(p, ".BIN", end);
    }
    if (p >= end) return false;
    *p = '\0';
    return true;
}

const Storage::KitSlot &Storage::kit_slot(int slot) const {
    static const KitSlot none{};
    return slot >= 0 && slot < kKitSlots ? kit_slots_[slot] : none;
}

void Storage::request_kit_scan() {
    if (!card_usable_) return;
    kit_scan_pending_ = true;
}

bool Storage::request_kit_save(int slot) {
    if (slot < 0 || slot >= kKitSlots || !card_usable_) return false;
    // The kit the machines read and the kit a file holds are different structures, so the audio
    // side copies one into the other first -- and posting it is what defers the write past every
    // edit before it, exactly as `request_save` does. It also makes the project's own KIT.BIN
    // differ from RAM, as it does for a project save.
    post(Cmd::kit_store);
    kit_dirty_ = true;
    kit_save_slot_ = slot;
    kit_load_slot_ = -1;      // the later of the two is the one the user meant
    return true;
}

bool Storage::request_kit_load(int slot) {
    if (slot < 0 || slot >= kKitSlots || !card_usable_) return false;
    kit_load_slot_ = slot;
    kit_save_slot_ = -1;
    return true;
}

void Storage::step_kit_scan() {
    const int slot = step_;
    step_++;
    if (slot >= kKitSlots) {
        finish_job(nullptr);
        return;
    }
    KitSlot &out = kit_slots_[slot];
    out = KitSlot{};
    char path[ENAGA_FILE_PATH_MAX + 1];
    if (!kit_path(path, sizeof path, slot, false)) {
        out.state = SlotState::bad;
        return;
    }
    const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
    if (got < 0) {
        // NOT FOUND is an empty slot. Anything else is a slot that could not be read, which the
        // browser shows rather than offering it as empty and inviting an overwrite.
        out.state = got == ENAGA_FILE_NOT_FOUND ? SlotState::empty : SlotState::bad;
        return;
    }
    if (kit_read(kit_scratch_, file_, uint32_t(got)) != SerializeResult::ok) {
        out.state = SlotState::bad;
        return;
    }
    out.state = SlotState::used;
    for (int t = 0; t < kTracks; t++) out.machines[t] = kit_scratch_.tracks[t].machine;
}

void Storage::run_kit_save() {
    const int slot = kit_save_slot_;
    kit_save_slot_ = -1;
    begin_job(Job::kit_save);

    char path[ENAGA_FILE_PATH_MAX + 1];
    if (!kit_path(path, sizeof path, slot, true)) {
        finish_job("BAD PATH");
        return;
    }
    const int32_t made = enaga::files::make_folder(path);
    if (made < 0) {
        finish_job(enaga::files::error_name(made));
        return;
    }
    if (!kit_path(path, sizeof path, slot, false)) {
        finish_job("BAD PATH");
        return;
    }
    // `kit_store` has been applied -- the ring was empty to get here -- so this is the kit as it
    // sounds, sample paths included.
    const Kit &kit = engine_->project().kit;
    uint32_t bytes = 0;
    const SerializeResult res = kit_write(kit, file_, kFileBytes, &bytes);
    if (res != SerializeResult::ok) {
        finish_job(serialize_result_name(res));
        return;
    }
    // files::save writes a .TMP and renames it, so a pulled card leaves the old kit or the new.
    const int32_t wrote = enaga::files::save(path, file_, bytes);
    if (wrote < 0) {
        finish_job(enaga::files::error_name(wrote));
        return;
    }
    KitSlot &shown = kit_slots_[slot];
    shown.state = SlotState::used;
    for (int t = 0; t < kTracks; t++) shown.machines[t] = kit.tracks[t].machine;
    finish_job(nullptr);
}

void Storage::run_kit_load() {
    const int slot = kit_load_slot_;
    kit_load_slot_ = -1;
    begin_job(Job::kit_load);

    char path[ENAGA_FILE_PATH_MAX + 1];
    if (!kit_path(path, sizeof path, slot, false)) {
        finish_job("BAD PATH");
        return;
    }
    const int32_t got = enaga::files::read_all(path, file_, kFileBytes);
    if (got < 0) {
        if (got == ENAGA_FILE_NOT_FOUND) kit_slots_[slot].state = SlotState::empty;
        finish_job(got == ENAGA_FILE_NOT_FOUND ? "EMPTY SLOT" : enaga::files::error_name(got));
        return;
    }
    // Into the scratch kit first, so a damaged file reaches neither the engine's inbox nor the
    // project's sample paths: a refused kit changes nothing at all.
    const SerializeResult res = kit_read(kit_scratch_, file_, uint32_t(got));
    if (res != SerializeResult::ok) {
        kit_slots_[slot].state = SlotState::bad;
        finish_job(serialize_result_name(res));
        return;
    }

    // The ring is empty (service() checked), so no recall is pending and the audio side is not
    // reading the inbox: this is the one window in which the main context may write it.
    Kit &inbox = engine_->kit_for_recall();
    inbox = kit_scratch_;

    // The sample paths are the main context's (app/engine.cpp, Cmd::kit_recall). A track whose
    // path changed goes through the full swap -- clear, wait for the epoch, release, load -- and
    // one whose path is the same keeps the sample it is playing, so recalling a kit that only
    // differs in its knobs does not drop a single note of a loop.
    Kit &kit = engine_->project_for_load().kit;
    for (int t = 0; t < kTracks; t++) {
        char *to = kit.tracks[t].sample;
        const char *from = kit_scratch_.tracks[t].sample;
        bool same = true;
        for (int i = 0; i < kSamplePathBytes; i++) {
            if (to[i] != from[i]) { same = false; break; }
            if (to[i] == '\0') break;
        }
        if (same) continue;
        for (int i = 0; i < kSamplePathBytes; i++) to[i] = from[i];
        to[kSamplePathBytes - 1] = '\0';
        sample_queue_ = uint8_t(sample_queue_ | (1u << t));
    }

    // A frozen loop is the old kit's sound.
    unfreeze_all();

    Command command;
    command.cmd = Cmd::kit_recall;
    command.a = 1;
    post(command);

    // RAM's kit now differs from the project's KIT.BIN.
    kit_dirty_ = true;
    KitSlot &shown = kit_slots_[slot];
    shown.state = SlotState::used;
    for (int t = 0; t < kTracks; t++) shown.machines[t] = kit_scratch_.tracks[t].machine;
    finish_job(nullptr);
}

// --- samples --------------------------------------------------------------
//
// The loader half of docs/SPEC.md's generation-counter handoff. The order is the whole point:
// post `clear_sample`, WAIT for the epoch, release, load, hand over. Every other order writes
// over memory a voice may still be reading.

bool Storage::take_busy() const {
    return job_ == Job::take_write || freeze_track_ >= 0 ||
           (take_pending_ &&
            engine_->status().resample_state.load(std::memory_order_relaxed) == 3u);
}

bool Storage::request_resample_write(int into_track) {
    if (into_track < -1 || into_track >= kTracks) return false;
    // A take on its way to the card keeps the destination it was asked for: the write runs over
    // many passes, and finish_take_write reads take_track_ only at the end.
    if (take_busy()) return false;
    take_track_ = into_track;
    take_pending_ = true;
    return true;
}

// --- freeze (dsp/frozen.hpp) ------------------------------------------------

bool Storage::request_freeze(int track, int bars) {
    if (track < 0 || track >= kTracks) return false;
    freeze_error_ = nullptr;
    const uint8_t bit = uint8_t(1u << track);
    const bool frozen = (engine_->status().frozen.load(std::memory_order_relaxed) & bit) != 0;
    if (frozen || (unfreeze_wait_ & bit) != 0) return false;
    if (engine_->machine_id(track) == uint8_t(MachineId::midi)) return false;
    // Any take armed, running or waiting for the card: one capture buffer, one take at a time.
    if (take_busy() || engine_->status().resample_state.load(std::memory_order_relaxed) != 0u) {
        return false;
    }
    // The performance mixer's looper holds the same buffer (dsp/looper.hpp): the engine would
    // refuse the arm, and this side would wait for a take that never comes.
    if (engine_->status().looper_state.load(std::memory_order_relaxed) != 0u) return false;
    // A write asked for a take that was then cancelled (RESAMPLE, OK then CLR) is stale: it must
    // not write this take to the card when it finishes.
    take_pending_ = false;
    take_track_ = -1;
    const int b = bars <= 1 ? 1 : bars <= 2 ? 2 : bars <= 4 ? 4 : 8;
    Command arm;
    arm.cmd = Cmd::resample_arm;
    arm.track = uint8_t(track);
    arm.a = uint8_t(b);
    arm.b = 3;                              // before the insert chain
    freeze_refused_seen_ = engine_->status().freeze_refused.load(std::memory_order_relaxed);
    post(arm);
    freeze_track_ = track;
    freeze_bars_ = uint8_t(b);
    return true;
}

bool Storage::request_unfreeze(int track) {
    if (track < 0 || track >= kTracks) return false;
    const uint8_t bit = uint8_t(1u << track);
    if (freeze_track_ == track) {
        // Its take, still on its way: thrown away, and the buffer given back either way.
        post(Cmd::resample_cancel);
        post(Cmd::resample_release);
        freeze_track_ = -1;
    }
    if ((engine_->status().frozen.load(std::memory_order_relaxed) & bit) == 0 &&
        pool_.slot(freeze_slot(track)).state == SampleState::empty) {
        return false;
    }
    if ((unfreeze_wait_ & bit) != 0) return true;           // already on its way back
    unfreeze_epoch_[track] = engine_->status().freeze_epoch[track].load(std::memory_order_relaxed);
    unfreeze_wait_ = uint8_t(unfreeze_wait_ | bit);
    Command clear;
    clear.cmd = Cmd::freeze_clear;
    clear.track = uint8_t(track);
    post(clear);
    return true;
}

void Storage::unfreeze_all() {
    if (freeze_track_ >= 0) request_unfreeze(freeze_track_);
    for (int t = 0; t < kTracks; t++) request_unfreeze(t);
}

bool Storage::service_freeze() {
    // A slot whose loop the audio side has let go of goes back to the pool.
    for (int t = 0; t < kTracks && unfreeze_wait_ != 0; t++) {
        const uint8_t bit = uint8_t(1u << t);
        if ((unfreeze_wait_ & bit) == 0) continue;
        if (engine_->status().freeze_epoch[t].load(std::memory_order_relaxed) ==
            unfreeze_epoch_[t]) {
            continue;
        }
        pool_.release(freeze_slot(t));
        unfreeze_wait_ = uint8_t(unfreeze_wait_ & ~bit);
    }

    // A take the audio side refused (the looper took the buffer in the same block, say): no
    // take is coming, so stop waiting for one and say why, rather than showing WAIT until the
    // track is unfrozen by hand.
    if (freeze_track_ >= 0 &&
        engine_->status().freeze_refused.load(std::memory_order_relaxed) != freeze_refused_seen_) {
        freeze_track_ = -1;
        freeze_error_ = "FREEZE REFUSED";
        return false;
    }

    // A finished freeze take: into the slot, and the engine told. The copy is one pass over at
    // most 2.9 MiB (30 s of mono int16), a few milliseconds of DDR, done once per freeze.
    if (freeze_track_ < 0 ||
        engine_->status().resample_state.load(std::memory_order_relaxed) != 3u) {
        return false;
    }
    const int track = freeze_track_;
    freeze_track_ = -1;
    const Capture &take = engine_->capture();
    const uint32_t frames = take.frames();
    const uint32_t bytes = frames * uint32_t(sizeof(int16_t));
    const int slot = freeze_slot(track);
    if (take.channels() != 1 || frames == 0u || frames > 0xFFFFFFu ||
        !pool_.reserve(slot, bytes)) {
        freeze_error_ = "NO ROOM";
        post(Cmd::resample_release);
        return true;
    }
    const SampleSlot &where = pool_.slot(slot);
    int16_t *to = region_ + where.offset / sizeof(int16_t);
    const int16_t *from = take.data();
    for (uint32_t i = 0; i < frames; i++) to[i] = from[i];
    pool_.finish(slot, frames, 1, uint32_t(ENAGA_SAMPLE_RATE));

    Command set;
    set.cmd = Cmd::freeze_set;
    set.track = uint8_t(track);
    set.a = freeze_bars_;
    set.c = uint8_t(frames);
    set.d = uint8_t(frames >> 8);
    set.e = uint8_t(frames >> 16);
    set.u = where.offset;
    post(set);
    post(Cmd::resample_release);
    return true;
}

// --- the sample browser ---------------------------------------------------

bool Storage::folder_exists(const char *path) const {
    if (!card_usable_) return false;
    enaga::files::Info info;
    return enaga::files::info(path, info) == ENAGA_FILE_OK && (info.flags & ENAGA_FILE_FOLDER) != 0;
}

bool Storage::request_browse(const char *folder) {
    if (folder == nullptr) return false;
    int n = 0;
    while (folder[n] != '\0') n++;
    if (n + 1 > SampleList::kFolderBytes) return false;
    for (int i = 0; i <= n; i++) browse_wanted_[i] = folder[i];
    browse_error_ = nullptr;
    if (list_in_flight_) {
        browse_pending_ = true;            // started by list_done, after the one in flight
        return true;
    }
    browse_pending_ = true;
    start_browse();
    return true;
}

void Storage::start_browse() {
    browse_pending_ = false;
    if (browse_wanted_[0] == '\0') {
        // The virtual root needs no card; POOL and SMPLTREK join it when the card has the stock
        // firmware's /SmplTrek folder (one immediate files::info, milliseconds).
        list_.open_root(folder_exists("/SmplTrek"));
        // For the emulator scenario: 4 on a card with /SmplTrek, else 2.
        enaga::log(list_.count() > 2 ? "browse root 4" : "browse root 2");
        return;
    }
    if (!list_.open(browse_wanted_)) {
        list_.open_root(folder_exists("/SmplTrek"));
        browse_error_ = "BAD PATH";
        return;
    }
    if (!card_usable_) {
        browse_error_ = "NO CARD";
        return;
    }
    list_in_flight_ = true;
    const int32_t queued =
        enaga::files::list<&Storage::list_entry, &Storage::list_done>(list_.folder(), this);
    if (queued < 0) {
        list_in_flight_ = false;
        browse_error_ = enaga::files::error_name(queued);
    }
}

void Storage::list_entry(const enaga::files::Info &entry) {
    // Main context, from inside Enaga's list request. The table is the one start_browse opened:
    // only one listing is ever in flight, so these entries cannot belong to another folder.
    list_.add(entry.name, (entry.flags & ENAGA_FILE_FOLDER) != 0u);
}

void Storage::list_done(int32_t result) {
    list_in_flight_ = false;
    if (result < 0) browse_error_ = enaga::files::error_name(result);
    // For tests/scenario_samples.sh, which can only read the log.
    enaga::log(result < 0 ? "browse failed" : "browse listed");
    if (browse_pending_) start_browse();
}

bool Storage::request_sample_file(int track, const char *path) {
    if (track < 0 || track >= kTracks || path == nullptr || path[0] == '\0') return false;
    int n = 0;
    while (path[n] != '\0') n++;
    if (n + 1 > kSamplePathBytes) return false;
    // The sample paths are the main context's (app/engine.cpp, Cmd::kit_recall); the audio side
    // never reads them. The loader reads this path when the track's swap starts, so a second
    // pick while the first is still loading simply loads again with the newer one.
    char *out = engine_->project_for_load().kit.tracks[track].sample;
    for (int i = 0; i <= n; i++) out[i] = path[i];
    kit_dirty_ = true;
    return request_sample(track);
}

bool Storage::request_sample(int track) {
    if (track < 0 || track >= kTracks) return false;
    if (engine_->project().kit.tracks[track].sample[0] == '\0') return false;
    sample_queue_ = uint8_t(sample_queue_ | (1u << track));
    return true;
}

void Storage::request_all_samples() {
    for (int t = 0; t < kTracks; t++) request_sample(t);
}

void Storage::start_next_sample() {
    for (int t = 0; t < kTracks; t++) {
        if ((sample_queue_ & (1u << t)) == 0) continue;
        sample_queue_ = uint8_t(sample_queue_ & ~(1u << t));
        sample_track_ = t;

        // Step one: take the sample away from the track and remember the epoch we are waiting to
        // see change. Reading it before the post is what makes the wait meaningful -- the audio
        // side bumps it when it has stopped reading, so a change from THIS value is the proof.
        epoch_before_ = engine_->status().sample_epoch[t].load(std::memory_order_relaxed);
        epoch_waited_ = 0;
        Command command;
        command.cmd = Cmd::clear_sample;
        command.track = uint8_t(t);
        post(command);
        begin_job(Job::sample_epoch);
        return;
    }
    sample_queue_ = 0;
}

void Storage::step_sample_epoch() {
    const int track = sample_track_;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }

    const uint8_t now = engine_->status().sample_epoch[track].load(std::memory_order_relaxed);
    if (now == epoch_before_) {
        // Not yet. A wrap is harmless -- this compares against the value it saw rather than
        // counting -- but a track whose epoch never moves must not hang the loader for ever.
        if (++epoch_waited_ < kEpochPasses) return;
        // Giving the range back unconfirmed is safe for one reason only: the `clear_sample`
        // posted above is still in the ring (or in `backlog_`, if the ring was full), and
        // service() starts nothing while either holds anything. So the audio side kills the voice before anything can write here again.
        // tests/test_storage.cpp pins it; drop that gate and this line becomes the bug.
        pool_.fail(track);
        sample_track_ = -1;
        finish_job("NO SWAP");
        return;
    }

    // Confirmed: the audio side has stopped reading, so the memory is ours. Only now. Its
    // stream too, if it played one: clear_sample stopped the reading before the epoch moved.
    end_stream(track);
    pool_.release(track);

    // A track the kit names no file for is finished here: back on the placeholder (Cmd::clear_sample),
    // its memory given back. That is how a project load takes the previous project's sample off a track.
    if (engine_->project().kit.tracks[track].sample[0] == '\0') {
        sample_track_ = -1;
        finish_job(nullptr);
        return;
    }

    char path[ENAGA_FILE_PATH_MAX + 1];
    char *out = path;
    const char *end = path + sizeof path - 1;
    append_text(out, engine_->project().kit.tracks[track].sample, end);
    if (out >= end) {
        pool_.fail(track);
        sample_track_ = -1;
        finish_job("BAD PATH");
        return;
    }
    *out = '\0';

    // A file bigger than raw_ is read a chunk at a time (app/storage.hpp, "a file bigger than
    // raw_"). Asked first, because files::load would only refuse it as TOO BIG after reading
    // nothing. An info that fails falls through to the load, which reports the same error.
    src_done_ = 0;
    out_done_ = 0;
    src_frames_ = 0;
    out_frames_ = 0;
    chunked_ = false;
    {
        enaga::files::Info info{};
        if (enaga::files::info(path, info) == ENAGA_FILE_OK && info.size > kRawBytes) {
            if (start_chunked(path, info.size)) begin_job(Job::sample_read);
            return;
        }
    }

    // Background, with the callback in the main context: docs/SPEC.md names files::load for
    // exactly this. `raw_` belongs to Enaga until the callback, which is why nothing else may
    // start a read while `read_in_flight_` is set.
    read_in_flight_ = true;
    read_done_ = false;
    const int32_t queued = enaga::files::load<&Storage::sample_loaded>(path, raw_, kRawBytes, this);
    if (queued < 0) {
        read_in_flight_ = false;
        pool_.fail(track);
        sample_track_ = -1;
        finish_job(enaga::files::error_name(queued));
        return;
    }
    begin_job(Job::sample_read);
    src_done_ = 0;
    out_done_ = 0;
    src_frames_ = 0;
    out_frames_ = 0;
}

void Storage::sample_loaded(int32_t result) {
    // Main context, between update() calls. It records and returns: the work belongs to
    // service(), which is where the slicing happens.
    read_in_flight_ = false;
    read_result_ = result;
    read_done_ = true;
}

void Storage::step_sample_convert() {
    const int track = sample_track_;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }

    // The first slice parses the header and reserves the room. Enaga parses WAV headers
    // (<enaga/wav.hpp>); what it does not do is convert formats or resample, which is why
    // core/pcm exists.
    if (src_frames_ == 0) {
        const uint32_t got = uint32_t(read_result_);
        if (!enaga::wav::parse(raw_, got, format_)) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("NOT A WAV");
            return;
        }
        encoding_ = pcm_format_of(format_.encoding, format_.bits);
        const uint32_t frame_bytes = format_.frame_bytes();
        if (encoding_ == PcmFormat::unknown || frame_bytes == 0 ||
            format_.data_offset >= got) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("BAD FORMAT");
            return;
        }
        // dsp/sample.cpp plays mono and stereo. More channels are a refusable file rather than a
        // guess at which two of them the user meant.
        if (format_.channels > 2) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("TOO MANY CH");
            return;
        }
        // From the data chunk's OWN length, not from "everything after the header". A WAV may
        // carry chunks after its data -- LIST, cue and id3 are all common -- and those bytes are
        // not audio: played, they are a burst of noise on the end of the sample. <enaga/wav.hpp>
        // notes that a header still being written can report 0, which is the one case where the
        // rest of the file is the better answer, so that is the fallback and not the rule. A
        // length longer than what was read means a truncated file, and takes the same fallback.
        const uint32_t rest = got - format_.data_offset;
        const uint32_t data_bytes =
            (format_.data_bytes == 0 || format_.data_bytes > rest) ? rest : format_.data_bytes;
        src_frames_ = data_bytes / frame_bytes;
        if (src_frames_ == 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("EMPTY");
            return;
        }

        // Inactive unless this is a downsample; core/pcm.hpp says why.
        antialias_ = Antialias::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
        if (format_.rate == uint32_t(ENAGA_SAMPLE_RATE)) {
            out_frames_ = src_frames_;
        } else {
            // A resample reads the whole converted source, so it has to fit in src_. That buffer
            // is sized from docs/SPEC.md's own limit -- streams take over above 30 s -- counted
            // in the FILE's frames at up to 96 kHz (app/storage.hpp), so a file that does not
            // fit is longer than 30 s of itself and one the spec says should have been a stream.
            if (src_frames_ > kSrcFrames) {
                pool_.fail(track);
                sample_track_ = -1;
                finish_job("TOO LONG");
                return;
            }
            cursor_ = Resampler::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
            out_frames_ = cursor_.output_frames(src_frames_);
        }

        const uint32_t bytes = out_frames_ * uint32_t(format_.channels) * uint32_t(sizeof(int16_t));
        if (out_frames_ == 0 || !pool_.reserve(track, bytes)) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("NO ROOM");
            return;
        }
        out_offset_ = pool_.slot(track).offset;
        // The audio side refuses an odd offset, because a misaligned 16-bit read on the C674x
        // silently returns the wrong address. Every reservation is an even number of bytes, so
        // this cannot happen -- and if it ever does, it fails here rather than there.
        if ((out_offset_ % sizeof(int16_t)) != 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("MISALIGNED");
            return;
        }
    }

    const int channels = int(format_.channels);
    const uint32_t frame_bytes = format_.frame_bytes();
    const bool resampling = format_.rate != uint32_t(ENAGA_SAMPLE_RATE);

    // 48 kHz goes straight into the region; anything else into src_, which the resample below
    // reads from.
    int16_t *destination = resampling ? src_ : region_ + out_offset_ / sizeof(int16_t);

    const uint32_t left = src_frames_ - src_done_;
    const uint32_t want = left < kSliceFrames ? left : kSliceFrames;
    const uint32_t at = format_.data_offset + src_done_ * frame_bytes;
    const uint32_t available = uint32_t(read_result_) - at;
    const uint32_t did = pcm_to_s16(raw_ + at, available, encoding_, channels,
                                    destination + size_t(src_done_) * size_t(channels), want);
    // A downsample's source is low-passed as it arrives, slice by slice, so the resample below
    // has nothing above the new Nyquist to fold back down.
    if (resampling) pcm_antialias(destination + size_t(src_done_) * size_t(channels), did, channels,
                                  antialias_);
    // At 48 kHz this IS the sample, so its slice points are found as it lands (core/onsets.hpp).
    if (!resampling) {
        onsets_.feed(destination + size_t(src_done_) * size_t(channels), src_done_, did, channels);
    }
    src_done_ += did;

    if (did == 0 || src_done_ >= src_frames_) {
        // A short convert means the file held fewer whole frames than its header implied, which
        // core/pcm handles by converting as much as is actually there.
        if (did == 0) src_frames_ = src_done_;
        if (src_frames_ == 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("EMPTY");
            return;
        }
        if (resampling) {
            begin_job(Job::sample_resample);
            // begin_job clears the error, which is right: nothing has failed.
            out_done_ = 0;
            cursor_ = Resampler::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
            return;
        }
        out_done_ = src_done_;
        hand_sample_over();
    }
}

bool Storage::start_chunked(const char *path, uint32_t file_bytes) {
    const int track = sample_track_;
    const int32_t opened = chunk_file_.open(path, ENAGA_FILE_READ);
    if (opened < 0) {
        pool_.fail(track);
        sample_track_ = -1;
        finish_job(enaga::files::error_name(opened));
        return false;
    }
    chunked_ = true;
    chunk_file_bytes_ = file_bytes;
    chunk_file_pos_ = 0;
    chunk_have_ = 0;
    chunk_pos_ = 0;
    chunk_carry_ = 0;
    if (!read_next_chunk()) {
        pool_.fail(track);
        sample_track_ = -1;
        finish_job("READ FAILED");
        return false;
    }
    return true;
}

// Queue the next chunk into raw_, after the `chunk_carry_` bytes already at its start. False when
// the read could not be queued; the caller fails the job.
bool Storage::read_next_chunk() {
    const uint32_t left = chunk_file_bytes_ - chunk_file_pos_;
    const uint32_t room = kChunkBytes - chunk_carry_;
    const uint32_t want = left < room ? left : room;
    read_in_flight_ = true;
    read_done_ = false;
    const int32_t queued =
        chunk_file_.read<&Storage::sample_loaded>(raw_ + chunk_carry_, want, this);
    if (queued < 0) {
        read_in_flight_ = false;
        return false;
    }
    return true;
}

void Storage::step_sample_chunk() {
    const int track = sample_track_;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }

    // The first chunk: the header, the format and the reservation, as step_sample_convert does
    // for a whole file. What differs is that the file's length comes from its size, not from
    // what one read returned.
    if (src_frames_ == 0 && src_done_ == 0 && chunk_file_pos_ == chunk_have_) {
        if (!enaga::wav::parse(raw_, chunk_have_, format_)) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("NOT A WAV");
            return;
        }
        encoding_ = pcm_format_of(format_.encoding, format_.bits);
        const uint32_t frame_bytes = format_.frame_bytes();
        if (encoding_ == PcmFormat::unknown || frame_bytes == 0 ||
            format_.data_offset >= chunk_have_) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("BAD FORMAT");
            return;
        }
        if (format_.channels > 2) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("TOO MANY CH");
            return;
        }
        // The data chunk's own length, or the rest of the file when the header says 0 or more
        // than is there -- the same rule a whole file follows.
        const uint32_t rest = chunk_file_bytes_ - format_.data_offset;
        const uint32_t data_bytes =
            (format_.data_bytes == 0 || format_.data_bytes > rest) ? rest : format_.data_bytes;
        src_frames_ = data_bytes / frame_bytes;
        if (src_frames_ == 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("EMPTY");
            return;
        }
        antialias_ = Antialias::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
        // Whole if it fits, streamed if it does not (app/storage.hpp, "streaming"). `whole` is
        // why it cannot be loaded whole, and what the screen says if it cannot stream either.
        const char *whole = nullptr;
        if (format_.rate == uint32_t(ENAGA_SAMPLE_RATE)) {
            out_frames_ = src_frames_;
        } else if (src_frames_ > kSrcFrames) {
            whole = "TOO LONG";
        } else {
            cursor_ = Resampler::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
            out_frames_ = cursor_.output_frames(src_frames_);
        }
        // Cmd::set_sample carries 24 bits of frames: 349 s at 48 kHz. Decided before anything is
        // reserved, not after minutes of reading.
        if (whole == nullptr && out_frames_ > 0xFFFFFFu) whole = "TOO LONG";
        if (whole == nullptr &&
            !pool_.reserve(track, out_frames_ * uint32_t(format_.channels) *
                                      uint32_t(sizeof(int16_t)))) {
            whole = "NO ROOM";
        }
        if (whole != nullptr) {
            const char *why = begin_stream(track, whole);
            if (why != nullptr) {
                pool_.fail(track);
                sample_track_ = -1;
                finish_job(why);
                return;
            }
        }
        out_offset_ = pool_.slot(track).offset;
        if ((out_offset_ % sizeof(int16_t)) != 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("MISALIGNED");
            return;
        }
        chunk_pos_ = format_.data_offset;
    }

    const int channels = int(format_.channels);
    const uint32_t frame_bytes = format_.frame_bytes();
    // A stream's head is not resampled: the file plays at its own rate (begin_stream).
    const bool resampling = !streaming_ && format_.rate != uint32_t(ENAGA_SAMPLE_RATE);
    int16_t *destination = resampling ? src_ : region_ + out_offset_ / sizeof(int16_t);

    // One slice of this chunk.
    const uint32_t in_chunk = (chunk_have_ - chunk_pos_) / frame_bytes;
    const uint32_t left = src_frames_ - src_done_;
    uint32_t want = in_chunk < kSliceFrames ? in_chunk : kSliceFrames;
    if (want > left) want = left;
    const uint32_t did =
        want == 0 ? 0
                  : pcm_to_s16(raw_ + chunk_pos_, want * frame_bytes, encoding_, channels,
                               destination + size_t(src_done_) * size_t(channels), want);
    if (resampling && did != 0) {
        pcm_antialias(destination + size_t(src_done_) * size_t(channels), did, channels,
                      antialias_);
    }
    if (!resampling && !streaming_) {
        onsets_.feed(destination + size_t(src_done_) * size_t(channels), src_done_, did, channels);
    }
    src_done_ += did;
    chunk_pos_ += did * frame_bytes;

    const bool file_done = chunk_file_pos_ >= chunk_file_bytes_;
    const bool chunk_spent = chunk_have_ - chunk_pos_ < frame_bytes;
    if (src_done_ >= src_frames_ || (chunk_spent && file_done)) {
        // All of it, or all the file holds (shorter than its header said: what is there plays).
        src_frames_ = src_done_;
        if (chunk_file_.is_open()) chunk_file_.close();
        chunked_ = false;
        if (src_frames_ == 0) {
            pool_.fail(track);
            sample_track_ = -1;
            finish_job("EMPTY");
            return;
        }
        if (streaming_) {
            hand_stream_over();
            return;
        }
        if (resampling) {
            begin_job(Job::sample_resample);
            out_done_ = 0;
            cursor_ = Resampler::make(format_.rate, uint32_t(ENAGA_SAMPLE_RATE));
            return;
        }
        out_done_ = src_done_;
        hand_sample_over();
        return;
    }
    if (!chunk_spent) return;                  // more of this chunk on the next pass

    // The chunk is used up: keep the part of a frame that straddles its end, read the next.
    chunk_carry_ = chunk_have_ - chunk_pos_;
    for (uint32_t i = 0; i < chunk_carry_; i++) raw_[i] = raw_[chunk_pos_ + i];
    if (!read_next_chunk()) {
        pool_.fail(track);
        sample_track_ = -1;
        finish_job("READ FAILED");
        return;
    }
    begin_job(Job::sample_read);
}

void Storage::step_take_write() {
    // In chunks, one write in flight: the first pass names the file, puts the header in front
    // and queues the first chunk; each later pass waits for the last write and queues the next.
    if (take_file_.is_open()) {
        if (!take_write_done_) return;             // Enaga is still working that chunk
        take_write_done_ = false;
        if (take_write_result_ < 0) {
            end_take_write(enaga::files::error_name(take_write_result_));
            return;
        }
        take_written_ += uint32_t(take_write_result_);
        if (take_written_ < take_bytes_) {
            if (!write_next_take_chunk()) end_take_write("WRITE FAILED");
            return;
        }
        const int32_t closed = take_file_.close();
        if (closed < 0) {
            end_take_write(enaga::files::error_name(closed));
            return;
        }
        // The buffer goes back to the audio side only now, with the last byte on the card.
        post(Cmd::resample_release);
        finish_take_write();
        return;
    }

    const Capture &take = engine_->capture();
    const uint32_t frames = take.frames();
    if (frames == 0u || take.data() == nullptr) {
        finish_job("NO TAKE");
        return;
    }

    // /RECORD/STRATA_<n>.WAV, numbered from the settings so a take never overwrites an earlier
    // one across restarts -- the same counter discipline `request_new` uses for project names.
    const int32_t next = settings_.get(kKeyTakeSeq, 0) + 1;
    settings_.set(kKeyTakeSeq, next);
    {
        char *out = take_name_;
        const char *end = take_name_ + kTakeNameBytes - 1;
        append_text(out, "STRATA_", end);
        append_number(out, uint32_t(next), end);
        append_text(out, ".WAV", end);
        *out = '\0';
    }
    {
        char *out = take_path_;
        const char *end = take_path_ + sizeof take_path_ - 1;
        append_text(out, "/RECORD/", end);
        append_text(out, take_name_, end);
        *out = '\0';
    }

    const int32_t made = enaga::files::make_folder("/RECORD");
    if (made < 0) {
        end_take_write(enaga::files::error_name(made));
        return;
    }

    // No copy. dsp/capture.hpp reserves the 44 header bytes IN FRONT of the audio for exactly
    // this: the header goes into them and the file is that one buffer from start to end,
    // rather than a second 5.76 MiB buffer. The take is already interleaved int16 (stereo, or
    // mono for one track), so there is no conversion pass over it in the UI thread at all.
    enaga::wav::Format format{};
    format.encoding = enaga::wav::pcm;
    format.channels = uint16_t(engine_->capture().channels());
    format.rate = uint32_t(ENAGA_SAMPLE_RATE);
    format.bits = 16;

    Capture &writable = engine_->capture_for_write();
    enaga::wav::header(writable.file_base(), format, writable.data_bytes());
    take_bytes_ = writable.file_bytes();
    take_written_ = 0;
    take_write_done_ = false;
    const int32_t opened = take_file_.open(take_path_, ENAGA_FILE_WRITE);
    if (opened < 0) {
        end_take_write(enaga::files::error_name(opened));
        return;
    }
    if (!write_next_take_chunk()) end_take_write("WRITE FAILED");
}

bool Storage::write_next_take_chunk() {
    const uint32_t left = take_bytes_ - take_written_;
    const uint32_t want = left < kTakeChunkBytes ? left : kTakeChunkBytes;
    const uint8_t *from =
        static_cast<const uint8_t *>(engine_->capture_for_write().file_base()) + take_written_;
    take_write_in_flight_ = true;
    take_write_done_ = false;
    const int32_t queued =
        take_file_.write<&Storage::take_chunk_written>(from, want, this);
    if (queued < 0) {
        take_write_in_flight_ = false;
        return false;
    }
    return true;
}

void Storage::take_chunk_written(int32_t result) {
    take_write_in_flight_ = false;
    take_write_result_ = result;
    // A short write that made no progress would wait for ever; call it the card's fault.
    if (result == 0) take_write_result_ = ENAGA_FILE_CARD_ERROR;
    take_write_done_ = true;
}

// A failed take: the half-written file is closed, and the buffer goes back to the audio side
// either way -- a failed write must not leave a take wedged in it, because nothing else can arm
// the next one (dsp/capture.hpp).
void Storage::end_take_write(const char *error) {
    if (take_file_.is_open()) take_file_.close();
    take_write_done_ = false;
    post(Cmd::resample_release);
    take_track_ = -1;
    finish_job(error);
}

void Storage::finish_take_write() {
    // And into a track, if that is what was asked for. docs/SPEC.md wants the whole round trip
    // "in one action", so the kit's path is set and the ordinary sample loader does the rest --
    // the same swap any other sample goes through, epoch handshake included.
    if (take_track_ >= 0 && take_track_ < kTracks) {
        char *out = engine_->project_for_load().kit.tracks[take_track_].sample;
        const char *end = out + kSamplePathBytes - 1;
        append_text(out, take_path_, end);
        *out = '\0';
        kit_dirty_ = true;
        const int track = take_track_;
        take_track_ = -1;
        finish_job(nullptr);
        request_sample(track);
        return;
    }
    take_track_ = -1;
    finish_job(nullptr);
}

void Storage::step_sample_resample() {
    const int track = sample_track_;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }

    const int channels = int(format_.channels);
    int16_t *destination = region_ + out_offset_ / sizeof(int16_t);
    const uint32_t left = out_frames_ > out_done_ ? out_frames_ - out_done_ : 0;
    const uint32_t want = left < kSliceFrames ? left : kSliceFrames;

    const uint32_t did = want == 0 ? 0
                                   : pcm_resample(src_, src_frames_, channels, cursor_,
                                                  destination + size_t(out_done_) * size_t(channels),
                                                  want);
    onsets_.feed(destination + size_t(out_done_) * size_t(channels), out_done_, did, channels);
    out_done_ += did;
    // 0 means the cursor has passed the end of the source, which is the end of the job as well as
    // the end of the slice.
    if (did == 0 || out_done_ >= out_frames_) hand_sample_over();
}

void Storage::hand_sample_over() {
    const int track = sample_track_;
    sample_track_ = -1;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }

    const int channels = int(format_.channels);
    if (out_done_ == 0) {
        pool_.fail(track);
        finish_job("EMPTY");
        return;
    }
    // `set_sample` carries the frame count in 24 bits (app/commands.hpp), which is 349 s of mono
    // at 48 kHz -- far past anything loaded whole into RAM. Checked rather than assumed, because
    // a silent truncation here would play a fraction of the file and look like a decode bug.
    if (out_done_ > 0xFFFFFFu) {
        pool_.fail(track);
        finish_job("TOO LONG");
        return;
    }
    if (!pool_.finish(track, out_done_, channels, uint32_t(ENAGA_SAMPLE_RATE))) {
        pool_.fail(track);
        finish_job("POOL REFUSED");
        return;
    }

    // Its slice points, for SLC AUTO, through the inbox the command's apply reads. A detector
    // that did not see exactly these frames, in order, gives none rather than wrong ones.
    onsets_.finish(out_done_, engine_->slices_for_load(track));

    // The handover: an offset, a frame count and a channel count, and nothing else crosses. The
    // audio side checks all three against the region before it forms a pointer.
    Command command;
    command.cmd = Cmd::set_sample;
    command.track = uint8_t(track);
    command.a = uint8_t(channels);
    command.u = out_offset_;
    command.c = uint8_t(out_done_ & 0xFFu);
    command.d = uint8_t((out_done_ >> 8) & 0xFFu);
    command.e = uint8_t((out_done_ >> 16) & 0xFFu);
    post(command);

    finish_job(nullptr);
}

// --- streaming ----------------------------------------------------------------
//
// app/storage.hpp, "streaming: a file too big for the pool", has the protocol.

const char *Storage::begin_stream(int track, const char *whole) {
    const uint32_t rate = format_.rate;
    // The Sampler plays a stream at its own rate, as a step. Past the rates the loader takes at
    // all, refused as before.
    if (rate < 8000u || rate > kMaxSampleSourceRate) return whole;
    const uint32_t head = rate * kStreamHeadSeconds;
    // A file no longer than its head would be a stream of nothing: it simply does not fit.
    if (src_frames_ <= head) return whole;
    if (src_frames_ > Sampler::kMaxStreamFrames) return "TOO LONG";

    int ring = -1;
    for (int r = 0; r < kStreamRings && ring < 0; r++) {
        bool used = false;
        for (int t = 0; t < kTracks; t++) used = used || (streams_[t].on && streams_[t].ring == r);
        if (!used) ring = r;
    }
    if (ring < 0) return "STREAMS FULL";

    // The head comes out of the first chunk, which is 2 MiB: two seconds of even 96 kHz stereo
    // float is 1.5 MB. Checked anyway, since a header may put its data anywhere (and 192 kHz
    // float would not fit: such a file loads whole or is refused, as before streaming).
    const uint32_t frame_bytes = format_.frame_bytes();
    if (format_.data_offset + head * frame_bytes > chunk_have_) return whole;
    if (!pool_.reserve(track, head * uint32_t(format_.channels) * uint32_t(sizeof(int16_t)))) {
        return "NO ROOM";
    }
    streaming_ = true;
    stream_total_ = src_frames_;
    stream_ring_ = int8_t(ring);
    src_frames_ = head;
    out_frames_ = head;
    return nullptr;
}

void Storage::hand_stream_over() {
    const int track = sample_track_;
    sample_track_ = -1;
    if (track < 0) {
        finish_job(nullptr);
        return;
    }
    const int channels = int(format_.channels);
    const uint32_t head = src_done_;
    if (!pool_.finish(track, head, channels, format_.rate)) {
        pool_.fail(track);
        finish_job("POOL REFUSED");
        return;
    }

    // The inbox first, then the plan, then the post: the audio side reads the inbox when it
    // applies the command, and the plan starts from the stream_want value BEFORE that apply, so
    // the bump the apply makes is the one service_streams answers.
    Engine::StreamInfo &info = engine_->stream_for_load(track);
    info.head_offset = out_offset_;
    info.head_frames = head;
    info.total_frames = stream_total_;
    info.rate = format_.rate;
    info.channels = uint8_t(channels);
    info.encoding = uint8_t(encoding_);
    info.frame_bytes = uint8_t(format_.frame_bytes());

    StreamPlan &plan = streams_[track];
    plan = StreamPlan{};
    plan.on = true;
    plan.ring = stream_ring_;
    plan.offset = format_.data_offset + head * format_.frame_bytes();
    plan.seq = engine_->status().stream_want[track].load(std::memory_order_acquire);
    const char *path = engine_->project().kit.tracks[track].sample;
    for (int i = 0; i < kSamplePathBytes; i++) {
        plan.path[i] = path[i];
        if (path[i] == '\0') break;
    }
    plan.path[kSamplePathBytes - 1] = '\0';

    Command command;
    command.cmd = Cmd::set_stream;
    command.track = uint8_t(track);
    post(command);
    finish_job(nullptr);
}

void Storage::end_stream(int track) {
    StreamPlan &plan = streams_[track];
    if (!plan.on) return;
    engine_->stream(track).close();
    plan = StreamPlan{};
}

void Storage::service_streams() {
    for (int t = 0; t < kTracks; t++) {
        StreamPlan &plan = streams_[t];
        if (!plan.on) continue;
        enaga::files::Stream &stream = engine_->stream(t);
        const uint16_t want = engine_->status().stream_want[t].load(std::memory_order_acquire);
        if (want != plan.seq) {
            // The audio side stopped reading before it bumped `want`, so the stream is ours.
            stream.close();
            plan.seq = want;
            plan.attached = false;
            plan.failed = false;
            const int32_t opened =
                card_usable_ ? stream.open(plan.path, plan.offset, rings_[plan.ring],
                                           kStreamRingBytes)
                             : int32_t(ENAGA_FILE_NO_CARD);
            if (opened < 0) {
                plan.failed = true;
                error_ = "STREAM FAILED";
                continue;
            }
        }
        if (plan.attached || plan.failed || !stream.is_open()) continue;
        const enaga::files::Stream::Status status = stream.status();
        if (status.state < 0) {
            plan.failed = true;
            error_ = "STREAM FAILED";
            continue;
        }
        if (status.state < ENAGA_STREAM_RUNNING) continue;     // still filling
        Command command;
        command.cmd = Cmd::stream_attach;
        command.track = uint8_t(t);
        command.a = uint8_t(want & 0xFFu);
        command.b = uint8_t(want >> 8);
        post(command);
        plan.attached = true;
    }
}

void Storage::save_for_shutdown() {
    // App::stop has 3 s including saves, and destructors never run on this platform, so this is
    // the last chance to keep the user's work. Blocking is allowed here; it is the one place in
    // this file that waits.
    if (!card_usable_) return;

    post(Cmd::kit_store);

    // Drain the ring. The protocol wants an empty ring before a save, and nothing else will
    // empty it: Enaga removes the audio callback BEFORE it calls stop() (its apps_unload), so
    // the spin on the audio side this used to be ran out its 100 ms and saved without the
    // `kit_store` above -- the autosave's kit lost every knob turned since the last save. With
    // the audio side gone the main context applies the commands itself (Engine says why that is
    // safe), the backlog first.
    for (int i = 0; i < 64 && !flush_backlog(); i++) engine_->drain_commands_for_shutdown();
    engine_->drain_commands_for_shutdown();

    autosave_ = true;
    begin_job(Job::save);
    // Straight through rather than a slice per pass: there is no next pass.
    while (job_ == Job::save) step_save();
}

}  // namespace strata
