/*
 * RP2350 Cortex-M33 coprocessors: GPIO coprocessor (cp0) and
 * double-precision coprocessor (cp4)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RP2350_COPROC_H
#define HW_ARM_RP2350_COPROC_H

#include "hw/gpio/rp2350_sio.h"
#include "target/arm/cpu-qom.h"

#define RP2350_CP_GPIOC     0
#define RP2350_CP_DCP       4
/* value for the armv7m "coprocessors" property */
#define RP2350_COPROCESSORS ((1 << RP2350_CP_GPIOC) | (1 << RP2350_CP_DCP))

/* Register the coprocessor instructions on @cpu (after it is realized) */
void rp2350_coproc_init(ARMCPU *cpu, RP2350SIOState *sio);

#endif
