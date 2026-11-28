// Tests for ARR REC (core/arrrec): a jam written into the chain, a pass at a time.
//
// Synthetic jams: a pass is a pair of ticks handed to arr_pass_begin / arr_pass_end, so each test
// says exactly which passes were played, cut or skipped and what each one held, and checks the
// chain that comes out. The rules under test are the header's: a pass is what played at its
// start; the same thing again is a repeat, anything else a new entry; X16 splits; 64 entries
// end the take; a pass is written when at least half of it was recorded; a take that wrote
// nothing gives the old chain back; ARR UNDO swaps.
#include "check.hpp"

#include "arrrec.hpp"
#include "perform.hpp"
#include "queue.hpp"

using namespace strata;

namespace {

// A Project is about 1 MiB and a stack frame is capped at 16 KiB (CLAUDE.md).
Project g_project;
ArrRec g_rec;
ArrBackup g_backup;

constexpr uint32_t kLoop = 384u;   // a 16-step pattern at 1x

void reset() {
    for (int i = 0; i < kChainEntries; i++) {
        g_project.chain[i] = ChainEntry{};
        g_project.chain_extra[i] = SectionExtra{};
    }
    g_project.settings.chain_length = 0;
    g_rec = ArrRec{};
    g_backup = ArrBackup{};
}

ArrState state(uint8_t slot, uint8_t mutes = 0, int energy = 0, uint16_t morph = 0) {
    return arr_state(0, slot, mutes, 0, morph, energy, 13000, false);
}

// One whole pass of `s` starting at `*tick`, which moves on by the loop.
ArrPass pass(const ArrState &s, uint32_t &tick) {
    arr_pass_begin(g_rec, s, tick, kLoop);
    tick += kLoop;
    return arr_pass_end(g_project, g_rec, tick);
}

}  // namespace

TEST(arrrec_state_holds_cap_scene_side_energy_and_tempo) {
    const ArrState s = arr_state(2, 5, 0x81, 3, kMorphScale, -20, 12850, false);
    CHECK_EQ(int(s.bank), 2);
    CHECK_EQ(int(s.slot), 5);
    CHECK_EQ(int(s.fields.cap), 1);
    CHECK_EQ(int(s.fields.mask), 0x81);
    CHECK_EQ(int(s.fields.scene), 4);                       // scene 4 on side B
    CHECK_EQ(int(s.fields.morph), int(kSectionMorphSnap));
    CHECK_EQ(section_energy(s.fields.energy), -20);
    CHECK_EQ(int(s.fields.bpm), 12850);
    CHECK(section_valid(s.fields));
    // The morph to its nearer side; half way is A.
    CHECK_EQ(int(arr_state(0, 0, 0, 0, kMorphScale / 2, 0, 12000, false).fields.morph),
             int(kSectionMorphSnapA));
    CHECK_EQ(int(arr_state(0, 0, 0, 0, kMorphScale / 2 + 1, 0, 12000, false).fields.morph),
             int(kSectionMorphSnap));
    // An outside clock's tempo is not the song's.
    CHECK_EQ(int(arr_state(0, 0, 0, 0, 0, 0, 12000, true).fields.bpm), 0);
    // Out of range in, in range out.
    const ArrState wild = arr_state(0, 0, 0, 200, 0, 99, 100, false);
    CHECK(section_valid(wild.fields));
    CHECK_EQ(int(wild.fields.scene), 16);
    CHECK_EQ(section_energy(wild.fields.energy), 64);
    CHECK_EQ(int(wild.fields.bpm), int(kTempoCentiMin));
    CHECK_EQ(int(arr_state(0, 0, 0, 0, 0, 0, 65000, false).fields.bpm), int(kTempoCentiMax));
}

TEST(arrrec_start_keeps_the_chain_and_empties_it) {
    reset();
    CHECK(chain_insert(g_project, 0, 1, 2, 3, 4));
    CHECK(chain_insert(g_project, 1, 0, 7, 2));
    g_project.chain_extra[1].scene = 9;
    CHECK(arr_start(g_project, g_rec, g_backup));
    CHECK(g_rec.on);
    CHECK(g_backup.valid);
    CHECK_EQ(chain_length(g_project), 0);
    CHECK_EQ(int(g_backup.length), 2);
    CHECK_EQ(int(g_backup.chain[0].mutes), 4);
    CHECK_EQ(int(g_backup.extra[1].scene), 9);
    CHECK(section_empty(g_project.chain_extra[1]));
    // A second start while on changes nothing.
    CHECK(!arr_start(g_project, g_rec, g_backup));
    CHECK_EQ(int(g_backup.length), 2);
}

TEST(arrrec_a_synthetic_jam_becomes_the_expected_chain) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    // Intro: A01 with the kick muted, two passes.
    CHECK(pass(state(0, 0x01), tick) == ArrPass::added);
    CHECK(pass(state(0, 0x01), tick) == ArrPass::extended);
    // The kick comes in: same pattern, new mutes, four passes.
    for (int i = 0; i < 4; i++) pass(state(0, 0x00), tick);
    // Breakdown: ENERGY down, one pass.
    pass(state(0, 0x00, -40), tick);
    // Drop on A02, morph on B, three passes.
    for (int i = 0; i < 3; i++) pass(state(1, 0x00, 0, kMorphScale), tick);
    CHECK_EQ(chain_length(g_project), 4);
    CHECK_EQ(int(g_rec.written), 4);
    const int repeats[] = {2, 4, 1, 3};
    const int slots[] = {0, 0, 0, 1};
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(int(g_project.chain[i].repeats), repeats[i]);
        CHECK_EQ(int(g_project.chain[i].slot), slots[i]);
        CHECK_EQ(int(g_project.chain[i].mutes), 0);
        CHECK_EQ(int(g_project.chain_extra[i].cap), 1);
        CHECK(section_valid(g_project.chain_extra[i]));
    }
    CHECK_EQ(int(g_project.chain_extra[0].mask), 0x01);
    CHECK_EQ(int(g_project.chain_extra[1].mask), 0x00);
    CHECK_EQ(section_energy(g_project.chain_extra[2].energy), -40);
    CHECK_EQ(int(g_project.chain_extra[3].morph), int(kSectionMorphSnap));
    CHECK_EQ(int(g_project.chain_extra[2].morph), int(kSectionMorphSnapA));
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, tick), 4);
    CHECK(!g_rec.on);
    CHECK(g_backup.valid);
    CHECK_EQ(chain_length(g_project), 4);
}

TEST(arrrec_a_section_splits_at_x16) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    for (int i = 0; i < 35; i++) pass(state(3), tick);
    CHECK_EQ(chain_length(g_project), 3);
    CHECK_EQ(int(g_project.chain[0].repeats), 16);
    CHECK_EQ(int(g_project.chain[1].repeats), 16);
    CHECK_EQ(int(g_project.chain[2].repeats), 3);
    for (int i = 0; i < 3; i++) CHECK_EQ(int(g_project.chain[i].slot), 3);
}

TEST(arrrec_the_take_ends_when_a_65th_entry_is_needed) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    for (int i = 0; i < kChainEntries; i++) {
        CHECK(pass(state(uint8_t(i % 2)), tick) == ArrPass::added);
    }
    CHECK_EQ(chain_length(g_project), kChainEntries);
    // The 64th entry still takes repeats of itself...
    CHECK(pass(state(1), tick) == ArrPass::extended);
    // ...and anything new is the end.
    CHECK(pass(state(0), tick) == ArrPass::full);
    CHECK_EQ(chain_length(g_project), kChainEntries);
    CHECK_EQ(int(g_project.chain[63].repeats), 2);
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, tick), kChainEntries);
}

TEST(arrrec_a_pass_counts_when_half_of_it_was_recorded) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    // Cut in a quarter of the way through: three quarters recorded, written.
    arr_pass_begin(g_rec, state(0), kLoop / 4, kLoop);
    CHECK(arr_pass_end(g_project, g_rec, kLoop) == ArrPass::added);
    // Cut off exactly at half: written.
    arr_pass_begin(g_rec, state(1), kLoop, kLoop);
    CHECK(arr_pass_end(g_project, g_rec, kLoop + kLoop / 2) == ArrPass::added);
    // One tick short of half: dropped, and the chain is as it was.
    arr_pass_begin(g_rec, state(2), 2 * kLoop, kLoop);
    CHECK(arr_pass_end(g_project, g_rec, 2 * kLoop + kLoop / 2 - 1) == ArrPass::dropped);
    CHECK_EQ(chain_length(g_project), 2);
    // A transport that went back has recorded nothing.
    arr_pass_begin(g_rec, state(2), 5000, kLoop);
    CHECK(arr_pass_end(g_project, g_rec, 10) == ArrPass::dropped);
    // No pass open: nothing.
    CHECK(arr_pass_end(g_project, g_rec, 9000) == ArrPass::none);
    // A loop of 0 is one tick, never a divide or a pass that cannot count.
    arr_pass_begin(g_rec, state(3), 100, 0);
    CHECK_EQ(int(g_rec.pass_loop), 1);
    CHECK(arr_pass_end(g_project, g_rec, 101) == ArrPass::added);
}

TEST(arrrec_a_dropped_pass_between_two_of_the_same_keeps_one_entry) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    pass(state(0), tick);
    arr_pass_begin(g_rec, state(5), tick, kLoop);   // a pattern tried for a moment
    tick += 10;
    CHECK(arr_pass_end(g_project, g_rec, tick) == ArrPass::dropped);
    pass(state(0), tick);
    CHECK_EQ(chain_length(g_project), 1);
    CHECK_EQ(int(g_project.chain[0].repeats), 2);
}

TEST(arrrec_a_hand_edit_starts_a_new_entry) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    pass(state(0), tick);
    arr_chain_edited(g_rec);
    pass(state(0), tick);
    CHECK_EQ(chain_length(g_project), 2);
    CHECK_EQ(int(g_project.chain[1].repeats), 1);
    // The entry before the edit is not extended even when the edit removed the newer one.
    CHECK(chain_remove(g_project, 1));
    arr_chain_edited(g_rec);
    pass(state(0), tick);
    CHECK_EQ(chain_length(g_project), 2);
    CHECK_EQ(int(g_project.chain[0].repeats), 1);
}

TEST(arrrec_finish_counts_a_cut_pass_by_the_half_rule) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    pass(state(0), tick);
    arr_pass_begin(g_rec, state(1), tick, kLoop);
    // STOP three quarters into a pass of A02: it is written.
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, tick + kLoop * 3 / 4), 2);
    CHECK_EQ(chain_length(g_project), 2);
    CHECK(!g_rec.on && !g_rec.pass_open);
    // Finishing again does nothing.
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, tick + kLoop), 0);
    CHECK_EQ(chain_length(g_project), 2);
}

TEST(arrrec_a_take_that_wrote_nothing_gives_the_chain_back) {
    reset();
    CHECK(chain_insert(g_project, 0, 1, 2, 3, 4));
    CHECK(arr_start(g_project, g_rec, g_backup));
    arr_pass_begin(g_rec, state(0), 0, kLoop);
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, 20), 0);   // off at once
    CHECK_EQ(chain_length(g_project), 1);
    CHECK_EQ(int(g_project.chain[0].repeats), 3);
    CHECK_EQ(int(g_project.chain[0].mutes), 4);
    CHECK(!g_backup.valid);                                   // nothing to undo
    CHECK(!arr_undo(g_project, g_rec, g_backup));
}

TEST(arrrec_undo_swaps_the_take_and_the_chain_before_it) {
    reset();
    CHECK(chain_insert(g_project, 0, 1, 2, 3, 4));
    g_project.chain_extra[0].scene = 6;
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    pass(state(7), tick);
    pass(state(8), tick);
    // Not while a take is on.
    CHECK(!arr_undo(g_project, g_rec, g_backup));
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, tick), 2);
    CHECK(arr_undo(g_project, g_rec, g_backup));
    CHECK_EQ(chain_length(g_project), 1);
    CHECK_EQ(int(g_project.chain[0].mutes), 4);
    CHECK_EQ(int(g_project.chain_extra[0].scene), 6);
    CHECK(section_empty(g_project.chain_extra[1]));
    // Again: the take is back.
    CHECK(arr_undo(g_project, g_rec, g_backup));
    CHECK_EQ(chain_length(g_project), 2);
    CHECK_EQ(int(g_project.chain[0].slot), 7);
    CHECK_EQ(int(g_project.chain[1].slot), 8);
    CHECK_EQ(int(g_project.chain_extra[1].cap), 1);
    // A new take replaces the backup with the chain it replaces.
    CHECK(arr_start(g_project, g_rec, g_backup));
    CHECK_EQ(int(g_backup.length), 2);
    CHECK_EQ(int(g_backup.chain[1].slot), 8);
}

TEST(arrrec_passes_need_a_take) {
    reset();
    arr_pass_begin(g_rec, state(0), 0, kLoop);
    CHECK(!g_rec.pass_open);
    CHECK(arr_pass_end(g_project, g_rec, kLoop) == ArrPass::none);
    CHECK_EQ(chain_length(g_project), 0);
    CHECK_EQ(arr_finish(g_project, g_rec, g_backup, kLoop), 0);
}

TEST(arrrec_a_recorded_chain_writes_cap_so_an_old_build_reads_no_recall) {
    reset();
    CHECK(arr_start(g_project, g_rec, g_backup));
    uint32_t tick = 0;
    pass(state(0, 0x0F), tick);
    // CAP and a recall are one field: the recorded entry holds no recall for section 5 to write.
    CHECK_EQ(int(g_project.chain[0].mutes), 0);
    CHECK_EQ(int(g_project.chain_extra[0].cap), 1);
    CHECK_EQ(int(g_project.chain_extra[0].mask), 0x0F);
}
