// The sample browser's list: one folder of the card, filtered to what a SAMPLE or GRAIN track can
// load, sorted, and turned into the path the kit stores.
//
// docs/SPEC.md: samples live in /SAMPLES, and resampled takes in /RECORD. Before this there was
// no way to pick one on the box: a track got a file only by resampling into it, recalling a kit
// or overwriting a file on a PC. `app/storage.cpp` fills this from `enaga::files::list` (whose
// callbacks run in the main context, one entry at a time) and `app/app.hpp` draws it.
//
// It is in core/ because none of it needs Enaga: what is a WAV, what sorts where, whether a path
// still fits the kit's 112 bytes. All of that is host-tested in tests/test_browse.cpp. Main
// context only; the audio side never sees it.
//
// ## The root is not "/"
//
// The browser's top level is a virtual folder holding the places on the card samples come from
// (kRoots, in the order shown):
//
// - SAMPLES   /SAMPLES               Enaga's shared sample folder (docs/SPEC.md)
// - POOL      /SmplTrek/Pool/Audio   the stock SmplTrek firmware's sample pool: its manual (p.30)
//                                    says "Copy WAV files to the card > SmplTrek > Pool > Audio"
// - SMPLTREK  /SmplTrek              the stock firmware's whole tree: Pool, Preset, and Project,
//                                    whose <name>/Audio folders hold each project's own samples
// - RECORD    /RECORD                resampled takes
//
// POOL and SMPLTREK are shown only on a card that has a /SmplTrek folder (the caller checks:
// core/ has no files), so a card the stock firmware never saw shows just SAMPLES and RECORD.
// STRATA only reads there: Enaga lets an app read anywhere but write only in its data folder,
// /SAMPLES and /RECORD, and nothing in the browser writes. A kit stores a sample as its full
// path, so a file picked under /SmplTrek loads again from there like any other.
//
// Listing "/" would show ENAGA and whatever else a PC left there, none of it samples, and would
// put the system folders one OK away from a sample track.
//
// ## What is left out, and why
//
// - Anything that is neither a folder nor a name ending in .WAV (in any case): the loader reads
//   WAV only (core/pcm.hpp), and a list of everything would be mostly things that fail.
// - Names starting with '.': macOS writes a "._Kick.wav" resource fork next to every file it
//   copies to a FAT card. It ends in .wav, it is not audio, and there is one for every sample.
// - Names too long to store (kEntryNameBytes), and any entry whose path would not fit in
//   kSamplePathBytes. Both are counted in `skipped()`, so the screen can say something was left
//   out rather than pretend the folder holds less than it does.
// - Past kEntries, the rest. Counted in `skipped()` too.
#pragma once

#include "project.hpp"

#include <cstdint>

namespace strata {

class SampleList {
public:
    // 256 entries of 64 bytes: 16 KiB. A folder of more than 256 entries shows the first 256 the
    // card lists, sorted, and says how many it left out.
    static constexpr int kEntries = 256;
    // A name, terminator included. Long enough for any name a person types, short enough that a
    // full table stays at 16 KiB. A longer name is skipped, not cut: a cut name opens nothing.
    static constexpr int kEntryNameBytes = 64;
    // The folder being shown, as a path, terminator included. Nothing deeper can be loaded anyway:
    // the path of a file in it has to fit kSamplePathBytes as well.
    static constexpr int kFolderBytes = kSamplePathBytes;

    // The top level's entries, in the order shown: a label for the screen and the folder it opens.
    struct Root {
        const char *label;
        const char *path;
        bool smpltrek;                        // shown only when the card has /SmplTrek
    };
    static constexpr int kRoots = 4;
    static constexpr int kSamplesRoot = 0;    // root(kSamplesRoot).path is "/SAMPLES"
    static constexpr int kPoolRoot = 1;       // root(kPoolRoot).path is "/SmplTrek/Pool/Audio"
    static const Root &root(int i);           // 0 <= i < kRoots

    // The virtual top level, as folders: SAMPLES and RECORD, and POOL and SMPLTREK between them
    // when `smpltrek` (the card has a /SmplTrek folder).
    void open_root(bool smpltrek = false);

    // Start listing `folder` (an absolute path such as "/SAMPLES/KICKS"): empties the table and
    // remembers the folder. False, with nothing changed, when the folder does not fit
    // kFolderBytes or is not absolute.
    bool open(const char *folder);

    // One entry as files::list reports it. Filters, keeps the order sorted (folders first, then
    // names without regard to case) and counts what it leaves out. True when it was kept.
    bool add(const char *name, bool folder);

    bool at_root() const { return folder_[0] == '\0'; }
    const char *folder() const { return folder_; }

    int count() const { return count_; }
    int skipped() const { return skipped_; }

    // The i-th entry in sorted order. Out of range reads as an empty name and not a folder.
    const char *name(int i) const;
    bool is_folder(int i) const;

    // The full path of the i-th entry: folder + "/" + name, or at the root the folder the entry
    // opens ("/SmplTrek/Pool/Audio" for POOL). False when `i` is out of range or the path would
    // not fit `max` bytes (with its terminator) -- never a cut path, which would name a different
    // file or none.
    bool path_of(int i, char *out, int max) const;

    // The entry whose name is `name`, compared without regard to case (FAT is case-insensitive),
    // or -1. For putting the cursor on the file a track already plays.
    int find(const char *name) const;

    // Are these the same name, compared without regard to case as FAT does? Null is no name and
    // matches nothing.
    static bool same_name(const char *a, const char *b);

    // The folder above this one: "/SAMPLES/KICKS" -> "/SAMPLES". A top folder ("/SAMPLES") has no
    // parent on the card, so it writes "" -- the virtual root. POOL climbs through the card's own
    // tree: "/SmplTrek/Pool/Audio" -> "/SmplTrek/Pool" -> "/SmplTrek" -> "".
    static void parent_of(const char *folder, char *out, int max);

    // The folder part of a sample path, for opening the browser where the track's file is:
    // "/SAMPLES/KICKS/K1.WAV" -> "/SAMPLES/KICKS". Writes "" and returns false when the path has
    // no folder part or does not fit.
    static bool folder_of(const char *path, char *out, int max);

    // The name part of a path: "/SAMPLES/K1.WAV" -> "K1.WAV". A pointer into `path`.
    static const char *name_of(const char *path);

    // Does this name end in .WAV, in any case? The one filter on files.
    static bool is_wav(const char *name);

    // Is `path` inside one of the places the browser shows ("/SAMPLES/...", "/RECORD/..." or
    // "/SmplTrek/...", without regard to case)?
    static bool in_browsable_folder(const char *path);

private:
    struct Entry {
        char name[kEntryNameBytes];
        bool folder;
    };
    bool fits(const char *name) const;        // folder + "/" + name in kSamplePathBytes
    static int compare(const Entry &a, const Entry &b);

    Entry entries_[kEntries] = {};
    uint16_t order_[kEntries] = {};           // sorted order, indices into entries_
    int count_ = 0;
    int skipped_ = 0;
    char folder_[kFolderBytes] = {};
    int8_t root_of_[kRoots] = {};             // at the root: entry -> index into root()
};

}  // namespace strata
