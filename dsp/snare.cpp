// SNARE, after Mutable Instruments Plaits `synthetic_snare_drum.h` (MIT licensed,
// Copyright 2016 Emilie Gillet). See snare.hpp and docs/SOUND.md.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Two voices, so this is per voice per 64-frame block, counting multiplies and adds in the hot
// loops only. These are the figures the central measurement (docs/BUDGET.md, PLAN.md M0.5)
// should be compared against.
//
//   stage              expected to dominate                            flops/sample
//   the tuned pair     two soft sines, each a reciprocal (~8) and      ~24   THE LARGEST STAGE
//                      the fold. No clean-sine blend here, unlike
//                      KICK, so no sin_turns.
//   pair mix           the reference's -0.1 offset and its 0.60 /       4
//                      0.25 weights, plus two phase advances.
//   drum env and       Envelope::render, a scale, a multiply and        7
//   one-pole at 3*f0   the OnePole: cl6x block functions.
//   noise band         Svf::process with Q = 0.5 + 2*snappy, then      ~14
//                      a OnePole and the subtraction that makes
//                      the high-pass.
//   noise source       one xorshift to float per sample.               ~5
//   noise env          Envelope::render and one mix_product.           ~5
//
// So about 66 flops/sample, and no stage is skippable: a snare is always making both halves.
// Against Enaga's measured VA voice (1,868 cycles per block = 29 cycles/sample for roughly
// 70-80 flops/sample, so ~2.5 flops per cycle on this core) that is **about 1,700 cycles per
// voice per block, ~0.9x a full VA voice** -- not the "far simpler" the brief expected, for the
// same reason as KICK: the reference's shaper costs a reciprocal per oscillator per sample, and
// there are two oscillators. The cheap win, if the measurement disappoints, is a polynomial fit
// for 1/(1+|t|) over [1, 2]; the hand-written loop here is also the obvious candidate for a
// *.ti.c file, which CLAUDE.md asks for and which this agent's file list did not allow.
#include "snare.hpp"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::OnePole;
using enaga::dsp::Svf;
using enaga::dsp::SvfCoefficients;
using enaga::dsp::clamp;
using enaga::dsp::exp2;
using enaga::dsp::log2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;
using enaga::dsp::tan_pi;

// ln(100): EnvelopeShape::make wants the time to reach 1% of its target where the reference
// gives time constants, and one is ln(100) of the other.
constexpr float kToOnePercent = 4.60517019f;

// The rational soft sine both oscillators run through: a triangle folded by 2x/(1+|x|). The
// reciprocal is a short run of multiplies on the C674x, not a division.
inline float soft_sine(float phase) {
    const float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
    const float magnitude = triangle < 0.0f ? -triangle : triangle;
    return 2.0f * triangle * reciprocal(1.0f + magnitude);
}

// sqrt without libm, as exp2(log2(x)/2). Control rate only, so its cost does not matter.
inline float root(float x) { return x <= 0.0f ? 0.0f : exp2(0.5f * log2(x)); }

void mix_ramp(float *out, const float *in, int n, float gain, float step) {
    for (int i = 0; i < n; i++) {
        out[i] += gain * in[i];
        gain += step;
    }
}

}  // namespace

// --- parameters -----------------------------------------------------------
//
// SRC
//   0  TUN  f0, 90-439 Hz exponential (STRATA's range; the reference takes f0 from pitch CV, and
//           a 909 snare sits at 200 Hz, which is exactly knob 64). The note transposes from 60.
//   1  SNP  snappy, 0-1 linear: one knob with the reference's four correlated effects -- the drum
//           scaled by sqrt(1 - SNP), the noise by sqrt(SNP), the noise band's Q = 0.5 + 2 * SNP,
//           and 7 semitones off the drum envelope at the top.
//   2  TON  noise band, +-24 semitones on the reference's min(35 * f0, 0.5) cutoff, so knob 64 is
//           exactly the reference and the ends are a quarter and four times it.
//   3  CLP  clap mode: under 43 off, 43-84 three bursts, 85 and over four. In clap mode the pair
//           is retuned to the 808 shell ratio 2.00 and the noise hold gives way to the bursts.
// AMP
//   8  DEC  decay, 0-1. The drum envelope's time constant is 15 ms * 2^((DEC*72 + BDY*12 -
//           SNP*7)/12), so 15 ms to about a second, and the noise hold is 0.04 + 0.03 * DEC
//           seconds -- 40 ms at the bottom of the knob, 70 ms at the top.
//   9  SPR  clap spread, 2-40 ms between bursts, linear (STRATA's own: the reference has no clap).
//           The early bursts' decay follows it so they stay distinct.
//  10  BDY  the reference's fm_amount term on the drum envelope, 0 to +12 semitones of tail.
//  11  LVL  output level, square law, 0-1.

void Snare::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    for (Voice &v : voices_) {
        v = Voice{};
        v.noise_band.reset();
        v.drum_lp.reset();
        v.noise_hp.reset();
    }
    active_ = false;
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Snare::prepare(const TrackParams &params) {
    // 90-439 Hz, with the exponent chosen so that knob 64 is exactly 200 Hz, where a 909 snare
    // sits.
    base_hz_ = 90.0f * exp2(2.2860100f * params.unit(0));
    const float snappy = params.unit(1);
    drum_gain_ = root(1.0f - snappy);
    noise_gain_ = root(snappy);
    noise_k_ = reciprocal(0.5f + 2.0f * snappy);                     // k = 1/Q
    tone_ratio_ = semitones((params.unit(2) - 0.5f) * 48.0f);

    const float decay = params.unit(8);
    const float body = params.unit(10);
    const float drum_tau = 0.015f * semitones(decay * 72.0f + body * 12.0f - snappy * 7.0f);
    drum_shape_ = EnvelopeShape::make(0.0005f, drum_tau * kToOnePercent, 0.0f, 0.001f, rate_);
    // The hold: sustain 1, so the envelope climbs to full level and stays there. When the hold
    // runs out render() switches to the tail shape, whose sustain is 0, and the same envelope
    // starts falling from where it was -- no gate_off and no second envelope.
    noise_hold_shape_ = EnvelopeShape::make(0.0005f, 0.0005f, 1.0f, 0.001f, rate_);
    const float noise_tau = 0.015f * semitones(decay * 72.0f - snappy * 7.0f);
    noise_tail_shape_ = EnvelopeShape::make(0.0005f, noise_tau * kToOnePercent, 0.0f, 0.001f, rate_);
    hold_frames_ = int((0.04f + 0.03f * decay) * rate_);

    const uint8_t clap = params.get(3);
    clap_bursts_ = clap < 43 ? 0 : (clap < 85 ? 3 : 4);
    const float spread_s = 0.002f + 0.038f * params.unit(9);
    burst_frames_ = int(spread_s * rate_);
    if (burst_frames_ < 1) burst_frames_ = 1;
    // Short enough that the bursts do not run into each other at any spread.
    const float burst_s = clamp(spread_s * 0.35f, 0.001f, 0.012f);
    burst_shape_ = EnvelopeShape::make(0.0003f, burst_s, 0.0f, 0.001f, rate_);

    const float level = params.unit(11);
    out_gain_ = level * level * kMakeup;
}

void Snare::trigger(const Trig &trig) {
    int pick = -1;
    for (int i = 0; i < kVoices; i++) {
        if (!voices_[i].busy()) {
            pick = i;
            break;
        }
    }
    if (pick < 0) pick = voices_[0].level() <= voices_[1].level() ? 0 : 1;

    Voice &v = voices_[pick];
    if (v.fade == 0 && v.sounding() && v.level() > kFadeFloor) {
        v.fade = kFadeFrames;
        v.gain = 1.0f;
    }
    v.next = trig;
    v.wait = int(trig.frame);
    if (v.wait < v.fade) v.wait = v.fade;
    v.pending = true;
    active_ = true;
}

void Snare::release_all() {
    // One-shot percussion: the decay is the release and the tail finishes by itself, clap bursts
    // included. kill_all() is the hard cut.
}

void Snare::kill_all() {
    for (Voice &v : voices_) {
        v.pending = false;
        v.wait = 0;
        silence(v);
    }
    active_ = false;
}

void Snare::render_add(float *out, int frames) {
    if (!active_) return;
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

const char *Snare::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "SNP", "TON", "CLP",      // SRC
        "---", "---", "---", "---",      // FLTR
        "DEC", "SPR", "BDY", "LVL",      // AMP
        "---", "---", "---", "---",      // FX
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- voices ---------------------------------------------------------------

void Snare::start(Voice &v) {
    const Trig &t = v.next;
    v.pending = false;
    v.wait = 0;
    v.fade = 0;
    v.gain = 1.0f;
    v.note_ratio = clamp(semitones(float(t.note) - 60.0f), 0.05f, 12.0f);
    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    const float accent = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    v.trigger_level = 0.3f + 0.7f * accent;     // the reference's trigger level
    v.phase_a = 0.0f;
    v.phase_b = 0.25f;                          // a quarter turn apart, so the pair reads as one
    v.drum_lp.reset();
    v.noise_hp.reset();
    v.noise_band.reset();
    v.drum.gate_on(true);
    v.noise.gate_on(true);
    // A clap has no hold: its bursts are the texture. A snare holds, then decays. And a clap
    // takes the 808's shell interval where a snare takes the 909 schematic's.
    v.hold = clap_bursts_ > 0 ? 0 : hold_frames_;
    v.bursts = clap_bursts_ > 0 ? clap_bursts_ - 1 : 0;
    v.burst_wait = burst_frames_;
    v.ratio = clap_bursts_ > 0 ? kClapRatio : kDrumRatio;
}

void Snare::silence(Voice &v) {
    v.drum.kill();
    v.noise.kill();
    v.hold = 0;
    v.bursts = 0;
    v.fade = 0;
    v.gain = 1.0f;
}

void Snare::render_voice(Voice &v, float *out, int n) {
    int i = 0;
    while (i < n) {
        if (v.pending && v.wait == 0) {
            start(v);
            continue;
        }
        int k = n - i;
        if (v.pending && v.wait < k) k = v.wait;
        if (v.fade > 0 && v.fade < k) k = v.fade;
        if (v.hold > 0 && v.hold < k) k = v.hold;                 // the hold ends on its frame
        if (v.bursts > 0 && v.burst_wait < k) k = v.burst_wait;   // and a burst starts on its
        run(v, out + i, k);
        if (v.pending) v.wait -= k;
        if (v.hold > 0) v.hold -= k;
        if (v.fade > 0) {
            v.fade -= k;
            if (v.fade == 0) silence(v);
        }
        if (v.bursts > 0) {
            v.burst_wait -= k;
            if (v.burst_wait == 0) {
                v.noise.gate_on(true);
                v.bursts--;
                v.burst_wait = burst_frames_;
            }
        }
        i += k;
    }
}

void Snare::run(Voice &v, float *out, int n) {
    if (!v.sounding()) return;

    // Note-dependent coefficients: control rate, once per voice per block, because every one of
    // them is a multiple of f0 and the trig note moves f0.
    const float f0 = clamp(base_hz_ * v.note_ratio, 20.0f, 2000.0f);
    const float f0_norm = f0 * inv_rate_;
    const float dp_a = f0_norm > 0.5f ? 0.5f : f0_norm;
    const float dp_b = clamp(f0_norm * v.ratio, 0.0f, 0.5f);
    const float drum_lp = clamp(3.0f * f0_norm, 0.0f, 1.0f);
    const float band = clamp(35.0f * f0_norm * tone_ratio_, 0.001f, 0.49f);
    const SvfCoefficients band_coefficients = SvfCoefficients::from_g(tan_pi(band), noise_k_);
    const float hp = clamp(10.0f * f0, 20.0f, 0.49f * rate_);
    const float hp_coefficient = OnePole::coefficient(hp, rate_);

    // The drum: the tuned pair through the soft sine, with the reference's -0.1 offset and its
    // 0.60 / 0.25 weights.
    float phase_a = v.phase_a;
    float phase_b = v.phase_b;
    for (int i = 0; i < n; i++) {
        phase_a += dp_a;
        phase_a -= phase_a >= 1.0f ? 1.0f : 0.0f;
        phase_b += dp_b;
        phase_b -= phase_b >= 1.0f ? 1.0f : 0.0f;
        drum_[i] = -0.1f + 0.60f * soft_sine(phase_a) + 0.25f * soft_sine(phase_b);
    }
    v.phase_a = phase_a;
    v.phase_b = phase_b;

    v.drum.render(env_, n, drum_shape_);
    enaga_scale(env_, n, v.trigger_level * drum_gain_);
    enaga_multiply(drum_, env_, n);
    v.drum_lp.process(drum_, drum_, n, drum_lp);
    enaga_clear(mix_, n);
    enaga_add(mix_, drum_, n);

    // The noise: band-pass by a low-pass with snappy's Q and then a one-pole high-pass.
    enaga_clear(noise_, n);
    rng_.add_noise(noise_, n, 1.0f);
    v.noise_band.process(noise_, noise_, n, band_coefficients, Svf::low_pass);
    v.noise_hp.process(noise_, aux_, n, hp_coefficient);
    enaga_mix(noise_, aux_, n, -1.0f);     // one-pole high-pass: the signal less its low-pass

    // Which envelope shape: the hold while it lasts, then the tail; in clap mode a short burst
    // shape until the last burst, which keeps the tail.
    const EnvelopeShape &shape = v.hold > 0 ? noise_hold_shape_
                                            : (v.bursts > 0 ? burst_shape_ : noise_tail_shape_);
    v.noise.render(env_, n, shape);
    enaga_mix_product(mix_, noise_, env_, n, noise_gain_ * v.trigger_level);

    if (v.fade > 0) {
        mix_ramp(out, mix_, n, out_gain_ * v.gain, -out_gain_ * kFadeStep);
        v.gain -= kFadeStep * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        enaga_mix(out, mix_, n, out_gain_);
    }
}

}  // namespace strata
