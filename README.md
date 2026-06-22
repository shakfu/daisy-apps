# daisy-apps

A small collection of apps for the [Electrosmith Daisy](https://www.electro-smith.com/daisy) platform
(Pod / Seed). Currently this holds two standalone audio-language harnesses ported out of the
sk-engines firmware: a **Csound** engine and a **ChucK** engine, each running on a bare Daisy Pod as a
fast iteration sandbox for synthesis and CPU work.

## Layout

- `pod/` — the harness apps. `harness_csound.cpp` (built by `Makefile`) and `harness_chuck.cpp`
  (built by `Makefile.chuck`) each stand in for the platform: build an engine context, init the
  engine behind the `IEngine` interface, and drive `process()` from the audio callback. See
  [`pod/README.md`](pod/README.md) for what they do, the bootloader/heap caveats, and flashing.
- `src/` — the engine sources and contract headers the harnesses compile and include (the
  `CsoundEngine` / `ChuckEngine` implementations plus the shared `engine/*.h` interfaces). Trimmed to
  only the files the two harnesses actually depend on.
- `libs/` — vendored Daisy ecosystem (a DaisyExamples-style tree). `libDaisy` and `DaisySP` are git
  submodules; the other board folders are upstream examples. The harness Makefiles link
  `libs/libDaisy` and `libs/DaisySP`.
- `scripts/` — `fetch_csound.sh` / `fetch_chuck.sh`, which fetch and cross-build the Csound / ChucK
  static libraries on demand (see below).
- `thirdparty/` — cross-compiled `libcsound.a` / `libchuck.a` and their source trees. **Gitignored
  and reproduced on demand** by the fetch scripts rather than vendored.
- `bootloader-spotykach-v2.bin`, `alt_qspi_chuck.lds` — bootloader image and QSPI linker script used
  by the ChucK harness (the Csound harness uses the stock Daisy v5.4 bootloader + linker script that
  ship inside the fetched Csound Daisy port).

## Prerequisites

- The `arm-none-eabi` GCC toolchain (the [Daisy toolchain](https://daisy.audio/tutorials/cpp-dev-env/)
  works), plus `cmake` (for the Csound build) and either `curl`+`tar` or `git` (to fetch sources).
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

Both Makefiles share the same `build/` directory but compile shared objects with different defines,
so run `rm -rf build` when switching between the Csound and ChucK targets. Flashing instructions are
in [`pod/README.md`](pod/README.md).
