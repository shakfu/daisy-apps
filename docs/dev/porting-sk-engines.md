# Porting sk-engines Engines to daisy-apps

This guide explains how to bring a DSP engine from the [sk-engines](https://github.com/shakfu/sk-engines)
firmware (a Spotykach platform/engine fork) into this repository.

Since the `app/` engine host landed, a port is a much smaller job than it was: copy the engine's source
tree with one substitution, add two blocks of build glue, done. Sections 1-2 explain why; section 4 is
the procedure; section 6 has the per-engine notes.

## 1. Mental model

sk-engines is a fixed hardware/UI **platform** with a swappable DSP **engine** behind a single C++
contract, `IEngine`. The platform (encoders, LED rings, OLED, pads, transport/clock, SD storage, MIDI)
knows nothing about any specific engine; each engine implements `IEngine` and reads what it needs from
an injected `EngineContext`.

daisy-apps reuses that same contract and replaces the platform with `app/harness.cpp`, a generic host:
it brings up a board, builds an `EngineContext`, calls `engine.init(ctx)`, drives `engine.process()`
from the audio callback, and maps a Daisy Patch's four knobs, encoder, gates, MIDI and CV outs onto
`IEngine`. It knows nothing about any particular engine either - the control surface is generated from
the engine's own `live_params()` / `param_label()` declarations (see `app/README.md`).

The single most important idea in this guide: **porting an engine is not a DSP task.** The DSP already
compiles for the STM32H750 and already targets this exact `IEngine`. Porting is copying a source tree
and adding build glue. The two hardest engines in the whole set - `csound` and `chuck`, each needing an
externally cross-built static library and a QSPI build - were ported before any of this existed, and
keep their own harnesses under `pod/`.

## 2. What daisy-apps provides vs. what sk-engines assumes

### 2.1 The contract is a full copy, not a subset

`src/engine/iengine.h`, `engine_params.h`, `engine_context.h`, `mode.h`, `deck_ref.h` and
`terminal_io.h` are upstream's files with **one substitution**: `namespace spotykach` ->
`namespace daisyapps`. That includes the display/LED region (`render(DisplayModel&)`, `render_ring`,
the `*_leds` queries, `mix()`, `route()`) and the `SPK_TERMINAL` block.

This is a deliberate reversal of what this guide used to say. It previously told you to *strip* the
display overrides from each engine, because a Pod harness has no screen and the alternative was more
code to carry. Two things changed that:

- **The shared Faust and gen~ wrappers depend on it.** `engine/faust/faust_fx.h` and `faust_chain.h`
  include `display_model.h` and `indicators.h` directly, and every Faust engine (`reverb`, `chorus`,
  `filter`, `voice`, and `tape` later) is a thin `Traits` specialization of them. Stripping would mean
  rewriting the shared wrappers, not editing a per-engine override - once, and then again on every
  upstream sync.
- **There is now a target with a screen.** The Daisy Patch has a 128x64 OLED, which is the condition
  this guide already named for extending rather than stripping.

The knock-on effects are worth knowing:

- **`SPK_TERMINAL` is defined** (`-DSPK_TERMINAL=1`, in `app/Makefile`). daisy-apps has no serial
  terminal; the flag is on because engines declare their param liveness masks and knob **labels**
  inside that block, and those two declarations are exactly what the paged OLED UI needs to page and
  name an engine's params with no per-engine code. Everything else in the block is inert.
- **The contract now pulls in libDaisy transitively.** `iengine.h` -> `display_model.h` ->
  `led.ring.h` -> `color.h` -> `common.h` -> `<daisy.h>`. Upstream has the same shape and compiles the
  contract on a host through stub headers; daisy-apps has no host build, so nothing is blocked today.
  It would need addressing before one exists.
- **`abi_tag.h` came with `iengine.h`.** `SPK_TERMINAL` changes IEngine's vtable, so the contract wraps
  itself in an inline namespace encoding the flag - a mismatched object becomes a link error naming the
  symbol rather than a firmware that misbehaves like a hardware fault. `app/` (flag on) and `pod/`
  (flag off) are separate builds, so they simply never mix.

The practical result: **an engine's sources need no edits at all beyond the namespace**, which
`scripts/port_engine.sh` does for you.

### 2.2 EngineContext: the services the harness injects

`EngineContext` is the only channel from harness to engine. Its fields, and their status in
`app/harness.cpp`:

| Field | Type | What reads it | Status |
|---|---|---|---|
| `sample_rate` | `float` | every engine | provided (`board.SampleRate()`) |
| `block_size` | `float` | most engines | provided (48 frames = 1 ms) |
| `arena` | `EngineArena {base,bytes}` | any engine with SDRAM buffers | **provided** - 48 MB `DSY_SDRAM_BSS` |
| `time` | `const ITimeSource*` | clock-reading engines | **provided** - `SystemTime` over `daisy::System` |
| `transport` | `ITransport*` | tempo-synced engines (delay, qdelay, edrums) | **provided** - `HarnessTransport` |
| `stream` | `IStreamDeck*` | SD audio engines (tape, radio, pstretch, softcut, bard) | **provided** - the real `StreamDeck` for `SPK_USE_STREAM` builds |
| `qspi` | `void*` (`QSPIHandle*`) | engines persisting presets to flash (edrums kit) | **provided** - `Board::Qspi()` |

All three of the fields that once blocked whole tiers are done. `HarnessTransport`
(`app/harness_clock.h`) is a free-running 48-PPQN clock at a settable BPM, steerable by quarter-note
pulses at a gate input, with the tick fan-out `ITransport` specifies - more than the delay needs (it
reads `tempo()` and nothing else) and enough for a sequencing engine. Streaming is section 5.

Every field is now populated. `ctx.qspi` was the last, and only `edrums` reads it - to persist kit
presets at a 64 KB flash offset, clear of the calibration settings and the app image.

### 2.3 Two IStreamDeck implementations

The engine picks which one its build gets, by setting `SPK_USE_STREAM` in its Makefile block:

- **`src/hw/stream_deck.h` (`StreamDeck`)** - the real thing, ported from upstream: lock-free SDRAM
  rings touched by the audio ISR, FatFs I/O pumped from the main loop, per-deck play/record state
  machines, WAV and headerless-raw readers, bank scanning. Selected by `SPK_USE_STREAM`.
- **`src/sd_stream_deck.h` (`SdStreamDeck`)** - the lightweight deck the csound/chuck patch banks use:
  `exists()` and `read_text()` over a mounted card, streaming half stubbed. The default.

Both mount through the shared `src/sd_card.h`. `hw/stream_deck.cpp` and `hw/fat_file.cpp` are wrapped
in `#if defined(SPK_USE_STREAM)` and compile to nothing without it, so a non-streaming engine's build
is byte-identical to one where they were never listed.

## 3. Portability tiers

Engines group by their heaviest dependency. "In-tree deps" means source you copy into the repo (no
external build); "cross-built lib" means a separate toolchain step.

| Tier | Engines | Memory | External build | SD audio | Status |
|---|---|---|---|---|---|
| **1 - self-contained DSP** | `passthrough`, `glitch`, `reverb`, `chorus`, `filter`, `voice`, `gigaverb` | SRAM | none | no | **ported** |
| **1b - self-contained + clock** | `delay`, `qdelay`, `edrums` | SRAM | none | no | **ported** (`ctx.qspi` is now provided, so edrums saves kits) |
| **2 - vendored third-party** | `reso` (Rings), `mosc` (Plaits, QSPI), `granular`, `graincloud` (GrainflowLib) | SRAM (`mosc` QSPI) | none - copy the `thirdparty/` tree | no | **ported** |
| **3 - SD audio streaming** | `tape`, `radio`, `pstretch`, `softcut`, `bard`, `shuttle` | SRAM | none | **yes** | **ported** |
| **4 - external cross-built lib** | `csound`, `chuck` | QSPI | `libcsound.a` / `libchuck.a` | yes | **done**, own harnesses in `pod/` |

**Every engine in the upstream set is now ported.** What remains is not porting but polish: the
`granular` / `graincloud` IEngine surface is wide enough (recording, sequencer, tape storage, launch
quantization) that four knobs and one encoder cannot reach all of it, so those two would benefit from
UI work beyond the generic pager.

Three things the later ports established:

- **`.cc` sources need a rule.** Mutable's code is `.cc`, which stock libDaisy's Makefile does not
  compile; `app/Makefile` adds one after the include (see `ENGINE_CC_SOURCES`). It must use `CPPFLAGS`,
  not `CXXFLAGS` - libDaisy never defines the latter, and a rule using it compiles with no includes,
  no defines and no MCU flags, which fails in a thoroughly confusing way.
- **A QSPI engine needs the VTOR prologue.** `mosc` is BOOT_QSPI, and `app/Makefile` emits
  `-DHARNESS_BOOT_QSPI` for any `APP_TYPE=BOOT_QSPI` build so `harness.cpp` points the vector table at
  the app. libDaisy's own `BOOT_APP` define cannot gate this - it is set for BOOT_SRAM too.
- **Watch for the libDaisy fork's API.** sk-engines builds against bleeptools/libDaisy, and two of its
  divergences from stock reach engine code: `.cc` compilation (above) and `PersistentStorage`, whose
  fork version takes three template arguments and the QSPI handle in `Init()`. `edrums` is the only
  engine that touches the latter, and its store is rewritten against the stock one-argument API - the
  single source edit any ported engine has needed beyond the namespace.

## 4. Step-by-step port procedure

### 4.1 Copy the engine

```
scripts/port_engine.sh <engine> [path-to-sk-engines]
```

It copies `sk-engines/src/engine/<eng>/` - including any `thirdparty/` or `vendor/` tree the engine
ships - into `src/engine/<eng>/`, rewriting `spotykach` -> `daisyapps` in every source file and copying
everything else byte-for-byte. It then lists the engine's cross-tree includes and flags any that this
repo does not have.

Anything reported `MISSING` has to be copied by hand. In practice that means:

- **`src/dsp/` helpers** - small shared DSP headers (`dsp/diffuser.h` is here; `edrums` would also want
  `dsp/cpattern.h`, `biquad.h`, `lutsinosc.h`, `divider.h`; `bard` wants `dsp/biquad.h`).
- **The shared stmlib tree** - `reso` and `mosc` both need `sk-engines/src/engine/common/thirdparty/stmlib`.

### 4.2 Adapt to the contract

Nothing to do. See section 2.1 - the namespace rename the script already applied is the whole job.

### 4.3 Write the harness

Nothing to do. `app/harness.cpp` hosts any engine; the control surface comes from the engine's own
declarations. Two optional refinements are worth checking:

- **Does the engine narrow `live_params()`?** If not it inherits the all-live mask and the UI pages all
  24 slots. Adding the mask upstream (inside its `SPK_TERMINAL` block) is the right fix, and it makes
  `describe` honest there too.
- **Does it need a control this harness does not offer?** The panel map is in `app/README.md`. Anything
  categorical beyond `ConfigId::Mode` currently has no gesture.

### 4.4 Add the build glue

Two edits, both mechanical:

1. **`src/engine/engine_select.h`** - an `#elif defined(SPK_ENGINE_<ENG>)` arm including the engine
   header and aliasing `ActiveEngine`.
2. **`app/Makefile`** - an `else ifeq ($(ENGINE), <eng>)` block, plus the name in `ALL_ENGINES`. What
   goes in it, copying whatever the upstream Makefile's block for that engine sets:
   - `C_DEFS += -DSPK_ENGINE_<ENG>` (the define `engine_select.h` tests)
   - `ENGINE_SOURCES` - every `.cpp`/`.cc` the engine and its vendored tree contribute (empty for a
     header-only engine)
   - an include root for a vendored tree (`RESO_INC`-style), added to `C_INCLUDES`
   - `OPT = -Os` if it overflows at `-O2` (upstream does this for `reso`, `mosc`, `reverb`, `graincloud`)
   - `APP_TYPE = BOOT_QSPI` for an engine too big for the 480 KB SRAM (`mosc`)

Then:

```
cd app && make ENGINE=<eng> BOARD=patch
```

### 4.5 SD content

If the engine loads content from SD, add an `examples/<eng>/` tree and teach `scripts/provision_sd.sh`
/ `make sd-card` about it, mirroring `examples/csound/` and `examples/chuck/`.

## 5. SD audio streaming (done)

This was the last blocking piece of shared glue, and it was **ported rather than written**. Upstream's
`hw/stream_deck.{h,cpp}` already implements the contract and runs on hardware; the threading rules are
the delicate part of streaming, so reimplementing them would have been strictly worse than taking the
version that works. What came over:

| File | What it is |
|---|---|
| `src/hw/stream_deck.{h,cpp}` | the service: per-deck state machines, rings, bank scanning |
| `src/hw/fat_file.{h,cpp}` | the FatFs handle wrapper behind `ByteFile` |
| `src/memory/spsc_ring.h` | the lock-free single-producer/single-consumer ring |
| `src/memory/audio_stream.h` | play/record streams over a ring + scratch |
| `src/memory/wav_stream.h`, `raw_stream.h`, `wav.h`, `byte_file.h` | WAV / headerless-raw sources and the header parser |
| `src/engine/istreamdeck.h` | replaced with upstream's full version (adds `seek_play`, `write_text`, `bank_sort`) |

The harness provides the memory and the pump (`app/harness.cpp`): 1 MB power-of-two ring per deck plus
32 KB of shared scratch in SDRAM, and one `s_stream.process()` call per main-loop pass. That is the
platform's entire duty - upstream's own app does nothing else with the service.

The rules the contract documents, for reference when reading engine code:

- `play_consume` / `record_produce` run in the **audio ISR** and never block - rings only.
- `start_play` / `start_record` / `stop` run on the **main loop** and do the FatFs I/O.
- `scan_bank` (radio) enumerates 8.3 filenames + frame counts for a bank directory.

One capability remains unserved: `CapWavCues`. The parser exists (`memory/wav.h`'s `find_cue_points`),
but nothing calls it after a load, so an engine declaring that bit would get no markers. No engine in
the set declares it today.

### 5.1 The DTCMRAM trap (read this before debugging any SD problem)

**The SDMMC DMA cannot access DTCMRAM.** libDaisy says so in `src/sys/fatfs.h`, and under the
Electrosmith bootloader it is a live hazard rather than a footnote, because libDaisy's stock BOOT_SRAM
linker script puts **both `.bss` and the stack** in DTCMRAM. Anything FatFs hands to the DMA must
therefore live in AXI SRAM:

- **The `FatFSInterface` object** (it holds the FATFS window buffer). As a static it lands in `.bss`,
  so `.bss` has to be in AXI SRAM - which is why `linker/sram_bss_in_axi.lds` is the default LDSCRIPT
  for every BOOT_SRAM build here, not a per-engine exception.
- **Every `FIL` / `FatFile`**, because a `FIL` carries a 512-byte sector buffer the DMA writes into
  directly. A local one is on the stack and therefore broken. Declare them `static` (all the call
  sites here are main-loop only and non-reentrant) or otherwise place them in AXI SRAM.

`DIR` is fine: `f_readdir` buffers through the FATFS window rather than its own.

What makes this expensive is that it does not look like one bug. The same root cause appeared four
times during bring-up, each wearing a different mask:

| Symptom | Actually |
|---|---|
| `f_mount` returns `FR_DISK_ERR` (not `FR_NOT_READY`) | the FATFS window was in DTCMRAM |
| every `.wav` reports a bad/unreadable header | the probing `FIL` was on the stack |
| `read_text` / `write_text` silently return nothing | same, in `StreamDeck` |
| an engine ignores play and stays silent | `scan_bank` skipped every file whose header it could not read, so the bank was empty |

The tell is `FR_DISK_ERR` **with the peripheral reporting OK**: it means the card was detected and
initialised and only the data transfer failed. A truly absent card gives `FR_NOT_READY`. If you see
disk errors at every bus width and speed, stop suspecting the card and check where the buffer lives.

### 5.2 Card content

`scripts/make_sd_content.py` generates test audio for every streaming engine, in each one's layout and
format, and `make sd-card` copies it to a card. When porting another engine that reads from a card, add
a builder there and a directory to `provision_sd.sh`'s `AUDIO_ENGINES`.

The format trap is worth repeating, because it costs an evening the first time: the service reads WAV
through **two paths with incompatible formats**. `start_play` (tape, shuttle, softcut) requires mono
32-bit float at exactly 48 kHz; `start_play_wav` (radio, bard, pstretch) requires mono 16-bit PCM at
any rate. A file in the wrong one is refused at play time with no other symptom. `make sd-verify`
checks a tree or a card against both, plus the 8.3-name and 32 KB-minimum rules the bank scanner
enforces.

## 6. Per-engine notes

Engines already ported are covered in `app/README.md`. Two notes from porting the SD tier that
generalize:

- **A large static working set is a linker-script problem, not a diet.** `pstretch` (~291 KB of `.bss`)
  and `bard` overflow the stock script's 128 KB DTCMRAM `.bss` region. `linker/sram_bss_in_axi.lds`
  moves `.bss` into the 480 KB AXI SRAM alongside `.text`, where they are allocated sequentially -
  no hand-tuned code/data boundary of the kind upstream's `alt_sram*.lds` need. Select it with
  `LDSCRIPT` in the engine's block.
- **`SPK_TERMINAL=1` has one link-time cost.** Engines that answer terminal queries (`radio`, `tape`)
  format replies through `TextSink`, whose out-of-line half is `src/terminal/text_sink.cpp`. It is in
  `CPP_SOURCES` for every build; nothing else from the terminal is needed.

For the rest:

- **`edrums`** - tier 1b. Four-voice Euclidean drums; needs the transport (provided) and, optionally,
  `ctx.qspi` to persist kit presets to flash - leave it null to disable saving. Copy the
  `dsp/cpattern.h` / `biquad.h` / `lutsinosc.h` / `divider.h` helpers.
- **`mosc`** - tier 2, top recommendation alongside `reso`. Full Plaits 24-model macro-oscillator. Copy
  `src/engine/mosc/thirdparty` + `common/thirdparty/stmlib`. **QSPI build** (the 24-model voice is
  ~292 KB of .text, over the SRAM budget) - `APP_TYPE = BOOT_QSPI`, and it needs the `SCB->VTOR` inject
  the csound/chuck harnesses carry. Build `-Os`. Sets `-DPLAITS_USER_DATA_STUB`.
- **`reso`** - tier 2. Mutable Rings resonator/plucked-string. Copy `src/engine/reso/thirdparty` +
  `stmlib`. SRAM at `-Os` (Rings' ~30 KB of code+tables overflow at `-O2`). Already implements
  `handle_midi_note` - plays over MIDI immediately.
- **`granular`** - tier 2, the original Spotykach engine: 42 files, all under `src/engine/granular/`.
  The most `IEngine` surface of anything in the set (recording, sequencer, tape storage, launch quant),
  so it is the one engine where the harness's four knobs will feel genuinely narrow.
- **`graincloud`** - tier 2, and NOT a standalone tree: it is the granular engine compiled with
  `-DSPK_GRAIN_GF=1`, so it needs `src/engine/granular/` too, plus its own vendored GrainflowLib and a
  specific include order (`-Isrc/engine/graincloud` first). Build `-Os`.

## 7. Build, flash, verify

```
cd app
make ENGINE=<eng> BOARD=patch                                   # -> build-<eng>-patch/harness_<eng>.bin
make program-boot                                               # once per board (libDaisy bootloader)
while ! make ENGINE=<eng> program-dfu; do sleep 0.2; done        # catch the DFU window, then tap RESET
```

Bring-up tips lifted from the chuck harness: use `board.SetUserLed()` to prove how far `init()` got (the
onboard LED is visible even on a cased unit), and toggle it in the audio callback as an "the ISR is
alive" heartbeat. On the Patch the OLED is a better probe than either. If a QSPI app does not boot, the
`SCB->VTOR` inject is the first thing to check.

Validate on hardware: audio out, sweep the knobs, and (for MIDI engines) play NoteOn from a controller.
The Pod is the only board this repo has validated on a device; the Patch and patch.init() drivers
compile but have never been run.

## 8. Port checklist

- [ ] `scripts/port_engine.sh <eng>` - and copy anything it reports `MISSING` (`src/dsp/*` helpers, a
      shared `stmlib`).
- [ ] Add the `#elif` arm to `src/engine/engine_select.h`.
- [ ] Add the `else ifeq` block to `app/Makefile`, and the name to `ALL_ENGINES`.
- [ ] `make ENGINE=<eng> BOARD=patch`, and check `--print-memory-usage` for the SRAM headroom;
      `OPT = -Os` or `APP_TYPE = BOOT_QSPI` if it does not fit.
- [ ] Confirm the engine narrows `live_params()` upstream, so the OLED pages only real params.
- [ ] Add `examples/<eng>/` + `make sd-card` wiring if it loads SD content.
- [ ] Flash and verify audio + knobs + MIDI on hardware.
- [ ] Note the new engine in `app/README.md`, `README.md` and `CHANGELOG.md`.
