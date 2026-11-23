/*
 * RP2350 atomic register access aliases (datasheet 2.1.3)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rp2350_atomic.h"
#include "trace.h"

enum {
    ALIAS_NORMAL = 0,
    ALIAS_XOR = 1,
    ALIAS_SET = 2,
    ALIAS_CLR = 3,
};

static MemTxResult rp2350_atomic_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    RP2350AtomicAlias *s = opaque;

    return memory_region_dispatch_read(s->target,
                                       addr & (RP2350_ATOMIC_BLOCK_SIZE - 1),
                                       data, size_memop(size) | MO_LE, attrs);
}

static MemTxResult rp2350_atomic_write(void *opaque, hwaddr addr,
                                       uint64_t data, unsigned size,
                                       MemTxAttrs attrs)
{
    RP2350AtomicAlias *s = opaque;
    unsigned alias = addr / RP2350_ATOMIC_BLOCK_SIZE;
    hwaddr offset = addr & (RP2350_ATOMIC_BLOCK_SIZE - 1);
    uint64_t old = 0;
    MemTxResult r;

    if (alias != ALIAS_NORMAL) {
        r = memory_region_dispatch_read(s->target, offset, &old,
                                        size_memop(size) | MO_LE, attrs);
        if (r != MEMTX_OK) {
            return r;
        }
        switch (alias) {
        case ALIAS_XOR:
            data = old ^ data;
            break;
        case ALIAS_SET:
            data = old | data;
            break;
        case ALIAS_CLR:
            data = old & ~data;
            break;
        default:
            g_assert_not_reached();
        }
    }
    trace_rp2350_atomic_write(memory_region_name(&s->mr), alias, offset,
                              old, data);
    return memory_region_dispatch_write(s->target, offset, data,
                                        size_memop(size) | MO_LE, attrs);
}

static const MemoryRegionOps rp2350_atomic_ops = {
    .read_with_attrs = rp2350_atomic_read,
    .write_with_attrs = rp2350_atomic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

void rp2350_atomic_alias_init(RP2350AtomicAlias *s, Object *owner,
                              const char *name, MemoryRegion *target)
{
    memory_region_init_io(&s->mr, owner, &rp2350_atomic_ops, s, name,
                          RP2350_ATOMIC_ALIAS_SIZE);
    s->target = target;
}
