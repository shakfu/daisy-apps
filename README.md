# daisy-apps

A small collection of apps for the [Electrosmith Daisy](https://www.electro-smith.com/daisy) platform (Pod / Seed). Currently this holds two standalone audio-language harnesses ported out of the sk-engines firmware: a **Csound** engine and a **ChucK** engine, each running on a bare Daisy Pod as a fast iteration sandbox for synthesis and CPU work.

## Layout

- `pod/` — the harness apps. `harness_csound.cpp` (built by `Makefile`) and `harness_chuck.cpp` (built by `Makefile.chuck`) each stand in for the platform: build an engine context, init the engine behind the `IEngine` interface, and drive `process()` from the audio callback. See [`pod/README.md`](pod/README.md) for what they do, the bootloader/heap caveats, and flashing.

- `src/` — the engine sources and contract headers the harnesses compile and include (the `CsoundEngine` / `ChuckEngine` implementations plus the shared `engine/*.h` interfaces). Trimmed to only the files the two harnesses actually depend on.

- `src/board/` — the control/UI abstraction over the target boards (see Targets below). `controls.h` is the board-agnostic control snapshot; `board.h` selects the driver at compile time; `pod_board.h` / `patch_init_board.h` / `patch_board.h` wrap the respective libDaisy BSPs.

- `libs/` — vendored Daisy ecosystem (a DaisyExamples-style tree). `libDaisy` and `DaisySP` are git submodules; the other board folders are upstream examples. The harness Makefiles link `libs/libDaisy` and `libs/DaisySP`.

- `scripts/` — `fetch_csound.sh` / `fetch_chuck.sh`, which fetch and cross-build the Csound / ChucK static libraries on demand (see below).

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
