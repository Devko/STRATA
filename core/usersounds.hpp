// Your own sounds: the SOUNDS list's user entries, saved to the card beside the factory sounds.
//
// A factory sound (core/sounds.hpp) is a table in the build. A user sound is the same idea
// stored on the card: one track's sound pages, given a name, kept per machine and shared by every
// project. SOUNDS lists them after the factory sounds of the selected track's machine; OK loads
// one into the track exactly as it loads a factory sound, with AS WAS as the way back.
//
// ## What a user sound holds and what loading it writes
//
// Everything that makes the track sound and move the way it did (Roland, 2026-11-28: "save
// should have all parameter of a sound"): every parameter of the track -- SRC, FLTR, AMP, the FX
// page, LFO, ARP, ASR, OSC and SCN, HAT's CHK and GRAIN's POS and PIT among them -- and
// the kit settings that belong to those pages: LFO TRIG, the two mod slots and the arp's VAR,
// RHYTHM, ACCENT and RATCHET (UserSoundExtras). Loading writes all of it. That is more than a
// factory sound writes (core/sounds.hpp leaves FX, LFO and ARP alone): a factory sound is a
// starting point for any track, a user sound is a track the user finished.
//
// What stays the track's own is the mixer's: the fader, the chorus send, WID, the duck depth and
// the choke group a track cuts with (KitTrack). They say how the track sits beside the others,
// and they are set on MIXER, not on a sound page. The sample a SAMPLE or GRAIN track plays is the
// track's too (SAMPLE has no user sounds; GRAIN's sound plays whichever sample the track holds).
//
// "Compatible" is the same machine: a KCK sound is listed on KCK tracks only. Slots 0-31 mean
// different things to each machine (core/project.hpp), so a sound read by another machine would
// be noise. Copying a track's sound to a track with another machine (app/ui_browsers.hpp, PASTE)
// switches that track's machine first, which is what makes the bytes mean the same there.
//
// ## The file: <data folder>/SOUNDS/<MCH>.BIN, one per machine
//
// KCK.BIN, SNR.BIN, ... FM4.BIN, named by machine_name. A STRATA container (core/serialize.hpp:
// magic, version, section table, CRC-32 over the payload), version 1, two sections:
//
//     bank (id 1)
//         0   1  machine id (MachineId); must be the machine the file is named for
//         1   1  filter layout (kKitFilterLayout) the FLTR bytes were written in
//         2   1  LFO layout (kLfoLayoutNumbers) the LFO bytes were written in
//         3   1  S: records that follow (kUserSoundSlots when written)
//         4   1  R: bytes per record (1 + kUserSoundNameChars + 1 + P when written)
//         5   1  P: parameter bytes per record (kKitParams when written)
//         6   S x R records, one per slot, empty ones included:
//             0   1  1 = the slot holds a sound, 0 = empty
//             1   8  the name, upper case, NUL padded (kUserSoundNameChars)
//             9   1  reserved, 0
//             10  P  the track's parameter bytes, kit order (KitTrack::params)
//
//     extras (id 2, optional: a file from before it loads every sound with the defaults below)
//         0   1  S: records that follow (kUserSoundSlots when written)
//         1   1  E: bytes per record (kUserSoundExtraBytes when written)
//         2   S x E records, one per slot, in slot order, empty ones included:
//             0   1  LFO TRIG (KitTrack::lfo_trig)
//             1   3  mod slot 1: source, destination, depth (two's complement)
//             4   3  mod slot 2
//             7   4  the arp's VAR, RHYTHM, ACCENT, RATCHET
//         A missing byte (a smaller E, or no section) reads as a fresh track's: FREE, no mod
//         slot, the arp's settings off. A damaged one is made safe as KIT.BIN's would be.
//
// R and P are in the file so the record can GROW: a later build that saves more (a longer name
// is not one of them; a 36th parameter is) writes a larger R or P, and this reader takes the
// first min(P, kKitParams) bytes and steps over the rest by R. A file with fewer than kKitParams
// bytes fills the missing ones from the factory default of the machine, so an older sound still
// loads and sounds as it did. More than kUserSoundSlots records: the first sixteen are read.
// New data goes in a new optional section, as every STRATA file does (CLAUDE.md, "File format").
//
// Nothing here touches a project's files: user sounds live beside PROJECTS/, so every project,
// old or new, opens exactly as before.
#pragma once

#include "project.hpp"
#include "serialize.hpp"

#include <cstdint>

namespace strata {

constexpr int kUserSoundSlots = 16;
// Eight characters: two more than a factory name, and room for "NOW" after it on a list row.
constexpr int kUserSoundNameChars = 8;
constexpr uint16_t kUserSoundVersion = 1;
constexpr uint16_t kSectionUserSounds = 1;

constexpr uint16_t kSectionUserSoundExtras = 2;

// The kit settings a sound carries beside its parameters (see above). A fresh one is a fresh
// track's: KitTrack's own defaults.
struct UserSoundExtras {
    uint8_t lfo_trig = kLfoTrigFree;
    KitModSlot mod[kModSlots];
    KitArp arp;
};
constexpr int kUserSoundExtraBytes = 1 + 3 * kModSlots + kArpExtFields;

// A track's extras, and whether two sets are the same.
UserSoundExtras user_sound_extras_of(const KitTrack &track);
bool user_sound_extras_equal(const UserSoundExtras &a, const UserSoundExtras &b);

struct UserSound {
    bool used = false;
    char name[kUserSoundNameChars + 1] = {};
    uint8_t params[kKitParams] = {};
    UserSoundExtras extras;
};

struct UserSoundBank {
    uint8_t machine = 0;
    UserSound slots[kUserSoundSlots];
};

// The FX page's first parameter (lo-fi, pan, sends): bytes 0 .. 11 before it are SRC, FLTR and AMP.
constexpr int kUserSoundFxFirst = 12;

// Does a sound own parameter `p` on `machine` -- would loading one write it? Every real
// parameter (kit_param_real) of a machine that has sounds; never the hole.
bool user_sound_owns(uint8_t machine, int p);

// Can `machine` have user sounds? Those with a SOUNDS list: not SAMPLE (its sound is its file)
// or MID (its knobs are CCs for another instrument).
bool user_sounds_supported(uint8_t machine);

// An empty bank for `machine`.
void user_bank_init(UserSoundBank &bank, uint8_t machine);

// How many slots hold a sound; the slot of the `n`th that does (0-based), or -1; the first empty
// slot, or -1 when all sixteen are taken.
int user_bank_count(const UserSoundBank &bank);
int user_bank_nth(const UserSoundBank &bank, int n);
int user_bank_free_slot(const UserSoundBank &bank);

// The slot holding a sound called `name` (exactly), or -1.
int user_bank_find(const UserSoundBank &bank, const char *name);

// A name the 5x7 font draws and a file can hold: 1-8 characters of A-Z, 0-9, space - + . # !
// (Enaga's TextEntry alphabet), not starting or ending in a space.
bool user_sound_name_valid(const char *name);

// The name SAVE offers: `base` if it is valid and free (a factory sound's name, say), else the
// machine's name and the lowest number free, "KCK 1", "KCK 2", ... Always valid; at most
// kUserSoundNameChars characters and a NUL, so `out` needs kUserSoundNameChars + 1 bytes.
void user_sound_default_name(const UserSoundBank &bank, const char *base, char *out);

// Store a sound into `slot`: the name, the kKitParams bytes and the extras (each made safe, as a
// file's would be). False, nothing changed, for a slot outside 0..15 or a name that is not valid.
bool user_sound_store(UserSoundBank &bank, int slot, const char *name, const uint8_t *params,
                      const UserSoundExtras &extras = UserSoundExtras{});
bool user_sound_rename(UserSoundBank &bank, int slot, const char *name);
bool user_sound_delete(UserSoundBank &bank, int slot);

// Write a sound's bytes into `params` (kKitParams bytes): those user_sound_owns says a sound
// owns. False, nothing written, for an empty sound or a machine with no sounds. The extras go to
// the kit by their own commands (app/ui_browsers.hpp).
bool user_sound_apply(uint8_t machine, const UserSound &sound, uint8_t *params);

// True when every byte and kit setting a load would write already has the sound's value: the
// list's NOW mark.
bool user_sound_matches(uint8_t machine, const UserSound &sound, const uint8_t *params,
                        const UserSoundExtras &extras);

// "KCK.BIN" for KICK. False for a machine with no sounds.
bool user_bank_file_name(uint8_t machine, char *out, int max);

// The file. A bank always writes the same number of bytes, user_bank_bytes().
uint32_t user_bank_bytes();
SerializeResult user_bank_write(const UserSoundBank &bank, uint8_t *buffer, uint32_t capacity,
                                uint32_t *written = nullptr);

// Reads into `bank` only when the whole file checks out; on any failure `bank` is left alone.
// `machine` is the machine the file was opened for: a file that says otherwise is malformed.
SerializeResult user_bank_read(UserSoundBank &bank, uint8_t machine, const uint8_t *buffer,
                               uint32_t size);

}  // namespace strata
