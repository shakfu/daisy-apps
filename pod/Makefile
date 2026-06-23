# Standalone Csound-on-Daisy proof of life. Builds a BOOT_QSPI app (code in QSPI
# flash, heap in SDRAM) linked against the cross-compiled libcsound.a. This is
# deliberately separate from the sk-engines SRAM firmware/Makefile.
#
#   make            - build the harness (CsoundEngine behind IEngine)
#   make program-dfu- flash over USB DFU (catch the v5.4 bootloader's DFU window)
#
# NOTE: program-dfu/program-swd flash only the app. `make program-boot` flashes the
# stock Daisy v5.4 bootloader (dsy_bootloader_v5_4.bin, from the fetched Csound Daisy
# port) and overwrites whatever bootloader is currently on the board.

APP_TYPE = BOOT_QSPI
LDFLAGS += -u _printf_float

# Compile the FatFs sources (ff.c + the ccsbcs.c code-page file) into the app so the SD patch bank
# (sd_stream_deck.h -> csound/*.csd) links. The vendored libdaisy.a ships ff.o with ff_convert /
# ff_wtoupper left undefined - they live in option/ccsbcs.c, which libDaisy expects the app to add.
USE_FATFS = 1

# The sk-engines contract headers (common.h: std::clamp) need C++17; libDaisy defaults to gnu++14.
CPP_STANDARD = -std=gnu++17

# Target board for the control/UI abstraction (src/board/board.h). Default Pod; override on the
# command line: `make BOARD=patch_init` / `make BOARD=patch`. The board only changes a -DTARGET_*
# define - all three targets are STM32H750, so the QSPI linker script + bootloader are unchanged.
BOARD ?= pod
ifeq ($(BOARD),pod)
C_DEFS += -DTARGET_POD
else ifeq ($(BOARD),patch_init)
C_DEFS += -DTARGET_PATCH_INIT
else ifeq ($(BOARD),patch)
C_DEFS += -DTARGET_PATCH
else
$(error unknown BOARD '$(BOARD)' - use one of: pod | patch_init | patch)
endif

TARGET      = harness
CPP_SOURCES = harness_csound.cpp ../src/engine/csound/csound_engine.cpp

# Daisy libraries (vendored in the sk-engines repo)
LIBDAISY_DIR = ../libs/libDaisy
DAISYSP_DIR  = ../libs/DaisySP

# Cross-compiled Csound static library + headers (built via thirdparty/csound)
CSOUND_BASE        = ../thirdparty/csound/Daisy
CSOUND_INCLUDE_DIR = $(CSOUND_BASE)/include/csound
CSOUND_LIB_DIR     = $(CSOUND_BASE)/lib
CSOUND_STATIC_LIB  = $(CSOUND_LIB_DIR)/libcsound.a

C_INCLUDES += -I../src                 # sk-engines contract headers (engine/iengine.h etc.)
C_INCLUDES += -I$(CSOUND_INCLUDE_DIR)
LIBS       += $(CSOUND_STATIC_LIB)
LIBDIR     += -L$(CSOUND_LIB_DIR)

# Daisy v5.4 bootloader + the SDRAM-heap QSPI linker script from the Csound port
BOOT_BIN = $(CSOUND_BASE)/DaisyCsoundExamples/dsy_bootloader_v5_4.bin
LDSCRIPT = $(CSOUND_BASE)/DaisyCsoundExamples/STM32H750IB_qspi_custom.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
