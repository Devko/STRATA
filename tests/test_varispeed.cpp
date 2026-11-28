// VariSpeed (core/varispeed.hpp): the arithmetic, each machine's pitch, and the engine handing
// the amount to every track and the delay. The gesture, the clock and the notices are in
// tests/test_ui.cpp.
#include "apprig.hpp"
#include "check.hpp"

#include "fm.hpp"
#include "fmpoly.hpp"
#include "grain.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "lfo.hpp"
#include "perc.hpp"
#include "project.hpp"
#include "sample.hpp"
#include "snare.hpp"
#include "track.hpp"
#include "va.hpp"
#include "vapoly.hpp"
#include "varispeed.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kBlocks = 160;
float g_a[kBlocks * kBlk];
float g_b[kBlocks * kBlk];
constexpr uint32_t kSine = 48000;
int16_t g_sine[kSine];

TrackParams flat(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kTrackParams; i++) p.v[i] = value;
    return p;
}

Trig note(uint8_t n = 60) {
    Trig t;
    t.note = n;
    t.velocity = 127;
    return t;
}

// `semis` handed to the machine before the hit, then kBlocks blocks into `out`.
void render(Machine &m, const TrackParams &p, float semis, float *out) {
    m.init(kRate);
    m.prepare(p);
    m.set_pitch_mod(semis);
    m.trigger(note());
    for (int i = 0; i < kBlocks * kBlk; i++) out[i] = 0.0f;
    for (int b = 0; b < kBlocks; b++) {
        m.prepare(p);
        m.render_add(out + b * kBlk, kBlk);
    }
}

int sign_changes(const float *x, int n) {
    int count = 0;
    for (int i = 1; i < n; i++) count += (x[i - 1] < 0.0f) != (x[i] < 0.0f) ? 1 : 0;
    return count;
}

bool same(const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// A machine handed 0 renders exactly as one never handed anything, and +12 an octave up: twice
// the zero crossings over a window, within the window's rounding.
template <typename M>
void check_octave(const TrackParams &p, int from, int span) {
    static M m;
    render(m, p, 0.0f, g_a);
    static M fresh_one;
    fresh_one.init(kRate);
    fresh_one.prepare(p);
    fresh_one.trigger(note());
    for (int i = 0; i < kBlocks * kBlk; i++) g_b[i] = 0.0f;
    for (int b = 0; b < kBlocks; b++) {
        fresh_one.prepare(p);
        fresh_one.render_add(g_b + b * kBlk, kBlk);
    }
    CHECK(same(g_a, g_b, kBlocks * kBlk));
    const int low = sign_changes(g_a + from, span);
    render(m, p, 12.0f, g_b);
    const int high = sign_changes(g_b + from, span);
    CHECK(low > 0);
    CHECK_NEAR(float(high), 2.0f * float(low), 0.1 * double(high) + 2.0);
}

}  // namespace

TEST(vari_turn_moves_a_tenth_a_click_holds_an_octave_and_stops_on_zero) {
    CHECK_EQ(int(vari_turn(0, 1)), 1);
    CHECK_EQ(int(vari_turn(0, -12)), -12);
    CHECK_EQ(int(vari_turn(118, 5)), kVariRange);
    CHECK_EQ(int(vari_turn(-118, -5)), -kVariRange);
    // The detent: a turn through 0 lands on it, from either side, however fast.
    CHECK_EQ(int(vari_turn(3, -10)), 0);
    CHECK_EQ(int(vari_turn(-3, 10)), 0);
    CHECK_EQ(int(vari_turn(0, -1)), -1);          // and leaves it on the next click
    CHECK_EQ(int(vari_turn(int8_t(127), 0)), kVariRange);
}

TEST(vari_ratio_is_exact_at_rest_an_octave_at_the_ends_and_rises_all_the_way) {
    CHECK_EQ(vari_ratio(0), 1.0f);
    CHECK_EQ(vari_semis(0), 0.0f);
    CHECK_NEAR(vari_ratio(120), 2.0f, 1e-6);
    CHECK_NEAR(vari_ratio(-120), 0.5f, 1e-6);
    CHECK_NEAR(vari_ratio(10), 1.0594631f, 1e-6);
    CHECK_NEAR(vari_ratio(-10), 0.9438743f, 1e-6);
    CHECK_NEAR(vari_ratio(-13), 0.9276591f, 1e-6);        // 2^(-1.3 / 12)
    CHECK_NEAR(vari_ratio(57), 1.3899182f, 1e-6);         // 2^(5.7 / 12)
    CHECK_NEAR(vari_semis(-35), -3.5f, 1e-6);
    CHECK_NEAR(vari_ratio(int8_t(127)), 2.0f, 1e-6);       // held to the range
    for (int t = -kVariRange; t < kVariRange; t++) {
        CHECK(vari_ratio(int8_t(t)) < vari_ratio(int8_t(t + 1)));
        // Each way the same: a tenth up and a tenth down are inverses.
        CHECK_NEAR(vari_ratio(int8_t(t)) * vari_ratio(int8_t(-t)), 1.0f, 1e-6);
    }
}

TEST(vari_fit_keeps_the_moved_tempo_inside_the_clock) {
    // 130 BPM: an octave either way is 65 and 260, both inside 20-300.
    CHECK_EQ(int(vari_fit(120, 13000, 2000, 30000)), 120);
    CHECK_EQ(int(vari_fit(-120, 13000, 2000, 30000)), -120);
    // 200 BPM up an octave would be 400: it stops where 200 x ratio is still 300 or under.
    const int8_t up = vari_fit(120, 20000, 2000, 30000);
    CHECK(up > 0 && up < 120);
    CHECK(20000.0f * vari_ratio(up) <= 30000.0f);
    CHECK(20000.0f * vari_ratio(int8_t(up + 1)) > 30000.0f);
    // 30 BPM down an octave would be 15.
    const int8_t down = vari_fit(-120, 3000, 2000, 30000);
    CHECK(down < 0 && down > -120);
    CHECK(3000.0f * vari_ratio(down) >= 2000.0f);
    // At the edge itself only 0 fits that way, and the other way is free.
    CHECK_EQ(int(vari_fit(5, 30000, 2000, 30000)), 0);
    CHECK_EQ(int(vari_fit(-5, 30000, 2000, 30000)), -5);
}

TEST(vari_tempo_and_text_say_what_the_clock_runs_at) {
    CHECK_EQ(vari_tempo_tenths(1300, 0), 1300u);
    CHECK_EQ(vari_tempo_tenths(1300, 120), 2600u);
    CHECK_EQ(vari_tempo_tenths(1300, -120), 650u);
    CHECK_EQ(vari_tempo_tenths(1300, 10), 1377u);        // 130 x 1.0595 = 137.73
    char text[kVariTextMax];
    vari_text(0, text, kVariTextMax);
    CHECK_EQ(static_cast<const char *>(text), "0");
    vari_text(12, text, kVariTextMax);
    CHECK_EQ(static_cast<const char *>(text), "+1.2");
    vari_text(-5, text, kVariTextMax);
    CHECK_EQ(static_cast<const char *>(text), "-0.5");
    vari_text(-120, text, kVariTextMax);
    CHECK_EQ(static_cast<const char *>(text), "-12.0");
    vari_text(99, text, 3);                               // cut to the room given
    CHECK_EQ(static_cast<const char *>(text), "+9");
}

TEST(vari_moves_every_drum_an_octave_and_is_exact_at_rest) {
    TrackParams p = flat(64);
    p.set(11, 127);
    TrackParams kick = p;
    kick.set(1, 0);      // no sweep: the note and VariSpeed are all that set the pitch
    kick.set(3, 0);      // no click noise
    kick.set(8, 127);
    check_octave<Kick>(kick, 256, 4096);
    TrackParams perc = p;
    perc.set(8, 127);
    check_octave<Perc>(perc, 256, 2048);
    // SNARE and HAT are noise as much as tone, so no crossings to count: the base frequency's
    // ratio, and a different sound.
    static Snare snare;
    render(snare, p, 0.0f, g_a);
    render(snare, p, 12.0f, g_b);
    CHECK_NEAR(snare.pitch_mod_ratio(), 2.0f, 1e-3);
    CHECK(!same(g_a, g_b, kBlocks * kBlk));
    static Hat hat;
    render(hat, p, 0.0f, g_a);
    render(hat, p, -12.0f, g_b);
    CHECK(!same(g_a, g_b, kBlocks * kBlk));
    CHECK_NEAR(hat.pitch_mod_ratio(), 0.5f, 1e-3);
    hat.set_pitch_mod(0.0f);
    CHECK_EQ(hat.pitch_mod_ratio(), 1.0f);
}

TEST(vari_reads_a_sample_faster_and_rebuilds_a_grain_step) {
    for (uint32_t i = 0; i < kSine; i++) {
        // 220 Hz: a zero crossing every 109 frames.
        const uint32_t phase = (i * 220u) % 48000u;
        g_sine[i] = int16_t(phase < 24000u ? 12000 : -12000);
    }
    static Sampler sampler;
    // tests/test_sample.cpp's plain sound: PIT unity, LEN to the end, DEC hold, LVL unity.
    TrackParams p{};
    p.set(0, 64);
    p.set(2, 127);
    p.set(9, 127);
    p.set(11, 127);
    sampler.init(kRate);
    sampler.set_sample(g_sine, kSine, 1, kRate);
    sampler.prepare(p);
    sampler.trigger(note());
    for (int i = 0; i < kBlocks * kBlk; i++) g_a[i] = 0.0f;
    for (int b = 0; b < kBlocks; b++) {
        sampler.prepare(p);
        sampler.render_add(g_a + b * kBlk, kBlk);
    }
    sampler.kill_all();
    sampler.set_pitch_mod(12.0f);
    sampler.trigger(note());
    for (int i = 0; i < kBlocks * kBlk; i++) g_b[i] = 0.0f;
    for (int b = 0; b < kBlocks; b++) {
        sampler.prepare(p);
        sampler.render_add(g_b + b * kBlk, kBlk);
    }
    const int low = sign_changes(g_a + 1024, 4096);
    const int high = sign_changes(g_b + 1024, 4096);
    CHECK(low > 0);
    CHECK_NEAR(float(high), 2.0f * float(low), 0.1 * double(high) + 2.0);

    static Granular grain;
    grain.init(kRate);
    grain.set_sample(g_sine, kSine, 1, kRate);
    grain.prepare(p);
    const float step = grain.read_step();
    grain.set_pitch_mod(12.0f);              // no prepare: the step is rebuilt here
    CHECK_NEAR(grain.read_step(), 2.0f * step, 1e-3 * step);
    grain.set_pitch_mod(0.0f);
    CHECK_EQ(grain.read_step(), step);
}

TEST(vari_adds_semitones_to_the_synths_and_keeps_them_across_prepare) {
    static FmPoly fm;
    fm.init(kRate);
    fm.set_pitch_mod(-3.5f);
    fm.prepare(flat(64));
    CHECK_EQ(fm.settings().pitch_mod, -3.5f);
    // A voice an octave up sounds at twice the crossings, and at 0 exactly as before:
    // tests/test_fm.cpp's plain sine (ALG 1, RAT 1, IDX 0), held by SUS.
    TrackParams p{};
    p.set(1, 16);
    p.set(8, 80);
    p.set(11, 127);
    p.set(kAsrFirst + 1, 127);
    FmSettings s = FmSettings::make(p, kRate, 1.0f / kRate);
    float *outs[3] = {g_a, g_b, g_a + kBlocks * kBlk / 2};
    const float mods[3] = {0.0f, 12.0f, 0.0f};
    for (int k = 0; k < 3; k++) {
        s.pitch_mod = mods[k];
        FmVoice v;
        v.reset();
        v.set_note(60.0f, 1.0f);
        v.gate_on();
        float env[FmVoice::kControl];
        const int frames = kBlocks * kBlk / 2;
        for (int i = 0; i < frames; i++) outs[k][i] = 0.0f;
        for (int i = 0; i < frames; i += FmVoice::kControl) v.run(s, outs[k] + i, FmVoice::kControl, env);
    }
    const int half = kBlocks * kBlk / 2;
    CHECK(same(g_a, g_a + half, half));
    const int low = sign_changes(g_a + 1024, 4096);
    const int high = sign_changes(g_b + 1024, 4096);
    CHECK(low > 0);
    CHECK_NEAR(float(high), 2.0f * float(low), 0.1 * double(high) + 2.0);
}

TEST(vari_reaches_every_track_and_a_new_machine_and_the_delay_follows_the_clock) {
    using namespace strata::test;
    Engine &engine = fresh();
    pump(2);
    const float beat_delay = engine.sends().delay_seconds;
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(engine.machine(t)->pitch_mod_ratio(), 1.0f);
    }

    post(Cmd::set_varispeed, 0, 0, uint8_t(int8_t(120)));          // an octave up
    pump(2);
    CHECK_EQ(int(engine.varispeed()), 120);
    for (int t = 0; t < kTracks; t++) {
        const auto id = MachineId(engine.machine_id(t));
        if (id == MachineId::va_mono || id == MachineId::wave_mono) {
            CHECK_EQ(static_cast<const VaMono *>(engine.machine(t))->settings().pitch_mod, 12.0f);
        } else if (id == MachineId::va_poly || id == MachineId::wave_poly) {
            CHECK_EQ(static_cast<const VaPoly *>(engine.machine(t))->settings().pitch_mod, 12.0f);
        } else if (id == MachineId::fm) {
            CHECK_EQ(static_cast<const FmPoly *>(engine.machine(t))->settings().pitch_mod, 12.0f);
        } else {
            CHECK_NEAR(engine.machine(t)->pitch_mod_ratio(), 2.0f, 1e-3);
        }
    }
    // The delay's beat is half as long: its echoes keep to a clock running twice as fast.
    CHECK_NEAR(engine.sends().delay_seconds, 0.5f * beat_delay, 1e-4);

    // A machine switched onto a track takes the amount at once.
    post(Cmd::set_machine, 2, 0, uint8_t(MachineId::sample));
    pump(2);
    CHECK_NEAR(engine.machine(2)->pitch_mod_ratio(), 2.0f, 1e-3);
    post(Cmd::set_machine, 3, 0, uint8_t(MachineId::fm));
    pump(2);
    CHECK_EQ(static_cast<const FmPoly *>(engine.machine(3))->settings().pitch_mod, 12.0f);

    // Rest: every machine back to its own pitch, the delay to the project's beat, and an amount
    // past the range held to it.
    post(Cmd::set_varispeed, 0, 0, 0);
    pump(2);
    CHECK_EQ(engine.machine(2)->pitch_mod_ratio(), 1.0f);
    CHECK_EQ(static_cast<const FmPoly *>(engine.machine(3))->settings().pitch_mod, 0.0f);
    CHECK_EQ(engine.sends().delay_seconds, beat_delay);
    post(Cmd::set_varispeed, 0, 0, uint8_t(int8_t(-127)));
    pump();
    CHECK_EQ(int(engine.varispeed()), -kVariRange);
    post(Cmd::set_varispeed, 0, 0, 0);
    pump();
}

TEST(vari_rides_on_the_lfo_pitch_of_a_synth) {
    using namespace strata::test;
    Engine &engine = fresh();
    int va = -1;
    for (int t = 0; t < kTracks; t++) {
        if (MachineId(engine.machine_id(t)) == MachineId::va_mono) va = t;
    }
    CHECK(va >= 0);
    if (va < 0) return;
    post(Cmd::set_varispeed, 0, 0, uint8_t(int8_t(-30)));
    // As tests/test_lfo.cpp sets the LFO on TUN.
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoWave), lfo_wave_byte(LfoWave::sine));
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoRate), 20);
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoDepth), 100);
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoDest), lfo_dest_byte(0));
    pump(40);
    CHECK(engine.lfo_pitch_mod(va) != 0.0f);
    const auto *mono = static_cast<const VaMono *>(engine.machine(va));
    // The LFO's own figure stays the LFO's, for the screen; the machine hears both.
    CHECK_NEAR(mono->settings().pitch_mod, engine.lfo_pitch_mod(va) - 3.0f, 1e-4);
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoDepth), 0);
    post(Cmd::set_param, uint8_t(va), 0, uint8_t(kLfoDest), 0);
    post(Cmd::set_varispeed, 0, 0, 0);
    pump(2);
    CHECK_EQ(mono->settings().pitch_mod, 0.0f);
}
