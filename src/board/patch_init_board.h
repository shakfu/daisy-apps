#pragma once

// patch.init() target - Electrosmith's Eurorack module on the Daisy Patch Submodule. The submodule
// BSP (DaisyPatchSM) models the pots/CV/gates/LED; the faceplate's button + toggle are wired to
// submodule GPIO by the app (done here). patch.init() has NO rotary encoder and NO display. See
// pod_board.h for the reference implementation and the Board interface contract.

#include "daisy_patch_sm.h"
#include "board/controls.h"

namespace daisyapps {

// patch.init() faceplate -> Controls mapping (verified against the panel):
//   CV_1..CV_4   4 knobs            -> analog[0..3]   (ADC CV_1..CV_4)
//   CV_5..CV_8   4 CV input jacks   -> analog[4..7]   (ADC CV_5..CV_8)
//   B7           momentary button   -> button[0]
//   B8           SPDT toggle        -> button[1]
//   gate_in_1/2  (pins B10 / B9)    -> gate[0..1]     (DaisyPatchSM gate_in_1/gate_in_2)
//   daisy LED                       -> SetUserLed / SetIndicator(0)   (mono)
// No rotary encoder, no display, so the encoder fields stay inert. Outputs are present but deferred
// (input-focused scope): gate_out_1/2 (pins B5 / B6) and CV_OUT_1/CV_OUT_2 (DAC, pins C10 / C1) via
// DaisyPatchSM::WriteCvOut + gpio writes. Audio is stereo (IN_L/R, OUT_L/R).
class PatchInitBoard {
public:
    static constexpr int  kAnalogCount    = 8;   // 4 pots (CV_1..4) then 4 CV ins (CV_5..8)
    static constexpr int  kButtonCount    = 2;   // [0] = momentary button (B7), [1] = toggle (B8)
    static constexpr int  kGateCount      = 2;   // gate_in_1, gate_in_2
    static constexpr int  kIndicatorCount = 1;   // mono user LED (no RGB)
    static constexpr int  kAudioChannels  = 2;   // stereo in/out
    static constexpr int  kCvOutCount     = 2;   // CV_OUT_1 (C10), CV_OUT_2 (C1)
    static constexpr bool kHasScreen      = false;
    static constexpr int  kScreenWidth    = 0;
    static constexpr int  kScreenHeight   = 0;

    void Init(int block_size)
    {
        hw_.Init();
        hw_.SetAudioBlockSize(static_cast<size_t>(block_size));
        hw_.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
        hw_.StartAdc();
        button_.Init(daisy::patch_sm::DaisyPatchSM::B7);   // faceplate momentary button
        toggle_.Init(daisy::patch_sm::DaisyPatchSM::B8);   // faceplate SPDT toggle
    }

    void  StartAudio(daisy::AudioHandle::AudioCallback cb) { hw_.StartAudio(cb); }
    float SampleRate() { return hw_.AudioSampleRate(); }

    // patch_sm::CV_1..CV_8 are ADC indices 0..7, so Analog(i) maps straight onto GetAdcValue(i).
    float Analog(int i)
    {
        return (i >= 0 && i < kAnalogCount) ? hw_.GetAdcValue(i) : 0.f;
    }

    void Poll(Controls& c)
    {
        hw_.ProcessAllControls();
        button_.Debounce();
        toggle_.Debounce();
        c.analog_count = kAnalogCount;
        for (int i = 0; i < kAnalogCount; i++) c.analog[i] = hw_.GetAdcValue(i);
        c.gate_count   = kGateCount;
        c.gate[0]      = hw_.gate_in_1.State();
        c.gate[1]      = hw_.gate_in_2.State();
        c.button_count = kButtonCount;
        c.button[0]    = button_.Pressed();
        c.button[1]    = toggle_.Pressed();
        c.enc_inc      = 0;        // no encoder on patch.init()
        c.enc_press    = false;
    }

    // MIDI: not wired for this target (no MIDI input handled here). No-ops so the shared harness
    // compiles; only the Pod build receives MIDI.
    void StartMidi() {}
    template <typename Sink> void PollMidi(Sink&&) {}

    // --- Outputs --------------------------------------------------------------------------------
    // CV out `ch` (0 = CV_OUT_1 / pin C10, 1 = CV_OUT_2 / pin C1) from a 0..1 normalized value. The
    // submodule's WriteCvOut takes VOLTS over a 0..5 V range, so `norm` is scaled by 5. A bipolar
    // engine signal must be offset by the caller: v = 0.5f + 0.5f * cv.
    void SetCvOut(int ch, float norm)
    {
        if (ch < 0 || ch >= kCvOutCount) return;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;
        hw_.WriteCvOut(ch == 0 ? daisy::patch_sm::CV_OUT_1 : daisy::patch_sm::CV_OUT_2, norm * 5.f);
    }

    // Gate out 1 (pin B5). gate_out_2 is left to board-specific code via hw().
    void SetGateOut(bool on) { hw_.gate_out_1.Write(on); }

    // --- Screen: patch.init() has none, so the text/rect facade is inert (see patch_board.h). ----
    void ScreenClear() {}
    void ScreenText(int, int, const char*, bool = true) {}
    void ScreenRect(int, int, int, int, bool) {}
    void ScreenUpdate() {}

    // Mono LED: collapse the requested color to on/off (lit if any channel is non-zero).
    // (These went through the old C GPIO API, dsy_gpio_write(&pin, val), which current libDaisy has
    // replaced with the GPIO class - DaisyPatchSM::user_led is a daisy::GPIO. This target had never
    // been compiled, so the stale call had gone unnoticed.)
    void SetIndicator(int idx, float r, float g, float b)
    {
        if (idx != 0) return;
        hw_.user_led.Write(r > 0.f || g > 0.f || b > 0.f);
    }

    // The board's QSPI flash handle, as the void* EngineContext::qspi expects. The contract keeps this
    // opaque so it names no HAL type; the one engine that uses it (edrums, for kit presets) casts it
    // back to daisy::QSPIHandle* in target-only code.
    void* Qspi() { return &hw_.qspi; }

    void SetUserLed(bool on) { hw_.user_led.Write(on); }

    daisy::patch_sm::DaisyPatchSM& hw() { return hw_; }

private:
    daisy::patch_sm::DaisyPatchSM hw_;
    daisy::Switch                 button_;   // faceplate momentary (B7)
    daisy::Switch                 toggle_;   // faceplate SPDT toggle (B8)
};

} // namespace daisyapps
