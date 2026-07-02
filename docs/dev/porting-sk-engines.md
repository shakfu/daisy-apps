# Porting sk-engines Engines to daisy-apps

This guide explains how to bring a DSP engine from the [sk-engines](https://github.com/shakfu/sk-engines)
firmware (a Spotykach platform/engine fork) into this repository as a standalone Daisy Pod harness.

It is written for someone who has the two existing ports (`csound`, `chuck`) in front of them and wants
to add a third. Read it top to bottom once; after that the per-engine notes and the checklist at the end
are the working reference.

## 1. Mental model

sk-engines is a fixed hardware/UI **platform** with a swappable DSP **engine** behind a single C++
contract, `IEngine`. The platform (encoders, LED rings, OLED, pads, transport/clock, SD storage, MIDI)
knows nothing about any specific engine; each engine implements `IEngine` and reads what it needs from an
injected `EngineContext`. That decoupling is enforced upstream by `make check-boundary`.

daisy-apps reuses that same contract but replaces the whole platform with a **thin harness** on a bare
Daisy Pod (or Patch / patch.init() via the `src/board/` abstraction). A harness is ~150 lines: bring up
the board, build an `EngineContext`, call `engine.init(ctx)`, drive `engine.process()` from the audio
callback, and map two knobs plus MIDI onto engine parameters. There is no OLED, no LED ring, no pad
matrix, and no full transport service.

The consequence, and the single most important idea in this guide: **porting an engine is not a DSP
task**. The DSP already compiles for the STM32H750 and already targets this exact `IEngine`. Porting is
(a) copying the engine's source tree plus its in-tree dependencies, (b) adapting it to daisy-apps's
*trimmed* copy of the contract, and (c) providing, in the harness, the handful of `EngineContext`
services that particular engine reads. Most of the work is mechanical.

The two engines already ported (`csound`, `chuck`) are, by dependency weight, the **hardest** ones in the
whole set: each needs an externally cross-built static library (`libcsound.a` / `libchuck.a`), a QSPI
build, and SD access. Everything recommended below is strictly easier than what already works.

## 2. What daisy-apps provides vs. what sk-engines assumes

### 2.1 The contract is a trimmed subset

daisy-apps's `src/engine/iengine.h` is a copy of the upstream contract with the **display and LED-ring
methods removed** (there is no screen or ring on the harness). Relative to sk-engines, daisy-apps's
`IEngine` drops these virtuals:

| Dropped in daisy-apps | Purpose upstream |
|---|---|
| `render(DisplayModel&)` | fills the OLED model |
| `render_ring(LEDRing&, ...)` | draws the encoder LED ring |
| `fx_leds` / `play_leds` / `alt_leds` / `deck_leds` | pad/mode LED state |
| `mix()` / `route()` | reports mix level / stereo topology to the platform |

`src/engine/engine_params.h` correspondingly drops the `CapOwnDisplay` capability bit, and both headers
live in **`namespace daisyapps`** rather than upstream's **`namespace spotykach`**.

So every engine you port needs two edits before it will even compile here:

1. **Rename the namespace** `spotykach` -> `daisyapps` in the engine's `.h`/`.cpp`.
2. **Remove the display/LED overrides.** Delete the `render(...)`, `render_ring(...)`, `*_leds(...)`,
   `mix()`, and `route()` method declarations/definitions, and drop any `#include "engine/display_model.h"`
   / `"engine/led.ring.h"`. If the engine advertises `CapOwnDisplay`, remove that bit from its
   `capabilities()`. The DSP path never calls these; they exist only for the platform UI.

If you would rather keep the engine source verbatim, the alternative is to *extend* daisy-apps's contract
(copy `display_model.h`, `led.ring.h`, the dropped virtuals, and `CapOwnDisplay` back in) and let the
harness ignore them. That is more code to carry and buys nothing on a Pod, so **stripping is the default**;
extend only if you are chasing exact upstream parity or intend to grow a real display target.

### 2.2 EngineContext: the services the harness injects

`EngineContext` (see `src/engine/engine_context.h`) is the only channel from harness to engine. Its fields,
and their status in the current harnesses:

| Field | Type | What reads it | Harness status today |
|---|---|---|---|
| `sample_rate` | `float` | every engine | provided (`board.SampleRate()`) |
| `block_size` | `float` | most engines | provided (`kBlock`, 256) |
| `arena` | `EngineArena {base,bytes}` | any engine with SDRAM buffers (delay lines, tape/loop RAM, grain clouds) | **empty `{nullptr,0}`** - csound/chuck use their own pools |
| `time` | `const ITimeSource*` | clock-reading engines | **null** |
| `transport` | `ITransport*` | tempo-synced engines (delay, qdelay, edrums, ...) | **null** |
| `stream` | `IStreamDeck*` | SD audio engines (tape, radio, pstretch, softcut) | provided but **audio-ring methods stubbed** |
| `qspi` | `void*` (`QSPIHandle*`) | engines persisting presets to flash (edrums kit) | **null** |

The three fields in bold are the harness glue you will add. They are shared: once you provision the SDRAM
arena you have unblocked every buffer engine; once you add a minimal transport you have unblocked every
tempo-synced engine; once you implement the streaming rings you have unblocked the SD engines. See
section 5.

### 2.3 The SD stream deck only does patch banks today

daisy-apps's `pod/sd_stream_deck.h` (`SdStreamDeck`) implements only the two "text" methods of
`IStreamDeck` - `exists()` (an `f_stat` probe) and `read_text()` (used by csound/chuck to load a numbered
`.csd`/`.ck` patch bank). The **audio streaming** methods - `play_consume`, `record_produce`,
`start_play`, `start_record`, `is_playing`, `scan_bank` - are present but return `0`/`false`. Any engine
that streams audio from the card needs those filled in (section 5.3).

## 3. Portability tiers

Engines group by their heaviest dependency. Effort rises down the table. "In-tree deps" means source you
copy into the repo (no external build); "cross-built lib" means a separate toolchain step.

| Tier | Engines | Memory | External build | SD audio | Extra harness glue |
|---|---|---|---|---|---|
| **1 - self-contained DSP** | `passthrough`, `glitch`, `reverb` (Faust, in-tree), `chorus` / `filter` / `voice` (Faust demos), `gigaverb` (gen~, in-tree) | SRAM | none | no | arena (if the engine buffers) |
| **1b - self-contained + clock** | `delay`, `qdelay`, `edrums` | SRAM | none | no | arena + minimal transport; edrums optionally `qspi` for kit save |
| **2 - vendored third-party** | `mosc` (Plaits, QSPI), `reso` (Rings), `graincloud` (GrainflowLib), `shuttle` | SRAM (`mosc` QSPI) | none - copy the `thirdparty/` tree | no (`shuttle` SD is save/load only) | arena; often build `-Os` to fit SRAM |
| **3 - SD audio streaming** | `tape`, `radio`, `pstretch`, `softcut` (also vendors softcut-lib) | SRAM | none | **yes** | real `IStreamDeck` rings |
| **4 - external cross-built lib** | `csound`, `chuck` | QSPI | `libcsound.a` / `libchuck.a` | yes | **done** |

Recommended order of attack for a synthesis sandbox that already forwards MIDI NoteOn:

1. **`delay`** first - simplest DSP, and it forces you to build the arena + minimal-transport glue that
   unblocks all of tier 1b and most of tier 2. Best learning port.
2. **`mosc`** (Plaits, 24 macro-oscillator models) and **`reso`** (Rings resonator) next - the highest
   value for a MIDI-playable Pod. No SD, no external build; `mosc` reuses the csound/chuck QSPI pattern,
   `reso` fits SRAM at `-Os`. `reso` already implements `handle_midi_note`.
3. A couple of tier-1 FX (`reverb`, `gigaverb`) as quick wins.
4. Defer tier 3 (`tape` / `radio` / `softcut`) until you decide daisy-apps should grow a real SD streaming
   layer.

## 4. Step-by-step port procedure

Concrete walkthrough. Replace `<eng>` with the engine name (e.g. `delay`).

### 4.1 Copy the engine sources and in-tree dependencies

Copy `sk-engines/src/engine/<eng>/` into `daisy-apps/src/engine/<eng>/`. Then chase its includes and copy
what is not already here:

- **`src/dsp/` helpers.** daisy-apps has **no `src/dsp/`** yet. Engines pull in small shared DSP headers
  from there - e.g. `delay`/`qdelay` need `dsp/diffuser.h` and `dsp/deline.h`; `edrums` needs
  `dsp/cpattern.h`, `dsp/biquad.h`, `dsp/lutsinosc.h`, `dsp/divider.h`. Copy just the files the engine
  references (grep its `#include "dsp/...`), creating `daisy-apps/src/dsp/`.
- **`engine/arena.h`.** Buffer engines sub-allocate the injected SDRAM through `Arena` (see
  `sk-engines/src/engine/arena.h`). Copy it to `daisy-apps/src/engine/arena.h` if absent.
- **Vendored third-party trees (tier 2).** Copy the engine's own `thirdparty/` verbatim, plus the shared
  `src/engine/common/thirdparty/stmlib` that `reso`/`mosc` both use. These are self-contained C++; no build
  step, they compile as part of the harness.
- **Generated C++ (Faust / gen~).** `reverb`/`chorus`/`filter`/`voice` ship generated Faust C++ in-tree;
  `gigaverb` ships gen~ output plus glue (`_ext_daisy.cpp`, `src/engine/gen/genlib_arena.cpp`). Copy the
  generated files and the glue TU - there is nothing to regenerate.

### 4.2 Adapt to the trimmed contract

Apply the two edits from section 2.1: rename `namespace spotykach` -> `namespace daisyapps`, and remove the
display/LED overrides and their includes. Compile-checking the engine TU in isolation is the fastest way to
find every leftover reference.

### 4.3 Write the harness

Copy `pod/harness_csound.cpp` to `pod/harness_<eng>.cpp` and adapt. The skeleton is always:

```cpp
#include "daisy_seed.h"
#include "board/board.h"
#include "engine/<eng>/<eng>_engine.h"

static const int kBlock = 256;              // or the engine's preferred block
daisyapps::Board       board;
daisyapps::<Eng>Engine engine;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out, size_t size) {
    engine.process(in, out, size);          // Daisy's non-interleaved buffers match IEngine::process
}

int main(void) {
    board.Init(kBlock);
    daisyapps::EngineContext ctx{};
    ctx.sample_rate = board.SampleRate();
    ctx.block_size  = static_cast<float>(kBlock);
    // ... fill arena / transport / stream as the engine requires (section 5) ...
    engine.init(ctx);
    board.StartAudio(AudioCallback);
    board.StartMidi();

    daisyapps::Controls controls;
    while (1) {
        board.Poll(controls);
        board.PollMidi([](uint8_t st, uint8_t d1, uint8_t d2) {
            if ((st & 0xf0) == 0x90 && d2 > 0)
                engine.handle_midi_note(static_cast<uint8_t>(st & 0x0f), d1);
        });
        // map controls.analog[i] -> engine.set_param(ParamId::..., DeckRef::A, value)
        engine.prepare();
    }
}
```

Notes drawn from the existing harnesses:

- **QSPI VTOR inject.** Only QSPI builds (`mosc`, and the csound/chuck pattern) need the
  `SCB->VTOR = 0x90040000; __DSB(); __ISB();` prologue. SRAM engines omit it.
- **Control cadence.** Push `set_param` at most once per audio block. The chuck harness learned this the
  hard way: calling `set_param` from the unthrottled main loop floods a queue-based engine. For plain C++
  DSP engines (delay, reso, mosc) a main-loop `set_param` is fine, but keep it deadbanded to avoid zipper
  noise from pot jitter (a `fabsf(new-last) > 0.004f` gate plus a one-pole smoother, as in
  `harness_chuck.cpp`).
- **Control mapping.** The Pod gives two pots (`analog[0]`, `analog[1]`), an encoder (`enc_inc`,
  `enc_press`), and two buttons. Map pot 0 -> the engine's primary continuous param and pot 1 -> mix/level.
  Reuse the encoder-hold gesture as the `Aux`/`Alt+PITCH` selector (`set_aux_active` + `ParamId::Aux`) if
  the engine has multiple models to browse (mosc's engine select, edrums' kit). Switch-position config
  (`set_config(ConfigId::Mode/Route, ...)`) has no dedicated Pod control - drive it from a button or leave
  it at a sensible default.

### 4.4 Write the Makefile

Copy `pod/Makefile` (SRAM engines) or keep the `BOOT_QSPI` shape of the csound/chuck Makefiles (QSPI
engines like `mosc`). The variables you touch:

- `TARGET` - output name (`harness_<eng>`).
- `CPP_SOURCES` - `harness_<eng>.cpp` plus every engine `.cpp` and copied dependency `.cpp` (dsp helpers,
  vendored TUs, gen/faust glue).
- `C_INCLUDES` - `-I../src` (always) plus any vendored `thirdparty/` include roots.
- `C_DEFS` - the board define comes from the `BOARD` switch already in the template; add engine defines
  (e.g. `M_PI=3.14159265358979323846` for stmlib engines, `SPK_USE_STREAM` for SD engines).
- `CPP_STANDARD = -std=gnu++17` - required (the contract headers use `std::clamp`).
- `USE_FATFS = 1` - only if the engine touches the SD card.
- **SRAM vs QSPI.** SRAM is the default linker script. `mosc` needs QSPI because the 24-model Plaits voice
  overflows the 186 KB execution SRAM; reuse the `APP_TYPE = BOOT_QSPI` setup from the chuck Makefile. Note
  the repo's `alt_qspi_chuck.lds` is **ChucK-specific** (its header says as much - it hands ChucK the full
  AXI SRAM and relocates the stack), so do not reuse it verbatim for `mosc`. Copy sk-engines' generic
  `alt_qspi.lds` (the plain QSPI-execution variant) instead, and point `LDSCRIPT` at it.
- **`-Os`.** `reso`, `mosc`, and `graincloud` are built at `-Os` upstream to fit; carry that over
  (`OPT = -Os`) if a `-O2` build overflows.

Because the shared `build/` directory is reused across harnesses with different defines, `rm -rf build`
when switching which engine you are building.

### 4.5 SD patch banks / example content

If the engine loads content from SD (csound/chuck orchestras; radio banks; tape/softcut clips), add an
`examples/<eng>/` tree and teach `scripts/provision_sd.sh` / `make sd-card` about it, mirroring
`examples/csound/` and `examples/chuck/`.

## 5. The shared harness glue (build once, reuse everywhere)

These three additions unblock whole tiers. Build them the first time an engine needs them, then every
later port just points `ctx.*` at them.

### 5.1 SDRAM arena (unblocks all buffer engines - tiers 1b, 2, 3)

The Daisy has 64 MB of SDRAM. Reserve a chunk with libDaisy's `DSY_SDRAM_BSS` attribute (already defined
by `daisy_seed.h`, via `libDaisy/src/dev/sdram.h` - do not redefine it) and hand it to the engine:

```cpp
static uint8_t DSY_SDRAM_BSS s_arena[48 * 1024 * 1024];   // size to the engine's needs
...
ctx.arena = { s_arena, sizeof(s_arena) };
```

The engine sub-allocates via `Arena` (copied in step 4.1). Delay lines, tape/loop RAM, grain buffers, and
reverb tails all come from here. Size it per engine - the delay's ~6 s lines need only a few MB; a tape or
graincloud engine wants tens of MB.

### 5.2 Minimal transport / time source (unblocks tempo-synced engines - `delay`, `qdelay`, `edrums`)

Upstream, the platform owns a full transport service (internal / TS4 / MIDI clock) and injects a read-only
`ITransport`. The harness does not need all of that. Implement the two interfaces
(`src/engine/itransport.h`, `src/engine/itimesource.h`) with a minimal **internal free-running clock**: a
fixed or knob-set BPM, a sample counter advanced in the audio callback, and the tick/phase queries the
engine actually calls. Read the specific engine's `ctx.transport->...` call sites to see the minimal set it
needs - a tempo-synced delay typically reads only the current BPM. Wire MIDI clock later if you want
external sync (the board already receives MIDI; forward 0xF8/Start/Stop into the transport).

### 5.3 Real SD audio streaming (unblocks tier 3 - `tape`, `radio`, `pstretch`, `softcut`)

Fill in the stubbed `IStreamDeck` audio methods in `pod/sd_stream_deck.h`. The contract's threading rules
(documented in `src/engine/istreamdeck.h`) are the hard part, not the FatFs calls:

- `play_consume` / `record_produce` run in the **audio ISR** and must never block - they only touch
  lock-free rings.
- `start_play` / `start_record` / `stop` run on the **main loop** and do the FatFs I/O, refilling/draining
  the rings.
- `scan_bank` (radio) enumerates 8.3 filenames + frame counts for a bank directory.

This is the one genuinely non-trivial piece of glue. Until it exists, tier 3 engines cannot run, and that
is the reason to defer them.

## 6. Per-engine notes

- **`delay` / `qdelay`** - tier 1b. Copy `dsp/diffuser.h` + `dsp/deline.h`. Needs arena (a few MB for the
  delay lines) and the minimal transport (reads BPM for tempo-synced divisions). `qdelay` = delay with a
  Diffuse/Duck character palette; same glue. Strip `render()` + `route()`. Good first port.
- **`reverb`** - tier 1. Three Faust algorithms generated in-tree (Dattorro / Zita / Greyhole). SRAM, no
  SD, no clock. Buffers come from the arena. Strip display overrides and port.
- **`gigaverb`** - tier 1. gen~-translated C++; copy the generated files plus `_ext_daisy.cpp` and
  `src/engine/gen/genlib_arena.cpp`, and add the `-Wno-unused-*` options and gen include roots the
  upstream CMake sets.
- **`chorus` / `filter` / `voice`** - tier 1, the smallest ports. Pure generated-Faust demos (single FX,
  dual parallel filter, and instrument->filter chain respectively). Minimal glue.
- **`edrums`** - tier 1b. Four-voice Euclidean drums; needs the transport for its clock and,
  optionally, `ctx.qspi` to persist kit presets to flash (leave null to disable saving). Copy the
  `dsp/cpattern.h` / `biquad.h` / `divider.h` helpers.
- **`mosc`** - tier 2, top recommendation for a MIDI Pod. Full Plaits 24-model macro-oscillator. Copy
  `src/engine/mosc/thirdparty` + `common/thirdparty/stmlib`. **QSPI build** (too big for SRAM) - reuse the
  chuck Makefile's QSPI setup and the VTOR prologue. No SD, no external library, synthesizes from the
  arena. Build `-Os`. Map pot 0 -> note/pitch, encoder-hold + pot -> engine select (`ParamId::Aux`), pots
  -> harmonics/timbre/morph.
- **`reso`** - tier 2. Mutable Rings resonator/plucked-string. Copy `src/engine/reso/thirdparty` +
  `stmlib`. SRAM at `-Os` (Rings' ~30 KB of code+tables overflow at `-O2`). Already implements
  `handle_midi_note` - plays over MIDI immediately.
- **`graincloud`** - tier 2. Vendors GrainflowLib; large (~388 KB, 51 files). Arena-heavy; build `-Os`.
- **`shuttle`** - tier 2. Buffer-based varispeed tape, four in-RAM tracks from the arena. Marked
  `SPK_USE_STREAM` only for SD clip load/save; you can port the in-RAM DSP first and stub the save/load.
- **`softcut`** - tier 3. Vendors monome's softcut-lib (`src/engine/softcut/vendor`) and streams clip
  load/save over SD. Port the vendored core + engine, then the streaming rings (5.3) for save/load.
- **`tape` / `radio` / `pstretch`** - tier 3. Their whole point is streaming from SD, so they need 5.3
  before they do anything useful. `radio` additionally needs `scan_bank`.

## 7. Build, flash, verify

From `pod/`:

```
rm -rf build                                   # when switching engines (shared build dir)
make -f Makefile.<eng>                          # -> build/harness_<eng>.bin
while ! make -f Makefile.<eng> program-dfu; do sleep 0.2; done   # catch the DFU window, then tap RESET
```

Bring-up tips lifted from the chuck harness: use `board.SetUserLed()` / a `blink(n)` helper to prove how
far `init()` got (the onboard LED is visible even on a cased unit), and toggle it slowly in the audio
callback as a "the ISR is alive" heartbeat. If a QSPI app does not boot, the `SCB->VTOR` inject is the
first thing to check.

Validate on hardware: confirm audio out, sweep both pots, and (for MIDI engines) play NoteOn from a
controller. The Pod is the only board validated on hardware; the Patch / patch.init() drivers compile but
are untested.

## 8. Port checklist

- [ ] Copy `src/engine/<eng>/` and every in-tree dependency (`src/dsp/*` helpers, `engine/arena.h`,
      `thirdparty/` trees, `stmlib`, generated Faust/gen glue).
- [ ] Rename `namespace spotykach` -> `namespace daisyapps` in the engine sources.
- [ ] Remove display/LED overrides (`render`, `render_ring`, `*_leds`, `mix`, `route`) and their includes;
      drop `CapOwnDisplay` from `capabilities()`.
- [ ] Provision the `EngineContext` fields the engine reads: arena (5.1), transport/time (5.2),
      stream (5.3), qspi - as required by its tier.
- [ ] Write `pod/harness_<eng>.cpp` from the template; map two pots + encoder + MIDI onto its params;
      throttle/deadband `set_param`.
- [ ] Write `pod/Makefile.<eng>`: sources, includes, defines, `-std=gnu++17`, SRAM-vs-QSPI linker,
      `-Os` if it overflows, `USE_FATFS` only if it uses SD.
- [ ] Add `examples/<eng>/` + `make sd-card` wiring if it loads SD content.
- [ ] `rm -rf build`, build, flash, and verify audio + pots + MIDI on hardware.
- [ ] Note the new engine in `README.md` and `CHANGELOG.md`.
