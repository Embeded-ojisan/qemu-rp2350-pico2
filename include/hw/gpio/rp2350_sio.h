/*
 * RP2350 single-cycle IO (SIO), core 0 view: CPUID, GPIO and spinlocks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RP2350_SIO_H
#define HW_GPIO_RP2350_SIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RP2350_SIO "rp2350-sio"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350SIOState, RP2350_SIO)

/* GPIO0..47 */
#define RP2350_SIO_NUM_GPIO 48

struct RP2350SIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t gpio_out[2];   /* [0]: GPIO0..31, [1]: GPIO_HI */
    uint32_t gpio_oe[2];
    uint32_t spinlock_st;
    qemu_irq gpio_out_irq[RP2350_SIO_NUM_GPIO];
};

#endif
