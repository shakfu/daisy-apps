# daisy-apps

A small collection of apps for the [Electrosmith Daisy](https://www.electro-smith.com/daisy) platform (Pod / Patch / patch.init()), holding DSP engines ported out of the sk-engines firmware and run against that firmware's `IEngine` contract on bare Daisy hardware.

Two families live here:

- **`app/` — the engine host.** One harness, any ported engine, any of the three boards, chosen at build time: `make ENGINE=reso BOARD=patch`. Twenty engines are ported: effects (`passthrough`, `delay`, `qdelay`, `glitch`, `reverb`, `chorus`, `filter`, `voice`, `gigaverb`), an SD-streaming set that plays and records audio off the card (`radio`, `tape`, `shuttle`, `pstretch`, `softcut`, `bard`), the two Mutable Instruments voices **`reso` (Rings)** and **`mosc` (Plaits)** — the ones you can play from a keyboard, a trigger or a V/Oct patch — the original Spotykach **`granular`** looper and its GrainflowLib variant **`graincloud`**, and the Euclidean drum machine **`edrums`**. The control surface is not written per engine: each engine declares which parameters it consumes and what it calls them, and the harness builds the OLED's parameter pages from that. See [`app/README.md`](app/README.md).

- **`app/diag.cpp` — the hardware diagnostic.** `make diag BOARD=patch` builds a firmware that links no engine and puts every subsystem on the OLED: analog inputs, encoder, gates, MIDI, SD mount and per-file format checks, audio levels, CV/gate outputs, SDRAM. Flash this first when something does not work.

- **`pod/` — the two audio-language harnesses.** A **Csound** engine and a **ChucK** engine, each a fast iteration sandbox for synthesis and CPU work. They predate the engine host and keep their own harnesses because each needs an externally cross-built static library and its own QSPI link.

## Layout

- `app/` — the engine host: `harness.cpp` (board bring-up, `EngineContext`, audio callback, panel mapping), `param_ui.h` (the paged parameter UI and its value pickup), `harness_clock.h` (the `ITimeSource` / `ITransport` glue), and a `Makefile` with one block per engine.

- `src/hw/`, `src/memory/` — the SD audio streaming service (`stream_deck.{h,cpp}`) and the lock-free ring / WAV / raw-stream primitives it is built from. Ported from sk-engines, where they run on hardware. The audio ISR touches only the rings; the harness pumps FatFs from the main loop. Compiled in only for engines that set `SPK_USE_STREAM`.

- `linker/` — `sram_bss_in_axi.lds`, libDaisy's SRAM script with `.bss` moved into the AXI SRAM for the engines whose static working set overflows the 128 KB DTCMRAM (`pstretch`, `bard`, `graincloud`).

- `pod/` — the audio-language harness apps. `harness_csound.cpp` (built by `Makefile`) and `harness_chuck.cpp` (built by `Makefile.chuck`) each stand in for the platform: build an engine context, init the engine behind the `IEngine` interface, and drive `process()` from the audio callback. Both inject a minimal SD service (`sd_stream_deck.h`) as `ctx.stream`, so the engines load a numbered patch bank from the card (`csound/0.csd` .. `csound/7.csd` / `chuck/0.ck` .. `chuck/7.ck`), with the built-in orchestra/program as fallback; an encoder-driven selector (hold encoder, turn knob 1, release) switches patches with a live recompile. Both also receive MIDI NoteOn from the Pod's input and forward it to the engine (Csound plays the patch's `instr MidiNote`; ChucK broadcasts a note Event a `.ck` program can wait on). See [`pod/README.md`](pod/README.md) for the patch bank, MIDI, the bootloader/heap caveats, and flashing.

- `src/` — the engine sources and contract headers the harnesses compile and include: one directory per engine under `src/engine/`, plus the shared `engine/*.h` interfaces. The contract headers (`iengine.h`, `engine_params.h`, `mode.h`, `terminal_io.h`, the display/LED layer) are sk-engines' files with the namespace substituted and nothing else changed, which is what lets an engine port with no source edits at all — see [`docs/dev/porting-sk-engines.md`](docs/dev/porting-sk-engines.md).

- `src/board/` — the control/UI abstraction over the target boards (see Targets below). `controls.h` is the board-agnostic control snapshot; `board.h` selects the driver at compile time; `pod_board.h` / `patch_init_board.h` / `patch_board.h` wrap the respective libDaisy BSPs.

- `libs/` — the Daisy ecosystem libraries the harnesses link (`libDaisy`, `DaisySP`). **Gitignored and reproduced on demand** by `scripts/fetch_libs.sh`, like `thirdparty/` below.

- `examples/` — example patch banks for the SD loader: `examples/csound/0.csd` .. `6.csd` and `examples/chuck/0.ck` .. `7.ck`, each with a README describing the patches and how they behave on the Pod harness. Copy them onto a card with `make sd-card` (see Release/SD below).

- `scripts/` — `fetch_libs.sh`, which clones and builds `libs/libDaisy` + `libs/DaisySP`; `fetch_csound.sh` / `fetch_chuck.sh`, which fetch and cross-build the Csound / ChucK static libraries on demand (see below); `port_engine.sh`, which copies an engine's tree out of an sk-engines checkout with the namespace rewritten; `provision_sd.sh`, which copies the example patch banks onto a mounted SD card (driven by `make sd-card`).

- `thirdparty/` — cross-compiled `libcsound.a` / `libchuck.a` and their source trees. **Gitignored and reproduced on demand** by the fetch scripts rather than vendored.

- `alt_qspi_chuck.lds` — QSPI linker script for the ChucK harness. (The Csound harness uses the linker script that ships inside the fetched Csound Daisy port.) Bootloader flashing uses stock images — libDaisy's bundled bootloader for ChucK, the Csound port's v5.4 for Csound — so none is vendored.

- [`CHANGELOG.md`](CHANGELOG.md) — notable changes since the initial commit ([Keep a Changelog](https://keepachangelog.com/) format).

## Targets (control/UI abstraction)

The harnesses talk to the hardware through a small board abstraction (`src/board/`) rather than a specific BSP, so the same engine/harness logic can run on different Daisy targets:

| Target | `BOARD=` | `-D` define | Board | Controls exposed |
|---|---|---|---|---|
| Daisy Pod | `pod` | `TARGET_POD` | `DaisyPod` | 2 pots, encoder (+click), 2 buttons, 2 RGB LEDs, MIDI in |
| patch.init() | `patch_init` | `TARGET_PATCH_INIT` | `DaisyPatchSM` | 4 pots + 4 CV in, 2 gate in, button + toggle, 1 mono LED, 2 CV out, gate out |
| Daisy Patch | `patch` | `TARGET_PATCH` | `DaisyPatch` | 4 knob+CV in, encoder (+click), 2 gate in, MIDI in, 128x64 OLED, 2 CV out, gate out |

Each board presents a uniform surface: `Init` / `StartAudio` / `SampleRate`, an ISR-safe `Analog(i)` read, a main-loop `Poll(Controls&)` filling a board-agnostic `Controls` snapshot, `StartMidi` / `PollMidi`, `SetIndicator` / `SetUserLed`, `SetCvOut` / `SetGateOut`, and a small `Screen*` text/rect facade. **Every board implements the whole surface**, with no-ops and zero counts where a peripheral is absent — that is what lets one harness drive all three targets with no `#ifdef` around its UI code. The target is chosen at **compile time** via one `-DTARGET_*`, so a build links only its driver with no virtual dispatch.

Select the target with `BOARD=` on the make line (all three are STM32H750):

```
cd app  && make ENGINE=reverb BOARD=patch    # engine host, Daisy Patch (its default board)
cd pod  && make BOARD=patch_init             # Csound harness for patch.init()
cd pod  && make -f Makefile.chuck BOARD=pod  # ChucK harness for the Pod
```

Note one difference between the two Eurorack targets that matters for anything CV-driven: on the Daisy Patch a knob and its jack are summed in analog hardware ahead of the ADC, so a `CTRL` reading is one inseparable knob+CV value and the knob acts as the offset for its input. patch.init() instead exposes four dedicated CV jacks after its four pots.

The Pod is the only board validated on hardware, and only with the csound/chuck harnesses. `patch_board.h` / `patch_init_board.h` and everything under `app/` compile for every target but have never been run on a device.

## Prerequisites

- The `arm-none-eabi` GCC toolchain (the [Daisy toolchain](https://daisy.audio/tutorials/cpp-dev-env/) works), plus `cmake` (for the Csound build) and either `curl`+`tar` or `git` (to fetch sources).

## Build

One-time dependency builds. `libs/` and `thirdparty/` are both gitignored and reproduced on demand, so nothing large is vendored here:

```
scripts/fetch_libs.sh        # clones + builds libs/libDaisy + libs/DaisySP
scripts/fetch_csound.sh      # -> thirdparty/csound/Daisy/lib/libcsound.a
scripts/fetch_chuck.sh       # -> thirdparty/chuck/Daisy/lib/libchuck.a
```

Only `fetch_libs.sh` is needed for the `app/` engine host; the two Csound/ChucK fetches are for the `pod/` harnesses.

Then build an engine from `app/`:

```
cd app
make ENGINE=delay BOARD=patch   # -> build-delay-patch/harness_delay.bin
make all-engines BOARD=patch    # every engine in turn
make list-engines
```

`BOARD` defaults to `patch` there and `ENGINE` to `passthrough`. Each `(engine, board)` pair gets its own build directory, so switching engines needs no clean. Panel map, pickup behaviour and flashing are in [`app/README.md`](app/README.md).

Or an audio-language harness from `pod/`:

```
cd pod
make                       # Csound harness -> build/harness.bin
make -f Makefile.chuck      # ChucK harness  -> build/harness_chuck.bin
```

Those two Makefiles do share one `build/` directory and compile shared objects with different defines, so run `rm -rf build` when switching between them. Flashing instructions are in [`pod/README.md`](pod/README.md).

## Tests and CI

```
make test                 # host suite + the documentation-reference check
make test SANITIZE=       # ...without ASan/UBSan
```

`host/` builds and runs the platform layer natively — no cross toolchain, no hardware, about a second. It covers the parts that are pure logic and happen to ship on a Cortex-M7: the lock-free `SpscRing` and its play/record stream pumps, `WavStreamReader`'s chunk walk and the writer round-trip, `HarnessTransport`'s tick grid and external-sync state machine, and `ParamUI`'s value pickup, page paging and generated action rows. AddressSanitizer and UndefinedBehaviorSanitizer are on by default, which is what turns "reads one chunk past a truncated file" from a field bug into a failing test.

This works because the engine contract is HAL-free: `#include "engine/iengine.h"` compiles under a plain host `g++` with only the standard library (see the note in [`src/math_util.h`](src/math_util.h)). Test doubles for the two injected surfaces live in `host/fakes.h` — a `FakeBoard` that records its draw calls, and a `FakeEngine` that records what was written to it. The harness itself is one dependency-free header, `host/check.h`, matching the stdlib-only convention in `scripts/`.

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs `make test`, then builds **every engine for every board** — three parallel jobs sweeping all 20 engines plus `diag`. That matrix is the point: only one board's driver compiles per build, so a change to `patch_init_board.h` is invisible to a `BOARD=patch` build, which is how that file once carried a stale libDaisy GPIO call through an API change unnoticed. Firmware sizes are written to each run's summary, because several engines needed `-Os` to fit SRAM_EXEC at all and a size regression is how a working image stops linking.

For a faster local check, `make smoke-engines` (in `app/`) builds one engine per **distinct build shape** — header-only, `.cpp`, source wildcard, `.cc`, the `-Os` overrides, streaming, the gen~ and vendored include paths, and the one `BOOT_QSPI` app — rather than a sample of the interesting DSP.

## SD cards

Each audio-language harness loads a numbered patch bank from a FAT32 SD card — `csound/0.csd` .. `7.csd` for the Csound harness, `chuck/0.ck` .. `7.ck` for the ChucK harness — at the card root, with the built-in orchestra/program as the fallback. The engine boot-loads the lowest slot ~1 s after power-on; **hold the encoder and turn knob 1** to scroll the bank, release to switch live. Insert the card before power-on (the harness mounts once at boot).

The six streaming engines read audio instead of text, in layouts of their own (`radio/<bank>/`, `pstretch/`, `bard/<shelf>/`, `tapes/`, `shuttle/`, `softcut/`). That content is **generated, not committed** — `make sd-content` synthesizes it into `examples/sd/`, and `make sd-card` does it for you if it is missing. Before putting your own material on a card, read [`examples/README.md`](examples/README.md): the service reads WAV through two paths that accept incompatible formats (float32 @48k for tape/shuttle/softcut, 16-bit PCM for radio/pstretch/bard), and `make sd-verify` will tell you which files a device would refuse.

`make sd-card` provisions a mounted card for every engine that reads one:

```
make sd-card SD=/Volumes/<card>                       # everything
make sd-card SD=/Volumes/<card> ENGINES=csound        # just one patch bank
make sd-card SD=/Volumes/<card> ENGINES='radio bard'  # just those audio trees
make sd-verify SD_OUT=/Volumes/<card>                 # check what a device would refuse
```

See the per-engine [`examples/csound/README.md`](examples/csound/README.md) / [`examples/chuck/README.md`](examples/chuck/README.md) for what each patch does on the Pod harness (only knob 1 = PITCH and knob 2 = MIX are driven as knobs; MIDI NoteOn is wired — channel 1 = deck A, channel 2 = deck B), and [`pod/README.md`](pod/README.md) for the loader internals.

## Licensing

The project is MIT (see [`LICENSE`](LICENSE)) **except for two engines that carry GPLv3 code**, each with its own in-tree notice:

| Engine | Derives from | Effect |
|---|---|---|
| `qdelay` | [qdelay](https://github.com/tiagolr/qdelay) by tilr (its allpass diffuser, ported as `src/dsp/diffuser.h`) | see [`src/engine/qdelay/NOTICE.md`](src/engine/qdelay/NOTICE.md) |
| `glitch` | [Noisferatu](https://github.com/rob-scape/noisferatu) by Rob Scape (its 12 algorithms, ported as `glitch_voice.h`) | see [`src/engine/glitch/NOTICE.md`](src/engine/glitch/NOTICE.md) |

A firmware built with `ENGINE=qdelay` or `ENGINE=glitch` is a combined work and **must be distributed under GPLv3** (source available, etc.). This matters when publishing binaries — see Release below. Every other engine and all the shared platform code remains MIT; nothing else links either file.

## Release

`make dist` (root `Makefile`) builds every firmware in the engine x board matrix in one shot and collects version-stamped, checksummed binaries under `dist/<version>/` for users who want to download-and-flash rather than build. It drives [`scripts/build_release.py`](scripts/build_release.py), which clean-builds each `(engine, board)` pair, names the artifacts `daisy-<engine>-<board>-<version>.bin`, and writes `MANIFEST.txt`, `SHA256SUMS`, and `RELEASE_NOTES.md` (the CHANGELOG section for the version plus flashing instructions). The script is stdlib-only, so plain `python3` suffices.

```
make dist                                          # describe-derived version, full matrix (2 engines x 3 boards)
make dist VERSION=0.1.0                             # explicit version (the bare tag you will create)
make dist WITH_HEX=1                                # also emit .hex (ST-Link / STM32CubeProgrammer)
```

`RELEASE_ENGINES` and `RELEASE_BOARDS` restrict the matrix to a subset (space-separated lists; they override the defaults of `csound chuck` and `pod patch_init patch`). Valid engines are `csound` / `chuck`; valid boards are `pod` / `patch_init` / `patch` — anything else errors with the valid list.

```
make dist RELEASE_BOARDS=pod                        # one board, both engines (2 artifacts)
make dist RELEASE_ENGINES=csound                    # one engine, all boards (3 artifacts)
make dist RELEASE_BOARDS=pod RELEASE_ENGINES=csound # a single pair (1 artifact)
```

`make dist` covers **both harness families**: all 20 engine-host engines plus the diagnostic, and the two `pod/` audio-language harnesses. The csound and chuck entries are skipped with a notice if their cross-built libraries have not been fetched, so a fresh checkout can build a release without them. `MANIFEST.txt` carries a per-artifact license column, and the release notes call out the GPLv3 binaries (`qdelay`, `glitch`) by name — see Licensing above.

The cross-compiled engine libs (`scripts/fetch_csound.sh` / `scripts/fetch_chuck.sh`) must exist first; the `libDaisy` / `DaisySP` archives are built on demand if missing. Only the `pod` board is hardware-validated, so the `patch_init` / `patch` artifacts are flagged untested in the manifest and notes. `dist/` is gitignored; `make gh-release VERSION=<v>` uploads an already-built `dist/<v>/` as a GitHub release via `gh`.
