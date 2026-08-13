#pragma once

// Shared MIDI event -> raw 3-byte message conversion, used by every board driver whose target has a
// UART MIDI input (Pod, Daisy Patch). libDaisy hands back a parsed daisy::MidiEvent; IEngine's
// handle_midi_message wants the wire form (status byte incl. channel, data1, data2), so the mapping
// belongs in one place rather than once per board.

#include "daisy.h"

namespace daisyapps {

// The MIDI status byte (including the channel nibble) for a channel-voice event, the realtime status
// for a forwarded system-realtime message, or 0 for events not representable as a 3-byte message (the
// caller skips those - SysEx and system-common). The daisy MidiMessageType enum is ordered
// NoteOff..PitchBend, i.e. status nibbles 0x80..0xE0; ChannelMode is control change (0xB0).
inline uint8_t midi_status_byte(const daisy::MidiEvent& ev)
{
    switch (ev.type) {
        case daisy::NoteOff:
        case daisy::NoteOn:
        case daisy::PolyphonicKeyPressure:
        case daisy::ControlChange:
        case daisy::ProgramChange:
        case daisy::ChannelPressure:
        case daisy::PitchBend:
            return static_cast<uint8_t>((0x80 + (static_cast<int>(ev.type) << 4)) | (ev.channel & 0x0f));
        case daisy::ChannelMode:
            return static_cast<uint8_t>(0xB0 | (ev.channel & 0x0f));   // channel-mode = CC 120..127
        case daisy::SystemRealTime:
            switch (ev.srt_type) {
                case daisy::TimingClock: return 0xF8;
                case daisy::Start:       return 0xFA;
                case daisy::Continue:    return 0xFB;
                case daisy::Stop:        return 0xFC;
                default:                 return 0;
            }
        default:
            return 0;
    }
}

// Drain a libDaisy UART MIDI handler into `sink(status, data1, data2)`. Shared by the board drivers'
// PollMidi(): the handler type differs only nominally between BSPs, so it is a template parameter.
template <typename MidiHandler, typename Sink>
inline void poll_midi_handler(MidiHandler& midi, Sink&& sink)
{
    midi.Listen();
    while (midi.HasEvents()) {
        daisy::MidiEvent ev     = midi.PopEvent();
        const uint8_t    status = midi_status_byte(ev);
        if (!status)        continue;                        // not a 3-byte-representable message
        if (status >= 0xF8) sink(status, 0, 0);              // system realtime: status byte only
        else                sink(status, ev.data[0], ev.data[1]);
    }
}

} // namespace daisyapps
