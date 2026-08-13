#pragma once

// Daisy Patch - the Seed-based Eurorack module, and the only target in this repo with a screen.
// See pod_board.h for the reference implementation and the Board interface contract.

#include "daisy_patch.h"
#include "board/controls.h"
#include "board/midi_status.h"

namespace daisyapps {

// Daisy Patch faceplate -> Controls mapping:
//   CTRL_1..CTRL_4   4 knobs, each SUMMED with its CV input jack -> analog[0..3]
//   encoder (+click)                                             -> enc_inc / enc_press
//   GATE_IN_1 / GATE_IN_2                                        -> gate[0..1]
//   MIDI IN (TRS, on the seed UART)                              -> StartMidi / PollMidi
//   CV_OUT_1 / CV_OUT_2 (seed DAC, 12-bit, 0..5 V)               -> SetCvOut(0|1, norm)
//   gate out                                                     -> SetGateOut(bool)
//   128x64 SSD1306 OLED                                          -> the Screen* calls
//
// A note on the analog inputs that matters for anything CV-driven: on the Patch a knob and its jack
// are summed in ANALOG hardware ahead of the ADC, so `analog[i]` is one reading of knob+CV and cannot
// be decomposed. The knob therefore acts as the offset/attenuator for its CV input; a patched jack
// moves the same value the knob sets. There is no separate CV array to route to IEngine's cv_* inputs
// (unlike patch.init(), where analog[4..7] are dedicated CV jacks), which is why the harness drives
// engine params from analog[0..3] and leaves cv_mix/cv_size_pos/cv_voct alone on this target.
//
// Audio: the Patch is a 4-in/4-out board. IEngine::process is stereo, so the harness passes it
// in/out channels 0-1 (IN_1/IN_2 -> OUT_1/OUT_2) and leaves 3/4 silent.
class PatchBoard {
public:
    static constexpr int  kAnalogCount    = 4;    // CTRL_1..CTRL_4 (knob + CV summed)
    static constexpr int  kButtonCount    = 0;    // encoder click only (reported via Controls.enc_press)
    static constexpr int  kGateCount      = 2;    // GATE_IN_1, GATE_IN_2
    static constexpr int  kIndicatorCount = 0;    // no discrete panel LEDs (it has the OLED instead)
    static constexpr int  kCvOutCount     = 2;    // CV_OUT_1, CV_OUT_2 (seed DAC)
    static constexpr int  kAudioChannels  = 4;    // IN/OUT 1-4; IEngine::process is stereo (see below)
    static constexpr bool kHasScreen      = true;
    static constexpr int  kScreenWidth    = 128;
    static constexpr int  kScreenHeight   = 64;

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
    // loop; reading it from the audio callback is a benign single-float race.
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

    // --- MIDI (TRS MIDI IN on the seed UART, same handler the Pod uses) -------------------------
    void StartMidi() { hw_.midi.StartReceive(); }

    // Main loop: parse buffered MIDI and forward each 3-byte-representable message to
    // sink(status, data1, data2). See board/midi_status.h for what is and is not forwarded.
    template <typename Sink>
    void PollMidi(Sink&& sink) { poll_midi_handler(hw_.midi, sink); }

    // --- Outputs --------------------------------------------------------------------------------
    // CV out `ch` (0 = CV_OUT_1, 1 = CV_OUT_2) from a 0..1 normalized value. The Patch's DAC is 12-bit
    // over a 0..5 V range, so `norm` maps linearly onto 0..5 V - a BIPOLAR engine signal (IEngine's
    // process_cv fills -1..1) must be offset by the caller: v = 0.5f + 0.5f * cv. Out-of-range channel
    // is a no-op, so harness code written for a board with more CV outs degrades gracefully.
    void SetCvOut(int ch, float norm)
    {
        if (ch < 0 || ch >= kCvOutCount) return;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;
        hw_.seed.dac.WriteValue(ch == 0 ? daisy::DacHandle::Channel::ONE
                                        : daisy::DacHandle::Channel::TWO,
                                static_cast<uint16_t>(norm * 4095.f));
    }

    void SetGateOut(bool on) { hw_.gate_output.Write(on); }

    // --- Screen ---------------------------------------------------------------------------------
    // A minimal text/rect facade over the OLED, mirroring how SetIndicator is a no-op on boards
    // without LEDs: boards without a screen implement these as no-ops, so harness UI code calls them
    // unconditionally and stays board-agnostic. Draws are buffered; ScreenUpdate() blits.
    void ScreenClear() { hw_.display.Fill(false); }

    // Draw `text` with its top-left at (x, y). `small` picks the 6x8 font (4 lines of ~21 chars on a
    // 128x64 panel); otherwise 7x10 for headings.
    void ScreenText(int x, int y, const char* text, bool small = true)
    {
        hw_.display.SetCursor(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
        hw_.display.WriteString(text, small ? Font_6x8 : Font_7x10, true);
    }

    void ScreenRect(int x, int y, int w, int h, bool fill)
    {
        if (w <= 0 || h <= 0) return;
        hw_.display.DrawRect(static_cast<uint_fast8_t>(x),
                             static_cast<uint_fast8_t>(y),
                             static_cast<uint_fast8_t>(x + w - 1),
                             static_cast<uint_fast8_t>(y + h - 1),
                             true,
                             fill);
    }

    void ScreenUpdate() { hw_.display.Update(); }

    // No discrete LEDs on the Patch (it has the OLED). Indicator output is a no-op; engine/harness
    // indicator calls are harmlessly ignored here.
    void SetIndicator(int, float, float, float) {}

    // The board's QSPI flash handle, as the void* EngineContext::qspi expects. The contract keeps this
    // opaque so it names no HAL type; the one engine that uses it (edrums, for kit presets) casts it
    // back to daisy::QSPIHandle* in target-only code.
    void* Qspi() { return &hw_.seed.qspi; }

    void SetUserLed(bool on) { hw_.seed.SetLed(on); }

    // Escape hatch for board-specific needs the abstraction does not cover.
    daisy::DaisyPatch& hw() { return hw_; }

private:
    daisy::DaisyPatch hw_;
};

} // namespace daisyapps
