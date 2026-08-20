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

### 2.4 The control surface an engine actually gets

A port compiles the moment the build glue is in place, and that is where it is easy to stop. It is also
where the expensive bugs live, because the harness reaches an engine through three separate channels
with three different sources of truth, and a method that no channel calls is indistinguishable, on the
bench, from an engine that does not work.

| Channel | What it drives | Where the harness learns what exists |
|---|---|---|
| Continuous params | the four knobs, paged, with value pickup | `live_params()` + `param_label()` |
| Pads and switches | the **action screen** (encoder click) | the pad mask (below) + `live_configs()` + `CapDualDeck` |
| Everything else | gates in, MIDI in, CV in/out, transport | fixed wiring in `harness.cpp` |

The second row is the one that bites. `IEngine`'s pads (`on_play_pad`, `on_record_pad`, the `on_seq_*`
group, `set_fx`) all have no-op default bodies, and `capabilities()` has no bit for most of them, so an
engine cannot be *asked* what it implements in the way it can be asked which params it consumes. Before
the action screen existed the harness simply did not call most of them, and the results all looked like
DSP faults:

| Symptom on the bench | Actually |
|---|---|
| `granular` and `graincloud` are silent from boot, forever | `on_record_pad` had no caller on any board, and recording is their only way to get audio into a deck |
| `tape`, `shuttle`, `softcut` play but will not record | same cause |
| `reverb` has one algorithm, not three | `ConfigId::Mode` was unreachable on any `CapDualDeck` engine - the click had to be the deck switch |
| `pstretch` ignores the clips on the card | its `Mode` switch is the source selector, same cause |
| `bard` does nothing at all | it boots paused by design, and the play pad had no route |

So the check that matters when porting is not "does it build" but **can every method this engine
implements be reached from the panel**. The action screen answers most of it automatically - see 4.3.

**How the harness knows which pads exist.** `app/engine_pads.h` measures it instead of asking. For an
inherited member, `&Derived::f` has type `R (Base::*)(...)`; only a class that declares `f` makes it
`R (Derived::*)(...)`. Comparing those two types folds to a constant at build time, where the concrete
`ActiveEngine` is known, and yields a bitmask the action screen builds its rows from. It costs nothing
at runtime, assumes nothing about vtable layout, and - the point - has nothing to keep in sync. The
declarative alternative had already drifted: `tape` and `shuttle` implemented `on_record_pad` for their
whole life here while declaring no `CapRecording`. A `static_assert` that the base class's own mask is
zero catches the one way the trait can go quietly wrong, which is a signature in that header ceasing to
match `IEngine`'s.

What the trait cannot tell you is whether an implementation *does* anything. An engine that declares a
pad and returns immediately still gets a row. That error is a spare line on a screen; the error it
replaced was a hidden feature.

**Board differences are not cosmetic here.** The action screen is a screen affordance, so it exists on
the Daisy Patch only. The Pod keeps a direct click (deck switch, else cycle `Mode`) and puts the pads on
its two buttons - 0 play, 1 record. patch.init() has no encoder at all, so its two buttons and two
gate inputs are the whole of its discrete control. An engine whose only route to a feature is a pad row is therefore reachable on
the Patch and not on patch.init(), which is worth knowing before concluding a board is broken.

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

**Every engine in the upstream set is now ported.** What remains is not porting but polish. The
`granular` / `graincloud` surface is the widest in the set - recording, a step sequencer, tape storage,
launch quantization, two FX layers - and for a while none of it was reachable from four knobs and an
encoder, which made both engines silent on every board. The action screen (2.4) now reaches the pads
and the switches; what is still missing there is anything momentary or velocity-like, since a menu row
is a discrete event by construction.

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

Nothing to do. `app/harness.cpp` hosts any engine, the knob pages come from the engine's own
`live_params()`, and the pads and switches it implements appear in the action screen on their own - the
pad rows are derived from the engine's type (2.4) and the switch rows from `live_configs()`, so an
engine that implements a sequencer gets sequencer rows and one that does not gets none.

Three things to check anyway, in the order they cost time:

- **Can everything the engine implements be reached?** Build it, open the action screen, and read the
  list against the engine's header. A pad in the list that the engine does not implement is impossible
  by construction; a pad the engine implements that is NOT in the list means the trait's signature no
  longer matches the contract, and the `static_assert` in `engine_pads.h` should have said so.
- **Does the engine narrow `live_params()`?** If not it inherits the all-live mask and the UI pages all
  24 slots. Adding the mask upstream (inside its `SPK_TERMINAL` block) is the right fix, and it makes
  `describe` honest there too. The same applies to `live_configs()`, which is what the switch rows are
  built from - an engine that narrows neither gets four pages of dead knobs and six dead switch rows.
- **Does it need a control shaped differently?** The panel map is in `app/README.md`. A menu row is a
  discrete event: it can fire a pad or step a switch, but it cannot be held, and it cannot be fast. An
  engine whose gesture is momentary (a pad held for the duration of an effect) or timing-critical (a
  launch) wants a gate input or MIDI, not a row.

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

If the engine loads content from SD there are two kinds of it. **Committed text slots** (the csound and
chuck patch banks) live in `examples/<eng>/`; mirror those two directories and add the name to
`provision_sd.sh`. **Generated audio** does not belong in git: add a builder to
`scripts/make_sd_content.py` in the engine's own layout and format, and the engine's name to
`provision_sd.sh`'s `AUDIO_ENGINES`. `make sd-content` then synthesizes it, `make sd-card` copies it,
and `make sd-verify` checks a tree or a real card against the format rules. Which format is not a free choice; see 5.2.

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

Two corollaries worth carrying, because they decide whether new code is exposed:

- **Small reads are safe wherever they live; bulk reads are not.** FatFs services a request shorter
  than a sector through the `FIL`'s own buffer, so a 12-byte WAV-header read into a stack local works
  even though the stack is in DTCMRAM. A full-sector read is handed to the DMA as the caller's pointer,
  so the destination itself must be reachable - which is why the streaming rings and scratch are in
  SDRAM and why nothing in the SD path reads bulk audio into a local.
- **The heap is not automatically safe either.** `linker/sram_bss_in_axi.lds` places `.heap` in RAM_D2.
  No engine in `app/` allocates, so this is inert today, but an engine that `malloc`s a buffer and
  hands it to FatFs would reproduce the whole bug class. The `pod/` csound and chuck harnesses do
  exactly that (a 64 KB patch-text buffer) and are safe only because their QSPI script puts the heap in
  AXI SRAM. If you add an allocating engine to a BOOT_SRAM build, check where its buffer lands before
  it reaches the card.

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
- **Reachability is a property of the pair, not the engine.** Several engines in this set were fully
  correct and completely unusable at the same time, because the platform never called the method they
  are built around (2.4). When an engine "does nothing", establish which of three things is true before
  touching DSP: the gesture never arrived, the engine received it and chose to stay silent, or it is
  producing sound you cannot hear. The header's two-character status field (`P4:2` - streaming, and the
  play-press count) exists to separate exactly those, and `make diag` separates them at board level.
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

Validate on hardware: audio out, sweep the knobs, open the action screen and fire the rows the engine
declares, and (for MIDI engines) play NoteOn from a controller. Board status as of this writing: the
Pod has been validated on a device, the Daisy Patch has had a bring-up pass (which is where the DTCMRAM
trap in 5.1 and the unreachable pad surface in 2.4 were both found, on hardware, by inference from
symptoms), and patch.init() has never been run at all. The action screen itself is compile-verified
across all 20 engines x 3 boards and has not yet been used on a device.

## 8. Port checklist

- [ ] `scripts/port_engine.sh <eng>` - and copy anything it reports `MISSING` (`src/dsp/*` helpers, a
      shared `stmlib`).
- [ ] Add the `#elif` arm to `src/engine/engine_select.h`.
- [ ] Add the `else ifeq` block to `app/Makefile`, and the name to `ALL_ENGINES`.
- [ ] `make ENGINE=<eng> BOARD=patch`, and check `--print-memory-usage` for the SRAM headroom;
      `OPT = -Os` or `APP_TYPE = BOOT_QSPI` if it does not fit.
- [ ] Confirm the engine narrows `live_params()` and `live_configs()` upstream, so the OLED pages only
      real params and the action screen lists only real switches.
- [ ] Open the action screen and check its rows against the engine's header: every pad the engine
      implements should be there (2.4), and every switch row should move something.
- [ ] Add `examples/<eng>/` + `make sd-card` wiring if it loads SD content.
- [ ] Flash and verify audio + knobs + MIDI on hardware - including the engine's own pad, which for
      several engines is the difference between silence and the whole feature.
- [ ] Note the new engine in `app/README.md`, `README.md` and `CHANGELOG.md`.
