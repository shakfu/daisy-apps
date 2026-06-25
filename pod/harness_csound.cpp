// Thin QSPI harness that runs CsoundEngine through the real IEngine interface (vs app.cpp, which
// calls the Csound C API directly). It stands in for the host firmware platform: builds an
// EngineContext, init()s the engine, drives process() from the audio callback and set_param() from
// the board's controls. The control surface is abstracted via board/board.h; this build targets the
// Daisy Pod (-DTARGET_POD), whose 2 knobs map to PITCH + MIX.
//
// Build (from pod/):  make            (Makefile targets the harness, passes -DTARGET_POD)
// Flash:                     while ! make program-dfu; do sleep 0.2; done   (then tap RESET)

#include "daisy_seed.h"
#include "board/board.h"
#include "engine/csound/csound_engine.h"
#include "sd_stream_deck.h"

using namespace daisy;

// CsoundEngine::init() calls csound_heap_arm() to arm the full firmware's dual SDRAM pool
// (csound_alloc.cpp, linked there via --wrap). This Pod build does NOT use that machinery - it puts
// Csound's heap straight in SDRAM via the Csound-port QSPI linker script (newlib malloc). So provide
// a no-op definition here to satisfy the symbol; Csound just allocates from the linker-script heap.
namespace daisyapps { void csound_heap_arm() noexcept {} }

static const int kBlock = 256;   // Csound-friendly block; becomes ksmps inside the engine

daisyapps::Board        board;
daisyapps::CsoundEngine engine;
daisyapps::SdStreamDeck sd;       // SD patch bank: csound/0.csd .. csound/7.csd (built-in if absent)

// Daisy's non-interleaving buffers are already de-interleaved (InputBuffer = const float* const*,
// OutputBuffer = float**), which is exactly IEngine::process's shape - forward straight through.
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    engine.process(in, out, size);
}

int main(void)
{
    // Defensive: point the vector table at our QSPI app. A no-op if the bootloader already did
    // it (v5.4), but it makes this image bootloader-agnostic - needed to test whether a QSPI
    // app boots at all under a bootloader that may not set VTOR.
    SCB->VTOR = 0x90040000;
    __DSB();
    __ISB();

    board.Init(kBlock);   // BSP up (seed + SDRAM + controls + ADC), audio block size + 48 kHz

    // Mount the SD card (if present) so the engine can load a `csound/*.csd` patch bank. Mount failure
    // is non-fatal: the stream's exists()/read_text() then just report "no file", and the engine runs
    // its built-in orchestra. Pass the deck unconditionally - the engine tolerates an empty card.
    sd.Init();

    // Build the context the platform would normally inject. CsoundEngine reads sample_rate and
    // block_size; its heap comes from the QSPI linker script, so the arena and the remaining service
    // pointers are left empty/null here (a real platform would populate them).
    daisyapps::EngineContext ctx{};
    ctx.sample_rate = board.SampleRate();
    ctx.block_size  = static_cast<float>(kBlock);
    ctx.arena       = { nullptr, 0 };
    ctx.time        = nullptr;
    ctx.transport   = nullptr;
    ctx.stream      = &sd;        // SD patch bank + boot auto-load + Alt selector (csound_patch.h)
    ctx.qspi        = nullptr;
    engine.init(ctx);

    board.StartAudio(AudioCallback);
    board.StartMidi();   // begin receiving MIDI (NoteOn -> engine.handle_midi_note, drained in the ISR)

    using daisyapps::DeckRef;
    using daisyapps::ParamId;
    daisyapps::Controls controls;
    while (1) {
        board.Poll(controls);

        // The board surfaces the full raw MIDI stream; Csound only plays NoteOns, so pull those out
        // (status NoteOn nibble, velocity > 0) and forward to the MidiNote instrument (channel -> deck,
        // note -> Hz). handle_midi_note only enqueues here; the audio ISR drains and schedules it.
        board.PollMidi([](uint8_t st, uint8_t d1, uint8_t d2) {
            if ((st & 0xf0) == 0x90 && d2 > 0) engine.handle_midi_note(static_cast<uint8_t>(st & 0x0f), d1);
        });

        // Patch selector (CapAux), the Pod analog of the firmware's Alt+PITCH gesture: hold the encoder
        // to open the bank, turn KNOB_1 to scroll the [built-in, csound/0.csd .. 7.csd] list (preview),
        // release to commit a live recompile. The encoder press stands in for the Alt pad; KNOB_1 is the
        // PITCH knob. While selecting, KNOB_1 drives the selector instead of pitch.
        const bool selecting = controls.enc_press;
        engine.set_aux_active(DeckRef::A, selecting);

        if (selecting) {
            if (controls.analog_count > 0) engine.set_param(ParamId::Aux, DeckRef::A, controls.analog[0]); // scroll bank
        } else {
            if (controls.analog_count > 0) engine.set_param(ParamId::Speed, DeckRef::A, controls.analog[0]); // knob1 -> pitch
            if (controls.analog_count > 1) engine.set_param(ParamId::Mix,   DeckRef::A, controls.analog[1]); // knob2 -> level
        }
        engine.prepare();
    }
}
