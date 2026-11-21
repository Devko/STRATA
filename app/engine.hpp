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
#include "capture.hpp"    // resampling's take buffer
#include "commands.hpp"
#include "editops.hpp"
#include "hat.hpp"
#include "kick.hpp"
#include "mutate.hpp"      // Cmd::mutate
#include "pattern.hpp"
#include "perc.hpp"
#include "perform.hpp"   // scenes, the morph, macros, mute states, auto-fill
#include "queue.hpp"       // and core/project.hpp through it: the project owns the live pattern
#include "repeat.hpp"    // beat repeat, on the master output
#include "sample.hpp"
#include "scale.hpp"       // the global key and scale: Cmd::set_root and set_scale
#include "scheduler.hpp"
#include "snare.hpp"
#include "stepedit.hpp"
#include "track.hpp"
#include "chord.hpp"     // a POLY step's chord byte, expanded in fire()
#include "undo.hpp"
#include "va.hpp"
#include "vapoly.hpp"    // the CHORD track's six voices

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

    // The finished take, for the card to write. Valid from the main context only while
    // `status().resample_state` reads 3, which is the same discipline a project save uses.
    const Capture &capture() const { return capture_; }

    // The same buffer, writable, so the card can put a WAV header in the bytes reserved for one.
    // Main context, and only while `status().resample_state` reads 3 -- see dsp/capture.hpp on
    // why the reserved bytes exist and why the audio side never touches them.
    Capture &capture_for_write() { return capture_; }
    bool muted(int track) const { return muted_[track]; }
    float level(int track) const { return mix_[track].level; }
    const InsertChain &chain(int track) const { return mixer_.chain(track); }

    // The master and send settings, for the UI to draw. Edits still go through the ring.
    const SendSettings &sends() const { return sends_; }
    const MasterSettings &master() const { return master_; }

    // --- the performance layer, for the UI to draw -------------------------
    const SceneSet &scenes() const { return scenes_; }
    const MacroSet &macros() const { return macros_; }
    int scene_slot(int which) const { return which == 0 ? scene_a_ : scene_b_; }
    uint16_t morph() const { return morph_; }
    uint8_t macro_position(int macro) const {
        return macro >= 0 && macro < kMacros ? macro_position_[macro] : 0;
    }
    AutoFill auto_fill() const { return auto_fill_; }
    uint32_t bar() const { return bar_; }
    bool fill() const { return perform_.fill; }
    int throwing() const { return throw_bus_; }
    const BeatRepeat &repeat() const { return repeat_; }
    RepeatLength repeat_length() const { return repeat_length_; }

    // The performance layer writes over the kit without changing it, so the UI has to be able to
    // draw what is actually being heard as well as what is stored. `params` is the kit; this is
    // the kit with the morph, the macros and any throw over the top.
    uint8_t performed(int track, int param) const {
        return track >= 0 && track < kTracks && param >= 0 && param < kParams
                   ? performed_[track][param]
                   : 0;
    }

    // The scenes and macros, writable, for a LOAD to fill -- the same hole and the same rules as
    // `project_for_load`: main context, and only before the audio side reads them again. They
    // are 2.4 KiB, which is more than a 16-byte command carries.
    SceneSet &scenes_for_load() { return scenes_; }
    MacroSet &macros_for_load() { return macros_; }

    // A load has replaced them, so what the performance layer produces is stale.
    void performance_changed() { perform_dirty_ = true; }

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

    // A bar started: release anything armed for one.
    void on_bar();
    void resolve_tick(uint32_t tick, uint32_t frame);

    // The eight playheads into Status, every block. See the implementation on why not only on a
    // tick.
    void publish_playheads();
    void fire(const StepEvent &event);

    // `Step::length` as a frame count for `Trig::gate_frames`; 0 for "no gate".
    uint32_t gate_frames_for(int track, uint8_t length_index) const;

    // The kit with the performance layer over it: the morph, then the macros, then any throw.
    // Control rate, and only when something has moved -- which is why `perform_dirty_` exists
    // rather than recomputing 128 parameters every block for a morph nobody is touching.
    void apply_performance();

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
    VaPoly stab_;      // CHORD: six voices, so a step's chord byte can be heard as a chord
    Sampler loop_;

    Machine *machines_[kTracks] = {};

    // Which tracks host a Sampler, for `Cmd::set_sample` and `Cmd::clear_sample`. The build has
    // no RTTI (CLAUDE.md), so a Machine* cannot be asked whether it is one; the assignment is
    // fixed at init anyway, so it is simply recorded alongside machines_ there. A track that
    // hosts no Sampler refuses a sample command rather than reinterpreting a pointer.
    Sampler *samplers_[kTracks] = {};

    // And which tracks host a VaPoly, for the same reason and recorded the same way: a chord is
    // several notes on one frame, so `fire` has to know that this track can take them. A track
    // with no VaPoly plays the step's note and ignores its chord byte, which is what every
    // machine but this one does with it.
    VaPoly *polys_[kTracks] = {};

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

    // `params_` with the performance layer written over it, which is what `fire` builds
    // `effective_` from. Separate from `params_` because a scene, a macro and a throw must not
    // change the kit: none of them is saved, none is undone, and letting go of a throw has to
    // get the stored value back.
    uint8_t performed_[kTracks][kParams] = {};
    bool perform_dirty_ = true;

    // Which of a track's parameters the CURRENT step has locked, one bit each. The performance
    // layer skips them, so a lock always wins over a morph -- which is what docs/REVIEW.md's
    // "base kit values only" means from the other side. Set by `fire` and therefore lasting
    // exactly as long as the step does.
    uint16_t locked_[kTracks] = {};

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

    // --- the performance layer (M5) ---
    //
    // All audio-side state. The scenes and macros are the only part that is saved, and they are
    // saved from the main context through `scenes_for_load` under the same rule as the project:
    // the transport stopped, or before the first block.
    SceneSet scenes_;
    MacroSet macros_;
    uint8_t scene_a_ = 0;
    uint8_t scene_b_ = 1;
    uint16_t morph_ = 0;
    uint8_t macro_position_[kMacros] = {};

    MuteLatch mute_latch_;

    // The FILL pad, as against the fill the auto-fill bar produces. Kept apart so that holding
    // the pad during an auto-fill bar is not a contradiction and releasing it does not cancel
    // the bar's own fill.
    bool fill_held_ = false;
    AutoFill auto_fill_ = AutoFill::off;

    // Bars since the transport started, for auto-fill. Counted rather than divided: a bar is 384
    // ticks at 96 PPQN and this chip has no integer divide (core/scheduler.cpp says the same
    // about samples-per-tick).
    uint32_t bar_ = 0;
    uint32_t next_bar_tick_ = 0;
    bool bar_known_ = false;

    // A throw: 0 none, 1 delay (send A), 2 reverb (send B), on `throw_track_`. Not a parameter
    // edit -- `params_` is untouched, so release puts the stored value back by itself.
    int throw_bus_ = 0;
    int throw_track_ = 0;

    BeatRepeat repeat_;
    RepeatLength repeat_length_ = RepeatLength::sixteenth;
    bool repeat_held_ = false;
    bool repeat_pitch_ = false;

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

    // --- ratchet hits waiting for the block they belong to ---
    //
    // A ratchet arrives as EVERY hit at once. `core/scheduler` resolves a step into one event per
    // hit, each at `frame` plus an offset inside the step, and its header is explicit about whose
    // problem that is: "which can be past the end of the block: spreading them is the caller's to
    // defer, not the scheduler's to withhold." The caller is this file, and it did not defer.
    //
    // What that cost: a machine stores ONE pending trig per voice and `trigger` overwrites a
    // pending one without looking, so a ratchet needs as many voices as it has hits and the
    // machines have 1, 2, 4 or 6. Measured on KICK at 130 BPM, where a step is 5,538 frames: a
    // 3-hit ratchet sounded 2 hits and the one it dropped was the hit ON THE STEP; a 4-hit
    // ratchet sounded 2; an 8-hit ratchet sounded 2. VA MONO, with one voice, kept only the LAST
    // hit, so a ratcheted bass note arrived most of a step late and did not ratchet at all.
    //
    // So an event whose frame is past this block is held here and its frame counted down a block
    // at a time until it lands. One hit per machine per block is then all that can arrive, which
    // is what the one-pending-trig contract can take -- and that is guaranteed rather than hoped
    // for: hits are `step_samples / hits` apart, and at the ABI's fastest tempo (300 BPM) on the
    // fastest speed (2x) that is 150 frames, well over a 64-frame block.
    //
    // 128 slots: 8 tracks x the 8-hit maximum is 64 outstanding, doubled because micro-timing can
    // pull the next step up to 11 ticks earlier and start a second ratchet before the first has
    // finished spreading. 2 KiB, and a member rather than a local because CLAUDE.md caps a frame
    // at 16 KiB. An overflow drops the NEWEST event and counts a refusal: the oldest is the one
    // nearest to sounding and, in a ratchet, the one on the step.
    static constexpr int kMaxDeferredEvents = 128;
    StepEvent deferred_[kMaxDeferredEvents];
    int deferred_count_ = 0;

    // This block's length, so `resolve_tick` can tell a frame inside it from one past it. Set at
    // the top of `process` rather than threaded through, because `resolve_tick` is called from
    // the tick loop and already reads half the engine's state.
    uint32_t block_frames_ = 0;

    // Fire `event` now if it falls in this block, else defer it. `frames` is the block's length.
    void dispatch(const StepEvent &event, uint32_t frames);

    // Count every deferred event down by a block and fire the ones that have arrived.
    void deliver_deferred(uint32_t frames);

    // Cached so a tempo change can re-derive the scheduler's ratchet spacing without dividing
    // every block. Enaga's clock reports the tempo; core/ cannot ask it.
    float tempo_ = 0.0f;
    float samples_per_tick_ = 0.0f;   // derived from it; see reconfigure()

    SendSettings sends_;
    MasterSettings master_;
    PerformState perform_;

    // Resampling's take buffer, 5.76 MiB, taken in init() like everything else.
    //
    // `resample_bars_` is what was asked for; the take BEGINS on the next bar line, because a
    // take that is not bar-aligned does not loop and a round number of bars is what makes it a
    // loop. dsp/capture.hpp holds the rest of the reasoning.
    Capture capture_;
    uint8_t resample_bars_ = 0;      // 0 = nothing armed

    // Which track's trigs fire the sidechain duck. docs/SPEC.md: track 1 by default.
    int duck_source_ = 0;

    // How far the other tracks duck under it: 0 bypasses, 1 ducks fully (dsp/buses.hpp). SPEC
    // gives the duck's attack, hold and release and no depth, so this is a judgement -- enough
    // to hear the kick come through a dense pattern, not enough to pump the whole mix. It is a
    // number to settle by ear on the unit, and it is not editable from the panel yet.
    static constexpr float kDuckDepth = 0.35f;

    Commands commands_;
    Status status_;

    float rate_ = 48000.0f;
    bool ready_ = false;
};

}  // namespace strata
