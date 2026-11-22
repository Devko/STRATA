// MIDI note out: which notes STRATA has sent, and when each one must end.
//
// docs/SPEC.md, "MIDI out". A track with a MIDI OUT channel sends every note it plays -- steps,
// ratchets, chords, pads, KEYS and notes arriving at MIDI IN -- as a note-on on that channel,
// and this module is what guarantees the matching note-off. A hanging note on an outboard synth
// is the one MIDI bug a performer cannot fix from STRATA's panel, so every way a note can end is
// here and host-tested:
//
//   - its GATE runs out (the step's length, counted in frames like a machine's gate),
//   - a key or MIDI IN note-off lets go of a HELD note (KEYS, MIDI IN),
//   - the TRACK is muted, changes channel, or changes machine,
//   - the transport STOPS, which ends everything; the second STOP and FUNC+F6 also send
//     All Notes Off on every channel in use, for gear that missed something.
//
// ## Why a note-off is never lost
//
// Enaga's `midi_send` is all or nothing and returns false when its 2 KiB queue is full
// (<enaga/midi.hpp>). A lost note-ON is a missing note, which is a glitch; a lost note-OFF is a
// note that never ends. So a note-on that does not fit is simply not recorded, while a note-off
// that does not fit keeps its slot, marked `ending`, and is retried every block until it goes.
// A new note on the same channel and pitch waits behind it: it is refused rather than sent
// ahead of the off that would then cut it.
//
// ## One external voice per channel and pitch
//
// Two tracks on one channel playing the same pitch are one voice to the receiver, which knows
// nothing about tracks. So a note-on for a pitch already sounding on that channel -- from any
// track -- sends the note-off first and takes the slot over. Without that the first note's
// gate would end the second note early, and with a held note it would hang.
//
// ## Timing
//
// Gates are kept in frames relative to the START of the current block, the same frame a Trig
// carries. A note-off falls due in the block that contains its frame and is sent at the start
// of that block, so it leaves up to one block (1.33 ms) early -- as every note-on leaves up to
// one block before its frame. Enaga's sends have no timestamp, so this is the resolution there
// is (docs/BUDGET.md, "MIDI out").
//
// core/ never includes Enaga, so the bytes leave through `MidiSink`; app/engine.cpp's sink is
// `enaga::midi::send` and the tests' sink records them.
#pragma once

#include <cstdint>

namespace strata {

// Where the bytes go. One complete message per call, all or nothing, exactly like
// `enaga::midi::send`: false means nothing was queued.
class MidiSink {
public:
    virtual bool send(const uint8_t *bytes, uint32_t count) = 0;

protected:
    ~MidiSink() = default;
};

// A track's MIDI OUT setting, as PROJECT.BIN stores it: 0 is off, 1-16 the channel as printed on
// gear (the wire carries 0-15).
constexpr uint8_t kMidiOutOff = 0;
constexpr uint8_t kMidiChannels = 16;

// A gate of this many frames never runs out: the note is HELD until something lets go of it.
constexpr uint32_t kMidiHeld = 0xFFFFFFFFu;

// Channel-mode CC 123, All Notes Off.
constexpr uint8_t kMidiAllNotesOff = 123;

// The three bytes of a channel message, so the engine and the tests build them one way. Channel
// 1-16, as above; the data bytes are masked to 7 bits as Enaga's own writers do.
void midi_message(uint8_t *out, uint8_t status_high, uint8_t channel, uint8_t a, uint8_t b);

class MidiOut {
public:
    // 8 tracks x a six-note chord, plus held KEYS and MIDI IN notes. Past it, the note nearest
    // to its end is ended to make room: a full table must not refuse the newest note, which is
    // the one the performer is playing.
    static constexpr int kSlots = 64;

    // Send a note-on and remember to end it `gate` frames after `frame` (kMidiHeld: never by
    // itself). `frame` is inside the current block. `channel` is 1-16; anything else sends
    // nothing. False when nothing was sent: the sink was full, or an earlier note-off for this
    // pitch is still waiting to leave.
    bool note_on(MidiSink &sink, int track, uint8_t channel, uint8_t note, uint8_t velocity,
                 uint32_t frame, uint32_t gate);

    // Let go of a held or gated note on `track` (a key or a MIDI IN note-off). By note, like
    // `Machine::release`: whichever started it.
    void release(MidiSink &sink, int track, uint8_t note);

    // End everything `track` has sounding: a mute, a channel or machine change.
    void release_track(MidiSink &sink, int track);

    // End everything: the transport stopped.
    void release_all(MidiSink &sink);

    // The start of a block of `frames`: send every note-off that falls inside it, and retry the
    // ones a full queue refused before.
    void begin_block(MidiSink &sink, uint32_t frames);

    // The end of that block: every gate is now `frames` closer.
    void end_block(uint32_t frames);

    // How many notes are sounding (sent, and their note-off not yet sent), all tracks or one.
    int sounding() const;
    int sounding(int track) const;
    // Note-offs a full queue has refused and that are waiting to be retried.
    int waiting() const;

    void clear();   // forget everything WITHOUT sending: init and tests only

private:
    struct Slot {
        bool used = false;
        bool ending = false;     // the note-off is due and has not left yet
        uint8_t track = 0;
        uint8_t channel = 0;     // 1-16
        uint8_t note = 0;
        uint32_t left = 0;       // frames from the start of this block to the note-off
    };

    // Send the slot's note-off, freeing it if it left and marking it `ending` if not.
    void end(MidiSink &sink, Slot &slot);
    int find(uint8_t channel, uint8_t note) const;
    int free_slot(MidiSink &sink);

    Slot slots_[kSlots];
};

}  // namespace strata
