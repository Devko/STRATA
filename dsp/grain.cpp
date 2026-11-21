// GRAIN, the granular player. dsp/grain.hpp has the ownership rules for the PCM, the bounds
// policy every grain obeys, and the argument for each of the choices that cost the cycles.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Per grain per 64-frame block, counting the hot loop only (dsp/grainloops.ti.c). These are the
// figures the central measurement (docs/BUDGET.md, PLAN.md M0.5) should be compared against, and
// they are an ESTIMATE: nothing in this file has been measured on a unit.
//
//   stage              expected to dominate                  flops/sample  bytes/sample
//   PCM read           2 int16 loads, 2 int-to-float          ~5            4
//                      converts and a linear blend. The
//                      loads are the traffic; at the root
//                      pitch one of the two is the previous
//                      sample's, so one new frame crosses
//                      from DDR per output sample.
//   window             the phase warp (a select and two       ~10           8 (L1D)
//                      flops), the clamp, and a linearly
//                      interpolated table read. THE LARGEST
//                      STAGE, and the price of SHP. The
//                      table is 1 KiB and stays in L1D.
//   VCA                the grain's own gain times the         ~3            0
//                      window, added into the scratch
//                      buffer. The voice envelope and the
//                      level are NOT here -- they are one
//                      pass over the sum, below.
//   advance            a 32.16 fixed-point add, the carry      ~1 (4 int)   0
//                      into the frame index, and the window
//                      phase. No division anywhere.
//
// So about 19 flops and 5 integer ops a sample, against 4 bytes a sample of int16 from DDR.
// Shared per block, however many grains sound: one `Envelope::render`, one `enaga_clear` and one
// `enaga_mix_product` over the block, plus up to 8 grain spawns (the hop floor is what bounds
// that) of about 60 control-rate operations each.
//
// Derived cycles, and the reasoning rather than the number, because the number is a guess. Enaga
// measured 1,868 cycles a block for a computed voice of roughly 70-80 flops/sample -- so about
// 2.5 flops/cycle on this core -- and 2,592 for one that reads DDR, a memory tax of 724 cycles
// (11.3 cycles/sample) for a voice gathering FOUR taps. Our 19 flops/sample is about 7.6
// cycles/sample of arithmetic, roughly 490 cycles a block; the two taps should cost less than
// Enaga's four, but eight grains are eight independent read streams rather than one, which is
// harder on a 64-byte line than one stream is. Call it 500-800 cycles of memory. That puts a
// grain at **1,000-1,300 cycles** and eight of them, with the shared passes and the spawns, at
// **about 9,000-11,000 cycles: 1.8-2.2% of the 496,000-cycle block**, against the 21,520
// docs/SPEC.md budgets. Without cl6x, expect the arithmetic part to be about 2.2x (docs/BUDGET.md)
// and the memory part unchanged, so roughly 1.7x overall.
//
// **Treat that as a hypothesis with wide error bars**, for two reasons that pull in opposite
// directions. It could be worse: eight streams walking a sample at eight different offsets is the
// worst cache pattern in the app, and SPR makes every grain start somewhere else again, so a
// cloud with heavy spray may be nothing but L1D misses. It could be better: at the root pitch the
// taps overlap almost completely and the window table never leaves L1D. The lever if the
// measurement disappoints is the grain COUNT, then the window table read -- not the interpolator,
// which is already the cheap one.
//
// --- where the remaining cost is, if it matters later ---------------------------------------
//
// Three things were considered and rejected, recorded here so they are not rediscovered:
//
//   - **Catmull-Rom taps, as dsp/sample.cpp uses.** Four taps at eight grains is 32 int16 loads an
//     output sample. sample.cpp's own cost note shows it is memory-bound on four taps for ONE
//     voice; this would be eight times that traffic for a fragment that a window already blurs.
//   - **A window table per shape, crossfaded.** Four loads and three lerps a sample instead of two
//     and one, to reach shapes the phase warp already reaches.
//   - **Applying the voice envelope per grain.** Eight `enaga_multiply` passes a block instead of
//     one `enaga_mix_product`, to get a result that is identical because the envelope is common to
//     every grain.
#include "grain.hpp"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::clamp;
using enaga::dsp::cos_turns;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;

// The output trim, calibrated on the LOUDEST case the way dsp/va.cpp's kHeadroom is, because a
// machine that sums eight copies of a signal needs calibrating somewhere. The loudest material
// there is, is a full-scale constant: eight grains all reading the same maximum value with nothing
// anywhere to cancel. Swept over SIZ, DEN and SPR and hammered with a trigger every block, that
// material peaked at 3.23 before this trim, so 0.5 puts the measured worst case at about 1.6 and a
// sustained cloud at ordinary settings nearer 0.5 -- a loop track sitting under the drums, which is
// where a loop track belongs, and MIXER mode has the fader for the rest. The HARD ceiling, which
// no material and no trigger pattern can pass, is kGrains * kHeadroom = 4: eight windows of at most
// 1 with latched gains of at most 1. tests/test_grain.cpp pins both numbers.
constexpr float kHeadroom = 0.5f;

// Long enough that STOP once leaves a granular tail rather than a click: the cloud thins out
// while the grains already in the air finish through their own windows. Six times SAMPLE's
// release, because a grain's tail is NOT the sample's own -- it is this envelope.
constexpr float kReleaseS = 0.18f;

inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : (x > hi ? hi : x); }


}  // namespace

// --- parameters -----------------------------------------------------------
//
// Eight of them: the SRC page and the AMP page. docs/SPEC.md's GRAIN row names five (position,
// size, density, spray, pitch) and the AMP page owes a level and an envelope, which is seven; the
// eighth is SHP, and dsp/grain.hpp explains why it is not a scan rate.
//
// SRC
//   0  POS  position, linear over the whole sample: 0 is frame 0 and 127 the last frame. The grain
//           START slides back from here when a whole grain would not fit, so POS at the top plays
//           the last grain of the sample rather than falling silent. **This is the parameter to
//           lock per step or to drive from a tempo-synced LFO** -- that is how a granular stretch
//           gets its tempo lock in STRATA, and why this machine needs no tempo of its own.
//   1  SIZ  grain size, 5 ms to about 390 ms, exponential, because size is heard as a ratio: at
//           the bottom the grain is shorter than a cycle of a bass note and the cloud is a buzz,
//           at the top each grain is a recognisable fragment.
//   2  DEN  density, as the OVERLAP: 0.25 (gaps between grains, a stutter) to 8 (the pool full, a
//           smooth cloud). The hop between onsets follows from SIZ and DEN together, so neither
//           knob is a rate and changing SIZ does not change how dense the cloud sounds.
//   3  SPR  spray, square law, up to half a second of position jitter, and the same amount as a
//           fraction of the hop jittering the onsets. Square law because the useful part is the
//           bottom: a few milliseconds is a chorus, 100 ms is a cloud, half a second is a wash.
//           The onset jitter rides along because a perfectly regular hop comb-filters -- the
//           regularity, not the grain, is what makes an unsprayed cloud sound metallic.
// AMP
//   8  PIT  pitch, (knob - 64) * 24/63 semitones: exactly unity at 64, +24 at 127, -24.4 at 0.
//           The step is then PIT x the trig note's ratio x source_rate/48 kHz, clamped to 1/64..32
//           so the fixed-point step and the bounds arithmetic both stay inside their words. The
//           trig note transposes from note 60, so a cloud plays chromatically across the pads.
//   9  SHP  grain shape: 0 is a symmetric Hann grain (the smoothest, and the classic granular
//           pad), 127 puts the peak 2% in, which is an instant attack with a long decay -- a
//           plucked grain, and what makes a dense cloud read as a texture of events rather than a
//           pad. It is one window table read either way; dsp/grainloops.ti.c has the trick.
//  10  DEC  decay to silence, 5 ms * 2^(11.6 * knob): 5 ms to about 16 s, and **127 means hold**
//           (sustain 1.0), which is the setting a loop track wants -- the cloud runs until the
//           sequencer stops it. There is no ATK: a grain's own window already ramps every onset,
//           so an attack knob would soften what is already soft, and the slot is SHP's.
//  11  LVL  output level, square law, times kHeadroom. Square gives usable control over the quiet
//           end; the trim is calibrated on the loudest case, as dsp/va.cpp's is, and the comment
//           on kHeadroom has the measurement.

void Granular::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    rate_ratio_ = source_rate_ * inv_rate_;

    // One cycle of Hann plus its two guard entries, built once here and never in audio(). 258
    // floats is the entire cost of having an arbitrary window shape in the inner loop.
    //
    // The ends are then forced to zero rather than left to cos_turns, which is accurate to 4e-6:
    // a grain's first and last sample are exactly silent, and the guards are exactly equal to the
    // first entry, which is what makes every index the loop's clamped phase can produce safe to
    // read -- and lets the loop carry no index clamp at all (dsp/grainloops.h has the argument).
    const float dt = reciprocal(float(kWindowSize));
    for (int i = 0; i < kWindowSize; i++) {
        window_[i] = 0.5f - 0.5f * cos_turns(float(i) * dt);
    }
    window_[0] = 0.0f;
    for (int i = 0; i < kWindowGuard; i++) window_[kWindowSize + i] = 0.0f;

    for (Grain &g : grains_) g = Grain{};
    amp_.kill();
    pending_ = false;
    wait_ = 0;
    voice_on_ = false;
    hop_countdown_ = 0;
    steal_cursor_ = 0;
    note_ratio_ = 1.0f;
    level_ = 1.0f;
    active_ = false;

    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Granular::prepare(const TrackParams &params) {
    position_unit_ = params.unit(0);
    recompute_position();

    const float size_s = 0.005f * exp2(6.3f * params.unit(1));      // 5 ms to ~390 ms
    grain_len_ = clampi(int32_t(size_s * rate_), kMinGrain, kGrainMax);

    // The hop follows from SIZ and the overlap, so DEN is an overlap and not a rate. The floor is
    // what bounds how many grains one block can spawn, and what keeps render_slice terminating.
    const float overlap = 0.25f * exp2(5.0f * params.unit(2));      // 0.25 to 8
    hop_ = clampi(int32_t(float(grain_len_) * reciprocal(overlap)), kHopMin, kGrainMax);

    spray_unit_ = params.unit(3);
    spray_ = int32_t(spray_unit_ * spray_unit_ * 0.5f * rate_);     // up to half a second
    if (playable_) spray_ = clampi(spray_, 0, frames_);

    // 64 is exactly unity, so a pattern written at the centre reads the sample at its own pitch.
    pitch_ratio_ = semitones((float(params.get(8)) - 64.0f) * (24.0f / 63.0f));
    recompute_step();

    // The window's peak, from the middle to 2% in. It stops at 2% rather than 0 because rise_ is
    // 0.5 / peak_, and 2% of even the shortest grain is still a ramp rather than a step.
    peak_ = 0.5f - 0.48f * params.unit(9);
    rise_ = 0.5f * reciprocal(peak_);
    fall_ = 0.5f * reciprocal(1.0f - peak_);

    // DEC at the top is hold. The attack is zero on purpose: a shape whose attack time is under
    // one step gets coefficient 1.0, which is a genuine zero-length attack (docs/ENAGA_API.md).
    // The times below are already times-to-1%, which is what EnvelopeShape::make wants.
    const float decay = 0.005f * exp2(11.6f * params.unit(10));     // 5 ms to ~16 s
    const float sustain = params.get(10) >= 127 ? 1.0f : 0.0f;
    amp_shape_ = EnvelopeShape::make(0.0f, decay, sustain, kReleaseS, rate_);

    const float level = params.unit(11);
    out_gain_ = kHeadroom * level * level;
}

// POS against the resident sample. Called from prepare() and from set_sample(), because the
// position is in frames and a new sample changes what a knob position means.
void Granular::recompute_position() {
    position_ = 0;
    if (!playable_) return;
    const int32_t last = frames_ - 1;
    position_ = clampi(int32_t(position_unit_ * float(last) + 0.5f), 0, last);
}

// The read step, in source frames per output sample, and its fixed-point and reciprocal forms.
// Control rate: from prepare() when PIT or the source rate moves, and from start_voice() when the
// trig note does. Nothing in the inner loop divides, and nothing in it rebuilds this.
void Granular::recompute_step() {
    static_assert(sizeof(kCrowdGain) / sizeof(kCrowdGain[0]) == kGrains,
                  "kCrowdGain holds one entry per grain in the pool");
    static_assert(kStepMax <= 32.0f,
                  "strata_grain_advance's 32-bit product assumes a whole step of 32 or less");
    static_assert(static_cast<unsigned long long>(kGrainMax - 1) * STRATA_GRAIN_FRAC_MASK <
                      0x80000000ull,
                  "a grain this long would overflow strata_grain_advance's fixed-point product, "
                  "and the bound it computes is the only thing keeping reads inside the buffer");

    step_ = clamp(rate_ratio_ * pitch_ratio_ * note_ratio_, kStepMin, kStepMax);
    istep_ = int32_t(step_);
    const float fraction = (step_ - float(istep_)) * float(STRATA_GRAIN_FRAC_ONE);
    fstep_ = uint32_t(int32_t(clamp(fraction, 0.0f, float(STRATA_GRAIN_FRAC_MASK))));
    inv_step_ = reciprocal(step_);
}

// The largest grain length, in output samples, whose every read from `start` -- the tap at the
// position and its linear partner one frame later -- stays inside [0, frames_). See "how a grain
// is bounded" in dsp/grain.hpp: the float estimate chooses where to start checking and the exact
// integer walk decides the answer.
int32_t Granular::fit_length(int32_t start, int32_t want) const {
    const int32_t max_adv = frames_ - 2 - start;   // the position AND its partner must be in range
    if (max_adv < 0) return 0;

    int32_t len = clampi(want, 0, kGrainMax);
    // A float division is hardware on this core, but enaga_reciprocal is only good to about 2e-7,
    // so this picks the candidate and never the answer. The estimate is clamped to kGrainMax
    // BEFORE the integer cast, because max_adv * inv_step_ can exceed what an int32 holds on a
    // long sample at the bottom of the pitch range.
    const float estimate = float(max_adv) * inv_step_ + 1.0f;
    const int32_t cap = estimate >= float(kGrainMax) ? kGrainMax
                                                     : (estimate >= 1.0f ? int32_t(estimate) : 0);
    if (cap < len) len = cap;
    // The fixed-point step is at most the float step it came from (its fraction is truncated), so
    // the estimate can only be high by rounding: this walks down one or two steps, never scans.
    while (len > 0 && strata_grain_advance(len - 1, istep_, fstep_) > max_adv) len--;
    return len;
}

void Granular::set_sample(const int16_t *data, uint32_t frames, int channels, float source_rate) {
    // Every live grain is reading the buffer that is being taken away, so they stop. A crossfade
    // between two samples belongs to the app, which has both buffers; the machine has one.
    kill_all();

    const bool ok = data != nullptr && (channels == 1 || channels == 2) &&
                    frames >= uint32_t(kMinFrames) && frames <= uint32_t(kMaxFrames);
    data_ = ok ? data : nullptr;
    frames_ = ok ? int32_t(frames) : 0;
    channels_ = ok ? channels : 1;
    playable_ = ok;

    source_rate_ = source_rate > 1.0f ? source_rate : rate_;
    rate_ratio_ = source_rate_ * inv_rate_;
    // int16 to float, with the stereo sum's halving folded in: the tap adds L and R, so a stereo
    // source is summed to mono for free (machines are mono; pan lives in the insert chain).
    src_scale_ = (channels_ == 2 ? 0.5f : 1.0f) * (1.0f / 32768.0f);
    recompute_position();
    recompute_step();
    if (playable_) spray_ = clampi(spray_, 0, frames_);
}

void Granular::trigger(const Trig &trig) {
    // No sample: silent, and the machine never goes active. docs/SPEC.md asks for exactly this --
    // a track whose sample is still loading stays silent rather than glitching.
    if (!playable_) return;
    // One voice, so a trigger retriggers it: the envelope restarts and the scheduler restarts
    // from this frame. Grains already in the air are LEFT ALONE to finish through their own
    // windows -- there is no splice to hide, and cutting them is what would click. `slide` is
    // ignored: it is for the mono VA's glide, and a cloud has no single pitch to glide.
    next_ = trig;
    wait_ = trig.frame > 65535u ? 65535 : int(trig.frame);
    pending_ = true;
    active_ = true;
}

void Granular::release_all() {
    // STOP once: the envelope goes to release and the scheduler KEEPS RUNNING, so the cloud thins
    // out over kReleaseS instead of stopping dead. That tail is the point of a granular release.
    // A pending trig still starts -- it was scheduled before the stop.
    if (voice_on_) amp_.gate_off();
}

void Granular::kill_all() {
    for (Grain &g : grains_) {
        g.live = false;
        g.left = 0;
    }
    amp_.kill();
    pending_ = false;
    wait_ = 0;
    voice_on_ = false;
    hop_countdown_ = 0;
    active_ = false;
}

void Granular::render_add(float *out, int frames) {
    if (!active_) return;      // nothing sounding and nothing pending: an idle machine costs a test
    if (!playable_) {
        // The sample went away between blocks (a swap, or a load that failed). Silence, now.
        kill_all();
        return;
    }
    int done = 0;
    while (done < frames) {
        const int n = frames - done < kBlock ? frames - done : kBlock;
        render_slice(out + done, n);
        done += n;
    }
    bool any = false;
    for (const Grain &g : grains_) any = any || g.live;
    active_ = pending_ || voice_on_ || any;
}

const char *Granular::param_name(int index) const {
    static const char *const names[kParams] = {
        "POS", "SIZ", "DEN", "SPR",      // SRC
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "PIT", "SHP", "DEC", "LVL",      // AMP
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- the voice and its grains ---------------------------------------------

void Granular::start_voice() {
    const Trig t = next_;
    pending_ = false;
    wait_ = 0;
    if (!playable_) {
        voice_on_ = false;
        return;
    }
    note_ratio_ = clamp(semitones(float(int(t.note) - kRootNote)), kStepMin, kStepMax);
    recompute_step();
    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    level_ = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    amp_.gate_on(true);
    voice_on_ = true;
    hop_countdown_ = 0;    // the first grain starts on the trig's own frame: sample accurate
}

// One grain onset. Control rate, at most a handful of times a block (the hop floor is what bounds
// that), so everything expensive about a grain happens here rather than in the loop.
void Granular::spawn() {
    // Reset the countdown FIRST, whatever happens below. render_slice sub-slices the block at the
    // next onset, so a spawn that declines to make a grain and left the countdown at zero would
    // not terminate -- it would call spawn() forever on one frame.
    //
    // The jitter is SPR's: a perfectly regular hop comb-filters, and the regularity rather than
    // the grain is what makes an unsprayed cloud sound metallic.
    const float jitter = spray_unit_ * 0.5f * float(hop_) * random_.bipolar();
    hop_countdown_ = clampi(hop_ + int32_t(jitter), 2, kGrainMax);

    if (!playable_) return;

    const int32_t offset = spray_ > 0 ? int32_t(float(spray_) * random_.bipolar()) : 0;
    int32_t start = position_ + offset;

    // The span a full-length grain needs, exactly. If the buffer can hold one anywhere, the start
    // slides back until it fits; only a buffer too short for a whole grain has its length cut.
    int32_t len = clampi(grain_len_, kMinGrain, kGrainMax);
    const int32_t span = strata_grain_advance(len - 1, istep_, fstep_);
    const int32_t room = frames_ - 2 - span;
    if (room >= 0) {
        start = clampi(start, 0, room);
    } else {
        start = 0;
        len = fit_length(0, len);
    }
    // Fewer frames than a grain is worth: silence rather than a click. A 1-frame sample, a 2-frame
    // sample and a POS at the very end of a tiny buffer all land here.
    if (len < kMinGrain) return;

    // Take a free grain from the pool, and steal the one furthest through its window only when all
    // eight are busy: that grain's window value is the smallest, so the splice is the least
    // audible one available. The cursor rotates, so equally advanced grains go in turn rather than
    // always the first.
    //
    // The pool, and not a cap derived from DEN, is what a spawn checks. A cap would have to steal a
    // grain the moment a retrigger arrived, and the grain it stole would be one that had only just
    // started -- its window still near its loudest. A long grain under sixteenth notes is an
    // ordinary way to use this machine and it would have clicked on every step.
    int pick = -1;
    int in_air = 0;
    for (int i = 0; i < kGrains; i++) {
        if (grains_[i].live) {
            in_air++;
        } else if (pick < 0) {
            pick = i;
        }
    }
    if (pick < 0) {
        static_assert((kGrains & (kGrains - 1)) == 0, "the steal cursor masks instead of dividing");
        pick = steal_cursor_;
        float furthest = grains_[pick].s.q;
        for (int n = 1; n < kGrains; n++) {
            const int i = (steal_cursor_ + n) & (kGrains - 1);
            if (grains_[i].s.q > furthest) {
                furthest = grains_[i].s.q;
                pick = i;
            }
        }
        steal_cursor_ = (pick + 1) & (kGrains - 1);
    }

    Grain &g = grains_[pick];
    g.s.pos = start;
    g.s.frac = 0;          // on a whole frame: that is what keeps the onset's bound exact
    g.s.istep = istep_;
    g.s.fstep = fstep_;
    g.s.q = 0.0f;
    g.s.dq = reciprocal(float(len));
    g.s.peak = peak_;
    g.s.rise = rise_;
    g.s.fall = fall_;
    // The velocity of the note that spawned it and the crowd compensation, both latched: a
    // retrigger at a different velocity does not rescale a cloud that is already in the air, and
    // the compensation cannot step in the middle of a block the way a shared block gain would.
    //
    // kCrowdGain[k] is 1 / sqrt(k + 1), so the first grain of a cloud is at unity and the eighth
    // at -9 dB. Grains taken from nearby positions are partly coherent, so neither 1/N (right for
    // identical signals) nor 1 (right for independent ones) holds across the SPR range; the
    // geometric mean of the two is the usual compromise. A steal does not change how many grains
    // are in the air, so it takes the last entry.
    g.s.gain = level_ * kCrowdGain[in_air < kGrains ? in_air : kGrains - 1];
    g.left = len;
    g.live = true;
}

// One slice of up to kBlock frames, cut again at the trig's frame and at every grain onset inside
// it. That is what makes both sample accurate without putting a test in the per-sample loop.
void Granular::render_slice(float *out, int n) {
    int i = 0;
    while (i < n) {
        if (pending_ && wait_ == 0) {
            start_voice();     // sample-accurate: the voice begins on trig.frame
            continue;
        }
        if (scheduling() && hop_countdown_ == 0) {
            spawn();           // and spawn() always moves the countdown on, so this terminates
            continue;
        }
        int k = n - i;
        if (pending_ && wait_ < k) k = wait_;
        if (scheduling() && hop_countdown_ < k) k = hop_countdown_;
        run(out + i, k);
        if (pending_) wait_ -= k;
        if (scheduling()) hop_countdown_ -= k;
        i += k;
    }
}

void Granular::run(float *out, int n) {
    // The envelope runs whether or not a grain sounds, because it is the voice's clock: a sparse
    // cloud (DEN at the bottom) has gaps between grains, and a decay must not pause in them.
    amp_.render(env_, n, amp_shape_);

    bool any = false;
    for (const Grain &g : grains_) any = any || g.live;
    if (any) {
        enaga_clear(scratch_, n);
        for (Grain &g : grains_) {
            if (!g.live) continue;
            // A grain that ends inside this slice renders only what it has left; the rest of the
            // scratch buffer is the zeros enaga_clear left there.
            const int m = g.left < n ? int(g.left) : n;
            if (channels_ == 2) {
                strata_grain_add_stereo(scratch_, m, data_, window_, &g.s);
            } else {
                strata_grain_add_mono(scratch_, m, data_, window_, &g.s);
            }
            g.left -= m;
            g.live = g.left > 0;
        }
        // The voice envelope, LVL and the int16 scale, all applied to the SUM of the grains in
        // one pass: out += gain * scratch * env. This ADDS into the track buffer and never clears
        // it (dsp/track.hpp). The crowd compensation is not here -- it is latched into each
        // grain's own gain at its onset, so that it never steps mid-block.
        enaga_mix_product(out, scratch_, env_, n, out_gain_ * src_scale_);
    }

    // The envelope reaching zero is what says the voice has finished (docs/ENAGA_API.md,
    // "Gotchas"): with DEC at 127 it never does, which is what a loop track wants. When it does,
    // the grains still in the air have been multiplied by about zero for a block already, so they
    // are retired rather than left to cost cycles for nothing.
    if (voice_on_ && done(amp_)) {
        voice_on_ = false;
        for (Grain &g : grains_) {
            g.live = false;
            g.left = 0;
        }
    }
}

}  // namespace strata
