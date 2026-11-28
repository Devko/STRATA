#include "fmpoly.hpp"

namespace strata {

void FmPoly::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = enaga::dsp::reciprocal(rate_);
    for (int i = 0; i < kVoices; i++) voice_at(i).reset();
    reset_slots();
    // Safe to trigger before the first prepare(): mid values, the OSC page at its rest.
    TrackParams defaults{};
    for (int i = 0; i < kTrackParams; i++) defaults.set(i, 64);
    for (int i = 0; i < kAsrParams; i++) defaults.set(kAsrFirst + i, 0);
    defaults.set(kOscFirst + 0, 0);
    defaults.set(kOscFirst + 2, 0);
    defaults.set(kOscFirst + 3, 0);
    prepare(defaults);
}

void FmPoly::prepare(const TrackParams &params) {
    settings_ = FmSettings::make(params, rate_, inv_rate_);
    settings_.pitch_mod = pitch_mod_;
}

void FmPoly::begin(FmVoice &v, const Trig &t, int slot) {
    // Unison's tuning, 0 outside it (core/width.hpp).
    v.set_note(float(t.note) + slot_shift(slot), float(t.velocity) * (1.0f / 127.0f));
    v.gate_on();
}

const char *FmPoly::param_name(int index) const {
    static const char *const names[kParams] = {
        "ALG", "RAT", "IDX", "FBK",      // SRC: the algorithm, the modulators, the feedback
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "DEC", "IEN", "VEL", "LVL",      // AMP: the envelopes and the output
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

}  // namespace strata
