// The WAVE machines' wavetables: sixteen built-in tables, computed in init() and never touched
// again.
//
// Ported from PolyWerk (github.com/Devko/PolyWerk, dsp/wavetable.cpp; MIT, Copyright Devko), the
// same author's wavetable synth for the Akai Force. The recipes -- which spectrum each frame
// has, and how the frames morph -- are PolyWerk's. What changed for STRATA:
//
//   - Smaller. 16 frames a table, not 32, and 1,024 samples at the top level rather than 2,048
//     (512 harmonics: at 48 kHz that still reaches 20 kHz on a 40 Hz bass). 10,260 bytes a frame,
//     164 KB a table, 2.5 MiB for all sixteen. PolyWerk's layout would have been 4.4 MiB.
//   - No libm, no std::vector, no doubles. The builder runs once, in init(), in float, with
//     Enaga's math (sin_turns has an absolute error under 4e-6, so the FFT's twiddles put its
//     own error some 100 dB down) and with scratch it allocates once.
//   - The time-domain recipes (sync, fold, FM...) are sampled 4x finer than a frame instead of
//     8x. What the naive rendering aliases into the kept harmonics is then at most -71 dB
//     instead of -78, and the build takes half as long.
//   - Tables that need to be played octaves down (Organ, Bell) are left out: on STRATA a note
//     should sound at the note.
//
// The format is PolyWerk's. A frame is kWaveMipLevels pre-filtered copies of one cycle; level k
// keeps harmonics 1..(512 >> k) and is stored at its own length (short for the high levels,
// which have few harmonics), each followed by a guard sample equal to its first. Samples are
// int16 with one float scale per frame, so a quiet frame keeps its resolution. The oscillator
// (dsp/waveloops.ti.c) picks the level whose top harmonic stays clear of aliasing at the pitch
// it plays, so the inner loop is two lookups and no filtering.
#pragma once

#include <cstdint>

namespace strata {

constexpr int kWaveTables = 16;
constexpr int kWaveFrames = 16;
constexpr int kWaveTopBits = 10;
constexpr int kWaveTopLength = 1 << kWaveTopBits;   // samples of level 0
constexpr int kWaveHarmonics = kWaveTopLength / 2;  // 512
constexpr int kWaveMipLevels = 10;                  // 512, 256, ... 1 harmonics

// Samples of level k = 1 << kWaveMipBits[k] = clamp(8 * (512 >> k), 256, 1024): linear
// interpolation wants about eight samples a cycle of the top harmonic, capped at the top length.
constexpr int kWaveMipBits[kWaveMipLevels] = {10, 10, 10, 9, 8, 8, 8, 8, 8, 8};

constexpr int wave_mip_length(int k) { return 1 << kWaveMipBits[k]; }
constexpr int wave_mip_offset(int k) {
    return k == 0 ? 0 : wave_mip_offset(k - 1) + wave_mip_length(k - 1) + 1;
}
constexpr int kWaveFrameStride =
    wave_mip_offset(kWaveMipLevels - 1) + wave_mip_length(kWaveMipLevels - 1) + 1;
static_assert(kWaveFrameStride == 5130, "mip layout changed: update the header comment");

constexpr uint32_t kWaveLibraryBytes =
    uint32_t(kWaveTables) * kWaveFrames * kWaveFrameStride * sizeof(int16_t);

// The highest harmonic h allowed at phase increment `inc` (cycles a sample) is the one whose
// alias folds back no lower than 18 kHz: h * inc <= (48 - 18) / 48. The top octave stays bright
// instead of every note being dulled to the strict Nyquist limit -- PolyWerk's rule, at 48 kHz.
constexpr float kWaveAliasLimit = 0.625f;

// The mip level to play at increment `inc`. Control rate; a loop of compares, no division.
inline int wave_mip_for(float inc) {
    int k = 0;
    while (k < kWaveMipLevels - 1 && float(kWaveHarmonics >> k) * inc > kWaveAliasLimit) k++;
    return k;
}

class WaveLibrary {
public:
    // Allocates the tables and builds all sixteen at once: begin(), then step() until it is
    // done. Tests and tools; the app builds a little at a time.
    bool init(void *scratch = nullptr, uint32_t scratch_bytes = 0);

    // The build in pieces. On the C6748 all sixteen tables take longer than Enaga's 1 s
    // main-loop watchdog, so the app cannot build them inside its init(): begin() allocates
    // (init only), and step(), from the main context's update(), does one piece -- a table's
    // recipe, or two of its frames at every mip level -- and returns true once the last is done.
    // 16 x 9 = 144 pieces; the order and the arithmetic are init()'s, so the tables are bit-for-
    // bit the same. The builder's scratch (wave_build_scratch_bytes) is carved from `scratch`
    // when it is at least that big plus 64 bytes: memory the caller keeps idle until step()
    // returns true, when it is zeroed and handed back. Else it is allocated.
    bool begin(void *scratch = nullptr, uint32_t scratch_bytes = 0);
    bool step();
    bool ready() const { return data_ != nullptr; }
    static constexpr int kBuildSteps = kWaveTables * (1 + kWaveFrames / 2);

    // Level `mip` of `frame` of `table`: wave_mip_length(mip) samples plus the guard sample.
    // Arguments are clamped, so a stale byte can never read outside the library.
    const int16_t *frame(int table, int frame, int mip) const;
    // The frame's scale: sample * scale is the value, peak 1 over the table.
    float scale(int table, int frame) const;

    // The table's name for the screen: three characters ("CLS", "PWM"...) and a long one.
    static const char *short_name(int table);
    static const char *long_name(int table);

private:
    int16_t *data_ = nullptr;       // set once the build is done: the tables to read
    int16_t *building_ = nullptr;   // the same memory while step() is filling it
    uint8_t *lent_ = nullptr;       // the borrowed scratch, zeroed when the build is done
    uint8_t *lent_end_ = nullptr;
    int next_table_ = 0;
    int next_pair_ = -1;            // -1: the table's recipe is next
    bool normalise_ = true;
    float scale_[kWaveTables][kWaveFrames] = {};
};

// The library is the same for every track and every instance, and built once: Engine::init
// begins it, the main context's update() steps it, and the engine publishes it here when it is
// done. The machines read it through this pointer (atomic: set by the main context, read by the
// audio one). nullptr until then, and a machine with no library plays silence rather than
// reading through it.
const WaveLibrary *wave_library();
// What WaveLibrary::init takes besides the tables: the builder's scratch (tests/test_memory.cpp).
uint32_t wave_build_scratch_bytes();
void set_wave_library(const WaveLibrary *library);

}  // namespace strata
