# Design: re-introducing ChucK `MidiIn` on the Daisy build

Status: **prototype implemented (build verified, hardware wake test pending).** The plan below was
executed: `libchuck.a` is rebuilt with the `MidiIn`/`MidiOut` device classes enabled and a thread-free,
RtMidi-stripped backend, and the engine feeds incoming NoteOns into `MidiIn` (device 0). The library,
the internal link-test, and the Pod harness all build and link with matching ABI. What remains is the
on-hardware confirmation that a shred blocked on `min => now` actually wakes and `min.recv()` drains
(step 4 / Risks) - see [Implementation status](#implementation-status).

## Implementation status

What was done (all build-verified; see the [References](#references) for the touched files):

- **Build flags / sources** (`scripts/fetch_chuck.sh`): dropped `__DISABLE_MIDI__`, added
  `midiio_rtmidi` to the compiled set, kept `rtmidi.cpp` out. A committed patch
  (`scripts/patches/midi_daisy.patch`, applied idempotently by the fetch script) carries the
  vendored-source edits, since `thirdparty/chuck` is gitignored and regenerated from upstream.
- **RtMidi stub** (`midiio_rtmidi.{h,cpp}`): removed every `RtMidiIn`/`RtMidiOut` backend call;
  `MidiInManager::open` now registers a single virtual UART device, and a new
  `MidiInManager::inject(device, status, d1, d2)` feeds bytes through the existing
  `cb_midi_input -> CBufferAdvance::put` path. `MidiOut` is registered but inert (open always fails).
  `RtMidiIn`/`RtMidiOut` are reduced to minimal stand-ins (only `getPortName()` is referenced, by
  `MidiIn.name()`).
- **ABI sync** (`pod/Makefile.chuck`): dropped `__DISABLE_MIDI__` to match the archive.
- **Host feed** (`src/engine/chuck/chuck_engine.cpp`): `process()` injects each NoteOn into device 0
  (status `NoteOn|deck`, note, fixed velocity 100) right before `ck->run()`, alongside the existing
  global bridge. Same thread as `run()`, so it can't race a shred's `min.open()`.
- **Example** (`examples/chuck/midi_in.ck`): opens `MidiIn`, blocks on `min => now`, drains with
  `min.recv(msg)`, sporks a voice per NoteOn - the desktop-portable idiom.

**The highest documented risk is now largely retired by evidence:** the shipped global bridge already
broadcasts a `Chuck_Event` from the run() thread under `__DISABLE_THREADS__` and a shred waiting on it
resumes (`chuck_engine.cpp` `broadcastGlobalEvent` + `examples/chuck/midi.ck` `noteOnA => now`). The
`MidiIn` path reuses the same per-VM event-buffer machinery, so the remaining hardware test is
confirmation, not discovery.

Known limitations of the prototype: NoteOn only with a fixed velocity (the bridge ring carries no
velocity), and `MidiIn` injection is fed from the lossy note ring rather than directly from the parsed
UART (so CC/pitch-bend are not yet delivered). Forwarding the full `MidiUartHandler` stream into
`inject()` is the natural follow-up once the wake is confirmed.

---

The original feasibility analysis and plan follow.

## Summary

ChucK's in-language MIDI device classes (`MidiIn` / `MidiOut`) are compiled out of this firmware, so
patches cannot read MIDI the desktop way (`MidiIn min; min.recv(msg);`). Today the host owns the Pod's
UART MIDI and injects notes into the VM through globals plus a broadcast Event (the "global bridge",
see [Current state](#current-state)). Re-introducing real `MidiIn` is possible, but not by toggling a
build flag: it requires rebuilding `libchuck.a` with a small custom, thread-free MIDI backend that
replaces RtMidi and feeds ChucK's MIDI buffers from the UART. This document records why, how, the
risks, and whether it is worth doing.

## Motivation

The global bridge already delivers polyphonic NoteOn, so for notes alone `MidiIn` adds nothing. It earns
its keep only for two things the bridge does not give cheaply:

- **Desktop-patch portability.** Unmodified ChucK patches that use `MidiIn` / `MidiMsg` would run as-is,
  instead of having to adopt the host-specific `notesA` / `noteCountA` / `noteOnA` global convention.
- **The full MIDI vocabulary for free.** Control change, pitch bend, program change, clock, aftertouch -
  all of it arrives as `MidiMsg.data1/2/3`. The global bridge would need a new global (and engine code)
  per message type; `MidiIn` exposes everything through one class.

If the project only ever needs notes, this initiative should stay shelved. It becomes worthwhile when a
goal appears that needs arbitrary MIDI input or running third-party ChucK MIDI patches unchanged.

## Background: how ChucK `MidiIn` works

`MidiIn` is a `Chuck_Event` subclass backed by a per-VM lock-free buffer
(`midiio_rtmidi.h:135`, `MidiInManager` at `:179`). The data path:

- A backend delivers bytes to `MidiInManager::cb_midi_input(deltatime, msg, devnum)`
  (`midiio_rtmidi.cpp:639`), which, for each VM registered on that device, does `cbuf->put(&m, 1)` into a
  `CBufferAdvance` (`:668`). This step needs no thread.
- A shred blocks with `min => now` and drains with `min.recv(msg)`; `recv` reads from the same buffer
  (`m_buffer->get(...)`, `midiio_rtmidi.cpp:629`).
- On the desktop, the backend is **RtMidi** (`rtmidi.cpp`), which talks to an OS MIDI API (CoreMIDI /
  ALSA / WinMM) and runs its own callback thread that calls `cb_midi_input`.

Note that `MidiMsg` (the plain data class) is compiled even with MIDI disabled - there is an explicit
"still compile MidiMsg even if `__DISABLE_MIDI__`" at `chuck_compile.cpp:1293`. So the message type
already exists in the VM; only the device class and its backend are missing.

## Why it is disabled on this build

Three independent layers stand in the way; the build flag is only the first.

1. **The device classes are not built.** `scripts/fetch_chuck.sh:285` omits `midiio_rtmidi` and `rtmidi`
   from the compiled source list, and `chuck_io.cpp` guards the `MidiIn` / `MidiOut` registration with
   `#ifndef __DISABLE_MIDI__`. `__DISABLE_MIDI__` is set at `scripts/fetch_chuck.sh:274`. So `MidiIn` is
   absent from the type system.
2. **RtMidi has no embedded backend.** RtMidi implements only OS APIs; there is no STM32/UART driver, and
   it expects a callback thread. It cannot run on bare metal as-is.
3. **Threads are disabled.** `__DISABLE_THREADS__` (`scripts/fetch_chuck.sh:275`) removes ChucK's and
   RtMidi's threading, so the desktop threaded-callback model is unavailable regardless.

The one piece of good news: the injection point, `cb_midi_input` -> `cbuf->put`, is plain C++ and needs
no thread. A bare-metal backend can call it directly from the main loop.

## Proposed approach

Replace RtMidi with a minimal, single-device, thread-free backend fed from libDaisy's
`MidiUartHandler`. Concrete steps:

1. **Build flags / sources (`scripts/fetch_chuck.sh`).** Drop `__DISABLE_MIDI__`; add `midiio_rtmidi` to
   the compiled set; do **not** add `rtmidi.cpp`. The 166 KB RtMidi translation unit stays out; only the
   ~57 KB ChucK-facing `midiio_rtmidi.cpp` is built, with its `RtMidiIn` references stubbed (below).
2. **Stub the backend.** In `MidiInManager::open` / `close`, drop the `RtMidiIn` creation and
   `setCallback`; register a single virtual UART device instead. Provide a host hook (e.g.
   `MidiInManager::inject(devnum, status, d1, d2)`) that builds the message and calls the existing
   `cb_midi_input` path (or writes `the_bufs` directly).
3. **Feed from the host.** In the ChucK harness / engine main loop, drain `MidiUartHandler` (the same
   source `PodBoard::PollMidi` already reads) and call the inject hook for each parsed message, rather
   than - or in addition to - the current global bridge.
4. **Wake the shred.** Confirm `min => now` resumes when the buffer is filled under `__DISABLE_THREADS__`.
   `cb_midi_input` only fills the buffer; the `Chuck_Event` resume path is the genuinely uncertain part
   and may need the `MidiIn` event broadcast on each block that has new data. **Prototype this first**
   (see [Risks](#risks)).
5. **Keep the ABI in sync.** Removing `__DISABLE_MIDI__` changes ChucK class layouts, so the engine TU's
   defines in `pod/Makefile.chuck` MUST match the set `libchuck.a` was rebuilt with - the same discipline
   the existing feature defines already follow (a mismatch silently corrupts vtables / member offsets).

## Risks

- **Threadless event wake (highest).** If a `Chuck_Event`-based `MidiIn` cannot be resumed cleanly from
  the main loop without ChucK's threading, the whole approach needs rethinking. This is cheap to
  derisk - a throwaway branch that fills a buffer and checks whether a waiting shred resumes - and should
  be done before any of the backend work.
- **Vendored-source surgery + rebuild.** Editing `midiio_rtmidi.cpp` and re-running the slow
  `fetch_chuck.sh` cross-build, then keeping `Makefile.chuck` ABI-aligned, is real maintenance cost that
  rides on top of the vendored ChucK tree.
- **Flash / RAM budget.** Re-enabling the MIDI classes adds code; measure against the QSPI budget (the
  ChucK app is already ~1.3 MB). Avoiding `rtmidi.cpp` keeps most of the weight out.

## Decision context (vs. the current global bridge)

| | Global bridge (today) | `MidiIn` (this proposal) |
|---|---|---|
| Notes (incl. chords) | yes, polyphonic | yes |
| CC / pitchbend / clock / etc. | no (one global + engine code per type) | yes, via `MidiMsg` |
| Desktop patch portability | no (host-specific globals) | yes (`MidiIn` runs unchanged) |
| Cost | implemented | rebuild `libchuck.a` + custom backend + ABI care |

Recommendation: keep the global bridge as the default. Pursue `MidiIn` only when a concrete need for the
full MIDI vocabulary or unmodified desktop patches arrives, and gate the work behind the wake-path
prototype (step 4 / Risks).

## References

- Current global bridge: `src/engine/chuck/chuck_engine.cpp` (`handle_midi_note` + the `process()`
  note-batch drain), the shared ring/map in `src/engine/midi_note.h`, the harness wiring in
  `pod/harness_chuck.cpp`, and `PodBoard::PollMidi` in `src/board/pod_board.h`. Patch-side convention and
  example: `examples/chuck/README.md` and `examples/chuck/midi.ck`.
- ChucK MIDI internals: `thirdparty/chuck/src/core/midiio_rtmidi.{h,cpp}` (the `MidiIn` / `MidiInManager`
  classes and `cb_midi_input`), `rtmidi.{h,cpp}` (the OS backend, to be replaced), `chuck_io.cpp` (the
  `#ifndef __DISABLE_MIDI__` registration), `chuck_compile.cpp:1293` (MidiMsg always compiled).
- Build flags: `scripts/fetch_chuck.sh:274-275` (`__DISABLE_MIDI__`, `__DISABLE_THREADS__`) and `:285`
  (the compiled source list); the matching engine-side defines in `pod/Makefile.chuck`.
