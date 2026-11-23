// FM: the four-operator machine (dsp/fm.hpp has the voice). Six voices, chords, the ASR page's
// held notes, and VA POLY's share of the 12-voice budget (dsp/polypool.hpp).
//
//     SRC   ALG algorithm 1-8   RAT modulator ratio   IDX index           FBK op 4 feedback
//     AMP   DEC decay           IEN index envelope    VEL velocity>index  LVL level
//     ASR   ATK attack          SUS sustain           REL release         FSU index sustain
//     OSC   DET pair detune     FIN ratio fine        BRT index keytrack  CAR carrier ratio
#pragma once

#include "fm.hpp"
#include "polypool.hpp"

namespace strata {

class FmPoly final : public PolyPool<FmPoly, FmVoice> {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    const char *param_name(int index) const override;

    const FmSettings &settings() const { return settings_; }

    // PolyPool's hooks.
    void begin(FmVoice &v, const Trig &t, int slot);
    void step(FmVoice &v, float *out, int n) {
        if (v.sounding()) v.run(settings_, out, n, env_);
    }
    bool sustains() const { return settings_.sustain > 0.0f; }

private:
    FmSettings settings_{};
    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float env_[FmVoice::kControl] = {};
};

}  // namespace strata
