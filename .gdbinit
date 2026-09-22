# Attach to a running `make debug-server`.
target extended-remote :3333
set print pretty on
set mem inaccessible-by-default off

# Under BOOT_SRAM the ELF is linked for SRAM at 0x24000000, so GDB can load
# straight into RAM and skip the bootloader's QSPI staging entirely. This is the
# fast edit-build-break loop; `make program-dfu` is for standalone running.
monitor reset halt
load
