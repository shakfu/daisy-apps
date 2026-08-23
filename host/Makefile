# Host (native) test build for the platform layer.
#
# Everything under test here is pure logic that happens to ship on a Cortex-M7: the lock-free ring and
# its stream pumps, the WAV chunk walk, the transport's tick grid, and the paged parameter UI's pickup
# and action-row generation. None of it needs hardware, and all of it is the kind of arithmetic that
# is obviously right and subtly wrong.
#
# This is possible because the engine contract is HAL-free again (src/math_util.h - see the note in
# src/engine/color.h): `#include "engine/iengine.h"` compiles under a plain host g++ with only the
# standard library, which is what app/param_ui.h needs to be testable at all.
#
#   make            build + run every suite
#   make compile    build only, do not run
#   make clean
#
# Run from the repo root as `make test`.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wshadow -fno-omit-frame-pointer

# SPK_TERMINAL gates IEngine's self-description group (live_params / live_configs / param_label /
# ParamMask), which is what the paged UI builds its pages from - the app/ firmware defines it for
# exactly the same reason. See the note in app/Makefile.
CPPFLAGS += -DSPK_TERMINAL=1 -I. -I../src -I..

# Address + UB sanitizers by default: these tests exist to catch the class of bug the review found by
# reading (out-of-bounds reads in chunk walks, unsigned overflow in ring arithmetic), and a sanitizer
# catches that class automatically. Disable with `make SANITIZE=`.
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += $(SANITIZE)

BUILD := build
SUITES := test_spsc_ring test_wav test_transport test_param_ui
BINS   := $(addprefix $(BUILD)/,$(SUITES))

# The output directory is created by the compile recipe itself ($(@D)), NOT by a target named after it.
# A `$(BUILD):` rule would collide with the phony target that builds the binaries if the two ever share
# a name - make then resolves the order-only prerequisite to the phony one, reports "Circular build <-
# build/test_wav dependency dropped", skips the mkdir, and the link fails with "cannot open output
# file". That only shows up on a CLEAN checkout, because an already-present build/ hides it.
.PHONY: all compile run clean
all: run

compile: $(BINS)

$(BUILD)/%: %.cpp check.h fakes.h
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

# Run every suite, and fail the target if any one of them fails. `set -e` is not enough inside a
# for-loop, so the status is accumulated explicitly.
run: $(BINS)
	@fail=0; \
	for b in $(BINS); do \
	  echo "== $$b"; \
	  ./$$b || fail=1; \
	  echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "all suites passed"

clean:
	rm -rf $(BUILD)
