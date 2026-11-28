// Renders a KICK sound preset, through the insert chain's drive and filter (the chain's tap,
// before fader, pan and sends), four on the floor, to raw float32 mono at 48 kHz on stdout.
// Offline sound design only: tools/kick_lab/lab.py builds the rumble variants on top.
//
//   kick_render PRESET BPM BEATS [index=value ...] > out.f32
#include "chain.hpp"
#include "kick.hpp"
#include "project.hpp"
#include "sounds.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace strata;

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "kick_render PRESET BPM BEATS [i=v...]\n"); return 2; }
    const uint8_t m = uint8_t(MachineId::kick);
    int idx = -1;
    for (int i = 0; i < sound_preset_count(m); i++)
        if (std::strcmp(sound_preset(m, i)->name, argv[1]) == 0) idx = i;
    if (idx < 0) { std::fprintf(stderr, "no KICK preset %s\n", argv[1]); return 2; }
    static uint8_t kit[kKitParams];
    sound_preset_apply(m, idx, kit);
    TrackParams p{};
    for (int i = 0; i < kTrackParams && i < kKitParams; i++) p.set(i, kit[i]);
    p.set(13, 64);   // PAN centred
    for (int a = 4; a < argc; a++) {
        int i = 0, v = 0;
        if (std::sscanf(argv[a], "%d=%d", &i, &v) == 2) p.set(i, uint8_t(v));
    }
    const double bpm = std::atof(argv[2]);
    const int beats = std::atoi(argv[3]);
    const double beat = 48000.0 * 60.0 / bpm;
    const int frames = (int(beat * beats) / 64 + 1) * 64;
    static Kick k;
    k.init(48000.0f);
    k.prepare(p);
    static InsertChain chain;
    chain.init(48000.0f);
    static float block[64], dl[64], dr[64], tap[64];
    ChainOut to;
    to.dry_l = dl;
    to.dry_r = dr;
    int next = 0;
    for (int b = 0; b < frames / 64; b++) {
        const int start = b * 64;
        while (next < beats && int(next * beat) < start + 64) {
            Trig t;
            t.note = 60;
            t.velocity = 100;
            t.frame = uint32_t(int(next * beat) - start);
            k.trigger(t);
            next++;
        }
        for (float &x : block) x = 0.0f;
        k.prepare(p);
        k.render_add(block, 64);
        chain.prepare(p, ChainMix{});
        to.tap = tap;
        chain.process(block, 64, to);
        std::fwrite(tap, sizeof(float), 64, stdout);
    }
    return 0;
}
