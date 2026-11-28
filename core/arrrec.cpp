// ARR REC: see core/arrrec.hpp.
#include "arrrec.hpp"

#include "perform.hpp"   // kMorphScale
#include "queue.hpp"

namespace strata {

namespace {

bool same_fields(const SectionExtra &a, const SectionExtra &b) {
    return a.cap == b.cap && a.mask == b.mask && a.scene == b.scene && a.morph == b.morph &&
           a.energy == b.energy && a.bpm == b.bpm;
}

// Copy the chain into or out of a backup, whole (all 64, so the zeroed tail goes with it).
void store(const Project &project, ArrBackup &backup) {
    for (int i = 0; i < kChainEntries; i++) {
        backup.chain[i] = project.chain[i];
        backup.extra[i] = project.chain_extra[i];
    }
    backup.length = project.settings.chain_length;
}

void restore(Project &project, const ArrBackup &backup) {
    for (int i = 0; i < kChainEntries; i++) {
        project.chain[i] = backup.chain[i];
        project.chain_extra[i] = backup.extra[i];
    }
    project.settings.chain_length = backup.length;
}

bool same_mix(const ArrState &a, const ArrState &b) {
    if (a.bank != b.bank || a.slot != b.slot) return false;
    for (int t = 0; t < kTracks; t++) {
        if (a.lanes[t] != b.lanes[t]) return false;
    }
    return true;
}

bool borrows(const ArrState &state) {
    for (int t = 0; t < kTracks; t++) {
        if (state.lanes[t] != kLaneOwn) return true;
    }
    return false;
}

// CAPTURE what `pass` played into a free slot of its bank, avoiding both what it played and
// what plays now (core/launch.hpp). The slot, or -1 when none is free or the locks do not fit.
int capture_pass(Project &project, const ArrState &pass) {
    uint8_t avoid[kTracks + 1];
    avoid[0] = lane_source(pass.bank, pass.slot);
    for (int t = 0; t < kTracks; t++) avoid[t + 1] = pass.lanes[t];
    const int slot = launch_capture_slot_avoiding(project, pass.bank, avoid, kTracks + 1);
    if (slot < 0) return -1;
    if (!launch_compose_lanes(project, avoid[0], pass.lanes, project.patterns[pass.bank][slot])) {
        return -1;
    }
    project.bank_dirty[pass.bank] = true;
    return slot;
}

}  // namespace

void arr_state_lanes(ArrState &state, const Project &project) {
    for (int t = 0; t < kTracks; t++) {
        state.lanes[t] = lane_borrowed(project, t) ? project.settings.lane_source[t] : kLaneOwn;
    }
}

ArrState arr_state(uint8_t bank, uint8_t slot, uint8_t mutes, uint8_t scene_b, uint16_t morph,
                   int energy, uint16_t tempo_centi, bool external) {
    ArrState state;
    state.bank = bank;
    state.slot = slot;
    SectionExtra &f = state.fields;
    f.cap = 1;
    f.mask = mutes;
    f.scene = uint8_t((scene_b < kSectionScenesMax ? scene_b : kSectionScenesMax - 1) + 1);
    // Where the morph sits, to its nearer side, as the quick fill does (choice 3: SNAP, never a
    // guessed RIDE). Half way exactly is side A.
    f.morph = uint32_t(morph) * 2u > kMorphScale ? kSectionMorphSnap : kSectionMorphSnapA;
    f.energy = section_energy_byte(energy);
    if (external) {
        f.bpm = 0;
    } else {
        f.bpm = tempo_centi < kTempoCentiMin
                    ? kTempoCentiMin
                    : (tempo_centi > kTempoCentiMax ? kTempoCentiMax : tempo_centi);
    }
    return state;
}

bool arr_start(Project &project, ArrRec &rec, ArrBackup &backup) {
    if (rec.on) return false;
    store(project, backup);
    backup.valid = true;
    for (int i = 0; i < kChainEntries; i++) {
        project.chain[i] = ChainEntry{};
        project.chain_extra[i] = SectionExtra{};
    }
    project.settings.chain_length = 0;
    rec = ArrRec{};
    rec.on = true;
    return true;
}

void arr_pass_begin(ArrRec &rec, const ArrState &state, uint32_t tick, uint32_t loop) {
    if (!rec.on) return;
    rec.pass_open = true;
    rec.pass_from = tick;
    rec.pass_loop = loop < 1u ? 1u : loop;
    rec.pass = state;
}

ArrPass arr_pass_end(Project &project, ArrRec &rec, uint32_t tick) {
    rec.launch = ArrLaunch::none;
    if (!rec.on || !rec.pass_open) return ArrPass::none;
    rec.pass_open = false;
    // A transport that went backwards has recorded nothing of this pass.
    const uint32_t played = tick >= rec.pass_from ? tick - rec.pass_from : 0u;
    // At least half of the pass, compared without a divide (this chip has none for integers).
    if (uint64_t(played) * 2u < uint64_t(rec.pass_loop)) return ArrPass::dropped;

    // The pattern this pass goes into the chain as: the one the same mix went into last time,
    // the playing pattern when no lane is borrowed, or (below) a capture.
    bool known = false;
    uint8_t bank = rec.pass.bank;
    uint8_t slot = rec.pass.slot;
    if (rec.last_known && same_mix(rec.last, rec.pass)) {
        known = true;
        bank = rec.target_bank;
        slot = rec.target_slot;
    } else if (!borrows(rec.pass)) {
        known = true;
    }

    const int length = chain_length(project);
    if (known && rec.extending && length > 0) {
        ChainEntry &last = project.chain[length - 1];
        // Against the pass the entry was written from: the entry itself may hold BPM `--`.
        if (last.bank == bank && last.slot == slot && last.mutes == 0 && rec.last_known &&
            same_fields(rec.last.fields, rec.pass.fields) &&
            int(last.repeats) < kChainRepeatsMax) {
            last.repeats = uint8_t(last.repeats + 1);
            return ArrPass::extended;
        }
    }
    // Full before a capture, so a take that is over does not fill a slot for nothing.
    if (length >= kChainEntries) return ArrPass::full;
    ArrLaunch launch = ArrLaunch::none;
    if (!known) {
        const int captured = capture_pass(project, rec.pass);
        if (captured >= 0) {
            slot = uint8_t(captured);
            launch = ArrLaunch::kept;
        } else {
            launch = ArrLaunch::lost;
        }
    }
    // The BPM only where the tempo changed (header, "The tempo").
    SectionExtra fields = rec.pass.fields;
    const bool first = rec.written == 0;
    const bool changed = !first && rec.last_known && fields.bpm != rec.last.fields.bpm;
    if (!changed) fields.bpm = 0;
    // A pattern that cannot exist (damage) is not written; nothing else is refused.
    if (!chain_insert(project, length, bank, slot, 1, 0, &fields)) {
        return ArrPass::dropped;
    }
    if (first) {
        rec.first_entry = length;
        rec.start_bpm = rec.pass.fields.bpm;
    } else if (changed && !rec.tempo_written) {
        rec.tempo_written = true;
        if (rec.first_entry >= 0 && rec.first_entry < length && rec.start_bpm != 0) {
            project.chain_extra[rec.first_entry].bpm = rec.start_bpm;
        }
    }
    rec.extending = true;
    rec.written = uint8_t(rec.written + 1);
    rec.last_known = true;
    rec.last = rec.pass;
    rec.target_bank = bank;
    rec.target_slot = slot;
    rec.launch = launch;
    rec.launch_bank = bank;
    rec.launch_slot = slot;
    return ArrPass::added;
}

void arr_chain_edited(ArrRec &rec) {
    rec.extending = false;
    rec.first_entry = -1;
}

int arr_finish(Project &project, ArrRec &rec, ArrBackup &backup, uint32_t tick) {
    if (!rec.on) return 0;
    arr_pass_end(project, rec, tick);
    const int written = int(rec.written);
    const ArrLaunch launch = rec.launch;
    const uint8_t launch_bank = rec.launch_bank;
    const uint8_t launch_slot = rec.launch_slot;
    rec = ArrRec{};
    rec.launch = launch;
    rec.launch_bank = launch_bank;
    rec.launch_slot = launch_slot;
    if (written == 0 && backup.valid) {
        restore(project, backup);
        backup.valid = false;
    }
    return written;
}

bool arr_undo(Project &project, const ArrRec &rec, ArrBackup &backup) {
    if (rec.on || !backup.valid) return false;
    // A swap, entry by entry: the take goes into the backup, so ARR UNDO again brings it back.
    for (int i = 0; i < kChainEntries; i++) {
        const ChainEntry entry = project.chain[i];
        project.chain[i] = backup.chain[i];
        backup.chain[i] = entry;
        const SectionExtra extra = project.chain_extra[i];
        project.chain_extra[i] = backup.extra[i];
        backup.extra[i] = extra;
    }
    const uint8_t length = project.settings.chain_length;
    project.settings.chain_length = backup.length;
    backup.length = length;
    return true;
}

}  // namespace strata
