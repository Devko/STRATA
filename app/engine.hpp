// The audio-side engine: everything that happens inside audio().
//
// It owns the live pattern, the kit, the eight machine slots, the scheduler and the mixer, and it
// is the only thing that touches them. The main context reaches it exclusively through the
// command ring and reads nothing but the published Status. docs/SPEC.md, "Software architecture".
//
// Everything here must hold the audio contract: no allocation, no files, no blocking, no drawing,
// no LEDs. All storage is taken in init(), which runs in App::init().
//
// ## One block, in order
//
//   1. apply at most Commands::kMaxPerBlock commands
//   2. ask Enaga's clock which ticks fall in this block, each with its frame
//   3. for each tick, let the scheduler resolve it into events and trigger the machines
//   4. prepare machines and insert chains at control rate
//   5. render each sounding track into the mixer, then the buses and the master
//   6. publish the playhead, the peak and the voice count for the UI
//
// Steps 2 and 3 are why timing is sample accurate: the tick carries the frame it falls on, the
// scheduler passes that into the event, and the machine starts the voice on that exact frame.
#pragma once

#include "buses.hpp"
#include "commands.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "pattern.hpp"
#include "perc.hpp"
#include "sample.hpp"
#include "scheduler.hpp"
#include "snare.hpp"
#include "track.hpp"
#include "va.hpp"

#include <cstdint>

namespace strata {

class Engine {
public:
    // App::init() only: this is where every buffer in the signal path is taken.
    bool init(float sample_rate);

    // The audio callback's whole body. `out` is interleaved stereo, 2 * frames floats, and is
    // written in full.
    void process(const float *in, float *out, uint32_t frames);

    // --- the main context's side ------------------------------------------
    // Both are safe to touch from the main context: the ring is lock-free and Status is
    // single-writer words.
    Commands &commands() { return commands_; }
    const Status &status() const { return status_; }

    // Read-only views for the UI to draw from. The UI must not write through these: every edit
    // goes through the command ring, so that a save and the transport cannot race.
    const Pattern &pattern() const { return pattern_; }
    const TrackParams &params(int track) const { return params_[track]; }
    const Machine *machine(int track) const { return machines_[track]; }
    bool muted(int track) const { return muted_[track]; }
    float level(int track) const { return mix_[track].level; }
    const InsertChain &chain(int track) const { return mixer_.chain(track); }

    // The master and send settings, for the UI to draw. Edits still go through the ring.
    const SendSettings &sends() const { return sends_; }
    const MasterSettings &master() const { return master_; }

private:
    void apply_commands();
    void apply(const Command &command);
    void resolve_tick(uint32_t tick, uint32_t frame);
    void fire(const StepEvent &event);
    void load_default_kit();
    void load_default_pattern();
    void build_placeholder_sample();

    // --- the machine pool ---
    // Allocated once, here, and pointed at by the track slots. docs/SPEC.md wants any track to
    // host any machine; M1 wires the default techno kit's first four tracks and leaves the rest
    // silent until M2 brings PERC, SAMPLE and VA MONO.
    // One instance per track of the default kit, constructed here so every allocation happens
    // in init(). docs/SPEC.md wants any track to host any machine; doing that at runtime needs
    // the voice-pool question answered (a track switching to VA POLY would want six voices it
    // does not currently own), so for now the assignment is fixed at init and a machine change
    // is a later milestone. Tracks with no machine are silent and cost one compare.
    Kick kick_;
    Snare snare_;
    Hat hat_closed_;
    Hat hat_open_;
    Perc perc_;
    VaMono bass_;
    VaMono stab_;      // standing in for CHORD until VA POLY arrives in M6
    Sampler loop_;

    Machine *machines_[kTracks] = {};

    Pattern pattern_;

    // The kit: a track's parameters as stored, with no locks applied.
    TrackParams params_[kTracks] = {};

    // What the machines and chains are actually prepared from: the kit with the CURRENT step's
    // parameter locks written over it.
    //
    // These have to be separate. A lock lasts for its step, which spans many blocks, so applying
    // it only around trigger() is not enough -- the render loop prepares every block and would
    // overwrite it. That was a real bug: locks on a filter cutoff or a decay were inaudible
    // because only the values trigger() happened to latch survived. docs/SPEC.md makes per-step
    // sound changes one of STRATA's reasons to exist, so this is the path that matters.
    //
    // A step without a lock on a parameter reverts that parameter to the kit value, which is the
    // behaviour every box with p-locks has.
    TrackParams effective_[kTracks] = {};
    ChainMix mix_[kTracks] = {};
    bool muted_[kTracks] = {};

    Scheduler scheduler_;
    Mixer mixer_;

    // The per-tick event buffer lives here rather than on the audio stack: at the true worst
    // case it is 4 KiB, and the platform caps a stack frame at 16 KiB.
    StepEvent events_[kMaxEventsPerTick];

    // Cached so a tempo change can re-derive the scheduler's ratchet spacing without dividing
    // every block. Enaga's clock reports the tempo; core/ cannot ask it.
    float tempo_ = 0.0f;

    SendSettings sends_;
    MasterSettings master_;
    PerformState perform_;

    // Which track's trigs fire the sidechain duck. docs/SPEC.md: track 1 by default.
    int duck_source_ = 0;

    Commands commands_;
    Status status_;

    float rate_ = 48000.0f;
    bool ready_ = false;
};

}  // namespace strata
