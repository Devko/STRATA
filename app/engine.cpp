#include "engine.hpp"

#include <enaga/app.hpp>
#include <enaga/clock.hpp>
#include <enaga/dsp.hpp>

namespace strata {

namespace {

// How many of a step's locks can change a parameter before a trig. Locks are applied to the
// track's working parameters just before the machine is prepared, then reverted, so a lock never
// leaks into the next step.
constexpr int kMaxLocksApplied = kLocksPerStep;

// Middle C. The pattern's root is a pitch class (core/pattern.hpp), so turning it into a note
// needs an octave, and this is the one the default pattern uses.
constexpr uint8_t kDefaultOctaveNote = 60;

uint8_t clamp_u8(int value) {
    return uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

}  // namespace

// The kit on disk and the kit the machines read have to agree about how many parameters a track
// has. core/project.hpp cannot say so itself: core/ does not depend on dsp/ (core/pattern.hpp
// explains why), so the two constants are declared apart and checked here, in the one file that
// includes both.
static_assert(kKitParams == kParams,
              "core/project.hpp's kit record and dsp/track.hpp's TrackParams have diverged; a "
              "saved project would come back with the wrong number of parameters per track");

bool Engine::init(float sample_rate) {
    rate_ = sample_rate;

    // Every allocation in the signal path happens here and nowhere else.
    kick_.init(rate_);
    snare_.init(rate_);
    hat_closed_.init(rate_);
    hat_open_.init(rate_);
    perc_.init(rate_);
    bass_.init(rate_);
    stab_.init(rate_);
    loop_.init(rate_);
    mixer_.init(rate_);

    // Beat repeat's 2-bar ring, 2.93 MiB, here with every other buffer in the signal path.
    if (!repeat_.init(rate_)) return false;

    perform_init(scenes_, macros_);

    // Sample memory is NOT taken here. The region and the SamplePool that carves it up belong
    // to the main context: App::init allocates the region and hands the base over with
    // set_sample_region before the first audio block, which keeps every byte of pool state on
    // one side of the ring (app/engine.hpp's `sample_region_`). It is still allocated once, at
    // init, as CLAUDE.md requires -- just one level up.

    build_placeholder_sample();

    // The default techno kit of docs/SPEC.md. Every slot is filled now that VA POLY exists;
    // GRAIN has no track in the default kit and arrives with machine switching. An empty slot
    // would be silent, not a crash.
    machines_[0] = &kick_;
    machines_[1] = &snare_;
    machines_[2] = &hat_closed_;
    machines_[3] = &hat_open_;
    machines_[4] = &perc_;
    machines_[5] = &bass_;
    machines_[6] = &stab_;     // CHORD, on VA POLY's six voices
    machines_[7] = &loop_;

    // The one track hosting a Sampler today. docs/SPEC.md gives SAMPLE to track 8 in the default
    // kit; M6 makes a track's machine switchable and this gains the rest.
    samplers_[7] = &loop_;

    // And the one hosting VA POLY, so `fire` knows which track can be sent a chord.
    polys_[6] = &stab_;

    // Track 3 chokes track 4: a closed hat cuts an open one. The spec's choke group, wired here
    // because the machines deliberately do not know about tracks.
    hat_closed_.set_choke_target(&hat_open_);

    // The duck fires from the kick, and the kick itself must not duck.
    mixer_.set_duck(0.003f, 0.040f, 0.120f);
    for (int t = 0; t < kTracks; t++) mix_[t].duck = t == duck_source_ ? 0.0f : 0.0f;

    load_default_kit();
    for (int t = 0; t < kTracks; t++) effective_[t] = params_[t];   // no locks active yet
    apply_performance();
    load_default_pattern();

    scheduler_.configure(live());
    scheduler_.reset(true);

    ready_ = true;
    return true;
}

// Main context, App::init only. Nothing is read through `sample_region_` until a `Cmd::set_sample`
// arrives, and that command cannot arrive before App::init has returned, so this needs no
// synchronisation with the audio side.
void Engine::set_sample_region(const int16_t *base, uint32_t bytes) {
    sample_region_ = base;
    sample_region_bytes_ = bytes;
}

void Engine::load_default_kit() {
    for (int t = 0; t < kTracks; t++) {
        TrackParams &p = params_[t];
        for (int i = 0; i < kParams; i++) p.v[i] = 64;   // the middle of every knob

        // FLTR: filter off is both the cheapest setting and the right default for drums.
        p.v[page_first(Page::fltr) + 0] = 0;    // DRV: no drive
        p.v[page_first(Page::fltr) + 3] = 0;    // MOD: filter off

        // FX: lo-fi off, centred, sends closed. Closed sends matter: the mixer stops running
        // delay and reverb entirely when nothing feeds them, which is 16% of the budget.
        p.v[page_first(Page::fx) + 0] = 0;      // LOF off
        p.v[page_first(Page::fx) + 1] = 64;     // PAN centre
        p.v[page_first(Page::fx) + 2] = 0;      // SNA closed
        p.v[page_first(Page::fx) + 3] = 0;      // SNB closed

        p.v[page_first(Page::amp) + 3] = 100;   // LVL
        mix_[t].level = 1.0f;
    }

    // A starting point that sounds like techno rather than like test tones. AMP slot 0 is the
    // decay on all three machines.
    params_[0].v[page_first(Page::amp) + 0] = 75;    // KICK: a long-ish body
    params_[0].v[page_first(Page::fltr) + 0] = 24;   // and a little drive, which the 909 likes
    params_[1].v[page_first(Page::amp) + 0] = 50;    // SNARE
    params_[2].v[page_first(Page::amp) + 0] = 22;    // HAT closed: tight
    params_[3].v[page_first(Page::amp) + 0] = 86;    // HAT open: long

    // Pan the hats apart a little so the stereo image is not a single point.
    params_[2].v[page_first(Page::fx) + 1] = 52;
    params_[3].v[page_first(Page::fx) + 1] = 76;

    // PERC as a rimshot, using the settings dsp/perc.cpp documents for one: a short, high,
    // inharmonic hit (ratio pi) that cuts through without competing with the snare.
    // SRC is TUN, RAT, IDX, IDD; AMP is DEC, SWP, SWT, LVL.
    const uint8_t perc_src[4] = {85, 48, 75, 20};
    const uint8_t perc_amp[4] = {12, 30, 10, 100};
    for (int i = 0; i < 4; i++) {
        params_[4].v[page_first(Page::src) + i] = perc_src[i];
        params_[4].v[page_first(Page::amp) + i] = perc_amp[i];
    }
    params_[4].v[page_first(Page::fx) + 1] = 44;    // a touch left of the hats

    // BASS (VA MONO). SRC is TUN, MOD, CUT, RES; AMP is DEC, ENV, GLD, LVL. A techno bass wants
    // a low-ish cutoff with real resonance and a short filter envelope, so the note moves.
    const uint8_t bass_src[4] = {64, 40, 46, 84};
    const uint8_t bass_amp[4] = {38, 72, 18, 104};
    for (int i = 0; i < 4; i++) {
        params_[5].v[page_first(Page::src) + i] = bass_src[i];
        params_[5].v[page_first(Page::amp) + i] = bass_amp[i];
    }

    // The CHORD slot's stand-in: the same machine, tuned up and opened out so it reads as a
    // stab rather than a second bass. Replaced by VA POLY in M6, which will cost more (six
    // voices against this one), so do not read the 8-track figure as final.
    const uint8_t stab_src[4] = {64, 70, 92, 40};
    const uint8_t stab_amp[4] = {70, 40, 0, 76};
    for (int i = 0; i < 4; i++) {
        params_[6].v[page_first(Page::src) + i] = stab_src[i];
        params_[6].v[page_first(Page::amp) + i] = stab_amp[i];
    }
    params_[6].v[page_first(Page::fx) + 1] = 84;    // off to the right

    // LOOP (SAMPLE). SRC is PIT, STA, LEN, SLC; AMP is ATK, DEC, PLY, LVL. Eight slices, so a
    // note picks one; the placeholder sample below is built with eight.
    // SLC must be 60, not 64: the slice count is 1 + knob * 16 / 128 in zones eight knob values
    // wide, so 64 asks for NINE slices while the placeholder below builds eight. The boundaries
    // would then fall mid-blip instead of on them. ATK 0 is a true zero attack, which is what a
    // sliced one-shot wants -- its transient IS the sound.
    const uint8_t loop_src[4] = {64, 0, 127, 60};   // PIT unity, STA 0, LEN to the end, SLC 8
    const uint8_t loop_amp[4] = {0, 110, 0, 96};    // ATK instant, DEC long, PLY fwd one-shot
    for (int i = 0; i < 4; i++) {
        params_[7].v[page_first(Page::src) + i] = loop_src[i];
        params_[7].v[page_first(Page::amp) + i] = loop_amp[i];
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

    loop_.set_sample(pcm, uint32_t(kTotalFrames), 1, rate_);
}

void Engine::load_default_pattern() {
    // Every one of the 128 patterns, not just the live one: a project with 127 uninitialised
    // patterns would play whatever zeroed memory means the first time someone queued one, and
    // core/timing.hpp's numbering is deliberately chosen so that zeroed memory is at least sane
    // rather than correct. Cheap, once, at init.
    project_init(project_);

    pattern_init(live());
    // A PITCH CLASS, not a note. core/pattern.hpp documents `root` as 0-11 and
    // core/serialize.cpp enforces it with `root % 12` on load, so the 60 this used to hold came
    // back as 0 after a save and a load -- and the notes below, taken straight from it, would
    // have come back at the very bottom of the MIDI range.
    live().root = 0;                       // C
    live().scale = uint8_t(Scale::minor);  // the default techno key (core/scale.hpp)
    live().seed = 0x5713A7Au;
    live().master_length = 16;
    live().swing = kSwingMin;

    for (int t = 0; t < kTracks; t++) {
        live().tracks[t].length = 16;
        live().tracks[t].speed = uint8_t(Speed::x1);
    }

    // Four on the floor, a backbeat clap, offbeat closed hats and one open hat lifting the bar.
    const uint8_t kick[] = {0, 4, 8, 12};
    const uint8_t snare[] = {4, 12};
    const uint8_t closed[] = {2, 6, 10, 14};
    const uint8_t open[] = {14};
    const uint8_t perc[] = {7, 15};        // a rim on the last 8th of each half-bar
    const uint8_t bass[] = {2, 6, 10, 14}; // offbeat under the four-on-the-floor
    const uint8_t stab[] = {0, 8};
    const uint8_t loop[] = {1, 5, 9, 13};  // a different slice each time, via the note

    auto put = [&](int track, const uint8_t *steps, int count, uint8_t velocity) {
        for (int i = 0; i < count; i++) {
            Step &s = live().tracks[track].steps[steps[i]];
            s.flags = kStepTrig;
            // The root in a playable octave. The pattern's root is a pitch class, so it needs
            // an octave to become a note.
            s.note = uint8_t(kDefaultOctaveNote + live().root);
            s.velocity = velocity;
            s.length = 8;            // one step
        }
    };

    put(0, kick, 4, 110);
    put(1, snare, 2, 100);
    put(2, closed, 4, 80);
    put(3, open, 1, 90);
    put(4, perc, 2, 85);
    put(5, bass, 4, 100);
    put(6, stab, 2, 78);
    put(7, loop, 4, 88);

    // The bass plays low, and slides into the third note: the movement that makes a line groove
    // rather than pulse. Slide is MONO-only, which is exactly what this track is.
    for (int i = 0; i < 4; i++) {
        Step &s = live().tracks[5].steps[bass[i]];
        s.note = 36;                                   // C2
    }
    live().tracks[5].steps[6].flags |= kStepAccent;
    live().tracks[5].steps[10].flags |= kStepSlide;
    live().tracks[5].steps[10].note = 43;            // slides up a fifth
    live().tracks[5].steps[14].flags |= kStepAccent;

    // Successive notes pick successive slices of the placeholder loop.
    for (int i = 0; i < 4; i++) {
        live().tracks[7].steps[loop[i]].note = uint8_t(60 + i);
    }

    // The CHORD track plays a chord, which is what its name says and what makes VA POLY's six
    // voices reachable without touching the step page. MIN7 is the dub-techno stab, and it is
    // four notes rather than three, so the chord path is exercised past a triad: `fire` expands
    // it on one frame and the allocator has to give each note its own voice. Like the two locks
    // below, this is here so the feature runs every time anyone plays the default kit or an
    // emulator scenario, rather than only when someone remembers to set it.
    for (int i = 0; i < 2; i++) {
        live().tracks[6].steps[stab[i]].chord = chord_pack(ChordType::min7, 0);
    }

    // Accent the downbeat kick: techno lives on that one.
    live().tracks[0].steps[0].flags |= kStepAccent;
    live().tracks[0].steps[0].velocity = 127;

    // Two parameter locks, so the default pattern actually uses the feature docs/SPEC.md calls
    // STRATA's edge over the stock firmware -- and so the lock path is exercised every time
    // anyone plays the default kit or runs the emulator scenario.
    //
    // The third kick of the bar gets a shorter body, and one closed hat opens up: small
    // per-step moves that are the whole point of locks.
    pattern_set_lock(live(), 0, 8, uint8_t(page_first(Page::amp) + 0), 52);   // KICK DEC
    pattern_set_lock(live(), 2, 10, uint8_t(page_first(Page::amp) + 0), 64);  // HAT DEC
}

// --- commands -------------------------------------------------------------

void Engine::apply_commands() {
    Command command;
    // The spec's limit: a burst of edits must not be able to break the deadline.
    for (uint32_t i = 0; i < Commands::kMaxPerBlock; i++) {
        if (!commands_.take(command)) break;
        apply(command);
    }
}

void Engine::apply(const Command &command) {
    const int track = command.track < kTracks ? int(command.track) : 0;
    const int step = command.step < kStepsPerTrack ? int(command.step) : 0;

    switch (command.cmd) {
        case Cmd::play:
            // Nothing to do here. enaga::clock's transport calls are MAIN CONTEXT (see
            // <enaga/clock.hpp>), so app/ drives them directly; routing them through this ring
            // would call them from the audio context, where they silently do nothing. The
            // command still exists because the UI's intent is worth seeing in a trace.
            break;

        case Cmd::stop:
            // Only the voice side of a stop belongs here; the clock itself was already stopped
            // by the main context.
            if (command.a) {
                for (int t = 0; t < kTracks; t++) {
                    if (machines_[t]) machines_[t]->kill_all();
                }
            } else {
                for (int t = 0; t < kTracks; t++) {
                    if (machines_[t]) machines_[t]->release_all();
                }
            }
            scheduler_.reset(true);
            break;

        case Cmd::set_tempo:
            // Also main context, for the same reason. Handled in app/.
            break;

        case Cmd::toggle_trig: {
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
            pattern_clear_lock(live(), track, step, command.a);
            break;

        case Cmd::set_track_length:
            live().tracks[track].length =
                uint8_t(command.a < 1 ? 1 : (command.a > kStepsPerTrack ? kStepsPerTrack : command.a));
            reconfigure();
            break;

        case Cmd::set_track_speed:
            if (command.a < uint8_t(Speed::count)) {
                live().tracks[track].speed = command.a;
                reconfigure();
            }
            break;

        case Cmd::set_pattern_swing:
            live().swing = uint8_t(command.a < kSwingMin ? kSwingMin
                                                           : (command.a > kSwingMax ? kSwingMax : command.a));
            reconfigure();
            break;

        // --- M3: deltas, undo, ranges and recording ---------------------------

        case Cmd::step_field_delta:
            if (command.a < uint8_t(StepField::count)) {
                step_field_apply(live().tracks[track].steps[step], StepField(command.a),
                                 int(int8_t(command.b)));
            }
            break;

        case Cmd::lock_delta: {
            if (command.a >= kParams) break;
            const uint8_t param = command.a;

            // The base is the step's own lock if it has one, and the kit value if it does not --
            // so the first click of an encoder on an unlocked step moves away from what you are
            // currently hearing rather than from zero.
            uint8_t base = params_[track].v[param];
            pattern_get_lock(live(), track, step, param, base);
            const uint8_t value = clamp_u8(int(base) + int(int8_t(command.b)));

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
            if (scheduler_.playhead(track) == uint8_t(step)) effective_[track].v[param] = value;
            break;
        }

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
            if (undo_.undo(live())) reconfigure();
            break;

        case Cmd::redo:
            if (undo_.redo(live())) reconfigure();
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

        case Cmd::range_transpose:
            range_transpose(live(), track, step, int(command.a), int(int8_t(command.b)));
            break;

        case Cmd::range_nudge:
            range_nudge(live(), track, step, int(command.a), int(int8_t(command.b)));
            break;

        case Cmd::record_hit: {
            // The step and the micro-timing were resolved in the main context by core/record,
            // from the clock state at the moment the hit arrived. Nothing here re-decides them.
            Step &s = live().tracks[track].steps[step];
            const int flags = (int(s.flags) | int(kStepTrig)) & ~int(kStepTrigless);
            s.flags = uint8_t(flags);
            s.note = clamp_u8(command.a);
            s.velocity = uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
            s.micro = int8_t(command.c);
            if (!length_valid(s.length)) s.length = 8;
            // The step's locks are deliberately left alone. Overdubbing onto a programmed part
            // should keep the sound it was programmed with; a hit replaces the note, not the
            // step's whole character.
            break;
        }

        // --- projects and banks, M4 ---------------------------------------

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
            if (!queue_arm(queue_, bank, slot, mode, tick_, master_loop_ticks(live()))) {
                status_.bump_refused();
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
                } else {
                    status_.bump_refused();
                }
            } else {
                // Stopping a chain leaves the pattern it reached playing, rather than jumping
                // back: a performer turning the chain off wants to stay where they are.
                chain_.active = false;
            }
            break;

        case Cmd::kit_store:
            kit_from_params();
            break;

        case Cmd::kit_recall:
            // NOTHING POSTS THIS YET. A project load does the same work through
            // Cmd::project_loaded, which also has to reset the scheduler and the undo history,
            // so this arm is waiting for the gesture docs/SPEC.md's key table gives to F1 + F8:
            // "Kit and machine select". Left in rather than deleted because the UI for that is
            // M6 and re-adding it would be the same three lines -- but it is dead until then,
            // and the reachability audit in docs/HANDOFF.md will keep saying so.
            params_from_kit();
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
            undo_.clear();
            queue_clear(queue_);
            chain_.active = false;
            scheduler_.reset(true);
            reconfigure();
            boundary_known_ = false;
            break;

        case Cmd::clear_sample:
            // The first half of the sample swap docs/SPEC.md requires. The voices go first,
            // because they are reading the buffer that is about to be released, and the epoch
            // goes last, because the loader may act on it the instant it sees the change -- and
            // what it does is write over that memory. Reversing the two is the bug this protocol
            // exists to prevent.
            if (samplers_[track] != nullptr) {
                samplers_[track]->kill_all();
                // And drop the pointer itself: a null sample is a legal, expected state and
                // leaves the track silent rather than glitching (dsp/sample.hpp).
                samplers_[track]->set_sample(nullptr, 0, 1, rate_);
            }
            // Bumped even for a track that hosts no Sampler. The loader waits for this change
            // before it releases the range, so a track that was never reading anything must
            // still report that it has stopped, or the wait never ends.
            status_.bump_sample_epoch(track);
            break;

        case Cmd::set_sample: {
            if (samplers_[track] == nullptr) {
                status_.bump_refused();
                break;
            }
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
            samplers_[track]->set_sample(sample_region_ + command.u / sizeof(int16_t), frames,
                                         channels, rate_);
            break;
        }

        // --- kit and sound ---

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

        case Cmd::set_param:
            if (command.a < kParams) {
                params_[track].v[command.a] = clamp_u8(command.b);
                // Mirror into the effective set so a knob turn is heard at once instead of at
                // the next step. The next step rebuilds effective_ from the kit anyway, so a
                // lock on this parameter still wins from then on.
                effective_[track].v[command.a] = clamp_u8(command.b);
            }
            break;

        case Cmd::set_track_level:
            mix_[track].level = command.f < 0.0f ? 0.0f : (command.f > 2.0f ? 2.0f : command.f);
            break;

        case Cmd::set_mute:
            if (command.b != 0) {
                // At the next bar. The latch holds a whole STATE rather than one toggle, so the
                // state to arm is what the mutes are NOW with this track flipped -- which only
                // this side can know.
                uint8_t state = mute_state_capture(muted_);
                state = uint8_t(state ^ uint8_t(1u << track));
                mute_latch_arm(mute_latch_, state);
            } else {
                muted_[track] = command.a != 0;
            }
            break;

        case Cmd::set_fill:
            // The pad. Auto-fill ORs into this per bar in resolve_tick, so holding FILL during
            // an auto-fill bar is not a contradiction.
            fill_held_ = command.a != 0;
            // Also applied here, because resolve_tick is the only other place that sets it and
            // it does not run with the clock stopped. Without this, holding FILL while stopped
            // would do nothing and the pad would look broken.
            perform_.fill = fill_held_;
            break;

        // --- performance, M5 ----------------------------------------------

        case Cmd::mutate: {
            const int count = command.c == 0 ? kStepsPerTrack : int(command.c);
            if (!mutate_apply(live(), track, step, count, Mutation(command.a), command.u,
                              int(command.b))) {
                // Nothing changed, which is worth saying: the UI should not spend an undo level
                // on an edit that did nothing, and a press that looks like it did nothing had
                // better say so.
                status_.bump_refused();
            }
            break;
        }

        case Cmd::scene_capture:
            if (command.a < kScenes) {
                // From the KIT, not from `performed_`: a scene is a sound to come back to, and
                // capturing the morph's own output would fold the performance into the thing
                // being performed.
                if (command.b != 0) scene_capture_track(scenes_.scenes[command.a], project_.kit,
                                                        track);
                else scene_capture(scenes_.scenes[command.a], project_.kit);
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
                perform_dirty_ = true;
            }
            break;

        case Cmd::set_morph:
            morph_ = uint16_t(command.u > kMorphScale ? kMorphScale : command.u);
            perform_dirty_ = true;
            break;

        case Cmd::set_macro:
            if (command.a < kMacros) {
                macro_position_[command.a] = command.b > 127 ? uint8_t(127) : command.b;
                perform_dirty_ = true;
            }
            break;

        case Cmd::macro_assign:
            if (command.a < kMacros && command.b < kParams) {
                // The next free target. Which of the four it lands in is not something a
                // performer should have to think about; a macro that is full refuses, visibly.
                Macro &macro = macros_.macros[command.a];
                int slot = -1;
                for (int i = 0; i < kMacroTargets && slot < 0; i++) {
                    if (macro.targets[i].enabled == 0) slot = i;
                }
                if (slot < 0) {
                    status_.bump_refused();
                    break;
                }
                macro.targets[slot].enabled = 1;
                macro.targets[slot].track = uint8_t(track);
                macro.targets[slot].param = command.b;
                macro.targets[slot].low = command.c;
                macro.targets[slot].high = command.d;
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
                else mute_state_apply(state, muted_);
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

        case Cmd::trigger_note: {
            // A pad hit, played live. It sounds now, at the start of the block: the main context
            // cannot know a frame offset, and the spec's latency stamping belongs to recording.
            if (machines_[track]) {
                Trig trig;
                trig.note = clamp_u8(command.a);
                trig.velocity = uint8_t(command.b < 1 ? 1 : (command.b > 127 ? 127 : command.b));
                trig.frame = 0;
                machines_[track]->trigger(trig);
                status_.bump_triggered(track);
            }
            break;
        }

        case Cmd::none:
        default:
            break;
    }
}

// --- the kit, between the project and the machines ------------------------
//
// `params_` is what the machines read and what a knob turns; `project_.kit` is what a file
// holds. They are deliberately separate structures -- one belongs to dsp/, the other to core/ --
// so these two functions are the only place they meet.

void Engine::kit_from_params() {
    for (int t = 0; t < kTracks; t++) {
        KitTrack &track = project_.kit.tracks[t];
        for (int i = 0; i < kParams; i++) track.params[i] = params_[t].v[i];
        track.level = mix_[t].level;
        // `machine` and `sample` are not derived from anything the engine owns yet: a track's
        // machine is fixed at init until M6 makes it switchable, and sample paths arrive with
        // project loading. Leaving them alone keeps what a loaded project said.
    }
}

void Engine::params_from_kit() {
    for (int t = 0; t < kTracks; t++) {
        const KitTrack &track = project_.kit.tracks[t];
        for (int i = 0; i < kParams; i++) {
            params_[t].v[i] = track.params[i] > 127 ? uint8_t(127) : track.params[i];
        }
        // Rebuilt from the kit so a loaded project is heard immediately rather than at the next
        // step, which is the same reason Cmd::set_param mirrors into effective_.
        effective_[t] = params_[t];
        mix_[t].level = track.level < 0.0f ? 0.0f : (track.level > 2.0f ? 2.0f : track.level);
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

void Engine::apply_performance() {
    if (!perform_dirty_) return;
    perform_dirty_ = false;

    // The kit as stored.
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kParams; p++) performed_[t][p] = params_[t].v[p];
    }

    // The scenes, if two are assigned. An empty scene behaves as the kit, so an unassigned slot
    // costs one pass and changes nothing (core/perform.hpp).
    if (scene_a_ < kScenes && scene_b_ < kScenes) {
        scene_morph(scenes_.scenes[scene_a_], scenes_.scenes[scene_b_], morph_, project_.kit,
                    performed_);
    }

    // The macros, over the morph. Later targets win over earlier ones, which core/perform.hpp
    // calls a consequence rather than a decision.
    for (int m = 0; m < kMacros; m++) {
        macro_apply(macros_.macros[m], macro_position_[m], performed_);
    }

    // The throw, over everything: it is the most immediate thing on the panel and should not be
    // argued with by a macro. Send A is the delay and send B the reverb (dsp/chain.hpp).
    if (throw_bus_ != 0 && throw_track_ >= 0 && throw_track_ < kTracks) {
        const int param = page_first(Page::fx) + (throw_bus_ == 1 ? 2 : 3);
        performed_[throw_track_][param] = 127;
    }
}

// --- pattern changes ------------------------------------------------------

void Engine::switch_pattern(int bank, int slot) {
    if (!project_index_valid(bank, slot)) return;
    if (bank == int(project_.bank) && slot == int(project_.slot)) return;

    project_.bank = uint8_t(bank);
    project_.slot = uint8_t(slot);

    // The undo history belonged to the pattern just left. Restoring it over this one would
    // overwrite a different pattern with another's steps, which core/undo.hpp's clear() exists
    // to prevent -- its own header asks for exactly this call.
    undo_.clear();

    // A pattern change is what the 1ST condition tests for, and the scheduler's PRE/NEI chain
    // was resolved against a pattern that is no longer playing.
    scheduler_.reset(true);
    reconfigure();

    // The new pattern's master length makes the old boundary meaningless, and deriving the new
    // one needs a tick, which only resolve_tick has.
    boundary_known_ = false;
}

// A bar started. The only thing that waits for one is a mute change armed for it.
void Engine::on_bar() {
    uint8_t state = 0;
    if (mute_latch_release(mute_latch_, state)) mute_state_apply(state, muted_);
}

void Engine::cross_boundary(uint32_t tick) {
    (void)tick;
    if (!chain_.active) return;

    uint8_t bank = 0, slot = 0;
    // chain_advance reports a PATTERN change, not an entry change, so a chain that repeats one
    // pattern returns false here and nothing is switched -- which is what keeps 1ST from firing
    // on every pass of a one-pattern chain (core/queue.hpp).
    if (chain_advance(project_, chain_, bank, slot)) switch_pattern(bank, slot);
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
    tick_ = tick;

    // A queued change takes effect BEFORE the tick is resolved, so this tick plays the new
    // pattern's steps rather than one last step of the old one.
    if (queue_due(queue_, tick)) {
        switch_pattern(queue_.bank, queue_.slot);
        queue_clear(queue_);
    }

    // The end of a pass, for chains. One division per pass of the pattern -- about every two
    // seconds -- rather than one per tick, which is the only reason `next_boundary_` is kept
    // rather than derived.
    const uint32_t loop = uint32_t(master_loop_ticks(live()));
    if (!boundary_known_) {
        next_boundary_ = (tick / loop + 1) * loop;
        boundary_known_ = true;
    } else if (tick >= next_boundary_) {
        next_boundary_ = (tick / loop + 1) * loop;
        cross_boundary(tick);
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

    const int count = scheduler_.tick(tick, frame, live(), perform_, events_, kMaxEventsPerTick);
    for (int i = 0; i < count; i++) fire(events_[i]);

    for (int t = 0; t < kTracks; t++) status_.publish_playhead(t, scheduler_.playhead(t));
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
    for (int p = 0; p < kParams; p++) effective_[track].v[p] = performed_[track][p];

    const LockView locks = pattern_step_locks(live(), track, int(event.step));
    int applied = 0;
    uint16_t locked = 0;
    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        if (lock->param >= kParams || applied >= kMaxLocksApplied) continue;
        effective_[track].v[lock->param] = lock->value;
        locked = uint16_t(locked | uint16_t(1u << lock->param));
        applied++;
    }
    // Which parameters this step has locked, so the performance layer leaves them alone until
    // the next step. A lock always wins over a morph.
    locked_[track] = locked;

    if (!event.trigless && machines_[track] != nullptr && !muted_[track]) {
        // Prepare before triggering so the voice starts from the locked values. The render loop
        // then keeps preparing from effective_, so the lock holds for the whole step rather than
        // just the instant of the trigger.
        machines_[track]->prepare(effective_[track]);

        // The note goes to the machine UNFOLDED -- it is not put through `scale_fold` here, and
        // that is a decision rather than an omission.
        //
        // `core/chord`'s `chord_expand` folds the step's note before stacking on it, and it does
        // so even for a type-`none` chord, which is a single note. So identical step data sounds
        // different depending on which machine a track hosts: folded on a chord track, as
        // written on a MONO one. docs/SPEC.md's "melodic tracks fold notes into the scale" argues
        // for folding both, but there is no notion here of WHICH machines are melodic, and
        // folding every track would retune the drums -- dsp/kick.cpp reads `note` as a pitch.
        // Inventing that notion now means inventing it twice: machine switching is the change
        // that gives the engine a real idea of what a track hosts, and that is where this
        // belongs. Recorded in docs/UNIT_CHECKLIST.md as well, so the unit can be listened to
        // with the asymmetry in mind rather than discovered by surprise.
        Trig trig;
        trig.note = event.note;
        trig.velocity = event.velocity;
        trig.accent = event.accent;
        trig.slide = event.slide;
        trig.frame = event.frame;
        trig.gate_frames = gate_frames_for(track, event.length_index);

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
            const Pattern &pattern = live();
            const uint8_t packed = pattern.tracks[track].steps[event.step].chord;
            uint8_t notes[kChordMaxNotes];
            const int voiced = chord_expand(event.note, packed, pattern.root, int(pattern.scale),
                                            notes, VaPoly::kVoices);
            for (int i = 0; i < voiced; i++) {
                trig.note = notes[i];
                machines_[track]->trigger(trig);
            }
        } else {
            machines_[track]->trigger(trig);
        }

        // Once per step, not once per note: a chord is one trig as far as the screen is
        // concerned, and eight LEDs flashing six times would be a worse display, not a better
        // one.
        status_.bump_triggered(track);

        if (track == duck_source_) mixer_.trigger_duck(event.frame);
    }
}

// --- the block ------------------------------------------------------------

void Engine::process(const float *in, float *out, uint32_t frames) {
    (void)in;

    if (!ready_) {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
        return;
    }

    apply_commands();

    // The kit with the morph, the macros and any throw over it. Control rate, and only when one
    // of them has moved.
    apply_performance();

    // The scheduler spaces ratchet hits in samples, so it needs to know how long a tick is.
    // Re-derive only when the tempo actually moves: this is a float divide, which the C674x does
    // in hardware, but there is no reason to do it every block. (Integer division is the one to
    // avoid here -- this chip has no integer divide instruction at all.)
    const enaga::clock::State clock_now = enaga::clock::state();
    if (clock_now.tempo != tempo_) {
        tempo_ = clock_now.tempo;
        const float bpm = tempo_ > 1.0f ? tempo_ : 1.0f;
        samples_per_tick_ = rate_ * 60.0f / (bpm * float(enaga::clock::ppqn));
        reconfigure();
    }

    // Enaga's clock reports the ticks falling in this block, each with the frame it lands on.
    // That frame is what makes a step start on an exact sample.
    enaga::clock::Tick ticks[16];
    const uint32_t tick_count = enaga::clock::ticks(ticks, 16);
    for (uint32_t i = 0; i < tick_count; i++) {
        resolve_tick(ticks[i].tick, ticks[i].frame);
    }

    // What the performance layer produced, into the values the machines read -- except where the
    // current step has a lock, which always wins. Without this a morph would only be heard at
    // the next trig, and sweeping VALUE between steps would do nothing at all.
    for (int t = 0; t < kTracks; t++) {
        const uint16_t locked = locked_[t];
        for (int p = 0; p < kParams; p++) {
            if ((locked >> p & 1u) != 0) continue;
            effective_[t].v[p] = performed_[t][p];
        }
    }

    mixer_.prepare(sends_, master_);
    mixer_.begin_block(int(frames));

    uint8_t voices = 0;
    for (int t = 0; t < kTracks; t++) {
        Machine *machine = machines_[t];
        if (machine == nullptr || muted_[t] || !machine->active()) continue;

        // From effective_, not params_: this is where a parameter lock has to be honoured, for
        // every block of the step rather than only the one that triggered it.
        machine->prepare(effective_[t]);
        mixer_.chain(t).prepare(effective_[t], mix_[t]);

        float *mono = mixer_.scratch();
        enaga_clear(mono, int(frames));
        machine->render_add(mono, int(frames));
        mixer_.add_track(t, mono);
        voices++;
    }

    mixer_.finish_block(out, int(frames));

    // Beat repeat is LAST, on the finished master: that is what "the master output" means, and
    // it is why it captures the limiter and the master filter too (dsp/repeat.hpp). The length
    // is a note value, so the frames follow the tempo.
    repeat_.set(uint32_t(float(repeat_length_ticks(repeat_length_)) * samples_per_tick_),
                repeat_pitch_);
    repeat_.hold(repeat_held_);
    repeat_.process(out, int(frames));

    // Publish for the UI. Relaxed stores of single words: the UI only ever wants the newest.
    const enaga::clock::State clock = enaga::clock::state();
    status_.tick.store(clock.position, std::memory_order_relaxed);
    status_.running.store(clock.running ? 1u : 0u, std::memory_order_relaxed);
    status_.bump_blocks();
    status_.voices.store(voices, std::memory_order_relaxed);
    status_.undo_depth.store(uint8_t(undo_.depth()), std::memory_order_relaxed);
    status_.redo_depth.store(uint8_t(undo_.redo_depth()), std::memory_order_relaxed);

    // Which pattern is live. Published here rather than in switch_pattern because a project load
    // moves it without going through that function, and two stores a block cost nothing.
    status_.live_bank.store(project_.bank, std::memory_order_relaxed);
    status_.live_slot.store(project_.slot, std::memory_order_relaxed);

    const float peak = mixer_.peak();
    const float scaled = peak * 1000.0f;
    status_.peak_milli.store(uint16_t(scaled > 65535.0f ? 65535.0f : scaled),
                             std::memory_order_relaxed);
}

}  // namespace strata
