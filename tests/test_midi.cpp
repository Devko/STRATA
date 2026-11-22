// MIDI note out through the engine (app/engine.cpp over core/midiout), against the fake host's
// MIDI OUT. core/midiout's own tests hold the bookkeeping; these hold the engine to calling it
// at every place a note can start or must end: a step, a chord, a pad, a held note, a mute, a
// channel or machine change, a kit recall, both STOPs -- and to MID playing nothing here.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::pump;
using strata::test::put_trig;

namespace {

constexpr uint8_t kMid = uint8_t(MachineId::midi);

Engine &eng() { return strata::test::engine(); }
FakeHost &host() { return FakeHost::the(); }

void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
          uint8_t c = 0) {
    strata::test::post(cmd, track, step, a, b, c);
}

Step &raw_step(int track, int step) {
    return eng().project_for_load().live().tracks[track].steps[step];
}

// A fresh engine with MIDI OUT's log empty: `fresh()` posts a STOP, which may end notes the
// previous test left, and those offs belong to that test.
void start(int track = 0, uint8_t channel = 0, uint8_t machine = 0xFF) {
    fresh();
    if (machine != 0xFF) post(Cmd::set_machine, uint8_t(track), 0, machine);
    if (channel != 0) post(Cmd::set_midi_out, uint8_t(track), 0, channel);
    pump(2);
    host().clear_midi();
}

int ons(int channel, int note = -1) { return host().midi_messages(0x90, channel, note); }
int offs(int channel, int note = -1) { return host().midi_messages(0x80, channel, note); }
int ccs(int channel, int cc = -1) { return host().midi_messages(0xB0, channel, cc); }

// One step at tick 0, frame 0, on a running clock.
void play_step0() {
    host().set_running(true);
    host().deliver_tick(0, 0);
    pump();
}

}  // namespace

TEST(midi_out_a_track_with_no_channel_sends_nothing) {
    start();
    put_trig(0, 0);
    play_step0();
    pump(100);
    CHECK_EQ(host().midi_count(), 0);
}

TEST(midi_out_a_step_sends_its_note_and_ends_it_after_its_gate) {
    // Track 7 is VA MONO in the default kit's place; its note is folded into the key, and MIDI
    // out sends exactly what the machine was handed.
    start(0, 5);
    put_trig(0, 0);
    raw_step(0, 0).note = 48;
    raw_step(0, 0).velocity = 90;
    raw_step(0, 0).length = 8;               // one step
    play_step0();
    CHECK_EQ(ons(5), 1);
    CHECK_EQ(int(host().midi_sent(0).status), 0x94);
    CHECK_EQ(int(host().midi_sent(0).a), int(eng().sent_note(0)));
    CHECK_EQ(int(host().midi_sent(0).b), 90);
    CHECK_EQ(offs(5), 0);

    // One step at 130 BPM is 24 ticks of ~230.8 frames: 5,538 frames, 86.5 blocks. The off
    // comes in the block that holds that frame, not before.
    pump(85);
    CHECK_EQ(offs(5), 0);
    pump(2);
    CHECK_EQ(offs(5), 1);
    CHECK_EQ(eng().midi_notes().sounding(), 0);
}

TEST(midi_out_an_accent_is_the_top_velocity) {
    start(0, 1);
    put_trig(0, 0);
    raw_step(0, 0).flags = uint8_t(raw_step(0, 0).flags | kStepAccent);
    raw_step(0, 0).velocity = 60;
    play_step0();
    CHECK_EQ(ons(1), 1);
    CHECK_EQ(int(host().midi_sent(0).b), 127);
}

TEST(midi_out_mid_plays_no_audio_and_sends_the_note_as_written) {
    start(2, 10, kMid);
    CHECK(eng().machine(2) != nullptr);
    put_trig(2, 0);
    raw_step(2, 0).note = 37;                // C#, outside C minor: MID does not fold
    play_step0();
    CHECK_EQ(ons(10, 37), 1);
    // Nothing rendered: the track is not counted as sounding, and the machine says idle.
    CHECK(!eng().machine(2)->active());
    CHECK_EQ(int(eng().status().voices.load()), 0);
}

TEST(midi_out_mid_sends_a_chord_step_as_its_chord) {
    start(3, 4, kMid);
    put_trig(3, 0);
    raw_step(3, 0).note = 60;
    raw_step(3, 0).chord = uint8_t(ChordType::minor);
    play_step0();
    CHECK_EQ(ons(4), 3);
    CHECK_EQ(ons(4, 60), 1);
    pump(200);
    CHECK_EQ(offs(4), 3);
}

TEST(midi_out_va_poly_sends_the_whole_chord) {
    start(6, 2, uint8_t(MachineId::va_poly));
    put_trig(6, 0);
    raw_step(6, 0).note = 60;
    raw_step(6, 0).chord = uint8_t(ChordType::minor);
    play_step0();
    CHECK_EQ(ons(2), 3);
}

TEST(midi_out_muting_the_track_ends_its_notes_at_once) {
    start(0, 3);
    put_trig(0, 0);
    raw_step(0, 0).length = 20;              // 64 steps: it would not end by itself here
    play_step0();
    CHECK_EQ(ons(3), 1);
    post(Cmd::set_mute, 0, 0, 1);
    pump();
    CHECK_EQ(offs(3), 1);
    CHECK_EQ(eng().midi_notes().sounding(), 0);
}

TEST(midi_out_a_muted_track_sends_nothing) {
    start(0, 3);
    post(Cmd::set_mute, 0, 0, 1);
    pump();
    put_trig(0, 0);
    play_step0();
    post(Cmd::trigger_note, 0, 0, 60, 100);
    pump();
    CHECK_EQ(ons(3), 0);
}

TEST(midi_out_stop_ends_every_note_and_the_second_stop_sends_all_notes_off) {
    start(0, 3);
    post(Cmd::set_midi_out, 1, 0, 9);
    pump();
    host().clear_midi();
    put_trig(0, 0);
    put_trig(1, 0);
    raw_step(0, 0).length = 20;
    raw_step(1, 0).length = 20;
    play_step0();
    CHECK_EQ(ons(3) + ons(9), 2);

    post(Cmd::stop, 0, 0, 0);                // the first press: notes end, no CC
    pump();
    CHECK_EQ(offs(3), 1);
    CHECK_EQ(offs(9), 1);
    CHECK_EQ(ccs(3), 0);

    post(Cmd::stop, 0, 0, 1);                // the second press, and FUNC+F6: All Notes Off
    pump();
    CHECK_EQ(ccs(3, 123), 1);
    CHECK_EQ(ccs(9, 123), 1);
    CHECK_EQ(ccs(1), 0);                     // only on channels a track uses
    CHECK_EQ(offs(3), 1);                    // and nothing ended twice
}

TEST(midi_out_two_tracks_on_one_channel_send_all_notes_off_once) {
    start(0, 7);
    post(Cmd::set_midi_out, 4, 0, 7);
    pump();
    host().clear_midi();
    post(Cmd::stop, 0, 0, 1);
    pump();
    CHECK_EQ(ccs(7, 123), 1);
}

TEST(midi_out_changing_the_channel_ends_the_notes_on_the_old_one) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 62, 100, 1);    // held
    pump();
    CHECK_EQ(ons(3, 62), 1);
    post(Cmd::set_midi_out, 0, 0, 0, 2, kDeltaFlag);   // 3 -> 5
    pump();
    CHECK_EQ(offs(3, 62), 1);
    CHECK_EQ(int(eng().midi_out(0)), 5);
    post(Cmd::trigger_note, 0, 0, 62, 100, 1);
    pump();
    CHECK_EQ(ons(5, 62), 1);
}

TEST(midi_out_the_channel_knob_stops_at_off_and_sixteen) {
    start(0, 0);
    post(Cmd::set_midi_out, 0, 0, 0, uint8_t(int8_t(-3)), kDeltaFlag);
    pump();
    CHECK_EQ(int(eng().midi_out(0)), 0);
    post(Cmd::set_midi_out, 0, 0, 0, 40, kDeltaFlag);
    pump();
    CHECK_EQ(int(eng().midi_out(0)), 16);
    post(Cmd::set_midi_out, 0, 0, 200);              // an absolute value past the end
    pump();
    CHECK_EQ(int(eng().midi_out(0)), 16);
    CHECK(eng().project().settings.midi_out[0] == 16);   // what a save writes
}

TEST(midi_out_switching_the_machine_ends_the_tracks_notes) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    pump();
    post(Cmd::set_machine, 0, 0, kMid);
    pump();
    CHECK_EQ(offs(3, 60), 1);
}

TEST(midi_out_a_kit_recall_ends_every_tracks_notes) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    pump();
    post(Cmd::kit_recall);
    pump();
    CHECK_EQ(offs(3, 60), 1);
}

TEST(midi_out_a_pad_lasts_a_step_and_a_held_note_lasts_until_its_release) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 60, 100, 0);    // a pad
    post(Cmd::trigger_note, 0, 0, 64, 100, 1);    // a key
    pump(90);
    CHECK_EQ(offs(3, 60), 1);
    CHECK_EQ(offs(3, 64), 0);
    pump(1000);
    CHECK_EQ(offs(3, 64), 0);
    post(Cmd::release_note, 0, 0, 64);
    pump();
    CHECK_EQ(offs(3, 64), 1);
}

TEST(midi_out_a_full_queue_delays_a_note_off_but_does_not_lose_it) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    pump();
    host().set_midi_full(true);
    post(Cmd::release_note, 0, 0, 60);
    pump(10);
    CHECK_EQ(offs(3, 60), 0);
    CHECK(host().midi_refused() > 1);           // retried, block after block
    host().set_midi_full(false);
    pump();
    CHECK_EQ(offs(3, 60), 1);
}

TEST(midi_out_mid_sends_a_cc_when_a_knob_moves_and_not_when_it_becomes_mid) {
    start(0, 6, kMid);
    pump(5);
    CHECK_EQ(ccs(6), 0);                         // eight arbitrary values are NOT thrown at it

    post(Cmd::set_param, 0, 0, 0, 100);          // SRC slot 0: CC 74
    pump();
    CHECK_EQ(ccs(6, 74), 1);
    CHECK_EQ(int(host().midi_sent(host().midi_count() - 1).b), 100);
    pump(10);
    CHECK_EQ(ccs(6, 74), 1);                     // once per move, not per block

    post(Cmd::set_param, 0, 0, 9, 20);           // AMP slot 1: CC 75
    pump();
    CHECK_EQ(ccs(6, 75), 1);
    post(Cmd::set_param, 0, 0, 4, 20);           // FLTR: the insert chain's, no CC
    pump();
    CHECK_EQ(host().midi_count(), 2);
}

TEST(midi_out_mid_sends_a_locked_value_as_a_cc_and_goes_back_after_the_step) {
    // The lock is on step 2, so it is not the playing step when it is written (a lock on the
    // playing step is heard at once, which would send it before the step comes round).
    start(0, 6, kMid);
    pump(2);
    put_trig(0, 1);
    post(Cmd::lock_delta, 0, 1, 1, uint8_t(int8_t(30)));   // CC 71 locked to 64 + 30
    pump();
    host().clear_midi();
    play_step0();
    CHECK_EQ(ccs(6, 71), 0);
    host().clear_midi();
    host().deliver_tick(24, 0);                   // step 2
    pump();
    CHECK_EQ(ccs(6, 71), 1);
    // The CC goes BEFORE the note, so the synth plays the note with it.
    CHECK_EQ(int(host().midi_sent(0).status), 0xB5);
    CHECK_EQ(int(host().midi_sent(0).b), 94);
    CHECK_EQ(int(host().midi_sent(1).status), 0x95);
    post(Cmd::stop);                              // no step playing: the lock lets go
    pump();
    CHECK_EQ(ccs(6, 71), 2);
    CHECK_EQ(int(host().midi_sent(host().midi_count() - 1).b), 64);
}

TEST(midi_out_a_track_that_is_not_mid_sends_no_ccs) {
    start(0, 6);
    post(Cmd::set_param, 0, 0, 0, 100);
    pump(3);
    CHECK_EQ(ccs(6), 0);
}

TEST(midi_out_a_mid_track_with_no_channel_sends_nothing_at_all) {
    start(0, 0, kMid);
    put_trig(0, 0);
    play_step0();
    post(Cmd::set_param, 0, 0, 0, 100);
    pump(3);
    CHECK_EQ(host().midi_count(), 0);
}

TEST(midi_out_the_same_pitch_retriggered_is_ended_first) {
    start(0, 3);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    pump();
    CHECK_EQ(host().midi_count(), 3);
    CHECK_EQ(int(host().midi_sent(1).status), 0x82);
    CHECK_EQ(int(host().midi_sent(2).status), 0x92);
}

// --- the arp and note repeat ---------------------------------------------------------------

namespace {

// The ARP page's four knobs on `track`, at the bottom of each zone (as test_engine's set_arp).
void set_arp(uint8_t track, int mode, int rate) {
    auto smallest = [](int zone, int zones) { return uint8_t((zone * 128 + zones - 1) / zones); };
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 0), smallest(mode, int(ArpMode::count)));
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 1), smallest(rate, kArpRates));
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 2), 0);
    post(Cmd::set_param, track, 0, uint8_t(kArpFirst + 3), 0);
    pump();
}

}  // namespace

TEST(midi_out_each_arp_hit_is_a_note_with_its_own_note_off) {
    start(0, 4, kMid);
    put_trig(0, 0);
    set_arp(0, 1, 5);                              // UP, 1/32: hits on ticks 0 and 12
    host().clear_midi();
    host().set_running(true);
    for (uint32_t tick = 0; tick < 24; tick++) {
        host().deliver_tick(tick, 0);
        pump();
    }
    CHECK_EQ(ons(4), 2);
    pump(400);
    CHECK_EQ(offs(4), 2);                          // each hit ended, none hanging
    host().set_running(false);
}

TEST(midi_out_leaving_the_app_ends_every_note) {
    // App::stop: a held note and a gated one both end, with All Notes Off after them, because
    // no STRATA panel is left to stop a note that hangs on the outboard synth.
    start(0, 5);
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);       // a held KEYS note
    post(Cmd::trigger_note, 0, 0, 64, 100, 0);       // a pad: a step's gate
    pump();
    CHECK_EQ(ons(5), 2);
    CHECK_EQ(eng().midi_notes().sounding(), 2);
    eng().end_midi_for_shutdown();
    CHECK_EQ(offs(5, 60), 1);
    CHECK_EQ(offs(5, 64), 1);
    CHECK_EQ(ccs(5, kMidiAllNotesOff), 1);
    CHECK_EQ(eng().midi_notes().sounding(), 0);
}

TEST(midi_out_mid_arpeggiates_a_chord_step_through_its_chord) {
    // MID sends a chord step as its chord (above), so with the arp on it runs THROUGH the chord,
    // as VA POLY does. It used to take only the step's root: the chord was lost to one note
    // repeated at the arp's rate.
    start(3, 4, kMid);
    put_trig(3, 0);
    raw_step(3, 0).note = 60;
    raw_step(3, 0).chord = uint8_t(ChordType::minor);  // C Eb G
    set_arp(3, 1, 6);                                 // UP, 1/32T: hits on ticks 0, 8 and 16
    host().clear_midi();
    host().set_running(true);
    for (uint32_t tick = 0; tick < 36; tick++) {
        host().deliver_tick(tick, 0);
        pump();
    }
    host().set_running(false);
    pump(400);
    CHECK_EQ(ons(4, 60), 1);
    CHECK_EQ(ons(4, 63), 1);
    CHECK_EQ(ons(4, 67), 1);
    CHECK_EQ(offs(4), ons(4));
}

TEST(midi_out_note_repeat_sends_every_roll_hit_and_ends_each) {
    start(0, 9, kMid);
    host().set_running(false);
    host().set_tempo(120.0f);
    // Beat repeat's length is engine state no fresh() resets: a sixteenth, so 6,000 frames.
    post(Cmd::set_repeat, 0, 0, uint8_t(RepeatLength::sixteenth), 0, 0);
    pump();
    host().clear_midi();
    post(Cmd::note_repeat, 0, 0, 60, 100, 1);
    pump();
    CHECK_EQ(ons(9, 60), 1);                       // the press hits at once
    pump(1000);                                    // over half a second: four more sixteenths
    post(Cmd::note_repeat, 0, 0, 0, 0, 0);
    pump(300);
    CHECK(ons(9, 60) >= 5);
    CHECK_EQ(offs(9, 60), ons(9, 60));
}

TEST(midi_out_an_lfo_on_mid_sends_its_cc_at_a_capped_rate) {
    // Roland, 2026-11-22: an LFO on a MID track moves the synth, through the destination's CC,
    // at most once per Engine::kLfoCcMs (20 ms) so sixteen tracks fit down a DIN cable.
    start(0, 6, kMid);
    pump(2);
    post(Cmd::set_param, 0, 0, uint8_t(kLfoRate), 60);         // free and fast
    post(Cmd::set_param, 0, 0, uint8_t(kLfoDest), lfo_dest_byte(0));   // SRC slot 0: CC 74
    post(Cmd::set_param, 0, 0, uint8_t(kLfoDepth), 127);
    pump();
    host().clear_midi();
    pump(9600 / strata::test::kRigBlock);                       // 200 ms at 48 kHz
    const int sent = ccs(6, 74);
    CHECK(sent >= 8);
    CHECK(sent <= 11);
    int lowest = 127, highest = 0;
    for (int i = 0; i < host().midi_count(); i++) {
        const int b = host().midi_sent(i).b;
        lowest = b < lowest ? b : lowest;
        highest = b > highest ? b : highest;
    }
    CHECK(highest - lowest > 20);                               // it moves the synth

    // Turned off, the CC goes back to where the knob is, at once.
    post(Cmd::set_param, 0, 0, uint8_t(kLfoDepth), 0);
    pump(2);
    CHECK_EQ(int(host().midi_sent(host().midi_count() - 1).b), int(eng().params(0).v[0]));
}
