// FM: a four-operator FM voice for bells, tines, e-pianos, glassy pads and FM basses.
//
// PERC is STRATA's 2-operator FM drum. This is the melodic one: four sine operators in Yamaha's
// eight 4-operator algorithms (the TX81Z's), operator 4 with self-feedback. dsp/fmloops.h draws
// the network all eight share.
//
// The four operators get their ratios from two knobs, not four, so the machine stays dialable on
// a groovebox: the CARRIER pair (operators 1 and 3) sits at CAR, the MODULATOR pair (2 and 4) at
// RAT plus FIN, and DET pulls the second of each pair sharp. In the two-pair algorithm that is
// two detuned copies of one FM pair -- the thick, chorused sound of the classic FM e-piano -- and
// in the stack it is a modulator, carrier, modulator, carrier chain.
//
// Pages (the machine maps them; this voice reads only FmSettings):
//
//     SRC   ALG algorithm 1-8   RAT modulator ratio   IDX index           FBK op 4 feedback
//     AMP   DEC decay           IEN index envelope    VEL velocity>index  LVL level
//     OSC   DET pair detune     FIN ratio fine        BRT index keytrack  CAR carrier ratio
//     ASR   ATK attack          SUS sustain           REL release         FSU index sustain
//
// The index has its own envelope, faster than the amplitude's, because that is what makes FM
// percussive: a bright attack that mellows as the note rings (Chowning 1973). IEN says how much
// of IDX follows it -- 0 is a static timbre, 127 a timbre that decays to nothing.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "fmloops.h"
#include "track.hpp"

namespace strata {

constexpr int kFmAlgorithms = 8;
constexpr int kFmRatios = 8;

// The knob zones, for the screen as well as for `make`.
int fm_algorithm(uint8_t value);          // 0..7
float fm_ratio(uint8_t value);            // RAT: 0.5, 1, 2, 3, 3.5, 4, 7, 14
float fm_fine(uint8_t value);             // FIN: -0.5..+0.5 added to RAT, 0 at 64
float fm_carrier_ratio(uint8_t value);    // CAR: 1, 0.5, 2, 3, 4
float fm_detune_cents(uint8_t value);     // DET: 0..25 cents

// The operators' sine table (fmloops.h), 4 KiB in BSS, filled on the first call. FmPoly::init
// and FmVoice::reset call it, so the fill never lands in the audio callback.
const float *fm_sine_table();

struct FmSettings {
    // Reads SRC 0-3, AMP 8-11, the ASR page and the OSC page. Control rate, once per block.
    static FmSettings make(const TrackParams &params, float rate, float inv_rate);

    enaga::dsp::EnvelopeShape amp_shape{};
    enaga::dsp::EnvelopeShape index_shape{};   // stepped once per control slice
    int algorithm = 0;
    float ratio[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float index = 0.0f;           // IDX, turns of the target's phase at the envelope's peak
    float index_env = 0.0f;       // IEN, 0-1: the part of the index the envelope moves
    float velocity_index = 0.0f;  // VEL, 0-1
    float keytrack = 0.0f;        // BRT, 0-1: the index falls this many octaves' worth per octave
    float feedback = 0.0f;        // FBK, turns
    float decay_s = 0.5f;
    float sustain = 0.0f;
    float out_gain = 0.25f;       // LVL
    float rate = 48000.0f;
    float inv_rate = 1.0f / 48000.0f;
};

class FmVoice {
public:
    static constexpr int kControl = 16;
    static constexpr float kSilent = 1e-4f;
    static constexpr int kChokeFrames = 64;

    void reset();
    void kill();
    void set_note(float note, float velocity) {
        note_ = note;
        velocity_ = velocity;
    }
    void gate_on();
    void gate_off() {
        amp_.gate_off();
        index_env_.gate_off();
    }
    bool released() const { return amp_.stage() == enaga::dsp::Envelope::release; }
    bool sounding() const {
        const enaga::dsp::Envelope::Stage s = amp_.stage();
        return s != enaga::dsp::Envelope::idle &&
               (s == enaga::dsp::Envelope::attack || amp_.level() >= kSilent);
    }
    float amp_level() const { return amp_.level(); }
    float pitch() const { return note_; }
    void choke();
    bool choking() const { return choke_left_ > 0; }

    // One control slice of n frames (1..kControl), ADDED into out. `env` is a kControl-float
    // scratch buffer the caller owns.
    void run(const FmSettings &s, float *out, int n, float *env);

    // For tests: the index the last slice ended on, per modulator (operators 2-4).
    float last_index(int op) const { return op >= 2 && op <= 4 ? index_[op - 2] : 0.0f; }

private:
    enaga::dsp::Envelope amp_, index_env_;
    strata_fm_state state_{};
    float note_ = 60.0f;
    float velocity_ = 1.0f;
    float index_[3] = {};     // where each modulator's index ramp ended
    float fb_ = 0.0f;
    bool fresh_ = true;       // the first slice of a note: no ramp from the last note's index
    int choke_left_ = 0;
    float choke_step_ = 0.0f;
    float choke_gain_ = 1.0f;
};

}  // namespace strata
