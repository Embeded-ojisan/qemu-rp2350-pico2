/*
 * RP2350 Cortex-M33 coprocessors: GPIO coprocessor (cp0) and
 * double-precision coprocessor (cp4)
 *
 * GPIOC (datasheet 3.6.1) gives single-instruction access to the SIO GPIO
 * registers, so writes through it show up in the SIO trace events like
 * MMIO writes. The DCP (datasheet 3.6.2) is modelled only as far as
 * pico-sdk start-up needs: RCMP, which clears the engaged flag. Every
 * other DCP transfer is logged and reads as zero. CDP instructions are
 * not decoded by QEMU for M profile and stay UNDEFINED.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/rp2350_coproc.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"

/*
 * GPIOC field use (datasheet 3.6.1.6): CRm[3:2] selects the register
 * group (OUT, OE, IN), CRm[0] the low or high register of a group;
 * opc1[1:0] the write operation (write, XOR, set, clear) and opc1[3:2]
 * the addressing mode.
 */
#define GPIOC_GROUP(crm)    ((crm) >> 2)
#define GPIOC_BANK(crm)     ((crm) & 1)
#define GPIOC_OP(opc1)      ((opc1) & 3)
#define GPIOC_MODE(opc1)    ((opc1) >> 2)

enum { GPIOC_MODE_MASK, GPIOC_MODE_BIT, GPIOC_MODE_INDEX };

/*
 * ARMCPRegInfo carries no device pointer. Both cores' GPIOC ports reach
 * the one SIO, so a single pointer serves the machine.
 * shortcut: one RP2350 per QEMU process; asserted in rp2350_coproc_init().
 */
static RP2350SIOState *gpioc_sio;

static bool gpioc_valid_crm(const ARMCPRegInfo *ri, bool writable)
{
    unsigned max_group = writable ? RP2350_SIO_GPIO_OE : RP2350_SIO_GPIO_IN;

    return ri->crn == 0 && ri->opc2 == 0 && !(ri->crm & 2) &&
           GPIOC_GROUP(ri->crm) <= max_group;
}

static void gpioc_reserved(const ARMCPRegInfo *ri, bool isread)
{
    qemu_log_mask(LOG_GUEST_ERROR, "rp2350 GPIOC: reserved %s p0, #%d, "
                  "c%d, c%d, #%d\n", isread ? "read" : "write", ri->opc1,
                  ri->crn, ri->crm, ri->opc2);
}

/* Apply @op to OUT or OE with a 64-bit mask (bit n is GPIOn) */
static void gpioc_write_hilo(RP2350SIOState *sio, unsigned group,
                             unsigned op, uint64_t value)
{
    rp2350_sio_gpio_write(sio, group, 0, op, value);
    rp2350_sio_gpio_write(sio, group, 1, op, value >> 32);
}

static uint64_t gpioc_bit(uint32_t pin)
{
    return pin < 64 ? 1ull << pin : 0;
}

/* mrc p0, #0, Rt, c0, CRm */
static uint64_t gpioc_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    if (ri->opc1 != 0 || !gpioc_valid_crm(ri, false)) {
        gpioc_reserved(ri, true);
        return 0;
    }
    return rp2350_sio_gpio_read(gpioc_sio, GPIOC_GROUP(ri->crm),
                                GPIOC_BANK(ri->crm));
}

/* mcr p0, #opc1, Rt, c0, CRm */
static void gpioc_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    RP2350SIOState *sio = gpioc_sio;
    unsigned group = GPIOC_GROUP(ri->crm);
    unsigned op = GPIOC_OP(ri->opc1);

    if (!gpioc_valid_crm(ri, true)) {
        gpioc_reserved(ri, false);
        return;
    }
    switch (GPIOC_MODE(ri->opc1)) {
    case GPIOC_MODE_MASK:
        rp2350_sio_gpio_write(sio, group, GPIOC_BANK(ri->crm), op, value);
        break;
    case GPIOC_MODE_BIT:
        /* bit_xor/set/clr: Rt is a pin number */
        if (op == RP2350_SIO_OP_WRITE || GPIOC_BANK(ri->crm)) {
            gpioc_reserved(ri, false);
            break;
        }
        gpioc_write_hilo(sio, group, op, gpioc_bit(value));
        break;
    default:
        gpioc_reserved(ri, false);
        break;
    }
}

/* mrrc p0, #0, Rt, Rt2, CRm */
static uint64_t gpioc_read64(CPUARMState *env, const ARMCPRegInfo *ri)
{
    unsigned group = GPIOC_GROUP(ri->crm);

    if (ri->opc1 != 0 || !gpioc_valid_crm(ri, false) || GPIOC_BANK(ri->crm)) {
        gpioc_reserved(ri, true);
        return 0;
    }
    return rp2350_sio_gpio_read(gpioc_sio, group, 0) |
           (uint64_t)rp2350_sio_gpio_read(gpioc_sio, group, 1) << 32;
}

/* mcrr p0, #opc1, Rt, Rt2, CRm: @value is Rt2:Rt */
static void gpioc_write64(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    RP2350SIOState *sio = gpioc_sio;
    unsigned group = GPIOC_GROUP(ri->crm);
    unsigned op = GPIOC_OP(ri->opc1);
    uint32_t rt = value;
    uint32_t rt2 = value >> 32;

    if (!gpioc_valid_crm(ri, true) || GPIOC_BANK(ri->crm)) {
        gpioc_reserved(ri, false);
        return;
    }
    switch (GPIOC_MODE(ri->opc1)) {
    case GPIOC_MODE_MASK:
        /* hilo_put/xor/set/clr: Rt to the low, Rt2 to the high register */
        gpioc_write_hilo(sio, group, op, value);
        break;
    case GPIOC_MODE_BIT:
        if (op == RP2350_SIO_OP_WRITE) {
            /* bit_put: Rt is the pin, Rt2[0] the value */
            op = (rt2 & 1) ? RP2350_SIO_OP_SET : RP2350_SIO_OP_CLR;
            gpioc_write_hilo(sio, group, op, gpioc_bit(rt));
        } else {
            /* bit_xor2/set2/clr2: conditional on Rt2[0] */
            gpioc_write_hilo(sio, group, op, (rt2 & 1) ? gpioc_bit(rt) : 0);
        }
        break;
    case GPIOC_MODE_INDEX:
        /* index_put/xor/set/clr: Rt is the value, Rt2 selects lo or hi */
        if (rt2 > 1) {
            gpioc_reserved(ri, false);
            break;
        }
        rp2350_sio_gpio_write(sio, group, rt2, op, rt);
        break;
    default:
        gpioc_reserved(ri, false);
        break;
    }
}

static void dcp_unimplemented(const ARMCPRegInfo *ri, bool is64, bool isread)
{
    qemu_log_mask(LOG_UNIMP, "rp2350 DCP: %s p4, #%d, c%d, c%d, #%d is not "
                  "implemented\n",
                  is64 ? (isread ? "mrrc" : "mcrr") : (isread ? "mrc" : "mcr"),
                  ri->opc1, ri->crn, ri->crm, ri->opc2);
}

/* mrc p4, #opc1, Rt, c0, CRm, #opc2 */
static uint64_t dcp_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /*
     * RCMP (mrc p4, #0, Rt, c0, c0, #1) reads the processed status and
     * clears the engaged flag (datasheet 3.6.2.5). No operation ever runs
     * here, so the status is zero.
     */
    if (ri->opc1 == 0 && ri->crm == 0 && ri->opc2 == 1) {
        return 0;
    }
    dcp_unimplemented(ri, false, true);
    return 0;
}

static void dcp_write(CPUARMState *env, const ARMCPRegInfo *ri,
                      uint64_t value)
{
    dcp_unimplemented(ri, false, false);
}

static uint64_t dcp_read64(CPUARMState *env, const ARMCPRegInfo *ri)
{
    dcp_unimplemented(ri, true, true);
    return 0;
}

static void dcp_write64(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    dcp_unimplemented(ri, true, false);
}

void rp2350_coproc_init(ARMCPU *cpu, RP2350SIOState *sio)
{
    ARMCPRegInfo gpioc = {
        .name = "RP2350_GPIOC", .cp = RP2350_CP_GPIOC,
        .crn = 0, .crm = CP_ANY, .opc1 = CP_ANY, .opc2 = 0,
        .state = ARM_CP_STATE_AA32, .access = PL0_RW,
        .type = ARM_CP_IO | ARM_CP_NO_RAW,
        .readfn = gpioc_read, .writefn = gpioc_write,
    };
    ARMCPRegInfo dcp = {
        .name = "RP2350_DCP", .cp = RP2350_CP_DCP,
        .crn = 0, .crm = CP_ANY, .opc1 = CP_ANY, .opc2 = CP_ANY,
        .state = ARM_CP_STATE_AA32, .access = PL0_RW,
        .type = ARM_CP_NO_RAW,
        .readfn = dcp_read, .writefn = dcp_write,
    };

    assert(!gpioc_sio || gpioc_sio == sio);
    gpioc_sio = sio;
    define_one_arm_cp_reg(cpu, &gpioc);
    define_one_arm_cp_reg(cpu, &dcp);

    /* MCRR/MRRC have a 4-bit opc1, beyond the reach of CP_ANY */
    for (int opc1 = 0; opc1 < 16; opc1++) {
        ARMCPRegInfo gpioc64 = {
            .name = "RP2350_GPIOC64", .cp = RP2350_CP_GPIOC,
            .crm = CP_ANY, .opc1 = opc1,
            .state = ARM_CP_STATE_AA32, .access = PL0_RW,
            .type = ARM_CP_64BIT | ARM_CP_IO | ARM_CP_NO_RAW,
            .readfn = gpioc_read64, .writefn = gpioc_write64,
        };
        ARMCPRegInfo dcp64 = {
            .name = "RP2350_DCP64", .cp = RP2350_CP_DCP,
            .crm = CP_ANY, .opc1 = opc1,
            .state = ARM_CP_STATE_AA32, .access = PL0_RW,
            .type = ARM_CP_64BIT | ARM_CP_NO_RAW,
            .readfn = dcp_read64, .writefn = dcp_write64,
        };

        define_one_arm_cp_reg(cpu, &gpioc64);
        define_one_arm_cp_reg(cpu, &dcp64);
    }
}
