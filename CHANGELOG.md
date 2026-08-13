# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is
pre-release and does not yet follow semantic versioning, so everything since the initial commit lives
under **Unreleased**.

## [Unreleased]

### Added

- **Generic engine host (`app/`)** — one harness that runs any ported sk-engines engine on any of the
  three boards, with the engine chosen at build time (`make ENGINE=delay BOARD=patch`). Replaces the
  one-harness-per-engine pattern: `harness.cpp` builds the `EngineContext`, drives `process()` from the
  audio callback, and maps the panel onto `IEngine` without knowing which engine it is hosting. Each
  `(engine, board)` pair builds into its own `build-<engine>-<board>/`, so switching engines no longer
  needs `rm -rf build`. See [`app/README.md`](app/README.md).
- **Paged parameter UI driven by engine self-description** (`app/param_ui.h`). The pages are not
  written per engine: each engine declares which `ParamId`s it consumes (`live_params()`) and what it
  calls them (`param_label()`), and the UI builds pages of four from those answers and prints the
  engine's own words on the OLED. Knobs use value pickup — a knob writes only once it crosses the
  engine's current value — with the uncaught gap shown on screen rather than left to guess.
- **Hardware diagnostic firmware** (`app/diag.cpp`, `make diag BOARD=patch`). Links no engine and none
  of the engine contract: it drives the board and FatFs directly, because the questions worth asking
  when an engine is silent — did the ADC read, did the card mount, is the codec running — are ones an
  engine cannot answer. Seven OLED pages: live analog bars, encoder/buttons/gates with edge counters,
  MIDI activity with the last message decoded to a note name, SD mount plus a per-file verdict against
  the on-card format rules, audio I/O peak meters with a test tone, CV/gate output exercise, and system
  info (sample rate, block size, SDRAM read/write check, uptime). The audio path is a passthrough, so
  hearing your input confirms the codec and SAI.
- **Twenty engines ported from sk-engines** — the whole set bar none: `passthrough`, `delay`, `qdelay`,
  `glitch`, `reverb`, `chorus`, `filter`, `voice`, `gigaverb`; the SD-streaming set `radio`, `tape`,
  `shuttle`, `pstretch`, `softcut`, `bard`; the Mutable Instruments voices **`reso` (Rings)** and
  **`mosc` (Plaits)**; the original Spotykach **`granular`** looper and its GrainflowLib variant
  **`graincloud`**; and the Euclidean drum machine **`edrums`**. All 63 engine x board combinations
  compile (20 engines plus the diagnostic, three boards); none has been run on hardware.
- `graincloud` is not a second copy of the granular tree: it is the same sources compiled with
  `-DSPK_GRAIN_GF=1`, which swaps the per-sample voice array for a per-block GrainflowLib cloud. Its
  include order puts graincloud first so granular's guarded `#include "gf_cloud.h"` resolves, and it
  links with the `.bss`-in-AXI-SRAM script (over DTCMRAM by ~36 KB).
- **`EngineContext::qspi` is now populated** (`Board::Qspi()` on all three boards), closing the last
  null field in the context. `edrums` uses it to persist kit presets to QSPI flash at a 64 KB offset,
  clear of the calibration settings and the app image.
- **`reso` and `mosc` were the first playable engines here** — implementing
  `handle_midi_note`, `on_gate_trigger` and `cv_voct`, so a keyboard, a trigger and a V/Oct patch all
  do something. Both vendor their DSP under their own `thirdparty/`, sharing
  `src/engine/common/thirdparty/stmlib`. `reso` builds `-Os` into SRAM; `mosc` is the first QSPI-execute
  app in `app/` (the 24-model voice is ~266 KB of code), which added a `SCB->VTOR` prologue to the
  harness, gated on a new `HARNESS_BOOT_QSPI` define since libDaisy's `BOOT_APP` covers BOOT_SRAM too.
- **`.cc` compilation support in `app/Makefile`.** Mutable Instruments ships `.cc`, which stock
  libDaisy's Makefile does not compile (sk-engines does not hit this because it builds against a
  libDaisy fork that handles it). Adding the rule on this side keeps `libs/` a pristine upstream
  checkout. Note it must use `CPPFLAGS`, not `CXXFLAGS` — libDaisy does not define the latter, and a
  rule using it silently compiles with no includes, defines or MCU flags.
- **SD audio streaming** (`src/hw/stream_deck.{h,cpp}`, `src/hw/fat_file.{h,cpp}`, and the ring/stream
  primitives under `src/memory/`), ported from sk-engines rather than reimplemented — the threading
  contract is the delicate part of streaming and upstream's version already runs on hardware. The
  audio ISR touches only lock-free SDRAM rings; the harness pumps FatFs from the main loop, giving each
  deck a 1 MB power-of-two ring (~5.5 s of mono read-ahead at 48 kHz) plus 32 KB of shared staging.
  This is what unblocked the six streaming engines above. `src/engine/istreamdeck.h` was replaced with
  upstream's full version, which adds `seek_play`, `write_text` and the `bank_sort` helper.
- **SD card content for the streaming engines**, generated rather than vendored
  (`scripts/make_sd_content.py`, `make sd-content`): synthesized tones, sweeps, noise and blips —
  deliberately distinguishable by ear — written into each engine's expected layout, in the exact format
  its reader accepts. `scripts/provision_sd.sh` and `make sd-card` now cover all eight card-reading
  engines and generate the audio automatically if it is missing. The tree lands in `examples/sd/`
  (~20 MB, gitignored, reproduced on demand like `libs/` and `thirdparty/`).
- **`make sd-verify`** — checks a content tree or a mounted card against the rules the on-device readers
  apply. This exists because the service reads WAV through two paths that accept **incompatible**
  formats: `tape`/`shuttle`/`softcut` require mono 32-bit float at exactly 48 kHz, while
  `radio`/`pstretch`/`bard` require mono 16-bit PCM at any rate. A file in the wrong one is refused at
  play time with no other symptom. The check also catches non-8.3 names and files under the scanner's
  32 KB floor, both of which make a file silently invisible to the engine.
- **CV inputs routed to `IEngine`'s CV surface** (`cv_voct`, `cv_size_pos`, `cv_mix`, `cv_crossfade`),
  which `radio`, `pstretch`, `bard`, `reso`, `mosc`, `granular` and `graincloud` implement and nothing
  previously fed. Defaults differ by board
  because the hardware does: patch.init() routes all four dedicated CV jacks for free, while on the
  Daisy Patch knob and jack are summed ahead of the ADC, so a CV input costs a parameter knob and the
  default is off — opt in per build with `make ... CV_INPUTS=2`. `cv_voct` is sent in semitones, the
  unit upstream's calibrated corrector produces. **It is not calibrated here**: the scaling assumes a
  linear +/-5 V jack, so pitch tracking is approximate until measured on a real board.
- **`make dist` now covers both harness families** (`scripts/build_release.py`): the 20 engine-host
  engines plus the diagnostic, alongside the two `pod/` audio-language harnesses. The pod entries are
  skipped with a notice when their cross-built libraries are absent, so a fresh checkout can cut a
  release without fetching Csound or ChucK first. `MANIFEST.txt` gained a per-artifact license column
  and the release notes a Licensing section, because `qdelay` and `glitch` binaries are GPLv3 combined
  works and a downloader should not have to discover that.
- `examples/README.md` — the card layouts for every engine, and the two-WAV-format trap written out.
- `src/sd_card.h` — the SDMMC + FatFs mount, split out of `SdStreamDeck` so both IStreamDeck
  implementations share one mount rather than each carrying a copy.
- `linker/sram_bss_in_axi.lds` — libDaisy's stock SRAM script with `.bss` moved from the 128 KB
  DTCMRAM into the 480 KB AXI SRAM alongside `.text`. `pstretch` overflows the stock placement by
  ~172 KB (its FFT working set is ~291 KB of static data) and `bard` by ~10 KB; both select this
  variant. Because the two sections are allocated sequentially in one region, it needs none of the
  hand-tuned code/data boundary that upstream's `alt_sram*.lds` carry.
- `src/terminal/text_sink.cpp` — the out-of-line half of `terminal_io.h`'s `TextSink`, needed because
  this build defines `SPK_TERMINAL=1` and `radio`/`tape` format query replies through it.
- **Harness clock glue** (`app/harness_clock.h`): a `daisy::System`-backed `ITimeSource` and a minimal
  free-running `ITransport` — a 48-PPQN clock at a settable BPM, steerable by quarter-note pulses at a
  gate input, with the tick fan-out the interface specifies. This plus the 48 MB SDRAM arena in
  `harness.cpp` closes the two `EngineContext` gaps that previously blocked every buffer-using and
  tempo-synced engine.
- `scripts/port_engine.sh` — copies an engine's tree (including any vendored `thirdparty/`) from an
  sk-engines checkout, rewriting `spotykach` -> `daisyapps`, and reports cross-tree includes this repo
  is missing.
- `scripts/fetch_libs.sh` — clones and builds `libs/libDaisy` + `libs/DaisySP`, including libDaisy's own
  submodules (without which its Makefile stops at the first object file).
- `src/board/midi_status.h` — the `daisy::MidiEvent` -> raw 3-byte MIDI conversion, shared by the board
  drivers instead of copied into each one.

- Daisy Pod harnesses for two audio-language engines, ported from the sk-engines firmware:
  `pod/harness_csound.cpp` (`CsoundEngine`) and `pod/harness_chuck.cpp` (`ChuckEngine`), each a thin
  QSPI BOOT app that drives the real engine behind the `IEngine` contract.
- Engine sources and contract headers under `src/` (`CsoundEngine`, `ChuckEngine`, and the shared
  `engine/*.h` interfaces), plus the cross-compiled Csound/ChucK dependency trees under `thirdparty/`.
- Control/UI board abstraction (`src/board/`): a board-agnostic `Controls` snapshot (`controls.h`),
  compile-time target selection (`board.h`), and drivers for the three targets — Daisy Pod
  (`pod_board.h`), patch.init() (`patch_init_board.h`), and Daisy Patch (`patch_board.h`). Each board
  exposes a uniform surface (`Init` / `StartAudio` / `SampleRate` / ISR-safe `Analog` / `Poll` /
  `SetIndicator` / `SetUserLed`) with no virtual dispatch.
- `BOARD=` make variable (`pod` | `patch_init` | `patch`) that selects the board driver via a
  `-DTARGET_*` define; the same harness compiles for any target.
- On-demand dependency build scripts `scripts/fetch_csound.sh` and `scripts/fetch_chuck.sh`, which
  fetch and cross-compile `libcsound.a` / `libchuck.a`.
- Documentation: root `README.md` (layout, Targets table, prerequisites, build) and per-app
  `pod/README.md` (behavior, bootloader/heap notes, flashing).
- SD patch bank for both Pod harnesses: a minimal `pod/sd_stream_deck.h` (`SdStreamDeck`) implementing
  the two `IStreamDeck` methods the patch bank uses (`exists` / `read_text`) over a FatFs-mounted card,
  injected as `ctx.stream`. The Csound harness loads `csound/0.csd` .. `csound/7.csd` and the ChucK
  harness `chuck/0.ck` .. `chuck/7.ck` from the card (built-in orchestra/program as fallback / when no
  card), boot-auto-loads the first slot, and exposes an encoder-driven selector (hold encoder = Alt,
  turn knob 1 to scroll, release to recompile live). Both `Makefile`s set `USE_FATFS = 1` so the FatFs
  sources link. ChucK pushes its knobs from the audio ISR, so a `volatile` flag releases knob 1 to the
  selector while browsing; FatFs (`_USE_LFN=1`, static buffer) makes no `malloc` calls and stays out of
  ChucK's `--wrap` SDRAM pool.
- MIDI NoteOn input for both Pod harnesses. The board abstraction gains `StartMidi()` and a templated
  `PollMidi(sink)` (real on the Pod's UART MIDI, no-op on the patch targets); the harnesses forward each
  NoteOn to `engine.handle_midi_note(channel, note)` (channel 1 -> deck A, channel 2 -> deck B via
  `Config::dynamic()`), and the engines deliver notes from the audio ISR. The Csound engine already
  played its `instr MidiNote`. ChucK's own `MidiIn` is compiled out of this bare-metal build
  (`__DISABLE_MIDI__`), so the new `ChuckEngine::handle_midi_note` bridges via globals: per block a deck's
  NoteOns are handed to the VM as an int array (`notesA`/`notesB`) + count (`noteCountA`/`noteCountB`) and
  one broadcast Event (`noteOnA`/`noteOnB`), so a patch can spork a voice per note and chords play
  **polyphonically**. The generic note ring + note->Hz map moved to a shared `src/engine/midi_note.h`
  (used by both engines). NoteOn-only (finite, self-terminating voices); a `examples/chuck/midi.ck`
  reference patch shows the ChucK convention. `docs/dev/chuck-midi-in.md` captures a design note on
  re-introducing real ChucK `MidiIn` (the bridge was a deliberate choice over it).
- Real ChucK `MidiIn` on the bare-metal build (prototype; build-verified, on-hardware wake test
  pending). `libchuck.a` is now compiled with the `MidiIn`/`MidiOut` device classes enabled
  (`__DISABLE_MIDI__` dropped from `scripts/fetch_chuck.sh` and `pod/Makefile.chuck`), but with
  `midiio_rtmidi`'s RtMidi backend stripped - there is no OS MIDI API or callback thread on bare metal,
  and `rtmidi.cpp` (166 KB) stays out. A new `MidiInManager::inject()` feeds bytes straight into ChucK's
  per-VM MIDI buffer + event-wake path, so a patch can use the desktop-portable
  `MidiIn min; min.recv(msg);` idiom (`examples/chuck/midi_in.ck`). The **full channel-voice stream** is
  forwarded (real velocity, NoteOff, CC, pitch-bend, aftertouch, program change) plus system realtime
  (clock/start/continue/stop): the board abstraction's `PollMidi` now surfaces raw 3-byte messages, a new
  `IEngine::handle_midi_message(status, d1, d2)` virtual (no-op for engines that only use
  `handle_midi_note`) is driven from the Pod harness, and `ChuckEngine` enqueues raw bytes on a lock-free
  ring (`chuck_midi_in.h`) that `process()` drains and injects into device 0 right before `ck->run()`.
  NoteOns are still also fed to the global bridge (the `notesA`/`noteOnA` convention) so both delivery
  styles work at once; the Csound harness pulls NoteOns out of the same raw stream. The vendored-source
  edits live in `scripts/patches/midi_daisy.patch`, applied idempotently by `scripts/fetch_chuck.sh`
  (because `thirdparty/chuck` is gitignored and regenerated). Builds for all three board targets (and the
  Csound harness) link with matching ABI. See `docs/dev/chuck-midi-in.md` for status and the retired
  wake-path risk, and `docs/dev/chuck-midi-in-porting.md` for the replication guide (the same change was
  ported to the `sk-engines`/spotykach sibling). On-hardware wake test still pending; SysEx / system-common
  aren't representable in a 3-byte `MidiMsg` and are not forwarded.
- Example patch banks under `examples/` (`examples/csound/0.csd` .. `6.csd`,
  `examples/chuck/0.ck` .. `7.ck`), each with a README adapted to the Pod harness (encoder selector,
  no MIDI, only PITCH + MIX driven). `make sd-card SD=/Volumes/<card>` (a thin
  `scripts/provision_sd.sh`) copies the numbered slots into card-root `csound/` / `chuck/` folders for
  the loader. `.gitignore` now also ignores `.DS_Store`.
- Release packaging: a root `Makefile` with `make dist` (and `make gh-release`) driving
  `scripts/build_release.py`, which clean-builds the full engine x board matrix in one shot and
  collects version-stamped `daisy-<engine>-<board>-<version>.bin` artifacts under `dist/<version>/`
  with `SHA256SUMS`, `MANIFEST.txt`, and `RELEASE_NOTES.md` (CHANGELOG section + flashing guide).
  `RELEASE_ENGINES` / `RELEASE_BOARDS` restrict the matrix to a subset (e.g. a single board or pair),
  `VERSION` sets the version, and `WITH_HEX=1` also emits `.hex` artifacts.

### Fixed

- **The SD card never worked on a BOOT_SRAM build, in four places at once.** The SDMMC DMA cannot
  access DTCMRAM, and libDaisy's stock BOOT_SRAM linker script puts both `.bss` and the stack there —
  so every FatFs buffer handed to the DMA was unreachable. Found on hardware during Daisy Patch
  bring-up, where it presented as four unrelated-looking faults: the card would not mount
  (`FR_DISK_ERR` at every bus width), every `.wav` reported a bad header, `read_text`/`write_text`
  returned nothing, and `scan_bank` silently skipped every file — which made engines look like they
  were ignoring the play control. Fixes: `linker/sram_bss_in_axi.lds` is now the default LDSCRIPT for
  every BOOT_SRAM build (it was a per-engine exception for memory overflow), and the `FIL`/`FatFile`
  objects in `sd_stream_deck.h`, `StreamDeck::read_text` / `write_text` / `scan_bank` and the
  diagnostic are `static` rather than stack-allocated. Documented in the porting guide, section 5.1.
- **The play/record surface was unreachable on every board.** `IEngine::on_play_pad` was never called
  and `controls.button[]` never read, so engines that start idle by design — bard boots paused, being
  a resume-where-you-left-off player — looked broken rather than stopped. The Daisy Patch has no
  buttons at all, so it now gets a long-press-the-encoder gesture for play/pause; boards with buttons
  map button 0 to play and button 1 to play-with-reverse.
- `alignas(32)` after `DSY_SDRAM_BSS` on the streaming rings was silently discarded by GCC ("appertains
  to a type-specifier"). The section attribute was applied correctly — the rings were always in SDRAM —
  but the alignment request was not. Moved ahead of the declaration.

### Changed

- **The engine contract is now a full copy of upstream's, not a trimmed subset.** `iengine.h`,
  `engine_params.h`, `mode.h` and `terminal_io.h` are sk-engines' files with only the namespace
  substituted, restoring the display/LED region (`render`, `render_ring`, the `*_leds` queries, `mix`,
  `route`), the `CapOwnDisplay` / `CapWavCues` / `CapTerminal` bits, and the `SPK_TERMINAL` block. This
  reverses what `docs/dev/porting-sk-engines.md` used to prescribe (strip the display overrides per
  engine): the shared Faust and gen~ wrappers that every Faust engine specializes depend on
  `display_model.h` and `indicators.h` directly, so stripping would mean rewriting them on every
  upstream sync — and the Patch has a screen, which is the condition that guide already named for
  extending instead. Consequence: an engine now ports with **no source edits beyond the namespace**.
- `app/` builds with `-DSPK_TERMINAL=1`. There is no terminal here; the flag is on because engines
  declare their param-liveness masks and knob labels inside that block, and those are exactly what the
  paged OLED UI needs. `abi_tag.h` came along with the contract, so a `SPK_TERMINAL` mismatch between
  the `app/` and `pod/` builds is a link error rather than a runtime fault.
- The contract now depends on libDaisy headers transitively (`iengine.h` -> `display_model.h` ->
  `led.ring.h` -> `color.h` -> `common.h` -> `<daisy.h>`). Nothing is blocked today — every build here
  is a firmware build — but it would need addressing before a host/test build exists.
- **Daisy Patch driver rewritten** (`src/board/patch_board.h`), replacing the scaffold that had never
  been compiled: MIDI input on the seed UART, the OLED behind a small text/rect facade, CV outputs via
  the seed DAC, and a gate output. The `Board` contract grew `SetCvOut` / `SetGateOut` / `Screen*` and
  the `kCvOutCount` / `kHasScreen` / `kScreenWidth` / `kScreenHeight` constants; every board implements
  the whole surface, with no-ops where a peripheral is absent, so harness UI code needs no `#ifdef`.
- Moved `sd_stream_deck.h` from `pod/` to `src/` — it is a service both harness families use.
- Renamed the C++ namespace `spotykach` to `daisyapps` across all sources, and reworded the
  sk-engines/spotykach references in comments and docs for this standalone repo.
- Pointed the harness Makefiles at the vendored `libs/libDaisy` and `libs/DaisySP`.
- ChucK harness re-enables exceptions/RTTI through `CPP_STANDARD` (the stock upstream libDaisy in
  `libs/` has no `CPP_USER_FLAGS` hook), keeping `libs/` a pristine submodule.
- `thirdparty/` (the Csound/ChucK source trees and static libraries) is gitignored and reproduced on
  demand by the fetch scripts instead of being vendored.
- Trimmed `src/` to only the engine sources and headers the two harnesses actually compile.
- Dropped the `.gitignore` rule for the root `Makefile` (a vestigial CMake-era ignore) so the new
  release `Makefile` is tracked; `pod/Makefile` was already tracked. Added a `dist/` ignore for the
  release artifacts.

### Removed

- ~~The spotykach front-panel LED/ring/display layer: `led.ring.{h,cpp}`, `display_model.h`,
  `engine_leds.h`, `color.{h,cpp}`; the `render()` / `render_ring()` / LED-query virtuals from
  `IEngine`; the `render()` overrides and their output meters from both engines; and the now-unused
  `CapOwnDisplay` capability.~~ **Reinstated** when the engine host arrived — see Changed above. The
  csound/chuck engines' own `render()` overrides remain stripped (they were edited out at port time and
  the base-class no-op defaults cover them); everything else is back at upstream parity.
- `bootloader-spotykach-v2.bin` (project-specific bootloader). `program-boot` now flashes libDaisy's
  bundled stock bootloader for the ChucK harness; the Csound harness uses the Daisy v5.4 image from
  the fetched Csound port.

### Notes

- Validated on hardware: Daisy Pod only, and only the csound/chuck harnesses. The Daisy Patch and
  patch.init() drivers, the `app/` engine host, and all nine ported engines compile for every board but
  have never been run on a device.
- The Daisy Patch OLED and the CV/gate outputs on both Eurorack targets are no longer deferred — they
  are implemented in the board drivers and driven by `app/harness.cpp`. `patch_init_board.h`'s LED
  writes were also fixed: they used the removed `dsy_gpio_write` C API, which had gone unnoticed
  because that target had never been compiled.
- Every field of `EngineContext` is now populated. The one capability still unserved is `CapWavCues`:
  the parser exists in `memory/wav.h` but nothing calls it after a load, and no engine in the set
  declares the bit.
- One ported engine needed a source edit beyond the namespace — `edrums`, whose QSPI preset store was
  written against the bleeptools libDaisy fork's three-argument `PersistentStorage`. Rewritten against
  stock libDaisy's one-argument version, with the reasoning in a comment at the site. Every other
  engine is a verbatim copy.
