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
//   encoder click     open the ACTION SCREEN (app/param_ui.h), then one click per row: play, rec,
//                     alt, deck A/B, and the engine's categorical switches. The Patch has no buttons
//                     at all, so this is the only way to reach IEngine's pad and switch surface - and
//                     several engines are inert without it (granular has no audio until it records;
//                     bard boots paused; reverb ships as one of its three algorithms).
//   buttons           where a board has them (Pod, patch.init()): 0 = play, 1 = record. Those boards
//                     have no screen to put a list on, so their click keeps its direct meaning.
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
#include "app/system_time.h"    // SystemTime: the libDaisy-backed ITimeSource
#include "app/harness_clock.h"   // HarnessTransport: the HAL-free tick grid over it
#include "app/param_ui.h"
#include "app/display_adapter.h"  // DisplayModel -> the board's indicator LEDs

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
using HarnessUI = daisyapps::ParamUI<daisyapps::Board>;
static HarnessUI ui;

// The engine's panel: the indicator LEDs on a board that has them, and the engine page on a board with
// a display. Both axes are compile-time - the board's hardware, and whether THIS build's engine draws
// at all - so a target with neither carries none of it. See app/display_adapter.h.
using HarnessLeds = daisyapps::DisplayAdapter<daisyapps::Board,
                                              daisyapps::engine_draws<daisyapps::ActiveEngine>()>;
static HarnessLeds leds;

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

// How many times the play gesture has fired. Shown next to the stream state, because "nothing
// happens" has two very different causes: the gesture never reached the engine, or it did and the
// engine chose to stay silent.
//
// It shares the header's right-hand slot with the TEMPO, and it does not get to keep it. That slot
// held the play/stream state unconditionally, which meant the tempo was never drawn on any build -
// while `delay`, `qdelay` and `edrums` are tempo-synced and the only way to set the tempo is the
// encoder hold+turn gesture, i.e. the one control on the panel with no feedback at all. So the
// status is now transient: it appears while a stream deck is actually playing, and for a few
// seconds after a play press (long enough to answer "did the gesture arrive"), and the slot reverts
// to the tempo the rest of the time.
static int      s_play_presses = 0;
static uint32_t s_play_ms      = 0;   // when the last play gesture fired (0 = never)

// How long a play press keeps the header slot after it fires.
static constexpr uint32_t kStatusHoldMs = 3000;

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
    // Which of IEngine's optional pads this engine implements, measured at compile time from the
    // concrete type (app/engine_pads.h). This is the one place in the firmware that names ActiveEngine
    // besides its construction, and the only place the answer is available: everything downstream
    // holds an IEngine& and could not tell a play pad from a no-op.
    ui.init(engine, knobs, daisyapps::pad_mask<daisyapps::ActiveEngine>());
    // Whether this build's engine draws its own panel, measured from the concrete type rather than
    // taken from capabilities() - same reason as the pad mask above (app/engine_pads.h). An engine
    // that does not draw gets the platform's page/pickup fallback on the same LEDs.
    // On a board with a display, an engine that draws gets a page of its own at the end of the page
    // rotation. On the Daisy Patch that is the ONLY place it can draw at all: it has no discrete LEDs,
    // so the indicator path is a no-op there.
    ui.set_engine_page(daisyapps::engine_draws<daisyapps::ActiveEngine>()
                       && daisyapps::Board::kHasScreen);

    board.StartAudio(AudioCallback);
    board.StartMidi();

    using daisyapps::ParamId;
    using daisyapps::ConfigId;
    using daisyapps::DeckRef;

    // The action screen's `clk in` row cycles an index; this side owns what the index MEANS. The two
    // tables are written in the same order and are checked to be the same length, because a silent
    // mismatch here would read a 1/16 clock as quarters and be four times out on every tempo.
    static_assert(HarnessUI::kClockInCount
                      == static_cast<int>(sizeof(daisyapps::HarnessTransport::kExtPpqChoices)),
                  "param_ui's clock-input labels and HarnessTransport::kExtPpqChoices disagree");
    int clock_in_applied = -1;

    daisyapps::Controls controls;
    bool     prev_gate[daisyapps::Controls::kMaxGates]     = {false, false};
    bool     prev_button[daisyapps::Controls::kMaxButtons] = {false, false, false, false};
    bool     enc_was_held  = false;
    bool     enc_turned    = false;   // a turn while held is a gesture, not a click
    int      mode_config   = 0;       // screenless boards only: the click's Mode cycle position
    float    aux           = 0.f;     // the CapAux selector position (0..1), scrolled by hold+turn
    // Deadband reference per CV input. Seeded to 0.5 (the neutral mid-scale reading) rather than 0, so
    // an unpatched jack sitting at centre does not fire a spurious write on the first pass.
    float    cv_last[4]    = {0.5f, 0.5f, 0.5f, 0.5f};

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
        // Upstream drives these from dedicated Play/Rev/Rec pads. A board with buttons gets two of
        // them: button 0 is PLAY, button 1 is RECORD. Rising edges only.
        //
        // Button 1 used to be the play pad's `reverse` flavour, which meant on_record_pad was called
        // by nothing on any board - and record is the whole point of tape, shuttle and softcut, while
        // granular and graincloud have NO other way to get audio into a deck and were silent from
        // boot. Reverse is a flavour; record is the feature, so it wins the button. On the Patch both
        // live in the action screen and nothing is traded away.
        for (int b = 0; b < controls.button_count && b < daisyapps::Controls::kMaxButtons; b++) {
            const bool down = controls.button[b];
            if (down && !prev_button[b]) {
                if (b == 0)      { engine.on_play_pad(s_deck, false); s_play_presses++; s_play_ms = now_ms; }
                else if (b == 1) engine.on_record_pad(s_deck, false);
            }
            prev_button[b] = down;
        }

        // --- Encoder ----------------------------------------------------------------------------
        const bool held = controls.enc_press;
        const int  inc  = controls.enc_inc;

        ui.tick(now_ms);   // an action screen left open eventually hands the encoder back to the pages

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
                    // A CLICK. On a board with a screen it drives the ACTION SCREEN (param_ui.h):
                    // the first click opens the list, every further click fires the highlighted row.
                    // That is the whole of IEngine's non-knob surface - the play/record pads and the
                    // categorical switches - on the one button a Daisy Patch has.
                    //
                    // A screenless board (Pod, patch.init()) would be entering an invisible mode, so
                    // there the click keeps its direct meaning: switch deck on a dual-deck engine,
                    // else cycle Mode. Those boards reach the pads through their buttons instead.
                    if constexpr (daisyapps::Board::kHasScreen) {
                        if (!ui.in_actions()) {
                            ui.open_actions(engine, now_ms);
                        } else {
                            const daisyapps::ActionRow::Kind fired = ui.fire(engine, now_ms);
                            if (fired == daisyapps::ActionRow::Play) { s_play_presses++; s_play_ms = now_ms; }
                            s_deck = ui.deck();   // the `deck` row moves the focus the UI owns
                        }
                    } else {
                        if (engine.capabilities() & daisyapps::CapDualDeck) {
                            s_deck = (s_deck == DeckRef::A) ? DeckRef::B : DeckRef::A;
                            ui.set_deck(s_deck, engine);
                        } else {
                            mode_config = (mode_config + 1) % 3;
                            engine.set_config(ConfigId::Mode, s_deck, mode_config);
                        }
                    }
                }
            }
            // A turn moves the action cursor while the list is open, and the page otherwise.
            if (inc != 0) {
                if (ui.in_actions()) ui.move_cursor(inc, now_ms);
                else                 ui.set_page(ui.page() + inc, engine);
            }
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

        // Push the clock-input rate when the action screen's row moves. Polled rather than pushed from
        // the UI so param_ui.h stays ignorant of the transport - it cycles an index and nothing else.
        if (ui.clock_in() != clock_in_applied) {
            clock_in_applied = ui.clock_in();
            transport.set_ext_ppq(daisyapps::HarnessTransport::kExtPpqChoices[clock_in_applied]);
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
        //
        // Header right-hand slot: the TEMPO by default, the stream/play status when there is something
        // to say. `status` reads "P2" (a file is streaming, 2 play presses) or "-2" (nothing streaming);
        // it answers "did the gesture arrive, and is a file actually playing", which is worth the slot
        // right after a press and while a stream is live, and worth nothing the rest of the time. The
        // tempo is worth it the rest of the time: `delay`, `qdelay` and `edrums` are tempo-synced, and
        // the hold+turn tempo gesture below is otherwise completely blind.
        const bool tempo_gesture = held && (engine.capabilities() & daisyapps::CapAux) == 0;
        const bool stream_live   = s_stream.is_playing(s_deck);
        const bool press_recent  = s_play_ms != 0 && (now_ms - s_play_ms) < kStatusHoldMs;

        char        status[12];
        const char* status_p = nullptr;
        if (!tempo_gesture && (stream_live || press_recent)) {
            std::snprintf(status, sizeof(status), "%c%d",
                          stream_live ? 'P' : '-', s_play_presses % 10);
            status_p = status;
        }
        // One screen, two possible owners. The action list wins whenever it is open; otherwise the
        // engine page is drawn by the adapter (which owns the DisplayModel) and every other page by
        // the parameter UI. ParamUI::render draws nothing when the engine page is showing, so these
        // two never fight over the panel.
        ui.render(board, engine, SPK_ENGINE_STR, transport.tempo(), now_ms, status_p);
        if (ui.on_engine_page() && !ui.in_actions())
            leds.render_screen(board, engine, s_deck, SPK_ENGINE_STR, now_ms);

        // Indicator LEDs. Rate-limited inside tick(); on a board with none this is not compiled at
        // all. An engine that draws gets its own panel projected; one that does not gets the page hue
        // and the knob-pickup state, which on a screenless board is the paged UI's only feedback.
        leds.tick(board, engine, s_deck, ui.page(), ui.pages(), ui.all_caught(), now_ms);
    }
}
