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

#include "onsets.hpp"
#include "overview.hpp"
#include "streamfifo.hpp"
#include "track.hpp"

namespace strata {

class Sampler final : public Machine {
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

    // A STREAMED sample (app/storage.hpp, "Streaming"): `head` is its first `head_frames`,
    // resident like any sample, and the rest arrives in `fifo`, numbered as in the file, which
    // the engine fills from the card every block. Same ownership rules as set_sample, for the
    // head and for the fifo. `source_rate` is the file's own: a streamed file is played at its
    // rate rather than resampled, because nothing converts it ahead of time.
    //
    // A streamed sample plays forward from its first frame, on one voice: START, SLC and reverse
    // do not apply, since each would need the stream to jump, and an Enaga stream only
    // reads on. PIT, the trig note, the envelope, LVL, loop and LEN (an end point) do. Each time
    // playback goes back to frame 0 -- a trigger, or a loop wrapping past the head -- the fifo is
    // emptied and `stream_restarts()` moves, which is the engine's signal to have the stream
    // reopened after the head. A LEN loop inside the head never reopens it.
    // set_sample (or a nullptr head) leaves stream mode.
    void set_stream(const int16_t *head, uint32_t head_frames, uint32_t total_frames, int channels,
                    float source_rate, StreamFifo *fifo);
    bool streaming() const { return fifo_ != nullptr; }

    // SLC AUTO (docs/SPEC.md, "SAMPLE"): with `on`, SLC = n cuts the START/LEN window at the n - 1
    // strongest of the sample's points (core/onsets.hpp) instead of into n equal slices. Points
    // outside the window do not count, so the window can hold fewer slices than SLC asks for;
    // with none inside it, AUTO falls back to equal slices rather than to one. Off by default,
    // and off changes nothing: equal slicing is exactly what it was.
    //
    // The points belong to the sample, so set_sample forgets them; the engine hands the new
    // sample's points right after (app/engine.cpp, Cmd::set_sample). Control rate: the cuts are
    // worked out here and in prepare(), never per sample.
    void set_slice_points(const SlicePoints &points);
    void set_auto_slices(bool on);
    bool auto_slices() const { return auto_; }
    // How many slices the next trigger chooses among, and whether they are the detected ones.
    int slice_count() const { return cuts_ > 0 ? cuts_ : slices_; }
    bool slicing_by_points() const { return cuts_ > 0; }
    int point_count() const { return points_.count; }
    // The i-th slice's first frame, for tests and a later waveform display.
    int32_t slice_start(int i) const;
    uint32_t stream_restarts() const { return restarts_; }
    // Taps the fifo did not have when the voice wanted them: the card was late. They play as
    // silence and time goes on, so the loop stays on the beat.
    uint32_t stream_late() const { return late_; }
    // Where the stream voice is, in file frames, for the engine's drop and for tests.
    uint32_t stream_position() const { return uint32_t(voices_[0].pos); }
    uint32_t stream_frames() const { return stream_total_; }

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

    // Where this machine is in its sample, in the screen overview's columns (core/overview.hpp):
    // the START..END window, a bit for each slice start, and each playing voice with its
    // envelope's level. Audio context, called by the engine every few blocks; a few multiplies.
    void wave_view(WaveView &view) const;
    // The loudest playing voice's envelope.
    bool envelope_now(uint8_t &stage, float &level) const override;

    static constexpr int kVoices = 4;
    static constexpr int kSlicesMax = 16;
    // Four taps need four frames. Shorter than this and there is nothing to interpolate, so
    // nothing plays: see "4-point interpolation and the buffer edges" above.
    static constexpr int kMinFrames = 4;
    // Index math stays in int32 -- 67M frames is 128 MiB mono, twice the whole sample budget.
    static constexpr int kMaxFrames = 1 << 26;
    // A streamed file's length: 2^30 frames is over six hours at 48 kHz, and the position
    // (pos + 2 taps + a 32-frame step) stays well inside int32.
    static constexpr uint32_t kMaxStreamFrames = 1u << 30;
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
        bool release_on_start = false;   // released before its frame: let go once it starts
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
    // The stream voice's: taps from the head or the fifo, by file frame.
    template <int kCh>
    void generate_stream(Voice &voice, int32_t istep, uint32_t fstep, float *dst, int n);
    template <int kCh>
    float stream_tap(int32_t frame);
    void restart_stream();

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
    float view_scale_ = 0.0f;      // kWaveColumns / frames (the whole file's, streamed): wave_view

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
    // SLC AUTO: the sample's points, and the cuts SLC makes of them. cut_[0..cuts_] bound cuts_
    // slices; cuts_ is 0 when slicing is equal.
    SlicePoints points_{};
    bool auto_ = false;
    int cuts_ = 0;
    int32_t cut_[kSlicesMax + 1] = {};
    bool loop_ = false;            // PLY
    bool reverse_ = false;
    float out_gain_ = 0.25f;       // LVL
    int steal_cursor_ = 0;         // rotates, so equally loud voices are stolen in turn
    bool active_ = false;

    // Stream mode (set_stream). data_ and frames_ are the head.
    StreamFifo *fifo_ = nullptr;
    uint32_t stream_total_ = 0;
    uint32_t restarts_ = 0;
    uint32_t late_ = 0;

    float voice_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
