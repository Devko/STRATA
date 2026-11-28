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
    char out[kSamplePathBytes];
    CHECK(g_list.path_of(0, out, sizeof out));
    CHECK(std::strcmp(out, "/SAMPLES") == 0);
    CHECK(g_list.path_of(1, out, sizeof out));
    CHECK(std::strcmp(out, "/RECORD") == 0);
    CHECK(!g_list.path_of(2, out, sizeof out));
}

TEST(browse_root_adds_the_smpltrek_folders_when_the_card_has_them) {
    // The stock firmware's manual: samples in SmplTrek > Pool > Audio, projects in
    // SmplTrek > Project. POOL goes straight to the pool, SMPLTREK to the whole tree.
    g_list.open_root(true);
    CHECK(g_list.at_root());
    CHECK_EQ(g_list.count(), 4);
    const char *const names[] = {"SAMPLES", "POOL", "SMPLTREK", "RECORD"};
    const char *const paths[] = {"/SAMPLES", "/SmplTrek/Pool/Audio", "/SmplTrek", "/RECORD"};
    char out[kSamplePathBytes];
    for (int i = 0; i < 4; i++) {
        CHECK(std::strcmp(g_list.name(i), names[i]) == 0);
        CHECK(g_list.is_folder(i));
        CHECK(g_list.path_of(i, out, sizeof out));
        CHECK(std::strcmp(out, paths[i]) == 0);
    }
    // The cursor comes back to SMPLTREK after climbing out of /SmplTrek (FAT ignores case).
    CHECK_EQ(g_list.find("SmplTrek"), 2);
    char tiny[8];
    CHECK(!g_list.path_of(1, tiny, sizeof tiny));     // never a cut path
    CHECK(std::strcmp(SampleList::root(SampleList::kPoolRoot).path, "/SmplTrek/Pool/Audio") == 0);
    CHECK(std::strcmp(SampleList::root(SampleList::kSamplesRoot).path, "/SAMPLES") == 0);

    // Below the root, paths are folder + name again, and POOL climbs the card's own tree.
    CHECK(g_list.open("/SmplTrek/Pool/Audio"));
    CHECK(g_list.add("Kick 01.wav", false));
    CHECK(g_list.path_of(0, out, sizeof out));
    CHECK(std::strcmp(out, "/SmplTrek/Pool/Audio/Kick 01.wav") == 0);
    SampleList::parent_of("/SmplTrek/Pool/Audio", out, sizeof out);
    CHECK(std::strcmp(out, "/SmplTrek/Pool") == 0);
    SampleList::parent_of("/SmplTrek", out, sizeof out);
    CHECK(std::strcmp(out, "") == 0);

    // A track playing a SmplTrek file reopens the browser on it.
    CHECK(SampleList::in_browsable_folder("/SmplTrek/Pool/Audio/Kick 01.wav"));
    CHECK(SampleList::in_browsable_folder("/SMPLTREK/Project/SONG1/Audio/Vox.wav"));
    CHECK(!SampleList::in_browsable_folder("/SmplTrekX/K1.WAV"));
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

TEST(browse_same_name_ignores_case_and_nothing_else) {
    CHECK(SampleList::same_name("DEFAULT", "default"));
    CHECK(SampleList::same_name("", ""));
    CHECK(!SampleList::same_name("DEFAULT", "DEFAULT2"));
    CHECK(!SampleList::same_name("DEFAULT2", "DEFAULT"));
    CHECK(!SampleList::same_name("A-B", "A_B"));
    CHECK(!SampleList::same_name(nullptr, "A"));
    CHECK(!SampleList::same_name("A", nullptr));
}

// Every buffer edge exactly: a path that fills `max` to the byte fits, one byte more does not and
// leaves the buffer empty; no room at all touches nothing.
TEST(browse_buffers_fit_to_the_byte) {
    char buf[64];
    g_list.open_root(false);
    // "/SAMPLES" is 8 characters and its terminator.
    CHECK(g_list.path_of(0, buf, 9));
    CHECK(std::strcmp(buf, "/SAMPLES") == 0);
    std::memset(buf, 'x', sizeof buf);
    CHECK(!g_list.path_of(0, buf, 8));
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    CHECK(!g_list.path_of(0, buf, 1));
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    CHECK(!g_list.path_of(0, buf, 0));
    CHECK_EQ(buf[0], 'x');
    CHECK(!g_list.path_of(0, nullptr, 9));
    CHECK(!g_list.is_folder(g_list.count()));
    CHECK(&SampleList::root(-1) == &SampleList::root(0));
    CHECK(&SampleList::root(SampleList::kRoots) == &SampleList::root(SampleList::kRoots - 1));
    // In a folder: "/S" + "/" + "A.WAV" + terminator is 9.
    CHECK(g_list.open("/S"));
    CHECK(g_list.add("A.WAV", false));
    CHECK(g_list.path_of(0, buf, 9));
    CHECK(std::strcmp(buf, "/S/A.WAV") == 0);
    CHECK(!g_list.path_of(0, buf, 8));
    // A name of 63 characters is the longest an entry holds.
    char name[80];
    std::memset(name, 'N', sizeof name);
    name[SampleList::kEntryNameBytes - 1] = '\0';
    CHECK(g_list.add(name, true));
    name[SampleList::kEntryNameBytes - 1] = 'N';
    name[SampleList::kEntryNameBytes] = '\0';
    CHECK(!g_list.add(name, true));
    CHECK_EQ(g_list.skipped(), 1);
    // A new root starts the count again.
    g_list.open_root(false);
    CHECK_EQ(g_list.skipped(), 0);
}

TEST(browse_parent_and_folder_of_fit_to_the_byte) {
    char buf[16];
    SampleList::parent_of("/A/B", buf, 3);
    CHECK(std::strcmp(buf, "/A") == 0);
    std::memset(buf, 'x', sizeof buf);
    SampleList::parent_of("/A/B", buf, 2);
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    SampleList::parent_of("/A/B", buf, 1);
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    SampleList::parent_of("/A/B", buf, 0);
    CHECK_EQ(buf[0], 'x');
    SampleList::parent_of("/A/B", nullptr, 3);    // nothing to write to: nothing happens
    SampleList::parent_of("A/B", buf, 16);
    CHECK(std::strcmp(buf, "A") == 0);
    CHECK(SampleList::folder_of("/A/B.WAV", buf, 3));
    CHECK(std::strcmp(buf, "/A") == 0);
    std::memset(buf, 'x', sizeof buf);
    CHECK(!SampleList::folder_of("/A/B.WAV", buf, 2));
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    CHECK(!SampleList::folder_of("/A/B.WAV", buf, 1));
    CHECK_EQ(buf[0], '\0');
    std::memset(buf, 'x', sizeof buf);
    CHECK(!SampleList::folder_of("/A/B.WAV", buf, 0));
    CHECK_EQ(buf[0], 'x');
    CHECK(!SampleList::folder_of("/A/B.WAV", nullptr, 3));
    CHECK(!SampleList::folder_of(nullptr, buf, 16));
    CHECK(SampleList::folder_of("A/B.WAV", buf, 16));
    CHECK(std::strcmp(buf, "A") == 0);
}

TEST(browse_names_edges_one_letter_ties_and_the_first_character) {
    // A one-letter folder is a name; z and Z are the same letter; the first letters count.
    g_list.open("/SAMPLES");
    CHECK(g_list.add("A", true));
    CHECK(SampleList::same_name("z", "Z"));
    CHECK(!SampleList::same_name("ab", "bb"));
    CHECK(!SampleList::in_browsable_folder("XSAMPLES/A.WAV"));
    // Two names equal but for case keep the order they came in.
    CHECK(g_list.add("Kick.wav", false));
    CHECK(g_list.add("KICK.WAV", false));
    CHECK(std::strcmp(g_list.name(1), "Kick.wav") == 0);
    CHECK(std::strcmp(g_list.name(2), "KICK.WAV") == 0);
    // Trailing slashes go, down to the root slash itself.
    CHECK(g_list.open("//"));
    CHECK(std::strcmp(g_list.folder(), "/") == 0);
    CHECK(g_list.open("/SAMPLES//"));
    CHECK(std::strcmp(g_list.folder(), "/SAMPLES") == 0);
}
