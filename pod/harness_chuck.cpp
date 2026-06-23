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

using namespace daisy;

static const int kBlock = 256;   // matches ChuckEngine::kMaxBlock; == run() numFrames per callback

daisyapps::Board       board;
daisyapps::ChuckEngine engine;

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
    if (fabsf(sp_s - sp_tx) > 0.004f) { sp_tx = sp_s; engine.set_param(daisyapps::ParamId::Speed, daisyapps::DeckRef::A, sp_s); } // knob1 -> pitch
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

    blink(2);   // reached: hardware up, about to call engine.init() (new ChucK / init / compileCode)

    // Build the context the platform would normally inject. ChuckEngine reads sample_rate + block_size
    // and arms its own SDRAM pool; the arena and service pointers are unused on the Pod (null).
    daisyapps::EngineContext ctx{};
    ctx.sample_rate = board.SampleRate();
    ctx.block_size  = static_cast<float>(kBlock);
    ctx.arena       = { nullptr, 0 };
    ctx.time        = nullptr;
    ctx.transport   = nullptr;
    ctx.stream      = nullptr;
    ctx.qspi        = nullptr;
    engine.init(ctx);                           // new ChucK / compileCode(kProgram) - allocates in SDRAM

    blink(3);   // reached: engine.init() RETURNED (ChucK create/init/compile did not crash)

    board.StartAudio(AudioCallback);

    // Knob reads + set_param live in the audio callback (rate-limited to one block); the main loop
    // Poll()s the board to refresh those readings (and the encoder/buttons) and runs off-ISR
    // housekeeping. prepare() is the hook for the future SD .ck patch bank / live recompile (M3).
    daisyapps::Controls controls;
    while (1) {
        board.Poll(controls);
        engine.prepare();
    }
}
