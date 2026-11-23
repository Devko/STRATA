// TUN and PIT as notes: what pitch a machine's tuning byte puts a C-4 step at, as a name.
//
// Every pitched machine reads its tuning byte the same way at heart: "a step at note 60 (C-4)
// sounds at TUN, and the step's note transposes from there" (dsp/kick.cpp, dsp/va.cpp, ...). So
// one question has one answer on every machine: which note does a C-4 step play? The byte to
// pitch maps are the machines' own and are copied here as cents above C-4, so the UI can name
// any byte, a lock's included, without asking the audio side:
//
//   MNO, PLY   TUN   (knob - 64) * 24/64 semitones (dsp/va.cpp, dsp/vapoly.cpp)
//   SMP        PIT   (knob - 64) * 24/63 semitones (dsp/sample.cpp), "as if the sample were a C"
//   GRN        PIT   the same, on slot 8 (dsp/grain.cpp)
//   KCK SNR HAT PRC  TUN  an exponential Hz range (dsp/kick.cpp and the others), named against
//                    12-TET with A-4 = 440 Hz
//   MID        nothing: its notes go to another instrument
//
// tests/test_tuning.cpp holds these against the machines themselves, so a change to a range in
// dsp/ that is not made here fails a test rather than naming the wrong note.
//
// ## Turning in notes
//
// `tune_step` is what a knob click does in NOTE view: it moves the byte to the value nearest the
// NEXT semitone, chromatically, so one click is one note and lands as close to it as the byte
// can get. The byte stays the setting -- nothing about the sound or the file changes -- which
// is why a value between two notes is possible and shown with a '+' or '-' (`tune_text`).
//
// It is chromatic even with a scale set, on purpose. TUN is applied AFTER the scale fold, to
// every note of the line: the sequencer folds the step's note into the key and the machine then
// transposes the folded note by TUN. Moving a whole line by a scale degree does not keep it in
// the key (A minor up a whole tone is B minor, with C# and F#), so stepping TUN through the
// scale's notes would promise something it cannot do -- and would hide the notes a drum or a
// sample needs to be tuned to the key in the first place. Only an octave keeps a folded line
// in key, and twelve clicks is an octave.
//
// core/: no Enaga headers, no libm, no allocation. Control rate only.
#pragma once

#include <cstdint>

namespace strata {

// Which track parameter is `machine`'s tuning (TUN or PIT), or -1 when it has none.
int tune_param(uint8_t machine);

// The pitch a C-4 step plays with tuning byte `value`, in cents above C-4 (note 60). 0 for a
// machine with no tuning parameter.
float tune_cents(uint8_t machine, uint8_t value);

// The note nearest that pitch, 0..127.
uint8_t tune_note(uint8_t machine, uint8_t value);

// The note as text: "C-4", "F#2", and a '+' or '-' after it when the byte is kTuneMarkCents or
// more sharp or flat of it ("A-1+"). At most kTuneTextMax bytes, terminator included.
constexpr int kTuneMarkCents = 5;
constexpr int kTuneTextMax = 6;
void tune_text(uint8_t machine, uint8_t value, char *out, int max);

// The byte `semitones` notes from `value`'s nearest note, chromatically: the value whose pitch
// is closest to that note. Clamped to 0..127; moves at least one count in the direction asked
// whenever there is room, so a click is never swallowed. `value` itself for 0 semitones or a
// machine with no tuning parameter.
uint8_t tune_step(uint8_t machine, uint8_t value, int semitones);

}  // namespace strata
