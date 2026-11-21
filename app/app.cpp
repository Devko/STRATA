// STRATA: the M0 skeleton. Draws its name, the device it is running on, the audio load, and
// the state of Enaga's clock. It makes no sound and reads no files.
//
// The clock readout is here on purpose. docs/REVIEW.md 1.3 and 1.4 found that Enaga owns the
// tempo clock and that the user's CLOCK SRC setting governs it, so STRATA neither implements a
// clock nor owns the tempo -- the single most consequential correction to the original spec.
// Showing SRC and BPM at M0 proves that finding against real hardware before M1 is built on it.
//
// Text is assembled by hand: there is no printf and no libm on this platform, and the built-in
// 5 x 7 font has upper case, digits and a few symbols only (docs/SPEC.md, "Platform facts").
#include <enaga/app.hpp>
#include <enaga/clock.hpp>

#include <cstdint>

namespace {

// --- text helpers ---------------------------------------------------------

void append(char *&out, const char *text) {
    while (*text) *out++ = *text++;
}

void append_number(char *&out, uint32_t value) {
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
}

// value is tenths: 1234 prints as "123.4".
void append_tenths(char *&out, uint32_t tenths) {
    append_number(out, tenths / 10);
    *out++ = '.';
    *out++ = char('0' + tenths % 10);
}

const char *device_name(int device) {
    switch (device) {
        case ENAGA_DEVICE_SMPLTREK: return "SMPLTREK";
        case ENAGA_DEVICE_LOFI12XT: return "LOFI-12 XT";
        case ENAGA_DEVICE_CYDRUMS:  return "CYDRUMS";
        default:                    return "UNKNOWN";
    }
}

const char *clock_source_name(int source) {
    switch (source) {
        case ENAGA_CLOCK_INTERNAL: return "INT";
        case ENAGA_CLOCK_MIDI:     return "MIDI";
        case ENAGA_CLOCK_SYNC:     return "SYNC";
        default:                   return "?";
    }
}

// --- app ------------------------------------------------------------------

class Strata : public enaga::App {
public:
    bool init() override {
        enaga::log("strata m0 ready");
        return true;
    }

    // Silence, but the whole buffer is written: Enaga requires it, and the buffer is
    // interleaved stereo (L R L R), not two planes.
    void audio(const float *, float *out, uint32_t frames) override {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
    }

    void draw(enaga::Canvas &canvas) override {
        char line[32];
        char *out = line;

        canvas.clear();
        canvas.text(0, 0, "STRATA");
        canvas.text(0, 10, device_name(enaga::system::device()), 9);

        // audio_load() is per mille of the block budget, so 1000 = the whole block.
        out = line;
        append(out, "LOAD ");
        append_tenths(out, enaga::audio_load());
        *out++ = '%';
        *out = 0;
        canvas.text(0, 28, line, 12);

        out = line;
        append(out, "PEAK ");
        append_tenths(out, enaga::audio_load_peak());
        *out++ = '%';
        *out = 0;
        canvas.text(0, 38, line, 12);

        // The clock is Enaga's and unit-wide. Under an external source STRATA cannot set the
        // tempo at all, which is why M1 has to show EXT rather than a value it owns.
        const enaga::clock::State clock = enaga::clock::state();

        out = line;
        append(out, "CLK ");
        append(out, clock_source_name(int(clock.source)));
        *out++ = ' ';
        append(out, clock.running ? "RUN" : "STOP");
        *out = 0;
        canvas.text(0, 56, line, 10);

        out = line;
        append(out, "BPM ");
        append_tenths(out, uint32_t(clock.tempo * 10.0f + 0.5f));
        if (int(clock.source) != ENAGA_CLOCK_INTERNAL) append(out, " EXT");
        *out = 0;
        canvas.text(0, 66, line, 10);

        out = line;
        append(out, "PPQN ");
        append_number(out, enaga::clock::ppqn);
        *out = 0;
        canvas.text(0, 76, line, 7);

        canvas.text(0, 94, "M0 SKELETON", 6);
    }
};

}  // namespace

ENAGA_APP(Strata)
