#!/usr/bin/env bash
# Copy example content onto a mounted SD card for the engines that read from one.
#
# Two kinds of engine want cards, and they are provisioned differently:
#
#   PATCH BANKS (csound, chuck) - numbered text slots, committed under examples/<engine>/:
#     <card>/csound/0.csd .. 7.csd
#     <card>/chuck/0.ck   .. 7.ck
#   Copied here, keeping the numbered names and skipping each folder's README.md.
#
#   AUDIO CONTENT (radio, tape, shuttle, pstretch, softcut, bard) - GENERATED, not committed, by
#   scripts/make_sd_content.py into examples/sd/. This script generates it on demand if absent and
#   copies the per-engine trees across. See that script for the layouts and, importantly, for the two
#   incompatible WAV formats the readers accept.
#
# Existing same-named files are overwritten; other files on the card are left untouched.
#
# Usage:
#   scripts/provision_sd.sh <card-path> [engine ...]
#     <card-path>   mount point of the FAT32 SD card (e.g. /Volumes/DAISY)
#     engine        one or more of: csound chuck radio tape shuttle pstretch softcut bard
#                   (default: all of them)
#
# Examples:
#   scripts/provision_sd.sh /Volumes/DAISY                 # everything
#   scripts/provision_sd.sh /Volumes/DAISY csound          # just the csound patch bank
#   scripts/provision_sd.sh /Volumes/DAISY radio bard      # just those two audio trees
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD_CONTENT="$REPO_ROOT/examples/sd"

BANK_ENGINES=(csound chuck)
AUDIO_ENGINES=(radio tape shuttle pstretch softcut bard)

# Card-root directory each audio engine reads. Mostly the engine name; tape is the exception (its
# paths are "tapes/tape_a_1.wav").
dir_for_audio() {
  case "$1" in
    tape) echo "tapes" ;;
    *)    echo "$1" ;;
  esac
}

is_in() { local n="$1"; shift; for x in "$@"; do [ "$x" = "$n" ] && return 0; done; return 1; }

# File extension each patch-bank engine's slots use.
ext_for() {
  case "$1" in
    csound) echo "csd" ;;
    chuck)  echo "ck" ;;
    *) echo "ERROR: unknown engine '$1'" >&2; return 1 ;;
  esac
}

if [ "$#" -lt 1 ]; then
  echo "usage: scripts/provision_sd.sh <card-path> [engine ...]" >&2
  echo "  engines: ${BANK_ENGINES[*]} ${AUDIO_ENGINES[*]}" >&2
  exit 1
fi

CARD="$1"; shift
ENGINES=("$@")
[ "${#ENGINES[@]}" -eq 0 ] && ENGINES=("${BANK_ENGINES[@]}" "${AUDIO_ENGINES[@]}")

if [ ! -d "$CARD" ]; then
  echo "ERROR: card path '$CARD' is not a directory (is the SD card mounted?)" >&2
  exit 1
fi

# Validate the engine names up front, so a typo fails before anything is written to the card.
for engine in "${ENGINES[@]}"; do
  if ! is_in "$engine" "${BANK_ENGINES[@]}" "${AUDIO_ENGINES[@]}"; then
    echo "ERROR: unknown engine '$engine'" >&2
    echo "  engines: ${BANK_ENGINES[*]} ${AUDIO_ENGINES[*]}" >&2
    exit 1
  fi
done

# Generate the audio content if any audio engine was asked for and the tree is missing. It is
# reproduced on demand rather than committed (~20 MB), like libs/ and thirdparty/.
want_audio=0
for engine in "${ENGINES[@]}"; do
  is_in "$engine" "${AUDIO_ENGINES[@]}" && want_audio=1
done
if [ "$want_audio" = "1" ] && [ ! -d "$SD_CONTENT" ]; then
  echo "generating audio content (examples/sd/ is absent)..."
  python3 "$REPO_ROOT/scripts/make_sd_content.py" --quiet
fi

total=0
for engine in "${ENGINES[@]}"; do
  # --- audio engines: copy the generated per-engine tree wholesale --------------------------------
  if is_in "$engine" "${AUDIO_ENGINES[@]}"; then
    d="$(dir_for_audio "$engine")"
    src="$SD_CONTENT/$d"
    if [ ! -d "$src" ]; then
      echo "ERROR: no generated content for '$engine' at $src" >&2
      echo "  run: python3 scripts/make_sd_content.py" >&2
      exit 1
    fi
    mkdir -p "$CARD/$d"
    cp -Rf "$src/." "$CARD/$d/"
    count=$(find "$src" -type f | wc -l | tr -d ' ')
    echo "$engine: copied $count file(s) -> $CARD/$d/"
    total=$((total + count))
    continue
  fi

  # --- patch-bank engines: numbered slots only ---------------------------------------------------
  e="$(ext_for "$engine")"
  src="$REPO_ROOT/examples/$engine"
  if [ ! -d "$src" ]; then
    echo "ERROR: no examples for '$engine' at $src" >&2
    exit 1
  fi

  dst="$CARD/$engine"
  mkdir -p "$dst"

  count=0
  # Numbered slots only: <n>.<ext>. Skips README.md and any non-numbered file.
  for f in "$src"/[0-9]*."$e"; do
    [ -e "$f" ] || continue                      # no matches -> skip (nullglob-safe)
    base="$(basename "$f")"
    case "$base" in
      *[!0-9].$e) continue ;;                    # name before the dot must be all digits
    esac
    cp -f "$f" "$dst/$base"
    count=$((count + 1))
  done

  echo "$engine: copied $count slot(s) -> $dst/"
  total=$((total + count))
done

echo "done: $total file(s) written to $CARD. Eject the card before removing it."
