/*
 * RP2350 simple register blocks: resets, clocks, xosc, PLL, ticks,
 * IO_BANK0, PADS_BANK0 and BOOTRAM.
 *
 * These blocks only keep what the firmware writes, and report status
 * bits (reset done, clock selected, oscillator stable, PLL locked,
 * tick running) as ready at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2350_REGS_H
#define HW_MISC_RP2350_REGS_H

#include "hw/core/sysbus.h"
#include "hw/gpio/rp2350_sio.h"
#include "hw/misc/rp2350_atomic.h"
#include "qom/object.h"

#define TYPE_RP2350_REGS "rp2350-regs"
OBJECT_DECLARE_TYPE(RP2350RegsState, RP2350RegsClass, RP2350_REGS)

#define TYPE_RP2350_RESETS      "rp2350-resets"
#define TYPE_RP2350_CLOCKS      "rp2350-clocks"
#define TYPE_RP2350_XOSC        "rp2350-xosc"
#define TYPE_RP2350_PLL         "rp2350-pll"
#define TYPE_RP2350_TICKS       "rp2350-ticks"
#define TYPE_RP2350_IO_BANK0    "rp2350-io-bank0"
#define TYPE_RP2350_PADS_BANK0  "rp2350-pads-bank0"
#define TYPE_RP2350_BOOTRAM     "rp2350-bootram"

#define RP2350_REGS_NUM_WORDS (RP2350_ATOMIC_BLOCK_SIZE / 4)

struct RP2350RegsState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    struct RP2350SIOState *gpio_sio;
    struct RP2350AccessCtrl *accessctrl;
    struct RP2350PIOState *gpio_pio0;
    RP2350RegsState *gpio_pads;
    RP2350RegsState *io_bank0;
    qemu_irq dma_reset;
    qemu_irq pio0_reset;
    qemu_irq pio_gpio[RP2350_SIO_NUM_GPIO];
    uint64_t pio_levels;
    uint64_t pio_driven;
    uint32_t regs[RP2350_REGS_NUM_WORDS];
};

struct RP2350RegsClass {
    SysBusDeviceClass parent_class;

    /* Register values after reset, as (offset, value) pairs ending in 0, 0 */
    const uint32_t *reset_values;
    /* Optional: bits accessible through a Non-secure transaction. */
    uint32_t (*nonsecure_mask)(RP2350RegsState *s, hwaddr offset);
    /* Optional: compute the value seen by a read of @offset */
    uint32_t (*read)(RP2350RegsState *s, hwaddr offset);
    /* Optional: handle a write; return false to store it as is */
    bool (*write)(RP2350RegsState *s, hwaddr offset, uint32_t value);
};

void rp2350_io_bank0_update(RP2350RegsState *s, uint64_t mask);

#endif
