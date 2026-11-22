#include "browse.hpp"

namespace strata {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }

int length(const char *s) {
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

// Copy `s` into `out` with its terminator; false (and `out` empty) when it does not fit `max`.
bool copy(char *out, int max, const char *s) {
    const int n = length(s);
    if (max <= 0) return false;
    if (n + 1 > max) {
        out[0] = '\0';
        return false;
    }
    for (int i = 0; i <= n; i++) out[i] = s[i];
    return true;
}

}  // namespace

void SampleList::open_root() {
    folder_[0] = '\0';
    count_ = 0;
    skipped_ = 0;
    // Folders, so they sort and open like any other. Added straight, not through add(): at the
    // root there is no folder to join them to, and they are not names on the card at the root's
    // level anyway.
    const char *const tops[] = {"RECORD", "SAMPLES"};
    for (const char *top : tops) {
        Entry &e = entries_[count_];
        copy(e.name, kEntryNameBytes, top);
        e.folder = true;
        order_[count_] = uint16_t(count_);
        count_++;
    }
    // SAMPLES first: it is where a person's own sounds are, and RECORD holds takes.
    order_[0] = 1;
    order_[1] = 0;
}

bool SampleList::open(const char *folder) {
    if (folder == nullptr || folder[0] != '/') return false;
    char next[kFolderBytes];
    if (!copy(next, kFolderBytes, folder)) return false;
    // "/SAMPLES/" and "/SAMPLES" are the same folder; the path joiner adds the slash itself.
    int n = length(next);
    while (n > 1 && next[n - 1] == '/') next[--n] = '\0';
    copy(folder_, kFolderBytes, next);
    count_ = 0;
    skipped_ = 0;
    return true;
}

bool SampleList::fits(const char *name) const {
    // folder + '/' + name + terminator
    return length(folder_) + 1 + length(name) + 1 <= kSamplePathBytes;
}

bool SampleList::is_wav(const char *name) {
    const int n = length(name);
    if (n < 5) return false;                        // ".WAV" alone is not a name
    const char *ext = name + n - 4;
    return ext[0] == '.' && upper(ext[1]) == 'W' && upper(ext[2]) == 'A' && upper(ext[3]) == 'V';
}

int SampleList::compare(const Entry &a, const Entry &b) {
    if (a.folder != b.folder) return a.folder ? -1 : 1;
    for (int i = 0;; i++) {
        const char x = upper(a.name[i]), y = upper(b.name[i]);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
        if (x == '\0') return 0;
    }
}

bool SampleList::add(const char *name, bool folder) {
    if (name == nullptr || name[0] == '\0') return false;
    // Hidden files and macOS's "._" resource forks; also "." and "..", should a host list them.
    if (name[0] == '.') return false;
    if (!folder && !is_wav(name)) return false;
    // From here on it is something the browser would show: leaving it out is worth counting.
    if (count_ >= kEntries || length(name) + 1 > kEntryNameBytes || !fits(name)) {
        skipped_++;
        return false;
    }
    Entry &e = entries_[count_];
    copy(e.name, kEntryNameBytes, name);
    e.folder = folder;

    // Insertion into the sorted index: at most 256 two-byte moves, once per entry.
    int at = count_;
    while (at > 0 && compare(entries_[order_[at - 1]], e) > 0) {
        order_[at] = order_[at - 1];
        at--;
    }
    order_[at] = uint16_t(count_);
    count_++;
    return true;
}

const char *SampleList::name(int i) const {
    if (i < 0 || i >= count_) return "";
    return entries_[order_[i]].name;
}

bool SampleList::is_folder(int i) const {
    if (i < 0 || i >= count_) return false;
    return entries_[order_[i]].folder;
}

bool SampleList::path_of(int i, char *out, int max) const {
    if (out == nullptr || max <= 0) return false;
    out[0] = '\0';
    if (i < 0 || i >= count_) return false;
    const char *n = name(i);
    const int fl = length(folder_), nl = length(n);
    if (fl + 1 + nl + 1 > max) return false;
    int at = 0;
    for (int k = 0; k < fl; k++) out[at++] = folder_[k];
    out[at++] = '/';
    for (int k = 0; k < nl; k++) out[at++] = n[k];
    out[at] = '\0';
    return true;
}

int SampleList::find(const char *wanted) const {
    if (wanted == nullptr) return -1;
    for (int i = 0; i < count_; i++) {
        const char *n = name(i);
        int k = 0;
        while (n[k] != '\0' && upper(n[k]) == upper(wanted[k])) k++;
        if (n[k] == '\0' && wanted[k] == '\0') return i;
    }
    return -1;
}

void SampleList::parent_of(const char *folder, char *out, int max) {
    if (out == nullptr || max <= 0) return;
    out[0] = '\0';
    if (folder == nullptr) return;
    int last = -1;
    for (int i = 0; folder[i] != '\0'; i++) {
        if (folder[i] == '/') last = i;
    }
    // "/SAMPLES" (last == 0) and anything without a slash go to the virtual root.
    if (last <= 0 || last + 1 > max) return;
    for (int i = 0; i < last; i++) out[i] = folder[i];
    out[last] = '\0';
}

bool SampleList::folder_of(const char *path, char *out, int max) {
    if (out == nullptr || max <= 0) return false;
    out[0] = '\0';
    if (path == nullptr) return false;
    int last = -1;
    for (int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') last = i;
    }
    if (last <= 0 || last + 1 > max) return false;
    for (int i = 0; i < last; i++) out[i] = path[i];
    out[last] = '\0';
    return true;
}

const char *SampleList::name_of(const char *path) {
    if (path == nullptr) return "";
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') name = p + 1;
    }
    return name;
}

bool SampleList::in_browsable_folder(const char *path) {
    if (path == nullptr) return false;
    const char *const tops[] = {"/SAMPLES/", "/RECORD/"};
    for (const char *top : tops) {
        int i = 0;
        while (top[i] != '\0' && upper(path[i]) == top[i]) i++;
        if (top[i] == '\0') return true;
    }
    return false;
}

}  // namespace strata
