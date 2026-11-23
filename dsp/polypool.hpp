// The polyphonic machines' shared parts: what the engine's voice budget sees of each, and the
// voice-slot bookkeeping FM and STRING share.
//
// VA POLY (dsp/vapoly.hpp) was the only polyphonic machine, and the engine held its tracks as
// `VaPoly *`. With FM and STRING there are three, all drawing on one budget of 12 voices
// (Engine::kPolyVoiceBudget), so the engine now holds a `PolyMachine *`: the four calls the budget
// and the CPU guard make, and nothing else.
//
// `PolyPool` is VA POLY's slot logic -- allocate, retrigger in place, steal the quietest, a gate
// counted in frames, a pending trig counted from the moment it takes a slot -- written once for
// any voice, so FM and STRING do not each copy it. VA POLY keeps its own copy: its output is
// pinned bit-exact by tests/test_golden.cpp, and moving it is a refactor with nothing to gain.
// vapoly.hpp explains every rule below; this file only names where each is.
//
// The voice a pool holds needs: sounding(), choking(), choke(), amp_level(), gate_off(), kill(),
// and kControl. The machine (Derived, CRTP) supplies:
//
//     void begin(Voice &v, const Trig &t, int slot);   // start the note: pitch, gate_on
//     void step(Voice &v, float *out, int n);          // one slice of n <= kControl frames
//     bool sustains() const;                            // SUS above 0: one-shots let go at once
#pragma once

#include <cstdint>

#include "track.hpp"

namespace strata {

// What the engine's voice budget and CPU guard call on a polyphonic track (app/engine.cpp,
// make_room_for_poly_voice and shed_poly_voices). vapoly.hpp documents each.
class PolyMachine : public Machine {
public:
    // Every polyphonic machine has six voices: the chord cap (core/chord.hpp) is this.
    static constexpr int kVoices = 6;

    virtual int budget_voices() const = 0;
    virtual bool would_add_voice(uint8_t note) const = 0;
    virtual int quietest_voice(float &level) const = 0;
    virtual void choke(int slot) = 0;
};

template <class Derived, class Voice>
class PolyPool : public PolyMachine {
public:
    void trigger(const Trig &trig) override {
        const int pick = allocate(trig.note);
        Slot &s = slots_[pick];
        s.note = int(trig.note);
        s.next = trig;
        s.wait = int(trig.frame > kWaitMax ? kWaitMax : trig.frame);
        s.gate = trig.gate_frames == 0
                     ? -1
                     : int(trig.gate_frames > kGateMax ? kGateMax : trig.gate_frames);
        s.pending = true;
        active_ = true;
    }

    void release(uint8_t note) override {
        for (Slot &s : slots_) {
            if (s.note != int(note)) continue;
            if (s.pending) {
                s.gate = 1;                     // let go one frame after it starts
            } else if (s.voice.sounding()) {
                s.voice.gate_off();
                s.gate = -1;
            }
            s.one_shot = false;
        }
    }

    void release_all() override {
        for (Slot &s : slots_) {
            s.voice.gate_off();
            s.one_shot = false;
        }
    }

    void kill_all() override {
        for (Slot &s : slots_) {
            s.pending = false;
            s.wait = 0;
            s.note = -1;
            s.one_shot = false;
            s.gate = -1;
            s.voice.kill();
        }
        active_ = false;
    }

    void render_add(float *out, int frames) override {
        if (!active_) return;
        bool busy = false;
        for (int i = 0; i < kVoices; i++) {
            if (!slots_[i].busy()) continue;
            render_slot(i, out, frames);
            busy = busy || slots_[i].busy();
        }
        active_ = busy;
    }

    bool active() const override { return active_; }

    int budget_voices() const override {
        int n = 0;
        for (const Slot &s : slots_) n += s.pending || (s.voice.sounding() && !s.voice.choking());
        return n;
    }

    bool would_add_voice(uint8_t note) const override {
        for (const Slot &s : slots_) {
            if (!s.pending && s.note == int(note) && s.voice.sounding() && !s.voice.choking()) {
                return false;
            }
        }
        for (const Slot &s : slots_) {
            if (!s.busy()) return true;
        }
        for (const Slot &s : slots_) {
            if (s.voice.choking() && !s.pending) return true;
        }
        return false;
    }

    int quietest_voice(float &level) const override {
        int pick = -1;
        for (int i = 0; i < kVoices; i++) {
            const Slot &s = slots_[i];
            if (s.pending || !s.voice.sounding() || s.voice.choking()) continue;
            const float l = s.voice.amp_level();
            if (pick < 0 || l < level) {
                pick = i;
                level = l;
            }
        }
        return pick;
    }

    void choke(int slot) override {
        if (slot >= 0 && slot < kVoices) slots_[slot].voice.choke();
    }

    // For the UI and tests.
    int sounding_voices() const {
        int n = 0;
        for (const Slot &s : slots_) n += s.voice.sounding() ? 1 : 0;
        return n;
    }
    int busy_voices() const {
        int n = 0;
        for (const Slot &s : slots_) n += s.busy() ? 1 : 0;
        return n;
    }
    int voice_note(int slot) const {
        if (slot < 0 || slot >= kVoices) return -1;
        return slots_[slot].busy() ? slots_[slot].note : -1;
    }
    const Voice &voice(int slot) const { return slots_[slot].voice; }

protected:
    // Clears every slot; the machine's init() calls it after its voices are set up.
    void reset_slots() {
        for (Slot &s : slots_) {
            s.next = Trig{};
            s.wait = 0;
            s.gate = -1;
            s.pending = false;
            s.note = -1;
            s.one_shot = false;
        }
        steal_cursor_ = 0;
        active_ = false;
    }
    Voice &voice_at(int slot) { return slots_[slot].voice; }

private:
    // vapoly.cpp: a wait is at most a block's worth, a gate at most five minutes.
    static constexpr uint32_t kWaitMax = 65535u;
    static constexpr uint32_t kGateMax = 14400000u;

    struct Slot {
        Voice voice;
        Trig next{};
        int wait = 0;
        bool pending = false;
        int note = -1;
        bool one_shot = false;
        int gate = -1;
        bool busy() const { return pending || voice.sounding(); }
    };

    int allocate(uint8_t note) {
        for (int i = 0; i < kVoices; i++) {   // the same note, sounding: retrigger it in place
            const Slot &s = slots_[i];
            if (!s.pending && s.note == int(note) && s.voice.sounding()) return i;
        }
        for (int i = 0; i < kVoices; i++) {   // a free slot
            if (!slots_[i].busy()) return i;
        }
        int pick = -1;                        // steal the quietest started voice
        float lowest = 0.0f;
        int i = steal_cursor_;
        for (int n = 0; n < kVoices; n++) {
            if (!slots_[i].pending) {
                const float level = slots_[i].voice.amp_level();
                if (pick < 0 || level < lowest) {
                    pick = i;
                    lowest = level;
                }
            }
            if (++i >= kVoices) i = 0;
        }
        if (pick < 0) pick = steal_cursor_;   // every slot pending: the oldest in turn
        steal_cursor_ = pick + 1 >= kVoices ? 0 : pick + 1;
        return pick;
    }

    void start(int slot) {
        Slot &s = slots_[slot];
        const Trig t = s.next;
        s.pending = false;
        s.wait = 0;
        s.note = int(t.note);
        static_cast<Derived *>(this)->begin(s.voice, t, slot);
        s.one_shot = s.gate < 0 && !t.held;
    }

    void render_slot(int slot, float *out, int frames) {
        Slot &s = slots_[slot];
        Derived &self = *static_cast<Derived *>(this);
        int i = 0;
        while (i < frames) {
            if (s.pending && s.wait <= 0) {
                start(slot);
                continue;
            }
            int n = frames - i;
            if (n > Voice::kControl) n = Voice::kControl;
            if (s.pending && s.wait < n) n = s.wait;
            if (!s.pending && s.gate > 0 && s.gate < n) n = s.gate;
            if (!s.pending && s.one_shot && self.sustains()) {
                s.voice.gate_off();
                s.one_shot = false;
            }
            self.step(s.voice, out + i, n);   // a stolen voice still sounds until its trig starts
            if (s.pending) s.wait -= n;
            if (!s.pending && s.gate > 0) {
                s.gate -= n;
                if (s.gate <= 0) {
                    s.voice.gate_off();
                    s.gate = -1;
                }
            }
            i += n;
        }
    }

    Slot slots_[kVoices];
    int steal_cursor_ = 0;
    bool active_ = false;
};

}  // namespace strata
