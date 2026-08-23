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

# Host (native) test suite for the platform layer: the lock-free ring and its stream pumps, the WAV
# chunk walk, the transport's tick grid, and the paged parameter UI's pickup and action rows. None of
# it needs hardware - it is arithmetic and state machines that happen to ship on a Cortex-M7 - and all
# of it is the kind of code that is obviously right and subtly wrong.
#
# Runs under ASan + UBSan by default (host/Makefile), which is what turns "reads one chunk past a
# truncated file" from a silent field bug into a failing test.
#
#   make test                 build + run every suite
#   make test SANITIZE=       without the sanitizers
.PHONY: test
test: check-docs
	$(MAKE) -C host

# Cheap consistency check: no source comment or README may cite a docs/*.md that neither exists nor is
# catalogued as a known sk-engines absence in docs/dev/README.md. Part of `make test` because it is
# instant and because a dead link is a defect a reader pays for, not the author.
.PHONY: check-docs
check-docs:
	@scripts/check_docs.sh

.PHONY: test-clean
test-clean:
	$(MAKE) -C host clean

# Provision a mounted FAT32 SD card for every engine that reads one.
#
#   make sd-card SD=/Volumes/DAISY                       # everything
#   make sd-card SD=/Volumes/DAISY ENGINES=csound        # just the csound patch bank
#   make sd-card SD=/Volumes/DAISY ENGINES='radio bard'  # just those audio trees
#
# Two kinds of content. The csound/chuck PATCH BANKS are committed text slots under examples/. The
# AUDIO content for the streaming engines (radio, tape, shuttle, pstretch, softcut, bard) is generated
# on demand into examples/sd/ by `make sd-content`, because ~20 MB of synthesized audio does not
# belong in git - provision_sd.sh generates it automatically if it is missing.
ENGINES ?=
.PHONY: sd-card
sd-card:
	@test -n "$(SD)" || { echo "usage: make sd-card SD=/Volumes/<card> [ENGINES='radio bard']"; exit 1; }
	scripts/provision_sd.sh $(SD) $(ENGINES)

# Generate (or regenerate) the streaming engines' test audio into examples/sd/. Synthesized tones,
# sweeps, noise and blips, each written in the exact format its engine's reader accepts - which is not
# uniform: tape/shuttle/softcut need mono float32 @48k, while radio/bard/pstretch need mono 16-bit PCM.
# See scripts/make_sd_content.py.
.PHONY: sd-content
sd-content:
	$(REL_PY) scripts/make_sd_content.py $(if $(SD_OUT),--out $(SD_OUT),)

# Check a content tree - or a real card - against those format rules. A file in the wrong one of the
# two WAV formats is simply refused by the engine at play time with no other symptom, so this is worth
# running after putting your own material on a card.
#   make sd-verify                      # examples/sd/
#   make sd-verify SD_OUT=/Volumes/DAISY
.PHONY: sd-verify
sd-verify:
	$(REL_PY) scripts/make_sd_content.py --verify $(if $(SD_OUT),--out $(SD_OUT),)

# Upload an already-built dist/<version>/ as a GitHub release (requires `gh auth login`). Tag the
# release with the SAME bare version passed to `make dist VERSION=x` so names line up.
.PHONY: gh-release
gh-release:
	@test -n "$(VERSION)" || { echo "usage: make gh-release VERSION=0.1.0 (after make dist VERSION=0.1.0)"; exit 1; }
	@test -d dist/$(VERSION) || { echo "dist/$(VERSION) not found - run 'make dist VERSION=$(VERSION)' first"; exit 1; }
	gh release create $(VERSION) dist/$(VERSION)/* \
	  --title "daisy-apps $(VERSION)" \
	  --notes-file dist/$(VERSION)/RELEASE_NOTES.md
