// SAMPLE: one-shot and slice playback of already-resident PCM.
//
// docs/SPEC.md, "Sound architecture": *"One-shot or slice playback from RAM: pitch, start,
// length, loop, reverse, 4-point interpolation"*, 4 voices. Track 8 of the default kit is the
// loop track, and this is the one machine the stock firmware also does, so it is the one that
// has to be solid.
//
// **This machine does not load files.** Loading is M4 and belongs to `app/`: the loader fills a
// buffer in the main context and posts a swap command, and the audio side hands the resident PCM
// to `set_sample`. The machine holds that pointer and nothing else -- it never copies, allocates
// or frees PCM, so `init()` allocates nothing at all.
//
// Ownership rules a caller must obey (docs/SPEC.md, "Samples", is the design this follows):
//
//   - `set_sample(data, frames, channels, source_rate)` is called from the AUDIO context between
//     blocks -- that is, where the block's commands are applied, before `prepare()` -- or with
//     the audio side stopped. It is not safe to call it from the main context while `render_add`
//     is running, because it is several words of state and there are no atomics to spare.
//   - `frames` is frames PER CHANNEL. The buffer therefore holds `frames * channels` int16, and
//     only 1 and 2 channels are supported; anything else is refused and the track stays silent.
//   - The buffer stays alive until a later `set_sample` has been applied AND one further block
//     has been rendered. That is exactly the spec's generation-counter handoff: the loader frees
//     the old buffer only after the status ring confirms the swap.
//   - `set_sample` stops every sounding voice, because they are reading the buffer that is being
//     taken away. A crossfade between two samples is the app's business, not the machine's.
//   - `data == nullptr` (or a sample shorter than `kMinFrames`) is a legal, expected state: the
//     track is silent and `active()` is false. The spec asks for exactly this -- *"a track whose
//     sample is still loading stays silent rather than glitching"* -- so a null pointer is never
//     a special case to be avoided, it is the behaviour.
//
// --- 4-point interpolation and the buffer edges -----------------------------------------------
//
// Playback is Catmull-Rom (cubic Hermite) over four taps: x[-1], x[0], x[1], x[2] around the
// read position. An out-of-bounds read on this chip does not fault, it silently returns garbage,
// so the edges are handled deliberately and in three layers:
//
//   1. The window `[base, base + len)` is clamped into `[0, frames)` at trigger time, so the
//      BASE tap is always inside the buffer whatever the parameters say.
//   2. The integer read position is split into a safe interior, where all four taps are in
//      range, and up to a few samples at each end where one is not. The interior runs in a tight
//      loop with no bounds work at all; the ends run through a per-sample path that clamps every
//      tap index (hold-the-edge-sample, the usual choice -- it cannot ring and costs nothing
//      because it runs a handful of times per note or per loop wrap).
//   3. A sample or a window shorter than `kMinFrames` = 4 frames -- fewer frames than the
//      interpolator has taps -- does not play at all. So a 1-frame sample, a zero LEN and a
//      START at the very last frame are all silent rather than special cases inside the loop.
//
// Reverse is the same code with a negative stride: the position still counts up from the window
// start, and `dir` decides which way the four taps are gathered.
//
// It reads the SRC page (0-3) and the AMP page (8-11); FLTR and FX belong to the insert chain.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

class Sampler final : public Machine {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // Hand the machine already-resident PCM. See the ownership rules at the top of this file.
    // Passing nullptr (or frames < kMinFrames, or channels outside 1-2) leaves the track silent.
    void set_sample(const int16_t *data, uint32_t frames, int channels, float source_rate);

    // For the UI's voice count (docs/SPEC.md publishes one every block) and for tests.
    int playing_voices() const {
        int n = 0;
        for (const Voice &v : voices_) n += v.playing ? 1 : 0;
        return n;
    }

    // For the UI and for tests: what the machine is currently holding.
    const int16_t *sample_data() const { return data_; }
    int sample_frames() const { return frames_; }
    int sample_channels() const { return channels_; }
    bool has_sample() const { return playable_; }

    // The window the next trigger will use, in frames, after START, LEN and the slice division.
    // For the UI's waveform display and for tests that pin the mapping.
    int window_start() const { return window_start_; }
    int window_length() const { return window_len_; }
    int slices() const { return slices_; }
    int play_length() const { return play_len_; }

    static constexpr int kVoices = 4;
    static constexpr int kSlicesMax = 16;
    // Four taps need four frames. Shorter than this and there is nothing to interpolate, so
    // nothing plays: see "4-point interpolation and the buffer edges" above.
    static constexpr int kMinFrames = 4;
    // Index math stays in int32 -- 67M frames is 128 MiB mono, twice the whole sample budget.
    static constexpr int kMaxFrames = 1 << 26;
    // Note 60 is slice 0 and the untransposed pitch, the same root the drum machines use.
    static constexpr int kRootNote = 60;

private:
    // The slice render_add works in, so every scratch buffer is a fixed member array: nothing is
    // allocated and nothing large goes on the stack. ENAGA_BLOCK_FRAMES is 64; a longer call is
    // cut into slices of this size.
    static constexpr int kBlock = 64;
    // The declick fade a stolen voice gets, and the level below which it is not worth one.
    static constexpr int kFadeFrames = 24;              // 0.5 ms at 48 kHz
    static constexpr float kFadeStep = 1.0f / float(kFadeFrames);
    static constexpr float kFadeFloor = 0.01f;

    // The read position is 32.24 fixed point in two words: an int32 frame index and a 24-bit
    // fraction. The C674x has no integer divide at all, so the advance is an add, a shift and a
    // mask, and 24 bits of fraction is 6e-8 of a frame -- far finer than any audible pitch step.
    // The scale constants and the step limits live in sample.cpp, next to the loop that uses them.

    struct Voice {
        enaga::dsp::Envelope amp;
        int32_t read0 = 0;      // source frame read at position 0 (window start, or its last
                                // frame when reversed)
        int32_t len = 0;        // window length in frames
        int32_t pos = 0;        // integer frames from the window start, always counting up
        uint32_t frac = 0;      // the fractional part of pos, Q24
        int32_t safe_lo = 0;    // the range of pos over which all four taps are in the buffer;
        int32_t safe_hi = -1;   // empty (hi < lo) means every sample takes the clamped path
        int dir = 1;            // +1 forward, -1 reversed
        bool loop = false;
        bool playing = false;
        float level = 1.0f;     // velocity and accent
        float note_ratio = 1.0f;  // the trig note's pitch ratio, 1 in slice mode
        float gain = 1.0f;      // the declick fade's current gain
        int fade = 0;           // frames of fade left, 0 when not fading
        int wait = 0;           // frames until `next` starts
        bool pending = false;
        Trig next{};

        // An Envelope with sustain 0 parks in the `sustain` stage rather than reaching `idle`
        // (docs/ENAGA_API.md, "Gotchas"), so the level is what says a one-shot has finished. The
        // attack is excluded because it starts from zero.
        static bool done(const enaga::dsp::Envelope &e) {
            const enaga::dsp::Envelope::Stage s = e.stage();
            return s == enaga::dsp::Envelope::idle ||
                   (s != enaga::dsp::Envelope::attack && e.level() < 1e-4f);
        }
        bool busy() const { return pending || playing; }
    };

    void recompute_window();
    void start(Voice &voice);
    void silence(Voice &voice);
    void render_voice(Voice &voice, float *out, int n);
    void run(Voice &voice, float *out, int n);

    // Fills dst[0..n) with the window read at the current position, zeroing whatever is left
    // after the window ends. kCh is the source's channel count, so the tap gather has no branch
    // and the mono case does not pay for a stereo sum.
    template <int kCh>
    void generate(Voice &voice, int32_t istep, uint32_t fstep, float inv_step, float *dst, int n);

    Voice voices_[kVoices];
    enaga::dsp::EnvelopeShape amp_shape_{};

    // The resident PCM, owned by the caller.
    const int16_t *data_ = nullptr;
    int32_t frames_ = 0;           // frames per channel, 0 when nothing is playable
    int channels_ = 1;
    bool playable_ = false;
    float source_rate_ = 48000.0f;
    float rate_ratio_ = 1.0f;      // source_rate / sample_rate: the step at pitch 0
    float src_scale_ = 1.0f / 32768.0f;   // int16 to float, with the mono/stereo sum folded in

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;

    float start_unit_ = 0.0f;      // STA, LEN and SLC as read, so set_sample can redo the window
    float length_unit_ = 1.0f;
    float pitch_ratio_ = 1.0f;     // PIT
    int slices_ = 1;               // SLC
    int32_t window_start_ = 0;     // the START/LEN window, in frames
    int32_t window_len_ = 0;
    float slice_frames_ = 0.0f;    // window_len_ / slices_, kept in float: no integer divide
    int32_t play_len_ = 0;         // what one trigger plays: the window, or one slice of it
    bool loop_ = false;            // PLY
    bool reverse_ = false;
    float out_gain_ = 0.25f;       // LVL
    int steal_cursor_ = 0;         // rotates, so equally loud voices are stolen in turn
    bool active_ = false;

    float voice_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
