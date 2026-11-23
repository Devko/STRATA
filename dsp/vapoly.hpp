// VA POLY: the chord voice, track 7 of the default techno kit.
//
// docs/SPEC.md, "Machines (v1)": *"VA POLY | 6 | Same voice as VA MONO; chord mode, spread,
// unison 2"*. So this machine is six of dsp/va.hpp's `VaVoice`, one control-rate `VaSettings`
// they all read, and a voice allocator -- and that is nearly all of it. The sound, the knob
// curves and the envelopes are VA MONO's, by construction.
//
// --- shared, not duplicated, and why --------------------------------------------------------
//
// This codebase has form for the other choice. core/project.cpp duplicates fifty lines of
// container code rather than share it with core/serialize.cpp, core/perform.cpp has the same
// fifty a third time, and core/mutate.cpp repeats core/scheduler.cpp's hash finalizer. Each one
// records the same reason: so that two formats can be read and changed without reading each
// other, and so that one module does not have to link another to answer a question of its own.
//
// That reason does not reach this case, and one specific line decides it: *"Same voice as VA
// MONO"*. The duplications above are between things the spec defines SEPARATELY -- the pattern
// container and the project container may legitimately drift, and a version bump to one is not
// a bug in the other. Here the spec pins the two voices to be the same thing, docs/BUDGET.md
// budgets this machine at six times the same measured 1,868-cycle figure *because* it is the
// same voice, and dsp/va.cpp's filter and envelope chain is 200 lines of DSP with thirty
// by-ear constants in it. A drift between two copies of that would be a spec violation that
// nothing in the suite could see, discovered by ear months later; a drift between two copies of
// a documented byte layout is caught by its own CRC test on the next change.
//
// The independence those files buy is also worth nothing here, where the duplication would cost
// the most: dsp/ is compiled as one flat folder (tools/build.sh), so there is no build in which
// one of these two machines exists without the other, and nothing is saved by not linking
// dsp/va.cpp. What sharing does cost is a refactor of a machine that already worked and had a
// measured cost -- so the condition on it was that tests/test_va.cpp keeps passing UNCHANGED,
// which it does, and which is exactly what that file is for.
//
// The split is at the seam the two machines already disagree about: `VaVoice` holds what is per
// NOTE, `VaSettings` what is per BLOCK, and each machine keeps its own parameter mapping, its
// own pending-trig bookkeeping and its own idea of how a note starts.
//
// --- voice allocation -----------------------------------------------------------------------
//
// **A chord arrives as several `trigger` calls on the same frame**, not as one call carrying
// several notes: core/chord turns a step's chord byte into notes and the engine triggers each.
// So the first rule of the allocator is that it must not steal from itself while a chord is
// still arriving, and the mechanism for that is that a voice counts as BUSY from the moment it
// is given a trig, not from the moment that trig's frame arrives (`Slot::busy`, which is
// dsp/sample.cpp's rule for the same reason). Six notes on one frame therefore take six
// different voices, and so do three notes on frame 0 and three more on frame 31.
//
// In order, a trig takes:
//
//   1. **the voice already sounding that note**, if it has started. A repeated chord reuses its
//      own voices instead of stealing three fresh ones, and a retrigger behaves exactly as VA
//      MONO's does: the oscillator phases, the filter state and the amplitude envelope's level
//      all carry on, because resetting any of them under a sounding note is a click. A voice
//      that is still WAITING for its frame is not a match -- see the ratchet note below.
//   2. **a free voice**: one with nothing pending and nothing sounding. Voice 0 first, so a
//      single note always lands on the one slot DET leaves in tune.
//   3. **the quietest voice that has started**, which is the one furthest through its decay and
//      so the oldest note of a uniform chord. Ties go to a rotating cursor, so six voices held
//      at the same level are stolen in turn rather than always the same one. This is the path
//      the seventh note of a chord takes: it steals rather than being dropped.
//   4. **the cursor's voice**, when every voice is already committed to a note that has not
//      started yet -- more notes on one frame than there are voices. Something has to go and
//      the cursor decides which, so a pattern that overruns every step does not clobber the
//      same note every time.
//
// Rule 1 excludes pending voices so that a RATCHET keeps all of its hits: a ratcheted step
// fires the same note several times at different frames of one block, and if the second hit
// matched the first hit's waiting voice it would replace it and only one would sound. Taking a
// second voice instead costs a voice and keeps both hits, which is the polyphonic answer; VA
// MONO replaces, because with one voice there is no other answer.
//
// --- what this machine does not do ----------------------------------------------------------
//
// **No glide and no slide.** docs/SPEC.md and dsp/track.hpp both make them MONO-only, and a
// chord has nothing to glide from: `trig.slide` is ignored and every note starts at its pitch.
//
// **No stereo spread and no unison 2.** docs/SPEC.md lists both for this machine, and both need
// something dsp/track.hpp does not actually offer: `Machine::render_add` is mono, and the
// comment at the top of that file promises a `render_add_stereo` for VA POLY and GRAIN that is
// not declared in the interface. Spread is therefore implemented as a PITCH spread across the
// voice slots (DET below) rather than a stereo one, which is the part of it that survives a
// mono bus; stereo spread and unison both want the interface change and a voice-pool decision
// first, and belong with whoever makes it.
//
// **It sustains when SUS says so.** The ASR page (dsp/va.cpp: ATK SUS REL FSU, params 24-27) is
// read into the shared VaSettings. At SUS 0 -- every older kit -- the voice is the one-shot it
// always was and DEC is the note's length, cut short by the step's LEN (the gate below). With SUS
// up a note holds until its gate ends or `release(note)` lets it go: a KEYS pad coming up.
//
// It reads the SRC page (0-3) and the AMP page (8-11); FLTR and FX belong to the insert chain.
#pragma once

#include <cstdint>

#include "track.hpp"
#include "polypool.hpp"   // PolyMachine: what the shared voice budget calls
#include "va.hpp"

namespace strata {

class VaPoly final : public PolyMachine {
public:
    // docs/SPEC.md's figure. Six is also what docs/BUDGET.md budgets, at 1,868 cycles each;
    // docs/REVIEW.md measured that cutting it to four saves 0.75% of the block, which is why
    // the voice count is not a knob and not the lever when the budget gets tight.
    // (PolyMachine::kVoices, shared with FM and STRING.)

    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void release(uint8_t note) override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    int used_params() const override { return 8; }   // all of SRC and all of AMP
    const char *param_name(int index) const override;

    // --- for the UI, and for tests that need to pin a mapping rather than guess at it -------

    static VaMode mode_from(uint8_t value) { return va_mode_from(value); }
    VaMode mode() const { return settings_.mode; }
    // CUT in Hz, before the filter envelope and the accent move it.
    float base_cutoff_hz() const { return settings_.cutoff_hz; }
    float decay_seconds() const { return settings_.decay_s; }
    // DET at its widest, in semitones: what slot 1 is sharp and slot 2 flat by.
    float detune_semitones() const { return detune_; }

    // WAVE POLY: the same machine with the wavetable (VaSettings::make_wave). MOT takes slot
    // 10, so there is no chord detune; the OSC page's DET detunes each voice's two readers.
    // Set before init().
    void set_wave(bool wave) { wave_ = wave; }
    bool wave() const { return wave_; }
    const VaSettings &settings() const { return settings_; }

    // How many voices are making sound, for the UI's voice count (docs/SPEC.md publishes one
    // every block) and for tests. Cheap: six level reads.
    int sounding_voices() const {
        int n = 0;
        for (const Slot &s : slots_) n += s.voice.sounding() ? 1 : 0;
        return n;
    }
    // Voices holding a note, sounding or still waiting for their frame.
    int busy_voices() const {
        int n = 0;
        for (const Slot &s : slots_) n += s.busy() ? 1 : 0;
        return n;
    }
    // The note a slot holds, or -1 if it holds none. Slots are 0..kVoices-1.
    // --- the shared voice budget (app/engine.cpp, make_room_for_poly_voice) ---
    //
    // A voice counts against the budget from the moment it takes a trig until it is silent,
    // EXCEPT while it is choking: a choke is already paid for, and counting it would make the
    // next note of a chord choke a second voice for the room the first one freed.
    //
    // A PENDING trig counts even on a slot still choking. Its start cancels the choke (gate_on),
    // so it is a voice from the moment it takes the slot; left uncounted until its frame, a
    // later note in the same block found room that was not there and the block ended over budget.
    int budget_voices() const override {
        int n = 0;
        for (const Slot &s : slots_) n += s.pending || (s.voice.sounding() && !s.voice.choking());
        return n;
    }
    // Whether a trig on `note` would take a voice that is not already counted: false when it
    // retriggers the voice already sounding that note, or when every slot is busy and it would
    // steal one of this machine's own (allocate(), in vapoly.cpp, decides the same way).
    bool would_add_voice(uint8_t note) const override;
    // The quietest voice the budget may take: sounding, started, not already choking. Its slot,
    // or -1 when there is none; `level` is its amplitude envelope's level.
    int quietest_voice(float &level) const override;
    void choke(int slot) override {
        if (slot >= 0 && slot < kVoices) slots_[slot].voice.choke();
    }

    int voice_note(int slot) const {
        if (slot < 0 || slot >= kVoices) return -1;
        return slots_[slot].busy() ? slots_[slot].note : -1;
    }
    // A slot's pitch in note numbers, TUN and DET included.
    float voice_pitch(int slot) const {
        return slot >= 0 && slot < kVoices ? slots_[slot].voice.pitch() : 0.0f;
    }

private:
    // One voice and the trig it is waiting for.
    struct Slot {
        VaVoice voice;
        Trig next{};
        int wait = 0;           // frames until `next` starts
        bool pending = false;
        int note = -1;          // the note this slot is committed to, -1 when it holds none
        // No gate and no release coming (Trig::held): released as soon as SUS is up (va.hpp).
        bool one_shot = false;

        // Frames of gate left before this voice is released: `Step::length`, arriving as
        // `Trig::gate_frames`. -1 means NO gate, which is what a trig carrying 0 asks for and
        // what every note did before the field existed -- the voice's own envelope decides.
        //
        // It lives HERE and not in VaVoice on purpose. VaVoice is shared with VA MONO, and
        // putting a countdown in it would change how VA MONO sounds -- a machine whose arithmetic
        // is proven bit-identical across the refactor that produced VaVoice, and whose cost
        // docs/BUDGET.md already records. A gate is polyphonic bookkeeping, not part of a voice.
        int gate = -1;

        // A slot is busy from the moment it takes a trig, NOT from the frame that trig starts
        // on. That is what stops a chord arriving on one frame from stealing its own voices.
        bool busy() const { return pending || voice.sounding(); }
    };

    int allocate(uint8_t note);              // which slot this trig takes, and why: see above
    void start(int slot);                    // begin a slot's pending trig, on its exact frame
    void render_slot(int slot, float *out, int frames);

    Slot slots_[kVoices];
    VaSettings settings_{};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;

    float detune_ = 0.0f;                    // DET, semitones
    bool wave_ = false;                      // WAVE POLY
    int steal_cursor_ = 0;                   // rotates, so equal voices are stolen in turn
    bool active_ = false;

    // One slice, shared by all six voices: a voice renders into it and mixes out of it, so six
    // voices cost one buffer rather than six, and nothing goes on the audio stack (CLAUDE.md
    // caps a frame at 16 KiB).
    float work_[VaVoice::kControl] = {};
    float env_[VaVoice::kControl] = {};

    // And one set of filter coefficients, shared the same way: the voices of a chord all arrive
    // at the same ones, because the voice's filter does not track the key (dsp/va.hpp). Cleared
    // by `prepare`, which is what makes a stale entry impossible.
    VaFilterCache filter_cache_;
};

}  // namespace strata
