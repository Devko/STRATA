// A STRATA project from als2strata.py's project.inc: the files, read back, and renders through the
// real Engine (host build) with the project's samples loaded, as the unit would play them.
//
//   import_project OUTDIR [--no-render] [--stems]
//
// writes OUTDIR/card/ENAGA/DATA/STRATA/PROJECTS/<kName>/*.BIN, OUTDIR/audio/<kName>_ARRANGEMENT.wav
// (the chain, start to end) and with --stems one solo render of the chain per track,
// OUTDIR/audio/STEM_T<n>_<NAME>.wav. Built by build_import.sh with -include of the generated
// project.inc's folder on the include path; not part of the device or test build.
#include "showcase_head.inc"
#include "imported.inc"
#include "project.inc"

namespace {

constexpr int kStemSlot = 0;
const char *const kTrackNames[kTracks] = {
    kImportTracks[0].name, kImportTracks[1].name, kImportTracks[2].name, kImportTracks[3].name,
    kImportTracks[4].name, kImportTracks[5].name, kImportTracks[6].name, kImportTracks[7].name,
};

// SLC's knob for n equal slices: dsp/sample.cpp reads 1 + round(knob / 127 * 15).
uint8_t slice_knob(int n) { return uint8_t((n - 1) * 127 / 15 + ((n - 1) * 127 % 15 ? 1 : 0)); }

void kit(Kit &kit) {
    clear_kit(kit);
    for (int t = 0; t < kTracks; t++) {
        const ImportTrack &it = kImportTracks[t];
        KitTrack &k = kit.tracks[t];
        k.machine = uint8_t(it.machine);
        if (it.preset) {
            preset(k, MachineId(it.machine), it.preset);
        } else {
            machine_defaults_apply(k.machine, k.params);
        }
        if (it.machine == int(MachineId::sample)) {
            // The whole sample, cut into the importer's equal slices; no attack, held to the
            // slice's end (DEC 127), forward one-shot, full level.
            const uint8_t src[4] = {64, 0, 127, slice_knob(it.slices > 0 ? it.slices : 1)};
            const uint8_t amp[4] = {0, 127, 0, 127};
            for (int i = 0; i < 4; i++) { k.params[kSrc0 + i] = src[i]; k.params[kAmp0 + i] = amp[i]; }
            k.slice_mode = kSliceEqual;
            if (it.card) std::snprintf(k.sample, sizeof k.sample, "%s", it.card);
        }
        float level = it.level;
        if (level > 1.5f) level = 1.5f;     // the MIXER's ceiling
        k.level = level;
        k.params[kPan] = uint8_t(it.pan);
        k.params[kSna] = uint8_t(it.sna);
        k.params[kSnb] = uint8_t(it.snb);
        k.duck = uint8_t(it.duck);
    }
    for (const ImportParam &p : kImportParams)
        if (p.track >= 0) kit.tracks[p.track].params[p.slot] = uint8_t(p.value);
    kit.duck.source = 0;
    kit.duck.attack_ms = 2;
    kit.duck.hold = 4;
    kit.duck.release = 40;
}

void build(Project &project, SceneSet &scenes, MacroSet &macros) {
    project_init(project);
    perform_init(scenes, macros);
    kit(project.kit);
    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = kTempoCenti;
    s.root = uint8_t(48 + kRoot);
    s.scale = uint8_t(kScale);
    for (int i = 0; i < kPatterns; i++) {
        Pattern &p = project.patterns[i / kPatternsPerBank][i % kPatternsPerBank];
        begin(p, 0x1A500u + uint32_t(i), kRoot, kScale, kSwing);
        place_all(p, kPatternFrom[i]);
    }
    for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
    for (int i = 0; i < kChainLength; i++) {
        const int pat = kImportChain[i].pattern;
        project.chain[i] = ChainEntry{uint8_t(pat / kPatternsPerBank), uint8_t(pat % kPatternsPerBank),
                                      uint8_t(kImportChain[i].repeats), 0};
    }
    project.settings.chain_length = uint8_t(kChainLength);
    project.settings.chain_enabled = true;
    project.settings.chain_song = false;
    project.bank = 0;
    project.slot = 0;
}

static uint8_t g_buf[kBankMaxBytes + 65536];
static Project g_project, g_check;
static SceneSet g_scenes, g_scenes_check;
static MacroSet g_macros, g_macros_check;

void mkdirs(const char *path) {
    char tmp[512];
    std::snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

void save(const char *dir, const char *leaf, uint32_t n) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = std::fopen(path, "wb");
    if (!f || std::fwrite(g_buf, 1, n, f) != n) { std::fprintf(stderr, "write %s\n", path); std::exit(1); }
    std::fclose(f);
}

#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "CHECK FAILED: %s\n", what); std::exit(1); } } while (0)

void write_and_verify(const char *dir) {
    mkdirs(dir);
    uint32_t n = 0;
    CHECK(project_write(g_project, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "project_write");
    save(dir, "PROJECT.BIN", n);
    project_init(g_check);
    CHECK(project_read(g_check, g_buf, n) == SerializeResult::ok, "project_read");
    CHECK(g_check.settings.chain_length == g_project.settings.chain_length, "chain");
    for (int i = 0; i < kChainLength; i++)
        CHECK(std::memcmp(&g_check.chain[i], &g_project.chain[i], sizeof(ChainEntry)) == 0, "chain entry");
    CHECK(kit_write(g_project.kit, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "kit_write");
    save(dir, "KIT.BIN", n);
    CHECK(kit_read(g_check.kit, g_buf, n) == SerializeResult::ok, "kit_read");
    for (int t = 0; t < kTracks; t++) {
        CHECK(g_check.kit.tracks[t].machine == g_project.kit.tracks[t].machine, "machine");
        CHECK(std::memcmp(g_check.kit.tracks[t].params, g_project.kit.tracks[t].params, kKitParams) == 0, "params");
        CHECK(std::strcmp(g_check.kit.tracks[t].sample, g_project.kit.tracks[t].sample) == 0, "sample path");
    }
    CHECK(perform_write(g_scenes, g_macros, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "perform_write");
    save(dir, "PERFORM.BIN", n);
    CHECK(perform_read(g_scenes_check, g_macros_check, g_buf, n) == SerializeResult::ok, "perform_read");
    for (int b = 0; b < kBanks; b++) {
        char leaf[16];
        CHECK(bank_file_name(b, leaf, sizeof leaf), "bank name");
        CHECK(bank_write(g_project, b, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "bank_write");
        save(dir, leaf, n);
        int refused = -1;
        CHECK(bank_read(g_check, b, g_buf, n, &refused) == SerializeResult::ok && refused == 0, "bank_read");
    }
    for (int i = 0; i < kPatterns; i++) {
        const Pattern &a = g_project.patterns[i / kPatternsPerBank][i % kPatternsPerBank];
        const Pattern &c = g_check.patterns[i / kPatternsPerBank][i % kPatternsPerBank];
        for (int t = 0; t < kTracks; t++)
            for (int s = 0; s < kStepsPerTrack; s++) {
                const Step &x = a.tracks[t].steps[s], &y = c.tracks[t].steps[s];
                CHECK(x.flags == y.flags && x.note == y.note && x.velocity == y.velocity &&
                      x.length == y.length && x.chord == y.chord && x.micro == y.micro, "step");
            }
    }
}

// --- rendering ---------------------------------------------------------------

Engine g_engine;
constexpr int kBlock = int(ENAGA_BLOCK_FRAMES);
constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kMaxFrames = 48000 * 600;
static float g_block[2 * kBlock];
static int16_t g_pcm[2 * kMaxFrames];
constexpr uint32_t kRegionBytes = 64u << 20;
static int16_t g_region[kRegionBytes / 2];
uint32_t g_region_used = 0;

void post(Command c) { g_engine.commands().post(c); }
void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0) {
    Command c; c.cmd = cmd; c.track = track; c.step = step; c.a = a; c.b = b; post(c);
}
void run_blocks(int n) { for (int i = 0; i < n; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock)); }

void le16(FILE *f, uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); }
void le32(FILE *f, uint32_t v) { le16(f, uint16_t(v)); le16(f, uint16_t(v >> 16)); }
void write_wav(const char *path, int frames) {
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
    const uint32_t data = uint32_t(frames) * 4u;
    std::fwrite("RIFF", 1, 4, f); le32(f, 36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 2); le32(f, 48000); le32(f, 48000 * 4); le16(f, 4); le16(f, 16);
    std::fwrite("data", 1, 4, f); le32(f, data);
    for (int i = 0; i < 2 * frames; i++) le16(f, uint16_t(g_pcm[i]));
    std::fclose(f);
}

uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The importer's WAVs are 16-bit PCM at 48 kHz, mono or stereo: copied into the sample region
// and handed to the track as the loader would.
void load_sample(int track, const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(1); }
    uint8_t head[12];
    CHECK(std::fread(head, 1, 12, f) == 12 && std::memcmp(head, "RIFF", 4) == 0, "RIFF");
    int channels = 0;
    uint32_t bytes = 0;
    for (;;) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        const uint32_t size = rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            CHECK(size >= 16 && std::fread(fmt, 1, 16, f) == 16, "fmt");
            channels = fmt[2];
            CHECK(fmt[0] == 1 && (fmt[14] | fmt[15] << 8) == 16 && rd32(fmt + 4) == 48000, "16-bit 48 kHz PCM");
            std::fseek(f, long(size - 16 + (size & 1)), SEEK_CUR);
        } else if (std::memcmp(ch, "data", 4) == 0) {
            bytes = size;
            break;
        } else {
            std::fseek(f, long(size + (size & 1)), SEEK_CUR);
        }
    }
    CHECK(channels == 1 || channels == 2, "channels");
    CHECK(g_region_used + bytes <= kRegionBytes, "64 MiB of samples");
    const uint32_t offset = g_region_used;
    CHECK(std::fread(reinterpret_cast<uint8_t *>(g_region) + offset, 1, bytes, f) == bytes, "data");
    std::fclose(f);
    g_region_used += (bytes + 3u) & ~3u;
    const uint32_t frames = bytes / uint32_t(2 * channels);
    Command c;
    c.cmd = Cmd::set_sample;
    c.track = uint8_t(track);
    c.a = uint8_t(channels);
    c.u = offset;
    c.c = uint8_t(frames & 0xFF);
    c.d = uint8_t((frames >> 8) & 0xFF);
    c.e = uint8_t(frames >> 16);
    post(c);
    run_blocks(4);
    std::printf("T%d %-8s %s, %u frames x %d\n", track + 1, kTrackNames[track], path, frames, channels);
}

void load_into_engine() {
    post(Cmd::stop, 0, 0, 1);
    run_blocks(3000);
    Project &p = g_engine.project_for_load();
    std::memcpy(&p, &g_project, sizeof p);
    g_engine.scenes_for_load() = g_scenes;
    g_engine.macros_for_load() = g_macros;
    g_engine.performance_changed();
    post(Cmd::project_loaded);
    run_blocks(20);
    for (int t = 0; t < kTracks; t++)
        if (kImportTracks[t].wav) load_sample(t, kImportTracks[t].wav);
}

struct Stats { double peak, rms; };

Stats play_chain(int bars, int *frames_out, int solo) {
    FakeHost &host = FakeHost::the();
    Project &p = g_engine.project_for_load();
    p.bank = 0;
    p.slot = 0;
    post(Cmd::project_loaded);
    for (int t = 0; t < kTracks; t++) post(Cmd::set_mute, uint8_t(t), 0, uint8_t(solo >= 0 && t != solo));
    run_blocks(20);
    const float bpm = float(kTempoCenti) / 100.0f;
    host.set_tempo(bpm);
    host.set_running(true);
    const double fpt = double(kRate) * 60.0 / (double(bpm) * 96.0);
    const uint32_t ticks = uint32_t(bars) * 384u;
    const int frames = int(double(ticks) * fpt) + int(kRate * 60.0f / bpm * 4.0f);
    int done = 0;
    uint32_t next_tick = 0;
    double peak = 0, sum = 0;
    while (done + kBlock <= frames && done + kBlock <= kMaxFrames) {
        while (next_tick < ticks) {
            const double at = double(next_tick) * fpt;
            if (at >= double(done + kBlock)) break;
            host.deliver_tick(next_tick, uint32_t(at - double(done)));
            next_tick++;
        }
        for (float &x : g_block) x = 0.0f;
        g_engine.process(nullptr, g_block, uint32_t(kBlock));
        for (int i = 0; i < 4 && g_engine.step_waves(); i++) {}
        for (int i = 0; i < 2 * kBlock; i++) {
            const float x = g_block[i];
            peak = std::fmax(peak, std::fabs(double(x)));
            sum += double(x) * double(x);
            const float c = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
            g_pcm[2 * done + i] = int16_t(std::lround(c * 32767.0f));
        }
        done += kBlock;
    }
    host.set_running(false);
    post(Cmd::stop, 0, 0, 1);
    run_blocks(3000);
    *frames_out = done;
    return {peak, std::sqrt(sum / double(2 * (done ? done : 1)))};
}

double db(double x) { return x > 0 ? 20.0 * std::log10(x) : -999.0; }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR [--no-render] [--stems]\n", argv[0]); return 2; }
    const char *out = argv[1];
    bool render = true, stems = false;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-render") == 0) render = false;
        if (std::strcmp(argv[i], "--stems") == 0) stems = true;
    }
    build(g_project, g_scenes, g_macros);
    char dir[512];
    std::snprintf(dir, sizeof dir, "%s/card/ENAGA/DATA/STRATA/PROJECTS/%s", out, kName);
    write_and_verify(dir);
    std::printf("%s: %d patterns, chain of %d, %d bars; files verified\n", kName, kPatterns, kChainLength,
                kArrangementBars);
    if (!render) return 0;

    FakeHost::the().reset();
    if (!g_engine.init(kRate)) { std::fprintf(stderr, "engine init failed\n"); return 1; }
    g_engine.set_sample_region(g_region, kRegionBytes);
    char path[512];
    std::snprintf(path, sizeof path, "%s/audio", out);
    mkdirs(path);
    load_into_engine();
    int frames = 0;
    Stats s = play_chain(kArrangementBars, &frames, -1);
    std::snprintf(path, sizeof path, "%s/audio/%s_ARRANGEMENT.wav", out, kName);
    write_wav(path, frames);
    std::printf("ARRANGEMENT %.1f s peak %6.1f dBFS rms %6.1f dBFS\n", frames / 48000.0, db(s.peak), db(s.rms));
    if (stems) {
        for (int t = 0; t < kTracks; t++) {
            s = play_chain(kArrangementBars, &frames, t);
            std::snprintf(path, sizeof path, "%s/audio/STEM_T%d_%s.wav", out, t + 1, kTrackNames[t]);
            write_wav(path, frames);
            std::printf("T%d %-8s peak %6.1f dBFS rms %6.1f dBFS\n", t + 1, kTrackNames[t], db(s.peak), db(s.rms));
        }
    }
    return 0;
}
