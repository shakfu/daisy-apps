#pragma once

// SCAFFOLD - not yet build-wired to a harness/Makefile and not compile-verified. Fills in the older
// Daisy Patch target (the Seed-based Eurorack module). See pod_board.h for the reference
// implementation and the Board interface contract.

#include "daisy_patch.h"
#include "board/controls.h"

namespace daisyapps {

// Daisy Patch (Seed-based): 4 pots (CTRL_1..CTRL_4), 1 encoder (+click), 2 gate inputs, CV outputs
// (DAC), and a 128x64 OLED instead of LEDs. The OLED and CV outputs are deferred (see the abstraction
// scope: inputs + no-op indicators). SetIndicator() is therefore a no-op; SetUserLed() drives the
// onboard MCU LED.
class PatchBoard {
public:
    static constexpr int kAnalogCount    = 4;   // CTRL_1..CTRL_4
    static constexpr int kButtonCount    = 0;   // encoder click only (reported via Controls.enc_press)
    static constexpr int kGateCount      = 2;   // GATE_IN_1, GATE_IN_2
    static constexpr int kIndicatorCount = 0;   // OLED, not LEDs (deferred)

    void Init(int block_size)
    {
        hw_.Init();
        hw_.SetAudioBlockSize(static_cast<size_t>(block_size));
        hw_.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
        hw_.StartAdc();
    }

    void  StartAudio(daisy::AudioHandle::AudioCallback cb) { hw_.StartAudio(cb); }
    float SampleRate() { return hw_.AudioSampleRate(); }

    float Analog(int i)
    {
        return (i >= 0 && i < kAnalogCount)
                   ? hw_.GetKnobValue(static_cast<daisy::DaisyPatch::Ctrl>(i))
                   : 0.f;
    }

    void Poll(Controls& c)
    {
        hw_.ProcessAllControls();
        c.analog_count = kAnalogCount;
        for (int i = 0; i < kAnalogCount; i++)
            c.analog[i] = hw_.GetKnobValue(static_cast<daisy::DaisyPatch::Ctrl>(i));
        c.enc_inc      = hw_.encoder.Increment();
        c.enc_press    = hw_.encoder.Pressed();
        c.button_count = kButtonCount;
        c.gate_count   = kGateCount;
        c.gate[0]      = hw_.gate_input[daisy::DaisyPatch::GATE_IN_1].State();
        c.gate[1]      = hw_.gate_input[daisy::DaisyPatch::GATE_IN_2].State();
    }

    // MIDI: not wired for this target (no MIDI input handled here). No-ops so the shared harness
    // compiles; only the Pod build receives MIDI.
    void StartMidi() {}
    template <typename Sink> void PollMidi(Sink&&) {}

    // No discrete LEDs on the Patch (it has an OLED). Indicator output is a no-op until the OLED is
    // modelled; engine/harness indicator calls are harmlessly ignored here.
    void SetIndicator(int, float, float, float) {}

    void SetUserLed(bool on) { hw_.seed.SetLed(on); }

    daisy::DaisyPatch& hw() { return hw_; }

private:
    daisy::DaisyPatch hw_;
};

} // namespace daisyapps
