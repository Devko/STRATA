// The arithmetic behind core/pcm.hpp. The contract, and why these two jobs live in core/ at all,
// are in the header; this file is about how the numbers are formed.
//
// ## The rules this code is written to
//
//   - Nothing here points into the file's bytes with anything but a `const uint8_t *`. WAV
//     sample data is packed, so every other 24-bit stereo frame starts on an odd address, and a
//     misaligned 32-bit read on the C674x does not fault -- it silently drops the low address
//     bits and returns a different sample. Every value is assembled byte by byte, exactly as
//     <enaga/wav.hpp> assembles the header fields.
//   - No division in a loop, and no 64-bit division at all. The C674x has no integer divide
//     instruction, so even a 32-bit division is a libgcc call, and the 64-bit helper does not
//     link in this app (core/timing.cpp records the same finding). Conversion divides once per
//     call, to find the whole frames present; the resampler's step is a shift-and-subtract
//     division, also once per call.
//   - No libm: no roundf, no floorf, no fabsf. The rounding is written out below, and the one
//     place a float is involved (an f32 file) clamps BEFORE it converts, because converting an
//     out-of-range float to an integer is undefined behaviour rather than merely a wrong number.
//
// ## Rounding, to nearest
//
// Narrowing a 24- or 32-bit sample by shifting alone truncates towards negative infinity, which
// over a whole file is a half-LSB negative DC offset rather than a rounding error. So the
// integer paths add half an LSB before they shift -- one add and one shift, ties going up -- and
// the f32 path adds half a step away from zero. The two tie directions differ by half an LSB on
// the values that tie exactly; nothing else about them differs, and tests/test_pcm.cpp pins both.
//
// ## The two asymmetries worth knowing before reading on
//
//   - 8-bit PCM is UNSIGNED, offset by 128: 0x80 is silence and 0x00 is full-scale negative.
//     It is the only format in WAV that works this way.
//   - int16's range is not symmetric, so full scale is 32768 but the top value is 32767:
//     -1.0 converts to -32768 and +1.0 clamps to 32767.
#include "pcm.hpp"

#include <cstddef>
#include <cstring>      // memcpy, the one safe way to read a float out of its bits (see f32_to_s16)

namespace strata {
namespace {

static_assert(sizeof(float) == 4, "the f32 path reads a 32-bit IEEE-754 single out of four bytes");

// `enaga::wav::Encoding`, which core/ cannot include. wav.hpp's parse() has already resolved
// WAVE_FORMAT_EXTENSIBLE to its sub-format, so 0xFFFE never reaches pcm_format_of().
constexpr uint16_t kEncodingPcm = 1;
constexpr uint16_t kEncodingFloat = 3;

constexpr int32_t kS16Min = -32768;
constexpr int32_t kS16Max = 32767;

// 1.0 in the cursor's 32.32 fixed point.
constexpr uint64_t kOne = uint64_t(1) << 32;

// The band of sample rates worth believing. Wide enough for anything anyone will load (8 kHz to
// 384 kHz, with headroom either side) and narrow enough that `step` stays a few bits wide, so the
// 32.32 cursor cannot run out of integer part over a file that fits in RAM. Outside the band the
// rate is not a rate, and the header's rule applies: copy, do not divide by it.
constexpr uint32_t kRateMin = 1000;
constexpr uint32_t kRateMax = 768000;

// Little-endian readers, the same shape as enaga::wav::detail's. Byte by byte, for the alignment
// reason at the top of the file.
inline uint32_t u16le(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t u24le(const uint8_t *p) { return u16le(p) | uint32_t(p[2]) << 16; }
inline uint32_t u32le(const uint8_t *p) { return u16le(p) | u16le(p + 2) << 16; }

// Sign-extend the low bits of `bits`, where `sign_bit` is the top one (0x8000 for 16-bit data,
// 0x800000 for 24-bit). Written as a subtract rather than a cast of an out-of-range value
// because this *is* the content of "24-bit is sign-extended from the top byte", and a reader
// should be able to see that rather than infer it from the standard's conversion rules.
inline int32_t signed_of(uint32_t bits, uint32_t sign_bit) {
    return int32_t(bits & (sign_bit - 1u)) - int32_t(bits & sign_bit);
}

inline int16_t sat16(int32_t v) {
    // Clamped, never wrapped: rounding the top sample of a 24-bit file up carries it one past
    // 32767, and a wrap would turn that peak into a full-scale click of the opposite sign.
    return int16_t(v < kS16Min ? kS16Min : (v > kS16Max ? kS16Max : v));
}

// One IEEE-754 single, given as its 32 bits, to int16.
//
// The special values are decided on the bits, which is both free and the only way to do it here:
// there is no isnan() without libm, and a NaN compares false against every bound, so a plain
// clamp would hand it to the int conversion untouched.
//
//   - An infinity is an over-level peak as far as this file is concerned, so it clips to the
//     rail on its own side.
//   - A NaN is not a signal at all. It becomes silence rather than a rail, because the one thing
//     worse than a lost sample is a full-scale click.
inline int16_t f32_to_s16(uint32_t bits) {
    if ((bits & 0x7F800000u) == 0x7F800000u) {
        const bool is_nan = (bits & 0x007FFFFFu) != 0u;
        if (is_nan) return 0;
        return (bits >> 31) != 0u ? int16_t(kS16Min) : int16_t(kS16Max);
    }
    float value;
    std::memcpy(&value, &bits, sizeof value);     // both locals, so this is a register move

    const float scaled = value * 32768.0f;
    // Rounding to nearest by hand: `int(x + 0.5f)` rounds the wrong way for the negative half of
    // a signed sample, which is half of every waveform.
    const float rounded = scaled + (scaled < 0.0f ? -0.5f : 0.5f);
    // Clamped while it is still a float. Float WAVs routinely hold values past +/-1.0, and
    // converting one of those to int32 first is undefined behaviour, not a wrong number.
    const float held = rounded < -32768.0f ? -32768.0f : (rounded > 32767.0f ? 32767.0f : rounded);
    return int16_t(int32_t(held));
}

// floor(numerator / denominator) for 64-bit values, by shift and subtract.
//
// This is here because the 64-bit divide does not link (see the top of the file), and it is
// affordable because it runs once per call to make() or output_frames() -- never per frame. The
// loop is 64 iterations of a shift, a compare and a subtract.
//
// `denominator` must be under 2^63, so that the running remainder's shift cannot overflow.
uint64_t div_u64(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0) return 0;
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1u);
        const bool fits = remainder >= denominator;
        remainder -= fits ? denominator : uint64_t(0);
        quotient |= fits ? uint64_t(1) << bit : uint64_t(0);
    }
    return quotient;
}

uint64_t ceil_div_u64(uint64_t numerator, uint64_t denominator) {
    const uint64_t quotient = div_u64(numerator, denominator);
    // quotient * denominator is at most `numerator`, so it cannot overflow.
    return quotient * denominator < numerator ? quotient + 1 : quotient;
}

}  // namespace

PcmFormat pcm_format_of(uint16_t encoding, uint16_t bits) {
    if (encoding == kEncodingPcm) {
        switch (bits) {
        case 8: return PcmFormat::u8;
        case 16: return PcmFormat::s16;
        case 24: return PcmFormat::s24;
        case 32: return PcmFormat::s32;
        default: return PcmFormat::unknown;
        }
    }
    // 64-bit float files exist and are not supported: a guess at one would read every frame at
    // the wrong stride, which sounds like noise rather than like a refusal.
    if (encoding == kEncodingFloat && bits == 32) return PcmFormat::f32;
    return PcmFormat::unknown;
}

uint32_t pcm_sample_bytes(PcmFormat format) {
    switch (format) {
    case PcmFormat::u8: return 1;
    case PcmFormat::s16: return 2;
    case PcmFormat::s24: return 3;
    case PcmFormat::s32: return 4;
    case PcmFormat::f32: return 4;
    case PcmFormat::unknown:
    case PcmFormat::count:
        break;
    }
    return 0;
}

const char *pcm_format_name(PcmFormat format) {
    switch (format) {
    case PcmFormat::u8: return "8 BIT";
    case PcmFormat::s16: return "16 BIT";
    case PcmFormat::s24: return "24 BIT";
    case PcmFormat::s32: return "32 BIT";
    case PcmFormat::f32: return "FLOAT";
    case PcmFormat::unknown:
    case PcmFormat::count:
        break;
    }
    return "---";       // what the rest of the UI shows for a field with nothing in it
}

uint32_t pcm_to_s16(const uint8_t *in, uint32_t in_bytes, PcmFormat format, int channels,
                    int16_t *out, uint32_t max_frames) {
    const uint32_t sample_bytes = pcm_sample_bytes(format);
    if (in == nullptr || out == nullptr || sample_bytes == 0 || channels <= 0) return 0;

    const uint32_t frame_bytes = sample_bytes * uint32_t(channels);
    // The only division here, and it is once per call. Flooring is the point: a frame that is
    // not completely present must not be converted, so a file truncated mid-frame -- a card
    // pulled during a write, a download that stopped -- converts to the frames it really holds
    // and the caller is told how many that was.
    const uint32_t present = in_bytes / frame_bytes;
    const uint32_t frames = present < max_frames ? present : max_frames;
    // frames <= present, so this is at most in_bytes / sample_bytes and cannot overflow.
    const uint32_t samples = frames * uint32_t(channels);

    // The format is decided once, outside the loop, so the per-sample work is a load, a shift or
    // two and a store. Channels need no attention at all: interleaved in is interleaved out, in
    // the file's own order, so a stereo file stays stereo and keeps left before right.
    const uint8_t *p = in;
    switch (format) {
    case PcmFormat::u8:
        // Unsigned, offset by 128, and scaled by exactly 256: 0x00 becomes -32768, 0x80 becomes
        // 0 and 0xFF becomes 32512. The top of the range is deliberately not stretched to reach
        // 32767 -- that would make the step between codes uneven, and 1.5 LSB of missing
        // headroom is worth less than a uniform scale.
        for (uint32_t i = 0; i < samples; i++, p += 1) {
            out[i] = int16_t((int32_t(p[0]) - 128) * 256);
        }
        break;
    case PcmFormat::s16:
        // Already the target format; only the byte order is this file's business.
        for (uint32_t i = 0; i < samples; i++, p += 2) {
            out[i] = int16_t(signed_of(u16le(p), 0x8000u));
        }
        break;
    case PcmFormat::s24:
        // Three bytes little-endian, sign-extended from the top byte. The top two bytes already
        // ARE the int16; the third is the rounding decision.
        for (uint32_t i = 0; i < samples; i++, p += 3) {
            const uint32_t bits = u24le(p);
            const int32_t high = signed_of(bits >> 8, 0x8000u);
            out[i] = sat16(high + (((bits & 0xFFu) >= 0x80u) ? 1 : 0));
        }
        break;
    case PcmFormat::s32:
        for (uint32_t i = 0; i < samples; i++, p += 4) {
            const uint32_t bits = u32le(p);
            const int32_t high = signed_of(bits >> 16, 0x8000u);
            out[i] = sat16(high + (((bits & 0xFFFFu) >= 0x8000u) ? 1 : 0));
        }
        break;
    case PcmFormat::f32:
        for (uint32_t i = 0; i < samples; i++, p += 4) {
            out[i] = f32_to_s16(u32le(p));
        }
        break;
    case PcmFormat::unknown:
    case PcmFormat::count:
        return 0;       // unreachable: pcm_sample_bytes already refused these
    }
    return frames;
}

Resampler Resampler::make(uint32_t source_rate, uint32_t output_rate) {
    Resampler cursor;
    cursor.position = 0;
    const bool believable = source_rate >= kRateMin && source_rate <= kRateMax &&
                            output_rate >= kRateMin && output_rate <= kRateMax;
    // Rounded UP, which shows at the end of a file rather than in the middle. A step rounded
    // down is a hair short of the true ratio, so after a whole file's worth of frames the cursor
    // sits just before the last source frame instead of just past it, and the file yields one
    // output frame more than the arithmetic says it should. 44,100 to 48,000 is exactly 147/160,
    // so the common case is precisely the one that would show it. Rounded up the error goes the
    // other way and is bounded by one step over the whole file -- a fraction of a sample.
    //
    // An exact ratio stays exact either way: source_rate == output_rate gives exactly 1.0.
    cursor.step = believable ? ceil_div_u64(uint64_t(source_rate) << 32, output_rate) : kOne;
    return cursor;
}

uint32_t Resampler::output_frames(uint32_t source_frames) const {
    if (source_frames == 0) return 0;
    const uint64_t advance = step != 0 ? step : kOne;
    // Output frame k reads source frame (k * step) >> 32, so the frames that exist are those
    // with k * step < source_frames, and there are ceil(source_frames / step) of them. The
    // numerator cannot overflow: source_frames is 32 bits.
    const uint64_t count = ceil_div_u64(uint64_t(source_frames) << 32, advance);
    // A count past 2^32 cannot be expressed, and cannot be produced either: the caller's buffer
    // would have to be 8 GiB. Reporting the largest count leaves the refusal where it belongs.
    return count > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(count);
}

uint32_t pcm_resample(const int16_t *in, uint32_t in_frames, int channels, Resampler &cursor,
                      int16_t *out, uint32_t max_out) {
    if (in == nullptr || out == nullptr || channels <= 0 || in_frames == 0 || max_out == 0) return 0;
    // A step of zero cannot advance, so it would hand the caller the first frame max_out times
    // and never finish the file. make()'s rule for a rate it does not believe applies here too:
    // copy. Written back, so that a caller slicing a file sees one step for the whole run.
    if (cursor.step == 0) cursor.step = kOne;

    const uint64_t step = cursor.step;
    const int lanes = channels;
    uint64_t position = cursor.position;
    int16_t *write = out;
    uint32_t written = 0;

    while (written < max_out) {
        const uint64_t frame = position >> 32;
        if (frame >= uint64_t(in_frames)) break;        // the cursor has left the source
        const uint32_t index = uint32_t(frame);
        const uint32_t fraction = uint32_t(position);   // the low 32 bits: 0 <= fraction < 1

        const int16_t *a = in + size_t(index) * size_t(lanes);
        // The frame after the last one is the last one: the final output frame is the file's own
        // last sample instead of a step towards whatever follows the buffer in RAM. A select, so
        // the common case does not pay for a branch.
        const int16_t *b = a + (index + 1u < in_frames ? lanes : 0);

        for (int c = 0; c < lanes; c++) {
            // Linear, with the whole 32-bit fraction: a 32x32 multiply into 64 bits, which this
            // core does in one instruction, then the high word. Half a step is added so the
            // weight rounds rather than truncating.
            //
            // The result is always between a[c] and b[c], so it cannot leave int16 and needs no
            // clamp: the exact product is under |b - a| and rounding can only reach it.
            const int64_t delta =
                (int64_t(b[c] - a[c]) * int64_t(fraction) + (int64_t(1) << 31)) >> 32;
            write[c] = int16_t(int32_t(a[c]) + int32_t(delta));
        }

        write += lanes;
        position += step;
        written++;
    }

    cursor.position = position;
    return written;
}

}  // namespace strata
