// app/engine.cpp: the audio side, from the command ring to the machines.
//
// `tests/test_app.cpp` covers the harness and the four bugs a review of `app/` found. This file
// is the thorough pass over the engine itself -- the gate conversion, chord expansion, the tick
// resolution that swing, speed, polymeter, ratchets and probability all go through, the bar
// counter, undo, the pattern queue and chains, the mutes and the performance layer.
//
// ## What is asserted
//
// Properties, not sample values: tests/test_va.cpp's header explains the house style, and it
// matters more here than anywhere, because the engine's output goes through eight machines, eight
// insert chains, two send buses and a master limiter. So where an assertion has to be about
// sound it is about a RATIO or an exact zero, and where it can be about a number it is about one
// the engine publishes -- `Status::playhead`, `Status::triggered`, `Engine::performed`,
// `VaPoly::voice_note` -- which is also what the UI draws from.
//
// Two measurements deserve their own note.
//
// **Where a note starts** is read out of `last_block()`, which is only meaningful from exact
// silence; `clean()` below is what produces it, by closing the sends the rig's patch opens and
// then draining the buses.
//
// **How long a note lasts** is read from `VaPoly::sounding_voices()` rather than from the audio.
// That accessor follows the amplitude envelope's LEVEL, which neither the voice's noise nor the
// master bus touches, so the answer is a deterministic function of the gate and DEC alone and
// the only imprecision is the 64-frame block. Comparing the audio instead would compare two
// runs whose noise streams have diverged, because `VaVoice`'s RNG carries across notes.
//
// ## What cannot be checked here
//
// `on_transport` and every other Enaga event live in app/app.cpp, which host/Makefile
// deliberately does not build (its `ENAGA_APP(Strata)` defines a device package's entry points).
// What this file can check is the half of CLAUDE.md's transport rule that IS in the engine: that
// the engine never calls the transport itself, and that what it plays comes from the clock's own
// state rather than from a command. The sample region is NOT in that position: the rig's
// `Storage::init` hands the engine its 64 MiB region exactly as `App::init` does on the unit, so
// `Cmd::set_sample`'s offset, frame-count and alignment checks are reachable here, and
// tests/test_engine_gaps.cpp drives them.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include <enaga/abi.h>
#include <enaga/clock.hpp>

using namespace strata;
using strata::test::FakeHost;

namespace {

using strata::test::fresh;
using strata::test::kit_value;
using strata::test::last_block;
using strata::test::peak_of;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

constexpr int kBlk = strata::test::kRigBlock;
constexpr int kStereo = strata::test::kRigStereo;
constexpr float kRate = float(ENAGA_SAMPLE_RATE);

// The two tracks whose machines can be asked what they are playing. Track 7 of the default kit
// (index 6) is CHORD on VA POLY -- the only machine that honours `Trig::gate_frames` and the only
// one a chord byte reaches -- and track 6 (index 5) is BASS on VA MONO, which is the other half
// of the fold asymmetry below.
constexpr uint8_t kPoly = 6;
constexpr uint8_t kMono = 5;

// Parameter slots, named rather than counted at each use (dsp/track.hpp's four pages of four).
constexpr uint8_t kDrv = 4, kFilterMod = 7;
constexpr uint8_t kDec = 8, kLvl = 11;
constexpr uint8_t kLof = 12, kSendA = 14, kSendB = 15;

static_assert(kDrv == page_first(Page::fltr) + 0, "FLTR slot 0 is DRV");
static_assert(kFilterMod == page_first(Page::fltr) + 3, "FLTR slot 3 is MOD, the filter switch");
static_assert(kDec == page_first(Page::amp) + 0, "AMP slot 0 is the decay on every machine");
static_assert(kLvl == page_first(Page::amp) + 3, "AMP slot 3 is LVL on every machine");
static_assert(kLof == page_first(Page::fx) + 0, "FX slot 0 is the lo-fi");
static_assert(kSendA == page_first(Page::fx) + 2, "FX slot 2 is send A, the delay");
static_assert(kSendB == page_first(Page::fx) + 3, "FX slot 3 is send B, the reverb");

// --- reading the engine ----------------------------------------------------------------------

const VaPoly &poly() {
    return *static_cast<const VaPoly *>(strata::test::engine().machine(kPoly));
}

const VaMono &mono() {
    return *static_cast<const VaMono *>(strata::test::engine().machine(kMono));
}

uint8_t triggers(int track) {
    return strata::test::engine().status().triggered[track].load(std::memory_order_relaxed);
}

int playhead(int track) {
    return int(strata::test::engine().status().playhead[track].load(std::memory_order_relaxed));
}

uint32_t refused() {
    return strata::test::engine().status().refused.load(std::memory_order_relaxed);
}

// Render `blocks` blocks and return the total energy. Measured over a whole span rather than as
// a block peak, because a kick's block peak OSCILLATES with its phase against the block length
// (about 15 blocks per cycle at 50 Hz), so "louder" read from one block is noise. Energy over the
// span is monotone in what actually sounded.
float energy(int blocks) {
    float total = 0.0f;
    for (int b = 0; b < blocks; b++) {
        pump();
        const float *block = strata::test::last_block();
        for (int i = 0; i < strata::test::kRigStereo; i++) total += block[i] * block[i];
    }
    return total;
}

int live_bank() {
    return int(strata::test::engine().status().live_bank.load(std::memory_order_relaxed));
}

int live_slot() {
    return int(strata::test::engine().status().live_slot.load(std::memory_order_relaxed));
}

int undo_depth() {
    return int(strata::test::engine().status().undo_depth.load(std::memory_order_relaxed));
}

int redo_depth() {
    return int(strata::test::engine().status().redo_depth.load(std::memory_order_relaxed));
}

int running() {
    return int(strata::test::engine().status().running.load(std::memory_order_relaxed));
}

bool trig_at(int track, int step) {
    return (strata::test::engine().pattern().tracks[track].steps[step].flags & kStepTrig) != 0;
}

const Step &live_step(int track, int step) {
    return strata::test::engine().pattern().tracks[track].steps[step];
}

// --- posting ---------------------------------------------------------------------------------
//
// The rig's `post` carries the five bytes most commands need. These three are for the commands
// that need the wide fields as well, built the way app/app.cpp's `post_command` builds them.

void post_u(Cmd cmd, uint32_t u, uint8_t a = 0, uint8_t track = 0) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.a = a;
    command.u = u;
    strata::test::engine().commands().post(command);
}

// A delta on a send or master FIELD, the way app/app.hpp posts one: `a` names the field and `b`
// carries a signed byte. For the clamps and the refusals, where the exact landing is the point.
void post_delta_to(Cmd cmd, uint8_t field, int delta) {
    Command command;
    command.cmd = cmd;
    command.a = field;
    command.b = uint8_t(int8_t(delta < -120 ? -120 : (delta > 120 ? 120 : delta)));
    strata::test::engine().commands().post(command);
}

void post_f(Cmd cmd, float f, uint8_t a = 0, uint8_t track = 0) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.a = a;
    command.f = f;
    strata::test::engine().commands().post(command);
}

void post_macro_assign(uint8_t macro, uint8_t track, uint8_t param, uint8_t low, uint8_t high) {
    Command command;
    command.cmd = Cmd::macro_assign;
    command.track = track;
    command.a = macro;
    command.b = param;
    command.c = low;
    command.d = high;
    strata::test::engine().commands().post(command);
}

// --- a known starting state ------------------------------------------------------------------

// Drain the master buses, so `last_block()` starts from EXACT silence.
//
// The rig's patch sets all sixteen parameters of every track to 64, which opens both sends and
// switches the lo-fi on, so a note from the test before rings in the reverb for seconds --
// `Cmd::stop` cuts voices, not sends. With every send closed the mixer stops running the delay
// and the reverb outright once their tails run out, and the output is then exactly zero.
//
// It takes a RUN of silent blocks and not one, which was worth learning the hard way: the delay
// is silent BETWEEN its repeats, so the first zero block means nothing. Its time is 0.375 s
// (`SendSettings::delay_seconds`, which nothing can change -- see
// `engine_a_command_it_does_not_handle_changes_nothing`), which is 281 silent blocks, so an echo
// left in the line pops out a third of a second into a test that thought it had silence. The
// longest either bus keeps running unfed is `delay_tail_blocks_`, about 3,300 blocks, after which
// the output is exactly zero for ever.
constexpr int kSilentRun = 400;

bool settle(int limit = 8000) {
    int quiet = 0;
    for (int i = 0; i < limit && quiet < kSilentRun; i++) {
        quiet = pump() <= 0.0f ? quiet + 1 : 0;
    }
    return quiet >= kSilentRun;
}

// `fresh()`, plus everything `fresh()` cannot reach.
//
// A project load replaces the patterns and the kit, and nothing else: the mutes, the morph, the
// macros, the scenes, the throw, the fill and the beat repeat are all audio-side performance
// state that lives in no Pattern and no Kit, so a test that did not put them back would inherit
// whatever the test before it -- in this file or any other -- happened to leave set.
Engine &clean() {
    Engine &engine = fresh();

    for (uint8_t t = 0; t < kTracks; t++) {
        post(Cmd::set_mute, t, 0, 0, 0);
        post(Cmd::set_param, t, 0, kDrv, 0);
        post(Cmd::set_param, t, 0, kFilterMod, 0);
        post(Cmd::set_param, t, 0, kLof, 0);
        post(Cmd::set_param, t, 0, kSendA, 0);
        post(Cmd::set_param, t, 0, kSendB, 0);
    }
    post(Cmd::set_fill, 0, 0, 0);
    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::off));
    post(Cmd::set_throw, 0, 0, 0);
    post(Cmd::set_repeat, 0, 0, uint8_t(RepeatLength::sixteenth), 0, 0);
    for (uint8_t t = 0; t < kTracks; t++) post(Cmd::note_repeat, t, 0, 0, 0, 0);
    post_u(Cmd::set_morph, 0);
    for (uint8_t m = 0; m < kMacros; m++) {
        post(Cmd::macro_clear, 0, 0, m);
        post(Cmd::set_macro, 0, 0, m, 0);
    }
    for (uint8_t s = 0; s < kScenes; s++) post(Cmd::scene_erase, 0, 0, s);
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);

    // `a = 1`, not `c = 1`: the engine reads `command.a` for "cut all voices" and so does
    // app/app.cpp's STOP (app/commands.hpp). tests/apprig.cpp passes `c`, which makes its own
    // reset a release with a 60 ms tail rather than the kill its comment describes.
    post(Cmd::stop, 0, 0, 1);

    while (!engine.commands().empty()) pump();
    settle();
    return engine;
}

// --- driving the clock -----------------------------------------------------------------------

void tick_at(uint32_t tick, uint32_t frame = 0) {
    FakeHost::the().deliver_tick(tick, frame);
    pump();
}

// Deliver one tick and say how many events it produced on `track`. `Status::triggered` is bumped
// once per event the engine fires, which is once per step and once per ratchet hit.
int fired_on(int track, uint32_t tick, uint32_t frame = 0) {
    const uint8_t before = triggers(track);
    FakeHost::the().deliver_tick(tick, frame);
    pump();
    return int(uint8_t(triggers(track) - before));
}

// Drive the bar counter into a known state and return the bar it is on.
//
// `bar_`, `next_bar_tick_` and `bar_known_` are in no Pattern and no Kit, and `Cmd::project_loaded`
// does not reset them, so a test about bars starts wherever the test before it stopped. Tick 0
// leaves `next_bar_tick_` at most one bar ahead whatever came before; a tick two bars past that
// is then far enough ahead to force the resync branch, which is a division and not a count.
uint32_t sync_bars() {
    tick_at(0);
    tick_at(2u * kTicksPerBar);
    return 2;
}

// --- step fields -----------------------------------------------------------------------------
//
// Every one of these is a WALK of the field from its current value, because that is the only
// thing `Cmd::step_field_delta` can do: an encoder sends several clicks per block and the UI
// cannot read the pattern as it is, so the delta is applied on this side (app/commands.hpp).

void set_length_index(uint8_t track, uint8_t step, int index) {
    const int now = int(live_step(track, step).length);
    post(Cmd::step_field_delta, track, step, uint8_t(StepField::length),
         uint8_t(int8_t(index - now)));
    pump();
}

void set_probability(uint8_t track, uint8_t step, int value) {
    const int now = int(live_step(track, step).probability);
    post(Cmd::step_field_delta, track, step, uint8_t(StepField::probability),
         uint8_t(int8_t(value - now)));
    pump();
}

void set_chord(uint8_t track, uint8_t step, ChordType type, uint8_t inversion = 0) {
    // The encoder walks named chords, not bytes (core/stepedit.hpp), so the delta is a distance
    // in that walk.
    const int now = chord_walk_position(live_step(track, step).chord);
    const int want = chord_walk_position(chord_pack(type, inversion));
    post(Cmd::step_field_delta, track, step, uint8_t(StepField::chord),
         uint8_t(int8_t(want - now)));
    pump();
}

void set_condition_position(uint8_t track, uint8_t step, int position) {
    const int now = condition_position(live_step(track, step).condition);
    post(Cmd::step_field_delta, track, step, uint8_t(StepField::condition),
         uint8_t(int8_t(position - now)));
    pump();
}

// --- the CHORD track's note length -----------------------------------------------------------

// Fire step 0 of the CHORD track on tick 0, then count the frames until its last voice stops
// sounding. See the file header on why this is read from the envelope and not from the audio.
int frames_until_silent(int limit_blocks = 8000) {
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    for (int b = 0; b < limit_blocks; b++) {
        pump();
        if (poly().sounding_voices() == 0) return (b + 1) * kBlk;
    }
    return -1;
}

// A CHORD track note whose DEC is long enough that the GATE, and not the envelope, is what ends
// it, at a chosen table entry, track speed and tempo.
void arm_a_gated_chord_note(int length_index, Speed speed, float bpm) {
    clean();
    FakeHost::the().set_tempo(bpm);
    post(Cmd::set_param, kPoly, 0, kDec, 127);
    post(Cmd::set_track_speed, kPoly, 0, uint8_t(speed));
    put_trig(kPoly, 0);
    set_length_index(kPoly, 0, length_index);
}

// --- scenes ----------------------------------------------------------------------------------

// Scene 0 and scene 1, differing in one parameter of one track and in nothing else, assigned to
// slots A and B. A capture reads `project_.kit` and a knob turn writes `params_` -- two separate
// structures that only `Cmd::kit_store` copies between -- so the store is what makes the capture
// mean the value just set. See `engine_a_scene_capture_takes_the_stored_kit_and_not_the_knob`.
void two_scenes_differing_in(uint8_t track, uint8_t param, uint8_t low, uint8_t high) {
    post(Cmd::scene_erase, 0, 0, 0);
    post(Cmd::scene_erase, 0, 0, 1);
    post(Cmd::set_param, track, 0, param, low);
    post(Cmd::kit_store);
    post(Cmd::scene_capture, track, 0, 0, 0);
    post(Cmd::set_param, track, 0, param, high);
    post(Cmd::kit_store);
    post(Cmd::scene_capture, track, 0, 1, 0);
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);
    pump();
}

}  // namespace

// --- Engine::gate_frames_for -----------------------------------------------------------------
//
// `Step::length` has been stored since M3 and became audible two commits ago. The conversion is
// sixteenths/16 x step_ticks x samples_per_tick -- the one core/timing.hpp refuses to do itself,
// because a sixteenth of a 24-tick step is 1.5 ticks and the 96 PPQN grid cannot hold it -- and
// nothing tested the arithmetic.

TEST(engine_a_gate_is_sixteenths_of_a_step_at_the_tracks_own_speed_and_tempo) {
    // Three combinations that core/timing's own `length_samples` says are the same number of
    // frames, each reached by halving a different factor: half the table entry, half the step,
    // half the tick. If the engine dropped any one of them -- the 1/16, the step's ticks, the
    // tempo -- the three would stop agreeing.
    const float spt130 = kRate * 60.0f / (130.0f * float(enaga::clock::ppqn));
    const float spt260 = kRate * 60.0f / (260.0f * float(enaga::clock::ppqn));
    const float half_step_1x_130 = length_samples(6, ticks_per_step(Speed::x1), spt130);
    const float one_step_2x_130 = length_samples(8, ticks_per_step(Speed::x2), spt130);
    const float one_step_1x_260 = length_samples(8, ticks_per_step(Speed::x1), spt260);
    CHECK_NEAR(half_step_1x_130, one_step_2x_130, 1e-3);
    CHECK_NEAR(half_step_1x_130, one_step_1x_260, 1e-3);

    arm_a_gated_chord_note(6, Speed::x1, 130.0f);
    const int half_step = frames_until_silent();
    arm_a_gated_chord_note(8, Speed::x2, 130.0f);
    const int at_double_speed = frames_until_silent();
    arm_a_gated_chord_note(8, Speed::x1, 260.0f);
    const int at_double_tempo = frames_until_silent();

    CHECK(half_step > 0);
    CHECK_EQ(half_step, at_double_speed);
    CHECK_EQ(half_step, at_double_tempo);

    // And the gate really is what ended those notes rather than DEC: the table entry that asks
    // for twice as long holds for very nearly twice as long. The slack is one block, plus the
    // slightly longer release a note that was released later has left to run.
    arm_a_gated_chord_note(8, Speed::x1, 130.0f);
    const int one_step = frames_until_silent();
    const float expected = length_samples(8, ticks_per_step(Speed::x1), spt130) -
                           length_samples(6, ticks_per_step(Speed::x1), spt130);
    CHECK(expected > 2000.0f);
    CHECK(float(one_step - half_step) > expected - 256.0f);
    CHECK(float(one_step - half_step) < expected + 256.0f);

    // Four steps against two is the same difference again, one table entry further up.
    arm_a_gated_chord_note(10, Speed::x1, 130.0f);
    const int two_steps = frames_until_silent();
    const float expected_two = length_samples(10, ticks_per_step(Speed::x1), spt130) -
                               length_samples(8, ticks_per_step(Speed::x1), spt130);
    CHECK(float(two_steps - one_step) > expected_two - 384.0f);
    CHECK(float(two_steps - one_step) < expected_two + 384.0f);
}

TEST(engine_a_length_index_of_zero_asks_for_no_gate) {
    // Index 0 is core/timing.hpp's invalid sentinel, reserved so a zeroed or corrupt byte is
    // refusable rather than meaning the shortest note in the table. `gate_frames_for` answers 0
    // for it, which `Trig::gate_frames` defines as NO gate -- what every machine did before the
    // field existed. So the step has to ring exactly as long as a pad hit, which carries no gate
    // at all.
    //
    // The panel cannot produce the byte: core/stepedit clamps the field to 1..kLengthMax. A
    // damaged file can, which is the case this is about, so it is written through the load hole
    // -- main context, transport stopped, which is the contract `project_for_load` states.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kDec, 64);
    put_trig(kPoly, 0);
    engine.project_for_load().live().tracks[kPoly].steps[0].length = 0;
    pump();
    CHECK_EQ(int(live_step(kPoly, 0).length), 0);
    CHECK(!length_valid(live_step(kPoly, 0).length));
    const int sentinel = frames_until_silent();

    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 64);
    int pad = -1;
    post(Cmd::trigger_note, kPoly, 0, 60, 100);
    for (int b = 0; b < 8000; b++) {
        pump();
        if (poly().sounding_voices() == 0) {
            pad = (b + 1) * kBlk;
            break;
        }
    }

    CHECK(sentinel > 0);
    CHECK_EQ(sentinel, pad);

    // ... and a valid index really does cut it short, so the equality above is about the
    // sentinel and not about a gate that never worked.
    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 64);
    put_trig(kPoly, 0);
    CHECK_EQ(int(live_step(kPoly, 0).length), 8);   // a trig's own default: one step
    const int gated = frames_until_silent();
    CHECK(gated > 0);
    CHECK(gated < sentinel);
    CHECK(sentinel - gated > 4000);
}

TEST(engine_a_gate_shortens_a_chord_note) {
    // The audible half of it, on the one machine that honours the field (dsp/vapoly.cpp is the
    // only reader of `Trig::gate_frames`). The window is chosen past the gate AND past its
    // release, so a gated note is gone there and an ungated one is still on its own envelope.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kDec, 64);
    put_trig(kPoly, 0);
    set_length_index(kPoly, 0, 2);                 // an eighth of a step: about 14 ms at 130 BPM
    CHECK_EQ(int(live_step(kPoly, 0).length), 2);
    CHECK_EQ(pump(), 0.0f);

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(20) > 0.01f);                       // it sounded
    pump(140);
    const float late_gated = pump(140);

    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 64);
    put_trig(kPoly, 0);
    engine.project_for_load().live().tracks[kPoly].steps[0].length = 0;
    pump();
    CHECK_EQ(pump(), 0.0f);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(20) > 0.01f);
    pump(140);
    const float late_ungated = pump(140);

    CHECK(late_gated < 1e-5f);
    CHECK(late_ungated > 1e-4f);
    CHECK(late_ungated > 20.0f * late_gated);
}

// --- chords ----------------------------------------------------------------------------------
//
// A POLY step's chord byte is expanded in `fire()` onto VA POLY's voices ON ONE FRAME. That is
// deliberate: dsp/vapoly.hpp's allocator counts a voice busy from the moment it takes a trig
// rather than from the frame that trig starts on, precisely so that several notes on one frame
// take several voices instead of replacing one another.

TEST(engine_a_min7_step_sounds_four_notes_on_one_frame) {
    Engine &engine = clean();
    // The blank project's scale is chromatic, under which folding is the identity, so the notes
    // are the table's own intervals on the step's note: 0, 3, 7, 10 (core/chord.hpp).
    CHECK_EQ(int(engine.pattern().scale), int(Scale::chromatic));
    put_trig(kPoly, 0);
    set_chord(kPoly, 0, ChordType::min7);
    CHECK(chord_type(live_step(kPoly, 0).chord) == ChordType::min7);
    CHECK_EQ(int(live_step(kPoly, 0).note), 60);

    const uint8_t before = triggers(kPoly);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();

    CHECK_EQ(poly().busy_voices(), 4);
    CHECK_EQ(poly().voice_note(0), 60);
    CHECK_EQ(poly().voice_note(1), 63);
    CHECK_EQ(poly().voice_note(2), 67);
    CHECK_EQ(poly().voice_note(3), 70);
    CHECK_EQ(poly().voice_note(4), -1);
    CHECK_EQ(poly().voice_note(5), -1);

    // All four on the same frame, so all four have started by the end of the block.
    CHECK_EQ(poly().sounding_voices(), 4);

    // One trig as far as the screen is concerned: app/engine.cpp bumps `triggered` once per
    // step, because eight LEDs flashing four times would be a worse display, not a better one.
    CHECK_EQ(int(uint8_t(triggers(kPoly) - before)), 1);
}

TEST(engine_a_chord_type_of_none_is_one_note) {
    Engine &engine = clean();
    (void)engine;
    put_trig(kPoly, 0);
    CHECK_EQ(int(live_step(kPoly, 0).chord), 0);     // what zeroed memory means
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(poly().busy_voices(), 1);
    CHECK_EQ(poly().voice_note(0), 60);
    CHECK_EQ(poly().voice_note(1), -1);

    // Six voices is the cap, and MIN11 is the one chord that fills them.
    set_chord(kPoly, 0, ChordType::min11);
    FakeHost::the().deliver_tick(1536, 0);
    pump();
    CHECK_EQ(poly().busy_voices(), 6);
    CHECK_EQ(poly().voice_note(5), 60 + 17);
}

TEST(engine_a_chords_notes_are_folded_into_the_scale) {
    Engine &engine = clean();
    (void)engine;
    post(Cmd::set_root, 0, 0, 0);                        // C
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::minor));
    put_trig(kPoly, 0);
    set_chord(kPoly, 0, ChordType::major);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();

    // A MAJOR stack on the root of C minor comes out as the key's own tonic triad, which is
    // core/chord.hpp's "folding gives the diatonic behaviour anyway". 64 is not in C minor, and
    // 63 and 65 are both a semitone away, so the tie goes to the lower note (core/scale.hpp).
    CHECK_EQ(poly().busy_voices(), 3);
    CHECK_EQ(poly().voice_note(0), 60);
    CHECK_EQ(poly().voice_note(1), 63);
    CHECK_EQ(poly().voice_note(2), 67);
}

TEST(engine_a_melodic_tracks_note_is_folded_into_the_key) {
    // FIXED. `fire` used to send the note to the machine unfolded while `chord_expand` folded
    // the step's note before stacking on it -- even for a chord of `none`, which is a single
    // note -- so identical step data sounded different depending on which machine a track
    // hosted. docs/SPEC.md's "melodic tracks fold notes into the scale" meant both; what was
    // missing was any notion of which machines are melodic, and folding every track would
    // retune the drums. Machine switching supplied the notion and `machine_folds_note` is it.
    //
    // Both VAs now fold, and the drums still do not -- which is the other half of the claim and
    // is below.
    Engine &engine = clean();
    post(Cmd::set_root, 0, 0, 0);
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::minor));
    const uint8_t both[2] = {kMono, kPoly};
    for (uint8_t track : both) {
        put_trig(track, 0);
        post(Cmd::step_field_delta, track, 0, uint8_t(StepField::note), 4);
    }
    pump();
    CHECK_EQ(int(live_step(kMono, 0).note), 64);
    CHECK_EQ(int(live_step(kPoly, 0).note), 64);
    CHECK_EQ(int(live_step(kMono, 0).chord), 0);
    CHECK_EQ(int(live_step(kPoly, 0).chord), 0);
    CHECK(!scale_contains(64, engine.pattern().root, int(engine.pattern().scale)));

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(poly().voice_note(0), 63);                  // folded into the key...
    CHECK_NEAR(mono().target_pitch(), 63.0f, 1e-4);      // ...and so is the MONO track now

    // Put the transport back and cut the tails, so the next test measures its own sound and not
    // this one's: `clean()` resets the pattern and the kit, not a running clock or a ringing
    // reverb (tests/apprig.hpp says why `settle` exists).
    FakeHost::the().set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump(2);
    CHECK(settle());
}

// And the other half: a machine that reads a note as a tuning or a slice is left alone. Folding
// a kick's note would RETUNE it -- dsp/kick.cpp tunes the body from `note` -- and nobody writing
// a drum part is thinking in degrees.
TEST(engine_a_drum_tracks_note_is_not_folded) {
    Engine &engine = clean();
    post(Cmd::set_root, 0, 0, 0);
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::minor));

    // Track 1 is the KICK of the default kit, and track 6 the BASS on VA MONO. The same step
    // data on both, on a note the scale does not contain.
    put_trig(0, 0);
    put_trig(kMono, 0);
    post(Cmd::step_field_delta, 0, 0, uint8_t(StepField::note), 4);
    post(Cmd::step_field_delta, kMono, 0, uint8_t(StepField::note), 4);
    pump();
    CHECK_EQ(int(live_step(0, 0).note), 64);
    CHECK(!scale_contains(64, engine.pattern().root, int(engine.pattern().scale)));

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();

    // The bass folded to 63 and the kick did not move. The kick cannot be asked what note it is
    // playing, so this reads the predicate that decides -- the thing that would have to change
    // for a kick to start being retuned by the key.
    CHECK_NEAR(mono().target_pitch(), 63.0f, 1e-4);
    CHECK_EQ(int(engine.sent_note(0)), 64);
    CHECK_EQ(int(engine.sent_note(kMono)), 63);
    CHECK(machine_folds_note(engine.machine_id(kMono), 1));
    CHECK(!machine_folds_note(engine.machine_id(0), 1));

    FakeHost::the().set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump(2);
    CHECK(settle());
}

// The samplers, as decided on 2026-11-22: GRAIN folds like the VAs; SAMPLE folds only while it is
// unsliced (SLC = 1), where the note transposes the whole window; a SLICED sample never folds,
// because there the note picks the slice and a fold would play a different one.
TEST(engine_grain_and_an_unsliced_sample_fold_and_a_sliced_sample_does_not) {
    constexpr uint8_t kTrack = 7;
    constexpr uint8_t kSlc = 3;                  // SRC slot 3 on SAMPLE (dsp/sample.cpp)
    struct Case {
        MachineId machine;
        uint8_t slc;                             // the SLC byte: 0 is one slice, 127 sixteen
        int want;                                // the note the machine is handed
    };
    const Case cases[] = {
        {MachineId::grain, 64, 63},              // GRAIN folds whatever SLC's slot holds
        {MachineId::sample, 0, 63},              // unsliced: an instrument, folded
        {MachineId::sample, 127, 64},            // sliced: note 64 is slice 4, left alone
    };
    for (const Case &c : cases) {
        Engine &engine = clean();
        post(Cmd::set_root, 0, 0, 0);
        post(Cmd::set_scale, 0, 0, uint8_t(Scale::minor));
        post(Cmd::set_machine, kTrack, 0, uint8_t(c.machine));
        post(Cmd::set_param, kTrack, 0, kSlc, c.slc);
        put_trig(kTrack, 0);
        post(Cmd::step_field_delta, kTrack, 0, uint8_t(StepField::note), 4);
        pump();
        CHECK_EQ(int(engine.machine_id(kTrack)), int(c.machine));
        CHECK_EQ(int(live_step(kTrack, 0).note), 64);
        CHECK(!scale_contains(64, engine.pattern().root, int(engine.pattern().scale)));

        FakeHost::the().set_running(true);
        FakeHost::the().deliver_tick(0, 0);
        pump();
        CHECK_EQ(int(engine.sent_note(kTrack)), c.want);

        FakeHost::the().set_running(false);
        post(Cmd::stop, 0, 0, 1);
        pump(2);
        CHECK(settle());
    }

    // And the predicate itself, at the edges the engine cannot easily reach.
    CHECK(machine_folds_note(uint8_t(MachineId::va_mono), 16));
    CHECK(machine_folds_note(uint8_t(MachineId::va_poly), 16));
    CHECK(machine_folds_note(uint8_t(MachineId::grain), 16));
    CHECK(machine_folds_note(uint8_t(MachineId::sample), 1));
    CHECK(!machine_folds_note(uint8_t(MachineId::sample), 2));
    CHECK(!machine_folds_note(uint8_t(MachineId::kick), 1));
    CHECK(!machine_folds_note(uint8_t(MachineId::snare), 1));
    CHECK(!machine_folds_note(uint8_t(MachineId::hat), 1));
    CHECK(!machine_folds_note(uint8_t(MachineId::perc), 1));
    CHECK(!machine_folds_note(uint8_t(MachineId::count), 1));
}

TEST(engine_a_chord_byte_on_a_track_with_no_poly_machine_is_ignored) {
    Engine &engine = clean();
    (void)engine;
    put_trig(kMono, 0);
    set_chord(kMono, 0, ChordType::min7);
    CHECK(chord_type(live_step(kMono, 0).chord) == ChordType::min7);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    // `polys_` records which track can take a chord, because this build has no RTTI to ask a
    // Machine* with. Every other machine plays the step's note and ignores the byte; expanding
    // onto a monophonic voice would leave the chord's TOP note sounding and nothing else.
    CHECK_NEAR(mono().target_pitch(), 60.0f, 1e-4);
}

// --- resolving a tick ------------------------------------------------------------------------

TEST(engine_a_tick_starts_its_note_on_the_frame_the_clock_gave) {
    // The tick carries the frame it falls on and the note has to start on that exact sample.
    // Measured from real silence, which is what `clean()` is for: the assertion is that nothing
    // at all comes out before the frame, and that the later in the block a tick falls the less
    // of the note is in it.
    const uint32_t frames[4] = {0u, 16u, 32u, 48u};
    float loudest[4] = {};

    for (int i = 0; i < 4; i++) {
        clean();
        post(Cmd::set_param, kPoly, 0, kDec, 100);
        put_trig(kPoly, 0);
        CHECK_EQ(pump(), 0.0f);

        FakeHost::the().set_running(true);
        FakeHost::the().deliver_tick(0, frames[i]);
        loudest[i] = pump();

        const float *block = last_block();
        const int onset = int(2u * frames[i]);
        CHECK_EQ(peak_of(block, onset), 0.0f);
        CHECK(peak_of(block + onset, kStereo - onset) > 0.0f);
    }

    // "The later in the block a tick falls, the less of the note is in it" was asserted here as
    // a strict ordering and it is NOT TRUE. Measured: 0.113, 0.184, 0.180, 0.127 for frames
    // 0/16/32/48 -- the loudest is frame 16. A VA voice at note 60 has a period of about 366
    // frames, so a 64-frame block holds a fraction of one cycle and the PEAK inside it depends
    // on where in that cycle the samples land, not only on how many there are.
    //
    // The same phenomenon defeated two onset counters in tests/test_app.cpp before it was
    // understood, which is why it is spelled out rather than worked around. What is true, and is
    // asserted per iteration above, is the property this test is actually for: nothing before
    // the tick's frame and something from it. All four blocks hold some of the note, and that is
    // all the loudness figures can honestly say.
    for (int i = 0; i < 4; i++) CHECK(loudest[i] > 0.0f);
}

TEST(engine_swing_moves_the_odd_steps_and_leaves_the_even_ones) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    put_trig(kPoly, 1);
    put_trig(kPoly, 2);

    // Straight first, so what follows is about swing and not about the grid.
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMin));
    pump();
    CHECK_EQ(int(engine.pattern().swing), kSwingMin);
    CHECK_EQ(fired_on(kPoly, 24u), 1);
    CHECK_EQ(playhead(kPoly), 1);

    // 75% pushes an odd step half a step late: (75 - 50) * 24 / 50 = 12 ticks.
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMax));
    pump();
    CHECK_EQ(swing_offset(1, kSwingMax, ticks_per_step(Speed::x1)), 12);
    CHECK_EQ(fired_on(kPoly, 1536u + 24u), 0);
    CHECK_EQ(fired_on(kPoly, 1536u + 36u), 1);
    CHECK_EQ(playhead(kPoly), 1);
    CHECK_EQ(fired_on(kPoly, 1536u + 48u), 1);     // and the even ones do not move
    CHECK_EQ(playhead(kPoly), 2);

    // Out of range in either direction clamps rather than wrapping.
    post(Cmd::set_pattern_swing, 0, 0, 200);
    pump();
    CHECK_EQ(int(engine.pattern().swing), kSwingMax);
    post(Cmd::set_pattern_swing, 0, 0, 3);
    pump();
    CHECK_EQ(int(engine.pattern().swing), kSwingMin);
}

TEST(engine_a_tracks_own_swing_overrides_the_patterns_and_pat_follows_it_again) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 1);
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMin));
    post(Cmd::set_track_swing, kPoly, 0, uint8_t(kSwingMax));
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].swing), kSwingMax);
    // The pattern is straight and the track swings: step 1 lands 12 ticks late.
    CHECK_EQ(fired_on(kPoly, 24u), 0);
    CHECK_EQ(fired_on(kPoly, 36u), 1);

    // A delta walks down from 75; past 50 it reaches PAT and the track follows the pattern.
    post(Cmd::set_track_swing, kPoly, 0, 0, uint8_t(int8_t(-30)), kDeltaFlag);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].swing), int(kSwingInherit));
    CHECK_EQ(fired_on(kPoly, 1536u + 24u), 1);
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMax));
    pump();
    CHECK_EQ(fired_on(kPoly, 2u * 1536u + 24u), 0);
    CHECK_EQ(fired_on(kPoly, 2u * 1536u + 36u), 1);

    // An absolute value out of range clamps; 0 is PAT, not 50.
    post(Cmd::set_track_swing, kPoly, 0, 200);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].swing), kSwingMax);
    post(Cmd::set_track_swing, kPoly, 0, 3);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].swing), kSwingMin);
    post(Cmd::set_track_swing, kPoly, 0, uint8_t(kSwingInherit));
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].swing), int(kSwingInherit));
    // Other tracks were never touched.
    for (int t = 0; t < kTracks; t++) {
        if (t != kPoly) CHECK_EQ(int(engine.pattern().tracks[t].swing), int(kSwingInherit));
    }
}

TEST(engine_micro_timing_moves_a_step_off_the_grid_and_adds_to_swing) {
    Engine &engine = clean();
    (void)engine;
    FakeHost::the().set_running(true);
    put_trig(kPoly, 1);
    post(Cmd::step_field_delta, kPoly, 1, uint8_t(StepField::micro), uint8_t(int8_t(-11)));
    pump();
    CHECK_EQ(int(live_step(kPoly, 1).micro), -kMicroRange);
    CHECK_EQ(fired_on(kPoly, 24u), 0);             // not on its nominal tick any more
    CHECK_EQ(fired_on(kPoly, 13u), 1);             // 24 - 11

    // Swing and micro-timing add: 24 + 12 - 11 = 25.
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMax));
    pump();
    CHECK_EQ(fired_on(kPoly, 1536u + 13u), 0);
    CHECK_EQ(fired_on(kPoly, 1536u + 25u), 1);

    // And +11 the other way, which is a step firing EARLIER than its own tick -- the reason
    // core/timing maps backwards from the tick rather than forwards from the step.
    post(Cmd::set_pattern_swing, 0, 0, uint8_t(kSwingMin));
    post(Cmd::step_field_delta, kPoly, 1, uint8_t(StepField::micro), 22);
    pump();
    CHECK_EQ(int(live_step(kPoly, 1).micro), kMicroRange);
    CHECK_EQ(fired_on(kPoly, 2u * 1536u + 35u), 1);
}

TEST(engine_a_tracks_own_speed_sets_its_step_grid) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 1);
    CHECK_EQ(fired_on(kPoly, 12u), 0);             // 1x: a step is 24 ticks
    CHECK_EQ(fired_on(kPoly, 24u), 1);

    post(Cmd::set_track_speed, kPoly, 0, uint8_t(Speed::x2));
    pump();
    CHECK_EQ(fired_on(kPoly, 24u), 0);             // 2x: step 1 is at 12, step 2 at 24
    CHECK_EQ(fired_on(kPoly, 12u), 1);

    post(Cmd::set_track_speed, kPoly, 0, uint8_t(Speed::x1_2));
    pump();
    CHECK_EQ(fired_on(kPoly, 24u), 0);             // 1/2x: step 1 is at 48
    CHECK_EQ(fired_on(kPoly, 48u), 1);

    // One track's speed is its own: track 6 is still at 1x and still fires step 1 at 24.
    put_trig(kMono, 1);
    CHECK_EQ(fired_on(kMono, 24u), 1);

    // A speed this build does not have is refused, leaving the track as it was.
    post(Cmd::set_track_speed, kPoly, 0, uint8_t(Speed::count));
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].speed), int(Speed::x1_2));
}

TEST(engine_a_shorter_track_wraps_on_its_own_length) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    post(Cmd::set_track_length, kPoly, 0, 3);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].length), 3);

    CHECK_EQ(fired_on(kPoly, 0u), 1);
    CHECK_EQ(playhead(kPoly), 0);
    CHECK_EQ(fired_on(kPoly, 24u), 0);             // step 1 of three, empty
    CHECK_EQ(playhead(kPoly), 1);
    CHECK_EQ(fired_on(kPoly, 48u), 0);
    CHECK_EQ(playhead(kPoly), 2);

    // Three steps is 72 ticks, so step 0 comes round there -- and NOT at 96, which is where a
    // scheduler still configured for 64 steps would put step 4. `reconfigure()` is what makes
    // the difference, and forgetting it is the bug its own comment in app/engine.cpp describes.
    CHECK_EQ(fired_on(kPoly, 72u), 1);
    CHECK_EQ(playhead(kPoly), 0);
    CHECK_EQ(fired_on(kPoly, 96u), 0);
    CHECK_EQ(playhead(kPoly), 1);

    // A length of 0 is zeroed memory, and one step is the only reading of it that can play.
    post(Cmd::set_track_length, kPoly, 0, 0);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].length), 1);
    CHECK_EQ(fired_on(kPoly, 24u), 1);
    CHECK_EQ(fired_on(kPoly, 48u), 1);

    // Past the end it clamps rather than wrapping round to a tiny track.
    post(Cmd::set_track_length, kPoly, 0, 200);
    pump();
    CHECK_EQ(int(engine.pattern().tracks[kPoly].length), kStepsPerTrack);
}

TEST(engine_a_ratchet_gives_every_hit_its_own_event_and_its_own_voice) {
    Engine &engine = clean();
    put_trig(kPoly, 0);
    CHECK_EQ(int(ratchet_count(live_step(kPoly, 0).ratchet)), 1);
    FakeHost::the().set_running(true);
    CHECK_EQ(fired_on(kPoly, 0u), 1);
    CHECK_EQ(poly().busy_voices(), 1);

    clean();
    put_trig(kPoly, 0);
    post(Cmd::step_field_delta, kPoly, 0, uint8_t(StepField::ratchet_count), 3);
    pump();
    CHECK_EQ(int(ratchet_count(live_step(kPoly, 0).ratchet)), 4);

    FakeHost::the().set_running(true);

    // ONE hit in the tick's own block, not four. The hits sit `step_samples / hits` apart --
    // 1,385 frames at 130 BPM, twenty-one blocks -- and `app/engine.cpp` holds an event whose
    // frame is past the block and counts it down until it lands.
    //
    // This test asserted four voices busy in the first block, which was the behaviour BEFORE
    // that deferral existed: all four hits were handed to the machine at once, and because a
    // machine stores one pending trig per voice, a ratchet needed as many voices as it had hits.
    // VA POLY has six and survived four; KICK has two and sounded two of four; VA MONO has one
    // and kept only the last. The deferral is what fixed that, so one hit per block is the
    // contract now and four would mean it had been removed.
    CHECK_EQ(fired_on(kPoly, 0u), 1);
    CHECK_EQ(poly().busy_voices(), 1);
    CHECK_EQ(poly().voice_note(0), 60);
    CHECK_EQ(poly().voice_note(1), -1);

    // And all four DO arrive, each in the block its frame falls in. Counted as TRIGGERS, not as
    // voices: every hit is note 60, and dsp/vapoly.hpp's first allocation rule gives a repeated
    // note the voice already sounding it -- so `busy_voices()` stays at 1 all the way through a
    // ratchet of one note and cannot see this at all. `triggers()` is `Status::bump_triggered`,
    // which `fire` bumps once per hit that reaches a machine.
    const uint8_t before = triggers(kPoly);
    for (int b = 0; b < 110; b++) pump();          // 7,040 frames: the whole step and a tail
    CHECK_EQ(int(uint8_t(triggers(kPoly) - before)), 3);   // the other three hits
    CHECK_EQ(poly().voice_note(0), 60);                    // all on the one voice, by design
    (void)engine;
}

TEST(engine_probability_decides_whether_a_step_fires_at_all) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    CHECK_EQ(int(live_step(kPoly, 0).probability), 100);
    CHECK_EQ(fired_on(kPoly, 0u), 1);              // 100%: decided without a draw

    set_probability(kPoly, 0, 0);
    CHECK_EQ(int(live_step(kPoly, 0).probability), 0);
    CHECK_EQ(fired_on(kPoly, 1536u), 0);           // 0%: decided without a draw either

    // In between, the draw comes from the pattern's seed mixed with the track, step and loop and
    // never from a running RNG (core/scheduler.hpp), so the boundary can be NAMED rather than
    // sampled -- and the same tick delivered twice must decide the same way both times.
    const uint32_t loop = 2u;
    const uint32_t roll = probability_roll(engine.pattern().seed, kPoly, 0, loop);
    CHECK(roll < 100u);

    set_probability(kPoly, 0, int(roll));
    CHECK_EQ(fired_on(kPoly, loop * 1536u), 0);    // the draw is not BELOW the probability
    set_probability(kPoly, 0, int(roll) + 1);
    CHECK_EQ(fired_on(kPoly, loop * 1536u), 1);    // one percent more and the same draw passes
}

TEST(engine_the_playhead_follows_time_and_is_published_for_every_track) {
    Engine &engine = clean();
    (void)engine;
    FakeHost::the().set_running(true);
    // No trigs anywhere: the playhead follows TIME, not trigs (core/scheduler.cpp), and the
    // engine publishes all eight of them on every tick it resolves.
    tick_at(3u * 24u);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(playhead(t), 3);
    tick_at(7u * 24u);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(playhead(t), 7);

    // FIXED: a STOP resets the scheduler, so its playheads go back to step 1, and `Status` now
    // follows every block rather than only on a tick -- so the grid no longer keeps the step the
    // transport stopped on lit until it runs again. Pinned as a bug here; now pinned as the fix.
    post(Cmd::stop, 0, 0, 0);
    pump(2);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(playhead(t), 0);
    tick_at(0);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(playhead(t), 0);
}

TEST(engine_a_stop_re_arms_the_first_pass_conditions) {
    // What a stop really does to the scheduler, observed through the one thing that can see it:
    // `1ST` fires on the first loop pass after a reset and nothing else does.
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    set_condition_position(kPoly, 0, 7);           // the walk's position 7 is 1ST
    CHECK_EQ(int(condition_kind(live_step(kPoly, 0).condition)), int(Condition::FIRST));
    (void)engine;

    CHECK_EQ(fired_on(kPoly, 0u), 1);              // the first pass
    CHECK_EQ(fired_on(kPoly, 1536u), 0);           // the second
    post(Cmd::stop, 0, 0, 0);
    pump();
    CHECK_EQ(fired_on(kPoly, 2u * 1536u), 1);      // the first pass after the stop
}

TEST(engine_a_trigless_step_applies_its_locks_without_starting_a_note) {
    // The whole point of a trigless step, and the reason `fire` rebuilds `effective_` BEFORE it
    // looks at the trig bit. Audible as a note that is already ringing changing its sound.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kDec, 127);     // a note that rings over several steps
    put_trig(kPoly, 0);
    post(Cmd::toggle_trig, kPoly, 1, 1);           // a = 1: the trigless bit
    pump();
    CHECK(!trig_at(kPoly, 1));
    CHECK((live_step(kPoly, 1).flags & kStepTrigless) != 0);

    // A lock on step 1 taking the level to nothing.
    post(Cmd::lock_delta, kPoly, 1, kLvl, uint8_t(int8_t(-120)));
    post(Cmd::lock_delta, kPoly, 1, kLvl, uint8_t(int8_t(-120)));
    pump();
    uint8_t locked = 1;
    CHECK(pattern_get_lock(engine.pattern(), kPoly, 1, kLvl, locked));
    CHECK_EQ(int(locked), 0);
    CHECK_EQ(pump(), 0.0f);

    FakeHost::the().set_running(true);
    const uint8_t before = triggers(kPoly);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(4) > 0.01f);                        // step 0 sounded
    CHECK_EQ(int(uint8_t(triggers(kPoly) - before)), 1);

    const uint8_t after_note = triggers(kPoly);
    FakeHost::the().deliver_tick(24, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(kPoly) - after_note)), 0);   // no second note
    CHECK_EQ(poly().busy_voices(), 1);                         // and no second voice
    CHECK_EQ(pump(4), 0.0f);                                   // but the lock took it silent
    CHECK_EQ(playhead(kPoly), 1);
}

// --- bars, and what waits for one ------------------------------------------------------------

TEST(engine_the_bar_counter_counts_and_resyncs_forwards) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    CHECK_EQ(int(sync_bars()), 2);
    CHECK_EQ(int(engine.bar()), 2);
    CHECK_EQ(int(kTicksPerBar), 384);              // sixteen 1x steps, whatever the pattern is

    tick_at(3u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 3);
    tick_at(3u * kTicksPerBar + 1u);
    CHECK_EQ(int(engine.bar()), 3);
    tick_at(4u * kTicksPerBar - 1u);
    CHECK_EQ(int(engine.bar()), 3);
    tick_at(4u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 4);

    // A jump of ten bars resyncs by division instead of counting up one bar per tick.
    tick_at(14u * kTicksPerBar + 7u);
    CHECK_EQ(int(engine.bar()), 14);
}

TEST(engine_the_bar_counter_resyncs_on_a_backwards_jump) {
    // This was a real bug (docs/HANDOFF.md): the counter resynced on a FORWARD jump only, and a
    // stop and start relocates the clock -- so auto-fill fired in the wrong place for as many
    // bars as the jump was long.
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    sync_bars();
    tick_at(9u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 9);

    tick_at(0);
    CHECK_EQ(int(engine.bar()), 0);
    tick_at(kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 1);

    // A jump backwards of LESS than a bar is not a relocation: it is swing or micro-timing
    // pulling a step in front of a tick already seen, and the count must not move for it.
    tick_at(kTicksPerBar + 100u);
    CHECK_EQ(int(engine.bar()), 1);
    tick_at(kTicksPerBar + 10u);
    CHECK_EQ(int(engine.bar()), 1);
}

TEST(engine_auto_fill_fires_on_the_last_bar_of_its_period) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::every_4));
    pump();
    CHECK(engine.auto_fill() == AutoFill::every_4);

    sync_bars();
    CHECK(!engine.fill());
    tick_at(3u * kTicksPerBar);                    // the last bar of the first period
    CHECK_EQ(int(engine.bar()), 3);
    CHECK(engine.fill());
    tick_at(4u * kTicksPerBar);
    CHECK(!engine.fill());
    tick_at(7u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 7);
    CHECK(engine.fill());

    // Every 8 puts the fill on bar 7 of each eight, so bar 11 stops filling and bar 15 starts.
    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::every_8));
    pump();
    tick_at(11u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 11);
    CHECK(!engine.fill());
    tick_at(15u * kTicksPerBar);
    CHECK_EQ(int(engine.bar()), 15);
    CHECK(engine.fill());

    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::off));
    pump();
    tick_at(19u * kTicksPerBar);
    CHECK(!engine.fill());

    // A period this build does not have is off, not a period of its own.
    post(Cmd::set_auto_fill, 0, 0, 99);
    pump();
    CHECK(engine.auto_fill() == AutoFill::off);
}

TEST(engine_fill_works_while_the_transport_is_stopped) {
    // Also a real bug (docs/HANDOFF.md): `resolve_tick` was the only thing that resolved the
    // fill state, and no tick arrives with the clock still -- so holding FILL stopped did
    // nothing and the pad looked broken.
    Engine &engine = clean();
    CHECK(!FakeHost::the().running());
    post(Cmd::set_fill, 0, 0, 1);
    pump();
    CHECK(engine.fill());
    post(Cmd::set_fill, 0, 0, 0);
    pump();
    CHECK(!engine.fill());

    // Held through an auto-fill bar the two OR rather than fight: the bar must not cancel the
    // pad, and releasing the pad must not cancel the bar.
    FakeHost::the().set_running(true);
    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::every_4));
    post(Cmd::set_fill, 0, 0, 1);
    pump();
    sync_bars();
    CHECK(engine.fill());                          // bar 2 is no auto-fill bar, but the pad is held
    tick_at(3u * kTicksPerBar);
    CHECK(engine.fill());
    post(Cmd::set_fill, 0, 0, 0);
    pump();
    tick_at(3u * kTicksPerBar + 1u);
    CHECK(engine.fill());                          // the bar's own fill survives the release
}

TEST(engine_a_fill_condition_follows_the_fill_state) {
    Engine &engine = clean();
    (void)engine;
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    set_condition_position(kPoly, 0, 1);           // the walk's position 1 is FILL
    CHECK_EQ(int(condition_kind(live_step(kPoly, 0).condition)), int(Condition::FILL));

    CHECK_EQ(fired_on(kPoly, 0u), 0);              // not filling
    post(Cmd::set_fill, 0, 0, 1);
    pump();
    CHECK_EQ(fired_on(kPoly, 1536u), 1);
    post(Cmd::set_fill, 0, 0, 0);
    pump();
    CHECK_EQ(fired_on(kPoly, 2u * 1536u), 0);
}

// --- undo, through the ring ------------------------------------------------------------------

TEST(engine_sixteen_levels_of_undo_and_no_more) {
    Engine &engine = clean();
    (void)engine;
    // Twenty separate gestures: the UI's counter is what ends one (core/undo.hpp), so a
    // different `seq` is a different edit.
    for (int i = 0; i < 20; i++) {
        post(Cmd::undo_mark, 0, uint8_t(i), uint8_t(GestureKind::trig), uint8_t(i));
        post(Cmd::toggle_trig, 0, uint8_t(i));
        pump();
    }
    CHECK_EQ(undo_depth(), kUndoLevels);
    for (int i = 0; i < 20; i++) CHECK(trig_at(0, i));

    // Sixteen undos take back sixteen edits and not one more, so the four oldest survive.
    for (int i = 0; i < kUndoLevels; i++) {
        post(Cmd::undo);
        pump();
    }
    CHECK_EQ(undo_depth(), 0);
    for (int i = 0; i < 4; i++) CHECK(trig_at(0, i));
    for (int i = 4; i < 20; i++) CHECK(!trig_at(0, i));

    // A seventeenth is refused, and a refusal changes nothing.
    post(Cmd::undo);
    pump();
    CHECK_EQ(undo_depth(), 0);
    for (int i = 0; i < 4; i++) CHECK(trig_at(0, i));
    for (int i = 4; i < 20; i++) CHECK(!trig_at(0, i));
}

TEST(engine_one_gesture_is_one_undo_level) {
    Engine &engine = clean();
    post(Cmd::set_param, 0, 0, kLvl, 100);
    pump();

    // A sweep: the same gesture, mark and all, forty times over. Without coalescing one encoder
    // would fill all sixteen levels and throw away everything that came before it.
    for (int i = 0; i < 40; i++) {
        post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::lock), 3);
        post(Cmd::lock_delta, 0, 0, kLvl, uint8_t(int8_t(-1)));
        pump();
    }
    CHECK_EQ(undo_depth(), 1);
    uint8_t value = 0;
    CHECK(pattern_get_lock(engine.pattern(), 0, 0, kLvl, value));
    CHECK_EQ(int(value), 60);                      // 100, forty clicks down

    // One undo takes the whole sweep back, lock and all.
    post(Cmd::undo);
    pump();
    CHECK_EQ(undo_depth(), 0);
    CHECK(!pattern_get_lock(engine.pattern(), 0, 0, kLvl, value));

    // Ending the gesture -- on the panel, the step key coming up -- starts a new level, so two
    // sweeps are two undos.
    post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::lock), 4);
    post(Cmd::lock_delta, 0, 0, kLvl, uint8_t(int8_t(-10)));
    post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::lock), 5);
    post(Cmd::lock_delta, 0, 0, kLvl, uint8_t(int8_t(-10)));
    pump();
    CHECK_EQ(undo_depth(), 2);

    // A `none` gesture never coalesces, not even with another `none`: it means "no gesture", so
    // two of them are two edits (core/undo.cpp).
    post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::none), 6);
    post(Cmd::toggle_trig, 0, 7);
    post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::none), 6);
    post(Cmd::toggle_trig, 0, 8);
    pump();
    CHECK_EQ(undo_depth(), 4);
}

TEST(engine_redo_comes_back_and_a_new_edit_drops_the_branch) {
    Engine &engine = clean();
    (void)engine;
    post(Cmd::undo_mark, 0, 1, uint8_t(GestureKind::trig), 1);
    post(Cmd::toggle_trig, 0, 1);
    post(Cmd::undo_mark, 0, 2, uint8_t(GestureKind::trig), 2);
    post(Cmd::toggle_trig, 0, 2);
    pump();
    CHECK_EQ(undo_depth(), 2);
    CHECK_EQ(redo_depth(), 0);

    post(Cmd::undo);
    pump();
    CHECK(trig_at(0, 1));
    CHECK(!trig_at(0, 2));
    CHECK_EQ(redo_depth(), 1);

    post(Cmd::redo);
    pump();
    CHECK(trig_at(0, 2));
    CHECK_EQ(redo_depth(), 0);

    // Undo, then a NEW edit: the redo branch is dropped, which is what every editor does.
    post(Cmd::undo);
    pump();
    CHECK_EQ(redo_depth(), 1);
    post(Cmd::undo_mark, 0, 3, uint8_t(GestureKind::trig), 3);
    post(Cmd::toggle_trig, 0, 3);
    pump();
    CHECK_EQ(redo_depth(), 0);
    post(Cmd::redo);
    pump();
    CHECK(!trig_at(0, 2));                         // refused: nothing to come back to
    CHECK(trig_at(0, 3));
}

TEST(engine_an_undo_after_a_project_load_is_refused) {
    Engine &engine = clean();
    post(Cmd::undo_mark, 0, 5, uint8_t(GestureKind::trig), 1);
    post(Cmd::toggle_trig, 0, 5);
    pump();
    CHECK_EQ(undo_depth(), 1);
    CHECK(trig_at(0, 5));

    // A load replaces all 128 patterns through `project_for_load`, so the history belongs to a
    // pattern that no longer exists: restoring it would write one project's steps over another's.
    Project &incoming = engine.project_for_load();
    project_init(incoming);
    incoming.patterns[0][0].tracks[1].steps[40].flags = kStepTrig;
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(undo_depth(), 0);
    CHECK_EQ(redo_depth(), 0);
    CHECK(!trig_at(0, 5));
    CHECK(trig_at(1, 40));

    post(Cmd::undo);
    pump();
    CHECK(!trig_at(0, 5));                         // the edit did not come back ...
    CHECK(trig_at(1, 40));                         // ... and the loaded pattern was not overwritten
    CHECK_EQ(undo_depth(), 0);
}

TEST(engine_a_pattern_change_clears_the_undo_history) {
    Engine &engine = clean();
    (void)engine;
    post(Cmd::undo_mark, 0, 5, uint8_t(GestureKind::trig), 1);
    post(Cmd::toggle_trig, 0, 5);
    pump();
    CHECK_EQ(undo_depth(), 1);

    // Stopped, so the change lands at once whatever the mode says.
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK_EQ(live_slot(), 1);
    CHECK_EQ(undo_depth(), 0);

    post(Cmd::undo);
    pump();
    CHECK(!trig_at(0, 5));                         // A02 keeps its own history, which is empty
}

// --- the queue and chains --------------------------------------------------------------------

TEST(engine_a_queued_change_lands_at_once_when_the_transport_jumps_back) {
    // STOP and PLAY (or F1 + F5, or an outside clock relocating) put the clock back to 0. The
    // change was armed for tick 1536 of the OLD position, and used to wait there -- a change
    // queued deep into a set and then STOP, PLAY landed that many bars late.
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    tick_at(3000);
    post(Cmd::queue_pattern, 0, 0, 1, 3, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(engine.queue().pending);
    CHECK_EQ(int(engine.queue().switch_tick), 3072);

    tick_at(0);                                    // the transport starts again from 0
    CHECK(!engine.queue().pending);
    CHECK_EQ(live_bank(), 1);
    CHECK_EQ(live_slot(), 3);

    // And a forward tick does not: the next change still waits for its boundary.
    tick_at(100);
    post(Cmd::queue_pattern, 0, 0, 0, 0, uint8_t(QueueMode::end_of_pattern));
    pump();
    tick_at(101);
    CHECK(engine.queue().pending);
    CHECK_EQ(live_slot(), 3);
}

TEST(engine_a_queued_change_waits_for_the_end_of_the_pass) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    // A blank project's master length is 64 steps, so a pass is 64 x 24 = 1536 ticks.
    CHECK_EQ(master_loop_ticks(engine.pattern()), 1536);

    tick_at(100);                                  // the sequencer is on tick 100 ...
    post(Cmd::queue_pattern, 0, 0, 2, 5, uint8_t(QueueMode::end_of_pattern));
    pump();                                        // ... which is what the change is armed from
    CHECK(engine.queue().pending);
    CHECK_EQ(int(engine.queue().switch_tick), 1536);
    CHECK_EQ(live_bank(), 0);
    CHECK_EQ(live_slot(), 0);

    tick_at(1535);
    CHECK_EQ(live_slot(), 0);
    CHECK(engine.queue().pending);
    tick_at(1536);
    CHECK_EQ(live_bank(), 2);
    CHECK_EQ(live_slot(), 5);
    CHECK(!engine.queue().pending);

    // And cancelling is unconditional: the UI does it when the same pattern is pressed twice,
    // and cancelling a queue that is not armed is not an error.
    post(Cmd::queue_pattern, 0, 0, 2, 6, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(engine.queue().pending);
    post(Cmd::cancel_queue);
    post(Cmd::cancel_queue);
    pump();
    CHECK(!engine.queue().pending);
    CHECK_EQ(live_slot(), 5);
}

TEST(engine_a_queued_change_starts_the_new_pattern_at_step_1) {
    // docs/SPEC.md: the next pattern switches "at the end of the current one, or after 16 or 32
    // steps, or at once keeping the position". Only the last keeps the position. The new pattern
    // used to be resolved from the absolute tick, so a blank pattern's 64-step tracks came in on
    // step 16 at the first 16-step boundary.
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    tick_at(0);
    tick_at(100);
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::after_16));
    pump();
    CHECK_EQ(int(engine.queue().switch_tick), 384);

    tick_at(384);
    CHECK_EQ(live_slot(), 1);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(playhead(t), 0);
    tick_at(384 + 24);
    CHECK_EQ(playhead(0), 1);

    // And "the end of the current one" is now counted from where it started: one 64-step pass
    // after tick 384, not the next multiple of 1536 from tick 0.
    tick_at(400);
    post(Cmd::queue_pattern, 0, 0, 0, 2, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK_EQ(int(engine.queue().switch_tick), 384 + 1536);
    tick_at(384 + 1536);
    CHECK_EQ(live_slot(), 2);
    CHECK_EQ(playhead(0), 0);

    // NOW keeps the position, as the spec says.
    tick_at(384 + 1536 + 24 * 5);
    CHECK_EQ(playhead(0), 5);
    post(Cmd::queue_pattern, 0, 0, 0, 3, uint8_t(QueueMode::immediate));
    pump();
    tick_at(384 + 1536 + 24 * 6);
    CHECK_EQ(live_slot(), 3);
    CHECK_EQ(playhead(0), 6);

    // A START from tick 0 measures from 0 again.
    tick_at(0);
    CHECK_EQ(playhead(0), 0);
    tick_at(24 * 3);
    CHECK_EQ(playhead(0), 3);
}

TEST(engine_a_queued_change_lands_at_once_while_the_transport_is_stopped) {
    Engine &engine = clean();
    CHECK(!FakeHost::the().running());
    // With the clock still no tick will arrive to make a queued change due, so the only honest
    // reading of any mode is "now" -- anything else shows a pending switch for ever.
    post(Cmd::queue_pattern, 0, 0, 1, 2, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK_EQ(live_bank(), 1);
    CHECK_EQ(live_slot(), 2);
    CHECK(!engine.queue().pending);

    // Running, the same mode waits.
    FakeHost::the().set_running(true);
    tick_at(600);
    post(Cmd::queue_pattern, 0, 0, 3, 4, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(engine.queue().pending);
    CHECK_EQ(live_bank(), 1);
    CHECK_EQ(live_slot(), 2);
}

TEST(engine_an_immediate_change_does_not_wait_for_a_tick) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    tick_at(500);
    post(Cmd::queue_pattern, 0, 0, 0, 7, uint8_t(QueueMode::immediate));
    pump();
    // `immediate` is the same switch by request, so app/engine.cpp takes it in the command
    // rather than arming it: the position is kept and nothing is reset.
    CHECK_EQ(live_slot(), 7);
    CHECK(!engine.queue().pending);
}

TEST(engine_a_queued_change_to_a_pattern_that_does_not_exist_is_refused) {
    Engine &engine = clean();
    const uint32_t before = refused();
    post(Cmd::queue_pattern, 0, 0, uint8_t(kBanks), 0, uint8_t(QueueMode::immediate));
    post(Cmd::queue_pattern, 0, 0, 0, uint8_t(kPatternsPerBank), uint8_t(QueueMode::immediate));
    pump();
    CHECK_EQ(int(refused() - before), 2);
    CHECK_EQ(live_bank(), 0);
    CHECK_EQ(live_slot(), 0);
    CHECK(!engine.queue().pending);
}

TEST(engine_a_chain_advances_at_the_pass_boundary) {
    Engine &engine = clean();
    // Two entries of one pass each: A01, A02, round and round.
    Project &incoming = engine.project_for_load();
    project_init(incoming);
    incoming.settings.chain_length = 2;
    incoming.chain[0] = ChainEntry{0, 0, 1};
    incoming.chain[1] = ChainEntry{0, 1, 1};
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(chain_length(engine.project()), 2);
    CHECK(!engine.chain().active);                 // a load does not start one

    FakeHost::the().set_running(true);
    tick_at(0);                                    // the boundary is derived from a real tick
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    CHECK(engine.chain().active);
    CHECK_EQ(int(engine.chain().entry), 0);
    CHECK_EQ(live_slot(), 0);

    tick_at(1536);                                 // the end of the pass
    CHECK_EQ(int(engine.chain().entry), 1);
    CHECK_EQ(live_slot(), 1);

    // The switch cleared the boundary -- the new pattern's master length makes the old one
    // meaningless -- so it takes a tick to re-derive before the next pass can end.
    tick_at(1537);
    tick_at(2u * 1536u);
    CHECK_EQ(int(engine.chain().entry), 0);
    CHECK_EQ(live_slot(), 0);

    // Turning the chain off leaves the pattern it reached playing, rather than jumping back.
    post(Cmd::set_chain, 0, 0, 0);
    pump();
    CHECK(!engine.chain().active);
    tick_at(2u * 1536u + 1u);
    tick_at(3u * 1536u);
    CHECK_EQ(live_slot(), 0);
}

TEST(engine_a_chain_edited_while_it_plays_stays_on_the_entry_it_is_playing) {
    // The CHAIN page renumbers entries under a running player. Adding an entry above the playing
    // one used to move the chain onto the wrong entry, and removing the playing one skipped the
    // entry after it.
    Engine &engine = clean();
    Project &incoming = engine.project_for_load();
    project_init(incoming);
    incoming.settings.chain_length = 3;
    incoming.chain[0] = ChainEntry{0, 0, 1};
    incoming.chain[1] = ChainEntry{0, 1, 1};
    incoming.chain[2] = ChainEntry{0, 2, 1};
    post(Cmd::project_loaded);
    pump();
    FakeHost::the().set_running(true);
    tick_at(0);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    tick_at(1536);
    CHECK_EQ(live_slot(), 1);                      // entry 1, A02
    tick_at(1537);

    Command insert;                                // A10 at the top
    insert.cmd = Cmd::chain_edit;
    insert.step = 0;
    insert.a = uint8_t(ChainEdit::insert);
    insert.b = 0;
    insert.c = 9;
    insert.d = 1;
    CHECK(engine.commands().post(insert));
    pump();
    CHECK_EQ(int(engine.chain().entry), 2);        // still A02

    Command remove;                                // and A02 itself goes
    remove.cmd = Cmd::chain_edit;
    remove.step = 2;
    remove.a = uint8_t(ChainEdit::remove);
    CHECK(engine.commands().post(remove));
    pump();
    CHECK_EQ(live_slot(), 1);                      // it plays out its pass
    tick_at(2u * 1536u);
    CHECK_EQ(live_slot(), 2);                      // then A03, not A10
    CHECK_EQ(int(engine.chain().entry), 2);
}

// --- song mode -------------------------------------------------------------------------------

void chain_op(ChainEdit op, uint8_t at, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0, uint8_t e = 0) {
    Command command;
    command.cmd = Cmd::chain_edit;
    command.step = at;
    command.a = uint8_t(op);
    command.b = b;
    command.c = c;
    command.d = d;
    command.e = e;
    CHECK(strata::test::engine().commands().post(command));
    pump();
}

TEST(engine_a_chain_entry_recalls_its_mute_state_as_it_starts) {
    Engine &engine = clean();
    Project &incoming = engine.project_for_load();
    incoming.settings.chain_length = 2;
    incoming.chain[0] = ChainEntry{0, 0, 1, 0};   // leaves the mutes alone
    incoming.chain[1] = ChainEntry{0, 1, 1, 3};   // recalls state 3
    for (int i = 2; i < kChainEntries; i++) incoming.chain[i] = ChainEntry{};
    post(Cmd::project_loaded);
    pump();
    // Mute state 3 is track 1 muted, stored from the live mutes.
    post(Cmd::set_mute, 0, 0, 1, 0);
    post(Cmd::mute_store, 0, 0, 2);
    post(Cmd::set_mute, 0, 0, 0, 0);
    pump();

    FakeHost::the().set_running(true);
    tick_at(0);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    CHECK(!engine.muted(0));
    tick_at(1536);                                 // entry 2 starts: track 1 muted
    CHECK_EQ(live_slot(), 1);
    CHECK(engine.muted(0));
    CHECK(!engine.muted(1));

    // The knob walks the recall 0-8 and clamps; a set from a STEP key keeps it.
    chain_op(ChainEdit::mutes_delta, 1, uint8_t(int8_t(20)));
    CHECK_EQ(int(engine.project().chain[1].mutes), 8);
    chain_op(ChainEdit::mutes_delta, 1, uint8_t(int8_t(-20)));
    CHECK_EQ(int(engine.project().chain[1].mutes), 0);
    chain_op(ChainEdit::mutes_delta, 1, 2);
    chain_op(ChainEdit::pattern_delta, 1, 1);
    chain_op(ChainEdit::repeats_delta, 1, 1);
    CHECK_EQ(int(engine.project().chain[1].mutes), 2);
    const uint32_t before = refused();
    chain_op(ChainEdit::set, 1, 0, 1, 1, 9);       // past eight: refused
    CHECK_EQ(int(refused() - before), 1);

    post(Cmd::set_chain, 0, 0, 0);
    post(Cmd::set_mute, 0, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(engine_song_mode_plays_the_chain_once_and_asks_for_a_stop) {
    Engine &engine = clean();
    Project &incoming = engine.project_for_load();
    incoming.settings.chain_length = 2;
    incoming.chain[0] = ChainEntry{0, 0, 1, 0};
    incoming.chain[1] = ChainEntry{0, 1, 1, 0};
    for (int i = 2; i < kChainEntries; i++) incoming.chain[i] = ChainEntry{};
    post(Cmd::project_loaded);
    pump();
    put_trig(kMono, 0);                            // A01 and A02 each sound on step 1
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::immediate));
    pump();
    put_trig(kMono, 0);
    chain_op(ChainEdit::song, 0, 1);
    CHECK(engine.project().settings.chain_song);

    FakeHost::the().set_running(true);
    tick_at(0);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    tick_at(1);                                    // the switch to A01 re-derives the boundary
    const uint8_t ends = engine.status().song_end.load();
    tick_at(1536);                                 // entry 2
    CHECK_EQ(live_slot(), 1);
    CHECK_EQ(int(engine.status().song_end.load()), int(ends));
    tick_at(1537);
    // The end of the last entry: back on entry 1, a stop asked for, and silent meanwhile.
    CHECK_EQ(fired_on(kMono, 2u * 1536u), 0);
    CHECK_EQ(int(uint8_t(engine.status().song_end.load() - ends)), 1);
    CHECK(engine.chain().active);
    CHECK_EQ(int(engine.chain().entry), 0);
    CHECK_EQ(live_slot(), 0);
    int hits = 0;
    for (uint32_t t = 2u * 1536u + 1u; t < 3u * 1536u; t++) hits += fired_on(kMono, t);
    CHECK_EQ(hits, 0);

    // The stop the main context sends, then PLAY from 0: the song plays from the top.
    post(Cmd::stop, 0, 0, 0);
    pump();
    CHECK_EQ(fired_on(kMono, 0), 1);
    CHECK_EQ(live_slot(), 0);

    // Song off: the chain loops as before.
    chain_op(ChainEdit::song, 0, 0);
    tick_at(1536);
    tick_at(1537);
    tick_at(2u * 1536u);
    CHECK_EQ(int(uint8_t(engine.status().song_end.load() - ends)), 1);
    CHECK_EQ(live_slot(), 0);
    CHECK_EQ(int(engine.chain().entry), 0);
    post(Cmd::set_chain, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(engine_starting_an_empty_chain_is_refused) {
    Engine &engine = clean();
    CHECK_EQ(chain_length(engine.project()), 0);
    const uint32_t before = refused();
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    // A chain that is silently "on" and does nothing is worse than a refusal the UI can show.
    CHECK(!engine.chain().active);
    CHECK_EQ(int(refused() - before), 1);
}

// --- mutes -----------------------------------------------------------------------------------

TEST(engine_a_mute_stops_a_tracks_trigs_without_stopping_the_sequencer) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    post(Cmd::set_mute, kPoly, 0, 1, 0);
    pump();
    CHECK(engine.muted(kPoly));

    CHECK_EQ(fired_on(kPoly, 0u), 0);              // no trig reaches the machine ...
    CHECK_EQ(poly().busy_voices(), 0);
    CHECK_EQ(playhead(kPoly), 0);                  // ... and the sequencer walked over it anyway
    CHECK_EQ(fired_on(kPoly, 24u), 0);
    CHECK_EQ(playhead(kPoly), 1);

    post(Cmd::set_mute, kPoly, 0, 0, 0);
    pump();
    CHECK(!engine.muted(kPoly));
    CHECK_EQ(fired_on(kPoly, 1536u), 1);
    CHECK_EQ(poly().busy_voices(), 1);
}

TEST(engine_a_mute_silences_the_voices_it_finds_rather_than_freezing_them) {
    // A muted track is not rendered, so a voice left in its machine stopped mid-note and resumed
    // on unmute -- a sustaining POLY chord came back from where it was. And a pad hit on a muted
    // track waited in the machine and sounded on unmute. A mute now cuts the track's voices, and
    // a muted track takes no pad hits.
    Engine &engine = clean();
    post(Cmd::trigger_note, kPoly, 0, 60, 100);
    pump(2);
    CHECK(poly().busy_voices() > 0);

    post(Cmd::set_mute, kPoly, 0, 1, 0);
    pump();
    CHECK(engine.muted(kPoly));
    CHECK_EQ(poly().busy_voices(), 0);

    post(Cmd::trigger_note, kPoly, 0, 64, 100);    // a pad hit while muted
    pump();
    CHECK_EQ(poly().busy_voices(), 0);

    post(Cmd::set_mute, kPoly, 0, 0, 0);
    pump();
    CHECK_EQ(poly().busy_voices(), 0);             // nothing resumes, nothing was waiting
    CHECK_NEAR(energy(4), 0.0f, 1e-6);
}

TEST(engine_a_mute_armed_for_the_bar_waits_for_the_bar) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    sync_bars();                                   // bar 2, with the next bar at tick 1152
    CHECK(!engine.muted(3));

    // b = 1: at the next bar. The latch holds a whole mute STATE rather than one toggle, which
    // is why the arming happens on this side -- only it knows what the mutes are now.
    post(Cmd::set_mute, 3, 0, 1, 1);
    pump();
    CHECK(!engine.muted(3));
    tick_at(1151);
    CHECK(!engine.muted(3));
    tick_at(1152);
    CHECK(engine.muted(3));

    // A performer pressing two pads before the bar means the second one, not a queue of two.
    post(Cmd::set_mute, 4, 0, 1, 1);
    post(Cmd::set_mute, 5, 0, 1, 1);
    pump();
    CHECK(!engine.muted(4));
    CHECK(!engine.muted(5));
    tick_at(4u * kTicksPerBar);
    CHECK(engine.muted(3));
    CHECK(!engine.muted(4));
    CHECK(engine.muted(5));
}

TEST(engine_the_eight_stored_mute_states_round_trip) {
    Engine &engine = clean();
    const uint8_t first_state[3] = {1, 3, 6};
    for (uint8_t t : first_state) post(Cmd::set_mute, t, 0, 1, 0);
    post(Cmd::mute_store, 0, 0, 2);
    pump();
    CHECK_EQ(int(engine.project().settings.mute_states[2]),
             int(uint8_t((1u << 1) | (1u << 3) | (1u << 6))));

    for (uint8_t t = 0; t < kTracks; t++) post(Cmd::set_mute, t, 0, t == 0 ? 1 : 0, 0);
    post(Cmd::mute_store, 0, 0, 7);
    pump();
    CHECK_EQ(int(engine.project().settings.mute_states[7]), 1);

    post(Cmd::mute_recall, 0, 0, 2, 0);
    pump();
    for (int t = 0; t < kTracks; t++) CHECK_EQ(engine.muted(t), t == 1 || t == 3 || t == 6);

    post(Cmd::mute_recall, 0, 0, 7, 0);
    pump();
    for (int t = 0; t < kTracks; t++) CHECK_EQ(engine.muted(t), t == 0);

    // There are eight slots and no ninth, so a store or a recall past the end does nothing --
    // writing one would be a write past `ProjectSettings::mute_states`.
    post(Cmd::mute_recall, 0, 0, 8, 0);
    post(Cmd::mute_store, 0, 0, 8);
    pump();
    for (int t = 0; t < kTracks; t++) CHECK_EQ(engine.muted(t), t == 0);
    CHECK_EQ(int(engine.project().settings.mute_states[7]), 1);

    // And a recall can wait for the bar, exactly as a mute does.
    FakeHost::the().set_running(true);
    sync_bars();
    post(Cmd::mute_recall, 0, 0, 2, 1);
    pump();
    for (int t = 0; t < kTracks; t++) CHECK_EQ(engine.muted(t), t == 0);
    tick_at(3u * kTicksPerBar);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(engine.muted(t), t == 1 || t == 3 || t == 6);
}

// --- the performance layer -------------------------------------------------------------------

TEST(engine_a_morph_between_two_scenes_moves_the_parameters) {
    Engine &engine = clean();
    two_scenes_differing_in(kPoly, kLvl, 0, 127);
    CHECK_EQ(int(kit_value(kPoly, kLvl)), 127);

    post_u(Cmd::set_morph, 0);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);  // at rest the knob owns it, not scene A
    CHECK_EQ(int(kit_value(kPoly, kLvl)), 127);    // the kit is untouched by the morph

    post_u(Cmd::set_morph, 1);                     // one click off rest: scene A has it
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 0);

    post_u(Cmd::set_morph, kMorphScale);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);

    // A shift and a rounding add, not a divide: core/perform.cpp's own arithmetic, reached
    // through the ring.
    const uint32_t positions[3] = {256u, 512u, 768u};
    const int expected[3] = {32, 64, 95};
    for (int i = 0; i < 3; i++) {
        post_u(Cmd::set_morph, positions[i]);
        pump();
        CHECK_EQ(int(engine.morph()), int(positions[i]));
        CHECK_EQ(int(engine.performed(kPoly, kLvl)), expected[i]);
    }

    // Past the end it clamps rather than wrapping.
    post_u(Cmd::set_morph, kMorphScale + 1000u);
    pump();
    CHECK_EQ(int(engine.morph()), int(kMorphScale));
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);

    // A track neither scene mentions is left exactly as the kit has it.
    CHECK_EQ(int(engine.performed(kMono, kDec)), int(kit_value(kMono, kDec)));
}

TEST(engine_a_morph_is_heard_between_steps_and_not_only_at_the_next_trig) {
    // What the per-block copy of `performed_` into `effective_` exists for: without it a morph
    // would only be heard at the next trig and sweeping VALUE between steps would do nothing.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kDec, 127);     // a note that rings for seconds
    pump();
    two_scenes_differing_in(kPoly, kLvl, 0, 127);
    post_u(Cmd::set_morph, 1);                     // one click off rest, where scene A owns it:
                                                   // at rest the morph hands the level back to
                                                   // the knob (core/perform.cpp)
    put_trig(kPoly, 0);
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 0);
    CHECK_EQ(pump(), 0.0f);

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK_EQ(pump(8), 0.0f);                       // scene A has the level all the way down
    CHECK_EQ(poly().sounding_voices(), 1);         // but the note is there, and ringing

    post_u(Cmd::set_morph, kMorphScale);
    CHECK(pump(8) > 0.01f);                        // the same note, no new trig
}

TEST(engine_a_macro_at_rest_hands_its_parameter_back_to_the_knob) {
    // The bug this rule fixes: an assigned macro at 0 used to write its `low` on every rebuild,
    // so turning that parameter in EDIT moved the screen and not the sound.
    Engine &engine = clean();
    post_macro_assign(0, kPoly, kLvl, 40, 120);
    post(Cmd::set_macro, 0, 0, 0, 0);
    pump();
    post(Cmd::set_param, kPoly, 0, kLvl, 90);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 90);   // the knob is heard
    post(Cmd::set_param, kPoly, 0, kLvl, 17);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 17);   // every time

    // Moved off 0, the macro has it; back to 0, the knob has it again.
    post(Cmd::set_macro, 0, 0, 0, 127);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 120);
    post(Cmd::set_macro, 0, 0, 0, 0);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 17);
}

TEST(engine_a_macro_at_rest_lets_a_scene_morph_through) {
    // Scenes are built under macros (kit -> morph -> macros); a resting macro on the same
    // parameter used to overwrite the morph.
    Engine &engine = clean();
    two_scenes_differing_in(kPoly, kLvl, 10, 110);
    post_macro_assign(0, kPoly, kLvl, 50, 60);
    post(Cmd::set_macro, 0, 0, 0, 0);
    post_u(Cmd::set_morph, kMorphScale);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 110);
}

TEST(engine_a_macro_drives_its_assigned_targets) {
    Engine &engine = clean();
    // Two targets on one macro, one of them inverted: one knob opening a filter while it closes
    // a send is most of what makes a macro worth having (core/perform.hpp).
    // The parameters start where the ranges do: a macro sweeps from the LIVE value to `high`
    // (core/perform.hpp), so these are the values a knob turned to 0 and 127 would leave.
    post(Cmd::set_macro, 0, 0, 0, 0);
    post(Cmd::set_param, kPoly, 0, kLvl, 0);
    post(Cmd::set_param, kMono, 0, kLvl, 127);
    post_macro_assign(0, kPoly, kLvl, 0, 127);
    post_macro_assign(0, kMono, kLvl, 127, 0);
    pump();
    CHECK_EQ(macro_count(engine.macros().macros[0]), 2);

    // At 0 the macro writes nothing: both parameters are the kit's (core/perform.hpp).
    post(Cmd::set_macro, 0, 0, 0, 0);
    pump();
    CHECK_EQ(int(engine.macro_position(0)), 0);
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), int(engine.params(kPoly).v[kLvl]));
    CHECK_EQ(int(engine.performed(kMono, kLvl)), int(engine.params(kMono).v[kLvl]));

    // One detent up and the range is the macro's.
    post(Cmd::set_macro, 0, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 1);
    CHECK_EQ(int(engine.performed(kMono, kLvl)), 126);

    post(Cmd::set_macro, 0, 0, 0, 127);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);
    CHECK_EQ(int(engine.performed(kMono, kLvl)), 0);

    // Rounded to nearest and not truncated, so a knob swept up does not lag by a value for most
    // of its travel and two macros over one parameter agree (core/perform.cpp).
    post(Cmd::set_macro, 0, 0, 0, 64);
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 64);
    CHECK_EQ(int(engine.performed(kMono, kLvl)), 63);

    // A position past the end of the knob clamps, and the kit is untouched throughout.
    post(Cmd::set_macro, 0, 0, 0, 200);
    pump();
    CHECK_EQ(int(engine.macro_position(0)), 127);
    CHECK_EQ(int(kit_value(kPoly, kLvl)), 0);            // the value this test set, untouched

    post(Cmd::macro_clear, 0, 0, 0);
    pump();
    CHECK_EQ(macro_count(engine.macros().macros[0]), 0);
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), int(kit_value(kPoly, kLvl)));
    CHECK_EQ(int(engine.performed(kMono, kLvl)), int(kit_value(kMono, kLvl)));
}

TEST(engine_a_macro_with_four_targets_refuses_a_fifth) {
    Engine &engine = clean();
    for (uint8_t i = 0; i < kMacroTargets; i++) post_macro_assign(1, i, kLvl, 0, 127);
    pump();
    CHECK_EQ(macro_count(engine.macros().macros[1]), kMacroTargets);

    // Which of the four a target lands in is not something a performer should have to think
    // about, so the command takes the next free one -- and a macro that is full says so.
    const uint32_t before = refused();
    post_macro_assign(1, 7, kLvl, 0, 127);
    pump();
    CHECK_EQ(int(refused() - before), 1);
    CHECK_EQ(macro_count(engine.macros().macros[1]), kMacroTargets);

    // A parameter index no track has is not a target either (kTrackParams + 0-2 are the master
    // EQ's bands, so the first one with no meaning is past them).
    post_macro_assign(2, 0, uint8_t(kTrackParams + kMasterBands), 0, 127);
    pump();
    CHECK_EQ(macro_count(engine.macros().macros[2]), 0);
}

TEST(engine_a_lock_always_wins_over_the_performance_layer) {
    // docs/REVIEW.md settled "base kit values only" for scenes. From this side that means a
    // step's lock beats whatever the morph, the macros and a throw produced -- and for the WHOLE
    // step rather than only the block that triggered it, which is what `locked_` is for.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kDec, 127);
    pump();
    two_scenes_differing_in(kPoly, kLvl, 0, 127);
    post_u(Cmd::set_morph, kMorphScale);           // the morph asks for full level
    put_trig(kPoly, 0);
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);

    post(Cmd::lock_delta, kPoly, 0, kLvl, uint8_t(int8_t(-120)));
    post(Cmd::lock_delta, kPoly, 0, kLvl, uint8_t(int8_t(-120)));
    pump();
    uint8_t locked = 1;
    CHECK(pattern_get_lock(engine.pattern(), kPoly, 0, kLvl, locked));
    CHECK_EQ(int(locked), 0);
    CHECK_EQ(pump(), 0.0f);

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();                                        // the block the step started in ...
    CHECK_EQ(pump(40), 0.0f);                      // ... and forty more of the same step
    CHECK_EQ(int(engine.performed(kPoly, kLvl)), 127);   // the morph still asks for full level

    // Without the lock the same morph and the same step are loud, so the silence above is the
    // lock winning and not a silent patch.
    post(Cmd::clear_lock, kPoly, 0, kLvl);
    pump();
    CHECK(!pattern_get_lock(engine.pattern(), kPoly, 0, kLvl, locked));
    FakeHost::the().deliver_tick(1536, 0);
    pump();
    CHECK(pump(40) > 0.01f);
}

TEST(engine_a_throw_opens_a_send_without_touching_the_kit) {
    Engine &engine = clean();
    CHECK_EQ(int(kit_value(kPoly, kSendA)), 0);    // clean() closed both sends
    CHECK_EQ(int(engine.performed(kPoly, kSendA)), 0);

    post(Cmd::set_throw, kPoly, 0, 1);             // 1 = the delay, send A
    pump();
    CHECK_EQ(engine.throwing(), 1);
    CHECK_EQ(int(engine.performed(kPoly, kSendA)), 127);
    CHECK_EQ(int(engine.performed(kPoly, kSendB)), 0);
    CHECK_EQ(int(kit_value(kPoly, kSendA)), 0);    // not a parameter edit
    CHECK_EQ(int(engine.performed(kMono, kSendA)), 0);   // and only the thrown track moves

    post(Cmd::set_throw, kPoly, 0, 2);             // 2 = the reverb, send B
    pump();
    CHECK_EQ(int(engine.performed(kPoly, kSendA)), 0);
    CHECK_EQ(int(engine.performed(kPoly, kSendB)), 127);

    // Release, and the stored value comes back by itself: there is nothing to remember.
    post(Cmd::set_throw, kPoly, 0, 0);
    pump();
    CHECK_EQ(engine.throwing(), 0);
    CHECK_EQ(int(engine.performed(kPoly, kSendB)), 0);

    // A bus the panel cannot ask for is off, not a third send.
    post(Cmd::set_throw, kPoly, 0, 3);
    pump();
    CHECK_EQ(engine.throwing(), 0);
    CHECK_EQ(int(engine.performed(kPoly, kSendA)), 0);
}

TEST(engine_a_throw_really_routes_the_track_to_the_send) {
    // The audible half: with send B open the master keeps ringing long after the note itself has
    // stopped, where a closed send leaves exact silence.
    Engine &engine = clean();
    (void)engine;
    post(Cmd::set_param, kPoly, 0, kDec, 0);       // the shortest note the machine has
    put_trig(kPoly, 0);
    pump();
    CHECK_EQ(pump(), 0.0f);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(10) > 0.01f);
    pump(40);
    const float dry_tail = pump(300);

    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 0);
    put_trig(kPoly, 0);
    post(Cmd::set_throw, kPoly, 0, 2);
    pump();
    CHECK_EQ(pump(), 0.0f);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    CHECK(pump(10) > 0.01f);
    pump(40);
    const float wet_tail = pump(300);

    CHECK_EQ(dry_tail, 0.0f);
    CHECK(wet_tail > 1e-4f);
}

TEST(engine_a_scene_capture_takes_the_knob_and_not_the_stored_kit) {
    // **A BUG, pinned rather than hidden.** app/engine.cpp's own comment on Cmd::scene_capture
    // says "From the KIT, not from `performed_`: a scene is a sound to come back to... A capture
    // takes the CURRENT kit, which is why it happens on this side: `params_` is what the machines
    // read and the UI only has a block-old copy of it."
    //
    // It then captures `project_.kit`, which is NOT `params_`. The two are separate structures in
    // separate layers and `Cmd::kit_store` is the only thing that copies one into the other --
    // app/engine.cpp says so itself. Nothing posts `kit_store` except a SAVE (app/storage.cpp),
    // and app/app.cpp's PERFORM-mode OK posts `scene_capture` on its own. So turning a knob and
    // capturing a scene stores the value the kit had at the last save, not the sound in the room.
    //
    // FIXED: `Cmd::scene_capture` now calls `kit_from_params()` first, so the capture is the
    // sound in the room. This test was written to pin the bug and now pins the fix -- which is
    // what a tripwire is for.
    Engine &engine = clean();
    post(Cmd::set_param, kPoly, 0, kLvl, 11);      // a knob turn: `params_` moves ...
    pump();
    CHECK_EQ(int(kit_value(kPoly, kLvl)), 11);
    CHECK_EQ(int(engine.project().kit.tracks[kPoly].params[kLvl]), 127);   // ... `project_.kit` does not

    post(Cmd::scene_capture, kPoly, 0, 3, 0);
    pump();
    CHECK(scene_has(engine.scenes().scenes[3], kPoly, kLvl));
    CHECK_EQ(int(engine.scenes().scenes[3].values[kPoly][kLvl]), 11);      // the knob, not the file

    // And the capture refreshed the stored kit on its way past, which is the mechanism: there is
    // no longer any difference between capturing with and without a store first.
    CHECK_EQ(int(engine.project().kit.tracks[kPoly].params[kLvl]), 11);
    post(Cmd::scene_capture, kPoly, 0, 4, 0);
    pump();
    CHECK_EQ(int(engine.scenes().scenes[4].values[kPoly][kLvl]), 11);

    // One track only, which is what capturing with a track selected does.
    post(Cmd::set_param, kMono, 0, kLvl, 22);
    post(Cmd::kit_store);
    post(Cmd::scene_erase, 0, 0, 5);
    post(Cmd::scene_capture, kMono, 0, 5, 1);
    pump();
    CHECK_EQ(scene_count(engine.scenes().scenes[5]), kKitParams);
    CHECK(scene_has(engine.scenes().scenes[5], kMono, kLvl));
    CHECK(!scene_has(engine.scenes().scenes[5], kPoly, kLvl));

    // A scene slot that does not exist is ignored rather than clamped onto scene 0.
    post(Cmd::scene_capture, kMono, 0, uint8_t(kScenes), 0);
    pump();
    CHECK_EQ(scene_count(engine.scenes().scenes[0]), 0);
}

TEST(engine_kit_recall_is_drawn_and_heard) {
    // **A SECOND BUG of the same family, pinned rather than hidden.** M5 added the per-block copy
    // of `performed_` into `effective_`, and anything that writes `params_` without setting
    // `perform_dirty_` is reverted inside the same block: `apply_performance` returns early,
    // `performed_` still holds the old values, and the copy puts them back before a sample is
    // rendered. tests/test_app.cpp has three tests for exactly this, found by a review --
    // `Cmd::set_param`, `Cmd::project_loaded` and `Cmd::lock_delta` were each missing it.
    //
    // `Cmd::kit_recall` is the fourth and it was missed: it calls `params_from_kit()` and sets no
    // flag. So a recalled kit is DRAWN (the UI reads `params_`) and not HEARD (the machines read
    // `effective_`). The fix is the same one line, `perform_dirty_ = true;`.
    //
    // It is latent rather than live today: nothing posts `kit_recall` -- app/engine.cpp says so
    // in as many words, and the gesture it waits for is F1 + F8. The day that key lands, this is
    // what will be wrong with it.
    Engine &engine = clean();
    post(Cmd::set_param, 2, 0, kLvl, 42);
    post(Cmd::kit_store);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[2].params[kLvl]), 42);

    post(Cmd::set_param, 2, 0, kLvl, 99);
    pump();
    CHECK_EQ(int(kit_value(2, kLvl)), 99);
    CHECK_EQ(int(engine.performed(2, kLvl)), 99);

    // FIXED: `Cmd::kit_recall` now sets `perform_dirty_`, so the recall is heard as well as
    // drawn. It was the fourth member of the family tests/test_app.cpp has three tests for, and
    // the only one that was still latent -- nothing posts this command until F1 + F8 lands.
    post(Cmd::kit_recall);
    pump(2);
    CHECK_EQ(int(kit_value(2, kLvl)), 42);               // the screen followed the recall ...
    CHECK_EQ(int(engine.performed(2, kLvl)), 42);        // ... and so did what is played

    // And it stays right, where before the next knob turn anywhere was what repaired it.
    post(Cmd::set_param, 0, 0, kLvl, 127);
    pump();
    CHECK_EQ(int(engine.performed(2, kLvl)), 42);
}

// --- the clock, and what the engine may not do to it -----------------------------------------

TEST(engine_never_touches_the_transport_itself) {
    // enaga::clock's transport calls are MAIN CONTEXT (<enaga/clock.hpp>), so routing them
    // through the ring would call them from audio(), where they silently do nothing. `Cmd::play`
    // and `Cmd::set_tempo` exist so the UI's intent shows up in a trace, and for nothing else.
    clean();
    FakeHost &host = FakeHost::the();
    const int starts = host.control_calls(ENAGA_CLOCK_START);
    const int stops = host.control_calls(ENAGA_CLOCK_STOP);
    const int tempos = host.control_calls(ENAGA_CLOCK_TEMPO);
    const int locates = host.control_calls(ENAGA_CLOCK_LOCATE);

    post(Cmd::play);
    post_f(Cmd::set_tempo, 150.0f);
    post(Cmd::stop, 0, 0, 1);
    pump(4);

    CHECK_EQ(host.control_calls(ENAGA_CLOCK_START), starts);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_STOP), stops);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_TEMPO), tempos);
    CHECK_EQ(host.control_calls(ENAGA_CLOCK_LOCATE), locates);
    CHECK(!host.running());
    CHECK_NEAR(host.tempo(), 130.0f, 0.01);
}

TEST(engine_a_tempo_that_is_not_a_number_or_out_of_range_is_refused_before_conversion) {
    // int() of a NaN or of a float past INT_MAX is undefined, and the range check used to run
    // on the int AFTER that conversion. It now runs on the float.
    Engine &engine = clean();
    post_f(Cmd::set_tempo, 120.0f);
    pump();
    const uint16_t good = engine.project().settings.tempo_centi_bpm;
    CHECK_EQ(int(good), 12000);

    const uint32_t before = refused();
    post_f(Cmd::set_tempo, __builtin_nanf(""));
    post_f(Cmd::set_tempo, __builtin_inff());
    post_f(Cmd::set_tempo, 1.0e30f);
    post_f(Cmd::set_tempo, -1.0e30f);
    pump();
    CHECK_EQ(int(refused() - before), 4);
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), int(good));
}

TEST(engine_follows_the_clock_rather_than_a_play_command) {
    Engine &engine = clean();
    (void)engine;
    FakeHost &host = FakeHost::the();
    put_trig(kPoly, 0);
    post(Cmd::play);
    pump(2);
    CHECK_EQ(running(), 0);
    CHECK_EQ(poly().busy_voices(), 0);

    // An OUTSIDE clock, which is the half of CLAUDE.md's rule the emulator cannot reach: the
    // transport calls are refused and the engine plays what the clock delivers anyway.
    host.set_source(ENAGA_CLOCK_MIDI);
    CHECK(!enaga::clock::start());
    CHECK(!host.running());
    host.set_running(true);                        // MIDI said so, not STRATA
    pump();
    CHECK_EQ(running(), 1);
    CHECK_EQ(fired_on(kPoly, 0u), 1);

    host.set_running(false);
    pump();
    CHECK_EQ(running(), 0);
}

TEST(engine_a_queued_change_under_an_external_clock_follows_the_clocks_run_state) {
    Engine &engine = clean();
    FakeHost &host = FakeHost::the();
    host.set_source(ENAGA_CLOCK_SYNC);
    CHECK(!enaga::clock::stop());
    CHECK(!host.running());

    // Stopped, whatever the source: there is no end of a pass to wait for, so it lands now.
    post(Cmd::queue_pattern, 0, 0, 0, 3, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK_EQ(live_slot(), 3);
    CHECK(!engine.queue().pending);

    // Running under the outside clock it waits, exactly as it does on the internal one -- the
    // engine asks the CLOCK, not a flag of its own.
    host.set_running(true);
    tick_at(600);
    post(Cmd::queue_pattern, 0, 0, 0, 9, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(engine.queue().pending);
    CHECK_EQ(live_slot(), 3);
}

TEST(engine_stop_releases_the_voices_and_stop_twice_cuts_them) {
    Engine &engine = clean();
    (void)engine;
    put_trig(kPoly, 0);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(poly().sounding_voices(), 1);

    // STOP once: a release, with the tail. The voice is still sounding a block later.
    post(Cmd::stop, 0, 0, 0);
    pump();
    CHECK_EQ(poly().sounding_voices(), 1);
    CHECK(poly().busy_voices() > 0);

    // STOP twice: `a = 1` -- `a`, which is what app/app.cpp sends and what app/commands.hpp
    // documents -- and the voices go at once, with no tail.
    post(Cmd::stop, 0, 0, 1);
    pump();
    CHECK_EQ(poly().sounding_voices(), 0);
    CHECK_EQ(poly().busy_voices(), 0);
    CHECK_EQ(poly().voice_note(0), -1);
}

// --- the ring, and the guards on a damaged command -------------------------------------------

TEST(engine_applies_at_most_sixty_four_commands_in_one_block) {
    Engine &engine = clean();
    (void)engine;
    // docs/SPEC.md's figure, so that a burst of edits cannot break the deadline. Sixty-four
    // trigs on one track and six on another: the first block takes the sixty-four, and the
    // second takes the rest -- nothing is dropped, it waits.
    for (uint8_t s = 0; s < 64; s++) post(Cmd::toggle_trig, 1, s);
    for (uint8_t s = 0; s < 6; s++) post(Cmd::toggle_trig, 2, s);
    CHECK_EQ(int(Commands::kMaxPerBlock), 64);

    pump();
    for (int s = 0; s < 64; s++) CHECK(trig_at(1, s));
    for (int s = 0; s < 6; s++) CHECK(!trig_at(2, s));
    pump();
    for (int s = 0; s < 6; s++) CHECK(trig_at(2, s));
}

TEST(engine_a_parameter_a_track_does_not_have_is_ignored) {
    Engine &engine = clean();
    CHECK_EQ(int(kit_value(0, kLvl)), 127);
    post(Cmd::set_param, 0, 0, kLvl, 200);         // a byte is wider than a parameter
    pump();
    CHECK_EQ(int(kit_value(0, kLvl)), 127);        // clamped to the top of the range

    post(Cmd::set_param, 0, 0, uint8_t(kTrackParams), 5);
    post(Cmd::set_param, 0, 0, 200, 5);
    pump();
    for (int p = 0; p < kTrackParams; p++) CHECK(int(kit_value(0, p)) != 5);

    // And a lock on a parameter no track has is not stored either.
    post(Cmd::lock_delta, 0, 0, uint8_t(kTrackParams), uint8_t(int8_t(-1)));
    pump();
    CHECK_EQ(int(pattern_locks_reachable(engine.pattern())), 0);
}

// --- note repeat -----------------------------------------------------------------------------

// A track with nothing on it, so every trigger counted is the roll's.
void empty_track(Engine &engine, int track) {
    for (int s = 0; s < kStepsPerTrack; s++) engine.project_for_load().live().tracks[track].steps[s].flags = 0;
}

TEST(engine_note_repeat_stopped_rolls_free_at_the_repeat_length) {
    Engine &engine = clean();
    FakeHost::the().set_running(false);
    FakeHost::the().set_tempo(120.0f);
    empty_track(engine, kPoly);
    pump();
    // At 120 BPM a sixteenth is 0.125 s: 6,000 frames at 48 kHz.
    const float period = 24.0f * kRate * 60.0f / (120.0f * 96.0f);
    const uint8_t before = triggers(kPoly);
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 1);
    pump();
    CHECK_EQ(int(uint8_t(triggers(kPoly) - before)), 1);    // the press hits at once
    CHECK(engine.rolling(kPoly));
    const int blocks = int(4.0f * period / float(kBlk)) + 1;   // a little past four periods
    for (int b = 1; b < blocks; b++) pump();
    CHECK_EQ(int(uint8_t(triggers(kPoly) - before)), 5);

    // Let go and it stops.
    post(Cmd::note_repeat, kPoly, 0, 0, 0, 0);
    const uint8_t at_release = triggers(kPoly);
    for (int b = 0; b < blocks; b++) pump();
    CHECK(!engine.rolling(kPoly));
    CHECK_EQ(int(triggers(kPoly)), int(at_release));
}

TEST(engine_note_repeat_running_hits_the_grid_and_skips_a_flam) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kPoly);
    tick_at(100);
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 1);
    CHECK_EQ(fired_on(kPoly, 101), 1);          // the press, on the block it arrives
    int hits = 0;
    for (uint32_t t = 102; t <= 200; t++) hits += fired_on(kPoly, t);
    CHECK_EQ(hits, 4);                           // 120, 144, 168, 192

    // Pressed half a length or less before a grid tick, that tick is skipped: no flam.
    post(Cmd::note_repeat, kPoly, 0, 0, 0, 0);
    tick_at(205);
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 1);
    CHECK_EQ(fired_on(kPoly, 210), 1);          // the press
    CHECK_EQ(fired_on(kPoly, 216), 0);          // 6 ticks after the press: skipped
    CHECK_EQ(fired_on(kPoly, 240), 1);
    post(Cmd::note_repeat, kPoly, 0, 0, 0, 0);
    pump();

    // The length is beat repeat's: a thirty-second hits every 12 ticks.
    post(Cmd::set_repeat, 0, 0, uint8_t(RepeatLength::thirty_second), 0, 0);
    tick_at(300);
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 1);
    pump();
    hits = 0;
    for (uint32_t t = 301; t <= 360; t++) hits += fired_on(kPoly, t);
    CHECK_EQ(hits, 5);                           // 312, 324, 336, 348, 360
    post(Cmd::note_repeat, kPoly, 0, 0, 0, 0);
    post(Cmd::set_repeat, 0, 0, uint8_t(RepeatLength::sixteenth), 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(engine_note_repeat_velocity_mute_and_refusal) {
    Engine &engine = clean();
    FakeHost::the().set_running(false);
    empty_track(engine, kPoly);
    // Pressure on a pad that is not rolling changes nothing and starts nothing.
    post(Cmd::note_repeat, kPoly, 0, 0, 90, 2);
    pump();
    CHECK(!engine.rolling(kPoly));
    // A muted track rolls silently: the roll is held, nothing fires.
    post(Cmd::set_mute, kPoly, 0, 1, 0);
    const uint8_t before = triggers(kPoly);
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 1);
    for (int b = 0; b < 400; b++) pump();
    CHECK(engine.rolling(kPoly));
    CHECK_EQ(int(triggers(kPoly)), int(before));
    post(Cmd::note_repeat, kPoly, 0, 0, 0, 0);
    post(Cmd::set_mute, kPoly, 0, 0, 0);
    // An action past 2 is refused.
    const uint32_t refusals = refused();
    post(Cmd::note_repeat, kPoly, 0, 60, 100, 3);
    pump();
    CHECK_EQ(refused(), refusals + 1);
    CHECK(!engine.rolling(kPoly));
}

// --- the arpeggiator --------------------------------------------------------------------------

// The four ARP bytes on `track`, as the smallest byte of each zone (core/arp.hpp): MOD 1 = UP,
// RAT 5 = 1/32, OCT 1 = one octave.
void set_arp(uint8_t track, int mode, int rate, int octaves = 1, uint8_t gate = 0) {
    auto smallest = [](int zone, int zones) { return uint8_t((zone * 128 + zones - 1) / zones); };
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 0), smallest(mode, int(ArpMode::count)));
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 1), smallest(rate, kArpRates));
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 2), smallest(octaves - 1, kArpMaxOctaves));
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 3), gate);
    pump();
}

// One pass of a fresh pattern: 64 steps of 24 ticks.
constexpr uint32_t kPass = uint32_t(kStepsPerTrack) * 24u;

// Hits on `track` over ticks [from, to).
int hits_between(int track, uint32_t from, uint32_t to) {
    int hits = 0;
    for (uint32_t t = from; t < to; t++) hits += fired_on(track, t);
    return hits;
}

TEST(engine_arp_off_plays_a_step_once) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kMono);
    put_trig(kMono, 0);
    CHECK_EQ(hits_between(kMono, 0, kPass), 1);
    CHECK(!engine.arp_running(kMono));
    FakeHost::the().set_running(false);
}

TEST(engine_arp_a_step_hits_at_the_rate_for_its_length) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kMono);
    put_trig(kMono, 0);
    set_arp(kMono, 1, 5);                        // UP, 1/32: every 12 ticks
    // A one-step length (24 ticks): 0 and 12, and not the next step's own tick.
    CHECK_EQ(hits_between(kMono, 0, kPass), 2);
    CHECK(!engine.arp_running(kMono));
    // Every pass the same.
    CHECK_EQ(hits_between(kMono, kPass, 2 * kPass), 2);
    // Four steps (96 ticks): eight hits.
    set_length_index(kMono, 0, 12);
    CHECK_EQ(hits_between(kMono, 2 * kPass, 3 * kPass), 8);
    // A 1/8 (48 ticks) over the same four steps: two.
    set_arp(kMono, 1, 1);
    CHECK_EQ(hits_between(kMono, 3 * kPass, 4 * kPass), 2);
    // Back to OFF: the step plays once, as before.
    set_arp(kMono, 0, 0);
    CHECK_EQ(hits_between(kMono, 4 * kPass, 5 * kPass), 1);
    FakeHost::the().set_running(false);
}

TEST(engine_arp_a_lock_on_mod_arps_one_step_only) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kMono);
    put_trig(kMono, 0);
    put_trig(kMono, 8);
    set_arp(kMono, 0, 5);                        // OFF on the kit, 1/32
    CHECK(pattern_set_lock(engine.project_for_load().live(), kMono, 8, uint8_t(kArpFirst), 22));
    CHECK_EQ(hits_between(kMono, 0, 8 * 24), 1);         // step 0: one note
    CHECK_EQ(hits_between(kMono, 8 * 24, 9 * 24), 2);    // step 8, locked UP: 2 hits
    CHECK_EQ(hits_between(kMono, 9 * 24, kPass), 0);
    FakeHost::the().set_running(false);
}

TEST(engine_arp_a_new_step_takes_over_and_stop_ends_it) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kMono);
    put_trig(kMono, 0);
    put_trig(kMono, 2);
    set_length_index(kMono, 0, 16);              // 16 steps: longer than the gap to step 2
    set_arp(kMono, 1, 3);                        // 1/16: every 24 ticks
    // Step 0 hits on 0 and 24; step 2 restarts the run on 48 rather than adding to it.
    CHECK_EQ(hits_between(kMono, 0, 48), 2);
    CHECK_EQ(fired_on(kMono, 48), 1);
    CHECK(engine.arp_running(kMono));
    // A stop ends a sequenced run: nothing plays from it with the transport stopped.
    post(Cmd::stop, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
    CHECK(!engine.arp_running(kMono));
    const uint8_t before = triggers(kMono);
    for (int b = 0; b < 400; b++) pump();
    CHECK_EQ(int(triggers(kMono)), int(before));
}

TEST(engine_arp_on_a_chord_walks_its_notes_and_a_muted_track_stays_silent) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kPoly);
    put_trig(kPoly, 0);
    set_chord(kPoly, 0, ChordType::major);
    set_length_index(kPoly, 0, 12);              // four steps
    set_arp(kPoly, 1, 3);                        // UP, 1/16
    // One note at a time: the step's first hit starts one voice, not the chord's three, and the
    // four steps' length holds four hits.
    CHECK_EQ(fired_on(kPoly, 0), 1);
    CHECK_EQ(poly().sounding_voices(), 1);
    CHECK_EQ(hits_between(kPoly, 1, 96), 3);
    CHECK_EQ(hits_between(kPoly, kPass, kPass + 96), 4);
    // Muted, the run carries on and plays nothing.
    post(Cmd::set_mute, kPoly, 0, 1, 0);
    CHECK_EQ(hits_between(kPoly, 2 * kPass, 2 * kPass + 96), 0);
    post(Cmd::set_mute, kPoly, 0, 0, 0);
    CHECK(!engine.arp_running(kPoly));
    FakeHost::the().set_running(false);
}

TEST(engine_arp_held_keys_run_on_the_grid_and_stop_on_the_last_key_up) {
    Engine &engine = clean();
    FakeHost::the().set_running(true);
    empty_track(engine, kMono);
    set_arp(kMono, 1, 3);                        // UP, 1/16
    tick_at(100);
    post(Cmd::trigger_note, kMono, 0, 60, 100, 1);
    CHECK_EQ(fired_on(kMono, 101), 1);          // the first key hits at once
    CHECK(engine.arp_running(kMono));
    post(Cmd::trigger_note, kMono, 0, 64, 100, 1);
    // A second key joins the cycle; it does not hit on its own.
    CHECK_EQ(fired_on(kMono, 102), 0);
    CHECK_EQ(hits_between(kMono, 103, 200), 4); // 120, 144, 168, 192
    post(Cmd::release_note, kMono, 0, 60);
    CHECK_EQ(hits_between(kMono, 200, 241), 2); // 216, 240: 64 alone, still running
    post(Cmd::release_note, kMono, 0, 64);
    pump();
    CHECK(!engine.arp_running(kMono));
    CHECK_EQ(hits_between(kMono, 241, 340), 0);

    // A plain pad hit (no release to follow) plays once even with the arp on.
    post(Cmd::trigger_note, kMono, 0, 60, 100, 0);
    CHECK_EQ(fired_on(kMono, 341), 1);
    CHECK(!engine.arp_running(kMono));
    CHECK_EQ(hits_between(kMono, 342, 440), 0);
    FakeHost::the().set_running(false);
}

TEST(engine_arp_held_keys_run_free_while_stopped_and_follow_the_page) {
    Engine &engine = clean();
    FakeHost::the().set_running(false);
    FakeHost::the().set_tempo(120.0f);
    empty_track(engine, kMono);
    set_arp(kMono, 1, 3);                        // 1/16: 6,000 frames at 120 BPM
    const float period = 24.0f * kRate * 60.0f / (120.0f * 96.0f);
    const uint8_t before = triggers(kMono);
    post(Cmd::trigger_note, kMono, 0, 60, 100, 1);
    pump();
    CHECK_EQ(int(uint8_t(triggers(kMono) - before)), 1);
    const int blocks = int(4.0f * period / float(kBlk)) + 1;
    for (int b = 1; b < blocks; b++) pump();
    CHECK_EQ(int(uint8_t(triggers(kMono) - before)), 5);

    // RAT turned while the key is down: 1/32 from here on.
    set_arp(kMono, 1, 5);
    const uint8_t faster = triggers(kMono);
    for (int b = 0; b < blocks; b++) pump();
    const int hits = int(uint8_t(triggers(kMono) - faster));
    CHECK(hits >= 7 && hits <= 9);

    // MOD to OFF ends the run, with the key still down.
    set_arp(kMono, 0, 5);
    pump();
    CHECK(!engine.arp_running(kMono));
    const uint8_t off = triggers(kMono);
    for (int b = 0; b < blocks; b++) pump();
    CHECK_EQ(int(triggers(kMono)), int(off));
    post(Cmd::release_note, kMono, 0, 60);
    pump();
}

TEST(engine_release_note_lets_a_held_note_go_early) {
    // KEYS mode's key release. A pad note on VA POLY with DEC at the top rings for seconds; let
    // go after ten blocks, it is silent long before that.
    int held = -1;
    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 127);
    post(Cmd::trigger_note, kPoly, 0, 60, 100);
    for (int b = 0; b < 8000; b++) {
        pump();
        if (poly().sounding_voices() == 0) {
            held = b;
            break;
        }
    }

    int released = -1;
    clean();
    post(Cmd::set_param, kPoly, 0, kDec, 127);
    post(Cmd::trigger_note, kPoly, 0, 60, 100);
    post(Cmd::trigger_note, kPoly, 0, 67, 100);
    for (int b = 0; b < 10; b++) pump();
    post(Cmd::release_note, kPoly, 0, 60);
    for (int b = 10; b < 8000; b++) {
        pump();
        if (released < 0 && poly().sounding_voices() == 1) released = b;
    }
    CHECK(held > 100);
    CHECK(released > 0);
    CHECK(released < held / 2);
}

TEST(engine_a_command_it_does_not_handle_changes_nothing) {
    // The commands that legitimately do nothing: `none` is the ring's empty slot, a release of a
    // note nothing is holding is the second half of a gesture whose first half never fired, and
    // a command number this build does not have is what an older UI posting to a newer engine
    // would look like. None of them is an error, so none of them is counted as a refusal.
    Engine &engine = clean();
    const uint32_t before = refused();

    post(Cmd::none);
    post(Cmd::release_note, kPoly, 0, 60);
    post(Cmd(200));
    pump(2);

    CHECK(engine.sends() == strata::test::default_sends());
    CHECK(engine.master() == strata::test::default_master());
    CHECK_EQ(int(refused() - before), 0);
}

// FIXED. `Cmd::set_sends` and `Cmd::set_master` were in the enum with NOTHING handling them:
// both fell through `apply`'s default, so every delay and reverb setting -- time, feedback,
// tone, level, decay, damping, high-pass -- and docs/SPEC.md's "Master DJ filter" on F1 + VALUE
// were frozen at their compile-time defaults for the life of the app, unreachable from any
// gesture and absent from every file. This test is the live wire.
TEST(engine_a_send_setting_can_be_changed) {
    Engine &engine = clean();
    CHECK(engine.sends() == strata::test::default_sends());

    // `a` is the field and `b` a delta on the byte PROJECT.BIN stores; the rig's helper turns a
    // target into the one delta that reaches it.
    strata::test::set_send_byte(uint8_t(SendField::delay_feedback), 127);
    strata::test::set_send_byte(uint8_t(SendField::reverb_damping), 0);
    strata::test::set_send_byte(uint8_t(SendField::delay_level), 64);

    CHECK_NEAR(engine.sends().delay_feedback, 0.95f, 1e-6);      // the top of the field's range
    CHECK_NEAR(engine.sends().reverb_damping, 0.0f, 1e-6);
    CHECK_NEAR(engine.sends().delay_level, 64.0f / 127.0f, 1e-6);

    // The byte lands in the PROJECT, which is what makes it survive a save.
    CHECK_EQ(int(engine.project().settings.sends[uint8_t(SendField::delay_feedback)]), 127);

    // The byte CLAMPS at both ends rather than wrapping: a delta past the top of the range on a
    // level knob that wrapped to silence would be the worst possible behaviour on stage.
    post_delta_to(Cmd::set_sends, uint8_t(SendField::delay_feedback), +120);
    pump();
    CHECK_EQ(int(engine.project().settings.sends[uint8_t(SendField::delay_feedback)]), 127);
    post_delta_to(Cmd::set_sends, uint8_t(SendField::delay_feedback), -120);
    pump();
    CHECK_EQ(int(engine.project().settings.sends[uint8_t(SendField::delay_feedback)]), 7);

    // A field index this build does not have is refused and counted, and changes nothing.
    const uint32_t before = refused();
    const SendSettings held = engine.sends();
    post_delta_to(Cmd::set_sends, uint8_t(SendField::count), +10);
    post_delta_to(Cmd::set_sends, 200, +10);
    pump();
    CHECK(engine.sends() == held);
    CHECK_EQ(int(refused() - before), 2);
}

// The delay is TEMPO-SYNCED (docs/SPEC.md), which is why the file stores a DIVISION and not a
// time: the same project at 128 and at 140 BPM has to keep its dotted eighth on the grid. So a
// tempo change has to re-derive the delay, and that is the engine's job even though the clock
// itself belongs to the main context.
TEST(engine_the_delay_follows_the_tempo) {
    Engine &engine = clean();

    post_f(Cmd::set_tempo, 120.0f);                      // a beat is 0.5 s
    pump();
    strata::test::set_send_byte(uint8_t(SendField::delay_seconds), delay_division_byte(6));
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), 6);   // 1/4: one beat
    CHECK_NEAR(engine.sends().delay_seconds, 0.5f, 1e-5);

    post_f(Cmd::set_tempo, 240.0f);                      // half as long
    pump();
    CHECK_NEAR(engine.sends().delay_seconds, 0.25f, 1e-5);

    // And the division itself did not move: it is the time that followed the tempo.
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), 6);

    // A tempo so slow that the division passes the delay line's 2 s is CLAMPED rather than
    // wrapping or reading past the buffer.
    post_f(Cmd::set_tempo, 20.0f);                       // a beat is 3 s
    pump();
    strata::test::set_send_byte(uint8_t(SendField::delay_seconds), delay_division_byte(9));
    // 1/1 is four beats, so 12 s: the classic delay runs at its 2 s, and the field keeps the
    // whole 12 for the LONG mode, whose limit that is.
    CHECK_NEAR(effective_delay_seconds(engine.sends()), 2.0f, 1e-6);
    CHECK_NEAR(engine.sends().delay_seconds, 12.0f, 1e-5);

    // And the DIVISION walk holds at both ends rather than wrapping.
    post_delta_to(Cmd::set_sends, uint8_t(SendField::delay_seconds), +30);
    pump();
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), kDelayDivisions - 1);
    post_delta_to(Cmd::set_sends, uint8_t(SendField::delay_seconds), -30);
    pump();
    CHECK_EQ(delay_division_index(engine.project().settings.sends[0]), 0);

    post_f(Cmd::set_tempo, 130.0f);
    pump();
}

TEST(engine_a_master_setting_can_be_changed) {
    Engine &engine = clean();
    CHECK(engine.master() == strata::test::default_master());

    // The DJ filter is the one signed field: 64 is exactly the flat, bypassed centre, below it a
    // low-pass and above it a high-pass. docs/SPEC.md's F1 + VALUE gesture drives this byte.
    strata::test::set_master_byte(uint8_t(MasterField::dj), 0);
    strata::test::set_master_byte(uint8_t(MasterField::compressor), 32);
    CHECK_NEAR(engine.master().dj, -1.0f, 1e-6);
    CHECK_NEAR(engine.master().compressor, 32.0f / 127.0f, 1e-6);

    strata::test::set_master_byte(uint8_t(MasterField::dj), 64);
    CHECK_NEAR(engine.master().dj, 0.0f, 1e-6);          // exactly flat, not nearly

    strata::test::set_master_byte(uint8_t(MasterField::dj), 127);
    CHECK_NEAR(engine.master().dj, 1.0f, 1e-6);

    // A limiter threshold of zero is a limiter that removes everything, so byte 0 is the floor
    // `resolve_master` honours and not 0.
    strata::test::set_master_byte(uint8_t(MasterField::limit), 0);
    CHECK_NEAR(engine.master().limit, 0.1f, 1e-6);

    const uint32_t before = refused();
    const MasterSettings held = engine.master();
    post_delta_to(Cmd::set_master, uint8_t(MasterField::count), +10);
    pump();
    CHECK(engine.master() == held);
    CHECK_EQ(int(refused() - before), 1);
}

// The settings reach the sound, not just the struct: a long full-feedback delay at full level
// turns one hit into a line that is still running long after the hit itself has gone.
TEST(engine_a_send_setting_reaches_the_sound) {
    clean();

    // 1/32, the shortest division: 58 ms at 130 BPM, so 43 blocks to a repeat. The windows
    // below are 300 blocks -- long enough for the feedback to put several repeats in, which is
    // what makes the two energies differ by a margin rather than by a hair. At 60 blocks they
    // read 1.49x apart and this test failed for want of window, not for want of a delay.
    strata::test::set_send_byte(uint8_t(SendField::delay_seconds), 0);
    strata::test::set_send_byte(uint8_t(SendField::delay_level), 127);
    strata::test::set_send_byte(uint8_t(SendField::delay_feedback), 0);

    post(Cmd::set_param, 0, 0, kSendA, 127);
    post(Cmd::trigger_note, 0, 0, 60, 127);
    const float dry_energy = energy(300);

    // The send gain has to go back to zero before settling: `Mixer::add_track` restarts the
    // delay's tail every block a track with a non-zero send gain is rendered, so the line never
    // stops running and `settle()` -- which wants an exactly silent block -- would never return.
    post(Cmd::set_param, 0, 0, kSendA, 0);
    CHECK(settle());

    strata::test::set_send_byte(uint8_t(SendField::delay_feedback), 127);
    post(Cmd::set_param, 0, 0, kSendA, 127);
    post(Cmd::trigger_note, 0, 0, 60, 127);
    const float wet_energy = energy(300);

    post(Cmd::set_param, 0, 0, kSendA, 0);
    strata::test::set_send_byte(uint8_t(SendField::delay_feedback), 0);
    CHECK(settle());

    // Same hit, same send gain, same delay level: all that changed is the feedback.
    CHECK(wet_energy > dry_energy * 1.5f);
}

TEST(engine_a_command_naming_a_track_that_does_not_exist_is_refused) {
    // FIXED. `apply` used to CLAMP the track and the step out of a command, so an index past the
    // end read as 0: a command naming track 9 edited track 1 and step 64 edited step 1. A UI bug
    // there would corrupt a pattern silently, where every other out-of-range field in that
    // function refuses and counts. This test pinned the clamp; it now pins the refusal.
    Engine &engine = clean();
    const uint32_t refused_before = engine.status().refused.load(std::memory_order_relaxed);

    CHECK(!trig_at(0, 0));
    post(Cmd::toggle_trig, uint8_t(kTracks), 0);
    pump();
    CHECK(!trig_at(0, 0));                         // track 9 did not land on track 1
    CHECK(engine.status().refused.load(std::memory_order_relaxed) > refused_before);

    const uint32_t refused_mid = engine.status().refused.load(std::memory_order_relaxed);
    post(Cmd::toggle_trig, 0, uint8_t(kStepsPerTrack));
    pump();
    CHECK(!trig_at(0, 0));                         // and step 64 did not fold onto step 0
    CHECK(engine.status().refused.load(std::memory_order_relaxed) > refused_mid);

    // A command inside the range still works, so the guard is about the range and not about
    // refusing everything.
    post(Cmd::toggle_trig, uint8_t(kTracks - 1), uint8_t(kStepsPerTrack - 1));
    pump();
    CHECK(trig_at(kTracks - 1, kStepsPerTrack - 1));
}

TEST(engine_the_key_and_the_scale_land_in_both_places) {
    Engine &engine = clean();
    post(Cmd::set_root, 0, 0, 7);
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::phrygian));
    pump();
    CHECK_EQ(int(engine.pattern().root), 7);                   // a pitch class in the pattern ...
    CHECK_EQ(int(engine.project().settings.root), 67);         // ... and a MIDI note in the project
    CHECK_EQ(int(engine.pattern().scale), int(Scale::phrygian));
    CHECK_EQ(int(engine.project().settings.scale), int(Scale::phrygian));

    post(Cmd::set_root, 0, 0, 12);                 // not a pitch class
    pump();
    CHECK_EQ(int(engine.pattern().root), 7);

    // The scale is deliberately NOT range-checked: core/project.cpp leaves the byte unclamped so
    // an id from a later build survives a round trip, and core/scale plays one it does not know
    // chromatically. Clamping here would be the one place that threw it away.
    post(Cmd::set_scale, 0, 0, 200);
    pump();
    CHECK_EQ(int(engine.pattern().scale), 200);
    CHECK_EQ(int(engine.project().settings.scale), 200);
}

TEST(engine_the_key_is_global_and_survives_a_pattern_switch_and_an_undo) {
    // docs/SPEC.md: "Global key and scale". The engine folded with the LIVE PATTERN's copy, so
    // every other pattern played in its zeroed default (C chromatic), the key changed on a
    // pattern switch, and an undo reverted it. It now folds with PROJECT.BIN's.
    Engine &engine = clean();
    post(Cmd::set_root, 0, 0, 2);
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::dorian));
    pump();
    CHECK_EQ(int(engine.key_root()), 2);
    CHECK_EQ(int(engine.key_scale()), int(Scale::dorian));

    post(Cmd::queue_pattern, 0, 0, 0, 9, uint8_t(QueueMode::immediate));   // stopped: at once
    pump();
    CHECK_EQ(live_slot(), 9);
    CHECK_EQ(int(engine.key_root()), 2);
    CHECK_EQ(int(engine.key_scale()), int(Scale::dorian));

    post(Cmd::undo_mark, 0, 5, uint8_t(GestureKind::trig), 1);
    post(Cmd::toggle_trig, 0, 5);
    post(Cmd::set_root, 0, 0, 4);
    pump();
    post(Cmd::undo);
    pump();
    CHECK_EQ(int(engine.key_root()), 4);           // the edit undone, the key kept
}

TEST(engine_clearing_a_sample_bumps_the_epoch_even_for_a_track_that_has_none) {
    Engine &engine = clean();
    // Tracks 1 to 7 host no Sampler in the default kit, which is the case this is about. Track 8
    // is left alone on purpose: clearing ITS sample would drop the placeholder loop for every
    // test that runs after this one, because `build_placeholder_sample` runs once in `init()` and
    // the rig's engine is shared (tests/apprig.hpp). What clearing does to a real Sampler --
    // kill the voices, then drop the pointer -- is dsp/sample.cpp's, and tests/test_sample.cpp
    // has it.
    for (uint8_t t = 0; t + 1 < kTracks; t++) {
        const uint8_t before = engine.status().sample_epoch[t].load(std::memory_order_relaxed);
        post(Cmd::clear_sample, t);
        pump();
        const uint8_t after = engine.status().sample_epoch[t].load(std::memory_order_relaxed);
        // The loader WAITS for this change before it releases the range and writes over it, so a
        // track that was never reading anything must still report that it has stopped, or the
        // wait never ends.
        CHECK_EQ(int(uint8_t(after - before)), 1);
    }

    // A sample goes to the track's Sampler and Grain whatever the track hosts, so a POLY track
    // takes it too. These two carry no frames, which is refused whatever the region; the
    // bounds against the region are tests/test_engine_gaps.cpp's.
    uint32_t before = refused();
    post(Cmd::set_sample, kPoly, 0, 1);
    pump();
    CHECK_EQ(int(refused() - before), 1);
    before = refused();
    post(Cmd::set_sample, 7, 0, 1);
    pump();
    CHECK_EQ(int(refused() - before), 1);
}

// --- prepare only on change ----------------------------------------------------------------------

// app/engine.cpp's prepare memo claims to be bit-exact by construction. This holds it to that:
// two engines, initialised identically, one preparing every block as before and one skipping
// unchanged prepares, fed the same ticks and the same commands -- the default beat with its
// locks and chord, knobs moving mid-note, machine switches, a sample load, a mute, a kit recall
// from the browser and a project load -- must produce the same bits.
//
// Two engines rather than the rig's one rendered twice, because the rig's engine carries state
// between renders (oscillator phases, noise generators, the delay line) that no command resets,
// so the same passage rendered twice on it does not hash the same even with nothing changed.
TEST(engine_skipping_an_unchanged_prepare_is_bit_exact) {
    static Engine *every_block = nullptr;
    static Engine *on_change = nullptr;
    if (every_block == nullptr) {
        every_block = new Engine;
        on_change = new Engine;
        FakeHost::the().reset();
        FakeHost::the().set_tempo(130.0f);
        CHECK(every_block->init(kRate));
        CHECK(on_change->init(kRate));
        // A sample region of their own, so a real set_sample can land (the rig has none): one
        // second of a decaying saw, longer than the 0.4 s placeholder every machine starts with.
        static int16_t region[48000];
        for (int i = 0; i < 48000; i++) {
            region[i] = int16_t((i % 211) * 150 - 15000) * int16_t(1 + (48000 - i) / 12000) / 4;
        }
        every_block->set_sample_region(region, sizeof region);
        on_change->set_sample_region(region, sizeof region);
    }
    every_block->set_prepare_memo(false);
    on_change->set_prepare_memo(true);
    Engine *const engines[2] = {every_block, on_change};

    auto post_both = [&](Cmd cmd, uint8_t track, uint8_t step, uint8_t a, uint8_t b) {
        Command command;
        command.cmd = cmd;
        command.track = track;
        command.step = step;
        command.a = a;
        command.b = b;
        for (Engine *e : engines) CHECK(e->commands().post(command));
    };
    post_both(Cmd::set_param, 0, 0, kFilterMod, 40);      // a filter on two tracks, so the
    post_both(Cmd::set_param, kMono, 0, kFilterMod, 100); // chain's coefficients count too
    post_both(Cmd::set_param, 1, 0, kSendA, 90);
    post_both(Cmd::set_param, 2, 0, kSendB, 90);
    post_both(Cmd::lock_delta, kPoly, 4, kDec, uint8_t(int8_t(-40)));
    post_both(Cmd::lock_delta, 0, 8, kDrv, uint8_t(int8_t(60)));

    // A kit for the browser's recall: the live one with a different machine on two tracks and
    // other knobs, so the recall moves machines, parameters and faders at once.
    auto stage_recall = [&]() {
        for (Engine *e : engines) {
            Kit &kit = e->kit_for_recall();
            kit = e->project().kit;
            kit.tracks[4].machine = uint8_t(MachineId::va_mono);
            kit.tracks[5].params[2] = 20;       // VA CUT
            kit.tracks[1].params[8] = 110;      // SNARE DEC
            kit.tracks[1].level = 0.6f;
        }
    };

    FakeHost::the().set_running(true);
    const double frames_per_tick = double(kRate) * 60.0 / (130.0 * 96.0);
    double next = 0.0;
    uint32_t tick = 0;
    float out[2][kStereo];
    float peak = 0.0f;
    int differing = 0;
    // One bar is about 1,385 blocks at 130 BPM, so each event below has notes around it.
    for (int b = 0; b < 6000; b++) {
        if (b == 300) post_both(Cmd::set_param, kMono, 0, kDec, 100);     // a knob, mid-note
        if (b == 500) post_both(Cmd::set_machine, 4, 0, uint8_t(MachineId::va_poly), 0);
        if (b == 700) post_both(Cmd::set_param, kPoly, 0, kLvl, 90);
        // GRAIN on the loop track with SPR at the top, prepared against the 0.4 s placeholder,
        // which clamps the spray to the sample's length. A longer sample later lifts the clamp
        // only if the machine is prepared again: an input to prepare() that is not a parameter,
        // which is the case the memo's reset on every command exists for.
        if (b == 900) post_both(Cmd::set_machine, 7, 0, uint8_t(MachineId::grain), 0);
        if (b == 910) post_both(Cmd::set_param, 7, 0, 3, 127);
        if (b == 2600) {
            Command load;
            load.cmd = Cmd::set_sample;
            load.track = 7;
            load.a = 1;
            load.u = 0;
            load.c = uint8_t(48000 & 0xFF);
            load.d = uint8_t((48000 >> 8) & 0xFF);
            load.e = uint8_t(48000 >> 16);
            for (Engine *e : engines) CHECK(e->commands().post(load));
        }
        if (b == 3400) post_both(Cmd::set_mute, 3, 0, 1, 0);
        if (b == 3600) {
            stage_recall();
            post_both(Cmd::kit_recall, 0, 0, 1, 0);       // the kit browser
        }
        if (b == 4400) post_both(Cmd::project_loaded, 0, 0, 0, 0);   // the live project, again
        if (b == 5000) post_both(Cmd::set_machine, 7, 0, uint8_t(MachineId::sample), 0);
        const double block_start = double(b * kBlk);
        for (int e = 0; e < 2; e++) {
            // The fake hands its tick queue out once, so each engine is given the block's ticks.
            for (double t = next; t < block_start + double(kBlk); t += frames_per_tick) {
                FakeHost::the().deliver_tick(tick + uint32_t((t - next) / frames_per_tick + 0.5),
                                             uint32_t(t - block_start));
            }
            engines[e]->process(nullptr, out[e], uint32_t(kBlk));
        }
        while (next < block_start + double(kBlk)) {
            next += frames_per_tick;
            tick++;
        }
        for (int i = 0; i < kStereo; i++) {
            if (__builtin_memcmp(&out[0][i], &out[1][i], sizeof(float)) != 0) differing++;
        }
        const float p = peak_of(out[0], kStereo);
        if (p > peak) peak = p;
    }
    FakeHost::the().set_running(false);

    CHECK_EQ(differing, 0);
    // Every command landed, the sample included: a refused set_sample would test nothing.
    CHECK_EQ(int(on_change->status().refused.load(std::memory_order_relaxed)), 0);
    // And the passage really sounds: two silent renders would agree whatever the memo did.
    CHECK(peak > 0.1f);
}

// --- the shared voice budget ---------------------------------------------------------------------

namespace {

int poly_voices_counted() {
    int n = 0;
    for (int t = 0; t < kTracks; t++) {
        const Machine *m = strata::test::engine().machine(t);
        const uint8_t id = strata::test::engine().project().kit.tracks[t].machine;
        if (m != nullptr && (id == uint8_t(MachineId::va_poly) || id == uint8_t(MachineId::wave_poly))) {
            n += static_cast<const VaPoly *>(m)->budget_voices();
        }
    }
    return n;
}

}  // namespace

// A note that takes over a voice still fading from a choke is a voice from the moment it takes
// it, like any other pending trig. It used to go uncounted until its frame came, because a
// choking voice is not counted, so a later note in the same block found room that was not there
// and the budget ended the block one voice over. The case is an ordinary one: a chord repeated
// on its next step while a third POLY track has just choked one of its notes.
TEST(engine_a_note_on_a_choking_voice_counts_against_the_budget_at_once) {
    clean();
    const uint8_t tracks[3] = {2, 3, 4};
    uint8_t was[3];
    for (int i = 0; i < 3; i++) {
        was[i] = strata::test::engine().project().kit.tracks[tracks[i]].machine;
        post(Cmd::set_machine, tracks[i], 0, uint8_t(MachineId::va_poly));
        post(Cmd::set_param, tracks[i], 0, kDec, 127);
    }
    pump();

    // Two full chords, started: the budget is spent.
    for (int i = 0; i < 2; i++) {
        for (int n = 0; n < VaPoly::kVoices; n++) {
            post(Cmd::trigger_note, tracks[i], 0, uint8_t(48 + 2 * n), 100);
        }
    }
    pump(4);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);

    // One block: a note on the third track chokes a voice of the first; the first track
    // repeats that voice's note, which takes the fading voice back; and the third track plays
    // one more note.
    post(Cmd::trigger_note, tracks[2], 0, 72, 100);
    for (int n = 0; n < VaPoly::kVoices; n++) {
        post(Cmd::trigger_note, tracks[0], 0, uint8_t(48 + 2 * n), 100);
    }
    post(Cmd::trigger_note, tracks[2], 0, 74, 100);
    pump();
    CHECK(poly_voices_counted() <= Engine::kPolyVoiceBudget);
    pump(4);                                   // every note has started, every fade is over
    int sounding = 0;
    for (int i = 0; i < 3; i++) {
        sounding += static_cast<const VaPoly *>(strata::test::engine().machine(tracks[i]))
                        ->sounding_voices();
    }
    CHECK(sounding <= Engine::kPolyVoiceBudget);
    CHECK_EQ(poly_voices_counted(), sounding);

    for (int i = 0; i < 3; i++) post(Cmd::set_machine, tracks[i], 0, was[i]);
    pump();
}

// WAVE POLY is VA POLY with a wavetable, at the same cost a voice, so it spends the same budget:
// two of each, asking for 24 voices, get twelve between them.
TEST(engine_wave_poly_shares_the_poly_voice_budget) {
    clean();
    const uint8_t tracks[4] = {2, 3, 4, kPoly};
    uint8_t was[4];
    for (int i = 0; i < 4; i++) {
        was[i] = strata::test::engine().project().kit.tracks[tracks[i]].machine;
        post(Cmd::set_machine, tracks[i], 0,
             uint8_t(i % 2 == 0 ? MachineId::wave_poly : MachineId::va_poly));
        post(Cmd::set_param, tracks[i], 0, kDec, 127);
    }
    pump();
    CHECK(static_cast<const VaPoly *>(strata::test::engine().machine(tracks[0]))->wave());
    CHECK(!static_cast<const VaPoly *>(strata::test::engine().machine(tracks[1]))->wave());
    for (int i = 0; i < 4; i++) {
        for (int n = 0; n < VaPoly::kVoices; n++) {
            post(Cmd::trigger_note, tracks[i], 0, uint8_t(48 + 2 * n), 100);
        }
    }
    pump(8);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);
    for (int i = 0; i < 4; i++) post(Cmd::set_machine, tracks[i], 0, was[i]);
    pump();
}

TEST(engine_the_shared_voice_budget_caps_va_poly_across_tracks) {
    clean();
    // Four POLY tracks with long notes, so nothing ends on its own during the test.
    const uint8_t tracks[4] = {2, 3, 4, kPoly};
    uint8_t was[4];
    for (int i = 0; i < 4; i++) {
        was[i] = strata::test::engine().project().kit.tracks[tracks[i]].machine;
        post(Cmd::set_machine, tracks[i], 0, uint8_t(MachineId::va_poly));
        post(Cmd::set_param, tracks[i], 0, kDec, 127);
    }
    pump();
    CHECK_EQ(poly_voices_counted(), 0);

    // Six notes on each of the four tracks in one block: 24 asked for. The first twelve take
    // the budget; the other twelve find every counted voice still waiting for its frame, so
    // there is nothing to fade, and they are dropped rather than let the total grow.
    for (int i = 0; i < 4; i++) {
        for (int n = 0; n < VaPoly::kVoices; n++) {
            post(Cmd::trigger_note, tracks[i], 0, uint8_t(48 + 2 * n), 100);
        }
    }
    pump();
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);
    pump(20);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);

    // Now everything counted has started. Six more notes on a track with none sounding each
    // fade out the quietest voice elsewhere, so they sound and the total stays at the budget.
    const VaPoly &last = *static_cast<const VaPoly *>(strata::test::engine().machine(kPoly));
    CHECK_EQ(last.busy_voices(), 0);
    for (int n = 0; n < VaPoly::kVoices; n++) {
        post(Cmd::trigger_note, kPoly, 0, uint8_t(72 + n), 100);
    }
    pump();
    CHECK_EQ(last.busy_voices(), VaPoly::kVoices);
    CHECK(poly_voices_counted() <= Engine::kPolyVoiceBudget);
    pump(2);                                   // the fades (64 frames) are over
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);
    int sounding = 0;
    for (int i = 0; i < 4; i++) {
        sounding += static_cast<const VaPoly *>(strata::test::engine().machine(tracks[i]))
                        ->sounding_voices();
    }
    CHECK_EQ(sounding, Engine::kPolyVoiceBudget);   // the faded ones really stopped

    // A retrigger of a note already sounding costs nothing extra and chokes nothing.
    post(Cmd::trigger_note, kPoly, 0, 72, 100);
    pump();
    CHECK_EQ(last.busy_voices(), VaPoly::kVoices);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);

    for (int i = 0; i < 4; i++) post(Cmd::set_machine, tracks[i], 0, was[i]);
    pump();
}

// --- the CPU guard (core/cpuguard.hpp) -----------------------------------------------------------

namespace {

const VaPoly &poly_on(int track) {
    return *static_cast<const VaPoly *>(strata::test::engine().machine(track));
}

}  // namespace

// Load near the block's end lowers the shared POLY budget and sheds the quietest voices down to
// it, a fade at a time; new notes then meet the lower budget; and a hard stop puts it all back.
TEST(engine_the_cpu_guard_sheds_poly_voices_under_load_and_a_hard_stop_resets_it) {
    Engine &engine = clean();
    FakeHost &host = FakeHost::the();
    const uint8_t tracks[3] = {2, 3, 4};
    uint8_t was[3];
    for (int i = 0; i < 3; i++) {
        was[i] = engine.project().kit.tracks[tracks[i]].machine;
        post(Cmd::set_machine, tracks[i], 0, uint8_t(MachineId::va_poly));
        post(Cmd::set_param, tracks[i], 0, kDec, 127);
    }
    pump();
    for (int i = 0; i < 2; i++) {
        for (int n = 0; n < VaPoly::kVoices; n++) {
            post(Cmd::trigger_note, tracks[i], 0, uint8_t(48 + 2 * n), 100);
        }
    }
    pump(4);
    CHECK_EQ(engine.cpu_guard_level(), 0);
    CHECK_EQ(engine.poly_budget(), Engine::kPolyVoiceBudget);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget);

    // One panic block after another until the guard rises; then an ordinary load, so it holds.
    host.set_audio_load(CpuGuard::kPanic + 50);
    int blocks = 0;
    while (engine.cpu_guard_level() == 0 && blocks < 40) {
        pump();
        blocks++;
    }
    host.set_audio_load(600);
    CHECK(blocks <= int(CpuGuard::kPanicSettle) + 1);
    CHECK_EQ(engine.cpu_guard_level(), 1);
    CHECK_EQ(engine.poly_budget(), Engine::kPolyVoiceBudget - 2);
    CHECK_EQ(int(engine.status().cpu_guard.load(std::memory_order_relaxed)), 1);

    // One voice a block is choked, then the fades end: ten left, and they really stopped.
    pump(6);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget - 2);
    int sounding = 0;
    for (int i = 0; i < 3; i++) sounding += poly_on(tracks[i]).sounding_voices();
    CHECK_EQ(sounding, Engine::kPolyVoiceBudget - 2);

    // A new note meets the lower budget: it sounds, and something quieter makes room.
    post(Cmd::trigger_note, tracks[2], 0, 80, 100);
    pump(4);
    CHECK_EQ(poly_on(tracks[2]).busy_voices(), 1);
    CHECK_EQ(poly_voices_counted(), Engine::kPolyVoiceBudget - 2);
    CHECK_EQ(engine.cpu_guard_level(), 1);

    // A hard stop: nothing sounds, so the guard starts over with the whole budget.
    post(Cmd::stop, 0, 0, 1);
    pump();
    CHECK_EQ(engine.cpu_guard_level(), 0);
    CHECK_EQ(engine.poly_budget(), Engine::kPolyVoiceBudget);
    CHECK_EQ(int(engine.status().cpu_guard.load(std::memory_order_relaxed)), 0);

    host.set_audio_load(0);
    for (int i = 0; i < 3; i++) post(Cmd::set_machine, tracks[i], 0, was[i]);
    pump();
}

// The guard's GRAIN half: every track's pool follows the level, so a GRAIN track switched in
// while the guard is up starts with the smaller pool too, and level 0 gives the whole pool back.
TEST(engine_the_cpu_guard_shrinks_every_grain_pool_and_gives_it_back) {
    Engine &engine = clean();
    FakeHost &host = FakeHost::the();
    const uint8_t was = engine.project().kit.tracks[kPoly].machine;
    post(Cmd::set_machine, kPoly, 0, uint8_t(MachineId::grain));
    pump();
    const Granular &grain = *static_cast<const Granular *>(engine.machine(kPoly));
    CHECK_EQ(grain.grain_limit(), Granular::kGrains);

    host.set_audio_load(CpuGuard::kPanic);
    int blocks = 0;
    while (engine.cpu_guard_level() < 2 && blocks < 80) {
        pump();
        blocks++;
    }
    host.set_audio_load(600);
    CHECK_EQ(engine.cpu_guard_level(), 2);
    CHECK_EQ(grain.grain_limit(), CpuGuard::grain_limit(2, Granular::kGrains));

    // Cool: one level a second, down to the whole pool.
    host.set_audio_load(100);
    pump(int(CpuGuard::kFallBlocks) * 2 + 20);
    CHECK_EQ(engine.cpu_guard_level(), 0);
    CHECK_EQ(grain.grain_limit(), Granular::kGrains);
    CHECK_EQ(engine.poly_budget(), Engine::kPolyVoiceBudget);

    host.set_audio_load(0);
    post(Cmd::set_machine, kPoly, 0, was);
    pump();
}

// A lock lasts exactly as long as its step, and a stopped transport has no step playing. The
// mask of locked parameters used to outlive the stop, so the performance layer stayed shut out
// of every parameter the last step had locked: with the transport stopped, a macro (or the
// morph) on such a parameter moved the screen and not the sound, and a pad played the old
// step's lock. Stopping ends the step, so it ends its locks.
TEST(engine_a_stop_ends_the_last_steps_locks_so_a_macro_reaches_them) {
    clean();
    FakeHost &host = FakeHost::the();
    post(Cmd::set_param, 0, 0, kLvl, 100);
    put_trig(0, 0);

    host.set_running(true);
    host.deliver_tick(0, 0);
    pump();
    // A lock taking the level to 0 on the step that is playing: heard at once, and masked.
    post(Cmd::lock_delta, 0, 0, kLvl, uint8_t(int8_t(-100)));
    pump();

    host.set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump();
    CHECK(settle());

    // Macro B takes the level from the kit's 100 to 127.
    post_macro_assign(1, 0, kLvl, 100, 127);
    post(Cmd::set_macro, 0, 0, 1, 127);
    pump();
    CHECK_EQ(int(strata::test::engine().performed(0, kLvl)), 127);

    // A pad on the track: it sounds at the macro's level, not at the dead step's lock of 0.
    post(Cmd::trigger_note, 0, 0, 60, 100);
    CHECK(energy(20) > 0.01f);

    post(Cmd::macro_clear, 0, 0, 1);
    post(Cmd::set_macro, 0, 0, 1, 0);
    pump();
}

// --- the master kill EQ through the performance layer ------------------------------------------

// A macro on an EQ band sweeps it from the knob towards the far end, and at 0 lets go; the kill
// switches stay the panel's whatever the macro does.
TEST(engine_a_macro_on_the_master_eq_sweeps_a_band_and_lets_go) {
    Engine &engine = clean();
    strata::test::set_master_byte(uint8_t(MasterField::eq_low), 64);
    post_macro_assign(3, 0, uint8_t(kTrackParams + 0), 64, 0);         // LOW, towards a kill
    pump();
    CHECK_EQ(macro_count(engine.macros().macros[3]), 1);
    CHECK_EQ(int(engine.macros().macros[3].targets[0].track), int(kMacroMasterTrack));
    CHECK_EQ(engine.master().eq_low, 1.0f);

    post(Cmd::set_macro, 0, 0, 3, 127);
    pump();
    CHECK_EQ(int(engine.performed_master(0)), 0);
    CHECK_EQ(engine.master().eq_low, 0.0f);
    CHECK_EQ(engine.master().eq_mid, 1.0f);
    // The knob underneath is unchanged: a macro is a hand on top, not an edit.
    CHECK_EQ(int(engine.project().settings.master[int(MasterField::eq_low)]), 64);

    post(Cmd::set_macro, 0, 0, 3, 0);
    pump();
    CHECK_EQ(engine.master().eq_low, 1.0f);

    // Turning the knob with the macro at rest moves the band, as for a track parameter.
    strata::test::set_master_byte(uint8_t(MasterField::eq_low), 32);
    pump();
    CHECK_EQ(int(engine.performed_master(0)), 32);
    CHECK_NEAR(engine.master().eq_low, 0.25f, 1e-6);

    post(Cmd::macro_clear, 0, 0, 3);
    strata::test::set_master_byte(uint8_t(MasterField::eq_low), 64);
    pump();
}

// A whole capture stores the EQ's knobs; morphing to it sweeps them. A one-track capture does not.
TEST(engine_a_scene_captures_and_morphs_the_master_eq) {
    Engine &engine = clean();
    post(Cmd::scene_erase, 0, 0, 0);
    post(Cmd::scene_erase, 0, 0, 1);
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);
    strata::test::set_master_byte(uint8_t(MasterField::eq_high), 0);    // HI cut in scene 1
    post(Cmd::scene_capture, 0, 0, 1, 0);
    pump();
    CHECK_EQ(int(engine.scenes().scenes[1].master_stored), 0x7);
    CHECK_EQ(int(engine.scenes().scenes[1].master[2]), 0);

    strata::test::set_master_byte(uint8_t(MasterField::eq_high), 64);   // the knob back to flat
    Command morph;
    morph.cmd = Cmd::set_morph;
    morph.u = kMorphScale;
    strata::test::engine().commands().post(morph);
    pump();
    CHECK_EQ(int(engine.performed_master(2)), 0);
    CHECK_EQ(engine.master().eq_high, 0.0f);

    morph.u = 0;
    strata::test::engine().commands().post(morph);
    pump();
    CHECK_EQ(engine.master().eq_high, 1.0f);

    // One track's capture leaves the EQ out of the scene.
    post(Cmd::scene_capture, 2, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.scenes().scenes[0].master_stored), 0);

    post(Cmd::scene_erase, 0, 0, 0);
    post(Cmd::scene_erase, 0, 0, 1);
    pump();
}

// --- the ASR page: a sequenced note held for its LEN -----------------------------------------

TEST(engine_with_sus_up_a_chord_step_holds_for_its_len_and_not_its_dec) {
    // DEC short, SUS up, LEN 16 steps (a bar): the note sounds for the bar, not for DEC. With SUS
    // at 0 the same step is over in a fraction of that, as it always was.
    const float spt = kRate * 60.0f / (120.0f * float(enaga::clock::ppqn));
    const float bar = length_samples(16, ticks_per_step(Speed::x1), spt);
    int frames[2];
    for (int sus = 0; sus < 2; sus++) {
        clean();
        FakeHost::the().set_tempo(120.0f);
        post(Cmd::set_param, kPoly, 0, kDec, 20);
        post(Cmd::set_param, kPoly, 0, uint8_t(kAsrFirst + 1), uint8_t(sus ? 100 : 0));
        put_trig(kPoly, 0);
        set_length_index(kPoly, 0, 16);
        frames[sus] = frames_until_silent();
    }
    CHECK(frames[0] > 0 && frames[0] < int(bar * 0.25f));
    CHECK(float(frames[1]) > bar);
    CHECK(float(frames[1]) < bar + kRate * 0.2f);     // LEN, then the 60 ms release
}

TEST(engine_a_lock_on_an_asr_byte_reaches_the_machine) {
    // The VAs read the ASR page in `prepare`, and the prepare memo skips a prepare when the
    // bytes it compares have not moved. A lock arrives with no command to clear the memo, so the
    // memo itself has to compare the ASR bytes -- or a step locking SUS up would do nothing. A
    // trigless step 1 locks SUS under a note step 0 started.
    Engine &engine = clean();
    post(Cmd::set_param, kMono, 0, kDec, 127);     // a note still ringing at step 1
    put_trig(kMono, 0);
    post(Cmd::toggle_trig, kMono, 1, 1);           // trigless
    post(Cmd::lock_delta, kMono, 1, uint8_t(kAsrFirst + 1), 100);
    pump();
    uint8_t locked = 0;
    CHECK(pattern_get_lock(engine.pattern(), kMono, 1, uint8_t(kAsrFirst + 1), locked));
    CHECK(locked > 90);

    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump(4);
    CHECK_EQ(mono().sustain_level(), 0.0f);
    FakeHost::the().deliver_tick(24, 0);
    pump(2);
    CHECK(mono().sustain_level() > 0.7f);
}

TEST(engine_with_sus_up_a_pad_hit_lasts_a_step_and_a_held_key_until_it_comes_up) {
    // A performance pad hit has no release coming, so on a held-note envelope it must not hold
    // for ever: one step, as its MIDI note gets. A KEYS key (c = 1) holds until release_note.
    clean();
    FakeHost::the().set_tempo(120.0f);
    post(Cmd::set_param, kPoly, 0, kDec, 20);
    post(Cmd::set_param, kPoly, 0, uint8_t(kAsrFirst + 1), 100);
    pump();
    const float spt = kRate * 60.0f / (120.0f * float(enaga::clock::ppqn));
    const float step = length_samples(8, ticks_per_step(Speed::x1), spt);

    post(Cmd::trigger_note, kPoly, 0, 60, 100);              // a pad hit
    pump();
    CHECK_EQ(poly().sounding_voices(), 1);
    int blocks = 1;
    while (poly().sounding_voices() > 0 && blocks < 4000) {
        pump();
        blocks++;
    }
    CHECK(float(blocks * kBlk) > step);
    CHECK(float(blocks * kBlk) < step + kRate * 0.2f);

    post(Cmd::trigger_note, kPoly, 0, 64, 100, 1);           // a held key
    pump(int(kRate * 2.0f) / kBlk);                          // two seconds held
    CHECK_EQ(poly().sounding_voices(), 1);
    post(Cmd::release_note, kPoly, 0, 64);
    pump(int(kRate * 0.3f) / kBlk);
    CHECK_EQ(poly().sounding_voices(), 0);
}

// --- the sidechain duck, editable since it moved into the kit ------------------------------------

TEST(engine_the_duck_starts_as_v0_1_fixed_it_and_set_duck_moves_every_field) {
    Engine &engine = fresh();
    // The factory duck: track 1 fires it, every other track ducks exactly 0.35 -- the constant it
    // replaced, so an old project renders as it did -- and the source never ducks itself.
    CHECK_EQ(engine.duck_source(), 0);
    CHECK_EQ(engine.duck_depth(0), 0.0f);
    for (int t = 1; t < kTracks; t++) CHECK_EQ(engine.duck_depth(t), 0.35f);
    {
        // And the envelope is the one v0.1.0 set with literals, sample for sample.
        Duck now = engine.mixer().duck();
        Duck was;
        was.init(kRate);
        was.set(0.003f, 0.040f, 0.120f);
        now.trigger(0);
        was.trigger(0);
        constexpr int kRigBlock = strata::test::kRigBlock;
        static float a[kRigBlock], b[kRigBlock];
        int differ = 0;
        for (int block = 0; block < 200; block++) {
            now.render(a, kRigBlock);
            was.render(b, kRigBlock);
            for (int i = 0; i < kRigBlock; i++) differ += a[i] != b[i];
        }
        CHECK_EQ(differ, 0);
    }

    // The source moves, and the depths follow it: track 1 now ducks at its own 35%, track 3 not.
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::source), 2);
    pump();
    CHECK_EQ(engine.duck_source(), 2);
    CHECK_EQ(int(engine.project().kit.duck.source), 2);
    CHECK_EQ(engine.duck_depth(0), 0.35f);
    CHECK_EQ(engine.duck_depth(2), 0.0f);
    // And it stops at the last track rather than wrapping.
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::source), 20);
    pump();
    CHECK_EQ(engine.duck_source(), kTracks - 1);
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::source), -20);
    pump();
    CHECK_EQ(engine.duck_source(), 0);

    // A depth per track, stopping at 0 (off) and 100%.
    strata::test::post_delta(Cmd::set_duck, 3, 0, uint8_t(DuckField::depth), -50);
    strata::test::post_delta(Cmd::set_duck, 4, 0, uint8_t(DuckField::depth), 100);
    strata::test::post_delta(Cmd::set_duck, 5, 0, uint8_t(DuckField::depth), 5);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[3].duck), 0);
    CHECK_EQ(engine.duck_depth(3), 0.0f);
    CHECK_EQ(engine.duck_depth(4), 1.0f);
    CHECK_EQ(engine.duck_depth(5), 0.4f);
    CHECK_EQ(engine.duck_depth(6), 0.35f);                  // the others untouched

    // The times, each stopping at its ends.
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::attack), -10);
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::hold), 120);
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::release), -100);
    pump();
    CHECK_EQ(int(engine.project().kit.duck.attack_ms), int(kDuckAttackMin));
    CHECK_EQ(int(engine.project().kit.duck.hold), int(kDuckHoldMax));
    CHECK_EQ(int(engine.project().kit.duck.release), int(kDuckReleaseMin));
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::release), 120);
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::release), 120);
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::attack), 120);
    pump();
    CHECK_EQ(int(engine.project().kit.duck.release), int(kDuckReleaseMax));
    CHECK_EQ(int(engine.project().kit.duck.attack_ms), int(kDuckAttackMax));

    fresh();
    CHECK_EQ(engine.duck_source(), 0);                      // a new project is the factory duck
    CHECK_EQ(engine.duck_depth(4), 0.35f);
}

TEST(engine_the_duck_fires_from_its_source_track_and_from_no_other) {
    Engine &engine = fresh();
    FakeHost::the().set_running(true);

    // Track 1 by default: its trig fires the duck.
    put_trig(0, 0);
    tick_at(0);
    pump();
    CHECK(engine.mixer().duck().active());

    // Moved to track 3, track 1's trig no longer does, and track 3's does.
    FakeHost::the().set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump(400);                                              // past the 120 ms release
    CHECK(!engine.mixer().duck().active());
    strata::test::post_delta(Cmd::set_duck, 0, 0, uint8_t(DuckField::source), 2);
    pump();
    FakeHost::the().set_running(true);
    tick_at(0);
    pump();
    CHECK(!engine.mixer().duck().active());

    put_trig(2, 4);
    tick_at(4 * 24);
    pump();
    CHECK(engine.mixer().duck().active());

    FakeHost::the().set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump(2);
    CHECK(settle());
}

TEST(engine_a_recalled_kit_brings_its_duck) {
    Engine &engine = fresh();
    Kit &inbox = engine.kit_for_recall();
    inbox = engine.project().kit;
    inbox.duck.source = 4;
    inbox.duck.release = 60;
    inbox.tracks[1].duck = 80;
    post(Cmd::kit_recall, 0, 0, 1);
    pump();
    CHECK_EQ(engine.duck_source(), 4);
    CHECK_EQ(int(engine.project().kit.duck.release), 60);
    CHECK_EQ(engine.duck_depth(1), 0.8f);
    CHECK_EQ(engine.duck_depth(4), 0.0f);
    fresh();
}
