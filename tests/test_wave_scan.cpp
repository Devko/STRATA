// SCN, the WAVE pair's wavetable scan (dsp/va.cpp, "SCN"): each note moves its own position from
// POS towards END in one of ten modes, at SPD, along CRV.
//
// What is pinned here:
//   - the knobs' mappings and every mode's shape, CRV included;
//   - MOD OFF, and a scan whose END is its POS, are bit-exact with no page at all, so every
//     existing sound is untouched;
//   - FWD carries the timbre from POS to END (CLASSIC's sine to its square) and holds there;
//   - every note starts its scan again, a slid WAVE MONO note carries it on;
//   - a synced SPD follows the tempo the engine gives (Machine::set_tick_frames);
//   - a WAVE POLY chord scans as one;
//   - the random modes stay inside POS..END and keep moving.
#include "check.hpp"

#include <enaga/abi.h>

#include "lfo.hpp"
#include "va.hpp"
#include "vapoly.hpp"
#include "wavetable.hpp"

#include <cmath>
#include <new>
#include <string_view>

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kLen = ENAGA_SAMPLE_RATE / 2;   // half a second

constexpr int kTbl = 0, kPos = 1, kCut = 2, kRes = 3, kDec = 8, kEnv = 9, kMot = 10, kLvl = 11;
constexpr int kSus = kAsrFirst + 1;
constexpr int kMod = kScnFirst + 0, kSpd = kScnFirst + 1, kEnd = kScnFirst + 2, kCrv = kScnFirst + 3;

WaveLibrary g_lib;
float g_out[kLen];
float g_ref[kLen];

void library() {
    if (!g_lib.ready()) CHECK(g_lib.init());
    set_wave_library(&g_lib);
}

// A held, open patch on CLASSIC (sine at frame 0, square at 15), the scan's page as given.
TrackParams patch(ScanMode mode, uint8_t pos, uint8_t end, uint8_t spd, uint8_t crv = 64) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.set(i, 0);
    p.set(kTbl, 4);
    p.set(kPos, pos);
    p.set(kCut, 127);
    p.set(kRes, 0);
    p.set(kDec, 100);
    p.set(kEnv, 0);
    p.set(kMot, 64);
    p.set(kLvl, 127);
    p.set(kSus, 127);
    p.set(kMod, uint8_t(mode));
    p.set(kSpd, spd);
    p.set(kEnd, end);
    p.set(kCrv, crv);
    return p;
}

Trig hit(uint8_t note, bool slide = false) {
    Trig t;
    t.note = note;
    t.velocity = 127;
    t.held = true;
    t.slide = slide;
    return t;
}

void render(Machine &m, const TrackParams &p, float *out, int frames) {
    for (int i = 0; i < frames; i++) out[i] = 0.0f;
    for (int b = 0; b + kBlk <= frames; b += kBlk) {
        m.prepare(p);
        m.render_add(out + b, kBlk);
    }
}

// From a fresh init each time, so two plays of one patch match bit for bit.
void play(Machine &m, const TrackParams &p, float *out, int frames = kLen) {
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(57));
    render(m, p, out, frames);
}

double magnitude(const float *x, int n, double hz) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) {
        const double w = 2.0 * M_PI * hz * i / kRate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

// The SPD byte whose free rate is closest to `hz`, and the synced byte of `ticks` a cycle.
uint8_t free_byte(float hz) {
    uint8_t best = 0;
    for (int b = 0; b < int(kLfoSyncFirst); b++) {
        if (std::fabs(lfo_free_hz(uint8_t(b)) - hz) < std::fabs(lfo_free_hz(best) - hz)) best = uint8_t(b);
    }
    return best;
}
uint8_t synced_byte(uint32_t ticks) {
    for (int b = kLfoSyncFirst; b < 128; b++) {
        if (lfo_sync_ticks(uint8_t(b)) == ticks) return uint8_t(b);
    }
    return 0;
}

}  // namespace

TEST(wave_scan_knob_mappings) {
    CHECK(va_scan_mode(0) == ScanMode::off);
    CHECK(va_scan_mode(9) == ScanMode::drift);
    CHECK(va_scan_mode(200) == ScanMode::drift);        // past the last reads as the last
    CHECK_EQ(kScanModes, kKitScanModes);
    CHECK_EQ(int(kit_param_max(kMod)), kScanModes - 1);
    CHECK_EQ(int(kit_param_max(kSpd)), 127);
    const char *const names[kScanModes] = {"OFF", "FWD", "REV", "LOOP", "LREV",
                                           "PING", "RET", "SINE", "RAND", "DRFT"};
    for (int m = 0; m < kScanModes; m++) {
        CHECK(std::string_view(va_scan_mode_name(ScanMode(m))) == std::string_view(names[m]));
        CHECK(std::string_view(names[m]).size() <= 4);
    }
    CHECK(va_scan_curve(64) == 1.0f);
    CHECK(std::fabs(va_scan_curve(0) - 0.125f) < 1e-4f);
    CHECK(std::fabs(va_scan_curve(127) - 7.74f) < 0.02f);    // 8^(63/64)
    CHECK(va_scan_end(127) == va_wave_position(127));
    CHECK(va_scan_end(0) == 0.0f);
    // SPD is the LFO's RAT: free in Hz over the rate, synced by the ticks at the tempo.
    CHECK(std::fabs(va_scan_increment(28, 1.0f / kRate, 0.0f) - 1.0f / kRate) < 1e-9f);
    const uint8_t quarter = synced_byte(96);
    CHECK(quarter != 0);
    CHECK(std::fabs(va_scan_increment(quarter, 1.0f / kRate, 250.0f) - 1.0f / 24000.0f) < 1e-10f);
    CHECK(va_scan_increment(quarter, 1.0f / kRate, 0.0f) == 0.0f);   // no tempo yet: still
    // The kit's default: no scan, SPD at 1 Hz, END on the last frame, CRV straight.
    CHECK(std::fabs(lfo_free_hz(kKitScnDefault[1]) - 1.0f) < 1e-4f);
    CHECK_EQ(int(kKitScnDefault[2]), 127);
    CHECK_EQ(int(kKitScnDefault[3]), 64);
}

TEST(wave_scan_every_mode_has_its_shape) {
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    CHECK(near(va_scan_travel(ScanMode::off, 0.7f, 1.0f), 0.0f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 0.0f, 1.0f), 0.0f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 0.5f, 1.0f), 0.5f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 3.0f, 1.0f), 1.0f));      // and holds
    CHECK(near(va_scan_travel(ScanMode::rev, 0.0f, 1.0f), 1.0f));
    CHECK(near(va_scan_travel(ScanMode::rev, 0.25f, 1.0f), 0.75f));
    CHECK(near(va_scan_travel(ScanMode::rev, 2.0f, 1.0f), 0.0f));
    CHECK(near(va_scan_travel(ScanMode::loop, 1.25f, 1.0f), 0.25f));
    CHECK(near(va_scan_travel(ScanMode::loop_rev, 1.25f, 1.0f), 0.75f));
    CHECK(near(va_scan_travel(ScanMode::ping, 0.25f, 1.0f), 0.5f));
    CHECK(near(va_scan_travel(ScanMode::ping, 2.5f, 1.0f), 1.0f));
    CHECK(near(va_scan_travel(ScanMode::ping, 0.75f, 1.0f), 0.5f));
    CHECK(near(va_scan_travel(ScanMode::ret, 0.5f, 1.0f), 1.0f));
    CHECK(near(va_scan_travel(ScanMode::ret, 1.5f, 1.0f), 0.0f));      // once, then home
    CHECK(near(va_scan_travel(ScanMode::sine, 0.0f, 1.0f), 0.0f));
    CHECK(near(va_scan_travel(ScanMode::sine, 0.5f, 1.0f), 1.0f));
    CHECK(near(va_scan_travel(ScanMode::sine, 0.25f, 1.0f), 0.5f));
    // CRV: a slow start above 64, a fast one below, the ends where they were.
    CHECK(near(va_scan_travel(ScanMode::fwd, 0.5f, 2.0f), 0.25f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 0.25f, 0.5f), 0.5f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 1.0f, 8.0f), 1.0f));
    CHECK(near(va_scan_travel(ScanMode::fwd, 0.0f, 0.125f), 0.0f));
    // The random modes' picture: inside 0..1, one frame a cycle for RAND, no jump for DRFT.
    float last = va_scan_travel(ScanMode::drift, 0.0f, 1.0f);
    int moved = 0;
    for (int i = 0; i < 400; i++) {
        const float phase = float(i) * 0.01f;
        const float r = va_scan_travel(ScanMode::random, phase, 1.0f);
        CHECK(r >= 0.0f && r <= 1.0f);
        CHECK(r == va_scan_travel(ScanMode::random, float(int(phase)) + 0.5f, 1.0f));
        const float d = va_scan_travel(ScanMode::drift, phase, 1.0f);
        CHECK(d >= 0.0f && d <= 1.0f);
        CHECK(std::fabs(d - last) < 0.05f);
        if (std::fabs(d - last) > 1e-4f) moved++;
        last = d;
    }
    CHECK(moved > 100);
}

TEST(wave_scan_off_and_end_at_pos_are_bit_exact) {
    library();
    // A brand-new machine for every render: init() leaves the noise generator where it was, so
    // only two fresh machines play one patch bit for bit alike.
    alignas(VaMono) static unsigned char mono_room[sizeof(VaMono)];
    const auto mono = [](const TrackParams &p, float *out) {
        VaMono *m = new (mono_room) VaMono;
        m->set_wave(true);
        play(*m, p, out);
    };
    const auto same = [] {
        int differ = 0;
        for (int i = 0; i < kLen; i++) differ += g_out[i] != g_ref[i];
        return differ;
    };
    // The reference: no page at all (all four bytes 0).
    const TrackParams none = patch(ScanMode::off, 40, 0, 0, 0);
    mono(none, g_ref);
    mono(none, g_out);
    CHECK_EQ(same(), 0);
    // MOD OFF with the other three anywhere.
    mono(patch(ScanMode::off, 40, 127, 90, 10), g_out);
    CHECK_EQ(same(), 0);
    // Every mode, END on POS: nowhere to travel, so not one bit moves.
    for (int mode = 1; mode < kScanModes; mode++) {
        mono(patch(ScanMode(mode), 40, 40, 60, 90), g_out);
        CHECK_EQ(same(), 0);
    }
    // And WAVE POLY the same.
    alignas(VaPoly) static unsigned char poly_room[sizeof(VaPoly)];
    const auto poly = [](const TrackParams &p, float *out) {
        VaPoly *m = new (poly_room) VaPoly;
        m->set_wave(true);
        play(*m, p, out);
    };
    poly(none, g_ref);
    poly(patch(ScanMode::loop, 40, 40, 60), g_out);
    CHECK_EQ(same(), 0);
}

TEST(wave_scan_fwd_carries_the_timbre_from_pos_to_end) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    // A quarter-second sweep (4 Hz) from CLASSIC's sine to its square, then held.
    play(m, patch(ScanMode::fwd, 0, 127, free_byte(4.0f)), g_out);
    CHECK(m.scan_travel() == 1.0f);
    const double f_early = magnitude(g_out, 2400, 220.0);
    const double h3_early = magnitude(g_out, 2400, 660.0);
    const int from = kLen - 9600;
    const double f_late = magnitude(g_out + from, 9600, 220.0);
    const double h3_late = magnitude(g_out + from, 9600, 660.0);
    CHECK(f_early > 0.02 && f_late > 0.05);
    CHECK(h3_early < 0.1 * f_early);          // still the sine at the start
    CHECK(h3_late > 0.2 * f_late);            // the square by the end
    // REV goes the other way: the square first, the sine at the end.
    play(m, patch(ScanMode::rev, 0, 127, free_byte(4.0f)), g_out);
    CHECK(m.scan_travel() == 0.0f);
    CHECK(magnitude(g_out + from, 9600, 660.0) < 0.02 * magnitude(g_out + from, 9600, 220.0));
}

TEST(wave_scan_restarts_on_each_note_and_carries_on_through_a_slide) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    const TrackParams p = patch(ScanMode::loop, 0, 127, 0);   // a 16 s cycle: travel = time
    play(m, p, g_out, kLen);
    const float after_half = m.scan_travel();
    CHECK(std::fabs(after_half - 0.5f / 16.0f) < 0.002f);

    // A new note starts it again from POS.
    m.trigger(hit(60));
    render(m, p, g_out, 2 * kBlk);
    CHECK(m.scan_travel() < 0.002f);
    render(m, p, g_out, kLen / 2);
    const float before_slide = m.scan_travel();
    CHECK(before_slide > 0.01f);

    // A slid note is the old one carrying on.
    m.trigger(hit(62, true));
    render(m, p, g_out, 2 * kBlk);
    CHECK(m.scan_travel() > before_slide);
}

TEST(wave_scan_synced_speed_follows_the_tempo) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    // FWD over a quarter note. 250 frames a tick is 120 BPM: a quarter is half a second.
    const TrackParams p = patch(ScanMode::fwd, 0, 127, synced_byte(96));
    m.set_tick_frames(250.0f);
    m.kill_all();
    m.prepare(p);
    m.trigger(hit(57));
    render(m, p, g_out, kLen / 2);
    CHECK(std::fabs(m.scan_travel() - 0.5f) < 0.01f);
    // Twice the tempo mid-note: the rest of the sweep takes half as long.
    m.set_tick_frames(125.0f);
    render(m, p, g_out, kLen / 8);
    CHECK(std::fabs(m.scan_travel() - 0.75f) < 0.01f);
    // No tempo yet (0): the scan stays where it is rather than racing.
    m.set_tick_frames(0.0f);
    const float held = m.scan_travel();
    render(m, p, g_out, kLen / 8);
    CHECK(std::fabs(m.scan_travel() - held) < 0.01f);
}

TEST(wave_scan_a_poly_chord_scans_as_one) {
    library();
    static VaPoly m;
    m.set_wave(true);
    m.init(kRate);
    const TrackParams p = patch(ScanMode::sine, 10, 120, free_byte(2.0f));
    m.kill_all();
    m.prepare(p);
    m.trigger(hit(57));
    m.trigger(hit(61));
    m.trigger(hit(64));
    render(m, p, g_out, kLen / 3);
    const float first = m.voice_scan_travel(0);
    CHECK(first > 0.0f);
    int same = 0;
    for (int v = 0; v < VaPoly::kVoices; v++) {
        if (m.voice_note(v) >= 0 && m.voice_scan_travel(v) == first) same++;
    }
    CHECK_EQ(same, 3);
    for (int i = 0; i < kLen / 3; i++) CHECK(std::isfinite(g_out[i]) && std::fabs(g_out[i]) < 2.0f);
}

TEST(wave_scan_random_modes_stay_inside_and_keep_moving) {
    library();
    static VaMono m;
    m.set_wave(true);
    m.init(kRate);
    for (ScanMode mode : {ScanMode::random, ScanMode::drift}) {
        const TrackParams p = patch(mode, 30, 100, free_byte(16.0f));
        m.kill_all();
        m.prepare(p);
        m.trigger(hit(57));
        float lo = 2.0f, hi = -1.0f;
        for (int b = 0; b < kLen / kBlk; b++) {
            render(m, p, g_out, kBlk);
            const float u = m.scan_travel();
            CHECK(u >= 0.0f && u <= 1.0f);
            lo = u < lo ? u : lo;
            hi = u > hi ? u : hi;
            for (int i = 0; i < kBlk; i++) CHECK(std::isfinite(g_out[i]));
        }
        CHECK(hi - lo > 0.3f);       // eight cycles of draws cover a good part of the range
    }
}
