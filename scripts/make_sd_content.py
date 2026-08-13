#!/usr/bin/env python3
"""Synthesize SD-card test content for the streaming engines.

    scripts/make_sd_content.py                 # -> examples/sd/
    scripts/make_sd_content.py --out /Volumes/DAISY
    scripts/make_sd_content.py --engines radio bard

Audio is GENERATED rather than vendored, for the same reason libs/ and thirdparty/ are: this repo keeps
large reproducible artifacts out of git. It is also the only way to be sure the content is in the exact
format each engine's reader accepts - which is the subtle part, see below.

stdlib only (as scripts/build_release.py is), so plain python3 suffices.

THE TWO WAV FORMATS
-------------------
The streaming service reads WAV through two different paths, and they accept DIFFERENT formats. A file
that plays fine on one engine is silently refused by another, which is worth knowing before spending an
evening on it:

  A. start_play  -> WavStreamReader  (tape, shuttle, softcut)
     MONO, 32-bit IEEE float (AudioFormat 3), sample rate EXACTLY 48000.
     Anything else is rejected outright: this path does no resampling, and the format matches what the
     tape engine itself records. (A build defining LOFI_INT16 wants 16-bit PCM instead.)

  B. start_play_wav / start_play_raw -> RawStreamReader  (radio, bard, pstretch)
     MONO, 16-bit signed PCM (AudioFormat 1), ANY sample rate - the engine reads the rate from the
     header and resamples. Headerless `.raw` files are the same 16-bit mono body with no header, and
     take their rate from the engine's config file instead (radio/rate.txt, bard/bard.cfg).

So: a stock 16-bit mono WAV works for radio/bard/pstretch and is REFUSED by tape/shuttle/softcut.

Other constraints this script honours:
  - Station/book/clip names must be 8.3 (<= 8 chars + 3 extension). The bank scanner stores 13 bytes
    per name and SKIPS anything longer, so a long filename simply never appears in the engine.
  - A station under 32 KB is dropped by the scanner (it filters out macOS `._*` metadata stubs), so
    every generated file here is comfortably longer.
"""

import argparse
import math
import os
import struct
import sys

SAMPLE_RATE = 48000
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- signal generators ---------------------------------------------------------------------------
# Deliberately distinguishable by ear: when four stations are a fifth apart you can tell which one the
# knob landed on, which a set of identical sine tones would not give you.


def tone(freq, seconds, rate=SAMPLE_RATE, amp=0.4, harmonics=1):
    n = int(seconds * rate)
    for i in range(n):
        t = i / rate
        v = sum(amp / (h + 1) * math.sin(2 * math.pi * freq * (h + 1) * t) for h in range(harmonics))
        # 10 ms raised-cosine edges, so a looping file does not click at the seam
        edge = int(0.01 * rate)
        if i < edge:
            v *= 0.5 - 0.5 * math.cos(math.pi * i / edge)
        elif i > n - edge:
            v *= 0.5 - 0.5 * math.cos(math.pi * (n - i) / edge)
        yield v


def sweep(f0, f1, seconds, rate=SAMPLE_RATE, amp=0.4):
    n = int(seconds * rate)
    for i in range(n):
        t = i / rate
        f = f0 * (f1 / f0) ** (t / seconds)          # exponential sweep
        yield amp * math.sin(2 * math.pi * f * t)


def noise(seconds, rate=SAMPLE_RATE, amp=0.25, seed=1):
    n = int(seconds * rate)
    x = seed & 0xFFFFFFFF
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF                  # xorshift32: deterministic across runs
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        yield amp * ((x / 0xFFFFFFFF) * 2.0 - 1.0)


def pulses(period_s, seconds, freq=880.0, rate=SAMPLE_RATE, amp=0.5):
    """Short blips at a fixed interval - a legible test of speed/stretch changes."""
    n = int(seconds * rate)
    period = int(period_s * rate)
    blip = int(0.05 * rate)
    for i in range(n):
        p = i % period
        if p < blip:
            env = 1.0 - (p / blip)
            yield amp * env * math.sin(2 * math.pi * freq * (p / rate))
        else:
            yield 0.0


# --- writers -------------------------------------------------------------------------------------


def _wav_header(data_bytes, audio_format, bits, channels=1, rate=SAMPLE_RATE):
    """Canonical 44-byte RIFF/fmt/data header - the same layout src/memory/wav.h writes."""
    bytes_per_sample = bits // 8
    block_align = bytes_per_sample * channels
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", data_bytes + 36, b"WAVE",
        b"fmt ", 16, audio_format, channels, rate, rate * block_align, block_align, bits,
        b"data", data_bytes,
    )


def write_wav_f32(path, samples, rate=SAMPLE_RATE):
    """Format A: mono 32-bit float @48k. For tape / shuttle / softcut."""
    body = b"".join(struct.pack("<f", max(-1.0, min(1.0, s))) for s in samples)
    with open(path, "wb") as f:
        f.write(_wav_header(len(body), 3, 32, 1, rate))
        f.write(body)
    return len(body)


def write_wav_i16(path, samples, rate=SAMPLE_RATE):
    """Format B: mono 16-bit PCM, any rate. For radio / bard / pstretch."""
    body = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples)
    with open(path, "wb") as f:
        f.write(_wav_header(len(body), 1, 16, 1, rate))
        f.write(body)
    return len(body)


def write_raw_i16(path, samples):
    """Headerless 16-bit mono body. Rate comes from the engine's config file, not the file."""
    body = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples)
    with open(path, "wb") as f:
        f.write(body)
    return len(body)


def write_text(path, text):
    with open(path, "w") as f:
        f.write(text)
    return len(text)


# --- per-engine content --------------------------------------------------------------------------

NOTES = {"C3": 130.81, "G3": 196.00, "C4": 261.63, "E4": 329.63, "G4": 392.00, "C5": 523.25}


def build_radio(root, log):
    """radio/<bank>/<station> - banks are single digits; rate.txt gives the rate for headerless .raw."""
    for bank, stations in enumerate([
        # bank 0: pitched stations, mixed .raw and .wav so both code paths get exercised
        [("TONE_C3.RAW", lambda: tone(NOTES["C3"], 6, harmonics=3), "raw"),
         ("TONE_G3.RAW", lambda: tone(NOTES["G3"], 6, harmonics=3), "raw"),
         ("TONE_C4.WAV", lambda: tone(NOTES["C4"], 6, harmonics=3), "wav"),
         ("SWEEP.WAV",   lambda: sweep(80, 4000, 8),                "wav")],
        # bank 1: textures
        [("NOISE.RAW",   lambda: noise(6),                          "raw"),
         ("PULSES.RAW",  lambda: pulses(0.5, 8),                    "raw"),
         ("CHORD.WAV",   lambda: tone(NOTES["C4"], 6, harmonics=5), "wav")],
    ]):
        d = os.path.join(root, "radio", str(bank))
        os.makedirs(d, exist_ok=True)
        for name, gen, kind in stations:
            p = os.path.join(d, name)
            n = write_raw_i16(p, gen()) if kind == "raw" else write_wav_i16(p, gen())
            log(f"  radio/{bank}/{name}  {n // 1024} KB")
    # The rate headerless .raw stations were rendered at. A .wav carries its own.
    write_text(os.path.join(root, "radio", "rate.txt"), f"{SAMPLE_RATE}\n")
    log("  radio/rate.txt")


def build_pstretch(root, log):
    """pstretch/<clip> - a flat directory of source clips to time-smear."""
    d = os.path.join(root, "pstretch")
    os.makedirs(d, exist_ok=True)
    for name, gen in [("CHORD.WAV", lambda: tone(NOTES["C4"], 8, harmonics=6)),
                      ("SWEEP.WAV", lambda: sweep(120, 3000, 10)),
                      ("VOICEISH.RAW", lambda: tone(NOTES["G3"], 8, harmonics=8)),
                      ("NOISE.RAW", lambda: noise(8))]:
        p = os.path.join(d, name)
        n = write_raw_i16(p, gen()) if name.endswith(".RAW") else write_wav_i16(p, gen())
        log(f"  pstretch/{name}  {n // 1024} KB")


def build_bard(root, log):
    """bard/<shelf>/<book> plus bard.cfg. resume.txt is WRITTEN BY THE ENGINE - not seeded here."""
    d = os.path.join(root, "bard", "0")
    os.makedirs(d, exist_ok=True)
    for name, gen in [("BOOK1.WAV", lambda: pulses(1.0, 30, freq=NOTES["C4"])),
                      ("BOOK2.RAW", lambda: pulses(1.0, 30, freq=NOTES["G3"]))]:
        p = os.path.join(d, name)
        n = write_raw_i16(p, gen()) if name.endswith(".RAW") else write_wav_i16(p, gen())
        log(f"  bard/0/{name}  {n // 1024} KB")
    # Optional per-book sidecar; the audio scan filters to .raw/.wav so this is never taken for a book.
    write_text(os.path.join(d, "BOOK1.TXT"), "Test book one: one blip per second.\n")
    write_text(os.path.join(root, "bard", "bard.cfg"), f"resume=on\nrate={SAMPLE_RATE}\n")
    log("  bard/bard.cfg, bard/0/BOOK1.TXT")


def build_tape(root, log):
    """tapes/tape_<a|b>_<1..8>.wav - FORMAT A. Normally written by the engine when you record;
    one seeded slot per deck means play works before you have recorded anything."""
    d = os.path.join(root, "tapes")
    os.makedirs(d, exist_ok=True)
    for name, gen in [("tape_a_1.wav", lambda: tone(NOTES["C4"], 5, harmonics=4)),
                      ("tape_b_1.wav", lambda: tone(NOTES["G4"], 5, harmonics=4))]:
        n = write_wav_f32(os.path.join(d, name), gen())
        log(f"  tapes/{name}  {n // 1024} KB (float32)")


def build_shuttle(root, log):
    """shuttle/tape_<a|b>_<1..8>.wav - FORMAT A. Deck A's tracks 0/1 load slots 1/2."""
    d = os.path.join(root, "shuttle")
    os.makedirs(d, exist_ok=True)
    for name, gen in [("tape_a_1.wav", lambda: tone(NOTES["C4"], 5, harmonics=4)),
                      ("tape_a_2.wav", lambda: tone(NOTES["E4"], 5, harmonics=4)),
                      ("tape_b_1.wav", lambda: tone(NOTES["G4"], 5, harmonics=4)),
                      ("tape_b_2.wav", lambda: sweep(200, 2000, 5))]:
        n = write_wav_f32(os.path.join(d, name), gen())
        log(f"  shuttle/{name}  {n // 1024} KB (float32)")


def build_softcut(root, log):
    """softcut/loop_<a|b>_<1..8>.wav - FORMAT A. Loops, so the generators are seamless."""
    d = os.path.join(root, "softcut")
    os.makedirs(d, exist_ok=True)
    for name, gen in [("loop_a_1.wav", lambda: pulses(0.5, 4, freq=NOTES["C5"])),
                      ("loop_b_1.wav", lambda: tone(NOTES["C3"], 4, harmonics=3))]:
        n = write_wav_f32(os.path.join(d, name), gen())
        log(f"  softcut/{name}  {n // 1024} KB (float32)")


BUILDERS = {
    "radio": build_radio,
    "pstretch": build_pstretch,
    "bard": build_bard,
    "tape": build_tape,
    "shuttle": build_shuttle,
    "softcut": build_softcut,
}

# --- verification --------------------------------------------------------------------------------
# Re-reads a content tree and applies the same rules the on-device readers apply. Worth having as a
# check rather than a comment: the two WAV formats are easy to mix up, the failure mode on hardware is
# a file that simply refuses to play with no other symptom, and hand-made content is the normal case
# once someone puts their own material on a card.

MIN_STATION_BYTES = 32 * 1024        # StreamDeck::kMinStationBytes - smaller entries are skipped
FORMAT_A_DIRS = ("tapes", "shuttle", "softcut")     # start_play  -> float32 mono 48k
FORMAT_B_DIRS = ("radio", "pstretch", "bard")       # start_play_wav -> int16 mono, any rate


def _read_fmt(path):
    """Return (audio_format, channels, rate, bits, data_bytes) from a canonical WAV, or None."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
            return None
        fmt = None
        while True:
            ch = f.read(8)
            if len(ch) < 8:
                return None
            cid, size = ch[0:4], struct.unpack("<I", ch[4:8])[0]
            body = f.read(size + (size & 1))
            if cid == b"fmt " and size >= 16:
                af, nch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
                fmt = (af, nch, rate, bits)
            elif cid == b"data":
                return (*fmt, size) if fmt else None


def verify(root, log):
    problems = []

    def bad(path, msg):
        problems.append(f"{os.path.relpath(path, root)}: {msg}")

    for dirpath, _dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        top = rel.split(os.sep)[0]
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            ext = os.path.splitext(name)[1].lower()
            size = os.path.getsize(path)

            if ext in (".wav", ".raw"):
                stem, dot, suffix = name.partition(".")
                if len(stem) > 8 or len(suffix) > 3:
                    bad(path, f"name is not 8.3 - the bank scanner skips it (stem {len(stem)}, ext {len(suffix)})")
                # Bank-scanned directories drop anything under the minimum station size.
                if top in ("radio", "pstretch", "bard") and size < MIN_STATION_BYTES:
                    bad(path, f"{size} bytes is under the {MIN_STATION_BYTES}-byte scanner floor")

            if ext == ".raw":
                if size % 2:
                    bad(path, "odd byte count - a 16-bit mono body must be even")
            elif ext == ".wav":
                info = _read_fmt(path)
                if not info:
                    bad(path, "unreadable or non-canonical WAV (no fmt/data)")
                    continue
                af, nch, rate, bits, _data = info
                if nch != 1:
                    bad(path, f"{nch} channels - every reader requires mono")
                if top in FORMAT_A_DIRS:
                    if af != 3 or bits != 32:
                        bad(path, f"format A dir wants 32-bit float (fmt 3), found fmt {af}/{bits}-bit")
                    if rate != SAMPLE_RATE:
                        bad(path, f"format A dir wants exactly {SAMPLE_RATE} Hz, found {rate}")
                elif top in FORMAT_B_DIRS:
                    if af != 1 or bits != 16:
                        bad(path, f"format B dir wants 16-bit PCM (fmt 1), found fmt {af}/{bits}-bit")

    if problems:
        log(f"FAILED - {len(problems)} problem(s):")
        for p in problems:
            log(f"  {p}")
        return 1
    log("ok - every file matches the format its engine's reader accepts")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(REPO, "examples", "sd"),
                    help="output root (default: examples/sd/; pass a card mount point to write directly)")
    ap.add_argument("--engines", nargs="*", choices=sorted(BUILDERS), default=sorted(BUILDERS),
                    help="which engines to generate content for (default: all)")
    ap.add_argument("--verify", action="store_true",
                    help="do not generate; check an existing tree (or a card) against the format rules")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    def log(msg):
        if not args.quiet:
            print(msg)

    if args.verify:
        if not os.path.isdir(args.out):
            print(f"no such tree: {args.out}", file=sys.stderr)
            return 2
        log(f"verifying {args.out}")
        return verify(args.out, log)

    if not os.path.isdir(args.out):
        os.makedirs(args.out, exist_ok=True)

    log(f"writing SD content to {args.out}")
    for name in args.engines:
        log(f"{name}:")
        BUILDERS[name](args.out, log)

    log("\ndone. Copy the tree to a FAT32 card root, or run `make sd-card SD=/path/to/card`.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
