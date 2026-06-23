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

### SD patch bank

The harness injects a minimal SD stream service (`sd_stream_deck.h`) as `ctx.stream`, so the engine's
`.csd` patch bank works. Put numbered slots in a `csound/` folder at the card root —
`csound/0.csd` .. `csound/7.csd`, each a full CSD document. The engine still boots its built-in
orchestra; once the card mounts it auto-loads the first present slot (~1 s after boot), and you scroll
the `[built-in, present slots...]` list with the **encoder selector**:

- **Hold the encoder** to open the bank (the Pod analog of the firmware's Alt pad).
- **Turn knob 1** while holding to scroll/preview the list (the Alt+PITCH gesture).
- **Release** to commit — a live `csoundReset` + recompile via `ReloadGate`.

`SdStreamDeck` implements only the two `IStreamDeck` methods the bank uses — `exists()` (f_stat) and
`read_text()` (f_open/f_read) — over a FatFs-mounted card; the streaming/recording half is stubbed.
The card must be inserted at power-on (the harness mounts once and does not hot-remount). With no card,
mount fails harmlessly and the engine runs the built-in orchestra. Linking the bank needs the FatFs
sources compiled into the app, which the Makefile enables with `USE_FATFS = 1`.

Ready-made patches live in [`../examples/`](../examples/) (`examples/csound/*.csd`,
`examples/chuck/*.ck`, each with a README). Copy them onto a card with `make sd-card SD=/Volumes/<card>`
from the repo root. Note the harness drives only knob 1 (PITCH) and knob 2 (MIX), so patches that lean
on the other control channels play with those parameters fixed; MIDI NoteOn is wired (see below) — see
the `examples/*/README.md` per-patch notes.

## What it does NOT exercise

Beyond the SD patch bank above, the harness injects no other platform services, so it is a
**synthesis / opcode / CPU sandbox**, not a full repro of the engine's features:

- **No transport/clock or QSPI-settings** services.

### MIDI in

Both harnesses receive MIDI from the Pod's input. The board abstraction exposes `StartMidi()` and a
templated `PollMidi(sink)` (board-specific: real on the Pod, no-op on the patch targets); the harness
forwards each NoteOn to `engine.handle_midi_note(channel, note)` from the main loop, and the engine
drains the notes in the audio ISR. Channel 1 -> deck A, channel 2 -> deck B (`Config::dynamic()`).
NoteOn only (no NoteOff/velocity), so notes are finite, self-terminating voices. The Csound engine
plays each patch's `instr MidiNote`. ChucK's own `MidiIn` is compiled out of this bare-metal build
(`__DISABLE_MIDI__`), so the engine bridges instead: per block it hands a deck's NoteOns to the VM as an
int array (`notesA`) + count (`noteCountA`) and one broadcast `noteOnA` Event a `.ck` program waits on,
sporking a voice per note - so chords play **polyphonically**. The generic note ring + note->Hz map live
in `src/engine/midi_note.h`, shared by both engines. See the `examples/*/README.md` for the per-engine
note conventions.

Both harnesses wire the SD patch bank the same way: `harness_chuck.cpp` injects the same
`SdStreamDeck` for a `chuck/0.ck` .. `chuck/7.ck` bank with the identical encoder selector. Its one
difference is plumbing, not behavior: ChucK pushes the PITCH/MIX knobs from the audio ISR (rate-limited
per block), so a `volatile` flag tells the ISR to release knob 1 to the selector while the encoder is
held. (FatFs is built `_USE_LFN=1` / static-buffer, so it makes no `malloc` calls and stays out of
ChucK's `--wrap` SDRAM pool.)

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
