#pragma once

#include "daisy_pod.h"
#include "board/controls.h"

namespace daisyapps {

// Daisy Pod - the minimal target: 2 pots, 1 encoder (+click), 2 buttons, 2 RGB LEDs, stereo audio,
// onboard MCU LED. No CV/gate I/O and no display. Wraps daisy::DaisyPod.
class PodBoard {
public:
    static constexpr int kAnalogCount    = 2;   // KNOB_1, KNOB_2
    static constexpr int kButtonCount    = 2;   // BUTTON_1, BUTTON_2
    static constexpr int kGateCount      = 0;
    static constexpr int kIndicatorCount = 2;   // led1, led2 (RGB)

    // Bring up the Pod BSP (seed + SDRAM + pod controls), set the audio block size + 48 kHz, and start
    // the ADC. VTOR / QSPI-boot setup stays in the harness (it is bootloader concern, not board).
    void Init(int block_size)
    {
        hw_.Init();
        hw_.SetAudioBlockSize(static_cast<size_t>(block_size));
        hw_.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
        hw_.StartAdc();
    }

    void  StartAudio(daisy::AudioHandle::AudioCallback cb) { hw_.StartAudio(cb); }
    float SampleRate() { return hw_.AudioSampleRate(); }

    // ISR-safe: the last processed value (0..1) of analog control i. Refreshed by Poll() in the main
    // loop (ProcessAnalogControls); reading it from the audio callback is a benign single-float race.
    float Analog(int i)
    {
        switch (i) {
            case 0:  return hw_.GetKnobValue(daisy::DaisyPod::KNOB_1);
            case 1:  return hw_.GetKnobValue(daisy::DaisyPod::KNOB_2);
            default: return 0.f;
        }
    }

    // Main loop: debounce + refresh every control, then fill the board-agnostic snapshot.
    void Poll(Controls& c)
    {
        hw_.ProcessAllControls();
        c.analog_count = kAnalogCount;
        c.analog[0]    = hw_.GetKnobValue(daisy::DaisyPod::KNOB_1);
        c.analog[1]    = hw_.GetKnobValue(daisy::DaisyPod::KNOB_2);
        c.enc_inc      = hw_.encoder.Increment();
        c.enc_press    = hw_.encoder.Pressed();
        c.button_count = kButtonCount;
        c.button[0]    = hw_.button1.Pressed();
        c.button[1]    = hw_.button2.Pressed();
        c.gate_count   = kGateCount;
    }

    // --- MIDI (the Pod has a TRS/DIN MIDI input on its UART) ------------------------------------
    // Begin receiving MIDI. Call once after Init() (which already configured the UART MIDI handler),
    // before the main loop.
    void StartMidi() { hw_.midi.StartReceive(); }

    // Main loop: parse buffered MIDI and forward each representable message to sink(status, data1, data2)
    // - the full channel-voice stream (NoteOn/Off with real velocity, control change, pitch-bend, poly/
    // channel aftertouch, program change) plus system realtime (clock/start/continue/stop). `status` is
    // the raw MIDI status byte, including the channel nibble for channel-voice messages; data1/data2 are
    // the message data bytes (0 where unused). SysEx / system-common aren't representable in a 3-byte
    // message and are skipped. `sink` is any callable taking (uint8_t status, uint8_t data1, uint8_t
    // data2) - e.g. a lambda forwarding to engine.handle_midi_message (ChucK), or extracting NoteOn for
    // engine.handle_midi_note (Csound). Channels are 0-based (0 = MIDI channel 1).
    template <typename Sink>
    void PollMidi(Sink&& sink)
    {
        hw_.midi.Listen();
        while (hw_.midi.HasEvents()) {
            daisy::MidiEvent ev = hw_.midi.PopEvent();
            const uint8_t status = midi_status_byte(ev);
            if (!status)            continue;                       // not a 3-byte-representable message
            if (status >= 0xF8)     sink(status, 0, 0);            // system realtime: status byte only
            else                    sink(status, ev.data[0], ev.data[1]);
        }
    }

    // RGB indicator (idx 0 = led1, 1 = led2). Out-of-range index is a no-op, so engine/harness code
    // that addresses more indicators than a board has degrades gracefully.
    void SetIndicator(int idx, float r, float g, float b)
    {
        if (idx == 0)      hw_.led1.Set(r, g, b);
        else if (idx == 1) hw_.led2.Set(r, g, b);
        else               return;
        hw_.UpdateLeds();
    }

    // Onboard MCU LED - present on every Daisy; used for bring-up heartbeats.
    void SetUserLed(bool on) { hw_.seed.SetLed(on); }

    // Escape hatch for board-specific needs (MIDI, raw seed access) the abstraction does not cover.
    daisy::DaisyPod& hw() { return hw_; }

private:
    // MIDI status byte (incl. channel) for a channel-voice MidiEvent, the realtime status for the
    // forwarded system-realtime messages, or 0 for events not representable as a 3-byte message (the
    // caller skips those). The daisy MidiMessageType enum is ordered NoteOff..PitchBend, i.e. status
    // nibbles 0x80..0xE0; ChannelMode is control change (0xB0).
    static uint8_t midi_status_byte(const daisy::MidiEvent& ev)
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

    daisy::DaisyPod hw_;
};

} // namespace daisyapps
