/*
 * RP2350 stub ROM for QEMU: shared definitions
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RP2350_STUB_ROM_H
#define RP2350_STUB_ROM_H

#define ROM_SIZE                0x8000      /* 32 kB, datasheet 4.1 */
#define SRAM_END                0x20082000  /* datasheet 2.2.3 */

/*
 * Log ports at the top of the ROM. Writes to the ROM have no effect on
 * the chip; QEMU's raspi-pico2 turns them into trace events
 * (rp2350_stub_rom_*). Keep in sync with hw/arm/rp2350_soc.c.
 */
#define STUB_LOG_BLOCK          (ROM_SIZE - 0x10) /* IMAGE_DEF block, 0: none */
#define STUB_LOG_BOOT           (ROM_SIZE - 0x0c) /* vector table entered */
#define STUB_LOG_CALL           (ROM_SIZE - 0x08) /* implemented ROM func */
#define STUB_LOG_UNIMPL         (ROM_SIZE - 0x04) /* unimplemented ROM func */

/* pico-sdk bootrom_constants.h */
#define RT_FLAG_FUNC_ARM_SEC    0x0004
#define BOOTROM_ERROR_INVALID_STATE (-12)
#define BOOTROM_ERROR_BUFFER_TOO_SMALL (-13)
#define BOOTROM_ERROR_BAD_ALIGNMENT (-11)
#define BOOTROM_ERROR_INVALID_ARG (-5)
#define BOOTROM_ERROR_NOT_PERMITTED (-4)
#define BOOTROM_ERROR_UNSUPPORTED_MODIFICATION (-18)

#define BOOTRAM_BASE            0x400e0000
#define BOOTRAM_FLASH_DEVINFO   (BOOTRAM_BASE + 0x3dc)
#define BOOTRAM_RANDOM          (BOOTRAM_BASE + 0x3e0)

#endif
