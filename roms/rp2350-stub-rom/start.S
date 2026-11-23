/*
 * RP2350 stub ROM for QEMU: vectors, well-known pointers and ROM table
 *
 * The layout of the first 0x1a bytes and the ROM table format follow
 * what pico-sdk 2.3.1 expects (bootrom_constants.h, bootrom.h
 * rom_func_lookup_inline): a 16-bit pointer to the lookup function at
 * 0x16, called as lookup(code, flags). Each table entry is a 16-bit code,
 * a 16-bit flag set, then one 16-bit pointer per flag bit; a zero code
 * ends the table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "stub.h"

    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .vectors, "ax"
    .global __vectors
__vectors:
    .word SRAM_END              /* initial SP, used by the stub ROM only */
    .word rom_reset             /* Reset */
    .word rom_fault             /* NMI */
    .word rom_fault             /* HardFault */

    /* 0x10: magic and version, as in the A4 bootrom */
    .byte 'M', 'u', 2, 4
    /* 0x14: ROM table, 0x16: lookup_val(), 0x18: lookup_entry() */
    .hword rom_table
    .hword rom_table_lookup_val + 1
    .hword rom_table_lookup_entry + 1

    .text
    .thumb_func
    .global rom_fault
rom_fault:
    b rom_fault

/*
 * Every known ROM function gets a thunk that reports its code to
 * rom_unimplemented(). A C function named rom_<name> overrides the weak
 * alias and is entered instead.
 */
#define ROM_FUNC(name, code)            \
    .weak rom_##name;                   \
    .thumb_set rom_##name, unimpl_##name; \
    .thumb_func;                        \
unimpl_##name:                          \
    movw r0, code;                     \
    b rom_unimplemented;
#include "rom_funcs.h"
#undef ROM_FUNC

    .section .rodata
    .balign 2
    .global rom_table
rom_table:
#define ROM_FUNC(name, code) \
    .hword code, RT_FLAG_FUNC_ARM_SEC, rom_##name + 1;
#include "rom_funcs.h"
#undef ROM_FUNC
    .hword 0x4446, 0x0040, flash_devinfo_ptr /* 'FD', RT_FLAG_DATA */
    .hword 0, 0

    .balign 4
flash_devinfo_ptr:
    .word BOOTRAM_FLASH_DEVINFO
