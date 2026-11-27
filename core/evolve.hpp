// EVOLVE: a track that mutates on its own, every 1, 2, 4 or 8 passes of the pattern.
//
// Roland, 2026-11-27: "and than have option to mutate it, e.g. per loop or every 4x loops". The
// MUTATE pad is one variation a press, kept or reverted at once (core/mutate.hpp). EVOLVE runs
// the same mutations unattended, which changes what "undoable" has to mean: sixteen undo levels
// are gone in sixteen passes, and a set that wandered for ten minutes cannot be stepped back
// through anyway. So EVOLVE keeps its own way back instead, and spends no undo level at all.
//
// ## HOME
//
// Turning EVOLVE on takes a copy of the track's steps: its HOME, RAM only. Two ways to evolve:
//
//   ORBIT  every turn starts again from HOME: restore, then mutate. The part varies around
//          itself and never loses its identity, however long it runs. The default.
//   DRIFT  every turn mutates what is playing, so the changes add up and the part wanders off.
//
// KEEP makes what is playing the new HOME and stops; HOME puts it back and stops. The periods,
// the mode, the mutation and the amount are saved with the project (core/project.hpp); HOME is
// not, so a project opens evolving from the pattern as it was saved. Turning EVOLVE
// off from the menu is a KEEP: off never changes what you hear.
//
// ## Editing a track that is evolving
//
// In ORBIT a restore would throw away an edit made since the last turn -- a trig typed in, a
// note recorded -- which on a jam box is the one thing it must not do. So a turn first compares
// the track with what the last turn LEFT, step by step, and every step that differs was edited:
// it goes into HOME as it is now. The edit survives every later turn, and the steps nobody
// touched keep orbiting. A lock turned on a step changes the lock pool and not the step, so it
// needs no merging: a restore copies steps and never touches the pool.
//
// ## Chords
//
// A NOTES mutation moves a step's extra notes with it (core/stepnotes.hpp), and those live in
// the lock pool, which copying steps does not touch. So HOME and the last turn also hold each
// step's five extra-note values, and a restore writes them back into the locks the restored
// step points at. A chord edited since the last turn counts as an edit, like a step.
//
// ## Where it runs
//
// In the audio context, on the boundary of a pass (Engine::cross_boundary), before the first
// step of the next pass is resolved: one copy of at most 64 steps and one mutation per evolving
// track, at most once a pass. Nothing allocates; the two copies per track are the engine's own.
#pragma once

#include "mutate.hpp"
#include "pattern.hpp"
#include "stepnotes.hpp"

#include <cstdint>

namespace strata {

constexpr uint8_t kNoExtra = 0xFF;

enum class EvolveMode : uint8_t { orbit = 0, drift, count };

const char *evolve_mode_name(EvolveMode mode);

// The periods, in passes of the pattern. Index 0 is OFF. Stored as the index.
constexpr int kEvolvePeriods = 5;
constexpr uint8_t kEvolvePeriod[kEvolvePeriods] = {0, 1, 2, 4, 8};

// "OFF", "1 LOOP", "2 LOOPS" ...
const char *evolve_period_name(int index);

// One track's EVOLVE.
struct Evolve {
    uint8_t period = 0;               // index into kEvolvePeriod; 0 is off
    uint8_t passes = 0;               // passes since the last turn
    TrackPattern home;                // what ORBIT returns to; HOME restores
    TrackPattern last;                // the track as the last turn left it
    // Each step's extra notes (lock ids 32-36) in HOME and as last left, kNoExtra where the
    // step has none with that id.
    uint8_t home_extra[kStepsPerTrack][kStepExtraNotes];
    uint8_t last_extra[kStepsPerTrack][kStepExtraNotes];

    bool on() const { return period != 0; }
};

// Turns EVOLVE on at `period` (an index; 0 is the same as evolve_keep), taking HOME from the
// track as it is now. Already on, it only changes the period: HOME stays.
void evolve_set(Evolve &evolve, const Pattern &pattern, int track, int period);

// Counts one pass. On every period-th pass it turns: merges edits into HOME, restores HOME in
// ORBIT, applies `kind` at `amount` with `variant`, and records what it left. Reports whether
// the track changed. Off, it does nothing.
bool evolve_pass(Evolve &evolve, Pattern &pattern, int track, EvolveMode mode, Mutation kind,
                 int amount, uint32_t variant);

// The two halves of evolve_pass, for a caller that turns a track a little BEFORE the pass ends
// (Engine spreads the turns of eight tracks over the last ticks of a pass, one a tick, so they
// never land in one audio block). `evolve_due`: will the pass now ending be a turn? `evolve_turn`:
// turn now, and count this pass as the turn's. `evolve_count`: a pass ended without a turn.
bool evolve_due(const Evolve &evolve);
bool evolve_turn(Evolve &evolve, Pattern &pattern, int track, EvolveMode mode, Mutation kind,
                 int amount, uint32_t variant);
void evolve_count(Evolve &evolve);

// KEEP: what plays stays and becomes HOME, and EVOLVE stops.
void evolve_keep(Evolve &evolve);

// HOME: the track back to HOME (edits since the last turn kept, as a turn keeps them), and
// EVOLVE stops. Reports whether the track changed. Off, it does nothing.
bool evolve_home(Evolve &evolve, Pattern &pattern, int track);

}  // namespace strata
