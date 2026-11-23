/*
 * RP2350 Cortex-M33 Secure-to-Non-secure SIO and GPIO smoke test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include "test.h"
#include "hardware/gpio.h"
#include "hardware/regs/accessctrl.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/regs/pads_bank0.h"
#include "hardware/regs/sio.h"

#define SIO(offset) REG(SIO_BASE, offset)
#define ACCESSCTRL(offset) REG(ACCESSCTRL_BASE, offset)

#define TEST_PINS 3u
#define NS_PINS (RP2350_TEST_ACCESSCTRL ? 2u : 0u)
#define SIO_FUNC IO_BANK0_GPIO0_CTRL_FUNCSEL_VALUE_SIO_0
#define NULL_FUNC IO_BANK0_GPIO0_CTRL_FUNCSEL_VALUE_NULL
#define MIRROR_LOCK (SIO_NONSEC_BASE + SIO_SPINLOCK0_OFFSET)

/* The SDK copies this code to scratch X, which also holds the NS stack. */
static unsigned __scratch_x("ns") ns_gpio(void)
{
    uint64_t ns_pins = NS_PINS | ((uint64_t)NS_PINS << 32);

    if (SIO(SIO_SPINLOCK_ST_OFFSET) || SIO(SIO_SPINLOCK0_OFFSET) != 1 ||
        SIO(SIO_SPINLOCK0_OFFSET) || SIO(SIO_SPINLOCK_ST_OFFSET) != 1) {
        return 1;
    }
    if (SIO(SIO_GPIO_OUT_OFFSET) != NS_PINS ||
        SIO(SIO_GPIO_HI_OE_OFFSET) != NS_PINS ||
        gpioc_hilo_in_get() != ns_pins) {
        return 2;
    }
    SIO(SIO_GPIO_OUT_OFFSET) = 0;
    SIO(SIO_GPIO_HI_OUT_CLR_OFFSET) = TEST_PINS;
    gpioc_hilo_oe_xor(TEST_PINS | ((uint64_t)TEST_PINS << 32));
    if (gpioc_lo_out_get() || gpioc_hilo_out_get() || gpioc_hilo_oe_get()) {
        return 3;
    }
    SIO(SIO_GPIO_OUT_XOR_OFFSET) = TEST_PINS;
    gpioc_hi_out_set(TEST_PINS);
    if (gpioc_hilo_out_get() != ns_pins) {
        return 4;
    }
    SIO(SIO_GPIO_OUT_CLR_OFFSET) = TEST_PINS;
    gpioc_hilo_out_put(0);

    if (REG(IO_BANK0_BASE, IO_BANK0_GPIO0_CTRL_OFFSET) ||
        REG(PADS_BANK0_BASE, PADS_BANK0_GPIO0_OFFSET) ||
        REG(IO_BANK0_BASE, IO_BANK0_GPIO1_CTRL_OFFSET) !=
            (NS_PINS ? SIO_FUNC : 0) ||
        REG(PADS_BANK0_BASE, PADS_BANK0_GPIO1_OFFSET) !=
            (NS_PINS ? 0x50 : 0)) {
        return 5;
    }
    REG(IO_BANK0_BASE, IO_BANK0_GPIO0_CTRL_OFFSET) = NULL_FUNC;
    REG(IO_BANK0_BASE, IO_BANK0_GPIO1_CTRL_OFFSET) = NULL_FUNC;
    REG(PADS_BANK0_BASE + REG_ALIAS_CLR_BITS, PADS_BANK0_GPIO0_OFFSET) = 0x40;
    REG(PADS_BANK0_BASE + REG_ALIAS_CLR_BITS, PADS_BANK0_GPIO1_OFFSET) = 0x40;
    return 0;
}

static unsigned __scratch_x("ns") ns_read_mirror(void)
{
    return REG(MIRROR_LOCK, 0);
}

void isr_busfault(void)
{
    if (precise_busfault_at(MIRROR_LOCK)) {
        puts("PASS: Non-secure SIO mirror BusFault");
    } else {
        puts("FAIL: unexpected BusFault");
    }
    wait_for_stop();
}

int main(void)
{
    unsigned result;

    stdio_uart_init();
    puts("Secure: preparing SIO and GPIO");
    SIO(SIO_GPIO_OUT_OFFSET) = TEST_PINS;
    SIO(SIO_GPIO_HI_OUT_OFFSET) = TEST_PINS;
    SIO(SIO_GPIO_OE_OFFSET) = TEST_PINS;
    SIO(SIO_GPIO_HI_OE_OFFSET) = TEST_PINS;
    REG(IO_BANK0_BASE, IO_BANK0_GPIO0_CTRL_OFFSET) = SIO_FUNC;
    REG(IO_BANK0_BASE, IO_BANK0_GPIO1_CTRL_OFFSET) = SIO_FUNC;
    REG(PADS_BANK0_BASE, PADS_BANK0_GPIO0_OFFSET) = 0x50;
    REG(PADS_BANK0_BASE, PADS_BANK0_GPIO1_OFFSET) = 0x50;

#if RP2350_TEST_ACCESSCTRL
    ACCESSCTRL(ACCESSCTRL_GPIO_NSMASK0_OFFSET) = NS_PINS;
    ACCESSCTRL(ACCESSCTRL_GPIO_NSMASK1_OFFSET) = NS_PINS;
#endif
    /* Exercise the per-pin mask through peripherals visible to NSP. */
    ACCESSCTRL(ACCESSCTRL_IO_BANK0_OFFSET) = 0xacce0000u |
        ACCESSCTRL_IO_BANK0_RESET | ACCESSCTRL_IO_BANK0_NSP_BITS;
    ACCESSCTRL(ACCESSCTRL_PADS_BANK0_OFFSET) = 0xacce0000u |
        ACCESSCTRL_PADS_BANK0_RESET | ACCESSCTRL_PADS_BANK0_NSP_BITS;
    if (ACCESSCTRL(ACCESSCTRL_GPIO_NSMASK0_OFFSET) != NS_PINS ||
        ACCESSCTRL(ACCESSCTRL_GPIO_NSMASK1_OFFSET) != NS_PINS) {
        puts("FAIL: ACCESSCTRL GPIO assignment did not take effect");
        wait_for_stop();
    }
    if (SIO(SIO_SPINLOCK0_OFFSET) != 1) {
        puts("FAIL: Secure spinlock claim");
        wait_for_stop();
    }

    M33(M33_NSACR_OFFSET) |= M33_NSACR_CP0_BITS;
    REG(PPB_NONSEC_BASE, M33_CPACR_OFFSET) = M33_CPACR_CP0_BITS;
    prepare_ns();
    sau_region(1, SIO_BASE, SIO_NONSEC_BASE + 0x1ffff);
    sau_region(2, IO_BANK0_BASE, IO_BANK0_BASE + 0x3fff);
    sau_region(3, PADS_BANK0_BASE, PADS_BANK0_BASE + 0x3fff);
    sau_enable(true);
    result = ((ns_fn *)cmse_nsfptr_create(&ns_gpio))();
    sau_enable(false);

    if (result || SIO(SIO_SPINLOCK_ST_OFFSET) != 1 ||
        REG(SIO_NONSEC_BASE, SIO_SPINLOCK_ST_OFFSET) != 1 ||
        SIO(SIO_GPIO_OUT_OFFSET) != (TEST_PINS & ~NS_PINS) ||
        SIO(SIO_GPIO_HI_OUT_OFFSET) != (TEST_PINS & ~NS_PINS) ||
        SIO(SIO_GPIO_OE_OFFSET) != (TEST_PINS & ~NS_PINS) ||
        SIO(SIO_GPIO_HI_OE_OFFSET) != (TEST_PINS & ~NS_PINS) ||
        REG(IO_BANK0_BASE, IO_BANK0_GPIO0_CTRL_OFFSET) != SIO_FUNC ||
        REG(IO_BANK0_BASE, IO_BANK0_GPIO1_CTRL_OFFSET) !=
            (NS_PINS ? NULL_FUNC : SIO_FUNC) ||
        REG(PADS_BANK0_BASE, PADS_BANK0_GPIO0_OFFSET) != 0x50 ||
        REG(PADS_BANK0_BASE, PADS_BANK0_GPIO1_OFFSET) !=
            (NS_PINS ? 0x10 : 0x50)) {
        printf("FAIL: SIO/GPIO isolation (Non-secure result %u)\n", result);
        wait_for_stop();
    }
    puts("PASS: Secure to Non-secure SIO/GPIO isolation");

    sau_enable(true);
    ((ns_fn *)cmse_nsfptr_create(&ns_read_mirror))();
    puts("FAIL: Non-secure SIO mirror was accessible");
    wait_for_stop();
}
