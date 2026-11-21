// The shared rig for every app/ test: one Engine, one Storage, and a way to drive both contexts.
//
// ## Why it is shared
//
// `Engine::init` takes about 85 MiB -- a 64 MiB sample region, 18 MiB of loader staging and the
// 2.93 MiB repeat ring -- and allocates it with `new`, once, as CLAUDE.md requires. One engine
// per test FILE would be 85 MiB per file. So there is exactly one, here, and `init` runs on the
// first call to `fresh()`.
//
// ## Why both contexts have to be driven by hand
//
// The audio side and the main context meet at a 16-byte SPSC ring and a single-writer Status.
// `Storage::service()` WAITS while the ring is non-empty -- that is how a save is deferred past
// every pending edit (app/storage.cpp) -- and only `Engine::process()` drains it. A test that
// calls one without the other therefore waits forever. `pump()` runs a block of audio and then
// one pass of the card, which is the order `App::audio` and `App::draw` really run in.
//
// Driving them by hand is also the point: the emulator runs them in lockstep at real speed and
// cannot hold either one still.
#pragma once

#include <cstdint>

#include <enaga/abi.h>

#include "commands.hpp"
#include "engine.hpp"
#include "storage.hpp"

namespace strata::test {

constexpr int kRigBlock = int(ENAGA_BLOCK_FRAMES);
constexpr int kRigStereo = 2 * kRigBlock;

// The one engine and the one storage. `fresh()` is what makes them safe to use.
Engine &engine();
Storage &storage();

// Back to a known state: a blank card after a SWAP (which is what the fake's reset looks like to
// the app -- see the note in the implementation), an internal clock at 130 BPM, a new project
// with a kit that can make a sound, no voices ringing, and an empty ring.
//
// The project is BLANK, not the default beat: `Engine::init` loads the default beat once, and a
// test that inherited it would also inherit whatever the test before it did to it. Every test
// that wants a note puts its own trig down.
Engine &fresh();

// Run `blocks` audio blocks, servicing the card between each. Returns the loudest sample seen,
// so a caller that only cares whether something sounded needs no loop of its own.
float pump(int blocks = 1);

// Wait for a card job to START and then finish. Returns false if none ever ran, which is a real
// answer and not only a timeout: with no usable card `service()` returns before starting
// anything, and a save asked for then simply stays pending.
//
// It pumps BEFORE it looks, because `request_save` only sets a flag -- the job does not begin
// until `service()` finds the ring drained, so a check made first sees "idle" and a test then
// asserts about a save that never ran.
bool pump_until_idle(int limit = 4000);

// Pump until the output has been quiet for `quiet_blocks` blocks in a row, and return true if it
// got there. For a test that needs to measure WHERE in a block a sound started, which cannot be
// done while the block before it is still ringing.
//
// Two things make this necessary rather than fussy. `fresh()` cuts the voices with a kill, and a
// kill at a non-zero amplitude is a step in the output -- a click, which the master reverb then
// rings on for seconds. And the master DELAY is silent BETWEEN its 0.375 s repeats, so a single
// silent block proves nothing: an echo left in the line pops out a third of a second later and
// looks like a bug in whatever is being measured. Hence "quiet for a run of blocks", not "quiet".
bool settle(int quiet_blocks = 400, int limit = 4000);

// The last block `pump()` rendered, interleaved stereo, `kRigStereo` floats. For the tests that
// care WHERE in a block something happened rather than whether it happened at all.
const float *last_block();

// Post a command the way app/app.cpp's `post` does, minus the dirty-flag bookkeeping (a test
// that wants that calls Storage directly, which is what app/app.cpp's `post_command` does).
void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
          uint8_t c = 0);

// A delta command, clamped the way `post_delta` clamps it.
void post_delta(Cmd cmd, int track, int step, uint8_t a, int delta);

// The kit value the engine publishes for a track's parameter -- what the UI draws and what the
// machine is prepared with, which must agree.
uint8_t kit_value(int track, int param);

// Put a trig down on a step and let the command land.
void put_trig(int track, int step);

// The peak of a buffer, since every test wants it.
float peak_of(const float *x, int n);

}  // namespace strata::test
