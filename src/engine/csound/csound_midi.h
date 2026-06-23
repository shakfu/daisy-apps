// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// MIDI-note plumbing for the Csound engine (roadmap #3 in docs/dev/csound-impl.md): turn a platform MIDI
// NoteOn into a Csound instrument event so a patch is playable from a keyboard.
//
// Two design constraints come straight from the platform contract (src/ui/core.ui.midi.cpp):
//   1. NoteOn ONLY - the engine never sees NoteOff and gets no velocity. So a note must be
//      self-terminating: each NoteOn schedules a finite-duration instrument instance (a pluck/ping),
//      not a held-until-release voice.
//   2. handle_midi_note() runs in the MAIN LOOP, while csoundPerformKsmps runs in the AUDIO ISR.
//      Calling csoundEvent (which allocates) from the main loop would race the ISR on the
//      non-thread-safe SDRAM pool (see csound_pool.h). So handle_midi_note only enqueues onto a
//      lock-free SPSC ring here; the engine drains it INSIDE process() (the ISR) right before
//      csoundPerformKsmps, keeping all Csound allocation single-threaded.
//
// This header is the host-testable half (no libcsound): the note->frequency map, the p-field
// builder, and the ring. The engine (csound_engine.cpp) owns the csoundEvent / csoundGetInstrNumber
// calls. host/test_csound_midi.cpp exercises everything here.

#include "engine/midi_note.h"   // midi_note_to_hz, MidiNoteEvent, NoteQueue (shared with the ChucK engine)

#include <cstdint>

namespace daisyapps {

// The well-known instrument name a patch defines to be MIDI-playable. The engine looks it up with
// csoundGetInstrNumber; a patch that doesn't define it simply isn't keyboard-playable (NoteOns are
// dropped). The built-in fallback orchestra defines it.
inline constexpr const char* kMidiInstrName = "MidiNote";

// Sounding duration (seconds) of a MIDI note. Fixed, because the platform delivers no NoteOff - the
// instrument's envelope must fit within this and self-terminate. ~0.6 s reads as a pluck/mallet.
inline constexpr float kMidiNoteDur = 0.6f;

// p-field count the builder emits: p1 instr, p2 start, p3 dur, p4 freq, p5 deck.
inline constexpr int kMidiNoteFields = 5;

// Build the CS_INSTR_EVENT p-fields to instantiate the MIDI-note instrument:
//   p1 = instr number, p2 = 0 (start now), p3 = dur, p4 = frequency Hz, p5 = deck (0 = A, 1 = B).
// (No velocity p-field: the platform's NoteOn handler passes none. The instrument takes its level/
// timbre from the engine's control channels.) `out` must hold >= kMidiNoteFields floats; returns the
// count written. The engine widens these to MYFLT for csoundEvent.
inline int csound_note_pfields(float* out, int instr, float dur, uint8_t note, int deck) {
    out[0] = static_cast<float>(instr);
    out[1] = 0.0f;
    out[2] = dur;
    out[3] = midi_note_to_hz(note);
    out[4] = static_cast<float>(deck);
    return kMidiNoteFields;
}

} // namespace daisyapps
