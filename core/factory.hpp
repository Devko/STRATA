// The factory content: what a project holds before anyone has touched it.
//
// One definition, used in both places a project starts from nothing:
//
//   - `Engine::init`, which is what the instrument IS with no card, or on a first run;
//   - `Storage::start_new` (PROJECT, F1 + RIGHT), the user's NEW project.
//
// Those two used to disagree. `Engine::init` built a kit and a beat by hand in app/, while NEW
// ran `project_init`, whose kit is all zeroes -- LVL 0 on every track, so a new project was
// silent until the user turned up eight levels, and had nothing in any pattern. Moving the
// content here, into core/, puts it next to the format it is written in and under host tests
// (tests/test_factory.cpp) that load it, save it and read it back.
//
// ## What it holds
//
//   - the kit: the eight machines of docs/SPEC.md's default techno kit, each set up to sound
//     like the instrument it is named after (docs/SOUND.md's references), tuned to the key and
//     balanced against each other;
//   - the sends and the master: delay and reverb set for dub chords, a little delay into the
//     reverb;
//   - bank A, patterns 1-4: GROOVE, GROOVE+, BREAK and PEAK, which between them use locks,
//     trig conditions, probability, ratchets, slides, accents, chords, swing and polymeter;
//   - a chain over those four, built but OFF -- STRATA is a jamming instrument, and a chain
//     that advanced on its own the first time someone pressed PLAY would fight them;
//   - eight mute states, three scenes and three macros (B ACID, C DUB, D DRIVE; A is left for
//     pad pressure), so the performance row and PERFORM do something from the first minute.
//
// Banks B-H are empty, so a user's own work starts on a clean page.
//
// ## Why parameter indices are numbers here
//
// core/ does not include dsp/ (core/pattern.hpp), so `page_first` is not available. The page
// layout is dsp/track.hpp's -- SRC 0-3, FLTR 4-7, AMP 8-11, FX 12-15 -- and app/engine.cpp
// static_asserts that the constants below agree with it.
//
// Content, not behaviour: tests/test_factory.cpp pins that the content exists, is valid, loads
// and plays, not which note is on which step.
#pragma once

#include "perform.hpp"
#include "project.hpp"

namespace strata {

// The first parameter index of each page, as dsp/track.hpp lays them out.
constexpr int kFactorySrc = 0;
constexpr int kFactoryFltr = 4;
constexpr int kFactoryAmp = 8;
constexpr int kFactoryFx = 12;

// How many of bank A's patterns the factory fills.
constexpr int kFactoryPatterns = 4;

// Everything a fresh project holds: `project_init` and `perform_init` first, then the content
// above. Main context or init only, like `project_init`, and for the same reason: it rewrites
// about a megabyte of pattern memory.
void factory_project(Project &project, SceneSet &scenes, MacroSet &macros);

}  // namespace strata
