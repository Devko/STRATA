// Your own sounds: the SOUNDS list's user entries, saved to the card beside the factory sounds.
//
// A factory sound (core/sounds.hpp) is a table in the build. A user sound is the same idea
// stored on the card: one track's sound pages, given a name, kept per machine and shared by every
// project. SOUNDS lists them after the factory sounds of the selected track's machine; OK loads
// one into the track exactly as it loads a factory sound, with AS WAS as the way back.
//
// ## What a user sound holds and what loading it writes
//
// The file keeps all kKitParams bytes of the track as it was saved, so a later build can choose
// to carry more of it. Loading writes the bytes a factory sound writes and no others
// (sound_preset_writes): SRC, FLTR, AMP and, on the machines that read them, ASR and OSC. The
// FX page, LFO and ARP stay the track's own for the reason core/sounds.hpp gives: they are where
// a track sits in the mix and how it is played, not what it sounds like. HAT's choke group and
// GRAIN's POS and PIT stay too.
//
// "Compatible" is the same machine: a KCK sound is listed on KCK tracks only. Slots 0-31 mean
// different things to each machine (core/project.hpp), so a sound read by another machine would
// be noise. Copying a track's sound to a track with another machine (app/ui_browsers.hpp, PASTE)
// switches that track's machine first, which is what makes the bytes mean the same there.
//
// ## The file: <data folder>/SOUNDS/<MCH>.BIN, one per machine
//
// KCK.BIN, SNR.BIN, ... FM4.BIN, named by machine_name. A STRATA container (core/serialize.hpp:
// magic, version, section table, CRC-32 over the payload), version 1, one section:
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

struct UserSound {
    bool used = false;
    char name[kUserSoundNameChars + 1] = {};
    uint8_t params[kKitParams] = {};
};

struct UserSoundBank {
    uint8_t machine = 0;
    UserSound slots[kUserSoundSlots];
};

// The FX page's first parameter (lo-fi, pan, sends): bytes 0 .. 11 before it are SRC, FLTR and AMP.
constexpr int kUserSoundFxFirst = 12;

// Does a sound own parameter `p` on `machine` -- would loading one write it? sound_preset_writes's
// mask, read a bit at a time so the mask may widen with the parameters.
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

// Store a sound into `slot`: the name and the kKitParams bytes. False, nothing changed, for a
// slot outside 0..15 or a name that is not valid.
bool user_sound_store(UserSoundBank &bank, int slot, const char *name, const uint8_t *params);
bool user_sound_rename(UserSoundBank &bank, int slot, const char *name);
bool user_sound_delete(UserSoundBank &bank, int slot);

// Write a sound's bytes into `params` (kKitParams bytes): only those sound_preset_writes(machine)
// says a sound owns. False, nothing written, for an empty sound or a machine with no sounds.
bool user_sound_apply(uint8_t machine, const UserSound &sound, uint8_t *params);

// True when every byte a load would write already has the sound's value: the list's NOW mark.
bool user_sound_matches(uint8_t machine, const UserSound &sound, const uint8_t *params);

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
