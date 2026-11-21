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
#include "editops.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "pattern.hpp"
#include "perc.hpp"
#include "queue.hpp"       // and core/project.hpp through it: the project owns the live pattern
#include "sample.hpp"
#include "scale.hpp"       // the global key and scale: Cmd::set_root and set_scale
#include "scheduler.hpp"
#include "snare.hpp"
#include "stepedit.hpp"
#include "track.hpp"
#include "undo.hpp"
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
    const Pattern &pattern() const { return project_.live(); }

    // The project, for the UI to draw the bank and pattern grid from and for a save to serialize.
    //
    // A save runs in the MAIN context and reads this while the audio side may be applying an edit
    // to the live pattern, so the caller must drain the command ring first -- see
    // `App::save_project` in app/app.cpp, which defers a save by one frame rather than reading a
    // step halfway through being written. The UI is single-threaded, so once the ring is empty
    // and the UI is the one saving, nothing is writing pattern memory at all.
    const Project &project() const { return project_; }
    const Queue &queue() const { return queue_; }
    const ChainPlayer &chain() const { return chain_; }
    const TrackParams &params(int track) const { return params_[track]; }
    const Machine *machine(int track) const { return machines_[track]; }
    bool muted(int track) const { return muted_[track]; }
    float level(int track) const { return mix_[track].level; }
    const InsertChain &chain(int track) const { return mixer_.chain(track); }

    // The master and send settings, for the UI to draw. Edits still go through the ring.
    const SendSettings &sends() const { return sends_; }
    const MasterSettings &master() const { return master_; }

    // The project, writable, for a LOAD to fill. Main context, and only with the TRANSPORT
    // STOPPED: with the clock stopped `clock::ticks()` yields nothing, so the scheduler never
    // reads the pattern and a load cannot land under it. A load must also post `Cmd::kit_recall`
    // afterwards, since the machines read `params_` and not `project_.kit`.
    //
    // This is the one hole in "the main context never writes pattern memory", and it is a hole
    // on purpose: a load replaces all 128 patterns, which is 1.03 MiB that cannot travel through
    // a 16-byte command.
    Project &project_for_load();

    // App::init() only, before the first audio block. The region and the SamplePool that carves
    // it up belong to the MAIN context (see `sample_region_` below); the audio side is handed the
    // base so `Cmd::set_sample` can turn the loader's byte offset into a pointer, and the size so
    // that an offset from a damaged command cannot be followed out of the region.
    void set_sample_region(const int16_t *base, uint32_t bytes);

private:
    void apply_commands();
    void apply(const Command &command);
    void reconfigure();

    // Make `bank`/`slot` the live pattern. Clears the undo history (it belonged to the pattern
    // being left) and re-derives the scheduler's timing and the pass boundary.
    void switch_pattern(int bank, int slot);

    // The pass boundary passed: let a chain move on if one is playing.
    void cross_boundary(uint32_t tick);
    void resolve_tick(uint32_t tick, uint32_t frame);
    void fire(const StepEvent &event);

    // The kit the machines read (`params_`) and the kit a file holds (`project_.kit`) are
    // different structures in different layers, so they are copied across explicitly rather than
    // shared: `Cmd::kit_store` before a save, `Cmd::kit_recall` after a load.
    void kit_from_params();
    void params_from_kit();

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

    // Which tracks host a Sampler, for `Cmd::set_sample` and `Cmd::clear_sample`. The build has
    // no RTTI (CLAUDE.md), so a Machine* cannot be asked whether it is one; the assignment is
    // fixed at init anyway, so it is simply recorded alongside machines_ there. A track that
    // hosts no Sampler refuses a sample command rather than reinterpreting a pointer.
    Sampler *samplers_[kTracks] = {};

    // The whole project, about 1.03 MiB of it, and the sequencer plays a pattern INSIDE it
    // rather than a copy: `live()` is `project_.live()`. So an edit lands in the pattern that
    // will be saved, a pattern change is an index change with no 8 KiB copy in the audio path,
    // and a save reads exactly the memory the audio side plays from (core/project.hpp).
    //
    // The consequence to remember is that the undo history belongs to ONE pattern, so a pattern
    // change clears it. core/undo.hpp says the same from its side.
    Project project_;

    Pattern &live() { return project_.live(); }
    const Pattern &live() const { return project_.live(); }

    // The queued pattern change, and where a chain has got to. Both are audio-side state: the UI
    // arms a change by posting a command, so `queue_arm` and `queue_due` run in the same context
    // and there is no cross-context struct to tear. core/queue.hpp was written expecting that.
    Queue queue_;
    ChainPlayer chain_;

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

    // M3. Both live on the audio side because the pattern does: a snapshot taken from the main
    // context while the sequencer walks the pattern would tear, and a clipboard full of locks
    // has to be read and written under the same single-owner rule as the pool it comes from.
    // Every undo, copy and paste therefore arrives as a command and happens at a block boundary,
    // which is also the only moment the pattern is in a consistent state.
    //
    // The undo stack is about 140 KiB of whole-pattern snapshots (core/undo.hpp explains why
    // snapshots rather than deltas). It makes Engine large, which is fine -- the app object is a
    // static, not a stack frame -- but it is the reason nothing here may be passed by value.
    UndoStack undo_;
    RangeClip clip_;

    // The base of the sample region, so `Cmd::set_sample` can turn the loader's byte offset into
    // a pointer. The region and the SamplePool that carves it up both belong to the MAIN context
    // (app/app.cpp): the loader is what reserves, fills and releases ranges, and keeping the
    // pool there means there is no pool state shared across contexts at all -- the handover is a
    // command carrying an offset, a frame count and a channel count.
    const int16_t *sample_region_ = nullptr;
    uint32_t sample_region_bytes_ = 0;

    // The tick at which the live pattern's current pass ends. Kept rather than derived so that
    // detecting the boundary is a comparison instead of a division per tick.
    uint32_t next_boundary_ = 0;

    // False until `next_boundary_` has been derived from a real tick. A pattern change clears it,
    // because the new pattern's master length makes the old boundary meaningless -- and because
    // deriving it needs a tick, which only `resolve_tick` has. Without this flag the first pass
    // after a change would end at the previous pattern's boundary.
    bool boundary_known_ = false;

    // The last tick the sequencer resolved. A queued change is armed from it: the arming happens
    // on the audio side precisely so that it can use the tick the sequencer is actually on, which
    // the main context cannot know (core/queue.hpp).
    uint32_t tick_ = 0;

    // The per-tick event buffer lives here rather than on the audio stack: at the true worst
    // case it is 4 KiB, and the platform caps a stack frame at 16 KiB.
    StepEvent events_[kMaxEventsPerTick];

    // Cached so a tempo change can re-derive the scheduler's ratchet spacing without dividing
    // every block. Enaga's clock reports the tempo; core/ cannot ask it.
    float tempo_ = 0.0f;
    float samples_per_tick_ = 0.0f;   // derived from it; see reconfigure()

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
