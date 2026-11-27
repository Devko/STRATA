// HAT, after Mutable Instruments Plaits `hi_hat.h` with its SquareNoise source (MIT licensed,
// Copyright 2016 Emilie Gillet). See hat.hpp and docs/SOUND.md.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Two voices, so this is per voice per 64-frame block, counting multiplies and adds in the hot
// loops only. These are the figures the central measurement (docs/archive/BUDGET_MILESTONES.md, PLAN.md M0.5)
// should be compared against.
//
//   stage              expected to dominate                            flops/sample
//   band-pass          Svf::process, a cl6x block function: two        ~9    THE LARGEST STAGE,
//                      one-pole integrators and the mix.                     with the high-pass
//   high-pass          the same again, after the VCA, so the two       ~9
//                      cannot be a cascade: Q differs and the VCA
//                      sits between them.
//   VCA                three multiplies, the asymmetric select and     ~9
//                      soft_clip's cubic. One select, no branch.
//   six squares        six accumulator adds, six shifts and five      ~19   mostly INTEGER, so
//                      integer adds for the count, then the 0.33             it costs far less
//                      mix. This is the whole oscillator bank: no            than the float
//                      sine, no PolyBLEP, no table.                          figure suggests
//   clocked noise      one add, one unsigned compare, a xorshift on    ~4    ~1 when NOI = 0
//                      the wrap, and the crossfade.
//   envelope           Envelope::render, then enaga_multiply.           3
//
// So about 51 flops/sample, around a third of it integer. Against Enaga's measured VA voice
// (1,868 cycles per block = 29 cycles/sample for roughly 70-80 flops/sample, so ~2.5 flops per
// cycle on this core) that is **about 1,300 cycles per voice per block, ~0.7x a full VA voice**
// -- the cheapest of the three, and the only one at all close to the brief's "far simpler".
// Reading six squares off the top bit of a 32-bit accumulator is what buys that: six
// `Wave::pulse` oscillators would have been roughly 3x this whole machine, and the band-pass
// that follows makes their PolyBLEP correction pointless anyway. The two SVFs are Enaga's own
// C block functions, which cl6x pipelines, so CLAUDE.md's *.ti.c rule bit only on the square
// and VCA loops here -- and both now live in dsp/drumloops.ti.c, which build_app.py sends to
// cl6x while this file goes through GCC (docs/archive/BUDGET_MILESTONES.md, M2, has what that bought). The
// arithmetic moved unchanged, the clocked noise included: the RNG still advances only on a
// sample-and-hold clock edge, so the noise stream is the one this machine always had.
#include "hat.hpp"

#include "drumloops.h"
#include "filterloops.h"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::Svf;
using enaga::dsp::SvfCoefficients;
using enaga::dsp::clamp;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;
using enaga::dsp::tan_pi;

constexpr float kToOnePercent = 4.60517019f;   // ln(100), see kick.cpp
constexpr float kTurn = 4294967296.0f;         // 2^32, as enaga's own oscillators use

// A 32-bit phase increment from a normalised frequency, the way enaga_oscillator_add does it.
// Control rate, once per oscillator per block, so it stays here.
inline uint32_t increment(float normalised) {
    return uint32_t(int32_t(clamp(normalised, 0.0f, 0.499f) * kTurn));
}

// The square bank, the VCA and the declick ramp are strata_hat_squares, strata_hat_vca and
// strata_mix_ramp in drumloops.ti.c, where cl6x compiles them.
// Where each oscillator starts in its cycle: i / 6 of a turn, as a Q32 phase.
//
// Written out rather than computed, and that is a CLAUDE.md requirement rather than taste. This
// used to be `uint32_t((uint64_t(i) << 32) / uint64_t(kOscillators))` inside `start`, which runs
// in AUDIO context -- and a 64-bit divide on this toolchain does not merely cost cycles, it does
// not LINK: it drags in libgcc's ARM unwinder and the whole app fails to build. core/pcm.cpp
// writes twenty lines of shift-and-subtract for exactly that reason.
//
// GCC 13 at -O2 happened to peel the six-iteration loop and fold the whole thing to these six
// constants, so the rule was being kept by the compiler's goodwill and not by the code. The
// values are the fold, checked against it: i * 2^32 / 6.
constexpr uint32_t kStartPhase[Hat::kOscillators] = {
    0u, 715827882u, 1431655765u, 2147483648u, 2863311530u, 3579139413u,
};

static_assert(Hat::kOscillators == STRATA_HAT_OSCILLATORS,
              "drumloops.ti.c unrolls exactly this many accumulators");

}  // namespace

// --- parameters -----------------------------------------------------------
//
// SRC
//   0  TUN  f0 of the lowest square, 180-940 Hz exponential, with the reference's nominal 414 Hz
//           at knob 64. The trig note transposes from note 60.
//   1  TON  tone, 0-1: the band-pass cutoff 150 Hz * 2^(TON * 72/12), so 150 Hz to 9.6 kHz, with
//           Q = 3 + 3 * TON, and the high-pass after the VCA at the same cutoff with Q 0.5.
//   2  NOI  noisiness, 0-1: crossfades the squares towards clocked sample-and-hold noise running
//           at f0 * (16 + 16 * (1 - NOI)). An extra, not part of the 808 circuit.
//   3  DTN  detune spread on the reference's ratios, 0.25 at knob 0, exactly 1.0 at knob 64 and
//           1.75 at 127: near unison, the 808, or clangy and cymbal-like.
// AMP
//   8  DEC  decay, 0-1: a time constant of 7 ms to 0.9 s exponential. It also sets the envelope's
//           trigger level, (1.5 + 0.5 * (1 - DEC)), as in the reference.
//   9  CHO  choke tail, 2-120 ms exponential: how fast choke() takes the sound away.
//  10  CHK  choke group, 0 (none) to 4. Advisory; the app wires the choking, see choke().
//  11  LVL  output level, square law, 0-1.

void Hat::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    for (Voice &v : voices_) {
        v = Voice{};
        v.band.reset();
        v.high.reset();
    }
    active_ = false;
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Hat::prepare(const TrackParams &params) {
    // 180-940 Hz, with the exponent chosen so that knob 64 is exactly the reference's 414 Hz.
    base_hz_ = 180.0f * exp2(2.3844930f * params.unit(0));
    const float tone = params.unit(1);
    // The reference's 150 Hz * 2^(tone * 72/12), clamped as it clamps it.
    const float cutoff = clamp(150.0f * semitones(tone * 72.0f), 20.0f, 16000.0f);
    const float g = tan_pi(clamp(cutoff * inv_rate_, 0.0f, 0.49f));
    band_ = SvfCoefficients::from_g(g, reciprocal(3.0f + 3.0f * tone));   // k = 1/Q
    high_ = SvfCoefficients::from_g(g, 2.0f);                             // Q 0.5
    noisiness_ = params.unit(2);
    sample_hold_ratio_ = 16.0f + 16.0f * (1.0f - noisiness_);

    // Two segments so that knob 64 is exactly the reference's ratios, with a usable sweep each
    // side of it rather than a centre that is almost but not quite 1.
    const uint8_t detune = params.get(3);
    const float spread = detune <= 64 ? 0.25f + 0.75f * float(detune) * (1.0f / 64.0f)
                                      : 1.0f + 0.75f * float(detune - 64) * (1.0f / 63.0f);
    for (int i = 0; i < kOscillators; i++) ratio_[i] = 1.0f + spread * (kRatio[i] - 1.0f);

    decay_ = params.unit(8);
    const float tau = 0.007f * exp2(7.0063466f * decay_);             // log2(0.9/0.007)
    amp_shape_ = EnvelopeShape::make(0.0003f, tau * kToOnePercent, 0.0f, 0.001f, rate_);
    // The faster stage of the reference's two-stage envelope, which is what a choke engages.
    const float choke_tau = 0.002f * exp2(5.9068906f * params.unit(9));   // 2-120 ms
    choke_shape_ = EnvelopeShape::make(0.0003f, choke_tau * kToOnePercent, 0.0f, 0.001f, rate_);
    choke_group_ = (int(params.get(10)) * 5) / 128;
    const float level = params.unit(11);
    out_gain_ = level * level * kMakeup;
}

float Hat::detune_ratio(int index) const {
    return index >= 0 && index < kOscillators ? ratio_[index] : 0.0f;
}

void Hat::trigger(const Trig &trig) {
    // A closed hat cuts the open one it is wired to. The app owns that wiring; this only fires
    // it, and it happens before the voice is picked so the two hats never fight over a block.
    if (choke_target_ != nullptr) choke_target_->choke();

    int pick = -1;
    for (int i = 0; i < kVoices; i++) {
        if (!voices_[i].busy()) {
            pick = i;
            break;
        }
    }
    if (pick < 0) pick = voices_[0].amp.level() <= voices_[1].amp.level() ? 0 : 1;

    Voice &v = voices_[pick];
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

void Hat::choke() {
    for (Voice &v : voices_) {
        v.pending = false;   // a hit that has not started yet is choked before it is heard
        v.wait = 0;
        if (v.sounding()) v.choked = true;
    }
}

void Hat::release_all() {
    // One-shot percussion: the decay is the release, so an open hat rings out on its own.
    // choke() and kill_all() are the two ways to cut it short.
}

void Hat::kill_all() {
    for (Voice &v : voices_) {
        v.pending = false;
        v.wait = 0;
        silence(v);
    }
    active_ = false;
}

void Hat::render_add(float *out, int frames) {
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

const char *Hat::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "TON", "NOI", "DTN",      // SRC
        "---", "---", "---", "---",      // FLTR
        "DEC", "CHO", "CHK", "LVL",      // AMP
        "---", "---", "---", "---",      // FX
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- voices ---------------------------------------------------------------

void Hat::start(Voice &v) {
    const Trig &t = v.next;
    v.pending = false;
    v.wait = 0;
    v.fade = 0;
    v.gain = 1.0f;
    v.choked = false;
    v.note_ratio = clamp(semitones(float(t.note) - 60.0f), 0.05f, 8.0f);
    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    const float accent = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    // The reference's trigger level. It feeds the asymmetric VCA, so a harder hit saturates it
    // further: an accent brightens as well as lifts.
    v.trigger_level = (1.5f + 0.5f * (1.0f - decay_)) * (0.3f + 0.7f * accent);
    // Spread around the cycle rather than all starting together, so the attack is the filter's
    // step response and not six squares flipping on the same sample.
    for (int i = 0; i < kOscillators; i++) v.phase[i] = kStartPhase[i];
    v.sample_hold_phase = 0;
    v.sample_hold = 0.0f;
    v.band.reset();
    v.high.reset();
    v.amp.gate_on(true);
}

void Hat::silence(Voice &v) {
    v.amp.kill();
    v.fade = 0;
    v.gain = 1.0f;
    v.choked = false;
}

void Hat::render_voice(Voice &v, float *out, int n) {
    int i = 0;
    while (i < n) {
        if (v.pending && v.wait == 0) {
            start(v);
            continue;
        }
        int k = n - i;
        if (v.pending && v.wait < k) k = v.wait;
        if (v.fade > 0 && v.fade < k) k = v.fade;
        run(v, out + i, k);
        if (v.pending) v.wait -= k;
        if (v.fade > 0) {
            v.fade -= k;
            if (v.fade == 0) silence(v);
        }
        i += k;
    }
}

void Hat::run(Voice &v, float *out, int n) {
    if (!v.sounding()) return;

    // Control rate, once per voice per block: the note moves f0 and everything here follows it.
    const float f0 = base_hz_ * v.note_ratio;
    uint32_t step[kOscillators];
    for (int j = 0; j < kOscillators; j++) step[j] = increment(f0 * ratio_[j] * inv_rate_);
    const uint32_t sample_hold_step = increment(f0 * sample_hold_ratio_ * inv_rate_);

    // The metallic source: six squares read off the top bit of a 32-bit accumulator, mixed as
    // 0.33 * (how many are high) - 1, then crossfaded towards clocked noise -- one cl6x-built
    // pass over the block, and the RNG stream is this machine's own (drumloops.ti.c).
    strata_hat_squares(mix_, n, v.phase, step, sample_hold_step, &v.sample_hold_phase,
                       &v.sample_hold, rng_.state(), noisiness_);

    // The band-pass, then the VCA -- the envelope times the trigger level, then asymmetric and
    // soft clipped, which is where the trigger level turns into brightness rather than only
    // level -- then the high-pass, in one cl6x-built loop (strata_hat_back): the two filters'
    // recurrences overlap there. The envelope does not hear the signal, so it renders first.
    v.amp.render(env_, n, v.choked ? choke_shape_ : amp_shape_);
    strata_hat_back(mix_, env_, n, &v.band.state_, &band_, &v.high.state_, &high_, v.trigger_level);

    if (v.fade > 0) {
        strata_mix_ramp(out, mix_, n, out_gain_ * v.gain, -out_gain_ * kFadeStep);
        v.gain -= kFadeStep * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        enaga_mix(out, mix_, n, out_gain_);
    }
}

}  // namespace strata
