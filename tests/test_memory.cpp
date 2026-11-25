// What init() takes from the heap, measured, against what the unit has.
//
// docs/SPEC.md budgets memory in a table (76 MiB of about 124) that predates the loader: the
// 18 MiB of staging in app/storage was never in it, and nothing checked the total. On the unit a
// failed `new` is not a nullptr to handle -- Enaga's runtime calls `enaga::fatal("out of
// memory")` -- so a budget that has crept past the heap is an app that will not start, and the
// first place anyone would learn that is the unit. This file is where it is learned instead.
//
// ## How the heap is sized
//
// <enaga/abi.h>: the image is linked at ENAGA_APP_BASE and the heap runs from the end of the
// image (payload plus bss, rounded up to 4 KiB) to ENAGA_APP_LIMIT. That is 124 MiB less the
// image. The image is a device-build figure this suite cannot produce, so it is a stated
// allowance here, checked against the last measured build (docs/HANDOFF.md: payload 1,104,992 +
// bss 1,565,120 bytes, cl6x). The App object, and the Engine and Storage in it, live in that bss:
// ENAGA_APP makes the app a function-local static.
//
// ## How an allocation is costed
//
// As Enaga's runtime does it (third_party/enaga/runtime/runtime.cpp, heap_alloc): an 8-byte
// header, rounded up to 8, at least 16. Allocation happens once, in init(), and nothing is
// freed, so a first-fit list never fragments and the sum is the whole story.
#include "check.hpp"
#include "fakehost.hpp"

#include "capture.hpp"
#include "engine.hpp"
#include "repeat.hpp"
#include "storage.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <enaga/abi.h>

namespace {

// --- a counting operator new, for this whole test binary --------------------------------------
//
// Replacing the global allocation functions is one-per-program, and this file is the one. It
// changes nothing for any other test: it forwards to malloc and free exactly as libstdc++'s own
// does, and the counters only move while `g_counting` is set.
bool g_counting = false;
uint64_t g_requested = 0;     // bytes asked for
uint64_t g_device = 0;        // bytes Enaga's heap would take for the same requests
int g_allocations = 0;

uint64_t device_cost(std::size_t bytes) {
    uint64_t need = (uint64_t(bytes) + 8u + 7u) & ~uint64_t(7);
    return need < 16u ? 16u : need;
}

void *counted(std::size_t bytes) {
    if (g_counting) {
        g_requested += bytes;
        g_device += device_cost(bytes);
        g_allocations++;
    }
    void *pointer = std::malloc(bytes == 0 ? 1 : bytes);
    if (pointer == nullptr) std::abort();     // -fno-exceptions: no bad_alloc to throw
    return pointer;
}

}  // namespace

void *operator new(std::size_t bytes) { return counted(bytes); }
void *operator new[](std::size_t bytes) { return counted(bytes); }
// The nothrow forms too, which dsp/wavetable.cpp (and the STRING voice) use. Without these,
// libstdc++'s own nothrow new calls the counted one above -- but under ASan the sanitizer
// intercepts the nothrow forms directly, and the tables went uncounted (CI, PR #122).
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept { return counted(bytes); }
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept { return counted(bytes); }
void operator delete(void *pointer, const std::nothrow_t &) noexcept { std::free(pointer); }
void operator delete[](void *pointer, const std::nothrow_t &) noexcept { std::free(pointer); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }

using namespace strata;

namespace {

constexpr uint64_t kMiB = 1u << 20;

// The whole span the image and the heap share.
constexpr uint64_t kAppSpan = uint64_t(ENAGA_APP_LIMIT) - uint64_t(ENAGA_APP_BASE);

// The last measured device image (docs/HANDOFF.md, tools/build.sh), payload plus bss.
constexpr uint64_t kImageMeasured = 1104992u + 1565120u;

// What this file lets the image grow to before it fails. Twice the measured image: generous
// enough that ordinary work never trips it, small enough that a doubling -- a pattern bank
// pulled into bss, say -- is noticed. Raise it with a fresh measurement, not to make this pass.
constexpr uint64_t kImageAllowance = 4u * kMiB;
static_assert(kImageMeasured <= kImageAllowance, "the image allowance is below the last build");

// Heap left after the image, rounded the way Enaga rounds it.
constexpr uint64_t kHeap = kAppSpan - ((kImageAllowance + 4095u) & ~uint64_t(4095));

// What must stay free after init(). Nothing in STRATA allocates after init() (CLAUDE.md), so
// this is not working space for the app; it is room for the docs/SPEC.md allowances that have no
// allocation yet (recording ring and stream buffers, 1 MiB) and for growth. 8 MiB is the
// smallest margin worth calling one: two Enaga reverbs and a second repeat ring.
constexpr uint64_t kHeadroom = 8u * kMiB;

// One pair of its own, not the shared rig's: the rig's init() has already run by the time any
// given test does, so its allocations cannot be counted. File scope, not locals: the objects are
// the size of the App's bss share and would blow the 16 KiB frame rule many times over.
Engine g_engine;
Storage g_storage;

struct Footprint {
    bool ok = false;
    uint64_t requested = 0;
    uint64_t device = 0;
    int allocations = 0;
};

const Footprint &footprint() {
    static Footprint measured;
    static bool done = false;
    if (done) return measured;
    done = true;
    // The fake host first: its 48 MiB card arena is taken on first use, which is inside the
    // count below when this test runs on its own (--only) and nothing has touched it yet.
    (void)strata::test::FakeHost::the();
    g_requested = 0;
    g_device = 0;
    g_allocations = 0;
    g_counting = true;
    measured.ok = g_engine.init(float(ENAGA_SAMPLE_RATE)) && g_storage.init(g_engine);
    g_counting = false;
    measured.requested = g_requested;
    measured.device = g_device;
    measured.allocations = g_allocations;
    std::printf("    init: %d allocations, %.2f MiB requested, %.2f MiB on Enaga's heap of %.2f "
                "MiB (%.2f MiB spare)\n",
                measured.allocations, double(measured.requested) / double(kMiB),
                double(measured.device) / double(kMiB), double(kHeap) / double(kMiB),
                double(kHeap - measured.device) / double(kMiB));
    return measured;
}

}  // namespace

TEST(memory_init_fits_the_heap_with_headroom) {
    const Footprint &f = footprint();
    CHECK(f.ok);
    CHECK(f.allocations > 0);
    CHECK(f.device + kHeadroom <= kHeap);
}

TEST(memory_init_is_what_the_budget_says) {
    // Every allocation init() makes, by owner, so that a change to any one of them is a change
    // someone sees here rather than a drift in a total. A new allocation in init() that is in
    // none of these fails the last check, and must be added with its reason.
    const Footprint &f = footprint();

    // app/storage: the sample region at its CEILING (a project may raise its budget after
    // init), the loader's float and int16 staging, and one project file.
    const uint64_t region = uint64_t(kSampleBudgetMaxMib) * kMiB;
    // 30 s of the FILE at up to 96 kHz (app/storage.hpp): stereo 24-bit plus 64 KiB of header
    // and trailing chunks as read, stereo int16 as converted for the resample.
    // One chunk of a long file, or a small file whole: 2 MiB (2026-11-23; it was 30 s of 96 kHz
    // stereo 24-bit, 16.5 MiB, until every bigger file was read in chunks anyway).
    const uint64_t raw = 2u * kMiB;
    const uint64_t src = uint64_t(kMaxSampleSeconds) * kMaxSampleSourceRate * 2u * 2u;
    const uint64_t file = kBankMaxBytes;
    // Streaming (app/storage.hpp): four Enaga stream rings of 256 KiB, what docs/SPEC.md's
    // "recording ring and stream buffers" line budgeted. (The engine's per-track fifos are
    // members, not allocations.)
    const uint64_t rings = 4u * 256u * 1024u;

    // app/engine: the beat-repeat ring (stereo float) and the resample take (stereo int16 plus
    // a WAV header, dsp/capture.hpp).
    const uint64_t repeat = uint64_t(48000.0f * kRepeatSeconds) * 2u * 4u;
    const uint64_t capture = Capture::kHeaderBytes + uint64_t(48000.0f * kCaptureSeconds) * 2u * 2u;

    // The delay freeze's loop (two delay times of stereo float, dsp/freeze.hpp) and the tape
    // stop's ring (dsp/tapestop.hpp).
    const uint64_t freeze = uint64_t(48000.0f * 2.0f * Mixer::kMaxDelaySeconds) * 2u * 4u;
    const uint64_t tape = uint64_t(48000.0f * kTapeStopRingSeconds) * 2u * 4u;

    // The sends' ambient modes (dsp/ambient.hpp): the LONG delay's 12 s of stereo float plus its
    // eight all-pass rings, and the HALL's eight lines, four all-passes and shimmer ring.
    const uint64_t long_delay =
        (uint64_t(48000.0f * LongDelay::kMaxSeconds + float(STRATA_LONG_AHEAD) + 64.0f) * 2u +
         2u * STRATA_LONG_DIFFUSERS * STRATA_LONG_DIFFUSER_RING) * 4u;
    const uint64_t hall = (uint64_t(STRATA_HALL_LINES) * STRATA_HALL_RING +
                           STRATA_HALL_DIFFUSERS * STRATA_HALL_DIFFUSER_RING + STRATA_SHIMMER_RING) *
                          4u;
    CHECK(uint64_t(g_engine.mixer().long_delay().bytes()) == long_delay);

    // The WAVE pair's table slots (dsp/wavetable.hpp), reserved at init and filled on demand,
    // and the builder's scratch.
    const uint64_t waves = uint64_t(Engine::kWaveSlots) * kWaveTableBytes + wave_build_scratch_bytes();

    const uint64_t itemised = region + raw + src + file + rings + repeat + capture + freeze + tape +
                              long_delay + hall + waves;
    CHECK(f.requested >= itemised);

    // The rest is Enaga's master delay and reverb lines, sized inside Enaga. docs/SPEC.md budgets
    // them at 2 MiB, which is the bound here.
    const uint64_t buses = f.requested - itemised;
    std::printf("    region %.2f, loader staging %.2f, file %.2f, stream rings %.2f, repeat %.2f, "
                "capture %.2f, freeze %.2f, tape stop %.2f, long delay %.2f, hall %.2f, "
                "wavetables %.2f, delay+reverb %.2f MiB\n",
                double(region) / double(kMiB), double(raw + src) / double(kMiB),
                double(file) / double(kMiB), double(rings) / double(kMiB),
                double(repeat) / double(kMiB), double(capture) / double(kMiB),
                double(freeze) / double(kMiB), double(tape) / double(kMiB),
                double(long_delay) / double(kMiB), double(hall) / double(kMiB),
                double(waves) / double(kMiB), double(buses) / double(kMiB));
    CHECK(buses > 0);
    CHECK(buses <= 2u * kMiB);
}

TEST(memory_nothing_allocates_after_init) {
    // CLAUDE.md: allocate in init(), never after. Count across a run of blocks on this pair,
    // with nothing loaded: the audio side and the card side's idle service both, which is where
    // a stray `new` in a hot path would show.
    const Footprint &f = footprint();
    CHECK(f.ok);
    float out[2 * ENAGA_BLOCK_FRAMES];
    g_allocations = 0;
    g_counting = true;
    for (int b = 0; b < 200; b++) {
        for (float &x : out) x = 0.0f;
        g_engine.process(nullptr, out, uint32_t(ENAGA_BLOCK_FRAMES));
        g_storage.service();
    }
    g_counting = false;
    CHECK_EQ(g_allocations, 0);
}

// The WAVE tables are built on demand (Engine::step_waves): a project with no WAVE track builds
// nothing, and a WAVE track's table is built into a slot init() reserved, allocating nothing.
TEST(memory_wave_tables_are_built_only_when_a_track_sounds_one_and_allocate_nothing) {
    const Footprint &f = footprint();
    CHECK(f.ok);
    float out[2 * ENAGA_BLOCK_FRAMES];
    g_allocations = 0;
    g_counting = true;
    // The default kit has no WAVE track: nothing to do, nothing in.
    for (int i = 0; i < 100; i++) CHECK(!g_engine.step_waves());
    CHECK_EQ(g_engine.waves().builds(), 0);
    for (int t = 0; t < kWaveTables; t++) CHECK(!g_engine.waves().resident(t));
    // Track 7 to WAVE POLY: its table (TBL from the kit's byte) is built, and only that one.
    Command command;
    command.cmd = Cmd::set_machine;
    command.track = 6;
    command.a = uint8_t(MachineId::wave_poly);
    g_engine.commands().post(command);
    for (float &x : out) x = 0.0f;
    g_engine.process(nullptr, out, uint32_t(ENAGA_BLOCK_FRAMES));
    int pieces = 0;
    while (g_engine.step_waves() && pieces <= WaveLibrary::kMostSteps) pieces++;
    g_counting = false;
    CHECK_EQ(g_allocations, 0);
    CHECK(pieces >= 1 + WaveLibrary::kStoreSteps && pieces <= WaveLibrary::kMostSteps);
    CHECK_EQ(g_engine.waves().builds(), 1);
    const int table = va_wave_table(g_engine.sounding(6).get(0));
    CHECK(g_engine.waves().resident(table));
}
