/*
 * QTest for the RP2350 atomic aliases, NVIC and SIO banks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define ALIAS_XOR       0x1000
#define ALIAS_SET       0x2000
#define ALIAS_CLR       0x3000

/* IO_BANK0 GPIO0_CTRL: plain storage in this model */
#define IO_BANK0_GPIO0_CTRL     (0x40028000 + 0x004)
/* RESETS: RESET_DONE reads the inverse of RESET */
#define RESETS_RESET            (0x40020000 + 0x000)
#define RESETS_RESET_DONE       (0x40020000 + 0x008)
#define RESETS_ALL              0x1fffffff
/* UART0 (PL011, behind the bus interposer): UARTIBRD is 16 bits */
#define UART0_UARTIBRD          (0x40070000 + 0x024)
/* Cortex-M33 ICTR (RP2350 datasheet 3.7.5) */
#define M33_ICTR                0xe000e004
#define SIO_GPIO_OUT            0xd0000010
#define SIO_GPIO_HI_OUT         0xd0000014
#define SIO_SPINLOCK_ST         0xd000005c
#define SIO_SPINLOCK0           0xd0000100
#define SIO_NONSEC_ALIAS        0x00020000

static void check_all_aliases(uint32_t addr, uint32_t expected)
{
    g_assert_cmphex(readl(addr), ==, expected);
    g_assert_cmphex(readl(addr + ALIAS_XOR), ==, expected);
    g_assert_cmphex(readl(addr + ALIAS_SET), ==, expected);
    g_assert_cmphex(readl(addr + ALIAS_CLR), ==, expected);
}

static void test_xor_set_clr(void)
{
    writel(IO_BANK0_GPIO0_CTRL, 0x0000f0f0);
    check_all_aliases(IO_BANK0_GPIO0_CTRL, 0x0000f0f0);

    writel(IO_BANK0_GPIO0_CTRL + ALIAS_XOR, 0x000000ff);
    check_all_aliases(IO_BANK0_GPIO0_CTRL, 0x0000f00f);

    writel(IO_BANK0_GPIO0_CTRL + ALIAS_SET, 0x00030000);
    check_all_aliases(IO_BANK0_GPIO0_CTRL, 0x0003f00f);

    writel(IO_BANK0_GPIO0_CTRL + ALIAS_CLR, 0x0000f001);
    check_all_aliases(IO_BANK0_GPIO0_CTRL, 0x0003000e);
}

static void test_resets(void)
{
    const uint32_t uart0 = 1u << 26;

    g_assert_cmphex(readl(RESETS_RESET), ==, RESETS_ALL);
    g_assert_cmphex(readl(RESETS_RESET_DONE), ==, 0);

    writel(RESETS_RESET + ALIAS_CLR, uart0);
    g_assert_cmphex(readl(RESETS_RESET), ==, RESETS_ALL & ~uart0);
    g_assert_cmphex(readl(RESETS_RESET_DONE), ==, uart0);

    writel(RESETS_RESET + ALIAS_SET, uart0);
    g_assert_cmphex(readl(RESETS_RESET_DONE), ==, 0);
}

static void test_interposer_pl011(void)
{
    writel(UART0_UARTIBRD, 0x0041);
    writel(UART0_UARTIBRD + ALIAS_SET, 0x1000);
    g_assert_cmphex(readl(UART0_UARTIBRD), ==, 0x1041);
    writel(UART0_UARTIBRD + ALIAS_CLR, 0x0001);
    g_assert_cmphex(readl(UART0_UARTIBRD), ==, 0x1040);
    writel(UART0_UARTIBRD + ALIAS_XOR, 0x1100);
    g_assert_cmphex(readl(UART0_UARTIBRD), ==, 0x0140);
}

static void test_nvic_ictr(void)
{
    /* RP2350's 52 IRQ inputs span two banks. */
    g_assert_cmphex(readl(M33_ICTR), ==, 1);
}

static void test_sio_banks(void)
{
    writel(SIO_GPIO_OUT, 3);
    g_assert_cmphex(readl(SIO_GPIO_OUT + SIO_NONSEC_ALIAS), ==, 0);
    writel(SIO_GPIO_OUT + SIO_NONSEC_ALIAS, 0);
    g_assert_cmphex(readl(SIO_GPIO_OUT), ==, 3);
    writel(SIO_GPIO_HI_OUT, 0xffffffff);
    g_assert_cmphex(readl(SIO_GPIO_HI_OUT), ==, 0xff00ffff);
    g_assert_cmphex(readl(SIO_GPIO_HI_OUT + SIO_NONSEC_ALIAS), ==, 0);

    g_assert_cmphex(readl(SIO_SPINLOCK0), ==, 1);
    g_assert_cmphex(readl(SIO_SPINLOCK_ST + SIO_NONSEC_ALIAS), ==, 0);
    g_assert_cmphex(readl(SIO_SPINLOCK0 + SIO_NONSEC_ALIAS), ==, 1);
    g_assert_cmphex(readl(SIO_SPINLOCK0), ==, 0);
    writel(SIO_SPINLOCK0 + SIO_NONSEC_ALIAS, 0);
    g_assert_cmphex(readl(SIO_SPINLOCK_ST + SIO_NONSEC_ALIAS), ==, 0);
    g_assert_cmphex(readl(SIO_SPINLOCK_ST), ==, 1);
    g_assert_cmphex(readl(SIO_SPINLOCK0 + SIO_NONSEC_ALIAS), ==, 1);

    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(SIO_SPINLOCK_ST), ==, 0);
    g_assert_cmphex(readl(SIO_SPINLOCK_ST + SIO_NONSEC_ALIAS), ==, 0);
    g_assert_cmphex(readl(SIO_GPIO_OUT), ==, 0);
    g_assert_cmphex(readl(SIO_GPIO_HI_OUT), ==, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2350/atomic/xor-set-clr", test_xor_set_clr);
    qtest_add_func("/rp2350/atomic/resets", test_resets);
    qtest_add_func("/rp2350/atomic/interposer-pl011", test_interposer_pl011);
    qtest_add_func("/rp2350/nvic/ictr", test_nvic_ictr);
    qtest_add_func("/rp2350/sio/banks", test_sio_banks);

    qtest_start("-machine raspi-pico2");
    ret = g_test_run();
    qtest_end();

    return ret;
}
