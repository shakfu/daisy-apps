#!/usr/bin/env python3
"""Build distributable Daisy firmware binaries for download-and-flash users.

daisy-apps ships two firmware families, and `make dist` covers both. A "firmware" is one
(engine, board) pair; every pair is buildable for all three boards (pod, patch_init, patch).

  app/  the engine host - one harness, the engine picked by ENGINE=. Twenty engines plus the
        `diag` hardware diagnostic (APP_ENGINES below). Builds entirely from source in-tree
        with no external dependency, which is why these make up the bulk of the matrix.
  pod/  the two audio-language harnesses, `csound` and `chuck`. Each links a cross-built
        static library that must be fetched first, so each is SKIPPED WITH A NOTICE if its
        library is absent - a fresh checkout can still cut a release.

This script does a clean build of every pair in the matrix, names the artifacts
daisy-<engine>-<board>-<version>.bin, and collects them under dist/<version>/ with SHA-256
checksums, MANIFEST.txt and RELEASE_NOTES.md (the CHANGELOG section for the version followed
by flashing instructions). It lets users without the ARM toolchain (and the cmake /
fetch-script dependencies the audio-language engines need) download a ready-to-flash binary
instead of building one.

`qdelay` and `glitch` incorporate GPLv3 code, so their binaries are a combined work that must
be distributed under GPLv3; MANIFEST.txt carries a per-artifact license column and the release
notes call those two out by name (GPL_ENGINES below).

Unlike the sk-engines release script this is ported from, the daisy-apps harnesses bake no
version banner into the binary, so there is no in-binary provenance check: the version lives
in the dist/<version>/ directory name, MANIFEST.txt, and SHA256SUMS only.

Each Makefile assumes its own directory is the working directory (its paths are relative:
../src, ../libs, ../thirdparty), so builds run via `make -C app` / `make -C pod`. app/ gives
each (engine, board) pair its own build-<engine>-<board>/ directory, so those are already
independent; the two pod/ Makefiles SHARE one pod/build/ with conflicting per-object defines,
so it is removed between pairs.

Usage:
    python scripts/build_release.py [VERSION] [--engines E ...] [--boards B ...] [--hex]

    VERSION     Version used in artifact names and the output directory. Defaults to
                `git describe --tags --always` (a bare tag on a clean tagged checkout, else
                the short commit SHA). Pass it explicitly to override.
    --engines   Engines to build (default: $RELEASE_ENGINES, else every engine above).
    --boards    Boards to build (default: $RELEASE_BOARDS, else pod patch_init patch).
    --hex       Also emit .hex artifacts (for ST-Link / STM32CubeProgrammer). Off by default:
                both documented flash paths (the Daisy Web Programmer and dfu-util) use .bin.

Examples:
    python scripts/build_release.py                        # describe version, full matrix
    python scripts/build_release.py 0.1.0                  # explicit version, full matrix
    python scripts/build_release.py 0.1.0 --engines reverb delay --boards patch
    python scripts/build_release.py 0.1.0 --engines csound --boards pod    # one pair
    python scripts/build_release.py 0.1.0 --hex            # also emit .hex alongside each .bin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The firmwares this repo can ship, in two families that build differently.
#
#   app/  - the engine host: one harness, the engine picked by ENGINE=. Each (engine, board) pair has
#           its own build-<engine>-<board>/ directory, so pairs are already independent and no cleaning
#           between them is needed. Everything here builds from source in-tree, with no external
#           dependency, which is why these make up the default matrix.
#   pod/  - the two audio-language harnesses, which predate app/ and keep their own Makefiles. Each
#           needs a cross-built static library that must be fetched first, and the two SHARE one
#           pod/build/ directory with conflicting per-object defines, so it is removed between pairs.
#
# `license` marks the artifacts that are NOT MIT: qdelay and glitch incorporate GPLv3 code (see the
# NOTICE.md beside each engine), so a binary built from them is a combined work that must be
# distributed under GPLv3. The manifest and release notes call those out by name rather than leaving
# a downloader to discover it.
APP_ENGINES = [
    "passthrough", "delay", "qdelay", "glitch", "reverb", "chorus", "filter", "voice", "gigaverb",
    "radio", "tape", "shuttle", "pstretch", "softcut", "bard",
    "reso", "mosc", "granular", "graincloud", "edrums",
    "diag",          # not an engine: the hardware diagnostic, and the most useful single download
]

GPL_ENGINES = {
    "qdelay": "derives from qdelay (tilr) - see src/engine/qdelay/NOTICE.md",
    "glitch": "derives from Noisferatu (Rob Scape) - see src/engine/glitch/NOTICE.md",
}

# The pod/ harnesses, keyed as engines too so the matrix stays one flat list of (engine, board).
POD_ENGINES = {
    "csound": {
        "make_args": [],
        "bin": "harness.bin",
        "prereq": ("thirdparty/csound/Daisy/lib/libcsound.a", "run scripts/fetch_csound.sh first"),
    },
    "chuck": {
        "make_args": ["-f", "Makefile.chuck"],
        "bin": "harness_chuck.bin",
        "prereq": ("thirdparty/chuck/Daisy/lib/libchuck.a", "run scripts/fetch_chuck.sh first"),
    },
}

ALL_ENGINES = APP_ENGINES + list(POD_ENGINES)


def default_engines() -> list[str]:
    """The default release matrix: every app/ engine, plus the pod/ harnesses.

    The pod entries are filtered out later if their cross-built libraries are absent - see main().
    """
    return list(ALL_ENGINES)


def is_pod_engine(engine: str) -> bool:
    return engine in POD_ENGINES


def app_artifact(engine: str, board: str) -> tuple[str, str]:
    """(build directory, binary name) for an app/ engine. diag names its target `diag`."""
    target = "diag" if engine == "diag" else f"harness_{engine}"
    return f"build-{engine}-{board}", f"{target}.bin"


# The control/UI board targets (src/board/board.h). All three are STM32H750, so they differ only by a
# -DTARGET_* define (the harness Makefiles map BOARD= to it); the QSPI linker script and bootloader are
# unchanged across them. Only `pod` is hardware-validated - see the note in write_manifest / the notes.
BOARDS = ["pod", "patch_init", "patch"]
VALIDATED_BOARDS = {"pod"}

# libDaisy / DaisySP archives the harness link step needs. Unlike the fetched engine libs these build
# offline from the vendored submodules, so the script builds them on demand rather than erroring.
DAISY_ARCHIVES = [
    ("libs/libDaisy", "build/libdaisy.a"),
    ("libs/DaisySP", "build/libdaisysp.a"),
]

# Filename prefix for the distributed artifacts: daisy-<engine>-<board>-<version>.bin.
ARTIFACT_PREFIX = "daisy"

# DFU flash address / PID for a Daisy QSPI app (mirrors the harness Makefiles' BOOT_QSPI app base): the
# app is written to QSPI flash at this address via the df11 DFU PID. Installing a QSPI-capable bootloader
# is a separate, device-level procedure deliberately not documented here.
APP_ADDRESS = "0x90040000"
DFU_PID = "df11"

# Electrosmith's browser-based DFU flasher (WebUSB; Chrome/Edge only). The friendliest path for end
# users: no toolchain, pick the .bin and click Flash. It handles the app address itself.
WEB_PROGRAMMER_URL = "https://flash.daisy.audio/"


def git_output(*args: str) -> str:
    """Return stripped stdout of a git command, or '' if git/command fails."""
    try:
        out = subprocess.run(
            ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True, check=True
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def default_version() -> str:
    return git_output("describe", "--tags", "--always") or "dev"


def is_dirty() -> bool:
    """True if the working tree differs from HEAD in any way that could affect the build.

    `git diff --quiet` alone reports only UNSTAGED changes to tracked files, which is the wrong
    question here: a release built from a tree with staged edits, or with an untracked source file
    that a Makefile wildcard picks up (`$(wildcard ../src/dsp/*.cpp)`,
    `$(wildcard ../src/engine/granular/*.cpp)`), would be stamped clean in MANIFEST.txt. `git status
    --porcelain` covers staged, unstaged and untracked in one shot; it honours .gitignore, so build
    directories and dist/ do not count.
    """
    out = git_output("status", "--porcelain")
    return bool(out)


def run_make(*args: str) -> None:
    """Run make from the repo root, surfacing output only on failure."""
    proc = subprocess.run(["make", *args], cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"make {' '.join(args)} failed (exit {proc.returncode})")


def ensure_daisy_archives() -> None:
    """Build libdaisy.a / libdaisysp.a if missing (offline, from the vendored submodules)."""
    for directory, archive in DAISY_ARCHIVES:
        if not (REPO_ROOT / directory / archive).exists():
            print(f"==> building {archive} (missing)")
            run_make("-C", directory)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_pair(engine: str, board: str, version: str, jobs: int, out_dir: Path,
               emit_hex: bool = False) -> int:
    """Clean-build one (engine, board) pair, copy its artifacts into out_dir, return .bin size.

    The .bin is always produced (both documented flash paths use it); the .hex is copied only when
    emit_hex is set, for users flashing via ST-Link / STM32CubeProgrammer.
    """
    print(f"==> building {engine} / {board}")

    if is_pod_engine(engine):
        spec = POD_ENGINES[engine]
        rel, fix = spec["prereq"]
        if not (REPO_ROOT / rel).exists():
            raise SystemExit(f"ERROR: {engine} needs {rel} - {fix}")
        # The two pod harnesses share one build directory with conflicting per-object defines.
        shutil.rmtree(REPO_ROOT / "pod" / "build", ignore_errors=True)
        run_make("-C", "pod", f"-j{jobs}", f"BOARD={board}", *spec["make_args"])
        built = REPO_ROOT / "pod" / "build"
        built_bin = spec["bin"]
    else:
        build_dir, built_bin = app_artifact(engine, board)
        # Already per-pair, so this is about reproducibility rather than correctness: a release
        # should not be able to pick up an object from an earlier, differently-configured build.
        shutil.rmtree(REPO_ROOT / "app" / build_dir, ignore_errors=True)
        run_make("-C", "app", f"-j{jobs}", f"ENGINE={engine}", f"BOARD={board}")
        built = REPO_ROOT / "app" / build_dir

    base = f"{ARTIFACT_PREFIX}-{engine}-{board}-{version}"
    bin_path = out_dir / f"{base}.bin"
    shutil.copyfile(built / built_bin, bin_path)
    if emit_hex:
        shutil.copyfile(built / built_bin.replace(".bin", ".hex"), out_dir / f"{base}.hex")

    return bin_path.stat().st_size


def write_manifest(path: Path, version: str, dirty: str, git_sha: str,
                   sizes: dict[tuple[str, str], int],
                   skipped: list[tuple[str, str]] | None = None) -> None:
    lines = [
        "daisy-apps firmware release",
        f"version:    {version}{dirty}",
        f"git commit: {git_sha}",
        "note:       these apps require a Daisy bootloader already installed (see RELEASE_NOTES.md)",
        "note:       NOTHING here is hardware-validated except the pod csound/chuck harnesses;",
        "            every app/ engine and the patch / patch_init board drivers compile but are untested",
    ]
    shipped_gpl = sorted({e for (e, _b) in sizes if e in GPL_ENGINES})
    if shipped_gpl:
        lines.append("note:       GPLv3 artifacts in this release: " + ", ".join(shipped_gpl))
    for engine, why in (skipped or []):
        lines.append(f"skipped:    {engine} ({why})")
    lines += ["", f"{'engine':<12} {'board':<12} {'bytes':>12}  {'license':<7}  binary"]

    for (engine, board), size in sizes.items():
        name = f"{ARTIFACT_PREFIX}-{engine}-{board}-{version}.bin"
        lic = "GPLv3" if engine in GPL_ENGINES else "MIT"
        lines.append(f"{engine:<12} {board:<12} {size:>12}  {lic:<7}  {name}")

    if shipped_gpl:
        lines += ["", "GPLv3 artifacts (a combined work with GPLv3 DSP; source must be available):"]
        for e in shipped_gpl:
            lines.append(f"  {e}: {GPL_ENGINES[e]}")
    path.write_text("\n".join(lines) + "\n")


def write_checksums(out_dir: Path) -> None:
    """Write SHA256SUMS over every artifact, in `shasum -a 256 -c`-compatible format."""
    files = sorted(p for p in out_dir.iterdir() if p.suffix in (".bin", ".hex"))
    lines = [f"{sha256(p)}  {p.name}" for p in files]
    (out_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n")


def changelog_section(version: str, changelog: Path | None = None) -> str | None:
    """Return the CHANGELOG.md body under `## [<version>]`, trimmed of blank edges.

    Falls back to `## [Unreleased]` when the version-named heading is absent or empty (e.g. a
    describe-derived version with no matching heading, or the pre-release state where everything
    still lives under Unreleased). Returns None when neither is found. `==` heading compare, so no
    regex pitfalls.
    """
    changelog = changelog or (REPO_ROOT / "CHANGELOG.md")
    if not changelog.exists():
        return None
    text = changelog.read_text()

    def extract(name: str) -> str | None:
        body: list[str] = []
        in_section = False
        for line in text.splitlines():
            if line == f"## [{name}]":
                in_section = True
                continue
            if in_section and line.startswith("## ["):
                break
            if in_section:
                body.append(line)
        return "\n".join(body) if in_section else None

    section = extract(version)
    if not (section and section.strip()):
        section = extract("Unreleased")
    if not (section and section.strip()):
        return None
    lines = section.splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def flashing_section(version: str, engines: list[str], boards: list[str]) -> str:
    """The `## Flashing ...` section of the release notes (markdown)."""
    untested = [b for b in boards if b not in VALIDATED_BOARDS]
    untested_note = ("""
### Note on board targets

Only the `pod` binaries are validated on hardware. The {names} build{plural} are provided for
convenience but {have} not been tested on a device.
""".format(
        names=" and ".join(f"`{b}`" for b in untested),
        plural="s" if len(untested) > 1 else "",
        have="have" if len(untested) > 1 else "has",
    ) if untested else "")

    return f"""## Flashing a daisy-apps firmware ({version})

Each `.bin` here is a complete app for one engine on one board, named
`{ARTIFACT_PREFIX}-<engine>-<board>-{version}.bin`. Flash exactly one at a time.

### Prerequisite

These are QSPI apps (they execute in place from QSPI flash because the language runtimes are too
big for SRAM), loaded at the standard Daisy app base. They are not standalone: a QSPI-capable Daisy
bootloader must already be installed on the device. Installing the bootloader is a separate,
device-level procedure not covered here.

### Step 1: enter bootloader mode

Put the device in its bootloader's DFU mode first. On the Daisy Pod: hold BOOT, tap RESET, release
BOOT to drop into the bootloader's DFU window (the LEDs indicate DFU). See pod/README.md for details.

### Step 2, option A: Daisy Web Programmer (easiest)

Needs a WebUSB browser - Chrome or Edge (Firefox and Safari will not work).

1. With the device in bootloader mode, open {WEB_PROGRAMMER_URL}
2. On the "File Upload" tab, choose your binary ({ARTIFACT_PREFIX}-<engine>-<board>-{version}.bin).
3. Click FLASH.

### Step 2, option B: dfu-util (command line)

    dfu-util -a 0 -s {APP_ADDRESS}:leave -D {ARTIFACT_PREFIX}-<engine>-<board>-{version}.bin -d ,0483:{DFU_PID}

The `:leave` step may print a harmless `Error 74` / "get_status" message at the end - the write has
already succeeded. Ignore it (the Web Programmer does not show this).

### Verify

Confirm the download is intact:  `shasum -a 256 -c SHA256SUMS`
{untested_note}"""


def write_release_notes(path: Path, version: str, engines: list[str], boards: list[str]) -> None:
    """Write RELEASE_NOTES.md: the CHANGELOG section for this version, then the flashing guide."""
    changelog = changelog_section(version)
    if changelog is None:
        sys.stderr.write(
            f"warning: no CHANGELOG section for '{version}' or '[Unreleased]' - "
            "release notes will note the changelog is missing\n"
        )
        changelog = "_No CHANGELOG entry for this release._"
    gpl = sorted(e for e in engines if e in GPL_ENGINES)
    licensing = ""
    if gpl:
        rows = "\n".join(f"- **`{e}`** - {GPL_ENGINES[e]}" for e in gpl)
        licensing = (
            "\n## Licensing\n\n"
            "Most of this project is MIT, but the binaries below incorporate GPLv3 DSP and are "
            "therefore **distributed under GPLv3** as combined works - source must be available to "
            "anyone you give them to:\n\n"
            f"{rows}\n\n"
            "Every other artifact in this release is MIT. See `MANIFEST.txt` for the per-file "
            "license column.\n"
        )
    path.write_text(
        f"## Changes since the last Release\n\n{changelog}\n\n"
        f"{flashing_section(version, engines, boards)}\n{licensing}"
    )


def resolve_list(cli: list[str], env_var: str, default: list[str], kind: str,
                 valid: list[str]) -> list[str]:
    """Pick the CLI list, else the env var (space-separated), else the default; validate membership."""
    chosen = cli or os.environ.get(env_var, "").split() or default
    unknown = [x for x in chosen if x not in valid]
    if unknown:
        raise SystemExit(f"ERROR: unknown {kind}: {' '.join(unknown)} (valid: {' '.join(valid)})")
    return chosen


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build distributable, version-stamped Daisy firmware binaries (engine x board).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "version", nargs="?", default=None,
        help="release version (default: git describe --tags --always)",
    )
    parser.add_argument(
        "--engines", nargs="*", default=[],
        help="engines to build (default: $RELEASE_ENGINES, else: " + " ".join(ALL_ENGINES),
    )
    parser.add_argument(
        "--boards", nargs="*", default=[],
        help="boards to build (default: $RELEASE_BOARDS, else: " + " ".join(BOARDS),
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=int(os.environ.get("JOBS", "8")),
        help="parallel make jobs (default: 8 or $JOBS)",
    )
    parser.add_argument(
        "--hex", action="store_true",
        help="also emit .hex artifacts (for ST-Link / STM32CubeProgrammer; the web flasher and "
             "dfu-util both use .bin, so .hex is omitted by default)",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    version = args.version or default_version()
    explicit = bool(args.engines) or bool(os.environ.get("RELEASE_ENGINES", "").split())
    engines = resolve_list(args.engines, "RELEASE_ENGINES", default_engines(), "engine", ALL_ENGINES)

    # An explicitly requested pod engine with a missing library is a hard error (build_pair raises);
    # one that merely came from the default matrix is skipped with a notice, so `make dist` works out
    # of the box on a checkout that has never run the csound/chuck fetch scripts.
    skipped: list[tuple[str, str]] = []
    if not explicit:
        keep = []
        for e in engines:
            if is_pod_engine(e):
                rel, fix = POD_ENGINES[e]["prereq"]
                if not (REPO_ROOT / rel).exists():
                    skipped.append((e, f"{rel} absent - {fix}"))
                    continue
            keep.append(e)
        engines = keep
    boards = resolve_list(args.boards, "RELEASE_BOARDS", BOARDS, "board", BOARDS)

    git_sha = git_output("rev-parse", "--short", "HEAD") or "unknown"
    dirty = " (dirty tree - not a clean release build)" if is_dirty() else ""
    out_dir = REPO_ROOT / "dist" / version

    print(f"Release version : {version}{dirty}")
    print(f"Git commit      : {git_sha}")
    print(f"Engines         : {' '.join(engines)}")
    for engine, why in skipped:
        print(f"  skipping {engine}: {why}")
    print(f"Boards          : {' '.join(boards)}")
    print(f"Output          : {out_dir.relative_to(REPO_ROOT)}/\n")

    ensure_daisy_archives()

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    sizes: dict[tuple[str, str], int] = {}
    for engine in engines:
        for board in boards:
            sizes[(engine, board)] = build_pair(engine, board, version, args.jobs, out_dir, args.hex)

    write_manifest(out_dir / "MANIFEST.txt", version, dirty, git_sha, sizes, skipped)
    write_release_notes(out_dir / "RELEASE_NOTES.md", version, engines, boards)
    write_checksums(out_dir)

    print(f"\nDone. Artifacts in {out_dir.relative_to(REPO_ROOT)}/")
    for name in sorted(p.name for p in out_dir.iterdir()):
        print(f"  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
