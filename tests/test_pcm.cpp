// Tests for core/pcm: WAV sample data to int16, and int16 to the engine's rate.
//
// This is the whole path from "bytes off the card" to "something the sampler can play", and it is
// the only part of sample loading that can be tested without hardware, so the tests here are
// about exact numbers rather than about "it is not silent". Three kinds of bug are what they are
// written to catch:
//
//   - A format decoded with the wrong sign or the wrong scale. 8-bit PCM is the dangerous one:
//     it is UNSIGNED, and a signed reading turns silence into a full-scale buzz -- loud on
//     hardware and invisible in a test that only checks for non-silence.
//   - A wrap where there should be a clamp. Float WAVs hold values past +/-1.0 and 24-bit files
//     round up past the top of int16; a wrap turns either into a full-scale click of the
//     opposite sign.
//   - A result that depends on where the slices fell. A 32 MiB sample is converted and resampled
//     a few thousand frames per main-loop pass, so "in slices" and "in one call" must produce
//     the same bytes. Two tests compare them byte for byte, because that property is what the
//     whole slicing design rests on.
#include "check.hpp"

#include "pcm.hpp"

#include <cmath>
#include <cstring>

using namespace strata;

namespace {

// 1.0 in the cursor's 32.32 fixed point.
constexpr uint64_t kOne = uint64_t(1) << 32;
constexpr double kTwoPow32 = 4294967296.0;

// check.hpp prints through an overload set that has no uint64_t in it (on this host uint64_t is
// `unsigned long`, which converts to several of the overloads equally well), so 64-bit values go
// through this on their way into a check.
inline unsigned long long u64(uint64_t v) { return v; }

// --- building file bytes by hand -------------------------------------------
//
// Little-endian writers, so that every test's input is the bytes a file would really hold rather
// than a struct this host happens to lay out that way.

void put16(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = uint8_t(v >> 16);
}

void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

void put_f32(uint8_t *p, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    put32(p, bits);
}

constexpr PcmFormat kFormats[] = {PcmFormat::u8, PcmFormat::s16, PcmFormat::s24, PcmFormat::s32,
                                  PcmFormat::f32};
constexpr int kFormatCount = 5;

// Write one sample holding `value`, an int16-domain level, at the format's own full scale. Exact
// for every format if `value` is a multiple of 256, which is what the ramp below uses; 8-bit
// rounds, since it only has 256 codes to say it with.
void put_level(uint8_t *p, PcmFormat format, int32_t value) {
    switch (format) {
    case PcmFormat::u8: {
        // Round to nearest without libm and without int(x + 0.5) on a negative value.
        const int32_t code = value >= 0 ? (value + 128) / 256 : -((-value + 128) / 256);
        const int32_t held = code < -128 ? -128 : (code > 127 ? 127 : code);
        p[0] = uint8_t(128 + held);
        break;
    }
    case PcmFormat::s16:
        put16(p, uint32_t(value) & 0xFFFFu);
        break;
    case PcmFormat::s24:
        put24(p, uint32_t(value * 256) & 0xFFFFFFu);
        break;
    case PcmFormat::s32:
        put32(p, (uint32_t(value) & 0xFFFFu) << 16);
        break;
    case PcmFormat::f32:
        put_f32(p, float(value) * (1.0f / 32768.0f));
        break;
    case PcmFormat::unknown:
    case PcmFormat::count:
        break;
    }
}

// Write one sample holding `value`, normalized to +/-1.0, at the format's own full scale -- the
// way any other tool would write it.
void put_normalized(uint8_t *p, PcmFormat format, double value) {
    const double held = value > 1.0 ? 1.0 : (value < -1.0 ? -1.0 : value);
    if (format == PcmFormat::f32) {
        put_f32(p, float(held));
        return;
    }
    // The integer formats: round to nearest away from zero, then clamp to the format's range.
    const double full = format == PcmFormat::u8 ? 128.0 : (format == PcmFormat::s16   ? 32768.0
                                                           : format == PcmFormat::s24 ? 8388608.0
                                                                                      : 2147483648.0);
    const double scaled = held * full;
    const double rounded = scaled + (scaled < 0.0 ? -0.5 : 0.5);
    const double low = -full;
    const double high = full - 1.0;
    const double clamped = rounded < low ? low : (rounded > high ? high : rounded);
    const int64_t code = int64_t(clamped);
    switch (format) {
    case PcmFormat::u8: p[0] = uint8_t(128 + int32_t(code)); break;
    case PcmFormat::s16: put16(p, uint32_t(int32_t(code)) & 0xFFFFu); break;
    case PcmFormat::s24: put24(p, uint32_t(int32_t(code)) & 0xFFFFFFu); break;
    case PcmFormat::s32: put32(p, uint32_t(int32_t(code))); break;
    default: break;
    }
}

// How far a format's own resolution allows the decoded value to sit from the ideal int16. Only
// 8-bit is coarse: its step is 256 int16 codes wide, so half a step is 128.
double tolerance_of(PcmFormat format) { return format == PcmFormat::u8 ? 129.0 : 1.0; }

// --- a deterministic byte stream -------------------------------------------
//
// The slice-invariance tests need data that is neither symmetric nor special (a ramp can hide a
// rounding bug by being monotonic), and the same data on every run.

uint32_t g_lcg = 1;

void lcg_reset(uint32_t seed) { g_lcg = seed; }

uint8_t lcg_byte() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return uint8_t(g_lcg >> 24);
}

int16_t lcg_s16() {
    const uint32_t low = lcg_byte();
    const uint32_t high = lcg_byte();
    return int16_t((low | high << 8) & 0xFFFFu);
}

// Scratch, static because a few hundred KiB does not belong on a stack frame.
constexpr uint32_t kScratchSamples = 1u << 16;      // int16s, so 32,768 stereo frames
uint8_t g_bytes[kScratchSamples * 4];
int16_t g_source[kScratchSamples];
int16_t g_one_call[kScratchSamples];
int16_t g_sliced[kScratchSamples];

}  // namespace

// --- what the file says it holds ------------------------------------------

TEST(pcm_format_of_knows_what_wav_can_hold) {
    CHECK(pcm_format_of(1, 8) == PcmFormat::u8);
    CHECK(pcm_format_of(1, 16) == PcmFormat::s16);
    CHECK(pcm_format_of(1, 24) == PcmFormat::s24);
    CHECK(pcm_format_of(1, 32) == PcmFormat::s32);
    CHECK(pcm_format_of(3, 32) == PcmFormat::f32);

    // A refusal, not a guess: decoding one of these at the wrong stride would sound like noise,
    // which is harder to diagnose than a file the app declines to load.
    CHECK(pcm_format_of(1, 0) == PcmFormat::unknown);
    CHECK(pcm_format_of(1, 4) == PcmFormat::unknown);
    CHECK(pcm_format_of(1, 12) == PcmFormat::unknown);
    CHECK(pcm_format_of(1, 20) == PcmFormat::unknown);
    CHECK(pcm_format_of(1, 64) == PcmFormat::unknown);
    CHECK(pcm_format_of(3, 16) == PcmFormat::unknown);
    CHECK(pcm_format_of(3, 64) == PcmFormat::unknown);      // 64-bit float files do exist
    CHECK(pcm_format_of(0, 16) == PcmFormat::unknown);
    CHECK(pcm_format_of(2, 16) == PcmFormat::unknown);      // MS ADPCM
    CHECK(pcm_format_of(7, 8) == PcmFormat::unknown);       // mu-law
    // enaga::wav::parse() resolves WAVE_FORMAT_EXTENSIBLE to its sub-format before this is
    // reached, so the tag itself is not a format.
    CHECK(pcm_format_of(0xFFFE, 16) == PcmFormat::unknown);
}

TEST(every_format_has_a_size_and_a_name) {
    CHECK_EQ(pcm_sample_bytes(PcmFormat::u8), 1u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::s16), 2u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::s24), 3u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::s32), 4u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::f32), 4u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::unknown), 0u);
    CHECK_EQ(pcm_sample_bytes(PcmFormat::count), 0u);

    CHECK(std::strcmp(pcm_format_name(PcmFormat::u8), "8 BIT") == 0);
    CHECK(std::strcmp(pcm_format_name(PcmFormat::s16), "16 BIT") == 0);
    CHECK(std::strcmp(pcm_format_name(PcmFormat::s24), "24 BIT") == 0);
    CHECK(std::strcmp(pcm_format_name(PcmFormat::s32), "32 BIT") == 0);
    CHECK(std::strcmp(pcm_format_name(PcmFormat::f32), "FLOAT") == 0);

    // A format cannot be half-added: whatever decodes has a size and something to show on the
    // screen, and the name is short enough for the field it goes in.
    for (uint8_t i = uint8_t(PcmFormat::u8); i < uint8_t(PcmFormat::count); i++) {
        const PcmFormat format = PcmFormat(i);
        CHECK(pcm_sample_bytes(format) > 0u);
        CHECK(pcm_format_name(format)[0] != '\0');
        CHECK(std::strlen(pcm_format_name(format)) <= 6u);
    }
}

// --- the formats, value by value -----------------------------------------

TEST(u8_is_unsigned_and_offset_by_128) {
    // The bug: reading 8-bit PCM as signed. Then 0x80 -- silence in the file -- becomes -32768
    // and plays as a square-ish buzz, while 0x00, which is full-scale negative, becomes silence.
    const uint8_t silence[4] = {0x80, 0x80, 0x80, 0x80};
    int16_t out[4] = {1, 1, 1, 1};
    CHECK_EQ(pcm_to_s16(silence, 4, PcmFormat::u8, 1, out, 4), 4u);
    for (int i = 0; i < 4; i++) CHECK_EQ(out[i], 0);

    const uint8_t ramp[5] = {0x00, 0x40, 0x80, 0xC0, 0xFF};
    int16_t got[5] = {};
    CHECK_EQ(pcm_to_s16(ramp, 5, PcmFormat::u8, 1, got, 5), 5u);
    CHECK_EQ(got[0], -32768);       // 0 is full-scale NEGATIVE, not silence
    CHECK_EQ(got[1], -16384);
    CHECK_EQ(got[2], 0);
    CHECK_EQ(got[3], 16384);
    CHECK_EQ(got[4], 32512);        // 255 stops one step short of the rail: the scale stays even
}

TEST(s16_is_little_endian_and_passes_through_unchanged) {
    const int16_t want[6] = {0, 1, -1, 32767, -32768, -4096};
    uint8_t bytes[12];
    for (int i = 0; i < 6; i++) put16(bytes + i * 2, uint32_t(want[i]) & 0xFFFFu);

    int16_t got[6] = {};
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 1, got, 6), 6u);
    for (int i = 0; i < 6; i++) CHECK_EQ(got[i], want[i]);

    // Spelled out, so a byte swap cannot pass by symmetry: 00 80 is -32768 and 34 12 is 0x1234.
    const uint8_t most_negative[2] = {0x00, 0x80};
    int16_t one = 0;
    CHECK_EQ(pcm_to_s16(most_negative, 2, PcmFormat::s16, 1, &one, 1), 1u);
    CHECK_EQ(one, -32768);
    const uint8_t low_byte_first[2] = {0x34, 0x12};
    CHECK_EQ(pcm_to_s16(low_byte_first, 2, PcmFormat::s16, 1, &one, 1), 1u);
    CHECK_EQ(one, 0x1234);
}

TEST(s24_is_three_bytes_sign_extended_from_the_top_one) {
    struct Case {
        uint32_t bits;
        int16_t want;
    };
    const Case cases[] = {
        {0x000000u, 0},
        {0x7FFFFFu, 32767},      // the most positive: rounding carries past the rail, so clamped
        {0x800000u, -32768},     // the most negative
        {0x400000u, 16384},
        {0xC00000u, -16384},
        {0xFF0000u, -256},       // -65,536, a negative value with the sign bits set throughout
        {0xFFFFFFu, 0},          // -1, which is a 256th of an int16 step
    };
    for (const Case &c : cases) {
        uint8_t bytes[3];
        put24(bytes, c.bits);
        int16_t got = 123;
        CHECK_EQ(pcm_to_s16(bytes, 3, PcmFormat::s24, 1, &got, 1), 1u);
        CHECK_EQ(got, c.want);
    }
}

TEST(s24_rounds_to_nearest_rather_than_truncating) {
    // Shifting alone truncates towards negative infinity, which over a whole file is half an LSB
    // of negative DC rather than a rounding error. Ties go up.
    struct Case {
        uint32_t bits;
        int16_t want;
    };
    const Case cases[] = {
        {0x00007Fu, 0},       // 0.496
        {0x000080u, 1},       // 0.5, a tie
        {0x00017Fu, 1},       // 1.496
        {0x000180u, 2},       // 1.5, a tie
        {0xFFFF00u, -1},      // -1.0 exactly
        {0xFFFE80u, -1},      // -1.5, a tie, so up
        {0xFFFE7Fu, -2},      // -1.504
    };
    for (const Case &c : cases) {
        uint8_t bytes[3];
        put24(bytes, c.bits);
        int16_t got = 123;
        CHECK_EQ(pcm_to_s16(bytes, 3, PcmFormat::s24, 1, &got, 1), 1u);
        CHECK_EQ(got, c.want);
    }
}

TEST(s32_takes_the_top_sixteen_bits_and_rounds_the_rest) {
    struct Case {
        uint32_t bits;
        int16_t want;
    };
    const Case cases[] = {
        {0x00000000u, 0},
        {0x7FFFFFFFu, 32767},      // rounding carries past the rail: clamped, never wrapped
        {0x80000000u, -32768},
        {0x80008000u, -32767},
        {0x00008000u, 1},          // half an int16 step, a tie
        {0x00007FFFu, 0},
        {0xFFFF0000u, -1},
        {0xFFFFFFFFu, 0},          // -1, a 65,536th of a step
        {0x12345678u, 0x1234},
        {0x1234A678u, 0x1235},     // the dropped half-word is over half, so the top one rounds up
    };
    for (const Case &c : cases) {
        uint8_t bytes[4];
        put32(bytes, c.bits);
        int16_t got = 123;
        CHECK_EQ(pcm_to_s16(bytes, 4, PcmFormat::s32, 1, &got, 1), 1u);
        CHECK_EQ(got, c.want);
    }
}

TEST(f32_scales_by_32768_and_clamps_at_both_rails) {
    struct Case {
        float value;
        int16_t want;
    };
    const Case cases[] = {
        {0.0f, 0},
        {-0.0f, 0},
        {0.25f, 8192},
        {0.5f, 16384},
        {-0.5f, -16384},
        {-0.75f, -24576},
        {1.0f, 32767},             // full scale is 32768, which int16 cannot hold going up
        {-1.0f, -32768},
        {1.5f, 32767},             // float files routinely hold values past the rails
        {-1.5f, -32768},
        {1e9f, 32767},
        {-1e9f, -32768},
        {1.0f / 32768.0f, 1},
        {-1.0f / 32768.0f, -1},
        {0.6f / 32768.0f, 1},      // rounded to nearest, not truncated towards zero
        {-0.6f / 32768.0f, -1},
        {0.4f / 32768.0f, 0},
        {-0.4f / 32768.0f, 0},
    };
    for (const Case &c : cases) {
        uint8_t bytes[4];
        put_f32(bytes, c.value);
        int16_t got = 123;
        CHECK_EQ(pcm_to_s16(bytes, 4, PcmFormat::f32, 1, &got, 1), 1u);
        CHECK_EQ(got, c.want);
    }
}

TEST(f32_turns_nan_and_infinity_into_something_finite) {
    // A plain clamp cannot do this: every comparison against a NaN is false, so a NaN would
    // reach the integer conversion untouched and the result would be whatever the hardware felt
    // like. An infinity is an over-level peak and clips; a NaN is not a signal at all, so it
    // becomes silence rather than a rail-to-rail click.
    struct Case {
        uint32_t bits;
        int16_t want;
    };
    const Case cases[] = {
        {0x7F800000u, 32767},      // +inf
        {0xFF800000u, -32768},     // -inf
        {0x7FC00000u, 0},          // a quiet NaN
        {0xFFC00000u, 0},          // a quiet NaN with the sign set
        {0x7F800001u, 0},          // a signalling NaN
        {0xFFFFFFFFu, 0},          // a NaN with every payload bit set
        {0x00000001u, 0},          // the smallest subnormal: far below one int16 step
        {0x80000001u, 0},
        {0x00800000u, 0},          // the smallest normal, likewise
    };
    for (const Case &c : cases) {
        uint8_t bytes[4];
        put32(bytes, c.bits);
        int16_t got = 123;
        CHECK_EQ(pcm_to_s16(bytes, 4, PcmFormat::f32, 1, &got, 1), 1u);
        CHECK_EQ(got, c.want);
    }
}

// --- a ramp and a sine, through every format ------------------------------

namespace {

// The ramp is multiples of 256 so that 8 bits can hold it exactly too, which lets one set of
// expected numbers pin all five decoders.
constexpr int16_t kRamp[] = {-32768, -24576, -16384, -8192, -256, 0, 256, 8192, 16384, 24576, 32512};
constexpr uint32_t kRampLength = sizeof kRamp / sizeof kRamp[0];

void check_ramp(PcmFormat format) {
    const uint32_t sample_bytes = pcm_sample_bytes(format);
    for (uint32_t i = 0; i < kRampLength; i++) put_level(g_bytes + i * sample_bytes, format, kRamp[i]);

    for (uint32_t i = 0; i < kRampLength; i++) g_one_call[i] = 999;
    CHECK_EQ(pcm_to_s16(g_bytes, kRampLength * sample_bytes, format, 1, g_one_call, kRampLength),
             kRampLength);
    for (uint32_t i = 0; i < kRampLength; i++) CHECK_EQ(g_one_call[i], kRamp[i]);
}

// 32 samples of a half-scale sine. The quarter-cycle points land exactly on sample 8 and sample
// 24, so the peak is an exact +/-16384 in every format and a scale that is out by a factor of
// two -- or a format whose peak lands in the wrong place -- cannot hide inside a tolerance.
void check_sine(PcmFormat format) {
    constexpr uint32_t n = 32;
    constexpr double pi = 3.14159265358979323846;
    const uint32_t sample_bytes = pcm_sample_bytes(format);
    const double tolerance = tolerance_of(format);

    double ideal[n];
    for (uint32_t i = 0; i < n; i++) {
        ideal[i] = 0.5 * std::sin(2.0 * pi * double(i) / double(n));
        put_normalized(g_bytes + i * sample_bytes, format, ideal[i]);
    }

    CHECK_EQ(pcm_to_s16(g_bytes, n * sample_bytes, format, 1, g_one_call, n), n);

    double peak = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        CHECK_NEAR(g_one_call[i], ideal[i] * 32768.0, tolerance);
        const double magnitude = g_one_call[i] < 0 ? -double(g_one_call[i]) : double(g_one_call[i]);
        if (magnitude > peak) peak = magnitude;
    }
    // Half scale is half scale, whatever the file said it in.
    CHECK_NEAR(peak, 16384.0, tolerance);
    CHECK_EQ(g_one_call[0], 0);
    CHECK_NEAR(g_one_call[8], 16384.0, tolerance);
    CHECK_NEAR(g_one_call[24], -16384.0, tolerance);
}

}  // namespace

TEST(u8_carries_a_ramp_and_a_sine) {
    check_ramp(PcmFormat::u8);
    check_sine(PcmFormat::u8);
}

TEST(s16_carries_a_ramp_and_a_sine) {
    check_ramp(PcmFormat::s16);
    check_sine(PcmFormat::s16);
}

TEST(s24_carries_a_ramp_and_a_sine) {
    check_ramp(PcmFormat::s24);
    check_sine(PcmFormat::s24);
}

TEST(s32_carries_a_ramp_and_a_sine) {
    check_ramp(PcmFormat::s32);
    check_sine(PcmFormat::s32);
}

TEST(f32_carries_a_ramp_and_a_sine) {
    check_ramp(PcmFormat::f32);
    check_sine(PcmFormat::f32);
}

// --- framing --------------------------------------------------------------

TEST(a_frame_that_is_not_all_there_is_not_converted) {
    // Stereo 24-bit, six bytes a frame, so five lengths in every six are short -- and the bug is
    // always at one particular remainder, which is why every length is swept rather than the
    // obvious one. A truncated file (a card pulled mid-write, a transfer that stopped) must
    // convert to the frames it really holds and say how many that was.
    constexpr uint32_t frames = 16;
    constexpr uint32_t frame_bytes = 6;
    for (uint32_t f = 0; f < frames; f++) {
        put_level(g_bytes + f * frame_bytes, PcmFormat::s24, int32_t(f) * 256);
        put_level(g_bytes + f * frame_bytes + 3, PcmFormat::s24, -int32_t(f) * 256);
    }

    for (uint32_t length = 0; length <= frames * frame_bytes; length++) {
        constexpr uint32_t slots = frames * 2 + 2;
        int16_t got[slots];
        for (uint32_t i = 0; i < slots; i++) got[i] = 0x7777;      // a canary, to see overruns

        const uint32_t n = pcm_to_s16(g_bytes, length, PcmFormat::s24, 2, got, frames);
        CHECK_EQ(n, length / frame_bytes);
        for (uint32_t f = 0; f < n; f++) {
            CHECK_EQ(got[f * 2], int16_t(f * 256));
            CHECK_EQ(got[f * 2 + 1], int16_t(-int32_t(f) * 256));
        }
        for (uint32_t i = n * 2; i < slots; i++) CHECK_EQ(got[i], 0x7777);
    }

    // The case from the contract, said once in its own terms: one byte short of a whole stereo
    // 24-bit frame converts every frame but the last.
    int16_t got[frames * 2] = {};
    CHECK_EQ(pcm_to_s16(g_bytes, frames * frame_bytes - 1, PcmFormat::s24, 2, got, frames),
             frames - 1);
}

TEST(the_frame_count_follows_the_channel_count) {
    // Nothing here mixes down or splits: the frame is however many channels the file has, and a
    // three-channel file's frames are three samples long. 12 bytes of 16-bit data is six frames
    // in mono, three in stereo and two in three channels, with the remainder dropped.
    uint8_t bytes[12] = {};
    int16_t got[12] = {};
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 1, got, 12), 6u);
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 2, got, 12), 3u);
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 3, got, 12), 2u);
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 5, got, 12), 1u);
    CHECK_EQ(pcm_to_s16(bytes, 12, PcmFormat::s16, 7, got, 12), 0u);
}

TEST(max_frames_is_respected_exactly) {
    constexpr uint32_t frames = 20;
    constexpr uint32_t asked = 7;
    for (uint32_t f = 0; f < frames; f++) {
        put_level(g_bytes + f * 4, PcmFormat::s16, int32_t(f) * 256);
        put_level(g_bytes + f * 4 + 2, PcmFormat::s16, -int32_t(f) * 256);
    }

    constexpr uint32_t slots = frames * 2;
    int16_t got[slots];
    for (uint32_t i = 0; i < slots; i++) got[i] = 0x7777;

    CHECK_EQ(pcm_to_s16(g_bytes, frames * 4, PcmFormat::s16, 2, got, asked), asked);
    for (uint32_t f = 0; f < asked; f++) {
        CHECK_EQ(got[f * 2], int16_t(f * 256));
        CHECK_EQ(got[f * 2 + 1], int16_t(-int32_t(f) * 256));
    }
    // Not one int16 past what it said it wrote.
    for (uint32_t i = asked * 2; i < slots; i++) CHECK_EQ(got[i], 0x7777);

    CHECK_EQ(pcm_to_s16(g_bytes, frames * 4, PcmFormat::s16, 2, got, 0), 0u);
    CHECK_EQ(got[0], int16_t(0));      // the first frames are the ones converted above
}

TEST(conversion_refuses_what_it_cannot_decode) {
    uint8_t bytes[8] = {};
    int16_t out[4] = {7, 7, 7, 7};
    CHECK_EQ(pcm_to_s16(nullptr, 8, PcmFormat::s16, 1, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::s16, 1, nullptr, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::unknown, 1, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::count, 1, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::s16, 0, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::s16, -2, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 0, PcmFormat::s16, 1, out, 4), 0u);
    // More channels than WAV can state, and -- the one that matters -- a count big enough to
    // overflow the frame size to zero, which without the refusal is a division by zero.
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::s16, 65536, out, 4), 0u);
    CHECK_EQ(pcm_to_s16(bytes, 8, PcmFormat::s32, 1073741824, out, 4), 0u);
    for (int i = 0; i < 4; i++) CHECK_EQ(out[i], 7);      // and wrote nothing while refusing
}

TEST(stereo_keeps_its_channel_order_through_conversion) {
    // A file whose left channel is +1,000 throughout and right -1,000 must come out that way:
    // swapped channels and a lost sign both show here, and neither shows in a mono test.
    constexpr uint32_t frames = 8;
    for (int f = 0; f < kFormatCount; f++) {
        const PcmFormat format = kFormats[f];
        const uint32_t sample_bytes = pcm_sample_bytes(format);
        for (uint32_t i = 0; i < frames; i++) {
            put_level(g_bytes + (i * 2) * sample_bytes, format, 1000);
            put_level(g_bytes + (i * 2 + 1) * sample_bytes, format, -1000);
        }
        CHECK_EQ(pcm_to_s16(g_bytes, frames * 2 * sample_bytes, format, 2, g_one_call, frames),
                 frames);
        for (uint32_t i = 0; i < frames; i++) {
            CHECK_NEAR(g_one_call[i * 2], 1000.0, tolerance_of(format));
            CHECK_NEAR(g_one_call[i * 2 + 1], -1000.0, tolerance_of(format));
        }
    }
}

// --- conversion in slices -------------------------------------------------

namespace {

// The property app/ depends on: a caller converting a file 1,000 frames per main-loop pass gets
// the same bytes as a caller converting it in one go. Random bytes, so that the f32 case covers
// NaNs and infinities as well -- those have to be slice-invariant too.
void check_slicing(PcmFormat format) {
    constexpr uint32_t frames = 2500;
    constexpr uint32_t chunk = 1000;
    constexpr int channels = 2;
    const uint32_t sample_bytes = pcm_sample_bytes(format);
    const uint32_t frame_bytes = sample_bytes * channels;

    lcg_reset(0x5EEDu + sample_bytes);
    for (uint32_t i = 0; i < frames * frame_bytes; i++) g_bytes[i] = lcg_byte();

    CHECK_EQ(pcm_to_s16(g_bytes, frames * frame_bytes, format, channels, g_one_call, frames),
             frames);

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = pcm_to_s16(g_bytes + done * frame_bytes, (frames - done) * frame_bytes,
                                      format, channels, g_sliced + done * channels, chunk);
        CHECK(n > 0u);
        if (n == 0) break;
        done += n;
    }
    CHECK_EQ(done, frames);
    CHECK(std::memcmp(g_one_call, g_sliced, frames * channels * sizeof(int16_t)) == 0);
}

}  // namespace

TEST(converting_u8_in_slices_matches_one_call) { check_slicing(PcmFormat::u8); }
TEST(converting_s16_in_slices_matches_one_call) { check_slicing(PcmFormat::s16); }
TEST(converting_s24_in_slices_matches_one_call) { check_slicing(PcmFormat::s24); }
TEST(converting_s32_in_slices_matches_one_call) { check_slicing(PcmFormat::s32); }
TEST(converting_f32_in_slices_matches_one_call) { check_slicing(PcmFormat::f32); }

// --- the step -------------------------------------------------------------

TEST(the_step_is_exactly_one_when_the_rates_match) {
    // Exactly, not nearly: a step a hair off 1.0 makes a 48 kHz file that should be copied drift
    // against itself, and the drift only shows at the end of a long sample.
    CHECK_EQ(u64(Resampler::make(48000, 48000).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(44100, 44100).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(96000, 96000).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(22050, 22050).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(48000, 48000).position), 0ull);
}

TEST(the_step_is_the_ratio_of_the_rates) {
    CHECK_EQ(u64(Resampler::make(96000, 48000).step), u64(2 * kOne));
    CHECK_EQ(u64(Resampler::make(192000, 48000).step), u64(4 * kOne));
    CHECK_EQ(u64(Resampler::make(24000, 48000).step), u64(kOne / 2));
    CHECK_EQ(u64(Resampler::make(12000, 48000).step), u64(kOne / 4));

    // 44,100 / 48,000 is exactly 147/160, so 0.91875 * 2^32 = 3,946,001,203.2. Rounded UP: a
    // step rounded down is a hair short of the ratio, and after a whole file's frames the cursor
    // sits just before the last source frame rather than just past it, which yields one output
    // frame more than the arithmetic says. pcm.cpp's make() has the reasoning.
    CHECK_EQ(u64(Resampler::make(44100, 48000).step), 3946001204ull);

    CHECK_NEAR(double(Resampler::make(44100, 48000).step) / kTwoPow32, 44100.0 / 48000.0, 1e-9);
    CHECK_NEAR(double(Resampler::make(48000, 44100).step) / kTwoPow32, 48000.0 / 44100.0, 1e-9);
    CHECK_NEAR(double(Resampler::make(32000, 48000).step) / kTwoPow32, 2.0 / 3.0, 1e-9);
    CHECK_NEAR(double(Resampler::make(8000, 48000).step) / kTwoPow32, 1.0 / 6.0, 1e-9);
}

TEST(a_rate_that_is_not_a_rate_gives_a_step_of_one) {
    // Copy, rather than divide by zero or produce silence: a file whose header says something
    // absurd is still a file full of samples, and playing it at the wrong rate is a far better
    // failure than playing nothing.
    const uint32_t pairs[][2] = {
        {0, 48000}, {48000, 0}, {0, 0}, {1, 48000}, {48000, 1},
        {10000000, 48000}, {48000, 10000000}, {0xFFFFFFFFu, 48000}, {48000, 0xFFFFFFFFu},
        {999, 48000}, {48000, 999}, {768001, 48000}, {48000, 768001},
    };
    for (const auto &pair : pairs) {
        const Resampler cursor = Resampler::make(pair[0], pair[1]);
        CHECK_EQ(u64(cursor.step), u64(kOne));
        CHECK_EQ(u64(cursor.position), 0ull);
    }
    // The edges of the believable band itself are believed (pcm.cpp documents where they are).
    // 1/48 is not a whole number of 2^-32 steps, so that one is the ratio rounded up rather than
    // an exact value; 16 is exact and stays exact.
    CHECK_NEAR(double(Resampler::make(1000, 48000).step) / kTwoPow32, 1.0 / 48.0, 1e-9);
    CHECK_EQ(u64(Resampler::make(768000, 48000).step), u64(16 * kOne));
}

TEST(output_frames_has_the_obvious_answers) {
    CHECK_EQ(Resampler::make(48000, 48000).output_frames(1000), 1000u);
    CHECK_EQ(Resampler::make(96000, 48000).output_frames(1000), 500u);
    CHECK_EQ(Resampler::make(96000, 48000).output_frames(1001), 501u);   // a part step still counts
    CHECK_EQ(Resampler::make(24000, 48000).output_frames(3), 6u);
    CHECK_EQ(Resampler::make(44100, 48000).output_frames(44100), 48000u);
    CHECK_EQ(Resampler::make(48000, 44100).output_frames(48000), 44100u);
    CHECK_EQ(Resampler::make(22050, 48000).output_frames(22050), 48000u);
    CHECK_EQ(Resampler::make(48000, 48000).output_frames(0), 0u);
    CHECK_EQ(Resampler::make(48000, 48000).output_frames(1), 1u);
}

TEST(output_frames_counts_exactly_what_the_resampler_writes) {
    // If these two disagree, a caller that sizes its buffer from output_frames() either overruns
    // it or loses the tail of every file it loads.
    const uint32_t pairs[][2] = {
        {48000, 48000}, {96000, 48000}, {44100, 48000}, {48000, 44100},
        {24000, 48000}, {32000, 48000}, {192000, 48000}, {8000, 48000}, {22050, 48000},
    };
    const uint32_t lengths[] = {1, 2, 3, 7, 64, 100, 101, 999, 4096};

    for (uint32_t i = 0; i < kScratchSamples; i++) g_source[i] = int16_t(i * 7);

    for (const auto &pair : pairs) {
        for (const uint32_t length : lengths) {
            Resampler cursor = Resampler::make(pair[0], pair[1]);
            const uint32_t predicted = cursor.output_frames(length);
            CHECK(predicted <= kScratchSamples);

            uint32_t written = 0;
            for (;;) {
                const uint32_t n = pcm_resample(g_source, length, 1, cursor,
                                                g_one_call + written, 64);
                if (n == 0) break;
                written += n;
                CHECK(written <= kScratchSamples);
            }
            CHECK_EQ(written, predicted);
        }
    }
}

// --- resampling -----------------------------------------------------------

TEST(resampling_at_the_same_rate_is_a_copy_that_does_not_drift) {
    constexpr uint32_t frames = 30000;      // stereo, so 60,000 int16
    lcg_reset(0xC0FFEEu);
    for (uint32_t i = 0; i < frames * 2; i++) g_source[i] = lcg_s16();

    Resampler cursor = Resampler::make(48000, 48000);
    CHECK_EQ(pcm_resample(g_source, frames, 2, cursor, g_one_call, frames), frames);
    CHECK(std::memcmp(g_one_call, g_source, frames * 2 * sizeof(int16_t)) == 0);

    // The cursor's own arithmetic, which is where drift would be: exactly one source frame per
    // output frame, with nothing accumulated in the fraction.
    CHECK_EQ(u64(cursor.position), u64(uint64_t(frames) << 32));

    // And it is finished: a cursor past the end writes nothing however often it is called.
    g_one_call[0] = 0x4242;
    CHECK_EQ(pcm_resample(g_source, frames, 2, cursor, g_one_call, frames), 0u);
    CHECK_EQ(pcm_resample(g_source, frames, 2, cursor, g_one_call, frames), 0u);
    CHECK_EQ(g_one_call[0], 0x4242);
}

TEST(halving_the_rate_takes_every_other_frame) {
    // 96k to 48k: the step is exactly 2, so the fraction is always zero and nothing is
    // interpolated at all. The output is the even frames, unchanged.
    constexpr uint32_t frames = 101;
    for (uint32_t i = 0; i < frames; i++) g_source[i] = int16_t(int32_t(i) * 37 - 1000);

    Resampler cursor = Resampler::make(96000, 48000);
    const uint32_t n = pcm_resample(g_source, frames, 1, cursor, g_one_call, frames);
    CHECK_EQ(n, 51u);
    CHECK_EQ(n, Resampler::make(96000, 48000).output_frames(frames));
    for (uint32_t i = 0; i < n; i++) CHECK_EQ(g_one_call[i], g_source[i * 2]);
}

TEST(doubling_the_rate_keeps_every_source_frame_exactly) {
    // The step is exactly 0.5, so every other output frame sits on a source frame, where the
    // kernel is 1 at its centre and 0 everywhere else: those frames are the source, unchanged.
    constexpr uint32_t frames = 64;
    lcg_reset(0xD0B1Eu);
    for (uint32_t i = 0; i < frames; i++) g_source[i] = lcg_s16();
    Resampler cursor = Resampler::make(24000, 48000);
    CHECK_EQ(pcm_resample(g_source, frames, 1, cursor, g_one_call, kScratchSamples), 2 * frames);
    for (uint32_t i = 0; i < frames; i++) CHECK_EQ(g_one_call[2 * i], g_source[i]);
}

TEST(the_ends_hold_the_first_and_last_sample_instead_of_reading_past_them) {
    // The source is a window inside a bigger buffer with a canary on both sides. A tap on either
    // canary would put something far outside the source's own range in the output.
    constexpr uint32_t frames = 40;
    constexpr uint32_t pad = 40;
    static int16_t buffer[(frames + 2 * pad) * 2];
    for (uint32_t i = 0; i < (frames + 2 * pad) * 2; i++) buffer[i] = 30000;
    int16_t *source = buffer + pad * 2;
    for (uint32_t i = 0; i < frames; i++) {
        source[i * 2] = 800;            // a constant, so the held ends are the same constant
        source[i * 2 + 1] = -800;
    }
    const uint32_t pairs[][2] = {{44100, 48000}, {32000, 48000}, {24000, 48000}, {48000, 44100}};
    for (const auto &pair : pairs) {
        Resampler cursor = Resampler::make(pair[0], pair[1]);
        const uint32_t n = pcm_resample(source, frames, 2, cursor, g_one_call, kScratchSamples / 2);
        CHECK_EQ(n, Resampler::make(pair[0], pair[1]).output_frames(frames));
        for (uint32_t i = 0; i < n; i++) {
            CHECK_EQ(g_one_call[i * 2], 800);
            CHECK_EQ(g_one_call[i * 2 + 1], -800);
        }
        CHECK_EQ(pcm_resample(source, frames, 2, cursor, g_one_call + n * 2, 8), 0u);
    }
}

TEST(a_tone_at_44_1_khz_resamples_with_its_error_far_below_it) {
    // The reason the resampler is a sinc: linear interpolation left a 5 kHz tone's error 27 dB
    // under it and 15 kHz's only 8 dB. Each tone is compared with the ideal sine at the cursor's
    // own positions, over the middle of the output, past both ends' held frames.
    struct Case { double hz; double max_db; };
    const Case cases[] = {{1000, -75}, {5000, -80}, {10000, -78}, {15000, -75}, {18000, -68}};
    constexpr uint32_t frames = 8000;
    constexpr double amplitude = 16000.0;
    for (const Case &tone : cases) {
        for (uint32_t i = 0; i < frames; i++)
            g_source[i] = int16_t(std::lround(amplitude * std::sin(2.0 * M_PI * tone.hz * i / 44100.0)));
        Resampler cursor = Resampler::make(44100, 48000);
        const uint64_t step = cursor.step;
        const uint32_t n = pcm_resample(g_source, frames, 1, cursor, g_one_call, kScratchSamples);
        double error = 0.0, power = 0.0;
        for (uint32_t k = n / 4; k < n * 3 / 4; k++) {
            const double at = double(k) * double(step) / 4294967296.0;
            const double ideal = amplitude * std::sin(2.0 * M_PI * tone.hz * at / 44100.0);
            error += (g_one_call[k] - ideal) * (g_one_call[k] - ideal);
            power += ideal * ideal;
        }
        CHECK(10.0 * std::log10(error / power) < tone.max_db);
    }
}

TEST(a_sinc_overshoot_clamps_instead_of_wrapping) {
    // A full-scale square rings past the rails between samples. A wrap would show as a sample of
    // the opposite sign beside the edge.
    constexpr uint32_t frames = 400;
    for (uint32_t i = 0; i < frames; i++) g_source[i] = (i / 50) % 2 ? int16_t(-32768) : int16_t(32767);
    Resampler cursor = Resampler::make(44100, 48000);
    const uint32_t n = pcm_resample(g_source, frames, 1, cursor, g_one_call, kScratchSamples);
    bool clamped = false;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t i = uint32_t(double(k) * 44100.0 / 48000.0);
        const uint32_t into = i % 50;
        if (into >= 5 && into < 45) {
            const bool high = (i / 50) % 2 == 0;
            CHECK(high ? g_one_call[k] > 30000 : g_one_call[k] < -30000);
        }
        if (g_one_call[k] == 32767 || g_one_call[k] == -32768) clamped = true;
    }
    CHECK(clamped);
}

TEST(resampling_in_slices_matches_one_call) {
    // Same property as for conversion, and the reason the cursor belongs to the caller: the main
    // loop hands this a few frames at a time and the result must not depend on where the slices
    // fell. 44,100 to 48,000, the ratio that will actually happen.
    constexpr uint32_t frames = 5000;
    constexpr int channels = 2;
    lcg_reset(0xBEEF1u);
    for (uint32_t i = 0; i < frames * channels; i++) g_source[i] = lcg_s16();

    Resampler one = Resampler::make(44100, 48000);
    const uint32_t total = pcm_resample(g_source, frames, channels, one, g_one_call,
                                        kScratchSamples / channels);
    CHECK_EQ(total, one.output_frames(frames));

    // An awkward slice size, deliberately: nothing about 97 lines up with the ratio.
    Resampler sliced = Resampler::make(44100, 48000);
    uint32_t done = 0;
    for (;;) {
        const uint32_t n = pcm_resample(g_source, frames, channels, sliced,
                                        g_sliced + done * channels, 97);
        if (n == 0) break;
        done += n;
    }
    CHECK_EQ(done, total);
    CHECK(std::memcmp(g_one_call, g_sliced, total * channels * sizeof(int16_t)) == 0);
    CHECK_EQ(u64(sliced.position), u64(one.position));

    // And one frame at a time, which is the worst case the design allows.
    Resampler single = Resampler::make(44100, 48000);
    done = 0;
    for (;;) {
        const uint32_t n = pcm_resample(g_source, frames, channels, single,
                                        g_sliced + done * channels, 1);
        if (n == 0) break;
        done += n;
    }
    CHECK_EQ(done, total);
    CHECK(std::memcmp(g_one_call, g_sliced, total * channels * sizeof(int16_t)) == 0);
}

TEST(stereo_keeps_its_channel_order_through_resampling) {
    // A constant signal interpolates to itself, so every output frame must be exactly the input
    // frame, sign included. A swapped channel or a lane read at the wrong stride shows here.
    constexpr uint32_t frames = 100;
    for (uint32_t i = 0; i < frames; i++) {
        g_source[i * 2] = 1000;
        g_source[i * 2 + 1] = -1000;
    }

    const uint32_t pairs[][2] = {{44100, 48000}, {48000, 44100}, {96000, 48000}, {24000, 48000},
                                 {8000, 48000}};
    for (const auto &pair : pairs) {
        Resampler cursor = Resampler::make(pair[0], pair[1]);
        const uint32_t n = pcm_resample(g_source, frames, 2, cursor, g_one_call,
                                        kScratchSamples / 2);
        CHECK(n > 0u);
        for (uint32_t i = 0; i < n; i++) {
            CHECK_EQ(g_one_call[i * 2], 1000);
            CHECK_EQ(g_one_call[i * 2 + 1], -1000);
        }
    }
}

TEST(a_cursor_with_no_step_copies_rather_than_stalling) {
    // A default-constructed Resampler has a step of zero, and a step of zero cannot advance: it
    // would hand the caller frame 0 until the buffer was full and never reach the end of the
    // file. Same rule as a rate that is not a rate: copy.
    const int16_t source[4] = {-5, 7, -9, 11};
    int16_t got[4] = {};
    Resampler cursor;
    CHECK_EQ(pcm_resample(source, 4, 1, cursor, got, 4), 4u);
    for (int i = 0; i < 4; i++) CHECK_EQ(got[i], source[i]);
    CHECK_EQ(u64(cursor.step), u64(kOne));
    CHECK_EQ(Resampler{}.output_frames(10), 10u);
}

TEST(resampling_refuses_what_it_cannot_read) {
    const int16_t source[4] = {1, 2, 3, 4};
    int16_t got[4] = {9, 9, 9, 9};
    Resampler cursor = Resampler::make(48000, 48000);

    CHECK_EQ(pcm_resample(nullptr, 4, 1, cursor, got, 4), 0u);
    CHECK_EQ(pcm_resample(source, 4, 1, cursor, nullptr, 4), 0u);
    CHECK_EQ(pcm_resample(source, 0, 1, cursor, got, 4), 0u);
    CHECK_EQ(pcm_resample(source, 4, 0, cursor, got, 4), 0u);
    CHECK_EQ(pcm_resample(source, 4, -1, cursor, got, 4), 0u);
    CHECK_EQ(pcm_resample(source, 4, 1, cursor, got, 0), 0u);

    CHECK_EQ(u64(cursor.position), 0ull);      // the cursor did not move while refusing
    for (int i = 0; i < 4; i++) CHECK_EQ(got[i], 9);
}

// --- anti-aliasing ----------------------------------------------------------

namespace {

// A mono tone at `hz` in a `rate` file, low-passed if `filter` says so, then resampled to 48 kHz.
// Returns the output's RMS over its middle, past the filter's start-up, as a fraction of the
// input's.
double tone_through(double hz, uint32_t rate, bool filter) {
    constexpr uint32_t frames = 16000;
    constexpr double amplitude = 16000.0;
    for (uint32_t i = 0; i < frames; i++) {
        g_source[i] = int16_t(std::lround(amplitude * std::sin(2.0 * M_PI * hz * i / rate)));
    }
    Antialias lowpass = Antialias::make(rate, 48000);
    if (filter) pcm_antialias(g_source, frames, 1, lowpass);
    Resampler cursor = Resampler::make(rate, 48000);
    const uint32_t n = pcm_resample(g_source, frames, 1, cursor, g_one_call, kScratchSamples);
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t i = n / 4; i < n * 3 / 4; i++, count++) sum += double(g_one_call[i]) * g_one_call[i];
    return std::sqrt(sum / count) / (amplitude / std::sqrt(2.0));
}

}  // namespace

TEST(the_antialias_filter_is_only_for_a_downsample) {
    CHECK(Antialias::make(96000, 48000).active);
    CHECK(Antialias::make(88200, 48000).active);
    CHECK(Antialias::make(48001, 48000).active);
    CHECK(!Antialias::make(48000, 48000).active);
    CHECK(!Antialias::make(44100, 48000).active);
    CHECK(!Antialias::make(0, 48000).active);
    CHECK(!Antialias::make(4000000, 48000).active);

    // Inactive means untouched.
    int16_t samples[4] = {-32768, 5, 32767, -1};
    Antialias off = Antialias::make(44100, 48000);
    pcm_antialias(samples, 2, 2, off);
    CHECK_EQ(samples[0], -32768);
    CHECK_EQ(samples[1], 5);
    CHECK_EQ(samples[2], 32767);
    CHECK_EQ(samples[3], -1);
}

TEST(a_downsample_no_longer_folds_a_high_tone_back_down) {
    // 34 kHz in a 96 kHz file folds to 14 kHz at 48 kHz. Linear interpolation at exactly 2:1
    // takes every other sample, so without the filter the alias arrives at full level: the
    // first check is that this test can see the problem at all.
    CHECK(tone_through(34000.0, 96000, false) > 0.9);
    CHECK(tone_through(34000.0, 96000, true) < 0.03);        // more than 30 dB down
    CHECK(tone_through(30000.0, 88200, true) < 0.1);         // folds to 18 kHz: 20 dB down

    // And what is below the corner passes. 1 kHz and 10 kHz within a tenth of a dB; 18 kHz, near
    // the corner, within 1 dB.
    const double low = tone_through(1000.0, 96000, true);
    CHECK(low > 0.988 && low < 1.012);
    const double mid = tone_through(10000.0, 96000, true);
    CHECK(mid > 0.988 && mid < 1.012);
    const double high = tone_through(18000.0, 96000, true);
    CHECK(high > 0.89 && high < 1.12);
}

TEST(antialiasing_in_slices_matches_one_call) {
    constexpr uint32_t frames = 5000;
    constexpr int channels = 2;
    lcg_reset(0xA11A5u);
    for (uint32_t i = 0; i < frames * channels; i++) g_source[i] = lcg_s16();
    std::memcpy(g_one_call, g_source, frames * channels * sizeof(int16_t));
    std::memcpy(g_sliced, g_source, frames * channels * sizeof(int16_t));

    Antialias one = Antialias::make(96000, 48000);
    pcm_antialias(g_one_call, frames, channels, one);
    Antialias sliced = Antialias::make(96000, 48000);
    for (uint32_t at = 0; at < frames; at += 97) {
        const uint32_t n = frames - at < 97 ? frames - at : 97;
        pcm_antialias(g_sliced + at * channels, n, channels, sliced);
    }
    CHECK(std::memcmp(g_one_call, g_sliced, frames * channels * sizeof(int16_t)) == 0);
    // Something happened: random noise is mostly above 20 kHz at 96 kHz.
    CHECK(std::memcmp(g_one_call, g_source, frames * channels * sizeof(int16_t)) != 0);
}

TEST(the_antialias_filter_keeps_channels_apart_and_clamps_instead_of_wrapping) {
    // A full-scale square in the left channel rings past the rails; the right channel is silent
    // and must stay silent. A wrap would show as a sample of the wrong sign mid-plateau.
    constexpr uint32_t frames = 4000;
    for (uint32_t i = 0; i < frames; i++) {
        g_source[i * 2] = (i / 200) % 2 ? int16_t(-32768) : int16_t(32767);
        g_source[i * 2 + 1] = 0;
    }
    Antialias filter = Antialias::make(96000, 48000);
    pcm_antialias(g_source, frames, 2, filter);
    bool clamped = false;
    for (uint32_t i = 0; i < frames; i++) {
        CHECK_EQ(g_source[i * 2 + 1], 0);
        const uint32_t into = i % 200;
        if (into >= 20 && into < 180) {       // the plateau, past the ringing
            const bool high = (i / 200) % 2 == 0;
            CHECK(high ? g_source[i * 2] > 30000 : g_source[i * 2] < -30000);
        }
        if (g_source[i * 2] == 32767 || g_source[i * 2] == -32768) clamped = true;
    }
    CHECK(clamped);
    // More channels than it keeps state for: left alone rather than half-filtered.
    int16_t three[3] = {100, 200, 300};
    Antialias other = Antialias::make(96000, 48000);
    pcm_antialias(three, 1, 3, other);
    CHECK_EQ(three[0], 100);
    CHECK_EQ(three[2], 300);
}

// --- the anti-aliasing loop in core/pcmloops.ti.c ---------------------------------------------

namespace {

// pcm_antialias's loop as it was written in core/pcm.cpp before it moved to core/pcmloops.ti.c,
// kept here as the reference the move is held to.
void antialias_reference(int16_t *samples, uint32_t frames, int channels, Antialias &filter) {
    for (int ch = 0; ch < channels; ch++) {
        float *z1 = filter.z1[ch];
        float *z2 = filter.z2[ch];
        int16_t *p = samples + ch;
        for (uint32_t i = 0; i < frames; i++, p += channels) {
            float x = float(*p);
            for (int k = 0; k < Antialias::kSections; k++) {
                const float y = filter.b0[k] * x + z1[k];
                z1[k] = filter.b1[k] * x - filter.a1[k] * y + z2[k];
                z2[k] = filter.b2[k] * x - filter.a2[k] * y;
                x = y;
            }
            const float r = x + (x >= 0.0f ? 0.5f : -0.5f);
            const int32_t v = r >= 32767.0f ? 32767 : (r <= -32768.0f ? -32768 : int32_t(r));
            *p = int16_t(v);
        }
    }
}

}  // namespace

TEST(the_antialias_loop_is_bit_exact_against_the_loop_it_replaced) {
    // Mono and stereo noise with full-scale square bursts in it, so the clamp and both rounding halves
    // are exercised, filtered in uneven slices with the state carried between them -- the way
    // Storage feeds it -- at two source rates.
    constexpr uint32_t frames = 20000;
    static int16_t moved[2 * frames];
    static int16_t reference[2 * frames];
    const uint32_t rates[2] = {96000, 88200};
    for (int channels = 1; channels <= 2; channels++)
    for (uint32_t rate : rates) {
        uint32_t seed = 0x2468ACE1u;
        for (uint32_t i = 0; i < 2 * frames; i++) {
            seed = seed * 1664525u + 1013904223u;
            const bool burst = (i / 997) % 5 == 0;
            moved[i] = burst ? int16_t((i / 23) % 2 == 0 ? 32767 : -32768)
                             : int16_t(int32_t(seed >> 16) - 32768);
            reference[i] = moved[i];
        }
        Antialias a = Antialias::make(rate, 48000);
        Antialias b = Antialias::make(rate, 48000);
        CHECK(a.active);
        uint32_t done = 0;
        uint32_t slice = 1;
        while (done < frames) {
            const uint32_t n = frames - done < slice ? frames - done : slice;
            pcm_antialias(moved + channels * done, n, channels, a);
            antialias_reference(reference + channels * done, n, channels, b);
            done += n;
            slice = slice * 3 + 7;              // 1, 10, 37, 118, ... uneven on purpose
        }
        int differing = 0;
        for (uint32_t i = 0; i < 2 * frames; i++) differing += moved[i] != reference[i] ? 1 : 0;
        CHECK_EQ(differing, 0);
        for (int ch = 0; ch < channels; ch++) {
            for (int k = 0; k < Antialias::kSections; k++) {
                CHECK(a.z1[ch][k] == b.z1[ch][k]);
                CHECK(a.z2[ch][k] == b.z2[ch][k]);
            }
        }
    }
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(a_ratio_that_is_not_a_power_of_two_gets_the_step_long_division_says) {
    // The other step tests are powers of two, which a long division gets right even with its
    // remainder comparison off by one. 44.1 to 48 kHz and 48 to 44.1 kHz are the common ones.
    const uint64_t up = ((uint64_t(44100) << 32) + 48000 - 1) / 48000;
    const uint64_t down = ((uint64_t(48000) << 32) + 44100 - 1) / 44100;
    CHECK_EQ(u64(Resampler::make(44100, 48000).step), u64(up));
    CHECK_EQ(u64(Resampler::make(48000, 44100).step), u64(down));
    const uint64_t odd = ((uint64_t(32000) << 32) + 44100 - 1) / 44100;
    CHECK_EQ(u64(Resampler::make(32000, 44100).step), u64(odd));
    // The frame count is the same long division the other way round.
    const Resampler r = Resampler::make(44100, 48000);
    const uint64_t frames = ((uint64_t(1000003) << 32) + up - 1) / up;
    CHECK_EQ(u64(r.output_frames(1000003)), u64(frames));
}

TEST(the_rate_limits_themselves_are_believable_rates) {
    CHECK_EQ(u64(Resampler::make(1000, 1000).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(768000, 768000).step), u64(kOne));
    CHECK_EQ(u64(Resampler::make(768000, 1000).step), u64(768 * kOne));
    CHECK_EQ(u64(Resampler::make(1000, 768000).step),
             u64(((uint64_t(1000) << 32) + 768000 - 1) / 768000));
    CHECK(Antialias::make(768000, 1000).active);
    CHECK(Antialias::make(2000, 1000).active);
    CHECK(!Antialias::make(999, 500).active);
}

TEST(an_interpolated_frame_rounds_to_nearest) {
    // A step from 0 to 1, read three quarters of the way across: the kernel puts well over half
    // its weight on the 1, which rounds to 1; truncating would give 0.
    const int16_t in[2] = {0, 1};
    int16_t out[8] = {};
    Resampler cursor;
    cursor.step = kOne / 4;
    CHECK(pcm_resample(in, 2, 1, cursor, out, 8) >= 4);
    CHECK_EQ(int(out[3]), 1);
}

TEST(the_antialias_coefficients_are_the_butterworth_the_header_describes) {
    // Against the textbook formula with the host's libm: the Taylor series in core/pcm.cpp has
    // to land on the same biquads to float precision.
    const uint32_t from = 96000, to = 48000;
    const Antialias f = Antialias::make(from, to);
    CHECK(f.active);
    const double k = std::tan(3.14159265358979323846 * (double(to) * 5.0 / 12.0) / double(from));
    for (int i = 0; i < Antialias::kSections; i++) {
        const double q = 1.0 / (2.0 * std::cos(double(2 * i + 1) * 3.14159265358979323846 / 16.0));
        const double norm = 1.0 / (1.0 + k / q + k * k);
        CHECK_NEAR(f.b0[i], float(k * k * norm), 1e-6);
        CHECK_NEAR(f.b1[i], float(2.0 * k * k * norm), 1e-6);
        CHECK_NEAR(f.a1[i], float(2.0 * (k * k - 1.0) * norm), 1e-6);
        CHECK_NEAR(f.a2[i], float((1.0 - k / q + k * k) * norm), 1e-6);
    }
}
