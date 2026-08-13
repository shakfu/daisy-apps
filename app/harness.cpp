// The generic engine host: one harness that runs ANY ported sk-engines engine on any of the three
// boards, with the engine chosen at build time (ENGINE=) and the board at build time (BOARD=).
//
//   make ENGINE=delay BOARD=patch            -> build/harness_delay.bin
//
// It stands in for sk-engines' platform: bring up the board, build an EngineContext (SDRAM arena,
// clock, transport, SD), init the engine, drive process() from the audio callback, and map the panel
// onto IEngine. What it deliberately does NOT do is know anything about a particular engine - the
// control surface is generated from the engine's own live_params()/param_label() declarations (see
// app/param_ui.h), so porting an engine costs a Makefile block and nothing here.
//
// Panel (Daisy Patch):
//   CTRL 1-4          the four params of the current page, with value pickup
//   encoder turn      change page
//   encoder click     dual-deck engine: switch deck A/B.  Otherwise: cycle ConfigId::Mode.
//   encoder LONG press PLAY/PAUSE (IEngine::on_play_pad). The Patch has no buttons, so without this
//                     gesture the play surface is unreachable - and engines that start idle by design
//                     (bard boots paused) look broken rather than stopped.
//   buttons           where a board has them (Pod, patch.init()): 0 = play, 1 = play with `reverse`
//   encoder hold+turn CapAux engine: scroll its Aux selector (upstream's Alt+PITCH gesture).
//                     Otherwise: set the internal tempo.
//   GATE IN 1         trigger the focused deck (IEngine::on_gate_trigger)
//   GATE IN 2         external clock: quarter-note pulses steer the transport tempo
//   MIDI IN           forwarded whole (handle_midi_message) plus NoteOn (handle_midi_note)
//   CV OUT 1/2        IEngine::process_cv, block-rate, mapped from bipolar to the 0-5 V output
//   GATE OUT          IEngine::gate_out_triggered on the focused deck
//   QSPI flash        EngineContext::qspi - edrums persists its kit presets there
//
// CV INPUTS are off by default on the Patch and on by default on patch.init() - see HARNESS_CV_INPUTS
// below, which explains why the two boards differ. With -DHARNESS_CV_INPUTS=2 the Patch trades CTRL 3
// and 4 for V/Oct and start-position, which is what the radio, pstretch and bard engines respond to.
//
// On a Pod the screen/CV/gate calls are board no-ops and only knobs 1-2 exist, so the same binary
// shape still runs - it is just a two-knob view of page 1.

#include <cmath>
#include <cstdio>

#include "daisy_seed.h"

#include "board/board.h"
#include "engine/engine_select.h"   // ActiveEngine, from the ENGINE= define
#include "app/harness_clock.h"
#include "app/param_ui.h"

// SD service. Two implementations of IStreamDeck live in this repo and the engine picks which one it
// needs: an engine that streams audio from the card (tape, radio, pstretch, softcut, bard) sets
// SPK_USE_STREAM in its Makefile block and gets the real StreamDeck - lock-free rings serviced by the
// audio ISR, FatFs I/O pumped from the main loop. Every other engine gets the lightweight deck, whose
// streaming half is stubbed, and its build stays free of the rings, the FatFs staging and the pump.
// The guard is upstream's, and hw/stream_deck.cpp / hw/fat_file.cpp compile to nothing without it.
#if defined(SPK_USE_STREAM)
#include "hw/stream_deck.h"
#include "sd_card.h"          // StreamDeck does FatFs I/O but does not own the mount
#else
#include "sd_stream_deck.h"   // (mounts the card itself, via the same SdCard)
#endif

using namespace daisy;

#ifndef SPK_ENGINE_STR
#define SPK_ENGINE_STR "engine"
#endif

// Audio block. 48 frames = 1 ms at 48 kHz: short enough that gate and CV-out are responsive (both are
// serviced per block), long enough that the per-block overhead stays negligible.
static constexpr int kBlock = 48;

// --- CV inputs ---------------------------------------------------------------------------------
// How many of the board's analog inputs to spend on IEngine's CV surface (cv_voct / cv_size_pos /
// cv_mix / cv_crossfade) instead of on parameters. They are taken from the END of the analog array,
// which is where a board that has dedicated CV jacks puts them.
//
// The default differs by board because the hardware does:
//   patch.init()  4 - analog[4..7] ARE dedicated CV jacks, so routing them costs nothing.
//   Daisy Patch   0 - knob and jack are summed in analog hardware ahead of the ADC, so there is no
//                     spare input: every CV input dedicated here COSTS A PARAMETER KNOB. Left at 0 so
//                     the default build keeps all four knobs; set -DHARNESS_CV_INPUTS=2 to hand CTRL 3
//                     and 4 to V/Oct and start-position, which is what makes radio, pstretch and bard
//                     playable from a CV patch.
//   Pod           0 - no CV inputs at all.
#ifndef HARNESS_CV_INPUTS
#  if defined(TARGET_PATCH_INIT)
#    define HARNESS_CV_INPUTS 4
#  else
#    define HARNESS_CV_INPUTS 0
#  endif
#endif

// The voltage a CV jack spans across the full 0..1 ADC reading, used to turn a normalized reading into
// the units IEngine expects. NOT CALIBRATED: upstream runs V/Oct through a per-unit calibration table
// (three measured reference voltages) and this is a plain linear assumption of a bipolar +/-5 V jack.
// Pitch tracking will therefore be approximate until someone measures a real board and corrects this.
static constexpr float kCvVoltSpan = 10.f;   // -5 V .. +5 V

// IEngine::cv_voct takes an offset in SEMITONES, 0 = neutral (upstream's corrector returns note
// numbers; pstretch divides by 12 to get octaves, radio reads it as a station offset). 1 V/oct over a
// 10 V span is 120 semitones.
static constexpr float kCvVoctSemitones = kCvVoltSpan * 12.f;

// SDRAM arena the engines sub-allocate their buffers from (delay lines, reverb tails, grain clouds).
// The Daisy has 64 MB; 48 MB is what sk-engines hands its engines, and the engines that want less
// simply take less - Arena is a bump allocator, so an unused tail costs nothing but address space.
static uint8_t DSY_SDRAM_BSS s_arena[48 * 1024 * 1024];

static daisyapps::Board            board;
static daisyapps::ActiveEngine     engine;
static daisyapps::SystemTime       time_source;
static daisyapps::HarnessTransport transport(time_source);
static daisyapps::ParamUI<daisyapps::Board> ui;

#if defined(SPK_USE_STREAM)
// Streaming build. One read-ahead / write-behind ring PER DECK - a deck is play-XOR-record, so one ring
// serves both - plus a chunk of scratch the two decks share, because the main-loop pump services them
// sequentially and never concurrently. 1 MB per ring is ~5.5 s of mono read-ahead at 48 kHz, which is a
// large cushion against SD latency spikes; SpscRing requires a power of two. Sizes follow upstream.
static constexpr uint32_t kStreamRingBytes    = 1u * 1024u * 1024u;
static constexpr uint32_t kStreamScratchBytes = 32u * 1024u;

alignas(32) static uint8_t DSY_SDRAM_BSS s_ring_a[kStreamRingBytes];
alignas(32) static uint8_t DSY_SDRAM_BSS s_ring_b[kStreamRingBytes];
alignas(32) static uint8_t DSY_SDRAM_BSS s_stream_scratch[kStreamScratchBytes];

static daisyapps::SdCard     s_card;
static daisyapps::StreamDeck s_stream;
#else
static daisyapps::SdStreamDeck s_stream;
#endif

// CV written by the audio callback, consumed by the main loop (see the callback for why).
static volatile float s_cv[2] = {0.f, 0.f};

static daisyapps::DeckRef::Ref s_deck = daisyapps::DeckRef::A;

// How many times the play gesture has fired. On screen next to the stream state, because "nothing
// happens" has two very different causes: the gesture never reached the engine, or it did and the
// engine chose to stay silent.
static int s_play_presses = 0;

// Daisy's non-interleaving buffers are already de-interleaved (InputBuffer = const float* const*,
// OutputBuffer = float**), which is exactly IEngine::process's shape - forward straight through.
static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    engine.process(in, out, size);

    // IEngine::process is stereo and writes out[0..1]. The Daisy Patch is a 4-in/4-out board, so on
    // that target out[2..3] are buffers the engine never touched - and libDaisy hands back the same
    // DMA memory each block, so leaving them alone emits whatever was last in them. Silence them.
    for (int ch = 2; ch < daisyapps::Board::kAudioChannels; ch++)
        for (size_t i = 0; i < size; i++) out[ch][i] = 0.f;

    // Block-rate CV, exactly as the contract specifies ("the platform's DAC ISR calls this ONCE per
    // block"). The values are handed to the main loop rather than written here: WriteCvOut on
    // patch.init() goes through the submodule's DAC helper, and keeping all peripheral writes on one
    // thread avoids racing the main loop's OLED/ADC traffic for a signal that only moves per block.
    float cv0 = 0.f, cv1 = 0.f;
    engine.process_cv(&cv0, &cv1, 1);
    s_cv[0] = cv0;
    s_cv[1] = cv1;
}

int main(void)
{
#if defined(HARNESS_BOOT_QSPI)
    // QSPI-execute builds only (mosc). Point the vector table at this app in QSPI. Defensive: the
    // bootloader normally does it, but an image that assumes so and is wrong does not boot at all, and
    // the pod/ harnesses carry the same prologue for the same reason. Harmless when already set.
    SCB->VTOR = 0x90040000;
    __DSB();
    __ISB();
#endif

    board.Init(kBlock);   // BSP up (seed + SDRAM + controls + ADC), audio block size + 48 kHz

    // Mount the SD card if one is present. Failure is non-fatal: every card path reports "no file" and
    // an engine that streams simply finds nothing to play.
#if defined(SPK_USE_STREAM)
    s_card.Mount();
    s_stream.init({ s_ring_a, kStreamRingBytes,
                    s_ring_b, kStreamRingBytes,
                    s_stream_scratch, kStreamScratchBytes });
#else
    s_stream.Init();
#endif

    daisyapps::EngineContext ctx{};
    ctx.sample_rate = board.SampleRate();
    ctx.block_size  = static_cast<float>(kBlock);
    ctx.arena       = { s_arena, sizeof(s_arena) };
    ctx.time        = &time_source;
    ctx.transport   = &transport;
    ctx.stream      = &s_stream;
    // The board's QSPI flash, opaque to the contract. edrums is the only engine that reads it, to
    // persist kit presets; every other engine ignores it. It writes at a 64 KB offset, clear of both
    // the calibration settings at offset 0 and the app image at 0x40000 - including a BOOT_QSPI app
    // like mosc, which is a different engine anyway.
    ctx.qspi        = board.Qspi();
    engine.init(ctx);

    // Split the analog inputs between parameters and the engine's CV surface. CV takes the tail of the
    // array (dedicated jacks live there), parameters take up to four of what is left.
    constexpr int kCvInputs = HARNESS_CV_INPUTS;
    constexpr int kCvBase   = daisyapps::Board::kAnalogCount - kCvInputs;
    static_assert(kCvBase >= 0, "HARNESS_CV_INPUTS exceeds this board's analog input count");
    // Four is the whole CV surface (voct, size_pos, mix, crossfade) and the size of the cv_last
    // deadband array below, so a larger value would index past it rather than do anything useful.
    static_assert(kCvInputs <= 4, "HARNESS_CV_INPUTS is at most 4 - there are only four CV inputs");

    const int param_inputs = kCvBase;
    const int knobs        = (param_inputs >= 4) ? 4 : param_inputs;
    ui.init(engine, knobs);

    board.StartAudio(AudioCallback);
    board.StartMidi();

    using daisyapps::ParamId;
    using daisyapps::ConfigId;
    using daisyapps::DeckRef;

    daisyapps::Controls controls;
    bool     prev_gate[daisyapps::Controls::kMaxGates]     = {false, false};
    bool     prev_button[daisyapps::Controls::kMaxButtons] = {false, false, false, false};
    bool     enc_was_held  = false;
    bool     enc_turned    = false;   // a turn while held is a gesture, not a click
    uint32_t enc_press_ms  = 0;       // when the current hold started, for the long-press gesture
    int      mode_config   = 0;
    float    aux           = 0.f;     // the CapAux selector position (0..1), scrolled by hold+turn
    // Deadband reference per CV input. Seeded to 0.5 (the neutral mid-scale reading) rather than 0, so
    // an unpatched jack sitting at centre does not fire a spurious write on the first pass.
    float    cv_last[4]    = {0.5f, 0.5f, 0.5f, 0.5f};

    // How long a hold has to last to read as PLAY rather than a click. Long enough not to fire on a
    // deliberate click, short enough not to feel like a wait.
    constexpr uint32_t kLongPressMs = 600;

    while (1) {
        board.Poll(controls);
        transport.poll();
        const uint32_t now_ms = time_source.now_ms();

#if defined(SPK_USE_STREAM)
        // The slow half of streaming: refill each playing deck's ring from the card and drain each
        // recording deck's ring to it, plus finalize a stopped recording's WAV header. This is why the
        // audio ISR never touches FatFs - and why it must be called often enough that a ring cannot run
        // dry between passes. Nothing else in this loop blocks, so that holds comfortably.
        s_stream.process();
#endif

        // --- MIDI ------------------------------------------------------------------------------
        // Forward the whole stream (engines like ChucK want the full vocabulary) and additionally
        // decode NoteOn for the engines that only implement handle_midi_note.
        board.PollMidi([](uint8_t st, uint8_t d1, uint8_t d2) {
            engine.handle_midi_message(st, d1, d2);
            if ((st & 0xf0) == 0x90 && d2 > 0) engine.handle_midi_note(static_cast<uint8_t>(st & 0x0f), d1);
            else if (st == 0xFA || st == 0xFB) engine.handle_midi_transport(true);
            else if (st == 0xFC)               engine.handle_midi_transport(false);
        });

        // --- Gates ------------------------------------------------------------------------------
        // Rising edges only. Gate 1 triggers the focused deck; gate 2 is the external clock.
        for (int g = 0; g < controls.gate_count && g < daisyapps::Controls::kMaxGates; g++) {
            const bool now = controls.gate[g];
            if (now && !prev_gate[g]) {
                if (g == 0) engine.on_gate_trigger(s_deck);
                else        transport.on_external_clock_edge();
            }
            prev_gate[g] = now;
        }

        // --- Buttons: the play/record pad surface ------------------------------------------------
        // Upstream drives these from dedicated Play/Rev pads. A board with buttons gets them here:
        // button 0 is PLAY, button 1 is the same pad with `reverse` set, which engines read as their
        // second gesture (bard: jump back 15 s; a looper: reverse playback). Rising edges only.
        for (int b = 0; b < controls.button_count && b < daisyapps::Controls::kMaxButtons; b++) {
            const bool down = controls.button[b];
            if (down && !prev_button[b]) {
                if (b == 0)      engine.on_play_pad(s_deck, false);
                else if (b == 1) engine.on_play_pad(s_deck, true);
            }
            prev_button[b] = down;
        }

        // --- Encoder ----------------------------------------------------------------------------
        const bool held = controls.enc_press;
        const int  inc  = controls.enc_inc;

        if (held && !enc_was_held) enc_press_ms = now_ms;

        if (held) {
            if (inc != 0) {
                enc_turned = true;
                if (engine.capabilities() & daisyapps::CapAux) {
                    // Upstream's Alt+PITCH gesture: scroll the engine's own selector (model, kit, slot).
                    aux += static_cast<float>(inc) * 0.05f;
                    if (aux < 0.f) aux = 0.f;
                    if (aux > 1.f) aux = 1.f;
                    engine.set_param(ParamId::Aux, s_deck, aux);
                } else {
                    transport.set_tempo(transport.internal_tempo() + static_cast<float>(inc));
                }
            }
            engine.set_aux_active(s_deck, (engine.capabilities() & daisyapps::CapAux) != 0);
        } else {
            if (enc_was_held) {
                engine.set_aux_active(s_deck, false);
                if (!enc_turned) {
                    if (now_ms - enc_press_ms >= kLongPressMs) {
                        // LONG PRESS = PLAY. The Daisy Patch has no buttons at all, so without this
                        // there is no way to reach on_play_pad - and several engines start idle by
                        // design (bard boots paused, being a resume-where-you-left-off player), which
                        // makes them look broken rather than stopped.
                        engine.on_play_pad(s_deck, false);
                        s_play_presses++;
                    } else if (engine.capabilities() & daisyapps::CapDualDeck) {
                        // A click. Dual-deck engines switch deck; the rest cycle their Mode config,
                        // which is the switch position upstream's panel would have provided.
                        s_deck = (s_deck == DeckRef::A) ? DeckRef::B : DeckRef::A;
                        ui.set_deck(s_deck, engine);
                    } else {
                        mode_config = (mode_config + 1) % 3;
                        engine.set_config(ConfigId::Mode, s_deck, mode_config);
                    }
                }
            }
            if (inc != 0) ui.set_page(ui.page() + inc, engine);
            enc_turned = false;
        }
        enc_was_held = held;

        // --- Knobs ------------------------------------------------------------------------------
        ui.poll_knobs(engine, controls);

        // --- CV inputs --------------------------------------------------------------------------
        // The engine SUMS these with the corresponding knob, so the neutral value is 0, not 0.5 - a
        // normalized reading is re-centred to bipolar first. Sent to the focused deck, matching what
        // the knobs address: with at most a few inputs there is no way to give each deck its own, and
        // silently driving only deck A would be worse than following the focus.
        //
        // Deadbanded like the knobs. These are plain state writes in the engine, but the ADC jitters
        // and some of them (radio's station select) act on change.
        for (int i = 0; i < kCvInputs && (kCvBase + i) < controls.analog_count; i++) {
            const float raw = controls.analog[kCvBase + i];
            if (std::fabs(raw - cv_last[i]) <= 0.004f) continue;
            cv_last[i] = raw;

            const float bipolar = raw * 2.f - 1.f;    // 0..1 reading -> -1..1 around the jack's 0 V
            switch (i) {
                case 0: engine.cv_voct(s_deck, bipolar * kCvVoctSemitones); break;  // semitones
                case 1: engine.cv_size_pos(s_deck, bipolar); break;
                case 2: engine.cv_mix(s_deck, bipolar); break;
                case 3: engine.cv_crossfade(bipolar); break;                        // global, not per-deck
                default: break;
            }
        }

        // An engine that repointed a deck's knobs (edrums' drum swap) invalidates the pickup cache.
        if (engine.take_param_reseed(s_deck)) ui.reseed(engine);

        // --- Outputs ----------------------------------------------------------------------------
        // process_cv fills -1..1; the boards' CV outs are unipolar 0-5 V, so centre at 2.5 V.
        board.SetCvOut(0, 0.5f + 0.5f * s_cv[0]);
        board.SetCvOut(1, 0.5f + 0.5f * s_cv[1]);
        board.SetGateOut(engine.gate_out_triggered(s_deck));

        engine.prepare();

        // Rate-limited inside render(): the OLED blit is ~1 KB over SPI and has no business running at
        // main-loop speed.
        // Header status: stream state + play-press count, e.g. "P4:2" (playing, deck A, 2 presses).
        char status[12];
        std::snprintf(status, sizeof(status), "%c%d",
                      s_stream.is_playing(s_deck) ? 'P' : '-', s_play_presses % 10);
        ui.render(board, engine, SPK_ENGINE_STR, transport.tempo(), now_ms, status);
    }
}
