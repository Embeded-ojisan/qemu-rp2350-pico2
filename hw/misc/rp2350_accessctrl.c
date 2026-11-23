/*
 * RP2350 ACCESSCTRL and system-bus security filters.
 *
 * RP-008373 sections 2.1.2, 2.2 and 10.6. Register layout and reset
 * values agree with pico-sdk 2.3.1 hardware/regs/accessctrl.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "hw/misc/rp2350_accessctrl.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

#define TYPE_RP2350_BUS "rp2350-filtered-bus"
#define BUS_PAGE_MASK 0xfff
#define STRIPED_BASE 0x20000000
#define STRIPED_SIZE 0x80000
#define STRIPED_GROUP_SIZE 0x40000
#define ACCESS_KEY 0xacce0000
#define LOCK 0
#define FORCE_CORE_NS 1
#define CFGRESET 2
#define GPIO_NSMASK0 3
#define GPIO_NSMASK1 4

enum {
    ROM = 0x14 / 4, XIP_MAIN, SRAM0, SRAM1, SRAM2, SRAM3, SRAM4, SRAM5,
    SRAM6, SRAM7, SRAM8, SRAM9, DMA, USBCTRL, PIO0, PIO1, PIO2,
    CORESIGHT_TRACE, CORESIGHT_PERIPH, SYSINFO, RESETS, IO_BANK0, IO_QSPI,
    PADS_BANK0, PADS_QSPI, BUSCTRL, ADC, HSTX, I2C0, I2C1, PWM, SPI0, SPI1,
    TIMER0, TIMER1, UART0, UART1, OTP, TBMAN, POWMAN, TRNG, SHA256,
    SYSCFG, CLOCKS, XOSC, ROSC, PLL_SYS, PLL_USB, TICKS, WATCHDOG, PSM,
    XIP_CTRL, XIP_QMI, XIP_AUX,
};

enum {
    NSU, NSP, SU, SP, LEVEL_COUNT,
    BYPASS = RP2350_MANAGER_COUNT * LEVEL_COUNT,
    INVALID,
};

/* APB peripheral slots have a 32 KiB stride, including the atomic aliases. */
static const uint8_t apb_ports[] = {
    SYSINFO, SYSCFG, CLOCKS, PSM, RESETS, IO_BANK0, IO_QSPI, PADS_BANK0,
    PADS_QSPI, XOSC, PLL_SYS, PLL_USB, 0, BUSCTRL, UART0, UART1,
    SPI0, SPI1, I2C0, I2C1, ADC, PWM, TIMER0, TIMER1, HSTX, XIP_CTRL,
    XIP_QMI, WATCHDOG, 0, ROSC, TRNG, SHA256, POWMAN, TICKS,
    0, 0, OTP, OTP, OTP, OTP, CORESIGHT_PERIPH, CORESIGHT_PERIPH,
    CORESIGHT_PERIPH, 0, TBMAN,
};

static const uint8_t ahb_ports[] = {
    DMA, USBCTRL, PIO0, PIO1, PIO2, XIP_AUX, HSTX, CORESIGHT_TRACE,
};

uint32_t rp2350_accessctrl_ns_reset_mask(RP2350AccessCtrl *s)
{
    /* RESETS bits are ordered as in pico-sdk 2.3.1 hardware/regs/resets.h. */
    static const uint8_t reset_ports[] = {
        ADC, BUSCTRL, DMA, HSTX, I2C0, I2C1, IO_BANK0, IO_QSPI, 0 /* JTAG */,
        PADS_BANK0, PADS_QSPI, PIO0, PIO1, PIO2, PLL_SYS, PLL_USB, PWM,
        SHA256, SPI0, SPI1, SYSCFG, SYSINFO, TBMAN, TIMER0, TIMER1, TRNG,
        UART0, UART1, USBCTRL,
    };
    uint32_t mask = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(reset_ports); i++) {
        unsigned port = reset_ports[i];

        if (port && (s->permissions[port] & BIT(NSP))) {
            mask |= BIT(i);
        }
    }
    return mask;
}

static int bus_index(MemTxAttrs attrs)
{
    unsigned master = attrs.requester_id;
    unsigned level = (attrs.secure || attrs.unspecified ? 2 : 0) |
                     !attrs.user;

    if (attrs.debug) {
        return BYPASS;
    }
    return master < RP2350_MANAGER_COUNT ? master * LEVEL_COUNT + level :
           INVALID;
}

static unsigned bus_level(RP2350AccessCtrl *s, int index)
{
    unsigned level = index % LEVEL_COUNT;

    if (index / LEVEL_COUNT == RP2350_CORE1 &&
        (s->force_core_ns & BIT(1))) {
        level &= 1;
    }
    return level;
}

static bool port_allows(RP2350AccessCtrl *s, unsigned port, int index)
{
    unsigned level, bits;

    if (index == BYPASS) {
        return true;
    }
    if (index == INVALID) {
        return false;
    }
    level = bus_level(s, index);
    bits = BIT(4 + index / LEVEL_COUNT) | BIT(level);
    if (!(level & 1)) {
        bits |= BIT(level + 1);
    }
    return (s->permissions[port] & bits) == bits;
}

static unsigned stripe_port(hwaddr addr)
{
    return SRAM0 + ((addr >> 2) & 3) + (addr >= STRIPED_GROUP_SIZE ? 4 : 0);
}

static MemTxResult stripe_read(void *opaque, hwaddr addr, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    RP2350AccessCtrl *s = opaque;
    uint32_t data = 0;
    MemTxResult result;

    if (!port_allows(s, stripe_port(addr), bus_index(attrs))) {
        return MEMTX_ACCESS_ERROR;
    }
    result = address_space_read(&address_space_memory, STRIPED_BASE + addr,
                                attrs, &data, size);
    *value = le32_to_cpu(data);
    return result;
}

static MemTxResult stripe_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    RP2350AccessCtrl *s = opaque;
    uint32_t data = cpu_to_le32(value);

    if (!port_allows(s, stripe_port(addr), bus_index(attrs))) {
        return MEMTX_ACCESS_ERROR;
    }
    return address_space_write(&address_space_memory, STRIPED_BASE + addr,
                               attrs, &data, size);
}

static const MemoryRegionOps stripe_ops = {
    .read_with_attrs = stripe_read,
    .write_with_attrs = stripe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.max_access_size = 8,
    .impl.max_access_size = 4,
};

static MemTxResult denied_read(void *opaque, hwaddr addr, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return MEMTX_ACCESS_ERROR;
}

static MemTxResult denied_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    return MEMTX_ACCESS_ERROR;
}

static const MemoryRegionOps denied_ops = {
    .read_with_attrs = denied_read,
    .write_with_attrs = denied_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.max_access_size = 8,
    .impl.max_access_size = 8,
};

static unsigned address_port(hwaddr addr)
{
    if (addr < 0x10000000) {
        return ROM;
    }
    if (addr < 0x20000000) {
        return XIP_MAIN;
    }
    if (addr >= 0x20080000 && addr < 0x20082000) {
        return addr < 0x20081000 ? SRAM8 : SRAM9;
    }
    if (addr >= 0x40000000 && addr < 0x40000000 + sizeof(apb_ports) * 0x8000) {
        return apb_ports[(addr - 0x40000000) / 0x8000];
    }
    if (addr >= 0x50000000 &&
        addr < 0x50000000 + sizeof(ahb_ports) * 0x100000) {
        return ahb_ports[(addr - 0x50000000) / 0x100000];
    }
    return 0;
}

static IOMMUTLBEntry rp2350_bus_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                         IOMMUAccessFlags flags, int index)
{
    RP2350AccessCtrl *s = container_of(iommu, RP2350AccessCtrl, bus);
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr & ~BUS_PAGE_MASK,
        .translated_addr = addr & ~BUS_PAGE_MASK,
        .addr_mask = BUS_PAGE_MASK,
        .perm = IOMMU_RW,
    };
    unsigned port;
    bool allow = true;

    if (index == BYPASS) {
        return entry;
    }
    if (addr >= STRIPED_BASE && addr < STRIPED_BASE + STRIPED_SIZE) {
        port = stripe_port((addr - STRIPED_BASE) & ~0xf);
        allow = port_allows(s, port, index);
        for (unsigned i = 1; i < 4; i++) {
            if (port_allows(s, port + i, index) != allow) {
                /*
                 * When banks disagree, a CPU TLB page cannot represent
                 * the four word-sized stripes. Check each beat in MMIO.
                 */
                entry.target_as = &s->striped_as;
                return entry;
            }
        }
    } else if (addr >= 0x400e0000 && addr < 0x400e4000) {
        /* Boot RAM and its aliases have no programmable ACL. */
        allow = index != INVALID && bus_level(s, index) >= SU;
    } else if (addr >= 0xd0000000 && addr < 0xe0000000) {
        /* The CPU's SIO selects its bank internally and has no DMA port. */
        allow = index != INVALID && index / LEVEL_COUNT != RP2350_DMA_MANAGER;
    } else {
        port = address_port(addr);
        if (port) {
            allow = port_allows(s, port, index);
        }
    }
    if (!allow) {
        /* Preserve the address and deliver a device error to the manager. */
        entry.target_as = &s->denied_as;
    }
    return entry;
}

static int rp2350_bus_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    return bus_index(attrs);
}

static int rp2350_bus_num_indexes(IOMMUMemoryRegion *iommu)
{
    return INVALID + 1;
}

static void invalidate_bus(RP2350AccessCtrl *s)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &address_space_memory,
            .addr_mask = UINT32_MAX,
            .perm = IOMMU_NONE,
        },
    };

    for (int i = 0; i < INVALID; i++) {
        memory_region_notify_iommu(&s->bus, i, event);
    }
}

static void reset_permissions(RP2350AccessCtrl *s)
{
    memset(s->permissions, 0xfc, sizeof(s->permissions));
    memset(&s->permissions[ROM], 0xff, SRAM9 - ROM + 1);
    s->permissions[SYSINFO] = 0xff;
    s->permissions[CORESIGHT_TRACE] = s->permissions[CORESIGHT_PERIPH] = 0xb8;
    memset(&s->permissions[POWMAN], 0xb8, XIP_QMI - POWMAN + 1);
    s->permissions[SHA256] = s->permissions[XIP_AUX] = 0xf8;
    rp2350_sio_gpio_set_nsmask(s->sio, 0, 0);
    rp2350_sio_gpio_set_nsmask(s->sio, 1, 0);
    invalidate_bus(s);
}

static uint32_t reg_value(RP2350AccessCtrl *s, unsigned reg)
{
    switch (reg) {
    case LOCK:
        return s->lock;
    case FORCE_CORE_NS:
        return s->force_core_ns;
    case GPIO_NSMASK0:
    case GPIO_NSMASK1:
        return s->sio->gpio_nsmask[reg - GPIO_NSMASK0];
    default:
        return reg >= ROM && reg <= XIP_AUX ? s->permissions[reg] : 0;
    }
}

static uint64_t accessctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    return reg_value(opaque, (addr & BUS_PAGE_MASK) / 4);
}

static MemTxResult accessctrl_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    RP2350AccessCtrl *s = opaque;
    unsigned reg = (addr & BUS_PAGE_MASK) / 4;
    unsigned alias = addr / (BUS_PAGE_MASK + 1);
    int index = bus_index(attrs);
    unsigned master = index == BYPASS ? RP2350_DEBUGGER : index / LEVEL_COUNT;
    unsigned level = index == BYPASS ? SP : bus_level(s, index);
    uint32_t old;

    if (index == INVALID || master == RP2350_DMA_MANAGER || !(level & 1)) {
        return MEMTX_ACCESS_ERROR;
    }
    if (reg != GPIO_NSMASK0 && reg != GPIO_NSMASK1 &&
        (value & 0xffff0000) != ACCESS_KEY) {
        return MEMTX_ACCESS_ERROR;
    }
    if (s->lock & BIT(master)) {
        return MEMTX_OK;
    }

    old = reg_value(s, reg);
    switch (alias) {
    case 1:
        value = old ^ value;
        break;
    case 2:
        value = old | value;
        break;
    case 3:
        value = old & ~value;
        break;
    }
    if (level == NSP) {
        if (reg < ROM || reg > XIP_AUX || !(old & BIT(NSP))) {
            return MEMTX_OK;
        }
        value = (old & ~BIT(NSU)) | (value & BIT(NSU));
    }
    switch (reg) {
    case LOCK:
        s->lock |= value & 0xf;
        break;
    case FORCE_CORE_NS:
        s->force_core_ns = value & BIT(1);
        invalidate_bus(s);
        break;
    case CFGRESET:
        if (value & 1) {
            reset_permissions(s);
        }
        break;
    case GPIO_NSMASK0:
    case GPIO_NSMASK1:
        rp2350_sio_gpio_set_nsmask(s->sio, reg - GPIO_NSMASK0, value);
        break;
    default:
        if (reg <= XIP_AUX && s->permissions[reg] != (uint8_t)value) {
            s->permissions[reg] = value;
            invalidate_bus(s);
        }
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps accessctrl_ops = {
    .read = accessctrl_read,
    .write_with_attrs = accessctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void accessctrl_reset(Object *obj, ResetType type)
{
    RP2350AccessCtrl *s = RP2350_ACCESSCTRL(obj);

    s->lock = BIT(RP2350_DMA_MANAGER);
    s->force_core_ns = 0;
    reset_permissions(s);
}

static void accessctrl_init(Object *obj)
{
    RP2350AccessCtrl *s = RP2350_ACCESSCTRL(obj);

    memory_region_init_io(&s->regs, obj, &accessctrl_ops, s,
                          TYPE_RP2350_ACCESSCTRL, RP2350_ACCESSCTRL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
    memory_region_init_iommu(&s->bus, sizeof(s->bus), TYPE_RP2350_BUS,
                             obj, "rp2350.bus", UINT64_C(1) << 32);
    memory_region_init(&s->striped_root, obj, "rp2350.striped", UINT64_MAX);
    memory_region_init_io(&s->striped_io, obj, &stripe_ops, s,
                          "rp2350.striped-io", STRIPED_SIZE);
    memory_region_add_subregion(&s->striped_root, STRIPED_BASE, &s->striped_io);
    address_space_init(&s->striped_as, &s->striped_root, "rp2350.striped");
    memory_region_init_io(&s->denied_io, obj, &denied_ops, s,
                          "rp2350.denied-io", UINT64_C(1) << 32);
    address_space_init(&s->denied_as, &s->denied_io, "rp2350.denied");
}

static int accessctrl_post_load(void *opaque, int version_id)
{
    invalidate_bus(opaque);
    return 0;
}

static const VMStateDescription accessctrl_vmstate = {
    .name = TYPE_RP2350_ACCESSCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = accessctrl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(lock, RP2350AccessCtrl),
        VMSTATE_UINT8(force_core_ns, RP2350AccessCtrl),
        VMSTATE_UINT8_ARRAY(permissions, RP2350AccessCtrl,
                             RP2350_ACCESSCTRL_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void accessctrl_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    rc->phases.hold = accessctrl_reset;
    dc->vmsd = &accessctrl_vmstate;
    dc->user_creatable = false;
}

static void bus_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = rp2350_bus_translate;
    imrc->attrs_to_index = rp2350_bus_index;
    imrc->num_indexes = rp2350_bus_num_indexes;
}

static const TypeInfo accessctrl_types[] = {
    {
        .name = TYPE_RP2350_ACCESSCTRL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350AccessCtrl),
        .instance_init = accessctrl_init,
        .class_init = accessctrl_class_init,
    }, {
        .name = TYPE_RP2350_BUS,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = bus_class_init,
    },
};

DEFINE_TYPES(accessctrl_types)
