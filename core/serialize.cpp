// The serializer: CRC-32, the two byte cursors, and the Pattern round trip.
//
// Three rules shape all of it.
//
//   1. No file byte is ever reached through a struct pointer. Values are assembled from bytes
//      by shift and mask, which is why the format is the same on the host and on a C674x that
//      would otherwise read a misaligned uint16_t from the wrong address entirely.
//   2. A reader never trusts a number it has just read. Sizes and offsets are checked against
//      the buffer before they are used, and `Reader` refuses to pass the end even if a check
//      is forgotten. Both together, because one of them is the belt and the other the braces.
//   3. A refused load leaves an empty pattern, never a half-filled one. docs/SPEC.md: an
//      unknown version or a bad CRC "never crashes the app".
#include "serialize.hpp"

namespace strata {

// --- CRC-32 ---------------------------------------------------------------

uint32_t crc32(const uint8_t *data, uint32_t size) {
    if (data == nullptr) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            // mask is all ones when the low bit is set, so no branch per bit.
            const uint32_t mask = uint32_t(0) - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// --- Writer ---------------------------------------------------------------

void Writer::u8(uint8_t value) {
    if (!ok_) return;
    if (size_ >= capacity_) {
        ok_ = false;
        return;
    }
    buffer_[size_++] = value;
}

void Writer::i8(int8_t value) { u8(uint8_t(value)); }

void Writer::u16(uint16_t value) {
    u8(uint8_t(value & 0xFFu));
    u8(uint8_t(value >> 8));
}

void Writer::u32(uint32_t value) {
    u16(uint16_t(value & 0xFFFFu));
    u16(uint16_t(value >> 16));
}

void Writer::bytes(const uint8_t *data, uint32_t size) {
    if (data == nullptr) {
        if (size != 0) ok_ = false;
        return;
    }
    for (uint32_t i = 0; i < size; i++) u8(data[i]);
}

void Writer::zeros(uint32_t size) {
    for (uint32_t i = 0; i < size; i++) u8(0);
}

void Writer::patch_u32(uint32_t at, uint32_t value) {
    if (!ok_) return;
    if (at > size_ || size_ - at < 4) {
        ok_ = false;
        return;
    }
    buffer_[at + 0] = uint8_t(value & 0xFFu);
    buffer_[at + 1] = uint8_t((value >> 8) & 0xFFu);
    buffer_[at + 2] = uint8_t((value >> 16) & 0xFFu);
    buffer_[at + 3] = uint8_t((value >> 24) & 0xFFu);
}

// --- Reader ---------------------------------------------------------------

uint8_t Reader::u8() {
    if (!ok_) return 0;
    if (position_ >= size_) {
        ok_ = false;
        return 0;
    }
    return buffer_[position_++];
}

int8_t Reader::i8() { return int8_t(u8()); }

// Wide reads are all or nothing: a value that straddles the end of the buffer comes back as 0
// with `ok()` cleared, rather than as the bytes that happened to be there. Half a value looks
// like data, and the one thing a reader of a damaged file must not do is act on it.
uint16_t Reader::u16() {
    if (!ok_) return 0;
    if (remaining() < 2) {
        position_ = size_;
        ok_ = false;
        return 0;
    }
    const uint16_t low = u8();
    const uint16_t high = u8();
    return uint16_t(low | uint16_t(high << 8));
}

uint32_t Reader::u32() {
    if (!ok_) return 0;
    if (remaining() < 4) {
        position_ = size_;
        ok_ = false;
        return 0;
    }
    const uint32_t low = u16();
    const uint32_t high = u16();
    return low | (high << 16);
}

void Reader::skip(uint32_t size) {
    if (!ok_) return;
    if (size > size_ - position_) {
        position_ = size_;
        ok_ = false;
        return;
    }
    position_ += size;
}

bool Reader::seek(uint32_t position) {
    if (!ok_) return false;
    if (position > size_) {
        ok_ = false;
        return false;
    }
    position_ = position;
    return true;
}

// --- results --------------------------------------------------------------

const char *serialize_result_name(SerializeResult result) {
    switch (result) {
        case SerializeResult::ok: return "OK";
        case SerializeResult::buffer_too_small: return "BUFFER TOO SMALL";
        case SerializeResult::truncated: return "FILE TRUNCATED";
        case SerializeResult::bad_magic: return "NOT A STRATA FILE";
        case SerializeResult::bad_version: return "UNKNOWN VERSION";
        case SerializeResult::bad_crc: return "BAD CHECKSUM";
        case SerializeResult::malformed: return "DAMAGED PATTERN";
    }
    return "UNKNOWN ERROR";
}

// --- pattern ---------------------------------------------------------------

namespace {

constexpr uint32_t kTableBytes = kPatternSections * kSectionEntryBytes;
constexpr uint32_t kHeaderSectionOffset = kTableBytes;
constexpr uint32_t kTracksSectionOffset = kHeaderSectionOffset + kPatternHeaderSectionBytes;
constexpr uint32_t kPlayModesSectionOffset = kTracksSectionOffset + kTracksSectionBytes;
constexpr uint32_t kLocksSectionOffset = kPlayModesSectionOffset + kPlayModesSectionBytes;
static_assert(kHeaderBytes + kLocksSectionOffset == kPatternFixedBytes,
              "the fixed part of a pattern file is not the sum of its sections");

uint8_t clamp_u8(uint8_t value, uint8_t low, uint8_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// How many of a step's locks a save will emit. Both passes of the writer and `pattern_bytes`
// apply this same rule to the same steps in the same order, so the lock indices the first pass
// writes are exactly the positions the second pass fills. `limit` only bites for a pattern
// assembled by hand with overlapping runs; a loaded one cannot have any.
uint8_t emit_count(uint32_t running, uint8_t count, uint16_t limit) {
    return running + count > limit ? 0 : count;
}

// A track's swing is either "follow the pattern" or a real amount.
uint8_t sanitize_track_swing(uint8_t swing) {
    if (swing == kSwingInherit) return kSwingInherit;
    return clamp_u8(swing, uint8_t(kSwingMin), uint8_t(kSwingMax));
}

}  // namespace

uint32_t pattern_bytes(const Pattern &pattern) {
    return kPatternFixedBytes + uint32_t(pattern_locks_reachable(pattern)) * kLockBytes;
}

SerializeResult pattern_write(const Pattern &pattern, uint8_t *buffer, uint32_t capacity,
                              uint32_t *written) {
    if (written != nullptr) *written = 0;

    const uint16_t locks = pattern_locks_reachable(pattern);
    const uint32_t lock_bytes = uint32_t(locks) * kLockBytes;
    const uint32_t total = kPatternFixedBytes + lock_bytes;
    // Checked before a single byte is stored, so a buffer that is too small comes back
    // untouched rather than half written.
    if (buffer == nullptr || capacity < total) return SerializeResult::buffer_too_small;

    const uint32_t payload = total - kHeaderBytes;

    Writer w(buffer, capacity);

    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kMagic2);
    w.u8(kMagic3);
    w.u16(kPatternVersion);
    w.u16(uint16_t(kPatternSections));
    w.u32(payload);
    w.u32(0);   // CRC-32, patched once the payload is written

    w.u16(kSectionPatternHeader);
    w.u16(0);
    w.u32(kHeaderSectionOffset);
    w.u32(kPatternHeaderSectionBytes);

    w.u16(kSectionTracks);
    w.u16(0);
    w.u32(kTracksSectionOffset);
    w.u32(kTracksSectionBytes);

    w.u16(kSectionPlayModes);
    w.u16(0);
    w.u32(kPlayModesSectionOffset);
    w.u32(kPlayModesSectionBytes);

    w.u16(kSectionLocks);
    w.u16(0);
    w.u32(kLocksSectionOffset);
    w.u32(lock_bytes);

    w.u8(uint8_t(kTracks));
    w.u8(uint8_t(kStepsPerTrack));
    w.u8(pattern.master_length);
    w.u8(pattern.swing);
    w.u8(pattern.root);
    w.u8(pattern.scale);
    w.u16(locks);
    w.u32(pattern.seed);

    // Pass one: the steps, with each step's locks renumbered into the compacted positions the
    // second pass will write them to.
    uint32_t running = 0;
    for (int t = 0; t < kTracks; t++) {
        const TrackPattern &track = pattern.tracks[t];
        w.u8(track.length);
        w.u8(track.speed);
        w.u8(track.swing);
        for (int i = 0; i < kStepsPerTrack; i++) {
            const Step &step = track.steps[i];
            const uint8_t count =
                emit_count(running, pattern_step_locks(pattern, t, i).count, locks);
            w.u8(step.flags);
            w.u8(step.note);
            w.u8(step.velocity);
            w.u8(step.length);
            w.i8(step.micro);
            w.u8(step.probability);
            w.u8(step.condition);
            w.u8(step.ratchet);
            w.u16(count == 0 ? kNoLock : uint16_t(running));
            w.u8(count);
            w.u8(step.chord);
            running += count;
        }
    }

    // The play modes, between the tracks and the variable-length lock pool.
    for (int t = 0; t < kTracks; t++) w.u8(pattern.tracks[t].play);

    // Pass two: the pool itself, in the same order.
    running = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const LockView view = pattern_step_locks(pattern, t, i);
            const uint8_t count = emit_count(running, view.count, locks);
            for (uint8_t k = 0; k < count; k++) {
                w.u8(view.locks[k].param);
                w.u8(view.locks[k].value);
            }
            running += count;
        }
    }

    // `pattern_bytes` promised this exact size; if it did not come out, the format constants
    // and the writer disagree and the file must not be handed on as valid.
    if (!w.ok() || w.size() != total) return SerializeResult::buffer_too_small;

    w.patch_u32(kCrcOffset, crc32(buffer + kHeaderBytes, payload));
    if (!w.ok()) return SerializeResult::buffer_too_small;

    if (written != nullptr) *written = total;
    return SerializeResult::ok;
}

SerializeResult pattern_read(Pattern &pattern, const uint8_t *buffer, uint32_t size,
                             uint32_t *consumed) {
    if (consumed != nullptr) *consumed = 0;

    // Every exit but the last one goes through here, so a refused load always leaves an empty
    // pattern rather than whatever the caller had or a half-read one.
    const auto refuse = [&pattern](SerializeResult why) {
        pattern_init(pattern);
        return why;
    };

    if (buffer == nullptr) return refuse(SerializeResult::truncated);
    if (size < kHeaderBytes) return refuse(SerializeResult::truncated);

    Reader r(buffer, size);
    const bool magic = r.u8() == kMagic0 && r.u8() == kMagic1 && r.u8() == kMagic2 &&
                       r.u8() == kMagic3;
    if (!magic) return refuse(SerializeResult::bad_magic);

    const uint16_t version = r.u16();
    const uint32_t sections = r.u16();
    const uint32_t payload = r.u32();
    const uint32_t want_crc = r.u32();
    if (!r.ok()) return refuse(SerializeResult::truncated);

    // The declared payload must be inside the buffer before the CRC may read it. `size` may be
    // larger: a bank file is 16 patterns back to back.
    if (payload > size - kHeaderBytes) return refuse(SerializeResult::truncated);
    if (crc32(buffer + kHeaderBytes, payload) != want_crc) return refuse(SerializeResult::bad_crc);
    if (version != kPatternVersion) return refuse(SerializeResult::bad_version);

    if (sections == 0 || sections > kMaxSections) return refuse(SerializeResult::malformed);
    const uint32_t table = sections * kSectionEntryBytes;
    if (table > payload) return refuse(SerializeResult::malformed);

    // The three sections a pattern needs, then the optional play modes; ids this build does not
    // know are skipped, which is what the offsets in the table are for.
    const uint16_t wanted[4] = {kSectionPatternHeader, kSectionTracks, kSectionLocks,
                                kSectionPlayModes};
    uint32_t offset[4] = {0, 0, 0, 0};
    uint32_t length[4] = {0, 0, 0, 0};
    bool found[4] = {false, false, false, false};

    for (uint32_t i = 0; i < sections; i++) {
        if (!r.seek(kHeaderBytes + i * kSectionEntryBytes)) {
            return refuse(SerializeResult::malformed);
        }
        const uint16_t id = r.u16();
        r.skip(2);   // reserved
        const uint32_t at = r.u32();
        const uint32_t len = r.u32();
        if (!r.ok()) return refuse(SerializeResult::truncated);
        // Inside the payload and clear of the table. `len > payload - at` rather than
        // `at + len > payload`, which could wrap.
        if (at < table || at > payload || len > payload - at) {
            return refuse(SerializeResult::malformed);
        }
        for (int k = 0; k < 4; k++) {
            if (id != wanted[k]) continue;
            if (found[k]) return refuse(SerializeResult::malformed);   // the same id twice
            offset[k] = at;
            length[k] = len;
            found[k] = true;
        }
    }
    if (!found[0] || !found[1] || !found[2]) return refuse(SerializeResult::malformed);

    if (length[0] < kPatternHeaderSectionBytes) return refuse(SerializeResult::malformed);
    if (length[1] < kTracksSectionBytes) return refuse(SerializeResult::malformed);
    // Present but short is damage, not an old file: a writer that declared it declared 8 bytes.
    if (found[3] && length[3] < kPlayModesSectionBytes) return refuse(SerializeResult::malformed);
    if (!r.seek(kHeaderBytes + offset[0])) return refuse(SerializeResult::malformed);

    const uint8_t tracks = r.u8();
    const uint8_t steps = r.u8();
    const uint8_t master_length = r.u8();
    const uint8_t pattern_swing = r.u8();
    const uint8_t root = r.u8();
    const uint8_t scale = r.u8();
    const uint16_t lock_total = r.u16();
    const uint32_t seed = r.u32();
    if (!r.ok()) return refuse(SerializeResult::truncated);

    // Version 1 is 8 tracks of 64 steps. A file claiming anything else is not something this
    // build can lay out, whatever its version number says.
    if (tracks != uint8_t(kTracks) || steps != uint8_t(kStepsPerTrack)) {
        return refuse(SerializeResult::malformed);
    }
    if (lock_total > uint16_t(kLockPool)) return refuse(SerializeResult::malformed);
    if (length[2] < uint32_t(lock_total) * kLockBytes) return refuse(SerializeResult::malformed);

    pattern_init(pattern);
    pattern.master_length = clamp_u8(master_length, 1, uint8_t(kStepsPerTrack));
    pattern.swing = clamp_u8(pattern_swing, uint8_t(kSwingMin), uint8_t(kSwingMax));
    pattern.root = uint8_t(root % 12u);
    pattern.scale = scale;
    pattern.seed = seed;

    if (!r.seek(kHeaderBytes + offset[1])) return refuse(SerializeResult::malformed);

    uint32_t running = 0;
    for (int t = 0; t < kTracks; t++) {
        TrackPattern &track = pattern.tracks[t];
        track.length = clamp_u8(r.u8(), 1, uint8_t(kStepsPerTrack));
        const uint8_t speed = r.u8();
        track.speed = speed < uint8_t(Speed::count) ? speed : uint8_t(Speed::x1);
        track.swing = sanitize_track_swing(r.u8());

        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &step = track.steps[i];
            step.flags = r.u8();
            step.note = r.u8();
            step.velocity = r.u8();
            step.length = r.u8();
            step.micro = r.i8();
            step.probability = r.u8();
            step.condition = r.u8();
            step.ratchet = r.u8();
            const uint16_t first = r.u16();
            const uint8_t count = r.u8();
            step.chord = r.u8();
            if (!r.ok()) return refuse(SerializeResult::truncated);

            // The writer always emits the compacted layout: runs in track-then-step order,
            // starting at 0, no gaps and no overlaps. Requiring it here is what makes the
            // in-RAM invariant true for every loaded pattern -- so the sequencer can walk a
            // run without checking it, and `pattern_clear_lock` can hand a slot back knowing
            // no other step shares it.
            if (count > uint8_t(kLocksPerStep)) return refuse(SerializeResult::malformed);
            if (count == 0) {
                step.lock_first = kNoLock;
                step.lock_count = 0;
            } else {
                if (first != uint16_t(running)) return refuse(SerializeResult::malformed);
                if (running + count > lock_total) return refuse(SerializeResult::malformed);
                step.lock_first = first;
                step.lock_count = count;
                running += count;
            }
            step_sanitize(step);
        }
    }
    if (running != lock_total) return refuse(SerializeResult::malformed);

    if (!r.seek(kHeaderBytes + offset[2])) return refuse(SerializeResult::malformed);
    // A lock's value is a track parameter, 0-127 like every kit byte; a file says otherwise only
    // if it is damaged or was made by hand, and the screen draws some of them as positions on a
    // fixed-size picture (EDIT's envelopes), so an out-of-range byte is clamped here, once, as
    // the kit's are. The param byte is left alone: an unknown one survives a round trip.
    for (uint16_t i = 0; i < lock_total; i++) {
        pattern.locks[i].param = r.u8();
        pattern.locks[i].value = clamp_u8(r.u8(), 0, 127);
    }
    if (!r.ok()) return refuse(SerializeResult::truncated);
    pattern.locks_used = lock_total;

    // Absent: a pattern from before play modes, and pattern_init left every track FWD. An
    // unknown mode (a newer build's) plays forward too, rather than being guessed at.
    if (found[3]) {
        if (!r.seek(kHeaderBytes + offset[3])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            const uint8_t play = r.u8();
            pattern.tracks[t].play = play < uint8_t(PlayMode::count) ? play : uint8_t(PlayMode::fwd);
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    if (consumed != nullptr) *consumed = kHeaderBytes + payload;
    return SerializeResult::ok;
}

}  // namespace strata
