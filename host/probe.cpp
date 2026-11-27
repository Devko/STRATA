// The sound probe: renders each synth machine alone, with the insert chain and the mixer out of
// the way, in the cases where synthesis usually goes wrong -- high notes (aliasing), the filter
// at full resonance (self-oscillation and its tuning), every note of the range (tuning), hard
// drive, fast retriggers (clicks) -- and writes each as a 32-bit float WAV with a line in
// OUTDIR/manifest.tsv. tools/sound_probe.py reads them and prints the measurements.
//
//   make -C host probe                  # into $(OUT)/probe
//   build/host/strata-probe OUTDIR
//
// The machines are driven directly (init, prepare once a block, trigger, render_add), as the
// engine drives them, so what is measured is the machine and nothing after it.
#include "fm.hpp"
#include "fmpoly.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "perc.hpp"
#include "snare.hpp"
#include "va.hpp"
#include "vapoly.hpp"
#include "wavetable.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace strata;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 64;
constexpr int kMax = kRate * 8;
float g_buf[kMax];

FILE *g_manifest = nullptr;
const char *g_dir = ".";

void le16(FILE *f, uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); }
void le32(FILE *f, uint32_t v) { le16(f, uint16_t(v)); le16(f, uint16_t(v >> 16)); }

void write_wav(const char *name, const float *x, int n) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s.wav", g_dir, name);
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
    const uint32_t data = uint32_t(n) * 4u;
    std::fwrite("RIFF", 1, 4, f); le32(f, 36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 3); le16(f, 1); le32(f, kRate); le32(f, kRate * 4); le16(f, 4); le16(f, 32);
    std::fwrite("data", 1, 4, f); le32(f, data);
    std::fwrite(x, 4, size_t(n), f);
    std::fclose(f);
}

// One event in a probe: a note on at `at` seconds, held for `len` seconds (0: no gate).
struct Note {
    float at;
    int note;
    float len;
    int velocity = 100;
    bool accent = false;
};

// Renders `seconds` of `m` with `p`, the notes triggered on their exact frames.
int render(Machine &m, const TrackParams &p, const Note *notes, int count, float seconds) {
    const int total = int(seconds * kRate);
    std::memset(g_buf, 0, sizeof(float) * size_t(total));
    int next = 0;
    for (int b = 0; b < total; b += kBlock) {
        m.prepare(p);
        const int n = total - b < kBlock ? total - b : kBlock;
        while (next < count) {
            const int f = int(notes[next].at * kRate);
            if (f >= b + n) break;
            Trig t;
            t.note = uint8_t(notes[next].note);
            t.velocity = uint8_t(notes[next].velocity);
            t.accent = notes[next].accent;
            t.frame = uint32_t(f - b);
            t.gate_frames = notes[next].len > 0.0f ? uint32_t(notes[next].len * kRate) : 0u;
            m.trigger(t);
            next++;
        }
        m.render_add(g_buf + b, n);
    }
    return total;
}

void emit(const char *name, const char *kind, int note, const char *what, int frames) {
    write_wav(name, g_buf, frames);
    std::fprintf(g_manifest, "%s\t%s\t%d\t%s\n", name, kind, note, what);
}

TrackParams patch(uint8_t fill) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.v[i] = fill;
    return p;
}

// A held, open VA: LP12 wide open, no envelope on the filter, sustain full, no sub or drive.
TrackParams va_open() {
    TrackParams p = patch(64);
    p.set(1, 64);    // LP12
    p.set(2, 127);   // CUT 12 kHz
    p.set(3, 0);     // RES
    p.set(9, 0);     // ENV
    p.set(11, 100);  // LVL
    p.set(kAsrFirst + 0, 0);
    p.set(kAsrFirst + 1, 127);
    p.set(kAsrFirst + 2, 0);
    p.set(kAsrFirst + 3, 0);
    p.set(kOscFirst + 0, 0);
    p.set(kOscFirst + 1, 0);
    p.set(kOscFirst + 2, 0);
    p.set(kOscFirst + 3, 0);
    return p;
}

void held(Machine &m, const TrackParams &p, const char *name, const char *kind, int note,
          const char *what, float seconds = 1.5f) {
    const Note n{0.0f, note, seconds};
    emit(name, kind, note, what, render(m, p, &n, 1, seconds));
}

void probe_va(Machine &m, const char *tag, bool wave) {
    char name[128], what[128];
    // Aliasing and tuning: the oscillator, open, at every octave.
    const int notes[] = {24, 36, 48, 60, 72, 84, 96, 108};
    for (int note : notes) {
        TrackParams p = va_open();
        if (wave) p.set(0, 0);
        std::snprintf(name, sizeof name, "%s_saw_%d", tag, note);
        held(m, p, name, "tone", note, wave ? "table 0" : "saw");
        if (!wave) {
            p.set(kOscFirst + 0, 80);   // pulse
            std::snprintf(name, sizeof name, "%s_pulse_%d", tag, note);
            held(m, p, name, "tone", note, "pulse");
            p.set(kOscFirst + 0, 0);
            p.set(kOscFirst + 1, 110);  // hard sync
            std::snprintf(name, sizeof name, "%s_sync_%d", tag, note);
            held(m, p, name, "tone", note, "sync +17");
            p.set(kOscFirst + 1, 0);
            p.set(kOscFirst + 3, 127);  // drive
            std::snprintf(name, sizeof name, "%s_drive_%d", tag, note);
            held(m, p, name, "tone", note, "DRV 127");
        }
    }
    if (wave) {
        for (int table = 0; table < kWaveTables; table++) {
            TrackParams p = va_open();
            p.set(0, uint8_t(table * 8));
            p.set(1, 64);
            for (int note : {60, 96}) {
                std::snprintf(name, sizeof name, "%s_tbl%02d_%d", tag, table, note);
                std::snprintf(what, sizeof what, "table %d", table);
                held(m, p, name, "tone", note, what, 1.0f);
            }
        }
    }
    // The filter's resonance: CUT at five points, RES full, a low note so the peak is clear of
    // the harmonics' fundamental. The base cutoff is reported so the analysis can compare.
    for (int cut : {32, 64, 80, 96, 112}) {
        for (int res : {100, 127}) {
            TrackParams p = va_open();
            p.set(1, 0);          // LP24, the acid filter
            p.set(2, uint8_t(cut));
            p.set(3, uint8_t(res));
            std::snprintf(name, sizeof name, "%s_res%d_cut%d", tag, res, cut);
            std::snprintf(what, sizeof what, "cut=%d res=%d", cut, res);
            held(m, p, name, "reso", 36, what, 1.5f);
        }
    }
    // Clicks: fast retriggers, a short note, an accent, the release.
    {
        TrackParams p = va_open();
        p.set(2, 90);
        p.set(9, 80);
        p.set(kAsrFirst + 1, 0);   // SUS 0: DEC is the note
        p.set(8, 20);              // a short DEC
        Note ns[16];
        for (int i = 0; i < 16; i++) ns[i] = Note{0.05f + 0.115f * float(i), (i & 3) ? 48 : 36, 0.0f, 100, (i & 7) == 0};
        std::snprintf(name, sizeof name, "%s_retrig", tag);
        emit(name, "click", 36, "16ths, SUS 0, short DEC", render(m, p, ns, 16, 2.0f));
    }
    // DC: a pulse at its narrowest through LP24, then silence.
    {
        TrackParams p = va_open();
        if (!wave) p.set(kOscFirst + 0, 127);
        p.set(1, 0);
        p.set(2, 60);
        std::snprintf(name, sizeof name, "%s_dc", tag);
        const Note n{0.0f, 36, 1.0f};
        emit(name, "dc", 36, "narrow pulse, LP24, then silence", render(m, p, &n, 1, 2.5f));
    }
}

void probe_fm(FmPoly &m) {
    char name[128], what[128];
    // ALG 1 (the stack), IDX at three values, RAT 1, at every octave: sidebands climb past
    // Nyquist on high notes, which is where FM aliases.
    for (int idx : {40, 90, 127}) {
        for (int note : {36, 60, 84, 96}) {
            TrackParams p = patch(64);
            p.set(0, 0);       // ALG 1
            p.set(1, 20);      // RAT 1
            p.set(2, uint8_t(idx));
            p.set(3, 0);       // FBK 0
            p.set(9, 0);       // IEN 0: a static timbre
            p.set(10, 0);
            p.set(11, 100);
            p.set(kAsrFirst + 0, 0);
            p.set(kAsrFirst + 1, 127);
            p.set(kAsrFirst + 2, 0);
            p.set(kAsrFirst + 3, 127);
            p.set(kOscFirst + 0, 0);
            p.set(kOscFirst + 1, 64);
            p.set(kOscFirst + 2, 0);   // BRT 0: no keytrack
            p.set(kOscFirst + 3, 0);
            std::snprintf(name, sizeof name, "fm_idx%d_%d", idx, note);
            std::snprintf(what, sizeof what, "alg1 rat1 idx%d", idx);
            held(m, p, name, "tone", note, what, 1.0f);
        }
    }
    for (int fb : {64, 100, 127}) {
        for (int note : {48, 84}) {
            TrackParams p = patch(64);
            p.set(0, 127);     // ALG 8: four sines, so op 4's feedback is heard alone
            p.set(1, 20);      // RAT 1, so op 4 sits on the fundamental
            p.set(2, 0);
            p.set(3, uint8_t(fb));
            p.set(9, 0);
            p.set(11, 100);
            p.set(kAsrFirst + 0, 0);
            p.set(kAsrFirst + 1, 127);
            p.set(kAsrFirst + 2, 0);
            p.set(kAsrFirst + 3, 127);
            p.set(kOscFirst + 0, 0);
            p.set(kOscFirst + 1, 64);
            p.set(kOscFirst + 2, 0);
            p.set(kOscFirst + 3, 0);
            std::snprintf(name, sizeof name, "fm_fb%d_%d", fb, note);
            std::snprintf(what, sizeof what, "alg8 fbk%d", fb);
            held(m, p, name, "tone", note, what, 1.0f);
        }
    }
}

// Musical phrases for listening, not measuring: a rising arpeggio, two notes a beat at 130 BPM,
// from C4 to C7, so the change from note to note is what is heard.
void probe_fm_riffs(FmPoly &m) {
    struct Riff { const char *name; uint8_t alg, rat, idx, fbk, ien, dec; };
    const Riff riffs[] = {
        {"fm_riff_stack", 0, 20, 90, 0, 64, 70},     // ALG 1, the four-deep stack: a hard digital lead
        {"fm_riff_bell", 64, 120, 100, 30, 90, 80},  // ALG 5, RAT 14: a bright bell
        {"fm_riff_bass", 32, 20, 110, 60, 50, 50},   // ALG 3 with feedback: a growl
    };
    const int arp[] = {48, 55, 60, 67, 72, 79, 84, 91, 96, 91, 84, 79, 72, 67, 60, 55};
    for (const Riff &r : riffs) {
        TrackParams p = patch(64);
        p.set(0, r.alg);
        p.set(1, r.rat);
        p.set(2, r.idx);
        p.set(3, r.fbk);
        p.set(8, r.dec);
        p.set(9, r.ien);
        p.set(10, 40);
        p.set(11, 100);
        p.set(kAsrFirst + 0, 0);
        p.set(kAsrFirst + 1, 0);
        p.set(kAsrFirst + 2, 0);
        p.set(kAsrFirst + 3, 0);
        p.set(kOscFirst + 0, 0);
        p.set(kOscFirst + 1, 64);
        p.set(kOscFirst + 2, 0);
        p.set(kOscFirst + 3, 0);
        Note ns[16];
        for (int i = 0; i < 16; i++) ns[i] = Note{0.05f + 0.2308f * float(i), arp[i], 0.0f, 100, (i & 3) == 0};
        emit(r.name, "riff", 60, "listen", render(m, p, ns, 16, 4.0f));
    }
}

void probe_drum(Machine &m, const char *tag) {
    char name[128];
    TrackParams p = patch(64);
    Note ns[8];
    for (int i = 0; i < 8; i++) ns[i] = Note{0.05f + 0.23f * float(i), 60, 0.0f, 100, (i & 3) == 0};
    std::snprintf(name, sizeof name, "%s_hits", tag);
    emit(name, "drum", 60, "default kit, 8 hits", render(m, p, ns, 8, 2.2f));
}

}  // namespace

int main(int argc, char **argv) {
    g_dir = argc > 1 ? argv[1] : ".";
    char path[512];
    std::snprintf(path, sizeof path, "%s/manifest.tsv", g_dir);
    g_manifest = std::fopen(path, "w");
    if (!g_manifest) { std::fprintf(stderr, "cannot write %s\n", path); return 1; }

    static WaveLibrary lib;
    if (!lib.init()) { std::fprintf(stderr, "wave library failed\n"); return 1; }
    set_wave_library(&lib);

    static VaMono va;
    va.init(float(kRate));
    probe_va(va, "vamono", false);

    static VaMono wave;
    wave.set_wave(true);
    wave.init(float(kRate));
    probe_va(wave, "wavemono", true);

    static FmPoly fm;
    fm.init(float(kRate));
    probe_fm(fm);
    probe_fm_riffs(fm);

    static Kick kick;
    kick.init(float(kRate));
    probe_drum(kick, "kick");
    static Snare snare;
    snare.init(float(kRate));
    probe_drum(snare, "snare");
    static Hat hat;
    hat.init(float(kRate));
    probe_drum(hat, "hat");
    static Perc perc;
    perc.init(float(kRate));
    probe_drum(perc, "perc");

    std::fclose(g_manifest);
    return 0;
}
