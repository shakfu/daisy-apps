# The engine host (`app/`)

One harness, any ported engine, any of the three boards. The engine is chosen at build time; the
control surface is generated from what the engine declares about itself, so hosting a newly ported
engine costs a Makefile block and no code here.

```
cd app
make ENGINE=delay BOARD=patch      # -> build-delay-patch/harness_delay.bin
make all-engines BOARD=patch       # every engine, one after another
make list-engines
```

`BOARD` defaults to `patch`, `ENGINE` to `passthrough`. Each `(engine, board)` pair builds into its own
`build-<engine>-<board>/`, so switching engines does not need the `rm -rf build` that the single-engine
`pod/` harnesses require.

This is separate from `pod/harness_csound.cpp` / `pod/harness_chuck.cpp`, which predate it: those two
engines each need an externally cross-built static library and their own QSPI link, so they keep their
own harnesses and Makefiles.

## Engines

| `ENGINE=` | What it is | Notes |
|---|---|---|
| `passthrough` | stereo passthrough + level meter | no arena, no clock; the "is it alive" build |
| `delay` | tempo-synced stereo delay | reads the transport BPM; delay lines from the arena |
| `qdelay` | delay with a Clean/Diffuse/Duck palette | as above, plus the allpass diffuser |
| `glitch` | dual-deck lo-fi/circuit-bent noise voice | 12 algorithms, self-contained |
| `reverb` | Dattorro plate / Zita hall / Greyhole | Faust-generated; built `-Os`; voice picked by Mode |
| `chorus` | Faust chorus | header-only engine |
| `filter` | Faust dual filter | header-only engine |
| `voice` | Faust instrument -> filter chain | header-only engine |
| `gigaverb` | gen~-translated Gigaverb | genlib runtime bound to the arena |
| `radio` | RadioMusic-style station player | streams a bank of `.raw`/`.wav` stations |
| `tape` | dual-deck tape, plays and records | records arbitrarily long takes to the card |
| `shuttle` | bipolar/reverse varispeed tape | 4 in-SDRAM tracks; card is load/save only |
| `pstretch` | real-time PaulStretch time-smear | vendored FFT; own linker script (see below) |
| `softcut` | dual-deck overdub looper | vendors monome's softcut-lib |
| `bard` | long-form spoken-word player | WSOLA, bookmarks, writes a resume file |
| `reso` | Mutable Instruments **Rings** | resonator / plucked string; **plays from MIDI** |
| `mosc` | Mutable Instruments **Plaits** | 24-model macro-oscillator; **plays from MIDI**; QSPI build |
| `granular` | the original Spotykach looper | widest IEngine surface in the set; MIDI, gate, V/Oct |
| `graincloud` | granular compiled with GrainflowLib | same tree, `-DSPK_GRAIN_GF`; own linker script |
| `edrums` | four-voice Euclidean drum machine | reads the transport; saves kits to QSPI flash |

Six of these stream audio from the SD card (`radio` through `bard`): they set `SPK_USE_STREAM`, which
swaps the lightweight patch-bank deck for the real `StreamDeck` (`src/hw/stream_deck.h`) and adds the
ring memory and the main-loop pump. Every other engine's build is unchanged by their presence.

**`reso`, `mosc`, `granular` and `graincloud` are the ones you can play.** They implement
`handle_midi_note`, `on_gate_trigger` and `cv_voct` — so a keyboard on MIDI IN, a trigger into GATE IN
1, and (with `CV_INPUTS`) a V/Oct patch all do what you expect. Both are `CapAux`, so hold the encoder
and turn to pick the model: Rings' resonator type, Plaits' 24 synthesis models. Their knob labels come
from the engines themselves — `reso` prints *note, damping, position, brightness, structure, dry/wet*.

`mosc` is the one **QSPI** build here: the 24-model Plaits voice is ~266 KB of code, past what the SRAM
app can hold, so it executes from QSPI flash like the csound and chuck harnesses. That is transparent
to flashing (`make ENGINE=mosc program-dfu` as usual) but it does mean the app carries the `SCB->VTOR`
prologue, and QSPI is slower than SRAM if you are chasing CPU headroom.

### edrums and QSPI

`edrums` is the only engine that uses `EngineContext::qspi`: it persists kit presets to the board's
QSPI flash at a 64 KB offset, clear of both the calibration settings at offset 0 and the app image at
0x40000. The harness passes a real handle (`Board::Qspi()`), so saving works — this was the last null
field in `EngineContext`.

Its store needed the one source edit any ported engine has required: upstream uses the bleeptools
libDaisy fork's three-argument `PersistentStorage<T, Slug, Version>`, while stock libDaisy's takes one
template argument, the QSPI handle in its constructor, and a `Save()` with no argument. The block is
rewritten against the stock API in `edrums_engine.cpp`, with the reasoning in a comment there. What is
lost is the fork's slug/version boot guard; `KitData` carries its own version field and `apply()`
falls back to defaults on a mismatch, which is the check that actually matters.

## The diagnostic

Before chasing a silent engine, flash this:

```
make diag BOARD=patch                                  # -> build-diag-patch/diag.bin
while ! make ENGINE=diag program-dfu; do sleep 0.2; done
```

It links no engine at all. Everything it shows comes from the board driver and FatFs directly, because
the questions worth asking first are board-level ones an engine cannot answer. Encoder turns between
pages; a click is the page's action.

| Page | Shows | Click |
|---|---|---|
| ANALOG | every analog input as a live bar plus its raw value | — |
| DIGITAL | encoder increment and press, buttons, gate states, gate edge counters | reset counters |
| MIDI | message count, last message decoded (type, channel, bytes, note name) | — |
| SD | mount status, file count, and a per-file verdict against the format rules | next file |
| AUDIO | callback block counter, input and output peak meters | toggle a 440 Hz test tone |
| OUTPUTS | CV out state, gate out state, the board's CV out count | toggle a CV sweep |
| SYSTEM | sample rate, block size, SDRAM read/write result, control counts, uptime | rescan the card |

The audio callback passes input to output, so a diag build is also a passthrough: if you hear your
input, the codec and the SAI are working. The onboard LED blinks throughout, which is the only signal
on a board with no screen.

The SD page applies the same rules as `make sd-verify`, on the actual card — so "the engine plays
nothing" and "these three files are in the wrong WAV format" stop being the same symptom.

## Panel (Daisy Patch)

| Control | Does |
|---|---|
| CTRL 1-4 | the four parameters of the current page, with value pickup |
| encoder turn | change page |
| encoder click | open the **action screen**, then one click per row (see below) |
| encoder hold + **knob 1** | `CapAux` engine: scroll its Aux selector (model / kit / slot) - upstream's Alt+PITCH gesture, and the same shape `pod/` uses for its patch bank. Knob 1 is lent for the duration and re-catches on release |
| encoder hold + turn | `CapAux` engine: nudge the same selector one step per detent. Otherwise: set the internal tempo |
| GATE IN 1 | trigger the focused deck (`IEngine::on_gate_trigger`) |
| GATE IN 2 | external clock - quarter-note pulses steer the transport tempo |
| MIDI IN | forwarded whole, plus decoded NoteOn and start/stop |
| CV OUT 1/2 | `IEngine::process_cv`, block-rate, bipolar mapped onto the 0-5 V output |
| GATE OUT | `IEngine::gate_out_triggered` on the focused deck |

### The action screen

Everything an engine exposes that is not a knob — the play/record pads and the categorical switches —
lives behind the encoder click. Click once to open the list, turn to move the cursor, click again to
fire the highlighted row; the list stays open, so a repeated action is one click. `back` leaves, and
so does six seconds of not touching anything. Only the encoder changes meaning — the four knobs go on
driving the parameters of the page underneath, so nothing is frozen while the list is open. The cursor is remembered between visits, which puts the
action you use most two clicks away.

| Row | Calls | Present when |
|---|---|---|
| `back` | — | always |
| `play` | `on_play_pad(deck, false)` | the engine implements it |
| `alt` | `on_play_pad(deck, true)` | with `play` — the pad's `reverse` half (bard jumps back 15 s; shuttle and softcut swap track; edrums swaps drum; granular plays backwards) |
| `rec` | `on_record_pad(deck, false)` | the engine implements it |
| `stop`, `clear buf` | `stop_if_generating`, `clear_buffer` | ditto |
| `arm seq`, `trig`, `clr seq`, `disarm` | the `on_seq_*` / `clear_sequence` / `disarm_track` pads | ditto |
| `flux`, `grit` | `set_fx(deck, kind, on)`, toggling | ditto |
| `flux lock`, `grit lock` | `toggle_fx_lock(deck, kind)` | ditto |
| `deck` | switches the focused deck A/B | `CapDualDeck` |
| `mode`, `route`, … | `set_config(id, deck, n)`, cycling | one row per bit in `live_configs()` |

"The engine implements it" is **measured, not declared** — see `app/engine_pads.h`. `IEngine`'s pads
have no-op default bodies and `capabilities()` has no bit for most of them, so the harness asks the
compiler instead: for an inherited member `&Derived::f` has type `R (Base::*)(...)`, and only a class
that declares `f` makes it `R (Derived::*)(...)`. Comparing the two types folds to a constant at build
time, costs nothing at runtime, assumes nothing about vtable layout, and has no declaration to keep in
sync — which matters, because the nearest declarative proxy was wrong: `tape` and `shuttle` implemented
`on_record_pad` while claiming no `CapRecording`.

The result is that a list is exactly as long as the engine is deep. `reverb` and the Faust engines show
`back` and their switches. `delay` adds play and alt. `granular` and `graincloud` show all fourteen pad
rows — their sequencer and FX pads had no route to them on any board before this. What the trait cannot
tell you is whether an implementation does anything useful; a declared no-op still gets a row, which is
a spare line rather than a hidden feature.

The switch rows show their position (`mode 2/3`) because the harness knows what it last wrote — the
contract has no reader for a config. Before a row has been used it reads `-/3`, because the engine is
sitting at whatever default it chose and nothing here can read that back; the first click selects
position 1 and every click after it cycles, so screen and engine agree from first use onward. The **names** are the generic slot names from `param_names.h`,
not the engine's word for the value: `reverb` reads `mode 3/3`, not `greyhole`. Adding
`config_label(ConfigId, int)` to `IEngine` alongside the existing `param_label` would fix that for
every engine at once.

Why this matters more than it sounds: a lot of the engine set is inert without it. `granular` and
`graincloud` have no audio at all until something records into a deck; `tape`, `shuttle` and `softcut`
are recorders that could only play; `bard` boots paused by design, being a resume-where-you-left-off
player; and every `CapDualDeck` engine — 15 of the 20 — had no way to reach `ConfigId::Mode`, which is
`reverb`'s algorithm, `pstretch`'s source, `reso`'s and `mosc`'s voice mode.

Boards with no screen cannot show a list, so on the Pod and patch.init() the click keeps its direct
meaning (switch deck, else cycle `Mode`) and the pads live on the two buttons: **button 0 = play,
button 1 = record**. `reverse` is unreachable there; record is the feature and reverse is a flavour,
so record takes the button.

The top-right of the header shows two characters of state instead of the tempo: `P` or `-` for whether
the SD stream deck is actually playing a file, and a count of play-presses. That distinguishes "the
gesture never arrived" from "the engine chose to stay silent" from "a file is streaming but you cannot
hear it" — which took four flashes to establish the first time, by inference.

### CV inputs

`radio`, `pstretch`, `bard`, `reso`, `mosc`, `granular` and `graincloud` respond to V/Oct,
start-position and level CV (`cv_voct`, `cv_size_pos`, `cv_mix`); `bard` also takes `cv_crossfade`. Whether the harness feeds them depends on
the board, because the hardware differs:

- **patch.init()** — `analog[4..7]` are dedicated CV jacks, so all four are routed by default and
  nothing is given up.
- **Daisy Patch** — knob and jack are summed in analog hardware ahead of the ADC, so there is no spare
  input: a CV input costs a parameter knob. Off by default. Turn it on per build:

  ```
  make ENGINE=radio BOARD=patch CV_INPUTS=2     # CTRL 3+4 become V/Oct and start position
  ```

- **Pod** — no CV inputs.

Inputs are taken from the end of the analog array and assigned in order: V/Oct, start position, level,
crossfade. The engine *sums* them with the corresponding knob, so 0 V is neutral; a normalized reading
is re-centred to bipolar before being sent. They address the focused deck, as the knobs do.

**V/Oct is not calibrated.** Upstream corrects it through a per-unit table built from three measured
reference voltages; this assumes a plain linear +/-5 V jack (`kCvVoltSpan` in `harness.cpp`). Pitch
tracking will be approximate until someone measures a real board and corrects that constant.

On a Pod the screen, CV and gate calls are board no-ops and only two knobs exist, so the same build
runs as a two-knob view of page 1. On patch.init() there is no encoder, so the page stays at 1.

`IEngine::process` is stereo. The Patch is a 4-in/4-out board, so the engine reads IN 1/2 and writes
OUT 1/2, and the harness silences OUT 3/4 each block (libDaisy reuses the same DMA buffers, so leaving
them untouched would emit whatever was last in them).

### Where the pages come from

Nothing here knows a delay from a reverb. Each engine declares which `ParamId`s it actually consumes
(`live_params()`) and what it calls them (`param_label()` - "feedback", "division", "cutoff"), and
`param_ui.h` builds the pages from those two answers, four params to a page, and prints the engine's
own words on the screen.

Both declarations live behind `SPK_TERMINAL` upstream, where they serve the serial `describe` command.
There is no terminal here, but a screen wants exactly the same information, so the app Makefile builds
with `-DSPK_TERMINAL=1` and the terminal's own transport simply does not exist. An engine that narrows
neither keeps the inherited all-live mask and gets all 24 slots paged in enum order - honest ("I did
not say"), if verbose.

### Value pickup

Four knobs standing in for up to 24 parameters means a knob is almost always somewhere other than the
value it now addresses. Writing immediately would jump that parameter on every page turn, so a knob is
*caught* only once it crosses (or already sits within 0.02 of) the engine's current value. Until then
it writes nothing, and the screen marks the row with a leading `.` and puts a caret on the bar at the
knob's physical position - the gap you have to close is shown rather than guessed.

## SD streaming

An engine that plays or records audio from the card never touches the filesystem from the audio ISR.
`StreamDeck` gives each deck a 1 MB power-of-two SDRAM ring (a deck is play-XOR-record, so one ring
serves both) plus a shared 32 KB staging buffer. The ISR only does `play_consume` / `record_produce`
on the rings; the harness's main loop calls `s_stream.process()`, which is where FatFs reads, writes
and the WAV-header finalize actually happen. 1 MB is roughly 5.5 s of mono read-ahead at 48 kHz - a
wide cushion against SD latency spikes.

Both the service and the ring/stream primitives it is built from (`src/memory/spsc_ring.h`,
`audio_stream.h`, `wav_stream.h`, `raw_stream.h`) are ported from sk-engines, where they run on
hardware. The threading contract is the delicate part of streaming, and reimplementing it would have
been strictly worse than taking the version that already works.

All six streaming engines declare `CapAux`, so the encoder hold-and-turn gesture is live and
meaningful on each: radio's station, tape's slot, softcut's loop, bard's bookmark. All six are also
`CapDualDeck`, so the action screen carries a `deck` row that switches which deck the knobs address.

Card layout is per engine (radio scans a bank directory, bard reads and writes a resume file, and so
on) - see each engine's source. `make sd-content` generates test audio for all six in each one's own
layout and format, `make sd-card SD=<path>` copies it to a card alongside the csound and chuck patch
banks, and `make sd-verify` checks a tree or a real card against the format rules.

### Linker note

**Every** BOOT_SRAM build links with `linker/sram_bss_in_axi.lds` instead of libDaisy's stock SRAM
script - it is the Makefile's default, not a per-engine exception. Two reasons. Capacity: the stock
script places `.bss` in the 128 KB DTCMRAM, which `pstretch` overflows by ~172 KB (its FFT working set
is ~291 KB of static data), `bard` by ~10 KB and `graincloud` likewise; the variant moves `.bss` into
the 480 KB AXI SRAM alongside `.text`, where the two are allocated sequentially and need no hand-tuned
boundary. And correctness: the SDMMC DMA cannot reach DTCMRAM at all, so a FatFs buffer landing there
fails every transfer - see section 5.1 of the porting guide, which is where that cost a bring-up.
`pstretch` then links at ~90% of that region - the tightest build in the set.

## Flashing

The default `APP_TYPE = BOOT_SRAM` runs the app from the 480 KB execution SRAM, which needs libDaisy's
bootloader on the board once:

```
make program-boot                                            # hold BOOT, tap RESET, then run this
while ! make ENGINE=delay program-dfu; do sleep 0.2; done     # catch the DFU window, then tap RESET
```

Every engine in the set fits with room to spare - measured against the 480 KB region, `passthrough`
occupies 39% and `reverb`, the largest, 49%. A small engine can also be built without a bootloader at
all, straight into internal flash:

```
make ENGINE=passthrough APP_TYPE=BOOT_NONE program-dfu
```

## Prerequisites

`scripts/fetch_libs.sh` clones and builds `libs/libDaisy` and `libs/DaisySP` (both gitignored and
reproduced on demand). It needs the `arm-none-eabi` toolchain on `PATH`.

## Status

Every engine x board combination in the table compiles. **None of it has been run on hardware** - the
Patch driver (MIDI, OLED, CV/gate out) and this harness are new, and the Pod is the only board this
repo has ever validated on a device.
