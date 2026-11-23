/*
 * RP2350 OTP used by the stub ROM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2350_OTP_H
#define HW_MISC_RP2350_OTP_H

#include "hw/core/sysbus.h"

#define TYPE_RP2350_OTP "rp2350-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350OTPState, RP2350_OTP)

#define RP2350_OTP_PAGES 64
#define RP2350_OTP_ROWS (RP2350_OTP_PAGES * 64)

struct RP2350OTPState {
    SysBusDevice parent_obj;
    MemoryRegion ctrl, data;
    uint32_t rows[RP2350_OTP_ROWS];
    uint8_t sw_lock[RP2350_OTP_PAGES];
    uint32_t selected_row;
};

#endif
