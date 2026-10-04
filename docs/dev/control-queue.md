# Control queue (rejected, 2026-10-04)

A design that was built, measured against the engines, and reverted. Recorded so it is not
re-derived.

## Proposal

`process()` runs in the audio ISR. The panel calls `set_param`, `set_config`, the pads, MIDI and CV
mutators from the main loop, and each engine synchronises these on its own or not at all.

The proposal: a proxy implementing `IEngine` that queues every mutator and applies them in the audio
callback before `process()`. Knobs and CV would go to latest-value slots; pads, configs, MIDI and
gates to a FIFO.

## Why it was rejected

The queue must be opt-in per engine. A mutator that does SD or flash I/O, a large memset, or shares
state with `prepare()` cannot run in the ISR. The per-engine audit below found:

- **The engines with serious races cannot opt in.** That covers shuttle and softcut (the double-load
  race, E9, and the voice mutation, E10), bard, and granular/graincloud.
- **Most engines that can opt in had no race worth fixing.** delay, qdelay, reverb, chorus, filter,
  voice, gigaverb, passthrough, mosc and radio take single 32-bit stores, which do not tear on the
  M7.
- **The real gains were small:**
  - glitch's torn algorithm switch: one block of wrong audio, in bounds;
  - reso's lost model change (low);
  - Csound's control-channel writes during perform, partly covered already by the malloc lock.
  - ChucK's queue corruption was already fixed by a dirty mask.

Against that it cost:
- **Latency.** Up to one block for engines that read parameters per sample: 1 ms in `app/` (48
  frames), 5.3 ms in `pod/` (256 frames). This is reasoned from the code, not measured. Engines that
  read once per block lose roughly nothing.
- **Coalescing.** At most one value per parameter per block.
- **Lost return values.** `set_config`, `on_play_pad` and `handle_midi_note` could no longer return
  anything.
- **Edges.** A press and release inside one block reached the engine as no call.
- **Contract drift.** Two `iengine.h` statements no longer held: `handle_midi_message` runs on the
  main loop, and `set_aux_active` is pushed every loop.
- **Upkeep.** A hand-maintained opt-in list in `app/Makefile` that does not travel with the engine
  source.

The races worth fixing live in specific engines, and most can only be fixed by editing those engines.
That is where the fix belongs.

## When to reconsider

- A high-rate control source arrives (terminal, OSC) and a deadband no longer bounds main-loop
  traffic.
- Enough engines move their I/O behind a `prepare()` flag upstream, as radio already does, that most
  of them could opt in.

## Audit (2026-10-04)

Criteria: every overridden mutator, followed through its helpers,
1. does no SD, StreamDeck, QSPI or flash I/O, no allocation, and no long loop;
2. shares no multi-field state with `prepare()`, `render()` or another main-loop method.

| Engine | Could opt in | Deciding evidence |
|-|-|-|
| passthrough | yes | Overrides no mutator. |
| delay, qdelay | yes | Plain stores; `prepare()` is empty. |
| glitch | yes | `set_algo` / `regen` rewrite a 4000-sample buffer. |
| reverb, chorus, filter, voice | yes | Faust zone stores (`faust_fx.h`, `faust_chain.h`). |
| gigaverb | yes | gen~ clamp-and-store setters. |
| reso, mosc | yes | Deck field writes; `trigger()` already runs in `process()`. |
| radio | yes | Mutators set values and flags; `prepare()` does the scan and open. |
| chuck, csound | yes | Mutators cache, flag and enqueue. |
| edrums | after one edit | `take_param_reseed`'s test-then-clear would lose a reseed set in the ISR. |
| granular, graincloud | no | `clear_buffer` and record-arm reach a 16-32 MB SDRAM memset. |
| tape | no | Pads call `start_play` / `start_record` (FatFs). |
| shuttle | no | A slot pick in `set_param` calls `start_play`. |
| softcut | no | `on_record_pad(reverse)` calls `start_record`. |
| pstretch | no | `set_config(Mode)` stops and opens SD streams. |
| bard | no | `on_record_pad` appends to `_marks`, which `prepare()` also rewrites. |
