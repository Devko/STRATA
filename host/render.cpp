// The offline renderer docs/SPEC.md's testing table asks for: "an offline renderer that writes N
// bars to WAV". It drives app/'s Engine directly on the host, against tests/fakehost.cpp's clock,
// so it needs no emulator and no cross toolchain -- it plays the SAME engine code the unit runs,
// built with the host compiler (so its CPU figures mean nothing; tools/render.sh is the emulator
// route, sample for sample the unit's output).
//
//   make -C host render                     # every factory pattern, 8 bars each
//   make -C host render BARS=4
//   build/host/strata-render OUTDIR [BARS] [STEMS]   # STEMS 1-4: whose tracks to solo
//
// Writes OUTDIR/A01.wav .. A04.wav (the mix), OUTDIR/A01_T1.wav .. _T8.wav (each track of A01
// soloed, so levels can be compared), and prints peak and RMS for each, which is what the
// factory kit's levels were balanced against.
#include "engine.hpp"
#include "factory.hpp"
#include "fakehost.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <enaga/abi.h>

using namespace strata;
using strata::test::FakeHost;

namespace {

Engine g_engine;
constexpr int kBlock = int(ENAGA_BLOCK_FRAMES);
constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr float kBpm = 130.0f;
float g_block[2 * kBlock];
// 8 bars at 130 BPM is 14.8 s; room for 32.
constexpr int kMaxFrames = 48000 * 64;
int16_t g_pcm[2 * kMaxFrames];

void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0) {
    Command c;
    c.cmd = cmd;
    c.track = track;
    c.step = step;
    c.a = a;
    c.b = b;
    g_engine.commands().post(c);
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

// Silence the engine between renders so no tail leaks into the next file.
void settle() {
    post(Cmd::stop, 0, 0, 1);
    for (int i = 0; i < 3000; i++) g_engine.process(nullptr, g_block, uint32_t(kBlock));
}

struct Stats { float peak; float rms; };

// Play pattern `slot` of bank A from its first tick for `bars` bars, with `solo` (or -1 for all).
Stats render(int slot, int bars, int solo, int *frames_out) {
    FakeHost &host = FakeHost::the();
    Project &p = g_engine.project_for_load();
    p.bank = 0;
    p.slot = uint8_t(slot);
    post(Cmd::project_loaded);
    for (int t = 0; t < kTracks; t++) post(Cmd::set_mute, uint8_t(t), 0, uint8_t(solo >= 0 && t != solo));
    settle();
    for (int t = 0; t < kTracks; t++) post(Cmd::set_mute, uint8_t(t), 0, uint8_t(solo >= 0 && t != solo));

    host.set_tempo(kBpm);
    host.set_running(true);
    const double frames_per_tick = double(kRate) * 60.0 / (double(kBpm) * 96.0);
    const uint32_t ticks = uint32_t(bars) * 384u;
    // One extra bar of tail.
    const int frames = int(double(ticks) * frames_per_tick) + int(kRate * 60.0f / kBpm * 4.0f);
    int done = 0;
    uint32_t next_tick = 0;
    double peak = 0.0, sum = 0.0;
    while (done + kBlock <= frames && done + kBlock <= kMaxFrames) {
        while (next_tick < ticks) {
            const double at = double(next_tick) * frames_per_tick;
            if (at >= double(done + kBlock)) break;
            host.deliver_tick(next_tick, uint32_t(at - double(done)));
            next_tick++;
        }
        for (float &x : g_block) x = 0.0f;
        g_engine.process(nullptr, g_block, uint32_t(kBlock));
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
    *frames_out = done;
    return {float(peak), float(std::sqrt(sum / double(2 * done)))};
}

float db(float x) { return x > 0.0f ? 20.0f * std::log10(x) : -999.0f; }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR [BARS]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    const int bars = argc > 2 && argv[2][0] ? std::atoi(argv[2]) : 8;
    // The pattern whose tracks are soloed one by one, 1-4.
    const int stems = argc > 3 ? std::atoi(argv[3]) - 1 : 0;
    FakeHost::the().reset();
    if (!g_engine.init(kRate)) { std::fprintf(stderr, "engine init failed\n"); return 1; }

    char path[512];
    static const char *names[kFactoryPatterns] = {"GROOVE", "GROOVE+", "BREAK", "PEAK"};
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        int frames = 0;
        const Stats s = render(slot, bars, -1, &frames);
        std::snprintf(path, sizeof path, "%s/A%02d.wav", dir, slot + 1);
        write_wav(path, frames);
        std::printf("A%02d %-8s peak %6.1f dBFS  rms %6.1f dBFS  %s\n", slot + 1, names[slot],
                    db(s.peak), db(s.rms), path);
    }
    static const char *tracks[kTracks] = {"KICK", "CLAP", "HAT C", "HAT O", "PERC", "BASS", "CHORD", "LOOP"};
    for (int t = 0; t < kTracks; t++) {
        int frames = 0;
        const Stats s = render(stems, bars, t, &frames);
        std::snprintf(path, sizeof path, "%s/A%02d_T%d.wav", dir, stems + 1, t + 1);
        write_wav(path, frames);
        std::printf("A%02d T%d %-6s peak %6.1f dBFS  rms %6.1f dBFS\n", stems + 1, t + 1, tracks[t],
                    db(s.peak), db(s.rms));
    }
    return 0;
}
