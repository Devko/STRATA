// app/engine.cpp, the gaps a systematic mutation run found.
//
// tools/mutation_test.py mutated every line of app/engine.cpp one operator at a time and ran the
// whole suite against each mutant. Each test below is named for the behaviour that nothing was
// holding: a clamp nobody pushed against, a refusal nobody counted, a reset nobody looked at
// afterwards. The mutants that SURVIVE on purpose -- the ones that change no behaviour -- are
// argued where they live, in app/engine.cpp, and summarised in docs/MUTATION.md.
//
// Every test drives the engine through the command ring, as the UI does, and reads back what
// the engine publishes. `project_for_load()` is used only to put a step into a state no command
// can reach (a velocity of 0, an invalid length), which is what a damaged or hand-made file
// does -- and it is used, as the header requires, with the transport stopped.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include "pattern.hpp"
#include "queue.hpp"
#include "samplepool.hpp"
#include "grain.hpp"
#include "sample.hpp"
#include "vapoly.hpp"

#include <enaga/abi.h>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

namespace {

constexpr uint8_t kPoly = 6;     // CHORD, VA POLY: the machine that reports its prepared DEC
constexpr uint8_t kDec = 8;      // AMP slot 0
constexpr int kRigBlock = strata::test::kRigBlock;

Engine &eng() { return strata::test::engine(); }

uint32_t refused() { return eng().status().refused.load(std::memory_order_relaxed); }

uint8_t triggers(int track) {
    return eng().status().triggered[track].load(std::memory_order_relaxed);
}

int playhead(int track) {
    return int(eng().status().playhead[track].load(std::memory_order_relaxed));
}

const Step &step_at(int track, int step) { return eng().pattern().tracks[track].steps[step]; }

// The live step, writable. Only with the transport stopped (engine.hpp, project_for_load).
Step &raw_step(int track, int step) {
    return eng().project_for_load().live().tracks[track].steps[step];
}

const VaPoly &poly() { return *static_cast<const VaPoly *>(eng().machine(kPoly)); }

void post_full(Cmd cmd, uint8_t track, uint8_t step, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
               uint32_t u = 0) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.step = step;
    command.a = a;
    command.b = b;
    command.c = c;
    command.d = d;
    command.u = u;
    eng().commands().post(command);
}

void chain_edit(ChainEdit op, uint8_t at, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) {
    post_full(Cmd::chain_edit, 0, at, uint8_t(op), b, c, d);
    pump();
}

}  // namespace

// --- step edits --------------------------------------------------------------------------------

TEST(gaps_a_toggled_trig_gets_a_playable_velocity_and_length_and_keeps_its_own) {
    // `toggle_trig` repairs a step that a file left with a velocity of 0 or a length the table
    // does not have -- but only when it turns the trig ON, and only those two fields.
    fresh();
    Step &bad = raw_step(0, 3);
    bad.flags = 0;
    bad.velocity = 0;
    bad.length = 0;                       // index 0 is the table's invalid sentinel
    post(Cmd::toggle_trig, 0, 3);
    pump();
    CHECK((step_at(0, 3).flags & kStepTrig) != 0);
    CHECK_EQ(int(step_at(0, 3).velocity), 100);
    CHECK_EQ(int(step_at(0, 3).length), 8);

    // A step that already has a velocity and a length keeps them.
    Step &good = raw_step(0, 4);
    good.flags = 0;
    good.velocity = 77;
    good.length = 3;
    post(Cmd::toggle_trig, 0, 4);
    pump();
    CHECK_EQ(int(step_at(0, 4).velocity), 77);
    CHECK_EQ(int(step_at(0, 4).length), 3);

    // Turning a trig OFF repairs nothing: the step is not going to play.
    Step &off = raw_step(0, 5);
    off.flags = kStepTrig;
    off.velocity = 0;
    off.length = 0;
    post(Cmd::toggle_trig, 0, 5);
    pump();
    CHECK((step_at(0, 5).flags & kStepTrig) == 0);
    CHECK_EQ(int(step_at(0, 5).velocity), 0);
    CHECK_EQ(int(step_at(0, 5).length), 0);
}

TEST(gaps_a_recorded_hit_writes_every_field_and_clamps_its_velocity) {
    fresh();
    Step &s = raw_step(1, 6);
    s.flags = kStepTrigless;
    s.length = 0;
    post(Cmd::record_hit, 1, 6, 61, 90, uint8_t(int8_t(-5)));
    pump();
    CHECK_EQ(int(step_at(1, 6).flags & (kStepTrig | kStepTrigless)), int(kStepTrig));
    CHECK_EQ(int(step_at(1, 6).note), 61);
    CHECK_EQ(int(step_at(1, 6).velocity), 90);
    CHECK_EQ(int(step_at(1, 6).micro), -5);
    CHECK_EQ(int(step_at(1, 6).length), 8);            // repaired

    // A length the step already had is the step's, not the hit's.
    raw_step(1, 7).length = 3;
    post(Cmd::record_hit, 1, 7, 60, 0, 0);              // velocity 0: the quietest that sounds
    pump();
    CHECK_EQ(int(step_at(1, 7).velocity), 1);
    CHECK_EQ(int(step_at(1, 7).length), 3);
    post(Cmd::record_hit, 1, 8, 60, 200, 0);            // past the top: clamped
    pump();
    CHECK_EQ(int(step_at(1, 8).velocity), 127);
    post(Cmd::record_hit, 1, 9, 60, 127, 0);
    pump();
    CHECK_EQ(int(step_at(1, 9).velocity), 127);
}

TEST(gaps_a_lock_turned_on_the_playing_step_is_heard_now_and_on_another_step_is_not) {
    // `lock_delta` on the step under the playhead goes into `effective_` and into the locked
    // mask, so the render loop prepares the machine with it in the very next block. VA POLY
    // reports the DEC it was prepared with, which is that block's prepared state exactly.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(playhead(kPoly), 0);
    const float before = poly().decay_seconds();

    // Another step: in the pattern, not in the sound until that step plays.
    post(Cmd::lock_delta, kPoly, 3, kDec, uint8_t(int8_t(40)));
    pump();
    CHECK_NEAR(poly().decay_seconds(), before, 1e-6);

    // The playing step: heard in the next block, and still heard a few blocks on, which is the
    // locked mask keeping the per-block copy off it.
    post(Cmd::lock_delta, kPoly, 0, kDec, uint8_t(int8_t(40)));
    pump();
    const float now = poly().decay_seconds();
    CHECK(now > before * 1.5f);
    pump(4);
    CHECK_NEAR(poly().decay_seconds(), now, 1e-6);
}

// --- what is refused, and what is not -----------------------------------------------------------

TEST(gaps_a_paste_euclid_or_mutation_that_works_is_not_counted_as_refused) {
    fresh();
    put_trig(0, 0);
    const uint32_t start = refused();

    post(Cmd::range_copy, 0, 0, 4);
    post(Cmd::range_paste, 0, 8);
    pump();
    CHECK_EQ(refused(), start);

    post(Cmd::euclid, 2, 0, 5, 16, 0);
    pump();
    CHECK_EQ(refused(), start);
    // The same rhythm again changes nothing, which is counted so the UI spends no undo on it.
    post(Cmd::euclid, 2, 0, 5, 16, 0);
    pump();
    CHECK_EQ(refused(), start + 1);
}

TEST(gaps_a_mutation_that_changes_the_track_is_not_refused_and_one_that_cannot_is) {
    fresh();
    for (int s = 0; s < 4; s++) put_trig(0, s);
    const uint32_t start = refused();
    // Rotate left over the whole track (c = 0): four trigs at the start move.
    post_full(Cmd::mutate, 0, 0, uint8_t(Mutation::rotate_left), 50, 0, 0, 1234);
    pump();
    CHECK_EQ(refused(), start);
    CHECK((step_at(0, 63).flags & kStepTrig) != 0);     // step 1 wrapped round to the end
    CHECK((step_at(0, 3).flags & kStepTrig) == 0);

    // An empty track has nothing to rotate.
    post_full(Cmd::mutate, 3, 0, uint8_t(Mutation::rotate_left), 50, 0, 0, 1234);
    pump();
    CHECK_EQ(refused(), start + 1);
}

// --- restart -------------------------------------------------------------------------------------

TEST(gaps_restart_starts_the_first_pass_again) {
    // Cmd::restart resets the scheduler WITH the pattern-change flag, which is what makes a 1ST
    // step fire on the pass after it -- the user pressed play from the top, and to them that is
    // a first pass. Without the reset, or with it but without the flag, 1ST stays spent.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    raw_step(0, 0).condition = condition_pack(Condition::FIRST);
    post(Cmd::set_track_length, 0, 0, 4);
    pump();

    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);       // the first pass: 1ST fires

    // Second pass, tick 4 steps on: 1ST does not fire.
    const uint8_t t1 = triggers(0);
    FakeHost::the().deliver_tick(4 * 24, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t1)), 0);

    // Restart: the main context moved the clock to 0 and posts this.
    post(Cmd::restart);
    pump();
    const uint8_t t2 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t2)), 1);
    CHECK_EQ(playhead(0), 0);
}

// --- the chain's knobs ---------------------------------------------------------------------------

TEST(gaps_a_chain_entry_knob_walks_the_patterns_bank_by_bank_and_stops_at_both_ends) {
    fresh();
    chain_edit(ChainEdit::insert, 0, 2, 3, 1);     // C4, once
    CHECK_EQ(chain_length(eng().project()), 1);
    const uint32_t start = refused();

    chain_edit(ChainEdit::pattern_delta, 0, uint8_t(int8_t(1)));
    CHECK_EQ(int(eng().project().chain[0].bank), 2);
    CHECK_EQ(int(eng().project().chain[0].slot), 4);
    chain_edit(ChainEdit::pattern_delta, 0, uint8_t(int8_t(12)));   // across into bank D
    CHECK_EQ(int(eng().project().chain[0].bank), 3);
    CHECK_EQ(int(eng().project().chain[0].slot), 0);

    // The bottom: A1 and no further.
    chain_edit(ChainEdit::pattern_delta, 0, uint8_t(int8_t(-100)));
    CHECK_EQ(int(eng().project().chain[0].bank), 0);
    CHECK_EQ(int(eng().project().chain[0].slot), 0);
    // The top: the last slot of the last bank, and no further.
    chain_edit(ChainEdit::pattern_delta, 0, uint8_t(int8_t(127)));
    chain_edit(ChainEdit::pattern_delta, 0, uint8_t(int8_t(5)));
    CHECK_EQ(int(eng().project().chain[0].bank), kBanks - 1);
    CHECK_EQ(int(eng().project().chain[0].slot), kPatternsPerBank - 1);
    CHECK_EQ(refused(), start);                      // a clamp is not a refusal

    // Repeats: 1 to kChainRepeatsMax.
    chain_edit(ChainEdit::repeats_delta, 0, uint8_t(int8_t(-1)));
    CHECK_EQ(int(eng().project().chain[0].repeats), 1);
    chain_edit(ChainEdit::repeats_delta, 0, uint8_t(int8_t(3)));
    CHECK_EQ(int(eng().project().chain[0].repeats), 4);
    chain_edit(ChainEdit::repeats_delta, 0, uint8_t(int8_t(100)));
    CHECK_EQ(int(eng().project().chain[0].repeats), kChainRepeatsMax);
    CHECK_EQ(refused(), start);

    // A knob on an entry past the end is refused and changes nothing.
    chain_edit(ChainEdit::repeats_delta, 1, uint8_t(int8_t(1)));
    chain_edit(ChainEdit::pattern_delta, 1, uint8_t(int8_t(1)));
    CHECK_EQ(refused(), start + 2);
    CHECK_EQ(chain_length(eng().project()), 1);
}

TEST(gaps_removing_the_last_chain_entry_stops_the_chain_and_the_saved_flag) {
    fresh();
    chain_edit(ChainEdit::insert, 0, 0, 1, 1);
    chain_edit(ChainEdit::insert, 1, 0, 2, 1);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    CHECK(eng().chain().active);
    CHECK(eng().project().settings.chain_enabled);

    // One entry left: still a chain.
    chain_edit(ChainEdit::remove, 1);
    CHECK(eng().chain().active);
    CHECK(eng().project().settings.chain_enabled);

    // None left: not a chain, and a save must not reopen it as one.
    chain_edit(ChainEdit::remove, 0);
    CHECK(!eng().chain().active);
    CHECK(!eng().project().settings.chain_enabled);
}

TEST(gaps_restart_drops_ratchet_hits_still_waiting) {
    // A four-hit ratchet on a 24-tick step spreads its hits over thousands of frames, so three
    // of them are still held when the block ends. A restart jumps the clock to 0: those hits
    // belong to a step that is no longer playing, and firing them would be four hits from the
    // past landing on the new downbeat.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    raw_step(0, 0).ratchet = 4;
    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);

    post(Cmd::restart);
    pump(200);                                         // far longer than the step
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);

    // And without the restart the other three do come, so the test above measured something.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    raw_step(0, 0).ratchet = 4;
    const uint8_t t1 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump(200);
    CHECK_EQ(int(uint8_t(triggers(0) - t1)), 4);
}

TEST(gaps_restart_forgets_the_last_condition_result) {
    // PRE follows the track's last condition result, and on a fresh pass it reads TRUE
    // (core/scheduler.cpp). Step 2 is 2:2, so it fails on the first pass and leaves FALSE
    // behind; a restart must clear that, or the PRE on step 1 is decided by a step of the pass
    // the user just abandoned.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    put_trig(0, 1);
    raw_step(0, 0).condition = condition_pack(Condition::PRE);
    raw_step(0, 1).condition = condition_pack_a_b(2, 2);
    post(Cmd::set_track_length, 0, 0, 4);
    pump();

    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);       // PRE on a fresh pass: fires
    FakeHost::the().deliver_tick(24, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);       // 2:2 on pass 1: does not

    post(Cmd::restart);
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 2);       // PRE reads TRUE again
}

TEST(gaps_restart_lands_a_mute_armed_for_the_next_bar) {
    // A restart is a new bar 1, so whatever was waiting for the next bar happens on it -- the
    // bar counter is resynced from the first tick after a restart, which calls on_bar().
    fresh();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    FakeHost::the().deliver_tick(100, 0);
    pump();
    post(Cmd::set_mute, 2, 0, 1, 1);                   // mute track 3 at the next bar
    pump();
    CHECK(!eng().muted(2));

    post(Cmd::restart);
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK(eng().muted(2));
}

TEST(gaps_a_master_length_turned_while_a_chain_plays_moves_the_next_boundary) {
    // The chain moves on at the end of a pass, and a pass is the MASTER length. Turning it
    // re-measures the next boundary from the pattern's start, so the chain does not play out the
    // rest of a pass that is no longer that long.
    fresh();
    chain_edit(ChainEdit::insert, 0, 0, 0, 1);
    chain_edit(ChainEdit::insert, 1, 0, 1, 1);
    FakeHost::the().set_running(true);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    FakeHost::the().deliver_tick(100, 0);
    pump();
    CHECK_EQ(int(eng().project().slot), 0);

    post(Cmd::set_master_length, 0, 0, 4);             // a pass is now 96 ticks
    pump();
    FakeHost::the().deliver_tick(101, 0);              // the clock delivers every tick
    pump();
    FakeHost::the().deliver_tick(192, 0);              // the first 96-tick boundary after 100
    pump();
    CHECK_EQ(int(eng().project().bank), 0);
    CHECK_EQ(int(eng().project().slot), 1);
}

TEST(gaps_a_master_length_of_zero_is_one_step) {
    fresh();
    post(Cmd::set_master_length, 0, 0, 0);
    pump();
    CHECK_EQ(int(eng().pattern().master_length), 1);
    post(Cmd::set_master_length, 0, 0, 200);
    pump();
    CHECK_EQ(int(eng().pattern().master_length), kStepsPerTrack);
    post_full(Cmd::set_master_length, 0, 0, 0, uint8_t(int8_t(-100)), kDeltaFlag, 0);
    pump();
    CHECK_EQ(int(eng().pattern().master_length), 1);
}

TEST(gaps_an_undo_or_redo_of_a_track_length_is_what_the_sequencer_plays) {
    fresh();
    FakeHost::the().set_running(true);
    post(Cmd::set_track_length, 0, 0, 16);
    pump();
    post(Cmd::undo_mark, 0, 0, uint8_t(GestureKind::length), 1);
    post(Cmd::set_track_length, 0, 0, 4);
    pump();

    post(Cmd::undo);
    pump();
    CHECK_EQ(int(eng().pattern().tracks[0].length), 16);
    FakeHost::the().deliver_tick(5 * 24, 0);
    pump();
    CHECK_EQ(playhead(0), 5);                          // 16 steps long, not 4

    post(Cmd::redo);
    pump();
    CHECK_EQ(int(eng().pattern().tracks[0].length), 4);
    FakeHost::the().deliver_tick(6 * 24, 0);
    pump();
    CHECK_EQ(playhead(0), 2);                          // 4 steps long again
}

TEST(gaps_a_pattern_chosen_now_cancels_one_that_was_waiting) {
    fresh();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    post(Cmd::queue_pattern, 0, 0, 0, 5, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(eng().queue().pending);

    post(Cmd::queue_pattern, 0, 0, 0, 7, uint8_t(QueueMode::immediate));
    pump();
    CHECK_EQ(int(eng().project().slot), 7);
    CHECK(!eng().queue().pending);
    // and the one that was waiting never lands
    FakeHost::the().deliver_tick(16 * 24, 0);
    pump();
    CHECK_EQ(int(eng().project().slot), 7);
}

// --- a project arriving ---------------------------------------------------------------------------

TEST(gaps_a_loaded_kit_is_what_the_performance_layer_plays_without_any_other_command) {
    // Cmd::project_loaded marks the performance layer stale by itself. fresh() also posts the
    // macro clears, which mark it too, so this loads a second kit with nothing else posted.
    fresh();
    pump();
    Project &incoming = eng().project_for_load();
    incoming.kit.tracks[3].params[5] = 17;
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(int(eng().params(3).get(5)), 17);
    CHECK_EQ(int(eng().performed(3, 5)), 17);
}

TEST(gaps_a_load_forgets_the_undo_history_and_the_queue) {
    fresh();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    post(Cmd::undo_mark, 0, 2, uint8_t(GestureKind::trig), 1);
    post(Cmd::toggle_trig, 0, 2);
    post(Cmd::queue_pattern, 0, 0, 0, 3, uint8_t(QueueMode::end_of_pattern));
    pump();
    CHECK(eng().queue().pending);

    eng().project_for_load().live().tracks[0].steps[2].flags = 0;
    post(Cmd::project_loaded);
    pump();
    CHECK(!eng().queue().pending);
    // An undo now has nothing of the old project's to put back.
    post(Cmd::undo);
    pump();
    CHECK((step_at(0, 2).flags & kStepTrig) == 0);
}

TEST(gaps_a_load_drops_ratchet_hits_of_the_project_it_replaced) {
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    raw_step(0, 0).ratchet = 4;
    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);
    post(Cmd::project_loaded);
    pump(200);
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);
}

TEST(gaps_a_load_with_scene_ids_this_build_lacks_falls_back_to_the_default_pair) {
    fresh();
    const ProjectSettings defaults;
    eng().project_for_load().settings.scene_a = uint8_t(kScenes);
    eng().project_for_load().settings.scene_b = uint8_t(kScenes);
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(eng().scene_slot(0), int(defaults.scene_a));
    CHECK_EQ(eng().scene_slot(1), int(defaults.scene_b));
    // And the project's copy is repaired too, so the next save does not write the bad id back.
    CHECK_EQ(int(eng().project().settings.scene_a), int(defaults.scene_a));
    CHECK_EQ(int(eng().project().settings.scene_b), int(defaults.scene_b));

    // The last id this build has is kept.
    eng().project_for_load().settings.scene_a = uint8_t(kScenes - 1);
    eng().project_for_load().settings.scene_b = uint8_t(kScenes - 2);
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(eng().scene_slot(0), kScenes - 1);
    CHECK_EQ(eng().scene_slot(1), kScenes - 2);
}

// --- samples ----------------------------------------------------------------------------------------
//
// The rig's Storage::init hands the engine its 64 MiB sample region, as App::init does on the
// unit, so Cmd::set_sample's bounds are reachable here. Nothing below reads a sample's bytes:
// the tests only ask WHICH pointer and how many frames the machines were handed, and they clear
// the track again before they end.

namespace {

constexpr uint32_t kRegion = kSampleBudgetMaxMib << 20;   // app/storage.hpp's kRegionBytes
constexpr uint32_t kMaxFrames = (1u << 24) - 1;           // what c|d|e can carry
constexpr uint8_t kSampleTrack = 7;                       // SAMPLE in the default kit

void set_sample(uint8_t track, uint8_t channels, uint32_t offset_bytes, uint32_t frames) {
    Command command;
    command.cmd = Cmd::set_sample;
    command.track = track;
    command.a = channels;
    command.c = uint8_t(frames);
    command.d = uint8_t(frames >> 8);
    command.e = uint8_t(frames >> 16);
    command.u = offset_bytes;
    eng().commands().post(command);
    pump();
}

const Sampler &sampler_on(uint8_t track) {
    return *static_cast<const Sampler *>(eng().machine(track));
}

}  // namespace

TEST(gaps_a_sample_is_handed_over_only_when_it_fits_the_region_exactly) {
    fresh();
    post(Cmd::set_machine, kSampleTrack, 0, uint8_t(MachineId::sample));
    pump();
    const uint32_t start = refused();

    // The region's base, as the machines see it. Long enough to be playable: a Sampler keeps
    // no pointer to a sample too short to play.
    set_sample(kSampleTrack, 1, 0, 1000);
    CHECK_EQ(refused(), start);
    const int16_t *base = sampler_on(kSampleTrack).sample_data();
    CHECK(base != nullptr);

    // Stereo, ending on the region's last byte: accepted, and at the offset asked for.
    const uint32_t stereo_bytes = kMaxFrames * 4u;
    set_sample(kSampleTrack, 2, kRegion - stereo_bytes, kMaxFrames);
    CHECK_EQ(refused(), start);
    CHECK(sampler_on(kSampleTrack).sample_data() == base + (kRegion - stereo_bytes) / 2);
    CHECK_EQ(sampler_on(kSampleTrack).sample_frames(), int(kMaxFrames));
    CHECK_EQ(sampler_on(kSampleTrack).sample_channels(), 2);

    // Four bytes later it runs one frame past the end: refused, and the sample in place stays.
    set_sample(kSampleTrack, 2, kRegion - stereo_bytes + 4, kMaxFrames);
    CHECK_EQ(refused(), start + 1);
    CHECK(sampler_on(kSampleTrack).sample_data() == base + (kRegion - stereo_bytes) / 2);

    // Mono, ending on the last byte: accepted. Two bytes later: refused.
    const uint32_t mono_bytes = kMaxFrames * 2u;
    set_sample(kSampleTrack, 1, kRegion - mono_bytes, kMaxFrames);
    CHECK_EQ(refused(), start + 1);
    CHECK(sampler_on(kSampleTrack).sample_data() == base + (kRegion - mono_bytes) / 2);
    CHECK_EQ(sampler_on(kSampleTrack).sample_channels(), 1);
    set_sample(kSampleTrack, 1, kRegion - mono_bytes + 2, kMaxFrames);
    CHECK_EQ(refused(), start + 2);

    // One frame at the very end fits; at the region's size it does not; nor past it, nor at an
    // odd offset, nor with no frames at all.
    set_sample(kSampleTrack, 1, kRegion - 2, 1);
    CHECK_EQ(refused(), start + 2);
    set_sample(kSampleTrack, 1, kRegion, 1);
    set_sample(kSampleTrack, 1, kRegion + 2, 1);
    set_sample(kSampleTrack, 1, 3, 1);
    set_sample(kSampleTrack, 1, 0, 0);
    CHECK_EQ(refused(), start + 6);

    post(Cmd::clear_sample, kSampleTrack);
    pump();
}

TEST(gaps_clearing_a_sample_clears_the_grain_reader_too) {
    fresh();
    post(Cmd::set_machine, kSampleTrack, 0, uint8_t(MachineId::sample));
    pump();
    set_sample(kSampleTrack, 1, 0, 1000);
    CHECK(sampler_on(kSampleTrack).has_sample());

    post(Cmd::clear_sample, kSampleTrack);
    pump();
    CHECK(!sampler_on(kSampleTrack).has_sample());
    // GRAIN reads the same track's sample, and must have let go of it as well.
    post(Cmd::set_machine, kSampleTrack, 0, uint8_t(MachineId::grain));
    pump();
    const Granular &grain = *static_cast<const Granular *>(eng().machine(kSampleTrack));
    CHECK(!grain.has_sample());
    post(Cmd::set_machine, kSampleTrack, 0, uint8_t(MachineId::sample));
    pump();
}

// --- kit and performance knobs ---------------------------------------------------------------------

TEST(gaps_a_parameter_index_past_the_kit_writes_nothing) {
    fresh();
    const uint8_t next_track_first = eng().params(1).get(0);
    const uint8_t own_last = eng().params(0).get(kParams - 1);
    post(Cmd::set_param, 0, 0, uint8_t(kParams), 99);
    pump();
    CHECK_EQ(int(eng().params(1).get(0)), int(next_track_first));
    CHECK_EQ(int(eng().params(0).get(kParams - 1)), int(own_last));
}

TEST(gaps_a_parameter_turned_below_zero_stops_at_zero) {
    fresh();
    post(Cmd::set_param, 0, 0, 5, 10);
    pump();
    post_full(Cmd::set_param, 0, 0, 5, uint8_t(int8_t(-100)), kDeltaFlag, 0);
    pump();
    CHECK_EQ(int(eng().params(0).get(5)), 0);
    post_full(Cmd::set_param, 0, 0, 5, uint8_t(int8_t(100)), kDeltaFlag, 0);
    pump();
    post_full(Cmd::set_param, 0, 0, 5, uint8_t(int8_t(100)), kDeltaFlag, 0);
    pump();
    CHECK_EQ(int(eng().params(0).get(5)), 127);
}

TEST(gaps_an_auto_fill_or_repeat_length_this_build_lacks_is_the_default) {
    fresh();
    post(Cmd::set_auto_fill, 0, 0, uint8_t(AutoFill::count));
    pump();
    CHECK(eng().auto_fill() == AutoFill::off);
    post(Cmd::set_repeat, 0, 0, uint8_t(RepeatLength::count), 0, 0);
    pump();
    CHECK(eng().repeat_length() == RepeatLength::sixteenth);
    // and the last real one is kept
    post(Cmd::set_auto_fill, 0, 0, uint8_t(uint8_t(AutoFill::count) - 1));
    pump();
    CHECK_EQ(int(eng().auto_fill()), int(AutoFill::count) - 1);
    post(Cmd::set_auto_fill, 0, 0, 0);
    pump();
}

TEST(gaps_beat_repeat_follows_its_hold_and_its_pitch_switch) {
    fresh();
    FakeHost::the().set_running(true);
    post(Cmd::set_repeat, 0, 0, 0, 1, 1);             // held, pitched down
    pump(2);
    CHECK(eng().repeat().active());
    CHECK(eng().repeat().pitch_down());
    post(Cmd::set_repeat, 0, 0, 0, 1, 0);             // held, straight
    pump(2);
    CHECK(eng().repeat().active());
    CHECK(!eng().repeat().pitch_down());
    post(Cmd::set_repeat, 0, 0, 0, 0, 0);             // let go
    pump(2);
    CHECK(!eng().repeat().active());
}

// --- undo marks ---------------------------------------------------------------------------------

TEST(gaps_the_same_gesture_counter_on_another_track_or_step_is_another_undo_level) {
    // A mark carries the track and the step, and core/undo coalesces two marks only when the
    // kind, the counter, the track AND the step all agree. The UI's counter can repeat across
    // tracks, so a mark that dropped either field would fold an edit on track 2 into the
    // previous one on track 1, and one undo would take both back.
    fresh();
    const int depth = int(eng().status().undo_depth.load(std::memory_order_relaxed));
    post(Cmd::undo_mark, 0, 2, uint8_t(GestureKind::trig), 9);
    post(Cmd::toggle_trig, 0, 2);
    post(Cmd::undo_mark, 1, 2, uint8_t(GestureKind::trig), 9);    // another track
    post(Cmd::toggle_trig, 1, 2);
    post(Cmd::undo_mark, 1, 3, uint8_t(GestureKind::trig), 9);    // another step
    post(Cmd::toggle_trig, 1, 3);
    pump();
    CHECK_EQ(int(eng().status().undo_depth.load(std::memory_order_relaxed)), depth + 3);

    post(Cmd::undo);
    pump();
    CHECK((step_at(1, 3).flags & kStepTrig) == 0);
    CHECK((step_at(1, 2).flags & kStepTrig) != 0);
    post(Cmd::undo);
    pump();
    CHECK((step_at(1, 2).flags & kStepTrig) == 0);
    CHECK((step_at(0, 2).flags & kStepTrig) != 0);
}

// --- a load, again ------------------------------------------------------------------------------

TEST(gaps_a_load_with_no_chain_running_stops_the_one_playing_and_its_first_pass_is_a_first_pass) {
    fresh();
    chain_edit(ChainEdit::insert, 0, 0, 0, 1);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    CHECK(eng().chain().active);

    // A 1ST step in the project that arrives, after a pass of the old one has been played.
    FakeHost::the().set_running(true);
    post(Cmd::set_track_length, 0, 0, 4);
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    FakeHost::the().deliver_tick(4 * 24, 0);
    pump();
    FakeHost::the().set_running(false);

    // The project that arrives was saved with no chain running. (One saved with its chain on
    // resumes it -- chain_resume -- which is tests/test_engine.cpp's business.)
    eng().project_for_load().settings.chain_enabled = false;
    Pattern &incoming = eng().project_for_load().live();
    incoming.tracks[0].length = 4;
    incoming.tracks[0].steps[0].flags = kStepTrig;
    incoming.tracks[0].steps[0].velocity = 100;
    incoming.tracks[0].steps[0].condition = condition_pack(Condition::FIRST);
    post(Cmd::project_loaded);
    pump();
    CHECK(!eng().chain().active);

    FakeHost::the().set_running(true);
    const uint8_t before = triggers(0);
    FakeHost::the().deliver_tick(8 * 24, 0);           // the clock carries on; the project is new
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - before)), 1);
}

// --- the performance layer follows every edit that changes it ------------------------------------

TEST(gaps_capturing_or_selecting_a_scene_is_heard_without_anything_else_moving) {
    fresh();
    pump();
    constexpr int kP = 5;
    // Scene 2 holds 20, scene 3 holds 100, and the morph sits at the B end.
    post(Cmd::set_param, 0, 0, kP, 20);
    post_full(Cmd::scene_capture, 0, 0, 2, 0, 0, 0);
    post(Cmd::set_param, 0, 0, kP, 100);
    post_full(Cmd::scene_capture, 0, 0, 3, 0, 0, 0);
    Command morph;
    morph.cmd = Cmd::set_morph;
    morph.u = kMorphScale;
    eng().commands().post(morph);
    post(Cmd::scene_select, 0, 0, 0, 2);
    post(Cmd::scene_select, 0, 0, 1, 2);               // A and B both scene 2: 20
    pump();
    CHECK_EQ(int(eng().performed(0, kP)), 20);

    // Selecting scene 3 into B, and nothing else: heard.
    post(Cmd::scene_select, 0, 0, 1, 3);
    pump();
    CHECK_EQ(int(eng().performed(0, kP)), 100);

    // Capturing over the scene B points at, and nothing else: heard.
    post(Cmd::set_param, 0, 0, kP, 60);
    pump();
    post_full(Cmd::scene_capture, 0, 0, 3, 0, 0, 0);
    pump();
    CHECK_EQ(int(eng().performed(0, kP)), 60);

    // Back to the defaults so the next test starts from the kit.
    eng().commands().post(Command{Cmd::set_morph});
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 1);
    post_full(Cmd::scene_erase, 0, 0, 2, 0, 0, 0);
    post_full(Cmd::scene_erase, 0, 0, 3, 0, 0, 0);
    pump();
}

TEST(gaps_an_index_past_the_scene_macro_or_mute_tables_changes_nothing) {
    fresh();
    const int a = eng().scene_slot(0), b = eng().scene_slot(1);
    const uint32_t start = refused();
    post_full(Cmd::scene_capture, 0, 0, uint8_t(kScenes), 0, 0, 0);
    post_full(Cmd::scene_erase, 0, 0, uint8_t(kScenes), 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 0, uint8_t(kScenes));
    post(Cmd::scene_select, 0, 0, 1, uint8_t(kScenes));
    pump();
    CHECK_EQ(eng().scene_slot(0), a);
    CHECK_EQ(eng().scene_slot(1), b);

    post(Cmd::set_macro, 0, 0, uint8_t(kMacros), 99);
    post(Cmd::macro_clear, 0, 0, uint8_t(kMacros));
    pump();
    for (int m = 0; m < kMacros; m++) CHECK_EQ(int(eng().macro_position(m)), 0);

    const uint8_t mutes[2] = {eng().project().settings.mute_states[7],
                              eng().project().settings.live_mutes};
    post(Cmd::set_mute, 1, 0, 1, 0);                   // something to store
    pump();
    post(Cmd::mute_store, 0, 0, 8);
    post(Cmd::mute_recall, 0, 0, 8, 0);
    pump();
    CHECK_EQ(int(eng().project().settings.mute_states[7]), int(mutes[0]));
    CHECK(eng().muted(1));
    post(Cmd::set_mute, 1, 0, 0, 0);
    pump();
    (void)mutes[1];
    CHECK_EQ(refused(), start);
}

TEST(gaps_switching_a_machine_silences_the_one_it_leaves) {
    fresh();
    pump();
    post(Cmd::trigger_note, kPoly, 0, 60, 100);
    pump(2);
    CHECK(poly().sounding_voices() > 0);
    post(Cmd::set_machine, kPoly, 0, uint8_t(MachineId::va_mono));
    pump();
    post(Cmd::set_machine, kPoly, 0, uint8_t(MachineId::va_poly));
    pump();
    // Back on VA POLY: the chord that was ringing when it was left is not still there.
    CHECK_EQ(poly().sounding_voices(), 0);
}

TEST(gaps_switching_a_machine_is_heard_with_the_performance_layer_over_it) {
    // set_machine marks the performance layer stale, so the new machine is prepared from the
    // kit with the macros over it rather than from what the old one was last handed.
    fresh();
    pump();
    constexpr int kP = 8;                              // DEC on both VAs
    post(Cmd::set_param, kPoly, 0, kP, 10);
    pump();
    // A macro over DEC, with its knob at the top.
    post_full(Cmd::macro_assign, kPoly, 0, 0, kP, 10, 120);
    post(Cmd::set_macro, 0, 0, 0, 127);
    pump();
    const uint8_t played = eng().performed(kPoly, kP);
    CHECK(played > 100);

    post(Cmd::set_machine, kPoly, 0, uint8_t(MachineId::va_mono));
    pump();
    // The kit changes under the macro while it is VA MONO; the performed value follows it only
    // if set_machine re-ran the performance layer -- which is the same pass that hands the new
    // machine its parameters.
    CHECK_EQ(int(eng().performed(kPoly, kP)), int(played));
    post(Cmd::set_machine, kPoly, 0, uint8_t(MachineId::va_poly));
    post(Cmd::macro_clear, 0, 0, 0);
    pump();
}

TEST(gaps_a_kit_store_copies_every_track_and_every_parameter) {
    fresh();
    post(Cmd::set_param, 0, 0, 0, 33);                 // track 1, the first parameter
    post(Cmd::set_param, kTracks - 1, 0, kParams - 1, 44);
    post(Cmd::kit_store);
    pump();
    CHECK_EQ(int(eng().project().kit.tracks[0].params[0]), 33);
    CHECK_EQ(int(eng().project().kit.tracks[kTracks - 1].params[kParams - 1]), 44);
}

TEST(gaps_a_pad_hit_takes_its_velocity_and_a_zero_is_the_quietest_hit) {
    fresh();
    pump();
    // Track 1 is a kick. Energy over a span, as tests/test_engine.cpp explains.
    auto hit = [](int velocity) {
        fresh();
        strata::test::settle(4);
        if (velocity >= 0) post(Cmd::trigger_note, 0, 0, 36, uint8_t(velocity));
        float total = 0.0f;
        for (int b = 0; b < 60; b++) {
            pump();
            const float *block = strata::test::last_block();
            for (int i = 0; i < strata::test::kRigStereo; i++) total += block[i] * block[i];
        }
        return total;
    };
    const float none = hit(-1);                        // no hit at all
    const float soft = hit(0);
    const float loud = hit(127);
    // A zero is played as the quietest hit there is, not as silence: clearly above a run with
    // no hit at all, whose energy is the master reverb's tail from earlier tests.
    CHECK(soft > none * 1.5f + 1e-6f);
    CHECK(loud > soft * 2.0f);
}

TEST(gaps_a_resample_of_zero_bars_is_one_bar_and_of_twenty_is_eight) {
    fresh();
    FakeHost::the().set_running(true);
    pump(2);                                           // the tempo is read from the clock
    post(Cmd::resample_arm, 0, 0, 0);
    pump();
    CHECK(eng().capture().armed());
    const uint32_t one_bar = eng().capture().wanted();
    CHECK(one_bar > 0u);
    post(Cmd::resample_cancel);
    pump();
    post(Cmd::resample_arm, 0, 0, 20);
    pump();
    // Bars into frames is a float product rounded down, so N bars is N times one bar to within
    // a frame or two of rounding.
    CHECK_NEAR(double(eng().capture().wanted()), 8.0 * double(one_bar), 8.0);
    post(Cmd::resample_cancel);
    pump();
    post(Cmd::resample_arm, 0, 0, 2);
    pump();
    CHECK_NEAR(double(eng().capture().wanted()), 2.0 * double(one_bar), 2.0);
    post(Cmd::resample_cancel);
    pump();
}

// --- the block ------------------------------------------------------------------------------------

namespace {

// Blocks after the tick block until `track`'s trigger counter reaches `want`, or -1.
int blocks_until(int track, uint8_t start, int want, int limit) {
    for (int b = 0; b < limit; b++) {
        if (int(uint8_t(triggers(track) - start)) >= want) return b;
        pump();
    }
    return -1;
}

}  // namespace

TEST(gaps_a_ratchet_hit_lands_in_the_block_its_frame_falls_in_and_follows_the_tempo) {
    // 120 BPM is exactly 250 frames a tick, so a step is 6000 frames and the second hit of a
    // two-hit ratchet is at frame 3000: block 46 of 64 frames (2944..3007). A hit deferred
    // without its frame measured from the next block, or with its block length counted twice,
    // lands a block late or early.
    fresh();
    FakeHost::the().set_tempo(120.0f);
    FakeHost::the().set_running(true);
    pump(2);
    put_trig(0, 0);
    raw_step(0, 0).ratchet = 2;
    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();                                            // block 0: the first hit
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);
    CHECK_EQ(blocks_until(0, t0, 2, 200), 3000 / kRigBlock - 0);

    // At 60 BPM a tick is twice as long, so the same hit is at frame 6000: block 93. The tempo
    // change re-derives the scheduler's step length; without that it would stay at block 46.
    fresh();
    FakeHost::the().set_tempo(60.0f);
    FakeHost::the().set_running(true);
    pump(2);
    put_trig(0, 0);
    raw_step(0, 0).ratchet = 2;
    const uint8_t t1 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    CHECK_EQ(blocks_until(0, t1, 2, 400), 6000 / kRigBlock);
    FakeHost::the().set_tempo(120.0f);
    pump(2);
}

TEST(gaps_two_ticks_in_one_block_both_play) {
    // At 64 frames a block and hundreds of frames a tick the rig never put two ticks in one
    // block, so the loop over `clock::ticks()` was only ever run once. At a fast tempo or a
    // larger block it runs more, and every tick must be resolved.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(0, 0);
    put_trig(0, 1);
    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(0, 0);
    FakeHost::the().deliver_tick(24, 40);
    pump();
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 2);
    CHECK_EQ(playhead(0), 1);
}

TEST(gaps_an_engine_that_never_initialised_writes_silence) {
    // App::init can fail (a buffer it could not take), and the audio callback still runs.
    static Engine never;
    static float out[2 * kRigBlock];
    for (float &x : out) x = 1.0f;
    never.process(nullptr, out, uint32_t(kRigBlock));
    for (int i = 0; i < 2 * kRigBlock; i++) CHECK(out[i] == 0.0f);
}

TEST(gaps_the_status_counts_rendered_tracks_and_reports_the_peak) {
    fresh();
    strata::test::settle(400);
    pump();
    CHECK_EQ(int(eng().status().voices.load(std::memory_order_relaxed)), 0);
    post(Cmd::trigger_note, 0, 0, 36, 127);
    post(Cmd::trigger_note, kPoly, 0, 60, 127);
    post(Cmd::trigger_note, 5, 0, 48, 127);
    pump();
    CHECK_EQ(int(eng().status().voices.load(std::memory_order_relaxed)), 3);
    // A loud kick peaks well above a tenth of full scale; x1000 that is over 100.
    pump(4);
    CHECK(eng().status().peak_milli.load(std::memory_order_relaxed) > 100u);
    // A muted track is not rendered, so it is not counted.
    post(Cmd::set_mute, 5, 0, 1, 0);
    pump();
    CHECK_EQ(int(eng().status().voices.load(std::memory_order_relaxed)), 2);
    post(Cmd::set_mute, 5, 0, 0, 0);
    pump();

    // And the clock's own state, which the UI draws the transport from.
    FakeHost::the().set_running(true);
    FakeHost::the().set_position(777);
    pump();
    CHECK_EQ(eng().status().tick.load(std::memory_order_relaxed), 777u);
    CHECK_EQ(int(eng().status().running.load(std::memory_order_relaxed)), 1);
    FakeHost::the().set_running(false);
    pump();
    CHECK_EQ(int(eng().status().running.load(std::memory_order_relaxed)), 0);
}

TEST(gaps_a_throw_on_the_first_track_opens_its_send) {
    fresh();
    pump();
    constexpr int kSendA = 14;
    post(Cmd::set_param, 0, 0, kSendA, 0);
    pump();
    post(Cmd::set_throw, 0, 0, 1);                     // track 1, the delay
    pump();
    CHECK_EQ(int(eng().performed(0, kSendA)), 127);
    post(Cmd::set_throw, 0, 0, 0);
    pump();
    CHECK_EQ(int(eng().performed(0, kSendA)), 0);
}

TEST(gaps_a_loaded_kit_is_clamped_and_cuts_what_was_sounding) {
    fresh();
    pump();
    post(Cmd::trigger_note, 0, 0, 36, 127);
    post(Cmd::trigger_note, kPoly, 0, 60, 127);
    pump(2);
    CHECK(poly().sounding_voices() > 0);

    Project &incoming = eng().project_for_load();
    incoming.kit.tracks[0].params[0] = 200;            // past a knob's range
    incoming.kit.tracks[1].params[0] = 41;
    incoming.kit.tracks[0].level = 1.5f;
    incoming.kit.tracks[1].level = 3.0f;               // past the fader's
    incoming.kit.tracks[2].level = -1.0f;
    incoming.kit.tracks[4].machine = uint8_t(MachineId::count);   // a machine this build lacks
    post(Cmd::project_loaded);
    pump();
    CHECK_EQ(int(eng().params(0).get(0)), 127);
    CHECK_EQ(int(eng().params(1).get(0)), 41);
    CHECK_NEAR(eng().level(0), 1.5f, 1e-6);
    CHECK_NEAR(eng().level(1), 2.0f, 1e-6);
    CHECK_NEAR(eng().level(2), 0.0f, 1e-6);
    CHECK_EQ(int(eng().machine_id(4)), int(MachineId::kick));
    // Everything that was sounding was cut, the first track included.
    CHECK_EQ(poly().sounding_voices(), 0);
    CHECK_EQ(int(eng().status().voices.load(std::memory_order_relaxed)), 0);
}

TEST(gaps_muting_one_track_cuts_only_that_track) {
    fresh();
    pump();
    post(Cmd::trigger_note, 0, 0, 36, 127);
    post(Cmd::trigger_note, kPoly, 0, 60, 127);
    pump(2);
    post(Cmd::set_mute, 0, 0, 1, 0);                  // the first track
    pump();
    CHECK(eng().muted(0));
    CHECK(poly().sounding_voices() > 0);               // the chord rings on
    CHECK_EQ(int(eng().status().voices.load(std::memory_order_relaxed)), 1);
    post(Cmd::set_mute, 0, 0, 0, 0);
    pump();
    // Unmuting is not a cut either.
    CHECK(poly().sounding_voices() > 0);
}

TEST(gaps_bars_are_counted_one_by_one_and_resynced_on_a_jump_back) {
    fresh();
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    const uint32_t start = eng().bar();
    for (uint32_t b = 1; b <= 3; b++) {
        FakeHost::the().deliver_tick(b * kTicksPerBar - 1, 0);
        pump();
        CHECK_EQ(eng().bar(), start + b - 1);
        FakeHost::the().deliver_tick(b * kTicksPerBar, 0);
        pump();
        CHECK_EQ(eng().bar(), start + b);
    }
    // Back exactly one bar and a tick: resynced from the tick.
    FakeHost::the().deliver_tick(2 * kTicksPerBar - 1, 0);
    pump();
    CHECK_EQ(eng().bar(), 1u);
    FakeHost::the().deliver_tick(2 * kTicksPerBar, 0);
    pump();
    CHECK_EQ(eng().bar(), 2u);
}

TEST(gaps_a_chain_with_repeats_moves_on_after_the_right_number_of_passes) {
    // Every pass boundary after the first is derived from the one before; a boundary one pass
    // off moves the chain on a pass early or late.
    fresh();
    post(Cmd::set_master_length, 0, 0, 4);             // 96-tick passes, on A1 ...
    pump();
    chain_edit(ChainEdit::insert, 0, 0, 0, 3);         // A1 three times
    chain_edit(ChainEdit::insert, 1, 0, 1, 1);         // then A2
    FakeHost::the().set_running(true);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    for (uint32_t t = 0; t < 3 * 96; t += 12) {
        FakeHost::the().deliver_tick(t, 0);
        pump();
        CHECK_EQ(int(eng().project().slot), 0);
    }
    FakeHost::the().deliver_tick(3 * 96, 0);
    pump();
    CHECK_EQ(int(eng().project().slot), 1);
}

TEST(gaps_a_queued_pattern_starts_its_own_first_pass_with_its_own_lengths) {
    fresh();
    // The pattern to switch to: B1, track 1 four steps long with a 1ST trig on step 1.
    Pattern &next = eng().project_for_load().patterns[1][0];
    next.tracks[0].length = 4;
    next.tracks[0].steps[0].flags = kStepTrig;
    next.tracks[0].steps[0].velocity = 100;
    next.tracks[0].steps[0].condition = condition_pack(Condition::FIRST);
    FakeHost::the().set_running(true);
    FakeHost::the().deliver_tick(0, 0);
    pump();
    FakeHost::the().deliver_tick(30, 0);
    pump();
    post(Cmd::queue_pattern, 0, 0, 1, 0, uint8_t(QueueMode::immediate));
    pump();
    CHECK_EQ(int(eng().project().bank), 1);
    const uint8_t t0 = triggers(0);
    FakeHost::the().deliver_tick(48, 0);
    pump();
    // The switch restarted nothing about the clock, but the new pattern is 4 steps long, so
    // tick 48 is its step 3 -- not step 3 of a 16-step track, which would be the same here --
    // and tick 96 + 0 wraps to step 1, where 1ST fires because this is the first pass after a
    // pattern change.
    FakeHost::the().deliver_tick(96, 0);
    pump();
    CHECK_EQ(playhead(0), 0);
    CHECK_EQ(int(uint8_t(triggers(0) - t0)), 1);
}

TEST(gaps_a_restart_measures_the_next_pass_from_the_new_start) {
    // A restart puts the clock back at 0. The boundary waiting from before it was measured on the
    // old position, two passes on; kept, the chain would sit out two extra passes of A1.
    fresh();
    post(Cmd::set_master_length, 0, 0, 4);             // 96-tick passes on A1
    pump();
    chain_edit(ChainEdit::insert, 0, 0, 0, 3);         // A1 three times
    chain_edit(ChainEdit::insert, 1, 0, 1, 1);         // then A2
    FakeHost::the().set_running(true);
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    for (uint32_t t = 0; t <= 200; t += 8) {            // two boundaries crossed, the next at 288
        FakeHost::the().deliver_tick(t, 0);
        pump();
    }
    CHECK_EQ(int(eng().project().slot), 0);

    post(Cmd::restart);
    pump();
    for (uint32_t t = 0; t < 96; t += 8) {
        FakeHost::the().deliver_tick(t, 0);
        pump();
        CHECK_EQ(int(eng().project().slot), 0);
    }
    FakeHost::the().deliver_tick(96, 0);               // the third pass of A1 ends here
    pump();
    CHECK_EQ(int(eng().project().slot), 1);
}

TEST(gaps_a_knob_turned_over_a_locked_step_is_heard_until_that_step_plays_again) {
    // set_param mirrors the new value into `effective_`. For an unlocked parameter the per-block
    // copy would write the same value anyway; over a lock it is the only thing that does, so the
    // turn is heard at once instead of waiting for a step that has no lock.
    fresh();
    FakeHost::the().set_running(true);
    put_trig(kPoly, 0);
    post(Cmd::lock_delta, kPoly, 0, kDec, uint8_t(int8_t(40)));
    pump();
    FakeHost::the().deliver_tick(0, 0);
    pump();
    const float locked = poly().decay_seconds();

    post(Cmd::set_param, kPoly, 0, kDec, 0);           // the kit's DEC all the way down
    pump();
    const float turned = poly().decay_seconds();
    CHECK(turned < locked * 0.5f);
    pump(4);
    CHECK_NEAR(poly().decay_seconds(), turned, 1e-6);
}
