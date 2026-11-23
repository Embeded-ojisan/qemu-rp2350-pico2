/*
 * QTest for the RP2350 atomic register access aliases and NVIC
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

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2350/atomic/xor-set-clr", test_xor_set_clr);
    qtest_add_func("/rp2350/atomic/resets", test_resets);
    qtest_add_func("/rp2350/atomic/interposer-pl011", test_interposer_pl011);
    qtest_add_func("/rp2350/nvic/ictr", test_nvic_ictr);

    qtest_start("-machine raspi-pico2");
    ret = g_test_run();
    qtest_end();

    return ret;
}
