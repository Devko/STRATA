// The offline renderer docs/SPEC.md's testing table asks for: "an offline renderer that writes N
// bars to WAV". It drives app/'s Engine directly on the host, against tests/fakehost.cpp's clock,
// so it needs no emulator and no cross toolchain -- it plays the SAME engine code the unit runs,
// built with the host compiler (so its CPU figures mean nothing; tools/render.sh is the emulator
// route, sample for sample the unit's output).
//
//   make -C host render                     # every factory pattern, 8 bars each
//   make -C host render BARS=4
//   build/host/strata-render OUTDIR [BARS] [STEMS]   # STEMS 1-5: whose tracks to solo
//
// Writes OUTDIR/A01.wav .. A05.wav (the mix), OUTDIR/A01_T1.wav .. _T8.wav (each track of A01
// soloed, so levels can be compared), and prints peak and RMS for each, which is what the
// factory kit's levels were balanced against, and each mix's L-R balance: a mix more than
// 3 dB off centre fails the render (exit 1).
#include "engine.hpp"
#include "factory.hpp"
#include "fakehost.hpp"
#include "sounds.hpp"
#include "arppresets.hpp"
#include "pattern.hpp"

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

struct Stats { float peak; float rms; float balance_db; };   // balance: L over R RMS, dB

// Play pattern `slot` of bank A from its first tick for `bars` bars, with `solo` (or -1 for all).
Stats render(int slot, int bars, int solo, int *frames_out, int tail_bars = 1) {
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
    // An extra bar of tail, or more.
    const int frames = int(double(ticks) * frames_per_tick) +
                       int(kRate * 60.0f / kBpm * 4.0f * float(tail_bars));
    int done = 0;
    uint32_t next_tick = 0;
    double peak = 0.0, sum = 0.0, sum_l = 0.0, sum_r = 0.0;
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
            ((i & 1) ? sum_r : sum_l) += double(x) * double(x);
            const float c = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
            g_pcm[2 * done + i] = int16_t(std::lround(c * 32767.0f));
        }
        done += kBlock;
    }
    host.set_running(false);
    *frames_out = done;
    const double balance = sum_l > 0.0 && sum_r > 0.0 ? 10.0 * std::log10(sum_l / sum_r) : 0.0;
    return {float(peak), float(std::sqrt(sum / double(2 * done))), float(balance)};
}

float db(float x) { return x > 0.0f ? 20.0f * std::log10(x) : -999.0f; }

// A full mix sits in the middle. More than 3 dB between L and R means something is panned
// that should not be -- the TECHNO showcase shipped with its kick, clap and bass at PAN 0, hard
// left, because its kit started from zeroed bytes. A mix off centre fails the render; a soloed
// track may lean (a panned hat is meant to), so stems only print it.
constexpr float kMaxBalanceDb = 3.0f;
int g_off_centre = 0;
void check_balance(const char *what, const Stats &s) {
    const bool ok = std::fabs(s.balance_db) <= kMaxBalanceDb;
    std::printf("    balance L-R %+5.2f dB  %s\n", double(s.balance_db), ok ? "ok" : "OFF CENTRE");
    if (!ok) {
        std::fprintf(stderr, "%s: the mix is off centre by %+.2f dB\n", what, double(s.balance_db));
        g_off_centre++;
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTDIR [BARS 1-24] [STEMS 1-5]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    const int bars = argc > 2 && argv[2][0] ? std::atoi(argv[2]) : 8;
    // The pattern whose tracks are soloed one by one, 1-5.
    const int stems = argc > 3 ? std::atoi(argv[3]) - 1 : 0;
    if (stems < 0 || stems >= kFactoryPatterns || bars < 1 || bars > 24) {
        std::fprintf(stderr, "usage: %s OUTDIR [BARS 1-24] [STEMS 1-%d]\n", argv[0], kFactoryPatterns);
        return 2;
    }
    FakeHost::the().reset();
    if (!g_engine.init(kRate)) { std::fprintf(stderr, "engine init failed\n"); return 1; }

    char path[512];
    static const char *names[kFactoryPatterns] = {"GROOVE", "GROOVE+", "BREAK", "PEAK", "PAD"};
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        int frames = 0;
        const Stats s = render(slot, bars, -1, &frames);
        std::snprintf(path, sizeof path, "%s/A%02d.wav", dir, slot + 1);
        write_wav(path, frames);
        std::printf("A%02d %-8s peak %6.1f dBFS  rms %6.1f dBFS  %s\n", slot + 1, names[slot],
                    db(s.peak), db(s.rms), path);
        check_balance(names[slot], s);
    }
    // A01 again through the sends' ambient modes (dsp/ambient.hpp), for a pre-listen: the LONG
    // delay at dotted 1/8 x4 with wow and diffusion, into a HALL with some shimmer, and four bars
    // of tail. The project's bytes are set and put back, so nothing after this hears them.
    {
        Project &p = g_engine.project_for_load();
        uint8_t saved[kSendFields];
        for (int i = 0; i < kSendFields; i++) saved[i] = p.settings.sends[i];
        uint8_t *sends = p.settings.sends;
        sends[int(SendField::delay_mode)] = 127;
        sends[int(SendField::delay_multiplier)] = delay_multiplier_byte(3);   // X4
        sends[int(SendField::delay_feedback)] = 100;
        sends[int(SendField::delay_wow)] = 70;
        sends[int(SendField::delay_diffusion)] = 96;
        sends[int(SendField::delay_to_reverb)] = 64;
        sends[int(SendField::reverb_mode)] = 127;
        sends[int(SendField::reverb_decay_s)] = 100;
        sends[int(SendField::reverb_shimmer)] = 70;
        int frames = 0;
        const Stats s = render(0, bars, -1, &frames, 4);
        std::snprintf(path, sizeof path, "%s/A01_AMBIENT.wav", dir);
        write_wav(path, frames);
        std::printf("A01 AMBIENT  peak %6.1f dBFS  rms %6.1f dBFS  %s\n", db(s.peak), db(s.rms), path);
        check_balance("A01 AMBIENT", s);
        Project &q = g_engine.project_for_load();
        for (int i = 0; i < kSendFields; i++) q.settings.sends[i] = saved[i];
    }

    // A05 PAD again with the reverb in HALL mode -- a project setting (MIXER, REV+), so the
    // factory leaves it at ROOM for A01-A04's sake, but the pad is what HALL is for.
    {
        Project &p = g_engine.project_for_load();
        const uint8_t saved = p.settings.sends[int(SendField::reverb_mode)];
        p.settings.sends[int(SendField::reverb_mode)] = 127;
        int frames = 0;
        const Stats s = render(kFactoryPadSlot, bars, -1, &frames, 2);
        std::snprintf(path, sizeof path, "%s/A05_HALL.wav", dir);
        write_wav(path, frames);
        std::printf("A05 HALL     peak %6.1f dBFS  rms %6.1f dBFS  %s\n", db(s.peak), db(s.rms), path);
        check_balance("A05 HALL", s);
        g_engine.project_for_load().settings.sends[int(SendField::reverb_mode)] = saved;
    }

    static const char *tracks[kTracks] = {"KICK", "CLAP", "HAT C", "HAT O", "PERC", "BASS", "CHORD", "LOOP"};
    for (int t = 0; t < kTracks; t++) {
        int frames = 0;
        const Stats s = render(stems, bars, t, &frames);
        std::snprintf(path, sizeof path, "%s/A%02d_T%d.wav", dir, stems + 1, t + 1);
        write_wav(path, frames);
        std::printf("A%02d T%d %-6s peak %6.1f dBFS  rms %6.1f dBFS  balance %+5.2f dB\n", stems + 1,
                    t + 1, tracks[t], db(s.peak), db(s.rms), double(s.balance_db));
    }

    // Every factory sound (core/sounds.hpp) on its factory track, soloed in A01, two bars each:
    // OUTDIR/SOUNDS_T<track>_<NAME>.wav, with peak and RMS against the factory sound (entry 0),
    // which is what the presets' LVL values were balanced by. The kit is put back after each.
    static const int sound_tracks[] = {0, 1, 2, 3, 4, 5, 6};
    for (int t : sound_tracks) {
        Project &p = g_engine.project_for_load();
        KitTrack &k = p.kit.tracks[t];
        uint8_t saved[kKitParams];
        for (int i = 0; i < kKitParams; i++) saved[i] = k.params[i];
        const uint8_t machine = k.machine;
        for (int i = 0; i < sound_preset_count(machine); i++) {
            Project &q = g_engine.project_for_load();
            sound_preset_apply(machine, i, q.kit.tracks[t].params);
            post(Cmd::kit_recall, 0, 0, 0);
            int frames = 0;
            const Stats s = render(0, 2, t, &frames);
            const char *name = sound_preset(machine, i)->name;
            std::snprintf(path, sizeof path, "%s/SOUNDS_T%d_%s.wav", dir, t + 1, name);
            write_wav(path, frames);
            std::printf("SOUND T%d %-6s peak %6.1f dBFS  rms %6.1f dBFS\n", t + 1, name, db(s.peak),
                        db(s.rms));
        }
        Project &q = g_engine.project_for_load();
        for (int i = 0; i < kKitParams; i++) q.kit.tracks[t].params[i] = saved[i];
        post(Cmd::kit_recall, 0, 0, 0);
    }

    // Every factory arp (core/arppresets.hpp) over A05's chords, in the whole mix: the CHORD
    // track's pad locks taken off and its sound VA POLY's PLUCK, so each bar's chord is walked
    // by the arp for the bar. Four bars and one of tail each: OUTDIR/ARPS_<NAME>.wav. The
    // pattern and the track are put back after.
    {
        constexpr int kChordTrack = 6;
        Project &p = g_engine.project_for_load();
        static Pattern saved_pattern;
        saved_pattern = p.patterns[0][kFactoryPadSlot];
        const KitTrack saved_track = p.kit.tracks[kChordTrack];
        Pattern &pad = p.patterns[0][kFactoryPadSlot];
        for (int step = 0; step < kStepsPerTrack; step++) pattern_clear_step_locks(pad, kChordTrack, step);
        const uint8_t poly = uint8_t(MachineId::va_poly);
        for (int i = 0; i < sound_preset_count(poly); i++) {
            if (std::strcmp(sound_preset(poly, i)->name, "PLUCK") == 0) {
                sound_preset_apply(poly, i, p.kit.tracks[kChordTrack].params);
            }
        }
        for (int i = 0; i < arp_preset_count(); i++) {
            Project &q = g_engine.project_for_load();
            arp_preset_apply(i, &q.kit.tracks[kChordTrack].params[kKitLfoEnd], q.kit.tracks[kChordTrack].arp);
            post(Cmd::kit_recall, 0, 0, 0);
            int frames = 0;
            const Stats s = render(kFactoryPadSlot, 4, -1, &frames);
            const char *name = arp_preset(i)->name;
            std::snprintf(path, sizeof path, "%s/ARPS_%s.wav", dir, name);
            write_wav(path, frames);
            std::printf("ARP %-6s peak %6.1f dBFS  rms %6.1f dBFS\n", name, db(s.peak), db(s.rms));
            check_balance(name, s);
        }
        Project &q = g_engine.project_for_load();
        q.patterns[0][kFactoryPadSlot] = saved_pattern;
        q.kit.tracks[kChordTrack] = saved_track;
        post(Cmd::kit_recall, 0, 0, 0);
    }
    return g_off_centre == 0 ? 0 : 1;
}
