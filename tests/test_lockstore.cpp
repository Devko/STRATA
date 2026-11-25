// LOCK (core/lockstore.hpp and its wiring in app/engine.cpp): hold FUNC + MUTATE stores the live
// set for the playing pattern, a tap jumps to it at once, a second tap swaps back.
//
// What it has to do: a jump brings back the pattern, the sound bytes, the faders, the send and
// master bytes, the mutes, the morph and the macro positions -- and nothing else (not the
// machines, not the tempo). A second tap restores exactly what the jump replaced, sound and all,
// which undo cannot (R1). One lock per pattern (D14); a pattern change drops the swap-back; a
// project load drops every lock. The "changed" mark follows any difference and clears on a
// jump back.
#include "check.hpp"

#include "apprig.hpp"
#include "lockstore.hpp"

using namespace strata;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;
using strata::test::set_master_byte;
using strata::test::set_send_byte;

namespace {

// ~1.2 MiB between them: file scope, never locals (the 16 KiB frame rule).
LiveState g_states[LockStore::kStates];
LockStore g_store;

bool trig_at(int track, int step) {
    return (strata::test::engine().pattern().tracks[track].steps[step].flags & kStepTrig) != 0;
}

void set_param(int track, int param, uint8_t value) {
    post(Cmd::set_param, uint8_t(track), 0, uint8_t(param), value);
    pump();
}

void set_level(int track, float level) {
    Command command;
    command.cmd = Cmd::set_track_level;
    command.track = uint8_t(track);
    command.f = level;
    strata::test::engine().commands().post(command);
    pump();
}

void set_morph(uint32_t at) {
    Command command;
    command.cmd = Cmd::set_morph;
    command.u = at;
    strata::test::engine().commands().post(command);
    pump();
}

uint8_t lock_state() {
    return strata::test::engine().status().lock_state.load(std::memory_order_relaxed);
}

uint8_t lock_last() {
    return strata::test::engine().status().lock_last.load(std::memory_order_relaxed);
}

// Long enough for the engine's periodic compare to have run.
void settle_mark() { pump(Engine::kLockCheckBlocks + 1); }

}  // namespace

// --- the store's decisions --------------------------------------------------------------------

TEST(lockstore_a_tap_jumps_then_swaps_back_then_jumps_again) {
    g_store.attach(g_states);
    CHECK(g_store.tap(3) == LockTap::empty);          // nothing stored: nothing happens
    CHECK(!g_store.has(3));
    g_store.stored(3);
    CHECK(g_store.has(3));
    CHECK(!g_store.has(4));                          // one lock per pattern
    CHECK(!g_store.jumped(3));
    CHECK(g_store.tap(3) == LockTap::jumped);
    CHECK(g_store.jumped(3));
    CHECK(g_store.tap(3) == LockTap::swapped);
    CHECK(!g_store.jumped(3));
    CHECK(g_store.tap(3) == LockTap::jumped);         // an A/B compare, as often as you like
}

TEST(lockstore_a_store_or_a_pattern_change_drops_the_swap_back_and_clear_drops_all) {
    g_store.attach(g_states);
    g_store.stored(7);
    CHECK(g_store.tap(7) == LockTap::jumped);
    g_store.stored(7);                                // the state you are in is the lock now
    CHECK(!g_store.jumped(7));
    CHECK(g_store.tap(7) == LockTap::jumped);
    g_store.pattern_changed();
    CHECK(!g_store.jumped(7));
    CHECK(g_store.tap(7) == LockTap::jumped);         // a jump again, not a swap onto the wrong pattern
    g_store.clear();
    CHECK(!g_store.has(7));
    CHECK(g_store.tap(7) == LockTap::empty);
}

TEST(lockstore_an_unattached_store_holds_nothing) {
    LockStore store;
    store.stored(1);
    CHECK(!store.has(1));
    CHECK(store.tap(1) == LockTap::empty);
    CHECK(!store.has(-1));
    CHECK(!store.has(kProjectPatterns));
}

TEST(lockstore_two_states_compare_by_every_field) {
    LiveState &a = g_states[0];
    LiveState &b = g_states[1];
    a = LiveState{};
    __builtin_memcpy(&b, &a, sizeof a);
    CHECK(live_state_same(a, b));
    b.pattern.tracks[2].steps[5].flags = kStepTrig;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.params[7][31] = 1;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.level[3] = 0.5f;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.sends[kSendFields - 1] = 9;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.master[kMasterFields - 1] = 9;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.mutes = 0x80;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.macros[kMacros - 1] = 1;
    CHECK(!live_state_same(a, b));
    __builtin_memcpy(&b, &a, sizeof a);
    b.morph = 1;
    CHECK(!live_state_same(a, b));
}

// --- the engine -------------------------------------------------------------------------------

TEST(lock_engine_a_jump_brings_back_steps_sound_mixer_mutes_and_moves) {
    Engine &engine = fresh();
    put_trig(0, 0);
    set_param(1, 5, 40);
    set_level(2, 0.5f);
    set_send_byte(1, 90);
    set_master_byte(1, 20);
    post(Cmd::set_mute, 3, 0, 1);
    post(Cmd::set_macro, 0, 0, 2, 70);
    set_morph(1000);
    pump();
    const uint8_t send = engine.project().settings.sends[1];
    const uint8_t master = engine.project().settings.master[1];
    const uint16_t morph = engine.morph();

    post(Cmd::lock_store);
    pump();
    CHECK_EQ(int(lock_last()), int(kLockStored));
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas));        // "L": stored, nothing changed

    // Change everything the lock holds.
    put_trig(0, 4);
    put_trig(0, 0);
    set_param(1, 5, 100);
    set_level(2, 1.0f);
    set_send_byte(1, 10);
    set_master_byte(1, 90);
    post(Cmd::set_mute, 3, 0, 0);
    post(Cmd::set_macro, 0, 0, 2, 0);
    set_morph(0);
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas | kLockChanged));   // "L+"

    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::jumped));
    CHECK(trig_at(0, 0));
    CHECK(!trig_at(0, 4));
    CHECK_EQ(int(engine.params(1).v[5]), 40);
    CHECK_EQ(int(engine.sounding(1).v[5]), 40);       // heard in this block, not at the next step
    CHECK(engine.level(2) == 0.5f);
    CHECK_EQ(int(engine.project().settings.sends[1]), int(send));
    CHECK_EQ(int(engine.project().settings.master[1]), int(master));
    CHECK(engine.muted(3));
    CHECK_EQ(int(engine.macro_position(2)), 70);
    CHECK_EQ(int(engine.morph()), int(morph));
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas | kLockSwapReady));

    // The second tap: everything the jump replaced, sound included (R1).
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::swapped));
    CHECK(!trig_at(0, 0));
    CHECK(trig_at(0, 4));
    CHECK_EQ(int(engine.params(1).v[5]), 100);
    CHECK(engine.level(2) == 1.0f);
    CHECK(!engine.muted(3));
    CHECK_EQ(int(engine.macro_position(2)), 0);
    CHECK_EQ(int(engine.morph()), 0);
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas | kLockChanged));
    post(Cmd::set_mute, 3, 0, 0);
    post(Cmd::set_macro, 0, 0, 2, 0);
    pump();
}

TEST(lock_engine_the_machines_and_the_tempo_are_not_in_a_lock) {
    Engine &engine = fresh();
    const uint8_t machine = engine.machine_id(5);
    const uint16_t tempo = engine.project().settings.tempo_centi_bpm;
    post(Cmd::lock_store);
    pump();
    post(Cmd::set_machine, 5, 0, uint8_t(MachineId::perc) == machine ? uint8_t(MachineId::hat)
                                                                      : uint8_t(MachineId::perc));
    pump(2);
    const uint8_t switched = engine.machine_id(5);
    CHECK(switched != machine);
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::jumped));
    CHECK_EQ(int(engine.machine_id(5)), int(switched));   // the parameters only
    CHECK_EQ(int(engine.project().settings.tempo_centi_bpm), int(tempo));
}

TEST(lock_engine_one_lock_per_pattern_and_a_pattern_change_drops_the_swap_back) {
    Engine &engine = fresh();
    const int here = int(engine.project().bank) * kPatternsPerBank + int(engine.project().slot);
    post(Cmd::lock_store);
    pump();
    CHECK(engine.has_lock(here));
    CHECK(!engine.has_lock(here + 1));
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::jumped));
    // To the next pattern and back: no swap-back any more, and that pattern has no lock.
    post(Cmd::queue_pattern, 0, 0, engine.project().bank, uint8_t(engine.project().slot + 1));
    pump(2);
    pump();
    CHECK(int(engine.project().slot) == here % kPatternsPerBank + 1);
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::empty));
    CHECK_EQ(int(lock_state()), 0);
    post(Cmd::queue_pattern, 0, 0, engine.project().bank, uint8_t(here % kPatternsPerBank));
    pump(2);
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::jumped));   // a jump, not a swap onto it
}

TEST(lock_engine_a_project_load_clears_every_lock) {
    Engine &engine = fresh();
    const int here = int(engine.project().bank) * kPatternsPerBank + int(engine.project().slot);
    post(Cmd::lock_store);
    pump();
    CHECK(engine.has_lock(here));
    fresh();                                          // a blank project, through project_loaded
    CHECK(!engine.has_lock(here));
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::empty));
}

// REL ALL and LEN ALL are live state like the macro positions, and FEEL is in the pattern: a
// lock holds all three, a change to any of them marks "L+", and the swap-back restores them.
TEST(lock_engine_holds_rel_all_len_all_and_feel) {
    Engine &engine = fresh();
    post(Cmd::set_move_all, 0, 0, 0, uint8_t(int8_t(-10)));
    post(Cmd::set_move_all, 0, 0, 1, uint8_t(int8_t(20)));
    post(Cmd::set_pattern_feel, 0, 0, 3);
    pump();
    post(Cmd::lock_store);
    pump();
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas));

    post(Cmd::set_move_all, 0, 0, 0, 0);
    settle_mark();
    CHECK_EQ(int(lock_state()), int(kLockHas | kLockChanged));
    post(Cmd::set_move_all, 0, 0, 1, 0);
    post(Cmd::set_pattern_feel, 0, 0, 9);
    pump();

    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::jumped));
    CHECK_EQ(int(engine.rel_all()), -10);
    CHECK_EQ(int(engine.len_all()), 20);
    CHECK_EQ(int(engine.pattern().feel), 3);

    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(lock_last()), int(LockTap::swapped));
    CHECK_EQ(int(engine.rel_all()), 0);
    CHECK_EQ(int(engine.len_all()), 0);
    CHECK_EQ(int(engine.pattern().feel), 9);
    post(Cmd::set_pattern_feel, 0, 0, 0);
    pump();
}

TEST(lockstore_live_state_compares_rel_all_and_len_all) {
    static LiveState a;
    static LiveState b;
    CHECK(live_state_same(a, b));
    b.rel_all = 2;
    CHECK(!live_state_same(a, b));
    b.rel_all = 0;
    b.len_all = -2;
    CHECK(!live_state_same(a, b));
}
