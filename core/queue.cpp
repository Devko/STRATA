// Queued pattern changes and chains. queue.hpp has the reasoning; this file has the arithmetic
// and the three decisions the header left open.
//
// ## Decisions this file makes where the header was silent
//
// - **A round-up that would leave the tick range saturates at UINT32_MAX, it never wraps.**
//   2^32 ticks is about 239 days of continuous play at 130 BPM, so this is unreachable in
//   practice -- but wrapping would not be merely unreachable, it would be *wrong in the loud
//   direction*: a switch tick of 3 is in the past forever, so `queue_due` would say true on
//   every tick from then on and the queue would fire the instant it was armed. Saturating keeps
//   the one invariant the caller depends on, `switch_tick > armed_tick` for every mode except a
//   saturated one, where it is `>=`.
// - **An unknown `QueueMode` behaves as `end_of_pattern`.** A queue is live state and is never
//   serialized (queue.hpp), so an out-of-range mode can only be a programming error, not a bad
//   file. Falling back on the default mode keeps the change due at a musical point; refusing
//   the arm would lose a pattern change the user asked for, and treating it as `immediate`
//   would switch at a point nobody chose. `queue_arm` still stores the mode it was given, so
//   `queue_mode_name` shows "?" and the mistake is visible on the screen.
// - **`chain_advance` reports a change only when the PATTERN changes, not when the ENTRY
//   changes.** See the comment on it below -- it is the one case the caller's "do I need to
//   switch" logic can get wrong.
//
// Nothing here divides except the round-up in `queue_switch_tick`, which runs once when a change
// is armed. `queue_due`, which the audio side calls on every tick, is a comparison.
#include "queue.hpp"

namespace strata {

namespace {

constexpr uint32_t kMaxTick = 0xFFFFFFFFu;

// The 1x grid boundaries `after_16` and `after_32` land on: 16 and 32 sixteenths.
constexpr uint32_t kTicksPer16Steps = 16u * uint32_t(kTicksPerSixteenth);   // 384
constexpr uint32_t kTicksPer32Steps = 32u * uint32_t(kTicksPerSixteenth);   // 768

// Where a change armed at `tick` lands on a grid of boundaries `multiple` ticks apart: the next
// tick if the press was late by at most `kQueueGraceTicks`, and otherwise the next boundary,
// strictly after `tick` because the boundary tick itself has already been played (queue.hpp).
// Saturating rather than wrapping, for the reason in the file comment.
//
// `multiple` must not be 0 -- every caller below substitutes a sane value first, which is what
// keeps a damaged `pattern_ticks` from dividing by zero. A `multiple` smaller than the grace
// window makes every press late enough to count, which is the only sane reading of a loop
// shorter than a human's reaction error, and is in any case only reachable from a damaged value.
//
// This is the one division in the module. It runs when a change is armed, which is control rate:
// a pad press, not a tick.
// The boundary a change armed at `tick` belongs to: the one just missed when the press is
// forgiven, otherwise the next one. Equal to `switch_tick_on_grid` except in the grace window.
uint32_t boundary_on_grid(uint32_t tick, uint32_t multiple) {
    const uint32_t since = tick % multiple;
    const uint32_t previous = tick - since;
    if (since <= uint32_t(kQueueGraceTicks)) return previous;
    if (previous > kMaxTick - multiple) return kMaxTick;
    return previous + multiple;
}

uint32_t switch_tick_on_grid(uint32_t tick, uint32_t multiple) {
    const uint32_t since = tick % multiple;              // how far into the pass the press was

    // The late press, forgiven. `tick + 1` and not `tick`: the arming tick has been played.
    if (since <= uint32_t(kQueueGraceTicks)) return tick == kMaxTick ? kMaxTick : tick + 1;

    const uint32_t previous = tick - since;              // the boundary before `tick`
    if (previous > kMaxTick - multiple) return kMaxTick;
    return previous + multiple;
}

// How many entries of the chain are usable, and the index of the entry `player` is on, clamped
// into that range. Shared by the three chain functions so that they cannot disagree about a
// chain that was edited while it was playing.
struct ChainCursor {
    int length = 0;
    int entry = 0;
};

ChainCursor chain_cursor(const Project &project, const ChainPlayer &player) {
    ChainCursor cursor;
    cursor.length = chain_length(project);
    // A chain that was shortened under a running player leaves `entry` past the end. Wrapping to
    // the first entry is the same thing the player does at the end of a pass, so the chain keeps
    // playing from a point that exists instead of reading a dropped entry.
    cursor.entry = int(player.entry) < cursor.length ? int(player.entry) : 0;
    return cursor;
}

}  // namespace

const char *queue_mode_name(QueueMode mode) {
    switch (mode) {
        case QueueMode::end_of_pattern: return "END";
        case QueueMode::after_16:       return "16";
        case QueueMode::after_32:       return "32";
        case QueueMode::immediate:      return "NOW";
        default:                        return "?";
    }
}

uint32_t queue_switch_tick(QueueMode mode, uint32_t armed_tick, int pattern_ticks) {
    switch (mode) {
        case QueueMode::immediate:
            // The very next tick, keeping the position: nothing is reset and nothing is rounded,
            // so a switch in the middle of a bar stays in the middle of that bar.
            return armed_tick == kMaxTick ? kMaxTick : armed_tick + 1;

        case QueueMode::after_16:
            return switch_tick_on_grid(armed_tick, kTicksPer16Steps);

        case QueueMode::after_32:
            return switch_tick_on_grid(armed_tick, kTicksPer32Steps);

        case QueueMode::end_of_pattern:
        default: {
            // A pattern loop of 0 or less cannot divide and cannot end, so it is treated as one
            // 16th (queue.hpp): the change then lands within a step rather than never. A huge
            // value is left alone -- it is a long wait, but it is a wait that ends, and clamping
            // it would mean guessing at a loop length the caller believes in.
            const uint32_t loop = pattern_ticks > 0 ? uint32_t(pattern_ticks)
                                                    : uint32_t(kTicksPerSixteenth);
            return switch_tick_on_grid(armed_tick, loop);
        }
    }
}

bool queue_arm(Queue &queue, int bank, int slot, QueueMode mode, uint32_t armed_tick,
               int pattern_ticks, uint32_t origin) {
    // Checked before anything is written, so a refused arm leaves a change that is already
    // queued exactly as it was. The alternative -- clearing first -- would let a mistyped index
    // cancel a pattern change the user had already set up.
    if (!project_index_valid(bank, slot)) return false;

    queue.pending = true;
    queue.bank = uint8_t(bank);
    queue.slot = uint8_t(slot);
    queue.mode = mode;

    switch (mode) {
        case QueueMode::immediate:
            queue.switch_tick = queue_switch_tick(mode, armed_tick, pattern_ticks);
            queue.restart = false;
            queue.start_tick = 0;
            break;
        case QueueMode::after_16:
        case QueueMode::after_32: {
            const uint32_t grid = mode == QueueMode::after_16 ? kTicksPer16Steps : kTicksPer32Steps;
            queue.switch_tick = switch_tick_on_grid(armed_tick, grid);
            queue.restart = true;
            queue.start_tick = boundary_on_grid(armed_tick, grid);
            break;
        }
        case QueueMode::end_of_pattern:
        default: {
            // Counted from the current pattern's own start. Both results are offsets from
            // `origin` and are added back saturating, for the reason in the file comment.
            const uint32_t base = origin <= armed_tick ? origin : 0u;
            const uint32_t loop = pattern_ticks > 0 ? uint32_t(pattern_ticks)
                                                    : uint32_t(kTicksPerSixteenth);
            const uint32_t into = armed_tick - base;
            const uint32_t at = switch_tick_on_grid(into, loop);
            const uint32_t start = boundary_on_grid(into, loop);
            queue.switch_tick = at > kMaxTick - base ? kMaxTick : base + at;
            queue.start_tick = start > kMaxTick - base ? kMaxTick : base + start;
            queue.restart = true;
            break;
        }
    }
    return true;
}

bool queue_due(const Queue &queue, uint32_t tick) {
    // `>=`, not `==`: a block holds several ticks and a restarted transport lands wherever it
    // lands, so an equality test would strand the change forever (queue.hpp). This is called on
    // every tick in audio context and is deliberately nothing but the comparison.
    return queue.pending && tick >= queue.switch_tick;
}

void queue_clear(Queue &queue) {
    // The whole struct, not just `pending`: a stale bank and slot that still read as a real
    // pattern are exactly the kind of thing a later bug uses by accident.
    queue = Queue{};
}

// --- chains ---------------------------------------------------------------

int chain_length(const Project &project) {
    const int declared = int(project.settings.chain_length);
    const int bound = declared < kChainEntries ? declared : kChainEntries;

    for (int i = 0; i < bound; i++) {
        const ChainEntry &entry = project.chain[i];
        // `repeats == 0` is what zeroed memory means, so it ends the chain (core/project.hpp).
        if (entry.repeats == 0) return i;
        // An entry naming a pattern that cannot exist ends it too. The length is a prefix
        // count, so there is no way to report "entry 3 is unusable but entry 4 is fine" --
        // and truncating is the safe reading, because every entry below the length is then one
        // `chain_advance` may index without checking again.
        if (!project_index_valid(int(entry.bank), int(entry.slot))) return i;
    }
    return bound;
}

bool chain_start(const Project &project, ChainPlayer &player) {
    // `settings.chain_enabled` is not consulted: whether to run the chain at all is the app's
    // decision, and a caller that wants the chain started has already made it.
    if (chain_length(project) <= 0) {
        player = ChainPlayer{};        // inactive, and on no entry
        return false;
    }

    player.active = true;
    player.entry = 0;
    player.orphaned = false;
    player.played = 0;
    return true;
}

bool chain_resume(const Project &project, ChainPlayer &player) {
    if (!chain_start(project, player)) return false;
    // `orphaned` is exactly "what is playing is not an entry; entry 0 comes next".
    player.orphaned =
        project.chain[0].bank != project.bank || project.chain[0].slot != project.slot;
    return true;
}

bool chain_advance(const Project &project, ChainPlayer &player, uint8_t &bank, uint8_t &slot) {
    if (!player.active) return false;

    const ChainCursor cursor = chain_cursor(project, player);
    if (cursor.length <= 0) {
        // The chain was emptied while it was playing. Stop following it rather than report a
        // pattern from memory that is no longer in the chain.
        player.active = false;
        return false;
    }

    // The playing entry was removed: the one now at its index comes next, from its first pass.
    if (player.orphaned) {
        player.orphaned = false;
        player.entry = uint8_t(cursor.entry);
        player.played = 0;
        const ChainEntry &upcoming = project.chain[cursor.entry];
        if (upcoming.bank == project.bank && upcoming.slot == project.slot) return false;
        bank = upcoming.bank;
        slot = upcoming.slot;
        return true;
    }

    const ChainEntry &current = project.chain[cursor.entry];

    // `played` in int: `repeats` is a byte and a pass count that reached 256 would wrap to 0 and
    // repeat the entry forever.
    const int played = int(player.played) + 1;
    if (played < int(current.repeats)) {
        player.entry = uint8_t(cursor.entry);
        player.played = uint8_t(played);
        return false;                  // the same pattern simply plays again
    }

    // The entry is used up. Wrap rather than stop: a chain loops (queue.hpp).
    const int next = cursor.entry + 1 < cursor.length ? cursor.entry + 1 : 0;
    const ChainEntry &upcoming = project.chain[next];

    player.entry = uint8_t(next);
    player.played = 0;

    // **True only when the PATTERN changes, not whenever the ENTRY does.** A one-entry chain, or
    // two entries naming the same pattern, therefore returns FALSE on every pass even though the
    // player moved on, and leaves `bank` and `slot` untouched -- the house convention for a
    // false return (`pattern_get_lock`, `bank_file_name`). `chain_current` answers "where is the
    // chain" on those passes.
    //
    // The header's contract is "returns true when the pattern should change", and for a caller
    // that is the useful reading: a pattern change is not free. It reloads the live pattern
    // index, clears the undo history (core/undo.hpp) and restarts the `1ST` conditions for every
    // track -- so reporting a change where there is none would make `1ST` fire on every pass of
    // a one-pattern chain, which is audibly wrong, and would throw away an edit history the user
    // can still see.
    // Against the LIVE pattern rather than `current` (queue.hpp): an entry edited while it
    // played names a pattern that is not the one sounding.
    if (upcoming.bank == project.bank && upcoming.slot == project.slot) return false;

    bank = upcoming.bank;
    slot = upcoming.slot;
    return true;
}

bool chain_current(const Project &project, const ChainPlayer &player, uint8_t &bank,
                   uint8_t &slot) {
    if (!player.active || player.orphaned) return false;   // nothing in the chain is playing

    const ChainCursor cursor = chain_cursor(project, player);
    if (cursor.length <= 0) return false;

    bank = project.chain[cursor.entry].bank;
    slot = project.chain[cursor.entry].slot;
    return true;
}

bool chain_locate(const Project &project, uint32_t tick, ChainPlayer &player, uint32_t &origin,
                  bool &over) {
    over = false;
    origin = 0;
    const int length = chain_length(project);
    if (length <= 0) {
        player = ChainPlayer{};
        return false;
    }
    // One pass through the chain, in ticks. At most 64 entries x 255 repeats x 64 steps x 24
    // ticks, about 25 million: no overflow.
    uint32_t total = 0;
    for (int i = 0; i < length; i++) {
        const ChainEntry &entry = project.chain[i];
        const Pattern &pattern = project.patterns[entry.bank][entry.slot];
        total += uint32_t(entry.repeats) * uint32_t(master_loop_ticks(pattern));
    }
    player.active = true;
    player.orphaned = false;
    player.entry = 0;
    player.played = 0;
    if (project.settings.chain_song && tick >= total) {
        over = true;
        origin = total;
        return true;
    }
    // A looping chain: the position inside this time round, and where this time round began.
    // One division, once per locate: not a hot path.
    const uint32_t within = tick % total;
    const uint32_t round = tick - within;

    uint32_t at = 0, run = 0;
    for (int i = 0; i < length; i++) {
        const ChainEntry &entry = project.chain[i];
        const uint32_t loop = uint32_t(master_loop_ticks(project.patterns[entry.bank][entry.slot]));
        const uint32_t span = uint32_t(entry.repeats) * loop;
        const bool same = i > 0 && entry.bank == project.chain[i - 1].bank &&
                          entry.slot == project.chain[i - 1].slot;
        if (!same) run = at;
        if (within < at + span) {
            player.entry = uint8_t(i);
            player.played = uint8_t((within - at) / loop);
            origin = round + run;
            return true;
        }
        at += span;
    }
    return true;   // not reached: `within` is below `total`
}

// --- editing a chain -----------------------------------------------------------

namespace {

// Cut the chain back to its usable prefix and zero everything after it, so the edits below can
// treat `length` as the truth. Returns the length.
int chain_normalize(Project &project) {
    const int length = chain_length(project);
    for (int i = length; i < kChainEntries; i++) {
        project.chain[i] = ChainEntry{};
        project.chain_extra[i] = SectionExtra{};
    }
    project.settings.chain_length = uint8_t(length);
    return length;
}

bool chain_entry_valid(uint8_t bank, uint8_t slot, uint8_t repeats, uint8_t mutes) {
    return repeats != 0 && mutes <= kChainMutesMax && project_index_valid(int(bank), int(slot));
}

}  // namespace

bool chain_set(Project &project, int at, uint8_t bank, uint8_t slot, uint8_t repeats,
               uint8_t mutes) {
    // Every refusal is decided before `chain_normalize` writes anything, so a refused edit leaves
    // the project exactly as it was, as queue.hpp promises.
    if (!chain_entry_valid(bank, slot, repeats, mutes)) return false;
    if (at < 0 || at >= chain_length(project)) return false;
    chain_normalize(project);
    project.chain[at] = ChainEntry{bank, slot, repeats, mutes};
    // A mute recall replaces a CAP: the two are one field, MUT, on the screen.
    if (mutes != 0) {
        project.chain_extra[at].cap = 0;
        project.chain_extra[at].mask = 0;
    }
    return true;
}

bool chain_insert(Project &project, int at, uint8_t bank, uint8_t slot, uint8_t repeats,
                  uint8_t mutes, const SectionExtra *extra) {
    if (!chain_entry_valid(bank, slot, repeats, mutes)) return false;
    if (extra != nullptr && !section_valid(*extra)) return false;
    const int usable = chain_length(project);
    if (at < 0 || at > usable || usable >= kChainEntries) return false;
    // Read before the shift: `extra` may point into the chain itself (a copy of an entry).
    SectionExtra fields = extra != nullptr ? *extra : SectionExtra{};
    if (mutes != 0) {
        fields.cap = 0;
        fields.mask = 0;
    }
    const int length = chain_normalize(project);
    for (int i = length; i > at; i--) {
        project.chain[i] = project.chain[i - 1];
        project.chain_extra[i] = project.chain_extra[i - 1];
    }
    project.chain[at] = ChainEntry{bank, slot, repeats, mutes};
    project.chain_extra[at] = fields;
    project.settings.chain_length = uint8_t(length + 1);
    return true;
}

bool chain_remove(Project &project, int at) {
    if (at < 0 || at >= chain_length(project)) return false;
    const int length = chain_normalize(project);
    for (int i = at; i + 1 < length; i++) {
        project.chain[i] = project.chain[i + 1];
        project.chain_extra[i] = project.chain_extra[i + 1];
    }
    project.chain[length - 1] = ChainEntry{};
    project.chain_extra[length - 1] = SectionExtra{};
    project.settings.chain_length = uint8_t(length - 1);
    return true;
}

bool chain_at_end(const Project &project, const ChainPlayer &player) {
    if (!player.active || player.orphaned) return false;
    const ChainCursor cursor = chain_cursor(project, player);
    if (cursor.length <= 0 || cursor.entry != cursor.length - 1) return false;
    return int(player.played) + 1 >= int(project.chain[cursor.entry].repeats);
}

int chain_entry_mutes(const Project &project, const ChainPlayer &player) {
    if (!player.active || player.orphaned) return -1;
    const ChainCursor cursor = chain_cursor(project, player);
    if (cursor.length <= 0) return -1;
    const uint8_t mutes = project.chain[cursor.entry].mutes;
    return mutes == 0 || mutes > kChainMutesMax ? -1 : int(mutes) - 1;
}

void chain_player_inserted(ChainPlayer &player, int at) {
    if (!player.active) return;
    // At or above the playing entry: it moves down one, and so does the player.
    if (at <= int(player.entry) && player.entry < kChainEntries - 1) player.entry++;
}

void chain_player_removed(ChainPlayer &player, int at) {
    if (!player.active || at < 0) return;
    if (at < int(player.entry)) {
        player.entry--;
    } else if (at == int(player.entry) && !player.orphaned) {
        // The entry being played is gone. `entry` now names the one after it, which plays next.
        player.orphaned = true;
        player.played = 0;
    }
}


bool section_valid(const SectionExtra &extra) {
    return extra.cap <= 1 && (extra.cap != 0 || extra.mask == 0) &&
           extra.scene <= kSectionScenesMax && extra.morph <= kSectionMorphMax &&
           extra.energy <= kSectionEnergyMax && extra.transpose <= kSectionTransposeMax &&
           (extra.bpm == 0 || (extra.bpm >= kTempoCentiMin && extra.bpm <= kTempoCentiMax));
}

bool chain_set_section(Project &project, int at, const SectionExtra &extra) {
    if (!section_valid(extra)) return false;
    if (at < 0 || at >= chain_length(project)) return false;
    chain_normalize(project);
    const SectionExtra &fields = extra;
    project.chain_extra[at] = fields;
    // CAP replaces a mute recall: one field, MUT, on the screen and in the file.
    if (fields.cap != 0) project.chain[at].mutes = 0;
    return true;
}

namespace {

int clamp_walk(int value, int delta, int low, int high) {
    const int next = value + delta;
    return next < low ? low : (next > high ? high : next);
}

}  // namespace

bool section_step(Project &project, int at, SectionField field, int delta, uint8_t live_mutes,
                  uint16_t tempo_centi) {
    if (at < 0 || at >= chain_length(project) || field >= SectionField::count) return false;
    chain_normalize(project);
    SectionExtra &e = project.chain_extra[at];
    ChainEntry &entry = project.chain[at];
    switch (field) {
        case SectionField::mut: {
            // One line of ten: --, M1 .. M8, CAP.
            const int now = e.cap != 0 ? int(kChainMutesMax) + 1 : int(entry.mutes);
            const int next = clamp_walk(now, delta, 0, int(kChainMutesMax) + 1);
            if (next == now) break;
            if (next > int(kChainMutesMax)) {
                e.cap = 1;
                e.mask = live_mutes;
                entry.mutes = 0;
            } else {
                e.cap = 0;
                e.mask = 0;
                entry.mutes = uint8_t(next);
            }
            break;
        }
        case SectionField::scn:
            e.scene = uint8_t(clamp_walk(int(e.scene), delta, 0, int(kSectionScenesMax)));
            break;
        case SectionField::mrp:
            e.morph = uint8_t(clamp_walk(int(e.morph), delta, 0, int(kSectionMorphMax)));
            break;
        case SectionField::nrg:
            e.energy = e.energy == 0 ? kSectionEnergyZero
                                     : uint8_t(clamp_walk(int(e.energy), delta, 1,
                                                          int(kSectionEnergyMax)));
            break;
        case SectionField::bpm: {
            const int from = e.bpm != 0 ? int(e.bpm) : int(tempo_centi);
            const int step = e.bpm != 0 ? delta * 100 : 0;
            e.bpm = uint16_t(clamp_walk(from, step, int(kTempoCentiMin), int(kTempoCentiMax)));
            break;
        }
        case SectionField::trn:
            e.transpose = e.transpose == 0 ? kSectionTransposeZero
                                           : uint8_t(clamp_walk(int(e.transpose), delta, 1,
                                                                int(kSectionTransposeMax)));
            break;
        case SectionField::count:
            break;
    }
    return true;
}

bool section_clear(Project &project, int at, SectionField field) {
    if (at < 0 || at >= chain_length(project) || field >= SectionField::count) return false;
    chain_normalize(project);
    SectionExtra &e = project.chain_extra[at];
    switch (field) {
        case SectionField::mut:
            e.cap = 0;
            e.mask = 0;
            project.chain[at].mutes = 0;
            break;
        case SectionField::scn: e.scene = 0; break;
        case SectionField::mrp: e.morph = kSectionMorphNone; break;
        case SectionField::nrg: e.energy = 0; break;
        case SectionField::bpm: e.bpm = 0; break;
        case SectionField::trn: e.transpose = 0; break;
        case SectionField::count: break;
    }
    return true;
}

const SectionExtra *chain_entry_section(const Project &project, const ChainPlayer &player) {
    if (!player.active || player.orphaned) return nullptr;
    const ChainCursor cursor = chain_cursor(project, player);
    if (cursor.length <= 0) return nullptr;
    return &project.chain_extra[cursor.entry];
}

}  // namespace strata
