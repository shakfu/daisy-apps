# Daisy Pod — Csound harness

A standalone, quick-iteration target that runs the real `CsoundEngine` on a bare **Daisy** (Pod /
Seed), without the host firmware platform and front panel. A Pod is faster to flash and poke at than
the full firmware, so this is a handy sandbox for working on Csound synthesis, opcodes, and CPU
behaviour before changes land in the actual engine.

This harness is **not** shipping firmware - it builds the engine sources vendored in
`src/engine/csound/` purely as a fast hardware sandbox for synthesis and CPU work.

## What it does

`harness.cpp` stands in for the platform: it builds an `EngineContext`, `init()`s `CsoundEngine`,
drives `process()` from the audio callback, and forwards the Pod's two knobs to `set_param()`
(knob 1 → PITCH/pitch, knob 2 → MIX/level). It compiles the **actual** engine
(`../src/engine/csound/csound_engine.cpp`), so it exercises the real `IEngine` code path.

## What it does NOT exercise

The harness injects no platform services, so it is a **synthesis / opcode / CPU sandbox**, not a full
repro of the engine's features:

- **No SD card** (`ctx.stream = nullptr`) → only the built-in orchestra; no `/csound/*.csd` patch
  bank and no Alt+PITCH selector / live recompile.
- **No MIDI** wiring (the `MidiNote` path is untested here).
- **No transport/clock or QSPI-settings** services.

## Build & flash

This was copied here from the sk-engines repo. The engine sources it links live in `../src/`. The
cross-compiled `libcsound.a` / `libchuck.a` (and their source trees) are **not** vendored — `../thirdparty/`
is gitignored and reproduced on demand by the fetch scripts. Build the dependencies once:

```
make -C ../libs/libDaisy        # build libdaisy.a   (once)
make -C ../libs/DaisySP         # build libdaisysp.a (once)
../scripts/fetch_csound.sh      # fetch + cross-build ../thirdparty/csound/Daisy/lib/libcsound.a
../scripts/fetch_chuck.sh       # fetch + cross-build ../thirdparty/chuck/Daisy/lib/libchuck.a
```

(`fetch_csound.sh` needs `cmake`; both need the arm-none-eabi toolchain and curl+tar or git. Run only
the one for the harness you intend to build.) Then, from this directory:

```
make                       # Csound harness  -> build/harness.bin
make -f Makefile.chuck     # ChucK harness   -> build/harness_chuck.bin
make program-dfu           # flash over USB DFU (catch the bootloader's DFU window; tap RESET after)
```

Both Makefiles share the same `build/` dir but compile shared objects (e.g. `startup`) with
different defines, so **`rm -rf build` when switching between the Csound and ChucK targets**.

The Makefiles' `../src`, `../libs/{libDaisy,DaisySP}`, and `../thirdparty/{csound,chuck}` paths assume
this directory sits at the repo root (one level deep) — keep it there. (In sk-engines the Daisy libs
lived at `../lib`; here they are the vendored `../libs` tree, the only path that changed.)

## Bootloader & heap notes

1. **Bootloader.** Both harnesses are `BOOT_QSPI` apps loaded at the standard Daisy app base
   (`0x90040000`), so any QSPI-capable bootloader boots them (the ChucK harness also re-injects VTOR
   itself). `make program-dfu` / `program-swd` flash only the app and leave the bootloader alone;
   `make program-boot` (re)flashes a bootloader and **overwrites whatever bootloader is on the board**.
   For `program-boot` the Csound Makefile uses the stock **Daisy v5.4** image (`dsy_bootloader_v5_4.bin`,
   from the fetched Csound Daisy port) and the ChucK Makefile falls back to libDaisy's bundled stock
   bootloader (`dsy_bootloader_v6_2-intdfu-2000ms.bin`). No project-specific bootloader is vendored.

2. **Heap model.** The Pod puts Csound's heap straight in SDRAM via the port's
   `STM32H750IB_qspi_custom.lds` (newlib `malloc`), so `csound_heap_arm()` is a **no-op** here
   (defined in `harness_csound.cpp`). The ChucK harness routes ChucK's C-malloc family into the SDRAM
   `--wrap` pool (`chuck_alloc.cpp`) via `alt_qspi_chuck.lds`.

## Caveat

`harness.cpp` predates the SD/MIDI/selector work and isn't kept in lockstep with the engine — if a
build breaks after engine changes, it's usually a new symbol the harness needs to stub or a context
field to set. It builds the real engine, so it's worth keeping in sync as a fast hardware loop.
