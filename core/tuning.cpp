// TUN and PIT as notes. core/tuning.hpp has the reasoning.
#include "tuning.hpp"

#include "project.hpp"
#include "stepedit.hpp"

namespace strata {
namespace {

// Cents above C-4 at byte 0, and cents per count. The drums' Hz ranges are
// lo * 2^(span * knob / 127): at byte 0 that is 1200 * log2(lo / 261.6256) cents, and each count
// adds 1200 * span / 127. Worked out once (exact to well under a cent) because core/ has no libm.
struct Map {
    float at_zero;
    float per_count;
    float counts_per_cent;   // 1 / per_count, so a step solves with a multiply
};

constexpr float kVaStep = 2400.0f / 64.0f;       // 24/64 semitones a count
constexpr float kSampleStep = 2400.0f / 63.0f;   // 24/63 semitones a count

constexpr Map make(float at_zero, float per_count) {
    return {at_zero, per_count, 1.0f / per_count};
}

Map map_of(uint8_t machine) {
    switch (MachineId(machine)) {
        case MachineId::kick:  return make(-4065.0042f, 21.328243f);   // 25 Hz, 2.2572390 octaves
        case MachineId::snare: return make(-1847.4079f, 21.600094f);   // 90 Hz, 2.2860100
        case MachineId::hat:   return make(-647.40794f, 22.530643f);   // 180 Hz, 2.3844930
        case MachineId::perc:  return make(-2935.6767f, 49.572000f);   // 48 Hz, 5.2463700
        case MachineId::va_mono:
        case MachineId::va_poly: return make(-64.0f * kVaStep, kVaStep);
        case MachineId::sample:
        case MachineId::grain:   return make(-64.0f * kSampleStep, kSampleStep);
        default:                 return {0.0f, 0.0f, 0.0f};
    }
}

// Round to the nearest whole number, halves away from zero. No libm.
int round_int(float x) { return x >= 0.0f ? int(x + 0.5f) : -int(-x + 0.5f); }

int clamp_note(int note) { return note < 0 ? 0 : (note > 127 ? 127 : note); }

}  // namespace

int tune_param(uint8_t machine) {
    switch (MachineId(machine)) {
        case MachineId::kick:
        case MachineId::snare:
        case MachineId::hat:
        case MachineId::perc:
        case MachineId::sample:
        case MachineId::va_mono:
        case MachineId::va_poly:
            return 0;                       // SRC A: TUN, or SAMPLE's PIT
        case MachineId::grain:
            return 8;                       // AMP A: PIT
        default:
            return -1;
    }
}

float tune_cents(uint8_t machine, uint8_t value) {
    const Map map = map_of(machine);
    return map.at_zero + float(value > 127 ? 127 : value) * map.per_count;
}

uint8_t tune_note(uint8_t machine, uint8_t value) {
    return uint8_t(clamp_note(60 + round_int(tune_cents(machine, value) * 0.01f)));
}

void tune_text(uint8_t machine, uint8_t value, char *out, int max) {
    if (max <= 0) return;
    const uint8_t note = tune_note(machine, value);
    note_name(note, out, max);
    const float off = tune_cents(machine, value) - float((int(note) - 60) * 100);
    const float limit = float(kTuneMarkCents);
    const char mark = off >= limit ? '+' : (off <= -limit ? '-' : 0);
    if (mark == 0) return;
    int length = 0;
    while (out[length] != 0) length++;
    if (length + 1 >= max) return;          // no room: the note alone rather than half of it
    out[length] = mark;
    out[length + 1] = 0;
}

uint8_t tune_step(uint8_t machine, uint8_t value, int semitones) {
    if (value > 127) value = 127;
    if (semitones == 0 || tune_param(machine) < 0) return value;
    const int target = int(tune_note(machine, value)) + semitones;
    const float wanted = float((target - 60) * 100);

    // Every map is linear in cents, so the nearest byte is solved for rather than searched: it
    // runs on the audio side (app/engine.cpp, kSemitoneFlag), once per click.
    const Map map = map_of(machine);
    int best = round_int((wanted - map.at_zero) * map.counts_per_cent);
    best = best < 0 ? 0 : (best > 127 ? 127 : best);
    // Never swallow a click: a byte between two notes could otherwise round back to itself.
    if (semitones > 0 && best <= int(value)) best = value < 127 ? int(value) + 1 : 127;
    if (semitones < 0 && best >= int(value)) best = value > 0 ? int(value) - 1 : 0;
    return uint8_t(best);
}

}  // namespace strata
