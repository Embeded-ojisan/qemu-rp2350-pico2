/*
 * RP2350 64-bit microsecond timer with four alarms (TIMER0/TIMER1)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RP2350_TIMER_H
#define HW_TIMER_RP2350_TIMER_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RP2350_TIMER "rp2350-timer"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350TimerState, RP2350_TIMER)

#define RP2350_TIMER_NUM_ALARMS 4

struct RP2350TimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer *alarm_timer[RP2350_TIMER_NUM_ALARMS];
    qemu_irq irq[RP2350_TIMER_NUM_ALARMS];

    /* time = virtual clock in us - base_us, unless paused */
    int64_t base_us;
    uint64_t paused_time;
    uint32_t timelw;
    uint32_t latched_hi;
    uint32_t alarm[RP2350_TIMER_NUM_ALARMS];
    uint32_t armed;
    uint32_t dbgpause;
    uint32_t pause;
    uint32_t locked;
    uint32_t source;
    uint32_t intr;
    uint32_t inte;
    uint32_t intf;
};

#endif
