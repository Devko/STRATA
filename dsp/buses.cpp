// The mixer, the two sends and the master chain. dsp/buses.hpp has the block protocol.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Delay and reverb measured 16% of the block together (docs/SPEC.md, "CPU and memory budget"),
// more than all 18 voices put together, and they are a FIXED tax: they cost the same whether
// one track sounds or eight. Two things here are aimed squarely at that:
//
//   * Both sends stop running once nothing has fed them for a whole tail. Feeding is tracked by
//     the send gains the chains report, which the app only sets on tracks that are sounding, so
//     a kit with the sends at zero pays nothing for either effect instead of 16%.
//   * The sends are mono, which is where a track's send cost goes from three bus passes to one.
//
// Expected order, per block: reverb (eight damped lines plus four all-passes, all per sample)
// first by a wide margin, then the delay (two interpolated taps and the tone filter), then the
// master's limiter (a cubic per sample per channel) and DJ filter (two cascades), then
// everything else. The master's glue compressor is deliberately NOT per sample: its detector is
// the block's peak and its gain is one value a block, ramped across the block with the output
// level in a single pass, which is 1.33 ms of attack quantisation and perhaps a twentieth of
// the cost of a per-sample detector.
//
// The master's four hand-written loops -- the peak detector, the gain ramp, the limiter and the
// interleave -- are the fixed tax that runs on every block however few tracks sound, so they
// live in dsp/masterloops.ti.c, which build_app.py sends to cl6x while this file goes through
// GCC (CLAUDE.md's *.ti.c rule; docs/BUDGET.md, M2, has the figures). The arithmetic moved
// unchanged.
#include "buses.hpp"

#include "masterloops.h"

namespace strata {
namespace ed = enaga::dsp;

namespace {

// The reverb's right input. Its input is mono, the C block functions take restrict pointers, so
// the two inputs cannot be the same buffer; the network is linear, so a silent right channel
// with the mix doubled is exactly a mono feed into both.
const float kSilence[Mixer::kMaxFrames] = {};

// DJ filter: flat (and bypassed) inside the dead zone around the centre.
constexpr float kDjDeadZone = 0.03f;
constexpr float kDjSpan = 1.0f / (1.0f - kDjDeadZone);
constexpr float kDjLowTopHz = 20000.0f;     // the low-pass sweeps down from here...
constexpr float kDjLowOctaves = 8.0f;       // ...to 78 Hz
constexpr float kDjHighLowHz = 20.0f;       // the high-pass sweeps up from here...
constexpr float kDjHighOctaves = 8.6f;      // ...to 7.7 kHz
constexpr float kDjResonanceBase = 0.25f;
constexpr float kDjResonanceSpan = 0.35f;

// Glue compressor: a 4:1 knee above -6 dB at full amount, 10 ms attack and 150 ms release,
// smoothed once a block.
constexpr float kCompressorThreshold = 0.5f;
constexpr float kCompressorSlope = 0.75f;   // 1 - 1/4
constexpr float kCompressorAttackRate = 100.0f;      // 1 / 10 ms
constexpr float kCompressorReleaseRate = 6.6667f;    // 1 / 150 ms

float largest(float a, float b) { return a > b ? a : b; }

}  // namespace

void Mixer::init(float sample_rate) {
    rate_ = sample_rate;

    // The only allocation in this file, and the reason init() belongs in App::init().
    delay_.init(kMaxDelaySeconds, sample_rate);
    reverb_.init(sample_rate);

    duck_.init(sample_rate);
    for (int t = 0; t < kTracks; t++) chains_[t].init(sample_rate);

    reverb_high_pass_.reset();
    dj_l_a_.reset();
    dj_l_b_.reset();
    dj_r_a_.reset();
    dj_r_b_.reset();
    compressor_gain_ = 1.0f;
    master_gain_ = 1.0f;
    delay_tail_ = 0;
    reverb_tail_ = 0;

    prepare(SendSettings{}, MasterSettings{});
}

void Mixer::prepare(const SendSettings &sends, const MasterSettings &master_settings) {
    // Everything either call below resolves is derived purely from its struct, so a knob that
    // has not moved costs one comparison instead of a biquad design, the eight exp2 calls
    // inside Reverb::set and the rest of it.
    if (!resolved_ || !(sends == last_sends_)) {
        last_sends_ = sends;
        resolve_sends(sends);
    }
    if (!resolved_ || !(master_settings == last_master_)) {
        last_master_ = master_settings;
        resolve_master(master_settings);
    }
    resolved_ = true;
}

void Mixer::resolve_sends(const SendSettings &sends) {
    // --- send A: the delay. Seconds in, because the app owns the tempo (CLAUDE.md). ----------
    const float seconds = ed::clamp(sends.delay_seconds, 0.001f, kMaxDelaySeconds);
    const float feedback = ed::clamp(sends.delay_feedback, 0.0f, 0.95f);
    delay_.set(seconds, feedback, ed::clamp(sends.delay_tone, 0.0f, 1.0f), sends.delay_ping_pong);
    delay_level_ = ed::clamp(sends.delay_level, 0.0f, 4.0f);
    delay_to_reverb_ = ed::clamp(sends.delay_to_reverb, 0.0f, 1.0f);

    // --- send B: the reverb and the high-pass on its input -----------------------------------
    reverb_.set(sends.reverb_decay_s, sends.reverb_damping);
    reverb_level_ = ed::clamp(sends.reverb_level, 0.0f, 4.0f);
    reverb_high_pass_coefficients_ = ed::BiquadCoefficients::make(
        ed::BiquadCoefficients::high_pass, ed::clamp(sends.reverb_high_pass_hz, 20.0f, 2000.0f),
        0.7071f, rate_);

    // --- how long each send keeps running after it stops being fed ---------------------------
    const float blocks_per_second = rate_ * ed::reciprocal(float(kMaxFrames));
    const float delay_tail_s = ed::clamp(seconds * (2.0f + 24.0f * feedback), 0.05f, 8.0f);
    delay_tail_blocks_ = int(delay_tail_s * blocks_per_second) + 1;
    const float reverb_tail_s = ed::clamp(sends.reverb_decay_s * 1.5f, 0.3f, 12.0f);
    reverb_tail_blocks_ = int(reverb_tail_s * blocks_per_second) + 1;
}

void Mixer::resolve_master(const MasterSettings &master_settings) {
    // --- master: the one-knob DJ filter ------------------------------------------------------
    const float dj = ed::clamp(master_settings.dj, -1.0f, 1.0f);
    const float swing = dj < 0.0f ? -dj : dj;
    dj_on_ = swing > kDjDeadZone;
    if (dj_on_) {
        const float amount = (swing - kDjDeadZone) * kDjSpan;
        float cutoff;
        if (dj < 0.0f) {
            dj_output_ = ed::Svf::low_pass;
            cutoff = kDjLowTopHz * ed::exp2(-kDjLowOctaves * amount);
        } else {
            dj_output_ = ed::Svf::high_pass;
            cutoff = kDjHighLowHz * ed::exp2(kDjHighOctaves * amount);
        }
        dj_coefficients_ = ed::SvfCoefficients::make(
            cutoff, kDjResonanceBase + kDjResonanceSpan * amount, rate_);
    }

    // --- master: the glue compressor ---------------------------------------------------------
    const float compressor = ed::clamp(master_settings.compressor, 0.0f, 1.0f);
    compressor_on_ = compressor > 0.0f;
    compressor_slope_ = compressor * kCompressorSlope;
    compressor_inv_threshold_ = ed::reciprocal(kCompressorThreshold);
    const float block_s = float(kMaxFrames) * ed::reciprocal(rate_);
    compressor_attack_ = 1.0f - ed::exp(-block_s * kCompressorAttackRate);
    compressor_release_ = 1.0f - ed::exp(-block_s * kCompressorReleaseRate);
    if (!compressor_on_) compressor_gain_ = 1.0f;

    // --- master: the soft limiter and the output level ---------------------------------------
    // The threshold is what moves; the ceiling is always +-1, because the excess above the
    // threshold goes through enaga_soft_clip, which saturates at exactly 1.
    limit_threshold_ = ed::clamp(master_settings.limit, 0.1f, 0.98f);
    limit_headroom_ = 1.0f - limit_threshold_;
    limit_inv_headroom_ = ed::reciprocal(limit_headroom_);
    out_level_ = ed::clamp(master_settings.level, 0.0f, 1.0f);
}

void Mixer::begin_block(int frames) {
    frames_ = frames < 0 ? 0 : (frames > kMaxFrames ? kMaxFrames : frames);
    if (frames_ == 0) return;
    enaga_clear(mix_l_, frames_);
    enaga_clear(mix_r_, frames_);
    enaga_clear(send_a_, frames_);
    enaga_clear(send_b_, frames_);
    // One duck envelope for the whole mixer; false means idle, and then no chain touches it.
    duck_active_ = duck_.render(duck_curve_, frames_);
}

void Mixer::add_track(int track, float *mono) {
    if (track < 0 || track >= kTracks || frames_ <= 0) return;
    InsertChain &insert = chains_[track];

    // A non-zero send gain on a track the app is bothering to render is what "the send is being
    // fed" means here: it costs nothing to test, and the alternative (scanning the bus for a
    // non-zero sample) costs a pass over every block.
    if (insert.send_a_gain() > 0.0f) delay_tail_ = delay_tail_blocks_;
    if (insert.send_b_gain() > 0.0f) reverb_tail_ = reverb_tail_blocks_;

    ChainOut out;
    out.dry_l = mix_l_;
    out.dry_r = mix_r_;
    out.send_a = send_a_;
    out.send_b = send_b_;
    out.duck = duck_active_ ? duck_curve_ : nullptr;
    insert.process(mono, frames_, out);
}

void Mixer::finish_block(float *out, int frames) {
    const int n = frames_;

    // --- send A: the delay -------------------------------------------------------------------
    if (n > 0 && delay_tail_ > 0) {
        delay_tail_--;
        // The send bus is mono and the delay's two inputs must not alias, so the feed is copied
        // once per block: 64 floats against the delay's own cost is nothing.
        for (int i = 0; i < n; i++) send_a_right_[i] = send_a_[i];
        if (delay_to_reverb_ > 0.0f) {
            enaga_clear(wet_l_, n);
            enaga_clear(wet_r_, n);
            delay_.process(send_a_, send_a_right_, wet_l_, wet_r_, n, 1.0f);
            enaga_mix(mix_l_, wet_l_, n, delay_level_);
            enaga_mix(mix_r_, wet_r_, n, delay_level_);
            const float into_reverb = delay_to_reverb_ * 0.5f;
            enaga_mix(send_b_, wet_l_, n, into_reverb);
            enaga_mix(send_b_, wet_r_, n, into_reverb);
            reverb_tail_ = reverb_tail_blocks_;   // the delay is feeding it, so keep it running
        } else {
            // Nothing needs the wet signal on its own, so the delay writes straight into the mix.
            delay_.process(send_a_, send_a_right_, mix_l_, mix_r_, n, delay_level_);
        }
    }

    // --- send B: the reverb, high-passed first ------------------------------------------------
    if (n > 0 && reverb_tail_ > 0) {
        reverb_tail_--;
        reverb_high_pass_.process(send_b_, send_b_, n, reverb_high_pass_coefficients_);
        reverb_.process(send_b_, kSilence, mix_l_, mix_r_, n, reverb_level_ * 2.0f);
    }

    // --- master ------------------------------------------------------------------------------
    if (n > 0) master(n);

    // --- interleave, once, at the end --------------------------------------------------------
    // audio()'s buffer is L R L R (CLAUDE.md); everything above worked in planes because that
    // is the layout every Enaga block function wants.
    strata_interleave(out, mix_l_, mix_r_, n, frames);
}

void Mixer::master(int n) {
    // --- the one-knob DJ filter: 24 dB per channel, and bypassed in the dead zone -------------
    if (dj_on_) {
        ed::Svf::cascade(dj_l_a_, dj_coefficients_, dj_l_b_, dj_coefficients_, mix_l_, mix_l_, n,
                         dj_output_);
        ed::Svf::cascade(dj_r_a_, dj_coefficients_, dj_r_b_, dj_coefficients_, mix_r_, mix_r_, n,
                         dj_output_);
    }

    // --- the block's peak: the compressor's detector and the limiter's skip test --------------
    const float peak = strata_master_peak(mix_l_, mix_r_, n);
    peak_ = peak;

    // --- the glue compressor: one gain a block, smoothed ---------------------------------------
    if (compressor_on_) {
        const float over = peak * compressor_inv_threshold_;
        const float target = over > 1.0f ? ed::exp2(-compressor_slope_ * ed::log2(over)) : 1.0f;
        const float coefficient = target < compressor_gain_ ? compressor_attack_ : compressor_release_;
        compressor_gain_ += coefficient * (target - compressor_gain_);
    } else {
        compressor_gain_ = 1.0f;
    }

    // --- that gain and the output level, as one ramped pass ------------------------------------
    const float from = master_gain_;
    const float to = compressor_gain_ * out_level_;
    if (from != 1.0f || to != 1.0f) {
        if (n != ramp_frames_) {
            ramp_frames_ = n;
            inv_frames_ = ed::reciprocal(float(n));
        }
        const float step = (to - from) * inv_frames_;
        strata_master_ramp(mix_l_, mix_r_, n, from, step);
    }
    master_gain_ = to;

    // --- the soft limiter --------------------------------------------------------------------
    // Unity below the threshold, so a quiet master is bit-exact and the stage is skipped
    // outright when the block cannot reach the threshold even at the louder end of the ramp.
    if (peak * largest(from, to) > limit_threshold_) {
        strata_master_limit(mix_l_, mix_r_, n, limit_threshold_, limit_headroom_,
                            limit_inv_headroom_);
    }
}

}  // namespace strata
