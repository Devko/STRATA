// The serializer: CRC-32, the byte cursors, and the pattern round trip.
//
// docs/SPEC.md asks for three things of a pattern file and this file is where they are held to:
// a round trip that loses nothing, a refusal rather than a crash for an unknown version or a
// bad CRC, and never casting file bytes to a struct. The first two are tested directly. The
// third cannot be tested from outside, so what is tested instead is the property it buys:
// nothing outside the buffer handed in is ever read, checked by running every truncation twice
// over different trailing bytes and requiring the same answer both times.
//
// The buffers and patterns are file-scope statics. A Pattern is about 8 KiB and CLAUDE.md caps
// a stack frame at 16 KiB, so a test that wants two of them plus a file buffer cannot put them
// on the stack.
#include "check.hpp"

#include "pattern.hpp"
#include "serialize.hpp"

#include <initializer_list>

namespace {

using namespace strata;

constexpr uint32_t kSlack = 64;             // room past a pattern, to catch an overrun
constexpr uint32_t kBufferBytes = kPatternMaxBytes + kSlack;

uint8_t g_buffer[kBufferBytes];
uint8_t g_alternate[kBufferBytes];
uint8_t g_bank[2 * kPatternMaxBytes];
Pattern g_source;
Pattern g_loaded;

// --- little helpers over the raw bytes ------------------------------------
// The tests read and patch the container by hand, byte by byte, exactly as the format is
// documented in serialize.hpp. Nothing here uses the writer, so a writer that quietly changed
// the layout would be caught rather than followed.

uint32_t peek_u32(const uint8_t *buffer, uint32_t at) {
    return uint32_t(buffer[at]) | uint32_t(buffer[at + 1]) << 8 |
           uint32_t(buffer[at + 2]) << 16 | uint32_t(buffer[at + 3]) << 24;
}

uint16_t peek_u16(const uint8_t *buffer, uint32_t at) {
    return uint16_t(uint16_t(buffer[at]) | uint16_t(uint16_t(buffer[at + 1]) << 8));
}

void poke_u32(uint8_t *buffer, uint32_t at, uint32_t value) {
    buffer[at + 0] = uint8_t(value & 0xFFu);
    buffer[at + 1] = uint8_t((value >> 8) & 0xFFu);
    buffer[at + 2] = uint8_t((value >> 16) & 0xFFu);
    buffer[at + 3] = uint8_t((value >> 24) & 0xFFu);
}

void poke_u16(uint8_t *buffer, uint32_t at, uint16_t value) {
    buffer[at + 0] = uint8_t(value & 0xFFu);
    buffer[at + 1] = uint8_t(value >> 8);
}

// Makes a deliberately damaged file consistent again, so that a test about structure is not
// answered by the CRC instead.
void refresh_crc(uint8_t *buffer) {
    const uint32_t payload = peek_u32(buffer, 8);
    poke_u32(buffer, kCrcOffset, crc32(buffer + kHeaderBytes, payload));
}

// Where a section's bytes start, as an absolute index into the file.
uint32_t section_at(const uint8_t *buffer, uint16_t id) {
    const uint32_t sections = peek_u16(buffer, 6);
    for (uint32_t i = 0; i < sections; i++) {
        const uint32_t entry = kHeaderBytes + i * kSectionEntryBytes;
        if (peek_u16(buffer, entry) == id) return kHeaderBytes + peek_u32(buffer, entry + 4);
    }
    return 0;
}

// The first byte of one step's record inside the tracks section.
uint32_t step_at(const uint8_t *buffer, int track, int step) {
    return section_at(buffer, kSectionTracks) + uint32_t(track) * kTrackBytes + 3 +
           uint32_t(step) * kStepBytes;
}

// --- test material --------------------------------------------------------

// Every field of every step set to something different from its neighbours, every track header
// different, and a lock pool that is deliberately fragmented so that the save has to compact
// it. Returns the number of locks that should survive.
uint16_t make_busy_pattern(Pattern &pattern) {
    pattern_init(pattern);

    pattern.master_length = 48;
    pattern.swing = 67;
    pattern.root = 7;
    pattern.scale = 3;
    pattern.seed = 0xDEADBEEFu;

    const Speed speeds[] = {Speed::x2, Speed::x1,   Speed::x3_4,
                            Speed::x1_2, Speed::x1_4, Speed::x1_8};

    for (int t = 0; t < kTracks; t++) {
        TrackPattern &track = pattern.tracks[t];
        track.length = uint8_t(1 + t * 9);
        track.speed = uint8_t(speeds[t % 6]);
        track.swing = t == 0 ? kSwingInherit : uint8_t(kSwingMin + t * 3);

        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &step = track.steps[i];
            step.flags = uint8_t((t * 5 + i) & kStepFlagMask);
            step.note = uint8_t((24 + t * 11 + i * 3) & 0x7F);
            step.velocity = uint8_t(1 + (t * 17 + i * 5) % 127);
            // In range on purpose. A round-trip test asks "does every field come back as it went
            // in", and the loader is documented to sanitize a field it cannot use -- so a value
            // outside 1..kLengthMax was never a fair question to ask of it. This line used to
            // reach 32 and passed only because step_sanitize was missing its upper clamp: the
            // bug was holding the test up.
            step.length = uint8_t(1 + (t + i) % kLengthMax);
            step.micro = int8_t((i % 23) - 11);
            step.probability = uint8_t((t * 13 + i * 7) % 101);
            step.condition = ((t + i) & 1) == 0
                                 ? condition_pack(Condition((t + i) % kConditionKinds))
                                 : condition_pack_a_b(uint8_t(1 + i % 8), uint8_t(2 + t % 7));
            step.ratchet = ratchet_pack(uint8_t(1 + (t + i) % 8), RatchetRamp((t + i) % 3));
            step.chord = uint8_t((t * 3 + i) & 0x3F);
        }
    }

    // Locks in an interleaved order, so nearly every run has to relocate and the pool ends up
    // holding abandoned slots. Then take a few away again.
    for (uint8_t round = 0; round < 4; round++) {
        for (int t = 0; t < kTracks; t++) {
            for (int i = 0; i < 8; i++) {
                pattern_set_lock(pattern, t, i * 7, uint8_t(round * 2),
                                 uint8_t((t * 16 + i * 2 + round) & 127));   // a parameter: 0-127
            }
        }
    }
    for (int t = 0; t < kTracks; t++) pattern_clear_lock(pattern, t, 14, 2);

    return pattern_locks_reachable(pattern);
}

// Compares two patterns field by field. Mismatches are counted per field rather than reported
// one by one: 512 steps of a systematically broken field would otherwise bury the one line
// that says which field it was.
struct Diff {
    int flags = 0, note = 0, velocity = 0, length = 0, micro = 0, probability = 0;
    int condition = 0, ratchet = 0, chord = 0, lock_count = 0, lock_values = 0;
};

Diff diff_steps(const Pattern &a, const Pattern &b) {
    Diff diff;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &x = a.tracks[t].steps[i];
            const Step &y = b.tracks[t].steps[i];
            if (x.flags != y.flags) diff.flags++;
            if (x.note != y.note) diff.note++;
            if (x.velocity != y.velocity) diff.velocity++;
            if (x.length != y.length) diff.length++;
            if (x.micro != y.micro) diff.micro++;
            if (x.probability != y.probability) diff.probability++;
            if (x.condition != y.condition) diff.condition++;
            if (x.ratchet != y.ratchet) diff.ratchet++;
            if (x.chord != y.chord) diff.chord++;

            const LockView va = pattern_step_locks(a, t, i);
            const LockView vb = pattern_step_locks(b, t, i);
            if (va.count != vb.count) {
                diff.lock_count++;
                continue;
            }
            for (uint8_t k = 0; k < va.count; k++) {
                if (va.locks[k].param != vb.locks[k].param ||
                    va.locks[k].value != vb.locks[k].value) {
                    diff.lock_values++;
                }
            }
        }
    }
    return diff;
}

void check_no_diff(const Diff &diff) {
    CHECK_EQ(diff.flags, 0);
    CHECK_EQ(diff.note, 0);
    CHECK_EQ(diff.velocity, 0);
    CHECK_EQ(diff.length, 0);
    CHECK_EQ(diff.micro, 0);
    CHECK_EQ(diff.probability, 0);
    CHECK_EQ(diff.condition, 0);
    CHECK_EQ(diff.ratchet, 0);
    CHECK_EQ(diff.chord, 0);
    CHECK_EQ(diff.lock_count, 0);
    CHECK_EQ(diff.lock_values, 0);
}

}  // namespace

// --- CRC-32 ---------------------------------------------------------------

TEST(crc32_matches_the_standard_check_values) {
    // The published check values for CRC-32/ISO-HDLC, which is zip's and Ethernet's. If these
    // ever change, files written by an older build stop loading, so they are pinned here.
    const uint8_t nine[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc32(nine, 9), 0xCBF43926u);

    const uint8_t a[] = {'a'};
    CHECK_EQ(crc32(a, 1), 0xE8B7BE43u);

    CHECK_EQ(crc32(a, 0), 0u);
    CHECK_EQ(crc32(nullptr, 0), 0u);
}

TEST(crc32_notices_every_single_bit) {
    uint8_t data[64];
    for (uint32_t i = 0; i < 64; i++) data[i] = uint8_t(i * 7 + 1);
    const uint32_t base = crc32(data, 64);

    for (uint32_t i = 0; i < 64; i++) {
        for (int bit = 0; bit < 8; bit++) {
            data[i] = uint8_t(data[i] ^ (1u << bit));
            CHECK(crc32(data, 64) != base);
            data[i] = uint8_t(data[i] ^ (1u << bit));
        }
    }
    CHECK_EQ(crc32(data, 64), base);   // and back to where it started
}

// --- the byte cursors -----------------------------------------------------

TEST(the_writer_puts_bytes_down_little_endian) {
    uint8_t out[16] = {};
    Writer w(out, sizeof(out));
    w.u8(0x11);
    w.i8(-2);                  // 0xFE
    w.u16(0x2233);
    w.u32(0x44556677u);
    CHECK(w.ok());
    CHECK_EQ(int(w.size()), 8);

    CHECK_EQ(int(out[0]), 0x11);
    CHECK_EQ(int(out[1]), 0xFE);
    CHECK_EQ(int(out[2]), 0x33);
    CHECK_EQ(int(out[3]), 0x22);
    CHECK_EQ(int(out[4]), 0x77);
    CHECK_EQ(int(out[5]), 0x66);
    CHECK_EQ(int(out[6]), 0x55);
    CHECK_EQ(int(out[7]), 0x44);
}

TEST(the_cursors_round_trip_every_width) {
    uint8_t out[32] = {};
    Writer w(out, sizeof(out));
    const uint8_t blob[3] = {0xDE, 0xAD, 0xBE};
    w.u8(255);
    w.i8(-128);
    w.i8(127);
    w.u16(65535);
    w.u32(0xFFFFFFFFu);
    w.bytes(blob, 3);
    w.zeros(2);
    CHECK(w.ok());
    CHECK_EQ(int(w.size()), 1 + 1 + 1 + 2 + 4 + 3 + 2);

    Reader r(out, w.size());
    CHECK_EQ(int(r.u8()), 255);
    CHECK_EQ(int(r.i8()), -128);
    CHECK_EQ(int(r.i8()), 127);
    CHECK_EQ(int(r.u16()), 65535);
    CHECK_EQ(r.u32(), 0xFFFFFFFFu);
    CHECK_EQ(int(r.u8()), 0xDE);
    CHECK_EQ(int(r.u8()), 0xAD);
    CHECK_EQ(int(r.u8()), 0xBE);
    CHECK_EQ(int(r.u8()), 0);
    CHECK_EQ(int(r.u8()), 0);
    CHECK(r.ok());
    CHECK_EQ(int(r.remaining()), 0);
}

TEST(the_writer_stops_at_the_end_of_its_buffer) {
    uint8_t out[4] = {0, 0, 0, 0};
    Writer w(out, sizeof(out));
    w.u16(0x1234);
    CHECK(w.ok());
    w.u32(0xAABBCCDDu);        // only two bytes of room left
    CHECK(!w.ok());
    CHECK_EQ(int(w.size()), 4);
    // The two bytes that did fit were stored; nothing beyond the buffer was touched.
    CHECK_EQ(int(out[0]), 0x34);
    CHECK_EQ(int(out[1]), 0x12);

    // Once it has overflowed it stays overflowed, and stores nothing further.
    w.u8(0xFF);
    CHECK(!w.ok());
    CHECK_EQ(int(w.size()), 4);

    // A zero-capacity writer with no buffer at all must not dereference it.
    Writer none(nullptr, 0);
    none.u8(1);
    CHECK(!none.ok());
    CHECK_EQ(int(none.size()), 0);
}

TEST(the_writer_patches_only_inside_what_it_wrote) {
    uint8_t out[8] = {};
    Writer w(out, sizeof(out));
    w.u32(0);
    w.u32(0);
    w.patch_u32(0, 0x01020304u);
    CHECK(w.ok());
    CHECK_EQ(peek_u32(out, 0), 0x01020304u);

    w.patch_u32(4, 0x05060708u);
    CHECK(w.ok());
    CHECK_EQ(peek_u32(out, 4), 0x05060708u);

    // Past the end of what was written: refused, and the writer is no longer ok.
    w.patch_u32(6, 0u);
    CHECK(!w.ok());
    CHECK_EQ(peek_u32(out, 4), 0x05060708u);
}

TEST(the_reader_never_passes_the_end) {
    const uint8_t in[4] = {1, 2, 3, 4};
    Reader r(in, sizeof(in));
    CHECK_EQ(int(r.u32()), 0x04030201);
    CHECK(r.ok());
    CHECK_EQ(int(r.u8()), 0);        // one past the end
    CHECK(!r.ok());

    // A wide read that straddles the end yields zero rather than part of a value: half a
    // uint32_t from a damaged file would look like data.
    Reader straddle(in, 3);
    CHECK_EQ(straddle.u32(), 0u);
    CHECK(!straddle.ok());

    Reader half(in, 1);
    CHECK_EQ(int(half.u16()), 0);
    CHECK(!half.ok());

    Reader three(in, 3);
    CHECK_EQ(int(three.u16()), 0x0201);   // the first two bytes are there, so they are read
    CHECK(three.ok());
    CHECK_EQ(int(three.u16()), 0);        // the second value is not
    CHECK(!three.ok());

    Reader skipper(in, sizeof(in));
    skipper.skip(4);
    CHECK(skipper.ok());
    CHECK_EQ(int(skipper.remaining()), 0);
    skipper.skip(1);
    CHECK(!skipper.ok());

    Reader seeker(in, sizeof(in));
    CHECK(seeker.seek(4));           // the end itself is a legal position
    CHECK(!seeker.seek(5));
    CHECK(!seeker.ok());
    CHECK(!seeker.seek(0));          // and it stays refused

    Reader empty(nullptr, 0);
    CHECK_EQ(int(empty.u8()), 0);
    CHECK(!empty.ok());
}

// --- the container --------------------------------------------------------

TEST(the_format_constants_are_the_documented_ones) {
    CHECK_EQ(int(kHeaderBytes), 16);
    CHECK_EQ(int(kSectionEntryBytes), 12);
    CHECK_EQ(int(kCrcOffset), 12);
    CHECK_EQ(int(kVersionOffset), 4);
    CHECK_EQ(int(kPatternVersion), 1);
    CHECK_EQ(int(kStepBytes), 12);
    CHECK_EQ(int(kLockBytes), 2);
    CHECK_EQ(int(kTrackBytes), 3 + 64 * 12);
    CHECK_EQ(int(kTracksSectionBytes), 8 * (3 + 64 * 12));
    CHECK_EQ(int(kSectionPlayModes), 4);
    CHECK_EQ(int(kPlayModesSectionBytes), 8);
    CHECK_EQ(int(kSectionFeel), 5);
    CHECK_EQ(int(kFeelSectionBytes), 1);
    // 16 header + 60 table + 12 pattern header + 6168 tracks + 8 play modes + 1 FEEL + 2048 locks.
    CHECK_EQ(int(kPatternMaxBytes), 8313);
    CHECK_EQ(int(kPatternFixedBytes), 8313 - 2048);
}

TEST(a_written_pattern_starts_with_the_documented_header) {
    pattern_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    CHECK_EQ(int(g_buffer[0]), int('S'));
    CHECK_EQ(int(g_buffer[1]), int('T'));
    CHECK_EQ(int(g_buffer[2]), int('R'));
    CHECK_EQ(int(g_buffer[3]), int('A'));
    CHECK_EQ(int(peek_u16(g_buffer, kVersionOffset)), int(kPatternVersion));
    CHECK_EQ(int(peek_u16(g_buffer, 6)), int(kPatternSections));
    CHECK_EQ(peek_u32(g_buffer, 8), written - kHeaderBytes);
    CHECK_EQ(peek_u32(g_buffer, kCrcOffset),
             crc32(g_buffer + kHeaderBytes, written - kHeaderBytes));

    // The section table: five sections, back to back, starting right after the table.
    CHECK_EQ(int(peek_u16(g_buffer, 16)), int(kSectionPatternHeader));
    CHECK_EQ(peek_u32(g_buffer, 16 + 4), kPatternSections * kSectionEntryBytes);
    CHECK_EQ(peek_u32(g_buffer, 16 + 8), kPatternHeaderSectionBytes);
    CHECK_EQ(int(peek_u16(g_buffer, 28)), int(kSectionTracks));
    CHECK_EQ(peek_u32(g_buffer, 28 + 8), kTracksSectionBytes);
    CHECK_EQ(int(peek_u16(g_buffer, 40)), int(kSectionPlayModes));
    CHECK_EQ(peek_u32(g_buffer, 40 + 8), kPlayModesSectionBytes);
    CHECK_EQ(int(peek_u16(g_buffer, 52)), int(kSectionFeel));
    CHECK_EQ(peek_u32(g_buffer, 52 + 8), kFeelSectionBytes);
    CHECK_EQ(int(peek_u16(g_buffer, 64)), int(kSectionLocks));
    CHECK_EQ(peek_u32(g_buffer, 64 + 8), 0u);      // an empty pattern has no locks

    // An empty pattern is the fixed part and nothing more.
    CHECK_EQ(written, kPatternFixedBytes);
    CHECK_EQ(pattern_bytes(g_source), written);
}

// --- the round trip -------------------------------------------------------

TEST(a_full_pattern_survives_a_round_trip_field_by_field) {
    const uint16_t locks = make_busy_pattern(g_source);
    CHECK(locks > 0);

    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, pattern_bytes(g_source));
    CHECK_EQ(written, kPatternFixedBytes + uint32_t(locks) * kLockBytes);

    uint32_t consumed = 0;
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written, &consumed)),
             int(SerializeResult::ok));
    CHECK_EQ(consumed, written);

    // Pattern-level fields.
    CHECK_EQ(int(g_loaded.master_length), int(g_source.master_length));
    CHECK_EQ(int(g_loaded.swing), int(g_source.swing));
    CHECK_EQ(int(g_loaded.root), int(g_source.root));
    CHECK_EQ(int(g_loaded.scale), int(g_source.scale));
    CHECK_EQ(g_loaded.seed, g_source.seed);

    // Track-level fields.
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_loaded.tracks[t].length), int(g_source.tracks[t].length));
        CHECK_EQ(int(g_loaded.tracks[t].speed), int(g_source.tracks[t].speed));
        CHECK_EQ(int(g_loaded.tracks[t].swing), int(g_source.tracks[t].swing));
    }

    // One step in full, as a readable example of what the sweep below covers.
    const Step &want = g_source.tracks[3].steps[21];
    const Step &got = g_loaded.tracks[3].steps[21];
    CHECK_EQ(int(got.flags), int(want.flags));
    CHECK_EQ(int(got.note), int(want.note));
    CHECK_EQ(int(got.velocity), int(want.velocity));
    CHECK_EQ(int(got.length), int(want.length));
    CHECK_EQ(int(got.micro), int(want.micro));
    CHECK_EQ(int(got.probability), int(want.probability));
    CHECK_EQ(int(got.condition), int(want.condition));
    CHECK_EQ(int(got.ratchet), int(want.ratchet));
    CHECK_EQ(int(got.chord), int(want.chord));

    // Then every field of all 512 steps, and every lock.
    check_no_diff(diff_steps(g_source, g_loaded));

    // Conditions in particular: the A:B pairs have to come back as the same A and B.
    int ab_steps = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const uint8_t packed = g_loaded.tracks[t].steps[i].condition;
            if (condition_kind(packed) != Condition::A_B) continue;
            const uint8_t source = g_source.tracks[t].steps[i].condition;
            CHECK_EQ(int(condition_a(packed)), int(condition_a(source)));
            CHECK_EQ(int(condition_b(packed)), int(condition_b(source)));
            ab_steps++;
        }
    }
    CHECK(ab_steps > 0);
}

TEST(a_lock_value_past_127_in_a_file_loads_as_127) {
    // A damaged or hand-made file: lock values are parameters, 0-127, and EDIT draws attack,
    // decay and release locks as x positions on a 124-column picture, so a 255 must not survive
    // the load. The param byte is kept as it was, whatever it is.
    pattern_init(g_source);
    CHECK(pattern_set_lock(g_source, 0, 3, 9, 200));
    CHECK(pattern_set_lock(g_source, 0, 3, 200, 255));
    CHECK(pattern_set_lock(g_source, 2, 7, 4, 90));
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_loaded, 0, 3, 9, value));
    CHECK_EQ(int(value), 127);
    CHECK(pattern_get_lock(g_loaded, 0, 3, 200, value));
    CHECK_EQ(int(value), 127);
    CHECK(pattern_get_lock(g_loaded, 2, 7, 4, value));
    CHECK_EQ(int(value), 90);                          // in range: untouched
}

TEST(a_random_range_and_a_slide_survive_a_save_and_load) {
    // Both live in the lock's param byte (core/pattern.hpp), so the file carries them as it
    // carries any lock, with no section of their own.
    pattern_init(g_source);
    CHECK(pattern_set_lock(g_source, 1, 4, 6, 40));
    CHECK(pattern_set_slide(g_source, 1, 4, 6, true));
    CHECK(pattern_set_range(g_source, 1, 4, 6, 25));
    CHECK(pattern_set_range(g_source, 3, 0, 2, 9));
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_loaded, 1, 4, 6, value));
    CHECK_EQ(int(value), 40);
    CHECK(pattern_get_slide(g_loaded, 1, 4, 6));
    CHECK_EQ(int(pattern_get_range(g_loaded, 1, 4, 6)), 25);
    CHECK_EQ(int(pattern_get_range(g_loaded, 3, 0, 2)), 9);
    CHECK(!pattern_get_lock(g_loaded, 3, 0, 2, value));
}

TEST(saving_compacts_the_lock_pool_without_touching_the_pattern) {
    const uint16_t locks = make_busy_pattern(g_source);
    const uint16_t used_before = g_source.locks_used;
    CHECK(used_before > locks);        // fragmented on purpose

    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    // The save wrote the compacted pool but left the pattern in RAM exactly as it was: the
    // main context must not write memory the audio side owns.
    CHECK_EQ(int(g_source.locks_used), int(used_before));
    CHECK_EQ(written, kPatternFixedBytes + uint32_t(locks) * kLockBytes);

    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));

    // What came back is the same music in a compacted pool.
    CHECK_EQ(int(g_loaded.locks_used), int(locks));
    CHECK_EQ(int(pattern_locks_reachable(g_loaded)), int(locks));
    check_no_diff(diff_steps(g_source, g_loaded));

    // Runs are contiguous from zero, in track-then-step order.
    uint32_t expect = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &step = g_loaded.tracks[t].steps[i];
            if (step.lock_count == 0) {
                CHECK_EQ(int(step.lock_first), int(kNoLock));
                continue;
            }
            CHECK_EQ(int(step.lock_first), int(expect));
            expect += step.lock_count;
        }
    }
    CHECK_EQ(int(expect), int(locks));

    // Compacting the source gives exactly the same file.
    pattern_compact_locks(g_source);
    uint32_t again = 0;
    CHECK_EQ(int(pattern_write(g_source, g_alternate, kBufferBytes, &again)),
             int(SerializeResult::ok));
    CHECK_EQ(again, written);
    int byte_diffs = 0;
    for (uint32_t i = 0; i < written; i++) {
        if (g_buffer[i] != g_alternate[i]) byte_diffs++;
    }
    CHECK_EQ(byte_diffs, 0);
}

TEST(a_full_lock_pool_still_fits_the_maximum_buffer) {
    pattern_init(g_source);
    int added = 0;
    for (int step = 0; step < 128; step++) {
        for (uint8_t param = 0; param < kLocksPerStep; param++) {
            if (!pattern_set_lock(g_source, step / kStepsPerTrack, step % kStepsPerTrack, param,
                                  uint8_t((param * 3 + step) & 127))) {
                break;
            }
            added++;
        }
    }
    CHECK_EQ(added, kLockPool);
    CHECK_EQ(pattern_bytes(g_source), kPatternMaxBytes);

    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kPatternMaxBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kPatternMaxBytes);

    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.locks_used), kLockPool);
    check_no_diff(diff_steps(g_source, g_loaded));
}

TEST(a_bank_of_patterns_can_be_read_one_after_another) {
    // A bank file is 16 patterns back to back, so a read must take only its own bytes and say
    // how many it took.
    pattern_init(g_source);
    g_source.seed = 101;
    g_source.tracks[0].steps[0].flags = kStepTrig;
    uint32_t first = 0;
    CHECK_EQ(int(pattern_write(g_source, g_bank, sizeof(g_bank), &first)),
             int(SerializeResult::ok));

    g_source.seed = 202;
    CHECK(pattern_set_lock(g_source, 1, 2, 3, 44));
    uint32_t second = 0;
    CHECK_EQ(int(pattern_write(g_source, g_bank + first, uint32_t(sizeof(g_bank)) - first,
                               &second)),
             int(SerializeResult::ok));
    CHECK(second > first);     // the second one carries a lock

    uint32_t consumed = 0;
    CHECK_EQ(int(pattern_read(g_loaded, g_bank, first + second, &consumed)),
             int(SerializeResult::ok));
    CHECK_EQ(consumed, first);
    CHECK_EQ(g_loaded.seed, 101u);
    CHECK_EQ(int(g_loaded.locks_used), 0);

    CHECK_EQ(int(pattern_read(g_loaded, g_bank + consumed, second, &consumed)),
             int(SerializeResult::ok));
    CHECK_EQ(consumed, second);
    CHECK_EQ(g_loaded.seed, 202u);
    CHECK_EQ(int(g_loaded.locks_used), 1);
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_loaded, 1, 2, 3, value));
    CHECK_EQ(int(value), 44);
}

// --- writing into too small a buffer --------------------------------------

TEST(a_buffer_that_is_too_small_is_refused_untouched) {
    pattern_init(g_source);
    const uint32_t need = pattern_bytes(g_source);

    for (uint32_t i = 0; i < kBufferBytes; i++) g_buffer[i] = 0x5A;
    uint32_t written = 0xFFFFFFFFu;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, need - 1, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);

    int touched = 0;
    for (uint32_t i = 0; i < kBufferBytes; i++) {
        if (g_buffer[i] != 0x5A) touched++;
    }
    CHECK_EQ(touched, 0);

    // Zero capacity, and no buffer at all.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, 0, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(int(pattern_write(g_source, nullptr, kPatternMaxBytes, &written)),
             int(SerializeResult::buffer_too_small));

    // Exactly enough is enough.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, need, &written)), int(SerializeResult::ok));
    CHECK_EQ(written, need);
}

// --- refusing bad files ---------------------------------------------------

TEST(a_corrupted_crc_is_refused) {
    make_busy_pattern(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    // A byte flipped anywhere in the payload: in the section table, in the first step, in the
    // middle, and in the very last lock byte.
    const uint32_t spots[] = {kHeaderBytes, kHeaderBytes + 20, written / 2, written - 1};
    for (const uint32_t at : spots) {
        g_buffer[at] = uint8_t(g_buffer[at] ^ 0xFFu);
        CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::bad_crc));
        // Refused means refused: nothing of the file is kept.
        CHECK_EQ(int(g_loaded.locks_used), 0);
        CHECK_EQ(g_loaded.seed, 1u);
        g_buffer[at] = uint8_t(g_buffer[at] ^ 0xFFu);
    }

    // Unflipped, it still loads, so the test really did only change one byte each time.
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));

    // And a CRC field that is simply wrong is refused as well.
    const uint32_t good = peek_u32(g_buffer, kCrcOffset);
    poke_u32(g_buffer, kCrcOffset, good + 1);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::bad_crc));
    poke_u32(g_buffer, kCrcOffset, good);
}

TEST(an_unknown_version_is_refused) {
    pattern_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    // The version lives in the header, which the CRC does not cover, so a file from a future
    // build has a perfectly good CRC and must still be refused.
    for (const uint16_t version : {uint16_t(0), uint16_t(kPatternVersion + 1), uint16_t(99),
                                   uint16_t(0xFFFF)}) {
        poke_u16(g_buffer, kVersionOffset, version);
        CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)),
                 int(SerializeResult::bad_version));
        CHECK_EQ(g_loaded.seed, 1u);     // left empty, not half read
    }

    poke_u16(g_buffer, kVersionOffset, kPatternVersion);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
}

TEST(a_file_that_is_not_ours_is_refused) {
    pattern_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t keep = g_buffer[i];
        g_buffer[i] = uint8_t(keep + 1);
        CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)),
                 int(SerializeResult::bad_magic));
        g_buffer[i] = keep;
    }

    // Something else entirely, and a buffer of nothing at all.
    for (uint32_t i = 0; i < 64; i++) g_buffer[i] = uint8_t('A' + i % 26);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, 64)), int(SerializeResult::bad_magic));
    CHECK_EQ(int(pattern_read(g_loaded, nullptr, 0)), int(SerializeResult::truncated));
}

TEST(a_truncated_buffer_is_refused_without_reading_past_the_end) {
    make_busy_pattern(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));

    // Two copies of every prefix, whose bytes past `size` differ: zeros in one and 0xFF in the
    // other. If a truncated read depended on a byte it was not given, the two answers would
    // come apart. It is not a proof -- only a sanitizer is -- but it is the strongest check
    // the host build can make. g_bank holds the pristine file, since both copies get trampled.
    for (uint32_t i = 0; i < written; i++) g_bank[i] = g_buffer[i];

    // Counted rather than checked per size: 8,000 failing checks would bury the one fact that
    // matters, which is how many of the truncations behaved.
    int accepted = 0, disagreed = 0, consumed_set = 0, left_dirty = 0;
    for (uint32_t size = 0; size < written; size++) {
        for (uint32_t i = 0; i < size; i++) {
            g_buffer[i] = g_bank[i];
            g_alternate[i] = g_bank[i];
        }
        for (uint32_t i = size; i < kBufferBytes; i++) {
            g_buffer[i] = 0x00;
            g_alternate[i] = 0xFF;
        }

        uint32_t consumed = 1;
        const SerializeResult low = pattern_read(g_loaded, g_buffer, size, &consumed);
        const SerializeResult high = pattern_read(g_loaded, g_alternate, size);
        if (low == SerializeResult::ok) accepted++;
        if (low != high) disagreed++;
        if (consumed != 0) consumed_set++;
        if (g_loaded.locks_used != 0) left_dirty++;
    }
    CHECK_EQ(accepted, 0);
    CHECK_EQ(disagreed, 0);
    CHECK_EQ(consumed_set, 0);
    CHECK_EQ(left_dirty, 0);

    // One size spelled out, so the result itself is on the record and not just the count.
    for (uint32_t i = 0; i < written - 1; i++) g_buffer[i] = g_bank[i];
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written - 1)),
             int(SerializeResult::truncated));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, kHeaderBytes - 1)),
             int(SerializeResult::truncated));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, 0)), int(SerializeResult::truncated));

    // And the whole thing, restored, still loads.
    uint32_t again = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &again)),
             int(SerializeResult::ok));
    CHECK_EQ(again, written);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
}

TEST(a_declared_payload_outside_the_buffer_is_refused) {
    pattern_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    const uint32_t payload = peek_u32(g_buffer, 8);

    // Bigger than the buffer: the CRC must not be computed over bytes nobody gave us.
    poke_u32(g_buffer, 8, payload + 1);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::truncated));
    poke_u32(g_buffer, 8, 0xFFFFFFFFu);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::truncated));

    // Smaller: the CRC can be made to agree, and then the section table is out of bounds.
    poke_u32(g_buffer, 8, 100);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    poke_u32(g_buffer, 8, 0);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
}

TEST(a_damaged_section_table_is_refused) {
    pattern_init(g_source);
    uint32_t written = 0;

    // No sections at all, and more than any file could have. Both live in the header, so the
    // CRC still agrees and the reader has to catch them itself.
    const uint16_t counts[] = {0, uint16_t(kMaxSections + 1), 0xFFFF};
    for (const uint16_t count : counts) {
        CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
                 int(SerializeResult::ok));
        poke_u16(g_buffer, 6, count);
        CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)),
                 int(SerializeResult::malformed));
    }

    // A section that claims more bytes than the payload holds: the last one, the locks.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u32(g_buffer, 64 + 8, 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // A section that starts inside the table.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u32(g_buffer, 28 + 4, 8);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // A section this build needs, renamed to an id it does not know. Unknown ids are skipped,
    // which is the point of the table -- but then the tracks are missing.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u16(g_buffer, 28, 999);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // The same id twice.
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u16(g_buffer, 40, kSectionTracks);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
}

TEST(a_pattern_header_this_build_cannot_lay_out_is_refused) {
    pattern_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    const uint32_t header = section_at(g_buffer, kSectionPatternHeader);
    CHECK_EQ(header, kHeaderBytes + kPatternSections * kSectionEntryBytes);
    CHECK_EQ(int(g_buffer[header + 0]), kTracks);
    CHECK_EQ(int(g_buffer[header + 1]), kStepsPerTrack);

    // A different geometry is not something this build can read, whatever the version says.
    g_buffer[header + 0] = 4;
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
    g_buffer[header + 0] = uint8_t(kTracks);

    g_buffer[header + 1] = 32;
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
    g_buffer[header + 1] = uint8_t(kStepsPerTrack);

    // More locks than the pool holds.
    poke_u16(g_buffer, header + 6, uint16_t(kLockPool + 1));
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // A lock count the locks section has no room for.
    poke_u16(g_buffer, header + 6, 8);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
}

TEST(a_lock_run_that_breaks_the_layout_is_refused) {
    pattern_init(g_source);
    CHECK(pattern_set_lock(g_source, 0, 0, 1, 11));
    CHECK(pattern_set_lock(g_source, 0, 1, 2, 22));
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));

    const uint32_t step0 = step_at(g_buffer, 0, 0);
    const uint32_t step1 = step_at(g_buffer, 0, 1);
    CHECK_EQ(int(peek_u16(g_buffer, step0 + 8)), 0);     // lock_first
    CHECK_EQ(int(g_buffer[step0 + 10]), 1);              // lock_count
    CHECK_EQ(int(peek_u16(g_buffer, step1 + 8)), 1);

    // More locks on one step than a step may have.
    g_buffer[step0 + 10] = uint8_t(kLocksPerStep + 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
    g_buffer[step0 + 10] = 1;

    // A run that does not start where the compacted layout says it must: a gap...
    poke_u16(g_buffer, step1 + 8, 2);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // ...and an overlap, which would make two steps share a slot.
    poke_u16(g_buffer, step1 + 8, 0);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // A run that points outside the pool entirely.
    poke_u16(g_buffer, step1 + 8, 60000);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));

    // Fewer locks on the steps than the header declares.
    poke_u16(g_buffer, step1 + 8, 1);
    g_buffer[step1 + 10] = 0;
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::malformed));
    CHECK_EQ(int(g_loaded.locks_used), 0);
}

TEST(a_pattern_with_a_broken_run_in_ram_is_still_written_and_read_back) {
    // A command applier with a bug could leave a run pointing outside the pool. Writing such a
    // pattern must produce a file that loads -- with that step's locks gone, not the file.
    pattern_init(g_source);
    CHECK(pattern_set_lock(g_source, 0, 0, 1, 11));
    CHECK(pattern_set_lock(g_source, 2, 2, 5, 55));
    g_source.tracks[0].steps[0].lock_first = 900;
    g_source.tracks[0].steps[0].lock_count = 8;

    CHECK_EQ(int(pattern_locks_reachable(g_source)), 1);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, pattern_bytes(g_source));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));

    CHECK_EQ(int(g_loaded.locks_used), 1);
    CHECK_EQ(int(g_loaded.tracks[0].steps[0].lock_count), 0);
    uint8_t value = 0;
    CHECK(pattern_get_lock(g_loaded, 2, 2, 5, value));
    CHECK_EQ(int(value), 55);
}

// --- value tolerance ------------------------------------------------------

TEST(out_of_range_step_values_are_clamped_on_load) {
    // The CRC catches damage in transit; it cannot catch a pattern that was already wrong in
    // RAM. Loading clamps rather than refuses, so one bad byte costs one odd step.
    pattern_init(g_source);
    Step &step = g_source.tracks[0].steps[0];
    step.flags = 0xFF;
    step.note = 250;
    step.velocity = 0;
    step.length = 0;
    step.micro = 120;
    step.probability = 200;
    step.condition = 200;
    step.ratchet = 0x3F;          // 15 hits, ramp 3 (bit 7 clear: not a roll over LEN)
    g_source.tracks[0].length = 0;
    g_source.tracks[0].speed = 99;
    g_source.tracks[0].swing = 200;
    g_source.tracks[1].swing = 10;
    g_source.master_length = 0;
    g_source.swing = 1;
    g_source.root = 200;

    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));

    const Step &got = g_loaded.tracks[0].steps[0];
    CHECK_EQ(int(got.flags), int(kStepFlagMask));
    CHECK_EQ(int(got.note), 250 & 0x7F);
    CHECK_EQ(int(got.velocity), 1);
    CHECK_EQ(int(got.length), 1);
    CHECK_EQ(int(got.micro), 11);
    CHECK_EQ(int(got.probability), 100);
    CHECK_EQ(int(got.condition), int(Condition::NONE));
    CHECK_EQ(int(ratchet_count(got.ratchet)), 8);
    CHECK_EQ(int(ratchet_ramp(got.ratchet)), int(RatchetRamp::none));

    CHECK_EQ(int(g_loaded.tracks[0].length), 1);
    CHECK_EQ(int(g_loaded.tracks[0].speed), int(Speed::x1));
    CHECK_EQ(int(g_loaded.tracks[0].swing), int(kSwingMax));
    CHECK_EQ(int(g_loaded.tracks[1].swing), int(kSwingMin));
    CHECK_EQ(int(g_loaded.master_length), 1);
    CHECK_EQ(int(g_loaded.swing), int(kSwingMin));
    CHECK_EQ(int(g_loaded.root), 200 % 12);

    // A track that was told to follow the pattern still does.
    CHECK_EQ(int(g_loaded.tracks[2].swing), int(kSwingInherit));
}

TEST(every_result_has_a_message_for_the_screen) {
    const SerializeResult all[] = {
        SerializeResult::ok,          SerializeResult::buffer_too_small,
        SerializeResult::truncated,   SerializeResult::bad_magic,
        SerializeResult::bad_version, SerializeResult::bad_crc,
        SerializeResult::malformed};
    for (const SerializeResult result : all) {
        const char *name = serialize_result_name(result);
        CHECK(name != nullptr);
        CHECK(name[0] != 0);
        // The device font has upper case, digits and a few symbols only.
        for (const char *c = name; *c != 0; c++) {
            CHECK((*c >= 'A' && *c <= 'Z') || *c == ' ');
        }
    }
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

namespace {

uint32_t entry_of(const uint8_t *buffer, uint16_t id) {
    const uint32_t sections = peek_u16(buffer, 6);
    for (uint32_t i = 0; i < sections; i++) {
        const uint32_t entry = kHeaderBytes + i * kSectionEntryBytes;
        if (peek_u16(buffer, entry) == id) return entry;
    }
    return 0;
}

uint32_t write_busy() {
    make_busy_pattern(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    return written;
}

}  // namespace

TEST(a_reader_that_ran_out_returns_zero_for_every_width) {
    const uint8_t one[1] = {0xAB};
    Reader r(one, 1);
    CHECK_EQ(int(r.u16()), 0);
    CHECK(!r.ok());
    CHECK_EQ(int(r.u8()), 0);
    CHECK_EQ(int(r.u16()), 0);
    CHECK_EQ(r.u32(), 0u);
    Reader q(one, 1);
    CHECK_EQ(q.u32(), 0u);
    CHECK(!q.ok());
}

TEST(the_section_table_reserved_bytes_are_written_as_zero) {
    write_busy();
    for (uint32_t i = 0; i < peek_u16(g_buffer, 6); i++) {
        CHECK_EQ(int(peek_u16(g_buffer, kHeaderBytes + i * kSectionEntryBytes + 2)), 0);
    }
}

TEST(a_section_pointing_into_the_table_or_a_lock_section_one_byte_short_is_malformed) {
    const uint32_t size = write_busy();
    const uint32_t header = entry_of(g_buffer, kSectionPatternHeader);
    poke_u32(g_buffer, header + 4, kSectionEntryBytes);   // inside the three-entry table
    refresh_crc(g_buffer);
    uint32_t consumed = 0;
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size, &consumed)),
             int(SerializeResult::malformed));

    write_busy();
    const uint32_t locks = entry_of(g_buffer, kSectionLocks);
    poke_u32(g_buffer, locks + 8, peek_u32(g_buffer, locks + 8) - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size, &consumed)),
             int(SerializeResult::malformed));
}

TEST(a_speed_byte_one_past_the_table_loads_as_normal_speed) {
    const uint32_t size = write_busy();
    g_buffer[section_at(g_buffer, kSectionTracks) + 1] = uint8_t(Speed::count);
    refresh_crc(g_buffer);
    uint32_t consumed = 0;
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size, &consumed)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.tracks[0].speed), int(Speed::x1));
}

// --- play modes (section 4) -----------------------------------------------

TEST(every_tracks_play_mode_survives_a_round_trip) {
    make_busy_pattern(g_source);
    for (int t = 0; t < kTracks; t++) g_source.tracks[t].play = uint8_t(t % int(PlayMode::count));
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_loaded.tracks[t].play), t % int(PlayMode::count));
    }
    // One byte a track, in track order, in its own section.
    const uint32_t at = section_at(g_buffer, kSectionPlayModes);
    CHECK(at != 0u);
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_buffer[at + uint32_t(t)]), t % 8);
}

// A pattern from before play modes: no section 4. Renaming the id is what an older file looks
// like to this reader -- the section is simply not found -- and every track must play forward,
// with the rest of the pattern intact.
TEST(a_pattern_without_the_play_mode_section_loads_every_track_forward) {
    make_busy_pattern(g_source);
    for (int t = 0; t < kTracks; t++) g_source.tracks[t].play = uint8_t(PlayMode::rnd4);
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u16(g_buffer, entry_of(g_buffer, kSectionPlayModes), 999);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_loaded.tracks[t].play), int(PlayMode::fwd));
        CHECK_EQ(int(g_loaded.tracks[t].length), int(g_source.tracks[t].length));
    }
    CHECK_EQ(int(g_loaded.locks_used), int(pattern_locks_reachable(g_source)));
}

// --- FEEL (section 5) ------------------------------------------------------

TEST(every_feel_survives_a_round_trip_in_its_own_section) {
    for (int feel = 0; feel <= int(kFeelMax); feel++) {
        make_busy_pattern(g_source);
        g_source.feel = uint8_t(feel);
        uint32_t written = 0;
        CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
                 int(SerializeResult::ok));
        CHECK_EQ(written, pattern_bytes(g_source));
        CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
        CHECK_EQ(int(g_loaded.feel), feel);
        const uint32_t at = section_at(g_buffer, kSectionFeel);
        CHECK(at != 0u);
        CHECK_EQ(int(g_buffer[at]), feel);
    }
}

// A pattern from before FEEL: no section 5. It plays straight, and the rest is intact.
TEST(a_pattern_without_the_feel_section_loads_with_feel_off) {
    make_busy_pattern(g_source);
    g_source.feel = 9;
    uint32_t written = 0;
    CHECK_EQ(int(pattern_write(g_source, g_buffer, kBufferBytes, &written)),
             int(SerializeResult::ok));
    poke_u16(g_buffer, entry_of(g_buffer, kSectionFeel), 998);
    refresh_crc(g_buffer);
    g_loaded.feel = 5;
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.feel), 0);
    CHECK_EQ(int(g_loaded.swing), int(g_source.swing));
    CHECK_EQ(int(g_loaded.locks_used), int(pattern_locks_reachable(g_source)));
}

TEST(an_unknown_feel_loads_as_off_and_a_short_feel_section_is_malformed) {
    uint32_t size = write_busy();
    g_buffer[section_at(g_buffer, kSectionFeel)] = uint8_t(kFeelMax + 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.feel), 0);

    size = write_busy();
    poke_u32(g_buffer, entry_of(g_buffer, kSectionFeel) + 8, 0u);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size)), int(SerializeResult::malformed));
}

TEST(an_unknown_play_mode_loads_as_forward_and_a_short_section_is_malformed) {
    uint32_t size = write_busy();
    g_buffer[section_at(g_buffer, kSectionPlayModes) + 3] = uint8_t(PlayMode::count);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.tracks[3].play), int(PlayMode::fwd));

    size = write_busy();
    const uint32_t entry = entry_of(g_buffer, kSectionPlayModes);
    poke_u32(g_buffer, entry + 8, kPlayModesSectionBytes - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(pattern_read(g_loaded, g_buffer, size)), int(SerializeResult::malformed));
}
