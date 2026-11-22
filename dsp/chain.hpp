// The per-track insert chain: one machine's MONO block in, a stereo contribution to the mix bus
// and two mono send amounts out.
//
// Fixed order (docs/SPEC.md, "Sound architecture"):
//
//     drive -> multimode filter -> lo-fi -> sidechain duck -> pan -> level
//
// SPEC also lists an amplitude envelope between lo-fi and the duck. The machines own their
// amplitude envelopes (dsp/track.hpp: the AMP page is theirs), and an envelope, the duck, pan
// and level are all gains, which commute, so nothing audible moves by leaving it out here.
// There is deliberately no second envelope in this file.
//
// Parameters. dsp/track.hpp is the contract: the chain reads FLTR and FX, the machine reads SRC
// and AMP, and neither needs the other's.
//
//     FLTR  4 DRV  drive       0 bypasses; else +0 to +24 dB into an asymmetric soft clip
//           5 CUT  cutoff      20 Hz to 18 kHz, exponential
//           6 RES  resonance   0 to 0.95 (0.85 in the 24 dB modes, where it compounds)
//           7 MOD  mode        OFF | LP12 | LP24 | BP12 | BP24 | HP12 | HP24
//     FX   12 LOF  lo-fi       0 is OFF and genuinely skipped; else bits 16->3, rate 1 -> 1/32
//          13 PAN  pan         0 hard left, 64 centre, 127 hard right
//          14 SNA  send A      delay, post-fader
//          15 SNB  send B      reverb, post-fader
//
// Drive sits on the FLTR page rather than the FX page track.hpp's comment suggests, so that the
// four knobs of one page are the whole filter section in signal order (drive, cutoff, resonance,
// mode) and both sends get a lockable parameter of their own. Nothing else reads these slots.
//
// Level and sidechain depth are NOT kit parameters. The machine's AMP page owns its own level
// (track.hpp), and docs/SPEC.md puts the sidechain duck in the mixer, not the track. Both reach
// the chain in ChainMix, which the mixer fills from the performance mixer each block.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

// --- the shared sidechain duck -------------------------------------------------------------
//
// Trigger-based, not an envelope follower: an attack / hold / release envelope fired by an
// explicit call, which is both tighter and far cheaper than detecting a kick in the audio.
// There is ONE of these for the whole mixer, rendered once per block, and each chain applies it
// with its own depth; eight tracks therefore cost eight multiply-adds a sample, not eight
// envelopes. Which track fires it is the app's business, not this file's.
class Duck {
public:
    // Control rate. attack/hold/release in seconds; the segments are linear.
    void init(float sample_rate);
    void set(float attack_s, float hold_s, float release_s);

    // Fire the duck, at a frame of the block for sample accuracy. At most one trigger per block
    // is remembered: at 1.33 ms a second one inside the same block is not a musical event.
    void trigger(uint32_t frame = 0);

    // This block's duck amount: 0 open, 1 fully ducked. Returns false and leaves `out` alone
    // when the duck is idle and nothing is pending, so an idle duck costs nothing at all.
    bool render(float *out, int frames);

    bool active() const { return stage_ != Stage::idle || pending_ != kNoTrigger; }
    float amount() const { return amount_; }
    void reset();

private:
    enum class Stage : uint8_t { idle, attack, hold, release };
    static constexpr uint32_t kNoTrigger = 0xFFFFFFFFu;

    float rate_ = 48000.0f;
    float attack_step_ = 1.0f, release_step_ = 1.0f;
    int hold_samples_ = 0;
    float amount_ = 0.0f;
    int held_ = 0;
    Stage stage_ = Stage::idle;
    uint32_t pending_ = kNoTrigger;
};

// --- what the mixer owns, per track ---------------------------------------------------------

// Gains the mixer sets, not the kit: the track fader and the sidechain depth.
struct ChainMix {
    float level = 1.0f;   // 0 silences the track (and its sends: the sends are post-fader)
    float duck = 0.0f;    // 0 bypasses the duck for this track, 1 ducks it fully
};

// Where one chain adds its block. The mixer clears these, hands the same set to all eight
// chains, and processes them afterwards.
struct ChainOut {
    float *dry_l = nullptr;      // the stereo mix bus
    float *dry_r = nullptr;
    float *send_a = nullptr;     // send A, mono (delay)
    float *send_b = nullptr;     // send B, mono (reverb)
    const float *duck = nullptr; // this block's duck amount, or nullptr while the duck is idle
};

enum class FilterMode : uint8_t { off = 0, lp12, lp24, bp12, bp24, hp12, hp24, count };

class InsertChain {
public:
    // No allocation: a chain has no delay lines. init() only latches the sample rate.
    void init(float sample_rate);

    // Control rate, once per block, before process(). Every coefficient, gain and reciprocal
    // this chain needs is computed here; the hot loops below only multiply and add.
    void prepare(const TrackParams &params, const ChainMix &mix);

    // Runs the chain over the machine's mono block IN PLACE (the caller's buffer is scratch) and
    // adds the result into `out`. Call prepare() first.
    void process(float *mono, int frames, const ChainOut &out);

    // Filter state, lo-fi hold and gain ramps back to zero. init()-time and kit changes.
    void reset();
    // Filter state and lo-fi hold only, leaving the gain ramps where they are: for a track whose
    // voices were just killed, so the next note neither inherits stored energy nor fades in.
    void clear_state();

    // The three-letter encoder label for one of the 16 slots; "---" for the ones a machine owns.
    static const char *param_name(int index);

    // Introspection for the mixer, the UI and the tests.
    FilterMode mode() const { return mode_; }
    float cutoff_hz() const { return cutoff_hz_; }
    bool lofi_on() const { return lofi_on_; }
    float send_a_gain() const { return send_a_gain_; }
    float send_b_gain() const { return send_b_gain_; }
    // True when the chain would add nothing anywhere, so process() returns immediately.
    bool silent() const;

private:
    float rate_ = 48000.0f;

    // drive
    bool drive_on_ = false;
    float drive_pos_ = 1.0f, drive_neg_ = 1.0f;

    // filter
    FilterMode mode_ = FilterMode::off;
    bool filter_24_ = false;
    float cutoff_hz_ = 18000.0f;
    enaga::dsp::Svf::Output svf_output_ = enaga::dsp::Svf::low_pass;
    enaga::dsp::SvfCoefficients coefficients_{};
    enaga::dsp::Svf svf_a_{}, svf_b_{};

    // lo-fi
    bool lofi_on_ = false;
    float lofi_step_ = 1.0f, lofi_levels_ = 1.0f;
    uint32_t lofi_increment_ = 0xFFFFFFFFu, lofi_phase_ = 0xFFFFFFFFu;
    float lofi_hold_ = 0.0f;

    // duck
    float duck_depth_ = 0.0f;

    // pan, level and the sends
    float target_l_ = 0.0f, target_r_ = 0.0f;   // pan x level, this block's destination
    float gain_l_ = 0.0f, gain_r_ = 0.0f;       // where the ramp is now
    float send_a_gain_ = 0.0f, send_b_gain_ = 0.0f;

    // 1 / frames, cached: a block's gain ramp needs it and nothing may divide in a loop.
    int ramp_frames_ = 0;
    float inv_frames_ = 1.0f;
};

}  // namespace strata
