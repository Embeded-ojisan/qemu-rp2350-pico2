/*
 * RP2350 DMA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_RP2350_DMA_H
#define HW_DMA_RP2350_DMA_H

#include "hw/core/sysbus.h"

#define TYPE_RP2350_DMA "rp2350-dma"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350DMAState, RP2350_DMA)

#define RP2350_DMA_CHANNELS 16
#define RP2350_DMA_IRQS 4
#define RP2350_DMA_MPU_REGIONS 8

typedef struct RP2350DMAChannel {
    uint32_t read;
    uint32_t write;
    uint32_t reload;
    uint32_t count;
    uint32_t ctrl;
    uint32_t errors;
    uint8_t security;
    bool busy;
} RP2350DMAChannel;

struct RP2350DMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    AddressSpace *bus;
    QEMUTimer *timer;
    qemu_irq irq[RP2350_DMA_IRQS];
    RP2350DMAChannel channels[RP2350_DMA_CHANNELS];
    uint32_t intr;
    uint32_t inte[RP2350_DMA_IRQS];
    uint32_t intf[RP2350_DMA_IRQS];
    uint8_t irq_security[RP2350_DMA_IRQS];
    uint16_t misc_security;
    uint32_t mpu_ctrl;
    uint32_t mpu_bar[RP2350_DMA_MPU_REGIONS];
    uint32_t mpu_lar[RP2350_DMA_MPU_REGIONS];
    bool held_reset;
};

#endif
