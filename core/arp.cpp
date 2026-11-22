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

}  // namespace

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
    int length = 0;
    const int octaves = settings.octaves < 1 ? 1
                        : (settings.octaves > kArpMaxOctaves ? kArpMaxOctaves : settings.octaves);
    for (int o = 0; o < octaves; o++) {
        for (int i = 0; i < n; i++) {
            const int note = int(base[i]) + 12 * o;
            if (note <= 127) up[length++] = uint8_t(note);
        }
    }

    switch (settings.mode) {
        case ArpMode::down:
            for (int i = 0; i < length; i++) out[i] = up[length - 1 - i];
            return length;
        case ArpMode::up_down: {
            int k = 0;
            for (int i = 0; i < length; i++) out[k++] = up[i];
            // Back down, leaving out the top (just played) and the bottom (the next cycle's first).
            for (int i = length - 2; i >= 1; i--) out[k++] = up[i];
            return k;
        }
        default:
            for (int i = 0; i < length; i++) out[i] = up[i];
            return length;
    }
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
