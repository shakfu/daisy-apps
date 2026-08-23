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
#
# NOT PINNED - worth knowing before it bites. Both clones below are `--depth 1` of the DEFAULT BRANCH,
# so this script fetches whatever is at the tip on the day it runs. Consequences:
#
#   * The build is not reproducible across time. `make dist` records the app's own git SHA in
#     MANIFEST.txt but nothing about which libDaisy a binary was linked against, so two artifacts with
#     the same version string can differ.
#   * CI can go red with no commit on this side, which is why .github/workflows/ci.yml runs weekly and
#     writes the resolved revisions into each run's summary - so a drift failure is dated and
#     attributable rather than mysterious.
#
# Known-good as of 2026-08-23 (the full engine x board matrix builds clean against these):
#     libDaisy  cc146d5
#     DaisySP   599511b
#
# To pin, add a `git -C "$LIBS/$name" fetch --depth 1 origin <sha> && git -C ... checkout FETCH_HEAD`
# after the clone. That is a policy decision - pinning trades "picks up upstream fixes" for "builds the
# same thing next year" - so it is left to a deliberate choice rather than made here.

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
