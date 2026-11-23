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
#define QMI_DIRECT_CSR          0x400d0000u
#define QMI_DIRECT_TX           0x400d0004u
#define QMI_DIRECT_RX           0x400d0008u
#define QMI_EN                  0x1u
#define QMI_ASSERT_CS0          0x4u
#define QMI_TX_NOPUSH           0x100000u
#define TRNG_EHR0               0x400f0114u
#define TRNG_ENABLE             0x400f012cu
#define OTP_SW_LOCK             0x40120000u
#define OTP_PROGRAM             0x40120f00u
#define OTP_ECC                 0x40130000u
#define OTP_RAW                 0x40134000u
#define OTP_WRITE               0x10000u
#define OTP_ECC_FLAG            0x20000u
#define OTP_ROWS                4096u

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
/* ECC OTP and its caller buffers use halfword accesses. */
#define REG16(addr)             (*(volatile uint16_t *)(uintptr_t)(addr))

struct block {
    uint32_t link;          /* byte offset to the next block's header */
    bool image_def;         /* first item is an executable Arm IMAGE_DEF */
    uint32_t vector_table;  /* 0: no VECTOR_TABLE item */
};

static void stub_log(uint32_t port, uint32_t value)
{
    REG32(port) = value;
}

void rom_flash_enter_cmd_xip(void)
{
    stub_log(STUB_LOG_CALL, 0x5843);
    REG32(QMI_DIRECT_CSR) &= ~(QMI_EN | QMI_ASSERT_CS0);
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
    /* RP2350 pico-sdk copies the XIP restore routine out of BOOTRAM. */
    REG32(BOOTRAM_BASE) = 0x47004800; /* ldr r0, [pc, #0]; bx r0 */
    REG32(BOOTRAM_BASE + 4) = (uintptr_t)rom_flash_enter_cmd_xip;
    REG32(BOOTRAM_FLASH_DEVINFO) = 0x0a00; /* CS0: 4 MB, CS1: absent */

    REG32(TRNG_ENABLE) = 1;
    for (unsigned i = 0; i < 4; i++) {
        REG32(BOOTRAM_RANDOM + 4 * i) = REG32(TRNG_EHR0 + 4 * i);
    }
    REG32(TRNG_ENABLE) = 0;

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

int rom_get_sys_info(uint32_t *out, uint32_t words, uint32_t flags)
{
    stub_log(STUB_LOG_CALL, 0x5347);
    if (flags != 0x10) { /* SYS_INFO_BOOT_RANDOM */
        return BOOTROM_ERROR_INVALID_ARG;
    }
    if (words < 5) {
        return BOOTROM_ERROR_BUFFER_TOO_SMALL;
    }
    out[0] = flags;
    for (unsigned i = 0; i < 4; i++) {
        out[i + 1] = REG32(BOOTRAM_RANDOM + 4 * i);
    }
    return 5;
}

int rom_otp_access(uint8_t *buf, uint32_t len, uint32_t flags)
{
    bool write = flags & OTP_WRITE;
    bool ecc = flags & OTP_ECC_FLAG;
    unsigned row = flags & 0xffff;
    unsigned size = ecc ? 2 : 4;

    stub_log(STUB_LOG_CALL, 0x414f);
    if ((len | (uintptr_t)buf) & (size - 1)) {
        return BOOTROM_ERROR_BAD_ALIGNMENT;
    }
    if (flags >> 18 || row >= OTP_ROWS || len / size > OTP_ROWS - row) {
        return BOOTROM_ERROR_INVALID_ARG;
    }
    /* Check the entire range before programming any row. */
    for (unsigned i = 0; i < len / size; i++) {
        unsigned current = row + i;
        uint32_t lock = REG32(OTP_SW_LOCK + 4 * (current / 64)) & 3;

        if (lock >= 2 || (write && lock)) {
            return BOOTROM_ERROR_NOT_PERMITTED;
        }
        if (write) {
            uint32_t old = ecc ? REG16(OTP_ECC + 2 * current)
                               : REG32(OTP_RAW + 4 * current);
            uint32_t value = ecc ? REG16(buf + 2 * i) : REG32(buf + 4 * i);

            if (old & ~value) {
                return BOOTROM_ERROR_UNSUPPORTED_MODIFICATION;
            }
        }
    }
    for (unsigned i = 0; i < len / size; i++, row++, buf += size) {
        if (write) {
            REG32(OTP_PROGRAM) = row;
            REG32(OTP_PROGRAM + 4) = ecc ? REG16(buf) : REG32(buf);
        } else if (ecc) {
            REG16(buf) = REG16(OTP_ECC + 2 * row);
        } else {
            REG32(buf) = REG32(OTP_RAW + 4 * row);
        }
    }
    return 0;
}

void rom_connect_internal_flash(void)
{
    stub_log(STUB_LOG_CALL, 0x4649);
}

void rom_flash_exit_xip(void)
{
    stub_log(STUB_LOG_CALL, 0x5845);
    REG32(QMI_DIRECT_CSR) |= QMI_EN;
}

void rom_flash_flush_cache(void)
{
    /* QMI invalidates translated code when XIP is reenabled. */
    stub_log(STUB_LOG_CALL, 0x4346);
}

static void flash_select(bool selected)
{
    uint32_t csr = REG32(QMI_DIRECT_CSR) & ~QMI_ASSERT_CS0;

    REG32(QMI_DIRECT_CSR) = csr | (selected ? QMI_ASSERT_CS0 : 0);
}

static void flash_send(uint8_t byte)
{
    REG32(QMI_DIRECT_TX) = QMI_TX_NOPUSH | byte;
}

static void flash_begin_write(uint8_t command, uint32_t offset)
{
    flash_select(true);
    flash_send(0x06); /* WREN */
    flash_select(false);

    flash_select(true);
    flash_send(command);
    flash_send(offset >> 16);
    flash_send(offset >> 8);
    flash_send(offset);
}

static void flash_end_write(void)
{
    flash_select(false);
    flash_select(true);
    flash_send(0x05); /* RDSR */
    do {
        REG32(QMI_DIRECT_TX) = 0;
    } while (REG32(QMI_DIRECT_RX) & 1);
    flash_select(false);
}

void rom_flash_range_erase(uint32_t offset, uint32_t count,
                           uint32_t block_size, uint8_t block_cmd)
{
    (void)block_size;
    (void)block_cmd;
    stub_log(STUB_LOG_CALL, 0x4552);
    if (((offset | count) & 4095) || offset >= FLASH_SIZE ||
        count > FLASH_SIZE - offset) {
        return;
    }
    for (; count; count -= 4096, offset += 4096) {
        flash_begin_write(0x20, offset);
        flash_end_write();
    }
}

void rom_flash_range_program(uint32_t offset, const uint8_t *data,
                              uint32_t count)
{
    stub_log(STUB_LOG_CALL, 0x5052);
    if (((offset | count) & 255) || offset >= FLASH_SIZE ||
        count > FLASH_SIZE - offset) {
        return;
    }
    for (; count; count -= 256, offset += 256) {
        flash_begin_write(0x02, offset);
        for (unsigned i = 0; i < 256; i++) {
            flash_send(*data++);
        }
        flash_end_write();
    }
}
