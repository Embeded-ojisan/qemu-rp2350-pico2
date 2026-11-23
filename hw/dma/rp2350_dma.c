/*
 * RP2350 DMA, RP-008373 sections 10.7 and 12.6.
 * Register definitions follow pico-sdk 2.3.1 hardware/regs/dma.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/dma/rp2350_dma.h"
#include "hw/misc/rp2350_accessctrl.h"
#include "migration/vmstate.h"
#include "system/dma.h"

#define CTRL_EN          BIT(0)
#define CTRL_RING_WRITE  BIT(12)
#define CTRL_PERMANENT   0x3f
#define CTRL_IRQ_QUIET   BIT(23)
#define CTRL_BSWAP       BIT(24)
#define CTRL_BUSY        BIT(26)
#define CTRL_WRITE_ERROR BIT(29)
#define CTRL_READ_ERROR  BIT(30)
#define CTRL_AHB_ERROR   BIT(31)
#define CTRL_ERRORS      (CTRL_READ_ERROR | CTRL_WRITE_ERROR)
#define CTRL_RW          0x03ffffff
#define COUNT_MASK       0x0fffffff
#define MODE_SELF        1
#define MODE_ENDLESS     15
#define SEC_LOCK         BIT(2)
#define SEC_MASK         3
#define SEC_SECURE       BIT(1)
#define SEC_PRIV         BIT(0)
#define SEC_SP           3
#define MPU_ADDRESS      0xffffffe0
#define MPU_EN           BIT(0)
#define MPU_P            BIT(1)
#define MPU_S            BIT(2)
#define MPU_HIDE_ADDR    BIT(3)

enum {
    REG_INTR = 0x400,
    REG_IRQ_FIRST = 0x404,
    REG_IRQ_LAST = 0x43c,
    REG_MULTI_CHAN_TRIGGER = 0x450,
    REG_CHAN_ABORT = 0x464,
    REG_N_CHANNELS = 0x468,
    REG_SECCFG_CH0 = 0x480,
    REG_SECCFG_IRQ0 = 0x4c0,
    REG_SECCFG_MISC = 0x4d0,
    REG_MPU_CTRL = 0x500,
    REG_MPU_BAR0 = 0x504,
    REG_MPU_LAST = 0x540,
};

enum { READ, WRITE, COUNT, CTRL };

static const uint8_t channel_regs[] = {
    READ, WRITE, COUNT, CTRL,
    CTRL, READ, WRITE, COUNT,
    CTRL, COUNT, READ, WRITE,
    CTRL, WRITE, COUNT, READ,
};

static unsigned dma_level(MemTxAttrs attrs)
{
    if (attrs.debug || attrs.unspecified) {
        return SEC_SP;
    }
    return (attrs.secure ? SEC_SECURE : 0) | (attrs.user ? 0 : SEC_PRIV);
}

static uint32_t channels_visible(RP2350DMAState *s, unsigned level)
{
    uint32_t mask = 0;

    for (unsigned i = 0; i < RP2350_DMA_CHANNELS; i++) {
        if ((s->channels[i].security & SEC_MASK) <= level) {
            mask |= BIT(i);
        }
    }
    return mask;
}

static uint32_t irq_status(RP2350DMAState *s, unsigned irq)
{
    return ((s->intr & s->inte[irq]) | s->intf[irq]) &
           channels_visible(s, s->irq_security[irq]);
}

static void update_irqs(RP2350DMAState *s)
{
    for (unsigned i = 0; i < RP2350_DMA_IRQS; i++) {
        qemu_set_irq(s->irq[i], irq_status(s, i) != 0);
    }
}

static bool channel_ready(RP2350DMAChannel *c)
{
    /*
     * shortcut: only the permanent request is wired. Paced channels
     * wait until their peripheral or timer DREQ sources are modelled.
     */
    return c->busy && (c->ctrl & CTRL_EN) &&
           extract32(c->ctrl, 17, 6) == CTRL_PERMANENT;
}

static void schedule_dma(RP2350DMAState *s)
{
    for (unsigned i = 0; i < RP2350_DMA_CHANNELS; i++) {
        if (channel_ready(&s->channels[i])) {
            if (!timer_pending(s->timer)) {
                timer_mod_ns(s->timer,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
            }
            return;
        }
    }
    timer_del(s->timer);
}

static void start_channel(RP2350DMAChannel *c)
{
    if (!c->busy && !c->errors && (c->ctrl & CTRL_EN)) {
        c->count = c->reload;
        c->busy = (c->count & COUNT_MASK) ||
                  extract32(c->count, 28, 4) == MODE_ENDLESS;
    }
}

static unsigned mpu_level(RP2350DMAState *s, uint32_t addr)
{
    uint32_t block = addr & MPU_ADDRESS;

    for (unsigned i = 0; i < RP2350_DMA_MPU_REGIONS; i++) {
        uint32_t lar = s->mpu_lar[i];

        if ((lar & MPU_EN) && block >= s->mpu_bar[i] &&
            block <= (lar & MPU_ADDRESS)) {
            return extract32(lar, 1, 2);
        }
    }
    return extract32(s->mpu_ctrl, 1, 2);
}

static uint32_t advance_addr(uint32_t addr, unsigned size, unsigned inc,
                             uint32_t ring_mask)
{
    static const int strides[] = { 0, 1, 2, -1 };
    uint32_t next = addr + strides[inc] * size;

    return (addr & ~ring_mask) | (next & ring_mask);
}

static void transfer_beat(RP2350DMAState *s, unsigned channel)
{
    RP2350DMAChannel *c = &s->channels[channel];
    unsigned level = c->security & SEC_MASK;
    unsigned size = 1u << extract32(c->ctrl, 2, 2);
    unsigned mode = extract32(c->count, 28, 4);
    unsigned ring = extract32(c->ctrl, 8, 4);
    unsigned chain = extract32(c->ctrl, 13, 4);
    uint32_t mask = ring ? (1u << ring) - 1 : UINT32_MAX;
    MemTxAttrs attrs = {
        .secure = !!(level & SEC_SECURE),
        .user = !(level & SEC_PRIV),
        .requester_id = RP2350_DMA_MANAGER,
    };
    uint8_t data[4];

    if (size > sizeof(data) || !QEMU_IS_ALIGNED(c->read, size) ||
        level < mpu_level(s, c->read) ||
        dma_memory_read(s->bus, c->read, data, size, attrs) != MEMTX_OK) {
        c->errors |= CTRL_READ_ERROR;
        goto error;
    }
    if (c->ctrl & CTRL_BSWAP) {
        for (unsigned i = 0; i < size / 2; i++) {
            uint8_t tmp = data[i];

            data[i] = data[size - i - 1];
            data[size - i - 1] = tmp;
        }
    }
    if (!QEMU_IS_ALIGNED(c->write, size) || level < mpu_level(s, c->write) ||
        dma_memory_write(s->bus, c->write, data, size, attrs) != MEMTX_OK) {
        c->errors |= CTRL_WRITE_ERROR;
        goto error;
    }
    c->read = advance_addr(c->read, size, extract32(c->ctrl, 4, 2),
                           c->ctrl & CTRL_RING_WRITE ? UINT32_MAX : mask);
    c->write = advance_addr(c->write, size, extract32(c->ctrl, 6, 2),
                            c->ctrl & CTRL_RING_WRITE ? mask : UINT32_MAX);
    if (mode == MODE_ENDLESS || (--c->count & COUNT_MASK)) {
        return;
    }

    c->busy = false;
    if (!(c->ctrl & CTRL_IRQ_QUIET)) {
        s->intr |= BIT(channel);
    }
    if (chain != channel &&
        level >= (s->channels[chain].security & SEC_MASK)) {
        start_channel(&s->channels[chain]);
    }
    if (mode == MODE_SELF) {
        start_channel(c);
    }
    return;

error:
    c->busy = false;
    s->intr |= BIT(channel);
}

static void run_dma(void *opaque)
{
    RP2350DMAState *s = opaque;

    for (unsigned i = 0; i < RP2350_DMA_CHANNELS; i++) {
        if (channel_ready(&s->channels[i])) {
            transfer_beat(s, i);
        }
    }
    update_irqs(s);
    schedule_dma(s);
}

static unsigned irq_slot(hwaddr addr)
{
    return (addr - REG_IRQ_FIRST) / 0x10;
}

static MemTxResult dma_read(void *opaque, hwaddr addr, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    RP2350DMAState *s = opaque;
    unsigned level = dma_level(attrs);

    *value = 0;
    if (s->held_reset) {
        return MEMTX_OK;
    }
    if (addr < REG_INTR) {
        RP2350DMAChannel *c = &s->channels[addr >> 6];

        if (level < (c->security & SEC_MASK)) {
            return MEMTX_ACCESS_ERROR;
        }
        switch (channel_regs[(addr & 0x3f) / 4]) {
        case READ:
            *value = c->read;
            break;
        case WRITE:
            *value = c->write;
            break;
        case COUNT:
            *value = c->count;
            break;
        case CTRL:
            *value = c->ctrl | c->errors | (c->busy ? CTRL_BUSY : 0) |
                     (c->errors ? CTRL_AHB_ERROR : 0);
            break;
        }
        return MEMTX_OK;
    }
    switch (addr) {
    case REG_INTR:
        *value = s->intr & channels_visible(s, level);
        break;
    case REG_IRQ_FIRST ... REG_IRQ_LAST: {
        unsigned i = irq_slot(addr);
        unsigned reg = (addr - REG_IRQ_FIRST) & 0xf;

        if (level < s->irq_security[i]) {
            return MEMTX_ACCESS_ERROR;
        }
        if (reg == 0) {
            *value = s->inte[i] & channels_visible(s, s->irq_security[i]);
        } else if (reg == 4) {
            *value = s->intf[i] & channels_visible(s, s->irq_security[i]);
        } else if (reg == 8) {
            *value = irq_status(s, i);
        }
        break;
    }
    case REG_N_CHANNELS:
        *value = RP2350_DMA_CHANNELS;
        break;
    case REG_SECCFG_CH0 ... REG_SECCFG_IRQ0 - 4:
        *value = s->channels[(addr - REG_SECCFG_CH0) / 4].security;
        break;
    case REG_SECCFG_IRQ0 ... REG_SECCFG_MISC - 4:
        *value = s->irq_security[(addr - REG_SECCFG_IRQ0) / 4];
        break;
    case REG_SECCFG_MISC:
        *value = s->misc_security;
        break;
    case REG_MPU_CTRL ... REG_MPU_LAST: {
        unsigned i = (addr - REG_MPU_BAR0) / 8;
        uint32_t lar;

        if (!(level & SEC_PRIV)) {
            return MEMTX_ACCESS_ERROR;
        }
        if (addr == REG_MPU_CTRL) {
            *value = s->mpu_ctrl;
            break;
        }
        lar = s->mpu_lar[i];
        *value = (addr - REG_MPU_BAR0) & 4 ? lar : s->mpu_bar[i];
        if (level != SEC_SP && ((lar & MPU_S) ||
                               (s->mpu_ctrl & MPU_HIDE_ADDR))) {
            *value &= ~MPU_ADDRESS;
        }
        break;
    }
    default:
        break;
    }
    return MEMTX_OK;
}

static void channel_write(RP2350DMAState *s, unsigned channel,
                          unsigned slot, uint32_t value)
{
    RP2350DMAChannel *c = &s->channels[channel];

    c->security |= SEC_LOCK;
    switch (channel_regs[slot]) {
    case READ:
        c->read = value;
        break;
    case WRITE:
        c->write = value;
        break;
    case COUNT:
        c->reload = value;
        if (!c->busy) {
            c->count = value;
        }
        break;
    case CTRL:
        c->errors &= ~(value & CTRL_ERRORS);
        c->ctrl = value & CTRL_RW;
        break;
    }
    if ((slot & 3) == 3) {
        if (slot == 3 || value) {
            start_channel(c);
        } else if (c->ctrl & CTRL_IRQ_QUIET) {
            s->intr |= BIT(channel);
        }
    }
}

static uint8_t security_write(unsigned level, uint8_t old, uint32_t value)
{
    if (level == SEC_SP) {
        return value & (SEC_LOCK | SEC_MASK);
    }
    return old & SEC_SECURE ? old : (old & ~SEC_PRIV) | (value & SEC_PRIV);
}

static MemTxResult dma_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    RP2350DMAState *s = opaque;
    unsigned level = dma_level(attrs);
    uint32_t visible;

    if (s->held_reset) {
        return MEMTX_OK;
    }
    if (addr < REG_INTR) {
        unsigned channel = addr >> 6;

        if (level < (s->channels[channel].security & SEC_MASK)) {
            return MEMTX_ACCESS_ERROR;
        }
        channel_write(s, channel, (addr & 0x3f) / 4, value);
        goto update;
    }
    switch (addr) {
    case REG_INTR:
        s->intr &= ~(value & channels_visible(s, level));
        break;
    case REG_IRQ_FIRST ... REG_IRQ_LAST: {
        unsigned i = irq_slot(addr);
        unsigned reg = (addr - REG_IRQ_FIRST) & 0xf;

        if (level < s->irq_security[i]) {
            return MEMTX_ACCESS_ERROR;
        }
        visible = channels_visible(s, s->irq_security[i]);
        if (reg == 0) {
            s->inte[i] = value & visible;
        } else if (reg == 4) {
            s->intf[i] = value & visible;
        } else if (reg == 8) {
            s->intr &= ~(value & visible);
        }
        break;
    }
    case REG_MULTI_CHAN_TRIGGER:
    case REG_CHAN_ABORT:
        value &= channels_visible(s, level);
        for (unsigned i = 0; i < RP2350_DMA_CHANNELS; i++) {
            if (value & BIT(i)) {
                if (addr == REG_CHAN_ABORT) {
                    s->channels[i].busy = false;
                } else {
                    start_channel(&s->channels[i]);
                }
            }
        }
        break;
    case REG_SECCFG_CH0 ... REG_SECCFG_IRQ0 - 4: {
        RP2350DMAChannel *c = &s->channels[(addr - REG_SECCFG_CH0) / 4];

        if (!(level & SEC_PRIV)) {
            return MEMTX_ACCESS_ERROR;
        }
        if (!(c->security & SEC_LOCK)) {
            c->security = security_write(level, c->security, value);
        }
        break;
    }
    case REG_SECCFG_IRQ0 ... REG_SECCFG_MISC - 4: {
        unsigned i = (addr - REG_SECCFG_IRQ0) / 4;

        if (!(level & SEC_PRIV)) {
            return MEMTX_ACCESS_ERROR;
        }
        s->irq_security[i] = security_write(level, s->irq_security[i], value) &
                             SEC_MASK;
        break;
    }
    case REG_SECCFG_MISC:
        if (!(level & SEC_PRIV)) {
            return MEMTX_ACCESS_ERROR;
        }
        for (unsigned i = 0; i < 10; i += 2) {
            unsigned old = extract32(s->misc_security, i, 2);

            s->misc_security = deposit32(s->misc_security, i, 2,
                                security_write(level, old, value >> i) & 3);
        }
        break;
    case REG_MPU_CTRL ... REG_MPU_LAST: {
        unsigned i = (addr - REG_MPU_BAR0) / 8;

        if (!(level & SEC_PRIV)) {
            return MEMTX_ACCESS_ERROR;
        }
        if (addr == REG_MPU_CTRL) {
            if (level == SEC_SP) {
                s->mpu_ctrl = value & (MPU_S | MPU_P | MPU_HIDE_ADDR);
            } else if (!(s->mpu_ctrl & MPU_S)) {
                s->mpu_ctrl = (s->mpu_ctrl & ~MPU_P) | (value & MPU_P);
            }
        } else if ((addr - REG_MPU_BAR0) & 4) {
            if (level == SEC_SP) {
                s->mpu_lar[i] = value & (MPU_ADDRESS | MPU_S | MPU_P | MPU_EN);
            } else if (!(s->mpu_lar[i] & MPU_S)) {
                s->mpu_lar[i] = (s->mpu_lar[i] & ~MPU_P) | (value & MPU_P);
            }
        } else if (level == SEC_SP) {
            s->mpu_bar[i] = value & MPU_ADDRESS;
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "rp2350-dma: write to offset 0x%" HWADDR_PRIx
                      "\n", addr);
        break;
    }

update:
    update_irqs(s);
    schedule_dma(s);
    return MEMTX_OK;
}

static const MemoryRegionOps dma_ops = {
    .read_with_attrs = dma_read,
    .write_with_attrs = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dma_clear(RP2350DMAState *s)
{
    memset(s->channels, 0, sizeof(s->channels));
    for (unsigned i = 0; i < RP2350_DMA_CHANNELS; i++) {
        s->channels[i].security = SEC_SP;
    }
    s->intr = 0;
    memset(s->inte, 0, sizeof(s->inte));
    memset(s->intf, 0, sizeof(s->intf));
    memset(s->irq_security, SEC_SP, sizeof(s->irq_security));
    s->misc_security = 0x3ff;
    s->mpu_ctrl = 0;
    memset(s->mpu_bar, 0, sizeof(s->mpu_bar));
    memset(s->mpu_lar, 0, sizeof(s->mpu_lar));
    timer_del(s->timer);
    update_irqs(s);
}

static void dma_reset_input(void *opaque, int line, int level)
{
    RP2350DMAState *s = opaque;

    s->held_reset = level;
    if (s->held_reset) {
        dma_clear(s);
    }
}

static void dma_reset(Object *obj, ResetType type)
{
    RP2350DMAState *s = RP2350_DMA(obj);

    s->held_reset = true;
    dma_clear(s);
}

static void dma_init(Object *obj)
{
    RP2350DMAState *s = RP2350_DMA(obj);

    memory_region_init_io(&s->iomem, obj, &dma_ops, s, TYPE_RP2350_DMA, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < RP2350_DMA_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), dma_reset_input, "reset", 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, run_dma, s);
}

static void dma_finalize(Object *obj)
{
    RP2350DMAState *s = RP2350_DMA(obj);

    timer_free(s->timer);
}

static const VMStateDescription channel_vmstate = {
    .name = TYPE_RP2350_DMA "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(read, RP2350DMAChannel),
        VMSTATE_UINT32(write, RP2350DMAChannel),
        VMSTATE_UINT32(reload, RP2350DMAChannel),
        VMSTATE_UINT32(count, RP2350DMAChannel),
        VMSTATE_UINT32(ctrl, RP2350DMAChannel),
        VMSTATE_UINT32(errors, RP2350DMAChannel),
        VMSTATE_UINT8(security, RP2350DMAChannel),
        VMSTATE_BOOL(busy, RP2350DMAChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int dma_post_load(void *opaque, int version_id)
{
    RP2350DMAState *s = opaque;

    update_irqs(s);
    schedule_dma(s);
    return 0;
}

static const VMStateDescription dma_vmstate = {
    .name = TYPE_RP2350_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channels, RP2350DMAState, RP2350_DMA_CHANNELS, 1,
                             channel_vmstate, RP2350DMAChannel),
        VMSTATE_UINT32(intr, RP2350DMAState),
        VMSTATE_UINT32_ARRAY(inte, RP2350DMAState, RP2350_DMA_IRQS),
        VMSTATE_UINT32_ARRAY(intf, RP2350DMAState, RP2350_DMA_IRQS),
        VMSTATE_UINT8_ARRAY(irq_security, RP2350DMAState, RP2350_DMA_IRQS),
        VMSTATE_UINT16(misc_security, RP2350DMAState),
        VMSTATE_UINT32(mpu_ctrl, RP2350DMAState),
        VMSTATE_UINT32_ARRAY(mpu_bar, RP2350DMAState, RP2350_DMA_MPU_REGIONS),
        VMSTATE_UINT32_ARRAY(mpu_lar, RP2350DMAState, RP2350_DMA_MPU_REGIONS),
        VMSTATE_BOOL(held_reset, RP2350DMAState),
        VMSTATE_TIMER_PTR(timer, RP2350DMAState),
        VMSTATE_END_OF_LIST()
    },
};

static void dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = dma_reset;
    dc->vmsd = &dma_vmstate;
    dc->user_creatable = false;
}

static const TypeInfo dma_info = {
    .name = TYPE_RP2350_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RP2350DMAState),
    .instance_init = dma_init,
    .instance_finalize = dma_finalize,
    .class_init = dma_class_init,
};

static void dma_register_types(void)
{
    type_register_static(&dma_info);
}

type_init(dma_register_types)
