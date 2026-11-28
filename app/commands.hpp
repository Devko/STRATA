// The one channel from the UI to the audio side.
//
// docs/SPEC.md, "Software architecture": the UI never writes pattern memory directly. It posts
// every edit as a command, which the audio side applies at the start of the next block. That is
// what lets a save run in the main context while the transport keeps playing, and it is why
// there is no lock anywhere in this app.
//
// Rules from the spec, enforced here:
//   - commands are fixed-size 16-byte structs
//   - the ring is a power of two, single-producer single-consumer
//   - the audio side applies at most 64 per block, so a burst cannot break the deadline
//
// ## Why this is safe without a lock
//
// One writer (the main context) owns `write_`; one reader (the audio context) owns `read_`. Each
// only ever publishes its own index, and reads the other's. The payload is written before
// `write_` advances and read after `read_` is seen to lag, so a release/acquire pair on the two
// indices is all the ordering needed. The C674x is a single core with no store buffer to worry
// about, but the atomics are still correct and cost nothing measurable.
//
// A full ring drops the command and says so. Dropping is the right failure: the alternative is
// blocking the UI thread or overwriting an edit the audio side has not applied yet. The UI shows
// the count so a dropped edit is visible rather than mysterious.
#pragma once

#include <atomic>
#include <cstdint>

#include "overview.hpp"

namespace strata {

// What a command does. Serialized nowhere, so these may be renumbered freely.
enum class Cmd : uint8_t {
    none = 0,

    // --- transport and clock ---
    play,              // a:ignored
    stop,              // a: 1 = cut all voices (the second STOP press)
    set_tempo,         // f: BPM

    // --- pattern editing ---
    toggle_trig,       // track, step, a: 1 = the trigless bit instead of the trig, 2 = a FILL trig
    clear_lock,        // track, step, a: param: its value lock, its slide and its random range
    set_track_length,  // track, a: length; or c = 1: b is an int8 delta (kDeltaFlag below)
    set_track_speed,   // track, a: speed
    set_track_play,    // track, a: PlayMode (core/timing.hpp): FWD, REV, PING, RND1..RND16
    set_pattern_swing, // a: swing; or c = 1: b is an int8 delta
    // A track's own swing over the pattern's: a = 0 (kSwingInherit, follow the pattern) or
    // 50-75; or c = 1: b is an int8 delta walked along PAT, 50 .. 75 (core/pattern.hpp,
    // `track_swing_step`).
    set_track_swing,   // track, a: swing; or c = 1: b is an int8 delta

    // --- pattern editing, M3 -----------------------------------------------
    //
    // These carry a DELTA rather than a value, which is the opposite of the M2 commands above
    // and deliberate. An encoder sends several clicks per audio block, and the UI can only read
    // the pattern as it was a block ago, so computing "current + delta" on the UI side loses
    // every click after the first. Applying the delta on the audio side, to the authoritative
    // step, cannot lose one. The cost is that the screen shows the result one block late, which
    // at 1.33 ms is not a thing a person can see.
    step_field_delta,  // track, step, a: StepField, b: int8 delta
    lock_delta,        // track, step, a: param index, b: int8 delta; c = kSemitoneFlag: semitones
    // A parameter's random range on a step (core/pattern.hpp, kLockRange): the width moved by
    // the delta, 0-127; 0 takes the range off. Hold STEP + FUNC + turn A-D.
    lock_range_delta,  // track, step, a: param index, b: int8 delta
    // A parameter's slide on a step (kLockSlide), toggled. With no value lock the step gets one
    // at the kit value first, so the slide has something to glide from. Hold STEP + FUNC + push.
    lock_slide,        // track, step, a: param index

    // The Euclidean generator (docs/SPEC.md, "Musical defaults that help creativity"): a whole
    // track's rhythm from three numbers. One edit, so it carries an undo mark like any other and
    // a sweep of the knob coalesces into one level.
    euclid,            // track, a: hits, b: cycle steps, c: int8 rotation

    undo_mark,         // track, step, a: GestureKind, b: the UI's gesture counter
    undo,
    redo,

    range_copy,        // track, step: the first step, a: how many
    range_paste,       // track, step: the first step
    range_erase,       // track, step: the first step, a: how many
    track_erase,       // track
    pattern_erase,     // every track's steps and locks; the layout and the sounds stay
    // PROJECT's slots (backlog [2]): a whole pattern copied over another, or put back to a new
    // project's empty one. track: the destination bank, step: its slot; copy: a, b the source.
    pattern_copy,
    pattern_clear,
    pattern_double,    // track: bank, step: slot. Every track twice as long, copied (core/editops)
    range_transpose,   // track, step: the first step, a: how many, b: int8 semitones
    range_nudge,       // track, step: the first step, a: how many, b: int8 ticks

    // A live-recorded hit, already resolved to a step and a micro-timing value by core/record in
    // the main context -- where the clock state that stamped it was read.
    // d: a RecWrite, how the note meets the notes the step already has.
    // e: 0, or a ratchet byte to put on the step (a recorded note repeat: a roll over LEN).
    record_hit,        // track, step, a: note, b: velocity, c: int8 micro, d: RecWrite, e: roll
    // And how long that note was held, when its key comes up: core/record's held_length_index,
    // stored as the step's LEN if the step still holds that note.
    record_length,     // track, step, a: note, b: length index
    // A step's LEN set outright: hold a step and press a later one. Only on a step with a trig.
    set_step_length,   // track, step, a: length index

    // --- projects and banks, M4 --------------------------------------------
    //
    // A queued pattern change is armed on the AUDIO side, which is why core/queue.hpp's Queue
    // needs no atomics: both the arming and the per-tick "is it due yet" happen in the same
    // context. The UI's part is posting this.
    queue_pattern,     // a: bank, b: slot, c: QueueMode
    cancel_queue,
    set_chain,         // a: 0 off, 1 on (starting one jumps to its first entry)

    // Track Launch (core/launch.hpp), PROJECT's LAUNCH page. Armed on the audio side like a
    // queued pattern change, from the tick the sequencer is on.
    // launch: a: the lanes (bit t = lane t), b: the source (0 own, 1-128 A01-H16), c: QueueMode.
    launch,
    launch_cancel,     // a: the lanes whose queued launch is dropped
    launch_pin,        // track: PIN on or off
    // CAPTURE: what every lane plays, written into the first free slot of bank `track`, which then
    // plays with every lane on its own part. Status::launch_event says how it went.
    launch_capture,

    // The kit the machines read (`params_`) and the kit a file holds (`project_.kit`) are
    // different structures in different layers, so they are copied across explicitly: `store`
    // before a save, `recall` after a load. Both run on the audio side, which owns both.
    kit_store,
    kit_recall,        // a: 1 = from Engine::kit_for_recall (the kit browser), 0 = re-apply project_.kit

    // The global key and scale (docs/SPEC.md, "Musical defaults that help creativity"). Both
    // apply to the PROJECT's settings and to the live pattern at once: the pattern's own copy is
    // what the sequencer and core/mutate read, so a pattern saved in its own key keeps it, and
    // the project's copy is the default a new pattern gets.
    set_root,          // a: pitch class 0-11
    set_scale,         // a: a core/scale.hpp Scale id

    // A project was loaded: 1.03 MiB arrived through `Engine::project_for_load` rather than
    // through this ring, so everything derived from the pattern is now stale. `kit_recall` alone
    // is not enough -- the scheduler's track lengths, speeds and swing all came from the pattern
    // that was replaced, and the undo history belonged to it too.
    project_loaded,

    // Sample swapping, in the order docs/SPEC.md requires. `clear_sample` takes the sample away
    // from a track and bumps that track's epoch in Status; only once the UI has SEEN the epoch
    // change is the audio side known to have stopped reading, and only then may the loader
    // release the pool range and write over it. `set_sample` hands the new one over.
    //
    // Doing it the other way round -- release, then load, then hand over -- overwrites memory a
    // voice is still reading, which sounds like a broken sampler rather than a broken lifetime.
    clear_sample,      // track
    // track, a: channels, u: byte offset into the sample region, c|d|e: frames (24 bits, which
    // is 16.7 M frames -- 349 s of mono at 48 kHz, well past the 30 s above which docs/SPEC.md
    // streams from the card instead of loading into RAM)
    set_sample,
    // Streaming (app/storage.hpp, "Streaming"). `set_stream` hands over a file too big for the
    // pool: its head (resident, like a sample) and how to read the rest, from
    // Engine::stream_for_load(track) -- more than 16 bytes, so through that inbox, written by the
    // main context just before the post. It follows a clear_sample like set_sample does.
    // `stream_attach` tells the audio side that the track's stream is open at the head's end and
    // filled: a: the low byte, b: the high byte of the restart count it was opened for, so an
    // attach for a restart already superseded is ignored.
    set_stream,        // track
    stream_attach,     // track, a|b: Status::stream_want's value it answers

    // The metronome (dsp/click.hpp): a: 0 OFF, 1 ON. Not saved.
    set_click,         // a: on
    // A count-in for the next START: a: bars, 0 none. Posted BEFORE enaga::clock::start(), so
    // the audio side has it when the first tick comes; posted again with 0 if the start was
    // refused. The first a x 384 ticks then only click, and everything else runs as if the
    // clock had started when they end (Engine::count_in_end_).
    count_in,          // a: bars

    // --- kit and sound ---
    set_param,         // track, a: param index, b: value; or c = 1: b is an int8 delta
    set_track_level,   // track, f: level; or c = 1: f is a delta, the result clamped to 1.5
    // SLC AUTO on a track (KitTrack::slice_mode): a: kSliceAuto or kSliceEqual. The points it
    // cuts at came with the sample (Engine::slices_for_load).
    set_slice_mode,    // track, a: 0 equal, 1 AUTO
    // LFO TRIG on a track (KitTrack::lfo_trig, dsp/lfo.hpp): a: kLfoTrigFree, Retrig or OneShot.
    set_lfo_trig,      // track, a: 0 FREE, 1 RETRIG, 2 ONE-SHOT
    // One mod slot (KitTrack::mod, core/modslot.hpp), whole: a: the slot (0, 1), b: the source
    // (kModSrcOff..kModSrcRnd), c: the destination parameter, d: the depth as an int8. Sanitized as a
    // file's slot is, so a bad one lands OFF.
    set_mod_slot,      // track, a: slot, b: source, c: dest, d: int8 depth
    // A track's choke group (KitTrack::choke): a: 0 none or 1-4; or c = 1: one step on, OFF 1 2
    // 3 4 OFF, decided on the audio side so two fast pushes are two steps. A note on the track
    // cuts the others of its group (Engine::choke_group).
    set_choke,         // track, a: group; or c = 1: step
    // A track's chorus send (KitTrack::chorus, send C): b: an int8 encoder delta onto the 0-127
    // byte, clamped; or c = 1: a push, OFF to kChorusSendToggle and anything else to OFF, decided
    // on the audio side as set_choke's step is. MIXER's CHO 1-4 / 5-8. A kit setting, so a kit
    // save keeps it.
    set_chorus,        // track, b: int8 delta; or c = 1: toggle
    // A track's stereo width (KitTrack::width, core/width.hpp): b: an int8 encoder delta onto the
    // 0-127 byte, clamped; or c = 1: a push, stepping OFF, kWidthSpreadStep, kWidthUnisonStep,
    // OFF. MIXER's WID 1-4 / 5-8. A kit setting; only the POLY machines read it.
    set_width,         // track, b: int8 delta; or c = 1: step
    // A track pad's pressure, for a mod slot on PRS: a: 0-127, 0 when the pad is let go. Played,
    // not stored.
    set_pressure,      // track, a: pressure
    // One send or master setting. `a` is the field -- dsp/buses.hpp's SendField or MasterField,
    // whose numbers are permanent because they reach PROJECT.BIN -- and `b` is an encoder DELTA,
    // as a signed byte, exactly like `set_param` and the step fields and for the same reason.
    //
    // What moves is a BYTE, 0-127, because that is what the project stores and what the encoder
    // produces, so the file, the screen and the sound cannot disagree; the engine turns it into
    // the value the mixer runs on, and for the delay's time it needs the tempo to do it.
    set_sends,         // a: SendField, b: int8 delta
    set_master,        // a: MasterField, b: int8 delta
    // The sidechain duck (core/project.hpp, KitDuck): a: DuckField, b: int8 delta; track is the
    // track whose depth moves for DuckField::depth. Kit bytes, so a kit save keeps them.
    set_duck,
    // The line input's strip (core/project.hpp, ProjectSettings::input): a: InputField, b: an int8
    // encoder delta onto the byte, clamped to the field's domain (input_field_sanitized); or
    // c = 1: a push, which turns ON over, walks SRC and GAIN round by one, and is refused on
    // the other fields. Decided on the audio side, as set_choke's step is. PROJECT.BIN bytes.
    set_input,

    // --- performance, M5 ---------------------------------------------------
    //
    // Every one of these changes how the music sounds without touching a stored pattern, which
    // is docs/SPEC.md's whole point about the performance layer: "Eight tools change the music
    // live without touching the stored pattern unless you commit." The exceptions, which DO
    // commit and say so in the table there, are `mutate` (one undo level) and the scene and
    // macro setup (saved in PERFORM.BIN).
    set_mute,          // track, a: 0 or 1, b: 1 = at the next bar instead of now
    set_fill,          // a: 0 or 1

    // One undoable variation of one track's range. `u` is the variant: the same variant over the
    // same pattern reproduces the same mutation exactly, which is what "seeded" means in
    // core/mutate.hpp. "OK keeps it, CLR reverts" needs nothing here -- the UI marks an undo
    // level first and CLR is the undo that already exists.
    mutate,            // track, step: the first step, a: Mutation, b: amount 0-100, c: how many

    // GEN (core/generate.hpp): a new part for the track, one undo level like `mutate`. The
    // engine decides drum or melodic from the track's machine. `u` is the variant.
    generate,          // track, a: density 0-100, b: range 1-3, c: leap 0-100, d: GenWhat

    // EVOLVE (core/evolve.hpp): RAM only, no undo level. `evolve_set` with a: 0 is a KEEP.
    evolve_set,        // track, a: period index 0-4
    evolve_config,     // a: EvolveMode, b: Mutation, c: amount 0-100 -- for every track
    evolve_keep,       // track: what plays becomes HOME, and it stops
    evolve_home,       // track: back to HOME, and it stops

    // Scenes. A capture takes the CURRENT kit, which is why it happens on this side: `params_`
    // is what the machines read and the UI only has a block-old copy of it.
    scene_capture,     // a: scene, b: 0 = every track, 1 = `track` only
    scene_erase,       // a: scene
    scene_select,      // a: 0 = slot A, 1 = slot B, b: scene
    set_morph,         // u: 0..kMorphScale

    // Macros. `macro_assign` takes the next free target of that macro rather than an index: the
    // UI assigns by pointing at a parameter, and which of the four slots it lands in is not
    // something a performer should have to think about.
    set_macro,         // a: macro, b: position 0-127
    macro_assign,      // a: macro, track, b: param, c: low, d: high
    macro_clear,       // a: macro

    // The eight stored mute states (core/perform.hpp). A recall may wait for the bar, like a
    // mute does.
    mute_store,        // a: slot 0-7
    mute_recall,       // a: slot 0-7, b: 1 = at the next bar

    set_auto_fill,     // a: AutoFill

    // A throw holds a track's delay or reverb send at full and puts it back on release. It is
    // not a parameter edit: `params_` is untouched, so nothing is saved and nothing is undone.
    set_throw,         // track, a: 0 = off, 1 = delay (send A), 2 = reverb (send B)

    // Beat repeat. The length is in note values and the engine turns it into frames, because
    // only the audio side knows the tempo in samples (dsp/repeat.hpp).
    set_repeat,        // a: RepeatLength, b: 1 = held, c: 1 = pitch down
    // track, a: note, b: velocity -- played live. c = 1: HELD, a `release_note` will follow (a
    // KEYS key, a MIDI IN note); 0: a pad, which has no release, so its MIDI out lasts a step.
    trigger_note,
    release_note,      // track, a: note

    // Resampling, docs/SPEC.md's eighth performance tool. Three commands, because a take spans
    // both contexts: the audio side records it, the main context writes the file.
    //
    // `resample_arm` asks for `a` BARS (1, 2, 4 or 8). The engine turns bars into frames -- only
    // it knows the tempo in samples -- and begins on the next bar line, because a take that is
    // not bar-aligned does not loop, which is the whole point of recording a round number of
    // bars. `resample_release` is the main context saying it has finished with the buffer, which
    // is what lets the next take be armed (dsp/capture.hpp).
    //
    // `b` is the SOURCE: 0 the finished master, 1 the audio input (SPEC's "Resample with a chosen
    // input source"). Which input, and at what level, is the main context's enaga::system::input
    // call; the audio side only chooses which buffer the take is written from. 2 is one TRACK,
    // `track`, alone and mono: a bounce (Engine::capture_track_). 3 is the same track BEFORE its
    // insert chain, which is what a freeze records (dsp/frozen.hpp).
    resample_arm,      // a: bars 1-8, b: 0 master, 1 the audio input, 2 track `track`, 3 freeze
    resample_cancel,
    resample_release,

    // Which machine a track hosts (core/project.hpp's MachineId, which is what KIT.BIN stores).
    // The audio side swaps the pointer at a block boundary and kills the voices of the machine
    // being left, because a machine that is no longer rendered cannot release its own tail.
    set_machine,       // track, a: MachineId

    // The live-record quantize amount, 0-100 (docs/SPEC.md, "Recording"). It is a PROJECT
    // setting with a field in PROJECT.BIN, so the engine keeps it and `app/` reads it back
    // rather than holding a second copy that a save could not see.
    set_quantize,      // b: int8 delta, clamped to 0-100
    // REC MODE, the same kind of PROJECT setting: what a note recorded onto a chord track's
    // step does to the notes it has (ProjectSettings::rec_add).
    set_rec_mode,      // a: 0 REPLACE, 1 ADD

    // F1 + F5 (docs/SPEC.md): every track back to its step 1 from the next tick, without
    // touching the voices. The main context has already located the clock to 0; this resets
    // what the audio side derives from the position -- the pattern's origin, its pass count and
    // the chaining state that 1ST and the conditions read.
    restart,

    // PROJECT's CHAIN page. step: the entry index, a: ChainEdit, b: bank, c: slot, d: repeats.
    // core/queue.hpp's chain_set / chain_insert / chain_remove, which keep the chain a prefix.
    chain_edit,

    // The pattern's master length in steps, 1-64: what a queued change and a chain wait for.
    // a: length; or c = 1: b is an int8 delta.
    set_master_length,

    // The master kill EQ's kill switches (dsp/buses.hpp): a: band 0 LOW, 1 MID, 2 HIGH;
    // b: 0 back, 1 killed, 2 toggle. A switch rather than a knob delta because it is a switch:
    // FUNC + push A/B/C anywhere, or a plain push on the MIXER's EQ page. The switches live in
    // the project (ProjectSettings::eq_kills) and save with it, like the live mutes.
    eq_kill,

    // Note repeat (FUNC + FILL in any mode): a held lower pad rolls its track's note at beat
    // repeat's length, on the clock's grid while the transport runs and free from the press while
    // it is stopped. a: note, b: velocity, c: 0 let go, 1 press (the first hit sounds now),
    // 2 new velocity from the pad's pressure. Played, not recorded: the pattern is untouched.
    note_repeat,

    // Step stutter (hold REPEAT, then a STEP: docs/CONTROLS.md). a: the step, 0-63, the STEP
    // page included; b: 1 on (the first hit sounds now), 0 let go; c: which tracks, one bit
    // each (0xFF all of them; FUNC + STEP the selected one). While on, those tracks retrigger
    // that step at beat repeat's length and their own steps are silent, while the sequencer
    // runs on underneath: let go, and the beat is where it would have been. Played, not
    // recorded. A second `on` moves the stutter to its step.
    step_stutter,

    // A track's MIDI OUT channel (core/midiout.hpp): 0 off, 1-16. track, a: channel; or c = 1:
    // b is an int8 delta, stopping at OFF and 16. A change ends the track's sounding notes on
    // the channel they were sent on.
    set_midi_out,

    // PROG CHG OUT (core/project.hpp, `program_out`): a: 1 on, 0 off. Every change of the playing
    // pattern then sends its number, 0-127 for A01-H16, as a program change on the user's MIDI CH.
    set_program_out,
    // The user's MIDI CH (Enaga's setting, 0-15), which only the main context reads: posted at
    // start and whenever it changes. a: the channel. Live state, not saved.
    set_midi_channel,
    // An outside clock's song position pointer moved the clock (ENAGA_TRANSPORT_LOCATE under MIDI
    // clock, only while stopped). u: the position in ticks. The pattern, or the chain and its
    // entry, are put where that position falls, so the CONTINUE that follows plays from there.
    locate,

    // A factory sound (core/sounds.hpp) into one track: the SOUNDS list (OK from EDIT or MACHINE).
    // track, a: the MachineId it was picked for, b: the preset's index -- refused if the track
    // hosts another machine by the time it lands. b = kSoundPresetKeep remembers the track's
    // sound as it is (the list opening); b = kSoundPresetBack puts that back (its AS WAS row).
    // Only the bytes a preset writes are touched either way, so the mix stays the user's.
    sound_preset,

    // A saved sound (core/usersounds.hpp) or a copied one into one track: eight parameter bytes
    // a command, from parameter `step`: b c d e, then u's four bytes low first. track, a: the
    // MachineId the bytes are for -- refused if the track hosts another machine by the time it
    // lands. Only the bytes a sound owns (user_sound_owns) are written, as for sound_preset, so
    // the mix stays the user's. A whole sound is kKitParams / 8 of these, posted together, so
    // they land in one block. AS WAS (sound_preset with kSoundPresetBack) puts back what was
    // there, the machine included when a paste switched it.
    sound_bytes,

    // A factory arp (core/arppresets.hpp) into one track: the ARPS list (OK on the ARP page).
    // track, b: the preset's index, or kSoundPresetKeep / kSoundPresetBack as for sound_preset.
    // Writes the four ARP bytes and the kit's arp settings (KitTrack::arp), nothing else.
    arp_preset,

    // One of the arp's kit settings (KitTrack::arp, core/arp.hpp): track, a: the field (0 VAR,
    // 1 RHYTHM, 2 ACCENT, 3 RATCHET), b: its value; past the field's table reads as 0.
    set_arp_ext,

    // Freeze (dsp/frozen.hpp). `freeze_set` starts track `track` playing its freeze slot: `u` the
    // byte offset into the sample region, frames in c | d << 8 | e << 16 (mono int16), `a` the
    // loop's bars. The bar it began on and where in the take that bar falls are the engine's own
    // (the take it recorded). `freeze_clear` stops it and bumps Status::freeze_epoch; the
    // machine, silenced at the freeze, plays the next note. Performance, not edits: no file.
    freeze_set,
    freeze_clear,

    // The performance mixer (core/perffx.hpp), push VALUE + STEP 1-15 and VALUE. a: the PerfFx
    // (0 none, 1-15; the looper's and the morph's own are refused here), b: its amount, 0-127. One effect at a
    // time: a new one replaces the last, and the depths are smoothed on the audio side, so the
    // change is a fade. Performance, not edits: no file, no undo.
    perf_fx,
    // The master looper (dsp/looper.hpp), STEP 16. a: 0 the mix (b: 0-127), 1 record from the
    // next bar line (b: bars, 1/2/4/8; halved until the take fits the buffer), 2 throw it away.
    // A record is refused while RESAMPLE's take holds the buffer they share.
    looper,
    // LOCK (core/lockstore.hpp), FUNC + MUTATE. `lock_store` (a hold) keeps the live set as the
    // playing pattern's lock; `lock_tap` jumps to it, or swaps back when the last tap jumped.
    // What happened comes back as Status::lock_event / lock_last, the state as
    // Status::lock_state. The locks are RAM only, but a tap changes the pattern, the kit and the
    // mixer bytes, so it dirties all three files (app/storage.cpp).
    lock_store,
    lock_tap,

    // FEEL (core/timing.hpp), the PATTERN page's FEEL row: a: the byte, 0 OFF .. kFeelMax; or
    // c = 1: b is an int8 delta along OFF, DRIFT 1-6, WAVE 1-6. A pattern setting, like swing:
    // saved, undone and held by a LOCK.
    set_pattern_feel,
    // REL ALL and LEN ALL (core/moveall.hpp), FUNC + turn A / B in PERFORM. a: 0 REL ALL,
    // 1 LEN ALL; b: the amount as an int8, -64 .. 64, absolute (the UI keeps the position, as it
    // does a macro's). Performance, not edits: no file, no undo; a LOCK holds both.
    set_move_all,
    // LIVE RND (core/liverand.hpp), PERFORM's FUNC + turn D (the unit) and FUNC + push D (the
    // armed set). a: the unit, 0 OFF .. 5 RND16; b: the armed tracks, bit t = track t. Both
    // absolute; the UI keeps them, as it does REL ALL. Performance, not edits: no file, no undo;
    // a LOCK holds both.
    set_live_rnd,
    // ENERGY (core/energy.hpp), PERFORM's FUNC + turn C. a: the amount as an int8, -64 .. 64,
    // absolute (the UI keeps the position, as it does REL ALL's). Performance, not edits: no
    // file, no undo; a LOCK holds it.
    set_energy,
    // VariSpeed (core/varispeed.hpp), FUNC + turn VALUE. a: the amount as an int8, tenths of a
    // semitone, -120 .. 120, absolute (the UI keeps it, with the clock it sets). The pitch of
    // every machine and the delay's beat; Enaga's tempo is app/'s to set. Performance, not an
    // edit: no file, no undo, not in a LOCK.
    set_varispeed,

    // Sample audition (dsp/audition.hpp), push A in SAMPLES. `audition_start` is posted by the
    // card side once the file's stream is open and filled; what to play is in
    // Engine::audition_for_load(), written just before the post. `audition_stop` fades it out.
    // Either way, once the audio side has stopped reading the stream it bumps
    // Status::audition_epoch, and only then may the card side close it.
    audition_start,
    audition_stop,

    // JAM REC (app/storage.hpp, "jam recording"), the FUNC menu's second row: the finished master
    // to /RECORD as one long WAV through an Enaga recording stream. `jam_start` is posted by the
    // card side once Engine::jam_writer() is created; from the next block the audio side writes
    // every block into it. `jam_stop` ends that, and the audio side bumps Status::jam_epoch once
    // it has stopped writing: only then may the card side close the file. Neither touches a
    // project file.
    jam_start,
    jam_stop,

    count
};

// Exactly 16 bytes, as the spec requires. Everything an edit needs fits: a target, two small
// values and one float for the things that are genuinely continuous.
// `Command::c` set to this on set_param, set_track_length, set_pattern_swing, set_track_swing and
// set_track_level makes the value a DELTA, applied on the audio side to the authoritative value.
// The encoders send these: the UI reads engine state a block late, so "current + delta" computed
// there loses every click after the first in a block -- the reason the M3 commands are deltas.
// An absolute value (c = 0) is still what a load, a test or a recall sends.
constexpr uint8_t kDeltaFlag = 1;
// `Command::c` set to this on set_param and lock_delta makes `b` a delta in SEMITONES on the
// machine's tuning parameter (TUN or PIT): the knob moves to the byte nearest the next note
// (core/tuning.hpp, `tune_step`). What a knob click sends in NOTE view. Worked out on the audio
// side from the authoritative value, for the same reason as kDeltaFlag. On any other parameter
// it is an ordinary delta.
constexpr uint8_t kSemitoneFlag = 2;

// `Cmd::sound_preset`'s two special `b` values; every other value is a preset index.
constexpr uint8_t kSoundPresetKeep = 254;
constexpr uint8_t kSoundPresetBack = 255;

// `Cmd::set_duck`'s field, in `a`. Not on disk: KIT.BIN's duck section has its own layout.
enum class DuckField : uint8_t { depth, source, attack, hold, release, count };

// `Cmd::chain_edit`'s operation, in `a`.
// set, insert and remove carry the entry whole (bank b, slot c, repeats d, mute recall e);
// `pattern_delta`, `repeats_delta` and `mutes_delta` move the entry's pattern (through the 128,
// bank by bank), its repeat count (1-16) or its mute recall (0 none, 1-8) by the int8 in `b`,
// so knob clicks applied a block late are not lost. `song` sets song mode, b = 0 or 1, and
// ignores the entry.
enum class ChainEdit : uint8_t {
    set = 0, insert, remove, pattern_delta, repeats_delta, mutes_delta, song
};

// How a recorded hit meets the step it lands on (Cmd::record_hit's `d`). The UI decides, because
// it is the side that knows which pass a hit belongs to and what REC MODE is.
enum class RecWrite : uint8_t {
    note = 0,      // a track that plays one note: the hit replaces the step's note
    replace = 1,   // a chord track, the first note on this step this pass (REC MODE REPLACE):
                   // the step becomes that one note, its extras and chord type gone
    join = 2,      // a chord track, a note that adds to the step's chord: another note of a
                   // chord played this pass, or any note under REC MODE ADD
};

struct Command {
    Cmd cmd = Cmd::none;
    uint8_t track = 0;
    uint8_t step = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    // Three more bytes, which used to be padding. They cost nothing -- the struct has to be 16
    // bytes and `f` has to stay 4-byte aligned either way -- and they are what lets a recorded
    // hit carry its note, velocity and micro-timing in one command instead of three.
    uint8_t c = 0;
    uint8_t d = 0;
    uint8_t e = 0;
    float f = 0.0f;
    // A second 32-bit field, which used to be padding like c/d/e. A sample's byte offset into
    // the 64 MiB region needs 26 bits, so it needs a word of its own: `f` is a float and the
    // three spare bytes are not enough. Nothing is punned through `f` -- reading a float's bits
    // as an integer would be undefined behaviour for the sake of saving four bytes that the
    // struct has to carry anyway.
    uint32_t u = 0;
};

static_assert(sizeof(Command) == 16, "the spec fixes commands at 16 bytes");
static_assert(alignof(Command) == 4, "a misaligned 32-bit read on this chip silently misreads");

// A single-producer single-consumer ring. Capacity must be a power of two so the wrap is a mask
// rather than a division -- there is no division in the audio path.
template <uint32_t kCapacity>
class CommandRing {
public:
    static_assert(kCapacity >= 2, "a ring needs room");
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    // The most commands the audio side applies in one block. The spec's figure: a burst of edits
    // cannot be allowed to break the 100 ms deadline, and 64 is far more than a human generates.
    static constexpr uint32_t kMaxPerBlock = 64;

    // Main context only. Returns false when the ring is full, having changed nothing.
    bool post(const Command &command) {
        const uint32_t write = write_.load(std::memory_order_relaxed);
        const uint32_t next = (write + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire)) {
            // Load-then-store, not fetch_add: only the main context ever writes this, and the
            // tic6x toolchain is built without libatomic, so an atomic read-modify-write would
            // need a __atomic_fetch_add helper that does not exist. Single-writer discipline
            // makes the plain pair correct, which is what docs/SPEC.md asks for anyway.
            dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return false;
        }
        slots_[write] = command;
        write_.store(next, std::memory_order_release);   // publishes the payload with it
        return true;
    }

    // Audio context only. Returns false when empty.
    bool take(Command &out) {
        const uint32_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        out = slots_[read];
        read_.store((read + 1) & kMask, std::memory_order_release);
        return true;
    }

    // How many edits the UI posted that did not fit. Shown on screen: a dropped edit should be
    // visible, not mysterious.
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    bool empty() const {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

private:
    static constexpr uint32_t kMask = kCapacity - 1;

    Command slots_[kCapacity];
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    std::atomic<uint32_t> dropped_{0};
};

// 256 slots is 4 KiB: four blocks' worth of the per-block limit, so even a motion-recording
// sweep with every encoder moving has room.
using Commands = CommandRing<256>;

// --- the other direction --------------------------------------------------

// What the audio side publishes every block for the UI to read. Single writer (audio), single
// reader (UI), and the UI only ever wants the newest value -- so no ring, just words that are
// each written by one side. A torn read here costs one frame of a slightly stale meter, which
// is why these are plain relaxed atomics and not a lock.
//
// NOTE: no atomic read-modify-write anywhere in here. The tic6x toolchain is built with
// --disable-libatomic, so fetch_add on a byte needs a __atomic_fetch_add_1 helper that does not
// exist and the app fails to LINK (it compiles fine, which makes it a confusing failure). Plain
// loads and stores of a single word are fine, and with one writer per field the load-then-store
// pair below is correct without an RMW.
// What Status::launch_event reports (Track Launch). `arg` is a track for the first two, a slot
// for `captured`.
enum class LaunchEvent : uint8_t {
    read_only,       // an edit on lane `arg` was refused: it plays another pattern's part
    frozen,          // a launch landed on frozen lane `arg`: heard after UNFREEZE
    captured,        // CAPTURE wrote slot `arg` of the bank asked for, which now plays
    no_slot,         // CAPTURE found no free slot in the bank
    locks_full,      // CAPTURE: the parts' locks do not fit in one pattern
};

struct Status {
    std::atomic<uint32_t> tick{0};             // the clock's position, for the playhead
    // COUNT-IN (Engine::count_in_end_): the beats still to click before the pattern starts, 0
    // when none is running. While it is, `tick` stays 0 and nothing records.
    std::atomic<uint8_t> count_in_beats{0};
    // Ticks the pattern's position is behind the clock's: the count-in's length once it has
    // run, 0 without one. The recorder subtracts it from the clock it stamps a hit with.
    std::atomic<uint32_t> count_in_ticks{0};
    // Where the live pattern started, on that same count-in-less position (Engine::origin_): a
    // chain entry or a queued switch starts its pattern on a boundary, and steps and passes are
    // counted from there. The recorder subtracts it too, or a hit lands `origin mod loop` off.
    std::atomic<uint32_t> pattern_origin{0};
    std::atomic<uint32_t> blocks{0};           // audio blocks processed, for liveness
    std::atomic<uint8_t> playhead[8] = {};     // per-track step, for the grid
    std::atomic<uint8_t> triggered[8] = {};    // bumped on each trig, for the pad LEDs
    // How many slice points (core/onsets.hpp) the track's sample came with, 0-15, for EDIT's
    // SLICES line. Written when the audio side applies a set_sample or set_stream.
    std::atomic<uint8_t> slice_points[8] = {};
    std::atomic<uint16_t> peak_milli{0};       // master peak x 1000
    // The screen frame (docs/CONTROLS.md, "The screen frame"). `meter_milli` is the master peak
    // with a meter's fall already applied on the audio side, so a 20 fps draw that samples one
    // block in forty still sees every hit. `limit_seen` is bumped on each block whose peak
    // reached full scale (the limiter working): the UI compares, a wrap is harmless.
    std::atomic<uint16_t> meter_milli{0};
    // The glue compressor's gain, thousandths (1000 = no reduction), for MIXER's master page.
    std::atomic<uint16_t> comp_gain_milli{1000};
    std::atomic<uint8_t> limit_seen{0};
    // Where the playing pattern is: ticks into its current pass, and the pass's length in ticks
    // (master length x 24). For the top line's bar.beat and the progress line under it.
    std::atomic<uint16_t> pattern_into{0};
    std::atomic<uint16_t> pattern_loop{0};
    // How many TRACKS rendered last block (an active machine, not muted), 0-8. Not a voice
    // count: a six-note POLY chord is one. Shown as TRK on the status line.
    std::atomic<uint8_t> voices{0};
    // The CPU guard's level, 0-4 (core/cpuguard.hpp): 0 is off, each step sheds two POLY voices
    // from the shared budget and shrinks every GRAIN pool. Shown on the bottom line as GUARD.
    std::atomic<uint8_t> cpu_guard{0};
    std::atomic<uint8_t> running{0};

    // M3. How deep the undo history is, so the screen can say whether F1+CLR will do anything,
    // and a count of edits the audio side had to refuse -- a paste that did not fit in the lock
    // pool, almost always. A refused edit has to be visible: the same reasoning as the dropped
    // command count above, and the same failure if it is not (the user repeats the edit and
    // concludes the box is broken).
    std::atomic<uint8_t> undo_depth{0};
    std::atomic<uint8_t> redo_depth{0};
    std::atomic<uint32_t> refused{0};
    // Bumped by the audio side when a MUTATE changed nothing: an empty track, the amount at 0, a
    // rotation of identical steps, a density already at its end. The UI watches it so the press
    // says NOTHING TO MUTATE instead of naming a mutation that never happened -- on the unit that
    // looked exactly like a pad that does nothing. A wrap is harmless: the UI compares.
    std::atomic<uint8_t> mutate_unchanged{0};
    // Bumped by the audio side each time EVOLVE changed a track on a pass boundary: the card
    // side has to mark the bank dirty for a pattern edit no command made, and the scenario log
    // says EVOLVE. A wrap is harmless: the UI compares.
    std::atomic<uint8_t> evolve_turns{0};
    // Bumped when a load set EVOLVE from the file: the UI's copies of the mutation, its amount
    // and the mode (shared with PERFORM's MUTATE) follow. A wrap is harmless: the UI compares.
    std::atomic<uint8_t> evolve_loaded{0};

    // M4. Per track, bumped by the audio side when it stops reading that track's sample. The
    // loader watches for the change before it releases the memory and loads over it -- the
    // generation counter docs/SPEC.md requires for a sample swap. A wrap is harmless: the UI
    // compares against the value it saw, it does not count.
    std::atomic<uint8_t> sample_epoch[8] = {};

    // Freeze (dsp/frozen.hpp). `frozen` has bit t set while track t plays its frozen loop.
    // `freeze_epoch[t]` is bumped when the audio side stops reading track t's freeze slot, which
    // is what the main context waits for before it gives the slot back -- the sample epoch's
    // protocol, on a counter of its own so the two swaps can never be mistaken for each other.
    std::atomic<uint8_t> frozen{0};
    std::atomic<uint8_t> freeze_epoch[8] = {};
    // Bumped when the audio side refuses a freeze's take (Cmd::resample_arm with b 3): the main
    // context checks before posting, but a check one block old can lose a race (a loop armed in
    // the same block), and without this it would wait for a take that never comes.
    std::atomic<uint8_t> freeze_refused{0};

    // M6. Resampling. `resample_state` is 0 idle, 1 armed (waiting for the bar line), 2
    // recording, 3 a finished take the card has not had yet; `resample_frames` is how much is in
    // it. The screen draws a progress bar from the two, and the card side watches for state 3 --
    // the same single-writer-word discipline as everything else here, because the take's buffer
    // belongs to the audio side until then (dsp/capture.hpp).
    std::atomic<uint8_t> resample_state{0};
    std::atomic<uint32_t> resample_frames{0};
    std::atomic<uint8_t> resample_clamped{0};   // the take was longer than the buffer holds
    // The master looper (dsp/looper.hpp): 0 idle, 1 armed (waiting for the bar line), 2
    // recording, 3 playing; and the bars of the take armed, running or playing.
    std::atomic<uint8_t> looper_state{0};
    std::atomic<uint8_t> looper_bars{0};
    // The audio input's peak over the last block, 0-1000 (per mille of full scale), so the
    // RESAMPLE screen can show a level to set the input by before a take. 1000 means it clipped.
    std::atomic<uint16_t> input_peak{0};

    // Streaming. Per track, bumped (with release) each time the audio side stops reading the
    // track's stream and wants it reopened after the head: a trigger, a loop wrapping, or a new
    // streamed file. The card side reopens and answers with Cmd::stream_attach carrying the value
    // it saw. Written after the audio side stopped reading, so the card side may close the
    // stream as soon as it sees the change. `stream_late` counts taps that came before the card
    // did (dsp/sample.hpp), summed over the tracks, for the screen's STREAM LATE.
    std::atomic<uint16_t> stream_want[8] = {};
    std::atomic<uint32_t> stream_late{0};

    // Sample audition (dsp/audition.hpp). `audition_epoch` is bumped (with release) each time an
    // audition ends -- its last frame, or a stop -- after the audio side stopped reading its
    // stream, so the card side may close the stream the moment it sees the change.
    // `audition_frame` is where it is, in file frames, for the screen (0 when none plays).
    std::atomic<uint8_t> audition_epoch{0};
    std::atomic<uint32_t> audition_frame{0};

    // JAM REC. Bumped (with release) by every Cmd::jam_stop once the audio side has stopped
    // writing the jam's stream, so the card side may close it the moment it sees the change.
    std::atomic<uint8_t> jam_epoch{0};

    // Where a SAMPLE or GRAIN track is in its sample, for EDIT's waveform (core/overview.hpp,
    // WaveView): the window or the grain bracket, the slice starts, and up to eight playheads or
    // grains with a brightness each, all in the overview's columns. Written every
    // kWaveEveryBlocks blocks by the audio side for each track playing either machine; the
    // screen reads the selected track's. A read torn between two blocks draws one frame with a
    // playhead from one and a window from the other, which nobody can see.
    struct WaveStatus {
        std::atomic<uint8_t> lo{WaveView::kNone};
        std::atomic<uint8_t> hi{WaveView::kNone};
        std::atomic<uint32_t> cuts[4] = {};
        std::atomic<uint8_t> mark[WaveView::kMarks] = {};
        std::atomic<uint8_t> glow[WaveView::kMarks] = {};

        // Audio context only: one writer.
        void publish(const WaveView &view) {
            lo.store(view.lo, std::memory_order_relaxed);
            hi.store(view.hi, std::memory_order_relaxed);
            for (int i = 0; i < 4; i++) cuts[i].store(view.cuts[i], std::memory_order_relaxed);
            for (int i = 0; i < WaveView::kMarks; i++) {
                mark[i].store(view.mark[i], std::memory_order_relaxed);
                glow[i].store(view.glow[i], std::memory_order_relaxed);
            }
        }
        WaveView read() const {
            WaveView view;
            view.lo = lo.load(std::memory_order_relaxed);
            view.hi = hi.load(std::memory_order_relaxed);
            for (int i = 0; i < 4; i++) view.cuts[i] = cuts[i].load(std::memory_order_relaxed);
            for (int i = 0; i < WaveView::kMarks; i++) {
                view.mark[i] = mark[i].load(std::memory_order_relaxed);
                view.glow[i] = glow[i].load(std::memory_order_relaxed);
            }
            return view;
        }
    };
    WaveStatus wave[8];
    // And each track's amplitude envelope, for the dot on EDIT's AMP and ASR curve
    // (Machine::envelope_now): the stage (0 idle, 1 attack, 2 decay, 3 sustain, 4 release) and
    // the level, 0-255. Written with `wave`, for every track whose machine says.
    std::atomic<uint8_t> env_stage[8] = {};
    std::atomic<uint8_t> env_level[8] = {};
    // Live visuals (core/scope.hpp has the master's samples). Each track's level as the meter
    // reads it: the block's peak of the machine's output, before the chain and the fader, or
    // the last reading falling as the master meter falls; thousandths of full scale. 0 for a
    // muted or silent track once it has fallen. And each track's LFO phase, 0-255 a cycle, for
    // the dot riding the LFO page's shape.
    std::atomic<uint16_t> track_milli[8] = {};
    std::atomic<uint8_t> lfo_phase[8] = {};
    // What each track sounds now (Engine::sounding: the kit with the performance layer, the
    // step's locks and the LFO over it), the few bytes of it the main context acts on or draws,
    // so it never reads `effective_` or `modulated_`, which the audio side rewrites every block
    // (Engine::publish_heard). `heard_table` is the table a WAVE track's TBL plays, kNotHeard on
    // any other machine: Engine::step_waves builds from it. `heard_pos` is that track's POS byte.
    // `heard_sus` is the ASR's SUS byte (a KEYS note holds on it). `lfo_shown` is where the LFO
    // has its destination (Engine::lfo_shown), kNotHeard while the LFO moves nothing.
    // Engine::init publishes them once, so they are right before the first block.
    static constexpr uint8_t kNotHeard = 0xFF;
    std::atomic<uint8_t> heard_table[8] = {};
    std::atomic<uint8_t> heard_pos[8] = {};
    std::atomic<uint8_t> heard_sus[8] = {};
    std::atomic<uint8_t> lfo_shown[8] = {};
    // The insert filter's CUT, RES and MOD bytes as heard (FLTR slots 5-7), for the drawn
    // response on a drum's or SAMPLE's FLTR page (app/ui_live.hpp).
    std::atomic<uint8_t> heard_filter[8][3] = {};
    // Each MOD slot's destination as heard (the byte the slots and the LFO left), kNotHeard for
    // a slot that is off: the dot on EDIT's bar (app/ui_modslots.hpp).
    std::atomic<uint8_t> mod_heard[8][2] = {};
    // LOCK (Cmd::lock_store, lock_tap). `lock_state`: bit 0 the playing pattern has a lock, bit 1 the live set
    // differs from it (the screen's L+), bit 2 the next tap swaps back. `lock_event` is bumped
    // once per store or tap the audio side applied, with `lock_last` saying what it did: a
    // LockTap, or kLockStored. The UI watches the event for its notice and to take the morph and
    // macro positions back from the engine after a jump.
    std::atomic<uint8_t> lock_state{0};
    std::atomic<uint8_t> lock_event{0};
    std::atomic<uint8_t> lock_last{0};

    // Which pattern is live, so the UI can draw it without reaching into the project.
    std::atomic<uint8_t> live_bank{0};
    std::atomic<uint8_t> live_slot{0};

    // Song mode. Bumped by the audio side when a song reaches its end: the main context watches
    // for the change and stops the transport, which only it can do (<enaga/clock.hpp>).
    std::atomic<uint8_t> song_end{0};

    // Track Launch. Bumped by the audio side for each thing the UI says: `launch_last` is a
    // LaunchEvent, `launch_arg` its track or the slot CAPTURE wrote.
    std::atomic<uint8_t> launch_event{0};
    std::atomic<uint8_t> launch_last{0};
    std::atomic<uint8_t> launch_arg{0};
    // Bumped when a queued launch lands: PROJECT.BIN changed with no command at that moment, so
    // the main context marks the project to save (a save between queueing and landing would
    // otherwise have cleared the flag the launch command set).
    std::atomic<uint8_t> launch_landed{0};

    // Audio context only, one writer: the payload first, so a reader that sees the new event
    // (with acquire) sees what it was.
    void bump_launch_event(uint8_t last, uint8_t arg) {
        launch_last.store(last, std::memory_order_relaxed);
        launch_arg.store(arg, std::memory_order_relaxed);
        launch_event.store(uint8_t(launch_event.load(std::memory_order_relaxed) + 1),
                           std::memory_order_release);
    }

    // Audio context only, one writer, as bump_triggered.
    void bump_song_end() {
        song_end.store(uint8_t(song_end.load(std::memory_order_relaxed) + 1),
                       std::memory_order_release);
    }

    // Audio context only, so the load and the store cannot interleave with another writer.
    void bump_sample_epoch(int track) {
        const uint8_t next = uint8_t(sample_epoch[track].load(std::memory_order_relaxed) + 1);
        sample_epoch[track].store(next, std::memory_order_relaxed);
    }

    // Audio context only, as bump_sample_epoch.
    void bump_freeze_epoch(int track) {
        const uint8_t next = uint8_t(freeze_epoch[track].load(std::memory_order_relaxed) + 1);
        freeze_epoch[track].store(next, std::memory_order_relaxed);
    }

    // Audio context only, as bump_sample_epoch.
    void bump_freeze_refused() {
        freeze_refused.store(uint8_t(freeze_refused.load(std::memory_order_relaxed) + 1),
                             std::memory_order_relaxed);
    }

    // Audio context only, so load-then-store is safe with one writer (see the note above).
    void bump_refused() {
        refused.store(refused.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // Audio context only, one writer, as bump_song_end.
    void bump_limit_seen() {
        limit_seen.store(uint8_t(limit_seen.load(std::memory_order_relaxed) + 1),
                         std::memory_order_relaxed);
    }

    // Audio context only, one writer: `last` first, so a reader that sees the new event (with
    // acquire) sees what it was.
    void bump_lock_event(uint8_t last) {
        lock_last.store(last, std::memory_order_relaxed);
        lock_event.store(uint8_t(lock_event.load(std::memory_order_relaxed) + 1),
                         std::memory_order_release);
    }

    // Audio context only, one writer, as bump_song_end.
    void bump_evolve_loaded() {
        evolve_loaded.store(uint8_t(evolve_loaded.load(std::memory_order_relaxed) + 1),
                            std::memory_order_release);
    }

    void bump_evolve_turns() {
        evolve_turns.store(uint8_t(evolve_turns.load(std::memory_order_relaxed) + 1),
                           std::memory_order_release);
    }

    // Audio context only, one writer, as bump_song_end.
    void bump_mutate_unchanged() {
        mutate_unchanged.store(uint8_t(mutate_unchanged.load(std::memory_order_relaxed) + 1),
                               std::memory_order_release);
    }

    void publish_playhead(int track, uint8_t step) {
        playhead[track].store(step, std::memory_order_relaxed);
    }

    // Audio context only, so the read and the write cannot interleave with another writer.
    // The UI watches this for a change rather than reading the count, so a wrap is harmless.
    void bump_triggered(int track) {
        const uint8_t next = uint8_t(triggered[track].load(std::memory_order_relaxed) + 1);
        triggered[track].store(next, std::memory_order_relaxed);
    }

    void bump_blocks() {
        blocks.store(blocks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

}  // namespace strata
