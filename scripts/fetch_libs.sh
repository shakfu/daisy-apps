#!/usr/bin/env bash
# Fetch and build the Daisy ecosystem libraries the harnesses link: libs/libDaisy and libs/DaisySP.
#
#   scripts/fetch_libs.sh          # clone (if absent) + build both
#   scripts/fetch_libs.sh --clean  # rebuild from scratch
#
# libs/ is gitignored and reproduced on demand, the same arrangement the csound/chuck dependencies use
# (scripts/fetch_csound.sh / fetch_chuck.sh) - nothing large is vendored in this repo.
#
# libDaisy needs ITS submodules (the STM32 HAL under Drivers/) or its Makefile stops at the first .o
# with "No rule to make target". A plain `git clone` without --recursive leaves exactly that state.

set -euo pipefail

DA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS="$DA/libs"
CLEAN=""

if [ "${1:-}" = "--clean" ]; then CLEAN=1; fi

mkdir -p "$LIBS"

fetch() {
    local name="$1" url="$2"
    if [ -d "$LIBS/$name/.git" ]; then
        echo "== $name already cloned"
    else
        echo "== cloning $name"
        git clone --depth 1 "$url" "$LIBS/$name"
    fi
    # Depth-1 submodules: the HAL drivers are large and no history is needed to build them.
    ( cd "$LIBS/$name" && git submodule update --init --recursive --depth 1 )
}

fetch libDaisy https://github.com/electro-smith/libDaisy.git
fetch DaisySP  https://github.com/electro-smith/DaisySP.git

for name in libDaisy DaisySP; do
    echo "== building $name"
    if [ -n "$CLEAN" ]; then make -C "$LIBS/$name" clean >/dev/null 2>&1 || true; fi
    make -C "$LIBS/$name" -j"$(nproc 2>/dev/null || echo 4)"
done

echo
echo "built:"
ls -la "$LIBS/libDaisy/build/libdaisy.a" "$LIBS/DaisySP/build/libdaisysp.a"
