// Placing imported steps (tools/midi_import/grid.py emit_cpp) into a pattern.
//
// Include after tools/showcases/showcase_head.inc (it uses its `trig`). A generator includes the
// tables midi2strata.py wrote, then places windows of them:
//
//     place(p, kBass, kImport_bass_a);                // all of it, as written
//     place(p, kBass, kImport_bass_a, 64, 64);        // source steps 64..127 into 0..63
//     place(p, -1, kImport_drums_a);                  // drum rows carry their own track
//     place(p, kLead, kImport_lead_a, 0, 32, 32, 12); // 2 bars, at step 32, an octave up
//
// Not part of the device or test build, like the showcase generators.
#pragma once

#include <cstddef>
#include <cstdint>

namespace {

struct ImportedStep {
    int16_t step;      // 16th index in the source
    uint8_t note;
    uint8_t vel;
    uint8_t length;    // core/timing.hpp length index
    int8_t micro;      // -11..+11 ticks
    uint8_t chord;     // chord_pack byte, 0 for one note
    int8_t track;      // drums: the STRATA track; -1 = the caller's; -2 = an empty table's filler
};

// Rows with `from <= step < from + count` land at `step - from + at`, wrapped into the
// pattern's 64 steps (a window may start late and wrap). `track` < 0 takes each row's own
// track. `micro` false drops the micro-timing (dead straight). Returns how many it placed.
template <size_t N>
int place(Pattern &p, int track, const ImportedStep (&rows)[N], int from = 0, int count = 64,
          int at = 0, int transpose = 0, bool micro = true, int vel_percent = 100) {
    int placed = 0;
    for (const ImportedStep &r : rows) {
        if (r.track == -2 || r.step < from || r.step >= from + count) continue;
        const int t = track >= 0 ? track : r.track;
        if (t < 0 || t >= kTracks) continue;
        const int s = (r.step - from + at) % 64;
        int note = r.note + transpose;
        note = note < 0 ? 0 : (note > 127 ? 127 : note);
        int vel = r.vel * vel_percent / 100;
        vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
        Step &st = trig(p, t, s, uint8_t(note), uint8_t(vel), r.length);
        st.micro = micro ? r.micro : 0;
        st.chord = r.chord;
        placed++;
    }
    return placed;
}

}  // namespace
