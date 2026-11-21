// The byte-level serializer: a versioned container with a CRC-32, and the Pattern round trip
// built on it.
//
// CLAUDE.md: "Never cast file bytes to structs: serialize byte by byte." Nothing here ever
// forms a pointer to a struct inside a file buffer. `Writer` and `Reader` move one byte at a
// time and assemble wider values by shift and mask, so the code neither cares about the host's
// endianness nor about how the buffer happens to be aligned -- which on the C674x is the
// difference between reading a pattern and reading whatever was next to it in DDR.
//
// core/ knows nothing about files or Enaga. These functions work on a byte buffer the caller
// owns; `app/` is where the result meets `files::save`. Nothing here allocates.
//
// Container layout, little-endian throughout:
//
//     offset  size  field
//     0       4     magic "STRA"
//     4       2     format version
//     6       2     section count
//     8       4     payload bytes: everything after this 16-byte header
//     12      4     CRC-32 of those payload bytes
//     16      ...   payload
//
// and the payload starts with the section table, `section count` entries of
//
//     0       2     section id
//     2       2     reserved, 0
//     4       4     offset of the section's bytes from the start of the payload
//     8       4     size of the section's bytes
//
// followed by the sections themselves. Offsets rather than sizes alone so that a future
// version can grow a section or add one and an older reader can still find, or skip, the
// sections it knows.
//
// The header is fixed for all time and is not covered by the CRC, which docs/SPEC.md puts
// "over the payload". The consequence is deliberate: the magic and the version are checked by
// value, and `payload bytes` is checked against the buffer, so a damaged header can only make
// a load refuse -- it can never make the reader run off the end of the buffer. A damaged
// payload is what the CRC is for.
#pragma once

#include <cstdint>

#include "pattern.hpp"

namespace strata {

// --- CRC-32 ---------------------------------------------------------------

// CRC-32 as in IEEE 802.3 / zip: reflected, polynomial 0xEDB88320, initial value all ones,
// final complement. crc32(nullptr, 0) is 0. Computed a bit at a time; no 1 KiB table, because
// this runs on a save and a load, not in a hot loop.
uint32_t crc32(const uint8_t *data, uint32_t size);

// --- byte cursors ---------------------------------------------------------

// A bounds-checked write cursor over caller-owned memory. Once a write does not fit, `ok()` is
// false for good and nothing further is stored, so a caller may write a whole structure and
// check once at the end.
class Writer {
public:
    Writer(uint8_t *buffer, uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}

    void u8(uint8_t value);
    void i8(int8_t value);
    void u16(uint16_t value);
    void u32(uint32_t value);
    void bytes(const uint8_t *data, uint32_t size);
    void zeros(uint32_t size);

    // Rewrites a value already stored, for a header field whose value is only known later.
    // Does nothing, and clears `ok()`, if the range is not inside what has been written.
    void patch_u32(uint32_t at, uint32_t value);

    uint32_t size() const { return size_; }
    bool ok() const { return ok_; }

private:
    uint8_t *buffer_;
    uint32_t capacity_;
    uint32_t size_ = 0;
    bool ok_ = true;
};

// A bounds-checked read cursor. A read that would pass the end of the buffer yields 0 and
// clears `ok()` for good, so no sequence of reads can leave the buffer. Check `ok()` once
// after a group of reads rather than after each one.
class Reader {
public:
    Reader(const uint8_t *buffer, uint32_t size) : buffer_(buffer), size_(size) {}

    uint8_t u8();
    int8_t i8();
    uint16_t u16();
    uint32_t u32();
    void skip(uint32_t size);

    // Moves to an absolute position. False, and `ok()` cleared, if it is past the end.
    bool seek(uint32_t position);

    uint32_t position() const { return position_; }
    uint32_t remaining() const { return position_ <= size_ ? size_ - position_ : 0; }
    bool ok() const { return ok_; }

private:
    const uint8_t *buffer_;
    uint32_t size_;
    uint32_t position_ = 0;
    bool ok_ = true;
};

// --- container ------------------------------------------------------------

constexpr uint8_t kMagic0 = 'S';
constexpr uint8_t kMagic1 = 'T';
constexpr uint8_t kMagic2 = 'R';
constexpr uint8_t kMagic3 = 'A';

constexpr uint32_t kHeaderBytes = 16;
constexpr uint32_t kSectionEntryBytes = 12;

// Where the CRC-32 sits in the header. The writer back-fills it there once it knows the
// payload, and a test that corrupts a file on purpose needs to know as well.
constexpr uint32_t kVersionOffset = 4;
constexpr uint32_t kCrcOffset = 12;

// A reader with nothing better to do would otherwise believe a damaged count.
constexpr uint32_t kMaxSections = 64;

// Bumped when the meaning of the bytes changes. An older build must refuse a newer file rather
// than guess at it, which is what `bad_version` is for.
constexpr uint16_t kPatternVersion = 1;

// Section ids are permanent. A new section takes the next free number; a section that goes
// away leaves its number burnt.
constexpr uint16_t kSectionPatternHeader = 1;
constexpr uint16_t kSectionTracks = 2;
constexpr uint16_t kSectionLocks = 3;

enum class SerializeResult : uint8_t {
    ok = 0,
    buffer_too_small,   // writing: the caller's buffer cannot hold the whole pattern
    truncated,          // reading: the buffer ends inside the bytes the header declares
    bad_magic,          // reading: not a STRATA container
    bad_version,        // reading: a format this build does not know
    bad_crc,            // reading: the payload does not match its CRC-32
    malformed,          // reading: the CRC agrees but the structure does not add up
};

// A short, screen-ready reason, for the message docs/SPEC.md requires on a refused load.
// Upper case and spaces only: the device font has no lower case.
const char *serialize_result_name(SerializeResult result);

// --- pattern round trip ---------------------------------------------------

constexpr uint32_t kPatternSections = 3;
constexpr uint32_t kPatternHeaderSectionBytes = 12;

// On-disk sizes, stated separately from the structs. They agree with sizeof today and the
// asserts below say so, but the file format is not allowed to follow a struct that changes.
constexpr uint32_t kStepBytes = 12;
constexpr uint32_t kLockBytes = 2;
static_assert(kStepBytes == sizeof(Step), "the step record and the Step struct have diverged");
static_assert(kLockBytes == sizeof(Lock), "the lock record and the Lock struct have diverged");

constexpr uint32_t kTrackBytes = 3 + uint32_t(kStepsPerTrack) * kStepBytes;
constexpr uint32_t kTracksSectionBytes = kTrackBytes * uint32_t(kTracks);
constexpr uint32_t kPatternFixedBytes =
    kHeaderBytes + kPatternSections * kSectionEntryBytes + kPatternHeaderSectionBytes +
    kTracksSectionBytes;

// The most a pattern can ever take: size a static buffer with this and `pattern_write` can
// never refuse it. 8,280 bytes as the format stands.
constexpr uint32_t kPatternMaxBytes = kPatternFixedBytes + uint32_t(kLockPool) * kLockBytes;

// Exactly what `pattern_write` will produce for this pattern. Smaller than kPatternMaxBytes by
// the unused part of the lock pool.
uint32_t pattern_bytes(const Pattern &pattern);

// Writes `pattern` into `buffer`, which the caller owns and which is left untouched if it is
// too small. `written`, when given, receives the byte count -- the same number
// `pattern_bytes` returns.
//
// The lock pool is written compacted: locks are renumbered into track-then-step order and the
// slots abandoned by editing are dropped, which is docs/SPEC.md's "compacted on save". The
// pattern in RAM is not modified, so a save running in the main context writes nothing the
// audio side owns. Call `pattern_compact_locks` as well if the pool itself should be reclaimed.
SerializeResult pattern_write(const Pattern &pattern, uint8_t *buffer, uint32_t capacity,
                              uint32_t *written = nullptr);

// Reads a pattern written by `pattern_write`. `size` may be larger than the pattern -- a bank
// file is 16 of these back to back -- and `consumed`, when given, receives how many bytes this
// one took.
//
// On anything other than `ok` the pattern is left empty, never half filled, and nothing
// outside buffer[0, size) is read. Loading is value-tolerant but structure-strict: a step
// field out of range is clamped by `step_sanitize`, while a lock run that does not match the
// compacted layout the writer promises is `malformed`.
SerializeResult pattern_read(Pattern &pattern, const uint8_t *buffer, uint32_t size,
                             uint32_t *consumed = nullptr);

}  // namespace strata
