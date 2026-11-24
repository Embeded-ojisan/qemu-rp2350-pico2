/*
 * RP2350 PIO, subset for FIFO-driven GPIO output
 *
 * See RP2350 datasheet chapter 11 and pico-sdk 2.3.1 hardware/regs/pio.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-clock.h"
#include "hw/gpio/rp2350_pio.h"
#include "hw/misc/rp2350_regs.h"
#include "migration/vmstate.h"

#define PIO_CTRL            0x000
#define PIO_FSTAT           0x004
#define PIO_FDEBUG          0x008
#define PIO_FLEVEL          0x00c
#define PIO_TXF0            0x010
#define PIO_RXF0            0x020
#define PIO_DBG_PADOUT      0x03c
#define PIO_DBG_PADOE       0x040
#define PIO_DBG_CFGINFO     0x044
#define PIO_INSTR_MEM0      0x048
#define PIO_SM0             0x0c8
#define PIO_SM_STRIDE       0x018
#define PIO_GPIOBASE        0x168

#define SM_CLKDIV           0x00
#define SM_EXECCTRL         0x04
#define SM_SHIFTCTRL        0x08
#define SM_ADDR             0x0c
#define SM_INSTR            0x10
#define SM_PINCTRL          0x14

#define EXEC_STALLED        BIT(31)
#define FJOIN_RX            BIT(31)
#define FJOIN_TX            BIT(30)
#define OUT_SHIFTDIR        BIT(19)
#define AUTOPULL            BIT(17)
#define TX_DEPTH            4
#define TX_STALL(i)         BIT(24 + (i))
#define TX_OVER(i)          BIT(16 + (i))
#define RX_UNDER(i)         BIT(8 + (i))

static unsigned tx_capacity(RP2350PIOSM *sm)
{
    if (sm->shiftctrl & FJOIN_RX) {
        return 0;
    }
    return sm->shiftctrl & FJOIN_TX ? 2 * TX_DEPTH : TX_DEPTH;
}

static void pio_write_pins(RP2350PIOState *s, uint32_t *pins,
                           unsigned base, unsigned count, uint32_t value)
{
    if (count) {
        uint32_t mask = MAKE_64BIT_MASK(0, count);
        uint32_t next = (*pins & ~rol32(mask, base)) |
                        rol32(value & mask, base);
        uint32_t changed = *pins ^ next;

        *pins = next;
        rp2350_io_bank0_update(s->io_bank0, (uint64_t)changed << s->gpio_base);
    }
}

static bool pio_execute(RP2350PIOSM *sm, uint16_t insn, bool forced)
{
    RP2350PIOState *s = sm->pio;
    unsigned dest = extract32(insn, 5, 3);
    unsigned arg = extract32(insn, 0, 5);
    unsigned wrap_top = extract32(sm->execctrl, 12, 5);
    unsigned next = sm->pc == wrap_top ? extract32(sm->execctrl, 7, 5) :
                                        (sm->pc + 1) & 31;

    switch (insn >> 13) {
    case 0: /* JMP */
        if (dest != 0) {
            goto unsupported;
        }
        sm->pc = arg;
        return true;
    case 3: { /* OUT */
        unsigned count = arg ? arg : 32;
        uint32_t value;

        if ((sm->shiftctrl & AUTOPULL) ||
            (dest != 0 && dest != 3 && dest != 4)) {
            goto unsupported;
        }
        if (sm->shiftctrl & OUT_SHIFTDIR) {
            value = extract32(sm->osr, 0, count);
            sm->osr = count == 32 ? 0 : sm->osr >> count;
        } else {
            value = extract32(sm->osr, 32 - count, count);
            sm->osr = count == 32 ? 0 : sm->osr << count;
        }
        sm->osr_count = MIN(32, sm->osr_count + count);
        if (dest != 3) {
            pio_write_pins(s, dest == 0 ? &s->padout : &s->padoe,
                           extract32(sm->pinctrl, 0, 5),
                           MIN(32, extract32(sm->pinctrl, 20, 6)), value);
        }
        break;
    }
    case 4: { /* PULL */
        unsigned threshold = extract32(sm->shiftctrl, 25, 5);

        if (!(insn & BIT(7)) || (sm->shiftctrl & AUTOPULL)) {
            goto unsupported;
        }
        if ((insn & BIT(6)) && sm->osr_count < (threshold ? threshold : 32)) {
            break;
        }
        if (!fifo32_is_empty(&sm->tx)) {
            sm->osr = fifo32_pop(&sm->tx);
        } else if (insn & BIT(5)) {
            s->fdebug |= TX_STALL(sm->index);
            return false;
        } else {
            sm->osr = sm->x;
        }
        sm->osr_count = 0;
        break;
    }
    case 5: /* MOV Y, Y (NOP) */
        if ((insn & 0xff) != 0x42) {
            goto unsupported;
        }
        break;
    case 7: /* SET */
        if (dest == 1) {
            sm->x = arg;
        } else if (dest == 0 || dest == 4) {
            pio_write_pins(s, dest == 0 ? &s->padout : &s->padoe,
                           extract32(sm->pinctrl, 5, 5),
                           MIN(5, extract32(sm->pinctrl, 26, 3)), arg);
        } else {
            goto unsupported;
        }
        break;
    default:
    unsupported:
        qemu_log_mask(LOG_UNIMP, "rp2350-pio: SM%u instruction %04x\n",
                      sm->index, insn);
        break;
    }
    if (!forced) {
        sm->pc = next;
    }
    return true;
}

static void pio_schedule(RP2350PIOSM *sm)
{
    RP2350PIOState *s = sm->pio;
    unsigned hz = clock_get_hz(s->clk);
    uint32_t div = sm->clkdiv >> 8;
    uint64_t ns;

    if (!(s->enabled & BIT(sm->index)) || timer_pending(sm->timer) ||
        (sm->waiting_tx && fifo32_is_empty(&sm->tx)) || !hz) {
        return;
    }

    /* shortcut: round each interval to ns until cycle timing is needed. */
    if (div < 256) {
        div = 65536u << 8;
    }
    ns = DIV_ROUND_UP((uint64_t)div * NANOSECONDS_PER_SECOND * (1 + sm->delay),
                      (uint64_t)hz * 256);
    timer_mod_ns(sm->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void pio_step(void *opaque)
{
    RP2350PIOSM *sm = opaque;
    uint16_t insn = sm->pio->imem[sm->pc];

    sm->waiting_tx = !pio_execute(sm, insn, false);
    /* Side-set is outside this model. The sample uses all five delay bits. */
    sm->delay = sm->waiting_tx ? 0 : extract32(insn, 8, 5);
    pio_schedule(sm);
}

static void pio_force(RP2350PIOSM *sm, uint16_t insn)
{
    sm->waiting_tx = false;
    sm->forced_insn = insn;
    sm->forced_stalled = !pio_execute(sm, insn, true);
    if (sm->forced_stalled) {
        timer_del(sm->timer);
    } else {
        pio_schedule(sm);
    }
}

static void pio_write_ctrl(RP2350PIOState *s, uint32_t value)
{
    s->enabled = value & MAKE_64BIT_MASK(0, RP2350_PIO_SM_COUNT);
    for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
        RP2350PIOSM *sm = &s->sm[i];

        if (value & BIT(4 + i)) {
            sm->osr_count = 32;
            sm->delay = 0;
            sm->waiting_tx = false;
            sm->forced_stalled = false;
        }
        if (!(s->enabled & BIT(i)) || (value & BIT(8 + i))) {
            timer_del(sm->timer);
        }
        if (!sm->forced_stalled) {
            pio_schedule(sm);
        }
    }
}

static uint64_t pio_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2350PIOState *s = opaque;
    uint32_t value = 0;

    if (s->held_reset) {
        return 0;
    }
    switch (addr) {
    case PIO_CTRL:
        return s->enabled;
    case PIO_FSTAT:
        for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
            RP2350PIOSM *sm = &s->sm[i];

            value |= BIT(8 + i);
            if (sm->shiftctrl & FJOIN_TX) {
                value |= BIT(i);
            }
            if (fifo32_is_empty(&sm->tx)) {
                value |= BIT(24 + i);
            }
            if (fifo32_num_used(&sm->tx) >= tx_capacity(sm)) {
                value |= BIT(16 + i);
            }
        }
        return value;
    case PIO_FDEBUG:
        return s->fdebug;
    case PIO_FLEVEL:
        for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
            value |= fifo32_num_used(&s->sm[i].tx) << (8 * i);
        }
        return value;
    case PIO_RXF0 ... PIO_RXF0 + 4 * RP2350_PIO_SM_COUNT - 1:
        s->fdebug |= RX_UNDER((addr - PIO_RXF0) / 4);
        return 0;
    case PIO_DBG_PADOUT:
        return s->padout;
    case PIO_DBG_PADOE:
        return s->padoe;
    case PIO_DBG_CFGINFO:
        return BIT(28) | (RP2350_PIO_IMEM_SIZE << 16) |
               (RP2350_PIO_SM_COUNT << 8) | TX_DEPTH;
    case PIO_GPIOBASE:
        return s->gpio_base;
    case PIO_SM0 ... PIO_SM0 + PIO_SM_STRIDE * RP2350_PIO_SM_COUNT - 1: {
        RP2350PIOSM *sm = &s->sm[(addr - PIO_SM0) / PIO_SM_STRIDE];

        switch ((addr - PIO_SM0) % PIO_SM_STRIDE) {
        case SM_CLKDIV:
            return sm->clkdiv;
        case SM_EXECCTRL:
            return sm->execctrl | (sm->forced_stalled ? EXEC_STALLED : 0);
        case SM_SHIFTCTRL:
            return sm->shiftctrl;
        case SM_ADDR:
            return sm->pc;
        case SM_INSTR:
            return s->imem[sm->pc];
        case SM_PINCTRL:
            return sm->pinctrl;
        }
        break;
    }
    }
    qemu_log_mask(LOG_UNIMP, "rp2350-pio: read at 0x%" HWADDR_PRIx "\n", addr);
    return 0;
}

static void pio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RP2350PIOState *s = opaque;

    if (s->held_reset) {
        return;
    }
    switch (addr) {
    case PIO_CTRL:
        pio_write_ctrl(s, value);
        return;
    case PIO_FDEBUG:
        s->fdebug &= ~value;
        return;
    case PIO_TXF0 ... PIO_TXF0 + 4 * RP2350_PIO_SM_COUNT - 1: {
        RP2350PIOSM *sm = &s->sm[(addr - PIO_TXF0) / 4];

        if (fifo32_num_used(&sm->tx) >= tx_capacity(sm)) {
            s->fdebug |= TX_OVER(sm->index);
            return;
        }
        fifo32_push(&sm->tx, value);
        if (sm->forced_stalled) {
            pio_force(sm, sm->forced_insn);
        } else {
            pio_schedule(sm);
        }
        return;
    }
    case PIO_INSTR_MEM0 ... PIO_INSTR_MEM0 + 4 * RP2350_PIO_IMEM_SIZE - 1:
        s->imem[(addr - PIO_INSTR_MEM0) / 4] = value;
        return;
    case PIO_GPIOBASE: {
        unsigned old_base = s->gpio_base;

        s->gpio_base = value & BIT(4);
        if (old_base != s->gpio_base) {
            rp2350_io_bank0_update(s->io_bank0,
                                  ((uint64_t)UINT32_MAX << old_base) |
                                  ((uint64_t)UINT32_MAX << s->gpio_base));
        }
        return;
    }
    case PIO_SM0 ... PIO_SM0 + PIO_SM_STRIDE * RP2350_PIO_SM_COUNT - 1: {
        RP2350PIOSM *sm = &s->sm[(addr - PIO_SM0) / PIO_SM_STRIDE];

        switch ((addr - PIO_SM0) % PIO_SM_STRIDE) {
        case SM_CLKDIV:
            sm->clkdiv = value & 0xffffff00;
            return;
        case SM_EXECCTRL:
            sm->execctrl = value & ~EXEC_STALLED;
            return;
        case SM_SHIFTCTRL:
            if ((sm->shiftctrl ^ value) & (FJOIN_TX | FJOIN_RX)) {
                fifo32_reset(&sm->tx);
            }
            sm->shiftctrl = value & 0xffffc01f;
            return;
        case SM_INSTR:
            pio_force(sm, value);
            return;
        case SM_PINCTRL:
            sm->pinctrl = value;
            return;
        }
        break;
    }
    }
    qemu_log_mask(LOG_UNIMP, "rp2350-pio: write at 0x%" HWADDR_PRIx "\n", addr);
}

static const MemoryRegionOps pio_ops = {
    .read = pio_read,
    .write = pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pio_reset_input(void *opaque, int line, int level)
{
    RP2350PIOState *s = opaque;

    s->held_reset = level;
    if (!level) {
        return;
    }
    s->enabled = s->gpio_base = 0;
    s->fdebug = s->padout = s->padoe = 0;
    memset(s->imem, 0, sizeof(s->imem));
    for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
        RP2350PIOSM *sm = &s->sm[i];

        timer_del(sm->timer);
        fifo32_reset(&sm->tx);
        sm->clkdiv = 0x00010000;
        sm->execctrl = 0x0001f000;
        sm->shiftctrl = 0x000c0000;
        sm->pinctrl = 0x14000000;
        sm->osr = sm->x = sm->pc = sm->delay = sm->forced_insn = 0;
        sm->osr_count = 32;
        sm->waiting_tx = sm->forced_stalled = false;
    }
    rp2350_io_bank0_update(s->io_bank0, UINT64_MAX);
}

static void pio_reset_hold(Object *obj, ResetType type)
{
    pio_reset_input(RP2350_PIO(obj), 0, 1);
}

static void pio_init(Object *obj)
{
    RP2350PIOState *s = RP2350_PIO(obj);

    memory_region_init_io(&s->iomem, obj, &pio_ops, s, TYPE_RP2350_PIO,
                          RP2350_ATOMIC_BLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
    qdev_init_gpio_in_named(DEVICE(obj), pio_reset_input, "reset", 1);
    for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
        RP2350PIOSM *sm = &s->sm[i];

        sm->pio = s;
        sm->index = i;
        sm->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pio_step, sm);
        fifo32_create(&sm->tx, 2 * TX_DEPTH);
    }
}

static void pio_finalize(Object *obj)
{
    RP2350PIOState *s = RP2350_PIO(obj);

    for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
        timer_free(s->sm[i].timer);
        fifo32_destroy(&s->sm[i].tx);
    }
}

static const VMStateDescription vmstate_pio_sm = {
    .name = TYPE_RP2350_PIO "/sm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, RP2350PIOSM),
        VMSTATE_FIFO32(tx, RP2350PIOSM),
        VMSTATE_UINT32(clkdiv, RP2350PIOSM),
        VMSTATE_UINT32(execctrl, RP2350PIOSM),
        VMSTATE_UINT32(shiftctrl, RP2350PIOSM),
        VMSTATE_UINT32(pinctrl, RP2350PIOSM),
        VMSTATE_UINT32(osr, RP2350PIOSM),
        VMSTATE_UINT32(x, RP2350PIOSM),
        VMSTATE_UINT16(forced_insn, RP2350PIOSM),
        VMSTATE_UINT8(pc, RP2350PIOSM),
        VMSTATE_UINT8(osr_count, RP2350PIOSM),
        VMSTATE_UINT8(delay, RP2350PIOSM),
        VMSTATE_BOOL(forced_stalled, RP2350PIOSM),
        VMSTATE_BOOL(waiting_tx, RP2350PIOSM),
        VMSTATE_END_OF_LIST()
    },
};

static int pio_post_load(void *opaque, int version)
{
    RP2350PIOState *s = opaque;

    if (s->gpio_base != 0 && s->gpio_base != 16) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < RP2350_PIO_SM_COUNT; i++) {
        RP2350PIOSM *sm = &s->sm[i];

        if (sm->pc >= RP2350_PIO_IMEM_SIZE || sm->osr_count > 32 ||
            sm->delay > 31 || fifo32_num_used(&sm->tx) > tx_capacity(sm) ||
            sm->tx.fifo.head >= sm->tx.fifo.capacity ||
            (sm->tx.fifo.head | sm->tx.fifo.num) % sizeof(uint32_t)) {
            return -EINVAL;
        }
    }
    rp2350_io_bank0_update(s->io_bank0, UINT64_MAX);
    return 0;
}

static const VMStateDescription vmstate_pio = {
    .name = TYPE_RP2350_PIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(sm, RP2350PIOState, RP2350_PIO_SM_COUNT, 1,
                             vmstate_pio_sm, RP2350PIOSM),
        VMSTATE_UINT16_ARRAY(imem, RP2350PIOState, RP2350_PIO_IMEM_SIZE),
        VMSTATE_UINT32(padout, RP2350PIOState),
        VMSTATE_UINT32(padoe, RP2350PIOState),
        VMSTATE_UINT32(fdebug, RP2350PIOState),
        VMSTATE_UINT8(enabled, RP2350PIOState),
        VMSTATE_UINT8(gpio_base, RP2350PIOState),
        VMSTATE_BOOL(held_reset, RP2350PIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void pio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = pio_reset_hold;
    dc->vmsd = &vmstate_pio;
    dc->user_creatable = false; /* Uses the SoC clock and IO_BANK0. */
}

static const TypeInfo pio_types[] = {
    {
        .name = TYPE_RP2350_PIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350PIOState),
        .instance_init = pio_init,
        .instance_finalize = pio_finalize,
        .class_init = pio_class_init,
    },
};

DEFINE_TYPES(pio_types)
