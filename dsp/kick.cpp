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

using enaga::dsp::Envelope;
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
// Below this the transient envelope stops the click path (Kick::run): what the click and its
// noise would still add is under 3e-6 of full scale per unit of CLK, -110 dB, under the last
// bit of a 16-bit file. About 64 ms after the hit at tau = 5 ms; the body rings on far longer.
constexpr float kTransientFloor = 3.0e-6f;
// PNCH (kick.hpp). KNK at the top adds 15 x f0 to the increment at the hit: with TUN's 49 Hz the
// knock starts near 780 Hz, the top of the references' 250 Hz-1 kHz punch. Its time constant
// runs 1-16 ms (KND), against FMD's 8-128 ms: the knock is over before the body's sweep is.
constexpr float kKnockMax = 15.0f;
constexpr float kKnockTauLow = 0.001f;
constexpr float kKnockTauOctaves = 4.0f;
// The tick: noise high-passed at 3 kHz under a 1.5 ms envelope, kTickGain at TCK's top, set by
// measurement against the benchmark's references (kick.hpp).
constexpr float kTickHz = 3000.0f;
constexpr float kTickK = 1.4f;
constexpr float kTickTau = 0.0015f;
constexpr float kTickGain = 2.0f;
constexpr float kTickFloor = 1.0e-4f;
// TAIL's rumble (kick.hpp): the combs' feedback (a tail of about a second, which every hit ducks
// anyway), the input low-pass (the click and tick stay out of it), the duck's release (the
// rumble back up about 120 ms after a hit) and how long it rings unfed before it is skipped.
constexpr float kRumbleFeedback = 0.7f;
constexpr float kRumbleInHz = 400.0f;
constexpr float kRumbleRelease = 0.12f;
constexpr float kRumbleTailSeconds = 1.5f;
constexpr float kRumbleHpCoef = 0.004f;   // a one-pole high-pass near 30 Hz on the way out

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

float kick_knock_seconds(uint8_t value) {
    return kKnockTauLow * exp2(kKnockTauOctaves * float(value) * (1.0f / 127.0f));
}

int kick_drop_cents(uint8_t value) { return -int((int(value) * 1200 + 63) / 127); }

float kick_choke_seconds(uint8_t value) {
    // 60 ms at 1 down to 1 ms at 127, exponential: log2(60) = 5.907 octaves.
    return value == 0 ? 0.0f : 0.060f * exp2(-5.9069f * float(value - 1) * (1.0f / 126.0f));
}

void Kick::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    hold_frames_ = int(0.0013f * rate_);
    gate_frames_ = int(0.0010f * rate_);
    transient_shape_ =
        EnvelopeShape::make(0.0002f, kTransientTau * kToOnePercent, 0.0f, 0.001f, rate_);
    click_coefficients_ =
        SvfCoefficients::from_g(enaga::dsp::tan_pi(kClickHz * inv_rate_), kClickK);
    tick_coefficients_ = SvfCoefficients::from_g(enaga::dsp::tan_pi(kTickHz * inv_rate_), kTickK);
    tick_decay_ = exp(-inv_rate_ * reciprocal(kTickTau));
    for (uint8_t &b : punch_bytes_) b = 0xFF;   // a new rate: PNCH's coefficients are made again
    for (uint8_t &b : rumble_bytes_) b = 0xFF;
    rumble_lp_.reset();
    rumble_hp_.reset();
    choke_byte_ = 0;
    choke_frames_ = 0;
    for (Voice &v : voices_) {
        v = Voice{};
        v.click_band.reset();
        v.noise_lp.reset();
        v.noise_hp.reset();
        v.tone_lp.reset();
        v.tick_hp.reset();
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

    // CUT (kick.hpp): CHK, the OSC page's first slot. One byte, read every block, no exponential
    // unless it moved.
    {
        const uint8_t chk = params.get(kOscFirst);
        if (chk != choke_byte_) {
            choke_byte_ = chk;
            const float t = kick_choke_seconds(chk);
            choke_frames_ = chk == 0 ? 0 : (int(t * rate_) > 0 ? int(t * rate_) : 1);
        }
        // RMB RTN RDR, made again only when one moves.
        const uint8_t rb[3] = {params.get(kOscFirst + 1), params.get(kOscFirst + 2),
                               params.get(kOscFirst + 3)};
        if (rb[0] != rumble_bytes_[0] || rb[1] != rumble_bytes_[1] || rb[2] != rumble_bytes_[2]) {
            for (int i = 0; i < 3; i++) rumble_bytes_[i] = rb[i];
            const float level = float(rb[0]) * (1.0f / 127.0f);
            rumble_level_ = level * level * 2.0f;
            const float hz = 60.0f * exp2(2.737f * float(rb[1]) * (1.0f / 127.0f));   // 60-400 Hz
            rumble_coefficients_ = SvfCoefficients::make(hz, 0.3f, rate_);
            const float drive_db = 30.0f * float(rb[2]) * (1.0f / 127.0f);
            rumble_drive_ = exp2(drive_db * (1.0f / 6.0206f)) * 0.5f;
            rumble_makeup_ = reciprocal(1.0f + 0.15f * drive_db);
        }
    }
    // PNCH (kick.hpp): KNK KND TCK DRP, the ASR page's four slots. Made again only when one of
    // them, or DEC, has moved: a kick with the page at 0 pays one compare a block.
    const uint8_t bytes[5] = {params.get(kAsrFirst + 0), params.get(kAsrFirst + 1),
                              params.get(kAsrFirst + 2), params.get(kAsrFirst + 3), params.get(8)};
    bool same = true;
    for (int i = 0; i < 5; i++) same = same && bytes[i] == punch_bytes_[i];
    if (same) return;
    for (int i = 0; i < 5; i++) punch_bytes_[i] = bytes[i];
    knock_amount_ = kKnockMax * params.unit(kAsrFirst + 0);
    knock_decay_ = bytes[0] == 0 ? 0.0f : exp(-inv_rate_ * reciprocal(kick_knock_seconds(bytes[1])));
    tick_gain_ = kTickGain * params.unit(kAsrFirst + 2);
    // The fall: down to 2^-DRP of f0 (an octave at the top), on the amplitude's own time
    // constant, so the pitch has fallen most of the way by the time the body has.
    drop_target_ = bytes[3] == 0 ? 1.0f : exp2(-params.unit(kAsrFirst + 3));
    drop_rate_ = -inv_rate_ * reciprocal(amp_tau);
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
    // CHK: every other voice still sounding is faded out over the choke time, from the new
    // hit's frame on, so a long kick is cut at the next one as a one-beat sample is. A voice
    // already fading keeps the shorter of its fade and this one.
    if (choke_frames_ > 0) {
        for (int i = 0; i < kVoices; i++) {
            Voice &o = voices_[i];
            if (i == pick || !o.sounding() || o.pending) continue;
            const int frames = int(trig.frame) + choke_frames_;
            if (o.fade == 0 || frames < o.fade) {
                if (o.fade == 0) o.gain = 1.0f;
                o.fade = frames;
                o.fade_step = o.gain * reciprocal(float(frames));
            }
        }
    }
    // A voice that is still loud is faded out first and the new hit starts at the end of the
    // fade. Below kFadeFloor the splice is inaudible, so it starts straight away instead.
    if (v.fade == 0 && v.sounding() && v.amp.level() > kFadeFloor) {
        v.fade = kFadeFrames;
        v.fade_step = kFadeStep;
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
    const bool rumble = rumble_level_ > 0.0f;
    int done = 0;
    while (done < frames) {
        const int n = frames - done < kBlock ? frames - done : kBlock;
        if (!rumble) {
            for (Voice &v : voices_) render_voice(v, out + done, n);
        } else {
            // With TAIL's rumble the voices go to a block of their own first, which feeds it.
            enaga_clear(mix_, n);
            for (Voice &v : voices_) render_voice(v, mix_, n);
            enaga_add(out + done, mix_, n);
            render_rumble(out + done, n);
        }
        done += n;
    }
    bool busy = false;
    for (const Voice &v : voices_) busy = busy || v.busy();
    if (rumble && busy) rumble_tail_ = int(kRumbleTailSeconds * rate_);
    else if (rumble_tail_ > 0) rumble_tail_ -= frames;
    active_ = busy || (rumble && rumble_tail_ > 0);
}

void Kick::render_rumble(float *out, int n) {
    const float release = exp(-inv_rate_ * reciprocal(kRumbleRelease));
    const float lp_coef = 1.0f - exp(-6.2831853f * kRumbleInHz * inv_rate_);
    strata_kick_rumble(rumble_, mix_, n, lp_coef, &rumble_in_lp_, rumble_ap1_, kRumbleAp1,
                       &rumble_pos_[0], rumble_ap2_, kRumbleAp2, &rumble_pos_[1], rumble_c1_,
                       kRumbleC1, &rumble_pos_[2], rumble_c2_, kRumbleC2, &rumble_pos_[3],
                       kRumbleFeedback, rumble_drive_, rumble_makeup_, &rumble_duck_, release);
    strata_svf_inplace(&rumble_lp_.state_, &rumble_coefficients_, Svf::low_pass, rumble_, n);
    rumble_hp_.process(rumble_, rumble_aux_, n, kRumbleHpCoef);
    enaga_mix(rumble_, rumble_aux_, n, -1.0f);   // less its one-pole low-pass: a high-pass
    enaga_mix(out, rumble_, n, rumble_level_);
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
    v.knock_env = v.trigger_level;
    v.tick_env = v.trigger_level;
    v.drop_env = 1.0f;
    v.tick_hp.reset();
    rumble_duck_ = 1.0f;     // TAIL: the hit pulls the rumble under it
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
    // DRP: f0 falls from itself towards drop_target_ of itself along drop_env. Control rate: a
    // fall over hundreds of ms in 1.3 ms steps is a smooth glide on a low sine.
    const float drop = drop_target_ + (1.0f - drop_target_) * v.drop_env;
    if (drop_target_ < 1.0f) v.drop_env *= exp(float(n) * drop_rate_);
    const float f0 = clamp(base_hz_ * v.note_ratio * pitch_mod_ratio_ * drop, 10.0f, 2000.0f);
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
    if (knock_amount_ > 0.0f) {
        strata_kick_body_knock(body_, held, n, f0_norm, fm_amount_, fm_decay_, knock_amount_,
                               knock_decay_, dirtiness, &v.phase, &v.fm_env, &v.knock_env);
    } else {
        strata_kick_body(body_, held, n, f0_norm, fm_amount_, fm_decay_, dirtiness, &v.phase,
                         &v.fm_env);
    }

    v.amp.render(env_, n, amp_shape_);
    enaga_scale(env_, n, v.trigger_level);
    enaga_multiply(body_, env_, n);
    strata_one_pole_inplace(&v.tone_lp.state_, tone_coefficient, body_, n);

    // The transient: a 1 ms gate, slewed, high-passed and band-limited, plus attack noise. Both
    // ride the transient envelope. The gate and the slew are one pass, which keeps the state
    // out of a second loop.
    // Once the transient has decayed under kTransientFloor (it only falls after its attack), the
    // click path would add nothing audible for the rest of the note: skip it, and the
    // envelope with it, until the next hit resets both (start() gates the transient on from
    // zero). The shared noise generator still steps n times, so later hits on either voice draw
    // the same noise they would have. Not bit-exact: the skipped tail was under -110 dB.
    const bool transient_spent = v.transient.stage() != Envelope::attack &&
                                 v.transient.level() < kTransientFloor;
    if (transient_spent) {
        v.gate = v.gate > n ? v.gate - n : 0;
        if (click_level_ > 0.0f) strata_rng_skip(rng_.state(), n);
    } else if (click_level_ > 0.0f) {
        v.transient.render(transient_env_, n, transient_shape_);
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
        v.transient.render(transient_env_, n, transient_shape_);
        v.gate = 0;
    }

    // TCK: high-passed noise under its own 1.5 ms envelope, after the tone low-pass so TON
    // never dulls it. Skipped once the envelope is spent (under -80 dB), and entirely at TCK 0.
    if (tick_gain_ > 0.0f && v.tick_env > kTickFloor) {
        enaga_clear(aux_, n);
        tick_rng_.add_noise(aux_, n, 1.0f);
        strata_svf_inplace(&v.tick_hp.state_, &tick_coefficients_, Svf::band_pass, aux_, n);
        strata_kick_tick(body_, aux_, n, tick_gain_, tick_decay_, &v.tick_env);
    }

    if (v.fade > 0) {
        strata_mix_ramp(out, body_, n, out_gain_ * v.gain, -out_gain_ * v.fade_step);
        v.gain -= v.fade_step * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        enaga_mix(out, body_, n, out_gain_);
    }
}

}  // namespace strata
