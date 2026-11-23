/*
 * RP2350 QSPI memory interface (QMI): registers and the XIP read path
 *
 * The XIP window shows the m25p80 storage directly as read-only RAM, so
 * that code runs from flash at full speed (docs/adr/0003). QMI address
 * translation is fixed to the identity mapping and the XIP cache is not
 * modelled. Direct mode (serial commands for erase and program) is not
 * implemented yet; it will unmap xip_mem while it is active.
 *
 * Register offsets and reset values follow pico-sdk 2.3.1
 * src/rp2350/hardware_regs/include/hardware/regs/qmi.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/block/flash.h"
#include "hw/core/qdev.h"
#include "hw/ssi/rp2350_qmi.h"
#include "migration/vmstate.h"
#include "trace.h"

#define QMI_DIRECT_CSR      0x00
#define QMI_DIRECT_TX       0x04
#define QMI_DIRECT_RX       0x08
#define QMI_ATRANS0         0x34
#define QMI_NUM_ATRANS      8
#define QMI_DIRECT_CSR_EN   BIT(0)

static const uint32_t rp2350_qmi_reset_values[RP2350_QMI_NUM_REGS] = {
    [0x00 / 4] = 0x01800000,    /* DIRECT_CSR */
    [0x0c / 4] = 0x40000004,    /* M0_TIMING */
    [0x10 / 4] = 0x00001000,    /* M0_RFMT */
    [0x14 / 4] = 0x0000a003,    /* M0_RCMD */
    [0x18 / 4] = 0x00001000,    /* M0_WFMT */
    [0x1c / 4] = 0x0000a002,    /* M0_WCMD */
    [0x20 / 4] = 0x40000004,    /* M1_TIMING */
    [0x24 / 4] = 0x00001000,    /* M1_RFMT */
    [0x28 / 4] = 0x0000a003,    /* M1_RCMD */
    [0x2c / 4] = 0x00001000,    /* M1_WFMT */
    [0x30 / 4] = 0x0000a002,    /* M1_WCMD */
    [0x34 / 4] = 0x04000000,    /* ATRANS0..7: identity, 4 MB each */
    [0x38 / 4] = 0x04000400,
    [0x3c / 4] = 0x04000800,
    [0x40 / 4] = 0x04000c00,
    [0x44 / 4] = 0x04000000,
    [0x48 / 4] = 0x04000400,
    [0x4c / 4] = 0x04000800,
    [0x50 / 4] = 0x04000c00,
};

static uint64_t rp2350_qmi_read(void *opaque, hwaddr offset, unsigned size)
{
    RP2350QMIState *s = opaque;

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
    if (offset == QMI_DIRECT_RX) {
        qemu_log_mask(LOG_UNIMP, "%s: direct mode is not implemented\n",
                      __func__);
        return 0;
    }
    return s->regs[offset / 4];
}

static void rp2350_qmi_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RP2350QMIState *s = opaque;

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    trace_rp2350_qmi_write(offset, value);
    if ((offset == QMI_DIRECT_CSR && (value & QMI_DIRECT_CSR_EN)) ||
        offset == QMI_DIRECT_TX) {
        qemu_log_mask(LOG_UNIMP, "%s: direct mode is not implemented\n",
                      __func__);
    }
    if (offset >= QMI_ATRANS0 &&
        offset < QMI_ATRANS0 + 4 * QMI_NUM_ATRANS &&
        value != rp2350_qmi_reset_values[offset / 4]) {
        qemu_log_mask(LOG_UNIMP, "%s: address translation is fixed to the "
                      "identity mapping\n", __func__);
    }
    if (offset != QMI_DIRECT_RX) {
        s->regs[offset / 4] = value;
    }
}

static const MemoryRegionOps rp2350_qmi_ops = {
    .read = rp2350_qmi_read,
    .write = rp2350_qmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void rp2350_qmi_attach_xip(RP2350QMIState *s, unsigned cs, DeviceState *flash)
{
    g_autofree char *name = g_strdup_printf("rp2350-qmi.xip-cs%u", cs);
    uint32_t size;
    uint8_t *storage = m25p80_get_storage(flash, &size);

    assert(cs < RP2350_QMI_NUM_CS && size <= RP2350_QMI_CS_WINDOW);
    /* shortcut: no mirroring above the flash size; reads there bus-fault */
    memory_region_init_ram_ptr(&s->xip_mem[cs], OBJECT(s), name, size,
                               storage);
    memory_region_set_readonly(&s->xip_mem[cs], true);
    memory_region_add_subregion(&s->xip, cs * RP2350_QMI_CS_WINDOW,
                                &s->xip_mem[cs]);
}

static void rp2350_qmi_reset_hold(Object *obj, ResetType type)
{
    RP2350QMIState *s = RP2350_QMI(obj);

    memcpy(s->regs, rp2350_qmi_reset_values, sizeof(s->regs));
}

static void rp2350_qmi_init(Object *obj)
{
    RP2350QMIState *s = RP2350_QMI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rp2350_qmi_ops, s,
                          TYPE_RP2350_QMI, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init(&s->xip, obj, "rp2350-qmi.xip",
                       RP2350_QMI_NUM_CS * RP2350_QMI_CS_WINDOW);
    sysbus_init_mmio(sbd, &s->xip);
    s->ssi = ssi_create_bus(DEVICE(obj), "ssi");
    qdev_init_gpio_out_named(DEVICE(obj), s->cs_lines, "cs",
                             RP2350_QMI_NUM_CS);
}

static const VMStateDescription vmstate_rp2350_qmi = {
    .name = TYPE_RP2350_QMI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RP2350QMIState, RP2350_QMI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void rp2350_qmi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rp2350_qmi_reset_hold;
    dc->vmsd = &vmstate_rp2350_qmi;
}

static const TypeInfo rp2350_qmi_types[] = {
    {
        .name = TYPE_RP2350_QMI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350QMIState),
        .instance_init = rp2350_qmi_init,
        .class_init = rp2350_qmi_class_init,
    },
};

DEFINE_TYPES(rp2350_qmi_types)
