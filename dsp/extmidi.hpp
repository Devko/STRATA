// MID: a machine that plays nothing here, for a track whose sound is an outboard synth.
//
// A track's notes reach MIDI OUT whichever machine it hosts, once it has a MIDI OUT channel
// (core/midiout.hpp). MID is the machine for when that is ALL the track should do: it renders no
// audio, so it costs no voices and no CPU -- an external part played from STRATA does not eat
// into the shared VA POLY budget (Engine::kPolyVoiceBudget) -- and its eight SRC and AMP slots
// become MIDI CCs on the track's channel, so knobs, locks, scenes and macros reach the synth.
//
// The CC numbers are the General MIDI 2 sound controllers, because those are the ones a synth
// with no MIDI learn is most likely to answer: 74 brightness (cutoff), 71 resonance, 73 attack,
// 72 release on SRC, then 70, 75, 76 and 77 on AMP. The slot names say the number rather than a
// guess at what the synth does with it. `app/engine.cpp` sends them; this class only names them.
//
// FLTR and FX stay the insert chain's, as on every machine, and with nothing to filter they do
// nothing on a MID track.
#pragma once

#include <cstdint>

#include "track.hpp"

namespace strata {

class MidiMachine final : public Machine {
public:
    // Which CC each slot sends, or 0 for a slot that sends nothing (FLTR and FX).
    static uint8_t cc_for(int index);

    void init(float sample_rate) override { (void)sample_rate; }
    void prepare(const TrackParams &params) override { (void)params; }
    void trigger(const Trig &trig) override { (void)trig; }
    void release_all() override {}
    void kill_all() override {}
    void render_add(float *out, int frames) override {
        (void)out;
        (void)frames;
    }
    bool active() const override { return false; }   // never rendered: the mixer skips it
    const char *param_name(int index) const override;
};

}  // namespace strata
