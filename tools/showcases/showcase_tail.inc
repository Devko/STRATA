// Shared tail of the showcase generators. The genre's .cpp defines, in an anonymous namespace:
//   const char *kName;  uint16_t kTempoCenti;  int kPatterns;  const char *kPatternNames[];
//   const char *kTrackNames[kTracks];  void build(Project &, SceneSet &, MacroSet &);
//   const Event kMoves[];  int kArrangementBars;  int kStemSlot;
//
//   <genre> OUTDIR [--no-render]   writes OUTDIR/card/ENAGA/DATA/STRATA/PROJECTS/<kName>/*.BIN,
//                                  OUTDIR/audio/*.wav
namespace {

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
    std::printf("wrote %-12s %6u bytes\n", leaf, n);
}

#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "CHECK FAILED: %s\n", what); std::exit(1); } } while (0)

void write_and_verify(const char *dir) {
    mkdirs(dir);
    uint32_t n = 0;
    CHECK(project_write(g_project, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "project_write");
    save(dir, "PROJECT.BIN", n);
    project_init(g_check);
    CHECK(project_read(g_check, g_buf, n) == SerializeResult::ok, "project_read");
    CHECK(g_check.settings.tempo_centi_bpm == kTempoCenti, "tempo");
    CHECK(g_check.settings.chain_length == g_project.settings.chain_length, "chain");
    for (int i = 0; i < 8; i++) CHECK(g_check.settings.mute_states[i] == g_project.settings.mute_states[i], "mutes");
    for (int i = 0; i < kSendFields; i++) CHECK(g_check.settings.sends[i] == g_project.settings.sends[i], "sends");

    CHECK(kit_write(g_project.kit, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "kit_write");
    save(dir, "KIT.BIN", n);
    CHECK(kit_read(g_check.kit, g_buf, n) == SerializeResult::ok, "kit_read");
    for (int t = 0; t < kTracks; t++) {
        CHECK(g_check.kit.tracks[t].machine == g_project.kit.tracks[t].machine, "machine");
        CHECK(std::memcmp(g_check.kit.tracks[t].params, g_project.kit.tracks[t].params, kKitParams) == 0, "params");
        CHECK(g_check.kit.tracks[t].duck == g_project.kit.tracks[t].duck, "duck");
        CHECK(std::fabs(g_check.kit.tracks[t].level - g_project.kit.tracks[t].level) < 0.01f, "level");
    }

    CHECK(perform_write(g_scenes, g_macros, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "perform_write");
    save(dir, "PERFORM.BIN", n);
    CHECK(perform_read(g_scenes_check, g_macros_check, g_buf, n) == SerializeResult::ok, "perform_read");
    CHECK(std::memcmp(&g_scenes_check, &g_scenes, sizeof g_scenes) == 0, "scenes");

    for (int b = 0; b < kBanks; b++) {
        char leaf[16];
        CHECK(bank_file_name(b, leaf, sizeof leaf), "bank name");
        CHECK(bank_write(g_project, b, g_buf, sizeof g_buf, &n) == SerializeResult::ok, "bank_write");
        save(dir, leaf, n);
        int refused = -1;
        CHECK(bank_read(g_check, b, g_buf, n, &refused) == SerializeResult::ok && refused == 0, "bank_read");
    }
    for (int slot = 0; slot < kPatterns; slot++) {
        const Pattern &a = g_project.patterns[0][slot];
        const Pattern &c = g_check.patterns[0][slot];
        CHECK(a.swing == c.swing, "swing");
        for (int t = 0; t < kTracks; t++) {
            CHECK(a.tracks[t].length == c.tracks[t].length && a.tracks[t].speed == c.tracks[t].speed, "track");
            for (int i = 0; i < kStepsPerTrack; i++) {
                const Step &x = a.tracks[t].steps[i], &y = c.tracks[t].steps[i];
                CHECK(x.flags == y.flags && x.note == y.note && x.velocity == y.velocity &&
                      x.length == y.length && x.condition == y.condition && x.ratchet == y.ratchet &&
                      x.chord == y.chord && x.lock_count == y.lock_count && x.probability == y.probability,
                      "step");
            }
        }
    }
}

Engine g_engine;
constexpr int kBlock = int(ENAGA_BLOCK_FRAMES);
constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kMaxFrames = 48000 * 300;
static float g_block[2 * kBlock];
static int16_t g_pcm[2 * kMaxFrames];

void post(Command c) { g_engine.commands().post(c); }
void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0) {
    Command c; c.cmd = cmd; c.track = track; c.step = step; c.a = a; c.b = b; post(c);
}

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

void load_into_engine(bool chain_on) {
    post(Cmd::stop, 0, 0, 1);
    for (int i = 0; i < 3000; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    Project &p = g_engine.project_for_load();
    std::memcpy(&p, &g_project, sizeof p);
    p.settings.chain_enabled = chain_on;
    g_engine.scenes_for_load() = g_scenes;
    g_engine.macros_for_load() = g_macros;
    g_engine.performance_changed();
    post(Cmd::project_loaded);
    for (int i = 0; i < 20; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
}

struct Stats { double peak, rms, balance_db; };

Stats play(int slot, int bars, int tail_bars, int *frames_out, const Event *events, int n_events,
           int solo = -1) {
    FakeHost &host = FakeHost::the();
    Project &p = g_engine.project_for_load();
    p.bank = 0;
    p.slot = uint8_t(slot);
    post(Cmd::project_loaded);
    for (int t = 0; t < kTracks; t++) post(Cmd::set_mute, uint8_t(t), 0, uint8_t(solo >= 0 && t != solo));
    for (int i = 0; i < 20; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    const float bpm = float(kTempoCenti) / 100.0f;
    host.set_tempo(bpm);
    host.set_running(true);
    const double fpt = double(kRate) * 60.0 / (double(bpm) * 96.0);
    const uint32_t ticks = uint32_t(bars) * 384u;
    const int frames = int(double(ticks) * fpt) + int(kRate * 60.0f / bpm * 4.0f * float(tail_bars));
    int done = 0, ev = 0;
    uint32_t next_tick = 0;
    double peak = 0, sum = 0, sum_l = 0, sum_r = 0;
    while (done + kBlock <= frames && done + kBlock <= kMaxFrames) {
        const int bar_now = int(double(done) / (fpt * 384.0));
        while (ev < n_events && events[ev].bar <= bar_now) {
            const Event &e = events[ev++];
            Command c; c.cmd = e.cmd; c.track = e.track; c.a = e.a; c.b = e.b;
            c.u = e.morph;
            post(c);
        }
        while (next_tick < ticks) {
            const double at = double(next_tick) * fpt;
            if (at >= double(done + kBlock)) break;
            host.deliver_tick(next_tick, uint32_t(at - double(done)));
            next_tick++;
        }
        for (float &x : g_block) x = 0.0f;
        g_engine.process(nullptr, g_block, uint32_t(kBlock));
        // App::update's share of the work: the WAVE tables, built a piece at a time on the main
        // context. Without it a WAVE voice plays only its sub (app/engine.hpp, step_waves).
        for (int i = 0; i < 4 && g_engine.step_waves(); i++) {}
        for (int i = 0; i < 2 * kBlock; i++) {
            const float x = g_block[i];
            peak = std::fmax(peak, std::fabs(double(x)));
            sum += double(x) * double(x);
            ((i & 1) ? sum_r : sum_l) += double(x) * double(x);
            const float c = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
            g_pcm[2 * done + i] = int16_t(std::lround(c * 32767.0f));
        }
        done += kBlock;
    }
    host.set_running(false);
    post(Cmd::stop, 0, 0, 1);
    for (int i = 0; i < 3000; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
    *frames_out = done;
    const double balance = sum_l > 0 && sum_r > 0 ? 10.0 * std::log10(sum_l / sum_r) : 0.0;
    return {peak, std::sqrt(sum / double(2 * (done ? done : 1))), balance};
}

double db(double x) { return x > 0 ? 20.0 * std::log10(x) : -999.0; }

constexpr double kMaxBalanceDb = 3.0;
int g_balance_failures = 0;
void check_balance(const char *what, const Stats &s) {
    const bool ok = std::fabs(s.balance_db) <= kMaxBalanceDb;
    std::printf("    balance L-R %+5.2f dB %s\n", s.balance_db, ok ? "ok" : "OFF CENTRE");
    if (!ok) {
        std::fprintf(stderr, "%s: off centre by %+.2f dB\n", what, s.balance_db);
        g_balance_failures++;
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR [--no-render]\n", argv[0]); return 2; }
    const char *out = argv[1];
    build(g_project, g_scenes, g_macros);

    char dir[512];
    std::snprintf(dir, sizeof dir, "%s/card/ENAGA/DATA/STRATA/PROJECTS/%s", out, kName);
    write_and_verify(dir);
    std::printf("files verified (read back with core/'s readers)\n");
    if (argc > 2 && std::strcmp(argv[2], "--no-render") == 0) return 0;

    FakeHost::the().reset();
    if (!g_engine.init(kRate)) { std::fprintf(stderr, "engine init failed\n"); return 1; }
    char path[512];
    std::snprintf(path, sizeof path, "%s/audio", out);
    mkdirs(path);

    load_into_engine(false);
    for (int slot = 0; slot < kPatterns; slot++) {
        int frames = 0;
        const Stats s = play(slot, 8, 1, &frames, nullptr, 0);
        std::snprintf(path, sizeof path, "%s/audio/A%02d_%s.wav", out, slot + 1, kPatternNames[slot]);
        write_wav(path, frames);
        std::printf("A%02d %-7s peak %6.1f dBFS rms %6.1f dBFS\n", slot + 1, kPatternNames[slot], db(s.peak), db(s.rms));
        check_balance(kPatternNames[slot], s);
    }
    for (int t = 0; t < kTracks; t++) {
        int frames = 0;
        const Stats s = play(kStemSlot, 4, 1, &frames, nullptr, 0, t);
        std::snprintf(path, sizeof path, "%s/audio/STEM_A%02d_T%d_%s.wav", out, kStemSlot + 1, t + 1, kTrackNames[t]);
        write_wav(path, frames);
        std::printf("A%02d T%d %-7s peak %6.1f dBFS rms %6.1f dBFS  L-R %+5.2f dB\n", kStemSlot + 1, t + 1,
                    kTrackNames[t], db(s.peak), db(s.rms), s.balance_db);
    }

    load_into_engine(true);
    int frames = 0;
    const Stats s = play(0, kArrangementBars, 3, &frames, kMoves, int(sizeof kMoves / sizeof kMoves[0]));
    std::snprintf(path, sizeof path, "%s/audio/%s_ARRANGEMENT.wav", out, kName);
    write_wav(path, frames);
    std::printf("ARRANGEMENT %.1f s peak %6.1f dBFS rms %6.1f dBFS\n", frames / 48000.0, db(s.peak), db(s.rms));
    check_balance("ARRANGEMENT", s);
    return g_balance_failures == 0 ? 0 : 1;
}
