/*
 * RP2350 QSPI memory interface (QMI): registers and the XIP read path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RP2350_QMI_H
#define HW_SSI_RP2350_QMI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_RP2350_QMI "rp2350-qmi"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350QMIState, RP2350_QMI)

/* Two chip selects, each with a 16 MB window in the XIP address space */
#define RP2350_QMI_NUM_CS       2
#define RP2350_QMI_CS_WINDOW    (16 * MiB)
#define RP2350_QMI_NUM_REGS     (0x54 / 4)

struct RP2350QMIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    /* XIP window: lower 32 MB of the XIP segment (datasheet 2.2.2) */
    MemoryRegion xip;
    /* Flash contents shown as read-only RAM inside @xip, per chip select */
    MemoryRegion xip_mem[RP2350_QMI_NUM_CS];
    SSIBus *ssi;
    qemu_irq cs_lines[RP2350_QMI_NUM_CS];
    uint32_t regs[RP2350_QMI_NUM_REGS];
};

/*
 * Map the contents of the m25p80 @flash on chip select @cs into the XIP
 * window. Call after @flash is realized on @s->ssi.
 */
void rp2350_qmi_attach_xip(RP2350QMIState *s, unsigned cs, DeviceState *flash);

#endif
