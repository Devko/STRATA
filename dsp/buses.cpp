// The mixer, the two sends and the master chain. dsp/buses.hpp has the block protocol.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Delay and reverb measured 16% of the block together (docs/SPEC.md, "CPU and memory budget"),
// more than all 18 voices put together, and they are a FIXED tax: they cost the same whether
// one track sounds or eight. Two things here are aimed squarely at that:
//
//   * Both sends stop running once nothing has fed them for a whole tail. Feeding is tracked by
//     the send gains the chains report, which the app only sets on tracks that are sounding, so
//     a kit with the sends at zero pays nothing for either effect instead of 16%.
//   * The sends are mono, which is where a track's send cost goes from three bus passes to one.
//
// Expected order, per block: reverb (eight damped lines plus four all-passes, all per sample)
// first by a wide margin, then the delay (two interpolated taps and the tone filter), then the
// master's limiter (a cubic per sample per channel) and DJ filter (two cascades), then
// everything else. The master's glue compressor is deliberately NOT per sample: its detector is
// the block's peak and its gain is one value a block, ramped across the block with the output
// level in a single pass, which is 1.33 ms of attack quantisation and perhaps a twentieth of
// the cost of a per-sample detector.
//
// The master's four hand-written loops -- the peak detector, the gain ramp, the limiter and the
// interleave -- are the fixed tax that runs on every block however few tracks sound, so they
// live in dsp/masterloops.ti.c, which build_app.py sends to cl6x while this file goes through
// GCC (CLAUDE.md's *.ti.c rule; docs/BUDGET.md, M2, has the figures). The arithmetic moved
// unchanged.
#include "buses.hpp"

#include "masterloops.h"
#include "filterloops.h"

#include <cstddef>

namespace strata {
namespace {

float clamp_to(float value, float low, float high) {
    const float lowered = value < low ? low : value;
    return lowered > high ? high : lowered;
}

}  // namespace

// --- the send and master fields, by index ---------------------------------------------------
//
// Every setter CLAMPS to the range its field's comment in buses.hpp states, and that is the point
// of routing them through here rather than letting a caller assign: a reverb decay of 0 is a
// divide waiting to happen and a feedback of 1.0 is a delay that never stops growing. The UI
// clamps too, but the UI is not the only caller -- a loaded file is one, and a file can hold
// anything.

bool send_field_set(SendSettings &settings, uint8_t field, float value) {
    switch (SendField(field)) {
        case SendField::delay_seconds:
            // Up to the LONG delay's limit: the classic delay clamps to its own 2 s when it
            // resolves, so a slow 1/1 still lands there exactly as it always did.
            settings.delay_seconds = clamp_to(value, 0.001f, Mixer::kMaxLongDelaySeconds);
            return true;
        case SendField::delay_feedback:
            settings.delay_feedback = clamp_to(value, 0.0f, 0.95f);
            return true;
        case SendField::delay_tone:
            settings.delay_tone = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::delay_ping_pong:
            settings.delay_ping_pong = value >= 0.5f;
            return true;
        case SendField::delay_level:
            settings.delay_level = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::delay_to_reverb:
            settings.delay_to_reverb = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::reverb_decay_s:
            settings.reverb_decay_s = clamp_to(value, 0.2f, 30.0f);
            return true;
        case SendField::reverb_damping:
            settings.reverb_damping = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::reverb_high_pass_hz:
            settings.reverb_high_pass_hz = clamp_to(value, 20.0f, 2000.0f);
            return true;
        case SendField::reverb_level:
            settings.reverb_level = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::delay_mode:
            settings.delay_long = value >= 0.5f;
            return true;
        case SendField::delay_multiplier:
            settings.delay_multiplier = clamp_to(value, 1.0f, 8.0f);
            return true;
        case SendField::delay_wow:
            settings.delay_wow = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::delay_diffusion:
            settings.delay_diffusion = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::delay_low_cut_hz:
            settings.delay_low_cut_hz = clamp_to(value, 20.0f, 1000.0f);
            return true;
        case SendField::reverb_mode:
            settings.reverb_hall = value >= 0.5f;
            return true;
        case SendField::reverb_size:
            settings.reverb_size = clamp_to(value, HallReverb::kMinSize, HallReverb::kMaxSize);
            return true;
        case SendField::reverb_mod:
            settings.reverb_mod = clamp_to(value, 0.0f, 1.0f);
            return true;
        case SendField::reverb_shimmer:
            settings.reverb_shimmer = clamp_to(value, 0.0f, 1.0f);
            return true;
        default:
            return false;
    }
}

float send_field_get(const SendSettings &settings, uint8_t field) {
    switch (SendField(field)) {
        case SendField::delay_seconds:       return settings.delay_seconds;
        case SendField::delay_feedback:      return settings.delay_feedback;
        case SendField::delay_tone:          return settings.delay_tone;
        case SendField::delay_ping_pong:     return settings.delay_ping_pong ? 1.0f : 0.0f;
        case SendField::delay_level:         return settings.delay_level;
        case SendField::delay_to_reverb:     return settings.delay_to_reverb;
        case SendField::reverb_decay_s:      return settings.reverb_decay_s;
        case SendField::reverb_damping:      return settings.reverb_damping;
        case SendField::reverb_high_pass_hz: return settings.reverb_high_pass_hz;
        case SendField::reverb_level:        return settings.reverb_level;
        case SendField::delay_mode:          return settings.delay_long ? 1.0f : 0.0f;
        case SendField::delay_multiplier:    return settings.delay_multiplier;
        case SendField::delay_wow:           return settings.delay_wow;
        case SendField::delay_diffusion:     return settings.delay_diffusion;
        case SendField::delay_low_cut_hz:    return settings.delay_low_cut_hz;
        case SendField::reverb_mode:         return settings.reverb_hall ? 1.0f : 0.0f;
        case SendField::reverb_size:         return settings.reverb_size;
        case SendField::reverb_mod:          return settings.reverb_mod;
        case SendField::reverb_shimmer:      return settings.reverb_shimmer;
        default:                             return 0.0f;
    }
}

const char *send_field_name(uint8_t field) {
    static const char *const names[] = {"TIM", "FBK", "TON", "PNG", "LVL", "D>R", "DEC",
                                        "DMP", "HPF", "LVL", "TYP", "MUL", "WOW", "DIF",
                                        "LCT", "TYP", "SIZ", "MOD", "SHM"};
    static_assert(sizeof(names) / sizeof(names[0]) == std::size_t(SendField::count),
                  "a send field without a name");
    return field < uint8_t(SendField::count) ? names[field] : "---";
}

bool master_field_set(MasterSettings &settings, uint8_t field, float value) {
    switch (MasterField(field)) {
        case MasterField::dj:
            settings.dj = clamp_to(value, -1.0f, 1.0f);      // signed: low-pass below 0
            return true;
        case MasterField::compressor:
            settings.compressor = clamp_to(value, 0.0f, 1.0f);
            return true;
        case MasterField::limit:
            // The same window `resolve_master` honours, not a wider one: a field that reads back
            // 0.05 while the limiter is running at 0.1 is a lie to whoever draws it. Not down to
            // 0, because a threshold of zero is a limiter that removes everything.
            settings.limit = clamp_to(value, 0.1f, 0.98f);
            return true;
        case MasterField::level:
            settings.level = clamp_to(value, 0.0f, 1.0f);
            return true;
        case MasterField::eq_low:
            settings.eq_low = clamp_to(value, 0.0f, 2.0f);
            return true;
        case MasterField::eq_mid:
            settings.eq_mid = clamp_to(value, 0.0f, 2.0f);
            return true;
        case MasterField::eq_high:
            settings.eq_high = clamp_to(value, 0.0f, 2.0f);
            return true;
        default:
            return false;
    }
}

float master_field_get(const MasterSettings &settings, uint8_t field) {
    switch (MasterField(field)) {
        case MasterField::dj:         return settings.dj;
        case MasterField::compressor: return settings.compressor;
        case MasterField::limit:      return settings.limit;
        case MasterField::level:      return settings.level;
        case MasterField::eq_low:     return settings.eq_low;
        case MasterField::eq_mid:     return settings.eq_mid;
        case MasterField::eq_high:    return settings.eq_high;
        default:                      return 0.0f;
    }
}

const char *master_field_name(uint8_t field) {
    static const char *const names[] = {"DJ", "CMP", "LIM", "LVL", "LOW", "MID", "HI"};
    return field < uint8_t(MasterField::count) ? names[field] : "---";
}

// --- the encoder domain ---------------------------------------------------------------------
//
// A byte is what the encoder produces and what PROJECT.BIN stores (core/project.hpp says why).
// These turn one into the value the mixer runs on. They are CONTROL RATE at most -- a knob move
// or a project load -- so the two exponential fields are allowed their exp2.

namespace {

// 0..127 as 0..1. 1/127 rather than 1/128 so that 127 is exactly 1.0: a level knob at the top
// must be unity gain, not 0.992 of it.
constexpr float kByteScale = 1.0f / 127.0f;

float byte_unit(uint8_t byte) { return float(byte > 127 ? 127 : byte) * kByteScale; }

// Exponential, for the two fields where equal encoder steps should be equal musical steps: a
// reverb decay knob that is linear in seconds spends four fifths of its travel in cathedrals.
float byte_exponential(uint8_t byte, float low, float octaves) {
    return low * enaga::dsp::exp2(octaves * byte_unit(byte));
}

// The delay's divisions, in BEATS -- quarter notes, which is what a tempo in BPM counts.
// Straight, triplet and dotted, shortest first, so that turning the encoder up makes the delay
// longer. 1/1 is a bar of 4/4; at 130 BPM that is 1.85 s, just inside Mixer::kMaxDelaySeconds,
// and a slower tempo is clamped there by `send_field_set`.
constexpr float kDivisionBeats[kDelayDivisions] = {
    0.125f,          // 1/32
    1.0f / 6.0f,     // 1/16T
    0.25f,           // 1/16
    1.0f / 3.0f,     // 1/8T
    0.5f,            // 1/8
    0.75f,           // 1/8D
    1.0f,            // 1/4
    1.5f,            // 1/4D
    2.0f,            // 1/2
    4.0f,            // 1/1
};


}  // namespace

namespace {
constexpr int kMultipliers[kDelayMultipliers] = {1, 2, 3, 4, 6, 8};
}  // namespace

// The multiplier's bands, as the divisions' are: six of them across 0-127, each byte the middle.
int delay_multiplier_index(uint8_t byte) {
    const int index = int(byte > 127 ? 127 : byte) * kDelayMultipliers / 128;
    return index < kDelayMultipliers ? index : kDelayMultipliers - 1;
}

uint8_t delay_multiplier_byte(int index) {
    const int clamped =
        index < 0 ? 0 : (index >= kDelayMultipliers ? kDelayMultipliers - 1 : index);
    return uint8_t((clamped * 128 + 64) / kDelayMultipliers);
}

int delay_multiplier_value(uint8_t byte) { return kMultipliers[delay_multiplier_index(byte)]; }

int delay_division_index(uint8_t byte) {
    const int index = int(byte > 127 ? 127 : byte) * kDelayDivisions / 128;
    return index < kDelayDivisions ? index : kDelayDivisions - 1;
}

uint8_t delay_division_byte(int index) {
    const int clamped = index < 0 ? 0 : (index >= kDelayDivisions ? kDelayDivisions - 1 : index);
    // The MIDDLE of the band, not its edge. The bands are 12.8 bytes wide, not 12 -- 128 does
    // not divide by ten -- so this is (index + 1/2) * 128/10 in integers. Rounding it down to
    // twelve put the top division's byte in the band below it, and `delay_division_byte(9)`
    // read back as division 8.
    return uint8_t((clamped * 128 + 64) / kDelayDivisions);
}

const char *delay_division_name(uint8_t byte) {
    static const char *const names[kDelayDivisions] = {"1/32", "1/16T", "1/16", "1/8T", "1/8",
                                                       "1/8D", "1/4",   "1/4D", "1/2",  "1/1"};
    return names[delay_division_index(byte)];
}

float delay_division_seconds(uint8_t byte, float beat_seconds) {
    return kDivisionBeats[delay_division_index(byte)] * beat_seconds;
}

namespace {

uint8_t stepped(uint8_t byte, int delta) {
    const int value = int(byte) + delta;
    return uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

}  // namespace

int send_field_click(uint8_t field) {
    switch (SendField(field)) {
        case SendField::delay_seconds:   return 1;    // a division
        case SendField::delay_ping_pong: return 1;    // a sign
        case SendField::delay_mode:      return 1;    // a sign
        case SendField::reverb_mode:     return 1;    // a sign
        case SendField::delay_multiplier: return 1;   // a multiplier
        default:                         return 2;    // a knob
    }
}

uint8_t send_byte_step(uint8_t field, uint8_t byte, int delta) {
    switch (SendField(field)) {
        case SendField::delay_seconds: {
            // One click is one DIVISION, and the ends HOLD rather than wrap: a delay that jumps
            // from a bar to a 32nd because the knob went one click too far is not a musical
            // control.
            const int index = delay_division_index(byte) + delta;
            return delay_division_byte(index < 0 ? 0
                                                 : (index >= kDelayDivisions ? kDelayDivisions - 1
                                                                             : index));
        }
        case SendField::delay_multiplier: {
            const int index = delay_multiplier_index(byte) + delta;
            return delay_multiplier_byte(index);
        }
        case SendField::delay_ping_pong:
        case SendField::delay_mode:
        case SendField::reverb_mode:
            // A switch, not a knob: either direction throws it.
            return delta > 0 ? 127 : (delta < 0 ? 0 : byte);
        default:
            return field < uint8_t(SendField::count) ? stepped(byte, delta) : byte;
    }
}

uint8_t master_byte_step(uint8_t field, uint8_t byte, int delta) {
    return field < uint8_t(MasterField::count) ? stepped(byte, delta) : byte;
}

float send_value_from_byte(uint8_t field, uint8_t byte, float beat_seconds) {
    switch (SendField(field)) {
        case SendField::delay_seconds:   return delay_division_seconds(byte, beat_seconds);
        case SendField::delay_feedback:  return byte_unit(byte) * 0.95f;
        case SendField::delay_tone:      return byte_unit(byte);
        // A switch, not a knob: the upper half of the encoder's travel is on.
        case SendField::delay_ping_pong: return byte >= 64 ? 1.0f : 0.0f;
        case SendField::delay_level:     return byte_unit(byte);
        case SendField::delay_to_reverb: return byte_unit(byte);
        // 0.2 s to 30 s: log2(150) octaves.
        case SendField::reverb_decay_s:  return byte_exponential(byte, 0.2f, 7.2288f);
        case SendField::reverb_damping:  return byte_unit(byte);
        // 20 Hz to 2 kHz: log2(100) octaves.
        case SendField::reverb_high_pass_hz: return byte_exponential(byte, 20.0f, 6.6439f);
        case SendField::reverb_level:    return byte_unit(byte);
        case SendField::delay_mode:      return byte >= 64 ? 1.0f : 0.0f;
        case SendField::delay_multiplier: return float(delay_multiplier_value(byte));
        case SendField::delay_wow:       return byte_unit(byte);
        case SendField::delay_diffusion: return byte_unit(byte);
        // 20 Hz to 1 kHz: log2(50) octaves.
        case SendField::delay_low_cut_hz: return byte_exponential(byte, 20.0f, 5.6439f);
        case SendField::reverb_mode:     return byte >= 64 ? 1.0f : 0.0f;
        // 0.35 to 1.8 times the hall's lines: log2(1.8 / 0.35) octaves.
        case SendField::reverb_size:     return byte_exponential(byte, 0.35f, 2.3626f);
        case SendField::reverb_mod:      return byte_unit(byte);
        case SendField::reverb_shimmer:  return byte_unit(byte);
        default:                         return 0.0f;
    }
}

float master_value_from_byte(uint8_t field, uint8_t byte) {
    switch (MasterField(field)) {
        // The one SIGNED field. 64 is exactly 0, which is the DJ filter's flat, bypassed centre,
        // so the two halves get 64 and 63 positions rather than a centre that is slightly on.
        case MasterField::dj:
            return byte <= 64 ? (float(byte) - 64.0f) * (1.0f / 64.0f)
                              : float(byte - 64) * (1.0f / 63.0f);
        case MasterField::compressor: return byte_unit(byte);
        case MasterField::limit:      return 0.1f + byte_unit(byte) * 0.88f;
        case MasterField::level:      return byte_unit(byte);
        // The kill EQ's bands: centred like the DJ filter, 64 exactly flat. Below it a square
        // law down to a kill at 0 (-12 dB half way down, which is where a DJ isolator's knob
        // starts to sound like a cut); above it up to +6 dB at 127.
        case MasterField::eq_low:
        case MasterField::eq_mid:
        case MasterField::eq_high: {
            if (byte >= 64) return 1.0f + float((byte > 127 ? 127 : byte) - 64) * (1.0f / 63.0f);
            const float u = float(byte) * (1.0f / 64.0f);
            return u * u;
        }
        default:                      return 0.0f;
    }
}

void sends_from_bytes(SendSettings &settings, const uint8_t *bytes, float beat_seconds) {
    if (bytes == nullptr) return;
    for (uint8_t f = 0; f < uint8_t(SendField::count); f++) {
        send_field_set(settings, f, send_value_from_byte(f, bytes[f], beat_seconds));
    }
}

float effective_delay_seconds(const SendSettings &settings) {
    if (!settings.delay_long) return clamp_to(settings.delay_seconds, 0.001f, Mixer::kMaxDelaySeconds);
    return clamp_to(settings.delay_seconds * settings.delay_multiplier, 0.001f,
                    Mixer::kMaxLongDelaySeconds);
}

void master_from_bytes(MasterSettings &settings, const uint8_t *bytes) {
    if (bytes == nullptr) return;
    for (uint8_t f = 0; f < uint8_t(MasterField::count); f++) {
        master_field_set(settings, f, master_value_from_byte(f, bytes[f]));
    }
}

namespace ed = enaga::dsp;

namespace {

// The reverb's right input. Its input is mono, the C block functions take restrict pointers, so
// the two inputs cannot be the same buffer; the network is linear, so a silent right channel
// with the mix doubled is exactly a mono feed into both.
const float kSilence[Mixer::kMaxFrames] = {};

// DJ filter: flat (and bypassed) inside the dead zone around the centre.
constexpr float kDjDeadZone = 0.03f;
constexpr float kDjSpan = 1.0f / (1.0f - kDjDeadZone);
constexpr float kDjLowTopHz = 20000.0f;     // the low-pass sweeps down from here...
constexpr float kDjLowOctaves = 8.0f;       // ...to 78 Hz
constexpr float kDjHighLowHz = 20.0f;       // the high-pass sweeps up from here...
constexpr float kDjHighOctaves = 8.6f;      // ...to 7.7 kHz
constexpr float kDjResonanceBase = 0.25f;
constexpr float kDjResonanceSpan = 0.35f;

// Glue compressor: a 4:1 knee above -6 dB at full amount, 10 ms attack and 150 ms release,
// smoothed once a block.
constexpr float kCompressorThreshold = 0.5f;
constexpr float kCompressorSlope = 0.75f;   // 1 - 1/4
constexpr float kCompressorAttackRate = 100.0f;      // 1 / 10 ms
constexpr float kCompressorReleaseRate = 6.6667f;    // 1 / 150 ms

// Kill EQ: each band's gain moves this far to its target per block (64 frames), so a kill is
// -6 dB after 1.3 ms, -40 dB after 9 ms and gone after 13 ms -- quick enough to land on the one,
// smooth enough not to click on a full-scale kick. The stage fades in and out over four blocks
// (5.3 ms), and a gain this close to its target is taken as there.
constexpr float kKillEqGlide = 0.5f;
constexpr float kKillEqFadeStep = 0.25f;
constexpr float kKillEqSnap = 0.0001f;
constexpr float kButterworthK = 1.41421356f;   // 1 / Q for Q = 0.7071

static_assert(Mixer::kKillEqStates == STRATA_KILL_EQ_STATES,
              "the kill EQ's state size and its loop's have diverged");
static_assert(sizeof(enaga::dsp::SvfCoefficients) == 4 * sizeof(float),
              "strata_kill_eq reads SvfCoefficients as a1 a2 a3 k");

float largest(float a, float b) { return a > b ? a : b; }

}  // namespace

void Mixer::init(float sample_rate) {
    rate_ = sample_rate;

    // The only allocation in this file, and the reason init() belongs in App::init().
    delay_.init(kMaxDelaySeconds, sample_rate);
    // Two delay times: with ping-pong the echoes swap sides each pass and repeat every second.
    freeze_.init(sample_rate, 2.0f * kMaxDelaySeconds);
    reverb_.init(sample_rate);
    // The ambient modes' own lines (dsp/ambient.hpp): 4.4 MiB for 12 s of stereo, 0.55 MiB of hall.
    long_delay_.init(sample_rate);
    hall_.init(sample_rate);

    duck_.init(sample_rate);
    for (int t = 0; t < kTracks; t++) chains_[t].init(sample_rate);

    reverb_high_pass_.reset();
    dj_l_a_.reset();
    dj_l_b_.reset();
    dj_r_a_.reset();
    dj_r_b_.reset();
    for (int i = 0; i < kKillEqStates; i++) {
        eq_state_l_[i] = 0.0f;
        eq_state_r_[i] = 0.0f;
    }
    for (int b = 0; b < kKillBands; b++) {
        eq_target_[b] = 1.0f;
        eq_gain_[b] = 1.0f;
    }
    eq_wet_ = 0.0f;
    eq_low_split_ = ed::SvfCoefficients::from_g(ed::tan_pi(kKillLowHz * ed::reciprocal(sample_rate)),
                                                kButterworthK);
    eq_high_split_ = ed::SvfCoefficients::from_g(
        ed::tan_pi(kKillHighHz * ed::reciprocal(sample_rate)), kButterworthK);
    compressor_gain_ = 1.0f;
    master_gain_ = 1.0f;
    delay_tail_ = 0;
    reverb_tail_ = 0;
    long_tail_ = 0;
    hall_tail_ = 0;

    prepare(SendSettings{}, MasterSettings{});
}

void Mixer::prepare(const SendSettings &sends, const MasterSettings &master_settings) {
    // Everything either call below resolves is derived purely from its struct, so a knob that
    // has not moved costs one comparison instead of a biquad design, the eight exp2 calls
    // inside Reverb::set and the rest of it.
    if (!resolved_ || !(sends == last_sends_)) {
        last_sends_ = sends;
        resolve_sends(sends);
    }
    if (!resolved_ || !(master_settings == last_master_)) {
        last_master_ = master_settings;
        resolve_master(master_settings);
    }
    resolved_ = true;
}

void Mixer::resolve_sends(const SendSettings &sends) {
    // --- send A: the delay. Seconds in, because the app owns the tempo (CLAUDE.md). ----------
    const float seconds = ed::clamp(sends.delay_seconds, 0.001f, kMaxDelaySeconds);
    const float feedback = ed::clamp(sends.delay_feedback, 0.0f, 0.95f);
    delay_.set(seconds, feedback, ed::clamp(sends.delay_tone, 0.0f, 1.0f), sends.delay_ping_pong);
    delay_loop_frames_ = int(seconds * rate_) * (sends.delay_ping_pong ? 2 : 1);
    delay_level_ = ed::clamp(sends.delay_level, 0.0f, 4.0f);
    delay_to_reverb_ = ed::clamp(sends.delay_to_reverb, 0.0f, 1.0f);

    // --- send A's LONG mode ---------------------------------------------------------------------
    // Resolved whichever mode is on, so a switch to it finds the settings already in place. A
    // delay with nothing sounding jumps to its new time; one that is ringing glides there.
    long_mode_ = sends.delay_long;
    const float long_seconds = ed::clamp(sends.delay_seconds * sends.delay_multiplier, 0.001f,
                                         kMaxLongDelaySeconds);
    long_delay_.set(long_seconds, feedback, sends.delay_tone, sends.delay_low_cut_hz,
                    sends.delay_wow, sends.delay_diffusion, sends.delay_ping_pong,
                    long_tail_ == 0);

    // --- send B's HALL mode ----------------------------------------------------------------------
    hall_mode_ = sends.reverb_hall;
    hall_.set(sends.reverb_decay_s, sends.reverb_damping, sends.reverb_size, sends.reverb_mod,
              sends.reverb_shimmer, hall_tail_ == 0);

    // --- send B: the reverb and the high-pass on its input -----------------------------------
    reverb_.set(sends.reverb_decay_s, sends.reverb_damping);
    reverb_level_ = ed::clamp(sends.reverb_level, 0.0f, 4.0f);
    reverb_high_pass_coefficients_ = ed::BiquadCoefficients::make(
        ed::BiquadCoefficients::high_pass, ed::clamp(sends.reverb_high_pass_hz, 20.0f, 2000.0f),
        0.7071f, rate_);

    // --- how long each send keeps running after it stops being fed ---------------------------
    //
    // Long enough for the tail to fall 60 dB, and no shorter. The send stops RUNNING when this
    // runs out, which freezes the delay line and the reverb tanks with what they held; so a
    // tail cut short was audible twice -- echoes still at -18 dB stopped dead (an 8 s cap, at
    // feedback 0.95 on a 2 s delay), and the frozen buffer played back the next time the send
    // was fed. At -60 dB neither is audible.
    //
    // The delay: one pass per repeat, and -60 dB after log(0.001) / log(feedback) repeats. The
    // reverb: its decay setting is the RT60 (dsp/buses.hpp), plus a margin for the pre-delay
    // and the diffusion. Neither changes the worst case docs/BUDGET.md measures, which already
    // has both sends running; this only decides how long they keep running after the last hit.
    const float blocks_per_second = rate_ * ed::reciprocal(float(kMaxFrames));
    constexpr float kLog2Of60dB = -9.9658f;          // log2(0.001)
    const float repeats = feedback > 0.001f ? kLog2Of60dB * ed::reciprocal(ed::log2(feedback))
                                            : 0.0f;
    const float delay_tail_s = ed::clamp(seconds * (repeats + 1.0f) + 0.05f, 0.05f, kMaxTailSeconds);
    delay_tail_blocks_ = int(delay_tail_s * blocks_per_second) + 1;
    const float reverb_tail_s = ed::clamp(sends.reverb_decay_s * 1.2f + 0.2f, 0.3f, kMaxTailSeconds);
    reverb_tail_blocks_ = int(reverb_tail_s * blocks_per_second) + 1;
    // The LONG delay: the same count over its own period, plus the all-passes' smear. The HALL:
    // its lines are longer than the classic's and the shimmer re-feeds the tail, so half again.
    const float long_tail_s =
        ed::clamp(long_seconds * (repeats + 1.0f) + 0.25f, 0.3f, kMaxTailSeconds);
    long_tail_blocks_ = int(long_tail_s * blocks_per_second) + 1;
    const float hall_tail_s = ed::clamp(
        sends.reverb_decay_s * (1.3f + 0.7f * ed::clamp(sends.reverb_shimmer, 0.0f, 1.0f)) + 0.5f,
        0.5f, kMaxTailSeconds);
    hall_tail_blocks_ = int(hall_tail_s * blocks_per_second) + 1;
}

void Mixer::resolve_master(const MasterSettings &master_settings) {
    // --- master: the one-knob DJ filter ------------------------------------------------------
    const float dj = ed::clamp(master_settings.dj, -1.0f, 1.0f);
    const float swing = dj < 0.0f ? -dj : dj;
    dj_on_ = swing > kDjDeadZone;
    if (dj_on_) {
        const float amount = (swing - kDjDeadZone) * kDjSpan;
        float cutoff;
        if (dj < 0.0f) {
            dj_output_ = ed::Svf::low_pass;
            cutoff = kDjLowTopHz * ed::exp2(-kDjLowOctaves * amount);
        } else {
            dj_output_ = ed::Svf::high_pass;
            cutoff = kDjHighLowHz * ed::exp2(kDjHighOctaves * amount);
        }
        dj_coefficients_ = ed::SvfCoefficients::make(
            cutoff, kDjResonanceBase + kDjResonanceSpan * amount, rate_);
    }

    // --- master: the glue compressor ---------------------------------------------------------
    const float compressor = ed::clamp(master_settings.compressor, 0.0f, 1.0f);
    compressor_on_ = compressor > 0.0f;
    compressor_slope_ = compressor * kCompressorSlope;
    compressor_inv_threshold_ = ed::reciprocal(kCompressorThreshold);
    const float block_s = float(kMaxFrames) * ed::reciprocal(rate_);
    compressor_attack_ = 1.0f - ed::exp(-block_s * kCompressorAttackRate);
    compressor_release_ = 1.0f - ed::exp(-block_s * kCompressorReleaseRate);
    if (!compressor_on_) compressor_gain_ = 1.0f;

    // --- master: the soft limiter and the output level ---------------------------------------
    // The threshold is what moves; the ceiling is always +-1, because the excess above the
    // threshold goes through enaga_soft_clip, which saturates at exactly 1.
    limit_threshold_ = ed::clamp(master_settings.limit, 0.1f, 0.98f);
    limit_headroom_ = 1.0f - limit_threshold_;
    limit_inv_headroom_ = ed::reciprocal(limit_headroom_);
    out_level_ = ed::clamp(master_settings.level, 0.0f, 1.0f);

    // --- master: the kill EQ's targets, with the switches over the knobs ----------------------
    const float knobs[kKillBands] = {master_settings.eq_low, master_settings.eq_mid,
                                     master_settings.eq_high};
    for (int b = 0; b < kKillBands; b++) {
        const bool killed = (master_settings.kills >> b & 1u) != 0;
        eq_target_[b] = killed ? 0.0f : ed::clamp(knobs[b], 0.0f, 2.0f);
    }
}

void Mixer::begin_block(int frames) {
    frames_ = frames < 0 ? 0 : (frames > kMaxFrames ? kMaxFrames : frames);
    if (frames_ == 0) return;
    enaga_clear(mix_l_, frames_);
    enaga_clear(mix_r_, frames_);
    enaga_clear(send_a_, frames_);
    enaga_clear(send_b_, frames_);
    // One duck envelope for the whole mixer; false means idle, and then no chain touches it.
    duck_active_ = duck_.render(duck_curve_, frames_);
}

void Mixer::add_track(int track, float *mono) { add_track_tapped(track, mono, nullptr); }

void Mixer::add_track_tapped(int track, float *mono, float *tap) {
    if (track < 0 || track >= kTracks || frames_ <= 0) return;
    InsertChain &insert = chains_[track];

    // A non-zero send gain on a track the app is bothering to render is what "the send is being
    // fed" means here: it costs nothing to test, and the alternative (scanning the bus for a
    // non-zero sample) costs a pass over every block.
    // Only the mode that is on is fed; the other rings out what it holds.
    if (insert.send_a_gain() > 0.0f) {
        if (long_mode_) long_tail_ = long_tail_blocks_;
        else delay_tail_ = delay_tail_blocks_;
    }
    if (insert.send_b_gain() > 0.0f) {
        if (hall_mode_) hall_tail_ = hall_tail_blocks_;
        else reverb_tail_ = reverb_tail_blocks_;
    }

    ChainOut out;
    out.dry_l = mix_l_;
    out.dry_r = mix_r_;
    out.send_a = send_a_;
    out.send_b = send_b_;
    out.duck = duck_active_ ? duck_curve_ : nullptr;
    out.tap = tap;
    insert.process(mono, frames_, out);
}

void Mixer::finish_block(float *out, int frames) {
    const int n = frames_;

    // --- send A: the delay -------------------------------------------------------------------
    // A freeze keeps the delay running whatever its tail says, and while held cuts its feed.
    const bool freezing = freeze_.active();
    if (freezing) delay_tail_ = delay_tail_ > 1 ? delay_tail_ : 1;
    if (n > 0 && delay_tail_ > 0) {
        delay_tail_--;
        // In LONG mode the classic delay only rings out: its feed is silence, and the bus is
        // left alone for the long delay below.
        const float *feed = long_mode_ ? kSilence : send_a_;
        if (long_mode_) {
            enaga_clear(send_a_right_, n);
        } else {
            if (freeze_.held()) enaga_clear(send_a_, n);
            // The send bus is mono and the delay's two inputs must not alias, so the feed is
            // copied once per block: 64 floats against the delay's own cost is nothing.
            for (int i = 0; i < n; i++) send_a_right_[i] = send_a_[i];
        }
        if (delay_to_reverb_ > 0.0f || freezing) {
            // The wet signal on its own: the reverb wants it, or the freeze records or replaces it.
            enaga_clear(wet_l_, n);
            enaga_clear(wet_r_, n);
            delay_.process(feed, send_a_right_, wet_l_, wet_r_, n, 1.0f);
            if (freezing) freeze_.process(wet_l_, wet_r_, n);
            enaga_mix(mix_l_, wet_l_, n, delay_level_);
            enaga_mix(mix_r_, wet_r_, n, delay_level_);
            if (delay_to_reverb_ > 0.0f) {
                const float into_reverb = delay_to_reverb_ * 0.5f;
                enaga_mix(send_b_, wet_l_, n, into_reverb);
                enaga_mix(send_b_, wet_r_, n, into_reverb);
                // The delay is feeding it: keep it running.
                if (hall_mode_) hall_tail_ = hall_tail_blocks_;
                else reverb_tail_ = reverb_tail_blocks_;
            }
        } else {
            // Nothing needs the wet signal on its own, so the delay writes straight into the mix.
            delay_.process(feed, send_a_right_, mix_l_, mix_r_, n, delay_level_);
        }
    }

    // --- send A's LONG mode -------------------------------------------------------------------
    // Held, it keeps running whatever its tail says: the loop is the sound.
    if (long_delay_.held()) long_tail_ = long_tail_ > long_tail_blocks_ ? long_tail_ : long_tail_blocks_;
    if (n > 0 && long_tail_ > 0) {
        long_tail_--;
        const float *feed = long_mode_ ? send_a_ : kSilence;
        if (delay_to_reverb_ > 0.0f) {
            enaga_clear(wet_l_, n);
            enaga_clear(wet_r_, n);
            long_delay_.process(feed, wet_l_, wet_r_, n, 1.0f);
            enaga_mix(mix_l_, wet_l_, n, delay_level_);
            enaga_mix(mix_r_, wet_r_, n, delay_level_);
            const float into_reverb = delay_to_reverb_ * 0.5f;
            enaga_mix(send_b_, wet_l_, n, into_reverb);
            enaga_mix(send_b_, wet_r_, n, into_reverb);
            if (hall_mode_) hall_tail_ = hall_tail_blocks_;
            else reverb_tail_ = reverb_tail_blocks_;
        } else {
            long_delay_.process(feed, mix_l_, mix_r_, n, delay_level_);
        }
    }

    // --- send B: the reverb, high-passed first ------------------------------------------------
    // The high-pass runs once for whichever mode is fed; a mode ringing out unfed takes silence.
    const bool fed_b = hall_mode_ ? hall_tail_ > 0 : reverb_tail_ > 0;
    if (n > 0 && fed_b) {
        strata_biquad_inplace(&reverb_high_pass_.state_, &reverb_high_pass_coefficients_, send_b_, n);
    }
    if (n > 0 && reverb_tail_ > 0) {
        reverb_tail_--;
        reverb_.process(hall_mode_ ? kSilence : send_b_, kSilence, mix_l_, mix_r_, n,
                        reverb_level_ * 2.0f);
    }
    // --- send B's HALL mode -------------------------------------------------------------------
    if (n > 0 && hall_tail_ > 0) {
        hall_tail_--;
        hall_.process(hall_mode_ ? send_b_ : kSilence, mix_l_, mix_r_, n, reverb_level_);
    }

    // --- master ------------------------------------------------------------------------------
    if (n > 0) master(n);

    // --- interleave, once, at the end --------------------------------------------------------
    // audio()'s buffer is L R L R (CLAUDE.md); everything above worked in planes because that
    // is the layout every Enaga block function wants.
    strata_interleave(out, mix_l_, mix_r_, n, frames);
}

void Mixer::freeze_delay(bool held) {
    if (held) {
        if (delay_frozen()) return;
        freeze_on_long_ = long_mode_;
        if (freeze_on_long_) long_delay_.hold(true);
        else freeze_.hold(true, delay_loop_frames_);
        return;
    }
    if (freeze_on_long_) long_delay_.hold(false);
    else freeze_.hold(false, delay_loop_frames_);
}

void Mixer::master(int n) {
    // --- the one-knob DJ filter: 24 dB per channel, and bypassed in the dead zone -------------
    if (dj_on_) {
        // Both channels in one loop: two independent recurrences side by side, which is what
        // fills this core's units (filterloops.h). Bit-identical to two Svf::cascade calls.
        strata_svf_cascade_stereo_inplace(&dj_l_a_.state_, &dj_l_b_.state_, &dj_r_a_.state_,
                                          &dj_r_b_.state_, &dj_coefficients_, &dj_coefficients_,
                                          dj_output_, mix_l_, mix_r_, n);
    }

    // --- the kill EQ, after the filter and before the detector, so the compressor sees the cut -
    kill_eq(n);

    // --- the block's peak: the compressor's detector and the limiter's skip test --------------
    const float peak = strata_master_peak(mix_l_, mix_r_, n);
    peak_ = peak;

    // --- the glue compressor: one gain a block, smoothed ---------------------------------------
    if (compressor_on_) {
        const float over = peak * compressor_inv_threshold_;
        const float target = over > 1.0f ? ed::exp2(-compressor_slope_ * ed::log2(over)) : 1.0f;
        const float coefficient = target < compressor_gain_ ? compressor_attack_ : compressor_release_;
        compressor_gain_ += coefficient * (target - compressor_gain_);
    } else {
        compressor_gain_ = 1.0f;
    }

    // --- that gain and the output level, as one ramped pass ------------------------------------
    const float from = master_gain_;
    const float to = compressor_gain_ * out_level_;
    if (from != 1.0f || to != 1.0f) {
        if (n != ramp_frames_) {
            ramp_frames_ = n;
            inv_frames_ = ed::reciprocal(float(n));
        }
        const float step = (to - from) * inv_frames_;
        strata_master_ramp(mix_l_, mix_r_, n, from, step);
    }
    master_gain_ = to;

    // --- the soft limiter --------------------------------------------------------------------
    // Unity below the threshold, so a quiet master is bit-exact and the stage is skipped
    // outright when the block cannot reach the threshold even at the louder end of the ramp.
    if (peak * largest(from, to) > limit_threshold_) {
        strata_master_limit(mix_l_, mix_r_, n, limit_threshold_, limit_headroom_,
                            limit_inv_headroom_);
    }
}

void Mixer::kill_eq(int n) {
    // This block's gains: halfway to the targets, and exactly there once close.
    bool flat = true;
    float next[kKillBands];
    for (int b = 0; b < kKillBands; b++) {
        const float moved = eq_gain_[b] + kKillEqGlide * (eq_target_[b] - eq_gain_[b]);
        const float gap = moved - eq_target_[b];
        next[b] = (gap < kKillEqSnap && gap > -kKillEqSnap) ? eq_target_[b] : moved;
        flat = flat && next[b] == 1.0f && eq_gain_[b] == 1.0f;
    }

    // Fade the stage in while any band is off flat, and out again once all three are back and
    // have been for a block. The fade is what makes the bypass safe: the crossovers' sum is an
    // all-pass, not a wire, so switching straight between them would click on a loud low end.
    const float wet_to = flat ? (eq_wet_ - kKillEqFadeStep < 0.0f ? 0.0f : eq_wet_ - kKillEqFadeStep)
                              : (eq_wet_ + kKillEqFadeStep > 1.0f ? 1.0f : eq_wet_ + kKillEqFadeStep);
    if (eq_wet_ == 0.0f && wet_to == 0.0f) {
        // Bypassed, and staying so: nothing runs, nothing is touched.
        for (int b = 0; b < kKillBands; b++) eq_gain_[b] = next[b];
        return;
    }
    if (eq_wet_ == 0.0f) {
        // Starting from bypass: from silence, not from whatever the last use left behind.
        for (int i = 0; i < kKillEqStates; i++) {
            eq_state_l_[i] = 0.0f;
            eq_state_r_[i] = 0.0f;
        }
    }

    if (n != ramp_frames_) {
        ramp_frames_ = n;
        inv_frames_ = ed::reciprocal(float(n));
    }
    float steps[kKillBands];
    for (int b = 0; b < kKillBands; b++) steps[b] = (next[b] - eq_gain_[b]) * inv_frames_;
    strata_kill_eq(mix_l_, mix_r_, n, eq_state_l_, eq_state_r_, &eq_low_split_.a1,
                   &eq_high_split_.a1, eq_gain_, steps, eq_wet_, (wet_to - eq_wet_) * inv_frames_);
    for (int b = 0; b < kKillBands; b++) eq_gain_[b] = next[b];
    eq_wet_ = wet_to;
}

}  // namespace strata
