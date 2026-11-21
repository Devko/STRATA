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
