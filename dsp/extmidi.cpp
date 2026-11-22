// MID. dsp/extmidi.hpp says why it is empty; this is its CC map and its names.
#include "extmidi.hpp"

namespace strata {

namespace {
// Slot -> CC, 0 for none. SRC 0-3, AMP 8-11; FLTR and FX belong to the insert chain.
constexpr uint8_t kCc[kParams] = {
    74, 71, 73, 72,    // SRC
    0,  0,  0,  0,     // FLTR
    70, 75, 76, 77,    // AMP
    0,  0,  0,  0,     // FX
};
}  // namespace

uint8_t MidiMachine::cc_for(int index) {
    return index >= 0 && index < kParams ? kCc[index] : uint8_t(0);
}

const char *MidiMachine::param_name(int index) const {
    static const char *const names[kParams] = {
        "C74", "C71", "C73", "C72",      // SRC
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "C70", "C75", "C76", "C77",      // AMP
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

}  // namespace strata
