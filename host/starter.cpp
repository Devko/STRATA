// The card builder's starter content (web/, "Starter content"): a tool with two jobs, both run
// through the app's real code on the host so what they write is what the unit would.
//
//   build/host/strata-starter resave SRC NAME OUT
//       Opens the project folder SRC (its PROJECT.BIN, KIT.BIN, PERFORM.BIN and BANK_A-H.BIN)
//       through app/storage.cpp's open path, as PROJECTS/NAME, saves it again through the same
//       Storage, opens what was saved once more, and writes the eleven saved files to OUT. The
//       result is the project as this build's serializers write it: an older file comes out
//       with every optional section this build knows (core/project.hpp, "versions and
//       sections"). Exit 1 if either open or the save fails.
//
//   build/host/strata-starter samples OUT
//       Renders the sample pack: every factory sound of the synth machines (core/sounds.hpp),
//       one note each (a chord on the POLY machines), dry, through the whole track and the
//       master with the glue compressor off, into OUT/<FOLDER>/<NAME>.WAV as 48 kHz 16-bit
//       mono, normalised to -1 dBFS and trimmed where the tail falls under -60 dB. Prints one
//       line a file. GRAIN (it plays a sample), SAMPLE and MID have no sounds to render.
//
// `make -C host starter` builds it; web/tools/starter.mjs runs both jobs and packs the results
// for the site (web/README.md, "Starter content").
#include "apprig.hpp"
#include "fakehost.hpp"
#include "project.hpp"
#include "sounds.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

const char *const kFiles[] = {"PROJECT.BIN", "KIT.BIN",    "PERFORM.BIN", "BANK_A.BIN",
                              "BANK_B.BIN",  "BANK_C.BIN", "BANK_D.BIN",  "BANK_E.BIN",
                              "BANK_F.BIN",  "BANK_G.BIN", "BANK_H.BIN"};
constexpr int kFileCount = int(sizeof kFiles / sizeof kFiles[0]);

// A bank file is about 100 KB; room to spare.
uint8_t g_file[1 << 20];

bool open_through_storage(const char *name) {
    if (!test::storage().request_open(name)) return false;
    if (!test::pump_until_idle()) return false;
    const char *err = test::storage().last_error();
    if (err) {
        std::fprintf(stderr, "%s: %s\n", name, err);
        return false;
    }
    return std::strcmp(test::storage().project_name(), name) == 0;
}

int resave(const char *src, const char *name, const char *out) {
    test::fresh();
    FakeHost &host = FakeHost::the();
    char path[512];
    for (const char *f : kFiles) {
        std::snprintf(path, sizeof path, "%s/%s", src, f);
        FILE *in = std::fopen(path, "rb");
        if (!in) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
        const size_t n = std::fread(g_file, 1, sizeof g_file, in);
        std::fclose(in);
        std::snprintf(path, sizeof path, "/ENAGA/DATA/STRATA/PROJECTS/%s/%s", name, f);
        if (!host.write_file(path, g_file, uint32_t(n))) { std::fprintf(stderr, "fake card refused %s\n", path); return 1; }
    }
    if (!open_through_storage(name)) { std::fprintf(stderr, "%s: did not open\n", name); return 1; }

    // SAVE AS into a scratch folder, then take that folder's files: a save under the project's
    // own name would be refused as NAME TAKEN only for another folder, but a scratch name keeps
    // the source files untouched and proves the save wrote all eleven.
    const char *scratch = "STARTERX";
    if (!test::storage().request_save_as(scratch) || !test::pump_until_idle() ||
        test::storage().last_error()) {
        std::fprintf(stderr, "%s: save failed (%s)\n", name,
                     test::storage().last_error() ? test::storage().last_error() : "not started");
        return 1;
    }
    if (!open_through_storage(scratch)) { std::fprintf(stderr, "%s: the saved copy did not open\n", name); return 1; }

    mkdir(out, 0777);
    for (const char *f : kFiles) {
        std::snprintf(path, sizeof path, "/ENAGA/DATA/STRATA/PROJECTS/%s/%s", scratch, f);
        const int32_t n = host.read_file(path, g_file, sizeof g_file);
        if (n <= 0) { std::fprintf(stderr, "%s: the save left out %s\n", name, f); return 1; }
        std::snprintf(path, sizeof path, "%s/%s", out, f);
        FILE *o = std::fopen(path, "wb");
        if (!o || std::fwrite(g_file, 1, size_t(n), o) != size_t(n)) { std::fprintf(stderr, "cannot write %s\n", path); return 1; }
        std::fclose(o);
    }
    const Project &p = test::engine().project_for_load();
    std::printf("%-8s %6.1f BPM, chain %s (%d entries): opened, saved, opened again, %d files in %s\n",
                name, double(p.settings.tempo_centi_bpm) / 100.0, p.settings.chain_enabled ? "on" : "off",
                int(p.settings.chain_length), kFileCount, out);
    return 0;
}

// --- the sample pack ------------------------------------------------------

constexpr int kRate = int(ENAGA_SAMPLE_RATE);
constexpr int kMaxSeconds = 8;
// The longest file kept: the pack goes into the repository and every card zip, and a pad's last
// seconds of tail add size, not sound a SAMPLE track's own envelope would let through.
constexpr int kKeepSeconds = 3;
float g_mono[kRate * kMaxSeconds];
int16_t g_pcm[kRate * kMaxSeconds];

void le16(FILE *f, uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); }
void le32(FILE *f, uint32_t v) { le16(f, uint16_t(v)); le16(f, uint16_t(v >> 16)); }

bool write_wav(const char *path, const int16_t *pcm, int frames) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t data = uint32_t(frames) * 2u;
    std::fwrite("RIFF", 1, 4, f); le32(f, 36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 1); le32(f, uint32_t(kRate)); le32(f, uint32_t(kRate) * 2u);
    le16(f, 2); le16(f, 16);
    std::fwrite("data", 1, 4, f); le32(f, data);
    for (int i = 0; i < frames; i++) le16(f, uint16_t(pcm[i]));
    return std::fclose(f) == 0;
}

// What one file plays: notes held for `hold` seconds (0: a drum hit, no gate).
struct Take {
    uint8_t machine;
    int preset;
    const uint8_t *notes;
    int note_count;
    float hold;
};

// Track 1 as nothing but `machine` playing preset `preset`: the chain parked open and dry
// (core/factory.cpp's chain defaults), the machine's own defaults, then the preset, centred,
// no sends, LFO, arp, mod slots, duck, chorus or width.
void set_track(const Take &t) {
    Project &p = test::engine().project_for_load();
    for (int tr = 0; tr < kTracks; tr++) {
        KitTrack fresh_track{};
        fresh_track.machine = uint8_t(MachineId::kick);
        fresh_track.level = 0.0f;
        fresh_track.duck = 0;
        p.kit.tracks[tr] = fresh_track;
    }
    KitTrack &k = p.kit.tracks[0];
    k.machine = t.machine;
    k.params[4] = 0;     // DRV
    k.params[5] = 127;   // CUT
    k.params[6] = 20;    // RES
    k.params[7] = 0;     // MOD
    k.params[12] = 0;    // LOF
    k.params[13] = 64;   // PAN
    k.params[14] = 0;    // SNA
    k.params[15] = 0;    // SNB
    machine_defaults_apply(t.machine, k.params);
    sound_preset_apply(t.machine, t.preset, k.params);
    k.level = 0.5f;      // headroom: normalised afterwards
    test::post(Cmd::project_loaded);
    test::pump(4);
}

int render_take(const Take &t) {
    set_track(t);
    test::settle(200, 4000);
    for (int i = 0; i < t.note_count; i++) {
        test::post(Cmd::trigger_note, 0, 0, t.notes[i], 100, t.hold > 0.0f ? 1 : 0);
    }
    const int hold_frames = int(t.hold * float(kRate));
    const int max_frames = kRate * kMaxSeconds;
    bool released = t.hold <= 0.0f;
    int frames = 0;
    int quiet = 0;
    while (frames + test::kRigBlock <= max_frames) {
        if (!released && frames >= hold_frames) {
            for (int i = 0; i < t.note_count; i++) test::post(Cmd::release_note, 0, 0, t.notes[i]);
            released = true;
        }
        test::pump(1);
        const float *b = test::last_block();
        float peak = 0.0f;
        for (int i = 0; i < test::kRigBlock; i++) {
            g_mono[frames + i] = b[2 * i];
            peak = std::fmax(peak, std::fabs(b[2 * i]));
        }
        frames += test::kRigBlock;
        // A quarter second of near silence after the release: the sound is over.
        quiet = (released && peak < 1e-4f) ? quiet + 1 : 0;
        if (quiet * test::kRigBlock > kRate / 4) break;
    }
    return frames;
}

// Normalise to -1 dBFS, trim the start to the first sound and the end where the level falls under
// -54 dB of the peak (at most kKeepSeconds, faded out over its last quarter second when cut
// there), fade the last 5 ms, convert. Returns the frame count, 0 for silence.
int finish(int frames) {
    float peak = 0.0f;
    for (int i = 0; i < frames; i++) peak = std::fmax(peak, std::fabs(g_mono[i]));
    if (peak < 1e-5f) return 0;
    int first = 0;
    while (first < frames && std::fabs(g_mono[first]) < peak * 1e-3f) first++;
    int last = frames - 1;
    while (last > first && std::fabs(g_mono[last]) < peak * 2e-3f) last--;
    const bool cut = last - first + 1 > kRate * kKeepSeconds;
    const int n = cut ? kRate * kKeepSeconds : last - first + 1;
    const float gain = 0.891f / peak;   // -1 dBFS
    const int fade = cut ? kRate / 4 : kRate / 200;
    for (int i = 0; i < n; i++) {
        float x = g_mono[first + i] * gain;
        if (i >= n - fade) x *= float(n - 1 - i) / float(fade);
        g_pcm[i] = int16_t(std::lround(x * 32767.0f));
    }
    return n;
}

struct Group {
    MachineId machine;
    const char *folder;   // under OUT
    const char *prefix;   // the file name's start
    bool chord;           // a minor seventh instead of one note
    uint8_t note;         // the (lowest) note
    float hold;           // seconds; 0 for a drum
};

// The note each melodic file plays is in its name, so a SAMPLE track playing it at C4 knows
// what it hears. Basses on C2, leads on C3; chords are C minor seventh from C3 ("M7" in the
// name: the 5x7 font has no lower case for "m7").
const Group kGroups[] = {
    {MachineId::kick,      "DRUMS",  "KICK", false, 60, 0.0f},
    {MachineId::snare,     "DRUMS",  "SNARE", false, 60, 0.0f},
    {MachineId::hat,       "DRUMS",  "HAT", false, 60, 0.0f},
    {MachineId::perc,      "PERC",   "PERC", false, 60, 0.0f},
    {MachineId::va_mono,   "BASS",   "MONO", false, 36, 0.35f},
    {MachineId::wave_mono, "BASS",   "WAVE", false, 36, 0.35f},
    {MachineId::va_poly,   "CHORDS", "POLY", true, 48, 0.6f},
    {MachineId::wave_poly, "CHORDS", "WAVE", true, 48, 0.6f},
    {MachineId::fm,        "CHORDS", "FM4", true, 48, 0.6f},
};

// A few presets are not what their machine's group is: a bass on a POLY machine, a lead on a
// MONO one. They go where a user would look for them, on the note that suits them.
Group regroup(const Group &g, const char *name) {
    Group r = g;
    if (std::strstr(name, "BAS")) {                       // FM4 BASS, MONO DUBBAS
        r.folder = "BASS"; r.chord = false; r.note = 36; r.hold = 0.35f;
    } else if (!g.chord && g.hold > 0.0f &&
               (!std::strcmp(name, "LEAD") || !std::strcmp(name, "SYNC") ||
                !std::strcmp(name, "DRONE") || !std::strcmp(name, "GLIDE"))) {
        r.folder = "LEADS"; r.note = 48; r.hold = 0.6f;
    }
    return r;
}

const char *note_name(uint8_t note, char *buf) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(buf, 8, "%s%d", names[note % 12], note / 12 - 1);
    return buf;
}

int samples(const char *out) {
    test::fresh();
    test::set_master_byte(uint8_t(MasterField::compressor), 0);
    test::set_master_byte(uint8_t(MasterField::limit), 127);
    mkdir(out, 0777);
    int files = 0;
    double seconds = 0.0;
    char path[512], nb[8];
    for (const Group &group : kGroups) {
        const uint8_t m = uint8_t(group.machine);
        for (int i = 0; i < sound_preset_count(m); i++) {
            const SoundPreset *s = sound_preset(m, i);
            const Group g = regroup(group, s->name);
            std::snprintf(path, sizeof path, "%s/%s", out, g.folder);
            mkdir(path, 0777);
            const uint8_t chord[4] = {g.note, uint8_t(g.note + 3), uint8_t(g.note + 7), uint8_t(g.note + 10)};
            const uint8_t one[1] = {g.note};
            const Take t{m, i, g.chord ? chord : one, g.chord ? 4 : 1, g.hold};
            const int n = finish(render_take(t));
            if (n == 0) { std::fprintf(stderr, "%s %s: silent\n", g.prefix, s->name); return 1; }
            if (g.hold <= 0.0f) {
                std::snprintf(path, sizeof path, "%s/%s/%s_%s.WAV", out, g.folder, g.prefix, s->name);
            } else {
                std::snprintf(path, sizeof path, "%s/%s/%s_%s_%s%s.WAV", out, g.folder, g.prefix, s->name,
                              note_name(g.note, nb), g.chord ? "M7" : "");
            }
            if (!write_wav(path, g_pcm, n)) { std::fprintf(stderr, "cannot write %s\n", path); return 1; }
            std::printf("%-40s %5.2f s\n", std::strrchr(path, '/') + 1, double(n) / kRate);
            files++;
            seconds += double(n) / kRate;
        }
    }
    std::printf("%d files, %.1f s, %.1f MB\n", files, seconds, seconds * kRate * 2.0 / 1e6);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "resave") == 0) return resave(argv[2], argv[3], argv[4]);
    if (argc == 3 && std::strcmp(argv[1], "samples") == 0) return samples(argv[2]);
    std::fprintf(stderr, "usage: %s resave SRC NAME OUT | samples OUT\n", argv[0]);
    return 2;
}
