#pragma once

#include "daisy_pod.h"
#include "board/controls.h"
#include "board/midi_status.h"

namespace daisyapps {

// Daisy Pod - the minimal target: 2 pots, 1 encoder (+click), 2 buttons, 2 RGB LEDs, stereo audio,
// onboard MCU LED. No CV/gate I/O and no display. Wraps daisy::DaisyPod.
class PodBoard {
public:
    static constexpr int  kAnalogCount    = 2;   // KNOB_1, KNOB_2
    static constexpr int  kButtonCount    = 2;   // BUTTON_1, BUTTON_2
    static constexpr int  kGateCount      = 0;
    static constexpr int  kIndicatorCount = 2;   // led1, led2 (RGB)
    static constexpr int  kAudioChannels  = 2;   // stereo in/out
    static constexpr int  kCvOutCount     = 0;   // no CV outputs on the Pod
    static constexpr bool kHasScreen      = false;
    static constexpr int  kScreenWidth    = 0;
    static constexpr int  kScreenHeight   = 0;

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
    void PollMidi(Sink&& sink) { poll_midi_handler(hw_.midi, sink); }

    // --- Outputs the Pod does not have. No-ops so shared harness code compiles and degrades
    //     gracefully, exactly as SetIndicator does on a board without LEDs. ----------------------
    void SetCvOut(int, float) {}
    void SetGateOut(bool) {}

    // --- Screen: the Pod has none, so the whole text/rect facade is inert (see patch_board.h). ---
    void ScreenClear() {}
    void ScreenText(int, int, const char*, bool = true) {}
    void ScreenRect(int, int, int, int, bool) {}
    void ScreenUpdate() {}

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
    // The board's QSPI flash handle, as the void* EngineContext::qspi expects. The contract keeps this
    // opaque so it names no HAL type; the one engine that uses it (edrums, for kit presets) casts it
    // back to daisy::QSPIHandle* in target-only code.
    void* Qspi() { return &hw_.seed.qspi; }

    void SetUserLed(bool on) { hw_.seed.SetLed(on); }

    // Escape hatch for board-specific needs (MIDI, raw seed access) the abstraction does not cover.
    daisy::DaisyPod& hw() { return hw_; }

private:
    daisy::DaisyPod hw_;
};

} // namespace daisyapps
