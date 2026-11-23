/*
 * RP2350 TRNG and SHA-256 peripherals
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RP2350_CRYPTO_H
#define HW_MISC_RP2350_CRYPTO_H

#include "hw/core/sysbus.h"

#define TYPE_RP2350_TRNG "rp2350-trng"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350TRNGState, RP2350_TRNG)

struct RP2350TRNGState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t imr, isr, config, sample_count, debug;
    uint32_t entropy[6];
    bool enabled;
};

#define TYPE_RP2350_SHA256 "rp2350-sha256"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350SHA256State, RP2350_SHA256)

struct RP2350SHA256State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t csr;
    uint32_t digest[8];
    uint8_t block[64];
    uint8_t used;
};

#endif
