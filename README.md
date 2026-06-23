# daisy-apps

A small collection of apps for the [Electrosmith Daisy](https://www.electro-smith.com/daisy) platform (Pod / Seed). Currently this holds two standalone audio-language harnesses ported out of the sk-engines firmware: a **Csound** engine and a **ChucK** engine, each running on a bare Daisy Pod as a fast iteration sandbox for synthesis and CPU work.

## Layout

- `pod/` — the harness apps. `harness_csound.cpp` (built by `Makefile`) and `harness_chuck.cpp` (built by `Makefile.chuck`) each stand in for the platform: build an engine context, init the engine behind the `IEngine` interface, and drive `process()` from the audio callback. Both inject a minimal SD service (`sd_stream_deck.h`) as `ctx.stream`, so the engines load a numbered patch bank from the card (`csound/0.csd` .. `csound/7.csd` / `chuck/0.ck` .. `chuck/7.ck`), with the built-in orchestra/program as fallback; an encoder-driven selector (hold encoder, turn knob 1, release) switches patches with a live recompile. Both also receive MIDI NoteOn from the Pod's input and forward it to the engine (Csound plays the patch's `instr MidiNote`; ChucK broadcasts a note Event a `.ck` program can wait on). See [`pod/README.md`](pod/README.md) for the patch bank, MIDI, the bootloader/heap caveats, and flashing.

- `src/` — the engine sources and contract headers the harnesses compile and include (the `CsoundEngine` / `ChuckEngine` implementations plus the shared `engine/*.h` interfaces). Trimmed to only the files the two harnesses actually depend on.

- `src/board/` — the control/UI abstraction over the target boards (see Targets below). `controls.h` is the board-agnostic control snapshot; `board.h` selects the driver at compile time; `pod_board.h` / `patch_init_board.h` / `patch_board.h` wrap the respective libDaisy BSPs.

- `libs/` — vendored Daisy ecosystem (a DaisyExamples-style tree). `libDaisy` and `DaisySP` are git submodules; the other board folders are upstream examples. The harness Makefiles link `libs/libDaisy` and `libs/DaisySP`.

- `examples/` — example patch banks for the SD loader: `examples/csound/0.csd` .. `6.csd` and `examples/chuck/0.ck` .. `7.ck`, each with a README describing the patches and how they behave on the Pod harness. Copy them onto a card with `make sd-card` (see Release/SD below).

- `scripts/` — `fetch_csound.sh` / `fetch_chuck.sh`, which fetch and cross-build the Csound / ChucK static libraries on demand (see below); `provision_sd.sh`, which copies the example patch banks onto a mounted SD card (driven by `make sd-card`).

- `thirdparty/` — cross-compiled `libcsound.a` / `libchuck.a` and their source trees. **Gitignored and reproduced on demand** by the fetch scripts rather than vendored.

- `alt_qspi_chuck.lds` — QSPI linker script for the ChucK harness. (The Csound harness uses the linker script that ships inside the fetched Csound Daisy port.) Bootloader flashing uses stock images — libDaisy's bundled bootloader for ChucK, the Csound port's v5.4 for Csound — so none is vendored.

- [`CHANGELOG.md`](CHANGELOG.md) — notable changes since the initial commit ([Keep a Changelog](https://keepachangelog.com/) format).

## Targets (control/UI abstraction)

The harnesses talk to the hardware through a small board abstraction (`src/board/`) rather than a specific BSP, so the same engine/harness logic can run on different Daisy targets:

| Target | `BOARD=` | `-D` define | Board | Controls exposed |
|---|---|---|---|---|
| Daisy Pod | `pod` (default) | `TARGET_POD` | `DaisyPod` | 2 pots, encoder (+click), 2 buttons, 2 RGB LEDs |
| patch.init() | `patch_init` | `TARGET_PATCH_INIT` | `DaisyPatchSM` | 4 pots + 4 CV in, 2 gate in, button + toggle, 1 mono LED |
| Daisy Patch | `patch` | `TARGET_PATCH` | `DaisyPatch` | 4 pots, encoder (+click), 2 gate in, OLED |

Each board presents a uniform surface: `Init` / `StartAudio` / `SampleRate`, an ISR-safe `Analog(i)` read, a main-loop `Poll(Controls&)` that fills a board-agnostic `Controls` snapshot (normalized analog array + encoder + buttons + gates), and a no-op-safe `SetIndicator` / `SetUserLed`. The target is chosen at **compile time** via one `-DTARGET_*`, so a build links only its driver with no virtual dispatch.

Select the target with `BOARD=` on the make line (all three are STM32H750, so the QSPI linker script and bootloader are unchanged across them):

```
make BOARD=patch_init                  # Csound harness for patch.init()
make -f Makefile.chuck BOARD=patch     # ChucK harness for Daisy Patch
make                                   # Pod (the default)
```

All three drivers compile and link against both engines. The Pod is the only one validated on hardware; `patch_init_board.h` / `patch_board.h` are implemented but untested on a device. Two pieces remain deferred per the abstraction's input-focused scope: the Patch's OLED (its `SetIndicator` is a no-op) and the CV/gate **outputs** on patch.init()/Patch.

## Prerequisites

- The `arm-none-eabi` GCC toolchain (the [Daisy toolchain](https://daisy.audio/tutorials/cpp-dev-env/) works), plus `cmake` (for the Csound build) and either `curl`+`tar` or `git` (to fetch sources).

- Submodules checked out: `git submodule update --init --recursive`.

## Build

One-time dependency builds:

```
make -C libs/libDaisy        # libdaisy.a
make -C libs/DaisySP         # libdaisysp.a
scripts/fetch_csound.sh      # -> thirdparty/csound/Daisy/lib/libcsound.a
scripts/fetch_chuck.sh       # -> thirdparty/chuck/Daisy/lib/libchuck.a
```

Then build a harness from `pod/`:

```
cd pod
make                       # Csound harness -> build/harness.bin
make -f Makefile.chuck      # ChucK harness  -> build/harness_chuck.bin
```

Both Makefiles share the same `build/` directory but compile shared objects with different defines, so run `rm -rf build` when switching between the Csound and ChucK targets. Flashing instructions are in [`pod/README.md`](pod/README.md).

## SD patch banks

Each harness loads a numbered patch bank from a FAT32 SD card — `csound/0.csd` .. `7.csd` for the Csound harness, `chuck/0.ck` .. `7.ck` for the ChucK harness — at the card root, with the built-in orchestra/program as the fallback. The engine boot-loads the lowest slot ~1 s after power-on; **hold the encoder and turn knob 1** to scroll the bank, release to switch live. Insert the card before power-on (the harness mounts once at boot).

`make sd-card` copies the bundled [`examples/`](examples/) patches onto a mounted card:

```
make sd-card SD=/Volumes/<card>                 # both banks
make sd-card SD=/Volumes/<card> ENGINES=csound  # just one
```

See the per-engine [`examples/csound/README.md`](examples/csound/README.md) / [`examples/chuck/README.md`](examples/chuck/README.md) for what each patch does on the Pod harness (only knob 1 = PITCH and knob 2 = MIX are driven as knobs; MIDI NoteOn is wired — channel 1 = deck A, channel 2 = deck B), and [`pod/README.md`](pod/README.md) for the loader internals.

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

The cross-compiled engine libs (`scripts/fetch_csound.sh` / `scripts/fetch_chuck.sh`) must exist first; the `libDaisy` / `DaisySP` archives are built on demand if missing. Only the `pod` board is hardware-validated, so the `patch_init` / `patch` artifacts are flagged untested in the manifest and notes. `dist/` is gitignored; `make gh-release VERSION=<v>` uploads an already-built `dist/<v>/` as a GitHub release via `gh`.
