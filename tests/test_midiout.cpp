// Tests for MIDI note out's bookkeeping (core/midiout).
//
// The property that matters is the one a performer cannot fix from the panel: every note-on is
// followed by exactly one note-off, whatever ended it -- a gate, a release, a mute, a stop, a
// retrigger, a full table -- and a full send queue delays a note-off but never loses it.
#include "check.hpp"

#include "midiout.hpp"

using namespace strata;

namespace {

// A sink that records every message and can be told to refuse, as a full Enaga queue does.
struct Recorder final : MidiSink {
    static constexpr int kMax = 512;
    uint8_t msg[kMax][3] = {};
    int count = 0;
    bool full = false;
    int refused = 0;

    // Note-ons minus note-offs per channel and pitch, over EVERY message, including the ones
    // past `kMax` that are counted but not kept.
    int bal[16][128] = {};

    bool send(const uint8_t *bytes, uint32_t n) override {
        if (full || n != 3) {
            refused++;
            return false;
        }
        if (count < kMax) {
            for (int i = 0; i < 3; i++) msg[count][i] = bytes[i];
        }
        count++;
        const int kind = bytes[0] & 0xF0;
        if (kind == 0x90) bal[bytes[0] & 0x0F][bytes[1] & 0x7F]++;
        if (kind == 0x80) bal[bytes[0] & 0x0F][bytes[1] & 0x7F]--;
        return true;
    }

    bool is(int i, uint8_t status, uint8_t a, uint8_t b) const {
        return i < count && i < kMax && msg[i][0] == status && msg[i][1] == a && msg[i][2] == b;
    }

    // Note-ons minus note-offs for one channel (1-16) and pitch.
    int balance(uint8_t channel, uint8_t note) const { return bal[channel - 1][note]; }
};

MidiOut g_out;
Recorder g_sink;

void fresh() {
    g_out.clear();
    g_sink = Recorder{};
}

// Run `blocks` blocks of `frames`, the way the engine brackets one.
void run(int blocks, uint32_t frames = 64) {
    for (int b = 0; b < blocks; b++) {
        g_out.begin_block(g_sink, frames);
        g_out.end_block(frames);
    }
}

}  // namespace

TEST(midiout_message_bytes_are_status_with_channel_and_seven_bit_data) {
    uint8_t m[3];
    midi_message(m, 0x90, 1, 60, 100);
    CHECK_EQ(int(m[0]), 0x90);
    midi_message(m, 0x80, 16, 200, 255);
    CHECK_EQ(int(m[0]), 0x8F);
    CHECK_EQ(int(m[1]), 200 & 0x7F);
    CHECK_EQ(int(m[2]), 0x7F);
    midi_message(m, 0xB0, 10, kMidiAllNotesOff, 0);
    CHECK_EQ(int(m[0]), 0xB9);
    CHECK_EQ(int(m[1]), 123);
}

TEST(midiout_a_program_change_is_two_bytes_on_the_channel) {
    uint8_t m[3] = {0, 0, 0xEE};
    midi_program(m, 1, 0);
    CHECK_EQ(int(m[0]), 0xC0);
    CHECK_EQ(int(m[1]), 0);
    CHECK_EQ(int(m[2]), 0xEE);                     // the third byte is not written
    midi_program(m, 16, 127);
    CHECK_EQ(int(m[0]), 0xCF);
    CHECK_EQ(int(m[1]), 127);
    midi_program(m, 5, 200);                       // masked to 7 bits
    CHECK_EQ(int(m[1]), 200 & 0x7F);
}

TEST(midiout_a_note_on_is_sent_at_once_with_its_velocity) {
    fresh();
    CHECK(g_out.note_on(g_sink, 2, 3, 60, 100, 10, 1000));
    CHECK_EQ(g_sink.count, 1);
    CHECK(g_sink.is(0, 0x92, 60, 100));
    CHECK_EQ(g_out.sounding(), 1);
    CHECK_EQ(g_out.sounding(2), 1);
    CHECK_EQ(g_out.sounding(1), 0);
}

TEST(midiout_a_velocity_of_zero_is_sent_as_one_because_zero_would_be_a_note_off) {
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 0, 0, 100));
    CHECK(g_sink.is(0, 0x90, 60, 1));
}

TEST(midiout_channel_off_or_out_of_range_sends_nothing) {
    fresh();
    CHECK(!g_out.note_on(g_sink, 0, kMidiOutOff, 60, 100, 0, 100));
    CHECK(!g_out.note_on(g_sink, 0, 17, 60, 100, 0, 100));
    CHECK(!g_out.note_on(g_sink, -1, 1, 60, 100, 0, 100));
    CHECK_EQ(g_sink.count, 0);
    CHECK_EQ(g_out.sounding(), 0);
}

TEST(midiout_the_note_off_falls_in_the_block_that_holds_the_end_of_the_gate) {
    // Fired at frame 10 with a gate of 200: the off is at frame 210 from this block's start,
    // which is frame 18 of the fourth block (64 * 3 = 192). Not before, and in that block.
    fresh();
    g_out.begin_block(g_sink, 64);
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 10, 200));
    g_out.end_block(64);
    run(2);
    CHECK_EQ(g_sink.count, 1);                 // blocks 2 and 3: still sounding
    g_out.begin_block(g_sink, 64);              // block 4 holds frame 210
    CHECK_EQ(g_sink.count, 2);
    CHECK(g_sink.is(1, 0x80, 60, 0));
    CHECK_EQ(g_out.sounding(), 0);
}

TEST(midiout_a_gate_ending_exactly_on_the_block_boundary_goes_in_the_next_block) {
    fresh();
    g_out.begin_block(g_sink, 64);
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 0, 64));
    g_out.end_block(64);
    CHECK_EQ(g_sink.count, 1);
    g_out.begin_block(g_sink, 64);              // frame 64 is frame 0 of this block
    CHECK_EQ(g_sink.count, 2);
}

TEST(midiout_a_held_note_never_ends_by_itself_and_ends_on_release) {
    fresh();
    CHECK(g_out.note_on(g_sink, 4, 5, 48, 90, 0, kMidiHeld));
    run(100000);                                 // well over a minute of blocks
    CHECK_EQ(g_sink.count, 1);
    g_out.release(g_sink, 4, 48);
    CHECK_EQ(g_sink.count, 2);
    CHECK(g_sink.is(1, 0x84, 48, 0));
}

TEST(midiout_a_gate_that_would_overflow_is_held_rather_than_wrapped_short) {
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 50, kMidiHeld - 10));
    run(1000);
    CHECK_EQ(g_sink.count, 1);
}

TEST(midiout_release_is_by_note_and_track) {
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 0, kMidiHeld));
    CHECK(g_out.note_on(g_sink, 0, 1, 64, 100, 0, kMidiHeld));
    CHECK(g_out.note_on(g_sink, 1, 2, 60, 100, 0, kMidiHeld));
    g_out.release(g_sink, 0, 60);
    CHECK_EQ(g_out.sounding(0), 1);              // 64 still held
    CHECK_EQ(g_out.sounding(1), 1);              // another track's 60 is not this key's
    CHECK(g_sink.is(3, 0x80, 60, 0));
    g_out.release(g_sink, 0, 61);                // nothing to let go: nothing sent
    CHECK_EQ(g_sink.count, 4);
}

TEST(midiout_release_track_ends_that_track_only) {
    fresh();
    for (uint8_t n = 60; n < 66; n++) CHECK(g_out.note_on(g_sink, 6, 7, n, 100, 0, 5000));
    CHECK(g_out.note_on(g_sink, 2, 3, 36, 100, 0, 5000));
    g_out.release_track(g_sink, 6);
    CHECK_EQ(g_out.sounding(6), 0);
    CHECK_EQ(g_out.sounding(2), 1);
    for (uint8_t n = 60; n < 66; n++) CHECK_EQ(g_sink.balance(7, n), 0);
}

TEST(midiout_release_all_leaves_every_pitch_balanced) {
    fresh();
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 4; i++) {
            CHECK(g_out.note_on(g_sink, t, uint8_t(t + 1), uint8_t(40 + i * 3), 100, 0,
                                i == 0 ? kMidiHeld : 9000));
        }
    }
    CHECK_EQ(g_out.sounding(), 32);
    g_out.release_all(g_sink);
    CHECK_EQ(g_out.sounding(), 0);
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 4; i++) CHECK_EQ(g_sink.balance(uint8_t(t + 1), uint8_t(40 + i * 3)), 0);
    }
    run(1000);                                   // and nothing is sent twice afterwards
    CHECK_EQ(g_sink.count, 64);
}

TEST(midiout_the_same_pitch_on_the_same_channel_is_ended_before_it_is_played_again) {
    // From two tracks too: the receiver has one voice per channel and pitch, not per track.
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 0, 1000));
    CHECK(g_out.note_on(g_sink, 1, 1, 60, 80, 5, 30));
    CHECK_EQ(g_sink.count, 3);
    CHECK(g_sink.is(1, 0x80, 60, 0));
    CHECK(g_sink.is(2, 0x90, 60, 80));
    CHECK_EQ(g_out.sounding(), 1);
    CHECK_EQ(g_out.sounding(1), 1);              // the slot moved to the newer track
    // And the newer gate rules: the old 1000-frame gate cannot end it late or early.
    run(1);
    CHECK_EQ(g_sink.count, 4);
    CHECK_EQ(g_sink.balance(1, 60), 0);
}

TEST(midiout_a_refused_note_on_records_nothing_to_end) {
    fresh();
    g_sink.full = true;
    CHECK(!g_out.note_on(g_sink, 0, 1, 60, 100, 0, 100));
    CHECK_EQ(g_out.sounding(), 0);
    g_sink.full = false;
    run(10);
    CHECK_EQ(g_sink.count, 0);                   // no stray note-off for a note never sent
}

TEST(midiout_a_refused_note_off_is_retried_every_block_until_it_leaves) {
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 0, 10));
    g_sink.full = true;
    run(5);                                      // due in the first block, refused five times
    CHECK_EQ(g_out.waiting(), 1);
    CHECK_EQ(g_out.sounding(), 1);
    CHECK_EQ(g_sink.refused, 5);
    g_sink.full = false;
    run(1);
    CHECK_EQ(g_out.waiting(), 0);
    CHECK_EQ(g_out.sounding(), 0);
    CHECK_EQ(g_sink.balance(1, 60), 0);
}

TEST(midiout_a_stop_while_the_queue_is_full_still_ends_every_note_later) {
    fresh();
    for (uint8_t n = 0; n < 10; n++) CHECK(g_out.note_on(g_sink, 0, 1, uint8_t(60 + n), 100, 0, kMidiHeld));
    g_sink.full = true;
    g_out.release_all(g_sink);
    CHECK_EQ(g_out.waiting(), 10);
    g_sink.full = false;
    run(1);
    for (uint8_t n = 0; n < 10; n++) CHECK_EQ(g_sink.balance(1, uint8_t(60 + n)), 0);
    CHECK_EQ(g_out.sounding(), 0);
}

TEST(midiout_a_new_note_waits_behind_its_own_stuck_note_off) {
    // Sending the new note-on ahead of the old note-off would let the retried off cut it.
    fresh();
    CHECK(g_out.note_on(g_sink, 0, 1, 60, 100, 0, 10));
    g_sink.full = true;
    run(1);
    g_sink.full = false;
    // Make the queue refuse only the retried off by filling it again just for this call.
    g_sink.full = true;
    CHECK(!g_out.note_on(g_sink, 0, 1, 60, 100, 0, 10));
    g_sink.full = false;
    CHECK_EQ(g_sink.balance(1, 60), 1);          // still the first note, still owed its off
    run(1);
    CHECK_EQ(g_sink.balance(1, 60), 0);
}

TEST(midiout_a_full_table_ends_the_note_nearest_its_end_to_make_room) {
    fresh();
    for (int i = 0; i < MidiOut::kSlots; i++) {
        // Gates 1000 + i*10, except slot 17 which ends soonest.
        const uint32_t gate = i == 17 ? 50 : uint32_t(1000 + i * 10);
        CHECK(g_out.note_on(g_sink, i % 8, uint8_t(1 + i / 8), uint8_t(i), 100, 0, gate));
    }
    CHECK_EQ(g_out.sounding(), MidiOut::kSlots);
    CHECK(g_out.note_on(g_sink, 0, 16, 100, 100, 0, 1000));
    CHECK_EQ(g_out.sounding(), MidiOut::kSlots);
    CHECK_EQ(g_sink.balance(uint8_t(1 + 17 / 8), 17), 0);   // that one was ended
    CHECK_EQ(g_sink.balance(16, 100), 1);
    g_out.release_all(g_sink);
    CHECK_EQ(g_out.sounding(), 0);
}

TEST(midiout_a_full_table_of_stuck_note_offs_refuses_rather_than_steals_one) {
    fresh();
    for (int i = 0; i < MidiOut::kSlots; i++) {
        CHECK(g_out.note_on(g_sink, 0, 1, uint8_t(i), 100, 0, 1));
    }
    g_sink.full = true;
    run(1);
    CHECK_EQ(g_out.waiting(), MidiOut::kSlots);
    g_sink.full = false;
    // Every slot holds an off that has not left; none may be thrown away.
    g_sink.full = true;
    CHECK(!g_out.note_on(g_sink, 1, 2, 100, 100, 0, 10));
    g_sink.full = false;
    run(1);
    CHECK_EQ(g_out.sounding(), 0);
    for (int i = 0; i < MidiOut::kSlots; i++) CHECK_EQ(g_sink.balance(1, uint8_t(i)), 0);
}

TEST(midiout_every_pitch_balances_over_a_long_random_run) {
    // A fuzz of everything the engine can do, in random order, with the queue refusing a
    // quarter of the time. At the end, with the queue free and a stop, every channel and
    // pitch must have had as many offs as ons -- the no-hanging-note property in one number.
    fresh();
    uint32_t seed = 12345;
    auto rnd = [&seed](uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    for (int block = 0; block < 20000; block++) {
        g_sink.full = rnd(4) == 0;
        g_out.begin_block(g_sink, 64);
        const uint32_t events = rnd(4);
        for (uint32_t e = 0; e < events; e++) {
            const int track = int(rnd(8));
            const uint8_t channel = uint8_t(1 + rnd(3));
            const uint8_t note = uint8_t(60 + rnd(6));
            switch (rnd(10)) {
                case 0: g_out.release(g_sink, track, note); break;
                case 1: g_out.release_track(g_sink, track); break;
                case 2: if (rnd(20) == 0) g_out.release_all(g_sink); break;
                default:
                    g_out.note_on(g_sink, track, channel, note, 100, rnd(64),
                                  rnd(5) == 0 ? kMidiHeld : rnd(3000));
                    break;
            }
        }
        g_out.end_block(64);
    }
    g_sink.full = false;
    g_out.release_all(g_sink);
    run(1);
    CHECK_EQ(g_out.sounding(), 0);
    for (int c = 1; c <= 3; c++) {
        for (int n = 60; n < 66; n++) CHECK_EQ(g_sink.balance(uint8_t(c), uint8_t(n)), 0);
    }
}
