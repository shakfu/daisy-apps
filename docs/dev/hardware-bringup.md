# Hardware bring-up — Daisy Patch

A running record, not a finished document. Fill in the results as you go; a failure here is worth more
written down than remembered.

**Nothing in `app/` has ever run on a device, and neither has the Patch board driver.** That is the
whole point of this exercise, and it is also why the ORDER below matters more than the coverage: each
stage adds exactly one new unknown, so a failure names its own cause. Testing engines in a random
order would leave every symptom ambiguous between the engine, the harness and the driver.

## Status (Daisy Patch, session of 2026-08-23/24)

First hardware run of anything under `app/`. Flashed over DFU from the dev machine (`dfu-util` to QSPI
`0x90040000`). **11 of 21 builds confirmed, 1 parked, 9 untested.**

### Confirmed working

| Build | Notes |
|---|---|
| `diag` | **all seven pages OK.** Encoder counts **one per detent** despite the unthrottled main loop, and the **SDRAM check passes** - the two blockers, both clear. |
| `passthrough` | |
| `chorus` | first Faust-generated engine |
| `filter` | dual mono: IN1 -> deck A -> OUT1, IN2 -> deck B -> OUT2, no interaction and **no routing switch** (its wrapper returns `live_configs() == 0` deliberately) |
| `reverb` | first engine to allocate from the SDRAM arena |
| `delay` | **after Bug 1 fix** |
| `gigaverb` | **after Bug 2 fix**; gen~ runtime on the arena |
| `voice` | instrument, makes sound with no input |
| `qdelay` | had Bug 1 identically; fixed by the same `ParamUI` change, which is evidence the fix generalises |
| `glitch` | 12 algorithms via the Aux selector; prompted the knob-gesture change below |
| `edrums` | four voices on the transport; drum swap is the action screen's `alt` row |

### PARKED - needs a fresh look

**`granular`** - records and plays according to its own state, but produces **no audible output**.

What is known, so nobody re-derives it:

- `REC` and `PLY` appear in the header as expected, so the `rec`/`play` rows reach the engine, the deck
  arms, the start condition fires and playback runs. The transport chain
  (`Core::_on_transport_tick` -> `deck.tick(e.tick, e.key)`) was traced and is correct.
- It boots at `Mode::None`, where `_set_buf_armed()` is a no-op, so `rec` could never start until a
  mode was set. Fixed - see Bug 3.
- `Mix` and `Feedback` were misreported as 0 (real values 0.5 and 0.95). Fixed - see Bug 4. Confirmed
  on hardware: the rows now read **50** and **95**.
- Despite all of the above, still silent. So the fault is **downstream of the param cache**, in the
  audio path or in another unseeded global.

Next things to look at, in order:

1. **`Crossfade`** - in `live_params`, never seeded, so `param()` reports 0 and the same catch-and-write
   mechanism that killed `mix` applies. Deliberately not seeded this session because no getter for the
   engine's real value was found, and guessing a default would have risked a fourth instance of the
   same bug. Find or add the getter, then seed it.
2. **`Route`** - what Core's default route actually is, and whether that topology reaches OUT 1/2.
3. **The remaining 9 unseeded live params** (`Env`, `EnvSize`, `Win`, `PolySlice`, `Speed`, `ModAmp`,
   `ClickMix`, `PanSpeed`, `PanRange`, `Crossfade`). `init()` seeds 8 of 19. Any of them could be
   misreporting; `Speed` is the one most likely to silence playback.
4. Whether the deck buffer actually captures input - `REC` proves the state machine, not the data path.

### Untested

`graincloud`, `reso`, `mosc`, and the six SD engines (`radio`, `tape`, `shuttle`, `pstretch`,
`softcut`, `bard`).

Notes for when they are picked up:

- **`graincloud`** shares granular's tree and had the identical Bug 3 and Bug 4, fixed alongside it. It
  will very likely show the same silence, so it is probably not worth flashing until `granular` is
  understood.
- **`mosc`** is the only `BOOT_QSPI` build. If `program-dfu` misbehaves there, that is the first
  suspect, not the engine.
- **The six SD engines** need the card in **before power-on**. `make sd-card SD=/path` - the tree is
  generated and `make sd-verify` passes.
- **`radio`, `tape`, `shuttle`, `softcut`** boot at `Route::DoubleMono`, i.e. selector position 2.
  Their `route` row should read **2/3**, and that is a behaviour fix, not just a display one: before
  `config()` the first click on that row silently moved them to Stereo.
- **`bard`** boots paused by design; use the action screen to start it. It is the only engine that
  writes a file to the card (its resume table), so it is worth power-cycling to check the resume.

### Not verified even on the "working" engines

Reported as "works" means the engine made its noise. These were **not** separately confirmed and are
still host-tested only:

- boot pickup priming (params must not snap to knob positions at power-on)
- the header tempo readout
- the OLED engine page (last page in the rotation)
- the action-screen cursor rule
- `config()` showing a real position instead of `-/3`

## Bugs found on hardware

Four, all found within minutes of flashing, none visible to the host suite. Three share a root cause:
**`IEngine::param()` not telling the truth about engine state.** That is worse than it sounds in a
system with soft-takeover, because pickup's whole job is to trust that value and write it - so a
read-back bug becomes a write bug.

The reason none showed up in 144 mutation-verified host cases is the same each time: every test picked
a **valid, mid-range, initialised** value. The logic was right for the states written down; every real
defect lived in a state that had not been imagined - at the boundary, past it, or uninitialised.

**Bug 1 - `delay`/`qdelay`: `tone` and `division` maxed and unresponsive.** `init()` boots both at 1.0
on purpose. Soft-takeover catches by CROSSING the value, and crossing needs `knob >= value` - which an
ADC essentially never satisfies at exactly 1.0. Only the 0.02 proximity window remained, so both knobs
were dead across 98% of their travel. *Pre-existing, not introduced by this session's pickup work.*
Fixed with `kEndCatchWindow` (0.10, endpoint values only) plus a direction marker on uncaught rows:
`>` turn up, `<` turn down, replacing a bare `.` that said only "not live".

**Bug 2 - `gigaverb`: `pos` reported ~12 and its knob was dead permanently.** The gen~ export defaults
`revtime` to 11 while its setter clamps to [0.1, 1] and it advertises that range, so the normalized
read was 12.1. A value above 1 cannot be caught at all - the knob tops out at 1.0. Fixed at both
layers: the engine's `get_param` honours the contract's 0..1, and `ParamUI` enforces the range rather
than trusting it.

*Still open, and a manifest question:* `revtime` boots at 11 but clamps to 1, so the reverb starts with
an ~11 s tail that collapses to <=1 s on first touch, with no way back. Either the declared range is
wrong (gigaverb's revtime is conventionally in SECONDS) or the default is stale. Not changed here - it
alters how the engine sounds at boot.

**Bug 3 - `granular`/`graincloud`: would not record.** A deck constructs at `Mode::None`, and
`_set_buf_armed()` has an empty `case Mode::None: break;` - so `rec` armed a deck that could never
start, and the engine has no audio until it records. Inert from boot with nothing indicating why.
Fixed by implementing `config()` for both engines (reading Route/ModType/Mode back from Core) and
teaching the action screen's first-visit cursor to land on a switch the engine reports as **unset**.
That refines the earlier "never land on a config" rule rather than contradicting it: clicking a switch
with no position cannot lose anything, while clicking one that has a position would change it blind.

**Bug 4 - `granular`/`graincloud`: `Mix` and `Feedback` misreported as 0.** `init()` seeds 8 of 19 live
params; the deck's real defaults are `_in_out_mix = 0.5` and `_feedback = kDefaultFeedback` (0.95).
Because pickup catches AT the reported value, the first touch of the mix knob wrote 0 and killed the
deck's output. Fixed by seeding both from the deck itself (two new const getters) rather than copying
literals, so there is one source of truth.

## Changes made from bench feedback

- **The Aux selector now follows KNOB 1** while the encoder is held, not the encoder's rotation. That
  is upstream's Alt+PITCH gesture and what the rest of this repo already does (`pod/README.md`:
  "turn knob 1 while holding"); `app/harness.cpp` was the outlier. Knob 1 is lent for the duration
  (`ParamUI::set_knob_suspended`) so it is not also writing its own param, and it re-catches on
  release. The encoder nudge is kept as a fine adjustment.
- **The header shows the engine's transport state** - `REC` / `ARM` / `QUE` / `PLY` from
  `IEngine::play_leds()` - taking priority over the tempo. Without it `granular` was undiagnosable:
  silent by design, drawing no panel, on a board with no LEDs, so "nothing happens" could equally be a
  pad that never arrived, a deck waiting on a start condition, or a recording of silence. This is what
  turned the granular investigation from guesswork into measurement.

## Process notes for next time

- **Verify the write before asking for a result.** `grep "File downloaded successfully"` in the flash
  log. A test cycle was wasted reporting on an image that had never been written.
- **One flash loop at a time.** `program-dfu` writes whatever `.bin` is in the build directory *when it
  fires*, not when the loop started, so overlapping loops make "which binary is on the board"
  ambiguous exactly when it matters.
- **"The engine works" is not the same as "the checklist passed."** Keep them separate in this file.

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
