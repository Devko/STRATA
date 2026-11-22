// core/browse: the sample browser's list. What is shown, in what order, and the path it gives.
#include "browse.hpp"

#include "check.hpp"

#include <cstring>

using namespace strata;

namespace {
SampleList g_list;   // 16 KiB: file scope, never a local
}

TEST(browse_root_is_samples_then_record) {
    g_list.open_root();
    CHECK(g_list.at_root());
    CHECK_EQ(g_list.count(), 2);
    CHECK(std::strcmp(g_list.name(0), "SAMPLES") == 0);
    CHECK(std::strcmp(g_list.name(1), "RECORD") == 0);
    CHECK(g_list.is_folder(0));
    CHECK(g_list.is_folder(1));
}

TEST(browse_keeps_wavs_and_folders_and_drops_the_rest) {
    CHECK(g_list.open("/SAMPLES"));
    CHECK(!g_list.at_root());
    CHECK(g_list.add("kick.wav", false));
    CHECK(g_list.add("SNARE.WAV", false));
    CHECK(g_list.add("Hat.Wav", false));
    CHECK(g_list.add("LOOPS", true));
    CHECK(!g_list.add("notes.txt", false));
    CHECK(!g_list.add("wav", false));
    CHECK(!g_list.add(".wav", false));
    CHECK(!g_list.add("._kick.wav", false));      // macOS resource fork
    CHECK(!g_list.add(".Trashes", true));
    CHECK(!g_list.add("", false));
    CHECK(!g_list.add(nullptr, false));
    CHECK_EQ(g_list.count(), 4);
    CHECK_EQ(g_list.skipped(), 0);                // filtered is not skipped
}

TEST(browse_sorts_folders_first_then_names_ignoring_case) {
    g_list.open("/SAMPLES");
    g_list.add("b.wav", false);
    g_list.add("ZED", true);
    g_list.add("A.WAV", false);
    g_list.add("c.wav", false);
    g_list.add("alpha", true);
    CHECK_EQ(g_list.count(), 5);
    CHECK(std::strcmp(g_list.name(0), "alpha") == 0);
    CHECK(std::strcmp(g_list.name(1), "ZED") == 0);
    CHECK(std::strcmp(g_list.name(2), "A.WAV") == 0);
    CHECK(std::strcmp(g_list.name(3), "b.wav") == 0);
    CHECK(std::strcmp(g_list.name(4), "c.wav") == 0);
    CHECK(g_list.is_folder(1));
    CHECK(!g_list.is_folder(2));
    // Out of range reads as nothing, never as a neighbour.
    CHECK(std::strcmp(g_list.name(5), "") == 0);
    CHECK(std::strcmp(g_list.name(-1), "") == 0);
    CHECK(!g_list.is_folder(5));
}

TEST(browse_path_of_joins_folder_and_name) {
    g_list.open("/SAMPLES/KICKS/");               // a trailing slash is the same folder
    CHECK(std::strcmp(g_list.folder(), "/SAMPLES/KICKS") == 0);
    g_list.add("K 01.wav", false);
    char path[kSamplePathBytes];
    CHECK(g_list.path_of(0, path, sizeof path));
    CHECK(std::strcmp(path, "/SAMPLES/KICKS/K 01.wav") == 0);
    CHECK(!g_list.path_of(1, path, sizeof path));
    CHECK_EQ(int(path[0]), 0);
    char tiny[8];
    CHECK(!g_list.path_of(0, tiny, sizeof tiny));  // never a cut path
    CHECK_EQ(int(tiny[0]), 0);
}

TEST(browse_skips_what_the_kit_could_not_store) {
    g_list.open("/SAMPLES");
    // A name longer than an entry holds: skipped and counted, not cut.
    char longname[SampleList::kEntryNameBytes + 8];
    std::memset(longname, 'A', sizeof longname);
    std::memcpy(longname + sizeof longname - 5, ".WAV", 5);
    CHECK(!g_list.add(longname, false));
    CHECK_EQ(g_list.skipped(), 1);

    // A deep folder: every name fits an entry but the joined path would pass 112 bytes.
    char deep[SampleList::kFolderBytes];
    std::memset(deep, 'D', sizeof deep);
    deep[0] = '/';
    deep[100] = '\0';
    CHECK(g_list.open(deep));
    CHECK(g_list.add("SHORTS.WAV", false));       // 100 + 1 + 10 + 1 = 112: just fits
    CHECK(!g_list.add("LONGERS.WAV", false));     // 113: does not
    CHECK_EQ(g_list.skipped(), 1);
    char path[kSamplePathBytes];
    CHECK(g_list.path_of(0, path, sizeof path));
    CHECK_EQ(int(std::strlen(path)), kSamplePathBytes - 1);
}

TEST(browse_refuses_a_folder_it_could_not_hold) {
    g_list.open("/SAMPLES");
    g_list.add("A.WAV", false);
    char huge[SampleList::kFolderBytes + 4];
    std::memset(huge, 'X', sizeof huge);
    huge[0] = '/';
    huge[sizeof huge - 1] = '\0';
    CHECK(!g_list.open(huge));
    CHECK(!g_list.open("SAMPLES"));               // not absolute
    CHECK(!g_list.open(nullptr));
    // Nothing changed.
    CHECK(std::strcmp(g_list.folder(), "/SAMPLES") == 0);
    CHECK_EQ(g_list.count(), 1);
}

TEST(browse_stops_at_256_entries_and_counts_the_rest) {
    g_list.open("/SAMPLES");
    char name[16];
    for (int i = 0; i < SampleList::kEntries + 10; i++) {
        std::snprintf(name, sizeof name, "S%03d.WAV", (i * 37) % 1000);
        g_list.add(name, false);
    }
    CHECK_EQ(g_list.count(), SampleList::kEntries);
    CHECK_EQ(g_list.skipped(), 10);
    for (int i = 1; i < g_list.count(); i++) {
        CHECK(std::strcmp(g_list.name(i - 1), g_list.name(i)) <= 0);
    }
}

TEST(browse_find_ignores_case) {
    g_list.open("/SAMPLES");
    g_list.add("Kick.wav", false);
    g_list.add("Kick2.wav", false);
    CHECK_EQ(g_list.find("KICK.WAV"), 0);
    CHECK_EQ(g_list.find("kick2.WAV"), 1);
    CHECK_EQ(g_list.find("Kick"), -1);
    CHECK_EQ(g_list.find("Kick.wav2"), -1);
    CHECK_EQ(g_list.find(nullptr), -1);
}

TEST(browse_parent_folder_and_name_of_a_path) {
    char out[kSamplePathBytes];
    SampleList::parent_of("/SAMPLES/KICKS/808", out, sizeof out);
    CHECK(std::strcmp(out, "/SAMPLES/KICKS") == 0);
    SampleList::parent_of("/SAMPLES", out, sizeof out);
    CHECK(std::strcmp(out, "") == 0);             // the virtual root
    SampleList::parent_of("", out, sizeof out);
    CHECK(std::strcmp(out, "") == 0);

    CHECK(SampleList::folder_of("/SAMPLES/KICKS/K1.WAV", out, sizeof out));
    CHECK(std::strcmp(out, "/SAMPLES/KICKS") == 0);
    CHECK(SampleList::folder_of("/RECORD/STRATA_1.WAV", out, sizeof out));
    CHECK(std::strcmp(out, "/RECORD") == 0);
    CHECK(!SampleList::folder_of("K1.WAV", out, sizeof out));
    CHECK(!SampleList::folder_of("/K1.WAV", out, sizeof out));
    char tiny[4];
    CHECK(!SampleList::folder_of("/SAMPLES/K1.WAV", tiny, sizeof tiny));

    CHECK(std::strcmp(SampleList::name_of("/SAMPLES/KICKS/K1.WAV"), "K1.WAV") == 0);
    CHECK(std::strcmp(SampleList::name_of("K1.WAV"), "K1.WAV") == 0);
    CHECK(std::strcmp(SampleList::name_of(nullptr), "") == 0);

    CHECK(SampleList::in_browsable_folder("/SAMPLES/K1.WAV"));
    CHECK(SampleList::in_browsable_folder("/record/STRATA_1.WAV"));
    CHECK(!SampleList::in_browsable_folder("/SAMPLESX/K1.WAV"));
    CHECK(!SampleList::in_browsable_folder("/ENAGA/DATA/K1.WAV"));
    CHECK(!SampleList::in_browsable_folder(nullptr));
}
