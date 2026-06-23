# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is
pre-release and does not yet follow semantic versioning, so everything since the initial commit lives
under **Unreleased**.

## [Unreleased]

### Added

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
  reference patch shows the ChucK convention.
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

### Changed

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

- The spotykach front-panel LED/ring/display layer: `led.ring.{h,cpp}`, `display_model.h`,
  `engine_leds.h`, `color.{h,cpp}`; the `render()` / `render_ring()` / LED-query virtuals from
  `IEngine`; the `render()` overrides and their output meters from both engines; and the now-unused
  `CapOwnDisplay` capability. The engines are now audio-only behind a slimmer `IEngine`.
- `bootloader-spotykach-v2.bin` (project-specific bootloader). `program-boot` now flashes libDaisy's
  bundled stock bootloader for the ChucK harness; the Csound harness uses the Daisy v5.4 image from
  the fetched Csound port.

### Notes

- Validated on hardware: Daisy Pod only. The patch.init() and Daisy Patch drivers compile and link
  against both engines but are untested on a device.
- Deferred (per the abstraction's input-focused scope): the Daisy Patch OLED (`SetIndicator` is a
  no-op there) and the CV/gate **outputs** on patch.init() and Daisy Patch.
