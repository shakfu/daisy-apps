# ChucK engine example patches

Reference programs for the **ChucK Pod harness** (`pod/harness_chuck.cpp`). Each is a complete,
self-contained `.ck` program that compiles on the Daisy build and reads the panel knobs. They were
authored for the sk-engines firmware; this README describes how they behave on the slimmer daisy-apps
Pod harness (see the caveats below).

## Use

Flash the ChucK harness (from `pod/`: `make -f Makefile.chuck` then `make -f Makefile.chuck
program-dfu`), then put these on a FAT32 SD card in a `chuck/` folder at the **root of the card**,
keeping the numbered names. The easiest way is the provisioning helper from the repo root:

```
make sd-card SD=/Volumes/<card> ENGINES=chuck
```

or copy them by hand:

```
<card>/chuck/0.ck
<card>/chuck/1.ck
```

Insert the card **before power-on** (the harness mounts once at boot). The engine auto-loads the
lowest-numbered patch ~1 s after boot. To switch live: **hold the encoder and turn knob 1 (PITCH)** to
scroll the bank, then **release** to swap — the engine swaps the patch inside one persistent VM
(compiled the first time it is selected, then cached, so later switches are instant and memory stays
flat). The encoder stands in for the firmware's Alt pad; there is no LED/ring selector readout on the
Pod harness.

## What works on the Pod harness

Every patch here is **self-running** (it makes sound immediately, no MIDI needed), so they all play on
the Pod harness; for a MIDI-playable patch see the MIDI section below. The one limit: the harness drives
only two globals - knob 1 -> `speedA` (PITCH) and knob 2 ->
`mixA` (MIX). The patches below also map `sizeA` (SIZE) to a character parameter, but the 2-knob Pod
does not drive `sizeA`, so it stays at its default (0) and that parameter is fixed:

- `0.ck` — clean two-saw "fifth" drone. Calm baseline, fully knob-controlled (PITCH/MIX).
- `1.ck` — fat detuned super-saw with a sub-octave. Obviously different from `0.ck`, for testing live
  switching.
- `2.ck` — concurrent generative pad: several voice shreds play themselves in polyrhythm. Shows off
  ChucK's strongly-timed concurrency.
- `3.ck` — STK **Rhodey** auto-arpeggio, demonstrating the STK instruments (not just raw oscillators).
- `4.ck` — "a chuck is born" random melody (`SinOsc` -> `JCRev`). PITCH = tempo, MIX = level; SIZE
  (reverb amount) is fixed at 0 here, so it plays dry.
- `5.ck` — karp-o-matic (`StifKarp` -> `JCRev` -> three `Echo`s), the full network-reverb version.
  PITCH = octave, MIX = level; SIZE (echo amount) is fixed. **CPU-heavy** — `JCRev` is costly on this
  MCU; prefer `6.ck` if it strains the audio block.
- `6.ck` — karp-o-matic lightened for the Daisy (`StifKarp` -> two `Echo`s, no `JCRev`). Same controls
  as `5.ck` at a fraction of the CPU.
- `7.ck` — oscillatronx: a texture mixing every oscillator type. PITCH = base note, MIX = level; SIZE
  (FM index / timbre) is fixed, so it runs at its cleanest setting.

## MIDI

The harness receives MIDI. None of the numbered patches above listen for notes, so to try it use the
reference patch `midi.ck` here (not a numbered slot - the bank is full at `0.ck`..`7.ck`). Copy it over
any slot and select it:

```
cp examples/chuck/midi.ck <card>/chuck/3.ck
```

Then play a MIDI keyboard into the Pod's MIDI input - each NoteOn sporks a short enveloped voice; MIX
sets the level. (`make sd-card` does not copy `midi.ck`; it only provisions the numbered slots.)

## Writing your own

MIDI device UGens (`MidiIn`/`MidiOut`) are compiled out of this build, so a patch can't read MIDI the
desktop-ChucK way; instead the host hands notes to the VM through globals. To be MIDI-playable, declare
three globals - `global Event noteOnA; global int notesA[]; global int noteCountA;` (MIDI channel 1 ->
deck A, channel 2 -> the `B` versions) - and wait on the Event. Per audio block the engine fills `notesA`
with the note numbers of the NoteOns that arrived, sets `noteCountA`, then broadcasts `noteOnA` once; a
shred does `noteOnA => now;` then sporks a voice for each of `notesA[0 .. noteCountA-1]` (use
`Std.mtof(note)` for the frequency). Passing the whole batch per block is what makes it **polyphonic** -
a chord (several NoteOns in one block) plays in full. NoteOn-only (no NoteOff/velocity), so each voice is
finite. See `midi.ck` for a complete example.

Knob globals the engine exposes (declare and read whichever you use): `speedA` (PITCH), `mixA` (MIX),
`sizeA` (SIZE), `envA`, `fbA`, `modspA`, `modampA`, and the `B`-deck equivalents. **On the Pod harness
only `speedA` and `mixA` are actually driven** (the two knobs); declare the others if you like, but they
stay at 0 until a richer control surface sets them. The convention across these patches is PITCH =
pitch/tempo, SIZE = brightness/space, MIX = level — keep to it so the knobs feel consistent across
patches.

What is and isn't available (a bare-metal, core-only ChucK build):

- **Available:** the oscillators (`SinOsc`/`SawOsc`/`SqrOsc`/`TriOsc`/`PulseOsc`/`Noise`), filters
  (`LPF`/`HPF`/`BPF`/`ResonZ`/...), envelopes (`ADSR`/`Envelope`), `Gain`, delays, the reverbs
  (`JCRev`/`NRev`/`PRCRev`), the FFT/analysis UANAs, and the **STK instruments** (`Rhodey`, `Wurley`,
  `TubeBell`, `Mandolin`, `Moog`, ...). `Math`/`Std`, sporked shreds, events, and global variables.
- **Not available:** chugins (no dynamic loading), sound-file I/O (`SndBuf`/`WvIn`/`WvOut` — no
  filesystem audio), and MIDI/OSC/HID/serial device UGens (the host owns I/O).

Keep an eye on CPU: every concurrent shred and UGen costs time inside one audio block (there is no
on-device load meter on this harness, so test by ear — `5.ck` is the cautionary case). A UTF-8 BOM and
CRLF line endings are tolerated (the engine normalizes them); an empty or whitespace-only file falls
back to the built-in program.

See [`../../pod/README.md`](../../pod/README.md) for the SD patch bank mechanics.
