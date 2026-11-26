// The three project files: PROJECT.BIN, KIT.BIN and BANK_A.BIN .. BANK_H.BIN.
//
// core/serialize.cpp owns the container -- magic, version, section table, CRC-32 over the
// payload -- and the Pattern round trip. This file builds three more containers out of the same
// primitives (`Writer`, `Reader`, `crc32` and the kHeader/kSection constants) instead of
// sharing a helper with serialize.cpp. That costs the fifty lines of `write_container` and
// `find_sections` below; what it buys is that the pattern format and the project format can be
// read and changed without reading each other. The rules those two helpers enforce are
// serialize.cpp's, restated and -- this part matters -- checked in the same ORDER, so that all
// four file types give the same reason for the same damage.
//
// ## The bytes
//
// PROJECT.BIN, 393 bytes, six sections (the third, the mixer's, the fourth, the live state's,
// the fifth, song mode's, and the sixth, MIDI OUT's, were added later and are OPTIONAL on read).
// Fixed size: nothing in it is variable length.
//
//     settings (id 1), 19 bytes
//         0   2  tempo, centi-BPM
//         2   1  root note
//         3   1  scale
//         4   1  reserved: was a project swing nothing read. Written as 50, ignored on read
//         5   1  live-record quantize, 0..100
//         6   8  the 8 stored mute states, one bit per track
//        14   1  sample budget, MiB
//        15   1  chain entries in use
//        16   1  chain enabled, 0 or 1
//        17   1  the live pattern's bank
//        18   1  the live pattern's slot
//     chain (id 2), 192 bytes
//         64 entries of (bank, slot, repeats); repeats 0 means unused
//     mixer (id 3), the send and master bytes
//     live (id 4), 3 bytes
//         0   1  the live mutes, one bit per track
//         1   1  the scene in morph slot A
//         2   1  the scene in morph slot B
//     song (id 5), 65 bytes
//         0   1  SONG: 1 plays the chain once and stops, 0 loops it
//         1  64  each chain entry's mute recall: 0 none, 1-8 that stored mute state
//     midi (id 6), 8 bytes: each track's MIDI OUT channel, 0 off or 1-16
//
// KIT.BIN, twelve sections: the tracks (1,076 bytes alone), then the optional LFO, arp, SLC AUTO,
// ASR, duck, OSC, filter-layout, LFO TRIG, mod-slot, choke and arp-setting sections, each of
// which an older file may lack.
//
//     tracks (id 1), 8 x 131 bytes
//         0  16  the 16 parameters, 0..127
//        16   1  machine
//        17   2  mixer fader, thousandths of unity
//        19 112  sample path, NUL-terminated
//     lfo (id 2), 8 x 4 bytes: parameters 16-19
//     arp (id 3), 8 x 4 bytes: the arp's parameters
//     slices (id 4), 8 x 1 byte: KitTrack::slice_mode, 0 equal, 1 AUTO
//     asr (id 5), 8 x 4 bytes: the ASR page's parameters
//     duck (id 6), 12 bytes
//         0   8  each track's duck depth, 0-100 percent
//         8   1  the source track, 0-7
//         9   1  attack, ms
//        10   1  hold, x 5 ms
//        11   1  release, x 5 ms
//     osc (id 7), 8 x 4 bytes: the OSC page's parameters (absent: kKitOscLegacy)
//     filter (id 8), 1 byte: kKitFilterLayout (absent: the VA and WAVE tracks are mapped, D8)
//     lfo trig (id 9), 8 x 1 byte: KitTrack::lfo_trig, 0 FREE, 1 RETRIG, 2 ONE-SHOT
//     mod (id 10), 8 x 2 x 3 bytes: each track's two slots (core/modslot.hpp)
//         0   1  source, 0 OFF .. 5 RND
//         1   1  destination, a track parameter index
//         2   1  depth, -127..127 as a two's-complement byte
//     choke (id 11), 8 x 1 byte: KitTrack::choke, 0 none or a group 1-4
//     arp ext (id 12), 8 x 4 bytes: KitTrack::arp, VAR RHYTHM ACCENT RATCHET (core/arp.hpp)
//
// BANK_x.BIN, one section holding 16 patterns, each one `pattern_write`'s own container back to
// back. The patterns are the section's bytes, so the bank's CRC covers them as well as its own
// table -- see "one damaged pattern" below for what that does and does not buy.
//
// ## Loading is value-tolerant but structure-strict
//
// docs/SPEC.md's rule, and the pattern serializer's. A tempo of 0, a sample budget of 255 or a
// chain entry naming bank 9 are forced into the range the rest of STRATA is allowed to assume;
// a bad magic, an unknown version, a bad CRC or a section table that does not add up is
// refused. A refused load leaves what this file owns at its defaults, never half read.
//
// Two of the clamps are worth saying out loud because they are the reason this is not just
// paranoia: a kit parameter reaches dsp/ as a table index or a x(1/127) scale, and a fader
// reaches the mixer as a gain. A 255 from a damaged card is an out-of-range read in dsp/ and an
// infinity is a blown output, so neither can be allowed out of this file.
//
// ## One damaged pattern costs one pattern
//
// `bank_read` finds each pattern's extent in that pattern's OWN container header and hands
// `pattern_read` exactly those bytes, so a pattern this build refuses -- a newer pattern
// version, a lock pool that is not compacted, a CRC that disagrees -- costs that slot and
// nothing else. The 15 others load and `refused` says 1.
//
// What that does NOT cover is a bank whose own CRC fails: the bank's CRC is over its whole
// payload, patterns included, as serialize.hpp's container says it must be, so a flipped bit
// anywhere in the file refuses all 16 slots. That is the honest answer for a file whose
// structure cannot be trusted, and it is also why the per-pattern CRCs still earn their keep
// only against pattern-level format damage rather than against card rot. Narrowing card rot to
// one slot would mean a bank CRC that covered the table alone -- a container that does not match
// the one serialize.hpp documents -- so it is a format decision for the owner of that file and
// not something to slip in here.
#include "project.hpp"

#include "clamp.hpp"

namespace strata {
const char *machine_name(uint8_t id) {
    // Upper case and three characters, because that is what the device font draws and what every
    // other name in this project is (docs/ENAGA_API.md). Indexed by MachineId, so the order here
    // is the order on disk.
    static const char *const names[] = {"KCK", "SNR", "HAT", "PRC", "SMP", "GRN",
                                        "MNO", "PLY", "MID", "WMN", "WPL", "FM4"};
    static_assert(int(sizeof(names) / sizeof(names[0])) == int(MachineId::count),
                  "a machine with no name draws as the next machine's");
    return id < uint8_t(MachineId::count) ? names[id] : "---";
}

namespace {

// --- clamps ---------------------------------------------------------------

// docs/SPEC.md's figures, which core/samplepool.hpp states again as kSampleBudgetMaxMib. Stated
// here rather than included from there so that this module builds on its own; app/ is where the
// two are tied together, as it is for kKitParams.
//
// The minimum is 1 and not 0 because a budget of zero cannot hold a single sample, and a
// project that silently refuses every sample load is the least diagnosable failure there is.
// No UI offers 0, so a 0 in a file is a damaged byte.
constexpr uint8_t kBudgetMibMin = 1;
constexpr uint8_t kBudgetMibMax = 64;

// A kit parameter's domain (dsp/track.hpp: "Values are 0-127").
constexpr uint8_t kParamMax = 127;

// --- the mixer fader on disk ----------------------------------------------

// Thousandths of unity in a uint16_t, not the float's 32 bits. Same reasoning as project.hpp's
// "stored in MiB rather than bytes": a format that cannot express a NaN or an infinity cannot
// hand one to the mixer. A thousandth is 0.009 dB at unity, far below the UI's own step.
//
// The uint16_t has room to 65.535, so raising the fader's ceiling later moves this clamp and not
// the format.
constexpr uint16_t kLevelScale = 1000;
// The engine's ceiling (app/engine.cpp clamps `set_track_level` to 2.0; the mixer page reaches
// 1.5). This was 1.0, so a fader above unity came back at unity after a save and a reload.
constexpr uint16_t kLevelMilliMax = 2 * kLevelScale;
constexpr float kLevelMax = 2.0f;

uint16_t level_to_milli(float level) {
    // The value is read, not trusted, even on the way out: `level` comes from RAM, where an
    // uninitialised kit or a bad write could leave a NaN, and converting a NaN to an integer is
    // undefined. Comparison order is what catches it -- every comparison against a NaN is
    // false, so the `>= 0` test is the one that rejects it.
    if (!(level >= 0.0f)) return 0;
    if (level >= kLevelMax) return kLevelMilliMax;
    return uint16_t(level * float(kLevelScale) + 0.5f);
}

float level_from_milli(uint16_t milli) {
    // A division and not a multiply by 1/1000: the correctly rounded quotient gives back exactly
    // the float `level_to_milli` saw, where the reciprocal lands an ULP away. This runs on a
    // load, so CLAUDE.md's "no float division in hot loops" does not bite.
    const uint16_t clamped = milli > kLevelMilliMax ? kLevelMilliMax : milli;
    return float(clamped) / float(kLevelScale);
}

// --- sample paths ---------------------------------------------------------

void write_path(Writer &w, const char *path) {
    int length = 0;
    while (length < kSamplePathBytes && path[length] != '\0') length++;
    // A path filling all 112 bytes has no terminator to write, so the last character goes rather
    // than the terminator. app/ joins this to the project folder and hands it to enaga::files,
    // which reads until a NUL: a name one character short fails loudly, an unterminated one
    // reads whatever sits after it.
    if (length > kSamplePathBytes - 1) length = kSamplePathBytes - 1;
    for (int i = 0; i < length; i++) w.u8(uint8_t(path[i]));
    // Zeros, not the bytes past the terminator. A track that used to play
    // /SAMPLES/LONG NAME.WAV and now plays /SAMPLES/K.WAV would otherwise leave the tail of the
    // old name on the card, so the file would record the editing history rather than the kit.
    w.zeros(uint32_t(kSamplePathBytes - length));
}

void read_path(Reader &r, char *path) {
    bool ended = false;
    for (int i = 0; i < kSamplePathBytes; i++) {
        const uint8_t byte = r.u8();
        ended = ended || byte == 0;
        path[i] = ended ? '\0' : char(byte);
    }
    // 112 bytes with no terminator anywhere: the last character pays, because every reader of
    // `sample` stops at a NUL and there would otherwise be none.
    path[kSamplePathBytes - 1] = '\0';
}

// --- the container --------------------------------------------------------

struct Section {
    uint16_t id;
    uint32_t offset;   // from the start of the payload, so never inside the table
    uint32_t size;
};

// The 16-byte header and the section table, per serialize.hpp's layout. The CRC is left at zero
// for `finish_container` to patch once the sections are down and their bytes are known.
void write_container(Writer &w, uint16_t version, const Section *sections, uint32_t count,
                     uint32_t payload) {
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kMagic2);
    w.u8(kMagic3);
    w.u16(version);
    w.u16(uint16_t(count));
    w.u32(payload);
    w.u32(0);
    for (uint32_t i = 0; i < count; i++) {
        w.u16(sections[i].id);
        w.u16(0);   // reserved
        w.u32(sections[i].offset);
        w.u32(sections[i].size);
    }
}

// Patches the CRC over the payload and reports the size. `total` is the whole file, which each
// caller has already checked it wrote -- including `bank_write`, whose patterns do not go
// through `w` at all.
SerializeResult finish_container(Writer &w, uint8_t *buffer, uint32_t total, uint32_t *written) {
    if (!w.ok()) return SerializeResult::buffer_too_small;
    w.patch_u32(kCrcOffset, crc32(buffer + kHeaderBytes, total - kHeaderBytes));
    if (!w.ok()) return SerializeResult::buffer_too_small;
    if (written != nullptr) *written = total;
    return SerializeResult::ok;
}

// Checks the header and finds each wanted section by id. `offset` and `length` are filled in the
// order of `wanted`, and every one of them has to be there -- except the last `optional`, which
// come back as offset 0 when the file does not carry them.
//
// That tolerance is what lets a file format GROW. A section the writer added later is optional
// on read, so a file written before it existed still loads with the reader's defaults; and
// because the table is a lookup by id, an older build reading a newer file simply does not ask
// for the section it has never heard of. Only a change to what an EXISTING byte means needs the
// version bumped. Sections ordered so the optional ones are last keeps that to one number.
//
// The checks are pattern_read's, in pattern_read's order: the declared payload against the
// buffer, then the CRC, then the version, then the table. The order is part of the contract --
// a file that is both truncated and from a newer build must give one answer, and the same answer
// whichever of the four file types it is.
//
// An offset of 0 is never valid, since every section starts past the table, which is what lets
// `offset[k] == 0` stand for "not found yet" without a second array.
SerializeResult find_sections(const uint8_t *buffer, uint32_t size, uint16_t version,
                              const uint16_t *wanted, uint32_t count, uint32_t *offset,
                              uint32_t *length, uint32_t optional = 0) {
    for (uint32_t k = 0; k < count; k++) {
        offset[k] = 0;
        length[k] = 0;
    }
    if (buffer == nullptr || size < kHeaderBytes) return SerializeResult::truncated;

    Reader r(buffer, size);
    const bool magic =
        r.u8() == kMagic0 && r.u8() == kMagic1 && r.u8() == kMagic2 && r.u8() == kMagic3;
    if (!magic) return SerializeResult::bad_magic;

    const uint16_t have = r.u16();
    const uint32_t sections = r.u16();
    const uint32_t payload = r.u32();
    const uint32_t want_crc = r.u32();
    if (!r.ok()) return SerializeResult::truncated;

    // The declared payload must be inside the buffer before the CRC may read it.
    if (payload > size - kHeaderBytes) return SerializeResult::truncated;
    if (crc32(buffer + kHeaderBytes, payload) != want_crc) return SerializeResult::bad_crc;
    if (have != version) return SerializeResult::bad_version;

    if (sections == 0 || sections > kMaxSections) return SerializeResult::malformed;
    const uint32_t table = sections * kSectionEntryBytes;
    if (table > payload) return SerializeResult::malformed;

    for (uint32_t i = 0; i < sections; i++) {
        if (!r.seek(kHeaderBytes + i * kSectionEntryBytes)) return SerializeResult::malformed;
        const uint16_t id = r.u16();
        r.skip(2);   // reserved
        const uint32_t at = r.u32();
        const uint32_t len = r.u32();
        if (!r.ok()) return SerializeResult::truncated;
        // Inside the payload and clear of the table. `len > payload - at` rather than
        // `at + len > payload`, which could wrap.
        if (at < table || at > payload || len > payload - at) {
            return SerializeResult::malformed;
        }
        for (uint32_t k = 0; k < count; k++) {
            if (id != wanted[k]) continue;
            if (offset[k] != 0) return SerializeResult::malformed;   // the same id twice
            offset[k] = at;
            length[k] = len;
        }
    }
    const uint32_t required = optional >= count ? 0 : count - optional;
    for (uint32_t k = 0; k < required; k++) {
        if (offset[k] == 0) return SerializeResult::malformed;
    }
    return SerializeResult::ok;
}

// --- PROJECT.BIN sizes ----------------------------------------------------

constexpr uint32_t kProjectSections = 6;
constexpr uint32_t kProjectSettingsBytes = 19;
constexpr uint32_t kProjectChainBytes = uint32_t(kChainEntries) * 3;
// The sends, the master bytes and the kill switches. The section GREW when the kill EQ came
// (three band bytes after the master's four, then the switches): a reader takes what it knows from
// the front, so an older build reads a newer file's first 14 bytes and ignores the rest, and this
// build reads an older file's 14 and defaults the EQ. No version bump, because no existing byte
// changed meaning (find_sections says why that is the rule).
//
// It grew again with the ambient modes (2026-11-22): their nine send bytes go AFTER the switches,
// not after the first ten send bytes, so that every byte an older build knows stays where it was.
constexpr uint32_t kProjectMixerBytesV2 =
    uint32_t(kSendFieldsV1) + uint32_t(kMasterFields) + 1;
constexpr uint32_t kProjectMixerBytes = kProjectMixerBytesV2 + uint32_t(kSendFields - kSendFieldsV1);
constexpr uint32_t kProjectMixerBytesV1 = uint32_t(kSendFieldsV1) + uint32_t(kMasterFieldsV1);
constexpr uint32_t kProjectTableBytes = kProjectSections * kSectionEntryBytes;
constexpr uint32_t kProjectSettingsOffset = kProjectTableBytes;
constexpr uint32_t kProjectChainOffset = kProjectSettingsOffset + kProjectSettingsBytes;
constexpr uint32_t kProjectMixerOffset = kProjectChainOffset + kProjectChainBytes;
constexpr uint32_t kProjectLiveBytes = 3;          // live mutes, scene A, scene B
constexpr uint32_t kProjectLiveOffset = kProjectMixerOffset + kProjectMixerBytes;
// Song mode: the SONG switch, then each chain entry's mute recall.
constexpr uint32_t kProjectSongBytes = 1 + uint32_t(kChainEntries);
constexpr uint32_t kProjectSongOffset = kProjectLiveOffset + kProjectLiveBytes;
constexpr uint32_t kProjectMidiBytes = uint32_t(kTracks);   // MIDI OUT channel per track
constexpr uint32_t kProjectMidiOffset = kProjectSongOffset + kProjectSongBytes;
constexpr uint32_t kProjectTotalBytes = kHeaderBytes + kProjectMidiOffset + kProjectMidiBytes;

// How many of `wanted` at the END of the list a reader may do without. Four: the mixer, the
// live, the song and the MIDI sections, none of which existed when the format was first written,
// so a PROJECT.BIN from before them has two to five sections and must still load
// (core/project.hpp).
constexpr uint32_t kProjectOptionalSections = 4;

static_assert(sizeof(ProjectSettings::mute_states) == 8,
              "the settings section is 19 bytes on the strength of 8 mute states");
static_assert(kProjectSettingsBytes == 2 + 4 + sizeof(ProjectSettings::mute_states) + 3 + 2,
              "the settings section and the fields written into it have diverged");
static_assert(kProjectMixerBytes == sizeof(ProjectSettings::sends) +
                                        sizeof(ProjectSettings::master) +
                                        sizeof(ProjectSettings::eq_kills),
              "the mixer section and the bytes written into it have diverged");

// --- KIT.BIN sizes --------------------------------------------------------

constexpr uint32_t kKitSections = 12;
// The second to eleventh, the LFO, the arp, SLC AUTO, the ASR page, the duck, the OSC page, the
// filter layout, LFO TRIG, the mod slots, the choke groups and the arp's settings, are optional
// on read: see
// kSectionKitLfo.
constexpr uint32_t kKitOptionalSections = 11;
constexpr uint32_t kKitTrackBytes =
    uint32_t(kKitSoundParams) + 1 + 2 + uint32_t(kSamplePathBytes);
constexpr uint32_t kKitTracksBytes = kKitTrackBytes * uint32_t(kTracks);
constexpr uint32_t kKitLfoBytes = uint32_t(kKitLfoParams) * uint32_t(kTracks);
constexpr uint32_t kKitTableBytes = kKitSections * kSectionEntryBytes;
constexpr uint32_t kKitTracksOffset = kKitTableBytes;
constexpr uint32_t kKitLfoOffset = kKitTracksOffset + kKitTracksBytes;
constexpr uint32_t kKitArpBytes = uint32_t(kKitArpParams) * uint32_t(kTracks);
constexpr uint32_t kKitArpOffset = kKitLfoOffset + kKitLfoBytes;
constexpr uint32_t kKitSlicesBytes = uint32_t(kTracks);
constexpr uint32_t kKitSlicesOffset = kKitArpOffset + kKitArpBytes;
constexpr uint32_t kKitAsrBytes = uint32_t(kKitAsrParams) * uint32_t(kTracks);
constexpr uint32_t kKitAsrOffset = kKitSlicesOffset + kKitSlicesBytes;
constexpr uint32_t kKitDuckBytes = uint32_t(kTracks) + 4;
constexpr uint32_t kKitDuckOffset = kKitAsrOffset + kKitAsrBytes;
constexpr uint32_t kKitOscBytes = uint32_t(kKitOscParams) * uint32_t(kTracks);
constexpr uint32_t kKitOscOffset = kKitDuckOffset + kKitDuckBytes;
constexpr uint32_t kKitFilterBytes = 1;
constexpr uint32_t kKitFilterOffset = kKitOscOffset + kKitOscBytes;
constexpr uint32_t kKitLfoTrigBytes = uint32_t(kTracks);
constexpr uint32_t kKitLfoTrigOffset = kKitFilterOffset + kKitFilterBytes;
constexpr uint32_t kKitModSlotBytes = 3;
constexpr uint32_t kKitModBytes = kKitModSlotBytes * uint32_t(kModSlots) * uint32_t(kTracks);
constexpr uint32_t kKitModOffset = kKitLfoTrigOffset + kKitLfoTrigBytes;
constexpr uint32_t kKitChokeBytes = uint32_t(kTracks);
constexpr uint32_t kKitChokeOffset = kKitModOffset + kKitModBytes;
constexpr uint32_t kKitArpExtBytes = uint32_t(kArpExtFields) * uint32_t(kTracks);
constexpr uint32_t kKitArpExtOffset = kKitChokeOffset + kKitChokeBytes;
constexpr uint32_t kKitTotalBytes = kHeaderBytes + kKitArpExtOffset + kKitArpExtBytes;
static_assert(kKitSoundParams == 16, "KIT.BIN's track section has carried 16 parameters since M4");

// --- BANK_x.BIN sizes -----------------------------------------------------

constexpr uint32_t kBankSections = 1;
constexpr uint32_t kBankTableBytes = kBankSections * kSectionEntryBytes;
constexpr uint32_t kBankPatternsOffset = kBankTableBytes;

// project.hpp's kBankMaxBytes allows exactly one section entry, which is this layout. If a bank
// ever grows a second section, that constant grows with it.
static_assert(kBankMaxBytes == kHeaderBytes + kBankTableBytes +
                                   uint32_t(kPatternsPerBank) * kPatternMaxBytes,
              "kBankMaxBytes and the bank's section table have diverged");

void kit_clear(Kit &kit) {
    // One track at a time: `kit = Kit{}` would be a kilobyte of temporary, and the same habit
    // one struct up would be a megabyte (project.hpp).
    for (int t = 0; t < kTracks; t++) kit.tracks[t] = KitTrack{};
    kit.duck = KitDuck{};

    // And the eight machines docs/SPEC.md's default kit assigns, rather than eight of machine 0.
    //
    // This matters now that `KitTrack::machine` is honoured: a track hosts what its kit names, so
    // a kit of zeroes is eight KICKs and a new project would have no bass, no chord and no
    // sampler until the user switched seven tracks by hand. A NEW PROJECT SHOULD BE PLAYABLE.
    //
    // It is here rather than in app/ because it is a property of the file: a KIT.BIN written by
    // this build and read by the next one has to mean the same thing, and "what a blank kit
    // means" is as much part of the format as the field's domain is.
    static constexpr MachineId kDefaultMachines[kTracks] = {
        MachineId::kick,  MachineId::snare,   MachineId::hat,     MachineId::hat,
        MachineId::perc,  MachineId::va_mono, MachineId::va_poly, MachineId::sample,
    };
    for (int t = 0; t < kTracks; t++) {
        kit.tracks[t].machine = uint8_t(kDefaultMachines[t]);
        // A zero TON is the darkest low-pass, not "off" (D8): a blank synth track starts flat.
        if (machine_has_voice_filter(kit.tracks[t].machine)) kit.tracks[t].params[kSlotTone] = kToneFlat;
    }
}

}  // namespace

// --- initialising ---------------------------------------------------------

void project_init(Project &project) {
    // Field by field, and never `project = Project{}`: a Project is about 1.03 MiB and a stack
    // frame is capped at 16 KiB (CLAUDE.md).
    project.settings = ProjectSettings{};
    kit_clear(project.kit);
    for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
    for (int bank = 0; bank < kBanks; bank++) {
        for (int slot = 0; slot < kPatternsPerBank; slot++) {
            pattern_init(project.patterns[bank][slot]);
        }
        project.bank_dirty[bank] = false;
    }
    project.bank = 0;
    project.slot = 0;
}

// --- file names -----------------------------------------------------------

bool bank_file_name(int bank, char *out, int max) {
    const char letter = bank_letter(bank);
    if (letter == '?') return false;
    // There is no snprintf on this platform (CLAUDE.md) and the name has one variable
    // character, so it is assembled by hand. 11 bytes with the terminator.
    const char name[] = {'B', 'A', 'N', 'K', '_', letter, '.', 'B', 'I', 'N', '\0'};
    const int need = int(sizeof(name));
    if (out == nullptr || max < need) return false;
    for (int i = 0; i < need; i++) out[i] = name[i];
    return true;
}

// --- PROJECT.BIN ----------------------------------------------------------

uint32_t project_bytes() { return kProjectTotalBytes; }

SerializeResult project_write(const Project &project, uint8_t *buffer, uint32_t capacity,
                              uint32_t *written) {
    if (written != nullptr) *written = 0;
    // Checked before a byte is stored, so a buffer that is too small comes back untouched.
    if (buffer == nullptr || capacity < kProjectTotalBytes) {
        return SerializeResult::buffer_too_small;
    }

    const Section sections[kProjectSections] = {
        {kSectionProjectSettings, kProjectSettingsOffset, kProjectSettingsBytes},
        {kSectionProjectChain, kProjectChainOffset, kProjectChainBytes},
        {kSectionProjectMixer, kProjectMixerOffset, kProjectMixerBytes},
        {kSectionProjectLive, kProjectLiveOffset, kProjectLiveBytes},
        {kSectionProjectSong, kProjectSongOffset, kProjectSongBytes},
        {kSectionProjectMidi, kProjectMidiOffset, kProjectMidiBytes},
    };

    Writer w(buffer, capacity);
    write_container(w, kProjectVersion, sections, kProjectSections,
                    kProjectTotalBytes - kHeaderBytes);

    // Values go down as they stand. Clamping belongs to the reader, which is the side that
    // cannot trust its bytes; a writer that clamped would make a save and a load disagree about
    // what the project currently is.
    const ProjectSettings &s = project.settings;
    w.u16(s.tempo_centi_bpm);
    w.u8(s.root);
    w.u8(s.scale);
    w.u8(uint8_t(kSwingMin));   // the reserved byte: a legal swing, for builds that still read it
    w.u8(s.quantize);
    for (uint32_t i = 0; i < sizeof(s.mute_states); i++) w.u8(s.mute_states[i]);
    w.u8(s.sample_budget_mib);
    w.u8(s.chain_length);
    w.u8(s.chain_enabled ? 1 : 0);
    w.u8(project.bank);
    w.u8(project.slot);

    for (int i = 0; i < kChainEntries; i++) {
        w.u8(project.chain[i].bank);
        w.u8(project.chain[i].slot);
        w.u8(project.chain[i].repeats);
    }

    // The sends and the master, as the encoder bytes they are everywhere but inside the mixer.
    for (int i = 0; i < kSendFieldsV1; i++) w.u8(s.sends[i]);
    for (int i = 0; i < kMasterFields; i++) w.u8(s.master[i]);
    // The kill switches are a performance move and are NOT saved (Roland, 2026-11-22): a
    // project saved or autosaved with LOW killed came back with no bass. The byte stays, as 0,
    // so the layout an older build reads is unchanged.
    w.u8(0);
    for (int i = kSendFieldsV1; i < kSendFields; i++) w.u8(s.sends[i]);

    w.u8(s.live_mutes);
    w.u8(s.scene_a);
    w.u8(s.scene_b);

    w.u8(s.chain_song ? 1 : 0);
    for (int i = 0; i < kChainEntries; i++) w.u8(project.chain[i].mutes);
    for (int t = 0; t < kTracks; t++) w.u8(s.midi_out[t]);

    if (!w.ok() || w.size() != kProjectTotalBytes) return SerializeResult::buffer_too_small;
    return finish_container(w, buffer, kProjectTotalBytes, written);
}

// `-fno-unroll-loops` for THIS FUNCTION ONLY, and it is a compiler bug rather than anything
// about the code.
//
// GCC 13.3.0 for tic6x-elf crashes compiling this function with `-O2 -funroll-loops`, which is
// what Enaga's build_app.py passes: an internal compiler error in the instruction scheduler,
// "in priority, at haifa-sched.cc:1597", reached through undo_replacements_for_backtrack. It is
// not the optimisation level and not the scheduler on its own -- `-O2`, `-O2
// -fno-schedule-insns` and `-O2 -fno-schedule-insns2` all compile this file -- it needs
// `-funroll-loops` as well. Nothing in `app/` or on the host ever sees it, because the host
// build does not unroll.
//
// The flags belong to build_app.py and CLAUDE.md forbids modifying third_party/enaga, so the
// narrowest place to turn it off is here. It is scoped to one function with push_options, and
// the cost is that the chain and track loops in a project LOAD are not unrolled -- a load, in
// the main context, once per project.
//
// To be reported upstream with this file as the failing case. If a later toolchain fixes it,
// delete the pragma and this comment together.
#pragma GCC push_options
#pragma GCC optimize("no-unroll-loops")
SerializeResult project_read(Project &project, const uint8_t *buffer, uint32_t size) {
    // Every refusal goes through here, so a refused load leaves the defaults rather than half a
    // file -- but only of what PROJECT.BIN owns. The patterns are deliberately untouched: they
    // come from the bank files, and app/ reads PROJECT.BIN first, so emptying them here would
    // let one damaged PROJECT.BIN cost 128 patterns that are about to load perfectly well.
    const auto refuse = [&project](SerializeResult why) {
        project.settings = ProjectSettings{};
        for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
        project.bank = 0;
        project.slot = 0;
        return why;
    };

    // The four optional sections (mixer, live, song, MIDI) come LAST: kProjectOptionalSections.
    const uint16_t wanted[kProjectSections] = {kSectionProjectSettings, kSectionProjectChain,
                                               kSectionProjectMixer, kSectionProjectLive,
                                               kSectionProjectSong, kSectionProjectMidi};
    uint32_t offset[kProjectSections] = {};
    uint32_t length[kProjectSections] = {};
    const SerializeResult found =
        find_sections(buffer, size, kProjectVersion, wanted, kProjectSections, offset, length,
                      kProjectOptionalSections);
    if (found != SerializeResult::ok) return refuse(found);
    if (length[0] < kProjectSettingsBytes) return refuse(SerializeResult::malformed);
    if (length[1] < kProjectChainBytes) return refuse(SerializeResult::malformed);
    // Present but SHORT is damage, not an old file: a writer that declared the section declared
    // its size too. Absent is the old file, and that is the `offset[2] != 0` test below.
    if (offset[2] != 0 && length[2] < kProjectMixerBytesV1) {
        return refuse(SerializeResult::malformed);
    }
    if (offset[3] != 0 && length[3] < kProjectLiveBytes) {
        return refuse(SerializeResult::malformed);
    }
    if (offset[4] != 0 && length[4] < kProjectSongBytes) {
        return refuse(SerializeResult::malformed);
    }
    if (offset[5] != 0 && length[5] < kProjectMidiBytes) {
        return refuse(SerializeResult::malformed);
    }

    Reader r(buffer, size);
    if (!r.seek(kHeaderBytes + offset[0])) return refuse(SerializeResult::malformed);

    const uint16_t tempo = r.u16();
    const uint8_t root = r.u8();
    const uint8_t scale = r.u8();
    (void)r.u8();                // reserved (the old project swing)
    const uint8_t quantize = r.u8();
    uint8_t mutes[sizeof(ProjectSettings::mute_states)] = {};
    for (uint32_t i = 0; i < sizeof(mutes); i++) mutes[i] = r.u8();
    const uint8_t budget = r.u8();
    const uint8_t chain_length = r.u8();
    const uint8_t chain_enabled = r.u8();
    const uint8_t live_bank = r.u8();
    const uint8_t live_slot = r.u8();
    if (!r.ok()) return refuse(SerializeResult::truncated);

    ProjectSettings &s = project.settings;
    s.tempo_centi_bpm = clamp_u16(tempo, kTempoCentiMin, kTempoCentiMax);
    s.root = clamp_u8(root, 0, 127);
    // `scale` is not clamped, for core/pattern.hpp's reason about a lock's param: core/ does not
    // know how many scales there are, and a scale id this build does not have should survive a
    // round trip rather than be thrown away. The UI draws an unknown id as the chromatic
    // default.
    s.scale = scale;
    s.quantize = clamp_u8(quantize, 0, 100);
    // Every bit pattern is a legal set of mute states for 8 tracks, so these are taken as they
    // come.
    for (uint32_t i = 0; i < sizeof(mutes); i++) s.mute_states[i] = mutes[i];
    s.sample_budget_mib = clamp_u8(budget, kBudgetMibMin, kBudgetMibMax);
    s.chain_length = clamp_u8(chain_length, 0, uint8_t(kChainEntries));
    s.chain_enabled = chain_enabled != 0;

    if (!r.seek(kHeaderBytes + offset[1])) return refuse(SerializeResult::malformed);
    for (int i = 0; i < kChainEntries; i++) {
        const uint8_t bank = r.u8();
        const uint8_t slot = r.u8();
        const uint8_t repeats = r.u8();
        // An entry naming a pattern that cannot exist is dropped whole rather than clamped to
        // A01: core/queue.hpp reads entries in order and stops at the first with no repeats, so
        // a zeroed entry ends the chain where the damage is, while a clamp would quietly play a
        // pattern nobody put there. An entry with no repeats is unused, so it is canonicalised
        // to the zeroed entry that project.hpp says means exactly that.
        const bool usable = repeats != 0 && project_index_valid(bank, slot);
        project.chain[i].bank = usable ? bank : 0;
        project.chain[i].slot = usable ? slot : 0;
        project.chain[i].repeats = usable ? repeats : 0;
    }
    if (!r.ok()) return refuse(SerializeResult::truncated);

    // The sends and the master. Absent means a PROJECT.BIN written before the section existed,
    // and then these fields take the DEFAULTS -- explicitly, not by being left alone. `project`
    // is a buffer the app reuses (app/engine.hpp's `project_for_load`), so "left alone" would
    // mean the previous project's delay and reverb quietly carrying over into one whose file
    // says nothing about them. Every other field here is unconditionally read, so this is the
    // only one that can happen to.
    if (offset[2] == 0) {
        const ProjectSettings defaults;
        for (int i = 0; i < kSendFields; i++) s.sends[i] = defaults.sends[i];
        for (int i = 0; i < kMasterFields; i++) s.master[i] = defaults.master[i];
        s.eq_kills = defaults.eq_kills;
    } else {
        if (!r.seek(kHeaderBytes + offset[2])) return refuse(SerializeResult::malformed);
        // Clamped to 0-127, the encoder's range, because dsp/buses.hpp maps a byte through
        // `byte_unit` and a 255 there would be double the top of the field's range. Clamping
        // here rather than there keeps the mapping a pure function of a legal byte.
        for (int i = 0; i < kSendFieldsV1; i++) s.sends[i] = clamp_u8(r.u8(), 0, 127);
        // A section from before the kill EQ stops after the fourth master byte: the bands and
        // the switches then take the defaults, flat and off, for the same "never left alone"
        // reason as above.
        const bool has_eq = length[2] >= kProjectMixerBytesV2;
        const bool has_ambient = length[2] >= kProjectMixerBytes;
        const ProjectSettings defaults;
        const int master_fields = has_eq ? kMasterFields : kMasterFieldsV1;
        for (int i = 0; i < master_fields; i++) s.master[i] = clamp_u8(r.u8(), 0, 127);
        for (int i = master_fields; i < kMasterFields; i++) s.master[i] = defaults.master[i];
        // The switch byte is skipped: kills are not saved, and a file from before that decision
        // may hold some. A project always opens with every band in.
        if (has_eq) (void)r.u8();
        s.eq_kills = defaults.eq_kills;
        // The ambient modes, after the switches; an older file is classic on both sends.
        for (int i = kSendFieldsV1; i < kSendFields; i++) {
            s.sends[i] = has_ambient ? clamp_u8(r.u8(), 0, 127) : defaults.sends[i];
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The live mutes and the A/B scenes, defaulted rather than left alone when absent, for the
    // mixer's reason above. Every mute byte is legal; the scene ids are the engine's to check.
    if (offset[3] == 0) {
        const ProjectSettings defaults;
        s.live_mutes = defaults.live_mutes;
        s.scene_a = defaults.scene_a;
        s.scene_b = defaults.scene_b;
    } else {
        if (!r.seek(kHeaderBytes + offset[3])) return refuse(SerializeResult::malformed);
        s.live_mutes = r.u8();
        s.scene_a = r.u8();
        s.scene_b = r.u8();
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // Song mode, defaulted when absent for the mixer's reason: a looping chain, no mute recalls.
    // A recall past the eight states is dropped to none, and so is one on an entry the chain
    // section left unused, so an unused entry stays the zeroed entry project.hpp says it is.
    s.chain_song = false;
    for (int i = 0; i < kChainEntries; i++) project.chain[i].mutes = 0;
    if (offset[4] != 0) {
        if (!r.seek(kHeaderBytes + offset[4])) return refuse(SerializeResult::malformed);
        s.chain_song = r.u8() != 0;
        for (int i = 0; i < kChainEntries; i++) {
            const uint8_t mutes = r.u8();
            const bool usable = project.chain[i].repeats != 0 && mutes <= kChainMutesMax;
            project.chain[i].mutes = usable ? mutes : 0;
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The MIDI OUT channels: absent is an older file, where no track sent MIDI. A byte past 16
    // is damage and turns that track's MIDI out off rather than onto some channel nobody chose.
    for (int t = 0; t < kTracks; t++) s.midi_out[t] = kMidiOutOff;
    if (offset[5] != 0) {
        if (!r.seek(kHeaderBytes + offset[5])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            const uint8_t channel = r.u8();
            s.midi_out[t] = channel <= kMidiChannels ? channel : kMidiOutOff;
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The live pattern. Not a user setting but project state: docs/SPEC.md reopens the last
    // project on start, and reopening it on A01 rather than where the user left off is a loss
    // these two bytes avoid.
    const bool live_ok = project_index_valid(live_bank, live_slot);
    project.bank = live_ok ? live_bank : 0;
    project.slot = live_ok ? live_slot : 0;

    // `bank_dirty` is left alone on purpose. It says whether a bank's patterns differ from its
    // file, and this function has not touched a pattern.
    return SerializeResult::ok;
}
#pragma GCC pop_options

// --- KIT.BIN --------------------------------------------------------------

uint32_t kit_bytes() { return kKitTotalBytes; }

SerializeResult kit_write(const Kit &kit, uint8_t *buffer, uint32_t capacity, uint32_t *written) {
    if (written != nullptr) *written = 0;
    if (buffer == nullptr || capacity < kKitTotalBytes) return SerializeResult::buffer_too_small;

    const Section sections[kKitSections] = {
        {kSectionKitTracks, kKitTracksOffset, kKitTracksBytes},
        {kSectionKitLfo, kKitLfoOffset, kKitLfoBytes},
        {kSectionKitArp, kKitArpOffset, kKitArpBytes},
        {kSectionKitSlices, kKitSlicesOffset, kKitSlicesBytes},
        {kSectionKitAsr, kKitAsrOffset, kKitAsrBytes},
        {kSectionKitDuck, kKitDuckOffset, kKitDuckBytes},
        {kSectionKitOsc, kKitOscOffset, kKitOscBytes},
        {kSectionKitFilter, kKitFilterOffset, kKitFilterBytes},
        {kSectionKitLfoTrig, kKitLfoTrigOffset, kKitLfoTrigBytes},
        {kSectionKitMod, kKitModOffset, kKitModBytes},
        {kSectionKitChoke, kKitChokeOffset, kKitChokeBytes},
        {kSectionKitArpExt, kKitArpExtOffset, kKitArpExtBytes},
    };

    Writer w(buffer, capacity);
    write_container(w, kKitVersion, sections, kKitSections, kKitTotalBytes - kHeaderBytes);

    for (int t = 0; t < kTracks; t++) {
        const KitTrack &track = kit.tracks[t];
        for (int p = 0; p < kKitSoundParams; p++) w.u8(track.params[p]);
        w.u8(track.machine);
        w.u16(level_to_milli(track.level));
        write_path(w, track.sample);
    }
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitLfoEnd; p++) w.u8(kit.tracks[t].params[p]);
    }
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitLfoEnd; p < kKitArpEnd; p++) w.u8(kit.tracks[t].params[p]);
    }
    for (int t = 0; t < kTracks; t++) w.u8(kit.tracks[t].slice_mode);
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitArpEnd; p < kKitAsrEnd; p++) w.u8(kit.tracks[t].params[p]);
    }
    for (int t = 0; t < kTracks; t++) w.u8(kit.tracks[t].duck);
    w.u8(kit.duck.source);
    w.u8(kit.duck.attack_ms);
    w.u8(kit.duck.hold);
    w.u8(kit.duck.release);
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitAsrEnd; p < kKitParams; p++) w.u8(kit.tracks[t].params[p]);
    }
    w.u8(kKitFilterLayout);
    for (int t = 0; t < kTracks; t++) w.u8(kit.tracks[t].lfo_trig);
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &slot = kit.tracks[t].mod[s];
            w.u8(slot.source);
            w.u8(slot.dest);
            w.u8(uint8_t(slot.depth));
        }
    }
    for (int t = 0; t < kTracks; t++) w.u8(choke_group_sanitized(kit.tracks[t].choke));
    for (int t = 0; t < kTracks; t++) {
        for (int f = 0; f < kArpExtFields; f++) w.u8(arp_ext_get(kit.tracks[t].arp, f));
    }

    if (!w.ok() || w.size() != kKitTotalBytes) return SerializeResult::buffer_too_small;
    return finish_container(w, buffer, kKitTotalBytes, written);
}

SerializeResult kit_read(Kit &kit, const uint8_t *buffer, uint32_t size) {
    const auto refuse = [&kit](SerializeResult why) {
        kit_clear(kit);
        return why;
    };

    const uint16_t wanted[kKitSections] = {kSectionKitTracks, kSectionKitLfo, kSectionKitArp,
                                           kSectionKitSlices, kSectionKitAsr, kSectionKitDuck,
                                           kSectionKitOsc, kSectionKitFilter, kSectionKitLfoTrig,
                                           kSectionKitMod, kSectionKitChoke, kSectionKitArpExt};
    uint32_t offset[kKitSections] = {};
    uint32_t length[kKitSections] = {};
    const SerializeResult found = find_sections(buffer, size, kKitVersion, wanted, kKitSections,
                                                offset, length, kKitOptionalSections);
    if (found != SerializeResult::ok) return refuse(found);
    if (length[0] < kKitTracksBytes) return refuse(SerializeResult::malformed);
    // Present but short is damage; absent is a kit from before the LFO.
    if (offset[1] != 0 && length[1] < kKitLfoBytes) return refuse(SerializeResult::malformed);
    if (offset[2] != 0 && length[2] < kKitArpBytes) return refuse(SerializeResult::malformed);
    if (offset[3] != 0 && length[3] < kKitSlicesBytes) return refuse(SerializeResult::malformed);
    if (offset[4] != 0 && length[4] < kKitAsrBytes) return refuse(SerializeResult::malformed);
    if (offset[5] != 0 && length[5] < kKitDuckBytes) return refuse(SerializeResult::malformed);
    if (offset[6] != 0 && length[6] < kKitOscBytes) return refuse(SerializeResult::malformed);
    if (offset[7] != 0 && length[7] < kKitFilterBytes) return refuse(SerializeResult::malformed);
    if (offset[8] != 0 && length[8] < kKitLfoTrigBytes) return refuse(SerializeResult::malformed);
    if (offset[9] != 0 && length[9] < kKitModBytes) return refuse(SerializeResult::malformed);
    if (offset[10] != 0 && length[10] < kKitChokeBytes) return refuse(SerializeResult::malformed);
    if (offset[11] != 0 && length[11] < kKitArpExtBytes) return refuse(SerializeResult::malformed);

    Reader r(buffer, size);
    if (!r.seek(kHeaderBytes + offset[0])) return refuse(SerializeResult::malformed);

    for (int t = 0; t < kTracks; t++) {
        KitTrack &track = kit.tracks[t];
        // Clamped, not taken as they come: dsp/track.hpp maps a parameter either by x(1/127) or
        // as a table index, so a 255 that survived the CRC would be an out-of-range read down
        // in a machine rather than an odd sound.
        for (int p = 0; p < kKitSoundParams; p++) {
            track.params[p] = clamp_u8(r.u8(), 0, kParamMax);
        }
        const uint8_t machine = r.u8();
        // A machine this build does not have falls back to the FIRST machine, not the last one:
        // an unknown id should land somewhere known, and clamping to kMachines - 1 would hand a
        // damaged byte whichever machine happens to sit at the end of the list.
        track.machine = machine < uint8_t(kMachines) ? machine : 0;
        track.level = level_from_milli(r.u16());
        read_path(r, track.sample);
    }
    if (!r.ok()) return refuse(SerializeResult::truncated);

    // The LFO, the arp, then the ASR page. Absent means a kit written before it existed, and then
    // it is OFF (all zeros, which the ASR page reads as the VAs' old envelope) -- explicitly,
    // because `kit` may be a buffer holding the previous kit (the kit browser's inbox, a project
    // load), and "left alone" would carry its settings into a kit that has none.
    const int firsts[3] = {kKitSoundParams, kKitLfoEnd, kKitArpEnd};
    const int ends[3] = {kKitLfoEnd, kKitArpEnd, kKitAsrEnd};
    const int sections[3] = {1, 2, 4};
    for (int k = 0; k < 3; k++) {
        const uint32_t at = offset[sections[k]];
        if (at == 0) {
            for (int t = 0; t < kTracks; t++) {
                for (int p = firsts[k]; p < ends[k]; p++) kit.tracks[t].params[p] = 0;
            }
            continue;
        }
        if (!r.seek(kHeaderBytes + at)) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            for (int p = firsts[k]; p < ends[k]; p++) {
                kit.tracks[t].params[p] = clamp_u8(r.u8(), 0, kParamMax);
            }
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // SLC AUTO. Absent is a kit from before it: equal slices everywhere, set explicitly for the
    // reason above. Anything but 1 is equal, so a damaged byte slices as before.
    for (int t = 0; t < kTracks; t++) kit.tracks[t].slice_mode = kSliceEqual;
    if (offset[3] != 0) {
        if (!r.seek(kHeaderBytes + offset[3])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            kit.tracks[t].slice_mode = r.u8() == kSliceAuto ? kSliceAuto : kSliceEqual;
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The duck. Absent is a kit from before it was editable, and then it is the duck as it was
    // fixed -- set explicitly, for the same reason as the sections above. Clamped into the
    // domains core/project.hpp gives, so a damaged byte is an odd duck and never a divide by 0.
    for (int t = 0; t < kTracks; t++) kit.tracks[t].duck = kDuckDepthDefault;
    kit.duck = KitDuck{};
    if (offset[5] != 0) {
        if (!r.seek(kHeaderBytes + offset[5])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            kit.tracks[t].duck = clamp_u8(r.u8(), 0, kDuckDepthMax);
        }
        const uint8_t source = r.u8();
        kit.duck.source = source < uint8_t(kTracks) ? source : 0;
        kit.duck.attack_ms = clamp_u8(r.u8(), kDuckAttackMin, kDuckAttackMax);
        kit.duck.hold = clamp_u8(r.u8(), 0, kDuckHoldMax);
        kit.duck.release = clamp_u8(r.u8(), kDuckReleaseMin, kDuckReleaseMax);
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The OSC page. Absent is a kit from before it, which gets the legacy voice (kKitOscLegacy),
    // set explicitly for the reason above.
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitOscParams; p++) kit.tracks[t].params[kKitAsrEnd + p] = kKitOscLegacy[p];
    }
    if (offset[6] != 0) {
        if (!r.seek(kHeaderBytes + offset[6])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            for (int p = kKitAsrEnd; p < kKitParams; p++) {
                kit.tracks[t].params[p] = clamp_u8(r.u8(), 0, kParamMax);
            }
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // One filter on the synth machines (D8). Absent, or an older layout, is a kit whose VA and
    // WAVE tracks still hold the insert chain's DRV CUT RES MOD in slots 4-7: those become FDE
    // tied and the nearest TON (core/project.hpp, kit_track_to_voice_filter). Other machines'
    // slots 4-7 still mean the insert filter, so they are left as they are.
    uint8_t layout = 0;
    if (offset[7] != 0) {
        if (!r.seek(kHeaderBytes + offset[7])) return refuse(SerializeResult::malformed);
        layout = r.u8();
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }
    if (layout < kKitFilterLayout) {
        for (int t = 0; t < kTracks; t++) {
            if (machine_has_voice_filter(kit.tracks[t].machine)) {
                kit_track_to_voice_filter(kit.tracks[t].params);
            }
        }
    }

    // LFO TRIG. Absent is a kit from before it: every LFO FREE, set explicitly for the reason
    // the LFO's section gives. A byte past ONE-SHOT is damage and runs FREE.
    for (int t = 0; t < kTracks; t++) kit.tracks[t].lfo_trig = kLfoTrigFree;
    if (offset[8] != 0) {
        if (!r.seek(kHeaderBytes + offset[8])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            const uint8_t trig = r.u8();
            kit.tracks[t].lfo_trig = trig < kLfoTrigs ? trig : kLfoTrigFree;
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The mod slots. Absent is a kit from before them: every slot OFF, set explicitly as TRIG
    // is. A slot naming an unknown source or a destination no slot can have reads as OFF.
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kModSlots; s++) kit.tracks[t].mod[s] = KitModSlot{};
    }
    if (offset[9] != 0) {
        if (!r.seek(kHeaderBytes + offset[9])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            for (int s = 0; s < kModSlots; s++) {
                const uint8_t source = r.u8();
                const uint8_t dest = r.u8();
                const uint8_t depth = r.u8();
                kit.tracks[t].mod[s] = mod_slot_sanitized(source, dest, depth);
            }
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The choke groups. Absent is a kit from before them: no track in a group, set explicitly as
    // TRIG is. A byte past the last group is damage and reads as none.
    for (int t = 0; t < kTracks; t++) kit.tracks[t].choke = 0;
    if (offset[10] != 0) {
        if (!r.seek(kHeaderBytes + offset[10])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) kit.tracks[t].choke = choke_group_sanitized(r.u8());
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The arp's settings. Absent is a kit from before them: all four 0 on every track, the arp
    // as it played before. A value past its table is damage and reads as 0.
    for (int t = 0; t < kTracks; t++) kit.tracks[t].arp = KitArp{};
    if (offset[11] != 0) {
        if (!r.seek(kHeaderBytes + offset[11])) return refuse(SerializeResult::malformed);
        for (int t = 0; t < kTracks; t++) {
            KitArp arp;
            for (int f = 0; f < kArpExtFields; f++) arp_ext_set(arp, f, r.u8());
            kit.tracks[t].arp = arp;
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }
    return SerializeResult::ok;
}

// --- BANK_x.BIN -----------------------------------------------------------

uint32_t bank_bytes(const Project &project, int bank) {
    if (!project_index_valid(bank, 0)) return 0;
    uint32_t total = kHeaderBytes + kBankTableBytes;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        total += pattern_bytes(project.patterns[bank][slot]);
    }
    return total;
}

SerializeResult bank_write(const Project &project, int bank, uint8_t *buffer, uint32_t capacity,
                           uint32_t *written) {
    if (written != nullptr) *written = 0;
    // There is no result for "no such bank", and serialize.hpp is not this module's to extend,
    // so an index no pattern exists at is reported as `malformed` -- the file that would have
    // been written does not add up.
    if (!project_index_valid(bank, 0)) return SerializeResult::malformed;

    const uint32_t total = bank_bytes(project, bank);
    if (buffer == nullptr || capacity < total) return SerializeResult::buffer_too_small;

    const Section sections[kBankSections] = {
        {kSectionBankPatterns, kBankPatternsOffset, total - kHeaderBytes - kBankTableBytes},
    };

    Writer w(buffer, capacity);
    write_container(w, kBankVersion, sections, kBankSections, total - kHeaderBytes);
    if (!w.ok() || w.size() != kHeaderBytes + kBankTableBytes) {
        return SerializeResult::buffer_too_small;
    }

    // `pattern_write` takes a buffer, not a cursor, so the patterns are written past the end of
    // what `w` has stored and the position is tracked here. `w` is used once more, for the CRC.
    uint32_t at = w.size();
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        uint32_t one = 0;
        const SerializeResult res =
            pattern_write(project.patterns[bank][slot], buffer + at, capacity - at, &one);
        // `bank_bytes` sized the buffer from the same patterns, so this cannot happen unless the
        // two disagree -- in which case the file must not be handed on as valid.
        if (res != SerializeResult::ok) return res;
        at += one;
    }
    if (at != total) return SerializeResult::buffer_too_small;

    return finish_container(w, buffer, total, written);
}

SerializeResult bank_read(Project &project, int bank, const uint8_t *buffer, uint32_t size,
                          int *refused) {
    // `refused` counts patterns the file held and this build would not take. A container-level
    // refusal leaves it at 0: the file failed, not a pattern in it, and the result says which.
    if (refused != nullptr) *refused = 0;
    if (!project_index_valid(bank, 0)) return SerializeResult::malformed;

    const auto refuse = [&project, bank](SerializeResult why) {
        // All 16 slots, for pattern_read's reason one level up: a refused load leaves empty
        // patterns rather than a bank that is half this file and half whatever was there before.
        for (int slot = 0; slot < kPatternsPerBank; slot++) {
            pattern_init(project.patterns[bank][slot]);
        }
        project.bank_dirty[bank] = false;
        return why;
    };

    const uint16_t wanted[kBankSections] = {kSectionBankPatterns};
    uint32_t offset[kBankSections] = {};
    uint32_t length[kBankSections] = {};
    const SerializeResult found =
        find_sections(buffer, size, kBankVersion, wanted, kBankSections, offset, length);
    if (found != SerializeResult::ok) return refuse(found);

    int damaged = 0;
    uint32_t at = kHeaderBytes + offset[0];
    uint32_t left = length[0];

    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        // Where this pattern ends, from its own container header. `pattern_read` reports nothing
        // when it refuses, so the extent has to be known before it is called -- that is what
        // makes a refused pattern cost one slot instead of every slot after it.
        uint32_t one = 0;
        if (left >= kHeaderBytes) {
            Reader probe(buffer + at, left);
            probe.skip(8);   // magic, version, section count
            const uint32_t payload = probe.u32();
            if (probe.ok() && payload <= left - kHeaderBytes) one = kHeaderBytes + payload;
        }
        if (one == 0) {
            // The section ran out, or this pattern's header does not say where it ends. Either
            // way the patterns after it cannot be found, so this one and all of them are
            // refused rather than guessed at.
            for (int rest = slot; rest < kPatternsPerBank; rest++) {
                pattern_init(project.patterns[bank][rest]);
                damaged++;
            }
            break;
        }
        // Exactly this pattern's bytes, so nothing it reads can come from its neighbour.
        if (pattern_read(project.patterns[bank][slot], buffer + at, one) !=
            SerializeResult::ok) {
            damaged++;   // left empty by pattern_read, which never half fills one
        }
        at += one;
        left -= one;
    }

    // What is in RAM is now what is in the file, damaged slots included, so nothing here is
    // waiting to be saved. A refused slot stays clean on purpose: marking the bank dirty would
    // let the next save overwrite the damaged file and with it any chance of recovering it.
    project.bank_dirty[bank] = false;
    if (refused != nullptr) *refused = damaged;
    return SerializeResult::ok;
}

}  // namespace strata
