/*
 * RP2350 programmable I/O
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RP2350_PIO_H
#define HW_GPIO_RP2350_PIO_H

#include "hw/core/sysbus.h"
#include "qemu/fifo32.h"
#include "qom/object.h"

#define TYPE_RP2350_PIO "rp2350-pio"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350PIOState, RP2350_PIO)

#define RP2350_PIO_SM_COUNT 4
#define RP2350_PIO_IMEM_SIZE 32

typedef struct RP2350PIOSM {
    RP2350PIOState *pio;
    unsigned index;
    QEMUTimer *timer;
    Fifo32 tx;
    uint32_t clkdiv, execctrl, shiftctrl, pinctrl;
    uint32_t osr, x;
    uint16_t forced_insn;
    uint8_t pc, osr_count, delay;
    bool forced_stalled;
    bool waiting_tx;
} RP2350PIOSM;

struct RP2350PIOState {
    SysBusDevice parent_obj;
    qemu_irq tx_dreq[RP2350_PIO_SM_COUNT];

    MemoryRegion iomem;
    Clock *clk;
    struct RP2350RegsState *io_bank0;
    RP2350PIOSM sm[RP2350_PIO_SM_COUNT];
    uint16_t imem[RP2350_PIO_IMEM_SIZE];
    uint32_t padout, padoe, fdebug;
    uint8_t enabled, gpio_base;
    bool held_reset;
};

#endif
