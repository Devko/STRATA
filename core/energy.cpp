// ENERGY. core/energy.hpp has the design; the numbers below are the starting points, to be
// tuned by ear on the unit.
#include "energy.hpp"

#include "clamp.hpp"
#include "project.hpp"   // MachineId

namespace strata {

namespace {

// The "lowbias32" finalizer, as the scheduler and GEN use it.
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// A fixed draw in 0 .. n-1 for (seed, track, step, what): no pass, so a knob position plays the
// same groove every bar. Multiply-shift, no modulo.
uint32_t draw(uint32_t seed, int track, int step, uint32_t what, uint32_t n) {
    uint32_t h = mix32(seed ^ 0x6A09E667u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (uint32_t(step) * 0xC2B2AE3Du + 0x27D4EB2Fu));
    h = mix32(h ^ (what * 0x9E3779B1u + 0x85EBCA6Bu));
    return uint32_t((uint64_t(h) * uint64_t(n)) >> 32);
}

constexpr uint32_t kDrawStrength = 1;
constexpr uint32_t kDrawGhost = 2;
constexpr uint32_t kDrawVelocity = 3;

// Where a step sits in a 16-step bar, as GEN counts it: 0 the one, 1 another beat, 2 an off-beat
// 8th, 3 a 16th.
int position_class(int step) {
    const int in_bar = step & 15;
    if (in_bar == 0) return 0;
    if ((in_bar & 3) == 0) return 1;
    if ((in_bar & 1) == 0) return 2;
    return 3;
}

// Thinning, per tier: where on the way down (in counts, 0..64) the tier starts, the deepest cut
// it reaches (0..100), and ceiling * 256 / (64 - start), so the cut is a multiply and a shift.
struct ThinShape {
    uint8_t start;
    uint8_t ceiling;
    uint16_t slope_q8;
};
constexpr ThinShape kThin[int(EnergyTier::count)] = {
    {0, 100, 400},     // hats: from the first click
    {8, 100, 457},     // perc
    {16, 100, 533},    // snare, samples
    {24, 100, 640},    // lines
    {32, 60, 480},     // pads: half way down, and only to their strongest hits
    {40, 100, 1067},   // kick: last
    {16, 100, 533},    // MID, with the snare
};

// Filling, per tier: where on the way up the tier starts, and its appetite. The chance is
// weight * (energy - start) * gain >> 14.
struct FillShape {
    uint8_t start;
    uint16_t gain;
};
constexpr FillShape kFill[int(EnergyTier::count)] = {
    {0, 320},    // hats: the off-beat 8ths fill by the middle of the range
    {0, 200},    // perc
    {0, 100},    // snare, samples: ghosts
    {0, 90},     // lines: the off-beat bass note
    {0, 0},      // pads: never
    {32, 80},    // kick: only in the top half, and rarely
    {0, 0},      // MID: never
};

// A fill favours the off-beats, where a part has room and where a peak adds its drive: the 8ths
// most, then the 16ths, then the beats, and the one least (it is rarely empty, and a ghost there
// blurs the downbeat). Not GEN's table, which says where a PART's hits go; this says where extra
// hits go.
constexpr int kGhostWeight[4] = {40, 60, 100, 70};

}  // namespace

EnergyTier energy_tier(uint8_t machine) {
    switch (MachineId(machine)) {
        case MachineId::hat:       return EnergyTier::hats;
        case MachineId::perc:      return EnergyTier::perc;
        case MachineId::snare:
        case MachineId::sample:    return EnergyTier::mid;
        case MachineId::va_mono:
        case MachineId::wave_mono: return EnergyTier::line;
        case MachineId::va_poly:
        case MachineId::wave_poly:
        case MachineId::fm:
        case MachineId::grain:     return EnergyTier::pad;
        case MachineId::kick:      return EnergyTier::kick;
        case MachineId::midi:      return EnergyTier::midi;
        default:                   return EnergyTier::mid;
    }
}

int8_t energy_turn(int8_t current, int clicks) {
    const int from = clamp_int(int(current), -kEnergyRange, kEnergyRange);
    int to = from + clamp_int(clicks, -kEnergyRange, kEnergyRange) * kEnergyStep;
    // The detent: a turn that crosses rest stops on it.
    if ((from > 0 && to < 0) || (from < 0 && to > 0)) to = 0;
    return int8_t(clamp_int(to, -kEnergyRange, kEnergyRange));
}

int energy_cut(int8_t energy, EnergyTier tier) {
    if (energy >= 0 || uint8_t(tier) >= uint8_t(EnergyTier::count)) return 0;
    const ThinShape &shape = kThin[int(tier)];
    const int down = clamp_int(-int(energy), 0, kEnergyRange) - int(shape.start);
    if (down <= 0) return 0;
    const int cut = (down * int(shape.slope_q8) + 128) >> 8;
    return cut > int(shape.ceiling) ? int(shape.ceiling) : cut;
}

int energy_strength(uint32_t seed, int track, int step, bool accent) {
    const int band = (3 - position_class(step)) * 25;
    const int strength = band + int(draw(seed, track, step, kDrawStrength, 25)) + (accent ? 15 : 0);
    return strength > 99 ? 99 : strength;
}

bool energy_keeps(int8_t energy, EnergyTier tier, uint32_t seed, int track, int step,
                  bool accent) {
    const int cut = energy_cut(energy, tier);
    if (cut <= 0) return true;
    return energy_strength(seed, track, step, accent) >= cut;
}

uint8_t energy_probability(uint8_t probability, int8_t energy) {
    if (energy <= 0 || probability == 0 || probability >= 100) return probability;
    const int up = clamp_int(int(energy), 0, kEnergyRange);
    // p + (100 - p) * up / 64, rounded: a shift, since the range is 64.
    const int lifted = int(probability) + (((100 - int(probability)) * up + 32) >> 6);
    return uint8_t(lifted > 100 ? 100 : lifted);
}

int energy_ghost_chance(int8_t energy, EnergyTier tier, int step) {
    if (energy <= 0 || uint8_t(tier) >= uint8_t(EnergyTier::count)) return 0;
    const FillShape &shape = kFill[int(tier)];
    const int up = clamp_int(int(energy), 0, kEnergyRange) - int(shape.start);
    if (up <= 0 || shape.gain == 0) return 0;
    const int chance = (kGhostWeight[position_class(step)] * up * int(shape.gain)) >> 14;
    return chance > 100 ? 100 : chance;
}

bool energy_ghost(int8_t energy, EnergyTier tier, uint32_t seed, int track, int step,
                  uint8_t &velocity) {
    const int chance = energy_ghost_chance(energy, tier, step);
    if (chance <= 0) return false;
    if (int(draw(seed, track, step, kDrawGhost, 100)) >= chance) return false;
    const int up = clamp_int(int(energy), 0, kEnergyRange);
    const int spread = int(draw(seed, track, step, kDrawVelocity, 31));
    const int v = tier == EnergyTier::line ? 70 + spread + (up >> 3)    // 70 .. 108
                                           : 40 + spread + (up >> 2);   // 40 .. 86
    velocity = uint8_t(clamp_int(v, 1, 127));
    return true;
}

int energy_source_step(const TrackPattern &track, int length, int step) {
    const int n = clamp_int(length, 1, kStepsPerTrack);
    int index = step;
    while (index >= n) index -= n;    // no modulo: at most a few rounds
    if (index < 0) index = 0;
    for (int back = 1; back <= n; back++) {
        int s = index - back;
        if (s < 0) s += n;
        // A trig that does not play every pass lends nothing: a FILL-only crash, a 1:4 hit or a
        // 0% step copied onto empty steps would play every bar once ENERGY is up. The walk goes
        // on back to a plain trig. A step at 1-99% still lends, as ENERGY lifts it anyway.
        const Step &candidate = track.steps[s];
        if ((candidate.flags & kStepTrig) == 0) continue;
        if (condition_kind(candidate.condition) != Condition::NONE) continue;
        if (candidate.probability == 0) continue;
        return s;
    }
    return -1;
}

}  // namespace strata
