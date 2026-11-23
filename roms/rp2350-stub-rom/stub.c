/*
 * RP2350 stub ROM for QEMU: boot path and ROM functions
 *
 * Boot: search the first 4 kB of flash for the start of a block loop
 * holding an IMAGE_DEF (datasheet 5.1.5.1, 5.9.1). If the IMAGE_DEF has
 * a VECTOR_TABLE item (datasheet 5.9.3.3), enter through that table;
 * otherwise enter through the table at the start of flash (0x10000000).
 *
 * ROM functions: only those the firmware is known to call are
 * implemented. Any other code from the table logs its code and returns
 * BOOTROM_ERROR_INVALID_STATE.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include "stub.h"
#include "rom_table.h"

#define XIP_BASE                0x10000000u
#define FLASH_SEARCH_SIZE       0x1000u     /* datasheet 5.1.5.1 */
/* shortcut: blocks must lie in the 4 MB flash of the Pico 2 */
#define FLASH_SIZE              (4u << 20)
#define VTOR                    0xe000ed08u

/* pico-sdk picobin.h */
#define BLOCK_MARKER_START      0xffffded3u
#define BLOCK_MARKER_END        0xab123579u
#define ITEM_IMAGE_TYPE         0x42
#define ITEM_VECTOR_TABLE       0x03
#define ITEM_LAST               0xff
#define ITEM_SIZE_FLAG          0x80
#define IMAGE_TYPE_MASK         0x000f
#define IMAGE_TYPE_EXE          0x0001
#define IMAGE_CPU_MASK          0x0700
#define IMAGE_CPU_ARM           0x0000
/* datasheet 5.9.1: IMAGE_DEF blocks larger than 384 bytes are ignored */
#define BLOCK_MAX_WORDS         (384 / 4)
#define LOOP_MAX_BLOCKS         16

/* volatile: flash, ROM log ports and VTOR must be accessed as written */
#define REG32(addr)             (*(volatile uint32_t *)(uintptr_t)(addr))

struct block {
    uint32_t link;          /* byte offset to the next block's header */
    bool image_def;         /* first item is an executable Arm IMAGE_DEF */
    uint32_t vector_table;  /* 0: no VECTOR_TABLE item */
};

static void stub_log(uint32_t port, uint32_t value)
{
    REG32(port) = value;
}

static bool in_flash(uint32_t addr, uint32_t len)
{
    return addr >= XIP_BASE && addr - XIP_BASE + len <= FLASH_SIZE;
}

/* Check the block at @addr (datasheet 5.9.1) and pick out its items. */
static bool parse_block(uint32_t addr, struct block *b)
{
    uint32_t words = 0;
    uint32_t p = addr + 4;
    bool first = true;

    if (!in_flash(addr, 4) || (addr & 3) || REG32(addr) != BLOCK_MARKER_START) {
        return false;
    }
    b->image_def = false;
    b->vector_table = 0;
    for (;;) {
        uint32_t hdr, size;
        uint8_t type;

        if (!in_flash(p, 12) || words > BLOCK_MAX_WORDS) {
            return false;
        }
        hdr = REG32(p);
        type = hdr & 0xff;
        size = (hdr >> 8) & ((type & ITEM_SIZE_FLAG) ? 0xffff : 0xff);
        if (type == ITEM_LAST) {
            if (size != words || REG32(p + 8) != BLOCK_MARKER_END) {
                return false;
            }
            b->link = REG32(p + 4);
            return true;
        }
        if (size == 0) {
            return false;
        }
        if (first && type == ITEM_IMAGE_TYPE) {
            uint32_t flags = hdr >> 16;

            b->image_def = (flags & IMAGE_TYPE_MASK) == IMAGE_TYPE_EXE &&
                           (flags & IMAGE_CPU_MASK) == IMAGE_CPU_ARM;
        } else if (type == ITEM_VECTOR_TABLE && size == 2) {
            b->vector_table = REG32(p + 4);
        }
        first = false;
        words += size;
        p += size * 4;
    }
}

/*
 * Follow the block loop starting at @first. Returns the vector table of
 * the first IMAGE_DEF in the loop through @vt (0 when it has no
 * VECTOR_TABLE item), or false if the loop is broken or has no IMAGE_DEF.
 */
static bool walk_loop(uint32_t first, uint32_t *image_def, uint32_t *vt)
{
    uint32_t addr = first;
    bool found = false;

    for (int i = 0; i < LOOP_MAX_BLOCKS; i++) {
        struct block b;

        if (!parse_block(addr, &b)) {
            return false;
        }
        if (b.image_def && !found) {
            /* shortcut: the first IMAGE_DEF wins; signatures are ignored */
            found = true;
            *image_def = addr;
            *vt = b.vector_table;
        }
        addr += b.link;
        if (addr == first) {
            return found;
        }
    }
    return false;
}

static uint32_t find_vector_table(void)
{
    for (uint32_t addr = XIP_BASE; addr < XIP_BASE + FLASH_SEARCH_SIZE;
         addr += 4) {
        uint32_t image_def, vt;

        if (REG32(addr) == BLOCK_MARKER_START &&
            walk_loop(addr, &image_def, &vt)) {
            stub_log(STUB_LOG_BLOCK, image_def);
            return vt ? vt : XIP_BASE;
        }
    }
    stub_log(STUB_LOG_BLOCK, 0);
    return XIP_BASE;
}

void rom_reset(void) __attribute__((noreturn));
void rom_reset(void)
{
    uint32_t vt = find_vector_table();
    uint32_t sp = REG32(vt);
    uint32_t pc = REG32(vt + 4);

    stub_log(STUB_LOG_BOOT, vt);
    REG32(VTOR) = vt;
    __asm__ volatile("msr msp, %0\n\tbx %1" : : "r"(sp), "r"(pc));
    __builtin_unreachable();
}

/* ROM table lookup (see start.S for the table format) */

static unsigned bit_count(uint32_t v)
{
    unsigned n = 0;

    for (; v; v &= v - 1) {
        n++;
    }
    return n;
}

const uint16_t *rom_table_lookup_entry(uint32_t code, uint32_t mask)
{
    for (const uint16_t *p = rom_table; p[0]; p += 2 + bit_count(p[1])) {
        uint32_t match = p[1] & mask;

        if (p[0] == code && match) {
            /* the pointer for the lowest matching flag bit */
            return &p[2 + bit_count(p[1] & ((match & -match) - 1))];
        }
    }
    return 0;
}

uintptr_t rom_table_lookup_val(uint32_t code, uint32_t mask)
{
    const uint16_t *entry = rom_table_lookup_entry(code, mask);

    return entry ? *entry : 0;
}

int rom_unimplemented(uint32_t code)
{
    stub_log(STUB_LOG_UNIMPL, code);
    return BOOTROM_ERROR_INVALID_STATE;
}

/* 'SR': the stub ROM keeps no state, so there is nothing to reset. */
void rom_bootrom_state_reset(uint32_t flags)
{
    (void)flags;
    stub_log(STUB_LOG_CALL, 0x5253);
}
