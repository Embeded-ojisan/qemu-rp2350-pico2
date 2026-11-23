/*
 * RP2350 OTP software locks and data windows (RP-008373 section 13.9)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rp2350_otp.h"
#include "migration/vmstate.h"

#define OTP_LOCK1_ROW    0xf81
#define OTP_WINDOW_SIZE  0x4000
#define OTP_ROW_MASK     0xffffff
/* Private programming port used by roms/rp2350-stub-rom. */
#define OTP_PROGRAM_ROW  0xf00
#define OTP_PROGRAM_DATA 0xf04

static uint8_t otp_locks(RP2350OTPState *s, unsigned page)
{
    uint32_t row = s->rows[OTP_LOCK1_ROW + page * 2];
    uint8_t a = row, b = row >> 8, c = row >> 16;

    return s->sw_lock[page] | (a & b) | (a & c) | (b & c);
}

static bool otp_secure(MemTxAttrs attrs)
{
    return attrs.secure || attrs.unspecified;
}

static MemTxResult otp_ctrl_read(void *opaque, hwaddr offset, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    RP2350OTPState *s = opaque;

    *data = 0;
    if (offset < RP2350_OTP_PAGES * 4) {
        *data = otp_locks(s, offset / 4) & (otp_secure(attrs) ? 0xf : 0xc);
    }
    return MEMTX_OK;
}

static MemTxResult otp_ctrl_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    RP2350OTPState *s = opaque;

    if (offset < RP2350_OTP_PAGES * 4) {
        s->sw_lock[offset / 4] |= value & (otp_secure(attrs) ? 0xf : 0xc);
    } else if (offset == OTP_PROGRAM_ROW && otp_secure(attrs)) {
        s->selected_row = value;
    } else if (offset == OTP_PROGRAM_DATA && otp_secure(attrs) &&
               s->selected_row < RP2350_OTP_ROWS) {
        if (otp_locks(s, s->selected_row / 64) & 3) {
            return MEMTX_ACCESS_ERROR;
        }
        s->rows[s->selected_row] |= value & OTP_ROW_MASK;
    }
    return MEMTX_OK;
}

static MemTxResult otp_data_read(void *opaque, hwaddr offset, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    RP2350OTPState *s = opaque;
    unsigned window = offset / OTP_WINDOW_SIZE;
    unsigned stride = window & 1 ? 4 : 2;
    unsigned index = (offset % OTP_WINDOW_SIZE) / stride;
    unsigned shift = otp_secure(attrs) ? 0 : 2;

    *data = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned byte = (offset % stride) + i;
        unsigned row = index + byte / stride;
        unsigned lock;

        if (row >= RP2350_OTP_ROWS) {
            return MEMTX_DECODE_ERROR;
        }
        lock = (otp_locks(s, row / 64) >> shift) & 3;
        if (lock >= 2 && row < OTP_LOCK1_ROW - 1) {
            if (window >= 2) {
                return MEMTX_ACCESS_ERROR;
            }
            *data |= UINT64_C(0xff) << (i * 8);
            continue;
        }
        /* shortcut: ECC reads return the payload without checking parity. */
        *data |= (uint64_t)(s->rows[row] >> ((byte % stride) * 8) & 0xff)
                 << (i * 8);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps otp_ctrl_ops = {
    .read_with_attrs = otp_ctrl_read,
    .write_with_attrs = otp_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps otp_data_ops = {
    .read_with_attrs = otp_data_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void otp_init(Object *obj)
{
    RP2350OTPState *s = RP2350_OTP(obj);

    memory_region_init_io(&s->ctrl, obj, &otp_ctrl_ops, s,
                          "rp2350-otp-ctrl", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ctrl);
    memory_region_init_io(&s->data, obj, &otp_data_ops, s,
                          "rp2350-otp-data", 4 * OTP_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->data);
}

static void otp_reset(Object *obj, ResetType type)
{
    RP2350OTPState *s = RP2350_OTP(obj);

    /* Fuses start blank and survive system reset for the lifetime of the VM. */
    memset(s->sw_lock, 0, sizeof(s->sw_lock));
    s->selected_row = UINT32_MAX;
}

static const VMStateDescription vmstate_otp = {
    .name = TYPE_RP2350_OTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(rows, RP2350OTPState, RP2350_OTP_ROWS),
        VMSTATE_UINT8_ARRAY(sw_lock, RP2350OTPState, RP2350_OTP_PAGES),
        VMSTATE_UINT32(selected_row, RP2350OTPState),
        VMSTATE_END_OF_LIST()
    },
};

static void otp_class_init(ObjectClass *klass, const void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = otp_reset;
    DEVICE_CLASS(klass)->vmsd = &vmstate_otp;
}

static const TypeInfo rp2350_otp_types[] = {
    {
        .name = TYPE_RP2350_OTP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350OTPState),
        .instance_init = otp_init,
        .class_init = otp_class_init,
    },
};

DEFINE_TYPES(rp2350_otp_types)
