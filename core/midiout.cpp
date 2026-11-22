// MIDI note out. core/midiout.hpp has the reasoning; this is the bookkeeping.
#include "midiout.hpp"

namespace strata {

void midi_message(uint8_t *out, uint8_t status_high, uint8_t channel, uint8_t a, uint8_t b) {
    out[0] = uint8_t((status_high & 0xF0u) | ((channel - 1u) & 0x0Fu));
    out[1] = uint8_t(a & 0x7Fu);
    out[2] = uint8_t(b & 0x7Fu);
}

int MidiOut::find(uint8_t channel, uint8_t note) const {
    for (int i = 0; i < kSlots; i++) {
        const Slot &s = slots_[i];
        if (s.used && s.channel == channel && s.note == note) return i;
    }
    return -1;
}

void MidiOut::end(MidiSink &sink, Slot &slot) {
    uint8_t bytes[3];
    midi_message(bytes, 0x80u, slot.channel, slot.note, 0);
    if (sink.send(bytes, 3)) {
        slot = Slot{};
    } else {
        slot.ending = true;   // retried by begin_block until it leaves
    }
}

int MidiOut::free_slot(MidiSink &sink) {
    for (int i = 0; i < kSlots; i++) {
        if (!slots_[i].used) return i;
    }
    // Full. End the note nearest its own end -- the one losing least -- but never one already
    // ending, which is exactly the note-off the queue has no room for.
    int victim = -1;
    for (int i = 0; i < kSlots; i++) {
        const Slot &s = slots_[i];
        if (s.ending) continue;
        if (victim < 0 || s.left < slots_[victim].left) victim = i;
    }
    if (victim < 0) return -1;
    end(sink, slots_[victim]);
    return slots_[victim].used ? -1 : victim;
}

bool MidiOut::note_on(MidiSink &sink, int track, uint8_t channel, uint8_t note,
                      uint8_t velocity, uint32_t frame, uint32_t gate) {
    if (channel < 1 || channel > kMidiChannels || track < 0 || track > 255) return false;
    note = uint8_t(note & 0x7Fu);
    // A velocity of 0 is a note-off on the wire. A trig always sounds, so it is at least 1.
    velocity = uint8_t(velocity & 0x7Fu);
    if (velocity == 0) velocity = 1;

    // This pitch is already sounding on this channel: end it first, so the receiver hears a new
    // note rather than a second note-on it may ignore -- and so the old gate cannot cut this one.
    int index = find(channel, note);
    if (index >= 0) {
        end(sink, slots_[index]);
        if (slots_[index].used) return false;   // its note-off is stuck: this note waits too
    } else {
        index = free_slot(sink);
        if (index < 0) return false;
    }

    uint8_t bytes[3];
    midi_message(bytes, 0x90u, channel, note, velocity);
    if (!sink.send(bytes, 3)) return false;      // nothing sounding, so nothing to end

    Slot &s = slots_[index];
    s.used = true;
    s.ending = false;
    s.track = uint8_t(track);
    s.channel = channel;
    s.note = note;
    // From the start of this block. A gate so long it would pass kMidiHeld is held, which at
    // 48 kHz is a day.
    s.left = gate == kMidiHeld || gate > kMidiHeld - frame ? kMidiHeld : frame + gate;
    return true;
}

void MidiOut::release(MidiSink &sink, int track, uint8_t note) {
    note = uint8_t(note & 0x7Fu);
    for (int i = 0; i < kSlots; i++) {
        Slot &s = slots_[i];
        if (s.used && !s.ending && s.track == track && s.note == note) end(sink, s);
    }
}

void MidiOut::release_track(MidiSink &sink, int track) {
    for (int i = 0; i < kSlots; i++) {
        Slot &s = slots_[i];
        if (s.used && !s.ending && s.track == track) end(sink, s);
    }
}

void MidiOut::release_all(MidiSink &sink) {
    for (int i = 0; i < kSlots; i++) {
        Slot &s = slots_[i];
        if (s.used && !s.ending) end(sink, s);
    }
}

void MidiOut::begin_block(MidiSink &sink, uint32_t frames) {
    for (int i = 0; i < kSlots; i++) {
        Slot &s = slots_[i];
        if (!s.used) continue;
        if (s.ending || (s.left != kMidiHeld && s.left < frames)) end(sink, s);
    }
}

void MidiOut::end_block(uint32_t frames) {
    for (int i = 0; i < kSlots; i++) {
        Slot &s = slots_[i];
        if (!s.used || s.ending || s.left == kMidiHeld) continue;
        s.left = s.left > frames ? s.left - frames : 0;
    }
}

int MidiOut::sounding() const {
    int n = 0;
    for (int i = 0; i < kSlots; i++) n += slots_[i].used ? 1 : 0;
    return n;
}

int MidiOut::sounding(int track) const {
    int n = 0;
    for (int i = 0; i < kSlots; i++) n += slots_[i].used && slots_[i].track == track ? 1 : 0;
    return n;
}

int MidiOut::waiting() const {
    int n = 0;
    for (int i = 0; i < kSlots; i++) n += slots_[i].used && slots_[i].ending ? 1 : 0;
    return n;
}

void MidiOut::clear() {
    for (int i = 0; i < kSlots; i++) slots_[i] = Slot{};
}

}  // namespace strata
