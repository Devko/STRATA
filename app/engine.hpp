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
#include "frozen.hpp"     // freeze: a track playing a loop of itself
#include "grain.hpp"      // GRAIN, reachable at last: a track can host any machine
#include "commands.hpp"
#include "cpuguard.hpp"   // sheds POLY voices and grains when the block nears its budget
#include "editops.hpp"
#include "fmpoly.hpp"     // FM4: four operators, six voices, in the POLY budget
#include "extmidi.hpp"    // MID: the machine that plays nothing and sends MIDI
#include "hat.hpp"
#include "kick.hpp"
#include "midiout.hpp"    // MIDI note out: every note-on gets its note-off
#include "mutate.hpp"      // Cmd::mutate
#include "evolve.hpp"      // EVOLVE: a track mutating every few passes
#include "generate.hpp"    // Cmd::generate
#include "pattern.hpp"
#include "perc.hpp"
#include "perform.hpp"   // scenes, the morph, macros, mute states, auto-fill
#include "queue.hpp"       // and core/project.hpp through it: the project owns the live pattern
#include "repeat.hpp"    // beat repeat, on the master output
#include "tapestop.hpp"  // the tape stop, after beat repeat
#include "perffx.hpp"    // the performance mixer's slots: push VALUE + STEP
#include "masterfx.hpp"  // its own master effects: pump, crush, noise, gate
#include "looper.hpp"    // its slot 16: the master looper, in RESAMPLE's buffer
#include "lockstore.hpp" // LOCK: FUNC + MUTATE stores, jumps and swaps back
#include "launch.hpp"    // Track Launch: a lane playing another slot's part
#include "moveall.hpp"   // REL ALL and LEN ALL: every track's release and note length
#include "varispeed.hpp" // VariSpeed: every machine's pitch with the tempo
#include "arp.hpp"       // the arpeggiator's notes
#include "arrrec.hpp"    // ARR REC: the jam written into the chain
#include "sample.hpp"
#include "scale.hpp"       // the global key and scale: Cmd::set_root and set_scale
#include "scope.hpp"       // the master output for the screen's scopes and spectra
#include "scheduler.hpp"
#include "snare.hpp"
#include "stepedit.hpp"
#include "streamfifo.hpp"  // a streamed sample's read-ahead, per track
#include "audition.hpp"    // the sample browser's audition (push A in SAMPLES)
#include "click.hpp"       // the metronome and the count-in
#include "track.hpp"
#include "chord.hpp"     // a POLY step's chord byte, expanded in fire()
#include "undo.hpp"
#include "va.hpp"
#include "vapoly.hpp"    // the CHORD track's six voices
#include "wavetable.hpp" // the WAVE pair's tables, built once in init()
#include "lfo.hpp"       // one LFO per track, parameters 16-19

#include <cstdint>

#include <enaga/files.hpp>  // files::Stream: a streamed sample's tail, read in audio()
#include <enaga/wav.hpp>    // wav::Writer: JAM REC's file, written in audio()

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
    // The master output for the screen (core/scope.hpp): the audio side pushes, the main
    // context takes.
    ScopeRing &scope() { return scope_; }
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
    // Track Launch: a lane's queued launch (core/launch.hpp), and the pattern lane `track` plays.
    const LaneQueue &lane_queue(int track) const { return lane_queue_[track]; }
    const Pattern &lane(int track) const { return lane_pattern(project_, track); }
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

    // The budget in force now: kPolyVoiceBudget, or less while the CPU guard is up
    // (core/cpuguard.hpp). For tests and the screen.
    int poly_budget() const { return poly_budget_; }
    int cpu_guard_level() const { return cpu_guard_.level(); }

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
    const FrozenLoop &frozen(int track) const { return frozen_[track]; }

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
    // The sidechain duck as the mixer runs it: each track's depth (0 for the source) and which
    // track fires it. The kit's bytes are in project().kit.
    float duck_depth(int track) const { return mix_[track].duck; }
    int duck_source() const { return duck_source_; }
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
    // REL ALL and LEN ALL (core/moveall.hpp): 0 is rest.
    int8_t rel_all() const { return rel_all_; }
    int8_t len_all() const { return len_all_; }
    // LIVE RND (core/liverand.hpp): the unit (0 OFF) and the armed tracks.
    uint8_t live_rnd_unit() const { return perform_.rnd_unit; }
    uint8_t live_rnd_armed() const { return perform_.rnd_armed; }
    // ENERGY (core/energy.hpp): -64 .. +64, 0 is rest.
    int8_t energy() const { return perform_.energy; }
    // VariSpeed (core/varispeed.hpp): tenths of a semitone on every machine, 0 is rest.
    int8_t varispeed() const { return vari_tenths_; }
    AutoFill auto_fill() const { return auto_fill_; }
    // EVOLVE (core/evolve.hpp): a track's period index (0 OFF) and the shared settings.
    uint8_t evolve_period(int track) const {
        return track >= 0 && track < kTracks ? evolve_[track].period : uint8_t(0);
    }
    EvolveMode evolve_mode() const { return evolve_mode_; }
    Mutation evolve_kind() const { return evolve_kind_; }
    int evolve_amount() const { return int(evolve_amount_); }
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

    // The performance mixer (core/perffx.hpp), for the screen and the tests.
    PerfFx perf_fx() const { return perf_fx_; }
    uint8_t perf_amount() const { return perf_amount_; }
    const PerfPlan &perf_plan_now() const { return perf_plan_; }
    uint8_t looper_mix() const { return looper_mix_; }

    // LOCK (core/lockstore.hpp), for the tests: whether pattern `index` (bank * 16 + slot) has one.
    bool has_lock(int index) const { return locks_.has(index); }
    // How often the "changed since the lock" compare runs, in blocks (publish_lock_state).
    static constexpr int kLockCheckBlocks = 16;
    const Looper &looper() const { return looper_; }
    const MasterFx &master_fx() const { return master_fx_; }

    // Whether an arp is running on `track`: a sequenced step's, or held KEYS keys'.
    bool arp_running(int track) const {
        return track >= 0 && track < kTracks && (seq_arp_[track].on || held_arp_[track].on);
    }

    // Whether a note repeat is rolling on `track` (Cmd::note_repeat).
    bool rolling(int track) const { return track >= 0 && track < kTracks && rolls_[track].on; }
    // The step the stutter is retriggering (Cmd::step_stutter), or -1.
    int stutter_step() const { return stutter_step_; }

    // The performance layer writes over the kit without changing it, so the UI has to be able to
    // draw what is actually being heard as well as what is stored. `params` is the kit; this is
    // the kit with the morph, the macros and any throw over the top.
    uint8_t performed(int track, int param) const {
        return track >= 0 && track < kTracks && param >= 0 && param < kTrackParams
                   ? performed_[track][param]
                   : 0;
    }

    // The LFO: is it moving a parameter on `track`, its last value (-1..1), and what the
    // machines are prepared from -- the kit, performed, locked and LFO'd. Audio side and the
    // tests (the rig runs both contexts on one thread): `modulated_` is rewritten every block,
    // so the screen and step_waves read Status's heard_* and lfo_shown (publish_heard) instead.
    // The LFO's own DEP and DST: a mod slot alone also keeps `modulating_` on.
    bool lfo_on(int track) const { return track >= 0 && track < kTracks && lfo_running_[track]; }
    // A mod slot's source value this block (-1..1 or 0..1, core/modslot.hpp), 0 for a slot that
    // is off. Audio side and the tests; the screen reads Status::mod_heard.
    float mod_value(int track, int slot) const {
        return track >= 0 && track < kTracks && slot >= 0 && slot < kModSlots
                   ? mod_value_[track][slot]
                   : 0.0f;
    }
    float lfo_value(int track) const {
        return track >= 0 && track < kTracks ? lfo_value_[track] : 0.0f;
    }
    // Where the LFO has its destination this block, as a byte: the moved byte, or for TUN on a
    // synth machine (which moves by a float and leaves the byte alone) the byte it would be.
    uint8_t lfo_shown(int track) const {
        return track >= 0 && track < kTracks ? lfo_shown_[track] : uint8_t(0);
    }
    // The pitch offset the LFO is handing the track's machine, in semitones (0 when none).
    float lfo_pitch_mod(int track) const {
        return track >= 0 && track < kTracks ? pitch_mod_[track] : 0.0f;
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

    // Main context, from App::update: asks for the tables the WAVE tracks sound now and does one
    // small piece of building them (dsp/wavetable.hpp, WaveLibrary). False when there is
    // nothing to do. A WAVE voice plays its sub alone until its table is in.
    bool step_waves();
    const WaveLibrary &waves() const { return waves_; }
    // Slots for tables: one per track, so eight WAVE tracks on eight tables all play.
    static constexpr int kWaveSlots = kTracks;

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

    // --- sample audition (dsp/audition.hpp) ---

    // What Cmd::audition_start hands over: written by the main context just before the post and
    // read by the audio side only while applying it, as stream_for_load.
    struct AuditionInfo {
        uint32_t total_frames = 0;  // the file's
        uint32_t step = 0;          // Q8.24, Audition::step_for(the file's rate, 48 kHz)
        uint8_t channels = 1;
        uint8_t encoding = 0;       // a PcmFormat
        uint8_t frame_bytes = 0;    // in the file
    };
    AuditionInfo &audition_for_load() { return audition_inbox_; }
    // The audition's stream: the main context opens it and posts audition_start, the audio side
    // reads it until it bumps Status::audition_epoch, and only then does the main context close it.
    enaga::files::Stream &audition_stream() { return audition_stream_; }
    // For tests.
    const Audition &audition() const { return audition_; }
    bool audition_attached() const { return audition_on_; }

    // --- JAM REC (app/storage.hpp, "jam recording") ---

    // The jam's file. The main context creates it and posts Cmd::jam_start, the audio side writes
    // the finished master into it every block until a Cmd::jam_stop, after which it bumps
    // Status::jam_epoch, and only then does the main context close it.
    enaga::wav::Writer &jam_writer() { return jam_; }
    const enaga::wav::Writer &jam_writer() const { return jam_; }
    // For tests: is the audio side writing it?
    bool jam_attached() const { return jam_on_; }

private:
    void apply_commands();
    void apply(const Command &command);
    // apply() checks the track and step, then hands the command to its family. Each
    // returns false for a command not its own. In app/engine.cpp, in this order.
    bool apply_sound(const Command &command, int track, int step);
    bool apply_step_edit(const Command &command, int track, int step);
    bool apply_perform(const Command &command, int track, int step);
    bool apply_notes(const Command &command, int track, int step);
    bool apply_mixer(const Command &command, int track, int step);
    bool apply_track_timing(const Command &command, int track, int step);
    bool apply_bulk_edit(const Command &command, int track, int step);
    bool apply_transport(const Command &command, int track, int step);
    bool apply_arrangement(const Command &command, int track, int step);
    bool apply_project(const Command &command, int track, int step);
    bool apply_samples(const Command &command, int track, int step);
    bool apply_capture(const Command &command, int track, int step);
    void reconfigure();

    // Make `bank`/`slot` the live pattern. Clears the undo history (it belonged to the pattern
    // being left) and re-derives the scheduler's timing and the pass boundary.
    void switch_pattern(int bank, int slot);
    // A whole pattern in memory was replaced (pattern_copy, pattern_clear): when it is the one
    // playing, what was derived from it is stale, as after a switch.
    void pattern_replaced(int bank, int slot);
    // Track Launch: lane `track` plays `source` from now on, from its step 1 at `start` when
    // `restart`, else keeping its place. Its pending ratchet hits, roll and slides belong to the
    // part it left and go.
    void land_launch(int track, uint8_t source, bool restart, uint32_t start);
    // The lanes' own patterns for the scheduler.
    void lane_patterns(const Pattern *lanes[kTracks]) const;
    // The lane a track-edit command names plays another part: refused, and said (Status).
    bool launch_refuses(const Command &command, int track);

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
    // Each SAMPLE and GRAIN track's WaveView into Status::wave, every kWaveEveryBlocks blocks.
    void publish_waves();
    // What each track sounds, the bytes the main context needs of it, into Status (heard_table
    // and the rest), every block and once from init.
    void publish_heard();
    void fire(const StepEvent &event);

    // `Step::length` as a frame count for `Trig::gate_frames`; 0 for "no gate".
    uint32_t gate_frames_for(int track, uint8_t length_index) const;

    // The kit with the performance layer over it: the morph, then the macros, then any throw.
    // Control rate, and only when something has moved -- which is why `perform_dirty_` exists
    // rather than recomputing 128 parameters every block for a morph nobody is touching.
    void apply_performance();
    // The performance mixer's overrides on copies of the sends and the master.
    void apply_perf_plan(SendSettings &sends, MasterSettings &master) const;

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

    // And for the sidechain duck: the kit's bytes (KitTrack::duck, KitDuck) are the truth, and
    // this hands them to the mixer -- the envelope's times, which track fires it, and each
    // track's depth, with the source's forced to 0 so a track never ducks under itself. Called
    // at init, whenever a kit arrives (params_from_kit) and on every Cmd::set_duck.
    void apply_duck();

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
    // PROG CHG OUT: the user's MIDI CH (0-15, from the main context) and a program change still
    // to send, -1 for none. Kept until the queue takes it, as a refused note-off is: a lost
    // program change leaves the other gear on the wrong pattern for the rest of the set.
    uint8_t midi_channel_ = 0;
    int16_t program_pending_ = -1;
    void send_program_pending();
    // Cmd::locate: the pattern or chain put where an outside song position falls.
    void locate(uint32_t tick);

    // Send `count` notes of one trig to the track's MIDI OUT channel, if it has one.
    void send_midi_notes(int track, const uint8_t *notes, int count, uint8_t velocity,
                         uint32_t frame, uint32_t gate);
    // A MIDI gate in frames for a trig whose own gate is 0 (no gate): one step of the track.
    uint32_t midi_gate(int track, uint32_t gate_frames) const;
    // A MID track's eight CC slots: send each one whose value has moved (per block).
    // The per-block pass; `frames` counts down the LFO's CC wait. The one-track form goes
    // ahead of a note and is never held back.
    void send_midi_ccs(uint32_t frames);
    void send_midi_ccs(int track, bool throttle = false);
    // CC 123 on every channel a track sends on: the second STOP and FUNC+F6.
    void midi_all_notes_off();

    // The values each MID track's CC slots last sent, and whether they are known: a slot is sent
    // when it MOVES, never just because a track became MID or got a channel -- switching a track
    // to MID must not throw eight arbitrary values at a synth that was set up by hand.
    uint8_t cc_sent_[kTracks][kParams] = {};
    bool cc_known_[kTracks] = {};
    // An LFO on a MID track moves its destination's CC every block; the per-block pass sends
    // it at most once per kLfoCcMs, so sixteen tracks of LFO stay within a DIN port's ~1000
    // messages a second. Frames left before the next one may go.
    static constexpr float kLfoCcMs = 20.0f;
    uint32_t lfo_cc_wait_[kTracks] = {};

    // The placeholder's PCM (a static array in build_placeholder_sample), for `Cmd::clear_sample`
    // to fall back to: a track with no file plays it, after a swap as before the first one.
    const int16_t *placeholder_ = nullptr;
public:
    // For tests: the PCM a track with no file plays.
    const int16_t *placeholder() const { return placeholder_; }
    uint32_t placeholder_frames() const { return placeholder_frames_; }
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
        VaMono wave_mono;   // WMN: VA MONO with the wavetable (set_wave)
        VaPoly wave_poly;   // WPL: VA POLY with it, in polys_ like the other
        FmPoly fm;          // FM4: in polys_ too
        Sampler sampler;
        Granular grain;
        MidiMachine midi;   // no state at all: the vtable pointer
    };
    TrackMachines pool_[kTracks];
    // The WAVE pair's tables: kWaveSlots slots (1.25 MiB) reserved in init(), a table built into
    // one when a track first sounds it (step_waves), shared by every track (dsp/wavetable.hpp).
    WaveLibrary waves_;

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
    // One stream's next bytes into `fifo`, converted on the way: top_up_streams' read, shared
    // with the audition's. `carry` and `carry_bytes` keep a frame split across two reads.
    void fill_fifo(enaga::files::Stream &stream, StreamFifo &fifo, uint32_t total,
                   uint8_t frame_bytes, uint8_t encoding, uint8_t &carry, uint8_t *carry_bytes,
                   uint32_t most);
    // The audition reads at most 256 frames a block: twice what a 96 kHz file uses, so its fifo
    // still fills ahead, at half a track stream's peak cost (docs/BUDGET.md, "Sample audition").
    static constexpr uint32_t kAuditionReadFrames = 256;

    // Sample audition. Audio side only, apart from the inbox and the stream (rules above).
    void top_up_audition();           // before rendering
    void end_audition();              // stop reading, then bump Status::audition_epoch
    Audition audition_;
    AuditionInfo audition_inbox_{};
    enaga::files::Stream audition_stream_;
    bool audition_on_ = false;        // reading audition_stream_ into audition_.fifo()
    uint8_t audition_encoding_ = 0;
    uint8_t audition_frame_bytes_ = 0;
    uint8_t audition_carry_ = 0;
    uint8_t audition_carry_bytes_[8] = {};
    uint8_t audition_epoch_ = 0;      // what Status::audition_epoch holds

    // JAM REC. Audio side only, apart from the writer (rules above).
    enaga::wav::Writer jam_;
    bool jam_on_ = false;             // writing every block into jam_
    uint8_t jam_epoch_ = 0;           // what Status::jam_epoch holds

    // The hat choke is a pair, and the pair is tracks 3 and 4 of the DEFAULT kit. Machine
    // switching makes that an assumption rather than a fact, so it is re-derived on every
    // change: a closed hat chokes an open one only while both tracks still host a Hat.
    void rewire_machines();
    // Cmd::set_machine's work: silence, switch, load the machine's default sound.
    void switch_machine(int track, uint8_t id);

    Machine *machines_[kTracks] = {};

    uint8_t machine_id_[kTracks] = {};
    uint8_t sent_note_[kTracks] = {};  // see sent_note()

    // And which tracks host a VaPoly, for the same reason and recorded the same way: a chord is
    // several notes on one frame, so `fire` has to know that this track can take them. A track
    // with no VaPoly plays the step's note and ignores its chord byte, which is what every
    // machine but this one does with it.
    PolyMachine *polys_[kTracks] = {};

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
    // Track Launch (core/launch.hpp): each lane's queued launch, and where each lane's part
    // started. A lane whose bit is set in `lane_follows_` counts from the pattern's `origin_`, as
    // every lane did before; a launched lane, or a pinned one kept through a pattern change,
    // counts from its own `lane_origin_`.
    LaneQueue lane_queue_[kTracks];
    uint32_t lane_origin_[kTracks] = {};
    uint8_t lane_follows_ = 0xFF;
    // Song mode: the song has played to its end and the sequencer is silent until the transport
    // stops or starts again (`cross_boundary`).
    bool song_over_ = false;
    void chain_entry_started();
    // A section's MRP RIDE / BACK (chain_entry_started): the morph from `ride_from_` to
    // `ride_to_` over `ride_length_` ticks from `ride_start_`, `ride_rate_` a tick in 1/65536.
    void morph_ride_start(uint16_t to, uint32_t length);
    void morph_ride_stop();
    void morph_ride_tick(uint32_t tick);
    bool ride_on_ = false;
    // ARR REC (core/arrrec.hpp): the take, the chain it replaced, and what resolve_tick watches
    // to see a pass begin (a boundary, a pattern switch, the pattern's start moved).
    ArrRec arr_;
    ArrBackup arr_backup_;
    bool arr_undone_ = false;        // the backup holds the take, the chain the one before it
    bool arr_switched_ = false;      // switch_pattern ran since the last tick
    uint32_t arr_origin_ = 0;        // origin_ as resolve_tick last saw it
    ArrState arr_now() const;
    void arr_tick(uint32_t tick, uint32_t previous, bool new_pass, bool jumped_back);
    void arr_end(uint32_t tick, bool full);
    void arr_publish();
    uint16_t ride_from_ = 0;
    uint16_t ride_to_ = 0;
    uint32_t ride_start_ = 0;
    uint32_t ride_length_ = 1;
    int32_t ride_rate_ = 0;

    // See `kit_for_recall`. Written by the main context, read by `Cmd::kit_recall` only.
    Kit kit_inbox_;

    // The kit: a track's parameters as stored, with no locks applied.
    TrackParams params_[kTracks] = {};

    // `Cmd::sound_preset`'s memory: one track's sound as it was when the SOUNDS list opened, so
    // its AS WAS row can put it back. -1 when nothing is remembered.
    TrackParams sound_back_ = {};
    int sound_back_track_ = -1;
    uint8_t sound_back_machine_ = 0;   // and its machine, which a paste may have switched
    // And `Cmd::arp_preset`'s: the ARP bytes and the arp's kit settings when ARPS opened.
    uint8_t arp_back_[kArpBytes] = {};
    KitArp arp_back_ext_ = {};
    int arp_back_track_ = -1;

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
    // layer skips them, so a lock always wins over a morph -- which is what docs/archive/REVIEW.md's
    // "base kit values only" means from the other side. Set by `fire` and therefore lasting
    // exactly as long as the step does.
    // 64 bits (format decision C, 2026-11-26): room for a parameter page past the 32 bytes a
    // track has now without touching every user of the mask again.
    uint64_t locked_[kTracks] = {};
    static_assert(kTrackParams <= 64, "locked_ holds one bit per parameter");

    // Parameter slide (core/pattern.hpp, kLockSlide): a slid lock's parameter gliding from the
    // step's value to what the next step that plays holds, over the time until that step.
    // Started by `fire` on a step's first hit, moved once a block by `run_slides`, ended by the
    // next step's `fire` (which rebuilds the track) or when the glide arrives. The parameter is
    // in `locked_` for as long as its step plays, so the per-block copy leaves the glide alone.
    struct ParamSlide {
        uint8_t param = 0;
        uint8_t from = 0;
        uint8_t to = 0;
        float position = 0.0f;   // 0 .. 1 of the way
        float step = 0.0f;       // added per frame: 1 / the glide's frames, at control rate
    };
    ParamSlide slides_[kTracks][kLocksPerStep] = {};
    uint8_t slide_count_[kTracks] = {};
    void run_slides(uint32_t frames);
    void clear_slides() {
        for (int t = 0; t < kTracks; t++) slide_count_[t] = 0;
    }
    // The slides a step starts (fire, on its first hit): `event` carries where they glide to.
    void start_slides(int track, const StepEvent &event);

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
    uint8_t lfo_shown_[kTracks] = {};
    // LFO TRIG: a note started on the track since the last run_lfos (note_started).
    bool lfo_restart_[kTracks] = {};
    // The track's own LFO is on (DEP and DST): `modulating_` is also set by a mod slot alone.
    bool lfo_running_[kTracks] = {};

    // The mod slots (core/modslot.hpp, KitTrack::mod), applied in run_lfos after the LFO. Per
    // track: the last note's velocity (VEL), the pad's pressure (PRS, Cmd::set_pressure), the
    // note's random value (RND, drawn when a note starts), ENV's own one-shot cycle -- the LFO's
    // shape and RAT, run from each note whatever TRIG says -- and each slot's value this block.
    void run_mod_slots(int track, const LfoSettings &lfo, float lfo_value, float cycles,
                       bool restart, bool va_filter, bool insert_chain, float &pitch_mod);
    uint8_t mod_velocity_[kTracks] = {};
    float mod_pressure_[kTracks] = {};
    float mod_random_[kTracks] = {};
    TrackLfo mod_env_[kTracks];
    enaga::dsp::Random mod_dice_[kTracks];
    float mod_value_[kTracks][kModSlots] = {};
    // The LFO on TUN as a float, and the machine it was last handed to: a track that changes
    // machine hands the new one the current value, so none keeps a stale offset.
    float pitch_mod_[kTracks] = {};
    Machine *pitch_mod_machine_[kTracks] = {};
    // The tempo as each track's machine last heard it (Machine::set_tick_frames).
    float tick_frames_sent_[kTracks] = {};
    Machine *tick_frames_machine_[kTracks] = {};
    // What the machine was last handed: the LFO's semitones plus VariSpeed's.
    float sent_pitch_[kTracks] = {};
    // VariSpeed (Cmd::set_varispeed): the amount, its semitones for the machines, and the ratio
    // the delay's beat is divided by so its echoes keep to the faster or slower clock.
    int8_t vari_tenths_ = 0;
    float vari_semis_ = 0.0f;
    float vari_inv_ratio_ = 1.0f;

    // Every note-on a track's machine takes -- a step, a chord, an arp or roll hit, a pad or a
    // key -- goes through here: the screen's trig light, and LFO TRIG's restart.
    // `lit` false (an ENERGY ghost) keeps the trig light off and does the rest.
    void note_started(int track, uint8_t velocity, uint32_t frame, bool lit = true) {
        if (lit) status_.bump_triggered(track);
        lfo_restart_[track] = true;
        mod_velocity_[track] = velocity;
        choke_group(track, frame);
    }

    // --- the metronome and the count-in (dsp/click.hpp, CyDrums S6) ---
    //
    // The click is fired from the clock's own ticks, on every beat (96 ticks) and higher on a
    // bar's first (384), so it lands on the beat's frame as a step does. A count-in is the same
    // click with the rest of the engine held back: the clock starts at once (Enaga's transport is
    // the user's, and starting it later from the UI would be late by up to a frame), and the first
    // count_in_end_ ticks only click. After them every tick reaches the sequencer, the rolls, the
    // stutter and the arps less count_in_end_, so the pattern starts on step 1 exactly as from a
    // plain START. A STOP or a restart ends it. With CLOCK OUT on, an outboard box hears START at
    // the count-in's beginning: it runs the count-in's bars ahead (docs/CONTROLS.md says so).
    void metronome_tick(uint32_t tick, uint32_t frame);
    Click click_;
    bool click_on_ = false;
    uint32_t count_in_end_ = 0;     // ticks: the count-in's length, 0 for none
    bool was_running_ = false;      // the transport at the last block, to see a STOP

    // --- choke groups (KitTrack::choke, CyDrums S4) ---
    //
    // A note on a track in a group cuts every other track of that group: a 64-frame fade from the
    // note's own frame over the cut track's block (strata_choke_fade, after its machine renders),
    // then kill_all. It is a fade over the TRACK's output, not a voice's, so every machine
    // chokes alike, and the hat's own closed-to-open pair (rewire_machines) stays as it was.
    //
    // A block is rendered whole, so a track cannot be faded before a frame and not after it. Two
    // rules follow, both in choke_group: a note on the cut track at or after the choke's frame,
    // in the same block, keeps the track sounding (no fade; the new note retriggers the machine
    // anyway), and two tracks of a group starting on the same frame both sound. A choke still
    // fading when its track starts a note again is over at once.
    static constexpr uint32_t kNoChokeFrame = 0xFFFFFFFFu;
    static constexpr int kChokeFadeFrames = 64;
    void choke_group(int track, uint32_t frame) {
        // This track's own note ends any choke on it that lands at or before the note.
        if (choke_at_[track] != kNoChokeFrame && frame >= choke_at_[track]) {
            choke_at_[track] = kNoChokeFrame;
            choke_gain_[track] = 1.0f;
        }
        if (note_frame_[track] == kNoChokeFrame || frame > note_frame_[track]) {
            note_frame_[track] = frame;
        }
        const uint8_t group = project_.kit.tracks[track].choke;
        if (group == 0) return;
        for (int u = 0; u < kTracks; u++) {
            if (u == track || project_.kit.tracks[u].choke != group) continue;
            // Its own note at or after this one, in this block: it plays (above).
            if (note_frame_[u] != kNoChokeFrame && note_frame_[u] >= frame) continue;
            // A fade already running from an earlier block (0) or landing sooner stands.
            if (choke_at_[u] != kNoChokeFrame && choke_at_[u] <= frame) continue;
            choke_at_[u] = frame;
        }
    }
    // The fade over track `t`'s rendered block, and the cut once it reaches 0 (engine.cpp); true
    // when the cut is done.
    // `right`: a stereo track's other side (WID), faded alike; nullptr for a mono track.
    bool run_choke(int t, float *mono, uint32_t frames, float *right = nullptr);
    // Each POLY track's machine takes its kit's WID (core/width.hpp).
    void sync_widths();
    uint32_t choke_at_[kTracks] = {kNoChokeFrame, kNoChokeFrame, kNoChokeFrame, kNoChokeFrame,
                                   kNoChokeFrame, kNoChokeFrame, kNoChokeFrame, kNoChokeFrame};
    float choke_gain_[kTracks] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // The latest frame each track started a note on in this block; kNoChokeFrame for none.
    uint32_t note_frame_[kTracks] = {kNoChokeFrame, kNoChokeFrame, kNoChokeFrame, kNoChokeFrame,
                                     kNoChokeFrame, kNoChokeFrame, kNoChokeFrame, kNoChokeFrame};

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

    // The CPU guard (core/cpuguard.hpp). Audio side only. Each block feeds it the previous
    // block's cost; a level change resizes the POLY budget and every GRAIN pool, and
    // shed_poly_voices() chokes one voice a block while more are counted than the budget allows.
    void run_cpu_guard();
    void apply_cpu_guard(int level);
    void shed_poly_voices();
    CpuGuard cpu_guard_;
    int guard_applied_ = 0;
    int poly_budget_ = kPolyVoiceBudget;

    void prepare_machine(int track);
    void prepare_chain(int track);
    // The line input's strip (ProjectSettings::input), between the tracks and the sends: fades in
    // when turned on, plays one block down to silence when turned off, and costs nothing while
    // off. `in` is audio()'s input buffer, nullptr where the host gives none.
    void run_input(const float *in, uint32_t frames);
    void prepare_input(bool on);
    bool input_live_ = false;                    // the chain is running (on, or its last block)
    uint8_t input_prepared_[kInputFields] = {};
    float input_prepared_scale_ = -1.0f;         // the MUTE BUS's fader scale it was prepared at
    bool input_prepared_on_ = false;
    void forget_prepared();
    TrackParams machine_prepared_[kTracks] = {};
    const Machine *machine_prepared_for_[kTracks] = {};
    TrackParams chain_prepared_[kTracks] = {};
    ChainMix chain_prepared_mix_[kTracks] = {};
    bool chain_prepared_valid_[kTracks] = {};
    bool chain_prepared_tone_[kTracks] = {};     // prepared as TON (D8): the machine's own filter
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
    RangeClip double_clip_;        // pattern_double's scratch, so the user's clipboard survives

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
    // REL ALL and LEN ALL (Cmd::set_move_all). REL ALL goes on in apply_performance and on a
    // step's locked release byte in fire(); LEN ALL on every sequenced gate in fire().
    int8_t rel_all_ = 0;
    int8_t len_all_ = 0;

    MuteLatch mute_latch_;

    // The FILL pad, as against the fill the auto-fill bar produces. Kept apart so that holding
    // the pad during an auto-fill bar is not a contradiction and releasing it does not cancel
    // the bar's own fill.
    bool fill_held_ = false;
    AutoFill auto_fill_ = AutoFill::off;

    // EVOLVE (core/evolve.hpp): one per track, each with its HOME and its last turn -- about
    // 2.2 KiB a track, here so nothing is allocated. The mutation, its amount and ORBIT or DRIFT
    // are shared, and `evolve_variant_` counts turns so each one differs.
    Evolve evolve_[kTracks];
    EvolveMode evolve_mode_ = EvolveMode::orbit;
    Mutation evolve_kind_ = Mutation::velocity;
    uint8_t evolve_amount_ = 50;
    uint32_t evolve_variant_ = 1;
    void evolve_passes();            // cross_boundary: a pass has ended
    void evolve_lead();              // the last ticks before it: one turn a tick
    static constexpr int kEvolveLeadTicks = kTracks;
    uint8_t evolve_turned_ = 0;      // tracks that turned in this pass's lead
    void evolve_stop_all();          // a replaced pattern: HOME is gone
    void evolve_to_settings();       // the project's saved copy, after every change
    void evolve_from_settings();     // a load: EVOLVE as the file says

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

    // The performance mixer (core/perffx.hpp). The effect picked and its amount, the plan they
    // make, and the looper's mix, which is kept apart: picking an effect leaves a loop playing.
    PerfFx perf_fx_ = PerfFx::none;
    uint8_t perf_amount_ = 0;
    PerfPlan perf_plan_{};

    // LOCK. The states (128 locks, the swap-back and one scratch for the "changed" compare)
    // are taken in init(), 1.06 MiB (tests/test_memory.cpp). The compare runs every
    // kLockCheckBlocks blocks while the playing pattern has a lock: a ~8.5 KiB capture and memcmp,
    // a few thousand cycles.
    void lock_command(uint8_t action);
    void capture_live(LiveState &state) const;
    void apply_live(const LiveState &state);
    void publish_lock_state();
    LockStore locks_;
    LiveState *lock_memory_ = nullptr;     // LockStore::kStates, then the scratch
    int lock_check_countdown_ = 0;
    bool lock_changed_ = false;
    int live_index() const { return int(project_.bank) * kPatternsPerBank + int(project_.slot); }
    uint8_t looper_mix_ = 0;
    MasterFx master_fx_;
    Looper looper_;
    // Where the pump and the gate are in the beat: the clock's while it runs, free from there
    // while it is stopped.
    float perf_phase_ = 0.0f;
    // The delay freeze last handed to the mixer: the mixer's ECHO FREEZE is its only owner.
    bool freeze_applied_ = false;
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

    // Step stutter (Cmd::step_stutter): which step, on which tracks, and the roll's own grid
    // state -- the tick of the press, and frames to the next hit while the transport is stopped.
    int stutter_step_ = -1;
    uint8_t stutter_tracks_ = 0;
    uint32_t stutter_last_tick_ = 0;
    float stutter_countdown_ = 0.0f;
    void stutter_fire(uint32_t frame);
    void stutter_tick(uint32_t tick, uint32_t frame);
    void stutter_free(uint32_t frames);

    // The arpeggiator (core/arp.hpp). One run per track for a sequenced step -- started by
    // `fire`, ending with the step's length or at the next step -- and one for held KEYS keys,
    // which runs while any is down. Both hit on the clock's ticks while it runs; the held one
    // counts frames while it is stopped, as a note repeat does.
    //
    // The kit's arp settings (KitTrack::arp) shape each hit: RHYTHM rests a slot without moving
    // the note on (`slot` counts the grid, `hit` the notes), ACCENT accents it, and RATCHET
    // repeats its note inside the slot. A ratchet's later hits are counted down in frames
    // (`ratchet_*`) by `arp_ratchets`, after the deferred step hits and before this block's
    // ticks, the same way whether the transport runs or not. They are spaced at least a block
    // apart (`arp_ratchet_hits`), which keeps the one-pending-trig contract `dispatch` relies on.
    struct ArpRun {
        bool on = false;
        ArpMode mode = ArpMode::off;
        ArpVar var = ArpVar::off;
        uint8_t length = 0;
        uint8_t octaves = 1;
        uint8_t rate = 24;           // ticks between hits
        uint8_t gate_percent = 50;
        uint8_t velocity = 100;
        bool accent = false;
        uint32_t hit = 0;            // notes played
        uint32_t slot = 0;           // grid slots passed, rests included
        uint32_t next_tick = 0;
        uint32_t end_tick = 0xFFFFFFFFu;
        uint32_t seed = 0;
        float countdown = 0.0f;      // frames to the next hit, transport stopped (held runs)
        uint8_t ratchet_left = 0;    // the slot's repeats still to come
        uint8_t ratchet_note = 60;
        uint32_t ratchet_gate = 1;   // frames
        float ratchet_countdown = 0.0f;   // frames from the next block's start to the next repeat
        float ratchet_spacing = 0.0f;
        uint8_t cycle[kArpMaxCycle] = {};
    };
    ArpRun seq_arp_[kTracks];
    ArpRun held_arp_[kTracks];
    uint8_t held_notes_[kTracks][kArpMaxInput] = {};
    uint8_t held_count_[kTracks] = {};
    // Set once `arp_ratchets` has run in this block, so a hit after it counts its ratchet from
    // the next block's start and a hit before it (a command, a deferred step) from this one's.
    bool arp_ratchets_done_ = false;
    uint32_t arp_runs_ = 0;      // runs started, for arp_new_seed
    uint32_t arp_new_seed(int track, int step);
    ArpSettings arp_settings(int track) const;
    void arp_hit(int track, ArpRun &run, uint32_t frame);
    void arp_play(int track, uint8_t note, uint8_t velocity, bool accent, uint32_t frame,
                  uint32_t gate_frames);
    void arp_ratchets(uint32_t frames);
    void arp_tick(uint32_t tick, uint32_t frame);
    void arp_free(uint32_t frames);
    void arp_held_changed(int track);
    void arp_follow(int track);
    // Spanning ratchets (core/pattern.hpp, kRatchetSpan): the scheduler hands over a roll's first
    // hit, and `span_start` (from `fire`) keeps the rest here, to be hit on the clock's ticks by
    // `span_tick` at the roll's rate until its count is done. Each hit goes through `fire` as the
    // step again, with ratchet_index 1 so it neither restarts the step's slides nor ends its own
    // roll; its velocity is the ramp's across the whole roll and its gate half the rate, as a note
    // repeat's. A trig on the track ends a roll (a new step's first hit), and so does anything
    // that drops the deferred ratchet hits (span_clear beside each). Under the arp, the arp takes
    // the step and there is no roll.
    struct SpanRun {
        bool on = false;
        uint8_t rate = 0;            // ticks between hits
        RatchetRamp ramp = RatchetRamp::none;
        uint8_t velocity = 100;      // the step's own, which the ramp scales
        uint16_t hit = 0;            // the next hit's index
        uint16_t count = 1;
        float inv_count = 1.0f;
        uint32_t start_tick = 0;
        uint32_t next_tick = 0;
        StepEvent base{};
    };
    SpanRun span_[kTracks];
    void span_start(int track, const StepEvent &event);
    void span_tick(uint32_t tick, uint32_t frame);
    void span_clear() {
        for (int t = 0; t < kTracks; t++) span_[t].on = false;
    }
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

    // The master meter's reading, audio context only (Status::meter_milli).
    static constexpr float kMeterFall = 0.99f;
    float meter_ = 0.0f;
    // Each track's meter (Status::track_milli), falling as the master's does.
    float track_meter_[8] = {};
    ScopeRing scope_;

    // The waveform's playheads and grains (Status::wave): every fourth block is 5.3 ms, four
    // times finer than the screen's 50 ms frame, and a quarter of the stores.
    static constexpr uint32_t kWaveEveryBlocks = 4;
    uint32_t wave_block_ = 0;

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
    // A bounce: the track the armed or running take records on its own (Mixer::add_track_tapped),
    // or -1 for the master or the input. The tap is cleared every block it is in use, so a muted
    // or idle track records silence rather than the previous block.
    int8_t capture_track_ = -1;
    float capture_tap_[Mixer::kMaxFrames] = {};
    // A freeze's take is the same tap taken BEFORE the insert chain (dsp/frozen.hpp), and the
    // bar it began on and the bar tick's frame in that block are where its loop is in time.
    bool capture_prechain_ = false;
    uint32_t capture_bar_ = 0;
    int32_t capture_lead_ = 0;       // the take frame of its first bar line (negative: before it)
    uint32_t tick_now_ = 0;          // the tick resolve_tick() is resolving, for on_bar()
    uint32_t tick_frame_ = 0;        // and its frame in the block

    // Freeze: per track, the loop it plays instead of its machine while frozen.
    FrozenLoop frozen_[kTracks];

    // Which track's trigs fire the sidechain duck: the kit's KitDuck::source, as apply_duck last
    // read it. docs/SPEC.md: track 1 by default.
    int duck_source_ = 0;

    Commands commands_;
    Status status_;

    float rate_ = 48000.0f;
    bool ready_ = false;
};

}  // namespace strata
