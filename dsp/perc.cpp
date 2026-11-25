// PERC: 2-operator FM percussion. See perc.hpp for the design and docs/SPEC.md for the row it
// implements. No reference implementation: Plaits has no FM drum model, so this is STRATA's own
// arrangement of the one fact that matters about FM percussion (a falling index, not a static
// one) rather than a port of anything.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Two voices, so this is per voice per 64-frame block, counting multiplies and adds in the hot
// loop only. These are the figures the central measurement (docs/archive/BUDGET_MILESTONES.md, PLAN.md M0.5) should
// be compared against.
//
//   stage              expected to dominate                            flops/sample
//   the operators      TWO sin_turns: a floor_int, two selects and a   ~22   THE LARGEST STAGE
//                      degree-9 odd polynomial each, ~11 flops            (54% of the voice)
//                      apiece. One for the modulator, one for the
//                      carrier it phase-modulates. Nothing else in
//                      this machine comes close.
//   phase + index      two accumulates with a select wrap, the index    ~6
//                      applied to the modulator's output, and the
//                      index envelope's one multiply.
//   pitch envelope     1 + amount * env, its own multiply, and one      ~6   amount is 0 at
//                      multiply and cap per operator. Both operators         SWP = 0 but the
//                      take the same bend, which is what keeps the           loop is shared:
//                      ratio -- and so the timbre -- fixed through           the 6 flops are
//                      the sweep.                                            paid anyway.
//   amp env, VCA,      Envelope::render, enaga_scale, enaga_multiply    ~7
//   output             and enaga_mix: all cl6x block functions.
//
// So about 41 flops/sample, and unlike KICK and SNARE there is no stage that can be skipped: an
// FM pair is making both operators all the time. Against Enaga's measured VA voice (1,868 cycles
// per block = 29 cycles/sample for roughly 70-80 flops/sample, so ~2.5 flops per cycle on this
// core) that is **about 1,050 cycles per voice per block, ~0.56x a full VA voice** -- a little
// over half, and well under half of what KICK and SNARE each cost, because a phase-modulated
// sine needs no waveshaper, no reciprocal and no noise source. Both voices sounding at once is
// about 2,100 cycles, 0.42% of a 496,000-cycle block.
//
// The output is also bounded by construction: sin_turns of anything is within +-1 whatever the
// index is, so there is no soft clip here and no makeup gain to trim.
//
// *.ti.c candidates, as CLAUDE.md asks: `fm_block` below is the one and only hot loop, it is
// already branch-free (both wraps and both caps are selects), and it would be a near-ideal
// cl6x kernel -- two independent sine evaluations per iteration with no loop-carried dependency
// beyond four accumulators. If the measurement disappoints, that loop plus a shared
// sin_turns-of-two-values helper is where to start; `mix_ramp` is on the steal path only and is
// not worth moving. This agent's file list did not allow adding the .ti.c file itself.
#include "perc.hpp"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::clamp;
using enaga::dsp::exp;
using enaga::dsp::exp2;
using enaga::dsp::floor_int;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;
using enaga::dsp::sin_turns;

// ln(100): EnvelopeShape::make wants the time to reach 1% of its target, where every time below
// is written as a time constant. One is ln(100) of the other, and getting it wrong makes every
// envelope 4.6x too short (docs/ENAGA_API.md, "Gotchas found in practice").
constexpr float kToOnePercent = 4.60517019f;

// Phase increments are capped here, a shade under Nyquist, so that the wrap can be a single
// subtract rather than a floor: no increment can ever carry a phase past 2.0.
constexpr float kMaxDt = 0.49f;

// The largest index IDX can ask for, in turns of carrier phase. 2 turns is 12.6 radians of the
// usual FM index, which is far past the point where the spectrum is a wall of sidebands -- about
// right for the top of a knob whose job is to reach "struck metal".
constexpr float kIndexMax = 2.0f;
constexpr float kInvTwoPi = 0.15915494f;

// RAT's quantised table: 16 modulator : carrier ratios, 4 knob steps each over knob 0-63.
//
// Why quantise at all, when the obvious thing is a continuous sweep? Three reasons, and they are
// the reason this is the one mapping in the machine that is not a smooth curve:
//
//   1. FM ratio is not a perceptual continuum. Between 2.0 and 2.5 there is nothing that sounds
//      like "2.25" -- there is only 2.0 detuned sharp, which reads as a tom going out of tune
//      rather than as a new timbre. A continuous knob spends most of its travel in those
//      near-misses, so most of the knob is unusable and the few good spots are hard to find.
//   2. Parameters are stored as one byte and locked per step (dsp/track.hpp). A ratio of exactly
//      1.000 or exactly 3.000 has to be reachable and REPEATABLE from a 0-127 lock, or the same
//      tom recalls slightly different on reload. Quantising makes a knob value and a ratio the
//      same thing.
//   3. It lets the table alternate deliberately between the two families the spec asks for:
//      integer and simple-rational entries (hollow, tuned: toms) sit next to irrational ones
//      (metallic, no common period: rims and blips), so one step of the knob is a change of
//      CHARACTER and not just of brightness.
//
// Knob 64-127 is then a continuous exponential sweep from the top of the table (4.0) up to 16.0,
// joined at exactly 4.0 so the knob has no jump at the seam. That region is all inharmonic hash
// by nature -- its job is to be tuned by ear for a particular clang, which is the one case where
// quantising would get in the way.
//
// The irrational entries are the classics: sqrt(2), the golden ratio, sqrt(5), 1 + sqrt(2), pi
// and 2 + sqrt(3). Nothing mystical about them; they are simply ratios with no small-integer
// approximation nearby, which is what "no common period" requires.
constexpr float kRatioTable[Perc::kRatioSlots] = {
    0.50f,      //  0  knob  0-3   sub-octave modulator: half-integer partials, dark and hollow
    0.75f,      //  1  knob  4-7   3:4, hollow and low
    1.00f,      //  2  knob  8-11  the classic: every sideband on a harmonic of f0. Tuned tom
    1.25f,      //  3  knob 12-15  5:4
    1.41421f,   //  4  knob 16-19  sqrt(2): the canonical inharmonic ratio. Metallic
    1.50f,      //  5  knob 20-23  3:2, tuned and brighter than 1
    1.61803f,   //  6  knob 24-27  the golden ratio: inharmonic with no beating anywhere
    2.00f,      //  7  knob 28-31  the octave: a round, tuned tom
    2.23607f,   //  8  knob 32-35  sqrt(5): metallic
    2.41421f,   //  9  knob 36-39  1 + sqrt(2): metallic, the rimshot end
    2.50f,      // 10  knob 40-43  5:2, tuned and bright
    3.00f,      // 11  knob 44-47  woody and hollow: the other tom
    3.14159f,   // 12  knob 48-51  pi: strongly inharmonic. Rims and cowbells
    3.50f,      // 13  knob 52-55  7:2
    3.73205f,   // 14  knob 56-59  2 + sqrt(3): inharmonic clang
    4.00f,      // 15  knob 60-63  bright and hollow: blip territory, and the continuous join
};

// RAT's two step sizes. Written as constants because the obvious spelling of either -- slots per
// knob value, or one over the sweep's span in knob values -- is an INTEGER division, and the
// C674x has no integer divide instruction at all: every one is a libgcc call, and a 64-bit one
// also drags in the unwinder and fails to link. prepare() runs inside audio() like the rest.
constexpr float kSlotsPerKnob = 0.25f;           // kRatioSlots / (kQuantisedTop + 1) = 16 / 64
constexpr float kInvSweepSpan = 1.0f / 63.0f;    // 1 / (127 - (kQuantisedTop + 1))
constexpr float kRatioOctaves = 2.0f;            // 4.0 at the seam to 16.0 at the top

// The anti-alias ceiling on the index, applied once per hit.
//
// An FM pair's spectrum spans about +-(index_radians + 1) * f_modulator around the carrier
// (Carson's rule). Letting that band reach the SAMPLE RATE rather than Nyquist allows an octave
// of deliberate fold-over, which is part of what FM percussion has always sounded like -- the
// DX7 aliased constantly and its rims are the better for it -- while stopping the extremes of
// the knobs from turning into undifferentiated hash. So the limit is
// index_radians = 1 / dt_modulator - 1, and this machine's index is in turns.
//
// At any sane setting this does not bite: a 300 Hz carrier at ratio 2 allows 12.6 turns against a
// knob that only reaches 2. It bites exactly where it should, at a high note with a high ratio,
// where the modulator is already near Nyquist and no amount of index buys any more timbre.
// KICK scales its dirtiness by max(1 - 8*f0, 0) for the same reason.
inline float index_ceiling(float dt_modulator) {
    const float dt = clamp(dt_modulator, 1e-5f, kMaxDt);
    return clamp((reciprocal(dt) - 1.0f) * kInvTwoPi, 0.02f, kIndexMax);
}

// The hot loop: n samples of one 2-operator FM voice. Branch-free -- both phase wraps and both
// Nyquist caps are selects -- so cl6x can software-pipeline it, and the obvious *.ti.c kernel.
//
// The carrier reads sin(carrier_phase + index * sin(modulator_phase)), which is phase
// modulation; with a sine modulator it is indistinguishable from frequency modulation and it is
// what every "FM" synth since the DX7 has actually done. This is also precisely why Enaga's
// Oscillator cannot be used for the carrier: render_add advances its own phase from dt and has
// no way to be offset per sample, and there is no hook for a per-sample phase input. A bare
// float accumulator is the right call whenever a machine needs a phase behaviour the class does
// not offer (docs/ENAGA_API.md says so about the 909 kick's pinned phase, and it applies here).
struct FmState {
    float carrier_phase;
    float mod_phase;
    float index;
    float sweep;
};

void fm_block(float *out, int n, FmState &s, float dt_carrier, float dt_mod, float sweep_amount,
              float sweep_decay, float index_decay) {
    float carrier_phase = s.carrier_phase;
    float mod_phase = s.mod_phase;
    float index = s.index;
    float sweep = s.sweep;
    for (int i = 0; i < n; i++) {
        // One bend for both operators, so the ratio -- and so the timbre -- holds through the
        // sweep. Bending only the carrier would make the pitch envelope a timbre envelope too.
        const float bend = 1.0f + sweep_amount * sweep;
        sweep *= sweep_decay;

        float dp_mod = dt_mod * bend;
        dp_mod = dp_mod > kMaxDt ? kMaxDt : dp_mod;
        float dp_carrier = dt_carrier * bend;
        dp_carrier = dp_carrier > kMaxDt ? kMaxDt : dp_carrier;

        mod_phase += dp_mod;
        mod_phase -= mod_phase >= 1.0f ? 1.0f : 0.0f;
        carrier_phase += dp_carrier;
        carrier_phase -= carrier_phase >= 1.0f ? 1.0f : 0.0f;

        out[i] = sin_turns(carrier_phase + index * sin_turns(mod_phase));
        index *= index_decay;
    }
    s.carrier_phase = carrier_phase;
    s.mod_phase = mod_phase;
    s.index = index;
    s.sweep = sweep;
}

// out += gain * in, with gain walking by step each sample: the ramp a stolen voice fades out
// with. Two flops a sample and only on the steal path, so it never costs anything usually.
void mix_ramp(float *out, const float *in, int n, float gain, float step) {
    for (int i = 0; i < n; i++) {
        out[i] += gain * in[i];
        gain += step;
    }
}

}  // namespace

// --- parameters -----------------------------------------------------------
//
// SRC -- the FM pair itself: where it is, what shape it is, how deep, and how fast the depth
// falls. Those four are the whole engine, which is why the pitch envelope goes on AMP instead:
// the index envelope is what makes this machine percussive and the pitch envelope only
// flavours the attack.
//   0  TUN  the CARRIER's frequency, 48-1822 Hz exponential, exactly 300 Hz at knob 64. The trig
//           note transposes it from note 60, so note 60 sounds exactly at TUN and a perc line
//           written into a pattern follows the key. Toms live around knob 20-45, rims and
//           cowbells around 70-90, blips 100 and up.
//   1  RAT  the modulator's frequency as a ratio of the carrier's. Knob 0-63 is QUANTISED to the
//           16 ratios in kRatioTable above (4 knob steps each), alternating integer/simple-
//           rational (hollow, tuned) with irrational (metallic); knob 64-127 sweeps continuously
//           from 4.0 to 16.0, joined at exactly 4.0. kRatioTable's comment has the full
//           reasoning for the quantisation.
//   2  IDX  index: the modulation depth, and the main timbre control. Square law, 0 to 2 turns
//           of carrier phase (0 to 12.6 radians in the usual units), so the quiet half of the
//           knob -- where the ear hears the most change -- gets most of the travel. 0 is a bare
//           sine. Velocity scales it, so a hard hit brightens as well as lifts, which is what
//           docs/SOUND.md asks accent to do. Capped per hit by index_ceiling() above.
//   3  IDD  index envelope decay: time constant 2-400 ms exponential, about 29 ms at knob 64.
//           The index falls from its peak towards zero, so a note's timbre collapses into a
//           plain sine as it dies -- this is the knob that decides whether a hit is a click
//           (bottom), a drum (middle) or a bell (top). There is deliberately no "static index"
//           position: a static index is an organ, not percussion, and the top of this knob is
//           long enough relative to any DEC setting to be static in practice.
// AMP
//   8  DEC  amplitude decay: time constant 4-600 ms exponential, about 50 ms at knob 64. Linear
//           in the exponent rather than squared as on KICK, because percussion needs the whole
//           range: a rim at the bottom, a tom at the top, and no dead zone in between.
//   9  SWP  pitch envelope amount, 0-7 linear: the operators start at (1 + SWP) x f0 and fall to
//           f0, so full SWP starts a hit 8x up, about +36 semitones. Both operators bend
//           together. The same idea as KICK's FMA and used the same way -- a short sweep is the
//           difference between a tom and a sine blip.
//  10  SWT  pitch envelope decay: time constant 1-200 ms exponential, about 14 ms at knob 64.
//           Under about 10 ms it is an attack click; 20-60 ms is the tom "boing".
//  11  LVL  output level, square law, 0-1. Square gives usable control over the quiet end. No
//           makeup trim: an FM carrier is a sine, so a hit peaks at LVL x velocity and lands
//           where KICK's and HAT's do.

float Perc::ratio_for(uint8_t knob) {
    if (int(knob) <= kQuantisedTop) {
        // 16 slots over knob 0-63, 4 knob steps each.
        int slot = floor_int(float(knob) * kSlotsPerKnob);
        slot = slot < 0 ? 0 : (slot > kRatioSlots - 1 ? kRatioSlots - 1 : slot);
        return kRatioTable[slot];
    }
    // 4.0 to 16.0 over knob 64-127: two octaves of ratio, continuous at the seam.
    const float t = float(int(knob) - (kQuantisedTop + 1)) * kInvSweepSpan;
    return kRatioTable[kRatioSlots - 1] * exp2(kRatioOctaves * clamp(t, 0.0f, 1.0f));
}

void Perc::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    for (Voice &v : voices_) v = Voice{};
    active_ = false;
    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Perc::prepare(const TrackParams &params) {
    // 48-1822 Hz, with the exponent chosen so that knob 64 is exactly 300 Hz.
    base_hz_ = 48.0f * exp2(5.2463700f * params.unit(0));
    ratio_ = ratio_for(params.get(1));
    const float index = params.unit(2);
    index_ = index * index * kIndexMax;
    const float index_tau = 0.002f * exp2(7.6438562f * params.unit(3));        // 2-400 ms
    index_decay_ = exp(-inv_rate_ * reciprocal(index_tau));

    const float amp_tau = 0.004f * exp2(7.2288187f * params.unit(8));          // 4-600 ms
    // A 0.2 ms attack rather than a step: short enough to still read as instant on a rim, long
    // enough that the first sample is not a discontinuity in the bus.
    amp_shape_ = EnvelopeShape::make(0.0002f, amp_tau * kToOnePercent, 0.0f, 0.001f, rate_);
    sweep_amount_ = params.unit(9) * 7.0f;
    const float sweep_tau = 0.001f * exp2(7.6438562f * params.unit(10));       // 1-200 ms
    sweep_decay_ = exp(-inv_rate_ * reciprocal(sweep_tau));

    const float level = params.unit(11);
    out_gain_ = level * level;
}

void Perc::trigger(const Trig &trig) {
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

void Perc::release_all() {
    // A one-shot percussion voice has no sustain: its decay IS its release, so STOP once has
    // nothing to let go of and the tail finishes by itself. kill_all() is the hard cut.
}

void Perc::kill_all() {
    for (Voice &v : voices_) {
        v.pending = false;
        v.wait = 0;
        silence(v);
    }
    active_ = false;
}

void Perc::render_add(float *out, int frames) {
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

const char *Perc::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "RAT", "IDX", "IDD",      // SRC
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "DEC", "SWP", "SWT", "LVL",      // AMP
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- voices ---------------------------------------------------------------

void Perc::start(Voice &v) {
    const Trig &t = v.next;
    v.pending = false;
    v.wait = 0;
    v.fade = 0;
    v.gain = 1.0f;
    v.note_ratio = clamp(semitones(float(t.note) - 60.0f), 0.05f, 12.0f);
    // The same trigger level the other machines use: 0.3 + 0.7 * accent. It scales the index as
    // well as the amplitude, so a hard hit brightens rather than only getting louder.
    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    const float accent = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    v.trigger_level = 0.3f + 0.7f * accent;
    // Latched at the hit, both of them: a lock that moves RAT on the next step must not bend a
    // tail that is still ringing, and the index ceiling depends on the note this hit took.
    v.ratio = ratio_;
    const float f0 = clamp(base_hz_ * v.note_ratio, 10.0f, 4000.0f);
    const float ceiling = index_ceiling(f0 * inv_rate_ * v.ratio);
    v.index = index_ * v.trigger_level;
    v.index = v.index > ceiling ? ceiling : v.index;
    // The carrier starts at its peak, so the hit puts its energy out in the first samples rather
    // than a quarter cycle later; the modulator starts at zero, so the attack is the carrier's
    // own and the FM arrives within the modulator's first cycle. The amplitude envelope's 0.2 ms
    // attack is what keeps the carrier's peak from being a step in the bus.
    v.carrier_phase = 0.25f;
    v.mod_phase = 0.0f;
    v.sweep = 1.0f;
    v.amp.gate_on(true);
}

void Perc::silence(Voice &v) {
    v.amp.kill();
    v.index = 0.0f;
    v.sweep = 0.0f;
    v.fade = 0;
    v.gain = 1.0f;
}

void Perc::render_voice(Voice &v, float *out, int n) {
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

void Perc::run(Voice &v, float *out, int n) {
    if (!v.sounding()) return;

    // Note-dependent increments: still control rate, just once per voice per block rather than
    // once per machine, because the trig note moves the carrier and the modulator follows it.
    const float f0 = clamp(base_hz_ * v.note_ratio, 10.0f, 4000.0f);
    const float dt_carrier = clamp(f0 * inv_rate_, 0.0f, kMaxDt);
    const float dt_mod = clamp(dt_carrier * v.ratio, 0.0f, kMaxDt);

    FmState state{v.carrier_phase, v.mod_phase, v.index, v.sweep};
    fm_block(body_, n, state, dt_carrier, dt_mod, sweep_amount_, sweep_decay_, index_decay_);
    v.carrier_phase = state.carrier_phase;
    v.mod_phase = state.mod_phase;
    v.index = state.index;
    v.sweep = state.sweep;

    v.amp.render(env_, n, amp_shape_);
    enaga_scale(env_, n, v.trigger_level);
    enaga_multiply(body_, env_, n);

    if (v.fade > 0) {
        mix_ramp(out, body_, n, out_gain_ * v.gain, -out_gain_ * kFadeStep);
        v.gain -= kFadeStep * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        enaga_mix(out, body_, n, out_gain_);
    }
}

}  // namespace strata
