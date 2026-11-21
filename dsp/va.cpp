// VA MONO, the bass voice. va.hpp has the topology and why the filter is on the SRC page.
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
//                        PLUS a band-limited restart wherever OSC 1's           STAGE
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
//                        four exp2 for the knob curves.
//
// So about 65 flops/sample with LP24. Against Enaga's measured VA voice (1,868 cycles per block
// = 29 cycles/sample for roughly 70-80 flops/sample, so ~2.5 flops per cycle on this core) that
// is **about 1,660 cycles per voice per block, ~0.89x a full VA voice, 0.33% of the block** --
// inside the target, which it should be: the reference carries three ADSRs, two LFOs and an
// eight-slot modulation matrix that this voice does not.
//
// All of the per-sample work except the output ramp is already in Enaga's C block functions,
// which cl6x compiles and pipelines, so CLAUDE.md's *.ti.c rule bites in one place here:
// `mix_ramp` -- which is already written, in fact: `strata_mix_ramp` in dsp/drumloops.h is this
// loop exactly, shared by KICK, SNARE and HAT. It is not called from here only because
// drumloops.* belongs to another agent's file set. The worthwhile extraction is bigger than
// that one loop anyway: a single fused pass doing `soft clip -> multiply by the amplitude
// envelope -> ramped add into the bus` would replace three passes over the slice with one and
// save two buffer round-trips. That is the candidate to hand the *.ti.c agent.
#include "va.hpp"

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

// --- the voice's fixed proportions ---------------------------------------------------------
//
// Eight knobs is a hard budget (dsp/track.hpp: SRC plus AMP), and docs/SPEC.md's own guidance
// for this machine is that the filter and the accent matter more than oscillator count. So the
// oscillator mix is not a knob: these are the levels, chosen once, for a techno bass.
//
// They are set so the sum runs the post-filter soft clip slightly into its curve -- a typical
// note peaks near 0.8 there, which is about 10% of gentle compression on the peaks, and with
// resonance up it clips properly. That is deliberate: a driven bass is the sound, and it is
// also what keeps the voice bounded (see the clip in run()).
constexpr float kLevelOsc1 = 0.60f;        // saw: the backbone
constexpr float kLevelOsc2 = 0.14f;        // the synced pulse's bite, small until accented
constexpr float kLevelOsc2Accent = 0.34f;  // what an accent adds to it
constexpr float kLevelSub = 0.40f;         // triangle, an octave down: the weight
constexpr float kLevelNoise = 0.016f;      // a trace of grit
constexpr float kLevelNoiseAccent = 0.050f;

// OSC 2 sits +19 semitones above OSC 1 and is hard synced to it. A whole number of octaves
// would be pointless (the restart would fall where its own cycle already starts, so sync would
// do nothing); +19 is an octave and a fifth, which gives a strong, vocal formant.
constexpr float kOsc2Ratio = 2.99661f;     // 2^(19/12)

// LP24's first stage runs at a low Q so only the second stage resonates -- the reference's
// arrangement, and the reason a 4-pole sweep stays controllable at high RES.
constexpr float kStageOneK = 1.85f;

// --- the knob curves ------------------------------------------------------------------------
constexpr float kTuneStep = 24.0f / 64.0f;     // +-24 semitones, exactly 0 at knob 64
constexpr float kCutoffLowHz = 35.0f;
constexpr float kCutoffOctaves = 8.4214f;      // log2(12000 / 35)
constexpr float kResMax12 = 0.93f;             // one pole: squelchy, short of self-oscillation
constexpr float kResMax24 = 0.84f;             // it compounds through the cascade
constexpr float kEnvSemitones = 66.0f;         // 5.5 octaves of cutoff at ENV 127
constexpr float kDecayLowS = 0.015f;
constexpr float kDecayOctaves = 7.3219f;       // log2(2.4 / 0.015): 15 ms to 2.4 s
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

// --- accent ---------------------------------------------------------------------------------
constexpr float kAccentGain = 1.40f;           // louder, but that is the least of it
constexpr float kAccentCutSemis = 10.0f;       // the base cutoff opens
constexpr float kAccentEnvScale = 1.5f;        // and the sweep above it reaches further
constexpr float kAccentK = 0.08f;              // a touch more resonance (k = 1 / Q)
constexpr float kMinK = 0.13f;                 // the stability floor the accent cannot cross
constexpr float kVelocityFloor = 0.25f;        // velocity 1 is a quarter the level, not silence

// A one-pole coefficient that reaches its target to within 1% in `seconds`, stepped at `rate`.
// The same arithmetic EnvelopeShape::make uses, which is why glide and the envelopes agree
// about what a time means. Returns 1 (arrive at once) for anything under one step.
float to_target(float seconds, float rate) {
    const float steps = seconds * rate;
    return steps < 1.0f ? 1.0f : 1.0f - exp(-EnvelopeShape::kSpan * reciprocal(steps));
}

// out += gain * in, with gain walking by step each sample: the output ramp, so a change of LVL,
// of velocity or of accent never puts a step in the bus. Two flops a sample, and the one
// hand-written loop in this file (see the *.ti.c note at the top).
void mix_ramp(float *out, const float *in, int n, float gain, float step) {
    for (int i = 0; i < n; i++) {
        out[i] += gain * in[i];
        gain += step;
    }
}

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
//   3  RES  resonance, 0 to 0.93 as Enaga's SVF counts it (0.84 on LP24, because resonance
//           compounds through a cascade). It also drops the oscillator level by up to 37% and
//           raises the post-filter soft clip's drive by up to 70%, which is how the squelch
//           gets dirty rather than just loud, and how it stays bounded.
// AMP -- the envelopes and the output. Slot 0 is the decay and slot 3 the level on every STRATA
// machine, which this one keeps.
//   8  DEC  decay, 15 ms to 2.4 s exponential (about 190 ms at 64). ONE knob for both
//           envelopes, 303-style: the amplitude falls over DEC and the filter over 0.55 x DEC,
//           so the sweep always lands at the front of the note. The Machine interface has no
//           note-off, so this is also the note's length.
//   9  ENV  how much of the filter envelope reaches the cutoff, 0 to +66 semitones (5.5
//           octaves). At 0 the filter is static -- a dub bass; wide open with RES up it is the
//           acid squelch. The only knob an accent multiplies rather than replaces.
//  10  GLD  glide time, 0 to 0.5 s as 0.004 * (2^(7u) - 1), so 0 really is 0 and 64 is a
//           usable 42 ms. Portamento on every note; a slid step glides too, but never faster
//           than 55 ms (see start()).
//  11  LVL  output level, square law. Square gives usable control over the quiet end.

void VaMono::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    control_rate_ = rate_ * (1.0f / float(kControl));

    osc1_.reset();
    osc2_.reset();
    sub_.reset();
    svf_a_.reset();
    svf_b_.reset();
    amp_.kill();
    fenv_.kill();
    sync_carry_ = 0.0f;
    gain_ = 0.0f;
    pitch_ = 60.0f;
    target_ = 60.0f;
    velocity_ = 1.0f;
    accent_ = false;
    slide_ = false;
    first_note_ = true;
    pending_ = false;
    wait_ = 0;
    active_ = false;

    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

VaMono::Mode VaMono::mode_from(uint8_t value) {
    // Three compares, no division: the C674x has no integer divide instruction at all, so even
    // at control rate a `/` is a libgcc call rather than one cycle.
    if (value < 64) return Mode::lp24;
    if (value < 96) return Mode::lp12;
    if (value < 112) return Mode::bp12;
    return Mode::hp12;
}

void VaMono::prepare(const TrackParams &params) {
    tune_ = (float(params.get(0)) - 64.0f) * kTuneStep;
    mode_ = mode_from(params.get(1));
    cutoff_hz_ = kCutoffLowHz * exp2(kCutoffOctaves * params.unit(2));

    const float resonance = params.unit(3) * (mode_ == Mode::lp24 ? kResMax24 : kResMax12);
    k_res_ = 2.0f - 1.98f * resonance;             // the same k SvfCoefficients::make builds
    const float makeup = 1.0f - 0.4f * resonance;  // keep the filter's input out of the clip
    drive_ = 1.0f + 0.7f * resonance;              // and let the clip do the dirt instead

    decay_s_ = kDecayLowS * exp2(kDecayOctaves * params.unit(8));
    env_semis_ = params.unit(9) * kEnvSemitones;
    glide_s_ = kGlideScaleS * (exp2(kGlideOctaves * params.unit(10)) - 1.0f);
    glide_coef_ = to_target(glide_s_, control_rate_);
    slide_coef_ = to_target(kSlideFloorS, control_rate_);

    const float level = params.unit(11);
    out_gain_ = kHeadroom * level * level;

    // Accent has shapes of its own rather than scaling a shape, because an envelope shape is a
    // set of one-pole coefficients: there is nothing linear to scale.
    amp_shape_ = EnvelopeShape::make(kAttackS, decay_s_, 0.0f, kReleaseS, rate_);
    amp_shape_accent_ = EnvelopeShape::make(kAttackAccentS, decay_s_ * kAccentDecayRatio, 0.0f,
                                            kReleaseS, rate_);
    fenv_shape_ = EnvelopeShape::make(kFilterAttackS, decay_s_ * kFilterDecayRatio, 0.0f,
                                      kFilterReleaseS, control_rate_);
    fenv_shape_accent_ =
        EnvelopeShape::make(kFilterAttackS, decay_s_ * kFilterDecayRatio * kAccentFilterRatio,
                            0.0f, kFilterReleaseS, control_rate_);

    level_osc1_ = kLevelOsc1 * makeup;
    level_osc2_ = kLevelOsc2 * makeup;
    level_sub_ = kLevelSub * makeup;
    level_noise_ = kLevelNoise * makeup;
    accent_osc2_ = kLevelOsc2Accent * makeup;
    accent_noise_ = kLevelNoiseAccent * makeup;
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
    active_ = true;
}

void VaMono::release_all() {
    // STOP once: let the note go with its release tail. Both envelopes, so the filter closes as
    // the level falls rather than leaving a bright ghost behind.
    amp_.gate_off();
    fenv_.gate_off();
}

void VaMono::kill_all() {
    pending_ = false;
    wait_ = 0;
    amp_.kill();
    fenv_.kill();
    svf_a_.reset();
    svf_b_.reset();
    sync_carry_ = 0.0f;
    gain_ = 0.0f;
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
        if (n > kControl) n = kControl;
        if (pending_ && wait_ < n) n = wait_;    // cut the slice short at the trig's frame
        run(out + i, n);
        if (pending_) wait_ -= n;
        i += n;
    }
    active_ = pending_ || sounding();
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

// --- the voice ------------------------------------------------------------

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

    const bool was_sounding = sounding();
    slide_ = t.slide;
    accent_ = t.accent;
    velocity_ = float(t.velocity) * (1.0f / 127.0f);
    target_ = clamp(float(t.note) + tune_, 0.0f, 132.0f);

    // Nothing to glide from on the first note; and with GLD at 0 a plain note arrives at once.
    // A slid note never jumps: slide_coef_ is below 1 whatever GLD says.
    if (first_note_ || (!slide_ && glide_coef_ >= 1.0f)) pitch_ = target_;
    first_note_ = false;

    if (slide_ && was_sounding) return;    // the envelopes and the phases carry on untouched

    // A voice taken while still sounding keeps its oscillator phases and its filter state, and
    // its amplitude envelope goes on from the level it is at: resetting any of them under a
    // sounding note puts a step in the output, which is a click.
    if (!was_sounding) {
        osc1_.reset();
        osc2_.reset();
        sub_.reset();
        sync_carry_ = 0.0f;
        svf_a_.reset();
        svf_b_.reset();
    }
    amp_.gate_on();
    fenv_.gate_on(true);   // the sweep always starts from closed, however loud the old note was
}

void VaMono::run(float *out, int n) {
    if (!sounding()) return;

    // --- one control step --------------------------------------------------------------
    // Everything transcendental in the voice is here, at a sixteenth of the sample rate: the
    // pitch, the filter envelope and one set of SVF coefficients. Per sample this would be the
    // costliest thing in the file by a wide margin.
    const float coef = slide_ && slide_coef_ < glide_coef_ ? slide_coef_ : glide_coef_;
    pitch_ += coef * (target_ - pitch_);

    const float fenv = fenv_.step(accent_ ? fenv_shape_accent_ : fenv_shape_);

    const float hz = 440.0f * semitones(pitch_ - 69.0f);
    const float dt1 = clamp(hz * inv_rate_, 1e-5f, 0.45f);
    const float inv1 = reciprocal(dt1);
    const float dt2 = clamp(dt1 * kOsc2Ratio, 1e-5f, 0.45f);
    const float inv2 = reciprocal(dt2);

    // Accent opens the filter two ways: the base cutoff lifts, and the sweep above it reaches
    // further. No key tracking, deliberately -- the machine this follows has a fixed-frequency
    // filter, and a bass line wants the same squelch at every note of a pattern.
    float semis = env_semis_ * fenv;
    if (accent_) semis = semis * kAccentEnvScale + kAccentCutSemis;
    const float cutoff = clamp(cutoff_hz_ * semitones(semis), 20.0f, 0.45f * rate_);
    const float g = tan_pi(cutoff * inv_rate_);
    const float k = accent_ ? clamp(k_res_ - kAccentK, kMinK, k_res_) : k_res_;
    const SvfCoefficients c2 = SvfCoefficients::from_g(g, k);

    // --- the slice ---------------------------------------------------------------------
    // OSC 2 is rendered first, because sync reads OSC 1's phase without moving it on: it has to
    // see the phase this slice starts from.
    const float level2 = accent_ ? level_osc2_ + accent_osc2_ : level_osc2_;
    const float noise = accent_ ? level_noise_ + accent_noise_ : level_noise_;
    enaga_clear(voice_, n);
    osc2_.sync_add(voice_, n, osc1_, dt1, inv1, sync_carry_, Wave::pulse, dt2, inv2, level2);
    osc1_.render_add(voice_, n, Wave::saw, dt1, inv1, level_osc1_);
    sub_.render_add(voice_, n, Wave::triangle, 0.5f * dt1, 2.0f * inv1, level_sub_);
    rng_.add_noise(voice_, n, noise);

    switch (mode_) {
        case Mode::lp24: {
            const SvfCoefficients c1 = SvfCoefficients::from_g(g, kStageOneK);
            Svf::cascade(svf_a_, c1, svf_b_, c2, voice_, voice_, n);
            break;
        }
        case Mode::lp12:
            svf_b_.process(voice_, voice_, n, c2, Svf::low_pass);
            break;
        case Mode::bp12:
            svf_b_.process(voice_, voice_, n, c2, Svf::band_pass);
            break;
        case Mode::hp12:
            svf_b_.process(voice_, voice_, n, c2, Svf::high_pass);
            break;
    }

    // The soft clip sits AFTER the filter, which is what bounds the voice to +-1 however far
    // resonance pushes the peak, and is the right place for the dirt a driven bass wants.
    enaga_soft_clip_buffer(voice_, n, drive_);

    amp_.render(amp_env_, n, accent_ ? amp_shape_accent_ : amp_shape_);
    enaga_multiply(voice_, amp_env_, n);

    const float target = out_gain_ * (kVelocityFloor + (1.0f - kVelocityFloor) * velocity_) *
                         (accent_ ? kAccentGain : 1.0f);
    mix_ramp(out, voice_, n, gain_, (target - gain_) * reciprocal(float(n)));
    gain_ = target;
}

}  // namespace strata
