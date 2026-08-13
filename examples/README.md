# SD card content

Two kinds of engine in this repo read from a card, and they are provisioned differently.

**Patch banks** — `csound/` and `chuck/` — are small text files, committed here, and documented in
their own READMEs.

**Audio content** for the six streaming engines is *generated*, not committed:

```
make sd-content                        # -> examples/sd/  (~20 MB, gitignored)
make sd-card SD=/Volumes/DAISY         # generate if needed, then copy everything to the card
make sd-card SD=/Volumes/DAISY ENGINES='radio bard'
make sd-verify SD_OUT=/Volumes/DAISY   # check a card against the format rules
```

`scripts/make_sd_content.py` synthesizes tones, sweeps, noise and blips — deliberately distinguishable
by ear, so you can tell which station or slot you landed on — each written in the exact format its
engine's reader accepts. Regenerating is deterministic; there is nothing to download.

## The two WAV formats

This is the thing to know before putting your own material on a card. The streaming service reads WAV
through two different paths that accept **different and incompatible** formats. A file in the wrong one
is refused at play time with no other symptom.

| Path | Engines | Format |
|---|---|---|
| `start_play` → `WavStreamReader` | `tape`, `shuttle`, `softcut` | mono, **32-bit IEEE float**, sample rate **exactly 48000** |
| `start_play_wav` → `RawStreamReader` | `radio`, `pstretch`, `bard` | mono, **16-bit PCM**, any sample rate (the engine resamples) |

A headerless `.raw` file is the same 16-bit mono body with no header at all; its rate comes from the
engine's config file (`radio/rate.txt`, `bard/bard.cfg`) rather than from the file.

So a stock 16-bit mono WAV works for radio, pstretch and bard, and is **refused** by tape, shuttle and
softcut — which want float32 because that is what the tape engine itself records.

Two further rules the bank scanner enforces:

- **8.3 names.** It stores 13 bytes per name and skips anything longer, so a long filename simply never
  appears in the engine.
- **32 KB minimum.** Smaller files are dropped (the filter exists to skip macOS `._*` metadata stubs).

`make sd-verify` checks all of this against a tree or a mounted card.

## Layouts

```
radio/rate.txt              sample rate of the headerless .raw stations, e.g. "48000"
radio/<bank>/<station>      bank is a single digit; stations are .raw or .wav
pstretch/<clip>             flat directory of source clips
bard/bard.cfg               resume=on|off, rate=<hz>
bard/<shelf>/<book>         shelf is a single digit; books are .raw or .wav
bard/<shelf>/<BOOK>.TXT     optional sidecar text, ignored by the audio scan
bard/resume.txt             WRITTEN BY THE ENGINE - playback positions; do not hand-seed
tapes/tape_<a|b>_<1..8>.wav   float32; normally recorded by the tape engine itself
shuttle/tape_<a|b>_<1..8>.wav float32; deck A's two tracks load slots 1 and 2
softcut/loop_<a|b>_<1..8>.wav float32 loops
```

The generated tree seeds one or two slots per deck for the recording engines, so playback works before
you have recorded anything. Everything else is yours to fill.
