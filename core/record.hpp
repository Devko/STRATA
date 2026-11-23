// Live recording: turning a hit that happened at some moment into a step with micro-timing.
//
// docs/SPEC.md, "Recording": "Each hit is stamped with the audio clock's sample position minus
// the measured pad latency (about 4-8 ms), then quantized by a 0-100% amount", and the review's
// addition: "Incoming MIDI notes go through the same stamping, because on_midi is main context
// and carries no timestamp of its own."
//
// ## What a stamp is, and why it is not samples
//
// Enaga exposes no sample counter, but `clock::state()` gives `.position` (whole ticks) and
// `.fraction` (0-1 into the next), readable from the main context. That is a musical timestamp
// of the moment the event was dispatched, which is exactly what a quantizer wants -- and it
// needs no agreement between the two contexts about a counter. A sample position would have to
// be converted to ticks here anyway, at a tempo that may have changed since.
//
// The error left over is Enaga's event dispatch latency, not the quantizer's. Pad latency is
// compensated explicitly, because it is a constant of the hardware rather than of the software.
//
// ## Why core/ and not app/
//
// Because it is arithmetic, and because inverting swing is easy to get subtly wrong. A hit
// played exactly on a swung odd step must record as micro 0, not as micro +3: the stored micro
// is an offset from where the step ALREADY sits, after swing. Getting that backwards is
// inaudible on a straight pattern and wrong on every swung one, which is the worst kind of bug
// to find by ear. Here it is a test.
#pragma once

#include "timing.hpp"

#include <cstdint>

namespace strata {

// --- stamps ---------------------------------------------------------------

// A moment, as `enaga::clock::state()` reports it. core/ includes no Enaga headers, so the two
// fields are mirrored rather than imported; app/ fills them from `.position` and `.fraction`.
struct Stamp {
    uint32_t tick = 0;
    float fraction = 0.0f;   // 0 <= fraction < 1
};

// The spec's "about 4-8 ms". The real figure is a unit measurement (docs/UNIT_CHECKLIST.md);
// this is the default until it is measured.
constexpr float kPadLatencyMs = 6.0f;

// Ticks per millisecond at a tempo: bpm * kPpqn / 60000.
float ticks_per_ms(float bpm);

// `fraction` folded back into 0..1 with any whole ticks carried into `tick`. A negative
// fraction borrows; the result never goes below tick 0, fraction 0.
Stamp stamp_normalise(Stamp stamp);

// `now` moved back by a latency in milliseconds, which is what turns the moment an event was
// DISPATCHED into the moment the pad was actually struck. Clamped at zero: at the very start of
// a transport there is nothing to move back into.
Stamp stamp_back(Stamp now, float latency_ms, float tempo_bpm);

// --- quantizing -----------------------------------------------------------

// Where a hit lands on one track's grid.
struct Landing {
    uint8_t step = 0;       // 0 .. timing.length-1
    uint32_t loop = 0;      // which pass of the loop it belongs to, after any wrap
    int8_t micro = 0;       // what to store in Step::micro: -kMicroRange .. +kMicroRange
    bool clamped = false;   // the hit was further from the step than micro can express
};

// Resolve a hit to a step, a loop and a micro-timing value.
//
// `quantize_percent` is 0-100: 0 records where it was played (micro carries the whole offset),
// 100 snaps hard (micro is exactly 0), and in between micro is scaled by (100 - q) / 100.
//
// Exactly how, because the details are the contract:
//
//   1. Reduce the stamp into the track's loop: integer `loop` and `within`, plus the fraction.
//   2. Every step s of the track sits at effective tick s * step_ticks + swing_offset(s), the
//      same function core/timing uses -- swing included, the step's own stored micro NOT (that
//      is what is being computed). The step before the loop's start and the step after its end
//      are candidates too, so a hit just before step 0 quantizes forward into the next pass
//      rather than back to the last step of this one.
//   3. The chosen step is the candidate nearest the hit. A tie goes to the earlier one.
//   4. micro is the remaining offset scaled by (100 - quantize) / 100, rounded to nearest, and
//      clamped to +/- kMicroRange. `clamped` says the clamp changed the value, so the UI can
//      show that a hit could not be stored where it was played. Rounding to the tick grid is
//      not clamping -- it always happens and is not worth reporting.
//
// Pure: the same arguments always give the same Landing. No allocation; safe from audio().
Landing quantize_hit(Stamp hit, const TrackTiming &timing, int quantize_percent);

// --- how long a note was held ---------------------------------------------
//
// A KEYS pad, a STEP key or a MIDI note held while recording: the step it landed on gets the
// hold as its LEN (core/timing.hpp's note-length table), so a sustaining machine plays it back
// for as long as it was played. `down` and `up` are the two stamps, taken the same way (the pad
// latency is the same at both ends, so neither is moved back).
//
// The table entry NEAREST the hold, measured as a ratio rather than a difference -- the table
// doubles at its long end, so "nearest" in ticks would round a 5-step hold up to 6 but a
// 40-step hold down to 32. The boundary between two entries is their geometric mean, compared
// squared so nothing takes a root. A hold shorter than the first entry is the first entry; one
// longer than the last is the last (64 steps). An `up` before `down` (a clock that jumped back)
// is the shortest entry.
//
// Pure, and a valid index (1..kLengthMax) whatever it is given.
uint8_t held_length_index(Stamp down, Stamp up, const TrackTiming &timing);

}  // namespace strata
