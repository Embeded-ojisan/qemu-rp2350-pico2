/*
 * RP2350 ACCESSCTRL and system-bus security filters.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2350_ACCESSCTRL_H
#define HW_MISC_RP2350_ACCESSCTRL_H

#include "hw/core/sysbus.h"
#include "hw/gpio/rp2350_sio.h"

#define TYPE_RP2350_ACCESSCTRL "rp2350-accessctrl"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350AccessCtrl, RP2350_ACCESSCTRL)

#define RP2350_ACCESSCTRL_BASE 0x40060000
#define RP2350_ACCESSCTRL_SIZE 0x4000
#define RP2350_ACCESSCTRL_WORDS (0xec / 4)

/* Cortex-M33 core 0 supplies QEMU's default requester ID (zero). */
enum {
    RP2350_CORE0,
    RP2350_CORE1,
    RP2350_DMA_MANAGER,
    RP2350_DEBUGGER,
    RP2350_MANAGER_COUNT,
};

struct RP2350AccessCtrl {
    SysBusDevice parent_obj;

    RP2350SIOState *sio;
    MemoryRegion regs;
    IOMMUMemoryRegion bus;
    MemoryRegion striped_root;
    MemoryRegion striped_io;
    AddressSpace striped_as;
    MemoryRegion denied_io;
    AddressSpace denied_as;
    qemu_irq pio0_dreq_change;
    uint8_t permissions[RP2350_ACCESSCTRL_WORDS];
    uint8_t lock;
    uint8_t force_core_ns;
};

uint32_t rp2350_accessctrl_ns_reset_mask(RP2350AccessCtrl *s);
bool rp2350_accessctrl_pio0_dreq_allowed(RP2350AccessCtrl *s, unsigned level);

#endif
