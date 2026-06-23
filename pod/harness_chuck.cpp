// Thin QSPI harness that runs ChuckEngine through the real IEngine interface - the M1 vehicle from
// docs/dev/chuck-impl.md ("Pod tone from QSPI"). It stands in for the host firmware platform: builds an
// EngineContext, init()s the engine, drives process() from the audio callback and set_param() from
// the board's controls. The control surface is abstracted via board/board.h; this build targets the
// Daisy Pod (-DTARGET_POD), whose 2 knobs map to PITCH + MIX.
//
// Unlike the Csound harness, this one links the REAL SDRAM pool (chuck_alloc.cpp + --wrap), so
// ChuckEngine::init()'s chuck_heap_arm() arms the .sdram_bss pool - exactly the M2 firmware heap
// model, exercised on the quick-iterate Pod. The platform heap stays in SRAM (alt_qspi.lds).
//
// Build (from pod/):  make -f Makefile.chuck
// Flash:              while ! make -f Makefile.chuck program-dfu; do sleep 0.2; done   (then tap RESET)
//
// Prereq (once): build libchuck.a with scripts/fetch_chuck.sh.

#include <cmath>             // fabsf - knob deadband
#include "daisy_seed.h"
#include "board/board.h"
#include "engine/chuck/chuck_engine.h"
#include "sd_stream_deck.h"

using namespace daisy;

static const int kBlock = 256;   // matches ChuckEngine::kMaxBlock; == run() numFrames per callback

daisyapps::Board       board;
daisyapps::ChuckEngine engine;
daisyapps::SdStreamDeck sd;       // SD patch bank: chuck/0.ck .. chuck/7.ck (built-in if absent)

// Set in the main loop when the encoder is held to open the patch selector. Read in the audio ISR to
// suspend the knob-1 -> PITCH push while knob 1 is scrolling the bank (so browsing doesn't detune the
// playing patch). volatile: written main loop, read ISR; a single bool, no tearing.
static volatile bool g_selecting = false;

// Onboard-LED heartbeat for bring-up (the Pod's LED is visible - unlike a cased product unit). Blink
// the seed LED n times so we can see how far init() got; blocking, bring-up only.
static void blink(int n)
{
    daisy::System::Delay(500);
    for (int i = 0; i < n; i++) {
        board.SetUserLed(true);  daisy::System::Delay(160);
        board.SetUserLed(false); daisy::System::Delay(160);
    }
}

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    // Read the knobs and push them to the VM ONCE PER BLOCK (~187 Hz), here in the audio ISR - NOT in
    // main()'s loop. The main loop has no rate limit, so set_param there floods ChucK's global queue
    // (thousands/sec) and the VM chokes draining it inside run(), starving everything. Once-per-block
    // is the right cadence and lands the write right before run() consumes it. The board abstraction's
    // Analog(i) is an ISR-safe read of the last value Poll() refreshed in the main loop.
    // Deadband: a still pot still jitters in its low ADC bits; re-sending every block reassigns
    // s.freq/s.gain constantly and zippers the audio (and churns the heap in the ISR). Only push when
    // the reading actually moves. A light one-pole smooths the steps while turning.
    static float sp_s = 0.f, mx_s = 0.f;        // smoothed knob state
    static float sp_tx = -1.f, mx_tx = -1.f;    // last value sent to the VM
    sp_s += 0.25f * (board.Analog(0) - sp_s);
    mx_s += 0.25f * (board.Analog(1) - mx_s);
    // While the selector is open, knob 1 scrolls the patch bank (driven from the main loop), so suspend
    // its PITCH push here; the frozen sp_tx means release re-pushes pitch at the knob's new position.
    if (!g_selecting && fabsf(sp_s - sp_tx) > 0.004f) { sp_tx = sp_s; engine.set_param(daisyapps::ParamId::Speed, daisyapps::DeckRef::A, sp_s); } // knob1 -> pitch
    if (fabsf(mx_s - mx_tx) > 0.004f) { mx_tx = mx_s; engine.set_param(daisyapps::ParamId::Mix,   daisyapps::DeckRef::A, mx_s); } // knob2 -> level

    // Slow onboard-LED toggle = the audio ISR is alive (~0.7 Hz at 256/48k).
    static uint32_t c = 0; static bool s = false;
    if (((c++) & 0x7F) == 0) { s = !s; board.SetUserLed(s); }
    engine.process(in, out, size);
}

int main(void)
{
    // Defensive: point the vector table at our QSPI app. A no-op if the bootloader already did it,
    // but it makes this image bootloader-agnostic - the same VTOR inject the csound harness uses to
    // confirm a QSPI app boots SysTick + the audio DMA IRQ under a stock QSPI bootloader.
    SCB->VTOR = 0x90040000;
    __DSB();
    __ISB();

    board.Init(kBlock);   // BSP up; seed.Init powers the FMC -> SDRAM live (chuck_heap_arm needs it)

    // Mount the SD card (if present) so the engine can load a `chuck/*.ck` patch bank. Non-fatal on
    // failure: the stream then reports "no file" and the engine runs its built-in program. FatFs uses a
    // static LFN buffer (no malloc), so it does not touch ChucK's --wrap SDRAM pool - mount order vs.
    // engine.init() is unconstrained; mount first so boot can load slot 0 directly.
    sd.Init();

    blink(2);   // reached: hardware up, about to call engine.init() (new ChucK / init / compileCode)

    // Build the context the platform would normally inject. ChuckEngine reads sample_rate + block_size
    // and arms its own SDRAM pool; the arena/time/transport pointers are unused on the Pod (null).
    daisyapps::EngineContext ctx{};
    ctx.sample_rate = board.SampleRate();
    ctx.block_size  = static_cast<float>(kBlock);
    ctx.arena       = { nullptr, 0 };
    ctx.time        = nullptr;
    ctx.transport   = nullptr;
    ctx.stream      = &sd;        // SD patch bank + boot auto-load + Alt selector (chuck_patch.h)
    ctx.qspi        = nullptr;
    engine.init(ctx);                           // new ChucK / compileCode(kProgram) - allocates in SDRAM

    blink(3);   // reached: engine.init() RETURNED (ChucK create/init/compile did not crash)

    board.StartAudio(AudioCallback);
    board.StartMidi();   // begin receiving MIDI (NoteOn -> engine.handle_midi_note, drained in the ISR)

    // Knob reads + the PITCH/MIX set_param live in the audio callback (rate-limited to one block); the
    // main loop Poll()s the board to refresh those readings (and the encoder/buttons) and runs off-ISR
    // housekeeping. prepare() commits the SD .ck patch bank's boot auto-load + live recompile.
    //
    // Patch selector (CapAux), the Pod analog of the firmware's Alt+PITCH gesture: hold the encoder to
    // open the bank, turn knob 1 to scroll the [built-in, chuck/0.ck .. 7.ck] list (preview), release to
    // commit a live recompile. g_selecting tells the ISR to stop pushing knob 1 as PITCH while browsing.
    using daisyapps::DeckRef;
    using daisyapps::ParamId;
    daisyapps::Controls controls;
    while (1) {
        board.Poll(controls);

        // MIDI NoteOn -> the patch's note Event + frequency global (channel -> deck, note -> Hz).
        // handle_midi_note only enqueues here; the audio ISR drains and delivers to the VM (chuck_engine).
        board.PollMidi([](uint8_t ch, uint8_t note) { engine.handle_midi_note(ch, note); });

        const bool selecting = controls.enc_press;
        g_selecting = selecting;
        engine.set_aux_active(DeckRef::A, selecting);
        if (selecting && controls.analog_count > 0)
            engine.set_param(ParamId::Aux, DeckRef::A, controls.analog[0]); // scroll bank

        engine.prepare();
    }
}
