#!/usr/bin/env bash
# Copy an engine from an sk-engines checkout into this repo.
#
#   scripts/port_engine.sh <engine> [path-to-sk-engines]
#   scripts/port_engine.sh delay ~/projects/sk-engines
#
# The port is mechanical because daisy-apps carries the SAME IEngine contract as upstream (see
# docs/dev/porting-sk-engines.md): the engine's whole source tree, including any vendored thirdparty/
# it ships, is copied verbatim with one substitution - `namespace spotykach` -> `namespace daisyapps`.
# Nothing else in an engine is repo-specific.
#
# What this does NOT do, because it cannot be inferred from the source tree:
#   - add the engine's block to app/Makefile (sources, include roots, defines, OPT)
#   - add it to src/engine/engine_select.h
#   - copy any src/dsp/ helper it includes (the script reports which ones are missing)
# Both are a few lines; the guide's section 4.4 has the shapes.

set -euo pipefail

ENGINE="${1:-}"
SK="${2:-$HOME/projects/sk-engines}"
DA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ -z "$ENGINE" ]; then
    echo "usage: $(basename "$0") <engine> [path-to-sk-engines]" >&2
    exit 1
fi

SRC="$SK/src/engine/$ENGINE"
DST="$DA/src/engine/$ENGINE"

if [ ! -d "$SRC" ]; then
    echo "error: no such engine: $SRC" >&2
    echo "available:" >&2
    ls "$SK/src/engine" 2>/dev/null | sed 's/^/  /' >&2 || true
    exit 1
fi

if [ -e "$DST" ]; then
    echo "error: $DST already exists - remove it first if you mean to re-port" >&2
    exit 1
fi

echo "porting $ENGINE: $SRC -> $DST"
mkdir -p "$DST"

# Copy the tree, renaming the namespace in every text source. Binary/other files (Faust .dsp sources,
# .json manifests, LICENSE/NOTICE files) are copied byte-for-byte.
( cd "$SRC" && find . -type f -print0 ) | while IFS= read -r -d '' f; do
    mkdir -p "$DST/$(dirname "$f")"
    case "$f" in
        *.h|*.hpp|*.c|*.cc|*.cpp|*.inc)
            sed 's/\bspotykach\b/daisyapps/g' "$SRC/$f" > "$DST/$f"
            ;;
        *)
            cp "$SRC/$f" "$DST/$f"
            ;;
    esac
done

files=$(find "$DST" -type f | wc -l | tr -d ' ')
echo "  copied $files files"

# Report cross-tree includes the engine needs, so the caller knows what else to bring over. Anything
# already present in src/ is fine; anything missing has to be copied or the build will not resolve it.
echo "  cross-tree dependencies:"
grep -rhoE '#include[[:space:]]+"[^"]+"' "$DST" 2>/dev/null \
  | sed -E 's/#include[[:space:]]+"//; s/"//' \
  | grep -vE "^(engine/$ENGINE/|\./)" \
  | sort -u \
  | while read -r inc; do
        # In-directory includes resolve without a path; only worry about ones that look like src/ paths.
        case "$inc" in
            */*) ;;
            *) continue ;;
        esac
        if [ -e "$DA/src/$inc" ]; then
            printf '    ok      %s\n' "$inc"
        else
            printf '    MISSING %s\n' "$inc"
        fi
    done

echo
echo "next: add the engine to src/engine/engine_select.h and a block to app/Makefile,"
echo "      then: cd app && make ENGINE=$ENGINE BOARD=patch"
