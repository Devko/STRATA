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
// --- the GRN pages beyond SRC and AMP -----------------------------------------------------------
//
// GRAIN reads the ASR page and the OSC page as well, with its own meanings (core/project.hpp,
// machine_reads_asr), the way FM4 does. Every one of them at 0 is the machine as it was before
// it read them, which is what a kit from before then loads as:
//
//   ASR  ATK SUS REL ---   the cloud's envelope. ATK 0 is no attack, SUS 0 leaves DEC in charge
//                          (127 holds, anything less fades), REL 0 is the old fixed 180 ms tail.
//                          With SUS up the cloud holds at SUS while a KEYS pad is down or the
//                          step's LEN lasts, then fades over REL: a pad.
//   OSC  SCN PSP --- ---   SCN moves the read head through the sample while the note sounds, from
//                          still (0, the old machine) to twice real time: a time-stretch, at any
//                          PIT. Each trig starts it again from POS. PSP sprays the pitch of each
//                          grain: 1-63 a random detune of up to a semitone either way, 64-127 up
//                          to one grain in two playing an octave up (a shimmer).
//
// SCN is not tempo-locked, and does not need to be: a tempo-locked read head is still POS under a
// per-step lock or a synced LFO (docs/SPEC.md, "Parameters"). SCN is the free one, for textures
// that drift through a sample, and for a stretched loop whose length is set by ear.
//
// Neither costs anything in the inner loop. SCN is one add per slice; PSP picks each grain's own
// step at its onset, and a grain already carries its own step (strata_grain), so the loop is
// unchanged and the bounds argument above holds grain by grain.
//
// --- what is deliberately NOT here ------------------------------------------------------------
//
// **No reverse grains.** Not in the SPEC's GRAIN row, and a negative stride doubles every case in
// the bounds argument above for a sound that SPR and SIZ already reach past recognition.
//
// **Machines are MONO** (dsp/track.hpp): pan and stereo spread are the insert chain's, which is
// what lets this machine sum a stereo source to mono for free in the tap gather.
//
// It reads the SRC page (0-3), the AMP page (8-11), ATK SUS REL on the ASR page and SCN PSP on
// the OSC page; FLTR and FX belong to the insert chain.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "grainloops.h"
#include "overview.hpp"
#include "track.hpp"

namespace strata {

// GRAIN's knob curves, for the screen (app/app.hpp) as well as for `prepare`, and pinned by
// tests/test_grain.cpp. dsp/grain.cpp has the table that says why each one is shaped as it is.
float grain_size_seconds(uint8_t value);     // SIZ: 8 ms at 0, 680 ms at 127, exponential
float grain_overlap(uint8_t value);          // DEN: grains in the air, 0.25 at 0 to 8 at 127
float grain_spray_seconds(uint8_t value);    // SPR: position jitter, square law, 0 to 0.5 s
int grain_pitch_semitones(uint8_t value);    // PIT: whole semitones, 0 at 64, -24 to +24
constexpr uint8_t kGrainDecayHold = 127;     // DEC at the top holds
float grain_decay_seconds(uint8_t value);    // DEC: 5 ms to about 16 s below the top
float grain_attack_seconds(uint8_t value);   // ATK: 0 none, then 2.5 ms to 5 s
float grain_release_seconds(uint8_t value);  // REL: 180 ms at 0 (the old fixed tail) to 10 s
float grain_scan_rate(uint8_t value);        // SCN: x real time, 0 still, 1 at 90, 2 at 127
// PSP's two zones: off at 0, a random detune 1-63, an octave-up chance 64-127.
enum class GrainSpray : uint8_t { off = 0, detune, octave };
GrainSpray grain_pitch_spray_mode(uint8_t value);
float grain_detune_cents(uint8_t value);     // in DET's zone: the widest detune either way
float grain_octave_chance(uint8_t value);    // in OCT's zone: the share of grains an octave up

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
    // VariSpeed (Machine::set_pitch_mod): the read step is built at control rate in prepare, so
    // a change rebuilds it here; every grain spawned from then on reads at the new rate.
    void set_pitch_mod(float semis) override {
        Machine::set_pitch_mod(semis);
        recompute_step();
    }

    // Hand the machine already-resident PCM. See the ownership rules at the top of this file.
    // Passing nullptr (or frames < kMinFrames, or channels outside 1-2) leaves the track silent.
    void set_sample(const int16_t *data, uint32_t frames, int channels, float source_rate);

    // The CPU guard's grain pool (core/cpuguard.hpp, applied by the engine): at most `limit`
    // grains in the air, 1 to kGrains. Below kGrains a spawn into a pool already holding `limit`
    // is SKIPPED rather than stealing -- a steal splices a grain mid-window, a skip only thins
    // the cloud -- and grains already in the air past the limit live out their windows, so
    // lowering it never cuts a sound off. At kGrains (the default) nothing changes at all.
    static constexpr int kPoolSize = 8;   // grains in the pool; kGrains below
    void set_grain_limit(int limit) {
        grain_limit_ = limit < 1 ? 1 : (limit > kGrains ? kGrains : limit);
    }
    int grain_limit() const { return grain_limit_; }

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
    // SCN: how far the read head has moved from POS since the note began, in frames, and the
    // rate it moves at, in source frames per output sample.
    int scan_offset() const { return scan_whole_; }
    float scan_step() const { return scan_rate_; }
    // ASR, as the last prepare() resolved it: whether the cloud holds at SUS while gated.
    bool sustains() const { return sustains_; }

    // Where this machine is in its sample, in the screen overview's columns (core/overview.hpp):
    // the bracket the next grain starts in (POS plus SCN's travel, one grain's span wide), and
    // each live grain with its window times the cloud's envelope as its brightness. Audio
    // context, called by the engine every few blocks.
    void wave_view(WaveView &view) const;
    // The cloud's envelope.
    bool envelope_now(uint8_t &stage, float &level) const override {
        if (!active_) return false;
        stage = uint8_t(amp_.stage());
        level = amp_.level();
        return true;
    }

    static constexpr int kGrains = kPoolSize;
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
    // And its ceiling: four longest grains, so DEN's lowest overlap (0.25) holds at every SIZ.
    static constexpr int kHopMax = 4 * STRATA_GRAIN_LEN_MAX;
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
    void advance_scan(int n);
    void render_slice(float *out, int n);
    void run(float *out, int n);
    bool scheduling() const { return voice_on_ && playable_; }
    // The largest length, in output samples, whose every read from `start` stays inside the
    // buffer. Exact: see "how a grain is bounded" above.
    int32_t fit_length(int32_t start, int32_t want, int32_t istep, uint32_t fstep,
                       float inv_step) const;

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
    float view_scale_ = 0.0f;      // kWaveColumns / frames_: wave_view

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
    float scan_speed_ = 0.0f;      // SCN, times real time
    float scan_rate_ = 0.0f;       // and in source frames per output sample
    float scan_frac_ = 0.0f;       // the read head's offset from POS: its fraction of a frame
    int32_t scan_whole_ = 0;       // and its whole frames, kept inside [0, frames_)
    GrainSpray spray_mode_ = GrainSpray::off;   // PSP
    float detune_semis_ = 0.0f;    // PSP's detune zone: the widest detune, in semitones
    float octave_chance_ = 0.0f;   // PSP's octave zone: the share of grains an octave up
    float octave_acc_ = 0.0f;      // and its error-diffusion accumulator
    bool sustains_ = false;        // SUS up: hold at SUS while gated, release at LEN's end
    bool soft_attack_ = false;     // ATK up: a retrigger rises from where the cloud is

    float step_ = 1.0f;            // the read step, and its fixed-point and reciprocal forms
    int32_t istep_ = 1;
    uint32_t fstep_ = 0;
    float inv_step_ = 1.0f;

    float note_ratio_ = 1.0f;      // the trig note's pitch ratio, latched at the voice's start
    float level_ = 1.0f;           // velocity and accent, latched with it
    int32_t hop_countdown_ = 0;    // samples until the next grain is spawned
    int steal_cursor_ = 0;         // rotates, so equally advanced grains are stolen in turn
    int grain_limit_ = kGrains;    // the CPU guard's pool size, set_grain_limit
    int wait_ = 0;                 // frames until `next_` starts
    bool pending_ = false;
    bool release_on_start_ = false;   // released before its frame: let go as soon as it starts
    bool voice_on_ = false;        // the scheduler is running: gate on, or releasing
    int32_t gate_ = -1;            // frames left of this note's LEN with SUS up, -1 for none
    bool one_shot_ = false;        // a note no LEN or key-up will end
    bool active_ = false;
    Trig next_{};

    float window_[kWindowSize + kWindowGuard] = {};
    float scratch_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
