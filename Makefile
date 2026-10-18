# drom-triks — Daisy Seed3 drum machine
#
# Targets you'll actually use:
#   make libs        build libDaisy + DaisySP (once, after clone or submodule update)
#   make             build the firmware
#   make program-dfu flash over USB (hold BOOT, tap RESET first)
#   make program-boot flash the Daisy bootloader (once per board — see below)
#   make debug-server start OpenOCD for the ST-Link
#   make gdb         attach GDB to a running debug-server

# Fail with something readable instead of a wall of missing-header errors.
ifeq ($(shell command -v arm-none-eabi-gcc 2>/dev/null),)
$(error arm-none-eabi-gcc not on PATH. See docs/00-toolchain.md §1 — note that the brew `arm-none-eabi-gcc` formula ships without newlib and will not work)
endif

# Which program to build. `devkit` is the instrument on the Seed3 Desktop Dev
# Kit; `bringup` is the Phase 0 checklist program (docs/00-toolchain.md §6);
# `bench` profiles CPU per voice on the H750 (src/bench.cpp).
# Each gets its own TARGET so switching never links a stale object set.
PROGRAM ?= devkit

ifeq ($(PROGRAM),devkit)
TARGET      = drom-triks
CPP_SOURCES = src/main.cpp
else ifeq ($(PROGRAM),bringup)
TARGET      = drom-triks-bringup
CPP_SOURCES = src/bringup.cpp
else ifeq ($(PROGRAM),bench)
TARGET      = drom-triks-bench
CPP_SOURCES = src/bench.cpp
else
$(error Unknown PROGRAM "$(PROGRAM)": use devkit, bringup or bench)
endif

# BOOT_SRAM runs the app from SRAM (480 kB limit) with the image staged in QSPI
# at offset 0x40000. The alternative, BOOT_QSPI, executes in place from QSPI —
# which means a pattern save would be a write to the flash we're executing from,
# and that hard-faults. See docs/02-firmware.md §1.
#
# This requires the Daisy bootloader on the board: `make program-boot` once.
APP_TYPE = BOOT_SRAM

LIBDAISY_DIR = lib/libDaisy
DAISYSP_DIR  = lib/DaisySP

# ReverbSc and Compressor live in DaisySP-LGPL, a separate library under
# LGPL-2.1 (DaisySP proper is MIT). libDaisy's core Makefile knows how to add
# its include path and link -ldaisysp-lgpl; it just has to be asked.
#
# This carries a distribution obligation, not a development one — see
# docs/11-production.md §3 and the note atop src/engine/fx.h.
USE_DAISYSP_LGPL = 1

# The engine is C++17 (inline constexpr, std::atomic<float> members), and the
# core Makefile defaults to gnu++14 — fine for the bring-up stub, which never
# included the engine, and a wall of errors for anything that does.
CPP_STANDARD = -std=gnu++17

# libDaisy's BOOT_SRAM script plus one section, .sram_bss, for data in the free
# AXI SRAM after the app image (the reverb). See the comment in the file.
LDSCRIPT = ld/drom_sram.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# newlib-nano drops %f from printf unless asked, and prints nothing in its
# place: the status text would read " BPM" with no number and no warning.
LDFLAGS += -u _printf_float

# --- Additions beyond the stock Daisy template -------------------------------

# The core Makefile links against prebuilt archives but never builds them.
.PHONY: libs
libs:
	$(MAKE) -C $(LIBDAISY_DIR)
	$(MAKE) -C $(DAISYSP_DIR)
	$(MAKE) -C $(DAISYSP_DIR)/DaisySP-LGPL

.PHONY: libs-clean
libs-clean:
	$(MAKE) -C $(LIBDAISY_DIR) clean
	$(MAKE) -C $(DAISYSP_DIR) clean
	$(MAKE) -C $(DAISYSP_DIR)/DaisySP-LGPL clean

# OpenOCD's stock configs cover the ST-Link + STM32H7 pairing; libDaisy ships none.
OOCD_FLAGS = -f interface/stlink.cfg -f target/stm32h7x.cfg

.PHONY: debug-server
debug-server:
	openocd $(OOCD_FLAGS)

.PHONY: gdb
gdb: $(BUILD_DIR)/$(TARGET).elf
	arm-none-eabi-gdb -x .gdbinit $<

# Flash via ST-Link instead of DFU. Faster than the BOOT/RESET dance, and the
# only option that works without unplugging the debugger. Writes the app image
# to its QSPI staging address, so the bootloader must already be installed.
.PHONY: program-stlink
program-stlink: $(BUILD_DIR)/$(TARGET).bin
	openocd $(OOCD_FLAGS) \
		-c "program $< $(FLASH_ADDRESS) verify reset exit"

# Flash over the Seed's USB without touching the BOOT button: the ST-Link
# resets the board, and the Daisy bootloader then opens its DFU window for
# ~2 s, which dfu-util waits for (-w). Needs the bootloader installed once
# (`make program-boot`) and both the ST-Link and the Seed's USB-C attached.
.PHONY: flash
flash: $(BUILD_DIR)/$(TARGET).bin
	openocd $(OOCD_FLAGS) -c "init; reset run; shutdown"
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $< -d ,0483:$(USBPID)
