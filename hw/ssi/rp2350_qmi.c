/*
 * RP2350 QSPI memory interface (QMI)
 *
 * The XIP window shows the m25p80 storage directly as read-only RAM, so
 * that code runs from flash at full speed (docs/adr/0003). QMI address
 * translation is fixed to the identity mapping and the XIP cache is not
 * modelled. Direct mode sends SPI transfers through m25p80 and disables XIP.
 *
 * Register offsets and reset values follow pico-sdk 2.3.1
 * src/rp2350/hardware_regs/include/hardware/regs/qmi.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/translation-block.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/block/flash.h"
#include "hw/core/irq.h"
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
#define QMI_ASSERT_CS(cs)   BIT(2 + (cs))
#define QMI_AUTO_CS(cs)     BIT(6 + (cs))
#define QMI_TXEMPTY         BIT(11)
#define QMI_RXEMPTY         BIT(16)
#define QMI_RXFULL          BIT(17)
#define QMI_RXLEVEL_SHIFT   18
#define QMI_CSR_WRITABLE    0xffc000cd
#define QMI_TX_NOPUSH       BIT(20)
#define QMI_TX_DWIDTH       BIT(18)

static void rp2350_qmi_set_direct(RP2350QMIState *s, uint32_t csr)
{
    bool enabled = csr & QMI_DIRECT_CSR_EN;

    memory_region_transaction_begin();
    for (unsigned cs = 0; cs < RP2350_QMI_NUM_CS; cs++) {
        MemoryRegion *mr = &s->xip_mem[cs];

        qemu_set_irq(s->cs_lines[cs], !(csr & QMI_ASSERT_CS(cs)));
        if (memory_region_size(mr)) {
            if (!enabled && (s->regs[0] & QMI_DIRECT_CSR_EN)) {
                ram_addr_t start = memory_region_get_ram_addr(mr);

                tb_invalidate_phys_range(NULL, start,
                                         start + memory_region_size(mr) - 1);
            }
            memory_region_set_enabled(mr, !enabled);
        }
    }
    memory_region_transaction_commit();
}

static void rp2350_qmi_transfer(RP2350QMIState *s, uint32_t value)
{
    uint32_t csr = s->regs[QMI_DIRECT_CSR / 4];
    uint16_t result;

    if (!(csr & QMI_DIRECT_CSR_EN)) {
        return;
    }
    for (unsigned cs = 0; cs < RP2350_QMI_NUM_CS; cs++) {
        if (csr & QMI_AUTO_CS(cs)) {
            qemu_set_irq(s->cs_lines[cs], 0);
        }
    }
    /* shortcut: transfers complete synchronously and ignore bus width. */
    result = ssi_transfer(s->ssi, value & 0xff);
    if (value & QMI_TX_DWIDTH) {
        result |= ssi_transfer(s->ssi, (value >> 8) & 0xff) << 8;
    }
    for (unsigned cs = 0; cs < RP2350_QMI_NUM_CS; cs++) {
        if ((csr & QMI_AUTO_CS(cs)) && !(csr & QMI_ASSERT_CS(cs))) {
            qemu_set_irq(s->cs_lines[cs], 1);
        }
    }
    if (!(value & QMI_TX_NOPUSH) && s->rx_count < RP2350_QMI_RX_DEPTH) {
        s->rx[s->rx_count++] = result;
    }
}

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
    switch (offset) {
    case QMI_DIRECT_CSR:
        return s->regs[0] | QMI_TXEMPTY |
               (!s->rx_count ? QMI_RXEMPTY : 0) |
               (s->rx_count == RP2350_QMI_RX_DEPTH ? QMI_RXFULL : 0) |
               (s->rx_count << QMI_RXLEVEL_SHIFT);
    case QMI_DIRECT_RX: {
        uint16_t value = s->rx[0];

        if (!s->rx_count) {
            return 0;
        }
        s->rx_count--;
        memmove(s->rx, s->rx + 1, s->rx_count * sizeof(s->rx[0]));
        return value;
    }
    default:
        return s->regs[offset / 4];
    }
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
    if (offset == QMI_DIRECT_CSR) {
        value &= QMI_CSR_WRITABLE;
        rp2350_qmi_set_direct(s, value);
    } else if (offset == QMI_DIRECT_TX) {
        rp2350_qmi_transfer(s, value);
        return;
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
    qemu_set_irq(s->cs_lines[cs], 1);
}

static void rp2350_qmi_reset_hold(Object *obj, ResetType type)
{
    RP2350QMIState *s = RP2350_QMI(obj);

    rp2350_qmi_set_direct(s, rp2350_qmi_reset_values[0]);
    memcpy(s->regs, rp2350_qmi_reset_values, sizeof(s->regs));
    s->rx_count = 0;
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

static int rp2350_qmi_post_load(void *opaque, int version_id)
{
    RP2350QMIState *s = opaque;

    if (s->rx_count > RP2350_QMI_RX_DEPTH) {
        return -EINVAL;
    }
    rp2350_qmi_set_direct(s, s->regs[0]);
    return 0;
}

static const VMStateDescription vmstate_rp2350_qmi = {
    .name = TYPE_RP2350_QMI,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = rp2350_qmi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RP2350QMIState, RP2350_QMI_NUM_REGS),
        VMSTATE_UINT16_ARRAY_V(rx, RP2350QMIState, RP2350_QMI_RX_DEPTH, 2),
        VMSTATE_UINT8_V(rx_count, RP2350QMIState, 2),
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
