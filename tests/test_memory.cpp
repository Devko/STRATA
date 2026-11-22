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
// allowance here, checked against the last measured build (docs/HANDOFF.md: payload 484,160 +
// bss 1,334,848 bytes). The App object, and the Engine and Storage in it, live in that bss:
// ENAGA_APP makes the app a function-local static.
//
// ## How an allocation is costed
//
// As Enaga's runtime does it (third_party/enaga/runtime/runtime.cpp, heap_alloc): an 8-byte
// header, rounded up to 8, at least 16. Allocation happens once, in init(), and nothing is
// freed, so a first-fit list never fragments and the sum is the whole story.
#include "check.hpp"

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
constexpr uint64_t kImageMeasured = 484160u + 1334848u;

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
    const uint64_t raw = uint64_t(kMaxSampleSeconds) * 48000u * 2u * 4u;      // stereo f32
    const uint64_t src = uint64_t(kMaxSampleSeconds) * 48000u * 2u * 2u;      // stereo s16
    const uint64_t file = kBankMaxBytes;

    // app/engine: the beat-repeat ring (stereo float) and the resample take (stereo int16 plus
    // a WAV header, dsp/capture.hpp).
    const uint64_t repeat = uint64_t(48000.0f * kRepeatSeconds) * 2u * 4u;
    const uint64_t capture = Capture::kHeaderBytes + uint64_t(48000.0f * kCaptureSeconds) * 2u * 2u;

    const uint64_t itemised = region + raw + src + file + repeat + capture;
    CHECK(f.requested >= itemised);

    // The rest is Enaga's master delay and reverb lines, sized inside Enaga. docs/SPEC.md budgets
    // them at 2 MiB, which is the bound here.
    const uint64_t buses = f.requested - itemised;
    std::printf("    region %.2f, loader staging %.2f, file %.2f, repeat %.2f, capture %.2f, "
                "delay+reverb %.2f MiB\n",
                double(region) / double(kMiB), double(raw + src) / double(kMiB),
                double(file) / double(kMiB), double(repeat) / double(kMiB),
                double(capture) / double(kMiB), double(buses) / double(kMiB));
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
