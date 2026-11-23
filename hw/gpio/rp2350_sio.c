/*
 * RP2350 single-cycle IO (SIO), core 0 view: CPUID, GPIO and spinlocks
 *
 * Register offsets follow pico-sdk 2.3.1
 * src/rp2350/hardware_regs/include/hardware/regs/sio.h.
 * Interpolators, the inter-core FIFOs, the doorbells, TMDS encoders and
 * the RISC-V timer are not modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/gpio/rp2350_sio.h"
#include "migration/vmstate.h"
#include "trace.h"

#define SIO_CPUID           0x000
#define SIO_GPIO_IN         0x004
#define SIO_GPIO_HI_IN      0x008
#define SIO_GPIO_OUT        0x010   /* OUT, HI_OUT, then SET/CLR/XOR pairs */
#define SIO_GPIO_OE         0x030   /* OE, HI_OE, then SET/CLR/XOR pairs */
#define SIO_GPIO_REG_END    0x050
#define SIO_FIFO_ST         0x050
#define SIO_FIFO_ST_RDY     BIT(1)
#define SIO_SPINLOCK_ST     0x05c
#define SIO_SPINLOCK0       0x100
#define SIO_NUM_SPINLOCKS   32
#define SIO_BANK_SIZE       0x20000

#define SIO_GPIO_HI_PINS    0x0000ffff  /* GPIO32..47 in the HI registers */
#define SIO_GPIO_HI_BITS    0xff00ffff  /* Includes QSPI and USB bits */

void rp2350_sio_gpio_set_nsmask(RP2350SIOState *s, unsigned bank,
                               uint32_t mask)
{
    s->gpio_nsmask[bank] = mask & (bank ? SIO_GPIO_HI_BITS : UINT32_MAX);
}

static void rp2350_sio_update_out(RP2350SIOState *s, unsigned bank,
                                  uint32_t value)
{
    uint32_t pins = bank ? SIO_GPIO_HI_PINS : UINT32_MAX;
    uint32_t changed = (s->gpio_out[bank] ^ value) & pins;

    s->gpio_out[bank] = value;
    while (changed) {
        unsigned bit = ctz32(changed);
        unsigned pin = bank * 32 + bit;
        int level = extract32(value, bit, 1);

        changed &= changed - 1;
        trace_rp2350_sio_gpio_out(pin, level);
        qemu_set_irq(s->gpio_out_irq[pin], level);
    }
}

uint32_t rp2350_sio_gpio_read(RP2350SIOState *s, unsigned group,
                              unsigned bank, bool nonsecure)
{
    uint32_t value;

    switch (group) {
    case RP2350_SIO_GPIO_OUT:
        value = s->gpio_out[bank];
        break;
    case RP2350_SIO_GPIO_OE:
        value = s->gpio_oe[bank];
        break;
    default:
        /* shortcut: no external inputs; an output-enabled pin reads back */
        value = s->gpio_out[bank] & s->gpio_oe[bank];
        break;
    }
    if (bank) {
        value &= SIO_GPIO_HI_BITS;
    }
    return value & (nonsecure ? s->gpio_nsmask[bank] : UINT32_MAX);
}

void rp2350_sio_gpio_write(RP2350SIOState *s, unsigned group, unsigned bank,
                           unsigned op, uint32_t value, bool nonsecure)
{
    uint32_t mask = bank ? SIO_GPIO_HI_BITS : UINT32_MAX;
    uint32_t *reg;
    uint32_t new;

    if (group == RP2350_SIO_GPIO_OUT) {
        reg = &s->gpio_out[bank];
    } else if (group == RP2350_SIO_GPIO_OE) {
        reg = &s->gpio_oe[bank];
    } else {
        return;
    }
    switch (op) {
    case RP2350_SIO_OP_XOR:
        new = *reg ^ value;
        break;
    case RP2350_SIO_OP_SET:
        new = *reg | value;
        break;
    case RP2350_SIO_OP_CLR:
        new = *reg & ~value;
        break;
    default:
        new = value;
        break;
    }
    if (nonsecure) {
        mask &= s->gpio_nsmask[bank];
    }
    new = (new & mask) | (*reg & ~mask);
    if (group == RP2350_SIO_GPIO_OE) {
        *reg = new;
        trace_rp2350_sio_gpio_oe(bank, new);
    } else {
        rp2350_sio_update_out(s, bank, new);
    }
}

static uint64_t rp2350_sio_read(RP2350SIOState *s, hwaddr offset,
                                bool nonsecure)
{
    uint32_t *spinlock_st = &s->spinlock_st[nonsecure];

    switch (offset) {
    case SIO_CPUID:
        return 0;   /* only core 0 is modelled */
    case SIO_GPIO_IN:
    case SIO_GPIO_HI_IN:
        return rp2350_sio_gpio_read(s, RP2350_SIO_GPIO_IN,
                                    (offset - SIO_GPIO_IN) / 4, nonsecure);
    case SIO_GPIO_OUT:
    case SIO_GPIO_OUT + 4:
        return rp2350_sio_gpio_read(s, RP2350_SIO_GPIO_OUT,
                                    (offset - SIO_GPIO_OUT) / 4, nonsecure);
    case SIO_GPIO_OE:
    case SIO_GPIO_OE + 4:
        return rp2350_sio_gpio_read(s, RP2350_SIO_GPIO_OE,
                                    (offset - SIO_GPIO_OE) / 4, nonsecure);
    case SIO_FIFO_ST:
        return SIO_FIFO_ST_RDY; /* no core 1: the FIFO stays empty */
    case SIO_SPINLOCK_ST:
        return *spinlock_st;
    case SIO_SPINLOCK0 ... SIO_SPINLOCK0 + 4 * SIO_NUM_SPINLOCKS - 1: {
        /* Read to claim: 1 << n on success, 0 if already claimed */
        unsigned n = (offset - SIO_SPINLOCK0) / 4;

        if (*spinlock_st & BIT(n)) {
            return 0;
        }
        *spinlock_st |= BIT(n);
        return BIT(n);
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
}

static void rp2350_sio_write(RP2350SIOState *s, hwaddr offset, uint64_t value,
                             bool nonsecure)
{
    if (offset >= SIO_GPIO_OUT && offset < SIO_GPIO_REG_END) {
        /* MMIO order: value, SET, CLR, XOR, each as a lo/hi pair */
        static const unsigned mmio_op[] = {
            RP2350_SIO_OP_WRITE, RP2350_SIO_OP_SET,
            RP2350_SIO_OP_CLR, RP2350_SIO_OP_XOR,
        };
        bool is_oe = offset >= SIO_GPIO_OE;
        unsigned group = is_oe ? RP2350_SIO_GPIO_OE : RP2350_SIO_GPIO_OUT;
        hwaddr rel = offset - (is_oe ? SIO_GPIO_OE : SIO_GPIO_OUT);

        rp2350_sio_gpio_write(s, group, (rel / 4) & 1, mmio_op[rel / 8],
                              value, nonsecure);
        return;
    }

    switch (offset) {
    case SIO_SPINLOCK0 ... SIO_SPINLOCK0 + 4 * SIO_NUM_SPINLOCKS - 1:
        /* Write to release */
        s->spinlock_st[nonsecure] &= ~BIT((offset - SIO_SPINLOCK0) / 4);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", __func__, offset, value);
        break;
    }
}

static MemTxResult rp2350_sio_read_attrs(void *opaque, hwaddr offset,
                                         uint64_t *value, unsigned size,
                                         MemTxAttrs attrs)
{
    bool mirror = offset >= SIO_BANK_SIZE;
    /* qtest and other accesses without bus attributes use the Secure view. */
    bool nonsecure = !attrs.secure && !attrs.unspecified;

    if (mirror && nonsecure) {
        return MEMTX_ACCESS_ERROR;
    }
    *value = rp2350_sio_read(opaque, offset % SIO_BANK_SIZE,
                             nonsecure || mirror);
    return MEMTX_OK;
}

static MemTxResult rp2350_sio_write_attrs(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    bool mirror = offset >= SIO_BANK_SIZE;
    bool nonsecure = !attrs.secure && !attrs.unspecified;

    if (mirror && nonsecure) {
        return MEMTX_ACCESS_ERROR;
    }
    rp2350_sio_write(opaque, offset % SIO_BANK_SIZE, value,
                      nonsecure || mirror);
    return MEMTX_OK;
}

static const MemoryRegionOps rp2350_sio_ops = {
    .read_with_attrs = rp2350_sio_read_attrs,
    .write_with_attrs = rp2350_sio_write_attrs,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rp2350_sio_reset_hold(Object *obj, ResetType type)
{
    RP2350SIOState *s = RP2350_SIO(obj);

    memset(s->gpio_out, 0, sizeof(s->gpio_out));
    memset(s->gpio_oe, 0, sizeof(s->gpio_oe));
    memset(s->spinlock_st, 0, sizeof(s->spinlock_st));
    memset(s->gpio_nsmask, 0, sizeof(s->gpio_nsmask));
}

static void rp2350_sio_init(Object *obj)
{
    RP2350SIOState *s = RP2350_SIO(obj);

    memory_region_init_io(&s->iomem, obj, &rp2350_sio_ops, s,
                          TYPE_RP2350_SIO, 2 * SIO_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->gpio_out_irq, "gpio-out",
                             RP2350_SIO_NUM_GPIO);
}

static const VMStateDescription vmstate_rp2350_sio = {
    .name = TYPE_RP2350_SIO,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gpio_out, RP2350SIOState, 2),
        VMSTATE_UINT32_ARRAY(gpio_oe, RP2350SIOState, 2),
        VMSTATE_UINT32(spinlock_st[0], RP2350SIOState),
        VMSTATE_UINT32_V(spinlock_st[1], RP2350SIOState, 2),
        VMSTATE_UINT32_ARRAY_V(gpio_nsmask, RP2350SIOState, 2, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void rp2350_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rp2350_sio_reset_hold;
    dc->vmsd = &vmstate_rp2350_sio;
}

static const TypeInfo rp2350_sio_types[] = {
    {
        .name = TYPE_RP2350_SIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350SIOState),
        .instance_init = rp2350_sio_init,
        .class_init = rp2350_sio_class_init,
    },
};

DEFINE_TYPES(rp2350_sio_types)
