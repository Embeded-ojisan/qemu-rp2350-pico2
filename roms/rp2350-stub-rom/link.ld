/* RP2350 stub ROM for QEMU. SPDX-License-Identifier: GPL-2.0-or-later */
MEMORY
{
    /* the top 16 bytes hold the log ports (stub.h) */
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 0x8000 - 0x10
}

ENTRY(rom_reset)

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > ROM

    /* the stub ROM has no initialised or zeroed data */
    .data : { *(.data*) *(.bss*) *(COMMON) } > ROM
    ASSERT(SIZEOF(.data) == 0, "stub ROM must not use .data or .bss")

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) }
}
