// What the screen draws a sample from: a few hundred bytes made once, when the sample loads.
//
// docs/CONTROLS.md, "Waveforms and envelopes". SAMPLE's SRC page draws the track's sample with
// its START/END window, its slices and its playheads; GRAIN's draws it with the grains in the
// air. `draw()` runs in the main context every 50 ms and Enaga's main-loop watchdog is 1000 ms,
// so nothing there may walk sample memory -- a 30 s sample is 5.5 MiB. Instead:
//
//   - WaveOverview: the sample's lowest and highest value in each of kWaveColumns columns, one
//     signed byte each. 250 bytes a track. OverviewBuilder makes it from the PCM as the loader
//     writes it into the pool (app/storage.cpp), in the same slices and beside the same call
//     that finds the slice points (core/onsets.hpp), so there is no second pass over the file.
//   - WaveView: where the machine is in that sample right now, in the same columns: the window,
//     the slice starts, and up to eight playheads or grains with a brightness each. The audio
//     side fills it every few blocks (dsp/sample.hpp, dsp/grain.hpp) and publishes it through
//     Status (app/commands.hpp) as single-writer bytes and words.
//
// A column is frames [ceil(c * total / K), ceil((c + 1) * total / K)), K = kWaveColumns, so frame
// f is in column floor(f * K / total) -- the same arithmetic wave_column() does in float on the
// audio side, which is what lines a playhead up with the overview under it.
//
// Main context only for the builder. Integer only: no float, no libm, no allocation. Per frame
// it is one add, one shift and two compares, against the conversion the loader is already doing
// on the same frames. A division happens once per COLUMN (kWaveColumns of them per sample).
//
// core/ never includes Enaga headers, and this is plain data in, plain data out.
#pragma once

#include <cstdint>

namespace strata {

// x 0-123: the frame's meter owns x 126-127 (app/app.hpp, kFrameMeterX).
constexpr int kWaveColumns = 124;

struct WaveOverview {
    int8_t lo[kWaveColumns] = {};     // the column's lowest value, sample >> 8
    int8_t hi[kWaveColumns] = {};     // and its highest
    // Columns that hold data, from the left. All of them for a sample in RAM; a streamed file's
    // head fills only the first few, because the rest of the file is never read at load.
    uint8_t filled = 0;
    bool ready = false;               // finished; false while loading, or for nothing at all
};

class OverviewBuilder {
public:
    // Start an overview of a sample that will be `total` frames long, into `out` (which this
    // clears and marks not ready). `out` must outlive the build.
    void begin(WaveOverview &out, uint32_t total);

    // The next `frames` frames of interleaved int16 (1 or 2 channels; a stereo pair is drawn as
    // its mid, (L + R) / 2, which is what the mono machines play), `first` being the number of
    // the first of them. Frames must come in order: a feed that does not start where the last
    // ended marks the build broken, and finish() then leaves the overview not ready.
    void feed(const int16_t *pcm, uint32_t first, uint32_t frames, int channels);

    // The sample is `frames` long after all -- normally `total`. A file shorter than its header
    // said hands the sampler fewer frames than begin() was told, and the columns are stretched
    // over what is there, so they still line up with the machine's. Marks the overview ready.
    void finish(uint32_t frames);

    // A streamed file: only its head was fed. The columns keep the WHOLE file's scale (the
    // stream voice's playhead runs over the whole file), and `filled` says how many hold data.
    void finish_head();

    uint32_t fed() const { return next_; }

private:
    void close_column();
    uint32_t boundary(uint32_t column) const;   // the first frame of `column`

    WaveOverview *out_ = nullptr;
    uint32_t total_ = 0;
    uint32_t next_ = 0;            // the frame the next feed must start at
    uint32_t column_ = 0;          // the column being filled
    uint32_t column_end_ = 0;      // the first frame of the next column
    int32_t lo_ = 0;
    int32_t hi_ = 0;
    bool open_ = false;            // the column being filled has a frame in it
    bool broken_ = true;
};

// Where a machine is in its sample, in the overview's columns. Filled by the audio side.
struct WaveView {
    static constexpr int kMarks = 8;
    static constexpr uint8_t kNone = 0xFF;

    // SAMPLE: the START..END window, both inclusive. GRAIN: the bracket where the next grain
    // starts, POS (plus SCN's travel) to POS plus one grain's span. kNone when there is none.
    uint8_t lo = kNone;
    uint8_t hi = kNone;
    // A bit per column in which a slice starts (SAMPLE, SLC above 1): column c is bit c % 32 of
    // cuts[c / 32].
    uint32_t cuts[4] = {};
    // SAMPLE's playing voices, GRAIN's grains: a column each, kNone for none, and how bright to
    // draw it, 1-15 (the voice's envelope; the grain's window times the cloud's envelope).
    uint8_t mark[kMarks] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    uint8_t glow[kMarks] = {};
};
static_assert(kWaveColumns <= 32 * 4, "cuts holds a bit per column");
static_assert(kWaveColumns < WaveView::kNone, "a column is never kNone");

// The column of `frame` in a sample `scale` = kWaveColumns / its frames maps, clamped onto the
// overview. Float, for the audio side, where the scale is worked out once per sample.
inline uint8_t wave_column(float frame, float scale) {
    const float c = frame * scale;
    return c <= 0.0f ? uint8_t(0)
                     : (c >= float(kWaveColumns - 1) ? uint8_t(kWaveColumns - 1) : uint8_t(c));
}

inline void wave_set_cut(WaveView &view, uint8_t column) {
    if (column < kWaveColumns) view.cuts[column >> 5] |= 1u << (column & 31u);
}

inline bool wave_has_cut(const uint32_t *cuts, int column) {
    return column >= 0 && column < kWaveColumns && (cuts[column >> 5] >> (column & 31)) & 1u;
}

}  // namespace strata
