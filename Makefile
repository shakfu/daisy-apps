# daisy-apps release packaging.
#
# `make dist` builds every engine x board firmware in one shot and collects version-stamped,
# checksummed binaries under dist/<version>/ for users who want to download-and-flash rather than
# build (no ARM toolchain or fetch-script dependencies needed). scripts/build_release.py does a clean
# build of each (engine, board) pair, names the artifacts daisy-<engine>-<board>-<version>.bin, and
# adds SHA256SUMS, MANIFEST.txt, and RELEASE_NOTES.md (the CHANGELOG section + flashing instructions).
# The script is stdlib-only, so plain python3 suffices; override with REL_PY if needed.
#
#   make dist                                          # describe-derived version, full matrix
#   make dist VERSION=0.1.0                             # explicit version (the bare tag you will create)
#   make dist RELEASE_ENGINES=csound RELEASE_BOARDS=pod # subset (space-separated lists)
#   make dist WITH_HEX=1                                # also emit .hex (ST-Link / STM32CubeProgrammer)
#
# One-time prerequisites (the cross-compiled engine libs the harnesses link):
#   scripts/fetch_csound.sh   -> thirdparty/csound/Daisy/lib/libcsound.a
#   scripts/fetch_chuck.sh    -> thirdparty/chuck/Daisy/lib/libchuck.a
# The libDaisy / DaisySP archives are built on demand by the release script if missing.
REL_PY ?= python3
RELEASE_ENGINES ?=
RELEASE_BOARDS ?=

.PHONY: dist
dist:
	RELEASE_ENGINES="$(RELEASE_ENGINES)" RELEASE_BOARDS="$(RELEASE_BOARDS)" \
	  $(REL_PY) scripts/build_release.py $(VERSION) $(if $(WITH_HEX),--hex,)

# Upload an already-built dist/<version>/ as a GitHub release (requires `gh auth login`). Tag the
# release with the SAME bare version passed to `make dist VERSION=x` so names line up.
.PHONY: gh-release
gh-release:
	@test -n "$(VERSION)" || { echo "usage: make gh-release VERSION=0.1.0 (after make dist VERSION=0.1.0)"; exit 1; }
	@test -d dist/$(VERSION) || { echo "dist/$(VERSION) not found - run 'make dist VERSION=$(VERSION)' first"; exit 1; }
	gh release create $(VERSION) dist/$(VERSION)/* \
	  --title "daisy-apps $(VERSION)" \
	  --notes-file dist/$(VERSION)/RELEASE_NOTES.md
