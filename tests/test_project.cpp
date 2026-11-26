// The three project files: PROJECT.BIN, KIT.BIN and BANK_A.BIN .. BANK_H.BIN.
//
// core/project.cpp builds three more containers on core/serialize.cpp's primitives rather than
// sharing a helper with it, so the four file types can be changed without reading each other.
// The price of that choice is that the four can drift apart, and the thing most worth testing
// here is that they have not: `the_four_file_types_agree_about_damage` applies the same six kinds
// of damage to a pattern, a project, a kit and a bank and requires one answer from all four.
//
// The rest follows docs/SPEC.md's rule for patterns, now owed by three more files: loading is
// value-tolerant but structure-strict. So each round trip is checked field by field, each clamp
// is driven by poking the bytes directly and repairing the CRC (never by writing an out-of-range
// value, which the writer is documented not to clamp), and each structural refusal is checked to
// leave defaults rather than half a file.
//
// `one_damaged_pattern_costs_one_pattern` is the invariant project.hpp sells the bank format on,
// and `a_bank_is_sixteen_patterns_back_to_back` is the layout that makes it possible: the bank is
// re-parsed here with `pattern_read`'s `consumed` alone, so a bank that only read correctly
// because `bank_read` and `bank_write` agreed on something undocumented would be caught.
//
// The buffers and projects are file-scope statics. A Project is about 1.03 MiB and a bank file
// about 129 KiB, where CLAUDE.md caps a stack frame at 16 KiB.
#include "check.hpp"

#include "pattern.hpp"
#include "project.hpp"
#include "serialize.hpp"

#include <cstring>
#include <initializer_list>

namespace {

using namespace strata;

// The sizes project.cpp documents, stated here as constants because the buffers need them at
// compile time. `the_file_sizes_are_what_the_format_says` holds the functions to them.
// 277, not the 251 the format had before the mixer section: one more section-table entry (12)
// and the 14 send and master bytes. An OLD 251-byte file still loads, because that section is
// optional on read -- `a_project_file_from_before_the_mixer_section_still_loads` is the proof.
// 292 since the live section: another table entry (12) and its 3 bytes, optional likewise.
// 296 since the kill EQ grew the mixer section by its three band bytes and the kill switches.
// 373 since song mode: a fifth table entry (12) and its 65 bytes, optional like the others.
// 393 since MIDI OUT: a sixth entry (12) and one channel per track (8). 402 since the sends'
// ambient modes: nine bytes on the end of the mixer section.
constexpr uint32_t kProjectFileBytes = 402;
constexpr uint32_t kKitFileBytes = 1389;   // 1329 before the mod slots (12 + 48), 1309 before LFO TRIG (12 + 8), 1296 before the filter layout (12 + 1), 1076 before the LFO and arp (12 + 8 x 4 each), 1164 before SLC AUTO (12 + 8), 1184 before ASR (12 + 32), 1228 before the duck (12 + 12), 1252 before OSC (12 + 32)

// Room past the end of every file, filled with a sentinel, so that a writer going one byte long
// is caught by value rather than by luck.
constexpr uint32_t kSlack = 64;
constexpr uint8_t kSentinel = 0xAB;

uint8_t g_project[kProjectFileBytes + kSlack];
uint8_t g_project_alt[kProjectFileBytes + kSlack];
uint8_t g_kit_file[kKitFileBytes + kSlack];
uint8_t g_kit_alt[kKitFileBytes + kSlack];
uint8_t g_bank[kBankMaxBytes + kSlack];
uint8_t g_bank_alt[kBankMaxBytes + kSlack];
uint8_t g_pattern_file[kPatternMaxBytes + kSlack];

Project g_source;
Project g_loaded;
Kit g_kit_source;
Kit g_kit_loaded;
Pattern g_scratch;

// --- little helpers over the raw bytes ------------------------------------
// Deliberately not the Writer and Reader: the layout is read back here the way serialize.hpp
// documents it, so a cursor that quietly changed the layout would be caught rather than followed.

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

// Makes a deliberately damaged file consistent again, so that a test about a clamp or about
// structure is not answered by the CRC instead.
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

// The section table entry for an id, as an absolute index, so a test can shrink a section.
uint32_t section_entry_at(const uint8_t *buffer, uint16_t id) {
    const uint32_t sections = peek_u16(buffer, 6);
    for (uint32_t i = 0; i < sections; i++) {
        const uint32_t entry = kHeaderBytes + i * kSectionEntryBytes;
        if (peek_u16(buffer, entry) == id) return entry;
    }
    return 0;
}

// The file as the build before its LAST section existed wrote it: one table entry fewer, every
// section 12 bytes earlier, the last section's bytes gone. Returns the new size. The last section
// is the one at the end of the payload, which is how every writer here lays them out.
uint32_t drop_last_section(const uint8_t *file, uint8_t *out) {
    const uint32_t count = peek_u16(file, 6);
    const uint32_t last = kHeaderBytes + (count - 1) * kSectionEntryBytes;
    const uint32_t cut = peek_u32(file, last + 4);          // where the last section starts
    for (uint32_t i = 0; i < kHeaderBytes; i++) out[i] = file[i];
    for (uint32_t i = 0; i + 1 < count; i++) {
        const uint32_t from = kHeaderBytes + i * kSectionEntryBytes;
        poke_u16(out, from, peek_u16(file, from));
        poke_u16(out, from + 2, 0);
        poke_u32(out, from + 4, peek_u32(file, from + 4) - kSectionEntryBytes);
        poke_u32(out, from + 8, peek_u32(file, from + 8));
    }
    const uint32_t data_from = kHeaderBytes + count * kSectionEntryBytes;
    const uint32_t data_to = kHeaderBytes + cut;
    uint32_t at = kHeaderBytes + (count - 1) * kSectionEntryBytes;
    for (uint32_t i = data_from; i < data_to; i++) out[at++] = file[i];
    poke_u16(out, 6, uint16_t(count - 1));
    poke_u32(out, 8, at - kHeaderBytes);
    refresh_crc(out);
    return at;
}

void fill_sentinel(uint8_t *buffer, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) buffer[i] = kSentinel;
}

// A float from its bits, for the two values a float can hold and a file cannot.
float float_from_bits(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- test material --------------------------------------------------------

// Settings with every field different from its default and from its neighbours, so that a round
// trip that crossed two fields over shows up.
ProjectSettings busy_settings() {
    ProjectSettings s;
    s.tempo_centi_bpm = 14537;
    s.root = 65;
    s.scale = 4;
    s.quantize = 72;
    s.live_mutes = 0x3C;
    s.scene_a = 4;
    s.scene_b = 11;
    for (uint32_t i = 0; i < sizeof(s.mute_states); i++) s.mute_states[i] = uint8_t(0x11 * (i + 1));
    s.sample_budget_mib = 48;
    s.chain_length = 9;
    s.chain_enabled = true;
    // All 14 mixer bytes different from each other and from their defaults, and all inside the
    // encoder's 0-127, so that a round trip that mixed two of them up shows as a value.
    for (int i = 0; i < kSendFields; i++) s.sends[i] = uint8_t((3 + i * 11) % 127);
    for (int i = 0; i < kMasterFields; i++) s.master[i] = uint8_t(120 - i * 7);
    s.eq_kills = 0x5;      // LOW and HIGH switched, MID not
    return s;
}

// A chain whose entries are all usable and all different.
void fill_chain(Project &project) {
    for (int i = 0; i < kChainEntries; i++) {
        project.chain[i].bank = uint8_t(i % kBanks);
        project.chain[i].slot = uint8_t((i * 5) % kPatternsPerBank);
        project.chain[i].repeats = uint8_t(1 + i % 64);
    }
}

// A kit with every track different: parameters, machine, fader and a sample path of its own.
void fill_kit(Kit &kit) {
    for (int t = 0; t < kTracks; t++) {
        KitTrack &track = kit.tracks[t];
        for (int p = 0; p < kKitParams; p++) {
            track.params[p] = uint8_t((t * 16 + p * 7) % 128);
        }
        track.machine = uint8_t(t % kMachines);
        // Thousandths, so the fader survives the format exactly: project.cpp stores it as
        // milli-units and `the_fader_is_stored_in_thousandths` pins what happens in between.
        track.level = float(125 * t) / 1000.0f;
        const char *names[kTracks] = {
            "/SAMPLES/Kick 01.wav", "/SAMPLES/SNARE.WAV",  "/SAMPLES/hat.wav",
            "",                     "/SAMPLES/a/b/c/d.wav", "/SAMPLES/Perc 09.wav",
            "/x.wav",               "/SAMPLES/VERY LONG DIRECTORY NAME/take 12 - final.wav",
        };
        std::memset(track.sample, 0, sizeof(track.sample));
        const char *name = names[t];
        for (int i = 0; name[i] != '\0' && i < kSamplePathBytes - 1; i++) track.sample[i] = name[i];
    }
}

// Every field of every step set to something different from its neighbours, and a lock pool
// fragmented on purpose so the save has to compact it. `salt` makes each of a bank's 16 patterns
// different from the other 15.
void make_busy_pattern(Pattern &pattern, int salt) {
    pattern_init(pattern);

    pattern.master_length = uint8_t(1 + (salt * 3) % kStepsPerTrack);
    pattern.swing = uint8_t(kSwingMin + salt % (kSwingMax - kSwingMin + 1));
    pattern.root = uint8_t(salt % 12);
    pattern.scale = uint8_t(salt % 7);
    pattern.seed = 0xDEADBEEFu + uint32_t(salt);

    const Speed speeds[] = {Speed::x2,   Speed::x1,   Speed::x3_4,
                            Speed::x1_2, Speed::x1_4, Speed::x1_8};

    for (int t = 0; t < kTracks; t++) {
        TrackPattern &track = pattern.tracks[t];
        track.length = uint8_t(1 + (t * 9 + salt) % kStepsPerTrack);
        track.speed = uint8_t(speeds[(t + salt) % 6]);
        track.swing = t == 0 ? kSwingInherit : uint8_t(kSwingMin + (t * 3 + salt) % 26);

        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &step = track.steps[i];
            step.flags = uint8_t((t * 5 + i + salt) & kStepFlagMask);
            step.note = uint8_t((24 + t * 11 + i * 3 + salt) & 0x7F);
            step.velocity = uint8_t(1 + (t * 17 + i * 5 + salt) % 127);
            step.length = uint8_t(1 + (t + i + salt) % kLengthMax);
            step.micro = int8_t(((i + salt) % 23) - 11);
            step.probability = uint8_t((t * 13 + i * 7 + salt) % 101);
            step.condition = ((t + i + salt) & 1) == 0
                                 ? condition_pack(Condition((t + i + salt) % kConditionKinds))
                                 : condition_pack_a_b(uint8_t(1 + i % 8), uint8_t(2 + t % 7));
            step.ratchet = ratchet_pack(uint8_t(1 + (t + i + salt) % 8), RatchetRamp((t + i) % 3));
            step.chord = uint8_t((t * 3 + i + salt) & 0x3F);
        }
    }

    for (uint8_t round = 0; round < 4; round++) {
        for (int t = 0; t < kTracks; t++) {
            for (int i = 0; i < 8; i++) {
                pattern_set_lock(pattern, t, i * 7, uint8_t(round * 2),
                                 uint8_t((t * 16 + i * 2 + round + salt) & 127));   // 0-127
            }
        }
    }
    for (int t = 0; t < kTracks; t++) pattern_clear_lock(pattern, t, 14, 2);
}

// Counts differing fields rather than reporting each one: 512 steps of a systematically broken
// field would otherwise bury the line that says which field it was.
struct Diff {
    int header = 0, track_header = 0;
    int flags = 0, note = 0, velocity = 0, length = 0, micro = 0, probability = 0;
    int condition = 0, ratchet = 0, chord = 0, lock_count = 0, lock_values = 0;
};

Diff diff_patterns(const Pattern &a, const Pattern &b) {
    Diff diff;
    if (a.master_length != b.master_length) diff.header++;
    if (a.swing != b.swing) diff.header++;
    if (a.root != b.root) diff.header++;
    if (a.scale != b.scale) diff.header++;
    if (a.seed != b.seed) diff.header++;

    for (int t = 0; t < kTracks; t++) {
        if (a.tracks[t].length != b.tracks[t].length) diff.track_header++;
        if (a.tracks[t].speed != b.tracks[t].speed) diff.track_header++;
        if (a.tracks[t].swing != b.tracks[t].swing) diff.track_header++;

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
    CHECK_EQ(diff.header, 0);
    CHECK_EQ(diff.track_header, 0);
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

// True when a pattern is the empty one `pattern_init` makes, which is what a refused slot holds.
bool is_empty_pattern(const Pattern &pattern) {
    pattern_init(g_scratch);
    const Diff diff = diff_patterns(pattern, g_scratch);
    return diff.header == 0 && diff.track_header == 0 && diff.flags == 0 && diff.note == 0 &&
           diff.velocity == 0 && diff.length == 0 && diff.micro == 0 && diff.probability == 0 &&
           diff.condition == 0 && diff.ratchet == 0 && diff.chord == 0 && diff.lock_count == 0 &&
           diff.lock_values == 0;
}

// A whole project worth writing: busy settings, a full chain, a kit, and 16 distinct patterns in
// bank `bank`.
void fill_project(Project &project, int bank) {
    project_init(project);
    project.settings = busy_settings();
    fill_chain(project);
    fill_kit(project.kit);
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        make_busy_pattern(project.patterns[bank][slot], slot + 1);
    }
    project.bank = uint8_t(bank);
    project.slot = 11;
}

}  // namespace

// --- sizes, names and indices ---------------------------------------------

TEST(project_the_file_sizes_are_what_the_format_says) {
    // project.cpp's header comment states both figures, and a change to either breaks every file
    // an older build wrote. They are pinned here rather than recomputed from the parts.
    CHECK_EQ(project_bytes(), kProjectFileBytes);
    CHECK_EQ(kit_bytes(), kKitFileBytes);

    // 16 patterns at their largest, plus the header and a one-entry table.
    CHECK_EQ(kBankMaxBytes, kHeaderBytes + kSectionEntryBytes +
                                uint32_t(kPatternsPerBank) * kPatternMaxBytes);
}

TEST(project_index_validity_covers_exactly_the_eight_banks_of_sixteen) {
    static_assert(project_index_valid(0, 0), "A01 exists");
    static_assert(project_index_valid(kBanks - 1, kPatternsPerBank - 1), "H16 exists");
    static_assert(!project_index_valid(-1, 0), "there is no bank before A");
    static_assert(!project_index_valid(0, -1), "there is no slot before 1");
    static_assert(!project_index_valid(kBanks, 0), "there is no bank after H");
    static_assert(!project_index_valid(0, kPatternsPerBank), "there is no 17th slot");

    CHECK_EQ(kProjectPatterns, kBanks * kPatternsPerBank);
    CHECK_EQ(kProjectPatterns, 128);

    int valid = 0;
    for (int bank = -2; bank < kBanks + 2; bank++) {
        for (int slot = -2; slot < kPatternsPerBank + 2; slot++) {
            if (project_index_valid(bank, slot)) valid++;
        }
    }
    CHECK_EQ(valid, kProjectPatterns);
}

TEST(project_bank_letters_run_a_to_h) {
    CHECK_EQ(int(bank_letter(0)), int('A'));
    CHECK_EQ(int(bank_letter(7)), int('H'));
    CHECK_EQ(int(bank_letter(-1)), int('?'));
    CHECK_EQ(int(bank_letter(kBanks)), int('?'));
    static_assert(bank_letter(3) == 'D', "bank_letter is usable at compile time");
}

TEST(project_bank_file_names_are_bank_x_dot_bin) {
    char name[16] = {};
    const char *expect[kBanks] = {"BANK_A.BIN", "BANK_B.BIN", "BANK_C.BIN", "BANK_D.BIN",
                                  "BANK_E.BIN", "BANK_F.BIN", "BANK_G.BIN", "BANK_H.BIN"};
    for (int bank = 0; bank < kBanks; bank++) {
        std::memset(name, kSentinel, sizeof(name));
        CHECK(bank_file_name(bank, name, int(sizeof(name))));
        CHECK_EQ(std::strcmp(name, expect[bank]), 0);
        // Terminated, and nothing written past the terminator.
        CHECK_EQ(int(name[10]), 0);
        CHECK_EQ(int(uint8_t(name[11])), int(kSentinel));
    }
}

TEST(project_bank_file_names_refuse_what_they_cannot_write) {
    char name[16] = {};
    CHECK(!bank_file_name(-1, name, int(sizeof(name))));
    CHECK(!bank_file_name(kBanks, name, int(sizeof(name))));
    CHECK(!bank_file_name(0, nullptr, 16));

    // 11 bytes is exactly enough: ten characters and the terminator.
    CHECK(bank_file_name(0, name, 11));
    CHECK_EQ(std::strcmp(name, "BANK_A.BIN"), 0);

    // One short, and the buffer is left alone rather than half filled.
    std::memset(name, kSentinel, sizeof(name));
    CHECK(!bank_file_name(0, name, 10));
    for (uint32_t i = 0; i < sizeof(name); i++) CHECK_EQ(int(uint8_t(name[i])), int(kSentinel));
}

// --- project_init ---------------------------------------------------------

TEST(project_init_empties_everything_and_lives_on_a01) {
    fill_project(g_source, 3);
    for (int bank = 0; bank < kBanks; bank++) g_source.bank_dirty[bank] = true;

    project_init(g_source);

    const ProjectSettings fresh;
    CHECK_EQ(int(g_source.settings.tempo_centi_bpm), int(fresh.tempo_centi_bpm));
    CHECK_EQ(int(g_source.settings.root), int(fresh.root));
    CHECK_EQ(int(g_source.settings.quantize), int(fresh.quantize));
    CHECK_EQ(int(g_source.settings.sample_budget_mib), int(fresh.sample_budget_mib));
    CHECK_EQ(int(g_source.settings.chain_length), 0);
    CHECK(!g_source.settings.chain_enabled);

    CHECK_EQ(int(g_source.bank), 0);
    CHECK_EQ(int(g_source.slot), 0);

    int chain_used = 0;
    for (int i = 0; i < kChainEntries; i++) {
        if (g_source.chain[i].repeats != 0) chain_used++;
    }
    CHECK_EQ(chain_used, 0);

    int kit_set = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            // TON's flat is 64, not 0 (D8): a blank synth track starts with its tone flat.
            const bool tone = p == kSlotTone && machine_has_voice_filter(g_source.kit.tracks[t].machine);
            if (g_source.kit.tracks[t].params[p] != (tone ? kToneFlat : 0)) kit_set++;
        }
        if (g_source.kit.tracks[t].sample[0] != '\0') kit_set++;
        // The fader's default is unity, not zero: a project that opened silent would look broken.
        CHECK_NEAR(g_source.kit.tracks[t].level, 1.0f, 0.0f);
    }
    CHECK_EQ(kit_set, 0);

    // The MACHINES are not zero, and that is the point. `KitTrack::machine` is honoured now --
    // a track hosts what its kit names -- so a kit of zeroes would be eight KICKs and a new
    // project would have no bass, no chord and no sampler until seven tracks were switched by
    // hand. A blank kit is docs/SPEC.md's default kit instead.
    const MachineId expected[kTracks] = {
        MachineId::kick,  MachineId::snare,   MachineId::hat,     MachineId::hat,
        MachineId::perc,  MachineId::va_mono, MachineId::va_poly, MachineId::sample,
    };
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_source.kit.tracks[t].machine), int(expected[t]));
    }

    int non_empty = 0, dirty = 0;
    for (int bank = 0; bank < kBanks; bank++) {
        if (g_source.bank_dirty[bank]) dirty++;
        for (int slot = 0; slot < kPatternsPerBank; slot++) {
            if (!is_empty_pattern(g_source.patterns[bank][slot])) non_empty++;
        }
    }
    CHECK_EQ(non_empty, 0);
    CHECK_EQ(dirty, 0);

    // `live()` is the pattern inside the project, not a copy of it -- the decision project.hpp
    // records. Writing through it must be visible in the array.
    g_source.live().master_length = 17;
    CHECK_EQ(int(g_source.patterns[0][0].master_length), 17);
    g_source.bank = 2;
    g_source.slot = 5;
    g_source.live().master_length = 19;
    CHECK_EQ(int(g_source.patterns[2][5].master_length), 19);
    CHECK_EQ(int(g_source.patterns[0][0].master_length), 17);
}

// --- PROJECT.BIN ----------------------------------------------------------

TEST(project_round_trips_every_settings_field) {
    fill_project(g_source, 2);

    fill_sentinel(g_project, uint32_t(sizeof(g_project)));
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, kProjectFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kProjectFileBytes);
    // Nothing past the file.
    for (uint32_t i = kProjectFileBytes; i < sizeof(g_project); i++) {
        CHECK_EQ(int(g_project[i]), int(kSentinel));
    }

    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));

    const ProjectSettings &a = g_source.settings;
    const ProjectSettings &b = g_loaded.settings;
    CHECK_EQ(int(b.tempo_centi_bpm), int(a.tempo_centi_bpm));
    CHECK_EQ(int(b.root), int(a.root));
    CHECK_EQ(int(b.scale), int(a.scale));
    CHECK_EQ(int(b.quantize), int(a.quantize));
    CHECK_EQ(int(b.live_mutes), int(a.live_mutes));
    CHECK_EQ(int(b.scene_a), int(a.scene_a));
    CHECK_EQ(int(b.scene_b), int(a.scene_b));
    for (uint32_t i = 0; i < sizeof(a.mute_states); i++) {
        CHECK_EQ(int(b.mute_states[i]), int(a.mute_states[i]));
    }
    CHECK_EQ(int(b.sample_budget_mib), int(a.sample_budget_mib));
    CHECK_EQ(int(b.chain_length), int(a.chain_length));
    CHECK_EQ(b.chain_enabled, a.chain_enabled);

    // `scale` is the one field the reader does not clamp, so a project naming a scale this build
    // does not have still round trips. Checked by value here and by range below.
    CHECK_EQ(int(b.scale), 4);

    // The mixer section. These fourteen bytes are the sends and the master, and before the
    // section existed they were in NO file: every delay and reverb setting, and the master DJ
    // filter, came back at its compile-time default however the project had been left.
    for (int i = 0; i < kSendFields; i++) CHECK_EQ(int(b.sends[i]), int(a.sends[i]));
    for (int i = 0; i < kMasterFields; i++) CHECK_EQ(int(b.master[i]), int(a.master[i]));
    CHECK_EQ(int(b.sends[0]), 3);
    CHECK_EQ(int(b.master[kMasterFieldsV1 - 1]), 99);
    // The kill EQ's bands and switches, which grew the section.
    CHECK_EQ(int(b.master[kMasterFields - 1]), 78);
    // The switches are live only (Roland, 2026-11-22): set in RAM, every band is back in the file.
    CHECK_EQ(int(a.eq_kills), 0x5);
    CHECK_EQ(int(b.eq_kills), 0);
}

// A mixer byte past the encoder's range is clamped, not taken. dsp/buses.cpp maps a byte with
// `byte * (1/127)`, so a 255 that got through would put every linear field at twice the top of
// its range -- a delay level of 2.0, a feedback of 1.9 -- and a feedback above 1 is a delay that
// grows without bound. The clamp is in the READER, which is the side that cannot trust its bytes.
TEST(project_clamps_a_mixer_byte_past_the_encoder_range) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));

    // Index 1 of the sends is `SendField::delay_feedback` and index 3 of the master is
    // `MasterField::level`. Written as numbers because this is a core/ test and dsp/buses.hpp,
    // which owns those enums, reaches Enaga -- core/ never does (CLAUDE.md).
    const uint32_t mixer = section_at(g_project, kSectionProjectMixer);
    g_project[mixer + 1] = 255;
    g_project[mixer + uint32_t(kSendFieldsV1) + 3] = 200;
    refresh_crc(g_project);

    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.sends[1]), 127);
    CHECK_EQ(int(g_loaded.settings.master[3]), 127);
    // And the fields next to them are untouched, so the clamp is per byte and not a wipe.
    CHECK_EQ(int(g_loaded.settings.sends[0]), int(g_source.settings.sends[0]));
}

// The mixer section is OPTIONAL on read, which is what lets the format grow without a version
// bump. This builds the two-section file an earlier build wrote -- by rewriting the header's
// section count and payload length over a real file -- and checks it loads with the defaults.
TEST(project_a_file_from_before_the_mixer_section_still_loads) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));

    // Drop the third table entry by moving the second section's bytes back over it, which is
    // exactly the file a two-section writer produces: table, settings, chain, and no more.
    const uint32_t old_table = 2 * kSectionEntryBytes;
    const uint32_t old_settings = old_table;
    const uint32_t old_chain = old_settings + 19;
    const uint32_t old_payload = old_chain + uint32_t(kChainEntries) * 3;

    uint8_t rebuilt[kProjectFileBytes + kSlack];
    fill_sentinel(rebuilt, uint32_t(sizeof(rebuilt)));
    for (uint32_t i = 0; i < kHeaderBytes; i++) rebuilt[i] = g_project[i];

    // Two entries: {id, reserved, offset, size}.
    poke_u16(rebuilt, kHeaderBytes + 0, kSectionProjectSettings);
    poke_u16(rebuilt, kHeaderBytes + 2, 0);
    poke_u32(rebuilt, kHeaderBytes + 4, old_settings);
    poke_u32(rebuilt, kHeaderBytes + 8, 19);
    poke_u16(rebuilt, kHeaderBytes + 12, kSectionProjectChain);
    poke_u16(rebuilt, kHeaderBytes + 14, 0);
    poke_u32(rebuilt, kHeaderBytes + 16, old_chain);
    poke_u32(rebuilt, kHeaderBytes + 20, uint32_t(kChainEntries) * 3);

    // The settings and the chain, copied out of the three-section file.
    const uint32_t from_settings = section_at(g_project, kSectionProjectSettings);
    const uint32_t from_chain = section_at(g_project, kSectionProjectChain);
    for (uint32_t i = 0; i < 19; i++) {
        rebuilt[kHeaderBytes + old_settings + i] = g_project[from_settings + i];
    }
    for (uint32_t i = 0; i < uint32_t(kChainEntries) * 3; i++) {
        rebuilt[kHeaderBytes + old_chain + i] = g_project[from_chain + i];
    }

    poke_u16(rebuilt, 6, 2);                     // two sections
    poke_u32(rebuilt, 8, old_payload);
    refresh_crc(rebuilt);

    project_init(g_loaded);
    g_loaded.settings.sends[0] = 99;             // so "the defaults" is a claim and not a tie
    g_loaded.settings.live_mutes = 0x55;
    g_loaded.settings.scene_a = 9;
    CHECK_EQ(int(project_read(g_loaded, rebuilt, kHeaderBytes + old_payload)),
             int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.live_mutes), 0);
    CHECK_EQ(int(g_loaded.settings.scene_a), 0);
    CHECK_EQ(int(g_loaded.settings.scene_b), 1);

    // The two sections that are there arrived...
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(g_source.settings.tempo_centi_bpm));
    CHECK_EQ(int(g_loaded.settings.chain_length), int(g_source.settings.chain_length));
    // ...and the one that is not left the DEFAULTS, not the 99 and not the source's bytes.
    const ProjectSettings fresh;
    for (int i = 0; i < kSendFields; i++) CHECK_EQ(int(g_loaded.settings.sends[i]), int(fresh.sends[i]));
    for (int i = 0; i < kMasterFields; i++) {
        CHECK_EQ(int(g_loaded.settings.master[i]), int(fresh.master[i]));
    }
}

TEST(project_the_live_mutes_and_scenes_round_trip) {
    fill_project(g_source, 0);
    g_source.settings.live_mutes = 0xA5;
    g_source.settings.scene_a = 7;
    g_source.settings.scene_b = 12;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.live_mutes), 0xA5);
    CHECK_EQ(int(g_loaded.settings.scene_a), 7);
    CHECK_EQ(int(g_loaded.settings.scene_b), 12);
}

TEST(project_a_file_from_before_the_live_section_opens_unmuted_on_scenes_1_and_2) {
    // A three-section file is the same bytes with no live entry the reader asks for: renaming
    // the entry to an id nobody reads gives exactly that, without rebuilding the table.
    fill_project(g_source, 0);
    g_source.settings.live_mutes = 0xFF;
    g_source.settings.scene_a = 5;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    for (uint32_t e = 0; e < 4; e++) {
        const uint32_t at = kHeaderBytes + e * kSectionEntryBytes;
        if (peek_u16(g_project, at) == kSectionProjectLive) poke_u16(g_project, at, 99);
    }
    refresh_crc(g_project);
    project_init(g_loaded);
    g_loaded.settings.live_mutes = 0x0F;          // a previous project's, which must not survive
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.live_mutes), 0);
    CHECK_EQ(int(g_loaded.settings.scene_a), 0);
    CHECK_EQ(int(g_loaded.settings.scene_b), 1);
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(g_source.settings.tempo_centi_bpm));
}

TEST(project_the_midi_out_channels_round_trip) {
    fill_project(g_source, 0);
    for (int t = 0; t < kTracks; t++) g_source.settings.midi_out[t] = uint8_t(t * 2 + 1);
    g_source.settings.midi_out[3] = kMidiOutOff;
    g_source.settings.midi_out[7] = 16;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_loaded.settings.midi_out[t]), int(g_source.settings.midi_out[t]));
    }
}

TEST(project_a_file_from_before_the_midi_section_opens_with_midi_out_off) {
    fill_project(g_source, 0);
    for (int t = 0; t < kTracks; t++) g_source.settings.midi_out[t] = 9;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    for (uint32_t e = 0; e < 6; e++) {
        const uint32_t at = kHeaderBytes + e * kSectionEntryBytes;
        if (peek_u16(g_project, at) == kSectionProjectMidi) poke_u16(g_project, at, 99);
    }
    refresh_crc(g_project);
    project_init(g_loaded);
    for (int t = 0; t < kTracks; t++) g_loaded.settings.midi_out[t] = 4;   // a previous project's
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_loaded.settings.midi_out[t]), 0);
    CHECK_EQ(int(g_loaded.settings.live_mutes), int(g_source.settings.live_mutes));
}

TEST(project_a_damaged_midi_out_channel_turns_that_track_off) {
    fill_project(g_source, 0);
    for (int t = 0; t < kTracks; t++) g_source.settings.midi_out[t] = 2;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    const uint32_t midi = section_at(g_project, kSectionProjectMidi);
    g_project[midi + 1] = 17;        // one past the last channel
    g_project[midi + 5] = 0xFF;
    refresh_crc(g_project);
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.midi_out[0]), 2);
    CHECK_EQ(int(g_loaded.settings.midi_out[1]), 0);
    CHECK_EQ(int(g_loaded.settings.midi_out[5]), 0);
    CHECK_EQ(int(g_loaded.settings.midi_out[6]), 2);
}

TEST(project_refuses_a_midi_section_that_is_short) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    poke_u32(g_project, section_entry_at(g_project, kSectionProjectMidi) + 8, uint32_t(kTracks - 1));
    refresh_crc(g_project);
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::malformed));
}

// Present but short is damage, not an old file: a writer that declared the section declared its
// size too, so a reader that accepted a truncated one would read the chain's bytes as a mixer.
TEST(project_song_mode_and_the_mute_recalls_round_trip) {
    fill_project(g_source, 0);
    g_source.settings.chain_song = true;
    for (int i = 0; i < kChainEntries; i++) g_source.chain[i].mutes = uint8_t(i % 9);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK(g_loaded.settings.chain_song);
    for (int i = 0; i < kChainEntries; i++) {
        CHECK_EQ(int(g_loaded.chain[i].mutes), i % 9);
    }

    // A recall past the eight states is dropped, and so is one on an unused entry.
    const uint32_t song = section_at(g_project, kSectionProjectSong);
    g_project[song + 1 + 1] = 200;
    g_source.chain[5].repeats = 0;
    CHECK_EQ(int(project_write(g_source, g_project_alt, uint32_t(sizeof(g_project_alt)), &written)),
             int(SerializeResult::ok));
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.chain[1].mutes), 0);
    CHECK_EQ(int(project_read(g_loaded, g_project_alt, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.chain[5].mutes), 0);
    CHECK_EQ(int(g_loaded.chain[6].mutes), 6);
}

TEST(project_a_file_from_before_song_mode_loops_and_recalls_no_mutes) {
    fill_project(g_source, 0);
    g_source.settings.chain_song = true;
    for (int i = 0; i < kChainEntries; i++) g_source.chain[i].mutes = 3;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    // The MIDI section came after song mode, so a file from before song mode had neither.
    uint32_t old_size = drop_last_section(g_project, g_project_alt);
    old_size = drop_last_section(g_project_alt, g_project_alt);
    CHECK_EQ(old_size, kProjectFileBytes - 12 - 8 - 12 - 65);
    // A previous project's song settings, which must not survive into this one.
    project_init(g_loaded);
    g_loaded.settings.chain_song = true;
    for (int i = 0; i < kChainEntries; i++) g_loaded.chain[i].mutes = 5;
    CHECK_EQ(int(project_read(g_loaded, g_project_alt, old_size)), int(SerializeResult::ok));
    CHECK(!g_loaded.settings.chain_song);
    for (int i = 0; i < kChainEntries; i++) CHECK_EQ(int(g_loaded.chain[i].mutes), 0);
    // Everything else came through.
    CHECK_EQ(int(g_loaded.settings.live_mutes), int(g_source.settings.live_mutes));
    CHECK_EQ(int(g_loaded.chain[3].repeats), int(g_source.chain[3].repeats));

    // Present but short is damage.
    poke_u32(g_project, section_entry_at(g_project, kSectionProjectSong) + 8, 10);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::malformed));
}

// Present but short is damage, not an old file: a writer that declared the section declared its
// size too, so a reader that accepted a truncated one would read the chain's bytes as a mixer.
TEST(project_refuses_a_mixer_section_that_is_short) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));

    // The third table entry's size field: {id, reserved, offset, size} at 24 bytes in.
    poke_u32(g_project, kHeaderBytes + 2 * kSectionEntryBytes + 8, uint32_t(kSendFieldsV1));
    refresh_crc(g_project);

    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::malformed));
}

// The kill EQ GREW the mixer section rather than adding one: a file from before it has a 14-byte
// mixer section (sends and the first four master bytes) and must still load, with the bands flat
// and the switches off -- set to the defaults, not left as whatever the buffer held.
TEST(project_a_mixer_section_from_before_the_kill_eq_loads_flat) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));

    // The third table entry's size field, cut to what the old writer declared. The bytes after
    // it stay in the file, as junk an old reader never asked for.
    poke_u32(g_project, kHeaderBytes + 2 * kSectionEntryBytes + 8,
             uint32_t(kSendFieldsV1 + kMasterFieldsV1));
    refresh_crc(g_project);

    project_init(g_loaded);
    g_loaded.settings.master[int(kMasterFieldsV1)] = 3;
    g_loaded.settings.eq_kills = 0x7;
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    for (int i = 0; i < kSendFieldsV1; i++) {
        CHECK_EQ(int(g_loaded.settings.sends[i]), int(g_source.settings.sends[i]));
    }
    // Older than the ambient modes too: both sends classic, their knobs at the defaults.
    const ProjectSettings defaults;
    for (int i = kSendFieldsV1; i < kSendFields; i++) {
        CHECK_EQ(int(g_loaded.settings.sends[i]), int(defaults.sends[i]));
    }
    for (int i = 0; i < kMasterFieldsV1; i++) {
        CHECK_EQ(int(g_loaded.settings.master[i]), int(g_source.settings.master[i]));
    }
    for (int i = kMasterFieldsV1; i < kMasterFields; i++) {
        CHECK_EQ(int(g_loaded.settings.master[i]), 64);
    }
    CHECK_EQ(int(g_loaded.settings.eq_kills), 0);
    // And the live section, found by id, is still read whole.
    CHECK_EQ(int(g_loaded.settings.live_mutes), int(g_source.settings.live_mutes));
}

// The ambient modes grew the mixer section again, by nine send bytes after the kill switches. A
// file from between the two (18 bytes: ten sends, seven master, the switches) keeps every byte it
// has and opens with both sends in their classic modes.
TEST(project_a_mixer_section_from_before_the_ambient_modes_loads_classic) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    poke_u32(g_project, kHeaderBytes + 2 * kSectionEntryBytes + 8,
             uint32_t(kSendFieldsV1 + kMasterFields + 1));
    refresh_crc(g_project);

    project_init(g_loaded);
    for (int i = kSendFieldsV1; i < kSendFields; i++) g_loaded.settings.sends[i] = 99;
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    for (int i = 0; i < kSendFieldsV1; i++) {
        CHECK_EQ(int(g_loaded.settings.sends[i]), int(g_source.settings.sends[i]));
    }
    for (int i = 0; i < kMasterFields; i++) {
        CHECK_EQ(int(g_loaded.settings.master[i]), int(g_source.settings.master[i]));
    }
    CHECK_EQ(int(g_loaded.settings.eq_kills), 0);
    const ProjectSettings defaults;
    for (int i = kSendFieldsV1; i < kSendFields; i++) {
        CHECK_EQ(int(g_loaded.settings.sends[i]), int(defaults.sends[i]));
    }
    // Index 10 and 15 are the two mode switches (dsp/buses.hpp): classic is below 64.
    CHECK(defaults.sends[10] < 64);
    CHECK(defaults.sends[15] < 64);
}

// And the new bytes sit after the switches, where an older build never looks.
TEST(project_the_ambient_bytes_follow_the_kill_switches) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    const uint32_t mixer = section_at(g_project, kSectionProjectMixer);
    for (int i = 0; i < kSendFieldsV1; i++) {
        CHECK_EQ(int(g_project[mixer + uint32_t(i)]), int(g_source.settings.sends[i]));
    }
    CHECK_EQ(int(g_project[mixer + uint32_t(kSendFieldsV1 + kMasterFields)]), 0);
    for (int i = kSendFieldsV1; i < kSendFields; i++) {
        CHECK_EQ(int(g_project[mixer + uint32_t(kMasterFields + 1 + i)]),
                 int(g_source.settings.sends[i]));
    }
}

// A file written before the switches stopped being saved may still hold some: it opens with
// every band in, whatever the byte says.
TEST(project_ignores_a_saved_kill_switch_byte) {
    fill_project(g_source, 0);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));
    const uint32_t mixer = section_at(g_project, kSectionProjectMixer);
    g_project[mixer + uint32_t(kSendFieldsV1 + kMasterFields)] = 0x07;
    refresh_crc(g_project);
    project_init(g_loaded);
    g_loaded.settings.eq_kills = 0x5;                  // and never "left alone" from before
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.eq_kills), 0);
    // The bytes after it are still where they were.
    for (int i = kSendFieldsV1; i < kSendFields; i++) {
        CHECK_EQ(int(g_loaded.settings.sends[i]), int(g_source.settings.sends[i]));
    }
}

TEST(project_round_trips_the_live_pattern_and_the_whole_chain) {
    fill_project(g_source, 5);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), &written)),
             int(SerializeResult::ok));

    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));

    CHECK_EQ(int(g_loaded.bank), int(g_source.bank));
    CHECK_EQ(int(g_loaded.slot), int(g_source.slot));

    int wrong = 0;
    for (int i = 0; i < kChainEntries; i++) {
        if (g_loaded.chain[i].bank != g_source.chain[i].bank) wrong++;
        if (g_loaded.chain[i].slot != g_source.chain[i].slot) wrong++;
        if (g_loaded.chain[i].repeats != g_source.chain[i].repeats) wrong++;
    }
    CHECK_EQ(wrong, 0);
}

TEST(project_write_refuses_a_buffer_one_byte_short_and_touches_nothing) {
    fill_project(g_source, 0);
    fill_sentinel(g_project, uint32_t(sizeof(g_project)));

    uint32_t written = 123;
    CHECK_EQ(int(project_write(g_source, g_project, kProjectFileBytes - 1, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);
    for (uint32_t i = 0; i < sizeof(g_project); i++) CHECK_EQ(int(g_project[i]), int(kSentinel));

    CHECK_EQ(int(project_write(g_source, nullptr, kProjectFileBytes, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);

    // Exactly enough is enough.
    CHECK_EQ(int(project_write(g_source, g_project, kProjectFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kProjectFileBytes);
}

TEST(project_the_header_says_strata_and_the_version) {
    fill_project(g_source, 0);
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), nullptr)),
             int(SerializeResult::ok));

    CHECK_EQ(int(g_project[0]), int(kMagic0));
    CHECK_EQ(int(g_project[1]), int(kMagic1));
    CHECK_EQ(int(g_project[2]), int(kMagic2));
    CHECK_EQ(int(g_project[3]), int(kMagic3));
    CHECK_EQ(int(peek_u16(g_project, kVersionOffset)), int(kProjectVersion));
    CHECK_EQ(int(peek_u16(g_project, 6)), 6);   // settings, chain, mixer, live, song, MIDI
    CHECK_EQ(peek_u32(g_project, 8), kProjectFileBytes - kHeaderBytes);
    CHECK_EQ(peek_u32(g_project, kCrcOffset),
             crc32(g_project + kHeaderBytes, kProjectFileBytes - kHeaderBytes));

    // All five sections are there, in order, and clear of the table.
    const uint32_t settings = section_at(g_project, kSectionProjectSettings);
    const uint32_t chain = section_at(g_project, kSectionProjectChain);
    const uint32_t mixer = section_at(g_project, kSectionProjectMixer);
    const uint32_t live = section_at(g_project, kSectionProjectLive);
    const uint32_t song = section_at(g_project, kSectionProjectSong);
    CHECK_EQ(settings, kHeaderBytes + 6 * kSectionEntryBytes);
    CHECK_EQ(chain, settings + 19);
    CHECK_EQ(mixer, chain + 64 * 3);
    CHECK_EQ(live, mixer + uint32_t(kSendFields + kMasterFields + 1));
    const uint32_t midi = section_at(g_project, kSectionProjectMidi);
    CHECK_EQ(song, live + 3);
    CHECK_EQ(midi, song + 1 + 64);
    CHECK_EQ(midi + 8, kProjectFileBytes);
}

// --- PROJECT.BIN: value-tolerant ------------------------------------------
// Every clamp is driven by poking the bytes and repairing the CRC. Writing an out-of-range value
// into a Project and saving it would prove nothing: project_write is documented NOT to clamp,
// because a writer that clamped would make a save and a load disagree about what the project is.

namespace {

// Writes g_source to g_project, then hands back the absolute offset of the settings section.
uint32_t fresh_project_file() {
    fill_project(g_source, 0);
    const SerializeResult res =
        project_write(g_source, g_project, uint32_t(sizeof(g_project)), nullptr);
    CHECK_EQ(int(res), int(SerializeResult::ok));
    return section_at(g_project, kSectionProjectSettings);
}

}  // namespace

TEST(project_an_impossible_tempo_is_clamped_into_enagas_range) {
    const uint32_t at = fresh_project_file();

    poke_u16(g_project, at + 0, 0);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(kTempoCentiMin));

    poke_u16(g_project, at + 0, 65535);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(kTempoCentiMax));

    // The two ends of the range survive untouched, so the clamp is not off by one.
    poke_u16(g_project, at + 0, kTempoCentiMin);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(kTempoCentiMin));

    poke_u16(g_project, at + 0, kTempoCentiMax);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(kTempoCentiMax));
}

TEST(project_the_root_note_and_the_quantize_amount_are_clamped) {
    const uint32_t at = fresh_project_file();

    g_project[at + 2] = 255;     // root
    g_project[at + 5] = 255;     // quantize
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.root), 127);
    CHECK_EQ(int(g_loaded.settings.quantize), 100);

    g_project[at + 2] = 127;
    g_project[at + 5] = 100;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.root), 127);
    CHECK_EQ(int(g_loaded.settings.quantize), 100);
}

TEST(project_the_old_swing_byte_is_written_as_50_and_ignored_on_read) {
    // The project swing is gone (nothing read it); its byte stays so old files still line up.
    const uint32_t at = fresh_project_file();
    CHECK_EQ(int(g_project[at + 4]), int(kSwingMin));

    for (int value = 0; value < 256; value++) {
        g_project[at + 4] = uint8_t(value);
        refresh_crc(g_project);
        CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
                 int(SerializeResult::ok));
        if (g_loaded.settings.quantize != g_source.settings.quantize) {
            CHECK_EQ(int(g_loaded.settings.quantize), int(g_source.settings.quantize));
        }
    }
}

TEST(project_the_sample_budget_is_clamped_to_one_through_sixtyfour_mib) {
    const uint32_t at = fresh_project_file();

    // Zero is a damaged byte, not a setting: no UI offers it, and a project that silently
    // refused every sample load is the least diagnosable failure there is.
    g_project[at + 14] = 0;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.sample_budget_mib), 1);

    g_project[at + 14] = 255;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.sample_budget_mib), 64);

    // docs/SPEC.md's default and ceiling both survive.
    for (int value : {1, 32, 64}) {
        g_project[at + 14] = uint8_t(value);
        refresh_crc(g_project);
        CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
                 int(SerializeResult::ok));
        CHECK_EQ(int(g_loaded.settings.sample_budget_mib), value);
    }
}

TEST(project_an_unknown_scale_survives_the_round_trip) {
    const uint32_t at = fresh_project_file();

    // Not clamped, for core/pattern.hpp's reason about a lock's param: core/ does not know how
    // many scales there are, and the UI draws an unknown id as the chromatic default rather than
    // the file losing it.
    for (int value : {0, 1, 42, 200, 255}) {
        g_project[at + 3] = uint8_t(value);
        refresh_crc(g_project);
        CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
                 int(SerializeResult::ok));
        CHECK_EQ(int(g_loaded.settings.scale), value);
    }
}

TEST(project_every_bit_pattern_is_a_legal_set_of_mute_states) {
    const uint32_t at = fresh_project_file();

    for (uint32_t i = 0; i < 8; i++) g_project[at + 6 + i] = 0xFF;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    for (uint32_t i = 0; i < 8; i++) CHECK_EQ(int(g_loaded.settings.mute_states[i]), 0xFF);

    for (uint32_t i = 0; i < 8; i++) g_project[at + 6 + i] = uint8_t(i * 37);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    for (uint32_t i = 0; i < 8; i++) {
        CHECK_EQ(int(g_loaded.settings.mute_states[i]), int(uint8_t(i * 37)));
    }
}

TEST(project_the_chain_length_is_clamped_and_any_nonzero_enables_it) {
    const uint32_t at = fresh_project_file();

    g_project[at + 15] = 255;
    g_project[at + 16] = 7;      // not 1, and still "on"
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.chain_length), kChainEntries);
    CHECK(g_loaded.settings.chain_enabled);

    g_project[at + 15] = 0;
    g_project[at + 16] = 0;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.settings.chain_length), 0);
    CHECK(!g_loaded.settings.chain_enabled);
}

TEST(project_a_live_pattern_that_cannot_exist_falls_back_to_a01) {
    const uint32_t at = fresh_project_file();

    g_project[at + 17] = uint8_t(kBanks);          // one past H
    g_project[at + 18] = 3;
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.bank), 0);
    CHECK_EQ(int(g_loaded.slot), 0);

    g_project[at + 17] = 2;
    g_project[at + 18] = uint8_t(kPatternsPerBank);   // one past 16
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.bank), 0);
    CHECK_EQ(int(g_loaded.slot), 0);

    // H16 is the last one that does exist, and it survives.
    g_project[at + 17] = uint8_t(kBanks - 1);
    g_project[at + 18] = uint8_t(kPatternsPerBank - 1);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded.bank), kBanks - 1);
    CHECK_EQ(int(g_loaded.slot), kPatternsPerBank - 1);
}

TEST(project_a_chain_entry_naming_no_pattern_is_dropped_whole) {
    fresh_project_file();
    const uint32_t chain = section_at(g_project, kSectionProjectChain);

    // Entry 4 names bank 9: dropped to the zeroed entry, not clamped to A01. core/queue.hpp
    // stops at the first entry with no repeats, so a zeroed entry ends the chain where the
    // damage is, while a clamp would quietly play a pattern nobody put there.
    g_project[chain + 4 * 3 + 0] = 9;
    g_project[chain + 4 * 3 + 1] = 3;
    g_project[chain + 4 * 3 + 2] = 5;

    // Entry 6 names slot 16.
    g_project[chain + 6 * 3 + 0] = 1;
    g_project[chain + 6 * 3 + 1] = uint8_t(kPatternsPerBank);
    g_project[chain + 6 * 3 + 2] = 2;

    // Entry 8 is a real pattern with no repeats, which means unused: canonicalised to zeroes
    // rather than left naming C04.
    g_project[chain + 8 * 3 + 0] = 2;
    g_project[chain + 8 * 3 + 1] = 3;
    g_project[chain + 8 * 3 + 2] = 0;

    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));

    for (int i : {4, 6, 8}) {
        CHECK_EQ(int(g_loaded.chain[i].bank), 0);
        CHECK_EQ(int(g_loaded.chain[i].slot), 0);
        CHECK_EQ(int(g_loaded.chain[i].repeats), 0);
    }
    // Its neighbours are untouched: one damaged entry costs one entry.
    CHECK_EQ(int(g_loaded.chain[3].repeats), int(g_source.chain[3].repeats));
    CHECK_EQ(int(g_loaded.chain[5].repeats), int(g_source.chain[5].repeats));
    CHECK_EQ(int(g_loaded.chain[7].repeats), int(g_source.chain[7].repeats));
    CHECK_EQ(int(g_loaded.chain[9].repeats), int(g_source.chain[9].repeats));
}

// --- PROJECT.BIN: structure-strict ----------------------------------------

TEST(project_read_refuses_a_damaged_container) {
    fresh_project_file();
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)), int(SerializeResult::ok));

    // Not a STRATA file.
    fresh_project_file();
    g_project[0] = 'X';
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::bad_magic));

    // A version this build does not know, in both directions.
    fresh_project_file();
    poke_u16(g_project, kVersionOffset, uint16_t(kProjectVersion + 1));
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::bad_version));
    fresh_project_file();
    poke_u16(g_project, kVersionOffset, 0);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::bad_version));

    // A payload byte that no longer matches its CRC.
    fresh_project_file();
    g_project[kHeaderBytes + 30] = uint8_t(g_project[kHeaderBytes + 30] ^ 0x01);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::bad_crc));

    // A header claiming more payload than the file holds.
    fresh_project_file();
    poke_u32(g_project, 8, 0x00FFFFFFu);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::truncated));

    // A nonsensical section count, and an impossible one.
    fresh_project_file();
    poke_u16(g_project, 6, 0);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));
    fresh_project_file();
    poke_u16(g_project, 6, uint16_t(kMaxSections + 1));
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // Nothing at all, and less than a header.
    CHECK_EQ(int(project_read(g_loaded, nullptr, 0)), int(SerializeResult::truncated));
    fresh_project_file();
    CHECK_EQ(int(project_read(g_loaded, g_project, kHeaderBytes - 1)),
             int(SerializeResult::truncated));
}

TEST(project_read_refuses_a_section_table_that_does_not_add_up) {
    // A section whose bytes would start inside the table.
    fresh_project_file();
    uint32_t entry = section_entry_at(g_project, kSectionProjectChain);
    poke_u32(g_project, entry + 4, 4);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // A section running past the payload.
    fresh_project_file();
    entry = section_entry_at(g_project, kSectionProjectChain);
    poke_u32(g_project, entry + 8, 0x00FFFFFFu);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // The same id twice, and therefore a section missing.
    fresh_project_file();
    entry = section_entry_at(g_project, kSectionProjectChain);
    poke_u16(g_project, entry, kSectionProjectSettings);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // A section this file type does not use, in place of one it needs.
    fresh_project_file();
    entry = section_entry_at(g_project, kSectionProjectSettings);
    poke_u16(g_project, entry, 99);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // A settings section too short to hold the 19 bytes the format says it has.
    fresh_project_file();
    entry = section_entry_at(g_project, kSectionProjectSettings);
    poke_u32(g_project, entry + 8, 18);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));

    // And a chain section one entry short.
    fresh_project_file();
    entry = section_entry_at(g_project, kSectionProjectChain);
    poke_u32(g_project, entry + 8, uint32_t(kChainEntries) * 3 - 1);
    refresh_crc(g_project);
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::malformed));
}

TEST(project_a_refused_load_leaves_defaults_but_never_touches_a_pattern) {
    // The reason project_read is narrow: app/ reads PROJECT.BIN first, so a damaged 251 bytes
    // must not cost 128 patterns that are about to load perfectly well.
    fill_project(g_loaded, 4);
    for (int bank = 0; bank < kBanks; bank++) g_loaded.bank_dirty[bank] = true;
    const uint8_t keep_length = g_loaded.patterns[4][7].master_length;

    fresh_project_file();
    g_project[0] = 'X';
    CHECK_EQ(int(project_read(g_loaded, g_project, kProjectFileBytes)),
             int(SerializeResult::bad_magic));

    // What PROJECT.BIN owns is back at its defaults, not half read.
    const ProjectSettings fresh;
    CHECK_EQ(int(g_loaded.settings.tempo_centi_bpm), int(fresh.tempo_centi_bpm));
    CHECK_EQ(int(g_loaded.settings.sample_budget_mib), int(fresh.sample_budget_mib));
    CHECK_EQ(int(g_loaded.settings.chain_length), 0);
    CHECK(!g_loaded.settings.chain_enabled);
    CHECK_EQ(int(g_loaded.bank), 0);
    CHECK_EQ(int(g_loaded.slot), 0);
    int chain_used = 0;
    for (int i = 0; i < kChainEntries; i++) {
        if (g_loaded.chain[i].repeats != 0) chain_used++;
    }
    CHECK_EQ(chain_used, 0);

    // The patterns are untouched, and so is the kit, which KIT.BIN owns.
    CHECK_EQ(int(g_loaded.patterns[4][7].master_length), int(keep_length));
    CHECK(!is_empty_pattern(g_loaded.patterns[4][7]));
    CHECK(g_loaded.kit.tracks[1].sample[0] != '\0');

    // `bank_dirty` says whether a bank differs from its file, and this function has not touched
    // a pattern, so it is left alone.
    int dirty = 0;
    for (int bank = 0; bank < kBanks; bank++) {
        if (g_loaded.bank_dirty[bank]) dirty++;
    }
    CHECK_EQ(dirty, kBanks);
}

TEST(project_read_never_looks_past_the_bytes_it_was_given) {
    // The property that "never cast file bytes to structs" buys, and the only way to check it
    // from outside: every truncation is read twice over different trailing bytes and must give
    // the same answer both times.
    fill_project(g_source, 1);
    CHECK_EQ(int(project_write(g_source, g_project, uint32_t(sizeof(g_project)), nullptr)),
             int(SerializeResult::ok));
    std::memcpy(g_project_alt, g_project, kProjectFileBytes);
    for (uint32_t i = kProjectFileBytes; i < sizeof(g_project); i++) {
        g_project[i] = 0x00;
        g_project_alt[i] = 0xFF;
    }

    for (uint32_t size = 0; size <= kProjectFileBytes; size++) {
        const int a = int(project_read(g_loaded, g_project, size));
        const int b = int(project_read(g_loaded, g_project_alt, size));
        if (a != b) CHECK_EQ(a, b);
        // And only the whole file is accepted.
        const bool ok = a == int(SerializeResult::ok);
        if (ok != (size == kProjectFileBytes)) CHECK_EQ(ok, size == kProjectFileBytes);
    }
}

// --- KIT.BIN --------------------------------------------------------------

namespace {

// One track's record inside the tracks section: 16 parameters, the machine, the fader, the path.
constexpr uint32_t kTrackRecordBytes = uint32_t(kKitSoundParams) + 1 + 2 + uint32_t(kSamplePathBytes);

uint32_t kit_track_at(const uint8_t *file, int track) {
    return section_at(file, kSectionKitTracks) + uint32_t(track) * kTrackRecordBytes;
}

uint32_t fresh_kit_file() {
    fill_kit(g_kit_source);
    const SerializeResult res =
        kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), nullptr);
    CHECK_EQ(int(res), int(SerializeResult::ok));
    return section_at(g_kit_file, kSectionKitTracks);
}

// g_kit_file as a build from before the duck wrote it: the OSC page's section and then the
// duck's, the last two, dropped.
// g_kit_file as a build from before the mod slots wrote it: their section, the last, dropped.
void drop_mod_section() {
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1329u);
    for (uint32_t i = 0; i < 1329u; i++) g_kit_file[i] = g_kit_alt[i];
}

// g_kit_file as a build from before LFO TRIG wrote it: its section and the mod slots' dropped.
void drop_lfo_trig_section() {
    drop_mod_section();
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1309u);
    for (uint32_t i = 0; i < 1309u; i++) g_kit_file[i] = g_kit_alt[i];
}

// g_kit_file as a build from before one filter on the synth machines (D8) wrote it: the filter
// layout's section, the last, dropped. Its VA and WAVE tracks then load mapped (migrated()).
void drop_filter_section() {
    drop_lfo_trig_section();
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1296u);
    for (uint32_t i = 0; i < 1296u; i++) g_kit_file[i] = g_kit_alt[i];
}

// What g_kit_source's parameter p of track t loads as from a file without the filter layout:
// itself, except slots 4-7 of a VA or WAVE track, which become FDE 0 and the nearest TON.
int migrated(int t, int p) {
    uint8_t params[kKitParams];
    for (int i = 0; i < kKitParams; i++) params[i] = g_kit_source.tracks[t].params[i];
    if (machine_has_voice_filter(g_kit_source.tracks[t].machine)) kit_track_to_voice_filter(params);
    return int(params[p]);
}

void drop_duck_section() {
    drop_filter_section();
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1252u);
    CHECK_EQ(drop_last_section(g_kit_alt, g_kit_file), 1228u);
}

// What a parameter loads as when its section is absent: 0, except the OSC page's, which loads
// the legacy voice (core/project.hpp, kKitOscLegacy).
int absent_param(int p) { return p >= kKitAsrEnd ? int(kKitOscLegacy[p - kKitAsrEnd]) : 0; }

// The fader as it went down on disk, in thousandths of unity.
uint16_t stored_level(const uint8_t *file, int track) {
    return peek_u16(file, kit_track_at(file, track) + kKitSoundParams + 1);
}

}  // namespace

TEST(kit_round_trips_every_track_field) {
    fill_kit(g_kit_source);

    fill_sentinel(g_kit_file, uint32_t(sizeof(g_kit_file)));
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kKitFileBytes);
    for (uint32_t i = kKitFileBytes; i < sizeof(g_kit_file); i++) {
        CHECK_EQ(int(g_kit_file[i]), int(kSentinel));
    }

    kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), &written);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, written)), int(SerializeResult::ok));

    int params_wrong = 0, machines_wrong = 0, levels_wrong = 0, paths_wrong = 0;
    for (int t = 0; t < kTracks; t++) {
        const KitTrack &a = g_kit_source.tracks[t];
        const KitTrack &b = g_kit_loaded.tracks[t];
        for (int p = 0; p < kKitParams; p++) {
            if (a.params[p] != b.params[p]) params_wrong++;
        }
        if (a.machine != b.machine) machines_wrong++;
        // Thousandths on disk, so a fader set in thousandths comes back exactly.
        if (!(a.level == b.level)) levels_wrong++;
        if (std::strcmp(a.sample, b.sample) != 0) paths_wrong++;
    }
    CHECK_EQ(params_wrong, 0);
    CHECK_EQ(machines_wrong, 0);
    CHECK_EQ(levels_wrong, 0);
    CHECK_EQ(paths_wrong, 0);

    // The empty path is a track that plays no file, and it stays empty rather than becoming
    // a one-character name.
    CHECK_EQ(int(g_kit_loaded.tracks[3].sample[0]), 0);
}

TEST(kit_write_refuses_a_buffer_one_byte_short_and_touches_nothing) {
    fill_kit(g_kit_source);
    fill_sentinel(g_kit_file, uint32_t(sizeof(g_kit_file)));

    uint32_t written = 99;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes - 1, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);
    for (uint32_t i = 0; i < sizeof(g_kit_file); i++) {
        CHECK_EQ(int(g_kit_file[i]), int(kSentinel));
    }

    CHECK_EQ(int(kit_write(g_kit_source, nullptr, kKitFileBytes, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);

    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kKitFileBytes);
    CHECK_EQ(int(peek_u16(g_kit_file, kVersionOffset)), int(kKitVersion));
    CHECK_EQ(int(peek_u16(g_kit_file, 6)), 10);    // the tracks, the LFO, the arp, SLC AUTO, ASR, the duck, OSC, the filter layout, LFO TRIG, the mod slots
}

TEST(kit_a_parameter_a_file_cannot_hold_is_clamped_to_127) {
    const uint32_t at = fresh_kit_file();

    // dsp/track.hpp maps a parameter either by x(1/127) or as a table index, so a 255 that
    // survived the CRC would be an out-of-range read down in a machine rather than an odd sound.
    for (int p = 0; p < kKitSoundParams; p++) g_kit_file[at + uint32_t(p)] = uint8_t(128 + p);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int p = 0; p < kKitSoundParams; p++) CHECK_EQ(int(g_kit_loaded.tracks[0].params[p]), 127);

    // 127 itself is not clamped away.
    for (int p = 0; p < kKitSoundParams; p++) g_kit_file[at + uint32_t(p)] = 127;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int p = 0; p < kKitSoundParams; p++) CHECK_EQ(int(g_kit_loaded.tracks[0].params[p]), 127);
}

TEST(kit_the_lfo_and_arp_bytes_round_trip_and_are_clamped_on_the_way_in) {
    // The LFO's four bytes a track (kSectionKitLfo) and the arp's four (kSectionKitArp), each in
    // a section of its own.
    fresh_kit_file();
    CHECK(section_at(g_kit_file, kSectionKitLfo) != 0u);
    CHECK(section_at(g_kit_file, kSectionKitArp) != 0u);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }
    // Laid out track by track, four bytes each, in parameter order.
    const uint32_t lfo = section_at(g_kit_file, kSectionKitLfo);
    CHECK_EQ(int(g_kit_file[lfo + 4 * 3 + 2]), int(g_kit_source.tracks[3].params[kKitSoundParams + 2]));
    const uint32_t arp = section_at(g_kit_file, kSectionKitArp);
    CHECK_EQ(int(g_kit_file[arp + 4 * 5 + 1]), int(g_kit_source.tracks[5].params[kKitLfoEnd + 1]));
    // The ASR page after SLC AUTO, laid out the same way.
    CHECK(section_at(g_kit_file, kSectionKitAsr) > section_at(g_kit_file, kSectionKitSlices));
    CHECK_EQ(int(g_kit_file[section_at(g_kit_file, kSectionKitAsr) + 4 * 6 + 3]),
             int(g_kit_source.tracks[6].params[kKitArpEnd + 3]));

    for (uint32_t i = 0; i < uint32_t(kKitLfoParams * kTracks); i++) g_kit_file[lfo + i] = 200;
    for (uint32_t i = 0; i < uint32_t(kKitArpParams * kTracks); i++) g_kit_file[arp + i] = 201;
    const uint32_t asr = section_at(g_kit_file, kSectionKitAsr);
    for (uint32_t i = 0; i < uint32_t(kKitAsrParams * kTracks); i++) g_kit_file[asr + i] = 202;
    const uint32_t osc = section_at(g_kit_file, kSectionKitOsc);
    for (uint32_t i = 0; i < uint32_t(kKitOscParams * kTracks); i++) g_kit_file[osc + i] = 203;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), 127);
        }
    }
}

TEST(kit_a_file_from_before_the_lfo_loads_with_every_lfo_and_arp_off) {
    // Backward compatibility: a KIT.BIN with the tracks section only -- what every build before
    // the LFO wrote -- still loads, keeps its sound, and turns every LFO and arp OFF rather than
    // leaving whatever the kit it is read into held.
    fresh_kit_file();
    drop_duck_section();
    static uint8_t with_arp[kKitFileBytes];
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1184u);    // the ASR page's section first
    CHECK_EQ(drop_last_section(g_kit_alt, with_arp), 1164u);      // then SLC AUTO's
    const uint32_t lfo_size = drop_last_section(with_arp, g_kit_alt);
    CHECK_EQ(lfo_size, 1120u);                    // what the LFO's build wrote
    for (uint32_t i = 0; i < lfo_size; i++) g_kit_file[i] = g_kit_alt[i];

    // That one first: the LFO comes back, the arp is off.
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitParams; p++) g_kit_loaded.tracks[t].params[p] = 99;
    }
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, lfo_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitLfoEnd; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
        for (int p = kKitLfoEnd; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), absent_param(p));
        }
    }

    const uint32_t old_size = drop_last_section(g_kit_file, g_kit_alt);
    CHECK_EQ(old_size, 1076u);
    CHECK_EQ(int(peek_u16(g_kit_alt, 6)), 1);
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitSoundParams; p < kKitParams; p++) g_kit_loaded.tracks[t].params[p] = 99;
    }
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_alt, old_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitSoundParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), migrated(t, p));
        }
        for (int p = kKitSoundParams; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), absent_param(p));
        }
        CHECK_EQ(int(g_kit_loaded.tracks[t].machine), int(g_kit_source.tracks[t].machine));
    }

    // Present but short is damage, not an old file -- for any of the sections.
    for (uint16_t id : {kSectionKitLfo, kSectionKitArp, kSectionKitAsr}) {
        fresh_kit_file();
        const uint32_t entry = section_entry_at(g_kit_file, id);
        poke_u32(g_kit_file, entry + 8, peek_u32(g_kit_file, entry + 8) - 1);
        refresh_crc(g_kit_file);
        CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
                 int(SerializeResult::malformed));
    }
}

TEST(kit_an_unknown_machine_falls_back_to_the_first_one) {
    fresh_kit_file();

    // Not the last one: an unknown id should land somewhere known, and clamping to kMachines - 1
    // would hand a damaged byte whichever machine happens to sit at the end of the list.
    for (int t = 0; t < kTracks; t++) {
        g_kit_file[kit_track_at(g_kit_file, t) + kKitSoundParams] = uint8_t(kMachines + t);
    }
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_kit_loaded.tracks[t].machine), 0);

    // Every machine this build does have survives.
    for (int t = 0; t < kTracks; t++) {
        g_kit_file[kit_track_at(g_kit_file, t) + kKitSoundParams] = uint8_t(t % kMachines);
    }
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_kit_loaded.tracks[t].machine), t % kMachines);
}

TEST(kit_the_fader_is_stored_in_thousandths_and_cannot_carry_a_nan) {
    // A format that cannot express a NaN or an infinity cannot hand one to the mixer, which is
    // why the fader goes down as a uint16_t of thousandths rather than as a float's 32 bits.
    fill_kit(g_kit_source);
    g_kit_source.tracks[0].level = 0.0f;
    g_kit_source.tracks[1].level = 1.0f;
    g_kit_source.tracks[2].level = 0.5f;
    g_kit_source.tracks[3].level = 1.5f;                            // above unity: the UI's max
    g_kit_source.tracks[4].level = -1.0f;                           // below zero
    g_kit_source.tracks[5].level = float_from_bits(0x7FC00000u);    // a quiet NaN
    g_kit_source.tracks[6].level = float_from_bits(0x7F800000u);    // +infinity
    g_kit_source.tracks[7].level = float_from_bits(0xFF800000u);    // -infinity

    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), &written)),
             int(SerializeResult::ok));

    CHECK_EQ(int(stored_level(g_kit_file, 0)), 0);
    CHECK_EQ(int(stored_level(g_kit_file, 1)), 1000);
    CHECK_EQ(int(stored_level(g_kit_file, 2)), 500);
    CHECK_EQ(int(stored_level(g_kit_file, 3)), 1500);     // was clamped to 1000 and lost
    CHECK_EQ(int(stored_level(g_kit_file, 4)), 0);
    // Every comparison against a NaN is false, so the `>= 0` test is the one that rejects it.
    CHECK_EQ(int(stored_level(g_kit_file, 5)), 0);
    CHECK_EQ(int(stored_level(g_kit_file, 6)), 2000);     // the engine's ceiling
    CHECK_EQ(int(stored_level(g_kit_file, 7)), 0);

    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, written)), int(SerializeResult::ok));
    CHECK_NEAR(g_kit_loaded.tracks[0].level, 0.0f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[1].level, 1.0f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[2].level, 0.5f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[3].level, 1.5f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[4].level, 0.0f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[5].level, 0.0f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[6].level, 2.0f, 0.0f);
    CHECK_NEAR(g_kit_loaded.tracks[7].level, 0.0f, 0.0f);
}

TEST(kit_a_fader_above_the_engines_ceiling_in_a_file_is_clamped_on_the_way_in) {
    fresh_kit_file();
    poke_u16(g_kit_file, kit_track_at(g_kit_file, 2) + kKitSoundParams + 1, 65535);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    // An infinity is a blown output, so nothing above the engine's own ceiling (2.0, the most
    // `Cmd::set_track_level` accepts) is allowed out of this file.
    CHECK_NEAR(g_kit_loaded.tracks[2].level, 2.0f, 0.0f);
}

TEST(kit_every_thousandth_of_the_fader_survives_a_round_trip) {
    fill_kit(g_kit_source);
    for (uint16_t milli = 0; milli <= 1000; milli++) {
        for (int t = 0; t < kTracks; t++) {
            g_kit_source.tracks[t].level = float(milli) / 1000.0f;
        }
        uint32_t written = 0;
        const SerializeResult wrote =
            kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), &written);
        if (wrote != SerializeResult::ok) CHECK_EQ(int(wrote), int(SerializeResult::ok));
        if (stored_level(g_kit_file, 0) != milli) CHECK_EQ(int(stored_level(g_kit_file, 0)),
                                                           int(milli));
        const SerializeResult read = kit_read(g_kit_loaded, g_kit_file, written);
        if (read != SerializeResult::ok) CHECK_EQ(int(read), int(SerializeResult::ok));
        const float want = float(milli) / 1000.0f;
        if (!(g_kit_loaded.tracks[0].level == want)) {
            CHECK_NEAR(g_kit_loaded.tracks[0].level, want, 0.0f);
        }
    }
}

TEST(kit_a_path_filling_every_byte_loses_its_last_character_not_its_terminator) {
    fill_kit(g_kit_source);
    // 112 bytes with no room for a terminator. app/ hands this to enaga::files, which reads
    // until a NUL: a name one character short fails loudly, an unterminated one reads whatever
    // sits after it.
    for (int i = 0; i < kSamplePathBytes; i++) {
        g_kit_source.tracks[0].sample[i] = char('a' + i % 26);
    }

    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), &written)),
             int(SerializeResult::ok));
    const uint32_t path = kit_track_at(g_kit_file, 0) + kKitSoundParams + 3;
    CHECK_EQ(int(g_kit_file[path + kSamplePathBytes - 1]), 0);

    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, written)), int(SerializeResult::ok));
    CHECK_EQ(int(std::strlen(g_kit_loaded.tracks[0].sample)), kSamplePathBytes - 1);
    for (int i = 0; i < kSamplePathBytes - 1; i++) {
        CHECK_EQ(int(g_kit_loaded.tracks[0].sample[i]), int(char('a' + i % 26)));
    }
}

TEST(kit_a_shortened_path_leaves_no_trace_of_the_old_one) {
    fill_kit(g_kit_source);
    const char *was = "/SAMPLES/A VERY LONG SAMPLE NAME INDEED.WAV";
    std::memset(g_kit_source.tracks[0].sample, 0, sizeof(g_kit_source.tracks[0].sample));
    for (int i = 0; was[i] != '\0'; i++) g_kit_source.tracks[0].sample[i] = was[i];
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), nullptr)),
             int(SerializeResult::ok));

    // Now the same track plays something shorter. The file must record the kit, not the editing
    // history: the tail of the old name must not stay on the card.
    const char *now = "/K.WAV";
    std::memset(g_kit_source.tracks[0].sample, 0, sizeof(g_kit_source.tracks[0].sample));
    for (int i = 0; now[i] != '\0'; i++) g_kit_source.tracks[0].sample[i] = now[i];
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), &written)),
             int(SerializeResult::ok));

    const uint32_t path = kit_track_at(g_kit_file, 0) + kKitSoundParams + 3;
    CHECK_EQ(std::strcmp(reinterpret_cast<const char *>(g_kit_file + path), now), 0);
    int nonzero_tail = 0;
    for (int i = int(std::strlen(now)); i < kSamplePathBytes; i++) {
        if (g_kit_file[path + uint32_t(i)] != 0) nonzero_tail++;
    }
    CHECK_EQ(nonzero_tail, 0);
}

TEST(kit_nothing_past_a_paths_terminator_is_read_back) {
    fresh_kit_file();
    const uint32_t path = kit_track_at(g_kit_file, 1) + kKitSoundParams + 3;

    g_kit_file[path + 0] = 'A';
    g_kit_file[path + 1] = 'B';
    g_kit_file[path + 2] = '\0';
    for (int i = 3; i < kSamplePathBytes; i++) g_kit_file[path + uint32_t(i)] = 'Z';
    refresh_crc(g_kit_file);

    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(std::strcmp(g_kit_loaded.tracks[1].sample, "AB"), 0);
    for (int i = 2; i < kSamplePathBytes; i++) {
        CHECK_EQ(int(g_kit_loaded.tracks[1].sample[i]), 0);
    }

    // And 112 bytes with no terminator anywhere: the last character pays, because every reader
    // of `sample` stops at a NUL and there would otherwise be none.
    for (int i = 0; i < kSamplePathBytes; i++) g_kit_file[path + uint32_t(i)] = 'Q';
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(std::strlen(g_kit_loaded.tracks[1].sample)), kSamplePathBytes - 1);
}

TEST(kit_read_refuses_a_damaged_container_and_clears_the_kit) {
    fresh_kit_file();
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK(g_kit_loaded.tracks[0].sample[0] != '\0');

    fresh_kit_file();
    g_kit_file[1] = 'X';
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::bad_magic));

    // A refused load leaves the kit cleared, never half read.
    const KitTrack fresh;
    int set = 0;
    for (int t = 0; t < kTracks; t++) {
        const KitTrack &track = g_kit_loaded.tracks[t];
        for (int p = 0; p < kKitSoundParams; p++) {
            // TON's flat is 64 (D8), which is what kit_clear gives a synth track.
            const bool tone = p == kSlotTone && machine_has_voice_filter(track.machine);
            if (track.params[p] != (tone ? kToneFlat : 0)) set++;
        }
        if (track.sample[0] != '\0') set++;
        if (!(track.level == fresh.level)) set++;
    }
    CHECK_EQ(set, 0);

    // And a refused kit leaves the DEFAULT machines, not eight of machine 0: `kit_clear` is what
    // a refusal falls back to, and a cleared kit is docs/SPEC.md's default kit (core/project.cpp
    // says why the file format owns that and not app/).
    CHECK_EQ(int(g_kit_loaded.tracks[0].machine), int(MachineId::kick));
    CHECK_EQ(int(g_kit_loaded.tracks[6].machine), int(MachineId::va_poly));
    CHECK_EQ(int(g_kit_loaded.tracks[7].machine), int(MachineId::sample));

    fresh_kit_file();
    poke_u16(g_kit_file, kVersionOffset, uint16_t(kKitVersion + 1));
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::bad_version));

    fresh_kit_file();
    g_kit_file[kHeaderBytes + 40] = uint8_t(g_kit_file[kHeaderBytes + 40] ^ 0x80);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::bad_crc));

    // A tracks section too short to hold eight tracks.
    fresh_kit_file();
    poke_u32(g_kit_file, section_entry_at(g_kit_file, kSectionKitTracks) + 8,
             kTrackRecordBytes * uint32_t(kTracks) - 1);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::malformed));

    CHECK_EQ(int(kit_read(g_kit_loaded, nullptr, 0)), int(SerializeResult::truncated));
}

TEST(kit_read_never_looks_past_the_bytes_it_was_given) {
    fill_kit(g_kit_source);
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, uint32_t(sizeof(g_kit_file)), nullptr)),
             int(SerializeResult::ok));
    std::memcpy(g_kit_alt, g_kit_file, kKitFileBytes);
    for (uint32_t i = kKitFileBytes; i < sizeof(g_kit_file); i++) {
        g_kit_file[i] = 0x00;
        g_kit_alt[i] = 0xFF;
    }

    for (uint32_t size = 0; size <= kKitFileBytes; size++) {
        const int a = int(kit_read(g_kit_loaded, g_kit_file, size));
        const int b = int(kit_read(g_kit_loaded, g_kit_alt, size));
        if (a != b) CHECK_EQ(a, b);
        const bool ok = a == int(SerializeResult::ok);
        if (ok != (size == kKitFileBytes)) CHECK_EQ(ok, size == kKitFileBytes);
    }
}

// --- BANK_x.BIN -----------------------------------------------------------

namespace {

Pattern g_probe;

uint32_t bank_section_length(const uint8_t *file) {
    return peek_u32(file, section_entry_at(file, kSectionBankPatterns) + 8);
}

// The absolute offset of one pattern inside a bank file, found the way bank_read finds it: from
// each pattern's OWN container header, never from a size this test assumed.
uint32_t pattern_at_in_bank(const uint8_t *file, int slot) {
    uint32_t at = section_at(file, kSectionBankPatterns);
    for (int i = 0; i < slot; i++) at += kHeaderBytes + peek_u32(file, at + 8);
    return at;
}

// Writes bank `bank` of a freshly filled project into g_bank. Returns the file's size.
uint32_t fresh_bank_file(int bank) {
    fill_project(g_source, bank);
    uint32_t written = 0;
    const SerializeResult res =
        bank_write(g_source, bank, g_bank, uint32_t(sizeof(g_bank)), &written);
    CHECK_EQ(int(res), int(SerializeResult::ok));
    return written;
}

// How many of a bank's 16 slots differ from the project they were written from.
int slots_wrong(const Project &loaded, const Project &source, int bank) {
    int wrong = 0;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        const Diff diff = diff_patterns(loaded.patterns[bank][slot], source.patterns[bank][slot]);
        if (diff.header != 0 || diff.track_header != 0 || diff.flags != 0 || diff.note != 0 ||
            diff.velocity != 0 || diff.length != 0 || diff.micro != 0 || diff.probability != 0 ||
            diff.condition != 0 || diff.ratchet != 0 || diff.chord != 0 ||
            diff.lock_count != 0 || diff.lock_values != 0) {
            wrong++;
        }
    }
    return wrong;
}

}  // namespace

TEST(bank_bytes_is_the_header_the_table_and_the_sixteen_patterns) {
    fill_project(g_source, 6);

    uint32_t sum = 0;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        sum += pattern_bytes(g_source.patterns[6][slot]);
    }
    CHECK_EQ(bank_bytes(g_source, 6), kHeaderBytes + kSectionEntryBytes + sum);
    CHECK(bank_bytes(g_source, 6) <= kBankMaxBytes);

    // An empty bank is smaller than a busy one, because the lock pool is written compacted.
    CHECK(bank_bytes(g_source, 0) < bank_bytes(g_source, 6));

    // There is no such bank, so there is no size either.
    CHECK_EQ(bank_bytes(g_source, -1), 0u);
    CHECK_EQ(bank_bytes(g_source, kBanks), 0u);
}

TEST(bank_is_sixteen_patterns_back_to_back) {
    // The layout project.hpp sells the per-slot refusal on, re-parsed here with nothing but
    // `pattern_read`'s `consumed`. A bank that only read correctly because bank_read and
    // bank_write agreed on something undocumented would be caught.
    const uint32_t written = fresh_bank_file(1);
    CHECK_EQ(written, bank_bytes(g_source, 1));

    const uint32_t first = section_at(g_bank, kSectionBankPatterns);
    CHECK_EQ(first, kHeaderBytes + kSectionEntryBytes);
    CHECK_EQ(bank_section_length(g_bank), written - first);
    CHECK_EQ(int(peek_u16(g_bank, kVersionOffset)), int(kBankVersion));
    CHECK_EQ(int(peek_u16(g_bank, 6)), 1);

    uint32_t at = first;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        uint32_t consumed = 0;
        CHECK_EQ(int(pattern_read(g_probe, g_bank + at, written - at, &consumed)),
                 int(SerializeResult::ok));
        // Each pattern's container reports its own extent, and it is exactly what
        // `pattern_bytes` said it would take.
        CHECK_EQ(consumed, pattern_bytes(g_source.patterns[1][slot]));
        check_no_diff(diff_patterns(g_probe, g_source.patterns[1][slot]));
        at += consumed;
    }
    // The patterns are the section, with nothing between them and nothing after them.
    CHECK_EQ(at, written);
}

TEST(bank_round_trips_sixteen_distinct_patterns) {
    const uint32_t written = fresh_bank_file(2);

    project_init(g_loaded);
    g_loaded.bank_dirty[2] = true;
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 2, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 0);
    CHECK_EQ(slots_wrong(g_loaded, g_source, 2), 0);

    // A successful read means what is in RAM is what is in the file, so nothing is waiting to
    // be saved.
    CHECK(!g_loaded.bank_dirty[2]);

    // The 16 patterns really were distinct, so the round trip was not comparing a pattern with
    // itself 16 times.
    int distinct = 0;
    for (int slot = 1; slot < kPatternsPerBank; slot++) {
        const Diff diff = diff_patterns(g_loaded.patterns[2][slot], g_loaded.patterns[2][0]);
        if (diff.note != 0 || diff.header != 0) distinct++;
    }
    CHECK_EQ(distinct, kPatternsPerBank - 1);
}

TEST(bank_read_leaves_the_other_seven_banks_alone) {
    // The file holds bank 3 and nothing else, so bank 5 is set up AFTER it is written --
    // fresh_bank_file starts from project_init, which would otherwise empty it again.
    const uint32_t written = fresh_bank_file(3);
    // Bank 5 holds something of its own that no bank 3 load may disturb.
    make_busy_pattern(g_source.patterns[5][9], 77);

    project_init(g_loaded);
    make_busy_pattern(g_loaded.patterns[5][9], 77);
    for (int bank = 0; bank < kBanks; bank++) g_loaded.bank_dirty[bank] = true;

    CHECK_EQ(int(bank_read(g_loaded, 3, g_bank, written, nullptr)), int(SerializeResult::ok));
    check_no_diff(diff_patterns(g_loaded.patterns[5][9], g_source.patterns[5][9]));

    // Only bank 3's dirty flag is cleared: the others still differ from their files.
    int dirty = 0;
    for (int bank = 0; bank < kBanks; bank++) {
        if (g_loaded.bank_dirty[bank]) dirty++;
    }
    CHECK_EQ(dirty, kBanks - 1);
    CHECK(!g_loaded.bank_dirty[3]);
}

TEST(bank_one_damaged_pattern_costs_one_pattern) {
    // project.hpp's invariant. A pattern this build refuses costs that slot and nothing else:
    // the 15 others load and `refused` says 1.
    const uint32_t written = fresh_bank_file(4);
    const uint32_t seven = pattern_at_in_bank(g_bank, 7);

    // A byte inside pattern 7's payload, well clear of the four that say how long it is, so the
    // patterns after it are still found. Its own CRC now disagrees; the bank's is repaired, so
    // the file itself is sound and only the pattern in it is not.
    g_bank[seven + kHeaderBytes + 40] = uint8_t(g_bank[seven + kHeaderBytes + 40] ^ 0x5A);
    refresh_crc(g_bank);

    project_init(g_loaded);
    g_loaded.bank_dirty[4] = true;
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 4, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 1);

    CHECK(is_empty_pattern(g_loaded.patterns[4][7]));
    int wrong = 0;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        if (slot == 7) continue;
        const Diff diff = diff_patterns(g_loaded.patterns[4][slot], g_source.patterns[4][slot]);
        if (diff.note != 0 || diff.header != 0 || diff.lock_values != 0) wrong++;
    }
    CHECK_EQ(wrong, 0);

    // A refused slot stays clean on purpose: marking the bank dirty would let the next save
    // overwrite the damaged file and with it any chance of recovering it.
    CHECK(!g_loaded.bank_dirty[4]);
}

TEST(bank_a_pattern_from_a_newer_build_costs_one_pattern_too) {
    const uint32_t written = fresh_bank_file(0);
    const uint32_t three = pattern_at_in_bank(g_bank, 3);

    // The version sits in the header, which no CRC covers, so only the bank's needs repairing.
    poke_u16(g_bank, three + kVersionOffset, uint16_t(kPatternVersion + 1));
    refresh_crc(g_bank);

    project_init(g_loaded);
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 1);
    CHECK(is_empty_pattern(g_loaded.patterns[0][3]));
    CHECK(!is_empty_pattern(g_loaded.patterns[0][2]));
    CHECK(!is_empty_pattern(g_loaded.patterns[0][4]));
}

TEST(bank_two_damaged_patterns_cost_two) {
    const uint32_t written = fresh_bank_file(0);
    const uint32_t first = pattern_at_in_bank(g_bank, 0);
    const uint32_t last = pattern_at_in_bank(g_bank, kPatternsPerBank - 1);

    g_bank[first + kHeaderBytes + 24] = uint8_t(g_bank[first + kHeaderBytes + 24] ^ 0x01);
    g_bank[last + kHeaderBytes + 24] = uint8_t(g_bank[last + kHeaderBytes + 24] ^ 0x01);
    refresh_crc(g_bank);

    project_init(g_loaded);
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 2);
    CHECK(is_empty_pattern(g_loaded.patterns[0][0]));
    CHECK(is_empty_pattern(g_loaded.patterns[0][kPatternsPerBank - 1]));
    // The 14 in between are fine, so the damage did not spread along the file.
    int empty = 0;
    for (int slot = 1; slot < kPatternsPerBank - 1; slot++) {
        if (is_empty_pattern(g_loaded.patterns[0][slot])) empty++;
    }
    CHECK_EQ(empty, 0);
}

TEST(bank_a_banks_own_bad_crc_refuses_all_sixteen) {
    // What the per-pattern CRCs do NOT cover. The bank's CRC is over its whole payload, patterns
    // included, as serialize.hpp's container says it must be, so a flipped bit anywhere refuses
    // all 16 slots -- the honest answer for a file whose structure cannot be trusted.
    const uint32_t written = fresh_bank_file(0);
    const uint32_t three = pattern_at_in_bank(g_bank, 3);
    g_bank[three + kHeaderBytes + 40] = uint8_t(g_bank[three + kHeaderBytes + 40] ^ 0x5A);
    // and this time the bank's CRC is NOT repaired.

    fill_project(g_loaded, 0);
    g_loaded.bank_dirty[0] = true;
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::bad_crc));
    // The file failed, not a pattern in it, and the result says which: `refused` stays 0.
    CHECK_EQ(refused, 0);

    int empty = 0;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        if (is_empty_pattern(g_loaded.patterns[0][slot])) empty++;
    }
    CHECK_EQ(empty, kPatternsPerBank);
    CHECK(!g_loaded.bank_dirty[0]);
}

TEST(bank_a_pattern_whose_extent_runs_out_refuses_that_slot_onwards) {
    // The section ran out, or a pattern's header does not say where it ends. Either way the
    // patterns after it cannot be found, so that slot and all of them are refused rather than
    // guessed at.
    const uint32_t written = fresh_bank_file(0);
    const uint32_t entry = section_entry_at(g_bank, kSectionBankPatterns);
    const uint32_t full = bank_section_length(g_bank);

    // Ten bytes short of the last pattern.
    poke_u32(g_bank, entry + 8, full - 10);
    refresh_crc(g_bank);

    project_init(g_loaded);
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 1);
    CHECK(is_empty_pattern(g_loaded.patterns[0][kPatternsPerBank - 1]));
    CHECK(!is_empty_pattern(g_loaded.patterns[0][kPatternsPerBank - 2]));

    // A section with no patterns in it at all: every slot is refused, and the file itself is
    // still structurally sound, so the result is `ok` with all 16 counted.
    fresh_bank_file(0);
    poke_u32(g_bank, section_entry_at(g_bank, kSectionBankPatterns) + 8, 0);
    refresh_crc(g_bank);
    project_init(g_loaded);
    refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, kPatternsPerBank);
}

TEST(bank_write_and_read_refuse_a_bank_that_does_not_exist) {
    fill_project(g_source, 0);
    uint32_t written = 7;

    // There is no SerializeResult for "no such bank", and serialize.hpp is not project.cpp's to
    // extend, so the file that would have been written does not add up: `malformed`.
    for (int bank : {-1, kBanks, 99}) {
        CHECK_EQ(int(bank_write(g_source, bank, g_bank, uint32_t(sizeof(g_bank)), &written)),
                 int(SerializeResult::malformed));
        CHECK_EQ(written, 0u);

        int refused = -1;
        CHECK_EQ(int(bank_read(g_loaded, bank, g_bank, uint32_t(sizeof(g_bank)), &refused)),
                 int(SerializeResult::malformed));
        CHECK_EQ(refused, 0);
    }
}

TEST(bank_write_refuses_a_buffer_one_byte_short_and_touches_nothing) {
    fill_project(g_source, 0);
    const uint32_t need = bank_bytes(g_source, 0);
    CHECK(need > kHeaderBytes);

    fill_sentinel(g_bank, uint32_t(sizeof(g_bank)));
    uint32_t written = 5;
    CHECK_EQ(int(bank_write(g_source, 0, g_bank, need - 1, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);
    for (uint32_t i = 0; i < sizeof(g_bank); i++) CHECK_EQ(int(g_bank[i]), int(kSentinel));

    CHECK_EQ(int(bank_write(g_source, 0, nullptr, need, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);

    CHECK_EQ(int(bank_write(g_source, 0, g_bank, need, &written)), int(SerializeResult::ok));
    CHECK_EQ(written, need);
}

TEST(bank_an_empty_bank_round_trips_as_sixteen_empty_patterns) {
    project_init(g_source);
    uint32_t written = 0;
    CHECK_EQ(int(bank_write(g_source, 7, g_bank, uint32_t(sizeof(g_bank)), &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, bank_bytes(g_source, 7));

    fill_project(g_loaded, 7);
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 7, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 0);

    int empty = 0;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        if (is_empty_pattern(g_loaded.patterns[7][slot])) empty++;
    }
    CHECK_EQ(empty, kPatternsPerBank);
}

TEST(bank_read_refuses_a_damaged_container_and_empties_the_bank) {
    uint32_t written = fresh_bank_file(0);
    int refused = -1;

    g_bank[2] = 'X';
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::bad_magic));
    CHECK_EQ(refused, 0);

    written = fresh_bank_file(0);
    poke_u16(g_bank, kVersionOffset, uint16_t(kBankVersion + 1));
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::bad_version));
    CHECK_EQ(refused, 0);

    written = fresh_bank_file(0);
    poke_u32(g_bank, 8, 0x0FFFFFFFu);
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::truncated));

    written = fresh_bank_file(0);
    poke_u16(g_bank, 6, 0);
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::malformed));

    // The patterns section missing, under an id this file type does not use.
    written = fresh_bank_file(0);
    poke_u16(g_bank, section_entry_at(g_bank, kSectionBankPatterns), 42);
    refresh_crc(g_bank);
    CHECK_EQ(int(bank_read(g_loaded, 0, g_bank, written, &refused)),
             int(SerializeResult::malformed));

    CHECK_EQ(int(bank_read(g_loaded, 0, nullptr, 0, &refused)), int(SerializeResult::truncated));
    CHECK_EQ(refused, 0);
}

TEST(bank_read_never_looks_past_the_bytes_it_was_given) {
    // Sampled rather than exhaustive: a bank file is about 129 KiB and each refusal rewrites 16
    // patterns, so every truncation would be a few gigabytes of memset for one bit of news. The
    // sizes that matter are the boundaries -- the header, the table, and each pattern's end.
    const uint32_t written = fresh_bank_file(1);
    std::memcpy(g_bank_alt, g_bank, written);
    for (uint32_t i = written; i < sizeof(g_bank); i++) {
        g_bank[i] = 0x00;
        g_bank_alt[i] = 0xFF;
    }

    uint32_t sizes[2 + 2 * kPatternsPerBank + 6] = {};
    uint32_t count = 0;
    sizes[count++] = 0;
    sizes[count++] = kHeaderBytes - 1;
    sizes[count++] = kHeaderBytes;
    sizes[count++] = kHeaderBytes + kSectionEntryBytes - 1;
    sizes[count++] = kHeaderBytes + kSectionEntryBytes;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        const uint32_t at = pattern_at_in_bank(g_bank, slot);
        sizes[count++] = at;
        sizes[count++] = at + 1;
    }
    sizes[count++] = written - 1;
    sizes[count++] = written;

    for (uint32_t i = 0; i < count; i++) {
        int refused_a = -1, refused_b = -1;
        const int a = int(bank_read(g_loaded, 1, g_bank, sizes[i], &refused_a));
        const int b = int(bank_read(g_loaded, 1, g_bank_alt, sizes[i], &refused_b));
        if (a != b) CHECK_EQ(a, b);
        if (refused_a != refused_b) CHECK_EQ(refused_a, refused_b);
    }
}

// --- the four file types together -----------------------------------------
//
// project.cpp restates serialize.cpp's container rules rather than sharing them, and says it
// checks them in the same ORDER so that all four file types give the same reason for the same
// damage. These two tests are where that claim is held.

namespace {

enum class FileKind { pattern = 0, project = 1, kit = 2, bank = 3 };

const char *file_kind_name(FileKind kind) {
    switch (kind) {
        case FileKind::pattern: return "PATTERN";
        case FileKind::project: return "PROJECT.BIN";
        case FileKind::kit: return "KIT.BIN";
        default: return "BANK_A.BIN";
    }
}

// A freshly written file of each kind, in its own buffer.
uint8_t *make_file(FileKind kind, uint32_t &size) {
    switch (kind) {
        case FileKind::pattern: {
            make_busy_pattern(g_scratch, 3);
            const SerializeResult res = pattern_write(g_scratch, g_pattern_file,
                                                      uint32_t(sizeof(g_pattern_file)), &size);
            CHECK_EQ(int(res), int(SerializeResult::ok));
            return g_pattern_file;
        }
        case FileKind::project:
            fresh_project_file();
            size = kProjectFileBytes;
            return g_project;
        case FileKind::kit:
            fresh_kit_file();
            size = kKitFileBytes;
            return g_kit_file;
        default:
            size = fresh_bank_file(0);
            return g_bank;
    }
}

SerializeResult read_file(FileKind kind, const uint8_t *file, uint32_t size) {
    switch (kind) {
        case FileKind::pattern: return pattern_read(g_probe, file, size);
        case FileKind::project: return project_read(g_loaded, file, size);
        case FileKind::kit: return kit_read(g_kit_loaded, file, size);
        default: return bank_read(g_loaded, 0, file, size, nullptr);
    }
}

// The six kinds of damage every container can carry, applied by hand to the header or the
// payload. `refresh` says whether the CRC is repaired afterwards, so that a test about structure
// is not answered by the checksum instead.
enum class Damage { magic, version, payload_byte, declared_payload, no_sections, too_many };

void apply(uint8_t *file, Damage damage) {
    switch (damage) {
        case Damage::magic: file[3] = 'Z'; break;
        case Damage::version: poke_u16(file, kVersionOffset, 0xBEEF); break;
        case Damage::payload_byte:
            file[kHeaderBytes + 20] = uint8_t(file[kHeaderBytes + 20] ^ 0x11);
            break;
        case Damage::declared_payload: poke_u32(file, 8, 0x0FFFFFFFu); break;
        case Damage::no_sections: poke_u16(file, 6, 0); break;
        default: poke_u16(file, 6, uint16_t(kMaxSections + 1)); break;
    }
}

// Reports a mismatch in a way that names the file type: `serialize_result_name` is what the UI
// shows on a refused load, so the message reads the way the screen would.
void check_same_reason(FileKind kind, SerializeResult got, SerializeResult want) {
    if (got == want) {
        CHECK(got == want);
        return;
    }
    CHECK_EQ(file_kind_name(kind), serialize_result_name(want));
    CHECK_EQ(serialize_result_name(got), serialize_result_name(want));
}

}  // namespace

TEST(the_four_file_types_agree_about_damage) {
    const Damage damages[] = {Damage::magic,            Damage::version,
                              Damage::payload_byte,     Damage::declared_payload,
                              Damage::no_sections,      Damage::too_many};
    const SerializeResult want[] = {
        SerializeResult::bad_magic, SerializeResult::bad_version, SerializeResult::bad_crc,
        SerializeResult::truncated, SerializeResult::malformed,   SerializeResult::malformed,
    };

    for (uint32_t d = 0; d < 6; d++) {
        for (int k = 0; k < 4; k++) {
            const FileKind kind = FileKind(k);
            uint32_t size = 0;
            uint8_t *file = make_file(kind, size);
            apply(file, damages[d]);
            check_same_reason(kind, read_file(kind, file, size), want[d]);
        }
    }
}

TEST(the_four_file_types_agree_about_which_damage_wins) {
    // A file that is damaged twice over must give ONE answer, and the same answer whichever of
    // the four file types it is -- the order project.cpp's find_sections documents: the declared
    // payload against the buffer, then the CRC, then the version, then the table, with the magic
    // ahead of all of them.
    struct Case {
        Damage first;
        Damage second;
        SerializeResult want;
    };
    const Case cases[] = {
        // The magic is checked before anything else, so it wins over a broken checksum.
        {Damage::magic, Damage::payload_byte, SerializeResult::bad_magic},
        // A payload the buffer cannot hold is refused before the CRC may read it.
        {Damage::declared_payload, Damage::version, SerializeResult::truncated},
        // A build that read the version first would call this file "UNKNOWN VERSION" and send
        // the user looking for a newer firmware, when what they have is a damaged card.
        {Damage::payload_byte, Damage::version, SerializeResult::bad_crc},
        // And the version is checked before the section table, so a newer file is not reported
        // as damaged.
        {Damage::version, Damage::no_sections, SerializeResult::bad_version},
    };

    for (const Case &one : cases) {
        for (int k = 0; k < 4; k++) {
            const FileKind kind = FileKind(k);
            uint32_t size = 0;
            uint8_t *file = make_file(kind, size);
            apply(file, one.first);
            apply(file, one.second);
            check_same_reason(kind, read_file(kind, file, size), one.want);

            // And in the other order, because the answer is the reader's and not the damage's.
            file = make_file(kind, size);
            apply(file, one.second);
            apply(file, one.first);
            check_same_reason(kind, read_file(kind, file, size), one.want);
        }
    }
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(project_a_machine_id_one_past_the_table_has_no_name) {
    CHECK_EQ(machine_name(uint8_t(MachineId::count)), "---");
    CHECK_EQ(machine_name(uint8_t(MachineId::count) - 1), "FM4");
}

TEST(project_settings_at_the_bottom_of_their_ranges_survive_a_round_trip) {
    // C is root 0 and quantize 0 is "off": both are the bottom of a clamp, and a clamp one too
    // high would turn them into C# and 1%. The same for a send or master byte of 0.
    fill_project(g_source, 2);
    ProjectSettings &s = g_source.settings;
    s.root = 0;
    s.quantize = 0;
    for (int i = 0; i < kSendFields; i++) s.sends[i] = 0;
    for (int i = 0; i < kMasterFields; i++) s.master[i] = 0;
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, kProjectFileBytes, &written)),
             int(SerializeResult::ok));
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::ok));
    const ProjectSettings &l = g_loaded.settings;
    CHECK_EQ(int(l.root), 0);
    CHECK_EQ(int(l.quantize), 0);
    for (int i = 0; i < kSendFields; i++) CHECK_EQ(int(l.sends[i]), 0);
    for (int i = 0; i < kMasterFields; i++) CHECK_EQ(int(l.master[i]), 0);
}

TEST(project_a_payload_declared_one_byte_past_the_file_is_truncated) {
    fill_project(g_source, 2);
    uint32_t written = 0;
    CHECK_EQ(int(project_write(g_source, g_project, kProjectFileBytes, &written)),
             int(SerializeResult::ok));
    poke_u32(g_project, 8, written - kHeaderBytes + 1);
    refresh_crc(g_project);
    project_init(g_loaded);
    CHECK_EQ(int(project_read(g_loaded, g_project, written)), int(SerializeResult::truncated));
}

TEST(bank_a_section_cut_short_counts_every_pattern_it_lost) {
    const uint32_t written = fresh_bank_file(2);
    (void)written;
    // Keep the first ten patterns whole and eight bytes of the eleventh's header: the rest
    // cannot be found, and all six of them are reported.
    uint32_t keep = 0;
    for (int slot = 0; slot < 10; slot++) keep += pattern_bytes(g_source.patterns[2][slot]);
    poke_u32(g_bank, kHeaderBytes + 8, keep + 8);
    refresh_crc(g_bank);
    project_init(g_loaded);
    int refused = -1;
    CHECK_EQ(int(bank_read(g_loaded, 2, g_bank, written, &refused)), int(SerializeResult::ok));
    CHECK_EQ(refused, 6);
}

TEST(kit_slc_auto_round_trips_and_an_older_file_slices_equally) {
    fresh_kit_file();
    // fresh_kit_file wrote g_kit_source; give it AUTO on two tracks and write again.
    g_kit_source.tracks[2].slice_mode = kSliceAuto;
    g_kit_source.tracks[7].slice_mode = kSliceAuto;
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_kit_loaded.tracks[t].slice_mode), t == 2 || t == 7 ? 1 : 0);
    }
    const uint32_t slices = section_at(g_kit_file, kSectionKitSlices);
    CHECK_EQ(int(g_kit_file[slices + 7]), 1);

    // A damaged byte slices equally rather than as anything else.
    g_kit_file[slices + 2] = 77;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_kit_loaded.tracks[2].slice_mode), 0);

    // A KIT.BIN from before SLC AUTO (tracks and LFO) loads, LFO kept, every track equal --
    // even read into a kit that had AUTO on.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    drop_duck_section();
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1184u);   // the ASR page went on after it
    for (uint32_t i = 0; i < 1184u; i++) g_kit_file[i] = g_kit_alt[i];
    const uint32_t old_size = drop_last_section(g_kit_file, g_kit_alt);
    CHECK_EQ(old_size, 1164u);
    for (int t = 0; t < kTracks; t++) g_kit_loaded.tracks[t].slice_mode = kSliceAuto;
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_alt, old_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_kit_loaded.tracks[t].slice_mode), 0);
        for (int p = kKitSoundParams; p < kKitArpEnd; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }
    g_kit_source.tracks[2].slice_mode = kSliceEqual;
    g_kit_source.tracks[7].slice_mode = kSliceEqual;
}

TEST(kit_lfo_trig_round_trips_and_an_older_file_runs_every_lfo_free) {
    fresh_kit_file();
    g_kit_source.tracks[1].lfo_trig = kLfoTrigRetrig;
    g_kit_source.tracks[6].lfo_trig = kLfoTrigOneShot;
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_kit_loaded.tracks[t].lfo_trig), t == 1 ? 1 : (t == 6 ? 2 : 0));
    }
    const uint32_t at = section_at(g_kit_file, kSectionKitLfoTrig);
    CHECK_EQ(int(g_kit_file[at + 6]), 2);

    // A damaged byte runs FREE rather than as anything else.
    g_kit_file[at + 1] = 9;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_kit_loaded.tracks[1].lfo_trig), int(kLfoTrigFree));
    CHECK_EQ(int(g_kit_loaded.tracks[6].lfo_trig), int(kLfoTrigOneShot));

    // A KIT.BIN from before LFO TRIG loads every LFO FREE -- even read into a kit that had
    // RETRIG on -- and everything else as it was.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    drop_lfo_trig_section();
    for (int t = 0; t < kTracks; t++) g_kit_loaded.tracks[t].lfo_trig = kLfoTrigRetrig;
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, 1309u)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(g_kit_loaded.tracks[t].lfo_trig), int(kLfoTrigFree));
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }

    // Present but short is damage, not an old file.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    const uint32_t entry = section_entry_at(g_kit_file, kSectionKitLfoTrig);
    poke_u32(g_kit_file, entry + 8, peek_u32(g_kit_file, entry + 8) - 1);
    refresh_crc(g_kit_file);
    CHECK(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes) != SerializeResult::ok);

    g_kit_source.tracks[1].lfo_trig = kLfoTrigFree;
    g_kit_source.tracks[6].lfo_trig = kLfoTrigFree;
}

TEST(kit_mod_slots_round_trip_and_an_older_file_loads_every_slot_off) {
    fresh_kit_file();
    g_kit_source.tracks[2].mod[0] = {kModSrcVel, 5, 40};       // VEL on slot 5 (FLTR CUT), +40
    g_kit_source.tracks[2].mod[1] = {kModSrcRnd, 13, -127};    // RND on PAN, the deepest down
    g_kit_source.tracks[7].mod[1] = {kModSrcEnv, 30, 1};       // ENV on OSC's SUB
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kKitFileBytes);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kModSlots; s++) {
            const KitModSlot &want = g_kit_source.tracks[t].mod[s];
            const KitModSlot &got = g_kit_loaded.tracks[t].mod[s];
            CHECK_EQ(int(got.source), int(want.source));
            CHECK_EQ(int(got.dest), int(want.dest));
            CHECK_EQ(int(got.depth), int(want.depth));
        }
    }
    // Byte by byte: track 2's slot 1 is source, dest, depth as a two's-complement byte.
    const uint32_t at = section_at(g_kit_file, kSectionKitMod);
    CHECK_EQ(int(g_kit_file[at + 2 * 6 + 3]), int(kModSrcRnd));
    CHECK_EQ(int(g_kit_file[at + 2 * 6 + 4]), 13);
    CHECK_EQ(int(g_kit_file[at + 2 * 6 + 5]), 0x81);

    // A damaged slot is OFF: an unknown source, a destination no slot can have (the LFO's DST);
    // and -128 is -127.
    g_kit_file[at + 2 * 6 + 0] = 6;
    g_kit_file[at + 2 * 6 + 3 + 1] = 19;
    g_kit_file[at + 7 * 6 + 3 + 2] = 0x80;
    g_kit_file[at + 7 * 6 + 3 + 0] = kModSrcPrs;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    CHECK_EQ(int(g_kit_loaded.tracks[2].mod[0].source), int(kModSrcOff));
    CHECK_EQ(int(g_kit_loaded.tracks[2].mod[1].source), int(kModSrcOff));
    CHECK_EQ(int(g_kit_loaded.tracks[7].mod[1].source), int(kModSrcPrs));
    CHECK_EQ(int(g_kit_loaded.tracks[7].mod[1].depth), -127);

    // A KIT.BIN from before the slots loads every slot OFF -- even read into a kit that had one
    // -- and everything else as it was.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    drop_mod_section();
    g_kit_loaded.tracks[4].mod[0] = {kModSrcLfo, 1, 9};
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, 1329u)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kModSlots; s++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].mod[s].source), int(kModSrcOff));
            CHECK(!g_kit_loaded.tracks[t].mod[s].on());
        }
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
        CHECK_EQ(int(g_kit_loaded.tracks[t].lfo_trig), int(g_kit_source.tracks[t].lfo_trig));
    }

    // Present but short is damage, not an old file.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    const uint32_t entry = section_entry_at(g_kit_file, kSectionKitMod);
    poke_u32(g_kit_file, entry + 8, peek_u32(g_kit_file, entry + 8) - 1);
    refresh_crc(g_kit_file);
    CHECK(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes) != SerializeResult::ok);

    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kModSlots; s++) g_kit_source.tracks[t].mod[s] = KitModSlot{};
    }
}

TEST(kit_the_duck_round_trips_is_clamped_and_an_older_file_ducks_as_v0_1_did) {
    // The sidechain duck (kSectionKitDuck): each track's depth, then the source and the times.
    fresh_kit_file();
    for (int t = 0; t < kTracks; t++) g_kit_source.tracks[t].duck = uint8_t(t * 10);
    g_kit_source.duck.source = 5;
    g_kit_source.duck.attack_ms = 12;
    g_kit_source.duck.hold = 0;
    g_kit_source.duck.release = 200;
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_kit_loaded.tracks[t].duck), t * 10);
    CHECK_EQ(int(g_kit_loaded.duck.source), 5);
    CHECK_EQ(int(g_kit_loaded.duck.attack_ms), 12);
    CHECK_EQ(int(g_kit_loaded.duck.hold), 0);
    CHECK_EQ(int(g_kit_loaded.duck.release), 200);

    // Laid out as core/project.cpp says: eight depths, then source, attack, hold, release.
    const uint32_t duck = section_at(g_kit_file, kSectionKitDuck);
    CHECK_EQ(int(g_kit_file[duck + 3]), 30);
    CHECK_EQ(int(g_kit_file[duck + 8]), 5);
    CHECK_EQ(int(g_kit_file[duck + 11]), 200);

    // Damaged bytes land inside the domains: a depth over 100%, a source past track 8, an
    // attack and release of 0 (which the envelope would turn into a step of infinity) and a
    // hold past its end.
    for (int t = 0; t < kTracks; t++) g_kit_file[duck + uint32_t(t)] = 250;
    g_kit_file[duck + 8] = 9;
    g_kit_file[duck + 9] = 0;
    g_kit_file[duck + 10] = 255;
    g_kit_file[duck + 11] = 0;
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_kit_loaded.tracks[t].duck), int(kDuckDepthMax));
    CHECK_EQ(int(g_kit_loaded.duck.source), 0);
    CHECK_EQ(int(g_kit_loaded.duck.attack_ms), int(kDuckAttackMin));
    CHECK_EQ(int(g_kit_loaded.duck.hold), int(kDuckHoldMax));
    CHECK_EQ(int(g_kit_loaded.duck.release), int(kDuckReleaseMin));

    // A KIT.BIN from before the duck was editable loads with the duck it was fixed at -- 35% on
    // every track, track 1 the source, 3 / 40 / 120 ms -- even read into a kit that held another.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    drop_filter_section();
    CHECK_EQ(drop_last_section(g_kit_file, g_kit_alt), 1252u);    // the OSC page went on after it
    for (uint32_t i = 0; i < 1252u; i++) g_kit_file[i] = g_kit_alt[i];
    const uint32_t old_size = drop_last_section(g_kit_file, g_kit_alt);
    CHECK_EQ(old_size, 1228u);
    for (int t = 0; t < kTracks; t++) g_kit_loaded.tracks[t].duck = 99;
    g_kit_loaded.duck.source = 6;
    g_kit_loaded.duck.release = 3;
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_alt, old_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) CHECK_EQ(int(g_kit_loaded.tracks[t].duck), 35);
    CHECK_EQ(int(g_kit_loaded.duck.source), 0);
    CHECK_EQ(int(g_kit_loaded.duck.attack_ms), 3);
    CHECK_EQ(int(g_kit_loaded.duck.hold) * kDuckTimeStepMs, 40);
    CHECK_EQ(int(g_kit_loaded.duck.release) * kDuckTimeStepMs, 120);

    // Present but short is damage, not an old file.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)),
             int(SerializeResult::ok));
    const uint32_t entry = section_entry_at(g_kit_file, kSectionKitDuck);
    poke_u32(g_kit_file, entry + 8, peek_u32(g_kit_file, entry + 8) - 1);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::malformed));

    for (int t = 0; t < kTracks; t++) g_kit_source.tracks[t].duck = kDuckDepthDefault;
    g_kit_source.duck = KitDuck{};
}

TEST(kit_the_asr_page_round_trips_and_an_older_file_loads_it_at_zero) {
    // The VA machines' ATK SUS REL FSU (kSectionKitAsr). A kit from before the page has no such
    // section, and every byte comes back 0 -- which dsp/va.cpp reads as the envelope the VAs had
    // -- even read into a kit whose page was up.
    fresh_kit_file();
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitArpEnd; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }
    drop_duck_section();
    const uint32_t old_size = drop_last_section(g_kit_file, g_kit_alt);
    CHECK_EQ(old_size, 1184u);
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitArpEnd; p < kKitParams; p++) g_kit_loaded.tracks[t].params[p] = 99;
    }
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_alt, old_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitArpEnd; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), migrated(t, p));
        }
        for (int p = kKitArpEnd; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), absent_param(p));
        }
    }
}

TEST(kit_the_osc_page_round_trips_and_an_older_file_loads_the_legacy_voice) {
    // The VA machines' WAV OS2 SUB DRV (kSectionKitOsc), the last section. A kit from before the
    // page has none, and every track comes back with kKitOscLegacy -- a saw over the sub the
    // voice had fixed -- even read into a kit whose page held something else.
    fresh_kit_file();
    CHECK(section_at(g_kit_file, kSectionKitOsc) > section_at(g_kit_file, kSectionKitDuck));
    CHECK_EQ(int(g_kit_file[section_at(g_kit_file, kSectionKitOsc) + 4 * 5 + 2]),
             int(g_kit_source.tracks[5].params[kKitAsrEnd + 2]));
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitAsrEnd; p < kKitParams; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }
    drop_filter_section();
    const uint32_t old_size = drop_last_section(g_kit_file, g_kit_alt);
    CHECK_EQ(old_size, 1252u);
    for (int t = 0; t < kTracks; t++) {
        for (int p = kKitAsrEnd; p < kKitParams; p++) g_kit_loaded.tracks[t].params[p] = 99;
    }
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_alt, old_size)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitAsrEnd; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), migrated(t, p));
        }
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[kKitAsrEnd + 0]), 0);     // a saw
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[kKitAsrEnd + 1]), 0);     // OSC 2 off
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[kKitAsrEnd + 2]), 102);   // the old sub
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[kKitAsrEnd + 3]), 0);     // no drive
    }

    // Present but short is damage, not an old file.
    fresh_kit_file();
    const uint32_t entry = section_entry_at(g_kit_file, kSectionKitOsc);
    poke_u32(g_kit_file, entry + 8, peek_u32(g_kit_file, entry + 8) - 1);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)),
             int(SerializeResult::malformed));
}

// --- one filter on the synth machines (D8) ---------------------------------------------------

TEST(kit_an_older_kit_maps_its_insert_filter_onto_ton_on_the_synth_machines) {
    // Off is flat; a low-pass keeps its cutoff as TON's low-pass; fully open is flat; a high-pass
    // becomes TON's high-pass; a band-pass counts as its top cut. FDE comes back tied (0).
    CHECK_EQ(int(kit_tone_from_insert(127, 0)), int(kToneFlat));     // OFF
    CHECK_EQ(int(kit_tone_from_insert(40, 0)), int(kToneFlat));      // OFF, wherever CUT is
    CHECK_EQ(int(kit_tone_from_insert(127, 20)), int(kToneFlat));    // LP12 wide open
    CHECK_EQ(int(kit_tone_from_insert(126, 20)), 63);                // just closed: not flat
    CHECK_EQ(int(kit_tone_from_insert(100, 20)), 42);                // about 6 kHz
    CHECK_EQ(int(kit_tone_from_insert(0, 20)), 0);                   // as dark as TON goes
    CHECK_EQ(int(kit_tone_from_insert(100, 60)), 42);                // BP12: its top cut
    CHECK_EQ(int(kit_tone_from_insert(0, 110)), int(kToneFlat));     // HP at 20 Hz is flat
    CHECK_EQ(int(kit_tone_from_insert(1, 110)), 65);                 // but any more is not
    CHECK_EQ(int(kit_tone_from_insert(50, 110)), 103);               // about 140 Hz
    CHECK_EQ(int(kit_tone_from_insert(127, 127)), 127);              // HP24 at the top
    // The low-pass keeps its cutoff: TON 42 is 18 kHz x 2^(-6.17 x 22/64) = 4.8 kHz, the old
    // CUT 100 was 20 x 2^(9.81 x 100/127) = 4.7 kHz. Within a count.

    fill_kit(g_kit_source);
    for (int t = 0; t < kTracks; t++) {
        g_kit_source.tracks[t].machine = uint8_t(t < 4 ? MachineId::va_mono : MachineId::kick);
        g_kit_source.tracks[t].params[4] = 50;     // DRV
        g_kit_source.tracks[t].params[5] = 100;    // CUT
        g_kit_source.tracks[t].params[6] = 90;     // RES
        g_kit_source.tracks[t].params[7] = 20;     // LP12
    }
    g_kit_source.tracks[1].machine = uint8_t(MachineId::va_poly);
    g_kit_source.tracks[2].machine = uint8_t(MachineId::wave_mono);
    g_kit_source.tracks[3].machine = uint8_t(MachineId::wave_poly);
    g_kit_source.tracks[4].machine = uint8_t(MachineId::fm);    // no filter of its own: kept
    uint32_t written = 0;
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)), int(SerializeResult::ok));

    // This build's file: no mapping, every byte as written.
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        for (int p = 4; p < 8; p++) {
            CHECK_EQ(int(g_kit_loaded.tracks[t].params[p]), int(g_kit_source.tracks[t].params[p]));
        }
    }

    // An older one: the four synth tracks map, the others keep their insert filter.
    drop_filter_section();
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, 1296u)), int(SerializeResult::ok));
    for (int t = 0; t < kTracks; t++) {
        const bool synth = t < 4;
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[4]), synth ? 0 : 50);
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[5]), synth ? 42 : 100);
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[6]), synth ? 0 : 90);
        CHECK_EQ(int(g_kit_loaded.tracks[t].params[7]), synth ? 0 : 20);
    }

    // Present but short is damage, like every other section.
    CHECK_EQ(int(kit_write(g_kit_source, g_kit_file, kKitFileBytes, &written)), int(SerializeResult::ok));
    const uint32_t entry = section_entry_at(g_kit_file, kSectionKitFilter);
    poke_u32(g_kit_file, entry + 8, 0);
    refresh_crc(g_kit_file);
    CHECK_EQ(int(kit_read(g_kit_loaded, g_kit_file, kKitFileBytes)), int(SerializeResult::malformed));
}

TEST(edit_pages_put_a_synth_filter_on_fltr_and_cover_every_slot_once) {
    // CUT RES ENV FDE on FLTR for the four synth machines, DEC first on GRAIN's AMP; page x 4 +
    // knob for everyone else.
    for (int m = 0; m < kMachines; m++) {
        const uint8_t id = uint8_t(m);
        int seen[12] = {};
        for (int page = 0; page < 3; page++) {
            for (int knob = 0; knob < 4; knob++) {
                const int slot = edit_slot(id, page, knob);
                CHECK(slot >= 0 && slot < 12);
                seen[slot]++;
                CHECK_EQ(edit_page_of(id, slot), page);
                if (!machine_has_voice_filter(id) && !(machine_is_grain(id) && page == 2)) {
                    CHECK_EQ(slot, page * 4 + knob);
                }
            }
        }
        for (int slot = 0; slot < 12; slot++) CHECK_EQ(seen[slot], 1);
        // Pages from FX on never move.
        for (int page = 3; page < 8; page++) CHECK_EQ(edit_slot(id, page, 2), page * 4 + 2);
    }
    const uint8_t va = uint8_t(MachineId::va_mono);
    CHECK_EQ(edit_slot(va, 1, 0), 2);            // CUT
    CHECK_EQ(edit_slot(va, 1, 1), 3);            // RES
    CHECK_EQ(edit_slot(va, 1, 2), 9);            // ENV
    CHECK_EQ(edit_slot(va, 1, 3), kSlotFdec);    // FDE
    CHECK_EQ(edit_slot(va, 0, 2), kSlotTone);    // TON on SRC
    CHECK_EQ(edit_slot(va, 2, 0), 8);            // DEC stays AMP's first
    CHECK_EQ(edit_slot(va, 2, 3), 11);           // and LVL its last
    const uint8_t grain = uint8_t(MachineId::grain);
    CHECK_EQ(edit_slot(grain, 2, 0), 10);        // GRAIN's DEC first on AMP, as the others'
    CHECK_EQ(edit_slot(grain, 2, 1), 8);         // then PIT
    CHECK_EQ(edit_slot(grain, 2, 2), 9);         // SHP
    CHECK_EQ(edit_slot(grain, 2, 3), 11);        // LVL
    CHECK_EQ(edit_page_of(grain, 10), 2);
    CHECK(!machine_has_voice_filter(uint8_t(MachineId::fm)));
}
