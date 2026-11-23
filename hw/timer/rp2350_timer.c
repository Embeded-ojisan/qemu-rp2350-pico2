/*
 * RP2350 64-bit microsecond timer with four alarms (TIMER0/TIMER1)
 *
 * The timer counts 1 us ticks derived from QEMU's virtual clock. The tick
 * source selection (SOURCE, and the TICKS block) and debug pause are
 * accepted but ignored. Register offsets follow pico-sdk 2.3.1
 * src/rp2350/hardware_regs/include/hardware/regs/timer.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/timer/rp2350_timer.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TIMER_TIMEHW    0x00
#define TIMER_TIMELW    0x04
#define TIMER_TIMEHR    0x08
#define TIMER_TIMELR    0x0c
#define TIMER_ALARM0    0x10
#define TIMER_ARMED     0x20
#define TIMER_TIMERAWH  0x24
#define TIMER_TIMERAWL  0x28
#define TIMER_DBGPAUSE  0x2c
#define TIMER_PAUSE     0x30
#define TIMER_LOCKED    0x34
#define TIMER_SOURCE    0x38
#define TIMER_INTR      0x3c
#define TIMER_INTE      0x40
#define TIMER_INTF      0x44
#define TIMER_INTS      0x48

#define TIMER_ALARM_MASK MAKE_64BIT_MASK(0, RP2350_TIMER_NUM_ALARMS)

static int64_t rp2350_timer_clock_us(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_US;
}

static uint64_t rp2350_timer_get_time(RP2350TimerState *s)
{
    if (s->pause) {
        return s->paused_time;
    }
    return rp2350_timer_clock_us() - s->base_us;
}

static void rp2350_timer_set_time(RP2350TimerState *s, uint64_t time)
{
    s->paused_time = time;
    s->base_us = rp2350_timer_clock_us() - time;
}

static void rp2350_timer_update_irq(RP2350TimerState *s)
{
    uint32_t ints = (s->intr | s->intf) & s->inte;

    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        qemu_set_irq(s->irq[i], extract32(ints, i, 1));
    }
}

static void rp2350_timer_fire(RP2350TimerState *s, int n)
{
    trace_rp2350_timer_alarm_fire(n, (uint32_t)rp2350_timer_get_time(s));
    s->armed &= ~BIT(n);
    s->intr |= BIT(n);
    timer_del(s->alarm_timer[n]);
    rp2350_timer_update_irq(s);
}

/* Schedule alarm @n for the next time the low 32 bits equal ALARMn. */
static void rp2350_timer_schedule(RP2350TimerState *s, int n)
{
    uint32_t now = rp2350_timer_get_time(s);
    uint32_t delta = s->alarm[n] - now;

    if (!(s->armed & BIT(n))) {
        timer_del(s->alarm_timer[n]);
        return;
    }
    if (s->pause) {
        /* rescheduled when the timer is unpaused */
        timer_del(s->alarm_timer[n]);
        return;
    }
    if (delta == 0) {
        rp2350_timer_fire(s, n);
        return;
    }
    timer_mod(s->alarm_timer[n], (s->base_us + now + delta) * SCALE_US);
}

static void rp2350_timer_reschedule_all(RP2350TimerState *s)
{
    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        rp2350_timer_schedule(s, i);
    }
}

static void rp2350_timer_alarm_cb(void *opaque)
{
    RP2350TimerState *s = opaque;

    /* An armed alarm whose QEMU timer is no longer pending has expired */
    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        if ((s->armed & BIT(i)) && !timer_pending(s->alarm_timer[i])) {
            rp2350_timer_fire(s, i);
        }
    }
}

static uint64_t rp2350_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    RP2350TimerState *s = opaque;
    uint64_t time;

    switch (offset) {
    case TIMER_TIMEHR:
        return s->latched_hi;
    case TIMER_TIMELR:
        time = rp2350_timer_get_time(s);
        s->latched_hi = time >> 32;
        return (uint32_t)time;
    case TIMER_ALARM0 ... TIMER_ALARM0 + 4 * RP2350_TIMER_NUM_ALARMS - 1:
        return s->alarm[(offset - TIMER_ALARM0) / 4];
    case TIMER_ARMED:
        return s->armed;
    case TIMER_TIMERAWH:
        return rp2350_timer_get_time(s) >> 32;
    case TIMER_TIMERAWL:
        return (uint32_t)rp2350_timer_get_time(s);
    case TIMER_DBGPAUSE:
        return s->dbgpause;
    case TIMER_PAUSE:
        return s->pause;
    case TIMER_LOCKED:
        return s->locked;
    case TIMER_SOURCE:
        return s->source;
    case TIMER_INTR:
        return s->intr;
    case TIMER_INTE:
        return s->inte;
    case TIMER_INTF:
        return s->intf;
    case TIMER_INTS:
        return (s->intr | s->intf) & s->inte;
    case TIMER_TIMEHW:
    case TIMER_TIMELW:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void rp2350_timer_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RP2350TimerState *s = opaque;
    int n;

    if (s->locked) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to locked timer\n",
                      __func__);
        return;
    }

    switch (offset) {
    case TIMER_TIMELW:
        s->timelw = value;
        break;
    case TIMER_TIMEHW:
        rp2350_timer_set_time(s, deposit64(s->timelw, 32, 32, value));
        rp2350_timer_reschedule_all(s);
        break;
    case TIMER_ALARM0 ... TIMER_ALARM0 + 4 * RP2350_TIMER_NUM_ALARMS - 1:
        n = (offset - TIMER_ALARM0) / 4;
        s->alarm[n] = value;
        s->armed |= BIT(n);
        trace_rp2350_timer_alarm_set(n, value,
                                     (uint32_t)rp2350_timer_get_time(s));
        rp2350_timer_schedule(s, n);
        break;
    case TIMER_ARMED:
        s->armed &= ~(value & TIMER_ALARM_MASK);
        rp2350_timer_reschedule_all(s);
        break;
    case TIMER_DBGPAUSE:
        s->dbgpause = value & 0x6;
        break;
    case TIMER_PAUSE:
        if ((value & 1) != s->pause) {
            uint64_t time = rp2350_timer_get_time(s);

            s->pause = value & 1;
            rp2350_timer_set_time(s, time);
            rp2350_timer_reschedule_all(s);
        }
        break;
    case TIMER_LOCKED:
        s->locked = value & 1;
        break;
    case TIMER_SOURCE:
        s->source = value & 1;
        break;
    case TIMER_INTR:
        s->intr &= ~(value & TIMER_ALARM_MASK);
        rp2350_timer_update_irq(s);
        break;
    case TIMER_INTE:
        s->inte = value & TIMER_ALARM_MASK;
        rp2350_timer_update_irq(s);
        break;
    case TIMER_INTF:
        s->intf = value & TIMER_ALARM_MASK;
        rp2350_timer_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps rp2350_timer_ops = {
    .read = rp2350_timer_read,
    .write = rp2350_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rp2350_timer_reset_hold(Object *obj, ResetType type)
{
    RP2350TimerState *s = RP2350_TIMER(obj);

    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        timer_del(s->alarm_timer[i]);
        s->alarm[i] = 0;
    }
    s->pause = 0;
    rp2350_timer_set_time(s, 0);
    s->timelw = 0;
    s->latched_hi = 0;
    s->armed = 0;
    s->dbgpause = 0x6;  /* DBGPAUSE reset value: DBG0 | DBG1 */
    s->locked = 0;
    s->source = 0;
    s->intr = 0;
    s->inte = 0;
    s->intf = 0;
}

static void rp2350_timer_reset_exit(Object *obj, ResetType type)
{
    rp2350_timer_update_irq(RP2350_TIMER(obj));
}

static void rp2350_timer_init(Object *obj)
{
    RP2350TimerState *s = RP2350_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &rp2350_timer_ops, s,
                          TYPE_RP2350_TIMER, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
        s->alarm_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         rp2350_timer_alarm_cb, s);
    }
}

static void rp2350_timer_finalize(Object *obj)
{
    RP2350TimerState *s = RP2350_TIMER(obj);

    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        timer_free(s->alarm_timer[i]);
    }
}

static const VMStateDescription vmstate_rp2350_timer = {
    .name = TYPE_RP2350_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR_ARRAY(alarm_timer, RP2350TimerState,
                                RP2350_TIMER_NUM_ALARMS),
        VMSTATE_INT64(base_us, RP2350TimerState),
        VMSTATE_UINT64(paused_time, RP2350TimerState),
        VMSTATE_UINT32(timelw, RP2350TimerState),
        VMSTATE_UINT32(latched_hi, RP2350TimerState),
        VMSTATE_UINT32_ARRAY(alarm, RP2350TimerState,
                             RP2350_TIMER_NUM_ALARMS),
        VMSTATE_UINT32(armed, RP2350TimerState),
        VMSTATE_UINT32(dbgpause, RP2350TimerState),
        VMSTATE_UINT32(pause, RP2350TimerState),
        VMSTATE_UINT32(locked, RP2350TimerState),
        VMSTATE_UINT32(source, RP2350TimerState),
        VMSTATE_UINT32(intr, RP2350TimerState),
        VMSTATE_UINT32(inte, RP2350TimerState),
        VMSTATE_UINT32(intf, RP2350TimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void rp2350_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rp2350_timer_reset_hold;
    rc->phases.exit = rp2350_timer_reset_exit;
    dc->vmsd = &vmstate_rp2350_timer;
}

static const TypeInfo rp2350_timer_types[] = {
    {
        .name = TYPE_RP2350_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350TimerState),
        .instance_init = rp2350_timer_init,
        .instance_finalize = rp2350_timer_finalize,
        .class_init = rp2350_timer_class_init,
    },
};

DEFINE_TYPES(rp2350_timer_types)
