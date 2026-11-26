// The arpeggiator's note logic. core/arp.hpp has the design.
#include "arp.hpp"

namespace strata {
namespace {

// Slow to fast, at 96 PPQN.
constexpr uint8_t kRateTicks[kArpRates] = {96, 48, 32, 24, 16, 12, 8};
constexpr const char *kRateNames[kArpRates] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32",
                                               "1/32T"};
constexpr const char *kModeNames[int(ArpMode::count)] = {"OFF", "UP", "DN", "U/D", "ORD", "RND"};

int zone(uint8_t byte, int zones) {
    const int b = byte > 127 ? 127 : int(byte);
    return (b * zones) >> 7;
}

// Insertion sort, ascending: at most eight notes.
void sort_notes(uint8_t *notes, int count) {
    for (int i = 1; i < count; i++) {
        const uint8_t key = notes[i];
        int j = i - 1;
        while (j >= 0 && notes[j] > key) {
            notes[j + 1] = notes[j];
            j--;
        }
        notes[j + 1] = key;
    }
}

uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// --- the kit settings' tables (core/arp.hpp, "VAR, RHYTHM, ACCENT, RATCHET") -----------------
//
// A slot pattern is `length` slots long, and bit i of `mask` is slot i, the first hit in bit 0.
// Index order is what KIT.BIN stores: PERMANENT, new entries on the end.

struct SlotPattern {
    const char *name;     // five characters at most, in the 5x7 font
    uint8_t length;       // 1-16
    uint16_t mask;
};

constexpr const char *kVarNames[kArpVars] = {"OFF", "CONV", "DIVG", "THUMB", "PINKY"};

constexpr SlotPattern kRhythms[kArpRhythms] = {
    {"ALL", 1, 0x1},
    {"X-", 2, 0x1},          // every other hit: the run at half density
    {"XX-", 3, 0x3},         // two and a rest: a gallop that turns against the bar
    {"XXX-", 4, 0x7},
    {"X-XX", 4, 0xD},
    {"332", 8, 0x49},        // X..X..X. the tresillo
    {"E5/8", 8, 0x6D},       // X.XX.XX. the cinquillo, five in eight spread evenly
    {"E5/16", 16, 0x1249},   // X..X..X..X..X... five even hits and a gap
    {"E7/16", 16, 0x54A9},   // X..X.X.X..X.X.X.
    {"E9/16", 16, 0x56AD},   // X.XX.X.X.XX.X.X.
    {"-X", 2, 0x2},          // the offbeats only
    {"X--", 3, 0x1},         // one in three: sparse, and in threes against the four
};

// CYC (the last) is not a slot pattern: it accents the first note of each cycle.
constexpr int kAccentCycle = kArpAccents - 1;
constexpr SlotPattern kAccents[kArpAccents] = {
    {"OFF", 1, 0x0},
    {"X-", 2, 0x1},
    {"X--", 3, 0x1},         // in threes over a four-to-the-floor
    {"X---", 4, 0x1},
    {"X----", 5, 0x1},
    {"332", 8, 0x49},
    {"--X-", 4, 0x4},        // the offbeat of each beat at 1/16
    {"CYC", 1, 0x0},
};

struct RatchetPattern {
    const char *name;
    uint8_t length;
    uint16_t mask;        // the slots that ratchet
    uint8_t count;        // into how many hits
    bool chance;          // or: a quarter of the slots, drawn from the seed
};

constexpr RatchetPattern kRatchets[kArpRatchets] = {
    {"OFF", 1, 0x0, 1, false},
    {"2:4", 4, 0x8, 2, false},       // the last hit of every four, twice
    {"2:8", 8, 0x80, 2, false},
    {"3:8", 8, 0x80, 3, false},
    {"4:16", 16, 0x8000, 4, false},  // a roll into the next bar at 1/16
    {"2ALL", 1, 0x1, 2, false},      // every hit twice: the grid doubled, the notes not
    {"RND2", 1, 0x0, 2, true},
    {"RND3", 1, 0x0, 3, true},       // two or three
};

bool slot_set(uint8_t length, uint16_t mask, uint32_t slot) {
    // One modulo per hit, at control rate, as arp_note's.
    return ((mask >> (slot % length)) & 1u) != 0;
}

}  // namespace

KitArp kit_arp_sanitized(KitArp arp) {
    if (arp.var >= kArpVars) arp.var = 0;
    if (arp.rhythm >= kArpRhythms) arp.rhythm = 0;
    if (arp.accent >= kArpAccents) arp.accent = 0;
    if (arp.ratchet >= kArpRatchets) arp.ratchet = 0;
    return arp;
}

int arp_ext_count(int field) {
    switch (field) {
        case 0:  return kArpVars;
        case 1:  return kArpRhythms;
        case 2:  return kArpAccents;
        case 3:  return kArpRatchets;
        default: return 1;
    }
}

uint8_t arp_ext_get(const KitArp &arp, int field) {
    switch (field) {
        case 0:  return arp.var;
        case 1:  return arp.rhythm;
        case 2:  return arp.accent;
        case 3:  return arp.ratchet;
        default: return 0;
    }
}

void arp_ext_set(KitArp &arp, int field, uint8_t value) {
    const uint8_t v = value < arp_ext_count(field) ? value : 0;
    switch (field) {
        case 0:  arp.var = v; break;
        case 1:  arp.rhythm = v; break;
        case 2:  arp.accent = v; break;
        case 3:  arp.ratchet = v; break;
        default: break;
    }
}

const char *arp_ext_label(int field) {
    switch (field) {
        case 0:  return "VAR";
        case 1:  return "RHY";
        case 2:  return "ACC";
        case 3:  return "RTC";
        default: return "---";
    }
}

const char *arp_ext_name(int field, uint8_t value) {
    const uint8_t v = value < arp_ext_count(field) ? value : 0;
    switch (field) {
        case 0:  return kVarNames[v];
        case 1:  return kRhythms[v].name;
        case 2:  return kAccents[v].name;
        case 3:  return kRatchets[v].name;
        default: return "---";
    }
}

bool arp_rhythm_plays(uint8_t rhythm, uint32_t slot) {
    if (rhythm >= kArpRhythms) return true;
    return slot_set(kRhythms[rhythm].length, kRhythms[rhythm].mask, slot);
}

bool arp_accent_on(uint8_t accent, uint32_t slot, uint32_t note_hit, int length) {
    if (accent == 0 || accent >= kArpAccents) return false;
    if (accent == kAccentCycle) return length > 0 && note_hit % uint32_t(length) == 0;
    return slot_set(kAccents[accent].length, kAccents[accent].mask, slot);
}

int arp_ratchet_count(uint8_t ratchet, uint32_t slot, uint32_t seed) {
    if (ratchet == 0 || ratchet >= kArpRatchets) return 1;
    const RatchetPattern &r = kRatchets[ratchet];
    if (r.chance) {
        const uint32_t h = mix(seed * 0x9e3779b9u ^ (slot * 0x85ebca6bu) ^ 0x5bd1e995u);
        if ((h & 3u) != 0) return 1;
        return r.count > 2 && (h & 4u) != 0 ? 3 : 2;
    }
    return slot_set(r.length, r.mask, slot) ? r.count : 1;
}

int arp_mode_index(uint8_t byte) { return zone(byte, int(ArpMode::count)); }
int arp_rate_index(uint8_t byte) { return zone(byte, kArpRates); }
int arp_octaves(uint8_t byte) { return 1 + zone(byte, kArpMaxOctaves); }
int arp_gate_percent(uint8_t byte) {
    const int b = byte > 127 ? 127 : int(byte);
    return 50 + ((b * 50) >> 7);
}
uint8_t arp_rate_ticks(int index) {
    return kRateTicks[index < 0 ? 0 : (index >= kArpRates ? kArpRates - 1 : index)];
}

const char *arp_mode_name(uint8_t byte) { return kModeNames[arp_mode_index(byte)]; }
const char *arp_rate_name(uint8_t byte) { return kRateNames[arp_rate_index(byte)]; }

const char *arp_param_name(int offset) {
    switch (offset) {
        case 0:  return "MOD";
        case 1:  return "RAT";
        case 2:  return "OCT";
        case 3:  return "GAT";
        default: return "---";
    }
}

ArpSettings ArpSettings::make(const uint8_t bytes[kArpBytes]) {
    ArpSettings s;
    s.mode = ArpMode(arp_mode_index(bytes[0]));
    s.rate_ticks = arp_rate_ticks(arp_rate_index(bytes[1]));
    s.octaves = uint8_t(arp_octaves(bytes[2]));
    s.gate_percent = uint8_t(arp_gate_percent(bytes[3]));
    return s;
}

int arp_cycle(const uint8_t *notes, int count, const ArpSettings &settings, uint8_t *out) {
    if (!settings.on() || notes == nullptr || count <= 0) return 0;
    const int n = count > kArpMaxInput ? kArpMaxInput : count;
    uint8_t base[kArpMaxInput];
    for (int i = 0; i < n; i++) base[i] = notes[i] > 127 ? 127 : notes[i];
    if (settings.mode != ArpMode::order) sort_notes(base, n);

    // Up through the octaves.
    uint8_t up[kArpMaxInput * kArpMaxOctaves];
    uint8_t walk[kArpMaxWalk];
    int length = 0;
    const int octaves = settings.octaves < 1 ? 1
                        : (settings.octaves > kArpMaxOctaves ? kArpMaxOctaves : settings.octaves);
    for (int o = 0; o < octaves; o++) {
        for (int i = 0; i < n; i++) {
            const int note = int(base[i]) + 12 * o;
            if (note <= 127) up[length++] = uint8_t(note);
        }
    }

    // MOD's walk.
    int k = 0;
    switch (settings.mode) {
        case ArpMode::down:
            for (int i = 0; i < length; i++) walk[k++] = up[length - 1 - i];
            break;
        case ArpMode::up_down:
            for (int i = 0; i < length; i++) walk[k++] = up[i];
            // Back down, leaving out the top (just played) and the bottom (the next cycle's first).
            for (int i = length - 2; i >= 1; i--) walk[k++] = up[i];
            break;
        default:
            for (int i = 0; i < length; i++) walk[k++] = up[i];
            break;
    }

    // And VAR's reshaping of it.
    int m = 0;
    switch (settings.var) {
        case ArpVar::converge:
        case ArpVar::diverge: {
            // The two ends of the walk alternately, closing in: for UP, low high, next low next
            // high. DIVERGE is the same order backwards, from the middle out.
            int lo = 0, hi = k - 1;
            while (lo <= hi) {
                out[m++] = walk[lo++];
                if (lo <= hi) out[m++] = walk[hi--];
            }
            if (settings.var == ArpVar::diverge) {
                for (int i = 0, j = m - 1; i < j; i++, j--) {
                    const uint8_t t = out[i];
                    out[i] = out[j];
                    out[j] = t;
                }
            }
            return m;
        }
        case ArpVar::thumb:
        case ArpVar::pinky: {
            // A pedal: the lowest (THUMB) or highest (PINKY) note of the walk between every two
            // of the others -- before each on THUMB, after each on PINKY -- so UP over C E G is
            // C E C G and C G E G. A walk of the pedal alone stays as it is.
            const bool thumb = settings.var == ArpVar::thumb;
            uint8_t pedal = walk[0];
            for (int i = 1; i < k; i++) {
                pedal = thumb ? (walk[i] < pedal ? walk[i] : pedal)
                              : (walk[i] > pedal ? walk[i] : pedal);
            }
            for (int i = 0; i < k; i++) {
                if (walk[i] == pedal) continue;
                if (thumb) out[m++] = pedal;
                out[m++] = walk[i];
                if (!thumb) out[m++] = pedal;
            }
            if (m > 0) return m;
            break;
        }
        default:
            break;
    }
    for (int i = 0; i < k; i++) out[i] = walk[i];
    return k;
}

uint8_t arp_note(const uint8_t *cycle, int length, ArpMode mode, uint32_t hit, uint32_t seed) {
    if (cycle == nullptr || length <= 0) return 60;
    const uint32_t len = uint32_t(length);
    if (mode == ArpMode::random) {
        // A multiply-high instead of a modulo: no divide on this chip.
        const uint32_t r = mix(seed * 0x9e3779b9u ^ hit);
        return cycle[uint32_t((uint64_t(r) * len) >> 32)];
    }
    // One modulo per HIT, at control rate -- a few a second, never in a sample loop.
    return cycle[hit % len];
}

}  // namespace strata
