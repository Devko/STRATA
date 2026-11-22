// The fake Enaga host. tests/fakehost.hpp says why it exists and what it guarantees.
#include "fakehost.hpp"

#include <cstring>
#include <new>

namespace strata::test {
namespace {

bool same(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// Is the clock somebody else's? Under MIDI or SYNC the app can only follow (CLAUDE.md: "CLOCK
// SRC is the user's setting; start/stop/set_tempo return false under external sync"), so the
// transport calls are refused.
bool external(uint32_t source) {
    return source == ENAGA_CLOCK_MIDI || source == ENAGA_CLOCK_SYNC;
}

// Copy with a terminator, refusing rather than truncating: a fake that silently shortened a path
// would make two different files look like one.
bool copy_path(char *out, uint32_t max, const char *path) {
    const uint32_t n = uint32_t(std::strlen(path));
    if (n + 1u > max) return false;
    std::memcpy(out, path, n + 1u);
    return true;
}

}  // namespace

FakeHost &FakeHost::the() {
    static FakeHost host;
    return host;
}

FakeHost::FakeHost() {
    // The arena is 4 MiB and is taken once, here. `new[]` rather than a static array so the test
    // binary's bss does not carry it, and never freed, which is what the app does too.
    arena_ = new (std::nothrow) uint8_t[kArenaBytes];
    install();
    reset();
}

void FakeHost::install() {
    abi_ = enaga_host_t{};
    abi_.size = uint32_t(sizeof(enaga_host_t));

    abi_.log = t_log;
    abi_.ms = t_ms;
    abi_.fatal = t_fatal;
    abi_.text = t_text;
    abi_.audio_load = t_audio_load;
    abi_.audio_load_peak = t_audio_load_peak;
    abi_.led = t_led;

    abi_.file_open = t_file_open;
    abi_.file_close = t_file_close;
    abi_.file_size = t_file_size;
    abi_.file_seek = t_file_seek;
    abi_.file_info = t_file_info;
    abi_.file_remove = t_file_remove;
    abi_.file_make_folder = t_file_make_folder;
    abi_.file_read_all = t_file_read_all;
    abi_.file_save = t_file_save;
    abi_.file_read = t_file_read;
    abi_.file_write = t_file_write;
    abi_.file_load = t_file_load;
    abi_.file_list = t_file_list;
    abi_.file_data_folder = t_file_data_folder;

    abi_.stream_open = t_stream_open;
    abi_.stream_create = t_stream_create;
    abi_.stream_read = t_stream_read;
    abi_.stream_write = t_stream_write;
    abi_.stream_status = t_stream_status;
    abi_.stream_close = t_stream_close;

    abi_.open_menu = t_open_menu;
    abi_.set_busy = t_set_busy;
    abi_.speaker = t_speaker;
    abi_.headphones = t_headphones;
    abi_.device = t_device;
    abi_.input = t_input;
    abi_.midi_send = t_midi_send;
    abi_.midi_channel = t_midi_channel;

    abi_.clock_control = t_clock_control;
    abi_.clock_state = t_clock_state;
    abi_.clock_ticks = t_clock_ticks;
}

void FakeHost::reset() {
    clock_ = enaga_clock_state_t{};
    clock_.source = ENAGA_CLOCK_INTERNAL;
    clock_.tempo = 130.0f;            // the default beat's tempo, so a test that forgets is sane
    tick_count_ = 0;
    for (int i = 0; i < 8; i++) {
        control_[i] = 0;
        control_value_[i] = 0;
    }

    used_ = 0;
    count_ = 0;
    for (int i = 0; i < kMaxFiles; i++) files_[i] = Entry{};
    for (int i = 0; i < kHandles; i++) handles_[i] = Handle{};
    pending_ = 0;

    card_ = true;
    full_ = false;
    require_folders_ = false;
    read_error_ = false;
    in_audio_ = false;

    saves_ = 0;
    loads_ = 0;
    read_alls_ = 0;
    folders_made_ = 0;

    log_[0] = '\0';
    log_lines_ = 0;
    leds_ = 0;
    clear_text();
    audio_load_ = 0;
    device_ = 0;
    ms_ = 0;
}

// --- the clock ------------------------------------------------------------

void FakeHost::set_source(uint32_t source) { clock_.source = source; }
void FakeHost::set_tempo(float bpm) { clock_.tempo = bpm; }
void FakeHost::set_running(bool running) { clock_.running = running ? 1u : 0u; }

void FakeHost::set_position(uint32_t ticks, float fraction) {
    clock_.position = ticks;
    clock_.fraction = fraction;
}

void FakeHost::deliver_tick(uint32_t tick, uint32_t frame) {
    if (tick_count_ >= kMaxTicks) return;
    ticks_[tick_count_].tick = tick;
    ticks_[tick_count_].frame = frame;
    tick_count_++;
}

void FakeHost::deliver_ticks(uint32_t first, uint32_t count, uint32_t step, uint32_t frames) {
    for (uint32_t i = 0; i < count; i++) {
        // Spread them across the block. Integer arithmetic, so the last one lands inside it.
        const uint32_t frame = count <= 1u ? 0u : (i * (frames - 1u)) / (count - 1u);
        deliver_tick(first + i * step, frame);
    }
}

int FakeHost::control_calls(uint32_t command) const {
    return command < 8u ? control_[command] : 0;
}

int32_t FakeHost::control_value(uint32_t command) const {
    return command < 8u ? control_value_[command] : 0;
}

// --- the card -------------------------------------------------------------

void FakeHost::set_card(bool present) { card_ = present; }
void FakeHost::set_full(bool full) { full_ = full; }
void FakeHost::set_read_error(bool fail) { read_error_ = fail; }
void FakeHost::set_audio_context(bool in_audio) { in_audio_ = in_audio; }

int FakeHost::find(const char *path) const {
    for (int i = 0; i < count_; i++) {
        if (same(files_[i].path, path)) return i;
    }
    return -1;
}

const char *FakeHost::file_name(int index) const {
    return index >= 0 && index < count_ ? files_[index].path : "";
}

// The ABI's path rules, kept because a fake that accepts more than the real host tests nothing:
// "not absolute, '..' or '.' in it, an empty part, too long" is ENAGA_FILE_BAD_PATH.
int32_t FakeHost::path_check(const char *path) const {
    if (path == nullptr || path[0] != '/') return ENAGA_FILE_BAD_PATH;
    const uint32_t n = uint32_t(std::strlen(path));
    if (n == 0u || n > ENAGA_FILE_PATH_MAX) return ENAGA_FILE_BAD_PATH;
    uint32_t part = 0;
    for (uint32_t i = 1; i <= n; i++) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            if (part == 0u && !(c == '\0' && i == n && n == 1u)) return ENAGA_FILE_BAD_PATH;
            const char *start = path + i - part;
            if (part == 1u && start[0] == '.') return ENAGA_FILE_BAD_PATH;
            if (part == 2u && start[0] == '.' && start[1] == '.') return ENAGA_FILE_BAD_PATH;
            part = 0;
        } else {
            part++;
        }
    }
    return ENAGA_FILE_OK;
}

int32_t FakeHost::card_check() const {
    if (in_audio_) return ENAGA_FILE_WRONG_CONTEXT;   // the ABI: every file call is main context
    if (!card_) return ENAGA_FILE_NO_CARD;
    return ENAGA_FILE_OK;
}

int FakeHost::add(const char *path, uint32_t bytes, bool folder) {
    if (count_ >= kMaxFiles || arena_ == nullptr) return -1;
    if (used_ + bytes > kArenaBytes) return -1;
    Entry &e = files_[count_];
    if (!copy_path(e.path, kPathMax, path)) return -1;
    e.offset = used_;
    e.bytes = bytes;
    e.capacity = bytes;
    e.folder = folder;
    used_ += bytes;
    return count_++;
}

bool FakeHost::write_file(const char *path, const void *data, uint32_t bytes) {
    int index = find(path);
    if (index >= 0) {
        Entry &e = files_[index];
        if (bytes <= e.capacity) {
            // In place, so a rewrite does not leak the arena. The capacity stays the high-water
            // mark, which is what a real file system does with a truncating write too.
            if (bytes > 0u) std::memcpy(arena_ + e.offset, data, bytes);
            e.bytes = bytes;
            return true;
        }
        // Too big for its old slot: give it a new one and leave the old bytes stranded. The
        // arena is 4 MiB and a test writes a project or two, so that is cheaper than compaction.
        if (used_ + bytes > kArenaBytes) return false;
        e.offset = used_;
        e.capacity = bytes;
        e.bytes = bytes;
        used_ += bytes;
        if (bytes > 0u) std::memcpy(arena_ + e.offset, data, bytes);
        return true;
    }
    index = add(path, bytes, false);
    if (index < 0) return false;
    if (bytes > 0u) std::memcpy(arena_ + files_[index].offset, data, bytes);
    return true;
}

int32_t FakeHost::read_file(const char *path, void *out, uint32_t max) const {
    const int index = find(path);
    if (index < 0) return ENAGA_FILE_NOT_FOUND;
    const Entry &e = files_[index];
    if (e.bytes > max) return ENAGA_FILE_TOO_BIG;
    if (e.bytes > 0u) std::memcpy(out, arena_ + e.offset, e.bytes);
    return int32_t(e.bytes);
}

int32_t FakeHost::read_prefix(const char *path, void *out, uint32_t max) const {
    const int index = find(path);
    if (index < 0) return ENAGA_FILE_NOT_FOUND;
    const Entry &e = files_[index];
    const uint32_t take = e.bytes < max ? e.bytes : max;
    if (take > 0u) std::memcpy(out, arena_ + e.offset, take);
    return int32_t(take);
}

bool FakeHost::has_file(const char *path) const { return find(path) >= 0; }

int32_t FakeHost::file_bytes(const char *path) const {
    const int index = find(path);
    return index < 0 ? -1 : int32_t(files_[index].bytes);
}

bool FakeHost::remove_file(const char *path) {
    const int index = find(path);
    if (index < 0) return false;
    for (int i = index; i + 1 < count_; i++) files_[i] = files_[i + 1];
    count_--;
    files_[count_] = Entry{};
    return true;
}

// --- held asynchrony ------------------------------------------------------

void FakeHost::complete_loads() {
    // Oldest first, and the queue is emptied before any callback runs: a `done` that starts
    // another load must not be fired inside this pass, which is the real host's ordering too.
    Pending queue[kMaxPending];
    const int n = pending_;
    for (int i = 0; i < n; i++) queue[i] = pending_queue_[i];
    pending_ = 0;
    for (int i = 0; i < n; i++) {
        if (queue[i].done != nullptr) queue[i].done(queue[i].user, queue[i].result);
    }
}

void FakeHost::fail_pending_loads(int32_t error) {
    for (int i = 0; i < pending_; i++) pending_queue_[i].result = error;
    complete_loads();
}

bool FakeHost::logged(const char *text) const {
    const int n = log_lines_ < kLogHistory ? log_lines_ : kLogHistory;
    for (int i = 0; i < n; i++) {
        if (same(log_history_[i], text)) return true;
    }
    return false;
}

void FakeHost::set_audio_load(uint32_t per_mille) { audio_load_ = per_mille; }
void FakeHost::set_device(uint32_t device) { device_ = device; }

// --- the trampolines ------------------------------------------------------

void FakeHost::t_log(const char *text) {
    FakeHost &h = the();
    if (text == nullptr) return;
    copy_path(h.log_, kLogMax, text);
    if (h.log_lines_ < kLogHistory) copy_path(h.log_history_[h.log_lines_], kLogMax, text);
    h.log_lines_++;
}

uint32_t FakeHost::t_ms() { return the().ms_++; }

void FakeHost::t_fatal(const char *reason) {
    // Not an abort: a test asserting that the app does NOT call fatal() is more useful than a
    // dead test binary, and `logged("FATAL ...")` is how it checks.
    FakeHost &h = the();
    h.t_log(reason == nullptr ? "FATAL" : reason);
}

void FakeHost::t_text(uint8_t *, int x, int y, const char *text, uint8_t grey) {
    FakeHost &h = the();
    if (h.text_count_ >= kTextDraws) {
        h.text_dropped_++;
        return;
    }
    TextDraw &draw = h.text_[h.text_count_++];
    draw.x = x;
    draw.y = y;
    draw.grey = grey;
    draw.length = 0;
    while (text != nullptr && text[draw.length] != '\0') {
        if (draw.length < int(sizeof draw.text) - 1) draw.text[draw.length] = text[draw.length];
        draw.length++;
    }
    draw.text[draw.length < int(sizeof draw.text) - 1 ? draw.length : int(sizeof draw.text) - 1] = '\0';
}
uint32_t FakeHost::t_audio_load() { return the().audio_load_; }
uint32_t FakeHost::t_audio_load_peak() { return the().audio_load_; }
void FakeHost::t_led(uint32_t, uint32_t, uint32_t, uint32_t) { the().leds_++; }

int32_t FakeHost::t_file_open(const char *path, uint32_t mode) {
    FakeHost &h = the();
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;

    int slot = -1;
    for (int i = 0; i < kHandles; i++) {
        if (!h.handles_[i].open) { slot = i; break; }
    }
    if (slot < 0) return ENAGA_FILE_TOO_MANY;   // the ABI's limit, kept

    int index = h.find(path);
    if (mode == ENAGA_FILE_WRITE) {
        if (h.full_) return ENAGA_FILE_FULL;
        if (index < 0) index = h.add(path, 0, false);
        else h.files_[index].bytes = 0;          // "created, or emptied if it exists"
        if (index < 0) return ENAGA_FILE_FULL;
    } else if (index < 0) {
        return ENAGA_FILE_NOT_FOUND;
    }
    h.handles_[slot] = Handle{true, index, 0u, mode};
    return int32_t(slot);
}

int32_t FakeHost::t_file_close(int32_t file) {
    FakeHost &h = the();
    if (file < 0 || file >= kHandles || !h.handles_[file].open) {
        return ENAGA_FILE_BAD_HANDLE;
    }
    h.handles_[file] = Handle{};
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_size(int32_t file) {
    FakeHost &h = the();
    if (file < 0 || file >= kHandles || !h.handles_[file].open) {
        return ENAGA_FILE_BAD_HANDLE;
    }
    return int32_t(h.files_[h.handles_[file].entry].bytes);
}

int32_t FakeHost::t_file_seek(int32_t file, uint32_t position) {
    FakeHost &h = the();
    if (file < 0 || file >= kHandles || !h.handles_[file].open) {
        return ENAGA_FILE_BAD_HANDLE;
    }
    h.handles_[file].position = position;
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_info(const char *path, enaga_file_info_t *info) {
    FakeHost &h = the();
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    const int index = h.find(path);
    if (index < 0) return ENAGA_FILE_NOT_FOUND;
    if (info != nullptr) {
        *info = enaga_file_info_t{};
        copy_path(info->name, uint32_t(sizeof(info->name)), h.files_[index].path);
        info->size = h.files_[index].bytes;
        info->flags = h.files_[index].folder ? ENAGA_FILE_FOLDER : 0u;
    }
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_remove(const char *path) {
    FakeHost &h = the();
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    return h.remove_file(path) ? ENAGA_FILE_OK : ENAGA_FILE_NOT_FOUND;
}

int32_t FakeHost::t_file_make_folder(const char *path) {
    FakeHost &h = the();
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    h.folders_made_++;
    if (h.find(path) >= 0) return ENAGA_FILE_OK;   // "parents too; OK if it exists"
    if (h.full_) return ENAGA_FILE_FULL;
    return h.add(path, 0, true) >= 0 ? ENAGA_FILE_OK : ENAGA_FILE_FULL;
}

int32_t FakeHost::t_file_read_all(const char *path, void *buffer, uint32_t max) {
    FakeHost &h = the();
    h.read_alls_++;
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    if (h.read_error_) return ENAGA_FILE_CARD_ERROR;
    return h.read_file(path, buffer, max);
}

void FakeHost::set_require_folders(bool require) { require_folders_ = require; }

// True when the folder `path` sits in was made, or is the data folder or the root. make_folder
// makes parents too, so a folder is also there when a folder made INSIDE it is.
bool FakeHost::parent_exists(const char *path) const {
    int slash = -1;
    for (int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') slash = i;
    }
    if (slash <= 0) return true;                       // "/NAME": the root
    const char *data = t_file_data_folder();
    if (int(std::strlen(data)) == slash && std::strncmp(path, data, size_t(slash)) == 0) return true;
    for (int i = 0; i < count_; i++) {
        if (!files_[i].folder) continue;
        const char *f = files_[i].path;
        if (std::strncmp(f, path, size_t(slash)) == 0 && (f[slash] == '\0' || f[slash] == '/')) {
            return true;
        }
    }
    return false;
}

int32_t FakeHost::t_file_save(const char *path, const void *data, uint32_t bytes) {
    FakeHost &h = the();
    h.saves_++;
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    if (h.require_folders_ && !h.parent_exists(path)) return ENAGA_FILE_NOT_FOUND;
    if (h.full_) return ENAGA_FILE_FULL;
    return h.write_file(path, data, bytes) ? ENAGA_FILE_OK : ENAGA_FILE_FULL;
}

int32_t FakeHost::t_file_read(int32_t file, void *buffer, uint32_t bytes, enaga_file_done_t done,
                              void *user) {
    FakeHost &h = the();
    if (file < 0 || file >= kHandles || !h.handles_[file].open) {
        return ENAGA_FILE_BAD_HANDLE;
    }
    if (h.read_error_) return ENAGA_FILE_CARD_ERROR;
    if (h.pending_ >= kMaxPending) return ENAGA_FILE_TOO_MANY;

    Handle &handle = h.handles_[file];
    const Entry &e = h.files_[handle.entry];
    uint32_t available = handle.position >= e.bytes ? 0u : e.bytes - handle.position;
    if (available > bytes) available = bytes;
    if (available > 0u) std::memcpy(buffer, h.arena_ + e.offset + handle.position, available);
    handle.position += available;
    h.pending_queue_[h.pending_++] = Pending{done, user, int32_t(available)};
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_write(int32_t file, const void *data, uint32_t bytes,
                               enaga_file_done_t done, void *user) {
    FakeHost &h = the();
    if (file < 0 || file >= kHandles || !h.handles_[file].open) {
        return ENAGA_FILE_BAD_HANDLE;
    }
    if (h.full_) return ENAGA_FILE_FULL;
    if (h.pending_ >= kMaxPending) return ENAGA_FILE_TOO_MANY;

    Handle &handle = h.handles_[file];
    Entry &e = h.files_[handle.entry];
    const uint32_t end = handle.position + bytes;
    if (end > e.capacity) {
        if (h.used_ + end > kArenaBytes) return ENAGA_FILE_FULL;
        // Move the file to the end of the arena, carrying what it already holds.
        const uint32_t old = e.offset;
        e.offset = h.used_;
        if (e.bytes > 0u) std::memmove(h.arena_ + e.offset, h.arena_ + old, e.bytes);
        e.capacity = end;
        h.used_ += end;
    }
    if (bytes > 0u) std::memcpy(h.arena_ + e.offset + handle.position, data, bytes);
    handle.position = end;
    if (end > e.bytes) e.bytes = end;
    h.pending_queue_[h.pending_++] = Pending{done, user, int32_t(bytes)};
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_load(const char *path, void *buffer, uint32_t max,
                              enaga_file_done_t done, void *user) {
    FakeHost &h = the();
    h.loads_++;
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    if (h.read_error_) return ENAGA_FILE_CARD_ERROR;
    if (h.pending_ >= kMaxPending) return ENAGA_FILE_TOO_MANY;

    // The bytes are copied NOW and the completion is queued, which is the one thing that makes
    // this fake worth having: the load is in flight until complete_loads(), so a test can look
    // at what the app refuses in the meantime.
    const int32_t result = h.read_file(path, buffer, max);
    if (result < 0) return result;
    h.pending_queue_[h.pending_++] = Pending{done, user, result};
    return ENAGA_FILE_OK;
}

int32_t FakeHost::t_file_list(const char *path, enaga_file_entry_t entry, enaga_file_done_t done,
                              void *user) {
    FakeHost &h = the();
    const int32_t card = h.card_check();
    if (card != ENAGA_FILE_OK) return card;
    const int32_t bad = h.path_check(path);
    if (bad != ENAGA_FILE_OK) return bad;
    if (h.pending_ >= kMaxPending) return ENAGA_FILE_TOO_MANY;

    const uint32_t n = uint32_t(std::strlen(path));

    // A folder that is not there ends with NOT FOUND, as FatFs's f_opendir answers FR_NO_PATH.
    // The ABI does not say which error; it does say a list ends with "the number of entries ...
    // or a negative enaga_file_error", and "an empty folder" and "no such folder" are different
    // answers a browser shows differently. It exists when it was made, or when anything is in it.
    bool exists = n == 1;                        // "/" always does
    for (int i = 0; i < h.count_ && !exists; i++) {
        const char *p = h.files_[i].path;
        if (std::strncmp(p, path, n) != 0) continue;
        if (p[n] == '\0' ? h.files_[i].folder : p[n] == '/') exists = true;
    }
    if (!exists) {
        h.pending_queue_[h.pending_++] = Pending{done, user, int32_t(ENAGA_FILE_NOT_FOUND)};
        return ENAGA_FILE_OK;
    }

    int listed = 0;
    for (int i = 0; i < h.count_; i++) {
        // Immediate children only: the path is a prefix and what follows holds no further '/'.
        if (std::strncmp(h.files_[i].path, path, n) != 0) continue;
        const char *rest = h.files_[i].path + n;
        if (*rest == '/') rest++;
        if (*rest == '\0' || std::strchr(rest, '/') != nullptr) continue;
        if (entry != nullptr) {
            enaga_file_info_t info{};
            copy_path(info.name, uint32_t(sizeof(info.name)), rest);
            info.size = h.files_[i].bytes;
            info.flags = h.files_[i].folder ? ENAGA_FILE_FOLDER : 0u;
            entry(user, &info);
        }
        listed++;
    }
    h.pending_queue_[h.pending_++] = Pending{done, user, int32_t(listed)};
    return ENAGA_FILE_OK;
}

const char *FakeHost::t_file_data_folder() { return "/ENAGA/DATA/STRATA"; }

// Streams: STRATA does not use them (nothing in app/ calls stream_*), so these refuse rather
// than pretend. A test that starts needing them will see the refusal and this will grow.
int32_t FakeHost::t_stream_open(const char *, uint32_t, void *, uint32_t) {
    return ENAGA_FILE_NOT_FOUND;
}
int32_t FakeHost::t_stream_create(const char *, uint32_t, void *, uint32_t) {
    return ENAGA_FILE_DENIED;
}
uint32_t FakeHost::t_stream_read(int32_t, void *, uint32_t) { return 0; }
uint32_t FakeHost::t_stream_write(int32_t, const void *, uint32_t) { return 0; }
int32_t FakeHost::t_stream_status(int32_t, enaga_stream_status_t *) {
    return ENAGA_FILE_BAD_HANDLE;
}
int32_t FakeHost::t_stream_close(int32_t, const void *, uint32_t) { return ENAGA_FILE_BAD_HANDLE; }

void FakeHost::t_open_menu() {}
void FakeHost::t_set_busy(uint32_t) {}
void FakeHost::t_speaker(uint32_t) {}
int32_t FakeHost::t_headphones() { return 0; }
uint32_t FakeHost::t_device() { return the().device_; }
int32_t FakeHost::t_input(uint32_t, uint32_t) { return 0; }
int32_t FakeHost::t_midi_send(uint32_t, const uint8_t *, uint32_t) { return 0; }
uint32_t FakeHost::t_midi_channel() { return 0; }

int32_t FakeHost::t_clock_control(uint32_t command, int32_t value) {
    FakeHost &h = the();
    if (command < 8u) {
        h.control_[command]++;
        h.control_value_[command] = value;
    }

    // The refusal CLAUDE.md requires STRATA to handle: under an external clock the app can only
    // follow, so start, stop, continue and locate all fail. A non-zero return is the failure
    // (<enaga/clock.hpp> compares against 0), and the state is left alone.
    if (external(h.clock_.source) && command != ENAGA_CLOCK_TEMPO) return -1;

    switch (command) {
        case ENAGA_CLOCK_START:
            h.clock_.running = 1;
            h.clock_.position = 0;
            h.clock_.fraction = 0.0f;
            return 0;
        case ENAGA_CLOCK_STOP:
            h.clock_.running = 0;
            return 0;
        case ENAGA_CLOCK_CONTINUE:
            h.clock_.running = 1;
            return 0;
        case ENAGA_CLOCK_LOCATE:
            h.clock_.position = uint32_t(value < 0 ? 0 : value);
            h.clock_.fraction = 0.0f;
            return 0;
        case ENAGA_CLOCK_TEMPO:
            // The ABI's range: BPM x 100, 2000-30000. Outside it the real host refuses.
            if (value < 2000 || value > 30000) return -1;
            h.clock_.tempo = float(value) * 0.01f;
            return 0;
        default:
            return -1;
    }
}

void FakeHost::t_clock_state(enaga_clock_state_t *state) {
    if (state != nullptr) *state = the().clock_;
}

uint32_t FakeHost::t_clock_ticks(enaga_clock_tick_t *ticks, uint32_t max) {
    FakeHost &h = the();
    uint32_t n = uint32_t(h.tick_count_);
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) ticks[i] = h.ticks_[i];
    // Handed out once: a block that is given no ticks must report none, which is the common case
    // and the one a scheduler bug hides in.
    h.tick_count_ = 0;
    if (n > 0u) h.clock_.position = ticks[n - 1].tick;
    return n;
}

}  // namespace strata::test

// What <enaga/app.hpp> declares and the app runtime normally defines. host/Makefile does not
// link Enaga's runtime.cpp, so this is the only definition in the test binary.
namespace enaga {
const enaga_host_t &host() { return strata::test::FakeHost::the().abi(); }
}  // namespace enaga
