# The generic engine host: one harness (harness.cpp), any ported engine, any of the three boards.
#
#   make ENGINE=delay BOARD=patch          # -> build/harness_delay.bin
#   make ENGINE=reverb                     # BOARD defaults to patch (this harness's home target)
#   make ENGINE=passthrough APP_TYPE=BOOT_NONE   # no bootloader; internal flash, if it fits in 128K
#   make ENGINE=diag BOARD=patch           # the hardware diagnostic (app/diag.cpp), no engine linked
#   make all-engines                       # build every engine for BOARD, into build/<engine>/
#
# Flashing (BOOT_SRAM, the default) needs libDaisy's bootloader on the board ONCE:
#   make program-boot                      # flash the bootloader (hold BOOT, tap RESET first)
#   while ! make ENGINE=delay program-dfu; do sleep 0.2; done   # then tap RESET
#
# Each engine gets its own build directory, so switching ENGINE does NOT need `rm -rf build` the way
# the single-engine pod/ harnesses do.

# --- Engine selection ------------------------------------------------------------------------
# One -DSPK_ENGINE_* define, mapped to a concrete type by src/engine/engine_select.h. The define
# spelling is upstream's so ported engine sources compile unchanged.
ENGINE ?= passthrough

ifeq ($(ENGINE), diag)
# Not an engine: the hardware diagnostic (app/diag.cpp). It links NO engine and none of the contract,
# because the questions it answers are board-level - did the ADC read, did the card mount, is the codec
# running - and an engine cannot answer any of them. Its sources are set below, not here.
ENGINE_SOURCES =

else ifeq ($(ENGINE), passthrough)
C_DEFS += -DSPK_ENGINE_PASSTHROUGH
# Header-only (src/engine/passthrough/); no engine .cpp to compile.
ENGINE_SOURCES =

else ifeq ($(ENGINE), delay)
C_DEFS += -DSPK_ENGINE_DELAY
# Tempo-synced stereo delay; reads the harness transport for its BPM and takes its delay lines from
# the SDRAM arena.
ENGINE_SOURCES = ../src/engine/delay/delay_engine.cpp

else ifeq ($(ENGINE), qdelay)
C_DEFS += -DSPK_ENGINE_QDELAY
# The delay grammar with a Clean/Diffuse/Duck character palette. Its feedback diffuser is the
# header-only src/dsp/diffuser.h, so the engine .cpp is the only unit to compile.
ENGINE_SOURCES = ../src/engine/qdelay/qdelay_engine.cpp

else ifeq ($(ENGINE), glitch)
C_DEFS += -DSPK_ENGINE_GLITCH
# Dual-deck lo-fi/circuit-bent noise voice: 12 algorithms, self-contained (no SD, no arena - its two
# ~8 KB glitch buffers live in the engine object).
ENGINE_SOURCES = ../src/engine/glitch/glitch_engine.cpp

else ifeq ($(ENGINE), reverb)
C_DEFS += -DSPK_ENGINE_REVERB
# Three Faust-generated voices (Dattorro plate / Zita hall / Greyhole) selected by ConfigId::Mode.
# Greyhole's modulated diffusion + pitch-shifter overflow SRAM_EXEC at -O2 upstream, so build at -Os.
OPT = -Os
ENGINE_SOURCES = ../src/engine/reverb/reverb_engine.cpp

else ifeq ($(ENGINE), chorus)
C_DEFS += -DSPK_ENGINE_CHORUS
# Faust engine generated from chorus.dsp + chorus.json - header-only (the cyfaust kernel plus the
# shared FaustEngine<Traits> wrapper), so there is no engine .cpp.
ENGINE_SOURCES =

else ifeq ($(ENGINE), filter)
C_DEFS += -DSPK_ENGINE_FILTER
ENGINE_SOURCES =

else ifeq ($(ENGINE), voice)
C_DEFS += -DSPK_ENGINE_VOICE
ENGINE_SOURCES =

else ifeq ($(ENGINE), radio)
C_DEFS += -DSPK_ENGINE_RADIO
# RadioMusic-style station player: free-running playheads over a bank of headerless .raw / 16-bit-mono
# .wav files on the card. Pure streaming - it scans a bank directory and plays, never records.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = ../src/engine/radio/radio_engine.cpp

else ifeq ($(ENGINE), tape)
C_DEFS += -DSPK_ENGINE_TAPE
# Dual-deck tape: plays and RECORDS arbitrarily long takes to the card, bypassing the in-SDRAM length
# cap. Its saturation/wow FX are a Faust kernel (faust_kernel_tapefx.h), like the reverb's voices.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = ../src/engine/tape/tape_engine.cpp

else ifeq ($(ENGINE), shuttle)
C_DEFS += -DSPK_ENGINE_SHUTTLE
# Buffer-based bipolar/reverse varispeed tape: four in-SDRAM mono tape buffers (2 decks x 2 tracks).
# RECORD needs only the arena; the streaming service is for loading/saving a tape slot to the card.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = ../src/engine/shuttle/shuttle_engine.cpp

else ifeq ($(ENGINE), pstretch)
C_DEFS += -DSPK_ENGINE_PSTRETCH
# Real-time PaulStretch ambient time-smear. Self-contained DSP (a vendored radix-2 FFT, no CMSIS-DSP);
# per-voice input rings + FFT scratch come from the arena, and the clip source streams from the card.
# Upstream gives this engine a data-favoured linker split (~291 KB of SRAM data, nearly twice the next
# worst engine). It is one of the engines that would overflow the stock script's 128K DTCMRAM .bss
# region; the repo-wide linker script below handles that for everything.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = ../src/engine/pstretch/pstretch_engine.cpp

else ifeq ($(ENGINE), softcut)
C_DEFS += -DSPK_ENGINE_SOFTCUT
# Dual-deck overdub looper over monome's vendored softcut-lib. Loads loops from the card (slot select
# + boot preload), so it opts into the streaming service like tape/radio.
C_DEFS += -DSPK_USE_STREAM
# softcut's ReadWriteHead uses M_PI/M_PI_2, already defined for every build below.
SOFTCUT_TP  = ../src/engine/softcut/vendor
SOFTCUT_INC = -I$(SOFTCUT_TP)/include
ENGINE_SOURCES = ../src/engine/softcut/softcut_engine.cpp $(wildcard $(SOFTCUT_TP)/src/*.cpp)

else ifeq ($(ENGINE), bard)
C_DEFS += -DSPK_ENGINE_BARD
# Long-form spoken-word / audiobook player: WSOLA time-stretch, room tone, bookmarks, and a resume
# table it persists back to the card - the one engine that WRITES a text file (IStreamDeck::write_text)
# and re-seeks a playing file (seek_play).
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = ../src/engine/bard/bard_engine.cpp

else ifeq ($(ENGINE), granular)
C_DEFS += -DSPK_ENGINE_GRANULAR
# The original Spotykach engine: a dual-deck granular looper. The whole DSP is under
# src/engine/granular/ - Core, Buffer, Track, Vox, the FX and the modulators - and it has the widest
# IEngine surface of anything here (recording, sequencer, tape storage, launch quantization, MIDI,
# gate, V/Oct), so more of it is reachable from a big panel than from four knobs.
ENGINE_SOURCES = $(wildcard ../src/engine/granular/*.cpp)

else ifeq ($(ENGINE), graincloud)
C_DEFS += -DSPK_ENGINE_GRAINCLOUD -DSPK_GRAIN_GF=1
# NOT a copy of the granular engine: it IS the granular tree compiled with -DSPK_GRAIN_GF, which
# swaps Generator's per-sample voice array for a per-block GrainflowLib cloud (gf_cloud.*). Every other
# file is the same source compiled twice into two different images, which is why the sources below are
# the granular tree MINUS its IEngine wrapper, plus graincloud's own two files.
#
# Include order matters: graincloud comes FIRST so granular/generator.cpp's guarded
# `#include "gf_cloud.h"` resolves - the file is not in granular/, so the quoted lookup falls through.
GRAINCLOUD_TP  = ../src/engine/graincloud/thirdparty
GRAINCLOUD_INC = -I../src/engine/graincloud -I$(GRAINCLOUD_TP) -I../src/engine/granular
# Granular plus the GrainflowLib templates overflow SRAM_EXEC at -O2 upstream; -Os fits.
OPT = -Os
ENGINE_SOURCES = $(filter-out ../src/engine/granular/granular_engine.cpp, $(wildcard ../src/engine/granular/*.cpp)) \
                 $(wildcard ../src/engine/graincloud/*.cpp)

else ifeq ($(ENGINE), edrums)
C_DEFS += -DSPK_ENGINE_EDRUMS
# Four-voice Euclidean drum machine. Reads the harness transport for its clock, and is the one engine
# that wants EngineContext::qspi - it persists kit presets to the board's QSPI flash. The harness
# passes a real QSPIHandle, so saving works; see the ctx.qspi note in harness.cpp.
ENGINE_SOURCES = ../src/engine/edrums/edrums_engine.cpp

else ifeq ($(ENGINE), reso)
C_DEFS += -DSPK_ENGINE_RESO
# Mutable Instruments RINGS: a resonator / plucked-string voice, vendored under the engine's own
# thirdparty/. Plays from MIDI out of the box - it is one of the two engines here that implement
# handle_midi_note, on_gate_trigger and cv_voct.
# The Rings DSP (~30K of code + tables) overflows SRAM_EXEC at -O2 upstream; -Os fits with room, and
# the M7 at 480 MHz has ample headroom (Rings shipped on a 168 MHz F4).
OPT = -Os
# stmlib's filters use M_PI, already defined for every build below.
RESO_TP  = ../src/engine/reso/thirdparty
RESO_INC = -I$(RESO_TP) $(STMLIB_INC)
ENGINE_SOURCES = ../src/engine/reso/reso_engine.cpp
# Mutable Instruments ships .cc, which stock libDaisy's Makefile does not compile - see the rule at the
# bottom of this file.
ENGINE_CC_SOURCES = \
	$(RESO_TP)/rings/dsp/part.cc $(RESO_TP)/rings/dsp/string.cc \
	$(RESO_TP)/rings/dsp/resonator.cc $(RESO_TP)/rings/dsp/fm_voice.cc \
	$(RESO_TP)/rings/resources.cc \
	$(STMLIB_TP)/dsp/units.cc $(STMLIB_TP)/utils/random.cc $(STMLIB_TP)/dsp/atan.cc

else ifeq ($(ENGINE), mosc)
C_DEFS += -DSPK_ENGINE_MOSC
# Mutable Instruments PLAITS: the full 24-model macro-oscillator, vendored under the engine's own
# thirdparty/. Like reso it implements handle_midi_note / on_gate_trigger / cv_voct.
# Plaits' user_data.h normally pulls the original STM32F37x flash header for on-device patch storage;
# this firmware has none, so stub it (UserData::ptr() -> NULL, the built-in fm_patches_table is used).
C_DEFS += -DPLAITS_USER_DATA_STUB
# The 24-model voice is ~292 KB of .text - well past the 480 KB SRAM once the rest of the app is in -
# so mosc is a QSPI-EXECUTE build, the same shape the csound/chuck harnesses use. -Os as upstream.
OPT = -Os
APP_TYPE = BOOT_QSPI
MOSC_TP  = ../src/engine/mosc/thirdparty
MOSC_INC = -I$(MOSC_TP) $(STMLIB_INC)
ENGINE_SOURCES = ../src/engine/mosc/mosc_engine.cpp
ENGINE_CC_SOURCES = \
	$(MOSC_TP)/plaits/dsp/voice.cc \
	$(wildcard $(MOSC_TP)/plaits/dsp/engine/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/engine2/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/physical_modelling/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/speech/*.cc) \
	$(MOSC_TP)/plaits/dsp/chords/chord_bank.cc \
	$(MOSC_TP)/plaits/dsp/fm/algorithms.cc $(MOSC_TP)/plaits/dsp/fm/dx_units.cc \
	$(MOSC_TP)/plaits/resources.cc \
	$(STMLIB_TP)/dsp/units.cc $(STMLIB_TP)/utils/random.cc $(STMLIB_TP)/dsp/atan.cc

else ifeq ($(ENGINE), gigaverb)
C_DEFS += -DSPK_ENGINE_GIGAVERB
# gen~-translated C++ (Max/gen~ export) plus the genlib runtime bound to the SDRAM arena. The defines
# below are what the gen export expects of its host; GENLIB_NO_JSON drops the state-serialization
# path (nothing here reads it), and the -Wno-unused-* are for generated code we do not edit.
C_DEFS += -DGENLIB_NO_JSON
C_DEFS += -DDAISY_EXT_NAME=gigaverb
C_DEFS += -DGEN_EXPORTED_NAME=gen_exported
C_DEFS += -DGEN_EXPORTED_HEADER=\"gen_exported.h\"
C_DEFS += -DGEN_EXPORTED_CPP=\"gen_exported.cpp\"
C_DEFS += -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter
GEN_DIR = ../src/engine/gigaverb
GEN_INC = -I$(GEN_DIR) -I$(GEN_DIR)/gen -I$(GEN_DIR)/gen/gen_dsp
ENGINE_SOURCES = $(GEN_DIR)/_ext_daisy.cpp ../src/engine/gen/genlib_arena.cpp

else
$(error unknown ENGINE '$(ENGINE)' - use one of: $(ALL_ENGINES))
endif

# The engine name as a string, for the OLED header.
C_DEFS += -DSPK_ENGINE_STR='"$(ENGINE)"'

# The stmlib support library both Mutable Instruments engines (reso, mosc) build against. Shared, so
# it lives outside either engine's own thirdparty/ - the same arrangement upstream uses.
STMLIB_TP  = ../src/engine/common/thirdparty/stmlib
STMLIB_INC = -I../src/engine/common/thirdparty

# --- Board selection -------------------------------------------------------------------------
# Only changes a -DTARGET_* define (src/board/board.h). All three targets are STM32H750, so the
# linker script and bootloader are unchanged across them.
BOARD ?= patch
ifeq ($(BOARD),pod)
C_DEFS += -DTARGET_POD
else ifeq ($(BOARD),patch_init)
C_DEFS += -DTARGET_PATCH_INIT
else ifeq ($(BOARD),patch)
C_DEFS += -DTARGET_PATCH
else
$(error unknown BOARD '$(BOARD)' - use one of: pod | patch_init | patch)
endif

# --- CV inputs -------------------------------------------------------------------------------
# How many analog inputs to spend on the engine's CV surface (cv_voct / cv_size_pos / cv_mix /
# cv_crossfade) instead of on parameters, taken from the end of the board's analog array. Empty means
# "use the harness default", which is 4 on patch.init() (dedicated CV jacks, free) and 0 on the Patch
# and Pod. See the HARNESS_CV_INPUTS comment in harness.cpp for why the boards differ.
#
#   make ENGINE=radio BOARD=patch CV_INPUTS=2     # trade CTRL 3+4 for V/Oct + start position
#
# Set it HERE rather than via C_DEFS on the make line: C_DEFS is accumulated with +=, so assigning it
# from the command line would replace the engine and board defines and fail in a confusing way.
CV_INPUTS ?=
ifneq ($(CV_INPUTS),)
C_DEFS += -DHARNESS_CV_INPUTS=$(CV_INPUTS)
endif

# --- Build settings --------------------------------------------------------------------------
TARGET = $(if $(filter diag,$(ENGINE)),diag,harness_$(ENGINE))

# Per-(engine, board) build directory, so switching ENGINE does NOT silently reuse objects compiled
# with different -DSPK_ENGINE_* / -DTARGET_* defines (the `rm -rf build` hazard the single-engine pod/
# harnesses carry). `override` is required: libDaisy's core Makefile assigns BUILD_DIR
# unconditionally, and an ordinary assignment here would simply be replaced by its one - while an
# assignment placed AFTER the include comes too late for the rules whose prerequisites are expanded
# as the file is read.
#
# The name is FLAT (build-<engine>-<board>, not build/<engine>-<board>) because libDaisy's rule for it
# is a bare `mkdir $@` with no -p, which fails on a nested path. `build-*/` is already gitignored.
#
# CV_INPUTS is part of the name for the same reason ENGINE and BOARD are: it changes a -D define, so
# sharing a directory across values would hand you objects compiled for a different one. Without the
# suffix, `make CV_INPUTS=2` after a default build silently reuses the stale harness.o and the flag
# appears to do nothing.
override BUILD_DIR = build-$(ENGINE)-$(BOARD)$(if $(CV_INPUTS),-cv$(CV_INPUTS),)

# BOOT_SRAM: the app runs from the 480 KB execution SRAM, loaded by libDaisy's bootloader. Every
# engine in the ported set fits, and it leaves internal flash alone. Override on the command line
# (APP_TYPE=BOOT_NONE) for a bootloader-free build of a small engine.
APP_TYPE ?= BOOT_SRAM

# A QSPI-execute app (currently only mosc) has to point the vector table at itself before anything
# else - see the SCB->VTOR prologue in harness.cpp. BOOT_APP alone cannot gate that: libDaisy defines
# it for BOOT_SRAM too.
ifeq ($(APP_TYPE),BOOT_QSPI)
C_DEFS += -DHARNESS_BOOT_QSPI
endif

# --- Linker script ----------------------------------------------------------------------------
# EVERY BOOT_SRAM build uses the .bss-in-AXI-SRAM script, not libDaisy's stock one. Two reasons, and
# the second is not optional:
#
#   1. Capacity - pstretch (~291 KB of static data), bard and graincloud all overflow the stock
#      script's 128 KB DTCMRAM .bss region.
#   2. THE SD CARD DOES NOT WORK OTHERWISE. libDaisy's own note in src/sys/fatfs.h: "The SDMMC1
#      peripheral can only communicate with the AXI SRAM, and the DTCMRAM cannot communicate with the
#      DMA. So the FatFSInterface object should always be located in the AXI SRAM... applications
#      using the electrosmith bootloader will need special consideration." A BOOT_SRAM app is exactly
#      that case: the stock script puts .bss in DTCMRAM, so a statically-allocated FatFSInterface
#      lands in memory the SDMMC DMA cannot reach. The card initialises, then every read returns
#      FR_DISK_ERR - which looks like a broken card rather than a placement bug.
#
# BOOT_QSPI builds (mosc) keep libDaisy's QSPI script, which already places .bss in AXI SRAM.
ifeq ($(APP_TYPE),BOOT_SRAM)
LDSCRIPT ?= ../linker/sram_bss_in_axi.lds
endif

# The contract headers use std::clamp; libDaisy defaults to gnu++14.
CPP_STANDARD = -std=gnu++17

# Engines are ported verbatim from sk-engines, where their param-liveness masks and knob LABELS live
# behind SPK_TERMINAL. daisy-apps has no terminal, but it does have a screen, and those two
# declarations are exactly what the paged OLED UI needs to name and page an engine's params without
# per-engine code - so the flag is ON here and the terminal's own transport (a serial command
# channel) simply does not exist. See app/param_ui.h.
C_DEFS += -DSPK_TERMINAL=1

# stmlib/Faust/gen sources reach for M_PI, which strict -std=gnu++17 does not expose from <cmath> on
# arm-none-eabi. Harmless where unused.
C_DEFS += -DM_PI=3.14159265358979323846 -DM_PI_2=1.57079632679489661923

# The SD streaming service. Both TUs are wrapped in `#if defined(SPK_USE_STREAM)` upstream and compile
# to nothing without it, so they can be listed unconditionally: a non-streaming engine's build is
# byte-identical to one where they were never mentioned.
# text_sink.cpp is the out-of-line half of terminal_io.h's TextSink. It is here because this build
# defines SPK_TERMINAL=1 for the param labels the OLED prints (see above), and a couple of engines
# (radio, tape) format query replies through TextSink. Guarded by SPK_TERMINAL internally, -Os inside.
ifeq ($(ENGINE), diag)
# The diagnostic is standalone: board driver + FatFs, no engine and no contract sources at all.
CPP_SOURCES = diag.cpp
else
CPP_SOURCES = harness.cpp $(ENGINE_SOURCES) ../src/engine/color.cpp ../src/engine/led.ring.cpp \
              ../src/hw/stream_deck.cpp ../src/hw/fat_file.cpp ../src/terminal/text_sink.cpp \
              $(wildcard ../src/dsp/*.cpp)
endif

# Daisy libraries
LIBDAISY_DIR = ../libs/libDaisy
DAISYSP_DIR  = ../libs/DaisySP

C_INCLUDES += -I../src -I.. $(GEN_INC) $(SOFTCUT_INC) $(RESO_INC) $(MOSC_INC) $(GRAINCLOUD_INC)

# FatFs: the harness mounts an SD card for the engines that stream from it (sd_stream_deck.h). The
# vendored libdaisy.a ships ff.o with ff_convert / ff_wtoupper undefined - they live in
# option/ccsbcs.c, which libDaisy expects the app to add.
USE_FATFS = 1

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# --- .cc sources ------------------------------------------------------------------------------
# Stock libDaisy's Makefile compiles .c, .cpp and .s, but not .cc - and Mutable Instruments' code
# (Rings, Plaits, stmlib) is all .cc. Upstream does not hit this because sk-engines builds against a
# libDaisy fork that already handles it; here the rule is added on this side, which keeps libs/ a
# pristine upstream checkout.
#
# It has to come AFTER the include, and the elf needs an extra prerequisite line: the core Makefile's
# elf rule expanded its prerequisites when it was read, so appending to OBJECTS alone would put these
# objects on the link line without ever building them.
ifneq ($(ENGINE_CC_SOURCES),)
CC_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(ENGINE_CC_SOURCES:.cc=.o)))
OBJECTS   += $(CC_OBJECTS)
vpath %.cc $(sort $(dir $(ENGINE_CC_SOURCES)))

# Mirrors the core Makefile's .cpp rule exactly - note CPPFLAGS, not CXXFLAGS: libDaisy compiles C++
# with CPPFLAGS + CPP_STANDARD, and CXXFLAGS is not defined there at all (a rule using it silently gets
# no includes, no defines and no MCU flags).
$(BUILD_DIR)/%.o: %.cc Makefile | $(BUILD_DIR)
	$(CXX) -c $(CPPFLAGS) $(CPP_STANDARD) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.cc=.lst)) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(CC_OBJECTS)
endif

# --- Convenience -------------------------------------------------------------------------------
ALL_ENGINES = passthrough delay qdelay glitch reverb chorus filter voice gigaverb \
              radio tape shuttle pstretch softcut bard reso mosc \
              granular graincloud edrums

.PHONY: all-engines list-engines
all-engines:
	@for e in $(ALL_ENGINES); do \
	  echo "=== $$e ($(BOARD))"; \
	  $(MAKE) --no-print-directory ENGINE=$$e BOARD=$(BOARD) || exit 1; \
	done

list-engines:
	@echo $(ALL_ENGINES)

# The diagnostic is not an engine and is deliberately absent from ALL_ENGINES (so `make all-engines`
# stays a sweep of real firmware), but it should still be one command away.
.PHONY: diag
diag:
	@$(MAKE) --no-print-directory ENGINE=diag BOARD=$(BOARD)
