// GRAIN: granular playback of already-resident PCM. The costliest voice in STRATA.
//
// docs/SPEC.md's GRAIN row is *"Granular playback, tempo-locked stretch: position, size, density,
// spray, pitch"*, one voice of up to 8 grains, and docs/BUDGET.md prices a grain at 2,690 cycles
// a block -- as much as a whole SAMPLE voice, eight times over. 21,520 cycles is 4.3% of the
// block and the largest single line in the project's worst case. So unlike every other machine in
// dsp/, here the cost IS the design: every decision below was taken with "times eight" in front
// of it, and the ones that were not worth eight times are named as such.
//
// **This machine does not load files**, exactly as dsp/sample.hpp says of SAMPLE. Loading is
// `app/`'s: the loader fills a buffer in the main context and posts a swap command, and the audio
// side hands the resident PCM to `set_sample`. The machine holds that pointer and nothing else,
// so `init()` allocates no PCM at all -- only its own fixed scratch and window table.
//
// The ownership rules are SAMPLE's, unchanged, because the same loader serves both and a second
// contract would be a second thing to get wrong (dsp/sample.hpp has the full argument):
//
//   - `set_sample(data, frames, channels, source_rate)` is called from the AUDIO context between
//     blocks -- where the block's commands are applied, before `prepare()` -- or with the audio
//     side stopped. It is not safe from the main context while `render_add` runs.
//   - `frames` is frames PER CHANNEL, so the buffer holds `frames * channels` int16. Only 1 and 2
//     channels are supported; anything else is refused and the track stays silent.
//   - The buffer stays alive until a later `set_sample` has been applied AND one further block has
//     been rendered: the spec's generation-counter handoff, where the loader frees the old buffer
//     only after the status ring confirms the swap.
//   - `set_sample` kills every live grain, because they are reading the buffer being taken away.
//     A crossfade between two samples is the app's business; it has both buffers, the machine has
//     one.
//   - `data == nullptr`, or a sample shorter than `kMinFrames`, is a legal and expected state: the
//     track is silent and `active()` is false. docs/SPEC.md asks for exactly this -- *"a track
//     whose sample is still loading stays silent rather than glitching"* -- so a null pointer is
//     never a special case to be avoided, it is the behaviour.
//
// --- how a grain is bounded ------------------------------------------------------------------
//
// This is the only part of the file that is about correctness rather than cost, and it is the part
// to read first. There is no interpolated read anywhere in Enaga's library (docs/ENAGA_API.md
// says so), so a grain plays its own taps, and **an out-of-range read on this chip does not fault,
// it silently returns garbage.** The policy is one sentence: *a grain's length is settled exactly
// at its onset, and nothing in the per-sample loop ever checks a bound again.*
//
//   1. At onset the grain's read step is already fixed point (32.16), so the frame it reads after
//      `j` samples is exactly `start + strata_grain_advance(j, istep, fstep)` -- the same integer
//      arithmetic the loop accumulates, which is why that function lives beside the loop in
//      dsp/grainloops.h rather than here.
//   2. The grain wants `grain_length()` output samples. Its span is computed exactly, and if the
//      whole grain fits somewhere in the buffer the START SLIDES BACK until it does, so POS at the
//      top of its range plays the last grain of the sample instead of going silent.
//   3. Only when the buffer cannot hold a whole grain is the LENGTH cut instead. The cut uses
//      `enaga_reciprocal` to pick where to start looking and then walks down by exact integer
//      steps, because `enaga_reciprocal` is good to about 2e-7 and "never read outside the buffer"
//      is not a 2e-7 kind of rule (docs/ENAGA_API.md, "Gotchas"). The float estimate can only be
//      high by rounding, so the walk is one or two steps.
//   4. A sample shorter than `kMinFrames`, or a grain the buffer can only support for fewer than
//      `kMinGrain` samples, does not play. So a 1-frame sample, a 2-frame sample and a POS at the
//      very last frame of a tiny buffer are all silent rather than special cases inside the loop.
//
// The window table is bounded the other way -- a clamp on its phase in the loop, plus two guard
// entries, so that every index that clamp can produce is in range whatever the float error did.
// Its phase is a float accumulator, and float error is not something an integer check can settle;
// dsp/grainloops.h has the argument for padding over clamping.
//
// What this buys: the inner loop has no bounds work at all. A per-sample clamp of both tap indices
// would be four selects a sample a grain -- over a thousand a block at eight grains -- to re-prove
// something the onset already settled exactly. dsp/sample.cpp splits its loop into a guarded path
// and an unguarded interior for the same reason; a grain is short enough that the guarded path can
// be dispensed with entirely by trimming the grain instead.
//
// --- the decisions that cost the cycles -------------------------------------------------------
//
// **Linear interpolation, not SAMPLE's Catmull-Rom.** Four taps at eight grains is 32 int16 loads
// an output sample, and dsp/sample.cpp's own cost note shows that machine is already memory-bound
// on four taps for ONE voice. A grain is a few milliseconds long under a window that is zero at
// both ends, so what linear gives away is spread under a window whose own spectral skirt is wider
// than the error -- and spread eight times over by the overlap. This is the single biggest saving
// in the file and the one most likely to be questioned later: the answer is that quality here is
// bought with grain count and window shape, not with tap count.
//
// **One Hann table with a warped phase, not a table per shape.** SHP moves the window's peak from
// the middle to the very start. Warping the phase into a single table costs two flops and a
// select; and because a Hann window's slope at its peak is zero, the kink the warp leaves in the
// phase does not reach the output. Two tables crossfaded would have cost four loads and three
// lerps a sample.
//
// **The voice envelope is applied to the SUM of the grains**, not to each grain, so it is one
// `Envelope::render` and one `enaga_mix_product` a block however many grains sound. Each grain
// carries only the velocity of the note that spawned it, latched, so a retrigger at a new velocity
// does not rescale a cloud that is already in the air.
//
// **A grain's gain is 1 / sqrt(how many grains it was born into**, `kCrowdGain`). Grains taken
// from nearby positions are partly coherent, so neither 1/N (right for identical signals) nor 1
// (right for independent ones) holds across the SPR range; the geometric mean of the two is the
// usual compromise, and it keeps a cloud's level inside about 9 dB as DEN sweeps its whole range.
// It is latched per grain rather than applied to the sum because a shared gain derived from the
// live count would step every time a grain was born or died -- a few per block, each a few per
// cent, which is zipper noise on a sustained cloud. A gain that never moves once a grain is in the
// air cannot zipper, and a burst of triggers gets quieter grains instead of a louder cloud.
//
// **The pool, not a cap derived from DEN, is what a spawn checks.** A DEN-derived cap would have
// to steal the moment a retrigger arrived, and what it would steal is a grain that had only just
// started -- its window still near its loudest, which is the worst splice available. A long grain
// under sixteenth notes is an ordinary way to use this machine, and a cap made it click on every
// step. With the pool, a steal happens only when all eight are busy, and the one taken is the one
// furthest through its window, where the window is near zero. The price is the ceiling: eight
// windows of at most 1 with gains of at most 1, so the hard bound is kGrains times the output trim
// rather than something tighter. grain.cpp's kHeadroom is calibrated on the loudest case the way
// dsp/va.cpp's is, which puts that hard bound at 4 and the measured worst case -- a full-scale
// constant sample, swept and hammered -- at about 1.6. tests/test_grain.cpp pins both.
//
// --- what is deliberately NOT here ------------------------------------------------------------
//
// **No scan rate, and so no tempo.** docs/SPEC.md asks for tempo-locked stretch, and `Machine`
// (dsp/track.hpp) hands a machine nothing but its 16 parameters and a `Trig` -- no tempo, no tick.
// That is not an omission to work around: STRATA already has a tempo-locked way to move a read
// head, which is POS under a per-step parameter lock or a tempo-synced LFO (docs/SPEC.md,
// "Parameters"). Both are tempo-locked by construction, both are one byte a step, and neither
// needs this machine to know what the tempo is or whether the clock is external. A SCN parameter
// would duplicate them and would need the one free parameter slot that SHP holds.
//
// **No reverse grains.** Not in the SPEC's GRAIN row, and a negative stride doubles every case in
// the bounds argument above for a sound that SPR and SIZ already reach past recognition.
//
// **Machines are MONO** (dsp/track.hpp): pan and stereo spread are the insert chain's, which is
// what lets this machine sum a stereo source to mono for free in the tap gather.
//
// It reads the SRC page (0-3) and the AMP page (8-11); FLTR and FX belong to the insert chain.
// That is 8 parameters, which is what `Machine::used_params()` already returns, so this machine
// does not override it.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "grainloops.h"
#include "track.hpp"

namespace strata {

class Granular final : public Machine {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void release(uint8_t note) override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // Hand the machine already-resident PCM. See the ownership rules at the top of this file.
    // Passing nullptr (or frames < kMinFrames, or channels outside 1-2) leaves the track silent.
    void set_sample(const int16_t *data, uint32_t frames, int channels, float source_rate);

    // For the UI and for tests: what the machine is currently holding.
    const int16_t *sample_data() const { return data_; }
    int sample_frames() const { return frames_; }
    int sample_channels() const { return channels_; }
    bool has_sample() const { return playable_; }

    // For the UI's voice count (docs/SPEC.md publishes one every block) and for tests: how many
    // grains are in the air. Never more than kGrains -- a spawn into a full pool steals.
    int live_grains() const {
        int n = 0;
        for (const Grain &g : grains_) n += g.live ? 1 : 0;
        return n;
    }

    // 1 / sqrt(k + 1): the gain a grain latches when it is born into a cloud that already holds k
    // grains. Public because the UI shows a cloud's level and the tests pin the curve; the
    // literals are written out because there is no libm to build them with and a spawn should not
    // run two polynomial evaluations to find a constant.
    static constexpr float kCrowdGain[8] = {
        1.00000000f, 0.70710678f, 0.57735027f, 0.50000000f,
        0.44721360f, 0.40824829f, 0.37796447f, 0.35355339f,
    };

    // The i-th grain's per-sample state, whether it is sounding, and how many samples it has left.
    // For a UI that draws where in the sample each grain is reading, and for tests that check the
    // read bound as ARITHMETIC on a grain's own state rather than inferring it from the output --
    // which is the only way to see a read of an out-of-range frame that happens to land with an
    // interpolation weight of zero, and so contributes nothing to hear.
    bool grain_live(int index) const {
        return index >= 0 && index < kGrains && grains_[index].live;
    }
    int grain_remaining(int index) const {
        return index >= 0 && index < kGrains ? int(grains_[index].left) : 0;
    }
    const strata_grain &grain_state(int index) const {
        return grains_[index >= 0 && index < kGrains ? index : 0].s;
    }

    // What the last prepare() resolved the knobs to, for the UI's readouts and for tests that pin
    // the mapping. Position is a frame in the sample; the two lengths are in OUTPUT samples.
    int position_frames() const { return position_; }
    int grain_length() const { return grain_len_; }
    int hop_length() const { return hop_; }
    int spray_frames() const { return spray_; }
    // The read step at the root note, in source frames per output sample: PIT times the source's
    // own rate ratio. 1.0 plays the sample at its own pitch.
    float read_step() const { return step_; }

    static constexpr int kGrains = 8;
    // Linear interpolation needs two frames, but a buffer that small is not a sample. Four, as
    // dsp/sample.cpp uses, makes "too short to play" one testable state instead of a family of
    // degenerate ones.
    static constexpr int kMinFrames = 4;
    // Index math stays in int32 -- 67M frames is 128 MiB mono, twice the whole sample budget.
    static constexpr int kMaxFrames = 1 << 26;
    // Note 60 is the untransposed pitch, the same root the other machines use.
    static constexpr int kRootNote = 60;
    // The longest grain, in output samples, and the shortest worth spawning. The maximum is the
    // one that keeps strata_grain_advance exact in 32 bits (dsp/grainloops.h); grain.cpp
    // static_asserts that.
    static constexpr int kGrainMax = STRATA_GRAIN_LEN_MAX;
    static constexpr int kMinGrain = 4;

private:
    // The slice render_add works in, so every scratch buffer is a fixed member array: nothing is
    // allocated and nothing large goes on the stack. ENAGA_BLOCK_FRAMES is 64; a longer call is
    // cut into slices of this size.
    static constexpr int kBlock = 64;
    // One cycle of Hann, then two guard entries equal to the first. dsp/grainloops.h says why two:
    // with both present, every index the loop's clamped phase can produce is in range, so the loop
    // needs no index clamp of its own.
    static constexpr int kWindowSize = STRATA_GRAIN_WINDOW;
    static constexpr int kWindowGuard = 2;
    // The scheduler's hop floor. It bounds how many grains can be spawned in one block, which
    // bounds the control-rate work the sub-slicing in render_slice can be asked to do, and it is
    // what guarantees render_slice makes progress: a hop of zero would not terminate.
    static constexpr int kHopMin = 8;
    // The fixed-point step's limits: six octaves down, five up. Clamping here is what keeps the
    // step inside its word and keeps strata_grain_advance's products inside 32 bits.
    static constexpr float kStepMin = 1.0f / 64.0f;
    static constexpr float kStepMax = 32.0f;

    struct Grain {
        strata_grain s{};   // the per-sample state dsp/grainloops.h's loops work on
        int32_t left = 0;   // output samples still to render
        bool live = false;
    };

    void recompute_position();
    void recompute_step();
    void start_voice();
    void spawn();
    void render_slice(float *out, int n);
    void run(float *out, int n);
    bool scheduling() const { return voice_on_ && playable_; }
    // The largest length, in output samples, whose every read from `start` stays inside the
    // buffer. Exact: see "how a grain is bounded" above.
    int32_t fit_length(int32_t start, int32_t want) const;

    // An Envelope with sustain 0 parks in the `sustain` stage rather than reaching `idle`
    // (docs/ENAGA_API.md, "Gotchas"), so the level is what says the voice has finished. The
    // attack is excluded because it starts from zero.
    static bool done(const enaga::dsp::Envelope &e) {
        const enaga::dsp::Envelope::Stage s = e.stage();
        return s == enaga::dsp::Envelope::idle ||
               (s != enaga::dsp::Envelope::attack && e.level() < 1e-4f);
    }

    Grain grains_[kGrains];
    enaga::dsp::Envelope amp_;
    enaga::dsp::EnvelopeShape amp_shape_{};
    enaga::dsp::Random random_{0x9E3779B9u};   // spray and hop jitter; fixed seed, so tests repeat

    // The resident PCM, owned by the caller.
    const int16_t *data_ = nullptr;
    int32_t frames_ = 0;           // frames per channel, 0 when nothing is playable
    int channels_ = 1;
    bool playable_ = false;
    float source_rate_ = 48000.0f;
    float rate_ratio_ = 1.0f;      // source_rate / sample_rate: the step at PIT 64 and note 60
    float src_scale_ = 1.0f / 32768.0f;   // int16 to float, with the mono/stereo sum folded in

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;

    float position_unit_ = 0.0f;   // POS as read, so set_sample can redo the position
    int32_t position_ = 0;         // POS resolved against the sample, in frames
    int32_t grain_len_ = 240;      // SIZ, in output samples
    int32_t hop_ = 120;            // DEN against SIZ: samples between onsets
    int32_t spray_ = 0;            // SPR, in frames
    float spray_unit_ = 0.0f;      // SPR as read, for the hop jitter
    float pitch_ratio_ = 1.0f;     // PIT
    float peak_ = 0.5f;            // SHP: where the window's peak sits
    float rise_ = 1.0f;            // 0.5 / peak_, hoisted so the loop never divides
    float fall_ = 1.0f;            // 0.5 / (1 - peak_)
    float out_gain_ = 0.25f;       // LVL

    float step_ = 1.0f;            // the read step, and its fixed-point and reciprocal forms
    int32_t istep_ = 1;
    uint32_t fstep_ = 0;
    float inv_step_ = 1.0f;

    float note_ratio_ = 1.0f;      // the trig note's pitch ratio, latched at the voice's start
    float level_ = 1.0f;           // velocity and accent, latched with it
    int32_t hop_countdown_ = 0;    // samples until the next grain is spawned
    int steal_cursor_ = 0;         // rotates, so equally advanced grains are stolen in turn
    int wait_ = 0;                 // frames until `next_` starts
    bool pending_ = false;
    bool release_on_start_ = false;   // released before its frame: let go as soon as it starts
    bool voice_on_ = false;        // the scheduler is running: gate on, or releasing
    bool active_ = false;
    Trig next_{};

    float window_[kWindowSize + kWindowGuard] = {};
    float scratch_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
