// Turning a WAV file's sample data into what the sampler plays: int16, at the engine's rate.
//
// Enaga parses WAV headers (`<enaga/wav.hpp>`: RIFF, the fmt chunk, where the data starts, and
// WAVE_FORMAT_EXTENSIBLE by its sub-format), so none of that is here. What it does not do, and
// what docs/ENAGA_API.md lists among its gaps, is convert sample formats or resample -- there is
// no int16-to-float conversion anywhere in the library and no interpolated read. Both are ours.
//
// docs/SPEC.md, "Samples": files are "loaded whole into RAM as int16 at project open", and
// "files not at 48 kHz are resampled once at load, in main-loop slices". So this module does two
// jobs and both are pure arithmetic over caller-owned buffers, which is why they are in core/
// where they can be tested rather than in the loader where they cannot.
//
// ## Slices, and why the state is explicit
//
// A 32 MiB sample takes about 4.2 s to read from the card at the measured 7,600 KiB/s, and
// converting it is not free either. Neither may happen in one go: the main loop has to keep
// drawing and the audio callback has to keep running. Every function here therefore converts a
// bounded number of frames and tells the caller where it got to, so `app/` can do a slice per
// main-loop pass. There is no hidden state and no callback: the caller owns the cursor.
//
// ## Fixed point, not float, for the resampler's position
//
// The read position is 32.32 fixed point, as dsp/sample.cpp already uses for playback: a float
// position loses a sample's precision somewhere past 16.7 million frames, which is only six
// minutes at 48 kHz, and a sample that long is exactly the case where someone would notice the
// end drifting. The step is computed once per call, so the C674x's lack of an integer divide
// does not matter here.
#pragma once

#include <cstdint>

namespace strata {

// What a file's samples are in. These mirror `enaga::wav::Encoding` plus the bit depth, as one
// value, because that pair is what decides how a frame is decoded.
//
// Serialized nowhere, so free to renumber.
enum class PcmFormat : uint8_t {
    unknown = 0,
    u8,          // 8-bit PCM is UNSIGNED, offset by 128. The one asymmetry in the WAV format
    s16,
    s24,         // three bytes, little-endian, sign-extended from the top byte
    s32,
    f32,
    count
};

// The format for an `enaga::wav::Format`'s encoding and bits. `unknown` for anything else, which
// is a refusable file rather than a guess. `encoding` takes `enaga::wav::pcm` (1) or
// `floating` (3); core/ cannot include the Enaga header that names them, so they arrive as
// numbers and this function is the one place that knows what they mean.
PcmFormat pcm_format_of(uint16_t encoding, uint16_t bits);

// Bytes per sample (not per frame) for a format; 0 for `unknown`.
uint32_t pcm_sample_bytes(PcmFormat format);

// A short upper-case name for the screen ("16 BIT", "24 BIT", "FLOAT").
const char *pcm_format_name(PcmFormat format);

// --- converting -----------------------------------------------------------

// Convert up to `max_frames` frames from `in` into int16 at `out`, and report how many frames
// were converted.
//
// `channels` is the file's channel count and is preserved: a stereo file converts to interleaved
// stereo int16, a mono file to mono. Mixing down is the caller's decision, not this one's --
// dsp/sample.cpp plays both.
//
// Clamping, not wrapping: a float file may hold values outside +/-1.0 (plenty do), and a wrap
// turns an over-level peak into a full-scale click of the opposite sign. The rounding is to
// nearest.
//
// `in_bytes` bounds the read: a frame that is not completely present is not converted, so a
// truncated file converts to as much as it actually holds. Returns the frames written.
uint32_t pcm_to_s16(const uint8_t *in, uint32_t in_bytes, PcmFormat format, int channels,
                    int16_t *out, uint32_t max_frames);

// --- resampling -----------------------------------------------------------

// A resampling cursor over a source already converted to int16.
//
// Linear interpolation, deliberately: this runs once at load over a whole file, and the
// alternative worth having (a windowed sinc) costs a design decision about its window and a
// great deal more arithmetic for a difference that a 44.1-to-48 conversion of a drum loop will
// not show. dsp/sample.cpp interpolates with Catmull-Rom at PLAY time, where pitch changes make
// it audible; this is the one-off rate match.
struct Resampler {
    uint64_t position = 0;     // 32.32 into the source, in frames
    uint64_t step = 0;         // source frames per output frame, 32.32

    // step = source_rate / output_rate. Both in Hz; a zero or absurd rate gives a step of 1.0,
    // which copies rather than producing silence or dividing by zero.
    static Resampler make(uint32_t source_rate, uint32_t output_rate);

    // How many output frames `source_frames` will produce in total.
    uint32_t output_frames(uint32_t source_frames) const;
};

// Resample up to `max_out` frames from `in` (int16, `in_frames` frames, `channels` interleaved)
// into `out`, advancing `cursor`. Returns the output frames written, which is 0 once the cursor
// has passed the end of the source.
//
// The last source frame is held rather than interpolated against memory past the end, so the
// final output frame is the file's own last sample instead of a step towards whatever followed
// it in RAM.
uint32_t pcm_resample(const int16_t *in, uint32_t in_frames, int channels, Resampler &cursor,
                      int16_t *out, uint32_t max_out);

}  // namespace strata
