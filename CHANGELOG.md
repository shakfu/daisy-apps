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

### Changed

- Renamed the C++ namespace `spotykach` to `daisyapps` across all sources, and reworded the
  sk-engines/spotykach references in comments and docs for this standalone repo.
- Pointed the harness Makefiles at the vendored `libs/libDaisy` and `libs/DaisySP`.
- ChucK harness re-enables exceptions/RTTI through `CPP_STANDARD` (the stock upstream libDaisy in
  `libs/` has no `CPP_USER_FLAGS` hook), keeping `libs/` a pristine submodule.
- `thirdparty/` (the Csound/ChucK source trees and static libraries) is gitignored and reproduced on
  demand by the fetch scripts instead of being vendored.
- Trimmed `src/` to only the engine sources and headers the two harnesses actually compile.
- Scoped the `.gitignore` rule for `Makefile` to the repo root (`/Makefile`) so `pod/Makefile` is
  tracked.

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
