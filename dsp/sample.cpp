// SAMPLE, the one-shot and slice player. dsp/sample.hpp has the ownership rules for the PCM and
// the three-layer edge policy for 4-point interpolation.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Four voices, so this is per voice per 64-frame block, counting the hot loops only. These are
// the figures the central measurement (docs/BUDGET.md, PLAN.md M0.5) should be compared against.
//
//   stage              expected to dominate                  flops/sample  bytes/sample
//   tap gather         4 int16 loads and 4 int-to-float      4 converts    8 (mono)
//                      converts; stereo loads 8 and sums                   16 (stereo)
//                      the pair as ints, so one convert
//                      per tap either way.  THE LARGEST
//                      STAGE, and it is load slots, not
//                      arithmetic.
//   Catmull-Rom        four coefficients (8 mul, 8 add) and  ~19           0
//                      a Horner evaluation (3 mul, 3 add).
//   phase              one add, a shift, a mask, a select     0 (6 int)    0
//                      and two adds. No division: the
//                      position is 32.24 fixed point.
//   amp env + VCA      Envelope::render, then the VCA and     ~6            0
//                      the mix into the track buffer in one
//                      enaga_mix_product pass. Both are cl6x
//                      block functions.
//
// So about 25 flops/sample plus 6 integer ops, against 8 bytes/sample of int16 loads (16 for a
// stereo source). The *traffic* is smaller than the load count: at the root pitch three of four
// taps are the previous sample's, so one new frame (2 bytes) crosses from DDR per output sample
// and the other loads hit L1D; at an octave up it is four new frames, and a 64-byte L1D line is
// used up every 8 output samples.
//
// Derived cycles. Enaga measured 1,868 cycles per block for a computed voice of roughly 70-80
// flops/sample (so ~2.5 flops/cycle on this core) and 2,592 for one that reads DDR -- a memory
// tax of 724 cycles, 11.3 cycles/sample, for a voice doing much less interpolation than this.
// Our 25 flops/sample is ~10 cycles/sample of arithmetic, about 640 cycles a block, and the taps
// cost at least Enaga's 11.3 cycles/sample and more as the pitch rises. Expect **1,400 cycles
// per voice at the root pitch and up to about 2,600 an octave up**, so four voices are
// **5,600-10,400 cycles, 1.1-2.1% of the 496,000-cycle block** -- comfortably cheaper than the
// 8 grains (21,520 cycles) that docs/SPEC.md budgets for track 8.
//
// **Memory-bound, not compute-bound**, at any pitch at or above the root; only a voice pitched
// well down is compute-bound, and then only because each DDR frame is reused over many output
// samples. Two consequences, and they are the reason for the choices in this file:
//
//   - A `*.ti.c` rewrite of the inner loop would buy back at most the ~10 cycles/sample of
//     arithmetic and nothing of the DDR stream, so it is worth far less here than for a computed
//     voice. If the measurement disappoints, the levers are the tap count and the access
//     pattern, not hand-tuning the Hermite.
//   - Which is exactly why the interpolator is the good one. Catmull-Rom over 4 taps costs ~12
//     flops/sample more than linear over 2, but it needs only two more loads that are usually
//     already in L1D, and those flops hide under loads that are happening anyway. Paying for
//     quality is cheap in the direction this machine is already bound.
#include "sample.hpp"

#include "sampleloops.h"

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::clamp;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;

// The read position's fixed-point scale: 32 integer frames and 24 fractional bits.
constexpr int kFracBits = 24;
constexpr uint32_t kFracMask = (1u << kFracBits) - 1u;
constexpr float kFracScale = 16777216.0f;                     // 2^24
constexpr float kInvFracScale = 1.0f / kFracScale;

// Six octaves down, five up. Clamping here is what keeps the fixed-point step inside its word.
constexpr float kStepMin = 1.0f / 64.0f;
constexpr float kStepMax = 32.0f;

// A release so STOP-once leaves something to hear. Short, because a sampler's tail is the
// sample's own: this is a declick, not an effect.
constexpr float kReleaseS = 0.03f;

// 1/n for the slice count, as a table: dividing window_len by the slice count is the one place
// this machine would otherwise need a divide, and float or not it is not worth a run of Newton
// steps once per block.
constexpr float kInvSlices[Sampler::kSlicesMax] = {
    1.0f,        1.0f / 2.0f,  1.0f / 3.0f,  1.0f / 4.0f,  1.0f / 5.0f,  1.0f / 6.0f,
    1.0f / 7.0f, 1.0f / 8.0f,  1.0f / 9.0f,  1.0f / 10.0f, 1.0f / 11.0f, 1.0f / 12.0f,
    1.0f / 13.0f, 1.0f / 14.0f, 1.0f / 15.0f, 1.0f / 16.0f,
};

inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : (x > hi ? hi : x); }

// One tap, as a raw int16 value in float. The 1/32768 and the stereo halving are folded into the
// output gain, so nothing here scales. For a stereo source the pair is summed as integers, which
// keeps it to one int-to-float convert per tap.
template <int kCh>
inline float tap(const int16_t *p) {
    if constexpr (kCh == 2) return float(int(p[0]) + int(p[1]));
    else return float(p[0]);
}

// Catmull-Rom: the cubic through x1 and x2 whose slopes are the central differences. docs/SPEC.md
// asks for 4-point interpolation; this is the usual choice, continuous in value and slope, and
// it cannot overshoot the way a Lagrange cubic does on a transient.
//
//   y = x1 + t*(0.5*(x2-x0)) + t^2*(x0 - 2.5*x1 + 2*x2 - 0.5*x3) + t^3*(0.5*(x3-x0) + 1.5*(x1-x2))
//
// t = 0 gives exactly x1 and t = 1 exactly x2, which is what makes the loop's seam clean.
inline float hermite4(float x0, float x1, float x2, float x3, float t) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

// Four taps around p at fractional position t, gathered with a signed stride so that reverse
// playback is the same code walking backwards.
template <int kCh>
inline float read4(const int16_t *p, int stride, float t) {
    return hermite4(tap<kCh>(p - stride), tap<kCh>(p), tap<kCh>(p + stride), tap<kCh>(p + 2 * stride), t);
}

// The exact integer position of the k-th sample of a run, used to verify the float estimate of
// how many samples fit in the safe interior. k <= 64 and the fractional step is under 2^24, so
// the whole thing stays inside 32 bits with no division and no 64-bit arithmetic.
inline int32_t run_end(int32_t pos, uint32_t frac, int32_t istep, uint32_t fstep, int k) {
    const uint32_t j = uint32_t(k - 1);
    return pos + int32_t(j) * istep + int32_t((frac + j * fstep) >> kFracBits);
}

// out += gain * in with gain walking by step each sample: the ramp a stolen voice fades out
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
// SRC
//   0  PIT  pitch, (knob - 64) * 24/63 semitones: exactly unity at 64, +24 at 127, -24.4 at 0.
//           The step is then PIT x the trig note's ratio x source_rate/48 kHz, clamped to
//           1/64..32 (six octaves down, five up) so the fixed-point step cannot overflow. With
//           SLC = 1 the trig note transposes from note 60, so a one-shot plays chromatically
//           across the pads; with SLC > 1 the note picks the slice instead and PIT is the only
//           pitch control.
//   1  STA  start, linear over the whole sample: 0 is frame 0 and 127 is the last frame. 127 is
//           therefore a legal way to ask for silence (a 1-frame window does not play).
//   2  LEN  length, linear over what is left after START, so 127 always means "to the end"
//           whatever START is, and 0 means silence. The window is [START, START+LEN).
//   3  SLC  slice count, 1-16 in equal zones. 1 is one-shot mode (the whole window, the note
//           transposes). Above 1 the window is cut into SLC equal slices and the trig note
//           selects one: note 60 is slice 0, 61 is slice 1, and notes past the last slice clamp
//           to it rather than wrapping, so a chopped loop lies across the pads in order.
// AMP
//   8  ATK  attack. 0 is none at all -- the first sample is at full level, because a one-shot's
//           transient IS the sound -- and 1-127 is 0.5 ms * 2^(11 * knob), so 0.5 ms to about 1 s.
//   9  DEC  decay to silence, 5 ms * 2^(11.6 * knob): 5 ms to about 16 s, and **127 means hold**
//           (sustain 1.0), which is the setting a one-shot wants: the sound ends because the
//           sample ends. Below 127 the decay runs from the trigger with sustain 0, so DEC
//           shortens a hit without editing LEN.
//  10  PLY  play mode, four zones of 32: 0-31 forward one-shot, 32-63 forward loop, 64-95
//           reverse one-shot, 96-127 reverse loop. Loop and reverse are one knob because SRC and
//           AMP have 8 slots between them and both of these are a switch, not a sweep. Looping
//           repeats the window (or the slice), which is what makes a loop track a loop track.
//  11  LVL  output level, square law, 0-1. Square gives usable control over the quiet end.

void Sampler::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = reciprocal(rate_);
    rate_ratio_ = source_rate_ * inv_rate_;
    for (Voice &v : voices_) {
        v = Voice{};
        v.amp.kill();
    }
    steal_cursor_ = 0;
    active_ = false;
    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void Sampler::prepare(const TrackParams &params) {
    // 64 is exactly unity, so a pattern written at the centre plays at the sample's own pitch.
    pitch_ratio_ = semitones((float(params.get(0)) - 64.0f) * (24.0f / 63.0f));
    start_unit_ = params.unit(1);
    length_unit_ = params.unit(2);
    slices_ = clampi(1 + int32_t(params.unit(3) * float(kSlicesMax - 1) + 0.5f), 1, kSlicesMax);
    recompute_window();

    // ATK 0 is NO attack: a shape whose attack is under one step has coefficient 1, so the first
    // sample is already at full level. That is what a one-shot wants -- the transient is the
    // sound -- so it is the default rather than something to dial in.
    const float attack_knob = params.unit(8);
    const float attack = attack_knob > 0.0f ? 0.0005f * exp2(11.0f * attack_knob) : 0.0f;
    const float decay = 0.005f * exp2(11.6f * params.unit(9));      // 5 ms to ~16 s
    // The times above are already times-to-1%, which is what EnvelopeShape::make wants
    // (docs/ENAGA_API.md, "Gotchas"): there is no reference tau here to multiply by ln(100).
    const float sustain = params.get(9) >= 127 ? 1.0f : 0.0f;       // DEC at the top = hold
    amp_shape_ = EnvelopeShape::make(attack, decay, sustain, kReleaseS, rate_);

    const int zone = int(params.get(10)) >> 5;    // four zones of 32: a shift, not a divide
    loop_ = (zone & 1) != 0;
    reverse_ = zone >= 2;

    const float level = params.unit(11);
    out_gain_ = level * level;
}

// START, LEN and SLC against the resident sample. Called from prepare() and from set_sample(),
// because the window is in frames and a new sample changes what a knob position means.
void Sampler::recompute_window() {
    window_start_ = 0;
    window_len_ = 0;
    slice_frames_ = 0.0f;
    play_len_ = 0;
    cuts_ = 0;
    if (!playable_) return;

    if (fifo_ != nullptr) {
        // A stream plays from its first frame, forward: no START, no slices, no reverse, because
        // each would need the stream to jump (set_stream, docs/FEATURES.md). LEN is an END point
        // and needs nothing of the card: the voice stops, or loops back to the head, that far
        // into the file. At the top it is exactly the file (a float cannot hold every length
        // past 2^24 frames, and the last frame is not to be lost to rounding).
        const int32_t total = int32_t(stream_total_);
        window_len_ = length_unit_ >= 1.0f
                          ? total
                          : clampi(int32_t(length_unit_ * float(total) + 0.5f), 0, total);
        slice_frames_ = float(window_len_);
        play_len_ = window_len_;
        return;
    }

    const int32_t last = frames_ - 1;
    window_start_ = clampi(int32_t(start_unit_ * float(last) + 0.5f), 0, last);
    const int32_t available = frames_ - window_start_;
    window_len_ = clampi(int32_t(length_unit_ * float(available) + 0.5f), 0, available);
    slice_frames_ = float(window_len_) * kInvSlices[slices_ - 1];
    play_len_ = clampi(int32_t(slice_frames_), 0, window_len_);

    // SLC AUTO: the window cut at the strongest slices_ - 1 points inside it, in place order.
    if (!auto_ || slices_ < 2 || points_.count == 0) return;
    const int32_t end = window_start_ + window_len_;
    int n = 0;
    cut_[n++] = window_start_;
    for (int i = 0; i < points_.count; i++) {
        const int32_t at = int32_t(points_.at[i]);
        if (int(points_.rank[i]) >= slices_ - 1 || at <= window_start_ || at >= end) continue;
        cut_[n++] = at;
    }
    if (n < 2) return;                 // no point inside the window: equal slices, as without
    cut_[n] = end;
    cuts_ = n;
}

void Sampler::set_slice_points(const SlicePoints &points) {
    points_ = points;
    if (points_.count > SlicePoints::kMax) points_.count = 0;
    recompute_window();
}

void Sampler::set_auto_slices(bool on) {
    auto_ = on;
    recompute_window();
}

int32_t Sampler::slice_start(int i) const {
    if (cuts_ > 0) return cut_[clampi(i, 0, cuts_ - 1)];
    return window_start_ + int32_t(float(clampi(i, 0, slices_ - 1)) * slice_frames_);
}

void Sampler::set_sample(const int16_t *data, uint32_t frames, int channels, float source_rate) {
    // The voices are reading the buffer that is being taken away, so they stop. A crossfade
    // between two samples belongs to the app, which has both buffers; the machine has one.
    kill_all();
    fifo_ = nullptr;
    stream_total_ = 0;
    points_ = SlicePoints{};           // the old sample's; the engine hands the new one's next

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
    recompute_window();
}

void Sampler::set_stream(const int16_t *head, uint32_t head_frames, uint32_t total_frames,
                         int channels, float source_rate, StreamFifo *fifo) {
    set_sample(head, head_frames, channels, source_rate);
    const bool ok = playable_ && fifo != nullptr && total_frames > head_frames &&
                    total_frames <= kMaxStreamFrames;
    if (!ok) {
        set_sample(nullptr, 0, 1, rate_);
        return;
    }
    fifo_ = fifo;
    stream_total_ = total_frames;
    fifo_->reset(channels_, uint32_t(frames_));
    recompute_window();
}

void Sampler::trigger(const Trig &trig) {
    // No sample, or a window too short for four taps: silent, and the machine never goes active.
    // docs/SPEC.md asks for exactly this -- a track whose sample is still loading stays silent.
    if (!playable_ || play_len_ < kMinFrames) return;

    // A stream has one voice: there is one stream behind it, and it reads on from one place.
    int pick = fifo_ != nullptr ? 0 : -1;
    for (int i = 0; pick < 0 && i < kVoices; i++) {
        if (!voices_[i].busy()) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        // Steal the quietest voice, which is the one furthest through its decay. The scan starts
        // at a rotating cursor so that voices held at the same level (DEC = hold, the usual
        // setting) are stolen in turn rather than always the first one.
        static_assert((kVoices & (kVoices - 1)) == 0, "the steal cursor masks instead of dividing");
        pick = steal_cursor_;
        float lowest = voices_[pick].amp.level();
        for (int n = 1; n < kVoices; n++) {
            const int i = (steal_cursor_ + n) & (kVoices - 1);
            const float level = voices_[i].amp.level();
            if (level < lowest) {
                lowest = level;
                pick = i;
            }
        }
        steal_cursor_ = (pick + 1) & (kVoices - 1);
    }

    Voice &v = voices_[pick];
    // A voice that is still loud is faded out first and the new note starts at the end of the
    // fade. Below kFadeFloor the splice is inaudible, so it starts straight away instead.
    if (v.fade == 0 && v.playing && v.amp.level() > kFadeFloor) {
        v.fade = kFadeFrames;
        v.gain = 1.0f;
    }
    v.next = trig;
    v.release_on_start = false;
    v.wait = trig.frame > 65535u ? 65535 : int(trig.frame);
    if (v.wait < v.fade) v.wait = v.fade;
    v.pending = true;
    active_ = true;
}

void Sampler::release_all() {
    // STOP once: let the envelope go, so what is sounding gets kReleaseS of tail instead of a
    // step to zero. A pending trig still starts -- it was scheduled before the stop.
    for (Voice &v : voices_) {
        if (v.playing) v.amp.gate_off();
    }
}

void Sampler::release(uint8_t note) {
    // `next` is the trig a voice is playing once it has started, and the one it is waiting for
    // before that.
    for (Voice &v : voices_) {
        if (v.next.note != note) continue;
        if (v.pending) v.release_on_start = true;
        else if (v.playing) v.amp.gate_off();
    }
}

void Sampler::kill_all() {
    for (Voice &v : voices_) {
        v.pending = false;
        v.wait = 0;
        silence(v);
    }
    active_ = false;
}

void Sampler::render_add(float *out, int frames) {
    if (!active_) return;      // nothing sounding and nothing pending: an idle machine costs a test
    if (!playable_) {
        // The sample went away between blocks (a swap, or a load that failed). Silence, now.
        kill_all();
        return;
    }
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

const char *Sampler::param_name(int index) const {
    static const char *const names[kParams] = {
        "PIT", "STA", "LEN", "SLC",      // SRC
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "ATK", "DEC", "PLY", "LVL",      // AMP
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- voices ---------------------------------------------------------------

void Sampler::start(Voice &v) {
    const Trig t = v.next;
    v.pending = false;
    v.wait = 0;
    v.fade = 0;
    v.gain = 1.0f;
    v.playing = false;
    if (!playable_ || play_len_ < kMinFrames) return;

    if (fifo_ != nullptr) {
        // The whole file, forward, from frame 0: the head is resident and the rest comes once
        // the engine has the stream reopened after it.
        v.note_ratio = clamp(semitones(float(int(t.note) - kRootNote)), kStepMin, kStepMax);
        v.len = play_len_;
        v.dir = 1;
        v.read0 = 0;
        v.loop = loop_;
        v.pos = 0;
        v.frac = 0;
        v.safe_lo = 0;
        v.safe_hi = -1;
        restart_stream();
        const float velocity = float(t.velocity) * (1.0f / 127.0f);
        v.level = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
        v.amp.gate_on(true);
        v.playing = true;
        if (v.release_on_start) {
            v.amp.gate_off();
            v.release_on_start = false;
        }
        return;
    }

    int32_t base = window_start_;
    int32_t len = play_len_;
    if (cuts_ > 0) {
        // SLC AUTO: the note picks a slice between two detected cuts, clamped as below.
        const int slice = clampi(int32_t(t.note) - kRootNote, 0, cuts_ - 1);
        base = cut_[slice];
        len = cut_[slice + 1] - base;
        v.note_ratio = 1.0f;
    } else if (slices_ > 1) {
        // The note picks a slice. Clamped, not wrapped: notes past the last slice hold on it,
        // which keeps a chopped loop in order across the pads and needs no integer modulo.
        const int32_t slice = clampi(int32_t(t.note) - kRootNote, 0, slices_ - 1);
        base = window_start_ + int32_t(float(slice) * slice_frames_);
        base = clampi(base, 0, frames_ - 1);
        if (base + len > frames_) len = frames_ - base;      // the last slice can be short
        v.note_ratio = 1.0f;
    } else {
        v.note_ratio = clamp(semitones(float(int(t.note) - kRootNote)), kStepMin, kStepMax);
    }
    if (len < kMinFrames) return;      // fewer frames than the interpolator has taps: silent

    v.len = len;
    v.dir = reverse_ ? -1 : 1;
    v.read0 = reverse_ ? base + len - 1 : base;
    v.loop = loop_;
    v.pos = 0;
    v.frac = 0;

    // The safe interior, in window positions: the range of pos over which all four taps are
    // inside the buffer. Forward the taps are base-1..base+2, so the read frame must be in
    // [1, frames-3]; reversed they are base+1..base-2, so it must be in [2, frames-2]. Outside
    // this range -- which is at most a couple of frames at each end of the BUFFER, and only when
    // the window reaches them -- generate() takes the clamped path instead.
    const int32_t last = frames_ - 1;
    int32_t lo, hi;
    if (v.dir > 0) {
        lo = 1 - v.read0;
        hi = last - 2 - v.read0;
    } else {
        lo = v.read0 - (last - 1);
        hi = v.read0 - 2;
    }
    v.safe_lo = lo < 0 ? 0 : lo;
    v.safe_hi = hi > len - 1 ? len - 1 : hi;

    const float velocity = float(t.velocity) * (1.0f / 127.0f);
    v.level = clamp(velocity * (t.accent ? 1.25f : 1.0f), 0.0f, 1.0f);
    v.amp.gate_on(true);
    v.playing = true;
    if (v.release_on_start) {
        v.amp.gate_off();
        v.release_on_start = false;
    }
}

void Sampler::silence(Voice &v) {
    v.amp.kill();
    v.playing = false;
    v.fade = 0;
    v.gain = 1.0f;
}

void Sampler::render_voice(Voice &v, float *out, int n) {
    int i = 0;
    while (i < n) {
        if (v.pending && v.wait == 0) {
            start(v);      // sample-accurate: the note begins on trig.frame
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

void Sampler::run(Voice &v, float *out, int n) {
    if (!v.playing) return;

    // Control rate, once per voice per block, because the trig note and PIT both move the step.
    // The fixed-point step is built here and nothing in the loop divides.
    const float step = clamp(rate_ratio_ * pitch_ratio_ * v.note_ratio, kStepMin, kStepMax);
    const int32_t istep = int32_t(step);
    const float fraction = (step - float(istep)) * kFracScale;
    const uint32_t fstep = uint32_t(int32_t(clamp(fraction, 0.0f, kFracScale - 1.0f)));
    const float inv_step = reciprocal(step);

    if (fifo_ != nullptr) {
        if (channels_ == 2) generate_stream<2>(v, istep, fstep, voice_, n);
        else generate_stream<1>(v, istep, fstep, voice_, n);
    } else if (channels_ == 2) {
        generate<2>(v, istep, fstep, inv_step, voice_, n);
    } else {
        generate<1>(v, istep, fstep, inv_step, voice_, n);
    }

    v.amp.render(env_, n, amp_shape_);
    const float gain = out_gain_ * v.level * src_scale_;
    if (v.fade > 0) {
        // The steal fade needs a walking gain, so the VCA and the mix are two passes here. Only
        // on the steal path, and only for kFadeFrames.
        enaga_multiply(voice_, env_, n);
        mix_ramp(out, voice_, n, gain * v.gain, -gain * kFadeStep);
        v.gain -= kFadeStep * float(n);
        if (v.gain < 0.0f) v.gain = 0.0f;
    } else {
        // The VCA and the mix into the track buffer in one pass over the block.
        enaga_mix_product(out, voice_, env_, n, gain);
    }

    // The window ending stops the voice; so does the envelope reaching zero, which is what says
    // a sustain-0 one-shot has finished (docs/ENAGA_API.md, "Gotchas").
    if (!v.playing || Voice::done(v.amp)) {
        v.playing = false;
        v.amp.kill();
    }
}

template <int kCh>
void Sampler::generate(Voice &v, int32_t istep, uint32_t fstep, float inv_step, float *dst, int n) {
    const int16_t *const src = data_;
    const int32_t last = frames_ - 1;
    const int stride = v.dir * kCh;
    const int istride = int(istep) * stride;

    int i = 0;
    while (i < n && v.playing) {
        if (v.pos < v.safe_lo || v.pos > v.safe_hi) {
            // The guarded path. Every tap index is clamped into the buffer, so the edge sample
            // is held rather than read past -- an out-of-bounds read here does not fault, it
            // returns garbage, and a crackle nobody can trace is the worst outcome in this file.
            // A handful of samples per note and per loop wrap, so the clamps are never hot.
            const int32_t base = clampi(v.read0 + v.dir * v.pos, 0, last);
            const int32_t i0 = clampi(base - v.dir, 0, last);
            const int32_t i2 = clampi(base + v.dir, 0, last);
            const int32_t i3 = clampi(base + 2 * v.dir, 0, last);
            const float t = float(int32_t(v.frac)) * kInvFracScale;
            dst[i++] = hermite4(tap<kCh>(src + i0 * kCh), tap<kCh>(src + base * kCh),
                                tap<kCh>(src + i2 * kCh), tap<kCh>(src + i3 * kCh), t);
        } else {
            // The interior. How many samples fit before pos leaves the safe range: a float
            // estimate (float division is hardware, integer division does not exist on this
            // core), then an exact integer check, because enaga_reciprocal is only good to 2e-7
            // and "never read outside the buffer" is not a 2e-7 kind of rule.
            const int room = n - i;
            const float estimate = float(v.safe_hi - v.pos - 1) * inv_step + 1.0f;
            int k = estimate >= float(room) ? room : (estimate >= 1.0f ? int(estimate) : 1);
            while (k > 1 && run_end(v.pos, v.frac, istep, fstep, k) > v.safe_hi) k >>= 1;

            // The run itself is a cl6x loop (dsp/sampleloops.ti.c): the same read4 and the same
            // stepping as the loop that used to be here, which GCC compiled in every build. The
            // last sample is read without advancing, so the pointer is never walked past the
            // final valid tap.
            const int16_t *p = src + (v.read0 + v.dir * v.pos) * kCh;
            if constexpr (kCh == 2) {
                strata_sample_read_stereo(dst + i, k, p, stride, istride, &v.frac, fstep, &v.pos,
                                          istep);
            } else {
                strata_sample_read_mono(dst + i, k, p, stride, istride, &v.frac, fstep, &v.pos,
                                        istep);
            }
            i += k;
        }

        // Advance past the sample just emitted, then wrap or stop. Shift and mask, never a
        // divide; the wrap subtracts because a step is at most 32 frames and a window at least 4.
        v.frac += fstep;
        v.pos += istep + int32_t(v.frac >> kFracBits);
        v.frac &= kFracMask;
        if (v.pos >= v.len) {
            if (v.loop) {
                while (v.pos >= v.len) v.pos -= v.len;
            } else {
                v.playing = false;
            }
        }
    }
    while (i < n) dst[i++] = 0.0f;      // the window ended inside this slice
}

// --- the stream voice -------------------------------------------------------
//
// One voice, a per-sample tap fetch with a branch: the head, the fifo, or silence when the card
// is late. Not the interior fast path generate() has, and it does not need one: there is one
// such voice per streamed track, against generate()'s four, and the taps are a handful of
// compares on data already in L1D. Not bit-exact with a whole-loaded file either, and it need
// not be: a file is streamed only when it cannot be loaded whole.

void Sampler::restart_stream() {
    // Back to frame 0: the head plays from RAM while the engine has the stream reopened at the
    // head's end, and whatever the fifo held from the last pass is stale.
    fifo_->reset(channels_, uint32_t(frames_));
    restarts_++;
}

template <int kCh>
float Sampler::stream_tap(int32_t frame) {
    const int32_t last = int32_t(stream_total_) - 1;
    const int32_t f = clampi(frame, 0, last);
    if (f < frames_) return tap<kCh>(data_ + f * kCh);
    if (fifo_->has(uint32_t(f))) return tap<kCh>(fifo_->frame(uint32_t(f)));
    late_++;
    return 0.0f;
}

template <int kCh>
void Sampler::generate_stream(Voice &v, int32_t istep, uint32_t fstep, float *dst, int n) {
    int i = 0;
    while (i < n && v.playing) {
        const float t = float(int32_t(v.frac)) * kInvFracScale;
        const int32_t p = v.pos;
        dst[i++] = hermite4(stream_tap<kCh>(p - 1), stream_tap<kCh>(p), stream_tap<kCh>(p + 1),
                            stream_tap<kCh>(p + 2), t);
        v.frac += fstep;
        v.pos += istep + int32_t(v.frac >> kFracBits);
        v.frac &= kFracMask;
        if (v.pos >= v.len) {
            if (v.loop) {
                while (v.pos >= v.len) v.pos -= v.len;
                // A loop shorter than the head never leaves RAM: nothing to reopen.
                if (v.len > frames_) restart_stream();
            } else {
                v.playing = false;
            }
        }
    }
    while (i < n) dst[i++] = 0.0f;
    // Everything before the next sample's first tap is done with, including frames that have
    // not arrived yet: they will be thrown away when they do (core/streamfifo.hpp).
    if (v.pos > 1) fifo_->drop_before(uint32_t(v.pos - 1));
}

}  // namespace strata
