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
    mixer_.init(rate_);

    // The default techno kit of docs/SPEC.md, as far as M1's machines reach.
    machines_[0] = &kick_;
    machines_[1] = &snare_;
    machines_[2] = &hat_closed_;
    machines_[3] = &hat_open_;

    // Track 3 chokes track 4: a closed hat cuts an open one. The spec's choke group, wired here
    // because the machines deliberately do not know about tracks.
    hat_closed_.set_choke_target(&hat_open_);

    // The duck fires from the kick, and the kick itself must not duck.
    mixer_.set_duck(0.003f, 0.040f, 0.120f);
    for (int t = 0; t < kTracks; t++) mix_[t].duck = t == duck_source_ ? 0.0f : 0.0f;

    load_default_kit();
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

    // Accent the downbeat kick: techno lives on that one.
    pattern_.tracks[0].steps[0].flags |= kStepAccent;
    pattern_.tracks[0].steps[0].velocity = 127;
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
            Step &s = pattern_.tracks[track].steps[step];
            s.flags = uint8_t(s.flags ^ kStepTrig);
            if ((s.flags & kStepTrig) && s.velocity == 0) s.velocity = 100;
            if ((s.flags & kStepTrig) && !length_valid(s.length)) s.length = 8;
            break;
        }

        case Cmd::set_step_note:
            pattern_.tracks[track].steps[step].note = clamp_u8(command.a);
            break;

        case Cmd::set_step_velocity:
            pattern_.tracks[track].steps[step].velocity =
                uint8_t(command.a < 1 ? 1 : (command.a > 127 ? 127 : command.a));
            break;

        case Cmd::set_lock:
            pattern_set_lock(pattern_, track, step, command.a, clamp_u8(command.b));
            break;

        case Cmd::clear_lock:
            pattern_clear_lock(pattern_, track, step, command.a);
            break;

        case Cmd::clear_step:
            step_clear(pattern_.tracks[track].steps[step]);
            pattern_clear_step_locks(pattern_, track, step);
            break;

        case Cmd::clear_track:
            track_init(pattern_.tracks[track]);
            for (int s = 0; s < kStepsPerTrack; s++) pattern_clear_step_locks(pattern_, track, s);
            scheduler_.configure(pattern_);
            break;

        case Cmd::set_track_length:
            pattern_.tracks[track].length =
                uint8_t(command.a < 1 ? 1 : (command.a > kStepsPerTrack ? kStepsPerTrack : command.a));
            scheduler_.configure(pattern_);
            break;

        case Cmd::set_track_speed:
            if (command.a < uint8_t(Speed::count)) {
                pattern_.tracks[track].speed = command.a;
                scheduler_.configure(pattern_);
            }
            break;

        case Cmd::set_pattern_swing:
            pattern_.swing = uint8_t(command.a < kSwingMin ? kSwingMin
                                                           : (command.a > kSwingMax ? kSwingMax : command.a));
            scheduler_.configure(pattern_);
            break;

        case Cmd::set_param:
            if (command.a < kParams) params_[track].v[command.a] = clamp_u8(command.b);
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

// --- the tick -------------------------------------------------------------

void Engine::resolve_tick(uint32_t tick, uint32_t frame) {
    const int count = scheduler_.tick(tick, frame, pattern_, perform_, events_, kMaxEventsPerTick);
    for (int i = 0; i < count; i++) fire(events_[i]);

    for (int t = 0; t < kTracks; t++) status_.publish_playhead(t, scheduler_.playhead(t));
}

void Engine::fire(const StepEvent &event) {
    const int track = int(event.track);
    if (track < 0 || track >= kTracks) return;

    // Locks apply whether or not a note sounds: that is the whole point of a trigless step.
    const LockView locks = pattern_step_locks(pattern_, track, int(event.step));
    uint8_t saved[kMaxLocksApplied];
    uint8_t saved_param[kMaxLocksApplied];
    int saved_count = 0;

    for (const Lock *lock = locks.begin(); lock != locks.end(); ++lock) {
        if (lock->param >= kParams || saved_count >= kMaxLocksApplied) continue;
        saved_param[saved_count] = lock->param;
        saved[saved_count] = params_[track].v[lock->param];
        params_[track].v[lock->param] = lock->value;
        saved_count++;
    }

    if (!event.trigless && machines_[track] != nullptr && !muted_[track]) {
        // The machine reads the locked parameters, so prepare must happen inside the lock window.
        machines_[track]->prepare(params_[track]);

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

    // A lock lasts for its own step only; the kit value comes back immediately.
    for (int i = 0; i < saved_count; i++) params_[track].v[saved_param[i]] = saved[i];
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
        const float samples_per_tick = rate_ * 60.0f / (bpm * float(enaga::clock::ppqn));
        scheduler_.configure(pattern_, samples_per_tick);
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

        machine->prepare(params_[t]);
        mixer_.chain(t).prepare(params_[t], mix_[t]);

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

    const float peak = mixer_.peak();
    const float scaled = peak * 1000.0f;
    status_.peak_milli.store(uint16_t(scaled > 65535.0f ? 65535.0f : scaled),
                             std::memory_order_relaxed);
}

}  // namespace strata
