# Hardware bring-up — Daisy Patch

A running record, not a finished document. Fill in the results as you go; a failure here is worth more
written down than remembered.

**Nothing in `app/` has ever run on a device, and neither has the Patch board driver.** That is the
whole point of this exercise, and it is also why the ORDER below matters more than the coverage: each
stage adds exactly one new unknown, so a failure names its own cause. Testing engines in a random
order would leave every symptom ambiguous between the engine, the harness and the driver.

## Results so far (Daisy Patch, 2026-08-23)

First hardware run of anything under `app/`. Flashed over DFU from the dev machine
(`dfu-util` to QSPI `0x90040000`); every image so far has left DFU cleanly, which means the
bootloader validated it and handed off.

| Build | Flashed | Reported | Notes |
|---|---|---|---|
| `diag` | yes | **all seven pages OK** | Encoder counts **one per detent** despite the unthrottled main loop, and the **SDRAM check passes**. Those were the two blockers. |
| `passthrough` | yes | works | |
| `chorus` | yes | works | first Faust-generated engine |
| `filter` | yes | works | |
| `reverb` | yes | works | first engine to allocate from the SDRAM arena |
| `delay` | yes | pending | first engine to read the transport |

**Not yet confirmed on hardware**, and deliberately not marked as passing: the four UI behaviours
changed in this session (boot pickup priming, the header tempo, the OLED engine page, the
action-screen cursor rule) and the `config()` reader's `1/3`-not-`-/3` display. "The engine works"
does not cover them, and they are host-tested only. See the per-stage checklists below.

## What this board can and cannot show

The Daisy Patch has **no discrete LEDs** (`kIndicatorCount == 0`). The indicator projection in
`app/display_adapter.h` is compiled out here, so **none of the LED work is observable on this board** —
that is a Pod test. What the Patch does have is the OLED, so the engine page, the parameter pages and
the action screen are all in scope.

## Pre-flight

- [ ] **Bootloader flashed once.** `cd app && make program-boot` (hold BOOT, tap RESET first).
- [ ] **SD card.** FAT32, inserted *before* power-on (the harness mounts once at boot).
      `make sd-card SD=/path/to/card` — the tree is already generated and verified here (20 MB).
      Only the six streaming engines need it, but `diag` reports on it, so put it in from the start.
- [ ] **Audio in.** Several engines are effects and do nothing audible without a source.
- [ ] **MIDI in** for `reso` / `mosc` / `granular`, if you have a keyboard.
- [ ] Optional: a clock into GATE IN 2, a trigger into GATE IN 1.

Flashing an engine, once the bootloader is on:

```
cd app
while ! make ENGINE=<name> BOARD=patch program-dfu; do sleep 0.2; done   # then tap RESET
```

## Stage 0 — `diag` (do this first, and do not skip it)

```
make ENGINE=diag BOARD=patch program-dfu
```

Links **no engine and none of the engine contract**. It is the only build that can tell you whether
the board driver works, and every later failure is ambiguous without it. Seven pages, encoder turns
between them, click is the page's action.

| Page | Pass looks like | Result |
|---|---|---|
| ANALOG | four bars that track CTRL 1-4 smoothly, full travel 0..1 | |
| DIGITAL | encoder increments **by one per detent** in both directions; press registers; gate edges count | |
| MIDI | a note in shows the right type, channel and note name | |
| SD | card mounts, files found, every file passes its format check | |
| AUDIO | block counter climbing; input meters respond; click gives a clean 440 Hz tone | |
| OUTPUTS | CV sweep visible on a meter/scope; gate out toggles | |
| SYSTEM | 48 kHz, block 48, **SDRAM check passes**, uptime climbing | |

**Stop here if anything fails.** In particular: if the encoder counts more than one per detent, or the
SDRAM check fails, no engine result below will mean anything.

## Stage 1 — `passthrough`

The "is it alive" build: no arena, no clock, trivial DSP. This is really a test of the **harness and
the UI**, not of an engine.

- [ ] Audio passes through cleanly.
- [ ] OLED shows `passthrough 1/N` and a parameter page with named rows and value bars.
- [ ] **Boot pickup (new this session):** before powering on, leave the knobs somewhere obviously
      wrong. On boot the on-screen values must **not** jump to the knob positions — rows should show
      a `.` prefix and a caret until you sweep each knob across its value. *This is the priming fix;
      if params snap at boot it is regressed.*
- [ ] **Tempo in the header (new).** Top-right should show a BPM, not `-0`. Hold the encoder and turn:
      the number should move.
- [ ] Encoder turn changes page; the page count is right.
- [ ] **Engine page (new).** `passthrough` draws, so the last page in the rotation should be the
      engine view: `passthrough A eng`, a ring bar-strip row per deck, and a word line.
- [ ] **Action screen (new cursor rule).** Click the encoder. The cursor should land on a *momentary*
      row — `play` if there is one, otherwise `back`. It must **never** open onto a config switch,
      because the second click would then change a mode.

## Stage 2 — self-contained effects (no SD, no MIDI)

Needs an audio source. Nothing here should be silent.

| Engine | What to check | Result |
|---|---|---|
| `chorus` | audible chorusing; knobs named on screen | |
| `filter` | filter sweeps across its cutoff knob | |
| `voice` | makes sound on its own (instrument → filter) | |
| `gigaverb` | reverb tail; this is the gen~ runtime bound to the arena | |
| `delay` | repeats; **header BPM matters** — hold+turn should change the delay time | |
| `qdelay` | repeats + Clean/Diffuse/Duck character via the `mode` row | |
| `glitch` | 12 algorithms; dual-deck | |
| `reverb` | **the `-/3` fix**: open the action screen, `mode` should read a real position (`1/3`), not `-/3`, before you touch it. Cycling should step 1→2→3 and audibly change algorithm (Dattorro / Zita / Greyhole). | |

`reverb` is the sharpest test of the `config()` reader, and `qdelay`/`delay` also report now.

## Stage 3 — clock and persistence

| Engine | What to check | Result |
|---|---|---|
| `edrums` | four voices on the transport clock; hold+turn sets tempo; clock into GATE IN 2 should lock. Kit presets persist to QSPI **across a power cycle**. | |

## Stage 4 — the loopers (silent until you record)

**These have no audio until something is recorded into a deck**, and the Patch has no buttons — the
action screen is the *only* way to reach `rec`. If you skip that step they look broken.

| Engine | What to check | Result |
|---|---|---|
| `granular` | action screen → `rec`, then `play`. Widest surface in the set: sequencer rows, FX rows, deck row should all be present. | |
| `graincloud` | as granular, but the GrainflowLib cloud | |

Both report `-/3` on their config rows — expected, they are two of the four engines that do not
implement `config()`.

## Stage 5 — MIDI voices

| Engine | What to check | Result |
|---|---|---|
| `reso` | plays from MIDI note-on; `mode` row reports a real position | |
| `mosc` | **BOOT_QSPI build** — different app type. If `program-dfu` misbehaves, that is the first suspect, not the engine. 24 models via the Aux selector (hold encoder + turn). | |

## Stage 6 — the SD streaming set

Card must be in **before power-on**. If a deck stays silent, check the `diag` SD page first.

| Engine | What to check | Result |
|---|---|---|
| `radio` | tunes across the station bank; `route` row should report position **2/3** (it boots at DoubleMono) | |
| `tape` | plays the tapes; records a take to the card; `route` reports 2/3 | |
| `shuttle` | varispeed both directions; `route` reports 2/3 | |
| `softcut` | loads loops, overdubs; `route` reports 2/3 | |
| `pstretch` | time-smear on the clip | |
| `bard` | **boots paused by design** — use the action screen to start it. Writes a resume file; check it resumes after a power cycle. | |

The `route` positions above are the behaviour fix, not just a display one: those four boot at
`Route::DoubleMono`, and before `config()` the first click on that row silently moved them to Stereo.

## Reporting back

For anything that fails, the useful minimum is:

1. Engine and stage.
2. What you expected vs what happened.
3. Whether `diag` was clean on the same board.
4. Anything on the OLED — the header line especially (engine name, deck, page count).

"No audio" and "audio but wrong" are very different diagnoses; so are "the screen is blank" and "the
screen is fine but nothing responds".
