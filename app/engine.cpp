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

uint8_t clamp_u8(int value) {
    return uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

}  // namespace

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

    build_placeholder_sample();

    // The default techno kit of docs/SPEC.md, as far as the implemented machines reach.
    // Tracks 7 and 8 wait for VA POLY and GRAIN; track 6 for VA MONO and track 8's loop for
    // SAMPLE. An empty slot is silent, not a crash.
    machines_[0] = &kick_;
    machines_[1] = &snare_;
    machines_[2] = &hat_closed_;
    machines_[3] = &hat_open_;
    machines_[4] = &perc_;
    machines_[5] = &bass_;
    machines_[6] = &stab_;     // CHORD's slot; a mono stab until VA POLY lands in M6
    machines_[7] = &loop_;

    // Track 3 chokes track 4: a closed hat cuts an open one. The spec's choke group, wired here
    // because the machines deliberately do not know about tracks.
    hat_closed_.set_choke_target(&hat_open_);

    // The duck fires from the kick, and the kick itself must not duck.
    mixer_.set_duck(0.003f, 0.040f, 0.120f);
    for (int t = 0; t < kTracks; t++) mix_[t].duck = t == duck_source_ ? 0.0f : 0.0f;

    load_default_kit();
    for (int t = 0; t < kTracks; t++) effective_[t] = params_[t];   // no locks active yet
    load_default_pattern();

    scheduler_.configure(pattern_);
    scheduler_.reset(true);

    ready_ = true;
    return true;
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
    pattern_init(pattern_);
    pattern_.root = 60;
    pattern_.seed = 0x5713A7Au;
    pattern_.master_length = 16;
    pattern_.swing = kSwingMin;

    for (int t = 0; t < kTracks; t++) {
        pattern_.tracks[t].length = 16;
        pattern_.tracks[t].speed = uint8_t(Speed::x1);
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
            Step &s = pattern_.tracks[track].steps[steps[i]];
            s.flags = kStepTrig;
            s.note = pattern_.root;
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
        Step &s = pattern_.tracks[5].steps[bass[i]];
        s.note = 36;                                   // C2
    }
    pattern_.tracks[5].steps[6].flags |= kStepAccent;
    pattern_.tracks[5].steps[10].flags |= kStepSlide;
    pattern_.tracks[5].steps[10].note = 43;            // slides up a fifth
    pattern_.tracks[5].steps[14].flags |= kStepAccent;

    // Successive notes pick successive slices of the placeholder loop.
    for (int i = 0; i < 4; i++) {
        pattern_.tracks[7].steps[loop[i]].note = uint8_t(60 + i);
    }

    // Accent the downbeat kick: techno lives on that one.
    pattern_.tracks[0].steps[0].flags |= kStepAccent;
    pattern_.tracks[0].steps[0].velocity = 127;

    // Two parameter locks, so the default pattern actually uses the feature docs/SPEC.md calls
    // STRATA's edge over the stock firmware -- and so the lock path is exercised every time
    // anyone plays the default kit or runs the emulator scenario.
    //
    // The third kick of the bar gets a shorter body, and one closed hat opens up: small
    // per-step moves that are the whole point of locks.
    pattern_set_lock(pattern_, 0, 8, uint8_t(page_first(Page::amp) + 0), 52);   // KICK DEC
    pattern_set_lock(pattern_, 2, 10, uint8_t(page_first(Page::amp) + 0), 64);  // HAT DEC
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
            Step &s = pattern_.tracks[track].steps[step];
            const uint8_t bit = command.a ? uint8_t(kStepTrigless) : uint8_t(kStepTrig);
            const uint8_t other = command.a ? uint8_t(kStepTrig) : uint8_t(kStepTrigless);
            const int flags = (int(s.flags) ^ int(bit)) & ~int(other);
            s.flags = uint8_t(flags);
            if ((s.flags & kStepTrig) && s.velocity == 0) s.velocity = 100;
            if ((s.flags & kStepTrig) && !length_valid(s.length)) s.length = 8;
            break;
        }

        case Cmd::clear_lock:
            pattern_clear_lock(pattern_, track, step, command.a);
            break;

        case Cmd::set_track_length:
            pattern_.tracks[track].length =
                uint8_t(command.a < 1 ? 1 : (command.a > kStepsPerTrack ? kStepsPerTrack : command.a));
            reconfigure();
            break;

        case Cmd::set_track_speed:
            if (command.a < uint8_t(Speed::count)) {
                pattern_.tracks[track].speed = command.a;
                reconfigure();
            }
            break;

        case Cmd::set_pattern_swing:
            pattern_.swing = uint8_t(command.a < kSwingMin ? kSwingMin
                                                           : (command.a > kSwingMax ? kSwingMax : command.a));
            reconfigure();
            break;

        // --- M3: deltas, undo, ranges and recording ---------------------------

        case Cmd::step_field_delta:
            if (command.a < uint8_t(StepField::count)) {
                step_field_apply(pattern_.tracks[track].steps[step], StepField(command.a),
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
            pattern_get_lock(pattern_, track, step, param, base);
            const uint8_t value = clamp_u8(int(base) + int(int8_t(command.b)));

            if (!pattern_set_lock(pattern_, track, step, param, value)) {
                // "Full" usually means slots abandoned by relocation rather than 1,024 real
                // locks: growing a step's run moves it to the top of the pool and leaves the old
                // one behind (core/pattern.hpp). Compacting reclaims those, so try once more
                // before refusing -- otherwise locks start failing long before the pool is
                // actually full, and an undo level gets burned on an edit that did nothing.
                pattern_compact_locks(pattern_);
                if (!pattern_set_lock(pattern_, track, step, param, value)) {
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
            undo_.mark(pattern_, gesture);
            break;
        }

        case Cmd::undo:
            // A restored pattern may carry different track lengths, speeds or swing, so the
            // scheduler's derived timing has to be rebuilt. The PRE/NEI chain state is
            // deliberately NOT reset: an undo is an edit, not a pattern change, and restarting
            // the chain would make a conditional step somewhere else in the pattern fire
            // differently because of an edit that had nothing to do with it.
            if (undo_.undo(pattern_)) reconfigure();
            break;

        case Cmd::redo:
            if (undo_.redo(pattern_)) reconfigure();
            break;

        case Cmd::range_copy:
            range_copy(pattern_, track, step, int(command.a), clip_);
            break;

        case Cmd::range_paste:
            // A refused paste means the lock pool had no room even after compaction. It leaves
            // the pattern untouched (core/editops.hpp), so the only thing to do is count it.
            if (!range_paste(pattern_, track, step, clip_)) status_.bump_refused();
            break;

        case Cmd::range_erase:
            range_erase(pattern_, track, step, int(command.a));
            break;

        case Cmd::track_erase:
            track_erase(pattern_, track);
            break;

        case Cmd::range_transpose:
            range_transpose(pattern_, track, step, int(command.a), int(int8_t(command.b)));
            break;

        case Cmd::range_nudge:
            range_nudge(pattern_, track, step, int(command.a), int(int8_t(command.b)));
            break;

        case Cmd::record_hit: {
            // The step and the micro-timing were resolved in the main context by core/record,
            // from the clock state at the moment the hit arrived. Nothing here re-decides them.
            Step &s = pattern_.tracks[track].steps[step];
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
            muted_[track] = command.a != 0;
            break;

        case Cmd::set_fill:
            perform_.fill = command.a != 0;
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

// Rebuild the scheduler's derived timing after anything that changes a track's length, speed or
// swing -- including an undo, which can change all three at once.
//
// It exists so that `samples_per_tick_` cannot be forgotten. Calling configure() with its
// default leaves the scheduler with no idea how long a step is, and the only visible effect is
// that ratchets stop being spread across the step: every hit lands on the step's own frame. That
// was real, from M2 -- changing a track's length flattened its ratchets until the next tempo
// change, which is about as quiet as a bug gets.
void Engine::reconfigure() {
    scheduler_.configure(pattern_, samples_per_tick_);
}

// --- the tick -------------------------------------------------------------

void Engine::resolve_tick(uint32_t tick, uint32_t frame) {
    const int count = scheduler_.tick(tick, frame, pattern_, perform_, events_, kMaxEventsPerTick);
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
    effective_[track] = params_[track];

    const LockView locks = pattern_step_locks(pattern_, track, int(event.step));
    int applied = 0;
    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        if (lock->param >= kParams || applied >= kMaxLocksApplied) continue;
        effective_[track].v[lock->param] = lock->value;
        applied++;
    }

    if (!event.trigless && machines_[track] != nullptr && !muted_[track]) {
        // Prepare before triggering so the voice starts from the locked values. The render loop
        // then keeps preparing from effective_, so the lock holds for the whole step rather than
        // just the instant of the trigger.
        machines_[track]->prepare(effective_[track]);

        Trig trig;
        trig.note = event.note;
        trig.velocity = event.velocity;
        trig.accent = event.accent;
        trig.slide = event.slide;
        trig.frame = event.frame;
        machines_[track]->trigger(trig);

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

    // Publish for the UI. Relaxed stores of single words: the UI only ever wants the newest.
    const enaga::clock::State clock = enaga::clock::state();
    status_.tick.store(clock.position, std::memory_order_relaxed);
    status_.running.store(clock.running ? 1u : 0u, std::memory_order_relaxed);
    status_.bump_blocks();
    status_.voices.store(voices, std::memory_order_relaxed);
    status_.undo_depth.store(uint8_t(undo_.depth()), std::memory_order_relaxed);
    status_.redo_depth.store(uint8_t(undo_.redo_depth()), std::memory_order_relaxed);

    const float peak = mixer_.peak();
    const float scaled = peak * 1000.0f;
    status_.peak_milli.store(uint16_t(scaled > 65535.0f ? 65535.0f : scaled),
                             std::memory_order_relaxed);
}

}  // namespace strata
