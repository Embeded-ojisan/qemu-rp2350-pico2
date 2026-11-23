/*
 * QTest for the RP2350 atomic aliases, NVIC and SIO banks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
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
/* Last 32-bit word of the 1 KiB Boot RAM window. */
#define BOOTRAM_WORD            0x400e03fc
#define ACL_LOCK                0x40060000
#define ACL_CFGRESET            0x40060008
#define ACL_GPIO_MASK0          0x4006000c
#define ACL_GPIO_MASK1          0x40060010
#define ACL_SRAM0              0x4006001c
#define ACL_SRAM9              0x40060040
#define ACL_DMA                0x40060044
#define ACL_RESETS             0x40060064
#define ACL_UART0              0x400600a0
#define ACL_KEY                0xacce0000
#define SRAM0_WORD             0x20010000
#define SRAM9_WORD             0x20081000
#define SRAM8_WORD             0x20080080
#define DMA_BASE               0x50000000
#define DMA_READ               (DMA_BASE + 0x000)
#define DMA_WRITE              (DMA_BASE + 0x004)
#define DMA_COUNT              (DMA_BASE + 0x008)
#define DMA_CTRL               (DMA_BASE + 0x00c)
#define DMA_INTR               (DMA_BASE + 0x400)
#define DMA_IRQ_ENABLE         (DMA_BASE + 0x404)
#define DMA_IRQ_STATUS         (DMA_BASE + 0x40c)
#define DMA_SEC_CH0            (DMA_BASE + 0x480)
#define DMA_MPU_CTRL           (DMA_BASE + 0x500)
#define DMA_MPU_BAR0           (DMA_BASE + 0x504)
#define DMA_MPU_LAR0           (DMA_BASE + 0x508)
#define DMA_MPU_BAR1           (DMA_BASE + 0x50c)
#define DMA_MPU_LAR1           (DMA_BASE + 0x510)
#define DMA_WRITE_ERROR        BIT(29)
#define DMA_READ_ERROR         BIT(30)
#define DMA_AHB_ERROR          BIT(31)

enum {
    CPU0 = 0, DMA_MASTER = 2,
    ATTR_SECURE = 1, ATTR_USER = 2, ATTR_DEBUG = 4,
};

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

static void test_bootram_atomic(void)
{
    uint32_t saved = readl(BOOTRAM_WORD);

    writel(BOOTRAM_WORD, 0x317f5402);
    check_all_aliases(BOOTRAM_WORD, 0x317f5402);
    writel(BOOTRAM_WORD + ALIAS_SET, 1);
    g_assert_cmphex(readl(BOOTRAM_WORD), ==, 0x317f5403);
    writel(BOOTRAM_WORD, saved);
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

static uint32_t bus_read(uint64_t addr, unsigned flags, unsigned requester,
                         bool allowed)
{
    uint32_t value;
    unsigned result = qtest_memtxl(global_qtest, addr, flags, requester,
                                    false, &value);

    g_assert_cmpint(result == 0, ==, allowed);
    return value;
}

static void bus_write(uint64_t addr, unsigned flags, unsigned requester,
                      uint32_t value, bool allowed)
{
    unsigned result = qtest_memtxl(global_qtest, addr, flags, requester,
                                    true, &value);

    g_assert_cmpint(result == 0, ==, allowed);
}

static void set_acl(uint32_t addr, uint32_t value)
{
    writel(addr, ACL_KEY | value);
    g_assert_cmphex(readl(addr), ==, value);
}

static void test_accessctrl_registers(void)
{
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(ACL_LOCK), ==, 4);
    g_assert_cmphex(readl(ACL_SRAM9), ==, 0xff);
    g_assert_cmphex(readl(ACL_UART0), ==, 0xfc);
    g_assert_cmphex(readl(ACL_GPIO_MASK1), ==, 0);

    bus_write(ACL_UART0, ATTR_SECURE, CPU0, 0x12, false);
    bus_write(ACL_UART0, ATTR_SECURE | ATTR_USER, CPU0, ACL_KEY | 0x12, false);
    bus_write(ACL_UART0, ATTR_SECURE, DMA_MASTER, ACL_KEY | 0x12, false);
    g_assert_cmphex(bus_read(ACL_UART0, ATTR_USER, CPU0, true), ==, 0xfc);
    bus_write(ACL_UART0 + ALIAS_SET, 0, CPU0, ACL_KEY | 3, true);
    g_assert_cmphex(readl(ACL_UART0), ==, 0xfc);
    set_acl(ACL_UART0, 0x12);
    bus_write(ACL_UART0 + ALIAS_SET, 0, CPU0, ACL_KEY | 9, true);
    g_assert_cmphex(readl(ACL_UART0), ==, 0x13);
    bus_write(ACL_UART0 + ALIAS_CLR, 0, CPU0, ACL_KEY | 1, true);
    g_assert_cmphex(readl(ACL_UART0), ==, 0x12);

    writel(ACL_GPIO_MASK0, 0x80000001);
    writel(ACL_GPIO_MASK1, UINT32_MAX);
    g_assert_cmphex(readl(ACL_GPIO_MASK0), ==, 0x80000001);
    g_assert_cmphex(readl(ACL_GPIO_MASK1), ==, 0xff00ffff);
    writel(ACL_CFGRESET, ACL_KEY | 1);
    g_assert_cmphex(readl(ACL_UART0), ==, 0xfc);
    g_assert_cmphex(readl(ACL_GPIO_MASK0), ==, 0);
    g_assert_cmphex(readl(ACL_GPIO_MASK1), ==, 0);

    writel(ACL_LOCK + ALIAS_SET, ACL_KEY | 1);
    writel(ACL_LOCK + ALIAS_CLR, ACL_KEY | 1);
    writel(ACL_UART0, ACL_KEY | 0x12);
    g_assert_cmphex(readl(ACL_LOCK), ==, 5);
    g_assert_cmphex(readl(ACL_UART0), ==, 0xfc);
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(ACL_LOCK), ==, 4);
}

static void test_filter_matrix(void)
{
    static const struct {
        uint32_t acl;
        uint32_t address;
    } ports[] = {
        { ACL_UART0, UART0_UARTIBRD },
        { ACL_SRAM9, SRAM9_WORD },
    };
    static const unsigned managers[] = { CPU0, DMA_MASTER };

    for (unsigned p = 0; p < ARRAY_SIZE(ports); p++) {
        const uint32_t address = ports[p].address;

        for (unsigned enabled = 0; enabled < BIT(ARRAY_SIZE(managers));
             enabled++) {
            unsigned manager_mask = 0;

            for (unsigned m = 0; m < ARRAY_SIZE(managers); m++) {
                if (enabled & BIT(m)) {
                    manager_mask |= BIT(4 + managers[m]);
                }
            }
            for (unsigned states = 0; states < 16; states++) {
                set_acl(ports[p].acl, manager_mask | states);
                for (unsigned m = 0; m < ARRAY_SIZE(managers); m++) {
                    for (unsigned level = 0; level < 4; level++) {
                        unsigned flags = (level >= 2 ? ATTR_SECURE : 0) |
                                         (level & 1 ? 0 : ATTR_USER);
                        unsigned required = (level & 1) ? BIT(level) :
                                            3u << level;
                        bool allow = (enabled & BIT(m)) &&
                                     (states & required) == required;

                        bus_write(address, ATTR_DEBUG, CPU0, 0x1234, true);
                        uint32_t got = bus_read(address, flags, managers[m],
                                                allow);

                        if (allow) {
                            g_assert_cmphex(got, ==, 0x1234);
                        }
                        bus_write(address, flags, managers[m], 0x5678, allow);
                        g_assert_cmphex(bus_read(address, ATTR_DEBUG, CPU0,
                                                 true),
                                        ==, allow ? 0x5678 : 0x1234);
                    }
                }
            }
        }
        writel(ACL_CFGRESET, ACL_KEY | 1);
    }

    for (unsigned alias = ALIAS_XOR; alias <= ALIAS_CLR; alias += ALIAS_XOR) {
        uint32_t addr = UART0_UARTIBRD + alias;

        set_acl(ACL_UART0, 0x18);
        writel(UART0_UARTIBRD, 0x0e);
        bus_read(addr, 0, CPU0, false);
        bus_write(addr, 0, CPU0, 0x3, false);
        g_assert_cmphex(readl(UART0_UARTIBRD), ==, 0x0e);
        set_acl(ACL_UART0, 0x1a);
        g_assert_cmphex(bus_read(addr, 0, CPU0, true), ==, 0x0e);
        bus_write(addr, 0, CPU0, 0x3, true);
        g_assert_cmphex(readl(UART0_UARTIBRD), ==,
                        alias == ALIAS_XOR ? 0x0d :
                        alias == ALIAS_SET ? 0x0f : 0x0c);
    }
    writel(ACL_CFGRESET, ACL_KEY | 1);
}

static void test_bus_exceptions_and_stripes(void)
{
    writel(BOOTRAM_WORD, 0x31);
    g_assert_cmphex(bus_read(BOOTRAM_WORD, ATTR_SECURE | ATTR_USER, CPU0,
                             true), ==, 0x31);
    bus_read(BOOTRAM_WORD, 0, CPU0, false);
    bus_write(BOOTRAM_WORD + ALIAS_SET, 0, CPU0, 2, false);
    g_assert_cmphex(readl(BOOTRAM_WORD), ==, 0x31);
    bus_read(SIO_GPIO_OUT, ATTR_SECURE, DMA_MASTER, false);
    bus_read(SIO_GPIO_OUT + SIO_NONSEC_ALIAS, 0, CPU0, false);
    bus_read(SIO_GPIO_OUT, 0, CPU0, true);

    set_acl(ACL_RESETS, 0x1a);
    writel(RESETS_RESET + ALIAS_SET, BIT(2));
    bus_write(RESETS_RESET + ALIAS_CLR, 0, CPU0, BIT(2), true);
    g_assert_cmphex(readl(RESETS_RESET) & BIT(2), ==, BIT(2));
    set_acl(ACL_DMA, 0x1a);
    bus_write(RESETS_RESET + ALIAS_CLR, 0, CPU0, BIT(2), true);
    g_assert_cmphex(readl(RESETS_RESET) & BIT(2), ==, 0);

    set_acl(ACL_SRAM0, 0xfc);
    bus_write(SRAM0_WORD, 0, CPU0, 1, false);
    bus_write(SRAM0_WORD + 4, 0, CPU0, 2, true);
    bus_read(SRAM0_WORD, 0, CPU0, false);
    g_assert_cmphex(bus_read(SRAM0_WORD + 4, 0, CPU0, true), ==, 2);
    g_assert_cmphex(readl(SRAM0_WORD + 4), ==, 2);
    writel(ACL_CFGRESET, ACL_KEY | 1);
}

static void dma_copy(uint32_t src, uint32_t dest, unsigned count)
{
    writel(DMA_READ, src);
    writel(DMA_WRITE, dest);
    writel(DMA_COUNT, count);
    /* Enable, word size, increment both addresses, permanent request. */
    writel(DMA_CTRL, 0x007e0059);
    qtest_clock_step(global_qtest, 10);
}

static void test_dma_mpu(void)
{
    qtest_system_reset(global_qtest);
    qtest_irq_intercept_out_named(global_qtest, "/machine/soc/dma",
                                  "sysbus-irq");
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    set_acl(ACL_DMA, 0xff);
    g_assert_cmphex(readl(DMA_SEC_CH0), ==, 3);
    bus_write(DMA_READ, 0, CPU0, SRAM9_WORD, false);
    bus_write(DMA_SEC_CH0, ATTR_USER, CPU0, 1, false);
    writel(DMA_SEC_CH0, 1);
    writel(DMA_IRQ_ENABLE, 1);

    writel(SRAM9_WORD, 0x12345678);
    writel(SRAM9_WORD + 4, 0xaabbccdd);
    dma_copy(SRAM9_WORD, SRAM8_WORD, 2);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x12345678);
    g_assert_cmphex(readl(SRAM8_WORD + 4), ==, 0xaabbccdd);
    g_assert_cmphex(readl(DMA_COUNT), ==, 0);
    g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 1);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(DMA_SEC_CH0, 3);
    g_assert_cmphex(readl(DMA_SEC_CH0), ==, 5);
    writel(DMA_INTR, 1);
    g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    writel(DMA_MPU_BAR0, SRAM9_WORD);
    writel(DMA_MPU_LAR0, SRAM9_WORD | 7);
    writel(DMA_MPU_BAR1, SRAM9_WORD);
    writel(DMA_MPU_LAR1, SRAM9_WORD | 1);
    g_assert_cmphex(bus_read(DMA_MPU_BAR0, 0, CPU0, true), ==, 0);
    bus_read(DMA_MPU_CTRL, ATTR_USER, CPU0, false);
    bus_write(DMA_MPU_LAR0, 0, CPU0, 0, true);
    g_assert_cmphex(readl(DMA_MPU_LAR0), ==, SRAM9_WORD | 7);

    writel(SRAM8_WORD, 0x5a);
    dma_copy(SRAM9_WORD, SRAM8_WORD, 1);
    g_assert_cmphex(readl(DMA_CTRL) & (DMA_AHB_ERROR | DMA_READ_ERROR),
                    ==, DMA_AHB_ERROR | DMA_READ_ERROR);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x5a);
    g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 1);
    g_assert_true(qtest_get_irq(global_qtest, 0));

    writel(DMA_MPU_LAR0, 0);
    dma_copy(SRAM9_WORD, SRAM8_WORD, 1);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x5a);
    writel(DMA_CTRL, DMA_READ_ERROR);
    dma_copy(SRAM9_WORD, SRAM8_WORD, 1);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x12345678);

    writel(DMA_MPU_BAR0, SRAM9_WORD);
    writel(DMA_MPU_LAR0, SRAM9_WORD | 7);
    writel(SRAM8_WORD, 0xcafef00d);
    dma_copy(SRAM8_WORD, SRAM9_WORD, 1);
    g_assert_cmphex(readl(DMA_CTRL) & (DMA_AHB_ERROR | DMA_WRITE_ERROR),
                    ==, DMA_AHB_ERROR | DMA_WRITE_ERROR);
    g_assert_cmphex(readl(SRAM9_WORD), ==, 0x12345678);
    writel(RESETS_RESET + ALIAS_SET, BIT(2));
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    g_assert_cmphex(readl(DMA_SEC_CH0), ==, 3);
    g_assert_cmphex(readl(DMA_MPU_LAR0), ==, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rp2350/atomic/xor-set-clr", test_xor_set_clr);
    qtest_add_func("/rp2350/atomic/resets", test_resets);
    qtest_add_func("/rp2350/atomic/interposer-pl011", test_interposer_pl011);
    qtest_add_func("/rp2350/nvic/ictr", test_nvic_ictr);
    qtest_add_func("/rp2350/bootram/atomic", test_bootram_atomic);
    qtest_add_func("/rp2350/sio/banks", test_sio_banks);
    qtest_add_func("/rp2350/accessctrl/registers", test_accessctrl_registers);
    qtest_add_func("/rp2350/accessctrl/matrix", test_filter_matrix);
    qtest_add_func("/rp2350/accessctrl/exceptions",
                   test_bus_exceptions_and_stripes);
    qtest_add_func("/rp2350/dma/mpu", test_dma_mpu);

    qtest_start("-machine raspi-pico2");
    ret = g_test_run();
    qtest_end();

    return ret;
}
