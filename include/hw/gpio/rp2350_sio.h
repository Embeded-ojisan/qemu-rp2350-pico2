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

/* GPIO register groups and write operations, in GPIO coprocessor order */
enum {
    RP2350_SIO_GPIO_OUT,
    RP2350_SIO_GPIO_OE,
    RP2350_SIO_GPIO_IN,
};

enum {
    RP2350_SIO_OP_WRITE,
    RP2350_SIO_OP_XOR,
    RP2350_SIO_OP_SET,
    RP2350_SIO_OP_CLR,
};

struct RP2350SIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t gpio_out[2];   /* [0]: GPIO0..31, [1]: GPIO_HI */
    uint32_t gpio_oe[2];
    uint32_t spinlock_st[2]; /* [0]: Secure, [1]: Non-secure */
    uint32_t gpio_nsmask[2]; /* ACCESSCTRL GPIO_NSMASK0/1 */
    qemu_irq gpio_out_irq[RP2350_SIO_NUM_GPIO];
};

/*
 * GPIO register access for the GPIO coprocessor port (datasheet 9.9).
 * @group is RP2350_SIO_GPIO_*, @bank 0 for GPIO0..31 and 1 for GPIO_HI,
 * @op is RP2350_SIO_OP_*. Only OUT and OE can be written. @nonsecure
 * selects the view filtered by the GPIO assignment.
 */
uint32_t rp2350_sio_gpio_read(RP2350SIOState *s, unsigned group,
                              unsigned bank, bool nonsecure);
void rp2350_sio_gpio_write(RP2350SIOState *s, unsigned group, unsigned bank,
                           unsigned op, uint32_t value, bool nonsecure);

/* ACCESSCTRL GPIO_NSMASK0/1. The caller enforces register write permissions. */
void rp2350_sio_gpio_set_nsmask(RP2350SIOState *s, unsigned bank,
                               uint32_t mask);

#endif
