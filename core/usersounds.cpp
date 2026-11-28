// Your own sounds. core/usersounds.hpp has the design and the bytes.
//
// The container is built from serialize.hpp's primitives, the way core/project.cpp and
// core/perform.cpp build theirs, and its checks run in the same order (payload against the
// buffer, CRC, version, table) so that every STRATA file gives the same reason for the same
// damage.
#include "usersounds.hpp"

#include "sounds.hpp"

namespace strata {

namespace {

constexpr uint32_t kBankFixedBytes = 6;
constexpr uint32_t kRecordHeadBytes = 1 + uint32_t(kUserSoundNameChars) + 1;
constexpr uint32_t kRecordBytes = kRecordHeadBytes + uint32_t(kKitParams);
static_assert(kRecordBytes <= 255, "R is one byte on disk");
static_assert(kKitParams <= 255, "P is one byte on disk");
constexpr uint32_t kSectionBytes = kBankFixedBytes + uint32_t(kUserSoundSlots) * kRecordBytes;
constexpr uint32_t kExtrasFixedBytes = 2;
constexpr uint32_t kExtrasBytes =
    kExtrasFixedBytes + uint32_t(kUserSoundSlots) * uint32_t(kUserSoundExtraBytes);
static_assert(kUserSoundExtraBytes <= 255, "E is one byte on disk");
constexpr uint32_t kSections = 2;
constexpr uint32_t kTableBytes = kSections * kSectionEntryBytes;
constexpr uint32_t kTotalBytes = kHeaderBytes + kTableBytes + kSectionBytes + kExtrasBytes;

bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
           c == '+' || c == '.' || c == '#' || c == '!';
}

int length(const char *text) {
    int n = 0;
    while (text[n] != '\0') n++;
    return n;
}

bool same(const char *a, const char *b) {
    int i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i]) return false;
    }
    return a[i] == b[i];
}

void copy_name(char *out, const char *name) {
    int i = 0;
    for (; i < kUserSoundNameChars && name[i] != '\0'; i++) out[i] = name[i];
    for (; i <= kUserSoundNameChars; i++) out[i] = '\0';
}

// Extras made safe, as KIT.BIN's sections 9, 10 and 12 make theirs.
UserSoundExtras sanitized(const UserSoundExtras &in) {
    UserSoundExtras out;
    out.lfo_trig = in.lfo_trig < kLfoTrigs ? in.lfo_trig : kLfoTrigFree;
    for (int m = 0; m < kModSlots; m++) {
        out.mod[m] = mod_slot_sanitized(in.mod[m].source, in.mod[m].dest, uint8_t(in.mod[m].depth));
    }
    out.arp = kit_arp_sanitized(in.arp);
    return out;
}

}  // namespace

UserSoundExtras user_sound_extras_of(const KitTrack &track) {
    UserSoundExtras e;
    e.lfo_trig = track.lfo_trig;
    for (int m = 0; m < kModSlots; m++) e.mod[m] = track.mod[m];
    e.arp = track.arp;
    return e;
}

bool user_sound_extras_equal(const UserSoundExtras &a, const UserSoundExtras &b) {
    if (a.lfo_trig != b.lfo_trig) return false;
    for (int m = 0; m < kModSlots; m++) {
        if (a.mod[m].source != b.mod[m].source || a.mod[m].dest != b.mod[m].dest ||
            a.mod[m].depth != b.mod[m].depth) {
            return false;
        }
    }
    for (int f = 0; f < kArpExtFields; f++) {
        if (arp_ext_get(a.arp, f) != arp_ext_get(b.arp, f)) return false;
    }
    return true;
}

bool user_sound_owns(uint8_t machine, int p) {
    return user_sounds_supported(machine) && kit_param_real(p);
}

bool user_sounds_supported(uint8_t machine) { return sound_preset_writes(machine) != 0; }

void user_bank_init(UserSoundBank &bank, uint8_t machine) {
    bank = UserSoundBank{};
    bank.machine = machine;
}

int user_bank_count(const UserSoundBank &bank) {
    int n = 0;
    for (const UserSound &s : bank.slots) n += s.used ? 1 : 0;
    return n;
}

int user_bank_nth(const UserSoundBank &bank, int n) {
    if (n < 0) return -1;
    for (int slot = 0; slot < kUserSoundSlots; slot++) {
        if (!bank.slots[slot].used) continue;
        if (n == 0) return slot;
        n--;
    }
    return -1;
}

int user_bank_free_slot(const UserSoundBank &bank) {
    for (int slot = 0; slot < kUserSoundSlots; slot++) {
        if (!bank.slots[slot].used) return slot;
    }
    return -1;
}

int user_bank_find(const UserSoundBank &bank, const char *name) {
    if (name == nullptr) return -1;
    for (int slot = 0; slot < kUserSoundSlots; slot++) {
        if (bank.slots[slot].used && same(bank.slots[slot].name, name)) return slot;
    }
    return -1;
}

bool user_sound_name_valid(const char *name) {
    if (name == nullptr) return false;
    const int n = length(name);
    if (n < 1 || n > kUserSoundNameChars) return false;
    if (name[0] == ' ' || name[n - 1] == ' ') return false;
    for (int i = 0; i < n; i++) {
        if (!name_char(name[i])) return false;
    }
    return true;
}

void user_sound_default_name(const UserSoundBank &bank, const char *base, char *out) {
    if (base != nullptr && user_sound_name_valid(base) && user_bank_find(bank, base) < 0) {
        copy_name(out, base);
        return;
    }
    // "KCK 1" .. "KCK 17": seventeen candidates for sixteen slots, so one is always free.
    const char *machine = machine_name(bank.machine);
    for (int number = 1; number <= kUserSoundSlots + 1; number++) {
        char name[kUserSoundNameChars + 1] = {};
        int i = 0;
        for (; machine[i] != '\0' && i < 3; i++) name[i] = machine[i];
        name[i++] = ' ';
        if (number >= 10) name[i++] = char('0' + number / 10);
        name[i++] = char('0' + number % 10);
        name[i] = '\0';
        if (user_bank_find(bank, name) < 0) {
            copy_name(out, name);
            return;
        }
    }
    copy_name(out, "SOUND");   // unreachable: one of the seventeen is free
}

bool user_sound_store(UserSoundBank &bank, int slot, const char *name, const uint8_t *params,
                      const UserSoundExtras &extras) {
    if (slot < 0 || slot >= kUserSoundSlots || params == nullptr) return false;
    if (!user_sound_name_valid(name)) return false;
    UserSound &s = bank.slots[slot];
    s.used = true;
    copy_name(s.name, name);
    for (int p = 0; p < kKitParams; p++) {
        const uint8_t top = kit_param_max(p);
        s.params[p] = params[p] > top ? top : params[p];
    }
    s.extras = sanitized(extras);
    return true;
}

bool user_sound_rename(UserSoundBank &bank, int slot, const char *name) {
    if (slot < 0 || slot >= kUserSoundSlots || !bank.slots[slot].used) return false;
    if (!user_sound_name_valid(name)) return false;
    copy_name(bank.slots[slot].name, name);
    return true;
}

bool user_sound_delete(UserSoundBank &bank, int slot) {
    if (slot < 0 || slot >= kUserSoundSlots || !bank.slots[slot].used) return false;
    bank.slots[slot] = UserSound{};
    return true;
}

bool user_sound_apply(uint8_t machine, const UserSound &sound, uint8_t *params) {
    if (!sound.used || params == nullptr || !user_sounds_supported(machine)) return false;
    for (int p = 0; p < kKitParams; p++) {
        if (user_sound_owns(machine, p)) params[p] = sound.params[p];
    }
    return true;
}

bool user_sound_matches(uint8_t machine, const UserSound &sound, const uint8_t *params,
                        const UserSoundExtras &extras) {
    if (!sound.used || params == nullptr || !user_sounds_supported(machine)) return false;
    for (int p = 0; p < kKitParams; p++) {
        if (user_sound_owns(machine, p) && params[p] != sound.params[p]) return false;
    }
    return user_sound_extras_equal(sound.extras, extras);
}

bool user_bank_file_name(uint8_t machine, char *out, int max) {
    if (out == nullptr || max < 8 || !user_sounds_supported(machine)) return false;
    const char *name = machine_name(machine);
    int i = 0;
    for (; name[i] != '\0' && i < 3; i++) out[i] = name[i];
    const char *ext = ".BIN";
    for (int k = 0; ext[k] != '\0'; k++) out[i++] = ext[k];
    out[i] = '\0';
    return true;
}

uint32_t user_bank_bytes() { return kTotalBytes; }

SerializeResult user_bank_write(const UserSoundBank &bank, uint8_t *buffer, uint32_t capacity,
                                uint32_t *written) {
    if (written != nullptr) *written = 0;
    if (buffer == nullptr || capacity < kTotalBytes) return SerializeResult::buffer_too_small;

    Writer w(buffer, capacity);
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kMagic2);
    w.u8(kMagic3);
    w.u16(kUserSoundVersion);
    w.u16(uint16_t(kSections));
    w.u32(kTotalBytes - kHeaderBytes);
    w.u32(0);   // the CRC, patched below
    w.u16(kSectionUserSounds);
    w.u16(0);   // reserved
    w.u32(kTableBytes);
    w.u32(kSectionBytes);
    w.u16(kSectionUserSoundExtras);
    w.u16(0);
    w.u32(kTableBytes + kSectionBytes);
    w.u32(kExtrasBytes);

    w.u8(bank.machine);
    w.u8(kKitFilterLayout);
    w.u8(kLfoLayoutNumbers);
    w.u8(uint8_t(kUserSoundSlots));
    w.u8(uint8_t(kRecordBytes));
    w.u8(uint8_t(kKitParams));
    for (const UserSound &s : bank.slots) {
        w.u8(s.used ? 1 : 0);
        for (int i = 0; i < kUserSoundNameChars; i++) w.u8(s.used ? uint8_t(s.name[i]) : 0);
        w.u8(0);
        for (int p = 0; p < kKitParams; p++) w.u8(s.used ? s.params[p] : 0);
    }
    w.u8(uint8_t(kUserSoundSlots));
    w.u8(uint8_t(kUserSoundExtraBytes));
    for (const UserSound &s : bank.slots) {
        const UserSoundExtras e = s.used ? s.extras : UserSoundExtras{};
        w.u8(e.lfo_trig);
        for (int m = 0; m < kModSlots; m++) {
            w.u8(e.mod[m].source);
            w.u8(e.mod[m].dest);
            w.u8(uint8_t(e.mod[m].depth));
        }
        for (int f = 0; f < kArpExtFields; f++) w.u8(arp_ext_get(e.arp, f));
    }

    if (!w.ok() || w.size() != kTotalBytes) return SerializeResult::buffer_too_small;
    w.patch_u32(kCrcOffset, crc32(buffer + kHeaderBytes, kTotalBytes - kHeaderBytes));
    if (!w.ok()) return SerializeResult::buffer_too_small;
    if (written != nullptr) *written = kTotalBytes;
    return SerializeResult::ok;
}

SerializeResult user_bank_read(UserSoundBank &bank, uint8_t machine, const uint8_t *buffer,
                               uint32_t size) {
    if (buffer == nullptr || size < kHeaderBytes) return SerializeResult::truncated;
    Reader r(buffer, size);
    const bool magic =
        r.u8() == kMagic0 && r.u8() == kMagic1 && r.u8() == kMagic2 && r.u8() == kMagic3;
    if (!magic) return SerializeResult::bad_magic;
    const uint16_t version = r.u16();
    const uint32_t sections = r.u16();
    const uint32_t payload = r.u32();
    const uint32_t want_crc = r.u32();
    if (!r.ok()) return SerializeResult::truncated;
    if (payload > size - kHeaderBytes) return SerializeResult::truncated;
    if (crc32(buffer + kHeaderBytes, payload) != want_crc) return SerializeResult::bad_crc;
    if (version != kUserSoundVersion) return SerializeResult::bad_version;
    if (sections == 0 || sections > kMaxSections) return SerializeResult::malformed;
    const uint32_t table = sections * kSectionEntryBytes;
    if (table > payload) return SerializeResult::malformed;

    // Find the bank section and the extras; skip any this build does not know.
    uint32_t at = 0, len = 0, ext_at = 0, ext_len = 0;
    for (uint32_t i = 0; i < sections; i++) {
        if (!r.seek(kHeaderBytes + i * kSectionEntryBytes)) return SerializeResult::malformed;
        const uint16_t id = r.u16();
        r.skip(2);
        const uint32_t off = r.u32();
        const uint32_t bytes = r.u32();
        if (!r.ok()) return SerializeResult::truncated;
        if (off < table || off > payload || bytes > payload - off) return SerializeResult::malformed;
        if (id == kSectionUserSoundExtras) {
            if (ext_at != 0) return SerializeResult::malformed;
            ext_at = off;
            ext_len = bytes;
            continue;
        }
        if (id != kSectionUserSounds) continue;
        if (at != 0) return SerializeResult::malformed;   // the same id twice
        at = off;
        len = bytes;
    }
    if (at == 0 || len < kBankFixedBytes) return SerializeResult::malformed;

    Reader s(buffer + kHeaderBytes + at, len);
    const uint8_t file_machine = s.u8();
    const uint8_t filter_layout = s.u8();
    const uint8_t lfo_layout = s.u8();
    const uint32_t records = s.u8();
    const uint32_t record_bytes = s.u8();
    const uint32_t params = s.u8();
    if (file_machine != machine || !user_sounds_supported(machine)) return SerializeResult::malformed;
    // A layout this build does not know is a newer meaning of the same bytes.
    if (filter_layout != kKitFilterLayout || lfo_layout != kLfoLayoutNumbers) {
        return SerializeResult::bad_version;
    }
    if (record_bytes < kRecordHeadBytes || params > record_bytes - kRecordHeadBytes) {
        return SerializeResult::malformed;
    }
    if (uint64_t(records) * record_bytes > len - kBankFixedBytes) return SerializeResult::malformed;

    // The machine's factory sound under every byte the file does not carry, so a record from a
    // build with fewer parameters loads as it sounded there.
    uint8_t defaults[kKitParams] = {};
    machine_defaults_apply(machine, defaults);

    // The extras, when the file has them: a fresh track's for any slot or byte it does not carry.
    UserSoundExtras extras[kUserSoundSlots];
    if (ext_at != 0) {
        if (ext_len < kExtrasFixedBytes) return SerializeResult::malformed;
        Reader x(buffer + kHeaderBytes + ext_at, ext_len);
        const uint32_t ext_records = x.u8();
        const uint32_t ext_bytes = x.u8();
        if (uint64_t(ext_records) * ext_bytes > ext_len - kExtrasFixedBytes) {
            return SerializeResult::malformed;
        }
        const uint32_t ext_take =
            ext_records < uint32_t(kUserSoundSlots) ? ext_records : uint32_t(kUserSoundSlots);
        for (uint32_t k = 0; k < ext_take; k++) {
            if (!x.seek(kExtrasFixedBytes + k * ext_bytes)) return SerializeResult::malformed;
            uint8_t b[kUserSoundExtraBytes] = {};
            UserSoundExtras fresh;
            b[0] = fresh.lfo_trig;
            for (int m = 0; m < kModSlots; m++) {
                b[1 + 3 * m] = fresh.mod[m].source;
                b[2 + 3 * m] = fresh.mod[m].dest;
                b[3 + 3 * m] = uint8_t(fresh.mod[m].depth);
            }
            for (int f = 0; f < kArpExtFields; f++) b[1 + 3 * kModSlots + f] = arp_ext_get(fresh.arp, f);
            for (uint32_t i = 0; i < ext_bytes && i < uint32_t(kUserSoundExtraBytes); i++) b[i] = x.u8();
            if (!x.ok()) return SerializeResult::truncated;
            UserSoundExtras &e = extras[k];
            e.lfo_trig = b[0];
            for (int m = 0; m < kModSlots; m++) {
                e.mod[m].source = b[1 + 3 * m];
                e.mod[m].dest = b[2 + 3 * m];
                e.mod[m].depth = int8_t(b[3 + 3 * m]);
            }
            for (int f = 0; f < kArpExtFields; f++) arp_ext_set(e.arp, f, b[1 + 3 * kModSlots + f]);
        }
    }

    UserSoundBank read;
    user_bank_init(read, machine);
    const uint32_t take = records < uint32_t(kUserSoundSlots) ? records : uint32_t(kUserSoundSlots);
    for (uint32_t k = 0; k < take; k++) {
        if (!s.seek(kBankFixedBytes + k * record_bytes)) return SerializeResult::malformed;
        const uint8_t used = s.u8();
        char name[kUserSoundNameChars + 1] = {};
        for (int i = 0; i < kUserSoundNameChars; i++) name[i] = char(s.u8());
        s.skip(1);
        uint8_t bytes[kKitParams];
        for (int p = 0; p < kKitParams; p++) {
            bytes[p] = uint32_t(p) < params ? s.u8() : defaults[p];
        }
        if (!s.ok()) return SerializeResult::truncated;
        if (used == 0) continue;
        // A damaged name is still the user's sound: keep the sound, mend the name.
        for (int i = 0; i < kUserSoundNameChars; i++) {
            if (name[i] != '\0' && !name_char(name[i])) name[i] = '-';
        }
        int n = length(name);
        while (n > 0 && name[n - 1] == ' ') name[--n] = '\0';
        int lead = 0;
        while (name[lead] == ' ') lead++;
        if (!user_sound_store(read, int(k), name[lead] != '\0' ? name + lead : "SOUND", bytes,
                              extras[k])) {
            return SerializeResult::malformed;
        }
    }
    bank = read;
    return SerializeResult::ok;
}

}  // namespace strata
