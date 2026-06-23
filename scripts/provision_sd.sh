#!/usr/bin/env bash
# Copy the example patch banks onto a mounted SD card so the Pod harnesses can load them.
#
# The Csound / ChucK harnesses read numbered slots from an engine-named folder at the card root:
#   <card>/csound/0.csd .. 7.csd   (loaded by the csound harness)
#   <card>/chuck/0.ck   .. 7.ck    (loaded by the chuck harness)
# This copies examples/<engine>/<n>.<ext> into those folders, keeping the numbered names and skipping
# each folder's README.md (the bank only reads numbered slots). Existing same-named files are
# overwritten; other files on the card are left untouched.
#
# Usage:
#   scripts/provision_sd.sh <card-path> [engine ...]
#     <card-path>   mount point of the FAT32 SD card (e.g. /Volumes/DAISY)
#     engine        one or more of: csound chuck   (default: both)
#
# Examples:
#   scripts/provision_sd.sh /Volumes/DAISY                 # both banks
#   scripts/provision_sd.sh /Volumes/DAISY csound          # just the csound bank
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# File extension each engine's slots use.
ext_for() {
  case "$1" in
    csound) echo "csd" ;;
    chuck)  echo "ck" ;;
    *) echo "ERROR: unknown engine '$1' (use: csound chuck)" >&2; return 1 ;;
  esac
}

if [ "$#" -lt 1 ]; then
  echo "usage: scripts/provision_sd.sh <card-path> [engine ...]   (engines: csound chuck)" >&2
  exit 1
fi

CARD="$1"; shift
ENGINES=("$@")
[ "${#ENGINES[@]}" -eq 0 ] && ENGINES=(csound chuck)

if [ ! -d "$CARD" ]; then
  echo "ERROR: card path '$CARD' is not a directory (is the SD card mounted?)" >&2
  exit 1
fi

total=0
for engine in "${ENGINES[@]}"; do
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
