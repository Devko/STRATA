// The per-track insert chain. dsp/chain.hpp has the signal order and the parameter map.
//
// --- what is expected to dominate, per stage -------------------------------------------------
//
// Eight of these run every block, so this file is written for cycles first. The figures below
// are what the central measurement (docs/BUDGET.md, PLAN.md M0.5) should be compared against;
// they are per track, per 64-frame block, counting multiplies and adds in the hot loops only.
//
//   stage    expected to dominate                                flops/sample  neutral cost
//   drive    enaga_soft_clip: a clamp and a cubic. One select      ~7           0 (DRV = 0)
//            picks the positive or negative gain, so there is
//            no branch. The cubic is the whole cost.
//   filter   Svf::process, a cl6x block function: 2 one-pole      ~9 (12 dB)    0 (MOD = OFF)
//            integrators and the mix. THE LARGEST STAGE, and      ~12 (24 dB)
//            the reason 24 dB uses Svf::cascade: two poles for
//            about the price of one, so LP24 is NOT twice LP12.
//   lo-fi    the sample-and-hold select plus a round-trip         ~6           0 (LOF = 0,
//            through floor_int for the bit crush. Off by                       the default)
//            default and skipped outright, not computed and
//            thrown away. Expect this to be the first stage
//            cut if the measured tax is too high.
//   duck     one multiply-add against the mixer's shared duck      2           0 (depth = 0)
//            curve. Nothing per track is rendered: the envelope
//            is the mixer's, once per block for all eight.
//   pan +    enaga_ramp_stereo_add, one cl6x block function:       4           0 (level = 0,
//   level    the ramp, both gains and the add into the bus in                  whole chain
//            one pass. Pan and level are one multiply at                       returns)
//            control rate, not two stages.
//   sends    one enaga_mix each, mono. Stereo sends would cost     2 each      0 (amount = 0)
//            three times the bus traffic of the dry path; the
//            reverb sums to mono internally anyway and the
//            delay's ping-pong makes its own stereo.
//
// So the expectation is: the filter first (half the chain when it is on), drive second, lo-fi
// third, and everything after the filter close to free. A track at its neutral settings (no
// drive, MOD = OFF, no lo-fi, no duck, sends at 0) costs one pass over the block; a muted track
// costs one compare. The buffer and filter work is all in Enaga's C block functions, which cl6x
// compiles and pipelines - CLAUDE.md's *.ti.c rule bit only on the three hand-written loops
// here (drive, lo-fi, duck), and all three now live in dsp/chainloops.ti.c, which build_app.py
// sends to cl6x while this file goes through GCC (docs/BUDGET.md, M2, has the figures). The
// arithmetic moved unchanged, so a bypassed stage is still bit-identical.
//
// Duck::render below is deliberately NOT in that file: it is a sequential state machine run
// once per block for all eight tracks, not per track, and its per-sample work is one add.
#include "chain.hpp"

#include "chainloops.h"
#include "filterloops.h"

namespace strata {
namespace ed = enaga::dsp;

namespace {

// Drive: the negative half is driven less than the positive one, which is what makes the clip
// asymmetric and puts even harmonics in. Both halves still go through enaga_soft_clip, so the
// output cannot leave +-1 whatever the input is.
constexpr float kDriveAsymmetry = 0.7f;
constexpr float kDriveOctaves = 4.0f;       // +24 dB at DRV = 127

// Cutoff: 20 Hz to 18 kHz. log2(18000 / 20) = 9.8138.
constexpr float kCutoffLowHz = 20.0f;
constexpr float kCutoffOctaves = 9.8138f;

constexpr float kResonanceMax12 = 0.95f;
constexpr float kResonanceMax24 = 0.85f;    // it compounds through the cascade

// Lo-fi: 2^15 quantisation steps (transparent) down to 2^2 (four), and the sample rate down to
// a thirty-second of itself.
constexpr float kLofiBitsTop = 15.0f;
constexpr float kLofiBitsSpan = 13.0f;
constexpr float kLofiRateOctaves = 5.0f;
// The bit crush rounds through an int, so the sample it holds is bounded first: a machine
// handing over something wild would otherwise make the conversion undefined. Two selects, no
// branch, and only on a stage that is off by default anyway.
constexpr float kLofiCeiling = 8.0f;

// Mapping of the MOD knob onto FilterMode: seven equal zones, OFF first.
FilterMode mode_from(uint8_t value) {
    const int zones = int(FilterMode::count);                 // 7
    int zone = (int(value) * zones) / 128;
    zone = zone < 0 ? 0 : (zone >= zones ? zones - 1 : zone);
    return FilterMode(zone);
}

ed::Svf::Output output_of(FilterMode mode) {
    switch (mode) {
        case FilterMode::bp12:
        case FilterMode::bp24: return ed::Svf::band_pass;
        case FilterMode::hp12:
        case FilterMode::hp24: return ed::Svf::high_pass;
        default: return ed::Svf::low_pass;
    }
}

bool is_24_db(FilterMode mode) {
    return mode == FilterMode::lp24 || mode == FilterMode::bp24 || mode == FilterMode::hp24;
}

}  // namespace

// --- the duck -------------------------------------------------------------------------------

void Duck::init(float sample_rate) {
    rate_ = sample_rate;
    reset();
    set(0.004f, 0.050f, 0.140f);
}

void Duck::set(float attack_s, float hold_s, float release_s) {
    // One reciprocal each, at control rate: the render loop only adds.
    attack_step_ = ed::reciprocal(ed::clamp(attack_s, 0.0005f, 1.0f) * rate_);
    release_step_ = ed::reciprocal(ed::clamp(release_s, 0.0005f, 4.0f) * rate_);
    hold_samples_ = int(ed::clamp(hold_s, 0.0f, 2.0f) * rate_);
}

void Duck::trigger(uint32_t frame) {
    if (pending_ == kNoTrigger) pending_ = frame;
}

void Duck::reset() {
    amount_ = 0.0f;
    held_ = 0;
    stage_ = Stage::idle;
    pending_ = kNoTrigger;
}

bool Duck::render(float *out, int frames) {
    if (stage_ == Stage::idle && pending_ == kNoTrigger) return false;   // idle costs nothing

    // A trigger aimed past the end of this block is CARRIED, not consumed.
    //
    // It used to be cleared unconditionally, and the `i == at` test below then never fired: the
    // trigger was lost for good rather than deferred. Reachable, and measured: the only caller
    // hands this `StepEvent::frame`, and a ratcheted step's later hits sit thousands of frames
    // into the step -- so a kick roll on the duck-source track pumped once and then not at all.
    //
    // app/engine.cpp now defers those events itself, so in this app the frame always lands
    // inside the block. This is the belt as well as the braces: dsp/chain.hpp's contract states
    // no precondition, so the code should not have one, and a silently dropped trigger is the
    // worst way for a caller to find out.
    uint32_t at = kNoTrigger;
    if (pending_ != kNoTrigger) {
        if (pending_ < uint32_t(frames)) {
            at = pending_;
            pending_ = kNoTrigger;
        } else {
            pending_ -= uint32_t(frames);        // next block, at the frame it asked for
        }
    }
    for (int i = 0; i < frames; i++) {
        if (uint32_t(i) == at) {                 // sample-accurate retrigger
            stage_ = Stage::attack;
            held_ = hold_samples_;
        }
        switch (stage_) {
            case Stage::attack:
                amount_ += attack_step_;
                if (amount_ >= 1.0f) {
                    amount_ = 1.0f;
                    stage_ = Stage::hold;
                }
                break;
            case Stage::hold:
                if (--held_ <= 0) stage_ = Stage::release;
                break;
            case Stage::release:
                amount_ -= release_step_;
                if (amount_ <= 0.0f) {
                    amount_ = 0.0f;
                    stage_ = Stage::idle;
                }
                break;
            case Stage::idle:
                break;
        }
        out[i] = amount_;
    }
    return true;
}

// --- the chain ------------------------------------------------------------------------------

void InsertChain::init(float sample_rate) {
    rate_ = sample_rate;
    reset();
}

void InsertChain::reset() {
    clear_state();
    gain_l_ = 0.0f;
    gain_r_ = 0.0f;
}

void InsertChain::clear_state() {
    svf_a_.reset();
    svf_b_.reset();
    lofi_phase_ = 0xFFFFFFFFu;     // so the first sample of a block is taken, not a stale hold
    lofi_hold_ = 0.0f;
}

const char *InsertChain::param_name(int index) {
    switch (index) {
        case 4:  return "DRV";
        case 5:  return "CUT";
        case 6:  return "RES";
        case 7:  return "MOD";
        case 12: return "LOF";
        case 13: return "PAN";
        case 14: return "SNA";
        case 15: return "SNB";
        default: return "---";
    }
}

bool InsertChain::silent() const {
    return target_l_ == 0.0f && target_r_ == 0.0f && gain_l_ == 0.0f && gain_r_ == 0.0f &&
           send_a_gain_ == 0.0f && send_b_gain_ == 0.0f;
}

void InsertChain::prepare(const TrackParams &params, const ChainMix &mix) {
    const int fltr = page_first(Page::fltr);
    const int fx = page_first(Page::fx);

    // --- drive: 0 is a true bypass, not unity gain through the clip -------------------------
    const uint8_t drive = params.get(fltr + 0);
    drive_on_ = drive != 0;
    if (drive_on_) {
        drive_pos_ = ed::exp2(float(drive) * (1.0f / 127.0f) * kDriveOctaves);
        drive_neg_ = drive_pos_ * kDriveAsymmetry;
    }

    // --- filter ------------------------------------------------------------------------------
    mode_ = mode_from(params.get(fltr + 3));
    if (mode_ != FilterMode::off) {
        filter_24_ = is_24_db(mode_);
        svf_output_ = output_of(mode_);
        cutoff_hz_ = kCutoffLowHz * ed::exp2(params.unit(fltr + 1) * kCutoffOctaves);
        const float resonance =
            params.unit(fltr + 2) * (filter_24_ ? kResonanceMax24 : kResonanceMax12);
        coefficients_ = ed::SvfCoefficients::make(cutoff_hz_, resonance, rate_);
    }

    // --- lo-fi: off by default, and off means skipped ---------------------------------------
    const uint8_t lofi = params.get(fx + 0);
    lofi_on_ = lofi != 0;
    if (lofi_on_) {
        const float amount = float(lofi) * (1.0f / 127.0f);
        lofi_levels_ = ed::exp2(kLofiBitsTop - kLofiBitsSpan * amount);
        lofi_step_ = ed::reciprocal(lofi_levels_);
        // A 32-bit phase whose wrap says "take a new sample": no division, no compare against a
        // float period, and the ratio can sit anywhere between 1 and 1/32 of the sample rate.
        const float ratio = ed::clamp(ed::exp2(-kLofiRateOctaves * amount), 0.0f, 0.999f);
        lofi_increment_ = uint32_t(ratio * 4294967296.0f);
    }

    // --- duck --------------------------------------------------------------------------------
    duck_depth_ = ed::clamp(mix.duck, 0.0f, 1.0f);

    // --- pan, level and the sends ------------------------------------------------------------
    // PAN is bipolar around 64, so that 64 is exactly centre and 0 and 127 are exactly hard
    // left and hard right; the law is constant power, snapped at the ends so that a hard pan
    // puts a bit-exact signal in one channel and a true zero in the other.
    const int pan_value = int(params.get(fx + 1));
    const float pan = pan_value <= 64 ? float(pan_value - 64) * (1.0f / 64.0f)
                                      : float(pan_value - 64) * (1.0f / 63.0f);
    float pan_l, pan_r;
    if (pan <= -1.0f) {
        pan_l = 1.0f;
        pan_r = 0.0f;
    } else if (pan >= 1.0f) {
        pan_l = 0.0f;
        pan_r = 1.0f;
    } else {
        const float turns = 0.125f * (pan + 1.0f);     // 0 to a quarter turn
        pan_l = ed::cos_turns(turns);
        pan_r = ed::sin_turns(turns);
    }

    const float level = ed::clamp(mix.level, 0.0f, 4.0f);
    target_l_ = pan_l * level;                 // pan and level are one multiply, at control rate
    target_r_ = pan_r * level;
    send_a_gain_ = level * params.unit(fx + 2);   // post-fader: the fader is in the send gain
    send_b_gain_ = level * params.unit(fx + 3);
}

void InsertChain::process(float *mono, int frames, const ChainOut &out) {
    if (frames <= 0) return;
    if (silent()) return;      // a muted track costs one compare, not a block

    // --- drive: an asymmetric soft clip ------------------------------------------------------
    if (drive_on_) strata_chain_drive(mono, frames, drive_pos_, drive_neg_);

    // --- filter: 24 dB is a cascade, two poles for about the price of one --------------------
    if (mode_ != FilterMode::off) {
        if (filter_24_) {
            strata_svf_cascade_inplace(&svf_a_.state_, &coefficients_, &svf_b_.state_,
                                       &coefficients_, svf_output_, mono, frames);
        } else {
            strata_svf_inplace(&svf_a_.state_, &coefficients_, svf_output_, mono, frames);
        }
    }

    // --- lo-fi: sample-and-hold, then the bit crush, in one pass -----------------------------
    if (lofi_on_) {
        // The phase wrap is the sample-and-hold's clock edge; lofi_step_ is 1 / lofi_levels_,
        // taken at control rate so the loop does not divide.
        strata_chain_lofi(mono, frames, lofi_increment_, lofi_levels_, lofi_step_, kLofiCeiling,
                          &lofi_phase_, &lofi_hold_);
    }

    // --- a bounce's tap: one copy, on one track, only while a take records it ------------------
    if (out.tap != nullptr) {
        for (int i = 0; i < frames; i++) out.tap[i] = mono[i];
    }

    // --- duck: the mixer's one curve, this track's depth -------------------------------------
    if (duck_depth_ > 0.0f && out.duck != nullptr) {
        strata_chain_duck(mono, out.duck, frames, duck_depth_);
    }

    // --- pan, level and the add into the bus, in one block function --------------------------
    // The gains ramp across the block so that an edit or a parameter lock never clicks
    // (docs/SPEC.md, "Parameters"). One reciprocal per block, cached: nothing divides here.
    if (frames != ramp_frames_) {
        ramp_frames_ = frames;
        inv_frames_ = ed::reciprocal(float(frames));
    }
    const float step_l = (target_l_ - gain_l_) * inv_frames_;
    const float step_r = (target_r_ - gain_r_) * inv_frames_;
    enaga_ramp_stereo_add(mono, out.dry_l, out.dry_r, frames, &gain_l_, &gain_r_, step_l, step_r);
    gain_l_ = target_l_;   // snap, so a block with unchanged parameters ramps by exactly nothing
    gain_r_ = target_r_;

    // --- sends: mono, post-fader -------------------------------------------------------------
    // The send gain steps once per block rather than ramping: a step inside a delay or reverb
    // tail is inaudible, and this way a send costs one pass.
    if (send_a_gain_ > 0.0f && out.send_a != nullptr) {
        enaga_mix(out.send_a, mono, frames, send_a_gain_);
    }
    if (send_b_gain_ > 0.0f && out.send_b != nullptr) {
        enaga_mix(out.send_b, mono, frames, send_b_gain_);
    }
}

}  // namespace strata
