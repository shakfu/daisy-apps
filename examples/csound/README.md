# Csound engine example patches

Reference orchestras for the **Csound Pod harness** (`pod/harness_csound.cpp`). Each is a full,
correctly-structured CSD that compiles on the Daisy build and reads the panel knobs. They were authored
for the sk-engines firmware; this README describes how they behave on the slimmer daisy-apps Pod
harness (see the caveats below).

## Use

Flash the Csound harness (from `pod/`: `make` then `make program-dfu`), then put these on a FAT32 SD
card in a `csound/` folder at the **root of the card**, keeping the numbered names. The easiest way is
the provisioning helper from the repo root:

```
make sd-card SD=/Volumes/<card> ENGINES=csound
```

or copy them by hand:

```
<card>/csound/0.csd
<card>/csound/1.csd
```

Insert the card **before power-on** (the harness mounts once at boot). The engine auto-loads the
lowest-numbered patch ~1 s after boot. To switch live: **hold the encoder and turn knob 1 (PITCH)** to
scroll the bank, then **release** to recompile into the chosen patch. (The encoder stands in for the
firmware's Alt pad; there is no LED/ring selector readout on the Pod harness.)

## What works on the Pod harness

The Pod harness drives only two control channels - knob 1 -> `speedA` (PITCH) and knob 2 -> `mixA`
(MIX) - though it **does** receive MIDI. So, relative to the full firmware these patches target:

- **MIDI works.** NoteOn plays each patch's `instr MidiNote` (MIDI channel 1 -> deck A, channel 2 ->
  deck B). So the MIDI-driven and "MIDI over the top" behaviors below are live; plug a controller into
  the Pod's MIDI input. (NoteOn-only - notes are fixed-length plucks, no hold-to-sustain.)
- **Only PITCH + MIX are live as knobs.** The other channels these patches read (`sizeA`, `envA`,
  `fbA`, `modspA`, `modampA`) are never driven by the 2-knob Pod, so they sit at their default (0). A
  patch still plays, but its character knobs are fixed - a reverb's decay, a filter's resonance, an
  echo's feedback, etc. stay at minimum.

The patches, and how each fares on this harness:

- `0.csd` — clean two-saw "fifth" chord. A knob-controlled drone (PITCH/MIX), with MIDI plucks over the
  top.

- `1.csd` — fat detuned super-saw with a sub-octave. **Good fit:** another knob-controlled drone, handy
  for testing live switching against `0.csd`.

- `2.csd` — **MIDI-driven, polyphonic**, no drone: silent until you play. Now fully playable on the Pod
  (MIDI is wired) - each NoteOn is a fixed ~0.6 s stab; chords work.

- `3.csd` — resonant "acid" voice (saw -> self-oscillating `moogladder`, LFO-swept cutoff). Plays, but
  FEEDBACK (resonance) and MODSPEED/MODAMP (sweep) are not reachable from the Pod, so it runs at its
  default, fairly tame setting; only PITCH/MIX respond.

- `4.csd` — stereo reverb pad through `reverbsc`. Plays a soft detuned chord, but SIZE (decay) and ENV
  (damping) are fixed at 0, so the reverb is minimal; PITCH/MIX respond.

- `5.csd` — dub echo (a self-plucking source into a feedback delay). The source plays, but SIZE (echo
  time), FEEDBACK (repeats), and MODAMP (tone) are fixed, so you hear it largely dry; PITCH/MIX respond.

- `6.csd` — generative line (`randomh` + `metro`, self-playing). **Good fit:** it drives itself, so it
  sounds without the extra knobs; MODSPEED (rate) and MODAMP (range) are fixed at default, PITCH/MIX
  respond.

All use only core, table-less opcodes (so they build the same way on the Daisy) and pass desktop
`csound --syntax-check-only`.

## Writing your own

Control channels the engine exposes (read with `chnget`): `speedA` (PITCH), `mixA` (MIX), `sizeA`
(SIZE), `envA`, `fbA`, `modspA`, `modampA`, and the `B`-deck equivalents. **On the Pod harness only
`speedA` and `mixA` are actually driven** (the two knobs); the rest are available to patches but stay at
0 unless a richer control surface sets them. Define `instr MidiNote` (p4 = freq Hz) to be
keyboard-playable - the harness routes each NoteOn to it (a fixed ~0.6 s note, since the platform sends
no NoteOff).

Format rules that matter (the on-device CSD parser is line-oriented):

- Each section tag on its **own line** — `<CsScore>` and `</CsScore>` separately, never
  `<CsScore></CsScore>` on one line.
- Core opcodes only — no plugin opcodes, no soundfile I/O (`diskin`/`GEN01`); prefer table-less
  oscillators (`vco2`). Opcodes proven across these examples: `vco2`, `tone`, `moogladder`, `lfo`,
  `reverbsc`, `delayr`/`delayw`/`deltapi`, `randomh`, `metro`, `port`, `limit`, the `*seg`/`expon`
  envelopes, and `tanh`. A UTF-8 BOM and CRLF line endings are tolerated (the engine normalizes them).

See [`../../pod/README.md`](../../pod/README.md) for the SD patch bank mechanics, and the official
orchestras in `thirdparty/csound/Daisy/DaisyCsoundExamples/` for more.
