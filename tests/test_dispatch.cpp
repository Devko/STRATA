// The engine's command dispatch, one handler at a time.
//
// core/ tests what range_copy, the scenes and the macros DO. Nothing tested that Engine::apply
// actually calls them: mutation testing deleted `range_copy(...)`, `range_transpose(...)`,
// `range_nudge(...)`, `rewire_machines()` in set_machine, and every `perform_dirty_ = true` in
// the scene and macro handlers, one at a time, and the whole suite passed each time. A deleted
// `perform_dirty_` is the worst of them: the screen shows the edit and the machines go on
// playing the performance layer as it was, which is the per-block-copy bug in a new place.
//
// Each test drives one command through the ring and reads the result straight off the engine.
#include "apprig.hpp"
#include "check.hpp"

#include "editops.hpp"
#include "pattern.hpp"
#include "perform.hpp"
#include "track.hpp"

#include <cstdio>

using namespace strata;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;
using strata::test::put_trig;

namespace {

constexpr int kLvl = 11;

const Step &step_at(int track, int step) { return strata::test::engine().pattern().tracks[track].steps[step]; }

void post_full(Cmd cmd, uint8_t track, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
               uint32_t u = 0) {
    Command command;
    command.cmd = cmd;
    command.track = track;
    command.a = a;
    command.b = b;
    command.c = c;
    command.d = d;
    command.u = u;
    strata::test::engine().commands().post(command);
}

}  // namespace

TEST(dispatch_copy_paste_transpose_and_nudge_reach_the_pattern) {
    Engine &engine = fresh();
    put_trig(0, 0);
    put_trig(0, 1);
    const uint8_t note = step_at(0, 0).note;

    post(Cmd::range_copy, 0, 0, 2);
    post(Cmd::range_paste, 0, 8);
    pump(2);
    CHECK((step_at(0, 8).flags & kStepTrig) != 0);
    CHECK((step_at(0, 9).flags & kStepTrig) != 0);
    CHECK((step_at(0, 10).flags & kStepTrig) == 0);

    post(Cmd::range_transpose, 0, 8, 2, uint8_t(int8_t(5)));
    pump(2);
    CHECK_EQ(int(step_at(0, 8).note), int(note) + 5);
    CHECK_EQ(int(step_at(0, 9).note), int(note) + 5);
    CHECK_EQ(int(step_at(0, 0).note), int(note));       // outside the range: untouched

    post(Cmd::range_nudge, 0, 8, 1, uint8_t(int8_t(3)));
    pump(2);
    CHECK_EQ(int(step_at(0, 8).micro), 3);
    CHECK_EQ(int(step_at(0, 9).micro), 0);
    (void)engine;
}

TEST(dispatch_set_machine_rewires_the_track) {
    Engine &engine = fresh();
    const Machine *before = engine.machine(4);
    post(Cmd::set_machine, 4, 0, uint8_t(MachineId::va_mono));
    pump(2);
    CHECK_EQ(int(engine.machine_id(4)), int(MachineId::va_mono));
    CHECK(engine.machine(4) != nullptr);
    CHECK(engine.machine(4) != before);
    CHECK(engine.machine(4) == engine.machine(4));

    post(Cmd::set_machine, 4, 0, uint8_t(MachineId::perc));
    pump(2);
    CHECK(engine.machine(4) == before);                  // the pool's own PERC, back again
}

TEST(dispatch_every_macro_command_reaches_what_is_heard) {
    // performed() is what the machines read. Each step pumps and reads it, so a handler that
    // forgets to mark the performance layer stale fails at its own line.
    Engine &engine = fresh();
    const uint8_t kit = engine.params(1).v[kLvl];

    post(Cmd::set_macro, 0, 0, 0, 127);                  // the knob is already at the top...
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit));  // ...but drives nothing yet

    // Assigned with the parameter at 10, heading for 110. The knob is at the top, so the range
    // runs from 110 at the bottom up to 10 at the knob: the value does not jump on assignment
    // (core/perform.hpp, macro_range_through), and the knob then sweeps it towards 110.
    post_full(Cmd::macro_assign, 1, 0, kLvl, 10, 110);
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), 10);

    post(Cmd::set_macro, 0, 0, 0, 0);                    // moved: heard at once
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), 110);

    post(Cmd::macro_clear, 0, 0, 0);                     // cleared: back to the kit
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit));

    // And the refusals are counted, both of them.
    const uint32_t refused = engine.status().refused.load(std::memory_order_relaxed);
    post_full(Cmd::macro_assign, 1, kMacros, kLvl, 0, 127);
    post_full(Cmd::macro_assign, 1, 0, kParams, 0, 127);
    pump(2);
    CHECK_EQ(engine.status().refused.load(std::memory_order_relaxed), refused + 2);
}

TEST(dispatch_every_scene_command_reaches_what_is_heard) {
    Engine &engine = fresh();
    const uint8_t kit = engine.params(1).v[kLvl];
    const uint8_t other = uint8_t(kit > 60 ? 20 : 100);

    // A at an empty scene, B at scene 2, the morph all the way to B.
    post(Cmd::scene_select, 0, 0, 0, 0);
    post(Cmd::scene_select, 0, 0, 1, 2);
    post_full(Cmd::set_morph, 0, 0, 0, 0, 0, kMorphScale);
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit)); // both ends are empty: the kit

    // Capture scene 2 from a kit with another level, then put the kit back. With the morph
    // already at B, the capture alone must change what is heard.
    post(Cmd::set_param, 1, 0, kLvl, other);
    post(Cmd::kit_store);
    pump(2);
    post(Cmd::scene_capture, 1, 0, 2, 1);                // scene 2, this track only
    pump(2);
    // Stored as well as turned back. The morph's unstored end is the STORED kit
    // (project_.kit), which only a kit_store refreshes -- see the note in the PR: turning the
    // knob alone leaves that end at the old value.
    post(Cmd::set_param, 1, 0, kLvl, kit);
    post(Cmd::kit_store);
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(other));

    post_full(Cmd::set_morph, 0, 0, 0, 0, 0, 0);         // the morph back to A
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit));

    post_full(Cmd::set_morph, 0, 0, 0, 0, 0, kMorphScale);
    pump(2);
    post(Cmd::scene_select, 0, 0, 1, 3);                 // B to another, empty, scene
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit));

    post(Cmd::scene_select, 0, 0, 1, 2);
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(other));
    post(Cmd::scene_erase, 0, 0, 2);                     // erased under the morph
    pump(2);
    CHECK_EQ(int(engine.performed(1, kLvl)), int(kit));
}

TEST(dispatch_a_pad_hit_flashes_its_track) {
    Engine &engine = fresh();
    const uint8_t before = engine.status().triggered[2].load(std::memory_order_relaxed);
    post(Cmd::trigger_note, 2, 0, 60, 100);
    pump(2);
    CHECK_EQ(int(engine.status().triggered[2].load(std::memory_order_relaxed)),
             int(uint8_t(before + 1)));
}
