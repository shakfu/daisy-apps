#!/usr/bin/env bash
# Fetch and build the Daisy ecosystem libraries the harnesses link: libs/libDaisy and libs/DaisySP.
#
#   scripts/fetch_libs.sh          # fetch the pinned revisions (if not already there) + build both
#   scripts/fetch_libs.sh --clean  # rebuild from scratch
#   scripts/fetch_libs.sh --check  # exit 1 unless libs/ is at the pinned revisions (no network, no build)
#
# libs/ is gitignored and reproduced on demand, the same arrangement the csound/chuck dependencies use
# (scripts/fetch_csound.sh / fetch_chuck.sh) - nothing large is vendored in this repo.
#
# libDaisy needs ITS submodules (the STM32 HAL under Drivers/) or its Makefile stops at the first .o
# with "No rule to make target". A plain `git clone` without --recursive leaves exactly that state.
#
# PINNED. Both libraries are fetched at the exact commits below; the submodules follow the revisions
# those commits record. The full engine x board matrix and both pod/ harnesses build clean against this
# pair. To move a pin: change the SHA, run this script (it re-pins an existing checkout and rebuilds
# that library), rebuild every engine on every board, and record the change in CHANGELOG.md.
# `make dist` runs --check, so a release cannot be built against an unpinned libs/.
LIBDAISY_SHA=cc146d5065dd8286078a662e2830bf820c37a612   # 2026-08-11
DAISYSP_SHA=599511b740f8f3a9b8db72a0642aa45b8a23c3a3    # 2025-05-29

set -euo pipefail

DA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS="$DA/libs"
MODE="${1:-}"

head_of() { git -C "$LIBS/$1" rev-parse HEAD 2>/dev/null || echo none; }

if [ "$MODE" = "--check" ]; then
    rc=0
    for pair in "libDaisy $LIBDAISY_SHA" "DaisySP $DAISYSP_SHA"; do
        set -- $pair
        have="$(head_of "$1")"
        if [ "$have" = "$2" ]; then echo "ok   $1 ${2:0:7}"
        else echo "FAIL $1 is ${have:0:7}, pinned ${2:0:7} - run scripts/fetch_libs.sh"; rc=1; fi
    done
    exit $rc
fi

mkdir -p "$LIBS"

# Fetch exactly `sha` into libs/<name>, and add <name> to $changed if the checkout moved (it must then
# build clean). Called as a plain command, not an `if` condition, so set -e still applies inside it.
fetch() {
    local name="$1" url="$2" sha="$3" dir="$LIBS/$1"
    if [ "$(head_of "$name")" = "$sha" ]; then
        echo "== $name at pinned ${sha:0:7}"
        ( cd "$dir" && git submodule update --init --recursive --depth 1 )
        return 0
    fi
    if [ ! -d "$dir/.git" ]; then
        echo "== fetching $name ${sha:0:7}"
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    else
        echo "== re-pinning $name $(head_of "$name" | cut -c1-7) -> ${sha:0:7}"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$sha"
    git -C "$dir" checkout -q --detach FETCH_HEAD
    # Depth-1 submodules: the HAL drivers are large and no history is needed to build them.
    ( cd "$dir" && git submodule update --init --recursive --depth 1 )
    changed="$changed $name"
}

changed=""
fetch libDaisy https://github.com/electro-smith/libDaisy.git "$LIBDAISY_SHA"
fetch DaisySP  https://github.com/electro-smith/DaisySP.git  "$DAISYSP_SHA"

for name in libDaisy DaisySP; do
    echo "== building $name"
    # A re-pinned checkout keeps the old revision's objects; make would relink them. Build clean.
    if [ "$MODE" = "--clean" ] || [[ " $changed " == *" $name "* ]]; then
        make -C "$LIBS/$name" clean >/dev/null 2>&1 || true
    fi
    make -C "$LIBS/$name" -j"$(nproc 2>/dev/null || echo 4)"
done

echo
echo "built:"
ls -la "$LIBS/libDaisy/build/libdaisy.a" "$LIBS/DaisySP/build/libdaisysp.a"
