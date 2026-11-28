// The card builder's starter projects (content/starter/projects/, web/README.md "Starter
// content") open through app/storage.cpp's real open path on this build, as the unit would open
// them from the card. A format change that stops an older file loading fails here before it
// reaches anyone's card.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"
#include "project.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

const char *const kStarterFiles[] = {"PROJECT.BIN", "KIT.BIN",    "PERFORM.BIN", "BANK_A.BIN",
                                     "BANK_B.BIN",  "BANK_C.BIN", "BANK_D.BIN",  "BANK_E.BIN",
                                     "BANK_F.BIN",  "BANK_G.BIN", "BANK_H.BIN"};
uint8_t g_starter_file[1 << 20];

// content/starter/projects, found from this file's own path: the tests run from wherever make
// was started, and the Makefile compiles them by absolute path.
bool starter_dir(char *out, int max) {
    const char *self = __FILE__;
    const char *slash = std::strrchr(self, '/');
    if (!slash) return false;
    const int len = int(slash - self);
    return std::snprintf(out, size_t(max), "%.*s/../content/starter/projects", len, self) < max;
}

// Copy one project folder onto the fake card and open it. False with a message on any failure.
bool open_starter(const char *root, const char *name) {
    FakeHost &host = FakeHost::the();
    char path[1024];
    for (const char *f : kStarterFiles) {
        std::snprintf(path, sizeof path, "%s/%s/%s", root, name, f);
        FILE *in = std::fopen(path, "rb");
        if (!in) { std::printf("    %s: no %s\n", name, f); return false; }
        const size_t n = std::fread(g_starter_file, 1, sizeof g_starter_file, in);
        std::fclose(in);
        std::snprintf(path, sizeof path, "/ENAGA/DATA/STRATA/PROJECTS/%s/%s", name, f);
        if (!host.write_file(path, g_starter_file, uint32_t(n))) return false;
    }
    if (!test::storage().request_open(name) || !test::pump_until_idle()) return false;
    if (test::storage().last_error()) {
        std::printf("    %s: %s\n", name, test::storage().last_error());
        return false;
    }
    return std::strcmp(test::storage().project_name(), name) == 0;
}

}  // namespace

TEST(starter_projects_open_through_storage) {
    char root[512];
    CHECK(starter_dir(root, sizeof root));
    DIR *dir = opendir(root);
    CHECK(dir != nullptr);
    if (!dir) return;
    int opened = 0;
    while (dirent *e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        test::fresh();
        const bool ok = open_starter(root, e->d_name);
        CHECK(ok);
        if (!ok) { std::printf("    starter project %s did not open\n", e->d_name); continue; }
        Engine &engine = test::engine();
        const Project &p = engine.project_for_load();
        // A real project, not a blank one that happened to load: a tempo, a kit that sounds and
        // the pattern it opens on has notes.
        CHECK(p.settings.tempo_centi_bpm >= 6000 && p.settings.tempo_centi_bpm <= 20000);
        int trigs = 0;
        for (int t = 0; t < kTracks; t++) {
            for (int s = 0; s < kMaxSteps; s++) trigs += (engine.pattern().tracks[t].steps[s].flags & kStepTrig) ? 1 : 0;
        }
        CHECK(trigs > 0);
        // And it plays: a bar of its opening pattern (384 ticks, one a block) makes sound.
        FakeHost &host = FakeHost::the();
        host.set_running(true);
        float peak = 0.0f;
        for (uint32_t tick = 0; tick < 384; tick++) {
            host.deliver_tick(tick, 0);
            const float x = test::pump(1);
            peak = x > peak ? x : peak;
        }
        host.set_running(false);
        test::post(Cmd::stop, 0, 0, 1);
        test::pump(4);
        CHECK(peak > 0.01f);
        opened++;
    }
    closedir(dir);
    // The ten shipped today; fewer means a folder went missing.
    CHECK(opened >= 10);
}
