// VA MONO, the bass voice. va.hpp has the topology and why the filter is on the SRC page.
//
// It also holds `VaVoice` and `VaSettings`, the voice and its control-rate block, which VA POLY
// (dsp/vapoly.cpp) holds six of. Everything below the "--- the voice" rule is therefore shared
// code: the cost accounting in this comment is per VOICE, so VA POLY's figure is six times it,
// except for `prepare`, which VA POLY pays once a block however many voices sound.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// One voice, so this is per block of 64 frames, counting multiplies and adds in the hot loops
// only. The comparison that matters is Enaga's own measured VA voice -- `examples/synth`, 1,868
// cycles per voice per block, 0.377% of the 496,000-cycle block -- because this file is built
// on the same primitives and in the same shape, and because the whole of STRATA's budget is
// derived from that figure. These are the numbers the central measurement (docs/BUDGET.md,
// PLAN.md M0.5) should be compared against.
//
//   stage                expected to dominate                            flops/sample
//   OSC 2 (sync)         enaga_oscillator_sync_add: a PolyBLEP pulse      ~16   THE LARGEST
//   (OS2 64+ only)       PLUS a band-limited restart wherever OSC 1's           STAGE
//                        phase wraps, so it carries two corrections
//                        and runs a sample ahead of the block.
//   OSC 1 (saw)          enaga_oscillator_add: the ramp and one            ~9
//                        PolyBLEP correction per wrap.
//   filter               Svf::process: two one-pole integrators and        ~9 (12 dB)
//                        the output mix. 24 dB is Svf::cascade, two       ~12 (24 dB)
//                        poles for about the price of one, so LP24 is
//                        NOT twice LP12.
//   soft clip            enaga_soft_clip_buffer after the filter: a        ~7
//                        clamp and a cubic. This is what bounds the
//                        voice to +-1 however far resonance pushes it,
//                        and it is the 303's dirt as RES comes up.
//   SUB (triangle)       naive, piecewise linear: no PolyBLEP and no       ~5
//                        polynomial. A sine sub would cost ~13.
//   amp env + VCA        Envelope::render, enaga_multiply, and the         ~6
//   + output ramp        gain ramp into the caller's buffer.
//   noise                one xorshift to float and a mix. Always on,        ~3
//                        with no level test to skip it, because the
//                        trace is part of the voice: the level is a
//                        constant and is never 0.
//   control step         once per 16 frames: the glide, the filter        ~4.5
//   (1/16 rate)          envelope's step, one semitones() for the
//                        pitch, one tan_pi and three reciprocals for
//                        the coefficients. Per sample this would be
//                        the single costliest thing in the file.
//   prepare (1/64 rate)  four EnvelopeShape::make (twelve exp() in       ~2.5
//                        all, since accent has shapes of its own) and
//                        four exp2 for the knob curves. PER BLOCK, not
//                        per voice: VA POLY's six voices share one.
//
// So about 65 flops/sample with LP24. Against Enaga's measured VA voice (1,868 cycles per block
// = 29 cycles/sample for roughly 70-80 flops/sample, so ~2.5 flops per cycle on this core) that
// is **about 1,660 cycles per voice per block, ~0.89x a full VA voice, 0.33% of the block** --
// inside the target, which it should be: the reference carries three ADSRs, two LFOs and an
// eight-slot modulation matrix that this voice does not.
//
// **That derivation is wrong by a factor of eleven, and the table above is still here because
// the SHAPE of it is right.** Measured on the emulator, a voice costs about 18,900 cycles a
// block, not 1,660 (docs/BUDGET.md, "VA POLY costs 4.25 points per voice" and the optimisation
// loop after it). 2.5 flops per cycle was taken from Enaga's example and is far too generous
// for this code, which is branchier and, in the GCC-only build the figure was taken from, not
// pipelined at all. The ORDER of the stages has held up -- the synced pulse really is the
// largest, the control step really is about a fifth of a voice -- so read this table for what
// to attack and docs/BUDGET.md for what anything costs.
//
// All of the per-sample work is in C block functions that cl6x compiles and pipelines: Enaga's
// for the oscillators, the noise, the filter and the envelope, and `strata_va_tail` in
// dsp/valoops.ti.c for the output tail -- the soft clip, the amplitude envelope and the ramped
// add into the bus, fused into one pass. That fusion is what this comment used to ask for; it
// replaced three passes over the slice and two round-trips of `work` through memory, and
// docs/BUDGET.md has what it was worth. There is no hand-written loop left in this file.
#include "va.hpp"

#include "tihints.h"
#include "valoops.h"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::Svf;
using enaga::dsp::SvfCoefficients;
using enaga::dsp::Wave;
using enaga::dsp::clamp;
using enaga::dsp::exp;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;
using enaga::dsp::tan_pi;

// --- the oscillator levels ----------------------------------------------------------------
//
// The OSC page picks the waves and what OSC 2 does; these are the levels each choice runs at,
// set so every choice is about as loud as a lone saw (two detuned saws add in power, so each
// is 1/sqrt(2) of one), and so the sum runs the post-filter soft clip slightly into its curve.
constexpr float kLevelOsc1 = 0.60f;        // OSC 1 alone
constexpr float kLevelDetune = 0.43f;      // each of the two in DET's zone
constexpr float kLevelSync1 = 0.28f;       // SYN: OSC 1 is the body underneath ...
constexpr float kLevelSync2 = 0.46f;       // ... and the synced OSC 2 is the voice
constexpr float kLevelSyncAccent = 0.16f;  // an accent brings the sync forward
constexpr float kLevelSubMax = 0.50f;      // SUB at 127: triangle, an octave down
constexpr float kLevelNoise = 0.016f;      // a trace of grit
constexpr float kLevelNoiseAccent = 0.050f;

// OS2's zones: 0 OFF, 1-63 DET, 64-127 SYN.
constexpr float kDetuneMaxSemis = 0.60f;   // DET at 63: 60 cents between the two (square law)
constexpr float kSyncMaxSemis = 24.0f;     // SYN at 127: OSC 2 two octaves above OSC 1
// WAV: 0-31 the saw, 32-127 a pulse from square (50%) to thin (8%).
constexpr uint8_t kPulseFirst = 32;
constexpr float kPulseNarrowest = 0.08f;

// LP24's first stage is a gentle, Butterworth-damped pole pair (Q 0.71); only the second
// resonates. That keeps the 4-pole knee sharp and the resonance one controllable peak.
constexpr float kStageOneK = 1.414f;

// RES's curve: k = 2 x (kResFloorK / 2)^r, so the resonant peak rises by about the same number
// of dB for every count (va_resonance_k), minus a last push that takes k just below zero over
// the top few counts, where the filter self-oscillates at the level the resonance limiter in
// run() allows (kResLimitSq).
constexpr float kResFloorK = 0.03f;
constexpr float kResOscillate = 0.04f;
constexpr float kResFloorOctaves = -6.0589f;   // log2(kResFloorK / 2)
// The resonance limiter (VaVoice::run): above this amplitude, squared, of the resonant stage,
// k rises by kResLimitDamp per unit of excess.
constexpr float kResLimitSq = 0.4f;
constexpr float kResLimitDamp = 0.08f;

// DRV: the gain into the filter's input clip, 0.6 at 0 (the oscillator sum stays almost clean)
// to 6 at 127, and how much of it also lifts the post-filter clip.
constexpr float kPreDriveLow = 0.7f;
constexpr float kPreDriveOctaves = 3.1f;   // 0.7 x 2^3.1 = 6
constexpr float kPostDriveFromDrv = 0.6f;
constexpr float kDriveLevelTrim = 0.45f;   // a driven voice is denser: LVL loses up to 3 dB

// --- the knob curves ------------------------------------------------------------------------
constexpr float kTuneStep = 24.0f / 64.0f;     // +-24 semitones, exactly 0 at knob 64
constexpr float kCutoffLowHz = 35.0f;
constexpr float kCutoffOctaves = 8.4214f;      // log2(12000 / 35)
constexpr float kEnvSemitones = 66.0f;         // 5.5 octaves of cutoff at ENV 127
constexpr float kDecayLowS = 0.015f;
constexpr float kDecayOctaves = 7.3219f;       // log2(2.4 / 0.015): 15 ms to 2.4 s
constexpr float kAttackOctaves = 10.9658f;     // log2(5 / 0.0025): ATK, 2.5 ms to 5 s
constexpr float kReleaseOctaves = 7.0589f;     // log2(8 / 0.06): REL, 60 ms to 8 s
constexpr float kGlideScaleS = 0.004f;
constexpr float kGlideOctaves = 7.0f;          // 0 to 0.5 s, and 0 means 0
// The soft clip bounds the voice to +-1, so this times the accent's gain is the machine's hard
// ceiling: 0.72 x 1.4 = 1.008. It is calibrated on the LOUDEST case (accent, full resonance,
// velocity 127), which means a plain note at LVL 127 peaks nearer 0.5 -- a bass sitting a few
// dB under the kick, which is where a bass belongs, and MIXER mode has the fader for the rest.
constexpr float kHeadroom = 0.72f;

// --- the amplitude and filter envelopes -----------------------------------------------------
//
// EnvelopeShape::make wants a time to within 1% of the target, NOT a time constant
// (docs/ENAGA_API.md, "Gotchas"). Every time below is already a time to 1%, which is what DEC
// is defined as, so there is no ln(100) factor here -- contrast dsp/kick.cpp, which converts
// published time constants and therefore has to multiply by it.
constexpr float kAttackS = 0.0025f;            // short, but not a step: a step clicks
constexpr float kAttackAccentS = 0.0006f;      // an accent snaps
constexpr float kReleaseS = 0.060f;            // what STOP-once leaves behind
constexpr float kFilterAttackS = 0.0005f;      // the sweep starts at once
constexpr float kFilterReleaseS = 0.040f;
constexpr float kFilterDecayRatio = 0.55f;     // the sweep is shorter than the note: the "wow"
constexpr float kAccentDecayRatio = 0.82f;     // an accented note is a little shorter
constexpr float kAccentFilterRatio = 0.5f;     // and its sweep is a lot shorter
// With ATK up, the accent's attack keeps the proportion the two fixed attacks above have, and the
// filter's release keeps its proportion to the amplitude's, so it closes as the note fades.
constexpr float kAccentAttackRatio = kAttackAccentS / kAttackS;
constexpr float kFilterReleaseRatio = kFilterReleaseS / kReleaseS;

// --- accent ---------------------------------------------------------------------------------
constexpr float kAccentGain = 1.40f;           // louder, but that is the least of it
constexpr float kAccentCutSemis = 10.0f;       // the base cutoff opens
constexpr float kAccentEnvScale = 1.5f;        // and the sweep above it reaches further
constexpr float kVelocityFloor = 0.25f;        // velocity 1 is a quarter the level, not silence

// A one-pole coefficient that reaches its target to within 1% in `seconds`, stepped at `rate`.
// The same arithmetic EnvelopeShape::make uses, which is why glide and the envelopes agree
// about what a time means. Returns 1 (arrive at once) for anything under one step.
float to_target(float seconds, float rate) {
    const float steps = seconds * rate;
    return steps < 1.0f ? 1.0f : 1.0f - exp(-EnvelopeShape::kSpan * reciprocal(steps));
}

// The longest gate a note may carry, in frames: five minutes at 48 kHz, the same cap as VA
// POLY's (vapoly.cpp says why).
constexpr uint32_t kGateMax = 14400000u;

}  // namespace

// --- parameters -----------------------------------------------------------
//
// SRC -- the voice: its tuning and its filter. The filter is here rather than on AMP because it
// IS this machine's source character; the insert chain's FLTR page is the track's utility
// filter and is a different thing (see va.hpp).
//   0  TUN  tune, (knob - 64) * 24/64 semitones: +-24, exactly 0 at 64, exactly +12 at 96 and
//           -12 at 32. The trig note transposes on top of it, so note 60 sounds at TUN.
//   1  MOD  voice filter mode, in four zones that are deliberately NOT equal:
//              0-63   LP24    two poles cascaded: the acid filter, and half the knob because
//                             that is where a bass lives
//              64-95  LP12    one pole, the gentler roll-off
//              96-111 BP12    nasal, hollow: the one for mid-bass and reese-ish tones
//              112-127 HP12   for a bass layer that leaves the kick its own room
//           The default kit leaves every knob at 64, so the untouched default is LP12 rather
//           than a band-pass, which is what the weighting is for.
//   2  CUT  base cutoff, 35 Hz to 12 kHz exponential (8.42 octaves), so about 650 Hz at 64.
//           The filter envelope and the accent open it from there; nothing closes it, which is
//           why the knob's bottom end has to be genuinely closed.
//   3  RES  resonance, k = 2 x (kResFloorK / 2)^r less a small term at the very top, so each
//           count adds about the same number of dB to the peak (the old linear-in-k knob did
//           nothing for its first three quarters). Around 124 k crosses zero and the filter
//           self-oscillates, held at a fixed level by the control-rate limiter in run(). It
//           also drops the oscillator level by up to 30% and raises the post-filter soft
//           clip's drive by up to 30%, which is how the squelch gets dirty rather than just loud.
//           LP24's first stage stays at Butterworth damping, so the resonance is the second
//           stage's alone and LP24 squelches as hard as LP12 (it peaked at +4.6 dB before).
// AMP -- the envelopes and the output. Slot 0 is the decay and slot 3 the level on every STRATA
// machine, which this one keeps.
//   8  DEC  decay, 15 ms to 2.4 s exponential (about 190 ms at 64). ONE knob for both
//           envelopes, 303-style: the amplitude falls over DEC and the filter over 0.55 x DEC,
//           so the sweep always lands at the front of the note. With SUS (the ASR page) at 0
//           this is also the note's length; with SUS up it is the fall to the sustain level.
//   9  ENV  how much of the filter envelope reaches the cutoff, 0 to +66 semitones (5.5
//           octaves). At 0 the filter is static -- a dub bass; wide open with RES up it is the
//           acid squelch. The only knob an accent multiplies rather than replaces.
//  10  GLD  glide time, 0 to 0.5 s as 0.004 * (2^(7u) - 1), so 0 really is 0 and 64 is a
//           usable 42 ms. Portamento on every note; a slid step glides too, but never faster
//           than 55 ms (see start()). MONO's own knob: VA POLY spends this slot on DET.
//  11  LVL  output level, square law. Square gives usable control over the quiet end.

// ASR -- the note envelope, a page of its own after the arp (dsp/track.hpp: SRC and AMP are full).
// Every knob at 0 is EXACTLY the envelope this machine had before the page existed -- the same
// constants, not a curve that happens to land near them -- so an older kit, which loads with
// the four bytes at 0, keeps its envelope.
//  24  ATK  attack, 2.5 ms to 5 s exponential. The accent's attack scales with it (0.6 ms at
//           0), and above 0 the filter envelope's attack follows it too, so a slow pad swells
//           open as it fades in rather than arriving bright.
//  25  SUS  sustain, 0 to 1 of the peak, linear. At 0 the note is a one-shot whose length is
//           DEC, as it always was. Above 0 the note holds at SUS for as long as the KEYS pad is
//           held, or for the step's LEN; DEC becomes the fall from the peak to it. On VA MONO the
//           step's LEN only counts once SUS is up (va.hpp, gate_).
//  26  REL  release, 60 ms to 8 s exponential: the tail after the key comes up or LEN ends.
//           The filter's release keeps its old 2:3 proportion to it.
//  27  FSU  filter sustain, 0 to 1 of the filter envelope's sweep (ENV). At 0 the sweep
//           closes back to CUT over 0.55 x DEC, the 303 shape; up, it holds open, so a long pad
//           does not dull while it sustains.
//
// OSC -- the oscillators and the drive, the eighth page. An older kit loads WAV 0, OS2 0, SUB 102,
// DRV 0 (core/project.hpp, kKitOscLegacy): a saw and the old sub, without the old engine's
// always-on synced pulse, which is the closest thing the new page can play.
//  28  WAV  OSC 1's wave: 0-31 saw; 32-127 pulse, a square at 32 narrowing to 8% at 127.
//  29  OS2  OSC 2: 0 off; 1-63 a detuned second saw, 0.6 x u^2 semitones apart, the pair split
//           around the pitch so the note stays in tune; 64-127 a pulse hard-synced to OSC 1,
//           +0 at 64 to +24 semitones at 127. An accent lifts the synced pulse.
//  30  SUB  the triangle sub an octave down, level 0 to 0.5 linear. 0 skips it.
//  31  DRV  drive: a soft clip before the filter, 0.7 x 2^(3.1 x drv) into the clip, plus up to
//           60% more post-filter drive. The output is trimmed by 0.45 x drv^2 so turning it
//           thickens the sound rather than just making it louder. 0 skips the pre-filter clip.

float va_attack_seconds(uint8_t value) {
    return value == 0 ? kAttackS : kAttackS * exp2(kAttackOctaves * float(value) * (1.0f / 127.0f));
}

float va_release_seconds(uint8_t value) {
    return value == 0 ? kReleaseS
                      : kReleaseS * exp2(kReleaseOctaves * float(value) * (1.0f / 127.0f));
}

VaOsc2 va_osc2_mode(uint8_t value) {
    if (value == 0) return VaOsc2::off;
    return value < 64 ? VaOsc2::detune : VaOsc2::sync;
}

float va_osc2_detune_cents(uint8_t value) {
    if (va_osc2_mode(value) != VaOsc2::detune) return 0.0f;
    const float u = float(value) * (1.0f / 63.0f);
    return 100.0f * kDetuneMaxSemis * u * u;
}

float va_osc2_sync_semitones(uint8_t value) {
    if (va_osc2_mode(value) != VaOsc2::sync) return 0.0f;
    return kSyncMaxSemis * float(value - 64) * (1.0f / 63.0f);
}

int va_pulse_percent(uint8_t value) {
    if (value < kPulseFirst) return 0;
    const float u = float(value - kPulseFirst) * (1.0f / float(127 - kPulseFirst));
    return int(100.0f * (0.5f - (0.5f - kPulseNarrowest) * u) + 0.5f);
}

float va_resonance_k(uint8_t value) {
    const float r = float(value) * (1.0f / 127.0f);
    const float r2 = r * r;
    const float r8 = r2 * r2 * r2 * r2;
    return 2.0f * exp2(r * kResFloorOctaves) - kResOscillate * r8;
}

VaMode va_mode_from(uint8_t value) {
    // Three compares, no division: the C674x has no integer divide instruction at all, so even
    // at control rate a `/` is a libgcc call rather than one cycle.
    if (value < 64) return VaMode::lp24;
    if (value < 96) return VaMode::lp12;
    if (value < 112) return VaMode::bp12;
    return VaMode::hp12;
}

VaSettings VaSettings::make(const TrackParams &params, float rate, float inv_rate) {
    VaSettings s;
    s.rate = rate;
    s.inv_rate = inv_rate;
    const float control_rate = rate * (1.0f / float(VaVoice::kControl));

    s.tune = (float(params.get(0)) - 64.0f) * kTuneStep;
    s.mode = va_mode_from(params.get(1));
    s.cutoff_hz = kCutoffLowHz * exp2(kCutoffOctaves * params.unit(2));

    const float resonance = params.unit(3);
    s.k_res = va_resonance_k(params.get(3));
    const float makeup = 1.0f - 0.3f * resonance;  // keep the filter's input out of the clip
    const float drv = params.unit(Page::osc, 3);
    s.drive = (1.0f + 0.3f * resonance) * (1.0f + kPostDriveFromDrv * drv);  // the clip's dirt
    // DRV 0 is no input stage at all (Enaga's filter alone); above it, the gain into the clip.
    s.pre_drive = drv > 0.0f ? kPreDriveLow * exp2(kPreDriveOctaves * drv) : 0.0f;
    s.inv_pre_drive = drv > 0.0f ? reciprocal(s.pre_drive) : 0.0f;

    // The OSC page.
    const uint8_t wav = params.get(Page::osc, 0);
    const uint8_t os2 = params.get(Page::osc, 1);
    s.wave = wav < kPulseFirst ? Wave::saw : Wave::pulse;
    s.width = wav < kPulseFirst ? 0.5f : float(va_pulse_percent(wav)) * 0.01f;
    s.osc2 = va_osc2_mode(os2);
    float level1 = kLevelOsc1, level2 = 0.0f, accent2 = 0.0f;
    s.ratio1 = 1.0f;
    s.ratio2 = 1.0f;
    if (s.osc2 == VaOsc2::detune) {
        const float half = va_osc2_detune_cents(os2) * 0.005f;   // semitones each way
        s.ratio1 = semitones(-half);
        s.ratio2 = semitones(half);
        level1 = kLevelDetune;
        level2 = kLevelDetune;
    } else if (s.osc2 == VaOsc2::sync) {
        s.ratio2 = semitones(va_osc2_sync_semitones(os2));
        level1 = kLevelSync1;
        level2 = kLevelSync2;
        accent2 = kLevelSyncAccent;
    }
    const float level_sub = kLevelSubMax * params.unit(Page::osc, 2);

    s.decay_s = kDecayLowS * exp2(kDecayOctaves * params.unit(8));
    s.env_semis = params.unit(9) * kEnvSemitones;

    const float level = params.unit(11);
    s.out_gain = kHeadroom * level * level * (1.0f - kDriveLevelTrim * drv * drv);

    // The ASR page. Each 0 takes the old constant itself rather than the curve's value at 0, so
    // a kit from before the page renders bit-identically (the table above).
    const uint8_t atk = params.get(Page::asr, 0);
    const uint8_t rel = params.get(Page::asr, 2);
    s.attack_s = va_attack_seconds(atk);
    s.release_s = va_release_seconds(rel);
    s.sustain = params.unit(Page::asr, 1);
    s.filter_sustain = params.unit(Page::asr, 3);
    const float attack_accent = atk == 0 ? kAttackAccentS : s.attack_s * kAccentAttackRatio;
    const float filter_attack = atk == 0 ? kFilterAttackS : s.attack_s;
    const float filter_release = rel == 0 ? kFilterReleaseS : s.release_s * kFilterReleaseRatio;

    // Accent has shapes of its own rather than scaling a shape, because an envelope shape is a
    // set of one-pole coefficients: there is nothing linear to scale.
    s.amp_shape = EnvelopeShape::make(s.attack_s, s.decay_s, s.sustain, s.release_s, rate);
    s.amp_shape_accent = EnvelopeShape::make(attack_accent, s.decay_s * kAccentDecayRatio,
                                             s.sustain, s.release_s, rate);
    s.fenv_shape = EnvelopeShape::make(filter_attack, s.decay_s * kFilterDecayRatio,
                                       s.filter_sustain, filter_release, control_rate);
    s.fenv_shape_accent =
        EnvelopeShape::make(filter_attack, s.decay_s * kFilterDecayRatio * kAccentFilterRatio,
                            s.filter_sustain, filter_release, control_rate);

    s.level_osc1 = level1 * makeup;
    s.level_osc2 = level2 * makeup;
    s.level_sub = level_sub * makeup;
    s.level_noise = kLevelNoise * makeup;
    s.accent_osc2 = accent2 * makeup;
    s.accent_noise = kLevelNoiseAccent * makeup;
    return s;
}

// --- the voice ------------------------------------------------------------

void VaVoice::reset_phases() {
    osc1_.reset();
    osc2_.reset();
    sub_.reset();
    sync_carry_ = 0.0f;
    svf_a_.reset();
    svf_b_.reset();
}

void VaVoice::kill() {
    amp_.kill();
    fenv_.kill();
    svf_a_.reset();
    svf_b_.reset();
    sync_carry_ = 0.0f;
    gain_ = 0.0f;
    choke_left_ = 0;
}

void VaVoice::reset() {
    reset_phases();
    kill();
    pitch_ = 60.0f;
    target_ = 60.0f;
    velocity_ = 1.0f;
    accent_ = false;
    slide_ = false;
}

void VaVoice::set_note(float velocity, bool accent, bool slide) {
    velocity_ = velocity;
    accent_ = accent;
    slide_ = slide;
}

void VaVoice::gate_on() {
    choke_left_ = 0;       // a choked voice taken for a new note is a new note, not a fade
    amp_.gate_on();        // from the level it is at: resetting it under a note is a click
    fenv_.gate_on(true);   // the sweep always starts from closed, however loud the old note was
}

void VaVoice::run(const VaSettings &s, float *out, int n, float *work, float *env,
                  VaFilterCache *shared) {
    if (!sounding()) return;

    // --- one control step --------------------------------------------------------------
    // Everything transcendental in the voice is here, at a sixteenth of the sample rate: the
    // pitch, the filter envelope and one set of SVF coefficients. Per sample this would be the
    // costliest thing in the file by a wide margin.
    const float coef = slide_ && s.slide_coef < s.glide_coef ? s.slide_coef : s.glide_coef;
    pitch_ += coef * (target_ - pitch_);

    const float fenv = fenv_.step(accent_ ? s.fenv_shape_accent : s.fenv_shape);

    // The oscillators' increments, recomputed only when the pitch has moved: a semitones() and
    // two reciprocals a control step per voice, for a value that is constant for the whole of a
    // held note. Same inputs, same arithmetic, so the cached bits are the computed ones.
    if (pitch_ != osc_pitch_ || s.inv_rate != osc_inv_rate_ || s.ratio1 != osc_ratio1_ ||
        s.ratio2 != osc_ratio2_) {
        osc_pitch_ = pitch_;
        osc_inv_rate_ = s.inv_rate;
        osc_ratio1_ = s.ratio1;
        osc_ratio2_ = s.ratio2;
        const float hz = 440.0f * semitones(pitch_ - 69.0f);
        dt0_ = clamp(hz * s.inv_rate, 1e-5f, 0.45f);
        inv0_ = reciprocal(dt0_);
        if (s.ratio1 == 1.0f) {
            dt1_ = dt0_;
            inv1_ = inv0_;
        } else {
            dt1_ = clamp(dt0_ * s.ratio1, 1e-5f, 0.45f);
            inv1_ = reciprocal(dt1_);
        }
        dt2_ = clamp(dt0_ * s.ratio2, 1e-5f, 0.45f);
        inv2_ = reciprocal(dt2_);
    }
    const float dt1 = dt1_;
    const float inv1 = inv1_;
    const float dt2 = dt2_;
    const float inv2 = inv2_;

    // Accent opens the filter two ways: the base cutoff lifts, and the sweep above it reaches
    // further. No key tracking, deliberately -- the machine this follows has a fixed-frequency
    // filter, and a bass line wants the same squelch at every note of a pattern.
    //
    // That is also what lets `shared` work: nothing below depends on the NOTE, so the voices of
    // a chord -- whose filter envelopes run bit-identically, being gated together with the same
    // shapes -- all arrive at the same `g` and `k`. The first of them computes the `semitones`
    // and the `tan_pi`; the rest read them back (va.hpp, VaFilterCache). A hit is bit-exact by
    // construction: same inputs, same outputs.
    float g;
    float k;
    const int hit = shared != nullptr ? shared->find(fenv, accent_) : -1;
    if (hit >= 0) {
        g = shared->entries[hit].g;
        k = shared->entries[hit].k;
    } else {
        float semis = s.env_semis * fenv;
        if (accent_) semis = semis * kAccentEnvScale + kAccentCutSemis;
        const float cutoff = clamp(s.cutoff_hz * semitones(semis), 20.0f, 0.45f * s.rate);
        g = tan_pi(cutoff * s.inv_rate);
        // Not more resonance: with RES's new range that rang the body of an accented note on
        // and pushed its weight back, where an accent wants it forward (tests/test_va.cpp).
        k = s.k_res;
        if (shared != nullptr) shared->add(fenv, accent_, g, k);
    }

    // Not cached: `from_g` is one reciprocal and three multiplies, which is not worth a lookup,
    // and leaving it out keeps an entry to one key and two values.
    // The resonant stage's coefficients. k may be just below zero at RES's top, so 1 + g(g + k)
    // is floored well clear of zero.
    //
    // The resonance limiter: RES's top takes k just below zero, where the filter would ring up
    // without bound. Instead the resonant stage's own level, read from its two integrator
    // states at the end of the last slice (band and low-pass are in quadrature at the resonant
    // frequency, so s1^2 + s2^2 is its amplitude squared whatever the phase), adds damping
    // above kResLimit^2. So the filter self-oscillates at that level, and a loud resonant peak
    // compresses like an analogue one, at the cost of two multiplies a slice rather than a
    // nonlinearity in the per-sample recurrence (docs/BUDGET.md, "VA sound pass").
    const enaga_svf &st = svf_b_.state_;
    const float level = st.s1 * st.s1 + st.s2 * st.s2;
    const float over = level - kResLimitSq;
    const float k_live = over > 0.0f ? k + kResLimitDamp * over : k;
    const SvfCoefficients c2 = SvfCoefficients::from_g(g, k_live);

    // --- the slice ---------------------------------------------------------------------
    // OSC 2 is rendered first, because sync reads OSC 1's phase without moving it on: it has to
    // see the phase this slice starts from.
    const float level2 = accent_ ? s.level_osc2 + s.accent_osc2 : s.level_osc2;
    const float noise = accent_ ? s.level_noise + s.accent_noise : s.level_noise;
    enaga_clear(work, n);
    if (s.osc2 == VaOsc2::sync) {
        osc2_.sync_add(work, n, osc1_, dt1, inv1, sync_carry_, s.wave, dt2, inv2, level2, s.width);
    }
    osc1_.render_add(work, n, s.wave, dt1, inv1, s.level_osc1, s.width);
    if (s.osc2 == VaOsc2::detune) osc2_.render_add(work, n, s.wave, dt2, inv2, level2, s.width);
    if (s.level_sub > 0.0f) sub_.render_add(work, n, Wave::triangle, 0.5f * dt0_, 2.0f * inv0_, s.level_sub);
    rng_.add_noise(work, n, noise);

    float target = s.out_gain * (kVelocityFloor + (1.0f - kVelocityFloor) * velocity_) *
                   (accent_ ? kAccentGain : 1.0f);
    // A choked voice ramps its output gain to zero over kChokeFrames instead (choke(), below).
    if (choke_left_ > 0) {
        const float lowered = gain_ - choke_step_ * float(n);
        target = choke_left_ <= n || lowered < 0.0f ? 0.0f : lowered;
    }

    // The whole tail in ONE pass: the soft clip, the amplitude envelope and the ramped add.
    //
    // The soft clip sits AFTER the filter, which is what bounds the voice to +-1 however far
    // resonance pushes the peak, and is the right place for the dirt a driven bass wants. It
    // used to be `enaga_soft_clip_buffer`, then `enaga_multiply`, then a `mix_ramp` written out
    // in this file -- three passes over the slice with `work` round-tripped through memory. This
    // file's header has named the fusion as the thing to do since M2, and docs/BUDGET.md's
    // measurement is why it is worth doing: VA POLY runs this tail six times a block, four
    // slices each, and 82% of its 4.3 points per voice is the per-sample loop.
    //
    // Bit-exact, deliberately: same expressions, same order, same associations (dsp/valoops.h).
    if (n != ramp_n_) {
        ramp_n_ = n;
        inv_n_ = reciprocal(float(n));
    }
    // LP24's first stage only: the 12 dB modes never read c1, so they skip its reciprocal.
    const SvfCoefficients c1 =
        s.mode == VaMode::lp24 ? SvfCoefficients::from_g(g, kStageOneK) : c2;
#if STRATA_VA_FUSED
    // The filter, the amplitude envelope and the tail in ONE loop (valoops.h): three recurrences
    // that each kept this core's units mostly idle, overlapped. Bit-exact against the passes
    // below, which tests/test_va.cpp checks in every mode and envelope stage.
    strata_va_back(out, work, n, int(s.mode), &svf_a_.state_, &c1, &svf_b_.state_, &c2,
                   s.pre_drive, s.inv_pre_drive, &amp_.state_,
                   accent_ ? &s.amp_shape_accent : &s.amp_shape, s.drive, gain_,
                   (target - gain_) * inv_n_);
    (void)env;
#else
    if (s.pre_drive > 0.0f) {
        strata_va_drive_filter(work, n, int(s.mode), &svf_a_.state_, &c1, &svf_b_.state_, &c2,
                               s.pre_drive, s.inv_pre_drive);
    } else {
        switch (s.mode) {
            case VaMode::lp24:
                Svf::cascade(svf_a_, c1, svf_b_, c2, work, work, n);
                break;
            case VaMode::lp12:
                svf_b_.process(work, work, n, c2, Svf::low_pass);
                break;
            case VaMode::bp12:
                svf_b_.process(work, work, n, c2, Svf::band_pass);
                break;
            case VaMode::hp12:
                svf_b_.process(work, work, n, c2, Svf::high_pass);
                break;
        }
    }
    amp_.render(env, n, accent_ ? s.amp_shape_accent : s.amp_shape);
    strata_va_tail(out, work, env, n, s.drive, gain_, (target - gain_) * inv_n_);
#endif
    gain_ = target;

    if (choke_left_ > 0) {
        choke_left_ -= n;
        if (choke_left_ <= 0) {
            choke_left_ = 0;
            kill();            // the ramp has reached zero: nothing is cut that can be heard
        }
    }
}

// The shared voice budget's way out (app/engine.cpp, make_room_for_poly_voice): a fade of the
// output gain to zero over kChokeFrames, then a kill. A kill on its own would put a step the
// size of the voice's level into the mix; a release would keep the voice -- and its cost --
// running for another 60 ms, which is the cost the budget exists to bound. 64 frames is 1.3 ms:
// short enough that the budget is back within a block, long enough not to click.
void VaVoice::choke() {
    if (!sounding() || choke_left_ > 0) return;
    choke_left_ = kChokeFrames;
    choke_step_ = gain_ * (1.0f / float(kChokeFrames));
}

// --- the machine ----------------------------------------------------------

void VaMono::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    control_rate_ = rate_ * (1.0f / float(VaVoice::kControl));

    voice_.reset();
    first_note_ = true;
    pending_ = false;
    wait_ = 0;
    active_ = false;

    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void VaMono::prepare(const TrackParams &params) {
    settings_ = VaSettings::make(params, rate_, inv_rate_);

    // GLD is this machine's own: VaSettings::make leaves slot 10 alone because VA POLY spends
    // it on DET. A slid note never jumps, so its coefficient has a floor of its own.
    glide_s_ = kGlideScaleS * (exp2(kGlideOctaves * params.unit(10)) - 1.0f);
    settings_.glide_coef = to_target(glide_s_, control_rate_);
    settings_.slide_coef = to_target(kSlideFloorS, control_rate_);
}

void VaMono::trigger(const Trig &trig) {
    // Monophonic: one voice, so this always takes it. The note starts on trig.frame, which is
    // what makes micro-timing and ratchets land on an exact sample. A trig arriving while
    // another is still waiting for its frame REPLACES it: with one voice the earlier note would
    // have nothing to sound through, and at any sane tempo two ratchet hits are hundreds of
    // frames apart, so this only bites at the far end of the ratchet range.
    next_ = trig;
    wait_ = int(trig.frame);
    pending_ = true;
    release_on_start_ = false;
    active_ = true;
    // A slid note is the previous one carrying on, so the previous one's gate must not let it go
    // before the slide arrives: its own LEN takes over from its start.
    if (trig.slide) gate_ = -1;
}

void VaMono::release(uint8_t note) {
    // `next_` is the note sounding once it has started, and the one waiting before that. A
    // different note has taken the voice since (legato playing): leave it alone.
    if (next_.note != note) return;
    if (pending_) release_on_start_ = true;
    else voice_.gate_off();
    gate_ = -1;
    one_shot_ = false;
}

void VaMono::release_all() {
    // STOP once: let the note go with its release tail. Both envelopes, so the filter closes as
    // the level falls rather than leaving a bright ghost behind.
    voice_.gate_off();
    gate_ = -1;
    one_shot_ = false;
}

void VaMono::kill_all() {
    pending_ = false;
    release_on_start_ = false;
    gate_ = -1;
    one_shot_ = false;
    wait_ = 0;
    voice_.kill();
    active_ = false;
}

void VaMono::render_add(float *out, int frames) {
    if (!active_) return;   // nothing sounding and nothing pending: an idle voice costs a test
    int i = 0;
    while (i < frames) {
        if (pending_ && wait_ <= 0) {
            start();        // sample-accurate: the note begins on trig.frame
            continue;
        }
        int n = frames - i;
        if (n > VaVoice::kControl) n = VaVoice::kControl;
        if (pending_ && wait_ < n) n = wait_;    // cut the slice short at the trig's frame
        if (gate_ > 0 && gate_ < n) n = gate_;   // and at the gate's end: released on its frame
        // A one-shot whose SUS was raised under it: let it go now, or it would never end.
        if (one_shot_ && settings_.sustain > 0.0f) {
            voice_.gate_off();
            one_shot_ = false;
        }
        voice_.run(settings_, out + i, n, work_, env_);
        if (pending_) wait_ -= n;
        if (gate_ > 0) {
            gate_ -= n;
            if (gate_ <= 0) {
                voice_.gate_off();               // LEN is over: into the release, no click
                gate_ = -1;
            }
        }
        i += n;
    }
    active_ = pending_ || voice_.sounding();
}

const char *VaMono::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "MOD", "CUT", "RES",      // SRC: the voice and its filter
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "DEC", "ENV", "GLD", "LVL",      // AMP: the envelopes and the output
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// SLIDE and GLIDE, which are not the same thing.
//
// GLD is portamento: every note's pitch slews to the new note over GLD seconds, and GLD 0
// arrives at once. SLIDE is per step (docs/SPEC.md: MONO tracks only) and does two things more:
// it suppresses the envelope retrigger, so the note bends into the next pitch instead of
// starting again -- the 303 behaviour that makes an acid line move -- and it forces the glide
// to take at least kSlideFloorS, so a slid step is always audibly a slide even with GLD at 0.
//
// `Trig::slide` is read as "this step slides IN from the one before it", which is also what the
// flag has to mean for a slid step to be the thing whose envelope does not restart. A slide
// arriving when nothing is sounding has nothing to slide from, so it starts normally.
void VaMono::start() {
    const Trig t = next_;
    pending_ = false;
    wait_ = 0;

    const bool was_sounding = voice_.sounding();
    const float target = clamp(float(t.note) + settings_.tune, 0.0f, 132.0f);
    voice_.set_note(float(t.velocity) * (1.0f / 127.0f), t.accent, t.slide);
    voice_.set_target(target);

    // Nothing to glide from on the first note; and with GLD at 0 a plain note arrives at once.
    // A slid note never jumps: slide_coef is below 1 whatever GLD says.
    if (first_note_ || (!t.slide && settings_.glide_coef >= 1.0f)) voice_.set_pitch(target);
    first_note_ = false;

    // This note's own LEN, counted from now (va.hpp, gate_). Only with SUS up: at SUS 0 the
    // envelope ends the note by itself, as it always has. Long enough to outlast any pattern
    // (vapoly.cpp's kGateMax), so a held LEN rings across the pattern's end like a tie.
    gate_ = settings_.sustain > 0.0f && t.gate_frames > 0
                ? int(t.gate_frames > kGateMax ? kGateMax : t.gate_frames)
                : -1;
    one_shot_ = gate_ < 0 && !t.held;

    // A slid note bends the sounding one into its pitch: the envelopes and the phases carry on
    // untouched. Not one already let go, though -- a voice in its release tail has nothing held
    // to carry on, so the slide starts a note (gliding into it) rather than bending a fade.
    // At SUS 0 a release tail is only ever the 60 ms after a key-up, and a slide has always
    // bent it, so that stays as it was.
    if (t.slide && was_sounding && (settings_.sustain <= 0.0f || !voice_.released())) return;

    // A voice taken while still sounding keeps its oscillator phases and its filter state, and
    // its amplitude envelope goes on from the level it is at: resetting any of them under a
    // sounding note puts a step in the output, which is a click.
    if (!was_sounding) voice_.reset_phases();
    voice_.gate_on();
    if (release_on_start_) {
        voice_.gate_off();
        release_on_start_ = false;
        gate_ = -1;
        one_shot_ = false;
    }
}

}  // namespace strata
