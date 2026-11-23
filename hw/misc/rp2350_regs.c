/*
 * RP2350 simple register blocks: resets, clocks, xosc, PLL, ticks,
 * IO_BANK0, PADS_BANK0 and BOOTRAM.
 *
 * Register offsets and reset values follow pico-sdk 2.3.1
 * src/rp2350/hardware_regs/include/hardware/regs/<block>.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/misc/rp2350_regs.h"
#include "migration/vmstate.h"
#include "trace.h"

/* RESETS */
#define RESETS_RESET            0x000
#define RESETS_RESET_DONE       0x008
#define RESETS_ALL_BITS         0x1fffffff

/* CLOCKS: each clock has CTRL, DIV and SELECTED at a 12-byte stride */
#define CLOCKS_NUM_CLK          10
#define CLOCKS_CLK_STRIDE       0x0c
#define CLOCKS_SELECTED         0x08
#define CLOCKS_CLK_REF          4
#define CLOCKS_CLK_SYS          5
#define CLOCKS_CLK_REF_SRC      0x3
#define CLOCKS_CLK_SYS_SRC      0x1
#define CLOCKS_CLK_SYS_CTRL     0x3c

/* XOSC */
#define XOSC_STATUS             0x004
#define XOSC_STATUS_ENABLED     BIT(12)
#define XOSC_STATUS_STABLE      BIT(31)

/* PLL */
#define PLL_CS                  0x000
#define PLL_CS_LOCK             BIT(31)

/* TICKS: six generators with CTRL, CYCLES and COUNT at a 12-byte stride */
#define TICKS_NUM_GEN           6
#define TICKS_GEN_STRIDE        0x0c
#define TICKS_CTRL_ENABLE       BIT(0)
#define TICKS_CTRL_RUNNING      BIT(1)

/* BOOTRAM: 1 kB of SRAM followed by registers at 0x800 */
#define BOOTRAM_SRAM_SIZE       0x400
#define BOOTRAM_WRITE_ONCE0     0x800
#define BOOTRAM_WRITE_ONCE1     0x804
#define BOOTRAM_BOOTLOCK_STAT   0x808
#define BOOTRAM_BOOTLOCK0       0x80c
#define BOOTRAM_NUM_BOOTLOCKS   8

static uint64_t rp2350_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2350RegsState *s = opaque;
    RP2350RegsClass *rc = RP2350_REGS_GET_CLASS(s);
    hwaddr offset = addr & ~3;
    uint32_t value;

    value = rc->read ? rc->read(s, offset) : s->regs[offset / 4];
    trace_rp2350_regs_read(object_get_typename(OBJECT(s)), offset, value);
    return extract32(value, (addr & 3) * 8, size * 8);
}

static void rp2350_regs_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
    RP2350RegsState *s = opaque;
    RP2350RegsClass *rc = RP2350_REGS_GET_CLASS(s);
    hwaddr offset = addr & ~3;
    uint32_t value = deposit32(s->regs[offset / 4], (addr & 3) * 8, size * 8,
                               data);

    trace_rp2350_regs_write(object_get_typename(OBJECT(s)), offset, value);
    if (!rc->write || !rc->write(s, offset, value)) {
        s->regs[offset / 4] = value;
    }
}

static const MemoryRegionOps rp2350_regs_ops = {
    .read = rp2350_regs_read,
    .write = rp2350_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void rp2350_regs_reset_hold(Object *obj, ResetType type)
{
    RP2350RegsState *s = RP2350_REGS(obj);
    RP2350RegsClass *rc = RP2350_REGS_GET_CLASS(s);

    memset(s->regs, 0, sizeof(s->regs));
    for (const uint32_t *rv = rc->reset_values; rv && (rv[0] || rv[1]);
         rv += 2) {
        s->regs[rv[0] / 4] = rv[1];
    }
}

static void rp2350_regs_init(Object *obj)
{
    RP2350RegsState *s = RP2350_REGS(obj);

    memory_region_init_io(&s->iomem, obj, &rp2350_regs_ops, s,
                          object_get_typename(obj), RP2350_ATOMIC_BLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_rp2350_regs = {
    .name = TYPE_RP2350_REGS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RP2350RegsState, RP2350_REGS_NUM_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static void rp2350_regs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rp2350_regs_reset_hold;
    dc->vmsd = &vmstate_rp2350_regs;
}

/* RESETS: every block leaves reset as soon as its RESET bit is cleared. */
static const uint32_t rp2350_resets_reset_values[] = {
    RESETS_RESET, RESETS_ALL_BITS,
    0, 0
};

static uint32_t rp2350_resets_read(RP2350RegsState *s, hwaddr offset)
{
    if (offset == RESETS_RESET_DONE) {
        return ~s->regs[RESETS_RESET / 4] & RESETS_ALL_BITS;
    }
    return s->regs[offset / 4];
}

static bool rp2350_resets_write(RP2350RegsState *s, hwaddr offset,
                                uint32_t value)
{
    if (offset == RESETS_RESET) {
        trace_rp2350_resets(value);
    }
    return offset == RESETS_RESET_DONE; /* read-only */
}

static void rp2350_resets_class_init(ObjectClass *klass, const void *data)
{
    RP2350RegsClass *rc = RP2350_REGS_CLASS(klass);

    rc->reset_values = rp2350_resets_reset_values;
    rc->read = rp2350_resets_read;
    rc->write = rp2350_resets_write;
}

/*
 * CLOCKS: SELECTED reports the glitchless mux source at once. Clocks
 * without a glitchless mux have SELECTED hardwired to 1.
 */
static const uint32_t rp2350_clocks_reset_values[] = {
    CLOCKS_CLK_SYS_CTRL, 0x41,
    0, 0
};

static uint32_t rp2350_clocks_read(RP2350RegsState *s, hwaddr offset)
{
    unsigned clk = offset / CLOCKS_CLK_STRIDE;
    uint32_t ctrl;

    if (clk >= CLOCKS_NUM_CLK ||
        offset % CLOCKS_CLK_STRIDE != CLOCKS_SELECTED) {
        return s->regs[offset / 4];
    }
    ctrl = s->regs[clk * CLOCKS_CLK_STRIDE / 4];
    switch (clk) {
    case CLOCKS_CLK_REF:
        return 1u << (ctrl & CLOCKS_CLK_REF_SRC);
    case CLOCKS_CLK_SYS:
        return 1u << (ctrl & CLOCKS_CLK_SYS_SRC);
    default:
        return 1;
    }
}

static bool rp2350_clocks_write(RP2350RegsState *s, hwaddr offset,
                                uint32_t value)
{
    /* SELECTED is read-only */
    return offset / CLOCKS_CLK_STRIDE < CLOCKS_NUM_CLK &&
           offset % CLOCKS_CLK_STRIDE == CLOCKS_SELECTED;
}

static void rp2350_clocks_class_init(ObjectClass *klass, const void *data)
{
    RP2350RegsClass *rc = RP2350_REGS_CLASS(klass);

    rc->reset_values = rp2350_clocks_reset_values;
    rc->read = rp2350_clocks_read;
    rc->write = rp2350_clocks_write;
}

/* XOSC: always enabled and stable. */
static uint32_t rp2350_xosc_read(RP2350RegsState *s, hwaddr offset)
{
    if (offset == XOSC_STATUS) {
        return XOSC_STATUS_STABLE | XOSC_STATUS_ENABLED;
    }
    return s->regs[offset / 4];
}

static void rp2350_xosc_class_init(ObjectClass *klass, const void *data)
{
    RP2350_REGS_CLASS(klass)->read = rp2350_xosc_read;
}

/* PLL: always locked. */
static const uint32_t rp2350_pll_reset_values[] = {
    0x000, 0x00000001,  /* CS */
    0x004, 0x0000002d,  /* PWR */
    0x00c, 0x00077000,  /* PRIM */
    0, 0
};

static uint32_t rp2350_pll_read(RP2350RegsState *s, hwaddr offset)
{
    if (offset == PLL_CS) {
        return s->regs[0] | PLL_CS_LOCK;
    }
    return s->regs[offset / 4];
}

static void rp2350_pll_class_init(ObjectClass *klass, const void *data)
{
    RP2350RegsClass *rc = RP2350_REGS_CLASS(klass);

    rc->reset_values = rp2350_pll_reset_values;
    rc->read = rp2350_pll_read;
}

/* TICKS: a generator runs as soon as it is enabled. */
static uint32_t rp2350_ticks_read(RP2350RegsState *s, hwaddr offset)
{
    uint32_t value = s->regs[offset / 4];

    if (offset < TICKS_NUM_GEN * TICKS_GEN_STRIDE &&
        offset % TICKS_GEN_STRIDE == 0) {
        value &= ~TICKS_CTRL_RUNNING;
        if (value & TICKS_CTRL_ENABLE) {
            value |= TICKS_CTRL_RUNNING;
        }
    }
    return value;
}

static void rp2350_ticks_class_init(ObjectClass *klass, const void *data)
{
    RP2350_REGS_CLASS(klass)->read = rp2350_ticks_read;
}

/*
 * BOOTRAM: SRAM, two write-once registers, and the boot locks, which
 * behave like the SIO spinlocks (pico-sdk hardware_regs/bootram.h).
 */
static const uint32_t rp2350_bootram_reset_values[] = {
    BOOTRAM_BOOTLOCK_STAT, MAKE_64BIT_MASK(0, BOOTRAM_NUM_BOOTLOCKS),
    0, 0
};

static bool rp2350_bootlock_index(hwaddr offset, unsigned *n)
{
    if (offset < BOOTRAM_BOOTLOCK0 ||
        offset >= BOOTRAM_BOOTLOCK0 + 4 * BOOTRAM_NUM_BOOTLOCKS) {
        return false;
    }
    *n = (offset - BOOTRAM_BOOTLOCK0) / 4;
    return true;
}

static uint32_t rp2350_bootram_read(RP2350RegsState *s, hwaddr offset)
{
    uint32_t *stat = &s->regs[BOOTRAM_BOOTLOCK_STAT / 4];
    unsigned n;

    if (!rp2350_bootlock_index(offset, &n)) {
        return s->regs[offset / 4];
    }
    /* Read to claim: 1 << n on success, 0 if already claimed */
    if (*stat & BIT(n)) {
        *stat &= ~BIT(n);
        return BIT(n);
    }
    return 0;
}

static bool rp2350_bootram_write(RP2350RegsState *s, hwaddr offset,
                                 uint32_t value)
{
    unsigned n;

    if (offset < BOOTRAM_SRAM_SIZE) {
        return false;
    }
    if (offset == BOOTRAM_WRITE_ONCE0 || offset == BOOTRAM_WRITE_ONCE1) {
        s->regs[offset / 4] |= value;
    } else if (rp2350_bootlock_index(offset, &n)) {
        /* Write to unclaim */
        s->regs[BOOTRAM_BOOTLOCK_STAT / 4] |= BIT(n);
    }
    return true;
}

static void rp2350_bootram_class_init(ObjectClass *klass, const void *data)
{
    RP2350RegsClass *rc = RP2350_REGS_CLASS(klass);

    rc->reset_values = rp2350_bootram_reset_values;
    rc->read = rp2350_bootram_read;
    rc->write = rp2350_bootram_write;
}

static const TypeInfo rp2350_regs_types[] = {
    {
        .name = TYPE_RP2350_REGS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350RegsState),
        .instance_init = rp2350_regs_init,
        .class_size = sizeof(RP2350RegsClass),
        .class_init = rp2350_regs_class_init,
        .abstract = true,
    }, {
        .name = TYPE_RP2350_RESETS,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_resets_class_init,
    }, {
        .name = TYPE_RP2350_CLOCKS,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_clocks_class_init,
    }, {
        .name = TYPE_RP2350_XOSC,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_xosc_class_init,
    }, {
        .name = TYPE_RP2350_PLL,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_pll_class_init,
    }, {
        .name = TYPE_RP2350_TICKS,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_ticks_class_init,
    }, {
        /* shortcut: plain storage; no GPIO interrupts or status */
        .name = TYPE_RP2350_IO_BANK0,
        .parent = TYPE_RP2350_REGS,
    }, {
        /* shortcut: plain storage with zero reset values */
        .name = TYPE_RP2350_PADS_BANK0,
        .parent = TYPE_RP2350_REGS,
    }, {
        .name = TYPE_RP2350_BOOTRAM,
        .parent = TYPE_RP2350_REGS,
        .class_init = rp2350_bootram_class_init,
    },
};

DEFINE_TYPES(rp2350_regs_types)
