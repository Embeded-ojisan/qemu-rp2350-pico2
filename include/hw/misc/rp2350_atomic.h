/*
 * RP2350 atomic register access aliases (datasheet 2.1.3)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2350_ATOMIC_H
#define HW_MISC_RP2350_ATOMIC_H

#include "system/memory.h"

/* Each peripheral register block owns 4 kB; the four aliases span 16 kB. */
#define RP2350_ATOMIC_BLOCK_SIZE    0x1000
#define RP2350_ATOMIC_ALIAS_SIZE    (4 * RP2350_ATOMIC_BLOCK_SIZE)

/*
 * Wraps a 4 kB register block so that it is seen through the four RP2350
 * access aliases:
 *   +0x0000 normal access
 *   +0x1000 atomic XOR on write
 *   +0x2000 atomic bitmask set on write
 *   +0x3000 atomic bitmask clear on write
 * The atomic writes are done as a read-modify-write on the target block,
 * which is what the bus interposer does for UART, SPI and I2C. Reads from
 * any alias are passed through as normal reads.
 */
typedef struct RP2350AtomicAlias {
    MemoryRegion mr;
    MemoryRegion *target;
} RP2350AtomicAlias;

void rp2350_atomic_alias_init(RP2350AtomicAlias *s, Object *owner,
                              const char *name, MemoryRegion *target);

#endif
