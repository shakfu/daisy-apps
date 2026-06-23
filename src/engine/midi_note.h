#pragma once

// Generic MIDI-note plumbing shared by the engines that accept NoteOn (Csound, ChucK). The platform
// delivers NoteOn ONLY (no NoteOff, no velocity), on the MAIN LOOP, while the engine consumes notes in
// the AUDIO ISR (process()). So an engine enqueues here from handle_midi_note() and drains in process(),
// keeping all engine/VM mutation single-threaded. The note->frequency map and the lock-free ring are
// engine-agnostic; each engine adds its own delivery (Csound: a finite instr event; ChucK: a global
// frequency + a broadcast Event). See engine/csound/csound_midi.h for the Csound-specific half.

#include <atomic>
#include <cmath>
#include <cstdint>

namespace daisyapps {

// MIDI note number -> frequency in Hz (12-TET, note 69 = A4 = 440 Hz).
inline float midi_note_to_hz(uint8_t note) {
    return 440.0f * std::pow(2.0f, (static_cast<int>(note) - 69) * (1.0f / 12.0f));
}

// One pending MIDI note: the note number and which deck (0/1) it resolved to.
struct MidiNoteEvent { uint8_t note; uint8_t deck; };

// Lock-free single-producer / single-consumer ring for pending MIDI notes. Producer = the main loop
// (handle_midi_note); consumer = the audio ISR (process(), draining before the engine's compute call).
// On overflow it drops the newest note (a burst exceeding one audio block's worth is not musically
// meaningful, and dropping never blocks the producer). N must be a power of two.
template <uint32_t N>
class NoteQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two >= 2");
public:
    // Producer side. Returns false (dropped) if the ring is full.
    bool push(MidiNoteEvent e) {
        const uint32_t h = _head.load(std::memory_order_relaxed);
        const uint32_t t = _tail.load(std::memory_order_acquire);
        if (h - t >= N) return false;                 // full
        _buf[h & (N - 1)] = e;
        _head.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer side. Returns false (and leaves `out` untouched) if the ring is empty.
    bool pop(MidiNoteEvent& out) {
        const uint32_t t = _tail.load(std::memory_order_relaxed);
        const uint32_t h = _head.load(std::memory_order_acquire);
        if (t == h) return false;                     // empty
        out = _buf[t & (N - 1)];
        _tail.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    MidiNoteEvent         _buf[N];
    std::atomic<uint32_t> _head{0};
    std::atomic<uint32_t> _tail{0};
};

} // namespace daisyapps
