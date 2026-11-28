// Your own sounds (core/usersounds.hpp), Cmd::sound_bytes, and the bank on the card (Storage).
//
// The file first: it round-trips, it refuses damage without touching the bank it was read into,
// and it is versioned the way every STRATA file is -- a record from a build with fewer
// parameters loads with the machine's defaults under the missing ones, one with more is read
// for what this build knows, and an unknown section is stepped over. Then the command: it writes
// every parameter but the hole, refuses another machine's sound, and AS WAS takes back all of a
// user sound or a paste -- the machine, every parameter and the LFO and ARP kit settings. Then Storage: a save makes the file, a card swap reads it
// back, and a file it cannot read is never written over.
#include "check.hpp"
#include "apprig.hpp"
#include "fakehost.hpp"

#include "project.hpp"
#include "serialize.hpp"
#include "sounds.hpp"
#include "usersounds.hpp"

#include <enaga/files.hpp>

#include <cstdio>
#include <cstring>

using namespace strata;

namespace {

constexpr uint8_t kKick = uint8_t(MachineId::kick);
constexpr uint8_t kMono = uint8_t(MachineId::va_mono);

uint8_t g_file[4096];

// A track's bytes with every one set to something a sound could hold, distinct per parameter.
void pattern_bytes(uint8_t *params, int seed) {
    for (int p = 0; p < kKitParams; p++) {
        const int top = kit_param_max(p);
        params[p] = uint8_t((p * 7 + seed) % (top + 1));
    }
}

// What a byte becomes in a sound: clamped to what its parameter may hold (the LFO's WAV, DST).
int clamped(int p, int value) { return value > kit_param_max(p) ? kit_param_max(p) : value; }

// Kit settings unlike a fresh track's in every field.
UserSoundExtras some_extras(int seed) {
    UserSoundExtras e;
    e.lfo_trig = uint8_t(1 + seed % 2);
    e.mod[0] = mod_slot_sanitized(kModSrcLfo, 2, uint8_t(int8_t(-40 - seed)));
    e.mod[1] = mod_slot_sanitized(kModSrcVel, 6, uint8_t(30 + seed));
    for (int f = 0; f < kArpExtFields; f++) arp_ext_set(e.arp, f, uint8_t(1 + (f + seed) % 3));
    return e;
}

UserSoundBank bank_with_two(uint8_t machine) {
    UserSoundBank bank;
    user_bank_init(bank, machine);
    uint8_t a[kKitParams], b[kKitParams];
    pattern_bytes(a, 3);
    pattern_bytes(b, 11);
    CHECK(user_sound_store(bank, 0, "MY KICK", a, some_extras(1)));
    CHECK(user_sound_store(bank, 5, "BOOM2", b, some_extras(2)));
    return bank;
}

bool same_bank(const UserSoundBank &x, const UserSoundBank &y) {
    if (x.machine != y.machine) return false;
    for (int s = 0; s < kUserSoundSlots; s++) {
        if (x.slots[s].used != y.slots[s].used) return false;
        if (!x.slots[s].used) continue;
        if (std::strcmp(x.slots[s].name, y.slots[s].name) != 0) return false;
        if (std::memcmp(x.slots[s].params, y.slots[s].params, kKitParams) != 0) return false;
        if (!user_sound_extras_equal(x.slots[s].extras, y.slots[s].extras)) return false;
    }
    return true;
}

// The CRC over the payload again, after a test has changed bytes in it on purpose.
void reseal(uint8_t *file, uint32_t bytes) {
    const uint32_t crc = crc32(file + kHeaderBytes, bytes - kHeaderBytes);
    for (int i = 0; i < 4; i++) file[kCrcOffset + i] = uint8_t(crc >> (8 * i));
}

// A version 1 file built by hand, with `params` bytes a record and `extra` bytes after them, so
// an older and a newer build's records can be made without either build.
uint32_t hand_built(uint8_t machine, int params, int extra, int records, bool unknown_section) {
    const int record = 10 + params + extra;
    const uint32_t section = 6 + uint32_t(records * record);
    const uint32_t sections = unknown_section ? 2 : 1;
    const uint32_t table = sections * kSectionEntryBytes;
    const uint32_t junk = unknown_section ? 5 : 0;
    const uint32_t total = kHeaderBytes + table + junk + section;
    Writer w(g_file, sizeof g_file);
    w.u8('S'); w.u8('T'); w.u8('R'); w.u8('A');
    w.u16(kUserSoundVersion);
    w.u16(uint16_t(sections));
    w.u32(total - kHeaderBytes);
    w.u32(0);
    if (unknown_section) {
        w.u16(77);   // a section a later build added: stepped over
        w.u16(0);
        w.u32(table);
        w.u32(junk);
    }
    w.u16(kSectionUserSounds);
    w.u16(0);
    w.u32(table + junk);
    w.u32(section);
    for (uint32_t i = 0; i < junk; i++) w.u8(0xEE);
    w.u8(machine);
    w.u8(kKitFilterLayout);
    w.u8(kLfoLayoutNumbers);
    w.u8(uint8_t(records));
    w.u8(uint8_t(record));
    w.u8(uint8_t(params));
    for (int r = 0; r < records; r++) {
        w.u8(r == 0 || r == 16 ? 1 : 0);
        const char *name = r == 0 ? "OLD ONE" : "SEVENTEE";
        for (int i = 0; i < kUserSoundNameChars; i++) {
            w.u8(i < int(std::strlen(name)) ? uint8_t(name[i]) : 0);
        }
        w.u8(0);
        for (int p = 0; p < params; p++) w.u8(uint8_t(40 + (p % 50)));
        for (int i = 0; i < extra; i++) w.u8(0xAB);
    }
    CHECK(w.ok());
    CHECK_EQ(w.size(), total);
    reseal(g_file, total);
    return total;
}

}  // namespace

// --- names, slots and the mask -------------------------------------------------------------------

TEST(usersounds_machines_with_a_sounds_list_can_save_their_own_and_the_rest_cannot) {
    for (int m = 0; m < int(MachineId::count); m++) {
        CHECK_EQ(user_sounds_supported(uint8_t(m)), sound_preset_count(uint8_t(m)) > 0);
        char name[16];
        CHECK_EQ(user_bank_file_name(uint8_t(m), name, int(sizeof name)), user_sounds_supported(uint8_t(m)));
    }
    char name[16];
    CHECK(user_bank_file_name(kKick, name, int(sizeof name)));
    CHECK(std::strcmp(name, "KCK.BIN") == 0);
    CHECK(user_bank_file_name(uint8_t(MachineId::fm), name, int(sizeof name)));
    CHECK(std::strcmp(name, "FM4.BIN") == 0);
    CHECK(!user_bank_file_name(kKick, name, 7));
}

TEST(usersounds_a_name_is_what_the_font_draws_and_the_entry_types) {
    CHECK(user_sound_name_valid("A"));
    CHECK(user_sound_name_valid("MY KICK"));
    CHECK(user_sound_name_valid("8BIT-.#!"));
    CHECK(user_sound_name_valid("ACID+303"));
    CHECK(!user_sound_name_valid(""));
    CHECK(!user_sound_name_valid(nullptr));
    CHECK(!user_sound_name_valid("NINE CHAR"));   // nine characters
    CHECK(!user_sound_name_valid(" LEAD"));
    CHECK(!user_sound_name_valid("LEAD "));
    CHECK(!user_sound_name_valid("lower"));
    CHECK(!user_sound_name_valid("A_B"));
    CHECK(!user_sound_name_valid("A*B"));
    CHECK(!user_sound_name_valid("A,B"));
}

TEST(usersounds_the_offered_name_is_free_and_counts_up) {
    UserSoundBank bank;
    user_bank_init(bank, kKick);
    char name[kUserSoundNameChars + 1];
    user_sound_default_name(bank, nullptr, name);
    CHECK(std::strcmp(name, "KCK 1") == 0);
    user_sound_default_name(bank, "909", name);
    CHECK(std::strcmp(name, "909") == 0);
    uint8_t params[kKitParams] = {};
    CHECK(user_sound_store(bank, 3, "KCK 1", params));
    CHECK(user_sound_store(bank, 4, "909", params));
    user_sound_default_name(bank, "909", name);   // taken: the counted name instead
    CHECK(std::strcmp(name, "KCK 2") == 0);
    user_sound_default_name(bank, "bad name", name);
    CHECK(std::strcmp(name, "KCK 2") == 0);
    // All sixteen named KCK 1 .. KCK 16: the seventeenth is offered, always valid.
    for (int s = 0; s < kUserSoundSlots; s++) {
        char each[16];   // room for any int, as -Wformat-truncation wants; "KCK 16" at most
        std::snprintf(each, sizeof each, "KCK %d", s + 1);
        CHECK(user_sound_store(bank, s, each, params));
    }
    CHECK_EQ(user_bank_free_slot(bank), -1);
    user_sound_default_name(bank, nullptr, name);
    CHECK(std::strcmp(name, "KCK 17") == 0);
    CHECK(user_sound_name_valid(name));
}

TEST(usersounds_slots_store_rename_delete_and_count) {
    UserSoundBank bank = bank_with_two(kKick);
    CHECK_EQ(user_bank_count(bank), 2);
    CHECK_EQ(user_bank_nth(bank, 0), 0);
    CHECK_EQ(user_bank_nth(bank, 1), 5);
    CHECK_EQ(user_bank_nth(bank, 2), -1);
    CHECK_EQ(user_bank_nth(bank, -1), -1);
    CHECK_EQ(user_bank_free_slot(bank), 1);
    CHECK_EQ(user_bank_find(bank, "BOOM2"), 5);
    CHECK_EQ(user_bank_find(bank, "BOOM"), -1);
    CHECK_EQ(user_bank_find(bank, nullptr), -1);

    CHECK(user_sound_rename(bank, 5, "BOOM3"));
    CHECK_EQ(user_bank_find(bank, "BOOM3"), 5);
    CHECK(!user_sound_rename(bank, 1, "EMPTY"));           // nothing there
    CHECK(!user_sound_rename(bank, 5, "no good"));
    CHECK(std::strcmp(bank.slots[5].name, "BOOM3") == 0);

    CHECK(user_sound_delete(bank, 0));
    CHECK(!user_sound_delete(bank, 0));
    CHECK(!user_sound_delete(bank, kUserSoundSlots));
    CHECK_EQ(user_bank_count(bank), 1);
    CHECK_EQ(user_bank_free_slot(bank), 0);

    uint8_t params[kKitParams] = {};
    CHECK(!user_sound_store(bank, -1, "X", params));
    CHECK(!user_sound_store(bank, kUserSoundSlots, "X", params));
    CHECK(!user_sound_store(bank, 2, "X", nullptr));
    CHECK(!user_sound_store(bank, 2, "", params));
    CHECK_EQ(user_bank_count(bank), 1);

    // A byte past what its parameter may hold is clamped on the way in (the LFO's WAV and DST).
    uint8_t wild[kKitParams];
    for (int p = 0; p < kKitParams; p++) wild[p] = 255;
    CHECK(user_sound_store(bank, 2, "WILD", wild));
    for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(bank.slots[2].params[p]), int(kit_param_max(p)));
}

TEST(usersounds_loading_writes_every_parameter_and_never_the_hole) {
    for (int m = 0; m < int(MachineId::count); m++) {
        const uint8_t machine = uint8_t(m);
        UserSound sound;
        sound.used = true;
        pattern_bytes(sound.params, 5);
        sound.extras = some_extras(3);
        uint8_t track[kKitParams];
        for (int p = 0; p < kKitParams; p++) track[p] = uint8_t(127 - sound.params[p] / 2);
        uint8_t before[kKitParams];
        std::memcpy(before, track, kKitParams);
        const bool ok = user_sound_apply(machine, sound, track);
        CHECK_EQ(ok, user_sounds_supported(machine));
        for (int p = 0; p < kKitParams; p++) {
            // Roland, 2026-11-28: all of a sound -- FX, LFO and ARP too. Never the hole.
            const bool owns = ok && kit_param_real(p);
            CHECK_EQ(user_sound_owns(machine, p), owns);
            CHECK_EQ(int(track[p]), int(owns ? sound.params[p] : before[p]));
        }
        CHECK_EQ(user_sound_matches(machine, sound, track, sound.extras), ok);
        // The kit settings count for NOW as much as a byte does.
        CHECK(!user_sound_matches(machine, sound, track, UserSoundExtras{}));
        if (ok) {
            track[kUserSoundFxFirst + 1] = uint8_t(track[kUserSoundFxFirst + 1] ^ 1);   // pan
            CHECK(!user_sound_matches(machine, sound, track, sound.extras));
        }
    }
    CHECK(!user_sound_owns(kKick, -1));
    CHECK(!user_sound_owns(kKick, 200));
    CHECK(!user_sound_owns(kKick, kKitOscEnd));   // the hole
    UserSound empty;
    uint8_t track[kKitParams] = {};
    CHECK(!user_sound_apply(kKick, empty, track));
    CHECK(!user_sound_matches(kKick, empty, track, UserSoundExtras{}));
}

TEST(usersounds_the_kit_settings_are_stored_safe) {
    UserSoundBank bank;
    user_bank_init(bank, kKick);
    uint8_t params[kKitParams] = {};
    const UserSoundExtras mine = some_extras(4);
    CHECK(user_sound_store(bank, 0, "MINE", params, mine));
    CHECK(user_sound_extras_equal(bank.slots[0].extras, mine));
    // Damage reads as KIT.BIN's would: FREE, a slot OFF, the arp's 0.
    UserSoundExtras wild;
    wild.lfo_trig = 200;
    wild.mod[0].source = 99;
    wild.mod[0].dest = 3;
    wild.mod[0].depth = 20;
    wild.arp.var = 250;
    CHECK(user_sound_store(bank, 1, "WILD", params, wild));
    CHECK(user_sound_extras_equal(bank.slots[1].extras, UserSoundExtras{}));
    // A track's settings are what user_sound_extras_of takes.
    KitTrack kit;
    kit.lfo_trig = mine.lfo_trig;
    for (int m = 0; m < kModSlots; m++) kit.mod[m] = mine.mod[m];
    kit.arp = mine.arp;
    CHECK(user_sound_extras_equal(user_sound_extras_of(kit), mine));
    CHECK(user_sound_extras_equal(user_sound_extras_of(KitTrack{}), UserSoundExtras{}));
}

// --- the file ------------------------------------------------------------------------------------

TEST(usersounds_a_bank_round_trips_through_its_file) {
    const UserSoundBank bank = bank_with_two(kMono);
    uint32_t written = 0;
    CHECK(user_bank_write(bank, g_file, sizeof g_file, &written) == SerializeResult::ok);
    CHECK_EQ(written, user_bank_bytes());
    UserSoundBank back;
    user_bank_init(back, kKick);
    CHECK(user_bank_read(back, kMono, g_file, written) == SerializeResult::ok);
    CHECK(same_bank(bank, back));
    // Empty slots are written as zeroes, so a file says nothing about a deleted sound.
    UserSoundBank empty;
    user_bank_init(empty, kMono);
    CHECK(user_bank_write(empty, g_file, sizeof g_file, &written) == SerializeResult::ok);
    CHECK(user_bank_read(back, kMono, g_file, written) == SerializeResult::ok);
    CHECK_EQ(user_bank_count(back), 0);
    CHECK(user_bank_write(bank, g_file, user_bank_bytes() - 1, &written) ==
          SerializeResult::buffer_too_small);
    CHECK_EQ(written, 0u);
    CHECK(user_bank_write(bank, nullptr, sizeof g_file) == SerializeResult::buffer_too_small);
}

TEST(usersounds_damage_is_refused_and_leaves_the_bank_alone) {
    const UserSoundBank bank = bank_with_two(kKick);
    uint32_t bytes = 0;
    CHECK(user_bank_write(bank, g_file, sizeof g_file, &bytes) == SerializeResult::ok);
    UserSoundBank held = bank_with_two(kKick);
    CHECK(user_sound_rename(held, 0, "HELD"));
    const UserSoundBank kept = held;

    CHECK(user_bank_read(held, kKick, g_file, bytes - 1) == SerializeResult::truncated);
    CHECK(user_bank_read(held, kKick, g_file, 10) == SerializeResult::truncated);
    CHECK(user_bank_read(held, kKick, nullptr, bytes) == SerializeResult::truncated);
    // The wrong machine's file: a KCK.BIN that says it is for VA MONO.
    CHECK(user_bank_read(held, kMono, g_file, bytes) == SerializeResult::malformed);
    g_file[100] ^= 0x10;
    CHECK(user_bank_read(held, kKick, g_file, bytes) == SerializeResult::bad_crc);
    g_file[100] ^= 0x10;
    g_file[0] = 'X';
    CHECK(user_bank_read(held, kKick, g_file, bytes) == SerializeResult::bad_magic);
    g_file[0] = 'S';
    // A newer container version, and a newer meaning of the FLTR bytes, are a later build's.
    g_file[kVersionOffset] = 2;
    CHECK(user_bank_read(held, kKick, g_file, bytes) == SerializeResult::bad_version);
    g_file[kVersionOffset] = 1;
    const uint32_t bank_at = kHeaderBytes + 2 * kSectionEntryBytes;   // after the two entries
    g_file[bank_at + 1] = uint8_t(kKitFilterLayout + 1);
    reseal(g_file, bytes);
    CHECK(user_bank_read(held, kKick, g_file, bytes) == SerializeResult::bad_version);
    g_file[bank_at + 1] = kKitFilterLayout;
    // A record size too small for its own parameters.
    g_file[bank_at + 4] = 9;
    reseal(g_file, bytes);
    CHECK(user_bank_read(held, kKick, g_file, bytes) == SerializeResult::malformed);
    CHECK(same_bank(held, kept));
}

TEST(usersounds_an_older_record_loads_with_the_machines_defaults_under_what_it_lacks) {
    // A build with 24 parameters: SRC to ARP, no ASR or OSC.
    const uint32_t bytes = hand_built(kMono, 24, 0, 16, false);
    UserSoundBank bank;
    CHECK(user_bank_read(bank, kMono, g_file, bytes) == SerializeResult::ok);
    CHECK_EQ(user_bank_count(bank), 1);
    CHECK(std::strcmp(bank.slots[0].name, "OLD ONE") == 0);
    uint8_t defaults[kKitParams] = {};
    CHECK(machine_defaults_apply(kMono, defaults));
    for (int p = 0; p < kKitParams; p++) {
        const int stored = clamped(p, 40 + (p % 50));
        CHECK_EQ(int(bank.slots[0].params[p]), (p < 24 ? stored : int(defaults[p])));
    }
    // A file from before the kit settings section: a fresh track's.
    CHECK(user_sound_extras_equal(bank.slots[0].extras, UserSoundExtras{}));
}

TEST(usersounds_a_shorter_or_longer_kit_settings_record_reads_what_it_has) {
    UserSoundBank bank = bank_with_two(kKick);
    uint32_t bytes = 0;
    CHECK(user_bank_write(bank, g_file, sizeof g_file, &bytes) == SerializeResult::ok);
    // The extras section is the file's last: S, E, then sixteen records of E bytes.
    const uint32_t ext_at = bytes - (2 + uint32_t(kUserSoundSlots * kUserSoundExtraBytes));
    CHECK_EQ(int(g_file[ext_at]), kUserSoundSlots);
    CHECK_EQ(int(g_file[ext_at + 1]), kUserSoundExtraBytes);
    // Read as if each record were one byte long (TRIG only): the rest is a fresh track's. Only
    // the first sixteen bytes after S and E are records then, so slot 0's TRIG is the first.
    g_file[ext_at + 1] = 1;
    reseal(g_file, bytes);
    UserSoundBank back;
    CHECK(user_bank_read(back, kKick, g_file, bytes) == SerializeResult::ok);
    UserSoundExtras want;
    want.lfo_trig = some_extras(1).lfo_trig;
    CHECK(user_sound_extras_equal(back.slots[0].extras, want));
    // A section that claims more than it holds is damage.
    g_file[ext_at + 1] = 200;
    reseal(g_file, bytes);
    CHECK(user_bank_read(back, kKick, g_file, bytes) == SerializeResult::malformed);
}

TEST(usersounds_a_newer_record_and_section_are_read_for_what_this_build_knows) {
    // A build with 40 parameters, two bytes more a record after them, seventeen records, and a
    // section this build has never heard of in front of the bank.
    const uint32_t bytes = hand_built(kKick, kKitParams + 8, 2, 17, true);
    UserSoundBank bank;
    CHECK(user_bank_read(bank, kKick, g_file, bytes) == SerializeResult::ok);
    CHECK_EQ(user_bank_count(bank), 1);           // the seventeenth record is past slot 16
    CHECK(std::strcmp(bank.slots[0].name, "OLD ONE") == 0);
    for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(bank.slots[0].params[p]), clamped(p, 40 + (p % 50)));
}

TEST(usersounds_a_damaged_name_keeps_the_sound) {
    const uint32_t bytes = hand_built(kKick, kKitParams, 0, 1, false);
    const uint32_t name_at = kHeaderBytes + kSectionEntryBytes + 6 + 1;
    g_file[name_at] = ' ';            // " LD ONE": leading space
    g_file[name_at + 1] = 'l';        // lower case: not in the font
    reseal(g_file, bytes);
    UserSoundBank bank;
    CHECK(user_bank_read(bank, kKick, g_file, bytes) == SerializeResult::ok);
    CHECK(std::strcmp(bank.slots[0].name, "-D ONE") == 0);
    CHECK(user_sound_name_valid(bank.slots[0].name));
}

// --- Cmd::sound_bytes ----------------------------------------------------------------------------

namespace {

// As app/ui_browsers.hpp posts them: eight a command, the last one short (kKitParams is 44).
constexpr int kSoundCommands = (kKitParams + 7) / 8;

void post_sound(int track, uint8_t machine, const uint8_t *params) {
    for (int first = 0; first < kKitParams; first += 8) {
        uint8_t b[8] = {};
        for (int i = 0; i < 8 && first + i < kKitParams; i++) b[i] = params[first + i];
        Command command;
        command.cmd = Cmd::sound_bytes;
        command.track = uint8_t(track);
        command.step = uint8_t(first);
        command.a = machine;
        command.b = b[0];
        command.c = b[1];
        command.d = b[2];
        command.e = b[3];
        command.u = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
        test::engine().commands().post(command);
    }
}

// And its kit settings after the bytes, by the commands their pages post.
void post_extras(int track, const UserSoundExtras &x) {
    test::post(Cmd::set_lfo_trig, uint8_t(track), 0, x.lfo_trig);
    for (int m = 0; m < kModSlots; m++) {
        Command c;
        c.cmd = Cmd::set_mod_slot;
        c.track = uint8_t(track);
        c.a = uint8_t(m);
        c.b = x.mod[m].source;
        c.c = x.mod[m].dest;
        c.d = uint8_t(x.mod[m].depth);
        test::engine().commands().post(c);
    }
    for (int f = 0; f < kArpExtFields; f++) {
        test::post(Cmd::set_arp_ext, uint8_t(track), 0, uint8_t(f), arp_ext_get(x.arp, f));
    }
}

}  // namespace

TEST(usersounds_the_command_writes_a_sounds_bytes_into_one_track_and_no_others) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    TrackParams before[kTracks];
    for (int t = 0; t < kTracks; t++) before[t] = e.params(t);
    const uint8_t machine = e.machine_id(0);
    CHECK(user_sounds_supported(machine));
    uint8_t sound[kKitParams];
    pattern_bytes(sound, 9);
    post_sound(0, machine, sound);
    pump(2);
    for (int p = 0; p < kTrackParams; p++) {
        const int want = user_sound_owns(machine, p) ? sound[p] : before[0].v[p];
        CHECK_EQ(int(e.params(0).v[p]), want);
        CHECK_EQ(int(e.performed(0, p)), int(e.params(0).v[p]));
    }
    for (int t = 1; t < kTracks; t++) CHECK(std::memcmp(e.params(t).v, before[t].v, kTrackParams) == 0);

    // Another machine's bytes are refused: one refusal a command, nothing written.
    const TrackParams now = e.params(0);
    const uint32_t refused = e.status().refused.load(std::memory_order_relaxed);
    post_sound(0, uint8_t(machine == kMono ? kKick : kMono), sound);
    pump(2);
    CHECK(std::memcmp(e.params(0).v, now.v, kTrackParams) == 0);
    CHECK_EQ(int(e.status().refused.load(std::memory_order_relaxed) - refused), kSoundCommands);
}

TEST(usersounds_as_was_takes_back_a_paste_that_switched_the_machine) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    // Track 3 as the user left it, with something of their own on the mix and the LFO.
    post(Cmd::set_param, 2, 0, 1, 33);
    post(Cmd::set_param, 2, 0, uint8_t(kUserSoundFxFirst + 1), 21);
    pump(2);
    const uint8_t was = e.machine_id(2);
    const TrackParams mine = e.params(2);
    post(Cmd::sound_preset, 2, 0, 0, kSoundPresetKeep);

    // A VA MONO sound pasted in: the switch, then the bytes, as SOUNDS posts them.
    uint8_t sound[kKitParams];
    pattern_bytes(sound, 4);
    post(Cmd::set_machine, 2, 0, kMono);
    post_sound(2, kMono, sound);
    pump(2);
    CHECK_EQ(int(e.machine_id(2)), int(kMono));
    for (int p = 0; p < kTrackParams; p++) {
        if (user_sound_owns(kMono, p)) CHECK_EQ(int(e.params(2).v[p]), int(sound[p]));
    }
    CHECK_EQ(int(e.params(2).v[kUserSoundFxFirst + 1]), int(sound[kUserSoundFxFirst + 1]));

    // AS WAS: the machine and every byte as they were.
    post(Cmd::sound_preset, 2, 0, kMono, kSoundPresetBack);
    pump(2);
    CHECK_EQ(int(e.machine_id(2)), int(was));
    CHECK_EQ(int(e.project().kit.tracks[2].machine), int(was));
    for (int p = 0; p < kTrackParams; p++) CHECK_EQ(int(e.params(2).v[p]), int(mine.v[p]));
}

TEST(usersounds_as_was_takes_back_all_of_a_user_sound_and_its_kit_settings) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    const uint8_t machine = e.machine_id(4);
    CHECK(user_sounds_supported(machine));
    post(Cmd::set_param, 4, 0, uint8_t(kUserSoundFxFirst + 1), 21);
    pump(2);
    const TrackParams mine = e.params(4);
    const UserSoundExtras mine_extras = user_sound_extras_of(e.project().kit.tracks[4]);
    post(Cmd::sound_preset, 4, 0, 0, kSoundPresetKeep);

    // The sound in: every parameter, the FX page, LFO and ARP among them, and the kit settings.
    uint8_t sound[kKitParams];
    pattern_bytes(sound, 6);
    const UserSoundExtras extras = some_extras(5);
    post_sound(4, machine, sound);
    post_extras(4, extras);
    pump(2);
    for (int p = 0; p < kTrackParams; p++) {
        if (kit_param_real(p)) CHECK_EQ(int(e.params(4).v[p]), clamped(p, sound[p]));
    }
    CHECK(user_sound_extras_equal(user_sound_extras_of(e.project().kit.tracks[4]), extras));

    // A pan move after it, then AS WAS: everything as it was when the list opened.
    post(Cmd::set_param, 4, 0, uint8_t(kUserSoundFxFirst + 1), 90);
    post(Cmd::sound_preset, 4, 0, machine, kSoundPresetBack);
    pump(2);
    for (int p = 0; p < kTrackParams; p++) CHECK_EQ(int(e.params(4).v[p]), int(mine.v[p]));
    CHECK(user_sound_extras_equal(user_sound_extras_of(e.project().kit.tracks[4]), mine_extras));
}

// --- the bank on the card ------------------------------------------------------------------------

namespace {

void bank_path(uint8_t machine, char *out, int max) {
    char file[16];
    CHECK(user_bank_file_name(machine, file, int(sizeof file)));
    std::snprintf(out, size_t(max), "%s/SOUNDS/%s", enaga::files::data_folder(), file);
}

// Ask for the bank and run the card until it is in (or failed).
Storage::SoundBankState read_bank(uint8_t machine) {
    Storage &s = test::storage();
    s.want_sound_bank(machine);
    for (int i = 0; i < 20 && s.sound_bank_state(machine) == Storage::SoundBankState::reading; i++) {
        test::pump(1);
    }
    return s.sound_bank_state(machine);
}

}  // namespace

TEST(usersounds_a_save_makes_the_file_and_a_card_swap_reads_it_back) {
    using namespace strata::test;
    fresh();
    Storage &s = storage();
    char path[128];
    bank_path(kKick, path, int(sizeof path));
    CHECK(!FakeHost::the().has_file(path));
    // Nothing saved yet: an empty bank, and no file made by reading.
    CHECK(read_bank(kKick) == Storage::SoundBankState::ready);
    CHECK_EQ(user_bank_count(s.sound_bank()), 0);
    CHECK(!FakeHost::the().has_file(path));

    uint8_t params[kKitParams];
    pattern_bytes(params, 1);
    // Not for a bank that is not held, nor a name the file cannot hold.
    CHECK(!s.request_sound_store(kMono, 0, "LEAD", params));
    CHECK(!s.request_sound_store(kKick, 0, "bad", params));
    CHECK(s.request_sound_store(kKick, 0, "MY KICK", params));
    CHECK(s.sound_write_pending());
    pump(2);
    CHECK(!s.sound_write_pending());
    CHECK(s.sound_bank_error() == nullptr);
    CHECK(FakeHost::the().has_file(path));
    CHECK_EQ(FakeHost::the().file_bytes(path), int32_t(user_bank_bytes()));

    CHECK(s.request_sound_rename(kKick, 0, "RENAMED"));
    pump(2);
    s.on_card(false);
    s.on_card(true);
    CHECK(s.sound_bank_state(kKick) == Storage::SoundBankState::none);
    CHECK(read_bank(kKick) == Storage::SoundBankState::ready);
    CHECK_EQ(user_bank_count(s.sound_bank()), 1);
    CHECK(std::strcmp(s.sound_bank().slots[0].name, "RENAMED") == 0);
    CHECK(std::memcmp(s.sound_bank().slots[0].params, params, kKitParams) == 0);

    // Another machine's bank replaces it as the one held.
    CHECK(read_bank(kMono) == Storage::SoundBankState::ready);
    CHECK(s.sound_bank_state(kKick) == Storage::SoundBankState::none);
    CHECK_EQ(user_bank_count(s.sound_bank()), 0);

    CHECK(read_bank(kKick) == Storage::SoundBankState::ready);
    CHECK(s.request_sound_delete(kKick, 0));
    CHECK(!s.request_sound_delete(kKick, 0));
    pump(2);
    s.on_card(false);
    s.on_card(true);
    CHECK(read_bank(kKick) == Storage::SoundBankState::ready);
    CHECK_EQ(user_bank_count(s.sound_bank()), 0);
}

TEST(usersounds_a_file_that_cannot_be_read_is_never_written_over) {
    using namespace strata::test;
    fresh();
    Storage &s = storage();
    char path[128];
    bank_path(kKick, path, int(sizeof path));
    const uint8_t junk[40] = {'S', 'T', 'R', 'A', 9, 0};
    CHECK(FakeHost::the().write_file(path, junk, sizeof junk));
    CHECK(read_bank(kKick) == Storage::SoundBankState::failed);
    CHECK(s.sound_bank_error() != nullptr);
    uint8_t params[kKitParams] = {};
    CHECK(!s.request_sound_store(kKick, 0, "MINE", params));
    // Asked again it stays failed, without reading the card on every frame.
    const int reads = FakeHost::the().read_alls();
    s.want_sound_bank(kKick);
    pump(4);
    CHECK_EQ(FakeHost::the().read_alls(), reads);
    uint8_t back[40] = {};
    CHECK_EQ(FakeHost::the().read_file(path, back, sizeof back), int32_t(sizeof junk));
    CHECK(std::memcmp(back, junk, sizeof junk) == 0);

    // With no card nothing is asked at all.
    s.on_card(false);
    s.want_sound_bank(kKick);
    CHECK(s.sound_bank_state(kKick) == Storage::SoundBankState::none);
    s.on_card(true);
    // SAMPLE has no sounds of its own: nothing is read for it.
    s.want_sound_bank(uint8_t(MachineId::sample));
    CHECK(s.sound_bank_state(uint8_t(MachineId::sample)) == Storage::SoundBankState::none);
}
