# Host (native) test build for the platform layer.
#
# Everything under test here is pure logic that happens to ship on a Cortex-M7: the lock-free ring and
# its stream pumps, the WAV chunk walk, the transport's tick grid, the paged parameter UI's pickup and
# action-row generation, and the DisplayModel -> indicator-LED projection. None of it needs hardware,
# and all of it is the kind of arithmetic that is obviously right and subtly wrong.
#
# This is possible because the engine contract is HAL-free (src/math_util.h - see the note in
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

# Compiler-generated header dependencies, included at the bottom. Without them a suite is relinked
# only when its own .cpp changes - so editing app/param_ui.h or app/display_adapter.h and re-running
# `make test` silently RE-RUNS THE OLD BINARY and reports a pass. That is worse than having no tests:
# it is a green result that means nothing, and nothing about it looks wrong. (Found exactly that way:
# three deliberate mutations of the display adapter all "passed".) Listing headers by hand does not
# scale and rots silently; letting the compiler enumerate what it actually opened does not.
CPPFLAGS += -MMD -MP

# Address + UB sanitizers by default: these tests exist to catch the class of bug the review found by
# reading (out-of-bounds reads in chunk walks, unsigned overflow in ring arithmetic), and a sanitizer
# catches that class automatically. Disable with `make SANITIZE=`.
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += $(SANITIZE)

BUILD := build
OBJ   := $(BUILD)/obj

SUITES := test_spsc_ring test_wav test_transport test_param_ui test_display_adapter

# Project sources with out-of-line definitions the suites link against. LEDRing is the canvas an engine
# draws its panel on and the display adapter projects from; Color is what its pixels are made of. Both
# are HAL-free, which is the whole point - they compile here with no libDaisy at all.
PROJECT_SRC := ../src/engine/led.ring.cpp ../src/engine/color.cpp

BINS      := $(addprefix $(BUILD)/,$(SUITES))
SUITE_O   := $(addprefix $(OBJ)/,$(addsuffix .o,$(SUITES)))
PROJECT_O := $(addprefix $(OBJ)/,$(notdir $(PROJECT_SRC:.cpp=.o)))

vpath %.cpp $(sort $(dir $(PROJECT_SRC)))

# Compile then link, rather than one command per binary. One translation unit per compile is what makes
# -MMD produce one dependency file PER SOURCE; compiling several inputs in a single command overwrites
# the .d and leaves only the last input's headers recorded.
$(OBJ)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

.SECONDEXPANSION:
$(BUILD)/%: $(OBJ)/$$*.o $(PROJECT_O)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

.PHONY: all compile run clean
all: run

compile: $(BINS)

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

-include $(SUITE_O:.o=.d) $(PROJECT_O:.o=.d)
