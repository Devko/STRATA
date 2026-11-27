#include "engine.hpp"

#include "arp.hpp"       // the arpeggiator's notes; the engine schedules them
#include "factory.hpp"
#include "liverand.hpp"    // LIVE RND: the unit held to its range
#include "chainloops.h"    // strata_choke_fade, a choke group's fade
#include "masterloops.h"   // strata_master_peak, for the input meter
#include "pcm.hpp"         // a streamed file's bytes to int16, in top_up_streams
#include "sounds.hpp"      // Cmd::sound_preset: the factory sounds
#include "arppresets.hpp"  // Cmd::arp_preset: the factory arps
#include "stepnotes.hpp"   // a step's played-in extra notes, as locks
#include "tuning.hpp"      // a knob click in NOTE view: tune_step

#include <enaga/app.hpp>
#include <enaga/clock.hpp>
#include <enaga/dsp.hpp>
#include <enaga/midi.hpp>

namespace strata {

namespace {

// How many of a step's locks can change a parameter before a trig. Locks are applied to the
// track's working parameters just before the machine is prepared, then reverted, so a lock never
// leaks into the next step.
constexpr int kMaxLocksApplied = kLocksPerStep;

// Middle C. The pattern's root is a pitch class (core/pattern.hpp), so turning it into a note
// needs an octave, and this is the one the key command uses.
constexpr uint8_t kDefaultOctaveNote = 60;
// The mixer page's fader ceiling, which an encoder delta stops at (Cmd::set_track_level).
constexpr float kFaderDeltaMax = 1.5f;
// MIDI out's gate for a note with none of its own: core/timing.hpp's index 8, one step.
constexpr uint8_t kMidiDefaultLength = 8;

// A MIDI data byte, 0-127. (Not core/clamp.hpp's clamp_u8, which takes its limits.)
uint8_t clamp_7bit(int value) {
    return uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

// The notes a step plays, `notes[0..count)`, with its extra notes (core/stepnotes.hpp) merged
// in: ascending, none doubled, at most `max`. Most steps have none, and then this is one walk
// of the step's lock run.
int with_extra_notes(const Pattern &pattern, int track, int step, uint8_t *notes, int count,
                     uint8_t key, int scale, bool fold, int max) {
    uint8_t extras[kStepExtraNotes];
    const int extra = step_extra_notes(pattern, track, step, extras);
    if (extra == 0) return count;
    return notes_merge(notes, count, extras, extra, key, scale, fold, max);
}

// A level for Status in thousandths, saturating at what a uint16_t holds (the meters' unit).
uint16_t to_milli(float level) {
    const float scaled = level * 1000.0f;
    return uint16_t(scaled > 65535.0f ? 65535.0f : scaled);
}

}  // namespace

// The kit on disk and the kit the machines read have to agree about how many parameters a track
// has. core/project.hpp cannot say so itself: core/ does not depend on dsp/ (core/pattern.hpp
// explains why), so the two constants are declared apart and checked here, in the one file that
// includes both.
// core/factory.cpp writes the kit by index, because core/ cannot see dsp/track.hpp's pages.
static_assert(kFactorySrc == page_first(Page::src) && kFactoryFltr == page_first(Page::fltr) &&
                  kFactoryAmp == page_first(Page::amp) && kFactoryFx == page_first(Page::fx) &&
                  kFactoryAsr == page_first(Page::asr),
              "core/factory.hpp's page layout and dsp/track.hpp's have diverged");
static_assert(kSoundAsrFirst == kAsrFirst, "core/sounds.hpp's ASR page and dsp/track.hpp's have diverged");
static_assert(kSoundOscFirst == kOscFirst && kFactoryOsc == kOscFirst,
              "core/'s OSC page and dsp/track.hpp's have diverged");

static_assert(kLfoTrigFree == uint8_t(LfoTrig::free) && kLfoTrigRetrig == uint8_t(LfoTrig::retrig) &&
                  kLfoTrigOneShot == uint8_t(LfoTrig::one_shot) &&
                  kLfoTrigs == uint8_t(LfoTrig::count),
              "core/project.hpp's LFO TRIG values and dsp/lfo.hpp's have diverged");
static_assert(kKitParams == kTrackParams && kKitSoundParams == kParams &&
                  kKitLfoParams == kLfoParams && kKitArpParams == kArpParams &&
                  kKitLfoEnd == kArpFirst && kArpBytes == kArpParams &&
                  kKitAsrParams == kAsrParams && kKitArpEnd == kAsrFirst &&
                  kKitOscParams == kOscParams && kKitAsrEnd == kOscFirst,
              "core/project.hpp's kit record and dsp/track.hpp's TrackParams have diverged; a "
              "saved project would come back with the wrong number of parameters per track");

bool Engine::init(float sample_rate) {
    rate_ = sample_rate;

    // Every allocation in the signal path happens here and nowhere else. The machines are
    // initialised below, where the pool is: every track owns one of every machine.
    mixer_.init(rate_);
    click_.init(rate_);

    // Beat repeat's 2-bar ring, 2.93 MiB, here with every other buffer in the signal path.
    if (!repeat_.init(rate_)) return false;
    if (!tape_stop_.init(rate_)) return false;
    if (!capture_.init(rate_)) return false;
    // The looper records into RESAMPLE's buffer (dsp/looper.hpp): no RAM of its own.
    looper_.attach(capture_.lend(), capture_.capacity());
    master_fx_.init(rate_);
    // LOCK's states: one per pattern, the swap-back and the compare's scratch.
    if (lock_memory_ == nullptr) lock_memory_ = new LiveState[LockStore::kStates + 1];
    locks_.attach(lock_memory_);

    perform_init(scenes_, macros_);

    // Sample memory is NOT taken here. The region and the SamplePool that carves it up belong
    // to the main context: App::init allocates the region and hands the base over with
    // set_sample_region before the first audio block, which keeps every byte of pool state on
    // one side of the ring (app/engine.hpp's `sample_region_`). It is still allocated once, at
    // init, as CLAUDE.md requires -- just one level up.

    build_placeholder_sample();

    // The WAVE pair's tables (dsp/wavetable.hpp): built here, from their recipes, rather than
    // read from the card, so they need no file and cannot be missing. If the 1.25 MiB of slots
    // is not there the WAVE machines play their sub and noise alone, which is recoverable;
    // refusing to boot over one machine would not be.
    // Only the slots here: a table is built when a WAVE track first sounds it, a piece at a
    // time from App::update (step_waves). Building all sixteen at boot outlasted Enaga's 1 s
    // watchdog on the C6748, and cost every project 2.5 MiB whether it used WAVE or not.
    if (waves_.reserve(kWaveSlots)) set_wave_library(&waves_);

    // The default techno kit of docs/SPEC.md. Every slot is filled now that VA POLY exists;
    // GRAIN has no track in the default kit and arrives with machine switching. An empty slot
    // would be silent, not a crash.
    // Every machine of every track, once, here -- so a kit change later is a pointer swap and
    // not an allocation (CLAUDE.md: everything is taken in init()).
    for (int t = 0; t < kTracks; t++) {
        pool_[t].kick.init(rate_);
        pool_[t].snare.init(rate_);
        pool_[t].hat.init(rate_);
        pool_[t].perc.init(rate_);
        pool_[t].mono.init(rate_);
        pool_[t].poly.init(rate_);
        pool_[t].wave_mono.set_wave(true);
        pool_[t].wave_mono.init(rate_);
        pool_[t].wave_poly.set_wave(true);
        pool_[t].wave_poly.init(rate_);
        pool_[t].fm.init(rate_);
        pool_[t].sampler.init(rate_);
        pool_[t].grain.init(rate_);
        pool_[t].midi.init(rate_);
        // A seed per track, so eight S&H LFOs do not move together. Fixed, so a render is
        // repeatable.
        lfo_[t].init(0x9E3779B9u * uint32_t(t + 1));
        // ENV and RND, seeded apart from the LFO so a slot's S+H shape and RND do not follow it.
        mod_env_[t].init(0x85EBCA6Bu * uint32_t(t + 1));
        mod_dice_[t] = enaga::dsp::Random(0xC2B2AE35u * uint32_t(t + 1));
    }
    inv_rate_ = enaga::dsp::reciprocal(rate_);
    midi_out_.clear();

    // The sidechain duck comes from the kit (core/project.hpp, KitDuck), by apply_duck in
    // params_from_kit below. Its factory values are the ones it was fixed at in v0.1.0.

    // The factory project (core/factory.hpp): the same kit, mixer, patterns, scenes and macros
    // a NEW project gets, so the instrument with no card and a new project are one thing.
    // params_from_kit assigns the machines, rewires them and copies the kit into `params_` and
    // `effective_`; then the sends and the master from the project's bytes, so `sends_` does not
    // hold dsp/buses.hpp's struct defaults until the first knob move.
    factory_project(project_, scenes_, macros_);
    params_from_kit();
    apply_mixer_bytes();
    apply_performance();

    scheduler_.configure(live());
    scheduler_.reset(true);

    // Status's view of what each track sounds, before the first block: step_waves reads it, and
    // an all-zero Status would ask every track for table 0.
    publish_heard();

    ready_ = true;
    return true;
}

// Main context, from App::update (engine.hpp).
bool Engine::step_waves() {
    // The tables the WAVE tracks are sounding now: the kit's TBL with the scene, macro, lock and
    // LFO over it, as the machines are prepared from. A lock or an LFO on TBL asks for each
    // table it reaches while it is there; a table no longer asked for stays in its slot until
    // the slot is needed. From Status (publish_heard), never from `machine_id_` or `sounding`:
    // those are the audio side's, rewritten every block.
    uint32_t wanted = 0;
    for (int t = 0; t < kTracks; t++) {
        const uint8_t table = status_.heard_table[t].load(std::memory_order_relaxed);
        if (table != Status::kNotHeard) wanted |= 1u << (table & (kWaveTables - 1));
    }
    waves_.want(wanted);
    return waves_.step();
}

// Audio context, every block (process), and once from init.
void Engine::publish_heard() {
    // Some 32 relaxed byte stores: the UI wants the newest, and each byte stands alone.
    for (int t = 0; t < kTracks; t++) {
        const TrackParams &heard = sounding(t);
        const bool wave = machine_is_wave(machine_id_[t]);
        status_.heard_table[t].store(wave ? uint8_t(va_wave_table(heard.get(0))) : Status::kNotHeard,
                                     std::memory_order_relaxed);
        status_.heard_pos[t].store(heard.get(1), std::memory_order_relaxed);
        status_.heard_sus[t].store(heard.v[kAsrFirst + 1], std::memory_order_relaxed);
        for (int k = 0; k < 3; k++) {
            status_.heard_filter[t][k].store(heard.get(5 + k), std::memory_order_relaxed);
        }
        status_.lfo_shown[t].store(lfo_running_[t] ? lfo_shown_[t] : Status::kNotHeard,
                                   std::memory_order_relaxed);
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = project_.kit.tracks[t].mod[s];
            status_.mod_heard[t][s].store(slot.on() ? heard.v[slot.dest] : Status::kNotHeard,
                                          std::memory_order_relaxed);
        }
    }
}

// Main context, App::init only. Nothing is read through `sample_region_` until a `Cmd::set_sample`
// arrives, and that command cannot arrive before App::init has returned, so this needs no
// synchronisation with the audio side.
void Engine::set_sample_region(const int16_t *base, uint32_t bytes) {
    sample_region_ = base;
    sample_region_bytes_ = bytes;
}

void Engine::rewire_machines() {
    for (int t = 0; t < kTracks; t++) {
        TrackMachines &own = pool_[t];
        polys_[t] = nullptr;

        switch (MachineId(machine_id_[t])) {
            case MachineId::kick:    machines_[t] = &own.kick; break;
            case MachineId::snare:   machines_[t] = &own.snare; break;
            case MachineId::hat:     machines_[t] = &own.hat; break;
            case MachineId::perc:    machines_[t] = &own.perc; break;
            case MachineId::sample:  machines_[t] = &own.sampler; break;
            case MachineId::grain:   machines_[t] = &own.grain; break;
            case MachineId::va_mono: machines_[t] = &own.mono; break;
            case MachineId::midi:    machines_[t] = &own.midi; break;
            case MachineId::va_poly:
                machines_[t] = &own.poly;
                polys_[t] = &own.poly;
                break;
            case MachineId::wave_mono: machines_[t] = &own.wave_mono; break;
            case MachineId::wave_poly:
                machines_[t] = &own.wave_poly;
                polys_[t] = &own.wave_poly;
                break;
            case MachineId::fm:
                machines_[t] = &own.fm;
                polys_[t] = &own.fm;
                break;
            default:
                // Defensive only: kit_read and params_from_kit both map an id this build does
                // not have to KICK (core/project.cpp says why), so no kit reaches this. If one
                // ever did, a silent track is recoverable where a reinterpreted index is not.
                machines_[t] = nullptr;
                break;
        }
    }

    // The hat choke, re-derived rather than wired once. docs/SPEC.md's default kit puts a closed
    // hat on track 3 and an open one on track 4, and a closed hat cuts an open one -- but that is
    // a fact about the DEFAULT KIT, not about the tracks, and machine switching can take either
    // of them away. So the pair is only joined while both tracks still host a Hat, and every Hat
    // in the pool has its target cleared first.
    for (int t = 0; t < kTracks; t++) pool_[t].hat.set_choke_target(nullptr);
    if (MachineId(machine_id_[2]) == MachineId::hat &&
        MachineId(machine_id_[3]) == MachineId::hat) {
        pool_[2].hat.set_choke_target(&pool_[3].hat);
    }
}

// A sliced test loop, so track 8 has something to play before M4 can load a WAV from the card.
//
// Eight slices of a short decaying blip, each a fourth apart, which makes the slice feature
// audible and gives the 8-track load measurement real work to do. A static buffer, not an
// allocation: the platform's rule is that large buffers go in statics or on the heap, and the
// Sampler never copies what it is handed.
void Engine::build_placeholder_sample() {
    static constexpr int kSlices = 8;
    static constexpr int kSliceFrames = 2400;                  // 50 ms at 48 kHz
    static constexpr int kTotalFrames = kSlices * kSliceFrames;
    static int16_t pcm[kTotalFrames];

    for (int slice = 0; slice < kSlices; slice++) {
        // A fourth per slice, from 220 Hz up: distinguishable without being musical clutter.
        const float hz = 220.0f * enaga::dsp::semitones(float(slice) * 5.0f);
        const float dt = hz / rate_;
        float phase = 0.0f;

        for (int i = 0; i < kSliceFrames; i++) {
            // An exponential decay over the slice, computed without libm.
            const float t = float(i) * (1.0f / float(kSliceFrames));
            const float gain = enaga::dsp::exp2(-7.0f * t);
            const float s = enaga::dsp::sin_turns(phase) * gain * 0.7f;
            phase += dt;
            if (phase >= 1.0f) phase -= 1.0f;
            pcm[slice * kSliceFrames + i] = int16_t(s * 32000.0f);
        }
    }

    // Into EVERY track's Sampler and Granular, not just the one the default kit uses. A track
    // switched to SAMPLE or GRAIN before any card has been read should make a sound rather than
    // silence, and this is the only PCM the app owns before a load: eight pointers to one static
    // array, which costs nothing.
    placeholder_ = pcm;
    placeholder_frames_ = uint32_t(kTotalFrames);
    for (int t = 0; t < kTracks; t++) {
        pool_[t].sampler.set_sample(placeholder_, placeholder_frames_, 1, rate_);
        pool_[t].grain.set_sample(placeholder_, placeholder_frames_, 1, rate_);
    }
}

// --- commands -------------------------------------------------------------

void Engine::apply_commands() {
    Command command;
    // The spec's limit: a burst of edits must not be able to break the deadline.
    for (uint32_t i = 0; i < Commands::kMaxPerBlock; i++) {
        if (!commands_.take(command)) break;
        // Before apply(), not after: some commands prepare a machine themselves (a machine
        // switch), and that prepare must not be mistaken for one made with today's inputs.
        // Any command at all, because the inputs a prepare() reads beyond its parameters -- the
        // resident sample, which machine the track hosts, the kit -- change only here.
        forget_prepared();
        apply(command);
    }
}

// --- prepare only on change -------------------------------------------------------------------
//
// Every sounding track's machine and insert chain used to be prepared every block, and a trig
// prepared its machine once more. Each prepare() is a handful of exp2, exp, tan_pi and float
// divisions (EnvelopeShape::make alone is three of each), all scalar GCC code in either build,
// because only *.ti.c files go to cl6x. Parameters move only on a knob, a lock, a scene or a
// macro, so most blocks were recomputing the numbers they already had. docs/BUDGET.md has what
// it was worth.
//
// Bit-exact by construction: a prepare() is a pure function of its arguments and of state only
// a command changes, and the memo holds the arguments and is cleared by every command.

// --- the shared voice budget ------------------------------------------------------------------

// Before a VA POLY trig on `track`: true if it may go ahead. See kPolyVoiceBudget (engine.hpp).
bool Engine::make_room_for_poly_voice(int track, uint8_t note) {
    // A frozen track's machine plays nothing: it is the loop that sounds (dsp/frozen.hpp). Every
    // note to a machine asks here first, so this is the one place that holds it back.
    if (frozen_[track].on()) return false;
    PolyMachine *target = polys_[track];
    if (target == nullptr || !target->would_add_voice(note)) return true;

    int used = 0;
    for (int t = 0; t < kTracks; t++) {
        if (polys_[t] != nullptr) used += polys_[t]->budget_voices();
    }
    if (used < poly_budget_) return true;

    // The quietest started voice anywhere, this track included. Ties go to the lowest track,
    // so the choice is deterministic.
    int pick_track = -1;
    int pick_slot = -1;
    float quietest = 0.0f;
    for (int t = 0; t < kTracks; t++) {
        if (polys_[t] == nullptr) continue;
        float level = 0.0f;
        const int slot = polys_[t]->quietest_voice(level);
        if (slot >= 0 && (pick_track < 0 || level < quietest)) {
            pick_track = t;
            pick_slot = slot;
            quietest = level;
        }
    }
    if (pick_track < 0) return false;   // everything counted is still waiting: drop this note
    polys_[pick_track]->choke(pick_slot);
    return true;
}

// --- the CPU guard ----------------------------------------------------------------------------

void Engine::run_cpu_guard() {
    // The PREVIOUS block's cost, per mille. Enaga's own unit check app reads it from audio() the
    // same way; core/cpuguard.hpp says why it averages rather than reacting to one block.
    const int level = cpu_guard_.update(enaga::audio_load());
    if (level != guard_applied_) apply_cpu_guard(level);
    shed_poly_voices();
}

void Engine::apply_cpu_guard(int level) {
    guard_applied_ = level;
    poly_budget_ = CpuGuard::poly_budget(level, kPolyVoiceBudget);
    const int grains = CpuGuard::grain_limit(level, Granular::kPoolSize);
    for (int t = 0; t < kTracks; t++) pool_[t].grain.set_grain_limit(grains);
    status_.cpu_guard.store(uint8_t(level), std::memory_order_relaxed);
}

// While more POLY voices are counted than the budget allows -- the guard has just lowered it --
// choke the quietest started one: the same 64-frame fade a new note past the budget uses, so a
// shed voice is the voice least likely to be heard. One a block, so even a drop of several voices
// spreads its fades over a few milliseconds rather than landing in one.
void Engine::shed_poly_voices() {
    if (poly_budget_ >= kPolyVoiceBudget) return;
    int used = 0;
    for (int t = 0; t < kTracks; t++) {
        if (polys_[t] != nullptr) used += polys_[t]->budget_voices();
    }
    if (used <= poly_budget_) return;
    int pick_track = -1;
    int pick_slot = -1;
    float quietest = 0.0f;
    for (int t = 0; t < kTracks; t++) {
        if (polys_[t] == nullptr) continue;
        float level = 0.0f;
        const int slot = polys_[t]->quietest_voice(level);
        if (slot >= 0 && (pick_track < 0 || level < quietest)) {
            pick_track = t;
            pick_slot = slot;
            quietest = level;
        }
    }
    if (pick_track >= 0) polys_[pick_track]->choke(pick_slot);
}

void Engine::forget_prepared() {
    for (int t = 0; t < kTracks; t++) {
        machine_prepared_for_[t] = nullptr;
        chain_prepared_valid_[t] = false;
    }
}

void Engine::prepare_machine(int track) {
    Machine *machine = machines_[track];
    if (machine == nullptr) return;
    const TrackParams &params = sounding(track);
    // The sound's 16 and the ASR and OSC pages' 8: a VA reads all of them (dsp/track.hpp). The
    // LFO and arp bytes between them are not compared, so turning an LFO knob does not re-prepare
    // a machine.
    static_assert(kOscFirst == kAsrFirst + kAsrParams && kTrackParams == kOscFirst + kOscParams,
                  "ASR and OSC are the last eight bytes, compared as one run");
    if (prepare_memo_ && machine_prepared_for_[track] == machine &&
        __builtin_memcmp(machine_prepared_[track].v, params.v, kParams) == 0 &&
        __builtin_memcmp(machine_prepared_[track].v + kAsrFirst, params.v + kAsrFirst,
                         kAsrParams + kOscParams) == 0) {
        return;
    }
    machine->prepare(params);
    machine_prepared_[track] = params;
    machine_prepared_for_[track] = machine;
}

void Engine::prepare_chain(int track) {
    const TrackParams &params = sounding(track);
    // The mixer's MUTE BUS scales every fader (core/perffx.hpp); 1 leaves the fader as set, so
    // a dry plan prepares exactly what it did before.
    ChainMix mix = mix_[track];
    mix.level *= perf_plan_.tracks;
    // A VA or WAVE track's chain is TON alone (D8): its FLTR page is the machine's filter.
    const bool tone = machine_has_voice_filter(machine_id(track));
    if (prepare_memo_ && chain_prepared_valid_[track] && chain_prepared_tone_[track] == tone &&
        chain_prepared_mix_[track].level == mix.level &&
        chain_prepared_mix_[track].duck == mix.duck &&
        __builtin_memcmp(chain_prepared_[track].v, params.v, kParams) == 0) {
        return;
    }
    mixer_.chain(track).prepare(params, mix, tone);
    chain_prepared_[track] = params;
    chain_prepared_mix_[track] = mix;
    chain_prepared_valid_[track] = true;
    chain_prepared_tone_[track] = tone;
}

void Engine::apply(const Command &command) {
    // REFUSED, not clamped. These used to fall back to 0, so a command naming track 9 edited
    // track 1 and step 64 edited step 1 -- a UI bug would corrupt a pattern silently, where
    // every other out-of-range field in this function refuses and counts (queue_pattern,
    // set_sample, macro_assign, the param guards). A refusal the screen can show is recoverable;
    // an edit on the wrong step is someone's pattern.
    if (command.track >= kTracks || command.step >= kStepsPerTrack) {
        status_.bump_refused();
        return;
    }
    const int track = int(command.track);
    const int step = int(command.step);

    // Every command belongs to exactly one family below; the first that knows it applies
    // it. The order is only the likeliest first (an encoder sweep is set_param or a delta).
    // Cmd::none and an unknown value fall through all of them and do nothing, as before.
    const bool handled = apply_sound(command, track, step) ||
                         apply_step_edit(command, track, step) ||
                         apply_perform(command, track, step) ||
                         apply_notes(command, track, step) ||
                         apply_mixer(command, track, step) ||
                         apply_track_timing(command, track, step) ||
                         apply_bulk_edit(command, track, step) ||
                         apply_transport(command, track, step) ||
                         apply_arrangement(command, track, step) ||
                         apply_project(command, track, step) ||
                         apply_samples(command, track, step) ||
                         apply_capture(command, track, step);
    (void)handled;
}

// A track's sound: its parameters, presets, LFO TRIG, MOD slots, slices and machine.
bool Engine::apply_sound(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::set_param:
            if (command.a < kTrackParams) {
                uint8_t value = command.b;
                if (command.c == kSemitoneFlag && tune_param(machine_id_[track]) == command.a) {
                    value = tune_step(machine_id_[track], params_[track].v[command.a],
                                      int(int8_t(command.b)));
                } else if (command.c == kDeltaFlag || command.c == kSemitoneFlag) {
                    const int next = int(params_[track].v[command.a]) + int(int8_t(command.b));
                    value = uint8_t(next < 0 ? 0 : (next > 127 ? 127 : next));
                }
                params_[track].v[command.a] = clamp_7bit(value);
                // `performed_` is built FROM `params_`, and the per-block copy in process()
                // writes it over every unlocked parameter of `effective_`. So a kit change that
                // does not set this flag is reverted within the same block: `apply_performance`
                // returns early, `performed_` still holds the old value, and the copy puts it
                // back. Turning a knob in EDIT mode was inaudible until some unrelated
                // performance control happened to set the flag.
                perform_dirty_ = true;
                // Mirror into the effective set as well, so the turn is heard in THIS block even
                // before the copy runs. The next step rebuilds effective_ from the kit anyway,
                // so a lock on this parameter still wins from then on.
                effective_[track].v[command.a] = clamp_7bit(value);
            }
            break;

        case Cmd::sound_preset: {
            // core/sounds.hpp. Main context picks, the audio side writes, as for set_param --
            // into the kit's copy here, mirrored into effective_ so the sound changes in this
            // block, with perform_dirty_ so the per-block copy does not put the old one back.
            if (command.b == kSoundPresetKeep) {
                sound_back_ = params_[track];
                sound_back_track_ = track;
                break;
            }
            const uint8_t machine = machine_id_[track];
            uint8_t *bytes = params_[track].v;
            if (command.b == kSoundPresetBack) {
                if (sound_back_track_ != track) {
                    status_.bump_refused();
                    break;
                }
                const uint32_t mask = sound_preset_writes(machine);
                for (int p = 0; p < kTrackParams; p++) {
                    if ((mask >> p) & 1u) bytes[p] = sound_back_.v[p];
                }
            } else if (command.a != machine || !sound_preset_apply(machine, command.b, bytes)) {
                // Picked for a machine the track no longer hosts, or no such preset.
                status_.bump_refused();
                break;
            }
            const uint32_t locked = locked_[track];
            for (int p = 0; p < kTrackParams; p++) {
                // A lock on the current step still wins until the next step, as for a knob.
                if ((locked >> p & 1u) == 0) effective_[track].v[p] = bytes[p];
            }
            perform_dirty_ = true;
            break;
        }

        case Cmd::arp_preset: {
            // core/arppresets.hpp, as sound_preset: the four ARP bytes into the kit and, where no
            // lock holds them, effective_; the kit's arp settings beside them.
            uint8_t *bytes = &params_[track].v[kArpFirst];
            KitArp &ext = project_.kit.tracks[track].arp;
            if (command.b == kSoundPresetKeep) {
                for (int i = 0; i < kArpBytes; i++) arp_back_[i] = bytes[i];
                arp_back_ext_ = ext;
                arp_back_track_ = track;
                break;
            }
            if (command.b == kSoundPresetBack) {
                if (arp_back_track_ != track) {
                    status_.bump_refused();
                    break;
                }
                for (int i = 0; i < kArpBytes; i++) bytes[i] = arp_back_[i];
                ext = arp_back_ext_;
            } else if (!arp_preset_apply(command.b, bytes, ext)) {
                status_.bump_refused();
                break;
            }
            const uint32_t locked = locked_[track];
            for (int p = kArpFirst; p < kArpFirst + kArpBytes; p++) {
                if ((locked >> p & 1u) == 0) effective_[track].v[p] = params_[track].v[p];
            }
            perform_dirty_ = true;
            break;
        }

        case Cmd::set_arp_ext:
            // A held run picks a new VAR up at its next tick (arp_follow); RHYTHM, ACCENT and
            // RATCHET are read hit by hit.
            arp_ext_set(project_.kit.tracks[track].arp, command.a, command.b);
            break;

        case Cmd::set_lfo_trig: {
            // LFO TRIG (KitTrack::lfo_trig): the next note starts the cycle; a change to FREE
            // carries on from wherever the cycle is.
            project_.kit.tracks[track].lfo_trig =
                command.a < kLfoTrigs ? command.a : kLfoTrigFree;
            lfo_[track].rest();          // a new ONE-SHOT waits for its first note, at rest
            break;
        }

        case Cmd::set_mod_slot: {
            if (command.a >= kModSlots) break;
            project_.kit.tracks[track].mod[command.a] =
                mod_slot_sanitized(command.b, command.c, command.d);
            break;
        }

        case Cmd::set_choke: {
            const uint8_t now = project_.kit.tracks[track].choke;
            const uint8_t next = now >= kChokeGroups ? uint8_t(0) : uint8_t(now + 1);
            project_.kit.tracks[track].choke =
                command.c == 1 ? next : choke_group_sanitized(command.a);
            break;
        }

        case Cmd::set_pressure:
            mod_pressure_[track] = float(command.a > 127 ? 127 : command.a) * (1.0f / 127.0f);
            break;

        case Cmd::set_slice_mode: {
            const uint8_t mode = command.a == kSliceAuto ? kSliceAuto : kSliceEqual;
            project_.kit.tracks[track].slice_mode = mode;
            pool_[track].sampler.set_auto_slices(mode == kSliceAuto);
            break;
        }

        case Cmd::set_machine: {
            if (command.a >= uint8_t(MachineId::count)) {
                status_.bump_refused();
                break;
            }
            if (machine_id_[track] == command.a) break;      // already hosting it

            // The machine being LEFT is silenced first. It will not be rendered again, so it
            // cannot release its own tail -- and a voice left sounding in an object nothing
            // calls is a note that never ends and never ends up in the voice count either.
            silence_track(track);

            machine_id_[track] = command.a;
            project_.kit.tracks[track].machine = command.a;
            cc_known_[track] = false;     // a MID track's CCs start from what they are now
            rewire_machines();

            // The new machine reads this track's parameters, which mean different things to it:
            // slot 10 is GLD on VA MONO and DET on VA POLY, and the FLTR page is the insert
            // chain's on both. Until 2026-11-23 the knobs kept their positions across a switch.
            //
            // Since then (Roland) a switch loads the new machine's default sound
            // (core/sounds.hpp, machine_defaults_apply) into its sound pages, so nothing the old
            // machine left there leaks into the new one: a VA's knobs made a new SAMPLE start
            // half way into the file, sliced, and fade early. The mix, LFO and ARP stay. A lock on
            // the current step still wins until the next step, as for a knob or a preset.
            if (machine_defaults_apply(command.a, params_[track].v)) {
                const uint32_t locked = locked_[track];
                for (int p = 0; p < kTrackParams; p++) {
                    if ((locked >> p & 1u) == 0) effective_[track].v[p] = params_[track].v[p];
                }
            }
            if (machines_[track] != nullptr) machines_[track]->prepare(effective_[track]);

            // And the sample, if the new machine plays one. The loader's path is in the kit; a
            // track switched to SAMPLE or GRAIN before any card has been read falls back to the
            // placeholder every machine in the pool was given at init.
            perform_dirty_ = true;
            break;
        }

        default:
            return false;
    }
    return true;
}

// One step: trigs, locks, ranges, slides, its fields, live-recorded hits, Euclid.
bool Engine::apply_step_edit(const Command &command, int track, int step) {
    switch (command.cmd) {
        case Cmd::toggle_trig: {
            // a = 2: hold FILL + tap a STEP (backlog [5]). A trig whose condition is FILL comes
            // off; anything else becomes a trig with COND FILL, so a fill is one tap to write.
            if (command.a == 2) {
                Step &s = live().tracks[track].steps[step];
                const bool is_fill = (s.flags & kStepTrig) != 0 &&
                                     condition_kind(s.condition) == Condition::FILL;
                if (is_fill) {
                    s.flags = uint8_t(s.flags & ~kStepTrig);
                    s.condition = condition_pack(Condition::NONE);
                } else {
                    s.flags = uint8_t((s.flags | kStepTrig) & ~kStepTrigless);
                    s.condition = condition_pack(Condition::FILL);
                    if (s.velocity == 0) s.velocity = 100;
                    if (!length_valid(s.length)) s.length = 8;
                }
                break;
            }
            // a selects which bit: the trig, or the trigless bit that applies a step's locks
            // without sounding a note. They are mutually exclusive -- a step that is both would
            // be a trig, so letting both bits stand would make the trigless flag look broken.
            Step &s = live().tracks[track].steps[step];
            const uint8_t bit = command.a ? uint8_t(kStepTrigless) : uint8_t(kStepTrig);
            const uint8_t other = command.a ? uint8_t(kStepTrig) : uint8_t(kStepTrigless);
            const int flags = (int(s.flags) ^ int(bit)) & ~int(other);
            s.flags = uint8_t(flags);
            if ((s.flags & kStepTrig) && s.velocity == 0) s.velocity = 100;
            if ((s.flags & kStepTrig) && !length_valid(s.length)) s.length = 8;
            break;
        }

        case Cmd::clear_lock:
            // The whole parameter: its value, its slide and its random range (hold STEP + push).
            pattern_clear_param(live(), track, step, command.a);
            break;

        case Cmd::lock_range_delta: {
            if (command.a >= kTrackParams) break;
            const int width =
                int(pattern_get_range(live(), track, step, command.a)) + int(int8_t(command.b));
            const uint8_t wanted = uint8_t(width < 0 ? 0 : (width > 127 ? 127 : width));
            if (!pattern_set_range(live(), track, step, command.a, wanted)) {
                pattern_compact_locks(live());   // as lock_delta: abandoned slots first
                if (!pattern_set_range(live(), track, step, command.a, wanted)) {
                    status_.bump_refused();
                }
            }
            break;
        }

        case Cmd::lock_slide: {
            if (command.a >= kTrackParams) break;
            const uint8_t param = command.a;
            uint8_t value = 0;
            if (!pattern_get_lock(live(), track, step, param, value)) {
                // Nothing to glide from yet: lock the kit value, which is what the step plays.
                if (!pattern_set_lock(live(), track, step, param, params_[track].v[param])) {
                    pattern_compact_locks(live());
                    if (!pattern_set_lock(live(), track, step, param, params_[track].v[param])) {
                        status_.bump_refused();
                        break;
                    }
                }
            }
            pattern_set_slide(live(), track, step, param,
                              !pattern_get_slide(live(), track, step, param));
            break;
        }

        case Cmd::step_field_delta:
            if (command.a < uint8_t(StepField::count)) {
                Step &s = live().tracks[track].steps[step];
                const int was = int(s.note);
                step_field_apply(s, StepField(command.a), int(int8_t(command.b)));
                // NOTE moves a chord played into the step with it (core/stepnotes.hpp).
                step_shift_extra_notes(live(), track, step, int(s.note) - was);
            }
            break;

        case Cmd::lock_delta: {
            if (command.a >= kTrackParams) break;
            const uint8_t param = command.a;

            // The base is the step's own lock if it has one, and the kit value if it does not --
            // so the first click of an encoder on an unlocked step moves away from what you are
            // currently hearing rather than from zero.
            uint8_t base = params_[track].v[param];
            pattern_get_lock(live(), track, step, param, base);
            const uint8_t value = command.c == kSemitoneFlag && tune_param(machine_id_[track]) == param
                                      ? tune_step(machine_id_[track], base, int(int8_t(command.b)))
                                      : clamp_7bit(int(base) + int(int8_t(command.b)));

            if (!pattern_set_lock(live(), track, step, param, value)) {
                // "Full" usually means slots abandoned by relocation rather than 1,024 real
                // locks: growing a step's run moves it to the top of the pool and leaves the old
                // one behind (core/pattern.hpp). Compacting reclaims those, so try once more
                // before refusing -- otherwise locks start failing long before the pool is
                // actually full, and an undo level gets burned on an edit that did nothing.
                pattern_compact_locks(live());
                if (!pattern_set_lock(live(), track, step, param, value)) {
                    status_.bump_refused();  // genuinely full; saying nothing would be a lie
                    break;
                }
            }

            // Heard immediately if this is the step that is playing. Without this a
            // motion-recorded knob is silent until the next pass of the loop, because fire()
            // built this track's effective parameters for the current step before the lock
            // existed -- which would read as "motion recording does nothing".
            if (scheduler_.playhead(track) == uint8_t(step)) {
                effective_[track].v[param] = value;
                // And mark it LOCKED, or the per-block copy in process() -- which rewrites every
                // parameter this mask does not claim -- undoes the line above before a sample is
                // rendered, leaving exactly the silence the comment says it prevents. fire()
                // rebuilds the mask for the next step, so this only has to hold until then.
                locked_[track] = locked_[track] | (1u << param);
            }
            break;
        }

        case Cmd::record_hit: {
            // The step and the micro-timing were resolved in the main context by core/record,
            // from the clock state at the moment the hit arrived. Nothing here re-decides them.
            // `d` is how the note meets what the step holds (RecWrite, app/commands.hpp).
            Step &s = live().tracks[track].steps[step];
            const uint8_t note = clamp_7bit(command.a);
            const uint8_t velocity =
                uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
            const bool on = (s.flags & kStepTrig) != 0;
            if (command.d == uint8_t(RecWrite::join) && on) {
                // Another note of a chord on a step that already plays: it joins, and the
                // step's note, LEN and micro stay. The loudest note sets the velocity, and the
                // same note again is played again, so its velocity is the new one.
                NoteAdd added = step_add_note(live(), track, step, note);
                if (added == NoteAdd::full && s.lock_count < kLocksPerStep) {
                    pattern_compact_locks(live());     // abandoned slots, as lock_delta does
                    added = step_add_note(live(), track, step, note);
                }
                if (added == NoteAdd::full) {
                    status_.bump_refused();            // six notes, or no lock slot left
                    break;
                }
                s.velocity = added == NoteAdd::present
                                 ? velocity
                                 : (velocity > s.velocity ? velocity : s.velocity);
                break;
            }
            const int flags = (int(s.flags) | int(kStepTrig)) & ~int(kStepTrigless);
            s.flags = uint8_t(flags);
            s.note = note;
            s.velocity = velocity;
            s.micro = int8_t(command.c);
            if (!length_valid(s.length)) s.length = 8;
            // A note repeat recorded (CyDrums P3): the roll, at its rate, for the LEN its pad's
            // release gives (Cmd::record_length). 0 leaves the step's ratchet as it was.
            if (command.e != 0) s.ratchet = command.e;
            if (command.d != uint8_t(RecWrite::note)) {
                // A chord track's first note of a pass on this step: what was played is what
                // the step holds, so the notes and the chord type that were there go. (A join
                // onto a step that was off lands here too, and starts the chord.)
                step_clear_extra_notes(live(), track, step);
                s.chord = 0;
            }
            // The step's locks are deliberately left alone. Overdubbing onto a programmed part
            // should keep the sound it was programmed with; a hit replaces the note, not the
            // step's whole character.
            break;
        }

        case Cmd::record_length: {
            // The key that recorded this step came up. Only if the step still holds that note:
            // a later hit landing on the same step has replaced it, and its own key decides.
            // On a chord any of its notes counts, and the last key up gives the chord its LEN.
            Step &s = live().tracks[track].steps[step];
            if ((s.flags & kStepTrig) != 0 && step_has_note(live(), track, step, command.a) &&
                length_valid(command.b)) {
                s.length = command.b;
            }
            break;
        }

        case Cmd::set_step_length: {
            Step &s = live().tracks[track].steps[step];
            if ((s.flags & kStepTrig) != 0 && length_valid(command.a)) s.length = command.a;
            break;
        }

        case Cmd::euclid:
            // The whole track's rhythm, from hits, a cycle and a rotation. core/mutate clamps
            // all three, so an encoder cannot ask for anything it has to refuse -- but a
            // generate that produced what was already there is still worth reporting, so the UI
            // does not spend an undo level on it.
            if (!euclid_generate(live(), track, int(command.a), int(command.b),
                                 int(int8_t(command.c)))) {
                status_.bump_refused();
            }
            break;

        default:
            return false;
    }
    return true;
}

// The performance layer (M5): FILL, MUTATE, scenes, morph, macros, mute states, throws,
// repeat, the performance mixer and the looper.
bool Engine::apply_perform(const Command &command, int track, int step) {
    switch (command.cmd) {
        case Cmd::set_fill:
            // The pad. Auto-fill ORs into this per bar in resolve_tick, so holding FILL during
            // an auto-fill bar is not a contradiction.
            fill_held_ = command.a != 0;
            // Also applied here, because resolve_tick is the only other place that sets it and
            // it does not run with the clock stopped. Without this, holding FILL while stopped
            // would do nothing and the pad would look broken.
            perform_.fill = fill_held_;
            break;

        case Cmd::mutate: {
            const int count = command.c == 0 ? kStepsPerTrack : int(command.c);
            if (!mutate_apply(live(), track, step, count, Mutation(command.a), command.u,
                              int(command.b))) {
                // Nothing changed, which is worth saying: the UI should not spend an undo level
                // on an edit that did nothing, and a press that looks like it did nothing had
                // better say so.
                status_.bump_refused();
                status_.bump_mutate_unchanged();
            }
            break;
        }

        case Cmd::scene_capture:
            if (command.a < kScenes) {
                // The kit as it is NOW, which means refreshing it from `params_` first.
                //
                // `project_.kit` is only written by `Cmd::kit_store`, and only a save (of the
                // project or of a kit) posts that -- while a knob turn writes `params_`. So a
                // capture read the kit
                // as of the last SAVE: pressing OK in PERFORM stored the sound the project had
                // when it was last written, not the sound in the room. Found independently by
                // two reviews, from opposite directions, and the comment that used to be here
                // claimed the opposite ("a capture takes the CURRENT kit").
                kit_from_params();

                // From the KIT, not from `performed_`: a scene is a sound to come back to, and
                // capturing the morph's own output would fold the performance into the thing
                // being performed.
                if (command.b != 0) scene_capture_track(scenes_.scenes[command.a], project_.kit,
                                                        track);
                else {
                    scene_capture(scenes_.scenes[command.a], project_.kit);
                    // A whole capture is the whole sound, so the master EQ's knobs too (not
                    // its kill switches, which are a hand on the panel). A one-track capture
                    // leaves the EQ out: it is not that track's.
                    scene_capture_master(scenes_.scenes[command.a],
                                         &project_.settings.master[kMasterEqFirst]);
                }
                perform_dirty_ = true;
            }
            break;

        case Cmd::scene_erase:
            if (command.a < kScenes) {
                scene_erase(scenes_.scenes[command.a]);
                perform_dirty_ = true;
            }
            break;

        case Cmd::scene_select:
            if (command.b < kScenes) {
                if (command.a == 0) scene_a_ = command.b;
                else scene_b_ = command.b;
                // And the project's copy, which is what a save writes.
                project_.settings.scene_a = scene_a_;
                project_.settings.scene_b = scene_b_;
                perform_dirty_ = true;
            }
            break;

        case Cmd::set_morph:
            morph_ = uint16_t(command.u > kMorphScale ? kMorphScale : command.u);
            perform_dirty_ = true;
            break;

        case Cmd::set_move_all: {
            const int8_t amount = all_turn(int8_t(command.b), 0);   // held to the range
            if (command.a == 0) {
                rel_all_ = amount;
                perform_dirty_ = true;
            } else if (command.a == 1) {
                len_all_ = amount;                                 // read by fire(), per trig
            }
            break;
        }

        case Cmd::set_live_rnd:
            // Read by the scheduler on the next tick: an armed track jumps to its random block
            // at the next slot, and OFF puts every slot back on its own step.
            perform_.rnd_unit = live_rnd_turn(command.a, 0);          // held to OFF .. RND16
            perform_.rnd_armed = command.b;
            break;

        case Cmd::set_macro:
            if (command.a < kMacros) {
                macro_position_[command.a] = command.b > 127 ? uint8_t(127) : command.b;
                perform_dirty_ = true;
            }
            break;

        case Cmd::macro_assign:
            // `b` past the twenty track parameters names a master EQ band: kTrackParams + 0-2.
            // The track byte cannot carry it -- every command naming track 8 is refused at the top.
            if (command.a >= kMacros || command.b >= kTrackParams + kMasterBands) {
                // Counted, like the "macro is full" refusal below it. Two refusals in one
                // command reported two different ways is how a user concludes the box is broken.
                status_.bump_refused();
                break;
            }
            {
                // The next free target. Which of the four it lands in is not something a
                // performer should have to think about; a macro that is full refuses, visibly.
                Macro &macro = macros_.macros[command.a];
                const bool to_master = command.b >= kTrackParams;
                const uint8_t want_track = to_master ? kMacroMasterTrack : uint8_t(track);
                const uint8_t want_param =
                    to_master ? uint8_t(command.b - kTrackParams) : command.b;
                // The same parameter again takes it OFF the macro: a second press undoes the
                // first, rather than filling a second of the four slots with a copy of it.
                bool removed = false;
                for (int i = 0; i < kMacroTargets; i++) {
                    MacroTarget &t = macro.targets[i];
                    if (t.enabled != 0 && t.track == want_track && t.param == want_param) {
                        t = MacroTarget{};
                        removed = true;
                    }
                }
                if (removed) {
                    perform_dirty_ = true;
                    break;
                }
                int slot = -1;
                for (int i = 0; i < kMacroTargets && slot < 0; i++) {
                    if (macro.targets[i].enabled == 0) slot = i;
                }
                if (slot < 0) {
                    status_.bump_refused();
                    break;
                }
                macro.targets[slot].enabled = 1;
                const bool on_master = command.b >= kTrackParams;
                macro.targets[slot].track = on_master ? kMacroMasterTrack : uint8_t(track);
                macro.targets[slot].param = on_master ? uint8_t(command.b - kTrackParams) : command.b;
                // From the parameter's value now to the end the macro heads for. `c` is that
                // value and `d` the end. A macro sweeps from the LIVE value (core/perform.hpp), so
                // `low` is a record for the screen; with the macro knob already up, the parameter
                // goes straight to where the knob puts it, which is the macro taking effect.
                macro.targets[slot].low = command.c > 127 ? uint8_t(127) : command.c;
                macro.targets[slot].high = command.d > 127 ? uint8_t(127) : command.d;
                perform_dirty_ = true;
            }
            break;

        case Cmd::macro_clear:
            if (command.a < kMacros) {
                for (int i = 0; i < kMacroTargets; i++) {
                    macros_.macros[command.a].targets[i] = MacroTarget{};
                }
                perform_dirty_ = true;
            }
            break;

        case Cmd::mute_store:
            if (command.a < 8) {
                project_.settings.mute_states[command.a] = mute_state_capture(muted_);
            }
            break;

        case Cmd::mute_recall:
            if (command.a < 8) {
                const uint8_t state = project_.settings.mute_states[command.a];
                if (command.b != 0) mute_latch_arm(mute_latch_, state);
                else apply_mutes(state);
            }
            break;

        case Cmd::set_auto_fill:
            auto_fill_ = command.a < uint8_t(AutoFill::count) ? AutoFill(command.a)
                                                             : AutoFill::off;
            break;

        case Cmd::set_throw:
            // Not a parameter edit: `params_` is untouched, so letting go puts the stored send
            // back by itself with nothing to remember.
            throw_bus_ = command.a > 2 ? 0 : int(command.a);
            throw_track_ = track;
            perform_dirty_ = true;
            break;

        case Cmd::set_repeat:
            repeat_length_ = command.a < uint8_t(RepeatLength::count) ? RepeatLength(command.a)
                                                                     : RepeatLength::sixteenth;
            repeat_pitch_ = command.c != 0;
            repeat_held_ = command.b != 0;
            break;

        case Cmd::perf_fx: {
            // One effect at a time (core/perffx.hpp). The looper and the morph are not effects:
            // the looper's mix is Cmd::looper's and the morph Cmd::set_morph's, so an a naming
            // either, or past the end, is refused.
            if (command.a >= uint8_t(PerfFx::count) || perf_fx_is_control(PerfFx(command.a))) {
                status_.bump_refused();
                break;
            }
            perf_fx_ = PerfFx(command.a);
            perf_amount_ = command.b > kPerfAmountMax ? kPerfAmountMax : command.b;
            perf_plan_ = perf_plan(perf_fx_, perf_amount_);
            perform_dirty_ = true;          // the sends' raise is in apply_performance
            break;
        }

        case Cmd::looper:
            if (command.a == 0) {
                looper_mix_ = command.b > kPerfAmountMax ? kPerfAmountMax : command.b;
            } else if (command.a == 1) {
                // The buffer is RESAMPLE's: not while a take is armed, running or waiting.
                if (capture_.armed() || capture_.recording() || capture_.done()) {
                    status_.bump_refused();
                    break;
                }
                // Bars into frames, as resample_arm does; halved until the take fits (8 bars
                // fit down to 64 BPM, 4 to 32).
                uint32_t bars = command.b >= 8 ? 8u : (command.b >= 4 ? 4u : (command.b >= 2 ? 2u : 1u));
                float frames = float(bars) * float(kTicksPerBar) * samples_per_tick_;
                while (bars > 1u && frames > float(looper_.capacity())) {
                    bars >>= 1;
                    frames *= 0.5f;
                }
                if (frames < 1.0f || !looper_.arm(uint32_t(frames), bars)) {
                    status_.bump_refused();
                }
            } else if (command.a == 2) {
                looper_.clear();
            } else {
                status_.bump_refused();
            }
            break;

        default:
            return false;
    }
    return true;
}

// Notes played live: pads and keys, note repeat, the step stutter, MIDI out.
bool Engine::apply_notes(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::trigger_note: {
            // A held KEYS key on a track whose arp is on joins the held notes instead: the arp
            // plays them (core/arp.hpp). A pad or MIDI note has no release to end a hold.
            if (command.c == 1 && arp_settings(track).on()) {
                const uint8_t note = clamp_7bit(command.a);
                uint8_t &count = held_count_[track];
                bool have = false;
                for (int i = 0; i < count; i++) have = have || held_notes_[track][i] == note;
                if (!have && count < kArpMaxInput) held_notes_[track][count++] = note;
                held_arp_[track].velocity =
                    uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
                arp_held_changed(track);
                break;
            }
            // A pad hit, played live. It sounds now, at the start of the block: the main context
            // cannot know a frame offset, and the spec's latency stamping belongs to recording.
            // Not on a muted track: a muted track is not rendered, so the note would wait in the
            // machine and sound the moment the track was unmuted.
            if (machines_[track] && !muted_[track]) {
                Trig trig;
                trig.note = clamp_7bit(command.a);
                trig.velocity = uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
                trig.frame = 0;
                trig.held = command.c == 1;   // a key or MIDI IN note: its release comes
                // A pad hit (c = 0) has no release coming. On a VA whose ASR page holds notes
                // (SUS up) it would hold for ever, so it gets one step, as its MIDI note does. At
                // SUS 0 it keeps no gate: the envelope ends it, as it always has.
                if (command.c != 1 && sounding(track).v[kAsrFirst + 1] > 0) {
                    trig.gate_frames = gate_frames_for(track, kMidiDefaultLength);
                }
                // A pad on a POLY track counts against the shared budget like a step does.
                if (make_room_for_poly_voice(track, trig.note)) machines_[track]->trigger(trig);
                note_started(track, trig.velocity, 0);
                // And to MIDI OUT. A key or a MIDI IN note (c = 1) is held until its release; a
                // pad has none, so it lasts one step, as a step with no gate does.
                send_midi_notes(track, &trig.note, 1, trig.velocity, 0,
                                command.c == 1 ? kMidiHeld : midi_gate(track, 0));
            }
            break;
        }

        case Cmd::note_repeat: {
            Roll &roll = rolls_[track];
            const uint8_t velocity =
                uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
            if (command.c == 0) {
                // Let go. The last hit keeps its gate and ends by itself.
                roll.on = false;
            } else if (command.c == 1) {
                roll.on = true;
                roll.note = clamp_7bit(command.a);
                roll.velocity = velocity;
                roll.last_tick = tick_;
                roll.countdown = roll_period_frames();
                roll_fire(track, 0);
            } else if (command.c == 2) {
                if (roll.on) roll.velocity = velocity;   // pressure on a pad that is not rolling
            } else {
                status_.bump_refused();
            }
            break;
        }

        case Cmd::step_stutter:
            if (command.b == 0) {
                stutter_step_ = -1;           // the last hits keep their gates
                break;
            }
            if (command.a >= kStepsPerTrack || command.c == 0) {
                status_.bump_refused();
                break;
            }
            stutter_step_ = int(command.a);
            stutter_tracks_ = command.c;
            stutter_last_tick_ = tick_;
            stutter_countdown_ = roll_period_frames();
            stutter_fire(0);
            break;

        case Cmd::release_note: {
            // A KEYS key coming up. Not refused on a muted track or with nothing holding the
            // note: a release with nothing to let go is the second half of a gesture whose
            // first half was swallowed, and it changes nothing (dsp/track.hpp, `release`).
            if (machines_[track]) machines_[track]->release(clamp_7bit(command.a));
            midi_out_.release(midi_sink_, track, clamp_7bit(command.a));
            // And out of the arp's held notes, if it was one.
            const uint8_t note = clamp_7bit(command.a);
            uint8_t &count = held_count_[track];
            for (int i = 0; i < count; i++) {
                if (held_notes_[track][i] != note) continue;
                for (int j = i; j + 1 < count; j++) held_notes_[track][j] = held_notes_[track][j + 1];
                count--;
                arp_held_changed(track);
                break;
            }
            break;
        }

        case Cmd::set_midi_out: {
            int next = command.a;
            if (command.c == kDeltaFlag) {
                next = int(project_.settings.midi_out[track]) + int(int8_t(command.b));
            }
            next = next < 0 ? 0 : (next > kMidiChannels ? kMidiChannels : next);
            if (next == int(project_.settings.midi_out[track])) break;
            // The notes already sent end on the channel they went out on (each slot keeps its
            // own), and the CCs are sent afresh only when they next move.
            midi_out_.release_track(midi_sink_, track);
            project_.settings.midi_out[track] = uint8_t(next);
            cc_known_[track] = false;
            break;
        }

        default:
            return false;
    }
    return true;
}

// The mixer: duck, levels, mutes, sends, master and the kill EQ.
bool Engine::apply_mixer(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::set_duck: {
            // The sidechain duck (DuckField): an encoder delta onto the kit's byte, clamped into
            // the domain core/project.hpp gives it, then handed to the mixer.
            const int delta = int(int8_t(command.b));
            const auto moved = [delta](uint8_t now, int low, int high) {
                const int next = int(now) + delta;
                return uint8_t(next < low ? low : (next > high ? high : next));
            };
            KitDuck &duck = project_.kit.duck;
            switch (DuckField(command.a)) {
                case DuckField::depth:
                    project_.kit.tracks[track].duck =
                        moved(project_.kit.tracks[track].duck, 0, kDuckDepthMax);
                    break;
                case DuckField::source:
                    duck.source = moved(duck.source, 0, kTracks - 1);
                    break;
                case DuckField::attack:
                    duck.attack_ms = moved(duck.attack_ms, kDuckAttackMin, kDuckAttackMax);
                    break;
                case DuckField::hold:
                    duck.hold = moved(duck.hold, 0, kDuckHoldMax);
                    break;
                case DuckField::release:
                    duck.release = moved(duck.release, kDuckReleaseMin, kDuckReleaseMax);
                    break;
                default:
                    break;
            }
            apply_duck();
            break;
        }

        case Cmd::set_track_level: {
            // A NaN fails both comparisons and would pass straight through, so it is caught first.
            const float f = command.f == command.f ? command.f : 0.0f;
            if (command.c == kDeltaFlag) {
                const float next = mix_[track].level + f;
                mix_[track].level = next < 0.0f ? 0.0f : (next > kFaderDeltaMax ? kFaderDeltaMax : next);
            } else {
                mix_[track].level = f < 0.0f ? 0.0f : (f > 2.0f ? 2.0f : f);
            }
            break;
        }

        case Cmd::set_mute:
            if (command.b != 0) {
                // At the next bar. The latch holds a whole STATE rather than one toggle, so the
                // state to arm is this track flipped in what the bar will bring: the state
                // already armed if there is one, else the mutes as they are NOW -- which only
                // this side can know. From NOW alone, a second FUNC + pad in the same bar threw
                // the first one away (backlog [6]: "FUNC + pad mutes are unreliable").
                uint8_t state = mute_latch_.pending ? mute_latch_.state : mute_state_capture(muted_);
                state = uint8_t(state ^ uint8_t(1u << track));
                mute_latch_arm(mute_latch_, state);
            } else {
                uint8_t state = mute_state_capture(muted_);
                state = command.a != 0 ? uint8_t(state | (1u << track))
                                       : uint8_t(state & ~(1u << track));
                apply_mutes(state);
            }
            break;

        case Cmd::set_sends:
            // Both of these had NO HANDLER AT ALL until now: the two arms fell through `default`,
            // so every delay and reverb setting, and docs/SPEC.md's "Master DJ filter", was
            // frozen at its compile-time value for the life of the app -- unreachable from any
            // gesture and absent from every file.
            //
            // `a` is the field and `b` is an encoder DELTA, as a signed byte. The byte the
            // delta moves is what the project stores (core/project.hpp says why) and the float
            // the mixer runs on is derived from it, so the file and the sound cannot disagree --
            // and so a tempo change can re-derive the delay's time from the division the user
            // actually chose.
            //
            // A delta rather than a value for the reason app/commands.hpp gives for every
            // encoder gesture: the authoritative byte lives here, and a UI that computed the
            // next one would read the same stale byte for every click in a sweep.
            if (command.a >= uint8_t(SendField::count)) {
                status_.bump_refused();
                break;
            }
            project_.settings.sends[command.a] =
                send_byte_step(command.a, project_.settings.sends[command.a],
                               int(int8_t(command.b)));
            apply_mixer_bytes();
            break;

        case Cmd::set_master:
            if (command.a >= uint8_t(MasterField::count)) {
                status_.bump_refused();
                break;
            }
            project_.settings.master[command.a] =
                master_byte_step(command.a, project_.settings.master[command.a],
                                 int(int8_t(command.b)));
            apply_mixer_bytes();
            break;

        case Cmd::eq_kill: {
            if (command.a >= uint8_t(kKillBands) || command.b > 2) {
                status_.bump_refused();
                break;
            }
            const uint8_t bit = uint8_t(1u << command.a);
            uint8_t &kills = project_.settings.eq_kills;
            kills = command.b == 0 ? uint8_t(kills & ~bit)
                                   : (command.b == 1 ? uint8_t(kills | bit) : uint8_t(kills ^ bit));
            apply_mixer_bytes();
            break;
        }

        default:
            return false;
    }
    return true;
}

// A track's or the pattern's timing: length, speed, play mode, swing, FEEL.
bool Engine::apply_track_timing(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::set_track_length: {
            const int wanted = command.c == kDeltaFlag
                                   ? int(live().tracks[track].length) + int(int8_t(command.b))
                                   : int(command.a);
            live().tracks[track].length =
                uint8_t(wanted < 1 ? 1 : (wanted > kStepsPerTrack ? kStepsPerTrack : wanted));
            reconfigure();
            break;
        }

        case Cmd::set_track_speed:
            if (command.a < uint8_t(Speed::count)) {
                live().tracks[track].speed = command.a;
                reconfigure();
            }
            break;

        case Cmd::set_track_play:
            // The scheduler reads the mode fresh every tick, so the next step already follows it.
            if (command.a < uint8_t(PlayMode::count)) live().tracks[track].play = command.a;
            break;

        case Cmd::set_pattern_swing: {
            const int wanted = command.c == kDeltaFlag
                                   ? int(live().swing) + int(int8_t(command.b))
                                   : int(command.a);
            live().swing = uint8_t(wanted < kSwingMin ? kSwingMin
                                                      : (wanted > kSwingMax ? kSwingMax : wanted));
            reconfigure();
            break;
        }

        case Cmd::set_pattern_feel: {
            // The scheduler reads the byte fresh every tick, so the next step already follows it.
            live().feel = command.c == kDeltaFlag ? feel_step(live().feel, int(int8_t(command.b)))
                          : command.a <= kFeelMax ? command.a
                                                  : uint8_t(0);
            break;
        }

        case Cmd::set_track_swing: {
            // A delta walks PAT, 50 .. 75; an absolute value is taken if it is PAT or in range
            // and clamped otherwise, so a damaged command cannot store a swing the scheduler
            // would have to clamp again. The scheduler already reads `TrackPattern::swing`
            // (core/scheduler.cpp), so all this has to do is store it and re-derive the timing.
            uint8_t &swing = live().tracks[track].swing;
            swing = command.c == kDeltaFlag ? track_swing_step(swing, int(int8_t(command.b)))
                    : command.a == kSwingInherit
                        ? kSwingInherit
                        : uint8_t(command.a < kSwingMin ? kSwingMin
                                                        : (command.a > kSwingMax ? kSwingMax
                                                                                 : command.a));
            reconfigure();
            break;
        }

        case Cmd::set_master_length: {
            const int wanted = command.c == kDeltaFlag
                                   ? int(live().master_length) + int(int8_t(command.b))
                                   : int(command.a);
            live().master_length = uint8_t(wanted < 1 ? 1
                                                      : (wanted > kStepsPerTrack ? kStepsPerTrack
                                                                                 : wanted));
            // The pass length moved, so the next boundary is measured again from the origin.
            boundary_known_ = false;
            break;
        }

        default:
            return false;
    }
    return true;
}

// Undo, and edits over a range, a track or a whole pattern.
bool Engine::apply_bulk_edit(const Command &command, int track, int step) {
    switch (command.cmd) {
        case Cmd::undo_mark: {
            Gesture gesture;
            gesture.kind = command.a;
            gesture.track = uint8_t(track);
            gesture.step = uint8_t(step);
            gesture.seq = command.b;
            undo_.mark(live(), gesture);
            break;
        }

        case Cmd::undo:
            // A restored pattern may carry different track lengths, speeds or swing, so the
            // scheduler's derived timing has to be rebuilt. The PRE/NEI chain state is
            // deliberately NOT reset: an undo is an edit, not a pattern change, and restarting
            // the chain would make a conditional step somewhere else in the pattern fire
            // differently because of an edit that had nothing to do with it.
            // And the pass boundary: a restored pattern may carry a different master length,
            // and the boundary derived from the old one would end a chain entry or a song at it.
            if (undo_.undo(live())) {
                reconfigure();
                boundary_known_ = false;
            }
            break;

        case Cmd::redo:
            if (undo_.redo(live())) {
                reconfigure();
                boundary_known_ = false;
            }
            break;

        case Cmd::range_copy:
            range_copy(live(), track, step, int(command.a), clip_);
            break;

        case Cmd::range_paste:
            // A refused paste means the lock pool had no room even after compaction. It leaves
            // the pattern untouched (core/editops.hpp), so the only thing to do is count it.
            if (!range_paste(live(), track, step, clip_)) status_.bump_refused();
            break;

        case Cmd::range_erase:
            range_erase(live(), track, step, int(command.a));
            break;

        case Cmd::track_erase:
            track_erase(live(), track);
            break;

        case Cmd::pattern_copy: {
            // About 8 KiB copied here, on the audio side, which owns pattern memory: a few
            // microseconds, once, on a key press.
            const int bank = int(command.track), slot = int(command.step);
            if (!project_index_valid(bank, slot) || !project_index_valid(command.a, command.b) ||
                (bank == int(command.a) && slot == int(command.b))) {
                status_.bump_refused();
                break;
            }
            project_.patterns[bank][slot] = project_.patterns[command.a][command.b];
            pattern_replaced(bank, slot);
            break;
        }

        case Cmd::pattern_double: {
            const int bank = int(command.track), slot = int(command.step);
            if (!project_index_valid(bank, slot)) {
                status_.bump_refused();
                break;
            }
            if (!pattern_double(project_.patterns[bank][slot], double_clip_)) {
                status_.bump_refused();
            }
            pattern_replaced(bank, slot);
            break;
        }

        case Cmd::pattern_clear: {
            const int bank = int(command.track), slot = int(command.step);
            if (!project_index_valid(bank, slot)) {
                status_.bump_refused();
                break;
            }
            pattern_init(project_.patterns[bank][slot]);
            pattern_replaced(bank, slot);
            break;
        }

        case Cmd::pattern_erase:
            // FUNC menu, CLEAR PATTERN. Lengths, speeds and swing stay (core/editops.hpp), so
            // the scheduler's derived timing is still right and nothing needs reconfiguring.
            pattern_erase_steps(live());
            break;

        case Cmd::range_transpose:
            range_transpose(live(), track, step, int(command.a), int(int8_t(command.b)));
            break;

        case Cmd::range_nudge:
            range_nudge(live(), track, step, int(command.a), int(int8_t(command.b)));
            break;

        default:
            return false;
    }
    return true;
}

// The transport and the tempo.
bool Engine::apply_transport(const Command &command, int, int) {
    switch (command.cmd) {
        case Cmd::play:
            // Nothing to do here. enaga::clock's transport calls are MAIN CONTEXT (see
            // <enaga/clock.hpp>), so app/ drives them directly; routing them through this ring
            // would call them from the audio context, where they silently do nothing. The
            // command still exists because the UI's intent is worth seeing in a trace. It does end
            // a finished song's silence, which matters under an outside clock that never stopped.
            song_over_ = false;
            break;

        case Cmd::stop:
            song_over_ = false;
            // Only the voice side of a stop belongs here; the clock itself was already stopped
            // by the main context.
            if (command.a) {
                for (int t = 0; t < kTracks; t++) {
                    silence_track(t);
                }
                // Nothing is sounding now, so what the guard measured says nothing about what
                // comes next: start it over at level 0 (core/cpuguard.hpp).
                cpu_guard_.reset();
                apply_cpu_guard(0);
            } else {
                for (int t = 0; t < kTracks; t++) {
                    if (machines_[t]) machines_[t]->release_all();
                }
            }
            // MIDI out ends with the transport on either stop: an outboard synth has no tail of
            // STRATA's to cut, only notes it will hold until told. The second press and FUNC+F6
            // (both `a` = 1) also send All Notes Off on every channel in use, for a synth that
            // missed a note-off -- a cable pulled mid-note, a queue that was full -- since a
            // hanging note is the one MIDI fault the panel could not otherwise clear.
            midi_out_.release_all(midi_sink_);
            if (command.a) midi_all_notes_off();
            // And the ratchet hits still waiting for their block, on EITHER stop. They are new
            // notes, not tails: a soft stop releases what is sounding, and a hit landing four
            // blocks later would be a note after the transport stopped.
            deferred_count_ = 0;
            span_clear();   // and the rolls of spanning ratchets
            scheduler_.reset(true);
            // And a sequenced arp, whose step is gone. A held KEYS arp keeps going.
            for (int t = 0; t < kTracks; t++) seq_arp_[t].on = false;
            // And the last step's locks. A lock lasts as long as its step, and with the transport
            // stopped no step is playing: left masked, every parameter that step locked was shut
            // out of the performance layer until the next PLAY, so a macro or the morph on it
            // did nothing and a pad played the dead step's value. The per-block copy in
            // process() puts the performed values back in this same block.
            for (int t = 0; t < kTracks; t++) locked_[t] = 0;
            clear_slides();
            break;

        case Cmd::set_tempo:
            // The clock itself is the main context's business, for the same reason as above, and
            // app/ has already set it. What the audio side owns is everything DERIVED from the
            // tempo -- which is the delay, because docs/SPEC.md makes send A tempo-synced: a
            // project moved from 128 to 140 BPM must keep its dotted eighth on the grid instead
            // of holding the seconds it happened to be at.
            //
            // `f` carries BPM (app/commands.hpp); the project stores centi-BPM, because that is
            // what Enaga's clock takes and what one PROJECT.BIN field holds. Clamped as an int
            // after the multiply, so a NaN or a wild float cannot come out the other side as a
            // tempo: a comparison against NaN is false, which is why the clamp is written as
            // "inside the range, or the default" rather than as two one-sided tests.
            {
                // Range-checked as a FLOAT, before the conversion: int() of a NaN or of a value
                // past INT_MAX is undefined behaviour, so testing the int afterwards was too late.
                const float centi = command.f * 100.0f;
                const bool sane =
                    centi >= float(kTempoCentiMin) - 0.5f && centi < float(kTempoCentiMax) + 0.5f;
                const int rounded = sane ? int(centi + 0.5f) : 0;
                project_.settings.tempo_centi_bpm =
                    sane ? uint16_t(rounded) : project_.settings.tempo_centi_bpm;
                if (!sane) status_.bump_refused();
            }
            apply_mixer_bytes();
            break;

        default:
            return false;
    }
    return true;
}

// Which pattern plays next: the queue, restart and the chain.
bool Engine::apply_arrangement(const Command &command, int, int) {
    switch (command.cmd) {
        case Cmd::queue_pattern: {
            const int bank = int(command.a);
            const int slot = int(command.b);
            const QueueMode mode = command.c < uint8_t(QueueMode::count)
                                       ? QueueMode(command.c)
                                       : QueueMode::end_of_pattern;
            if (!project_index_valid(bank, slot)) {
                status_.bump_refused();
                break;
            }
            // With the transport STOPPED there is no end of the current pass to wait for: no
            // tick will arrive to make the change due, so a queued change would simply never
            // happen and the UI would show a pending switch for ever. The only honest reading of
            // any mode with the clock still is "now". `immediate` is the same switch by request.
            if (mode == QueueMode::immediate || !enaga::clock::running()) {
                switch_pattern(bank, slot);
                queue_clear(queue_);
                break;
            }
            // Armed from `tick_`, the tick the sequencer is actually on. That is the whole
            // reason arming happens on this side (core/queue.hpp): the main context would have
            // to guess, and a guess one block out lands the switch on the wrong pass.
            if (!queue_arm(queue_, bank, slot, mode, tick_, master_loop_ticks(live()), origin_)) {
                status_.bump_refused();
            }
            break;
        }

        case Cmd::set_click:
            click_on_ = command.a != 0;
            if (!click_on_ && count_in_end_ == 0) click_.stop();
            break;

        case Cmd::count_in:
            // Posted just before the START it is for, so the clock may already read running
            // here: it is not asked. The UI posts it only from stop (app/ui_input.hpp, F5).
            count_in_end_ = uint32_t(command.a > 2 ? 2 : command.a) * kTicksPerBar;
            break;

        case Cmd::restart:
            // A restart is a jump to the pattern's start, not another count-in.
            count_in_end_ = 0;
            // The position is the clock's, and it was moved to 0 by the main context. What is
            // left is everything measured from it: the pattern starts again at tick 0 with a
            // fresh pass count, ratchet hits still waiting would land after the jump, and the bar
            // count resyncs on the next tick. The voices ring on: this is a restart, not a stop.
            deferred_count_ = 0;
            span_clear();   // and the rolls of spanning ratchets
            scheduler_.reset(true);
            // And a sequenced arp, whose step is gone. A held KEYS arp keeps going.
            for (int t = 0; t < kTracks; t++) seq_arp_[t].on = false;
            // Mutation-equivalent (docs/MUTATION.md): resolve_tick already sends an origin past
            // the new tick back to 0. Kept so that this case says everything a restart resets.
            origin_ = 0;
            song_over_ = false;
            boundary_known_ = false;
            bar_known_ = false;
            break;

        case Cmd::chain_edit: {
            const int at = int(command.step);
            bool done = false;
            switch (ChainEdit(command.a)) {
                case ChainEdit::set:
                    done = chain_set(project_, at, command.b, command.c, command.d, command.e);
                    break;
                case ChainEdit::insert:
                    done = chain_insert(project_, at, command.b, command.c, command.d, command.e);
                    // The player's entry is an index: keep it on the entry it is playing.
                    if (done) chain_player_inserted(chain_, at);
                    break;
                case ChainEdit::remove:
                    done = chain_remove(project_, at);
                    if (done) chain_player_removed(chain_, at);
                    break;
                case ChainEdit::song:
                    project_.settings.chain_song = command.b != 0;
                    done = true;
                    break;
                case ChainEdit::pattern_delta:
                case ChainEdit::repeats_delta:
                case ChainEdit::mutes_delta: {
                    if (at >= chain_length(project_)) break;
                    const ChainEntry entry = project_.chain[at];
                    const int delta = int(int8_t(command.b));
                    if (ChainEdit(command.a) == ChainEdit::pattern_delta) {
                        int index = int(entry.bank) * kPatternsPerBank + int(entry.slot) + delta;
                        const int last = kBanks * kPatternsPerBank - 1;
                        index = index < 0 ? 0 : (index > last ? last : index);
                        done = chain_set(project_, at, uint8_t(index / kPatternsPerBank),
                                         uint8_t(index % kPatternsPerBank), entry.repeats,
                                         entry.mutes);
                    } else if (ChainEdit(command.a) == ChainEdit::mutes_delta) {
                        int mutes = int(entry.mutes) + delta;
                        mutes = mutes < 0 ? 0 : (mutes > kChainMutesMax ? kChainMutesMax : mutes);
                        done = chain_set(project_, at, entry.bank, entry.slot, entry.repeats,
                                         uint8_t(mutes));
                    } else {
                        int repeats = int(entry.repeats) + delta;
                        repeats = repeats < 1 ? 1 : (repeats > kChainRepeatsMax ? kChainRepeatsMax
                                                                                : repeats);
                        done = chain_set(project_, at, entry.bank, entry.slot, uint8_t(repeats),
                                         entry.mutes);
                    }
                    break;
                }
            }
            if (!done) {
                status_.bump_refused();
                break;
            }
            // A chain emptied while it plays stops being followed; the pattern keeps playing.
            if (chain_length(project_) == 0) {
                chain_.active = false;
                project_.settings.chain_enabled = false;
            }
            break;
        }

        case Cmd::cancel_queue:
            // Unconditional: cancelling a queue that is not armed is what the UI does when the
            // user presses the same pattern twice, and it is not an error.
            queue_clear(queue_);
            break;

        case Cmd::set_chain:
            if (command.a != 0) {
                // Starting a chain jumps to its first entry (app/commands.hpp). chain_start
                // leaves the player inactive for an empty chain, which is a refusal the UI
                // should see rather than a chain that is silently "on" and does nothing.
                if (chain_start(project_, chain_)) {
                    switch_pattern(int(project_.chain[chain_.entry].bank),
                                   int(project_.chain[chain_.entry].slot));
                    chain_entry_started();
                } else {
                    status_.bump_refused();
                }
            } else {
                // Stopping a chain leaves the pattern it reached playing, rather than jumping
                // back: a performer turning the chain off wants to stay where they are.
                chain_.active = false;
            }
            song_over_ = false;
            // And record it, which nothing did: `ProjectSettings::chain_enabled` round-tripped
            // through the serializer while never being written, so a project saved with its
            // chain running reopened with it off. The player's own `active` is the live truth;
            // this is the copy a file holds.
            project_.settings.chain_enabled = chain_.active;
            break;

        default:
            return false;
    }
    return true;
}

// The kit and the project as a whole: kit store and recall, LOCK, key, scale, quantize, a
// loaded project.
bool Engine::apply_project(const Command &command, int, int) {
    switch (command.cmd) {
        case Cmd::kit_store:
            kit_from_params();
            break;

        case Cmd::lock_store:
            lock_command(0);
            break;

        case Cmd::lock_tap:
            lock_command(1);
            break;

        case Cmd::kit_recall:
            // The kit browser (FUNC+EDIT twice, app/app.hpp). With `a` = 1 the kit comes from the
            // inbox the main context filled while the ring was empty (`kit_for_recall`); with
            // `a` = 0 it is `project_.kit` as it stands, re-applied.
            //
            // Everything a sound is -- the parameters, the machine, the fader -- but NOT the
            // sample path. Paths are the main context's: only the loader reads them and only the
            // main context writes them (a load, a resample, and this recall's own swap), so
            // Storage puts the recalled paths in place itself before it posts this.
            if (command.a == 1) {
                for (int t = 0; t < kTracks; t++) {
                    KitTrack &to = project_.kit.tracks[t];
                    const KitTrack &from = kit_inbox_.tracks[t];
                    for (int i = 0; i < kKitParams; i++) to.params[i] = from.params[i];
                    to.machine = from.machine;
                    to.level = from.level;
                    to.duck = from.duck;
                    // And the rest KIT.BIN holds, which each arrived in a later section and was
                    // never added here: a kit saved with SLC AUTO, an LFO TRIG, MOD slots or a
                    // choke group came back without them (app/storage.hpp: a kit is "exactly what
                    // a project's KIT.BIN holds"). params_from_kit applies SLC below.
                    to.slice_mode = from.slice_mode;
                    to.lfo_trig = from.lfo_trig;
                    for (int s = 0; s < kModSlots; s++) to.mod[s] = from.mod[s];
                    to.choke = from.choke;
                    lfo_[t].rest();          // a new TRIG starts as Cmd::set_lfo_trig's does
                    mod_env_[t].rest();
                    // The arp's kit settings go with the ARP bytes they shape.
                    to.arp = from.arp;
                }
                project_.kit.duck = kit_inbox_.duck;
            }
            params_from_kit();
            // And the fourth member of the family tests/test_app.cpp has three tests for: the
            // per-block copy rewrites `effective_` from `performed_`, so without this flag the
            // recalled kit is drawn on screen and the previous one goes on playing.
            perform_dirty_ = true;
            break;

        case Cmd::set_root:
            // A pitch class in both places, except that ProjectSettings::root is documented as a
            // MIDI note with a default of 60, so it keeps the middle octave while the pattern's
            // stays a pitch class. core/scale only ever reads `root % 12`, so the two name the
            // same key.
            if (command.a < 12) {
                live().root = command.a;
                project_.settings.root = uint8_t(kDefaultOctaveNote + command.a);
            }
            break;

        case Cmd::set_scale:
            // Not range-checked against kScaleCount, deliberately: core/project.cpp leaves the
            // scale byte unclamped so an id from a later build survives a round trip, and
            // core/scale plays an unknown id chromatically. Clamping here would be the one place
            // that threw it away.
            live().scale = command.a;
            project_.settings.scale = command.a;
            break;

        case Cmd::project_loaded:
            // Everything derived from the pattern that was just replaced. The kit first, because
            // the machines read `params_` and not `project_.kit`; then the undo history, which
            // belonged to a pattern that no longer exists (restoring it would write another
            // project's steps over this one); then the scheduler, whose track lengths, speeds and
            // swing all came from the old pattern.
            params_from_kit();
            // For the same reason as Cmd::set_param above: without this, `performed_` still
            // holds the PREVIOUS project's kit and the per-block copy plays it over the one just
            // loaded. The screen would show the new kit and the machines would sound the old.
            perform_dirty_ = true;
            undo_.clear();
            // Locks are RAM only and belong to the project they were taken in.
            locks_.clear();
            queue_clear(queue_);
            chain_.active = false;
            // A project saved with its chain running reopens with it running. The flag was
            // written by every save and read by nothing, so the chain came back off while the
            // file -- and the next save -- said on. The saved live pattern still opens; the
            // chain takes over from its first entry at the end of that pass (chain_resume).
            if (project_.settings.chain_enabled && !chain_resume(project_, chain_)) {
                project_.settings.chain_enabled = false;
            }
            deferred_count_ = 0;                 // hits of a pattern that no longer exists
            span_clear();   // and the rolls of spanning ratchets
            scheduler_.reset(true);
            // And both arps: the step is gone, and so is the kit whose ARP page the held run
            // followed. Keys still down play plainly until they come up.
            for (int t = 0; t < kTracks; t++) {
                seq_arp_[t].on = false;
                held_arp_[t].on = false;
                held_count_[t] = 0;
                // A one-shot LFO waits for this project's first note, at rest.
                lfo_[t].rest();
                mod_env_[t].rest();
                lfo_restart_[t] = false;
            }
            song_over_ = false;
            // The lock mask of a step of a pattern that no longer exists (Cmd::stop says why).
            for (int t = 0; t < kTracks; t++) locked_[t] = 0;
            clear_slides();
            reconfigure();
            // The sends and the master, which arrived with the file as fourteen bytes. Without
            // this the mixer would keep running the PREVIOUS project's delay and reverb while
            // the screen drew the new project's numbers.
            apply_mixer_bytes();
            boundary_known_ = false;
            // The set as it was left: the live mutes and the scenes in slots A and B. A scene id
            // this build does not have falls back to the default pair rather than indexing past
            // the scene table.
            apply_mutes(project_.settings.live_mutes);
            {
                const ProjectSettings defaults;
                scene_a_ = project_.settings.scene_a < kScenes ? project_.settings.scene_a
                                                               : defaults.scene_a;
                scene_b_ = project_.settings.scene_b < kScenes ? project_.settings.scene_b
                                                               : defaults.scene_b;
                project_.settings.scene_a = scene_a_;
                project_.settings.scene_b = scene_b_;
            }
            break;

        case Cmd::set_rec_mode:
            project_.settings.rec_add = command.a != 0;
            break;

        case Cmd::set_quantize: {
            // The live-record quantize amount, 0-100. It lived in `app/` alone, so it was lost
            // on every restart and absent from the file even though PROJECT.BIN has a field for
            // it. Here instead, so the project is the single copy and `app/` reads it back.
            //
            // A DELTA in `b`, like every other encoder gesture: the value lives here, so a UI
            // computing the next one would read the same stale number for every click of a
            // sweep and only the last would land (app/commands.hpp).
            const int next = int(project_.settings.quantize) + int(int8_t(command.b));
            project_.settings.quantize = uint8_t(next < 0 ? 0 : (next > 100 ? 100 : next));
            break;
        }

        default:
            return false;
    }
    return true;
}

// Samples, streams and the audition.
bool Engine::apply_samples(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::clear_sample:
            // The first half of the sample swap docs/SPEC.md requires. The voices go first,
            // because they are reading the buffer that is about to be released, and the epoch
            // goes last, because the loader may act on it the instant it sees the change -- and
            // what it does is write over that memory. Reversing the two is the bug this protocol
            // exists to prevent.
            //
            // Both of the track's sample readers, whichever machine the track hosts now. Every
            // track owns a Sampler AND a Grain (pool_), and a machine switched away still holds
            // its pointer: clearing only the hosted one left the other pointing into a range the
            // loader is about to overwrite, and switching back played whatever landed there.
            // set_sample kills each machine's voices first (dsp/sample.hpp, dsp/grain.hpp).
            //
            // What they are given instead is the placeholder, not nothing: it is a static array
            // outside the region, so the loader may write over the range at once, and a track
            // whose kit names no file plays what it played before any card was read. The
            // factory's LOOP track is SAMPLE with no file, so a nullptr here made NEW, and every
            // project saved from the factory and opened again, lose that track's part.
            pool_[track].sampler.set_sample(placeholder_, placeholder_frames_, 1, rate_);
            pool_[track].grain.set_sample(placeholder_, placeholder_frames_, 1, rate_);
            // A streamed sample stops being read here too, BEFORE the epoch: the card side closes
            // the track's stream once it sees the bump.
            stop_stream(track);
            // Bumped even for a track that hosts no Sampler. The loader waits for this change
            // before it releases the range, so a track that was never reading anything must
            // still report that it has stopped, or the wait never ends.
            status_.bump_sample_epoch(track);
            break;

        case Cmd::set_sample: {
            // To both readers, for the reason in clear_sample: the track's sample belongs to the
            // track, and SAMPLE and GRAIN are two ways of playing it. Refusing a track that did
            // not host a Sampler is why GRAIN never got a card sample.
            // 24 bits of frames: 16.7 M frames, which is 349 s of mono at 48 kHz and well past
            // the 30 s above which docs/SPEC.md streams from the card instead.
            const uint32_t frames = uint32_t(command.c) | uint32_t(command.d) << 8 |
                                    uint32_t(command.e) << 16;
            const int channels = command.a == 2 ? 2 : 1;
            // At most 2^26 int16, so the product cannot overflow a uint32_t.
            const uint32_t bytes = frames * uint32_t(channels) * uint32_t(sizeof(int16_t));

            // Every field is checked against the region BEFORE a pointer is formed. The offset
            // comes from the other side of the ring, and one past the end would be a read into
            // whatever follows 64 MiB of DDR. The subtraction is written to avoid a wrap, as
            // core/project.cpp's section bounds are. The even-offset test is not pedantry: a
            // misaligned 16-bit read on the C674x does not fault, it silently reads the wrong
            // address (core/pattern.hpp).
            const bool usable = sample_region_ != nullptr && frames != 0 &&
                                command.u % sizeof(int16_t) == 0 &&
                                command.u <= sample_region_bytes_ &&
                                bytes <= sample_region_bytes_ - command.u;
            if (!usable) {
                status_.bump_refused();
                break;
            }
            const int16_t *data = sample_region_ + command.u / sizeof(int16_t);
            stop_stream(track);
            pool_[track].sampler.set_sample(data, frames, channels, rate_);
            pool_[track].sampler.set_slice_points(slice_inbox_[track]);
            status_.slice_points[track].store(uint8_t(pool_[track].sampler.point_count()),
                                              std::memory_order_relaxed);
            pool_[track].grain.set_sample(data, frames, channels, rate_);
            break;
        }

        case Cmd::set_stream: {
            // The head is checked against the region exactly as set_sample's buffer is.
            const StreamInfo &info = stream_inbox_[track];
            const int channels = info.channels == 2 ? 2 : 1;
            const uint32_t head_bytes = info.head_frames * uint32_t(channels) * 2u;
            const bool usable = sample_region_ != nullptr && info.head_frames != 0 &&
                                info.head_frames <= uint32_t(Sampler::kMaxFrames) &&
                                info.head_offset % sizeof(int16_t) == 0 &&
                                info.head_offset <= sample_region_bytes_ &&
                                head_bytes <= sample_region_bytes_ - info.head_offset &&
                                info.frame_bytes != 0 && info.frame_bytes <= 8 &&
                                info.encoding != 0 && info.encoding < uint8_t(PcmFormat::count);
            if (!usable) {
                status_.bump_refused();
                break;
            }
            const int16_t *head = sample_region_ + info.head_offset / sizeof(int16_t);
            const float rate = float(info.rate);
            stop_stream(track);
            Sampler &sampler = pool_[track].sampler;
            sampler.set_stream(head, info.head_frames, info.total_frames, channels, rate,
                               &fifos_[track]);
            status_.slice_points[track].store(0, std::memory_order_relaxed);   // never sliced
            // GRAIN plays what is resident: the head. A grain cloud jumps about the file, which
            // a stream cannot do.
            pool_[track].grain.set_sample(head, info.head_frames, channels, rate);
            if (!sampler.streaming()) {
                status_.bump_refused();
                break;
            }
            StreamState &st = stream_state_[track];
            st.on = true;
            st.attached = false;
            st.encoding = info.encoding;
            st.frame_bytes = info.frame_bytes;
            st.carry = 0;
            st.total = info.total_frames;
            st.restarts_seen = sampler.stream_restarts();
            st.late_seen = sampler.stream_late();
            // Ask for the stream: the card side opens it after the head and attaches.
            st.want = uint16_t(st.want + 1u);
            status_.stream_want[track].store(st.want, std::memory_order_release);
            break;
        }

        case Cmd::stream_attach: {
            StreamState &st = stream_state_[track];
            const uint16_t answered = uint16_t(command.a | uint16_t(command.b) << 8);
            // An answer to an older request is stale: the stream it opened was for a restart
            // that has since been superseded, and the card side is already reopening.
            if (st.on && answered == st.want) {
                st.attached = true;
                st.carry = 0;
            }
            break;
        }

        case Cmd::audition_start: {
            // A start while one plays replaces it; the card side only posts one after the last
            // has ended (its epoch), so this is belt and braces.
            const AuditionInfo &info = audition_inbox_;
            if (audition_on_) end_audition();
            const bool ok = info.frame_bytes != 0 && info.frame_bytes <= 8 &&
                            audition_.start(info.total_frames, int(info.channels), info.step);
            if (!ok) {
                // Refused: the card side is waiting for an epoch before it closes the stream.
                end_audition();
                status_.bump_refused();
                break;
            }
            audition_encoding_ = info.encoding;
            audition_frame_bytes_ = info.frame_bytes;
            audition_carry_ = 0;
            audition_on_ = true;
            break;
        }

        case Cmd::audition_stop:
            // Faded over the next block; the epoch moves when the fade is done (process).
            audition_.stop();
            break;

        default:
            return false;
    }
    return true;
}

// RESAMPLE and freeze: the capture buffer.
bool Engine::apply_capture(const Command &command, int track, int) {
    switch (command.cmd) {
        case Cmd::resample_arm: {
            // Bars into FRAMES, here, because only this side knows the tempo in samples. A bar
            // is kTicksPerBar ticks whatever the pattern's length is -- a resample is a length of
            // TIME, not a number of passes, so a 7-step pattern still gives bars you can loop.
            const int bars = command.a < 1 ? 1 : (command.a > 8 ? 8 : int(command.a));
            const float frames = float(bars) * float(kTicksPerBar) * samples_per_tick_;
            // A freeze's take (b 3) refused says so on a counter of its own, which the main
            // context watches rather than waiting for the take (Status::freeze_refused).
            const bool freeze = command.b == 3;
            if (frames < 1.0f) {
                status_.bump_refused();          // the tempo has not settled yet
                if (freeze) status_.bump_freeze_refused();
                break;
            }
            // `b` 2 is one track, `command.track`: a mono take of that track alone. 3 is the
            // same before the track's insert chain: a freeze (dsp/frozen.hpp). A frozen track
            // is not running its machine, so freezing it again would record the loop it plays:
            // refused, as the UI's FROZEN row already is.
            const bool from_track = command.b == 2 || command.b == 3;
            if (freeze && frozen_[track].on()) {
                status_.bump_refused();
                if (freeze) status_.bump_freeze_refused();
                break;
            }
            // The looper holds the buffer (dsp/looper.hpp) until its loop is thrown away.
            if (looper_.holding()) {
                status_.bump_refused();
                if (freeze) status_.bump_freeze_refused();
                break;
            }
            if (!capture_.arm(uint32_t(frames), from_track ? 1 : 2)) {
                status_.bump_refused();          // a take is running, or one is waiting for the card
                if (freeze) status_.bump_freeze_refused();
                break;
            }
            // Written and cleared, but nothing reads it: the take's length is the `frames`
            // handed to capture_.arm(). A mutation run found it (docs/MUTATION.md); it is kept as
            // the record of what was asked for, for a future RESAMPLE screen to show.
            resample_bars_ = uint8_t(bars);
            // Chosen at the arm and kept for the take: a source switched halfway would make a
            // take that is half one thing and half another.
            capture_input_ = command.b == 1;
            capture_track_ = from_track ? int8_t(track) : int8_t(-1);
            capture_prechain_ = command.b == 3;
            break;
        }

        case Cmd::freeze_set: {
            const uint32_t frames = uint32_t(command.c) | uint32_t(command.d) << 8 |
                                    uint32_t(command.e) << 16;
            const uint32_t bytes = frames * uint32_t(sizeof(int16_t));      // mono
            // Checked against the region before a pointer is formed, as set_sample is.
            const bool usable = sample_region_ != nullptr && frames != 0 &&
                                command.u % sizeof(int16_t) == 0 &&
                                command.u <= sample_region_bytes_ &&
                                bytes <= sample_region_bytes_ - command.u;
            const int16_t *data =
                usable ? sample_region_ + command.u / sizeof(int16_t) : nullptr;
            if (data == nullptr || !frozen_[track].set(data, frames, command.a, capture_bar_,
                                                       capture_lead_, bar_)) {
                status_.bump_refused();
                break;
            }
            // The machine stops here: its voices would otherwise sit unrendered in it (and in
            // VA POLY's shared budget) until the unfreeze.
            if (machines_[track] != nullptr) machines_[track]->kill_all();
            status_.frozen.store(uint8_t(status_.frozen.load(std::memory_order_relaxed) |
                                         (1u << track)),
                                 std::memory_order_relaxed);
            break;
        }

        case Cmd::freeze_clear:
            frozen_[track].clear();
            status_.frozen.store(uint8_t(status_.frozen.load(std::memory_order_relaxed) &
                                         ~(1u << track)),
                                 std::memory_order_relaxed);
            // Always bumped, frozen or not: the main context waits for it before giving the
            // slot back, and a clear for a track that never froze must not leave it waiting.
            status_.bump_freeze_epoch(track);
            break;

        case Cmd::resample_cancel:
            capture_.cancel();
            resample_bars_ = 0;
            break;

        case Cmd::resample_release:
            // The card has the take. This is the only thing that frees the buffer, which is why
            // it is a command and not a method: the buffer belongs to the audio side.
            capture_.release();
            break;

        default:
            return false;
    }
    return true;
}

// --- the kit, between the project and the machines ------------------------
//
// `params_` is what the machines read and what a knob turns; `project_.kit` is what a file
// holds. They are deliberately separate structures -- one belongs to dsp/, the other to core/ --
// so these two functions are the only place they meet.

void Engine::kit_from_params() {
    for (int t = 0; t < kTracks; t++) {
        KitTrack &track = project_.kit.tracks[t];
        for (int i = 0; i < kTrackParams; i++) track.params[i] = params_[t].v[i];
        track.level = mix_[t].level;
        // `machine` and `sample` are not derived from anything the engine owns yet: a track's
        // machine is fixed at init until M6 makes it switchable, and sample paths arrive with
        // project loading. Leaving them alone keeps what a loaded project said.
    }
}

void Engine::apply_mixer_bytes() {
    // 60 / BPM, the length of a beat, which is all dsp/buses.cpp needs to turn the delay's
    // division into a time. The tempo is in centi-BPM, so this is 6000 / centi -- one divide at
    // control rate, through `reciprocal` rather than `/` because that is the rule for every
    // divide in this layer and a knob move is not the place to make an exception.
    const ProjectSettings &settings = project_.settings;
    const uint16_t centi = settings.tempo_centi_bpm < kTempoCentiMin
                               ? uint16_t(kTempoCentiMin)
                               : (settings.tempo_centi_bpm > kTempoCentiMax
                                      ? uint16_t(kTempoCentiMax)
                                      : settings.tempo_centi_bpm);
    const float beat_seconds = 6000.0f * enaga::dsp::reciprocal(float(centi));

    sends_from_bytes(sends_, settings.sends, beat_seconds);
    master_from_bytes(master_, settings.master);
    master_.kills = uint8_t(settings.eq_kills & kKillAll);
    // The EQ's bands come from the performance layer, which reads these bytes as its base: the
    // morph and the macros go over the knobs. Recomputed before this block's mixer.prepare.
    perform_dirty_ = true;
    // `Mixer::prepare` compares against what it last resolved, so an unchanged struct costs one
    // comparison and a changed one redesigns only what moved. Nothing to do here but hand it
    // over on the next block.
}

void Engine::params_from_kit() {
    // The machines first, because a loaded project names its own: KIT.BIN has held
    // `KitTrack::machine` since M4 and nothing honoured it until machine switching existed, so
    // every project opened with the default kit's eight whatever it had been saved with.
    for (int t = 0; t < kTracks; t++) {
        const uint8_t id = project_.kit.tracks[t].machine;
        machine_id_[t] = id < uint8_t(MachineId::count) ? id : uint8_t(MachineId::kick);
    }
    // Whatever was sounding goes first. A machine the new kit takes off a track is never
    // rendered again, so a voice left in it would freeze and resume if a later kit put that
    // machine back -- Cmd::set_machine silences the machine it leaves for the same reason.
    for (int t = 0; t < kTracks; t++) {
        silence_track(t);
        cc_known_[t] = false;
    }
    rewire_machines();

    for (int t = 0; t < kTracks; t++) {
        const KitTrack &track = project_.kit.tracks[t];
        for (int i = 0; i < kTrackParams; i++) {
            params_[t].v[i] = track.params[i] > 127 ? uint8_t(127) : track.params[i];
        }
        // Rebuilt from the kit so a loaded project is heard immediately rather than at the next
        // step, which is the same reason Cmd::set_param mirrors into effective_.
        effective_[t] = params_[t];
        mix_[t].level = track.level < 0.0f ? 0.0f : (track.level > 2.0f ? 2.0f : track.level);
        pool_[t].sampler.set_auto_slices(track.slice_mode == kSliceAuto);
    }
    apply_duck();
}

void Engine::apply_duck() {
    const KitDuck &duck = project_.kit.duck;
    // Clamped here as well as on read: a kit can also arrive from the factory or a test, and a
    // source past the last track would fire nothing and duck everything.
    duck_source_ = duck.source < kTracks ? int(duck.source) : 0;
    // Divided, at control rate and through the same correctly rounded divide as the depths
    // below: 40 / 1000 is exactly the 0.040f the duck was fixed at, where 8 x 0.005f need not
    // be, and 35 / 100 is exactly its 0.35f. So an old project renders as it did.
    const int hold_ms = int(duck.hold) * kDuckTimeStepMs;
    const int release_ms = int(duck.release) * kDuckTimeStepMs;
    mixer_.set_duck(float(duck.attack_ms) / 1000.0f, float(hold_ms) / 1000.0f,
                    float(release_ms) / 1000.0f);
    for (int t = 0; t < kTracks; t++) {
        const uint8_t depth = project_.kit.tracks[t].duck;
        mix_[t].duck = t == duck_source_ ? 0.0f
                                         : float(depth > kDuckDepthMax ? kDuckDepthMax : depth) /
                                               100.0f;
    }
}

// `Step::length` in frames, which is the only form a machine can use.
//
// core/timing.hpp's table is in SIXTEENTHS OF A STEP and deliberately offers no ticks
// conversion: a sixteenth of a 24-tick step is 1.5 ticks, which the 96 PPQN grid cannot express,
// and a gate does not have to land on the grid because it only decides when a voice is released.
// So the conversion happens here, at control rate, where the tempo is known -- exactly where
// that header says it should.
//
// A length index the pattern should not hold, or a tempo not yet read from the clock, both give
// 0, which means "no gate" and leaves the machine's own envelope in charge. That is what every
// machine did before this field existed, so a bad value degrades to the old behaviour rather
// than to silence.
uint32_t Engine::gate_frames_for(int track, uint8_t length_index) const {
    if (!length_valid(length_index) || samples_per_tick_ <= 0.0f) return 0;

    const int ticks = ticks_per_step(Speed(live().tracks[track].speed));
    const float sixteenths = float(length_sixteenths(length_index));
    // sixteenths/16 of a step, a step being `ticks` ticks. One multiply by 1/16 rather than a
    // divide: this runs per trig, but there is no reason to divide where a constant will do.
    const float frames = sixteenths * float(ticks) * samples_per_tick_ * (1.0f / 16.0f);
    return frames > 0.0f ? uint32_t(frames) : 0u;
}

// --- the performance layer ------------------------------------------------
//
// The kit, then the morph, then the macros, then any throw. In that order on purpose: a scene is
// a sound, a macro is a hand on top of it, and a throw is a moment on top of that.
//
// Nothing here touches `params_`. That is what makes every one of them free to let go of: the
// stored kit is still there, so releasing a throw or sweeping the morph back needs no memory of
// what it was.

static_assert(kMasterEqFirst == int(MasterField::eq_low) && kMasterBands == kKillBands,
              "core/perform.hpp's view of the master EQ bytes and dsp/buses.hpp's disagree");

void Engine::apply_performance() {
    if (!perform_dirty_) return;
    perform_dirty_ = false;

    // The kit as stored.
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kTrackParams; p++) performed_[t][p] = params_[t].v[p];
    }

    // The scenes, if two are assigned. An empty scene behaves as the kit, so an unassigned slot
    // costs one pass and changes nothing (core/perform.hpp).
    if (scene_a_ < kScenes && scene_b_ < kScenes) {
        // `performed_` holds the live kit (above), which is the end a scene does not store.
        scene_morph(scenes_.scenes[scene_a_], scenes_.scenes[scene_b_], morph_, performed_);
    }

    // The macros, over the morph. Later targets win over earlier ones, which core/perform.hpp
    // calls a consequence rather than a decision.
    for (int m = 0; m < kMacros; m++) {
        macro_apply(macros_.macros[m], macro_position_[m], performed_);
    }

    // A cutoff that the morph or a macro takes down stops at about 100 Hz, the LFO's floor
    // (dsp/lfo.hpp, D9), so a sweep dips the sound and never shuts it off. Only the knob closes
    // a filter all the way: a kit value already under the floor is where the floor sits for
    // that track, so the layers can never lift it, only stop going lower. A step's lock comes
    // later (`fire`) and is the knob's, so it is not floored either.
    for (int t = 0; t < kTracks; t++) {
        const auto id = MachineId(machine_id_[t]);
        const bool va_filter = machine_has_voice_filter(uint8_t(id));
        const int cut = lfo_cutoff_param(va_filter, id != MachineId::midi && !va_filter);
        if (cut < 0) continue;
        const uint8_t floor = lfo_cutoff_floor(cut, va_filter, !va_filter);
        const uint8_t knob = params_[t].v[cut];
        const uint8_t lowest = knob < floor ? knob : floor;
        if (performed_[t][cut] < lowest) performed_[t][cut] = lowest;
    }

    // The master kill EQ's bands, by the same layering: the knobs, the morph, the macros. Into
    // the mixer's settings here, because nothing else rebuilds them when only a macro moves.
    for (int b = 0; b < kMasterBands; b++) {
        performed_master_[b] = project_.settings.master[kMasterEqFirst + b];
    }
    if (scene_a_ < kScenes && scene_b_ < kScenes) {
        scene_morph_master(scenes_.scenes[scene_a_], scenes_.scenes[scene_b_], morph_,
                           performed_master_);
    }
    for (int m = 0; m < kMacros; m++) {
        macro_apply_master(macros_.macros[m], macro_position_[m], performed_master_);
    }
    for (int b = 0; b < kMasterBands; b++) {
        const uint8_t field = uint8_t(int(MasterField::eq_low) + b);
        master_field_set(master_, field, master_value_from_byte(field, performed_master_[b]));
    }

    // REL ALL (core/moveall.hpp), over the macros: every track's release bytes moved by the same
    // count. At 0 nothing is touched, so rest is the kit, the morph and the macros to the bit.
    if (rel_all_ != 0) {
        for (int t = 0; t < kTracks; t++) {
            uint8_t release[2];
            const int n = machine_release_params(machine_id_[t], release);
            for (int i = 0; i < n; i++) {
                performed_[t][release[i]] = rel_all_byte(performed_[t][release[i]], rel_all_);
            }
        }
    }

    // The throw, over everything: it is the most immediate thing on the panel and should not be
    // argued with by a macro. Send A is the delay and send B the reverb (dsp/chain.hpp).
    if (throw_bus_ != 0 && throw_track_ >= 0 && throw_track_ < kTracks) {
        const int param = page_first(Page::fx) + (throw_bus_ == 1 ? 2 : 3);
        performed_[throw_track_][param] = 127;
    }

    // The performance mixer's sends (core/perffx.hpp): every track into the delay or the reverb,
    // raised to at least the plan's byte. Raised, never lowered, so a track already sending more
    // keeps it, and amount 0 raises nothing.
    if (perf_plan_.send_delay > 0 || perf_plan_.send_reverb > 0) {
        const int delay = page_first(Page::fx) + 2;
        const int reverb = page_first(Page::fx) + 3;
        for (int t = 0; t < kTracks; t++) {
            if (performed_[t][delay] < perf_plan_.send_delay) performed_[t][delay] = perf_plan_.send_delay;
            if (performed_[t][reverb] < perf_plan_.send_reverb) performed_[t][reverb] = perf_plan_.send_reverb;
        }
    }
}

// The plan's overrides on copies of the sends and the master (core/perffx.hpp): the DJ filter
// replaced, the feedback and the decay raised, the kill EQ's LOW lowered.
void Engine::apply_perf_plan(SendSettings &sends, MasterSettings &master) const {
    const PerfPlan &plan = perf_plan_;
    if (plan.dj_on) master.dj = plan.dj;
    if (plan.eq_low < master.eq_low) master.eq_low = plan.eq_low;
    if (plan.delay_feedback > sends.delay_feedback) sends.delay_feedback = plan.delay_feedback;
    if (plan.reverb_decay_s > sends.reverb_decay_s) sends.reverb_decay_s = plan.reverb_decay_s;
}

// --- pattern changes ------------------------------------------------------

void Engine::pattern_replaced(int bank, int slot) {
    if (bank != int(project_.bank) || slot != int(project_.slot)) return;
    undo_.clear();               // the history belonged to the steps just replaced
    scheduler_.reset(true);
    reconfigure();
    // DOUBLE changes the master length, and so may a copy: the boundary is measured again, or
    // a chain entry ends where the pattern's old length did.
    boundary_known_ = false;
    deferred_count_ = 0;
    span_clear();   // and the rolls of spanning ratchets
}

void Engine::switch_pattern(int bank, int slot) {
    if (!project_index_valid(bank, slot)) return;
    if (bank == int(project_.bank) && slot == int(project_.slot)) return;

    project_.bank = uint8_t(bank);
    project_.slot = uint8_t(slot);

    // The undo history belonged to the pattern just left. Restoring it over this one would
    // overwrite a different pattern with another's steps, which core/undo.hpp's clear() exists
    // to prevent -- its own header asks for exactly this call.
    undo_.clear();
    // And LOCK's swap-back, for the same reason: it holds the pattern just left.
    locks_.pattern_changed();

    // A pattern change is what the 1ST condition tests for, and the scheduler's PRE/NEI chain
    // was resolved against a pattern that is no longer playing.
    scheduler_.reset(true);
    reconfigure();
    clear_slides();

    // Ratchet hits still waiting belong to a step of the pattern just left. Fired later, they
    // would read THIS pattern's locks and chord at that step index (`fire` reads the live
    // pattern), which is a hit of neither pattern.
    deferred_count_ = 0;
    span_clear();   // and the rolls of spanning ratchets

    // The new pattern's master length makes the old boundary meaningless, and deriving the new
    // one needs a tick, which only resolve_tick has.
    boundary_known_ = false;
}

// A bar started. The only thing that waits for one is a mute change armed for it.
// Cut a track dead: its voices, and its insert chain's filter and lo-fi state. A track stops
// being rendered the moment its machine goes idle, so the chain is frozen with whatever energy
// its filters held at the cut -- harmless after a natural decay, but after a kill it was a
// stored transient that played out on the track's next note (a kick at LVL 0 came out at 0.42).
void Engine::silence_track(int track) {
    if (machines_[track] != nullptr) machines_[track]->kill_all();
    mixer_.chain(track).clear_state();
    // Its MIDI notes too: every caller -- a mute, a machine switch, a kit recall or load, the
    // second STOP -- is a case where an outboard synth must stop with the track.
    midi_out_.release_track(midi_sink_, track);
}

// Mutes change here and nowhere else. A muted track is not rendered at all (that is the CPU it
// saves), so a voice left in its machine would freeze mid-note and resume on unmute. Newly muted
// tracks are therefore silenced on the spot.
void Engine::apply_mutes(uint8_t state) {
    for (int t = 0; t < kTracks; t++) {
        const bool now = (state >> t & 1u) != 0;
        if (now && !muted_[t]) silence_track(t);
    }
    mute_state_apply(state, muted_);
    // PROJECT.BIN's copy, so a save keeps the mutes as they stand (core/project.hpp).
    project_.settings.live_mutes = mute_state_capture(muted_);
}

void Engine::on_bar() {
    uint8_t state = 0;
    if (mute_latch_release(mute_latch_, state)) apply_mutes(state);

    // And an armed take starts HERE, on the bar line, which is the whole reason resampling has
    // an armed state at all: a take that begins under the user's finger does not loop, and a
    // round number of bars is what makes it a loop. dsp/capture.hpp keeps the bar out of the
    // buffer deliberately -- it only knows how to be armed and then to run for exactly as long
    // as it was told -- so this is the one place that decides when.
    //
    // Where the bar line is, in frames from this block's start: the tick's frame, less however
    // far into the bar the tick is. That is 0 ticks on an ordinary bar line; the first tick after
    // a start or a relocate may land mid-bar, and then the bar line was before this block.
    const uint32_t into = tick_now_ - bar_ * kTicksPerBar;
    const int32_t bar_line = int32_t(tick_frame_) - int32_t(float(into) * samples_per_tick_);

    const bool was_armed = capture_.armed();
    capture_.begin();
    if (was_armed && capture_.recording()) {
        capture_bar_ = bar_;
        capture_lead_ = bar_line;        // the take records this whole block, from frame 0
    }

    // Frozen tracks come back into time on their own bars (dsp/frozen.hpp), and so does the
    // master looper, which also starts an armed take here.
    for (int t = 0; t < kTracks; t++) frozen_[t].on_bar(bar_, bar_line);
    looper_.on_bar(bar_, bar_line);
}

void Engine::cross_boundary(uint32_t boundary) {
    if (!chain_.active) return;

    // Song mode: the last pass of the last entry has ended. The song goes back to entry 1 for
    // the next PLAY and is silent until then; the main context sees `song_end` and stops the
    // transport (only it can). Releasing here rather than waiting for its Cmd::stop is what
    // keeps the step on this boundary from sounding in the block or two before that arrives.
    if (project_.settings.chain_song && chain_at_end(project_, chain_)) {
        if (chain_start(project_, chain_)) {
            switch_pattern(int(project_.chain[chain_.entry].bank),
                           int(project_.chain[chain_.entry].slot));
            chain_entry_started();
        }
        origin_ = boundary;
        song_over_ = true;
        for (int t = 0; t < kTracks; t++) {
            if (machines_[t]) machines_[t]->release_all();
            seq_arp_[t].on = false;
        }
        deferred_count_ = 0;
        span_clear();   // and the rolls of spanning ratchets
        status_.bump_song_end();
        return;
    }

    uint8_t bank = 0, slot = 0;
    // chain_advance reports a PATTERN change, not an entry change, so a chain that repeats one
    // pattern returns false here and nothing is switched -- which is what keeps 1ST from firing
    // on every pass of a one-pattern chain (core/queue.hpp).
    if (chain_advance(project_, chain_, bank, slot)) {
        switch_pattern(bank, slot);
        // The next entry starts on the boundary the last one ended on, at its step 1.
        origin_ = boundary;
    }
    // A new entry (or a one-entry chain's next pass of it) recalls its mutes on the boundary.
    if (chain_.played == 0) chain_entry_started();
}

// A chain entry has just started: its mute recall, if it has one, now (song mode).
void Engine::chain_entry_started() {
    const int state = chain_entry_mutes(project_, chain_);
    if (state >= 0) apply_mutes(project_.settings.mute_states[state]);
}

Project &Engine::project_for_load() { return project_; }

// Rebuild the scheduler's derived timing after anything that changes a track's length, speed or
// swing -- including an undo, which can change all three at once.
//
// It exists so that `samples_per_tick_` cannot be forgotten. Calling configure() with its
// default leaves the scheduler with no idea how long a step is, and the only visible effect is
// that ratchets stop being spread across the step: every hit lands on the step's own frame. That
// was real, from M2 -- changing a track's length flattened its ratchets until the next tempo
// change, which is about as quiet as a bug gets.
void Engine::reconfigure() {
    scheduler_.configure(live(), samples_per_tick_);
}

// --- the tick -------------------------------------------------------------

void Engine::resolve_tick(uint32_t tick, uint32_t frame) {
    tick_now_ = tick;            // for on_bar(): where this bar line is, in frames
    tick_frame_ = frame;
    // The transport jumped BACK (a stop and start, F1 + F5, or an outside clock relocating). A
    // change still queued was armed for a boundary measured on the old position: left alone it
    // waited until the clock caught up with that tick again -- a change queued at bar 40 and
    // then STOP and PLAY landed forty bars late. The boundary it was waiting for is gone, so it
    // happens now, at this tick, from the new pattern's step 1: the same reading a change queued
    // with the clock stopped gets (Cmd::queue_pattern).
    const bool jumped_back = tick < tick_;
    tick_ = tick;
    // A finished song plays again from a transport that went back (a stop and PLAY).
    if (jumped_back) song_over_ = false;

    // A transport that relocated to before the pattern's start (a START from 0 after a switch)
    // measures from 0 again. Only ever backwards: a CONTINUE resumes past the origin and keeps it.
    if (tick < origin_) {
        origin_ = 0;
        boundary_known_ = false;
    }

    // A queued change takes effect BEFORE the tick is resolved, so this tick plays the new
    // pattern's steps rather than one last step of the old one. Every mode but NOW starts the
    // new pattern at its step 1 on the boundary the change was queued for (core/queue.hpp).
    if (jumped_back && queue_.pending) {
        switch_pattern(queue_.bank, queue_.slot);
        queue_clear(queue_);
        origin_ = tick;
        boundary_known_ = false;
    }

    if (queue_due(queue_, tick)) {
        const bool restart = queue_.restart;
        const uint32_t start = queue_.start_tick;
        switch_pattern(queue_.bank, queue_.slot);
        queue_clear(queue_);
        if (restart) {
            origin_ = start <= tick ? start : tick;
            boundary_known_ = false;
        }
    }

    // The end of a pass, for chains. One division per pass of the pattern -- about every two
    // seconds -- rather than one per tick, which is the only reason `next_boundary_` is kept
    // rather than derived.
    // Passes are counted from the pattern's own start, `origin_`, not from tick 0.
    const uint32_t loop = uint32_t(master_loop_ticks(live()));
    if (!boundary_known_) {
        next_boundary_ = origin_ + ((tick - origin_) / loop + 1) * loop;
        boundary_known_ = true;
    } else if (tick >= next_boundary_) {
        const uint32_t crossed = next_boundary_;
        next_boundary_ = origin_ + ((tick - origin_) / loop + 1) * loop;
        cross_boundary(crossed);
    }

    // Bars, for auto-fill and for anything armed to land on one. Counted rather than divided
    // past the first tick: a bar is kTicksPerBar and this chip has no integer divide.
    //
    // A BACKWARDS jump has to resync. A stop and start relocates the clock, and the forward
    // catch-up below would then leave the bar count wrong for as many bars as the jump was long
    // -- which is auto-fill firing in the wrong place for ten bars after every stop. The
    // `tick < next_bar_tick_` guard is what makes the subtraction safe to do unsigned.
    if (bar_known_ && tick < next_bar_tick_ && next_bar_tick_ - tick > kTicksPerBar) {
        bar_known_ = false;
    }

    if (!bar_known_) {
        bar_ = tick / kTicksPerBar;
        next_bar_tick_ = (bar_ + 1) * kTicksPerBar;
        bar_known_ = true;
        on_bar();
    } else if (tick >= next_bar_tick_) {
        bar_++;
        // Mutation-equivalent (docs/MUTATION.md): left out, the resync below finds the same
        // bar by a division. This line is what keeps that division off the ordinary bar.
        next_bar_tick_ += kTicksPerBar;
        // A transport that jumped forward by more than a bar: resync rather than counting up to
        // it one bar per tick.
        if (tick >= next_bar_tick_) {
            bar_ = tick / kTicksPerBar;
            next_bar_tick_ = (bar_ + 1) * kTicksPerBar;
        }
        on_bar();
    }

    // FILL is the pad OR the auto-fill bar. Resolved here rather than in the command, so that
    // turning auto-fill on takes effect at the next bar rather than mid-bar.
    perform_.fill = fill_held_ || auto_fill_active(auto_fill_, bar_);

    // A finished song is silent until the transport stops or starts again.
    if (song_over_) return;

    // The pattern's own tick: steps, passes and conditions all count from where it started.
    const int count =
        scheduler_.tick(tick - origin_, frame, live(), perform_, events_, kMaxEventsPerTick);
    // A stuttering track's own steps are silent: the scheduler still resolved them, so its
    // playhead, conditions and passes run on and the release lands back in the beat.
    const uint32_t silenced = stutter_step_ >= 0 ? uint32_t(stutter_tracks_) : 0u;
    for (int i = 0; i < count; i++) {
        if ((silenced >> events_[i].track & 1u) != 0) continue;
        dispatch(events_[i], block_frames_);
    }

    for (int t = 0; t < kTracks; t++) status_.publish_playhead(t, scheduler_.playhead(t));
}

void Engine::publish_playheads() {
    // Every block, not only on a tick. A STOP sends the scheduler's playheads back to 0, and
    // publishing them only from `resolve_tick` left Status holding the step the transport
    // stopped on -- so the grid kept that step lit until the transport ran again. Eight relaxed
    // stores of one byte; the tick path still publishes too, which is harmless and keeps a
    // step's own frame as the moment the playhead moves.
    for (int t = 0; t < kTracks; t++) status_.publish_playhead(t, scheduler_.playhead(t));
}

void Engine::publish_waves() {
    // A few multiplies per track that plays a sample, and nothing for any other: the screen
    // draws the selected track's, and which that is belongs to the main context.
    if (++wave_block_ < kWaveEveryBlocks) return;
    wave_block_ = 0;
    for (int t = 0; t < kTracks; t++) {
        uint8_t stage = 0;
        float level = 0.0f;
        if (machines_[t] == nullptr || !machines_[t]->envelope_now(stage, level)) stage = 0;
        const float scaled = level * 255.0f + 0.5f;
        status_.env_stage[t].store(stage, std::memory_order_relaxed);
        status_.env_level[t].store(stage == 0 ? 0u : uint8_t(scaled > 255.0f ? 255.0f
                                                             : (scaled < 0.0f ? 0.0f : scaled)),
                                   std::memory_order_relaxed);

        const uint8_t id = machine_id_[t];
        if (id != uint8_t(MachineId::sample) && id != uint8_t(MachineId::grain)) continue;
        // An idle machine was last prepared when it last played, so its window would lag the
        // knobs. Through the memo: nothing is recomputed unless a byte it reads has moved, and
        // a prepare is a pure function of those bytes, so what plays is unchanged.
        if (!frozen_[t].on()) prepare_machine(t);
        WaveView view;
        if (id == uint8_t(MachineId::sample)) {
            pool_[t].sampler.wave_view(view);
        } else if (id == uint8_t(MachineId::grain)) {
            pool_[t].grain.wave_view(view);
        } else {
            continue;
        }
        status_.wave[t].publish(view);
    }
}

void Engine::dispatch(const StepEvent &event, uint32_t frames) {
    // In this block: fire it, and `fire` hands the machine a frame it can schedule on.
    if (event.frame < frames) {
        fire(event);
        return;
    }

    // Past it: hold it, with its frame measured from the START of the next block. See
    // app/engine.hpp on why this is here and what it cost not to be.
    if (deferred_count_ >= kMaxDeferredEvents) {
        status_.bump_refused();   // the newest goes; the oldest is nearest to sounding
        return;
    }
    StepEvent &held = deferred_[deferred_count_++];
    held = event;
    held.frame -= frames;
}

// --- spanning ratchets ----------------------------------------------------

void Engine::span_start(int track, const StepEvent &event) {
    SpanRun &run = span_[track];
    run.on = false;
    if (event.span_ticks == 0 || event.span_hits < 2) return;
    if (arp_settings(track).on()) return;           // the arp takes the step
    run.rate = event.span_ticks;
    run.ramp = RatchetRamp(event.span_ramp);
    run.velocity = live().tracks[track].steps[event.step].velocity;
    run.hit = 1;
    run.count = event.span_hits;
    run.inv_count = 1.0f / float(event.span_hits);  // once a roll, not a hit
    run.start_tick = tick_;
    run.next_tick = tick_ + run.rate;
    run.base = event;
    run.base.ratchet_index = 1;
    run.on = true;
}

// On every tick: each roll whose next hit is due fires it on the tick's frame. A track that is
// stuttering keeps counting silently, as its steps do.
void Engine::span_tick(uint32_t tick, uint32_t frame) {
    const uint32_t silenced = stutter_step_ >= 0 ? uint32_t(stutter_tracks_) : 0u;
    for (int t = 0; t < kTracks; t++) {
        SpanRun &run = span_[t];
        if (!run.on) continue;
        if (tick < run.start_tick) {                // the transport went back: the roll is gone
            run.on = false;
            continue;
        }
        if (tick < run.next_tick) continue;
        StepEvent hit = run.base;
        hit.frame = frame;
        hit.velocity = span_velocity(run.velocity, run.hit, run.count, run.inv_count, run.ramp);
        run.next_tick += run.rate;
        run.hit++;
        if (run.hit >= run.count) run.on = false;
        if ((silenced >> t & 1u) == 0) fire(hit);
    }
}

// --- note repeat ---------------------------------------------------------

float Engine::roll_period_frames() const {
    return float(repeat_length_ticks(repeat_length_)) * samples_per_tick_;
}

// Half the period: a POLY or MONO roll is a string of notes, not one held note retriggering.
uint32_t Engine::roll_gate_frames() const {
    const float half = 0.5f * roll_period_frames();
    return half < 1.0f ? 1u : uint32_t(half);
}

void Engine::roll_fire(int track, uint32_t frame) {
    // Not on a muted track, for the reason Cmd::trigger_note gives.
    if (machines_[track] == nullptr || muted_[track]) return;
    const Roll &roll = rolls_[track];
    Trig trig;
    trig.note = roll.note;
    trig.velocity = roll.velocity;
    trig.frame = frame;
    trig.gate_frames = roll_gate_frames();
    if (make_room_for_poly_voice(track, trig.note)) machines_[track]->trigger(trig);
    note_started(track, trig.velocity, frame);
    send_midi_notes(track, &trig.note, 1, trig.velocity, frame, midi_gate(track, trig.gate_frames));
}

// The transport is running: hit on the grid of beat repeat's length, counted from the clock's
// zero so a roll lands on the same ticks as the pattern's steps. One modulo per tick, not per
// track -- this chip has no integer divide, and a tick is control rate.
void Engine::roll_tick(uint32_t tick, uint32_t frame) {
    bool any = false;
    for (int t = 0; t < kTracks; t++) any = any || rolls_[t].on;
    if (!any) return;
    const uint32_t length = uint32_t(repeat_length_ticks(repeat_length_));
    if (tick % length != 0) return;
    for (int t = 0; t < kTracks; t++) {
        Roll &roll = rolls_[t];
        if (!roll.on) continue;
        // The press already hit: a grid tick less than half a length after it would be a flam.
        if (tick >= roll.last_tick && tick - roll.last_tick < (length >> 1)) continue;
        roll.last_tick = tick;
        roll_fire(t, frame);
        roll.countdown = roll_period_frames();
    }
}

// The transport is stopped: a free roll from the press, at the tempo's length in frames.
void Engine::roll_free(uint32_t frames) {
    const float period = roll_period_frames();
    for (int t = 0; t < kTracks; t++) {
        Roll &roll = rolls_[t];
        if (!roll.on) continue;
        if (period < 1.0f) continue;              // the tempo has not settled: the press only
        if (roll.countdown > period) roll.countdown = period;   // the length got shorter
        while (roll.countdown < float(frames)) {
            const float at = roll.countdown < 0.0f ? 0.0f : roll.countdown;
            roll_fire(t, uint32_t(at));
            roll.countdown += period;
        }
        roll.countdown -= float(frames);
    }
}

// --- step stutter -------------------------------------------------------

// One hit of the stuttered step on every stuttering track: the step as written
// (core/scheduler.hpp's stutter_event), through `dispatch`, so its locks apply and a muted
// track stays silent exactly as for a sequenced step.
void Engine::stutter_fire(uint32_t frame) {
    for (int t = 0; t < kTracks; t++) {
        if ((stutter_tracks_ >> t & 1u) == 0) continue;
        StepEvent event;
        if (stutter_event(live(), t, stutter_step_, frame, event)) dispatch(event, block_frames_);
    }
}

// The transport is running: on the grid of beat repeat's length, as a note repeat hits, and
// with the same guard against a flam just after the press.
void Engine::stutter_tick(uint32_t tick, uint32_t frame) {
    if (stutter_step_ < 0) return;
    const uint32_t length = uint32_t(repeat_length_ticks(repeat_length_));
    if (tick % length != 0) return;
    if (tick >= stutter_last_tick_ && tick - stutter_last_tick_ < (length >> 1)) return;
    stutter_last_tick_ = tick;
    stutter_fire(frame);
    stutter_countdown_ = roll_period_frames();
}

// The transport is stopped: free from the press, at the tempo's length in frames.
void Engine::stutter_free(uint32_t frames) {
    if (stutter_step_ < 0) return;
    const float period = roll_period_frames();
    if (period < 1.0f) return;
    if (stutter_countdown_ > period) stutter_countdown_ = period;
    while (stutter_countdown_ < float(frames)) {
        const float at = stutter_countdown_ < 0.0f ? 0.0f : stutter_countdown_;
        stutter_fire(uint32_t(at));
        stutter_countdown_ += period;
    }
    stutter_countdown_ -= float(frames);
}

// --- the arpeggiator ------------------------------------------------------

ArpSettings Engine::arp_settings(int track) const {
    ArpSettings s = ArpSettings::make(&effective_[track].v[kArpFirst]);
    s.var = ArpVar(project_.kit.tracks[track].arp.var < kArpVars ? project_.kit.tracks[track].arp.var
                                                                  : 0);
    return s;
}

namespace {

// 1/n for a ratchet's n hits: a multiply, not a divide, at control rate.
constexpr float kInverseCount[kArpMaxRatchet + 1] = {1.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f};
// A block's length as a reciprocal: how many blocks a spacing holds.
constexpr float kInverseBlock = 1.0f / float(ENAGA_BLOCK_FRAMES);

}  // namespace

// A seed for a new run's RND notes and chance ratchets: the track and step, the clock's tick and
// a count of runs, so no two runs draw the same, whether the transport runs or not.
uint32_t Engine::arp_new_seed(int track, int step) {
    arp_runs_++;
    return (uint32_t(track) * 131u + uint32_t(step)) ^ (arp_runs_ * 0x9e3779b9u) ^
           (tick_ * 0x85ebca6bu);
}

// One slot of the arp's grid: a rest (RHYTHM), or the next note of the cycle -- accented by
// ACCENT or the step's own ACC on its first note -- and, on a RATCHET slot, its repeats queued
// for `arp_ratchets`.
void Engine::arp_hit(int track, ArpRun &run, uint32_t frame) {
    const KitArp &ext = project_.kit.tracks[track].arp;
    const uint32_t slot = run.slot++;
    run.ratchet_left = 0;                 // a new slot ends the last one's repeats
    if (!arp_rhythm_plays(ext.rhythm, slot)) return;
    const uint32_t note_hit = run.hit;
    const uint8_t note = arp_note(run.cycle, run.length, run.mode, note_hit, run.seed);
    run.hit++;
    const bool accent =
        (run.accent && note_hit == 0) || arp_accent_on(ext.accent, slot, note_hit, run.length);
    // The hit's spacing, shared by its ratchet; its gate GATE's share of that. Float multiplies
    // at control rate.
    const float spacing = float(run.rate) * samples_per_tick_;
    int count = arp_ratchet_count(ext.ratchet, slot, run.seed);
    // Never two hits of one track in a block (the one-pending-trig contract): fewer repeats when
    // the slot is too short for them, as a 1/32T at a fast tempo is.
    const int fits = int(spacing * kInverseBlock);
    if (count > fits) count = fits < 1 ? 1 : fits;
    const float each = spacing * kInverseCount[count];
    const float gate = each * float(run.gate_percent) * 0.01f;
    const uint32_t gate_frames = gate < 1.0f ? 1u : uint32_t(gate);
    arp_play(track, note, run.velocity, accent, frame, gate_frames);
    if (count > 1) {
        run.ratchet_left = uint8_t(count - 1);
        run.ratchet_note = note;
        run.ratchet_gate = gate_frames;
        run.ratchet_spacing = each;
        // From the start of the block `arp_ratchets` will count it down in next.
        run.ratchet_countdown =
            float(frame) + each - (arp_ratchets_done_ ? float(block_frames_) : 0.0f);
    }
}

// One note of an arp, a slot's or a ratchet's: to the machine and MIDI OUT.
void Engine::arp_play(int track, uint8_t note, uint8_t velocity, bool accent, uint32_t frame,
                      uint32_t gate_frames) {
    if (machines_[track] == nullptr || muted_[track]) return;
    Trig trig;
    trig.note = note;
    trig.velocity = velocity;
    trig.accent = accent;
    trig.frame = frame;
    trig.gate_frames = gate_frames;
    if (make_room_for_poly_voice(track, note)) machines_[track]->trigger(trig);
    note_started(track, trig.velocity, frame);   // each hit lights the track, as a step does
    // Each hit to MIDI OUT too, with its own gate; a MID track's CCs go ahead of the note.
    if (machine_id_[track] == uint8_t(MachineId::midi)) send_midi_ccs(track);
    send_midi_notes(track, &trig.note, 1, accent ? uint8_t(127) : velocity, frame, gate_frames);
}

// The repeats of ratcheted slots that land in this block. A run that ended (its step's length,
// a stop, OFF) takes its repeats with it.
void Engine::arp_ratchets(uint32_t frames) {
    for (int t = 0; t < kTracks; t++) {
        ArpRun *runs[2] = {&seq_arp_[t], &held_arp_[t]};
        for (ArpRun *run : runs) {
            if (run->ratchet_left == 0) continue;
            if (!run->on) {
                run->ratchet_left = 0;
                continue;
            }
            if (run->ratchet_countdown < float(frames)) {
                const float at = run->ratchet_countdown < 0.0f ? 0.0f : run->ratchet_countdown;
                arp_play(t, run->ratchet_note, run->velocity, false, uint32_t(at), run->ratchet_gate);
                run->ratchet_left--;
                run->ratchet_countdown += run->ratchet_spacing;
            }
            run->ratchet_countdown -= float(frames);
        }
    }
    arp_ratchets_done_ = true;
}

void Engine::arp_tick(uint32_t tick, uint32_t frame) {
    for (int t = 0; t < kTracks; t++) {
        ArpRun *runs[2] = {&seq_arp_[t], &held_arp_[t]};
        for (ArpRun *run : runs) {
            if (!run->on) continue;
            if (run == &held_arp_[t]) arp_follow(t);
            if (!run->on) continue;
            // (Written as a sum: next_tick - rate would wrap below a run started near tick 0.)
            if (tick >= run->end_tick || tick + run->rate < run->next_tick) {
                // Past the step's length -- or the transport jumped back past the run's start.
                if (run == &seq_arp_[t]) {
                    run->on = false;
                    continue;
                }
                run->next_tick = tick;                // a held run just picks the clock up again
            }
            if (tick < run->next_tick) continue;
            arp_hit(t, *run, frame);
            run->next_tick = tick + run->rate;
            run->countdown = float(run->rate) * samples_per_tick_;
        }
    }
}

// Held runs with the transport stopped: free from the first key, at the tempo's spacing.
void Engine::arp_free(uint32_t frames) {
    for (int t = 0; t < kTracks; t++) {
        ArpRun &run = held_arp_[t];
        if (!run.on) continue;
        arp_follow(t);
        if (!run.on) continue;
        const float spacing = float(run.rate) * samples_per_tick_;
        if (spacing < 1.0f) continue;
        if (run.countdown > spacing) run.countdown = spacing;
        while (run.countdown < float(frames)) {
            const float at = run.countdown < 0.0f ? 0.0f : run.countdown;
            arp_hit(t, run, uint32_t(at));
            run.countdown += spacing;
        }
        run.countdown -= float(frames);
    }
}

// A held run follows the ARP page while the keys are down: RAT and GAT at once, MOD and OCT by
// rebuilding the cycle, and MOD turned to OFF ends it. Four byte reads a track a tick, and the
// rebuild only when something it depends on moved.
void Engine::arp_follow(int track) {
    ArpRun &run = held_arp_[track];
    const ArpSettings arp = arp_settings(track);
    if (!arp.on()) {
        run.on = false;
        held_count_[track] = 0;
        return;
    }
    run.rate = arp.rate_ticks;
    run.gate_percent = arp.gate_percent;
    if (arp.mode != run.mode || arp.octaves != run.octaves || arp.var != run.var) {
        run.mode = arp.mode;
        run.octaves = arp.octaves;
        run.var = arp.var;
        run.length = uint8_t(arp_cycle(held_notes_[track], held_count_[track], arp, run.cycle));
        if (run.length == 0) run.on = false;
    }
}

// The held KEYS notes changed: rebuild the cycle, start the run on the first key and stop it on
// the last. The hit count carries on, so adding a note does not restart the pattern.
void Engine::arp_held_changed(int track) {
    ArpRun &run = held_arp_[track];
    const ArpSettings arp = arp_settings(track);
    run.length = uint8_t(arp_cycle(held_notes_[track], held_count_[track], arp, run.cycle));
    if (run.length == 0) {
        run.on = false;
        return;
    }
    run.mode = arp.mode;
    run.octaves = arp.octaves;
    run.var = arp.var;
    run.rate = arp.rate_ticks;
    run.gate_percent = arp.gate_percent;
    run.end_tick = 0xFFFFFFFFu;
    if (run.on) return;
    run.on = true;
    run.hit = 0;
    run.slot = 0;
    run.seed = arp_new_seed(track, 7);
    run.accent = false;
    // The first hit now; then on the grid of the rate while running, or free while stopped.
    arp_hit(track, run, 0);
    const uint32_t r = run.rate;
    run.next_tick = tick_ - tick_ % r + r;
    run.countdown = float(r) * samples_per_tick_;
}

void Engine::start_slides(int track, const StepEvent &event) {
    slide_count_[track] = 0;
    if (event.glide_frames == 0u) return;
    const Pattern &pattern = live();
    const LockView locks = pattern_step_locks(pattern, track, int(event.step));
    // One reciprocal per step at control rate; the per-block move is a multiply.
    const float step = enaga::dsp::reciprocal(float(event.glide_frames));
    uint8_t release[2];
    const int releases = rel_all_ != 0 ? machine_release_params(machine_id_[track], release) : 0;
    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        if (!lock_slides(lock->param)) continue;
        const uint8_t param = lock_param(lock->param);
        if (param >= kTrackParams || slide_count_[track] >= kLocksPerStep) continue;
        // What the step glided to will play: its lock and range, or the sound it starts from.
        uint8_t to = lock_resolve(pattern, track, int(event.glide_step), event.glide_loop,
                                  int(param), performed_[track][param]);
        // REL ALL moves a locked release (fire, above), so the target's too.
        uint8_t ignored = 0;
        for (int i = 0; i < releases; i++) {
            if (release[i] == param &&
                pattern_get_lock(pattern, track, int(event.glide_step), param, ignored)) {
                to = rel_all_byte(to, rel_all_);
            }
        }
        const uint8_t from = effective_[track].v[param];
        if (to == from) continue;
        ParamSlide &slide = slides_[track][slide_count_[track]++];
        slide.param = param;
        slide.from = from;
        slide.to = to;
        slide.position = 0.0f;
        slide.step = step;
    }
}

void Engine::run_slides(uint32_t frames) {
    for (int t = 0; t < kTracks; t++) {
        const int count = slide_count_[t];
        if (count == 0) continue;
        int keep = 0;
        for (int i = 0; i < count; i++) {
            ParamSlide slide = slides_[t][i];
            // Only while the step still has it locked: a stop or a pattern change unmasks it.
            if ((locked_[t] >> slide.param & 1u) == 0u) continue;
            slide.position += float(frames) * slide.step;
            const bool arrived = slide.position >= 1.0f;
            const float at = arrived ? 1.0f : slide.position;
            const float value = float(slide.from) + (float(slide.to) - float(slide.from)) * at;
            effective_[t].v[slide.param] = uint8_t(value + 0.5f);
            if (!arrived) slides_[t][keep++] = slide;
        }
        slide_count_[t] = uint8_t(keep);
    }
}

void Engine::deliver_deferred(uint32_t frames) {
    // Oldest first, and compacted in one pass: an event that fires is removed and the rest keep
    // their order, so a ratchet's hits stay in the order the scheduler produced them.
    int keep = 0;
    for (int i = 0; i < deferred_count_; i++) {
        StepEvent &event = deferred_[i];
        if (event.frame < frames) {
            fire(event);
            continue;
        }
        event.frame -= frames;
        deferred_[keep++] = event;
    }
    deferred_count_ = keep;
}

void Engine::fire(const StepEvent &event) {
    const int track = int(event.track);
    if (track < 0 || track >= kTracks) return;

    // Rebuild this track's effective parameters: the kit, then this step's locks over the top.
    // Starting from the kit each time is what makes a lock last exactly one step -- a step with
    // no lock on a parameter gets the kit value back, with no bookkeeping to go wrong.
    //
    // Locks apply whether or not a note sounds: that is the whole point of a trigless step, and
    // why this happens before the trig check.
    // From `performed_`, not `params_`: the morph, the macros and any throw are part of the
    // sound a step starts from, and only the step's own locks go over the top of them.
    for (int p = 0; p < kTrackParams; p++) effective_[track].v[p] = performed_[track][p];

    const LockView locks = pattern_step_locks(live(), track, int(event.step));
    int applied = 0;
    uint32_t locked = 0;
    // The values first, then the random ranges over them (core/pattern.hpp's lock byte): a range
    // spreads the step's lock, or the sound the step starts from when it has none. The slide bit
    // is read by start_slides below.
    uint32_t ranged = 0;
    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        const uint8_t param = lock_param(lock->param);
        if (param >= kTrackParams || applied >= kMaxLocksApplied) continue;
        applied++;
        if (lock_is_range(lock->param)) {
            ranged = ranged | (1u << param);
            continue;
        }
        effective_[track].v[param] = lock->value;
        locked = locked | (1u << param);
    }
    if (ranged != 0u) {
        const uint32_t seed = live().seed;
        for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
            const uint8_t param = lock_param(lock->param);
            if (!lock_is_range(lock->param) || param >= kTrackParams) continue;
            effective_[track].v[param] =
                lock_spread(effective_[track].v[param], lock->value, seed, track,
                            int(event.step), event.loop, int(param));
        }
        locked = locked | ranged;
    }
    // Which parameters this step has locked, so the performance layer leaves them alone until
    // the next step. A lock always wins over a morph.
    locked_[track] = locked;
    // REL ALL moves a locked release too: it is relative to what the step plays, and a lock is
    // that (core/moveall.hpp). An unlocked one came moved from `performed_`.
    if (rel_all_ != 0 && locked != 0u) {
        uint8_t release[2];
        const int n = machine_release_params(machine_id_[track], release);
        for (int i = 0; i < n; i++) {
            if ((locked >> release[i] & 1u) == 0) continue;
            effective_[track].v[release[i]] = rel_all_byte(effective_[track].v[release[i]], rel_all_);
        }
    }

    // Parameter slide: the first hit of a step (re)starts its track's glides; a ratchet's later
    // hits leave them running, so a slid step glides across its own ratchet.
    if (event.ratchet_index == 0) start_slides(track, event);
    // A trig ends the track's running roll, and a spanning step starts its own.
    if (!event.trigless && event.ratchet_index == 0) span_start(track, event);

    if (!event.trigless && machines_[track] != nullptr && !muted_[track]) {
        // This prepare is REDUNDANT today, and kept on purpose. It used to be justified as
        // "prepare before triggering so the voice starts from the locked values", which
        // described a data flow this engine does not have: `audio()` resolves every tick BEFORE
        // the render loop, and the render loop prepares each sounding track from the same
        // `effective_` this function has just written -- so the locked values are in place
        // before a single sample is produced, with or without this line. Removing it changed
        // nothing in 993 tests and 539,519 checks, which is what a redundant control-rate
        // `Settings::make` per trig looks like.
        //
        // What it is really defending is the invariant that no machine's `trigger` reads a
        // prepared setting: every one of them only stores the Trig and sets `active_`
        // (dsp/vapoly.cpp, dsp/va.cpp, dsp/kick.cpp and the rest). A machine that broke that
        // would be silently wrong for its first block without this, and a few hundred cycles
        // per trig is a cheap price for not having to notice. The comment, not the call, was
        // the thing that needed fixing.
        //
        // It goes through the memo, which keeps the invariant: when the memo skips, the machine
        // already holds settings prepared from these exact bytes, for this machine, since the
        // last command -- so a lock, a machine switch, a sample swap or a project load still
        // prepares here, and an unchanged step costs a 16-byte compare instead.
        prepare_machine(track);

        // The note is folded into the key on a MELODIC track and left alone on every other
        // kind; `machine_folds_note` (core/project.hpp) says which, and why. Both VAs and GRAIN
        // fold, SAMPLE folds only while unsliced -- its slice count is read after the prepare
        // above, so a locked or scene-moved SLC counts -- and the drums never do.
        //
        // History: `core/chord`'s `chord_expand` folds the step's note even for a type-`none`
        // chord, so before this the same step data played folded on a POLY track and as written
        // on a MONO one.
        const Pattern &folding = live();
        const uint8_t key_root_now = key_root();
        const int key_scale_now = int(key_scale());
        const int slices = pool_[track].sampler.slices();
        const uint8_t note = machine_folds_note(machine_id_[track], slices)
                                 ? scale_fold(event.note, key_root_now, key_scale_now)
                                 : event.note;
        sent_note_[track] = note;

        Trig trig;
        trig.note = note;
        trig.velocity = event.velocity;
        trig.accent = event.accent;
        trig.slide = event.slide;
        trig.frame = event.frame;
        // LEN ALL (core/moveall.hpp) on the step's gate; 0 and "no gate" come back unchanged.
        trig.gate_frames = len_all_frames(gate_frames_for(track, event.length_index), len_all_);
        // A roll's hit lasts half its rate, as a note repeat's does: LEN is the whole roll's.
        if (event.span_ticks != 0) {
            const float half = 0.5f * float(event.span_ticks) * samples_per_tick_;
            trig.gate_frames = half < 1.0f ? 1u : uint32_t(half);
        }

        // MIDI out's copy of the trig. An accent is the top velocity, the nearest thing MIDI
        // has, and a step with no gate lasts a step, because a note-on needs a note-off.
        const uint8_t midi_velocity = event.accent ? uint8_t(127) : event.velocity;
        const uint32_t midi_gate_frames = midi_gate(track, trig.gate_frames);
        const ArpSettings arp = arp_settings(track);
        if (arp.on()) {
            // The arp takes the step: its notes (the chord on POLY, the one note elsewhere) become
            // a cycle walked at the arp's rate for the step's length -- or until the next step,
            // for a step with no length. The first hit is now, on the step's own frame.
            uint8_t input[kChordMaxNotes];
            int count = 1;
            input[0] = note;
            const uint8_t packed = folding.tracks[track].steps[event.step].chord;
            if (polys_[track] != nullptr) {
                count = chord_expand(event.note, packed, key_root_now, key_scale_now, input,
                                     PolyMachine::kVoices);
                count = with_extra_notes(folding, track, event.step, input, count, key_root_now,
                                         key_scale_now, true, PolyMachine::kVoices);
            } else if (machine_id_[track] == uint8_t(MachineId::midi)) {
                // MID sends a chord step as its chord (below), so its arp runs through the
                // chord too, rather than repeating the root alone.
                const bool chord = (packed & kChordTypeMask) != uint8_t(ChordType::none);
                if (chord) {
                    count = chord_expand(event.note, packed, key_root_now, key_scale_now, input,
                                         kChordMaxNotes);
                }
                count = with_extra_notes(folding, track, event.step, input, count, key_root_now,
                                         key_scale_now, chord, kChordMaxNotes);
            }
            ArpRun &run = seq_arp_[track];
            run.mode = arp.mode;
            run.var = arp.var;
            run.rate = arp.rate_ticks;
            run.gate_percent = arp.gate_percent;
            run.length = uint8_t(arp_cycle(input, count, arp, run.cycle));
            run.velocity = event.velocity;
            run.accent = event.accent;
            run.hit = 0;
            run.slot = 0;
            // A new draw every run: RND and the chance ratchets are random on every pass, not one
            // fixed pattern (Roland, 2026-11-27).
            run.seed = arp_new_seed(track, event.step);
            run.next_tick = tick_ + run.rate;
            const uint32_t gate = trig.gate_frames;
            run.end_tick = 0xFFFFFFFFu;
            if (gate > 0 && samples_per_tick_ > 0.0f) {
                // Rounded, and exclusive: a one-step length at 1/32 is the hits on 0 and 12, and not
                // a third on the next step's own tick.
                run.end_tick = tick_ + uint32_t(float(gate) * inv_samples_per_tick_ + 0.5f);
            }
            run.on = run.length > 0;
            if (run.on) arp_hit(track, run, event.frame);
            if (track == duck_source_) mixer_.trigger_duck(event.frame);
            return;
        }

        if (polys_[track] != nullptr) {
            // A POLY track plays a CHORD, which is several notes on one frame rather than one
            // call carrying several notes. That is deliberate and it is why VA POLY's allocator
            // counts a voice busy from the moment it takes a trig: a machine that waited for the
            // trig's frame would give every note of the chord the same voice and play one note.
            //
            // `chord_expand` folds the step's note into the key and stacks the chord's intervals
            // on it, each folded too, and returns them ascending. It is capped at the voices
            // this machine actually has, and the cap applies BEFORE inverting, so a six-voice
            // eleventh comes out as the bottom six notes of that chord rather than six notes
            // from its middle (core/chord.hpp says why). A chord type of `none` expands to one
            // note -- the folded root -- so an ordinary melodic POLY part needs no special case
            // here, and gets the fold the comment above says a MONO part does not.
            const uint8_t packed = folding.tracks[track].steps[event.step].chord;
            uint8_t notes[kChordMaxNotes];
            // `event.note` and not `note`: `chord_expand` folds the root itself, so handing it
            // an already-folded note would be a second fold -- harmless, since folding a note
            // that is in the scale returns it, but it would hide which side owns the rule.
            int voiced = chord_expand(event.note, packed, key_root_now, key_scale_now, notes,
                                      PolyMachine::kVoices);
            // And the notes that were played into the step beside its own (core/stepnotes.hpp),
            // folded like the rest of the chord, within the same six voices.
            voiced = with_extra_notes(folding, track, event.step, notes, voiced, key_root_now,
                                      key_scale_now, true, PolyMachine::kVoices);
            for (int i = 0; i < voiced; i++) {
                trig.note = notes[i];
                // The budget across every POLY track (kPolyVoiceBudget, engine.hpp).
                if (!make_room_for_poly_voice(track, trig.note)) continue;
                machines_[track]->trigger(trig);
            }
            // MIDI out gets the whole chord: the budget is about this box's CPU, and an
            // outboard synth's voices are not ours to count.
            send_midi_notes(track, notes, voiced, midi_velocity, trig.frame, midi_gate_frames);
        } else if (machine_id_[track] == uint8_t(MachineId::midi)) {
            // MID plays nothing here. A step with a chord sends the chord, folded and voiced
            // exactly as VA POLY would play it; a plain step sends its note as written.
            // The step's locks first, as CCs, so the synth plays this note with them -- the
            // per-block pass would send them after the note-on, a block too late for its attack.
            send_midi_ccs(track);
            const uint8_t packed = folding.tracks[track].steps[event.step].chord;
            uint8_t notes[kChordMaxNotes];
            int count = 1;
            notes[0] = note;
            const bool chord = (packed & kChordTypeMask) != uint8_t(ChordType::none);
            if (chord) {
                count = chord_expand(event.note, packed, key_root_now, key_scale_now, notes,
                                     kChordMaxNotes);
            }
            // Played-in extra notes: folded with a chord type, as written without one.
            count = with_extra_notes(folding, track, event.step, notes, count, key_root_now,
                                     key_scale_now, chord, kChordMaxNotes);
            send_midi_notes(track, notes, count, midi_velocity, trig.frame, midi_gate_frames);
        } else {
            if (!frozen_[track].on()) machines_[track]->trigger(trig);
            send_midi_notes(track, &note, 1, midi_velocity, trig.frame, midi_gate_frames);
        }

        // Once per step, not once per note: a chord is one trig as far as the screen is
        // concerned, and eight LEDs flashing six times would be a worse display, not a better
        // one.
        note_started(track, trig.velocity, event.frame);

        if (track == duck_source_) mixer_.trigger_duck(event.frame);
    }
}

// A choke group's cut (choke_group in engine.hpp): the fade from where the choke landed, or from
// the block's start when it carries on from the last block, then kill_all once it reaches 0.
void Engine::run_choke(int t, float *mono, uint32_t frames) {
    const uint32_t from = choke_at_[t] < frames ? choke_at_[t] : frames;
    const float gain = strata_choke_fade(mono + from, int(frames - from), choke_gain_[t],
                                         1.0f / float(kChokeFadeFrames));
    if (gain > 0.0f) {
        choke_gain_[t] = gain;
        choke_at_[t] = 0;              // carries on from the next block's first frame
        return;
    }
    machines_[t]->kill_all();
    choke_at_[t] = kNoChokeFrame;
    choke_gain_[t] = 1.0f;
}

// The metronome (engine.hpp): on every beat while CLICK is on, and on every beat of a count-in
// whether it is or not. A bar's first beat is the higher click. Two modulos a tick, and only
// while it clicks: control rate, as roll_tick's one (this chip divides integers in software).
// The tick is the clock's own, and a count-in is whole bars, so the bars line up either way.
void Engine::metronome_tick(uint32_t tick, uint32_t frame) {
    const bool counting = tick < count_in_end_;
    if (!click_on_ && !counting) return;
    const uint32_t beat_ticks = uint32_t(enaga::clock::ppqn);
    if (tick % beat_ticks != 0u) return;
    const bool bar = tick % kTicksPerBar == 0u;
    click_.trigger(frame, bar);
}

// --- the LFOs -------------------------------------------------------------
//
// Once per block, after the locks: each track whose LFO is on gets `modulated_`, its effective
// parameters with the destination byte moved, and `sounding()` hands that to prepare. A track
// whose LFO is off is prepared from `effective_` exactly as before, which is what keeps every
// existing render bit-exact: nothing below runs for it but one compare.

void Engine::run_lfos(uint32_t frames, bool running, uint32_t position, float fraction_in) {
    for (int t = 0; t < kTracks; t++) {
        const uint8_t *bytes = &effective_[t].v[kParams];
        if (__builtin_memcmp(lfo_bytes_[t], bytes, kLfoParams) != 0) {
            for (int i = 0; i < kLfoParams; i++) lfo_bytes_[t][i] = bytes[i];
            lfo_settings_[t] = LfoSettings::make(bytes);
        }
        const LfoSettings &s = lfo_settings_[t];
        // A note since the last block (note_started): taken whether or not the LFO is on now,
        // so a flag cannot linger and restart a cycle on a later block with no note in it.
        const bool restart = lfo_restart_[t];
        lfo_restart_[t] = false;

        float pitch_mod = 0.0f;
        // The mod slots (core/modslot.hpp): either on keeps `modulated_`, and one on LFO keeps
        // the LFO running though its own DEP or DST is off. With neither, as before.
        const KitModSlot *const mod = project_.kit.tracks[t].mod;
        const bool slots = mod[0].on() || mod[1].on();
        const bool lfo_wanted = s.on() || (mod[0].on() && mod[0].source == kModSrcLfo) ||
                                (mod[1].on() && mod[1].source == kModSrcLfo);
        lfo_running_[t] = s.on();
        if (!lfo_wanted && !slots) {
            modulating_[t] = false;
        } else {
            const auto id = MachineId(machine_id_[t]);
            const bool va_filter = machine_has_voice_filter(uint8_t(id));
            const LfoTrig trig = lfo_trig(project_.kit.tracks[t].lfo_trig);
            // The cycle's advance this block: Hz x seconds free, ticks / cycle synced. RETRIG and
            // ONE-SHOT count from the note, so they always advance by the block; FREE synced to a
            // running clock seeks the clock's own position instead.
            const float cycles = s.sync_ticks == 0
                                     ? s.hz * float(frames) * inv_rate_
                                     : float(frames) * inv_samples_per_tick_ * s.inv_sync_ticks;
            float value = 0.0f;
            if (lfo_wanted) {
                if (trig != LfoTrig::free) {
                    // The note starts the cycle on this block's value at phase 0; it moves on from
                    // the next block. A block is 1.3 ms, well inside a note's attack.
                    if (restart) lfo_[t].restart();
                    const float step = restart ? 0.0f : cycles;
                    value = trig == LfoTrig::one_shot ? lfo_[t].one_shot(s.wave, step)
                                                      : lfo_[t].advance(s.wave, step);
                } else if (s.sync_ticks == 0) {
                    value = lfo_[t].advance(s.wave, cycles);
                } else if (running) {
                    // On the clock's own position, so a synced LFO sits on the bar wherever the
                    // song is and follows an external clock. A multiply and a floor rather than
                    // `%`: this chip has no integer divide. A float holds the position to well
                    // under a tick for the first 2^24 ticks, about 22 hours at 130 BPM.
                    const float fraction = fraction_in < 0.0f ? 0.0f
                                           : (fraction_in > 1.0f ? 1.0f : fraction_in);
                    float phase = (float(position) + fraction) * s.inv_sync_ticks;
                    phase -= float(enaga::dsp::floor_int(phase));
                    value = lfo_[t].seek(s.wave, phase >= 1.0f || phase < 0.0f ? 0.0f : phase);
                } else {
                    // Stopped: keep moving at the tempo, so the sound does not freeze between
                    // takes.
                    value = lfo_[t].advance(s.wave, cycles);
                }
                lfo_value_[t] = value;
                const float phase = lfo_[t].phase();
                status_.lfo_phase[t].store(
                    uint8_t(phase <= 0.0f ? 0.0f : (phase >= 1.0f ? 255.0f : phase * 255.0f)),
                    std::memory_order_relaxed);
            }

            modulated_[t] = effective_[t];
            const bool insert_chain = id != MachineId::midi && !va_filter;
            if (s.on()) {
                uint8_t &target = modulated_[t].v[s.dest];
                // A cutoff stops at about 100 Hz and swings about three octaves at most
                // (dsp/lfo.hpp). Worked out every block: the machine can change under an
                // unchanged LFO. MID has no audio, so its bytes go out unfloored and unscaled. A
                // VA or WAVE track's slot 5 is TON (D8), not the insert's cutoff: no floor there.
                const uint8_t floor = lfo_cutoff_floor(s.dest, va_filter, insert_chain);
                const float span = lfo_span(s.dest, va_filter, insert_chain);
                const uint8_t moved = lfo_apply(target, value, s.depth, floor, span);
                lfo_shown_[t] = moved;
                if (va_filter && s.dest == page_first(Page::src)) {
                    // TUN on a synth machine glides: a float to the voices, the byte left alone,
                    // so a slow sweep does not step and a held note follows (dsp/lfo.hpp, "Block
                    // rate").
                    pitch_mod = lfo_pitch_semis(value, s.depth);
                } else {
                    target = moved;
                }
            }
            // After the LFO, so a slot moves the byte from where the LFO left it.
            run_mod_slots(t, s, value, cycles, restart, va_filter, insert_chain, pitch_mod);
            modulating_[t] = true;
        }
        if (!slots) {
            mod_value_[t][0] = 0.0f;
            mod_value_[t][1] = 0.0f;
        }

        // To the machine only when it changes, or when the track has a new machine: with the
        // LFO off and nothing sent before, two compares.
        Machine *machine = machines_[t];
        if (pitch_mod != pitch_mod_[t] || machine != pitch_mod_machine_[t]) {
            if (machine != nullptr) machine->set_pitch_mod(pitch_mod);
            pitch_mod_machine_[t] = machine;
            pitch_mod_[t] = pitch_mod;
        }
    }
}

// The two mod slots, over `modulated_` after the LFO (core/modslot.hpp). `lfo` and `cycles` are
// the LFO's settings and this block's advance, which ENV runs its own one-shot on; `restart` is
// a note since the last block. A slot that is off costs one test.
void Engine::run_mod_slots(int t, const LfoSettings &lfo, float lfo_value, float cycles,
                           bool restart, bool va_filter, bool insert_chain, float &pitch_mod) {
    const KitModSlot *const mod = project_.kit.tracks[t].mod;
    // Each note: ENV from its start, a new RND. Only for a slot that reads them, so an unused
    // source costs nothing.
    bool env = false;
    bool rnd = false;
    for (int i = 0; i < kModSlots; i++) {
        env = env || (mod[i].on() && mod[i].source == kModSrcEnv);
        rnd = rnd || (mod[i].on() && mod[i].source == kModSrcRnd);
    }
    float env_value = 0.0f;
    if (env) {
        if (restart) mod_env_[t].restart();
        env_value = mod_env_[t].one_shot(lfo.wave, restart ? 0.0f : cycles);
    }
    if (rnd && restart) mod_random_[t] = mod_dice_[t].bipolar();

    for (int i = 0; i < kModSlots; i++) {
        const KitModSlot &slot = mod[i];
        if (!slot.on()) {
            mod_value_[t][i] = 0.0f;
            continue;
        }
        const float value = slot.source == kModSrcLfo   ? lfo_value
                            : slot.source == kModSrcEnv ? env_value
                            : slot.source == kModSrcVel ? mod_velocity_value(mod_velocity_[t])
                            : slot.source == kModSrcPrs ? mod_pressure_[t]
                                                     : mod_random_[t];
        mod_value_[t][i] = value;
        // TUN on a synth glides by a float, as the LFO's does; every other byte moves, and a
        // cutoff stops at the floor the LFO, the macros and the morph stop at (D9).
        if (va_filter && slot.dest == page_first(Page::src)) {
            pitch_mod += mod_pitch_semis(value, slot.depth);
            continue;
        }
        uint8_t &target = modulated_[t].v[slot.dest];
        target = mod_apply(target, value, slot.depth,
                           lfo_cutoff_floor(slot.dest, va_filter, insert_chain));
    }
}

// --- the block ------------------------------------------------------------

void Engine::process(const float *in, float *out, uint32_t frames) {

    if (!ready_) {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
        return;
    }

    arp_ratchets_done_ = false;          // see arp_ratchets, below
    // No track has started a note in this block yet (choke_group).
    for (int t = 0; t < kTracks; t++) note_frame_[t] = kNoChokeFrame;

    apply_commands();

    // The CPU guard, before any trig: a lowered budget must be in force for this block's notes.
    run_cpu_guard();

    // MIDI out's note-offs that fall in this block, and any a full queue refused earlier.
    // Before this block's trigs, so a note ending as the same pitch starts again is ended first.
    midi_out_.begin_block(midi_sink_, frames);

    // The kit with the morph, the macros and any throw over it. Control rate, and only when one
    // of them has moved.
    apply_performance();

    // The scheduler spaces ratchet hits in samples, so it needs to know how long a tick is.
    // Re-derive only when the tempo actually moves: this is a float divide, which the C674x does
    // in hardware, but there is no reason to do it every block. (Integer division is the one to
    // avoid here -- this chip has no integer divide instruction at all.)
    enaga::clock::State clock_now = enaga::clock::state();
    // A STOP ends a count-in, whether it ran out or not: the next START is a plain one unless the
    // UI arms another (Cmd::count_in).
    if (was_running_ && !clock_now.running) {
        count_in_end_ = 0;
        click_.stop();
    }
    was_running_ = clock_now.running != 0;
    // Everything below that follows the beat (the LFOs' sync, the master effects' phase) follows
    // the pattern's position, which a count-in puts behind the clock's.
    clock_now.position = clock_now.position > count_in_end_ ? clock_now.position - count_in_end_ : 0u;
    if (clock_now.tempo != tempo_) {
        tempo_ = clock_now.tempo;
        const float bpm = tempo_ > 1.0f ? tempo_ : 1.0f;
        samples_per_tick_ = rate_ * 60.0f / (bpm * float(enaga::clock::ppqn));
        inv_samples_per_tick_ = enaga::dsp::reciprocal(samples_per_tick_);
        reconfigure();
    }

    // What `resolve_tick` compares a ratchet hit's frame against.
    block_frames_ = frames;

    // Ratchet hits deferred from earlier blocks, before this block's own ticks: they are older,
    // and a machine's voice allocation sees triggers in the order they arrive.
    deliver_deferred(frames);

    // The arps' ratchet repeats due in this block. A hit before this line (a command, a deferred
    // step) counted its repeats from this block's start; one after it counts from the next's.
    arp_ratchets(frames);

    // Note repeat and held arps with the transport stopped: no ticks arrive, so they count frames.
    if (!enaga::clock::running()) {
        roll_free(frames);
        stutter_free(frames);
        arp_free(frames);
    }

    // Enaga's clock reports the ticks falling in this block, each with the frame it lands on.
    // That frame is what makes a step start on an exact sample.
    //
    // A count-in's ticks only click; after it, every tick is counted from its end, so the
    // pattern starts at tick 0 as from a plain START (metronome_tick in engine.hpp).
    enaga::clock::Tick ticks[16];
    const uint32_t tick_count = enaga::clock::ticks(ticks, 16);
    for (uint32_t i = 0; i < tick_count; i++) {
        metronome_tick(ticks[i].tick, ticks[i].frame);
        if (ticks[i].tick < count_in_end_) continue;
        const uint32_t tick = ticks[i].tick - count_in_end_;
        resolve_tick(tick, ticks[i].frame);
        span_tick(tick, ticks[i].frame);
        roll_tick(tick, ticks[i].frame);
        stutter_tick(tick, ticks[i].frame);
        arp_tick(tick, ticks[i].frame);
    }

    // What the performance layer produced, into the values the machines read -- except where the
    // current step has a lock, which always wins. Without this a morph would only be heard at
    // the next trig, and sweeping VALUE between steps would do nothing at all.
    for (int t = 0; t < kTracks; t++) {
        const uint32_t locked = locked_[t];
        // Most steps lock nothing: then it is one 32-byte copy, not 32 tested bytes.
        if (locked == 0u) {
            __builtin_memcpy(effective_[t].v, performed_[t], sizeof performed_[t]);
            continue;
        }
        for (int p = 0; p < kTrackParams; p++) {
            if ((locked >> p & 1u) != 0) continue;
            effective_[t].v[p] = performed_[t][p];
        }
    }

    // Parameter slides, over the step's locks they glide from.
    run_slides(frames);

    // The LFOs, last, over everything: the kit, the morph, the macros and the step's locks all
    // decide where the destination sits, and the LFO moves it about there (dsp/lfo.hpp).
    run_lfos(frames, clock_now.running, clock_now.position, clock_now.fraction);
    // A MID track's knobs, locks, scenes, macros and LFO, as CCs, now that all of them are in.
    send_midi_ccs(frames);

    // A streamed sample's next frames, before anything renders from them.
    top_up_streams();
    top_up_audition();

    // The performance mixer's plan over the sends and the master (core/perffx.hpp): copies, so
    // the project's settings are untouched and amount 0 hands the mixer exactly what it had.
    // The tracks' send raise is in apply_performance, with the rest of the performance layer.
    if (perf_plan_ == PerfPlan{}) {
        mixer_.prepare(sends_, master_);
    } else {
        SendSettings sends = sends_;
        MasterSettings master = master_;
        apply_perf_plan(sends, master);
        mixer_.prepare(sends, master);
    }
    // The held master moves, from the mixer's ECHO FREEZE and TAPE STOP (their FUNC chords
    // retired 2026-11-25, backlog D5).
    if (perf_plan_.delay_freeze != freeze_applied_) {
        mixer_.freeze_delay(perf_plan_.delay_freeze);
        freeze_applied_ = perf_plan_.delay_freeze;
    }
    tape_stop_.hold(perf_plan_.tape_stop);
    mixer_.begin_block(int(frames));

    // A bounce's tap, only while its take is running: the track's own block, from the mixer.
    const int tapped = capture_.recording() && capture_.channels() == 1 ? int(capture_track_) : -1;
    if (tapped >= 0) enaga_clear(capture_tap_, int(frames));

    uint8_t voices = 0;
    const bool running = clock_now.running;
    // Each track's block peak for its meter (Status::track_milli): the master's peak loop over
    // the machine's own block, before the chain consumes it. 0 for a track that did not render.
    float track_peak[kTracks] = {};
    for (int t = 0; t < kTracks; t++) {
        Machine *machine = machines_[t];

        // A frozen track plays its loop instead of its machine (dsp/frozen.hpp): through its
        // chain, fader, pan, sends and duck as ever. Muted, it only keeps its place.
        if (frozen_[t].on()) {
            if (muted_[t]) {
                frozen_[t].skip(int(frames));
                continue;
            }
            // A frozen track plays its loop, which no choke cuts.
            choke_at_[t] = kNoChokeFrame;
            choke_gain_[t] = 1.0f;
            prepare_chain(t);
            float *mono = mixer_.scratch();
            enaga_clear(mono, int(frames));
            frozen_[t].render_add(mono, int(frames), running);
            track_peak[t] = strata_master_peak(mono, mono, int(frames));
            // A bounce of a frozen track is its loop through its chain, as it is heard.
            if (t == tapped && !capture_prechain_) mixer_.add_track_tapped(t, mono, capture_tap_);
            else mixer_.add_track(t, mono);
            continue;
        }

        if (machine == nullptr || muted_[t] || !machine->active()) {
            // A choke on a track with nothing to render: nothing to fade, so it is over. A muted
            // track's voices are cut all the same, or they would sound again on unmute.
            if (choke_at_[t] != kNoChokeFrame) {
                if (machine != nullptr) machine->kill_all();
                choke_at_[t] = kNoChokeFrame;
                choke_gain_[t] = 1.0f;
            }
            continue;
        }

        // From effective_, not params_: this is where a parameter lock has to be honoured, for
        // every block of the step rather than only the one that triggered it.
        // Only when something they read has moved (prepare_machine, above).
        prepare_machine(t);
        prepare_chain(t);

        float *mono = mixer_.scratch();
        enaga_clear(mono, int(frames));
        machine->render_add(mono, int(frames));
        if (choke_at_[t] != kNoChokeFrame) run_choke(t, mono, frames);
        track_peak[t] = strata_master_peak(mono, mono, int(frames));
        // A freeze records the machine alone, before the chain it will go on running through.
        if (t == tapped && capture_prechain_) {
            for (uint32_t i = 0; i < frames; i++) capture_tap_[i] = mono[i];
        }
        if (t == tapped && !capture_prechain_) mixer_.add_track_tapped(t, mono, capture_tap_);
        else mixer_.add_track(t, mono);
        voices++;
    }

    mixer_.finish_block(out, int(frames));

    for (int t = 0; t < kTracks; t++) {
        const float fallen = track_meter_[t] * kMeterFall;
        track_meter_[t] = track_peak[t] > fallen ? track_peak[t] : fallen;
        status_.track_milli[t].store(to_milli(track_meter_[t] > 1.0f ? 1.0f : track_meter_[t]),
                                     std::memory_order_relaxed);
    }

    // A stream voice that went back to frame 0 in this block needs its stream reopened.
    watch_streams();

    // The master looper, on the finished mix and before the performance effects, so a loop is
    // the kit as it played and whatever is turned on next goes over it (dsp/looper.hpp). A take
    // the transport stops under is thrown away: it would no longer be in time.
    if (!running && looper_.state() == Looper::State::recording) looper_.clear();
    looper_.process(out, int(frames), float(looper_mix_) * (1.0f / float(kPerfAmountMax)), running);

    // The mixer's own effects (dsp/masterfx.hpp), in time with the beat: the clock's place while
    // it runs, carried on from there while it is stopped.
    const float phase_step = inv_samples_per_tick_ * (1.0f / float(enaga::clock::ppqn));
    if (running) {
        const uint32_t into_beat = clock_now.position % uint32_t(enaga::clock::ppqn);
        perf_phase_ = (float(into_beat) + clock_now.fraction) * (1.0f / float(enaga::clock::ppqn));
    }
    MasterFxSettings fx;
    fx.pump = perf_plan_.pump;
    fx.crush = perf_plan_.crush;
    fx.noise = perf_plan_.noise;
    fx.gate = perf_plan_.gate;
    master_fx_.set(fx);
    master_fx_.process(out, int(frames), perf_phase_, phase_step);
    perf_phase_ += phase_step * float(frames);
    perf_phase_ -= perf_phase_ >= 1.0f ? float(int(perf_phase_)) : 0.0f;

    // Beat repeat is LAST, on the finished master: that is what "the master output" means, and
    // it is why it captures the limiter and the master filter too (dsp/repeat.hpp). The length
    // is a note value, so the frames follow the tempo. The REPEAT pad is its only owner since
    // the mixer's slot 7 became the MUTE BUS (2026-11-25).
    repeat_.set(uint32_t(float(repeat_length_ticks(repeat_length_)) * samples_per_tick_),
                repeat_pitch_);
    repeat_.hold(repeat_held_);
    repeat_.process(out, int(frames));

    // The tape stop, after beat repeat: it stops whatever is coming out, a repeat included. Two
    // beats from full speed to a standstill, so it lands with the music.
    tape_stop_.set(2.0f * float(enaga::clock::ppqn) * samples_per_tick_);
    tape_stop_.process(out, int(frames));

    // What comes out, for the screen's scopes and spectra (core/scope.hpp): every second frame.
    scope_.push(out, frames);

    // The take, from the FINISHED master: after the mixer, the limiter and beat repeat, which is
    // what comes out of the box (dsp/capture.hpp). A tap, so `out` is not modified.
    //
    // Or from the audio INPUT, when the take was armed with that source: the codec's buffer as it
    // arrives, before anything STRATA plays, so a synth sampled into a track is the synth alone.
    // A host that gives no input buffer (the tests' rigs) records silence rather than nothing, so
    // the take still ends on its frame and the card side still gets a file.
    static const float kSilence[2u * ENAGA_BLOCK_FRAMES] = {};
    const float *input = in != nullptr ? in : (frames <= ENAGA_BLOCK_FRAMES ? kSilence : nullptr);
    if (capture_.channels() == 1) capture_.process_mono(tapped >= 0 ? capture_tap_ : nullptr, int(frames));
    else capture_.process(capture_input_ ? input : out, int(frames));

    // The input's level, every block, for the RESAMPLE screen's meter: one pass of the master's
    // own peak loop over the interleaved buffer (L and R alike, so one plane of 2n samples).
    if (in != nullptr) {
        const float peak = strata_master_peak(in, in, int(2u * frames));
        const float clipped = peak > 1.0f ? 1.0f : peak;
        status_.input_peak.store(to_milli(clipped), std::memory_order_relaxed);
    }

    // The sample browser's audition, last and outside everything: after the take's tap, so it
    // is never recorded, and after the master effects, beat repeat and the tape stop, so it is
    // the file as it is (dsp/audition.hpp).
    if (audition_on_) {
        audition_.render_add(out, int(frames));
        if (audition_.take_finished()) {
            end_audition();
        } else {
            status_.audition_frame.store(audition_.position(), std::memory_order_relaxed);
        }
    }
    // The metronome, beside the audition: for the player, never recorded (dsp/click.hpp).
    if (click_.active()) click_.render_add(out, int(frames));

    status_.resample_state.store(capture_.done()        ? 3u
                                 : capture_.recording() ? 2u
                                 : capture_.armed()     ? 1u
                                                        : 0u,
                                 std::memory_order_relaxed);
    status_.resample_frames.store(capture_.frames(), std::memory_order_relaxed);
    status_.resample_clamped.store(capture_.clamped() ? 1u : 0u, std::memory_order_relaxed);
    status_.looper_state.store(uint8_t(looper_.state()), std::memory_order_relaxed);
    status_.looper_bars.store(looper_.holding() ? uint8_t(looper_.bars()) : 0u,
                              std::memory_order_relaxed);

    publish_playheads();
    publish_waves();
    publish_heard();

    // Every MIDI gate is a block nearer its end.
    midi_out_.end_block(frames);

    // Publish for the UI. Relaxed stores of single words: the UI only ever wants the newest.
    const enaga::clock::State clock = enaga::clock::state();
    // The pattern's position: the clock's less a count-in, and 0 while one is still clicking.
    const bool counting = clock.running && clock.position < count_in_end_;
    status_.tick.store(clock.position > count_in_end_ ? clock.position - count_in_end_ : 0u,
                       std::memory_order_relaxed);
    status_.count_in_beats.store(
        counting ? uint8_t(float(count_in_end_ - clock.position) *
                               (1.0f / float(enaga::clock::ppqn)) + 0.999f)
                 : uint8_t(0),
        std::memory_order_relaxed);
    status_.count_in_ticks.store(count_in_end_, std::memory_order_relaxed);
    status_.pattern_origin.store(origin_, std::memory_order_relaxed);
    status_.running.store(clock.running ? 1u : 0u, std::memory_order_relaxed);
    status_.bump_blocks();
    waves_.audio_block();   // a WAVE slot taken out two blocks ago is no longer being read
    status_.voices.store(voices, std::memory_order_relaxed);
    status_.undo_depth.store(uint8_t(undo_.depth()), std::memory_order_relaxed);
    status_.redo_depth.store(uint8_t(undo_.redo_depth()), std::memory_order_relaxed);

    // Which pattern is live. Published here rather than in switch_pattern because a project load
    // moves it without going through that function, and two stores a block cost nothing.
    status_.live_bank.store(project_.bank, std::memory_order_relaxed);
    status_.live_slot.store(project_.slot, std::memory_order_relaxed);
    publish_lock_state();

    const float peak = mixer_.peak();
    status_.peak_milli.store(to_milli(peak), std::memory_order_relaxed);

    // The screen frame's master meter: the block's peak, or the last reading falling by about
    // 0.09 dB a block (20 dB in a third of a second), whichever is higher. One multiply a block.
    const float fallen = meter_ * kMeterFall;
    meter_ = peak > fallen ? peak : fallen;
    status_.meter_milli.store(to_milli(meter_), std::memory_order_relaxed);
    status_.comp_gain_milli.store(to_milli(mixer_.compressor_gain()), std::memory_order_relaxed);
    if (peak >= 1.0f) status_.bump_limit_seen();

    // And where the pattern is in its pass: a subtraction from the boundary resolve_tick keeps,
    // no divide. Before the first tick, or a master length changed in mid-pass, it reads 0.
    const uint32_t loop = uint32_t(master_loop_ticks(live()));
    const uint32_t left = next_boundary_ - tick_;
    const uint32_t into = boundary_known_ && next_boundary_ >= tick_ && left <= loop
                              ? loop - left : 0u;
    status_.pattern_into.store(uint16_t(into), std::memory_order_relaxed);
    status_.pattern_loop.store(uint16_t(loop), std::memory_order_relaxed);
}

// --- MIDI out ---------------------------------------------------------------

bool Engine::EnagaMidi::send(const uint8_t *bytes, uint32_t count) {
    return enaga::midi::send(bytes, count);
}

uint32_t Engine::midi_gate(int track, uint32_t gate_frames) const {
    if (gate_frames > 0) return gate_frames;
    // One step, the length a step with no gate of its own is drawn with. Index 8 of
    // core/timing.hpp's table is 16 sixteenths: one step whatever the track's speed.
    const uint32_t step = gate_frames_for(track, kMidiDefaultLength);
    // Before the clock has reported a tempo there is no step to measure, so a tenth of a second.
    return step > 0 ? step : uint32_t(rate_ * 0.1f);
}

void Engine::send_midi_notes(int track, const uint8_t *notes, int count, uint8_t velocity,
                             uint32_t frame, uint32_t gate) {
    const uint8_t channel = project_.settings.midi_out[track];
    if (channel == kMidiOutOff) return;
    for (int i = 0; i < count; i++) {
        midi_out_.note_on(midi_sink_, track, channel, notes[i], velocity, frame, gate);
    }
}

void Engine::send_midi_ccs(uint32_t frames) {
    for (int t = 0; t < kTracks; t++) {
        lfo_cc_wait_[t] = lfo_cc_wait_[t] > frames ? lfo_cc_wait_[t] - frames : 0u;
        send_midi_ccs(t, true);
    }
}

void Engine::send_midi_ccs(int t, bool throttle) {
    const uint8_t channel = project_.settings.midi_out[t];
    if (machine_id_[t] != uint8_t(MachineId::midi) || channel == kMidiOutOff || muted_[t]) return;
    // The LFO's destination is worked out here from `effective_` rather than read from
    // `modulated_`: ahead of a note, the step's lock is in `effective_` but `modulated_` is
    // still last block's.
    const LfoSettings &lfo = lfo_settings_[t];
    const int lfo_dest = modulating_[t] ? int(lfo.dest) : -1;
    if (!cc_known_[t]) {
        // The values as they stand become the reference; only a MOVE is sent.
        for (int p = 0; p < kParams; p++) cc_sent_[t][p] = effective_[t].v[p];
        cc_known_[t] = true;
        return;
    }
    for (int p = 0; p < kParams; p++) {
        const uint8_t cc = MidiMachine::cc_for(p);
        uint8_t value = effective_[t].v[p];
        bool moved_by_lfo = p == lfo_dest;
        if (moved_by_lfo) value = lfo_apply(value, lfo_value_[t], lfo.depth);
        // And the mod slots, on this block's values, throttled as the LFO is.
        for (int i = 0; i < kModSlots; i++) {
            const KitModSlot &slot = project_.kit.tracks[t].mod[i];
            if (!slot.on() || slot.dest != p) continue;
            value = mod_apply(value, mod_value_[t][i], slot.depth);
            moved_by_lfo = true;
        }
        if (cc == 0 || value == cc_sent_[t][p]) continue;
        if (moved_by_lfo && throttle && lfo_cc_wait_[t] > 0) continue;
        uint8_t bytes[3];
        midi_message(bytes, 0xB0u, channel, cc, value);
        // Remembered only once it has left: a full queue is retried next block with the newest
        // value, which is all a CC needs.
        if (midi_sink_.send(bytes, 3)) {
            cc_sent_[t][p] = value;
            if (moved_by_lfo) lfo_cc_wait_[t] = uint32_t(rate_ * (kLfoCcMs * 0.001f));
        }
    }
}

void Engine::drain_commands_for_shutdown() {
    // Bounded: a ring holds far fewer than this many blocks' worth of commands.
    for (int i = 0; i < 1024 && !commands_.empty(); i++) apply_commands();
}

void Engine::end_midi_for_shutdown() {
    midi_out_.release_all(midi_sink_);
    midi_all_notes_off();
}

void Engine::midi_all_notes_off() {
    bool done[kMidiChannels + 1] = {};
    for (int t = 0; t < kTracks; t++) {
        const uint8_t channel = project_.settings.midi_out[t];
        if (channel == kMidiOutOff || channel > kMidiChannels || done[channel]) continue;
        done[channel] = true;
        uint8_t bytes[3];
        midi_message(bytes, 0xB0u, channel, kMidiAllNotesOff, 0);
        midi_sink_.send(bytes, 3);   // best effort: the tracked note-offs went first
    }
}

// --- streaming ------------------------------------------------------------------
//
// app/storage.hpp, "Streaming", has the protocol. This side reads: each block, up to
// kStreamReadFrames of each attached track's file into its fifo, converted to int16 on the way
// (core/pcm.hpp). Enaga's stream never waits (a short read is an underrun or the end), so this
// costs a copy and a conversion of what arrived and nothing else.

void Engine::stop_stream(int track) {
    StreamState &st = stream_state_[track];
    st.on = false;
    st.attached = false;
    st.carry = 0;
}

void Engine::fill_fifo(enaga::files::Stream &stream, StreamFifo &fifo, uint32_t total,
                       uint8_t frame_bytes, uint8_t encoding, uint8_t &carry, uint8_t *carry_bytes,
                       uint32_t most) {
    const uint32_t next = fifo.next_frame();
    if (next >= total) return;                        // the whole file is in
    uint32_t room = 0;
    int16_t *span = fifo.write_span(&room);
    uint32_t n = room < most ? room : most;
    if (n > total - next) n = total - next;
    if (n == 0) return;

    const uint32_t fb = frame_bytes;
    for (uint32_t i = 0; i < carry; i++) stream_bytes_[i] = carry_bytes[i];
    const uint32_t want = n * fb - carry;
    const uint32_t got = stream.read(stream_bytes_ + carry, want);
    const uint32_t have = carry + (got > want ? want : got);
    const uint32_t frames = stream_frames_in(have, fb);
    if (frames != 0) {
        const uint32_t did = pcm_to_s16(stream_bytes_, frames * fb, PcmFormat(encoding),
                                        fifo.channels(), span, frames);
        fifo.commit(did);
    }
    const uint32_t used = frames * fb;
    carry = uint8_t(have - used);
    for (uint32_t i = 0; i < carry; i++) carry_bytes[i] = stream_bytes_[used + i];
}

void Engine::top_up_streams() {
    for (int t = 0; t < kTracks; t++) {
        StreamState &st = stream_state_[t];
        if (!st.on || !st.attached) continue;
        fill_fifo(streams_[t], fifos_[t], st.total, st.frame_bytes, st.encoding, st.carry,
                  st.carry_bytes, kStreamReadFrames);
    }
}

// --- sample audition (dsp/audition.hpp) -------------------------------------------
//
// The card side opens the file's stream at its data and, once it is filled, posts
// Cmd::audition_start; this side reads it into the player's fifo each block and plays it, until
// its last frame or a Cmd::audition_stop's fade. Then it stops reading and bumps
// Status::audition_epoch, after which the stream is the card side's to close.

void Engine::top_up_audition() {
    if (!audition_on_ || !audition_.wants_frames()) return;
    fill_fifo(audition_stream_, audition_.fifo(), audition_.total(), audition_frame_bytes_,
              audition_encoding_, audition_carry_, audition_carry_bytes_, kAuditionReadFrames);
}

void Engine::end_audition() {
    // Stop reading FIRST, then say so, as watch_streams does: the card side may close the stream
    // the moment it sees the new value.
    audition_on_ = false;
    audition_carry_ = 0;
    audition_epoch_ = uint8_t(audition_epoch_ + 1u);
    status_.audition_frame.store(0, std::memory_order_relaxed);
    status_.audition_epoch.store(audition_epoch_, std::memory_order_release);
}

void Engine::watch_streams() {
    for (int t = 0; t < kTracks; t++) {
        StreamState &st = stream_state_[t];
        if (!st.on) continue;
        const Sampler &sampler = pool_[t].sampler;
        const uint32_t late = sampler.stream_late();
        stream_late_ += late - st.late_seen;
        st.late_seen = late;
        const uint32_t restarts = sampler.stream_restarts();
        if (restarts == st.restarts_seen) continue;
        st.restarts_seen = restarts;
        // Stop reading FIRST, then say so: the card side may close the stream the moment it
        // sees the new value, and the release store is what orders the two.
        st.attached = false;
        st.carry = 0;
        st.want = uint16_t(st.want + 1u);
        status_.stream_want[t].store(st.want, std::memory_order_release);
    }
    status_.stream_late.store(stream_late_, std::memory_order_relaxed);
}

// --- LOCK (core/lockstore.hpp) ----------------------------------------------
//
// The live set is spread over the engine (the pattern in the project, the sound in `params_`,
// the faders in `mix_`, the send and master bytes in the settings, the mutes, the morph and the
// macro positions); these two gather it into one LiveState and put one back.

void Engine::capture_live(LiveState &state) const {
    __builtin_memcpy(&state.pattern, &live(), sizeof state.pattern);
    static_assert(kKitParams == kTrackParams, "a lock holds the kit's bytes as the engine has them");
    for (int t = 0; t < kTracks; t++) {
        __builtin_memcpy(state.params[t], params_[t].v, kKitParams);
        state.level[t] = mix_[t].level;
    }
    __builtin_memcpy(state.sends, project_.settings.sends, sizeof state.sends);
    __builtin_memcpy(state.master, project_.settings.master, sizeof state.master);
    state.mutes = mute_state_capture(muted_);
    for (int m = 0; m < kMacros; m++) state.macros[m] = macro_position_[m];
    state.morph = morph_;
    state.rel_all = rel_all_;
    state.len_all = len_all_;
    state.rnd_unit = perform_.rnd_unit;
    state.rnd_armed = perform_.rnd_armed;
}

// At once (D3): the next step plays from the restored pattern, and the sound is heard in this
// block. Like an undo, not a pattern change: the scheduler's timing is rebuilt (lengths, speeds
// and swing may differ) but its PRE/NEI chain is not reset. The machines are not switched: a
// lock taken before a machine switch restores the parameters only (docs/MANUAL.md).
void Engine::apply_live(const LiveState &state) {
    __builtin_memcpy(&live(), &state.pattern, sizeof state.pattern);
    deferred_count_ = 0;                 // ratchet hits of the steps just replaced
    span_clear();   // and the rolls of spanning ratchets
    for (int t = 0; t < kTracks; t++) {
        __builtin_memcpy(params_[t].v, state.params[t], kKitParams);
        const uint32_t locked = locked_[t];
        for (int p = 0; p < kTrackParams; p++) {
            // A p-lock on the step playing now still wins until the next step, as for a knob.
            if ((locked >> p & 1u) == 0) effective_[t].v[p] = params_[t].v[p];
        }
        mix_[t].level = state.level[t];
    }
    __builtin_memcpy(project_.settings.sends, state.sends, sizeof state.sends);
    __builtin_memcpy(project_.settings.master, state.master, sizeof state.master);
    apply_mixer_bytes();
    apply_mutes(state.mutes);
    for (int m = 0; m < kMacros; m++) macro_position_[m] = state.macros[m];
    morph_ = state.morph;
    rel_all_ = state.rel_all;
    len_all_ = state.len_all;
    perform_.rnd_unit = state.rnd_unit;
    perform_.rnd_armed = state.rnd_armed;
    perform_dirty_ = true;
    reconfigure();
}

void Engine::lock_command(uint8_t action) {
    const int index = live_index();
    if (lock_memory_ == nullptr) {
        status_.bump_refused();
        return;
    }
    if (action == 0) {
        capture_live(locks_.slot(index));
        locks_.stored(index);
        lock_changed_ = false;
        status_.bump_lock_event(kLockStored);
        return;
    }
    const LockTap tap = locks_.tap(index);
    if (tap == LockTap::jumped) {
        capture_live(locks_.back());
        apply_live(locks_.slot(index));
        lock_changed_ = false;
    } else if (tap == LockTap::swapped) {
        apply_live(locks_.back());
        lock_changed_ = true;            // checked again within kLockCheckBlocks
    }
    lock_check_countdown_ = 0;
    status_.bump_lock_event(uint8_t(tap));
}

void Engine::publish_lock_state() {
    const int index = live_index();
    if (!locks_.has(index)) {
        lock_changed_ = false;
        status_.lock_state.store(0, std::memory_order_relaxed);
        return;
    }
    if (--lock_check_countdown_ <= 0) {
        lock_check_countdown_ = kLockCheckBlocks;
        LiveState &scratch = lock_memory_[LockStore::kStates];
        capture_live(scratch);
        lock_changed_ = !live_state_same(scratch, locks_.slot(index));
    }
    status_.lock_state.store(uint8_t(kLockHas | (lock_changed_ ? kLockChanged : 0u) |
                                     (locks_.jumped(index) ? kLockSwapReady : 0u)),
                             std::memory_order_relaxed);
}

}  // namespace strata
