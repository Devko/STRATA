#include "modslot.hpp"

namespace strata {

namespace {

// dsp/track.hpp's numbering, stated again because core/ does not include dsp/ (core/project.hpp
// does the same with kKitSoundParams and friends; tests/test_modslot.cpp holds the two together).
constexpr int kSoundParams = 16;
constexpr int kAsrFirst = 24;
// The end of the OSC page. The SCN page past the hole (40-43) is no slot's destination: a slot
// moves a byte every block, and a scan is already a movement of its own.
constexpr int kOscEnd = 32;

}  // namespace

const char *mod_source_name(uint8_t source) {
    switch (source) {
        case kModSrcLfo: return "LFO";
        case kModSrcEnv: return "ENV";
        case kModSrcVel: return "VEL";
        case kModSrcPrs: return "PRS";
        case kModSrcRnd: return "RND";
        default:      return "OFF";
    }
}

uint8_t mod_source_next(uint8_t source) {
    return source >= kModSrcRnd ? kModSrcOff : uint8_t(source + 1);
}

bool mod_dest_allowed(int param) {
    return (param >= 0 && param < kSoundParams) || (param >= kAsrFirst && param < kOscEnd);
}

KitModSlot mod_slot_sanitized(uint8_t source, uint8_t dest, uint8_t depth_byte) {
    KitModSlot slot;
    const int8_t depth = int8_t(depth_byte);
    if (source >= kModSrcCount || source == kModSrcOff || !mod_dest_allowed(dest)) return slot;
    slot.source = source;
    slot.dest = dest;
    slot.depth = depth < -kModDepthMax ? int8_t(-kModDepthMax) : depth;
    return slot;
}

uint8_t mod_apply(uint8_t base, float value, int8_t depth, uint8_t floor) {
    const float offset = value * float(depth);
    const int rounded = offset >= 0.0f ? int(offset + 0.5f) : -int(-offset + 0.5f);
    const int next = int(base) + rounded;
    const int lowest = base < floor ? int(base) : int(floor);
    return uint8_t(next < lowest ? lowest : (next > 127 ? 127 : next));
}

float mod_pitch_semis(float value, int8_t depth) {
    return value * float(depth) * (24.0f / 64.0f);
}

}  // namespace strata
