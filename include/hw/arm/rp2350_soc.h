/*
 * Raspberry Pi RP2350 SoC (Arm side, core 0 only)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RP2350_SOC_H
#define HW_ARM_RP2350_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/char/pl011.h"
#include "hw/core/clock.h"
#include "hw/dma/rp2350_dma.h"
#include "hw/gpio/rp2350_pio.h"
#include "hw/gpio/rp2350_sio.h"
#include "hw/misc/rp2350_accessctrl.h"
#include "hw/misc/rp2350_atomic.h"
#include "hw/misc/rp2350_crypto.h"
#include "hw/misc/rp2350_otp.h"
#include "hw/misc/rp2350_regs.h"
#include "hw/ssi/rp2350_qmi.h"
#include "hw/timer/rp2350_timer.h"
#include "qom/object.h"

#define TYPE_RP2350 "rp2350"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350State, RP2350)

/* Address map, datasheet 2.2 */
#define RP2350_ROM_BASE         0x00000000
#define RP2350_ROM_SIZE         (32 * KiB)  /* datasheet 4.1 */
#define RP2350_XIP_BASE         0x10000000
#define RP2350_SRAM_BASE        0x20000000
#define RP2350_SRAM_SIZE        (520 * KiB) /* up to SRAM_END 0x20082000 */

/* Peripherals with the atomic aliases that this model implements */
enum {
    RP2350_REGS_CLOCKS,
    RP2350_REGS_RESETS,
    RP2350_REGS_IO_BANK0,
    RP2350_REGS_PADS_BANK0,
    RP2350_REGS_XOSC,
    RP2350_REGS_PLL_SYS,
    RP2350_REGS_PLL_USB,
    RP2350_REGS_BOOTRAM,
    RP2350_REGS_TICKS,
    RP2350_REGS_NUM
};

enum {
    RP2350_ALIAS_UART0 = RP2350_REGS_NUM,
    RP2350_ALIAS_TIMER0,
    RP2350_ALIAS_TIMER1,
    RP2350_ALIAS_QMI,
    RP2350_ALIAS_DMA,
    RP2350_ALIAS_TRNG,
    RP2350_ALIAS_SHA256,
    RP2350_ALIAS_OTP,
    RP2350_ALIAS_PIO0,
    RP2350_ALIAS_NUM
};

struct RP2350State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    RP2350RegsState regs[RP2350_REGS_NUM];
    RP2350SIOState sio;
    RP2350PIOState pio0;
    RP2350AccessCtrl accessctrl;
    RP2350DMAState dma;
    AddressSpace dma_bus;
    RP2350TimerState timer[2];
    RP2350QMIState qmi;
    RP2350TRNGState trng;
    RP2350SHA256State sha256;
    RP2350OTPState otp;
    PL011State uart0;
    RP2350AtomicAlias alias[RP2350_ALIAS_NUM];

    MemoryRegion rom;
    MemoryRegion sram;
    MemoryRegion xip_nocache_alias;
    MemoryRegion xip_notranslate_alias;

    Clock *sysclk;
    Clock *refclk;
};

#endif
