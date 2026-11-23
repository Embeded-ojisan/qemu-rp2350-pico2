/*
 * Cortex-M33 setup shared by the RP2350 TrustZone test firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RP2350_TRUSTZONE_TEST_H
#define RP2350_TRUSTZONE_TEST_H

#include <arm_cmse.h>
#include "pico/stdlib.h"
#include "hardware/regs/m33.h"
#include "hardware/sync.h"

#define REG(base, offset) (*(io_rw_32 *)((base) + (offset)))
#define M33(offset) REG(PPB_BASE, offset)

typedef unsigned __attribute__((cmse_nonsecure_call)) ns_fn(void);

static inline void sau_region(unsigned region, uint32_t base, uint32_t end)
{
    M33(M33_SAU_RNR_OFFSET) = region;
    M33(M33_SAU_RBAR_OFFSET) = base;
    M33(M33_SAU_RLAR_OFFSET) = (end & M33_SAU_RLAR_LADDR_BITS) |
                             M33_SAU_RLAR_ENABLE_BITS;
}

static inline void sau_enable(bool enable)
{
    M33(M33_SAU_CTRL_OFFSET) = enable ? M33_SAU_CTRL_ENABLE_BITS : 0;
    __dsb();
    __isb();
}

static inline void prepare_ns(void)
{
    M33(M33_SHCSR_OFFSET) |= M33_SHCSR_BUSFAULTENA_BITS;
    sau_region(0, SRAM_SCRATCH_X_BASE, SRAM_SCRATCH_Y_BASE - 1);
    __asm__ volatile("msr msp_ns, %0" : : "r"(SRAM_SCRATCH_Y_BASE) : "memory");
}

static inline bool precise_busfault_at(uint32_t address)
{
    const uint32_t mask = M33_CFSR_BFSR_PRECISERR_BITS |
                          M33_CFSR_BFSR_BFARVALID_BITS;

    return (M33(M33_CFSR_OFFSET) & mask) == mask &&
           M33(M33_BFAR_OFFSET) == address;
}

static inline void __attribute__((noreturn)) wait_for_stop(void)
{
    for (;;) {
        tight_loop_contents();
    }
}

#endif
