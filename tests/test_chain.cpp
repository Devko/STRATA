// Host tests for the per-track insert chain (dsp/chain.hpp) and the mixer buses (dsp/buses.hpp).
//
// These test measurable properties, not sample values: that the low-pass takes energy out of a
// near-Nyquist signal, that 24 dB takes out more than 12, that the drive is bounded, that a
// bypassed stage is bit-exact, that the limiter cannot be driven past full scale, and that
// nothing goes non-finite at the parameter extremes. A block or two of warm-up runs first
// wherever it matters, because every gain in the chain ramps across its first block on purpose.
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "buses.hpp"
#include "chain.hpp"
#include "track.hpp"

using namespace strata;
namespace ed = enaga::dsp;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kFrames = int(ENAGA_BLOCK_FRAMES);

// The eight parameter slots the chain owns (dsp/chain.hpp).
constexpr int kDrv = 4, kCut = 5, kRes = 6, kMod = 7;
constexpr int kLof = 12, kPan = 13, kSna = 14, kSnb = 15;

// MOD knob values that land squarely inside each of the seven zones.
constexpr uint8_t kModOff = 0, kModLp12 = 20, kModLp24 = 40, kModBp12 = 60;
constexpr uint8_t kModBp24 = 80, kModHp12 = 100, kModHp24 = 120;

// Pan knob values: 0 and 127 are exactly hard left and hard right, 64 is exactly centre.
constexpr uint8_t kPanLeft = 0, kPanCentre = 64, kPanRight = 127;

float g_capture[200 * kFrames];

TrackParams params_all(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

// A chain at its neutral settings: no drive, no filter, no lo-fi, centred, both sends at zero.
TrackParams neutral_params() {
    TrackParams p = params_all(0);
    p.set(kPan, kPanCentre);
    return p;
}

bool is_finite(float v) { return v == v && v < 1e30f && v > -1e30f; }

float absolute(float v) { return v < 0.0f ? -v : v; }

float peak_of(const float *x, int n) {
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = absolute(x[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

float energy_of(const float *x, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += x[i] * x[i];
    return sum;
}

int distinct_values(const float *x, int n) {
    float seen[128];
    int count = 0;
    for (int i = 0; i < n; i++) {
        bool found = false;
        for (int j = 0; j < count; j++) {
            if (seen[j] == x[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (count >= 128) return count;
            seen[count++] = x[i];
        }
    }
    return count;
}

// Where one chain writes its block.
struct Buses {
    float l[kFrames] = {};
    float r[kFrames] = {};
    float a[kFrames] = {};
    float b[kFrames] = {};

    void clear() {
        for (int i = 0; i < kFrames; i++) l[i] = r[i] = a[i] = b[i] = 0.0f;
    }
    ChainOut out(const float *duck = nullptr) {
        ChainOut o;
        o.dry_l = l;
        o.dry_r = r;
        o.send_a = a;
        o.send_b = b;
        o.duck = duck;
        return o;
    }
};

// --- signals ---------------------------------------------------------------------------------

// Alternating +-level: the highest frequency a 48 kHz block can carry.
void fill_nyquist(float *out, int n, int start, float level) {
    for (int i = 0; i < n; i++) out[i] = ((start + i) % 2) ? -level : level;
}

void fill_dc(float *out, int n, float level) {
    for (int i = 0; i < n; i++) out[i] = level;
}

void fill_sine(float *out, int n, int start, float hz, float level) {
    const float dt = hz * (1.0f / kRate);
    for (int i = 0; i < n; i++) out[i] = level * ed::sin_turns(dt * float(start + i));
}

void fill_noise(float *out, int n, ed::Random &random, float level) {
    for (int i = 0; i < n; i++) out[i] = level * random.bipolar();
}

// --- running a chain -------------------------------------------------------------------------

enum class Signal { nyquist, dc, sine, impulse };

void fill_signal(Signal signal, float *out, int n, int block, float level) {
    const int start = block * n;
    switch (signal) {
        case Signal::nyquist: fill_nyquist(out, n, start, level); break;
        case Signal::dc: fill_dc(out, n, level); break;
        case Signal::sine: fill_sine(out, n, start, 440.0f, level); break;
        case Signal::impulse:
            for (int i = 0; i < n; i++) out[i] = 0.0f;
            if (block == 0) out[0] = level;
            break;
    }
}

// Runs `blocks` blocks of `signal` through a fresh chain; `buses` holds the last block only.
void run_chain(const TrackParams &params, const ChainMix &mix, Signal signal, float level,
               int blocks, Buses &buses, const float *duck = nullptr) {
    InsertChain chain;
    chain.init(kRate);
    float mono[kFrames];
    for (int b = 0; b < blocks; b++) {
        fill_signal(signal, mono, kFrames, b, level);
        buses.clear();
        chain.prepare(params, mix);
        chain.process(mono, kFrames, buses.out(duck));
    }
}

// --- running the mixer -----------------------------------------------------------------------

// One mixer block with a single track on track 0. `mono` may be null for silence.
void mixer_block(Mixer &mixer, const SendSettings &sends, const MasterSettings &master,
                 const TrackParams &params, const ChainMix &mix, const float *mono_in,
                 float *out) {
    mixer.prepare(sends, master);
    mixer.chain(0).prepare(params, mix);
    mixer.begin_block(kFrames);
    float *mono = mixer.scratch();
    for (int i = 0; i < kFrames; i++) mono[i] = mono_in ? mono_in[i] : 0.0f;
    mixer.add_track(0, mono);
    mixer.finish_block(out, kFrames);
}

// The delay's time glides to its setting at 0.0005 a sample, so a timing test has to let it
// settle first. A non-zero send gain keeps the delay running on a silent feed, which is exactly
// what the tail counter in buses.cpp promises.
void settle_delay(Mixer &mixer, const SendSettings &sends, const MasterSettings &master,
                  const TrackParams &params, const ChainMix &mix) {
    float out[2 * kFrames];
    for (int b = 0; b < 500; b++) mixer_block(mixer, sends, master, params, mix, nullptr, out);
}

// Captures the left channel of `blocks` mixer blocks into g_capture, with one impulse at 0.
int capture_impulse(Mixer &mixer, const SendSettings &sends, const MasterSettings &master,
                    const TrackParams &params, const ChainMix &mix, int blocks) {
    float out[2 * kFrames];
    float mono[kFrames];
    int captured = 0;
    for (int b = 0; b < blocks; b++) {
        fill_signal(Signal::impulse, mono, kFrames, b, 1.0f);
        mixer_block(mixer, sends, master, params, mix, mono, out);
        for (int i = 0; i < kFrames; i++) g_capture[captured++] = out[2 * i];
    }
    return captured;
}

// A master that does as little as possible, so a test measures one stage at a time.
MasterSettings transparent_master() {
    MasterSettings master;
    master.dj = 0.0f;
    master.compressor = 0.0f;
    master.limit = 0.98f;
    master.level = 1.0f;
    return master;
}

SendSettings silent_sends() {
    SendSettings sends;
    sends.delay_level = 0.0f;
    sends.delay_to_reverb = 0.0f;
    sends.reverb_level = 0.0f;
    return sends;
}

}  // namespace

// --- the filter ------------------------------------------------------------------------------

TEST(chain_low_pass_takes_out_a_nyquist_signal) {
    TrackParams open = neutral_params();
    TrackParams filtered = open;
    filtered.set(kMod, kModLp12);
    filtered.set(kCut, 20);        // about 58 Hz

    ChainMix mix;
    Buses reference, low_pass;
    run_chain(open, mix, Signal::nyquist, 1.0f, 4, reference);
    run_chain(filtered, mix, Signal::nyquist, 1.0f, 4, low_pass);

    CHECK(peak_of(reference.l, kFrames) > 0.6f);
    CHECK(peak_of(low_pass.l, kFrames) < 0.05f * peak_of(reference.l, kFrames));
}

TEST(chain_high_pass_takes_out_dc) {
    TrackParams open = neutral_params();
    TrackParams filtered = open;
    filtered.set(kMod, kModHp12);
    filtered.set(kCut, 100);       // about 4.2 kHz

    ChainMix mix;
    Buses reference, high_pass;
    run_chain(open, mix, Signal::dc, 1.0f, 20, reference);
    run_chain(filtered, mix, Signal::dc, 1.0f, 20, high_pass);

    CHECK(peak_of(reference.l, kFrames) > 0.6f);
    CHECK(peak_of(high_pass.l, kFrames) < 0.01f);
}

TEST(chain_band_pass_takes_out_dc_and_nyquist) {
    TrackParams band = neutral_params();
    band.set(kMod, kModBp12);
    band.set(kCut, 64);            // about 610 Hz

    ChainMix mix;
    Buses dc, nyquist, through;
    run_chain(band, mix, Signal::dc, 1.0f, 20, dc);
    run_chain(band, mix, Signal::nyquist, 1.0f, 20, nyquist);
    // 610 Hz through a band-pass centred there survives, so the rejections mean something.
    TrackParams band_at_centre = band;
    run_chain(band_at_centre, mix, Signal::sine, 1.0f, 20, through);

    CHECK(peak_of(dc.l, kFrames) < 0.05f);
    CHECK(peak_of(nyquist.l, kFrames) < 0.05f);
    CHECK(peak_of(through.l, kFrames) > 0.1f);
}

TEST(chain_24_db_attenuates_more_than_12_db) {
    TrackParams twelve = neutral_params();
    twelve.set(kMod, kModLp12);
    twelve.set(kCut, 70);          // about 1.4 kHz, so 12 dB still passes something
    TrackParams twentyfour = twelve;
    twentyfour.set(kMod, kModLp24);

    ChainMix mix;
    Buses a, b;
    run_chain(twelve, mix, Signal::nyquist, 1.0f, 8, a);
    run_chain(twentyfour, mix, Signal::nyquist, 1.0f, 8, b);

    const float energy_12 = energy_of(a.l, kFrames);
    const float energy_24 = energy_of(b.l, kFrames);
    CHECK(energy_12 > 0.0f);
    CHECK(energy_24 < energy_12);

    // The same at the high-pass end, where the rejected band is DC.
    TrackParams hp12 = neutral_params();
    hp12.set(kMod, kModHp12);
    hp12.set(kCut, 60);
    TrackParams hp24 = hp12;
    hp24.set(kMod, kModHp24);
    Buses c, d;
    run_chain(hp12, mix, Signal::sine, 1.0f, 8, c);
    run_chain(hp24, mix, Signal::sine, 1.0f, 8, d);
    CHECK(energy_of(d.l, kFrames) < energy_of(c.l, kFrames));
}

TEST(chain_filter_mode_knob_covers_seven_zones) {
    InsertChain chain;
    chain.init(kRate);
    ChainMix mix;
    TrackParams p = neutral_params();

    const uint8_t values[7] = {kModOff, kModLp12, kModLp24, kModBp12, kModBp24, kModHp12, kModHp24};
    for (int zone = 0; zone < 7; zone++) {
        p.set(kMod, values[zone]);
        chain.prepare(p, mix);
        CHECK_EQ(int(chain.mode()), zone);
    }
    // The ends of the knob are OFF and the steepest high-pass.
    p.set(kMod, 0);
    chain.prepare(p, mix);
    CHECK_EQ(int(chain.mode()), int(FilterMode::off));
    p.set(kMod, 127);
    chain.prepare(p, mix);
    CHECK_EQ(int(chain.mode()), int(FilterMode::hp24));
}

// --- drive -----------------------------------------------------------------------------------

TEST(chain_drive_raises_a_quiet_signal) {
    TrackParams quiet = neutral_params();
    quiet.set(kPan, kPanLeft);     // a hard pan gives a gain of exactly 1, so nothing else moves
    TrackParams driven = quiet;
    driven.set(kDrv, 127);

    ChainMix mix;
    Buses clean, hot;
    run_chain(quiet, mix, Signal::sine, 0.05f, 3, clean);
    run_chain(driven, mix, Signal::sine, 0.05f, 3, hot);

    CHECK(peak_of(clean.l, kFrames) < 0.06f);
    CHECK(peak_of(hot.l, kFrames) > 4.0f * peak_of(clean.l, kFrames));
}

TEST(chain_drive_stays_inside_full_scale) {
    TrackParams driven = neutral_params();
    driven.set(kPan, kPanLeft);
    driven.set(kDrv, 127);

    ChainMix mix;
    Buses hot;
    run_chain(driven, mix, Signal::sine, 6.0f, 3, hot);     // deliberately far past full scale
    CHECK(peak_of(hot.l, kFrames) <= 1.0f);

    Buses nyquist;
    run_chain(driven, mix, Signal::nyquist, 6.0f, 3, nyquist);
    CHECK(peak_of(nyquist.l, kFrames) <= 1.0f);
}

TEST(chain_drive_is_asymmetric) {
    TrackParams driven = neutral_params();
    driven.set(kPan, kPanLeft);
    driven.set(kDrv, 127);

    ChainMix mix;
    Buses out;
    run_chain(driven, mix, Signal::sine, 0.08f, 3, out);

    float high = 0.0f, low = 0.0f;
    for (int i = 0; i < kFrames; i++) {
        if (out.l[i] > high) high = out.l[i];
        if (out.l[i] < low) low = out.l[i];
    }
    CHECK(high > 0.0f);
    CHECK(low < 0.0f);
    CHECK(high > -low * 1.1f);     // the positive half is driven harder: even harmonics
}

// --- lo-fi -----------------------------------------------------------------------------------

TEST(chain_lofi_off_is_bit_identical) {
    // Hard left at full level is a gain of exactly 1, so with every stage bypassed the left
    // channel must be the machine's block, sample for sample.
    TrackParams p = neutral_params();
    p.set(kPan, kPanLeft);
    CHECK_EQ(int(p.get(kLof)), 0);     // lo-fi is off by default

    InsertChain chain;
    chain.init(kRate);
    ChainMix mix;

    float mono[kFrames], expected[kFrames];
    Buses buses;
    for (int b = 0; b < 2; b++) {      // the first block ramps the gain up from zero
        fill_sine(mono, kFrames, 0, 440.0f, 0.5f);
        for (int i = 0; i < kFrames; i++) expected[i] = mono[i];
        buses.clear();
        chain.prepare(p, mix);
        chain.process(mono, kFrames, buses.out());
    }
    for (int i = 0; i < kFrames; i++) CHECK_EQ(buses.l[i], expected[i]);
    CHECK_EQ(peak_of(buses.r, kFrames), 0.0f);
}

TEST(chain_lofi_quantizes_at_full_amount) {
    TrackParams clean = neutral_params();
    clean.set(kPan, kPanLeft);
    TrackParams crushed = clean;
    crushed.set(kLof, 127);

    ChainMix mix;
    Buses smooth, rough;
    run_chain(clean, mix, Signal::sine, 0.9f, 3, smooth);
    run_chain(crushed, mix, Signal::sine, 0.9f, 3, rough);

    CHECK(distinct_values(smooth.l, kFrames) > 32);
    CHECK(distinct_values(rough.l, kFrames) < 12);
}

// --- pan and level ---------------------------------------------------------------------------

TEST(chain_pan_hard_left_and_right) {
    TrackParams left = neutral_params();
    left.set(kPan, kPanLeft);
    TrackParams right = neutral_params();
    right.set(kPan, kPanRight);

    ChainMix mix;
    Buses a, b;
    run_chain(left, mix, Signal::dc, 0.5f, 2, a);
    run_chain(right, mix, Signal::dc, 0.5f, 2, b);

    CHECK(peak_of(a.l, kFrames) > 0.4f);
    CHECK_EQ(peak_of(a.r, kFrames), 0.0f);
    CHECK_EQ(peak_of(b.l, kFrames), 0.0f);
    CHECK(peak_of(b.r, kFrames) > 0.4f);
}

TEST(chain_pan_centre_is_balanced) {
    TrackParams centre = neutral_params();
    ChainMix mix;
    Buses out;
    run_chain(centre, mix, Signal::dc, 0.5f, 2, out);

    CHECK(peak_of(out.l, kFrames) > 0.3f);
    for (int i = 0; i < kFrames; i++) CHECK_EQ(out.l[i], out.r[i]);
    // Constant power: a centred track is about -3 dB in each channel, not -6.
    CHECK_NEAR(out.l[kFrames - 1], 0.5f * 0.7071f, 0.002);
}

TEST(chain_level_zero_is_silent) {
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    p.set(kSnb, 127);

    ChainMix mix;
    mix.level = 0.0f;
    Buses out;
    run_chain(p, mix, Signal::dc, 1.0f, 4, out);

    CHECK_EQ(peak_of(out.l, kFrames), 0.0f);
    CHECK_EQ(peak_of(out.r, kFrames), 0.0f);
    CHECK_EQ(peak_of(out.a, kFrames), 0.0f);   // the sends are post-fader, so they go too
    CHECK_EQ(peak_of(out.b, kFrames), 0.0f);
}

TEST(chain_sends_are_post_fader) {
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    p.set(kSnb, 64);

    ChainMix full;
    Buses loud;
    run_chain(p, full, Signal::dc, 1.0f, 2, loud);
    CHECK_NEAR(loud.a[kFrames - 1], 1.0f, 1e-5);
    CHECK_NEAR(loud.b[kFrames - 1], 64.0f / 127.0f, 1e-5);

    ChainMix half;
    half.level = 0.5f;
    Buses quiet;
    run_chain(p, half, Signal::dc, 1.0f, 2, quiet);
    CHECK_NEAR(quiet.a[kFrames - 1], 0.5f, 1e-5);
}

// --- the duck --------------------------------------------------------------------------------

TEST(duck_envelope_attacks_holds_and_releases) {
    Duck duck;
    duck.init(kRate);
    duck.set(0.002f, 0.010f, 0.020f);   // 96, 480 and 960 samples

    float curve[kFrames];
    CHECK(!duck.render(curve, kFrames));    // idle renders nothing at all
    CHECK(!duck.active());

    duck.trigger(0);
    CHECK(duck.active());
    CHECK(duck.render(curve, kFrames));
    CHECK(curve[0] < curve[kFrames - 1]);   // rising
    CHECK(curve[0] >= 0.0f);

    // Fully ducked a couple of blocks in, and still held.
    for (int b = 0; b < 4; b++) duck.render(curve, kFrames);
    CHECK_NEAR(duck.amount(), 1.0f, 1e-6);

    // Open again well after hold plus release (1536 samples, 24 blocks).
    for (int b = 0; b < 40; b++) duck.render(curve, kFrames);
    CHECK_EQ(duck.amount(), 0.0f);
    CHECK(!duck.active());
    CHECK(!duck.render(curve, kFrames));    // and idle again, so it costs nothing again
}

TEST(chain_duck_attenuates_after_a_trigger_and_recovers) {
    TrackParams p = neutral_params();
    p.set(kPan, kPanLeft);

    ChainMix mix;
    mix.duck = 1.0f;

    Duck duck;
    duck.init(kRate);
    duck.set(0.002f, 0.010f, 0.020f);

    InsertChain chain;
    chain.init(kRate);

    float curve[kFrames], mono[kFrames];
    Buses buses;

    // One block before the trigger: the duck is idle, so the chain sees no curve at all.
    bool active = duck.render(curve, kFrames);
    CHECK(!active);
    fill_dc(mono, kFrames, 1.0f);
    buses.clear();
    chain.prepare(p, mix);
    chain.process(mono, kFrames, buses.out(nullptr));
    chain.prepare(p, mix);   // a second block, so the gain ramp has arrived
    fill_dc(mono, kFrames, 1.0f);
    buses.clear();
    chain.process(mono, kFrames, buses.out(nullptr));
    const float open = peak_of(buses.l, kFrames);
    CHECK_NEAR(open, 1.0f, 1e-5);

    // Trigger, then measure a block once the attack is over.
    duck.trigger(0);
    float ducked = 1.0f;
    for (int b = 0; b < 4; b++) {
        active = duck.render(curve, kFrames);
        CHECK(active);
        fill_dc(mono, kFrames, 1.0f);
        buses.clear();
        chain.prepare(p, mix);
        chain.process(mono, kFrames, buses.out(curve));
        ducked = peak_of(buses.l, kFrames);
    }
    CHECK(ducked < 0.1f);

    // And it comes back.
    for (int b = 0; b < 40; b++) {
        const float *feed = duck.render(curve, kFrames) ? curve : nullptr;
        fill_dc(mono, kFrames, 1.0f);
        buses.clear();
        chain.prepare(p, mix);
        chain.process(mono, kFrames, buses.out(feed));
    }
    CHECK_NEAR(peak_of(buses.l, kFrames), 1.0f, 1e-5);
}

TEST(chain_duck_depth_zero_is_bypassed) {
    TrackParams p = neutral_params();
    p.set(kPan, kPanLeft);

    ChainMix mix;
    mix.duck = 0.0f;

    float curve[kFrames];
    for (int i = 0; i < kFrames; i++) curve[i] = 1.0f;     // fully ducked, and ignored

    Buses out;
    run_chain(p, mix, Signal::dc, 1.0f, 2, out, curve);
    CHECK_NEAR(peak_of(out.l, kFrames), 1.0f, 1e-5);
}

// --- the master ------------------------------------------------------------------------------

TEST(master_limiter_keeps_an_overdriven_mix_inside_full_scale) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends = silent_sends();
    MasterSettings master;
    master.limit = 0.8f;

    TrackParams p = neutral_params();
    ChainMix mix;

    float mono[kFrames], out[2 * kFrames];
    for (int b = 0; b < 8; b++) {
        mixer.prepare(sends, master);
        mixer.begin_block(kFrames);
        for (int t = 0; t < Mixer::kTracks; t++) {
            mixer.chain(t).prepare(p, mix);
            fill_sine(mono, kFrames, b * kFrames, 220.0f, 4.0f);   // eight loud tracks
            float *scratch = mixer.scratch();
            for (int i = 0; i < kFrames; i++) scratch[i] = mono[i];
            mixer.add_track(t, scratch);
        }
        mixer.finish_block(out, kFrames);
        for (int i = 0; i < 2 * kFrames; i++) {
            CHECK(is_finite(out[i]));
            CHECK(absolute(out[i]) <= 1.0f);
        }
    }
    CHECK(peak_of(out, 2 * kFrames) > 0.5f);   // it limited rather than muted
}

TEST(master_dj_filter_is_flat_at_the_centre) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();

    TrackParams p = neutral_params();
    p.set(kPan, kPanLeft);
    ChainMix mix;

    float mono[kFrames], expected[kFrames], out[2 * kFrames];
    for (int b = 0; b < 2; b++) {
        fill_sine(mono, kFrames, 0, 440.0f, 0.5f);
        for (int i = 0; i < kFrames; i++) expected[i] = mono[i];
        mixer_block(mixer, sends, master, p, mix, mono, out);
    }
    // Flat means bypassed: the DJ filter, the compressor, the gain ramp and the limiter are all
    // skipped, so the interleaved output carries the track untouched.
    for (int i = 0; i < kFrames; i++) {
        CHECK_EQ(out[2 * i], expected[i]);
        CHECK_EQ(out[2 * i + 1], 0.0f);
    }
}

TEST(master_dj_filter_attenuates_at_both_extremes) {
    SendSettings sends = silent_sends();
    TrackParams p = neutral_params();
    ChainMix mix;

    float mono[kFrames], out[2 * kFrames];

    // Hard left on the knob: a low-pass, so a near-Nyquist mix disappears.
    {
        Mixer mixer;
        mixer.init(kRate);
        MasterSettings master = transparent_master();
        float flat_peak = 0.0f, filtered_peak = 0.0f;
        for (int b = 0; b < 8; b++) {
            fill_nyquist(mono, kFrames, b * kFrames, 0.5f);
            mixer_block(mixer, sends, master, p, mix, mono, out);
        }
        flat_peak = peak_of(out, 2 * kFrames);
        master.dj = -1.0f;
        for (int b = 0; b < 8; b++) {
            fill_nyquist(mono, kFrames, b * kFrames, 0.5f);
            mixer_block(mixer, sends, master, p, mix, mono, out);
        }
        filtered_peak = peak_of(out, 2 * kFrames);
        CHECK(flat_peak > 0.3f);
        CHECK(filtered_peak < 0.05f * flat_peak);
    }

    // Hard right: a high-pass, so DC disappears.
    {
        Mixer mixer;
        mixer.init(kRate);
        MasterSettings master = transparent_master();
        master.dj = 1.0f;
        for (int b = 0; b < 24; b++) {
            fill_dc(mono, kFrames, 0.5f);
            mixer_block(mixer, sends, master, p, mix, mono, out);
        }
        CHECK(peak_of(out, 2 * kFrames) < 0.02f);
    }
}

TEST(master_output_level_zero_is_silent) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();
    master.level = 0.0f;

    TrackParams p = neutral_params();
    ChainMix mix;
    float mono[kFrames], out[2 * kFrames];
    for (int b = 0; b < 4; b++) {
        fill_dc(mono, kFrames, 1.0f);
        mixer_block(mixer, sends, master, p, mix, mono, out);
    }
    CHECK_NEAR(peak_of(out, 2 * kFrames), 0.0f, 1e-6);
}

TEST(mixer_output_is_interleaved_left_right) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();

    TrackParams p = neutral_params();
    p.set(kPan, kPanRight);        // everything in the right channel
    ChainMix mix;

    float mono[kFrames], out[2 * kFrames];
    for (int b = 0; b < 2; b++) {
        fill_dc(mono, kFrames, 0.5f);
        mixer_block(mixer, sends, master, p, mix, mono, out);
    }
    for (int i = 0; i < kFrames; i++) {
        CHECK_EQ(out[2 * i], 0.0f);          // even samples are L
        CHECK(out[2 * i + 1] > 0.4f);        // odd samples are R
    }
}

// --- the sends -------------------------------------------------------------------------------

TEST(mixer_delay_repeats_at_about_the_set_time) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends;
    sends.delay_seconds = 0.05f;        // 2400 samples
    sends.delay_feedback = 0.3f;
    sends.delay_tone = 1.0f;
    sends.delay_ping_pong = false;
    sends.delay_level = 1.0f;
    sends.delay_to_reverb = 0.0f;
    sends.reverb_level = 0.0f;
    MasterSettings master = transparent_master();

    TrackParams p = neutral_params();
    p.set(kSna, 127);
    ChainMix mix;

    settle_delay(mixer, sends, master, p, mix);
    const int captured = capture_impulse(mixer, sends, master, p, mix, 120);

    int at = -1;
    float best = 0.0f;
    for (int i = 600; i < captured; i++) {
        const float a = absolute(g_capture[i]);
        if (a > best) {
            best = a;
            at = i;
        }
    }
    CHECK(best > 0.1f);               // there is a repeat
    CHECK(at > 2200 && at < 2600);    // at roughly the time that was asked for
}

namespace {

// One block through the mixer: track 0 fed `mono` when `feed`, otherwise not rendered at all --
// which is what the engine does with a track that has gone idle. Returns the block's peak.
float tail_block(Mixer &mixer, const SendSettings &sends, const MasterSettings &master,
                 const TrackParams &p, bool feed, float impulse) {
    static float mono[kFrames], out[2 * kFrames];
    ChainMix mix;
    mixer.prepare(sends, master);
    mixer.begin_block(kFrames);
    if (feed) {
        for (int i = 0; i < kFrames; i++) mono[i] = i == 0 ? impulse : 0.0f;
        mixer.chain(0).prepare(p, mix);
        mixer.add_track(0, mono);
    }
    mixer.finish_block(out, kFrames);
    float peak = 0.0f;
    for (int i = 0; i < 2 * kFrames; i++) peak = absolute(out[i]) > peak ? absolute(out[i]) : peak;
    return peak;
}

}  // namespace

TEST(mixer_a_long_delay_tail_is_not_cut_off_and_a_finished_one_leaves_nothing_to_replay) {
    // The tail gate stopped the delay after at most 8 s: at feedback 0.95 on a 1 s delay the
    // echoes were still plainly audible, and stopped dead. The frozen line then replayed the next
    // time the send was fed. The gate now holds until the tail is 60 dB down.
    static Mixer mixer;
    mixer.init(kRate);
    SendSettings sends;
    sends.delay_seconds = 1.0f;
    sends.delay_feedback = 0.95f;
    sends.delay_tone = 1.0f;
    sends.delay_ping_pong = false;
    sends.delay_level = 1.0f;
    sends.delay_to_reverb = 0.0f;
    sends.reverb_level = 0.0f;
    const MasterSettings master = transparent_master();
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    p.set(kSnb, 0);

    const int per_second = int(kRate) / kFrames;
    tail_block(mixer, sends, master, p, true, 1.0f);
    float first = 0.0f, at_ten = 0.0f;
    for (int b = 1; b < per_second * 11; b++) {
        const float peak = tail_block(mixer, sends, master, p, false, 0.0f);
        if (b < per_second * 2) first = peak > first ? peak : first;
        if (b >= per_second * 10) at_ten = peak > at_ten ? peak : at_ten;
    }
    CHECK(first > 0.01f);
    // Measured 0.91 and 0.15: the tone filter softens each pass, so the peak falls faster than
    // 0.95 per repeat, but -15 dB is still plainly audible. The old gate gave exactly 0 here.
    CHECK(at_ten > 0.05f * first);

    // A short, light delay, run out, then fed silence: nothing audible comes back.
    static Mixer quick;
    quick.init(kRate);
    sends.delay_seconds = 0.2f;
    sends.delay_feedback = 0.5f;
    tail_block(quick, sends, master, p, true, 1.0f);
    float echo = 0.0f;
    for (int b = 1; b < per_second * 4; b++) {
        const float peak = tail_block(quick, sends, master, p, false, 0.0f);
        echo = peak > echo ? peak : echo;
    }
    float replay = 0.0f;
    for (int b = 0; b < per_second; b++) {
        const float peak = tail_block(quick, sends, master, p, true, 0.0f);
        replay = peak > replay ? peak : replay;
    }
    CHECK(echo > 0.01f);
    CHECK(replay < 0.002f * echo);
}

TEST(mixer_reverb_produces_a_decaying_tail) {
    Mixer mixer;
    mixer.init(kRate);

    SendSettings sends;
    sends.delay_level = 0.0f;
    sends.delay_to_reverb = 0.0f;
    sends.reverb_decay_s = 0.3f;
    sends.reverb_damping = 0.3f;
    sends.reverb_level = 1.0f;
    MasterSettings master = transparent_master();

    TrackParams p = neutral_params();
    p.set(kSnb, 127);              // send B only: the delay is never fed
    ChainMix mix;

    const int captured = capture_impulse(mixer, sends, master, p, mix, 150);
    CHECK_EQ(captured, 150 * kFrames);

    // The network's output comes off its delay lines, the shortest of which is 1123 samples, so
    // the tail starts there rather than at the dry hit.
    const float early = energy_of(g_capture + 1200, 1024);
    const float late = energy_of(g_capture + 8000, 1024);
    CHECK(early > 0.0f);
    CHECK(late > 0.0f);                 // a tail, not a gate
    CHECK(late < 0.25f * early);        // and it decays
}

TEST(mixer_delay_can_feed_the_reverb) {
    SendSettings sends;
    sends.delay_seconds = 0.03f;       // 1440 samples
    sends.delay_feedback = 0.0f;
    sends.delay_ping_pong = false;
    sends.delay_level = 0.0f;          // the delay is not heard directly at all
    sends.delay_to_reverb = 1.0f;
    sends.reverb_decay_s = 0.5f;
    sends.reverb_level = 1.0f;
    MasterSettings master = transparent_master();

    TrackParams p = neutral_params();
    p.set(kSna, 127);                  // send A only
    p.set(kSnb, 0);
    ChainMix mix;

    float with_feed = 0.0f, without_feed = 0.0f;
    {
        Mixer mixer;
        mixer.init(kRate);
        settle_delay(mixer, sends, master, p, mix);
        const int captured = capture_impulse(mixer, sends, master, p, mix, 60);
        with_feed = energy_of(g_capture + 1600, captured - 1600);
    }
    {
        SendSettings no_feed = sends;
        no_feed.delay_to_reverb = 0.0f;
        Mixer mixer;
        mixer.init(kRate);
        settle_delay(mixer, no_feed, master, p, mix);
        const int captured = capture_impulse(mixer, no_feed, master, p, mix, 60);
        without_feed = energy_of(g_capture + 1600, captured - 1600);
    }
    // The only route from the track to the output after the echo time is delay -> reverb.
    CHECK(with_feed > 1e-6f);
    CHECK_EQ(without_feed, 0.0f);
}

// --- extremes --------------------------------------------------------------------------------

TEST(chain_stays_finite_at_parameter_extremes) {
    const uint8_t extremes[2] = {0, 127};
    for (int which = 0; which < 2; which++) {
        TrackParams p = params_all(extremes[which]);

        ChainMix mix;
        mix.level = which ? 4.0f : 1.0f;
        mix.duck = which ? 1.0f : 0.0f;

        float curve[kFrames];
        for (int i = 0; i < kFrames; i++) curve[i] = 1.0f;

        InsertChain chain;
        chain.init(kRate);
        ed::Random random(0x1234567u);

        float mono[kFrames];
        Buses buses;
        for (int b = 0; b < 60; b++) {
            fill_noise(mono, kFrames, random, 4.0f);
            buses.clear();
            chain.prepare(p, mix);
            chain.process(mono, kFrames, buses.out(curve));
            for (int i = 0; i < kFrames; i++) {
                CHECK(is_finite(buses.l[i]));
                CHECK(is_finite(buses.r[i]));
                CHECK(is_finite(buses.a[i]));
                CHECK(is_finite(buses.b[i]));
            }
        }
    }
}

TEST(mixer_stays_finite_and_bounded_at_parameter_extremes) {
    const uint8_t extremes[2] = {0, 127};
    for (int which = 0; which < 2; which++) {
        Mixer mixer;
        mixer.init(kRate);

        SendSettings sends;
        sends.delay_seconds = which ? 4.0f : 0.0f;     // both out of range, so both get clamped
        sends.delay_feedback = which ? 2.0f : -1.0f;
        sends.delay_tone = float(which);
        sends.delay_ping_pong = which != 0;
        sends.delay_level = which ? 4.0f : 0.0f;
        sends.delay_to_reverb = float(which);
        sends.reverb_decay_s = which ? 30.0f : 0.0f;
        sends.reverb_damping = float(which);
        sends.reverb_high_pass_hz = which ? 4000.0f : 0.0f;
        sends.reverb_level = which ? 4.0f : 0.0f;

        MasterSettings master;
        master.dj = which ? 1.0f : -1.0f;
        master.compressor = float(which);
        master.limit = which ? 0.98f : 0.0f;
        master.level = 1.0f;

        TrackParams p = params_all(extremes[which]);
        ChainMix mix;
        mix.level = which ? 4.0f : 1.0f;
        mix.duck = float(which);

        ed::Random random(0x89ABCDEu);
        float out[2 * kFrames];
        for (int b = 0; b < 120; b++) {
            if (b % 7 == 0) mixer.trigger_duck(uint32_t(b % kFrames));
            mixer.prepare(sends, master);
            mixer.begin_block(kFrames);
            for (int t = 0; t < Mixer::kTracks; t++) {
                mixer.chain(t).prepare(p, mix);
                float *scratch = mixer.scratch();
                fill_noise(scratch, kFrames, random, 4.0f);
                mixer.add_track(t, scratch);
            }
            mixer.finish_block(out, kFrames);
            for (int i = 0; i < 2 * kFrames; i++) {
                CHECK(is_finite(out[i]));
                CHECK(absolute(out[i]) <= 1.0f);   // the limiter's promise, whatever happens
            }
        }
    }
}

TEST(chain_parameter_names_cover_its_eight_slots) {
    const int owned[8] = {kDrv, kCut, kRes, kMod, kLof, kPan, kSna, kSnb};
    for (int i = 0; i < 8; i++) {
        const char *name = InsertChain::param_name(owned[i]);
        CHECK(name[0] != '-');
        CHECK(name[0] != 0 && name[1] != 0 && name[2] != 0 && name[3] == 0);
    }
    // The machine's own pages stay the machine's.
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(InsertChain::param_name(page_first(Page::src) + i)[0], '-');
        CHECK_EQ(InsertChain::param_name(page_first(Page::amp) + i)[0], '-');
    }
}

// --- the duck's trigger is deferred, not dropped --------------------------------------------

TEST(duck_carries_a_trigger_aimed_past_the_end_of_the_block) {
    // `Duck::render` used to clear `pending_` unconditionally and then look for the frame inside
    // the block. For a frame past the block's end the equality never fired, so the trigger was
    // LOST rather than deferred -- and the only caller hands it `StepEvent::frame`, whose later
    // ratchet hits sit thousands of frames into the step. A kick roll on the duck-source track
    // pumped once and then not at all.
    //
    // dsp/chain.hpp's contract states no precondition on the frame, so the code must not have
    // one. app/engine.cpp defers those events itself now; this is the other half of it.
    Duck duck;
    duck.init(kRate);
    duck.set(0.003f, 0.040f, 0.120f);

    // Frame 2769: where a 2-hit ratchet's second hit lands at 130 BPM, and 43 blocks away.
    duck.trigger(2769);

    float curve[kFrames];
    float reached = 0.0f;
    int fired_block = -1;
    for (int b = 0; b < 80; b++) {
        for (int i = 0; i < kFrames; i++) curve[i] = 0.0f;
        const bool active = duck.render(curve, kFrames);
        float peak = 0.0f;
        for (int i = 0; i < kFrames; i++) {
            if (curve[i] > peak) peak = curve[i];
        }
        if (peak > 0.01f && fired_block < 0) fired_block = b;
        if (peak > reached) reached = peak;
        (void)active;
    }
    CHECK(fired_block >= 0);                 // it fired at all, which it did not before
    CHECK_EQ(fired_block, 2769 / kFrames);      // in the block its frame falls in
    CHECK(reached > 0.9f);                   // and ducked fully

    // And one frame past the block end is the boundary that used to swallow it silently.
    Duck edge;
    edge.init(kRate);
    edge.set(0.003f, 0.040f, 0.120f);
    edge.trigger(uint32_t(kFrames));
    for (int i = 0; i < kFrames; i++) curve[i] = 0.0f;
    edge.render(curve, kFrames);
    float first = 0.0f;
    for (int i = 0; i < kFrames; i++) {
        if (curve[i] > first) first = curve[i];
    }
    CHECK_EQ(first, 0.0f);                   // nothing in the block before it
    for (int i = 0; i < kFrames; i++) curve[i] = 0.0f;
    CHECK(edge.render(curve, kFrames));
    CHECK(curve[0] > 0.0f);                  // and it fires on frame 0 of the next
}

// --- the encoder domain: bytes in, physical values out ----------------------------------------
//
// Every send and master field is a BYTE under the encoder and in PROJECT.BIN (core/project.hpp
// says why), and a float only once it reaches the mixer. dsp/buses.cpp is the only place that
// knows what a byte means, so this is where the mapping is pinned.

TEST(buses_a_division_byte_round_trips_to_its_index) {
    // The byte a division is stored as has to read back as THAT division. The bands are 12.8
    // bytes wide -- 128 does not divide by ten -- and rounding the band down to twelve put the
    // top division's byte in the band below it, so `delay_division_byte(9)` came back as 8.
    for (int i = 0; i < kDelayDivisions; i++) {
        CHECK_EQ(delay_division_index(delay_division_byte(i)), i);
    }

    // Out of range is clamped at both ends rather than reading off the table.
    CHECK_EQ(delay_division_index(delay_division_byte(-5)), 0);
    CHECK_EQ(delay_division_index(delay_division_byte(99)), kDelayDivisions - 1);

    // And every byte an encoder can produce names a division, with the two ends where they
    // belong -- a byte is 0-127 and nothing may read past the table.
    CHECK_EQ(delay_division_index(0), 0);
    CHECK_EQ(delay_division_index(127), kDelayDivisions - 1);
    CHECK_EQ(delay_division_index(255), kDelayDivisions - 1);
    for (int b = 0; b <= 255; b++) {
        const int index = delay_division_index(uint8_t(b));
        CHECK(index >= 0 && index < kDelayDivisions);
    }

    // The names are four characters at most: the screen's slot is that wide.
    for (int i = 0; i < kDelayDivisions; i++) {
        const char *name = delay_division_name(delay_division_byte(i));
        int length = 0;
        while (name[length] != 0) length++;
        CHECK(length >= 3 && length <= 5);
    }
    CHECK_EQ(delay_division_name(delay_division_byte(2)), "1/16");
    CHECK_EQ(delay_division_name(delay_division_byte(5)), "1/8D");
}

TEST(buses_a_division_is_a_time_only_once_the_tempo_is_known) {
    // docs/SPEC.md makes send A tempo-synced, so the file stores a division and dsp/ turns it
    // into seconds with the beat length the app hands it. At 120 BPM a beat is half a second.
    const float beat = 0.5f;
    CHECK_NEAR(delay_division_seconds(delay_division_byte(6), beat), 0.5f, 1e-6);   // 1/4
    CHECK_NEAR(delay_division_seconds(delay_division_byte(4), beat), 0.25f, 1e-6);  // 1/8
    CHECK_NEAR(delay_division_seconds(delay_division_byte(5), beat), 0.375f, 1e-6); // 1/8D
    CHECK_NEAR(delay_division_seconds(delay_division_byte(9), beat), 2.0f, 1e-6);   // a bar

    // The divisions are in order, shortest first, so turning the encoder up lengthens the delay.
    for (int i = 1; i < kDelayDivisions; i++) {
        CHECK(delay_division_seconds(delay_division_byte(i), beat) >
              delay_division_seconds(delay_division_byte(i - 1), beat));
    }

    // Halving the tempo doubles every division, and the triplet is a third of its straight
    // neighbour two places up -- which is what makes it a triplet rather than a nearby number.
    CHECK_NEAR(delay_division_seconds(delay_division_byte(6), 1.0f), 1.0f, 1e-6);
    CHECK_NEAR(delay_division_seconds(delay_division_byte(3), beat),
               delay_division_seconds(delay_division_byte(6), beat) / 3.0f, 1e-6);
}

TEST(buses_a_byte_maps_onto_each_field_s_own_range) {
    // The ends, which are the values that matter: a level knob at the top has to be unity gain
    // and not 0.992 of it, and a feedback at the top has to be the 0.95 the field allows and
    // not 1.0, which is a delay that never stops growing.
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::delay_level), 127, 0.5f), 1.0f, 1e-6);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::delay_level), 0, 0.5f), 0.0f, 1e-6);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::delay_feedback), 127, 0.5f), 0.95f, 1e-6);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_level), 127, 0.5f), 1.0f, 1e-6);

    // The ping-pong is a switch, so the upper half of the travel is on.
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::delay_ping_pong), 63, 0.5f), 0.0f, 1e-6);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::delay_ping_pong), 64, 0.5f), 1.0f, 1e-6);

    // The two EXPONENTIAL fields, where equal encoder steps are equal musical steps: a reverb
    // decay linear in seconds spends four fifths of its travel in cathedrals.
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_decay_s), 0, 0.5f), 0.2f, 1e-5);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_decay_s), 127, 0.5f), 30.0f, 0.01);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_high_pass_hz), 0, 0.5f), 20.0f,
               1e-4);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_high_pass_hz), 127, 0.5f), 2000.0f,
               1.0);
    // Equal steps, equal ratios: the middle of the travel is the geometric mean of the ends,
    // sqrt(0.2 * 30) = 2.449 s -- 2.498 here because byte 64 of 127 is a hair past halfway.
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_decay_s), 64, 0.5f), 2.498f, 0.01);
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::reverb_decay_s), 32, 0.5f) *
                   send_value_from_byte(uint8_t(SendField::reverb_decay_s), 96, 0.5f),
               send_value_from_byte(uint8_t(SendField::reverb_decay_s), 64, 0.5f) *
                   send_value_from_byte(uint8_t(SendField::reverb_decay_s), 64, 0.5f),
               0.01);

    // An index this build does not have reads 0 rather than off the end.
    CHECK_NEAR(send_value_from_byte(uint8_t(SendField::count), 127, 0.5f), 0.0f, 1e-6);
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::count), 127), 0.0f, 1e-6);

    // The master's one SIGNED field: 64 is exactly the DJ filter's flat, bypassed centre, which
    // has to be reachable by value and not merely nearly.
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::dj), 64), 0.0f, 1e-6);
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::dj), 0), -1.0f, 1e-6);
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::dj), 127), 1.0f, 1e-6);
    // The limiter's threshold does not reach 0, because a threshold of zero is a limiter that
    // removes everything, and it stops where `resolve_master` stops so the field reads back
    // what the limiter is actually doing.
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::limit), 0), 0.1f, 1e-6);
    CHECK_NEAR(master_value_from_byte(uint8_t(MasterField::limit), 127), 0.98f, 1e-6);
}

TEST(buses_the_default_bytes_give_the_documented_defaults) {
    // `ProjectSettings`'s fourteen bytes are the source of truth, and dsp/buses.hpp's float
    // defaults are what they are meant to mean. They have to agree, or a fresh project would
    // not sound like the one the header documents.
    const ProjectSettings defaults;
    SendSettings sends;
    MasterSettings master;
    sends_from_bytes(sends, defaults.sends, 60.0f / 130.0f);
    master_from_bytes(master, defaults.master);

    const SendSettings documented;
    const MasterSettings documented_master;
    CHECK_NEAR(sends.delay_feedback, documented.delay_feedback, 0.01);
    CHECK_NEAR(sends.delay_tone, documented.delay_tone, 0.01);
    CHECK(sends.delay_ping_pong == documented.delay_ping_pong);
    CHECK_NEAR(sends.delay_level, documented.delay_level, 0.01);
    CHECK_NEAR(sends.delay_to_reverb, documented.delay_to_reverb, 0.01);
    CHECK_NEAR(sends.reverb_decay_s, documented.reverb_decay_s, 0.05);
    CHECK_NEAR(sends.reverb_damping, documented.reverb_damping, 0.01);
    CHECK_NEAR(sends.reverb_high_pass_hz, documented.reverb_high_pass_hz, 5.0);
    CHECK_NEAR(sends.reverb_level, documented.reverb_level, 0.01);
    CHECK_NEAR(master.dj, documented_master.dj, 1e-6);
    CHECK_NEAR(master.compressor, documented_master.compressor, 0.01);
    CHECK_NEAR(master.limit, documented_master.limit, 0.01);
    CHECK_NEAR(master.level, documented_master.level, 0.01);

    // The delay's default is a DIVISION, so it is a time only at a tempo: dotted 1/8, which at
    // 130 BPM is 0.346 s -- near the 0.375 the struct documents and, unlike it, on the grid.
    CHECK_EQ(delay_division_index(defaults.sends[0]), 5);
    CHECK_NEAR(sends.delay_seconds, 0.75f * 60.0f / 130.0f, 1e-5);
}

TEST(buses_a_step_moves_a_byte_in_its_field_s_own_units) {
    // A knob: two counts per click, clamped at both ends.
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_feedback), 60, +4)), 64);
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_feedback), 60, -70)), 0);
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_feedback), 60, +120)), 127);
    CHECK_EQ(int(send_field_click(uint8_t(SendField::delay_feedback))), 2);

    // The delay's time: one DIVISION per unit, holding at both ends.
    const uint8_t at_4 = delay_division_byte(4);
    CHECK_EQ(delay_division_index(send_byte_step(uint8_t(SendField::delay_seconds), at_4, +1)), 5);
    CHECK_EQ(delay_division_index(send_byte_step(uint8_t(SendField::delay_seconds), at_4, -1)), 3);
    CHECK_EQ(delay_division_index(send_byte_step(uint8_t(SendField::delay_seconds), at_4, +99)),
             kDelayDivisions - 1);
    CHECK_EQ(delay_division_index(send_byte_step(uint8_t(SendField::delay_seconds), at_4, -99)), 0);
    CHECK_EQ(int(send_field_click(uint8_t(SendField::delay_seconds))), 1);

    // The switch: only the sign is read, and zero leaves it alone.
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_ping_pong), 0, +1)), 127);
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_ping_pong), 127, -1)), 0);
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::delay_ping_pong), 99, 0)), 99);

    // A field this build does not have is left where it was rather than written past the array.
    CHECK_EQ(int(send_byte_step(uint8_t(SendField::count), 40, +10)), 40);
    CHECK_EQ(int(master_byte_step(uint8_t(MasterField::count), 40, +10)), 40);
    CHECK_EQ(int(master_byte_step(uint8_t(MasterField::level), 40, +10)), 50);
}

TEST(buses_the_field_names_fit_the_screen) {
    // Three characters, which is what a MIXER slot and a notice have room for.
    for (uint8_t f = 0; f < uint8_t(SendField::count); f++) {
        const char *name = send_field_name(f);
        int length = 0;
        while (name[length] != 0) length++;
        CHECK(length >= 2 && length <= 3);
    }
    for (uint8_t f = 0; f < uint8_t(MasterField::count); f++) {
        const char *name = master_field_name(f);
        int length = 0;
        while (name[length] != 0) length++;
        CHECK(length >= 2 && length <= 3);
    }
    // And an index past the end says so rather than reading off the table.
    CHECK_EQ(send_field_name(uint8_t(SendField::count)), "---");
    CHECK_EQ(master_field_name(uint8_t(MasterField::count)), "---");
}

// --- gaps a mutation run found (docs/MUTATION.md) -------------------------------------------

TEST(buses_every_field_setter_stores_what_it_is_given_and_clamps_the_rest) {
    // sends_from_bytes on the default bytes lands on the defaults, so a setter that dropped its
    // value still passed: each field is set here to something that is NOT its default.
    struct Case {
        SendField field;
        float inside, low, high;   // a value in range, and the two ends of the range
    };
    const Case cases[] = {
        {SendField::delay_seconds, 1.25f, 0.001f, Mixer::kMaxDelaySeconds},
        {SendField::delay_feedback, 0.7f, 0.0f, 0.95f},
        {SendField::delay_tone, 0.2f, 0.0f, 1.0f},
        {SendField::delay_level, 0.3f, 0.0f, 1.0f},
        {SendField::delay_to_reverb, 0.6f, 0.0f, 1.0f},
        {SendField::reverb_decay_s, 7.5f, 0.2f, 30.0f},
        {SendField::reverb_damping, 0.9f, 0.0f, 1.0f},
        {SendField::reverb_high_pass_hz, 650.0f, 20.0f, 2000.0f},
        {SendField::reverb_level, 0.15f, 0.0f, 1.0f},
    };
    for (const Case &c : cases) {
        SendSettings s;
        const uint8_t f = uint8_t(c.field);
        CHECK(send_field_set(s, f, c.inside));
        CHECK_NEAR(send_field_get(s, f), c.inside, 1e-6);
        CHECK(send_field_set(s, f, c.low - 5.0f));
        CHECK_NEAR(send_field_get(s, f), c.low, 1e-6);
        CHECK(send_field_set(s, f, c.high + 50.0f));
        CHECK_NEAR(send_field_get(s, f), c.high, 1e-6);
    }
    // The switch: on from exactly half, and it reports that it took the value.
    SendSettings s;
    CHECK(send_field_set(s, uint8_t(SendField::delay_ping_pong), 0.0f));
    CHECK(!s.delay_ping_pong);
    CHECK(send_field_set(s, uint8_t(SendField::delay_ping_pong), 0.5f));
    CHECK(s.delay_ping_pong);
    CHECK(!send_field_set(s, uint8_t(SendField::count), 0.5f));
    CHECK_EQ(int(send_field_click(uint8_t(SendField::delay_ping_pong))), 1);

    MasterSettings m;
    CHECK(master_field_set(m, uint8_t(MasterField::dj), -0.4f));
    CHECK_NEAR(m.dj, -0.4f, 1e-6);
    CHECK(master_field_set(m, uint8_t(MasterField::dj), -3.0f));
    CHECK_NEAR(m.dj, -1.0f, 1e-6);
    CHECK(master_field_set(m, uint8_t(MasterField::compressor), 0.65f));
    CHECK_NEAR(m.compressor, 0.65f, 1e-6);
    CHECK(master_field_set(m, uint8_t(MasterField::limit), 0.02f));
    CHECK_NEAR(m.limit, 0.1f, 1e-6);
    CHECK(master_field_set(m, uint8_t(MasterField::level), 0.45f));
    CHECK_NEAR(m.level, 0.45f, 1e-6);
    CHECK(!master_field_set(m, uint8_t(MasterField::count), 0.5f));
}

TEST(buses_bytes_reach_every_field) {
    // The same gap from the file's side: bytes that are not the defaults must arrive in every
    // field, not only in the ones whose default happens to differ.
    uint8_t bytes[kSendFields];
    for (int f = 0; f < kSendFields; f++) bytes[f] = 100;
    SendSettings s;
    sends_from_bytes(s, bytes, 0.5f);
    for (uint8_t f = 0; f < uint8_t(SendField::count); f++) {
        CHECK_NEAR(send_field_get(s, f), send_value_from_byte(f, 100, 0.5f), 1e-5);
    }
    uint8_t master_bytes[kMasterFields] = {20, 90, 30, 70};
    MasterSettings m;
    master_from_bytes(m, master_bytes);
    for (uint8_t f = 0; f < uint8_t(MasterField::count); f++) {
        CHECK_NEAR(master_field_get(m, f), master_value_from_byte(f, master_bytes[f]), 1e-6);
    }
}

TEST(buses_a_division_out_of_range_is_stored_as_the_end_division_s_own_byte) {
    // The byte itself, not only the division it reads back as: a byte past 127 is clamped by
    // the reader, so a writer that let index 10 through still read back as division 9.
    CHECK_EQ(int(delay_division_byte(0)), 6);
    CHECK_EQ(int(delay_division_byte(9)), 121);
    CHECK_EQ(int(delay_division_byte(-1)), 6);
    CHECK_EQ(int(delay_division_byte(10)), 121);
    CHECK_EQ(int(delay_division_byte(99)), 121);
}

TEST(mixer_a_fresh_mixer_already_runs_the_default_settings) {
    // init() resolves the defaults itself, so a mixer that is handed exactly the defaults (the
    // memo sees nothing new) still has a delay and an output level, not zeros.
    static Mixer mixer;
    mixer.init(kRate);
    const SendSettings sends;
    const MasterSettings master;
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    ChainMix mix;
    float mono[kFrames], out[2 * kFrames];
    fill_dc(mono, kFrames, 0.5f);
    for (int b = 0; b < 4; b++) mixer_block(mixer, sends, master, p, mix, mono, out);
    CHECK(peak_of(out, 2 * kFrames) > 0.05f);       // the dry signal, at the output level
    float echo = 0.0f;
    const int blocks = int(0.6f * kRate) / kFrames;
    for (int b = 1; b < blocks; b++) {
        const float peak = tail_block(mixer, sends, master, p, false, 0.0f);
        echo = peak > echo ? peak : echo;
    }
    CHECK(echo > 0.05f);
}

namespace {

int blocks_running(Mixer &mixer, const SendSettings &sends, const TrackParams &p, bool delay) {
    const MasterSettings master = transparent_master();
    tail_block(mixer, sends, master, p, true, 0.5f);
    int blocks = 1;
    while (delay ? mixer.delay_running() : mixer.reverb_running()) {
        tail_block(mixer, sends, master, p, false, 0.0f);
        blocks++;
        if (blocks > 100000) break;
    }
    return blocks;
}

}  // namespace

TEST(mixer_each_send_runs_exactly_as_long_as_its_tail_takes_to_fall_60_db) {
    // The tail gate is what stops a send costing CPU after the last hit, and what stops it
    // freezing an echo that is still audible. Pinned to the block: at 48 kHz in 64-frame blocks
    // there are 750 blocks a second.
    //
    // Delay: 0.2 s at feedback 0.5 is 9.97 repeats to -60 dB, so (0.2 * 10.97 + 0.05) s =
    // 2.243 s = 1682.4 blocks, plus one: 1683, the first counting the block that fed it.
    static Mixer mixer;
    mixer.init(kRate);
    SendSettings sends = silent_sends();
    sends.delay_seconds = 0.2f;
    sends.delay_feedback = 0.5f;
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    CHECK_EQ(blocks_running(mixer, sends, p, true), 1683);

    // Reverb: its decay is the RT60, plus 20% and 0.2 s for the pre-delay and the diffusion:
    // 0.4321 s gives 0.71852 s = 538.9 blocks, plus one: 539.
    static Mixer reverb;
    reverb.init(kRate);
    SendSettings wet = silent_sends();
    wet.reverb_decay_s = 0.4321f;
    TrackParams q = neutral_params();
    q.set(kSnb, 127);
    CHECK_EQ(blocks_running(reverb, wet, q, false), 539);
}

TEST(mixer_the_output_level_scales_and_ramps_in_both_directions) {
    // Level 0.5 halves a block, and a move from 0.5 back to 1 ramps up within the next block
    // rather than staying at 0.5 (the ramp's skip test is only for unity at both ends).
    static Mixer mixer;
    mixer.init(kRate);
    const SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();
    TrackParams p = neutral_params();
    ChainMix mix;
    float mono[kFrames], out[2 * kFrames];
    fill_dc(mono, kFrames, 0.4f);

    mixer_block(mixer, sends, master, p, mix, mono, out);
    const float unity = out[2 * (kFrames - 1)];
    CHECK(unity > 0.1f);

    master.level = 0.5f;
    mixer_block(mixer, sends, master, p, mix, mono, out);   // the ramp down
    mixer_block(mixer, sends, master, p, mix, mono, out);
    CHECK_NEAR(out[0], 0.5f * unity, 1e-4);
    CHECK_NEAR(out[2 * (kFrames - 1)], 0.5f * unity, 1e-4);

    master.level = 1.0f;
    mixer_block(mixer, sends, master, p, mix, mono, out);   // the ramp up
    CHECK(out[2 * (kFrames - 1)] > 0.95f * unity);
    CHECK(out[0] < 0.6f * unity);
    mixer_block(mixer, sends, master, p, mix, mono, out);
    CHECK_NEAR(out[0], unity, 1e-5);
}

TEST(master_dj_filter_turns_from_high_pass_to_low_pass) {
    // The filter's response is chosen by the sign each time the knob moves, so going from the
    // high-pass side straight to the low-pass side must give a low-pass, not a high-pass at a
    // low-pass cutoff.
    static Mixer mixer;
    mixer.init(kRate);
    const SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();
    TrackParams p = neutral_params();
    ChainMix mix;
    float mono[kFrames], out[2 * kFrames];
    master.dj = 1.0f;
    for (int b = 0; b < 8; b++) {
        fill_dc(mono, kFrames, 0.5f);
        mixer_block(mixer, sends, master, p, mix, mono, out);
    }
    master.dj = -1.0f;
    for (int b = 0; b < 48; b++) {
        fill_dc(mono, kFrames, 0.5f);
        mixer_block(mixer, sends, master, p, mix, mono, out);
    }
    CHECK(peak_of(out, 2 * kFrames) > 0.2f);   // DC passes a low-pass
}

TEST(mixer_a_one_frame_block_is_cleared_mixed_and_mastered) {
    // A block of one frame is still a block: the bus is cleared first, a track is mixed into
    // it, and the master level applies.
    static Mixer mixer;
    mixer.init(kRate);
    const SendSettings sends = silent_sends();
    MasterSettings master = transparent_master();
    TrackParams p = neutral_params();
    ChainMix mix;
    float mono[kFrames], out[2 * kFrames];
    fill_dc(mono, kFrames, 0.4f);
    for (int b = 0; b < 4; b++) mixer_block(mixer, sends, master, p, mix, mono, out);
    const float unity = out[2 * (kFrames - 1)];
    CHECK(unity > 0.1f);

    // Nothing rendered: the frame is silent, not what the bus held last block.
    mixer.prepare(sends, master);
    mixer.begin_block(1);
    mixer.finish_block(out, 1);
    CHECK_NEAR(out[0], 0.0f, 1e-9);

    // One track, at level 0.5 once the ramp has run.
    master.level = 0.5f;
    for (int b = 0; b < 3; b++) {
        mixer.prepare(sends, master);
        mixer.chain(0).prepare(p, mix);
        mixer.begin_block(1);
        float *one = mixer.scratch();
        one[0] = 0.4f;
        mixer.add_track(0, one);
        mixer.finish_block(out, 1);
    }
    CHECK_NEAR(out[0], 0.5f * unity, 1e-4);
}

TEST(duck_init_sets_the_default_shape) {
    // init() gives the duck its documented 4 ms attack without a set(): 192 frames to full.
    Duck duck;
    duck.init(kRate);
    float curve[kFrames];
    duck.trigger(0);
    CHECK(duck.render(curve, kFrames));
    CHECK_NEAR(curve[kFrames - 1], float(kFrames) / 192.0f, 1e-3);
}

TEST(duck_reset_drops_the_envelope_and_a_pending_trigger) {
    Duck duck;
    duck.init(kRate);
    duck.set(0.004f, 0.05f, 0.14f);
    float curve[kFrames];
    duck.trigger(0);
    CHECK(duck.render(curve, kFrames));
    CHECK(curve[kFrames - 1] > 0.2f);

    // Mid-attack: reset is idle at once, with nothing carried.
    duck.reset();
    CHECK(!duck.render(curve, kFrames));

    // A trigger aimed past this block is dropped by a reset too.
    duck.trigger(uint32_t(kFrames + 10));
    duck.reset();
    CHECK(!duck.render(curve, kFrames));
    CHECK(!duck.render(curve, kFrames));

    // And the next attack starts from zero, not from where the last one was.
    duck.trigger(0);
    duck.render(curve, kFrames);
    CHECK_NEAR(curve[0], 1.0f / 192.0f, 1e-4);
}

TEST(chain_clear_state_forgets_the_filter_and_reset_ramps_up_from_silence) {
    // A resonant 24 dB low-pass rings after a loud block. clear_state() (the engine's, when a
    // track's machine changes) leaves nothing ringing into the next block.
    TrackParams p = neutral_params();
    p.set(kMod, kModLp24);
    p.set(kCut, 40);
    p.set(kRes, 120);
    ChainMix mix;
    InsertChain chain;
    chain.init(kRate);
    Buses buses;
    float mono[kFrames];
    for (int b = 0; b < 4; b++) {
        fill_nyquist(mono, kFrames, 0, 0.0f);
        fill_dc(mono, kFrames, b == 3 ? 0.9f : 0.0f);
        buses.clear();
        chain.prepare(p, mix);
        chain.process(mono, kFrames, buses.out());
    }
    CHECK(peak_of(buses.l, kFrames) > 0.0f);
    chain.clear_state();
    fill_dc(mono, kFrames, 0.0f);
    buses.clear();
    chain.process(mono, kFrames, buses.out());
    CHECK_EQ(peak_of(buses.l, kFrames), 0.0f);
    CHECK_EQ(peak_of(buses.r, kFrames), 0.0f);

    // The lo-fi's hold: after clear_state() the first sample is taken afresh, not a held one.
    TrackParams lofi = neutral_params();
    lofi.set(kLof, 127);
    InsertChain crush;
    crush.init(kRate);
    crush.prepare(lofi, mix);
    fill_dc(mono, kFrames, 0.5f);
    buses.clear();
    crush.process(mono, kFrames, buses.out());
    crush.clear_state();
    fill_dc(mono, kFrames, -0.5f);
    buses.clear();
    crush.process(mono, kFrames, buses.out());
    CHECK(buses.l[kFrames - 1] < 0.0f);
    CHECK(buses.l[1] <= 0.0f);

    // reset() puts both gains back to zero, so the next block fades in on both sides.
    InsertChain fade;
    fade.init(kRate);
    fade.prepare(neutral_params(), mix);
    fill_dc(mono, kFrames, 0.5f);
    for (int b = 0; b < 2; b++) {
        buses.clear();
        fade.process(mono, kFrames, buses.out());
    }
    fade.reset();
    fade.prepare(neutral_params(), mix);
    fill_dc(mono, kFrames, 0.5f);
    buses.clear();
    fade.process(mono, kFrames, buses.out());
    CHECK(absolute(buses.l[0]) < 0.02f);
    CHECK(absolute(buses.r[0]) < 0.02f);
    CHECK(buses.r[kFrames - 1] > 0.3f);
}

TEST(chain_pan_between_centre_and_hard_right_is_both_channels) {
    // 96 is half way to the right: constant power at an eighth of a turn plus a sixteenth.
    TrackParams p = neutral_params();
    p.set(kPan, 96);
    ChainMix mix;
    Buses out;
    run_chain(p, mix, Signal::dc, 0.5f, 2, out);
    const float turns = 0.125f * (1.0f + 32.0f / 63.0f);
    CHECK_NEAR(out.l[kFrames - 1], 0.5f * ed::cos_turns(turns), 1e-4);
    CHECK_NEAR(out.r[kFrames - 1], 0.5f * ed::sin_turns(turns), 1e-4);
}

TEST(chain_a_track_faded_to_zero_still_ramps_down_on_one_side) {
    // A hard-panned track has one gain at zero already; turning its level to 0 must still run
    // the block that fades the other side out, rather than calling the track silent and leaving
    // that gain where it was.
    for (int side = 0; side < 2; side++) {
        TrackParams p = neutral_params();
        p.set(kPan, side == 0 ? kPanLeft : kPanRight);
        ChainMix mix;
        InsertChain chain;
        chain.init(kRate);
        Buses buses;
        float mono[kFrames];
        for (int b = 0; b < 2; b++) {
            fill_dc(mono, kFrames, 0.5f);
            buses.clear();
            chain.prepare(p, mix);
            chain.process(mono, kFrames, buses.out());
        }
        mix.level = 0.0f;
        fill_dc(mono, kFrames, 0.5f);
        buses.clear();
        chain.prepare(p, mix);
        chain.process(mono, kFrames, buses.out());
        const float *bus = side == 0 ? buses.l : buses.r;
        CHECK(bus[0] > 0.4f);
        CHECK(bus[kFrames - 1] < 0.02f);
        CHECK(chain.silent());
    }
}

TEST(chain_a_send_with_no_bus_attached_is_skipped) {
    // Both sends up, but the caller passes no send buses: the dry mix still arrives, and nothing
    // is written through a null pointer.
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    p.set(kSnb, 127);
    ChainMix mix;
    InsertChain chain;
    chain.init(kRate);
    Buses buses;
    ChainOut out = buses.out();
    out.send_a = nullptr;
    out.send_b = nullptr;
    float mono[kFrames];
    fill_dc(mono, kFrames, 0.5f);
    chain.prepare(p, mix);
    chain.process(mono, kFrames, out);
    CHECK(peak_of(buses.l, kFrames) > 0.1f);
}

TEST(chain_a_one_frame_block_is_processed) {
    TrackParams p = neutral_params();
    p.set(kSna, 127);
    ChainMix mix;
    InsertChain chain;
    chain.init(kRate);
    Buses buses;
    float mono[kFrames];
    fill_dc(mono, kFrames, 0.5f);
    chain.prepare(p, mix);
    chain.process(mono, kFrames, buses.out());
    buses.clear();
    mono[0] = 0.5f;
    chain.process(mono, 1, buses.out());
    CHECK_NEAR(buses.l[0], 0.5f * 0.7071f, 0.002);
    CHECK(buses.a[0] > 0.1f);
    CHECK_EQ(buses.l[1], 0.0f);
}
