// KICK, after Mutable Instruments Plaits `synthetic_bass_drum.h` (MIT licensed,
// Copyright 2016 Emilie Gillet). See kick.hpp and docs/SOUND.md.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Two voices, so this is per voice per 64-frame block, counting multiplies and adds in the hot
// loops only. These are the figures the central measurement (docs/archive/BUDGET_MILESTONES.md, PLAN.md M0.5)
// should be compared against.
//
//   stage              expected to dominate                            flops/sample
//   body shaper        reciprocal for the fold (~8) and sin_turns      ~26   THE LARGEST STAGE
//                      for the clean end (~12), both per sample
//                      because dirtiness blends between them.
//   phase + FM         one multiply-add and a cap. The FM envelope      3
//                      is one multiply.
//   amp env, VCA,      Envelope::render, a scale, a multiply and        7
//   output low-pass    the OnePole: all cl6x block functions.
//   click              the gate select, its slew, a one-pole           ~15   0 when CLK = 0
//                      high-pass, then Svf::process at 5 kHz Q 2.            (skipped, not
//   attack noise       one xorshift to float, two OnePole passes,      ~18    computed and
//                      the subtraction and the mix_product.                   thrown away)
//
// So about 71 flops/sample with the transient and 38 without. Against Enaga's measured VA voice
// (1,868 cycles per block = 29 cycles/sample for roughly 70-80 flops/sample, so ~2.5 flops per
// cycle on this core) that is **about 1,800 cycles per voice per block, ~1.0x a full VA voice**.
//
// Worth saying plainly, because it corrects the brief these were written to: a drum machine
// following a reference topology is NOT far simpler than a VA voice. The cost is in the shaper,
// which needs a reciprocal and a sine every sample. If the measurement disappoints, the cheap
// win is a polynomial fit for 1/(1+|t|) over [1, 2] in place of enaga_reciprocal.
//
// The two loops that dominate -- the body shaper and the click's gate and slew -- now live in
// dsp/drumloops.ti.c, which build_app.py sends to cl6x while this file goes through GCC
// (CLAUDE.md's *.ti.c rule; docs/archive/BUDGET_MILESTONES.md, M2, has what that bought). The arithmetic moved
// unchanged; what is left here is control rate, the Enaga block functions and the voice logic.
#include "kick.hpp"

#include "drumloops.h"
#include "filterloops.h"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::Svf;
using enaga::dsp::SvfCoefficients;
using enaga::dsp::clamp;
using enaga::dsp::exp;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;

// The reference's transient filter: 5 kHz at Q 2, so k = 1/Q = 0.5.
constexpr float kClickHz = 5000.0f;
constexpr float kClickK = 0.5f;
// The reference's transient envelope time constant.
constexpr float kTransientTau = 0.005f;
// ln(100): EnvelopeShape::make wants the time to reach 1% of the target, where the reference
// gives time constants. One is ln(100) of the other, and the per-sample coefficient comes out
// the same as Plaits' 1 / (tau * rate).
constexpr float kToOnePercent = 4.60517019f;

// Plaits' distorted sine, which is lerp(clean sine, rational soft sine, dirtiness):
//
//     triangle = (phase < 0.5 ? phase : 1 - phase) * 4 - 1
//     fold     = 2 * triangle / (1 + |triangle|)      the dirty end
//     clean    = sin(phase + 0.75)                    the clean end
//     out      = clean + (fold - clean) * dirtiness
//
// So dirtiness 0 is a pure sine and dirtiness 1 is the fold. The bare triangle is only the
// intermediate the fold is computed from and is never the output. The 0.75 turn puts the clean
// sine in phase with the fold: both are zero and rising at phase 0.25, which is where a hit
// starts. The fold costs a reciprocal, which on the C674x is a short run of multiplies and not
// a division.
//
// It is the hot loop, so it lives in drumloops.ti.c as strata_kick_body, where cl6x compiles it;
// the declick ramp a stolen voice fades out with is strata_mix_ramp in the same file.

}  // namespace

// --- parameters -----------------------------------------------------------
//
// SRC
//   0  TUN  f0, 25-120 Hz exponential, exactly 55 Hz at knob 64 (STRATA's range; the reference
//           takes f0 from pitch CV). The trig note transposes it from note 60, so note 60 sounds
//           exactly at TUN and a kick written into a pattern follows the key.
//   1  FMA  fm_envelope_amount, 0-1 linear. The increment is f0 * (1 + 3.5 * FMA * fm_envelope),
//           so full FMA starts the hit 4.5x above f0: about +26 semitones of sweep.
//   2  FMD  fm_envelope_decay: time constant 8-128 ms exponential, from the reference's 8 ms
//           base. The production sweet spot for the trademark click is 20-70 ms, which is the
//           middle of the knob.
//   3  CLK  transient level, 0-1 linear: the 1 ms click plus the attack noise. The reference
//           ties this to `tone`; docs/SPEC.md wants a click control, so it is its own knob.
// AMP
//   8  DEC  decay, squared and then used as 20 ms * 2^(5 * decay^2): a time constant of 20 ms to
//           640 ms, which reaches 808 territory at the top.
//   9  TON  output low-pass, coefficient min(4 * f0 * 2^(TON * 108/12), 1) on the body. Dull
//           thud at the bottom, wide open over about 110.
//  10  DRT  dirtiness, 0-1 linear: the body travels from a clean sine at 0 to the reference's
//           rational soft sine at 127, so it brightens as the knob opens. Scaled by
//           max(1 - 8 * f0, 0) first, which forces it towards clean at high pitches where the
//           distortion would alias. The insert chain's drive sits after this, which is the
//           pairing the 909 is known for.
//  11  LVL  output level, square law, 0-1. Square gives usable control over the quiet end.

void Kick::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    hold_frames_ = int(0.0013f * rate_);
    gate_frames_ = int(0.0010f * rate_);
    transient_shape_ =
        EnvelopeShape::make(0.0002f, kTransientTau * kToOnePercent, 0.0f, 0.001f, rate_);
    click_coefficients_ =
        SvfCoefficients::from_g(enaga::dsp::tan_pi(kClickHz * inv_rate_), kClickK);
    for (Voice &v : voices_) {
        v = Voice{};
        v.click_band.reset();
        v.noise_lp.reset();
        v.noise_hp.reset();
        v.tone_lp.reset();
    }
    active_ = false;
    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Kick::prepare(const TrackParams &params) {
    // 25-120 Hz, with the exponent chosen so that knob 64 is exactly 55 Hz.
    base_hz_ = 25.0f * exp2(2.2572390f * params.unit(0));
    fm_amount_ = params.unit(1);
    const float fm_tau = 0.008f * exp2(4.0f * params.unit(2));       // 8-128 ms
    fm_decay_ = exp(-inv_rate_ * reciprocal(fm_tau));
    click_level_ = params.unit(3);

    const float decay = params.unit(8);
    const float amp_tau = 0.020f * exp2(5.0f * decay * decay);       // 20 ms to 640 ms
    // The reference holds the amplitude envelope for 1 ms before it decays; a 1 ms attack is the
    // same length and also keeps the start of a hit from being a step.
    amp_shape_ = EnvelopeShape::make(0.001f, amp_tau * kToOnePercent, 0.0f, 0.001f, rate_);
    tone_ratio_ = semitones(params.unit(9) * 108.0f);
    dirtiness_ = params.unit(10);
    const float level = params.unit(11);
    out_gain_ = level * level;
}

void Kick::trigger(const Trig &trig) {
    // Prefer a free voice; otherwise steal the quieter one, which is the older hit.
    int pick = -1;
    for (int i = 0; i < kVoices; i++) {
        if (!voices_[i].busy()) {
            pick = i;
            break;
        }
    }
    if (pick < 0) pick = voices_[0].amp.level() <= voices_[1].amp.level() ? 0 : 1;

    Voice &v = voices_[pick];
    // A voice that is still loud is faded out first and the new hit starts at the end of the
    // fade. Below kFadeFloor the splice is inaudible, so it starts straight away instead.
    if (v.fade == 0 && v.sounding() && v.amp.level() > kFadeFloor) {
        v.fade = kFadeFrames;
        v.gain = 1.0f;
    }
    v.next = trig;
    v.wait = int(trig.frame);
    if (v.wait < v.fade) v.wait = v.fade;
    v.pending = true;
    active_ = true;
}

void Kick::release_all() {
    // A one-shot percussion voice has no sustain: its decay IS its release, so STOP once has
    // nothing to let go of and the tail finishes by itself. kill_all() is the hard cut.
}

void Kick::kill_all() {
    for (Voice &v : voices_) {
        v.pending = false;
        v.wait = 0;
        silence(v);
    }
    active_ = false;
}

void Kick::render_add(float *out, int frames) {
    if (!active_) return;   // nothing sounding and nothing pending: an idle machine costs a test
    int done = 0;
    while (done < frames) {
        const int n = frames - done < kBlock ? frames - done : kBlock;
        for (Voice &v : voices_) render_voice(v, out + done, n);
        done += n;
    }
    bool busy = false;
    for (const Voice &v : voices_) busy = busy || v.busy();
    active_ = busy;
}

const char *Kick::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "FMA", "FMD", "CLK",      // SRC
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "DEC", "TON", "DRT", "LVL",      // AMP
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- voices ---------------------------------------------------------------

void Kick::start(Voice &v) {
    const Trig &t = v.next;
    v.pending = false;
    v.wait = 0;
    v.fade = 0;
    v.gain = 1.0f;
    v.note_ratio = clamp(semitones(float(t.note) - 60.0f), 0.05f, 12.0f);
    // The reference's trigger level, 0.3 + 0.7 * accent. It scales the FM envelope as well as the
    // amplitude, so a hard hit sweeps further and brightens rather than only getting louder.
    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    const float accent = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    v.trigger_level = 0.3f + 0.7f * accent;
    v.phase = 0.25f;           // pinned here for the next 1.3 ms
    v.hold = hold_frames_;
    v.gate = gate_frames_;
    v.fm_env = v.trigger_level;
    v.slew = 0.0f;
    v.click_hp = 0.0f;
    v.click_band.reset();
    v.noise_lp.reset();
    v.noise_hp.reset();
    v.tone_lp.reset();
    v.amp.gate_on(true);
    v.transient.gate_on(true);
}

void Kick::silence(Voice &v) {
    v.amp.kill();
    v.transient.kill();
    v.hold = 0;
    v.gate = 0;
    v.fade = 0;
    v.gain = 1.0f;
}

void Kick::render_voice(Voice &v, float *out, int n) {
    int i = 0;
    while (i < n) {
        if (v.pending && v.wait == 0) {
            start(v);   // sample-accurate: the hit begins on trig.frame
            continue;
        }
        int k = n - i;
        if (v.pending && v.wait < k) k = v.wait;
        if (v.fade > 0 && v.fade < k) k = v.fade;
        run(v, out + i, k);
        if (v.pending) v.wait -= k;
        if (v.fade > 0) {
            v.fade -= k;
            if (v.fade == 0) silence(v);   // faded to zero: drop whatever was sounding
        }
        i += k;
    }
}

void Kick::run(Voice &v, float *out, int n) {
    if (!v.sounding()) return;

    // Note-dependent coefficients: still control rate, just once per voice per block rather than
    // once per machine, because the trig note moves f0 and three of these follow f0.
    const float f0 = clamp(base_hz_ * v.note_ratio, 10.0f, 2000.0f);
    const float f0_norm = f0 * inv_rate_;
    const float dirtiness = dirtiness_ * clamp(1.0f - 8.0f * f0_norm, 0.0f, 1.0f);
    const float tone_coefficient = clamp(4.0f * f0_norm * tone_ratio_, 0.0f, 1.0f);

    // The body. While the phase is pinned at 0.25 the shaper reads 0, so those samples are
    // silence -- handled by clearing the buffer and starting the oscillator after the hold.
    enaga_clear(body_, n);
    const int held = v.hold < n ? v.hold : n;
    v.hold -= held;
    // The phase accumulator, the FM envelope (which runs through the pinned samples too) and the
    // shaper, in one cl6x-built pass over the block.
    strata_kick_body(body_, held, n, f0_norm, fm_amount_, fm_decay_, dirtiness, &v.phase,
                     &v.fm_env);

    v.amp.render(env_, n, amp_shape_);
    enaga_scale(env_, n, v.trigger_level);
    enaga_multiply(body_, env_, n);
    strata_one_pole_inplace(&v.tone_lp.state_, tone_coefficient, body_, n);

    // The transient: a 1 ms gate, slewed, high-passed and band-limited, plus attack noise. Both
    // ride the transient envelope. The gate and the slew are one pass, which keeps the state
    // out of a second loop.
    v.transient.render(transient_env_, n, transient_shape_);
    if (click_level_ > 0.0f) {
        const int gate = v.gate < n ? v.gate : n;
        v.gate -= gate;
        strata_kick_click(click_, n, gate, &v.slew, &v.click_hp);
        strata_svf_inplace(&v.click_band.state_, &click_coefficients_, Svf::low_pass, click_, n);

        enaga_clear(noise_, n);
        rng_.add_noise(noise_, n, 1.0f);
        strata_one_pole_inplace(&v.noise_lp.state_, 0.05f, noise_, n);
        v.noise_hp.process(noise_, aux_, n, 0.005f);
        enaga_mix(noise_, aux_, n, -1.0f);         // the band between the two one-poles
        enaga_add(click_, noise_, n);
        enaga_mix_product(body_, click_, transient_env_, n, click_level_ * v.trigger_level);
    } else {
        v.gate = 0;
    }

    if (v.fade > 0) {
        strata_mix_ramp(out, body_, n, out_gain_ * v.gain, -out_gain_ * kFadeStep);
        v.gain -= kFadeStep * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        enaga_mix(out, body_, n, out_gain_);
    }
}

}  // namespace strata
