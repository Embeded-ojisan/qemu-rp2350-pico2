/*
 * Raspberry Pi RP2350 SoC (Arm side, core 0 only)
 *
 * Address map: RP2350 datasheet (RP-008373) 2.2. Peripherals the firmware
 * does not need yet are unimplemented devices that log their accesses.
 * The ROM holds the stub ROM (roms/rp2350-stub-rom, docs/adr/0001 of the
 * project); its writes to the top of the ROM become trace events.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/rp2350_soc.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "trace.h"

/* clk_sys and the SysTick reference as set up by pico-sdk (fixed here) */
#define RP2350_SYSCLK_HZ        150000000
#define RP2350_REFCLK_HZ        1000000

#define RP2350_NUM_IRQ          52  /* pico-sdk platform_defs.h NUM_IRQS */
#define RP2350_TIMER0_IRQ_0     0   /* pico-sdk intctrl.h */
#define RP2350_UART0_IRQ        33

#define RP2350_SIO_BASE         0xd0000000

/* Stub ROM log ports; keep in sync with roms/rp2350-stub-rom/stub.h */
#define STUB_LOG_BLOCK          (RP2350_ROM_SIZE - 0x10)
#define STUB_LOG_BOOT           (RP2350_ROM_SIZE - 0x0c)
#define STUB_LOG_CALL           (RP2350_ROM_SIZE - 0x08)
#define STUB_LOG_UNIMPL         (RP2350_ROM_SIZE - 0x04)

static const struct {
    const char *type;
    hwaddr base;
} rp2350_regs_blocks[RP2350_REGS_NUM] = {
    [RP2350_REGS_CLOCKS]     = { TYPE_RP2350_CLOCKS,     0x40010000 },
    [RP2350_REGS_RESETS]     = { TYPE_RP2350_RESETS,     0x40020000 },
    [RP2350_REGS_IO_BANK0]   = { TYPE_RP2350_IO_BANK0,   0x40028000 },
    [RP2350_REGS_PADS_BANK0] = { TYPE_RP2350_PADS_BANK0, 0x40038000 },
    [RP2350_REGS_XOSC]       = { TYPE_RP2350_XOSC,       0x40048000 },
    [RP2350_REGS_PLL_SYS]    = { TYPE_RP2350_PLL,        0x40050000 },
    [RP2350_REGS_PLL_USB]    = { TYPE_RP2350_PLL,        0x40058000 },
    [RP2350_REGS_BOOTRAM]    = { TYPE_RP2350_BOOTRAM,    0x400e0000 },
    [RP2350_REGS_TICKS]      = { TYPE_RP2350_TICKS,      0x40108000 },
};

static const hwaddr rp2350_alias_base[RP2350_ALIAS_NUM] = {
    [RP2350_ALIAS_UART0]  = 0x40070000,
    [RP2350_ALIAS_TIMER0] = 0x400b0000,
    [RP2350_ALIAS_QMI]    = 0x400d0000,
};

static const struct {
    const char *name;
    hwaddr base;
    hwaddr size;
} rp2350_unimplemented[] = {
    { "sysinfo",            0x40000000, 0x8000 },
    { "syscfg",             0x40008000, 0x8000 },
    { "psm",                0x40018000, 0x8000 },
    { "io_qspi",            0x40030000, 0x8000 },
    { "pads_qspi",          0x40040000, 0x8000 },
    { "accessctrl",         0x40060000, 0x8000 },
    { "busctrl",            0x40068000, 0x8000 },
    { "uart1",              0x40078000, 0x8000 },
    { "spi0",               0x40080000, 0x8000 },
    { "spi1",               0x40088000, 0x8000 },
    { "i2c0",               0x40090000, 0x8000 },
    { "i2c1",               0x40098000, 0x8000 },
    { "adc",                0x400a0000, 0x8000 },
    { "pwm",                0x400a8000, 0x8000 },
    { "timer1",             0x400b8000, 0x8000 },
    { "hstx_ctrl",          0x400c0000, 0x8000 },
    { "xip_ctrl",           0x400c8000, 0x8000 },
    { "watchdog",           0x400d8000, 0x8000 },
    { "rosc",               0x400e8000, 0x8000 },
    { "trng",               0x400f0000, 0x8000 },
    { "sha256",             0x400f8000, 0x8000 },
    { "powman",             0x40100000, 0x8000 },
    { "otp",                0x40120000, 0x10000 },
    { "otp_data",           0x40130000, 0x10000 },
    { "coresight_periph",   0x40140000, 0x18000 },
    { "glitch_detector",    0x40158000, 0x8000 },
    { "tbman",              0x40160000, 0x8000 },
    { "dma",                0x50000000, 0x100000 },
    { "usbctrl",            0x50100000, 0x100000 },
    { "pio0",               0x50200000, 0x100000 },
    { "pio1",               0x50300000, 0x100000 },
    { "pio2",               0x50400000, 0x100000 },
    { "xip_aux",            0x50500000, 0x100000 },
    { "hstx_fifo",          0x50600000, 0x100000 },
    { "coresight_trace",    0x50700000, 0x100000 },
    { "xip_maintenance",    0x18000000, 0x4000000 },
    { "sio_nonsec",         0xd0020000, 0x20000 },
};

static uint64_t rp2350_rom_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Only reached outside ROMD mode, which this model never leaves */
    return 0;
}

static void rp2350_rom_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    switch (addr) {
    case STUB_LOG_BLOCK:
        trace_rp2350_stub_rom_block(value);
        break;
    case STUB_LOG_BOOT:
        trace_rp2350_stub_rom_boot(value);
        break;
    case STUB_LOG_CALL:
        trace_rp2350_stub_rom_call(value & 0xff, (value >> 8) & 0xff);
        break;
    case STUB_LOG_UNIMPL:
        trace_rp2350_stub_rom_unimplemented(value & 0xff, (value >> 8) & 0xff);
        qemu_log_mask(LOG_UNIMP, "rp2350 stub ROM: ROM function '%c%c' is "
                      "not implemented\n", (int)(value & 0xff),
                      (int)((value >> 8) & 0xff));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rp2350: write to ROM at 0x%"
                      HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps rp2350_rom_ops = {
    .read = rp2350_rom_read,
    .write = rp2350_rom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rp2350_init(Object *obj)
{
    RP2350State *s = RP2350(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    for (int i = 0; i < RP2350_REGS_NUM; i++) {
        object_initialize_child(obj, "regs[*]", &s->regs[i],
                                rp2350_regs_blocks[i].type);
    }
    object_initialize_child(obj, "sio", &s->sio, TYPE_RP2350_SIO);
    object_initialize_child(obj, "timer0", &s->timer0, TYPE_RP2350_TIMER);
    object_initialize_child(obj, "qmi", &s->qmi, TYPE_RP2350_QMI);
    object_initialize_child(obj, "uart0", &s->uart0, TYPE_PL011);

    /* The clock tree model only reports ready bits; clocks are fixed */
    s->sysclk = clock_new(obj, "sysclk");
    clock_set_hz(s->sysclk, RP2350_SYSCLK_HZ);
    s->refclk = clock_new(obj, "refclk");
    clock_set_hz(s->refclk, RP2350_REFCLK_HZ);
}

static void rp2350_map_alias(RP2350State *s, unsigned n, const char *name,
                             MemoryRegion *target, hwaddr base)
{
    rp2350_atomic_alias_init(&s->alias[n], OBJECT(s), name, target);
    memory_region_add_subregion(get_system_memory(), base, &s->alias[n].mr);
}

static void rp2350_realize(DeviceState *dev, Error **errp)
{
    RP2350State *s = RP2350(dev);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m = DEVICE(&s->armv7m);
    MemoryRegion *xip;
    SysBusDevice *sbd;

    if (!memory_region_init_rom_device(&s->rom, OBJECT(dev), &rp2350_rom_ops,
                                       s, "rp2350.rom", RP2350_ROM_SIZE,
                                       errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, RP2350_ROM_BASE, &s->rom);

    if (!memory_region_init_ram(&s->sram, OBJECT(dev), "rp2350.sram",
                                RP2350_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, RP2350_SRAM_BASE, &s->sram);

    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(armv7m, "num-irq", RP2350_NUM_IRQ);
    /* datasheet 3.7: 4 priority bits, 8 Secure and 8 Non-secure MPU regions */
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_uint32(armv7m, "mpu-ns-regions", 8);
    qdev_prop_set_uint32(armv7m, "mpu-s-regions", 8);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(armv7m), "memory", OBJECT(sysmem),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(armv7m), errp)) {
        return;
    }

    for (int i = 0; i < RP2350_REGS_NUM; i++) {
        sbd = SYS_BUS_DEVICE(&s->regs[i]);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        rp2350_map_alias(s, i, object_get_typename(OBJECT(sbd)),
                         sysbus_mmio_get_region(sbd, 0),
                         rp2350_regs_blocks[i].base);
    }

    sbd = SYS_BUS_DEVICE(&s->sio);
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    sysbus_mmio_map(sbd, 0, RP2350_SIO_BASE);

    sbd = SYS_BUS_DEVICE(&s->timer0);
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    rp2350_map_alias(s, RP2350_ALIAS_TIMER0, "rp2350-timer0",
                     sysbus_mmio_get_region(sbd, 0),
                     rp2350_alias_base[RP2350_ALIAS_TIMER0]);
    for (int i = 0; i < RP2350_TIMER_NUM_ALARMS; i++) {
        sysbus_connect_irq(sbd, i,
                           qdev_get_gpio_in(armv7m, RP2350_TIMER0_IRQ_0 + i));
    }

    sbd = SYS_BUS_DEVICE(&s->uart0);
    qdev_prop_set_chr(DEVICE(sbd), "chardev", serial_hd(0));
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    rp2350_map_alias(s, RP2350_ALIAS_UART0, "rp2350-uart0",
                     sysbus_mmio_get_region(sbd, 0),
                     rp2350_alias_base[RP2350_ALIAS_UART0]);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, RP2350_UART0_IRQ));

    sbd = SYS_BUS_DEVICE(&s->qmi);
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    rp2350_map_alias(s, RP2350_ALIAS_QMI, "rp2350-qmi",
                     sysbus_mmio_get_region(sbd, 0),
                     rp2350_alias_base[RP2350_ALIAS_QMI]);
    /*
     * XIP segment, datasheet 2.2.2. With the cache not modelled and
     * address translation fixed to identity, the cached, uncached and
     * untranslated views all show the same QMI window.
     */
    xip = sysbus_mmio_get_region(sbd, 1);
    memory_region_add_subregion(sysmem, RP2350_XIP_BASE, xip);
    memory_region_init_alias(&s->xip_nocache_alias, OBJECT(dev),
                             "rp2350.xip-nocache-noalloc", xip, 0,
                             memory_region_size(xip));
    memory_region_add_subregion(sysmem, 0x14000000, &s->xip_nocache_alias);
    memory_region_init_alias(&s->xip_notranslate_alias, OBJECT(dev),
                             "rp2350.xip-nocache-noalloc-notranslate", xip, 0,
                             memory_region_size(xip));
    memory_region_add_subregion(sysmem, 0x1c000000,
                                &s->xip_notranslate_alias);

    for (int i = 0; i < ARRAY_SIZE(rp2350_unimplemented); i++) {
        create_unimplemented_device(rp2350_unimplemented[i].name,
                                    rp2350_unimplemented[i].base,
                                    rp2350_unimplemented[i].size);
    }
}

static void rp2350_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rp2350_realize;
    /* Reason: uses serial_hd() and get_system_memory() */
    dc->user_creatable = false;
}

static const TypeInfo rp2350_types[] = {
    {
        .name = TYPE_RP2350,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350State),
        .instance_init = rp2350_init,
        .class_init = rp2350_class_init,
    },
};

DEFINE_TYPES(rp2350_types)
