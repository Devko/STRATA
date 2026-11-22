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
#include "grain.hpp"      // GRAIN, reachable at last: a track can host any machine
#include "commands.hpp"
#include "editops.hpp"
#include "extmidi.hpp"    // MID: the machine that plays nothing and sends MIDI
#include "hat.hpp"
#include "kick.hpp"
#include "midiout.hpp"    // MIDI note out: every note-on gets its note-off
#include "mutate.hpp"      // Cmd::mutate
#include "pattern.hpp"
#include "perc.hpp"
#include "perform.hpp"   // scenes, the morph, macros, mute states, auto-fill
#include "queue.hpp"       // and core/project.hpp through it: the project owns the live pattern
#include "repeat.hpp"    // beat repeat, on the master output
#include "tapestop.hpp"  // the tape stop, after beat repeat
#include "arp.hpp"       // the arpeggiator's notes
#include "sample.hpp"
#include "scale.hpp"       // the global key and scale: Cmd::set_root and set_scale
#include "scheduler.hpp"
#include "snare.hpp"
#include "stepedit.hpp"
#include "streamfifo.hpp"  // a streamed sample's read-ahead, per track
#include "track.hpp"
#include "chord.hpp"     // a POLY step's chord byte, expanded in fire()
#include "undo.hpp"
#include "va.hpp"
#include "vapoly.hpp"    // the CHORD track's six voices
#include "lfo.hpp"       // one LFO per track, parameters 16-19

#include <cstdint>

#include <enaga/files.hpp>  // files::Stream: a streamed sample's tail, read in audio()

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
    // The key, which docs/SPEC.md makes GLOBAL: PROJECT.BIN's root and scale, not the live
    // pattern's copies. Reading the pattern's meant every other pattern played in C chromatic
    // (its zeroed default), the key changed on a pattern switch, and an undo reverted it.
    uint8_t key_root() const { return uint8_t(project_.settings.root % 12); }
    uint8_t key_scale() const { return project_.settings.scale; }

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

    // The shared voice budget: at most kPolyVoiceBudget VA POLY voices sounding across every
    // track at once. Any track can host VA POLY (machine switching), each has six voices and a
    // sustaining voice costs ~2.2 points with cl6x (docs/BUDGET.md), so eight chord tracks
    // would be past 100% of the block with nothing to stop them. Past the budget, a new note
    // chokes the quietest started voice on ANY POLY track (a 1.3 ms fade, VaVoice::choke); if
    // every counted voice is still waiting for its frame, the new note is dropped rather than
    // let the total grow. Twelve is two full six-voice chords: a starting point to be set from
    // the unit's MAX readings (docs/BUDGET.md, U2), not a measured limit.
    static constexpr int kPolyVoiceBudget = 12;

    // A track's MIDI OUT channel, 0 off or 1-16 (ProjectSettings::midi_out). For the screen.
    uint8_t midi_out(int track) const {
        return track >= 0 && track < kTracks ? project_.settings.midi_out[track] : 0;
    }

    // The app is stopping (App::stop): end every MIDI note this side has sent, and All Notes Off
    // on each channel in use. MAIN context, and only once the audio callback is gone -- Enaga
    // clears it before it calls stop() -- so nothing else is writing the note table. Without it a
    // note sounding when the user switched app or powered off hung on the outboard synth, with
    // no STRATA panel left to stop it.
    void end_midi_for_shutdown();

    // Also App::stop only, for the same reason: apply what is still in the command ring from the
    // MAIN context, because the audio callback that would apply it is gone. The shutdown save
    // posts its own `kit_store` and needs it applied before it serializes the kit.
    void drain_commands_for_shutdown();

    // What MIDI out has sounding, for the tests. Audio side; the rig runs both on one thread.
    const MidiOut &midi_notes() const { return midi_out_; }

    // Tests only: false makes every prepare() run, as it did before the memo, so a test can
    // render the same input both ways and compare the output bit for bit.
    void set_prepare_memo(bool on) { prepare_memo_ = on; forget_prepared(); }

    // The finished take, for the card to write. Valid from the main context only while
    // `status().resample_state` reads 3, which is the same discipline a project save uses.
    const Capture &capture() const { return capture_; }

    // Which machine a track hosts, as a core/project.hpp machine id. For the screen.
    uint8_t machine_id(int track) const {
        return track >= 0 && track < kTracks ? machine_id_[track] : 0;
    }

    // The note the last trig on `track` handed its machine, after the scale fold. Written and
    // read on the audio side only; the host tests read it because the rig runs both contexts on
    // one thread, and a Sampler or Granular cannot be asked what note it was given.
    uint8_t sent_note(int track) const {
        return track >= 0 && track < kTracks ? sent_note_[track] : 0;
    }

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

    // The held master moves, for the screen and the tests.
    const TapeStop &tape_stop() const { return tape_stop_; }
    const DelayFreeze &delay_freeze() const { return mixer_.freeze(); }
    // Either delay's freeze: the classic's loop copy or the LONG delay's own.
    bool delay_frozen() const { return mixer_.delay_frozen(); }
    const Mixer &mixer() const { return mixer_; }

    // Whether an arp is running on `track`: a sequenced step's, or held KEYS keys'.
    bool arp_running(int track) const {
        return track >= 0 && track < kTracks && (seq_arp_[track].on || held_arp_[track].on);
    }

    // Whether a note repeat is rolling on `track` (Cmd::note_repeat).
    bool rolling(int track) const { return track >= 0 && track < kTracks && rolls_[track].on; }

    // The performance layer writes over the kit without changing it, so the UI has to be able to
    // draw what is actually being heard as well as what is stored. `params` is the kit; this is
    // the kit with the morph, the macros and any throw over the top.
    uint8_t performed(int track, int param) const {
        return track >= 0 && track < kTracks && param >= 0 && param < kTrackParams
                   ? performed_[track][param]
                   : 0;
    }

    // The LFO, for the screen: is it moving a parameter on `track`, its last value (-1..1), and
    // what the machines are prepared from -- the kit, performed, locked and LFO'd. Audio-side
    // single words and bytes, read as `performed` is.
    bool lfo_on(int track) const { return track >= 0 && track < kTracks && modulating_[track]; }
    float lfo_value(int track) const {
        return track >= 0 && track < kTracks ? lfo_value_[track] : 0.0f;
    }
    const TrackParams &sounding(int track) const {
        return modulating_[track] ? modulated_[track] : effective_[track];
    }

    // The master EQ band byte being heard (0-2), with the morph and macros over the knob.
    uint8_t performed_master(int band) const {
        return band >= 0 && band < kMasterBands ? performed_master_[band] : 0;
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

    // The kit browser's inbox (app/storage.cpp, "the kit library"): a kit read off the card,
    // waiting for `Cmd::kit_recall` with `a` = 1 to copy it into `project_.kit` on the audio side.
    //
    // A smaller hole than `project_for_load` and one that needs no stopped transport: the audio
    // side reads this ONLY while applying that command, and the main context writes it only
    // while the command ring is EMPTY -- so no recall that could read it is pending, and the one
    // it is about to post is the next thing to look. A recall therefore plays straight through a
    // running pattern, which is what a kit change on stage needs.
    Kit &kit_for_recall() { return kit_inbox_; }

    // App::init() only, before the first audio block. The region and the SamplePool that carves
    // it up belong to the MAIN context (see `sample_region_` below); the audio side is handed the
    // base so `Cmd::set_sample` can turn the loader's byte offset into a pointer, and the size so
    // that an offset from a damaged command cannot be followed out of the region.
    void set_sample_region(const int16_t *base, uint32_t bytes);

    // --- streaming (app/storage.hpp, "Streaming") ---

    // What Cmd::set_stream hands over, too much for a command: written by the main context just
    // before the post and read by the audio side only while applying it -- kit_for_recall's rule.
    struct StreamInfo {
        uint32_t head_offset = 0;   // the head's byte offset in the sample region
        uint32_t head_frames = 0;   // resident frames, the file's first
        uint32_t total_frames = 0;  // the whole file's
        uint32_t rate = 0;          // the file's own rate: a stream is not resampled
        uint8_t channels = 1;
        uint8_t encoding = 0;       // a PcmFormat
        uint8_t frame_bytes = 0;    // in the file
    };
    StreamInfo &stream_for_load(int track) { return stream_inbox_[track]; }

    // A sample's slice points (core/onsets.hpp), for SLC AUTO. An inbox like the stream's: the
    // loader writes the track's entry just before it posts Cmd::set_sample, and the audio side
    // copies it into the Sampler when it applies that command. A set_sample is only posted after
    // the track's previous one has been applied (the clear_sample epoch), so the two never meet.
    SlicePoints &slices_for_load(int track) { return slice_inbox_[track]; }

    // The track's stream. The MAIN context opens and closes it, the AUDIO context reads it, and
    // never both at once: the audio side reads only between a Cmd::stream_attach and its own
    // next Status::stream_want bump, and the card side opens or closes only after seeing a bump
    // (or the epoch of a clear_sample) and before posting an attach.
    enaga::files::Stream &stream(int track) { return streams_[track]; }
    // For tests: is the audio side reading the track's stream?
    bool stream_attached(int track) const { return stream_state_[track].attached; }
    const StreamFifo &stream_fifo(int track) const { return fifos_[track]; }

private:
    void apply_commands();
    void apply(const Command &command);
    void reconfigure();

    // Make `bank`/`slot` the live pattern. Clears the undo history (it belonged to the pattern
    // being left) and re-derives the scheduler's timing and the pass boundary.
    void switch_pattern(int bank, int slot);

    // The pass boundary passed: let a chain move on if one is playing.
    void cross_boundary(uint32_t boundary);

    // A bar started: release anything armed for one.
    void on_bar();
    void apply_mutes(uint8_t state);
    void silence_track(int track);     // kill_all plus the insert chain's state   // the only writer of muted_ after init
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

    // The same idea for the mixer: `project_.settings.sends` / `.master` are the fourteen
    // encoder bytes a file holds, and `sends_` / `master_` are the floats the mixer runs on.
    // This is the one direction that exists -- the bytes are the source of truth, so there is no
    // "read the floats back" to go wrong. Called by a knob move, by a tempo change (the delay is
    // tempo-synced) and by a project load.
    void apply_mixer_bytes();

    void build_placeholder_sample();

    // --- MIDI out (core/midiout.hpp) ---
    //
    // Sent from the audio context, which <enaga/midi.hpp> allows: a send queues bytes and never
    // blocks, and returns false rather than waiting when the queue is full. Sending here rather
    // than posting the notes to the main context is what keeps a note-on within a block of its
    // step: the main loop runs at the UI's pace, not the audio's.
    struct EnagaMidi final : MidiSink {
        bool send(const uint8_t *bytes, uint32_t count) override;
    };
    EnagaMidi midi_sink_;
    MidiOut midi_out_;

    // Send `count` notes of one trig to the track's MIDI OUT channel, if it has one.
    void send_midi_notes(int track, const uint8_t *notes, int count, uint8_t velocity,
                         uint32_t frame, uint32_t gate);
    // A MIDI gate in frames for a trig whose own gate is 0 (no gate): one step of the track.
    uint32_t midi_gate(int track, uint32_t gate_frames) const;
    // A MID track's eight CC slots: send each one whose value has moved (per block).
    void send_midi_ccs();
    void send_midi_ccs(int track);
    // CC 123 on every channel a track sends on: the second STOP and FUNC+F6.
    void midi_all_notes_off();

    // The values each MID track's CC slots last sent, and whether they are known: a slot is sent
    // when it MOVES, never just because a track became MID or got a channel -- switching a track
    // to MID must not throw eight arbitrary values at a synth that was set up by hand.
    uint8_t cc_sent_[kTracks][kParams] = {};
    bool cc_known_[kTracks] = {};

    // The placeholder's PCM (a static array in build_placeholder_sample), for `Cmd::clear_sample`
    // to fall back to: a track with no file plays it, after a swap as before the first one.
    const int16_t *placeholder_ = nullptr;
public:
    // For tests: the PCM a track with no file plays.
    const int16_t *placeholder() const { return placeholder_; }
private:
    uint32_t placeholder_frames_ = 0;

    // --- the machine pool ---
    // Allocated once, here, and pointed at by the track slots. docs/SPEC.md wants any track to
    // host any machine; M1 wires the default techno kit's first four tracks and leaves the rest
    // silent until M2 brings PERC, SAMPLE and VA MONO.
    // One instance per track of the default kit, constructed here so every allocation happens
    // in init(). docs/SPEC.md wants any track to host any machine; doing that at runtime needs
    // the voice-pool question answered (a track switching to VA POLY would want six voices it
    // does not currently own), so for now the assignment is fixed at init and a machine change
    // is a later milestone. Tracks with no machine are silent and cost one compare.
    // EVERY track's own instance of EVERY machine.
    //
    // docs/PLAN.md deferred machine switching because "a track switching to VA POLY would want
    // six voices it does not currently own", and this is the answer: give every track all of
    // them and point `machines_[t]` at the one its kit names. The question was about memory, so
    // it was settled by measuring rather than by designing a pool --
    //
    //   Kick 1840  Snare 1592  Hat 840  Perc 704  VaMono 384  VaPoly 896  Sampler 928
    //   Granular 2104  =  9,288 bytes a track, 72.6 KiB for eight
    //
    // -- against the 85 MiB this app already takes. A pool would have cost allocation logic, a
    // failure mode when it ran dry and a reason for a track to be refused a machine, to save
    // seventy kilobytes.
    //
    // What it does NOT solve is the budget: eight tracks of VA POLY is 48 voices at 4.25 points
    // each (docs/BUDGET.md), which is four times the block. Nothing here prevents that, and
    // nothing should -- the same is true of eight tracks of anything.
    struct TrackMachines {
        Kick kick;
        Snare snare;
        Hat hat;
        Perc perc;
        VaMono mono;
        VaPoly poly;
        Sampler sampler;
        Granular grain;
        MidiMachine midi;   // no state at all: the vtable pointer
    };
    TrackMachines pool_[kTracks];

    // Streaming, per track. Audio side only, apart from the inbox and the stream objects, whose
    // rules are at stream_for_load and stream() above.
    struct StreamState {
        bool on = false;              // the track's Sampler is in stream mode
        bool attached = false;        // reading streams_[t] into fifos_[t]
        uint8_t encoding = 0;
        uint8_t frame_bytes = 0;
        uint8_t carry = 0;            // bytes of a frame split across two reads
        uint8_t carry_bytes[8] = {};
        uint32_t total = 0;
        uint32_t restarts_seen = 0;
        uint32_t late_seen = 0;
        uint16_t want = 0;            // what Status::stream_want[t] holds
    };
    void top_up_streams();            // before rendering: the card's bytes into the fifos
    void watch_streams();             // after: a restart detaches and asks for a reopen
    void stop_stream(int track);      // leave stream mode (clear_sample)
    // Frames read per track per block at most: 512 is eight blocks of a voice at the root pitch,
    // so a fifo emptied by a restart is full again well before the head runs out.
    static constexpr uint32_t kStreamReadFrames = 512;
    StreamInfo stream_inbox_[kTracks]{};
    SlicePoints slice_inbox_[kTracks]{};
    StreamState stream_state_[kTracks]{};
    StreamFifo fifos_[kTracks];
    enaga::files::Stream streams_[kTracks];
    uint8_t stream_bytes_[kStreamReadFrames * 8u + 8u] = {};
    uint32_t stream_late_ = 0;        // Status::stream_late: only ever grows

    // The hat choke is a pair, and the pair is tracks 3 and 4 of the DEFAULT kit. Machine
    // switching makes that an assumption rather than a fact, so it is re-derived on every
    // change: a closed hat chokes an open one only while both tracks still host a Hat.
    void rewire_machines();

    Machine *machines_[kTracks] = {};

    uint8_t machine_id_[kTracks] = {};
    uint8_t sent_note_[kTracks] = {};  // see sent_note()

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
    // Song mode: the song has played to its end and the sequencer is silent until the transport
    // stops or starts again (`cross_boundary`).
    bool song_over_ = false;
    void chain_entry_started();

    // See `kit_for_recall`. Written by the main context, read by `Cmd::kit_recall` only.
    Kit kit_inbox_;

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
    uint8_t performed_[kTracks][kTrackParams] = {};
    // The master kill EQ's three band bytes with the morph and the macros over the project's,
    // which is what the mixer runs on. The kill switches are not in here: they are the panel's.
    uint8_t performed_master_[kMasterBands] = {64, 64, 64};
    bool perform_dirty_ = true;

    // Which of a track's parameters the CURRENT step has locked, one bit each. The performance
    // layer skips them, so a lock always wins over a morph -- which is what docs/REVIEW.md's
    // "base kit values only" means from the other side. Set by `fire` and therefore lasting
    // exactly as long as the step does.
    uint32_t locked_[kTracks] = {};
    static_assert(kTrackParams <= 32, "locked_ holds one bit per parameter");

    // The LFOs (dsp/lfo.hpp). `lfo_bytes_` is what `lfo_settings_` was made from, so the
    // settings are rebuilt only when a byte moves; `modulated_` is `effective_` with the
    // destination moved, valid while `modulating_` says so.
    void run_lfos(uint32_t frames, bool running, uint32_t position, float fraction);
    TrackLfo lfo_[kTracks];
    LfoSettings lfo_settings_[kTracks];
    uint8_t lfo_bytes_[kTracks][kLfoParams] = {};
    TrackParams modulated_[kTracks] = {};
    bool modulating_[kTracks] = {};
    float lfo_value_[kTracks] = {};

    ChainMix mix_[kTracks] = {};
    bool muted_[kTracks] = {};

    // What each track's machine and insert chain were last prepared from, so a block whose
    // parameters have not moved skips both prepare() calls (docs/BUDGET.md, "Prepare only on
    // change"). Every prepare() is a pure function of its arguments plus state that only a
    // COMMAND changes (a sample, a machine switch, a project load), so the memo is the bytes,
    // the machine it was prepared for, and a flag every applied command clears. Skipping is
    // therefore bit-exact by construction, which tests/test_engine.cpp holds it to by rendering
    // with the memo on and off.
    bool make_room_for_poly_voice(int track, uint8_t note);

    void prepare_machine(int track);
    void prepare_chain(int track);
    void forget_prepared();
    TrackParams machine_prepared_[kTracks] = {};
    const Machine *machine_prepared_for_[kTracks] = {};
    TrackParams chain_prepared_[kTracks] = {};
    ChainMix chain_prepared_mix_[kTracks] = {};
    bool chain_prepared_valid_[kTracks] = {};
    bool prepare_memo_ = true;

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
    TapeStop tape_stop_;
    RepeatLength repeat_length_ = RepeatLength::sixteenth;

    // Note repeat, one roll per track (Cmd::note_repeat). `countdown` is frames to the next hit
    // with the transport stopped; `last_tick` is the tick of the press, so the first grid tick
    // after it is skipped when it would land on top of the press's own hit.
    struct Roll {
        bool on = false;
        uint8_t note = 60;
        uint8_t velocity = 100;
        uint32_t last_tick = 0;
        float countdown = 0.0f;
    };
    Roll rolls_[kTracks];

    // The arpeggiator (core/arp.hpp). One run per track for a sequenced step -- started by
    // `fire`, ending with the step's length or at the next step -- and one for held KEYS keys,
    // which runs while any is down. Both hit on the clock's ticks while it runs; the held one
    // counts frames while it is stopped, as a note repeat does.
    struct ArpRun {
        bool on = false;
        ArpMode mode = ArpMode::off;
        uint8_t length = 0;
        uint8_t octaves = 1;
        uint8_t rate = 24;           // ticks between hits
        uint8_t gate_percent = 50;
        uint8_t velocity = 100;
        bool accent = false;
        uint32_t hit = 0;
        uint32_t next_tick = 0;
        uint32_t end_tick = 0xFFFFFFFFu;
        uint32_t seed = 0;
        float countdown = 0.0f;      // frames to the next hit, transport stopped (held runs)
        uint8_t cycle[kArpMaxCycle] = {};
    };
    ArpRun seq_arp_[kTracks];
    ArpRun held_arp_[kTracks];
    uint8_t held_notes_[kTracks][kArpMaxInput] = {};
    uint8_t held_count_[kTracks] = {};
    ArpSettings arp_settings(int track) const;
    void arp_hit(int track, ArpRun &run, uint32_t frame);
    void arp_tick(uint32_t tick, uint32_t frame);
    void arp_free(uint32_t frames);
    void arp_held_changed(int track);
    void arp_follow(int track);
    void roll_fire(int track, uint32_t frame);
    void roll_tick(uint32_t tick, uint32_t frame);
    void roll_free(uint32_t frames);
    float roll_period_frames() const;
    uint32_t roll_gate_frames() const;
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
    // The tick the playing pattern started on. Queued and chained switches restart the pattern
    // on their boundary, and the scheduler is handed `tick - origin_` (core/queue.hpp).
    uint32_t origin_ = 0;

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
    float inv_samples_per_tick_ = 0.0f;
    float inv_rate_ = 1.0f / 48000.0f;

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
    bool capture_input_ = false;     // the armed or running take records the input, not the master

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
