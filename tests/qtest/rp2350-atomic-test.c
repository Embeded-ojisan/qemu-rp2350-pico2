/*
 * QTest for RP2350 peripherals and the system bus
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
#define ACL_PIO0               0x4006004c
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
#define DMA_AL1_CTRL           (DMA_BASE + 0x010)
#define DMA_CH_STRIDE          0x040
#define DMA_INTR               (DMA_BASE + 0x400)
#define DMA_IRQ_ENABLE         (DMA_BASE + 0x404)
#define DMA_IRQ_STATUS         (DMA_BASE + 0x40c)
#define DMA_PACING(n)          (DMA_BASE + 0x440 + 4 * (n))
#define DMA_ABORT              (DMA_BASE + 0x464)
#define DMA_SEC_CH0            (DMA_BASE + 0x480)
#define DMA_SEC_MISC           (DMA_BASE + 0x4d0)
#define DMA_MPU_CTRL           (DMA_BASE + 0x500)
#define DMA_MPU_BAR0           (DMA_BASE + 0x504)
#define DMA_MPU_LAR0           (DMA_BASE + 0x508)
#define DMA_MPU_BAR1           (DMA_BASE + 0x50c)
#define DMA_MPU_LAR1           (DMA_BASE + 0x510)
#define DMA_ENABLE             BIT(0)
#define DMA_WORD               BIT(3)
#define DMA_INCR_READ          BIT(4)
#define DMA_INCR_WRITE         BIT(6)
#define DMA_CHAIN_TO(n)        ((n) << 13)
#define DMA_TREQ(n)            ((n) << 17)
#define DMA_TREQ_TIMER(n)      DMA_TREQ(0x3b + (n))
#define DMA_TREQ_FORCE         DMA_TREQ(0x3f)
#define DMA_BUSY               BIT(26)
#define DMA_WRITE_ERROR        BIT(29)
#define DMA_READ_ERROR         BIT(30)
#define DMA_AHB_ERROR          BIT(31)
#define QMI_CSR                0x400d0000
#define QMI_TX                 0x400d0004
#define QMI_RX                 0x400d0008
#define QMI_EN                 BIT(0)
#define QMI_CS0                BIT(2)
#define QMI_RXEMPTY            BIT(16)
#define QMI_DWIDTH             BIT(18)
#define QMI_NOPUSH             BIT(20)
#define XIP_BASE               0x10000000
#define XIP_NOCACHE            0x14000000
#define XIP_NOTRANSLATE        0x1c000000
#define TRNG_ISR               0x400f0104
#define TRNG_ICR               0x400f0108
#define TRNG_VALID             0x400f0110
#define TRNG_EHR0              0x400f0114
#define TRNG_ENABLE            0x400f012c
#define SHA_CSR                0x400f8000
#define SHA_WDATA              0x400f8004
#define SHA_SUM0               0x400f8008
#define OTP_LOCK3              0x4012000c
#define OTP_PROGRAM_ROW        0x40120f00
#define OTP_PROGRAM_DATA       0x40120f04
#define OTP_DATA3              (0x40130000 + 0xc0 * 2)
#define OTP_GUARDED3           (0x40138000 + 0xc0 * 2)
#define TIMER0_RAW_LO          0x400b0028
#define TIMER1_RAW_LO          0x400b8028
#define PIO0_BASE              0x50200000
#define PIO_CTRL               (PIO0_BASE + 0x000)
#define PIO_FSTAT              (PIO0_BASE + 0x004)
#define PIO_FDEBUG             (PIO0_BASE + 0x008)
#define PIO_FLEVEL             (PIO0_BASE + 0x00c)
#define PIO_TXF0               (PIO0_BASE + 0x010)
#define PIO_DBG_PADOUT         (PIO0_BASE + 0x03c)
#define PIO_IMEM(n)            (PIO0_BASE + 0x048 + 4 * (n))
#define PIO_SM0_SHIFTCTRL      (PIO0_BASE + 0x0d0)
#define PIO_SM0_ADDR           (PIO0_BASE + 0x0d4)
#define PIO_SM0_INSTR          (PIO0_BASE + 0x0d8)
#define PIO_SM0_PINCTRL        (PIO0_BASE + 0x0dc)
#define PIO_SM_STRIDE          0x018
#define PIO_TX0_EMPTY          BIT(24)
#define PIO_TX0_STALL          BIT(24)
#define PIO0_RESET             BIT(11)
#define LED_PIN                25
#define LED_STATUS             (IO_BANK0_GPIO0_CTRL - 4 + 8 * LED_PIN)
#define LED_CTRL               (LED_STATUS + 4)
#define LED_PAD                (0x40038004 + 4 * LED_PIN)
#define GPIO_OUTTOPAD          BIT(9)
#define GPIO_OETOPAD           BIT(13)

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

static void qmi_begin_command(uint8_t command)
{
    writel(QMI_CSR, QMI_EN | QMI_CS0);
    writel(QMI_TX, QMI_NOPUSH | command);
}

static void test_qmi_direct(void)
{
    const uint32_t offset = 0x3f0000;

    g_assert_cmphex(readl(XIP_BASE + offset), ==, 0xffffffff);
    writel(QMI_CSR, QMI_EN);
    bus_read(XIP_BASE + offset, ATTR_SECURE, CPU0, false);
    bus_read(XIP_NOCACHE + offset, ATTR_SECURE, CPU0, false);
    bus_read(XIP_NOTRANSLATE + offset, ATTR_SECURE, CPU0, false);

    writel(QMI_CSR, QMI_EN | QMI_CS0);
    writel(QMI_TX, QMI_DWIDTH | 0x9f); /* JEDEC ID, command then dummy */
    g_assert_cmphex(readl(QMI_RX) >> 8, ==, 0xef);
    writel(QMI_TX, 0);
    writel(QMI_TX, 0);
    g_assert_cmphex(readl(QMI_RX), ==, 0x40);
    g_assert_cmphex(readl(QMI_RX), ==, 0x16);
    g_assert_true(readl(QMI_CSR) & QMI_RXEMPTY);
    writel(QMI_CSR, QMI_EN);

    qmi_begin_command(0x06); /* WREN */
    writel(QMI_CSR, QMI_EN);
    qmi_begin_command(0x02); /* PAGE PROGRAM */
    writel(QMI_TX, QMI_NOPUSH | (offset >> 16));
    writel(QMI_TX, QMI_NOPUSH);
    writel(QMI_TX, QMI_NOPUSH);
    writel(QMI_TX, QMI_NOPUSH | 0x12);
    writel(QMI_CSR, 0);
    g_assert_cmphex(readl(XIP_BASE + offset), ==, 0xffffff12);
    g_assert_cmphex(readl(XIP_NOCACHE + offset), ==, 0xffffff12);
    g_assert_cmphex(readl(XIP_NOTRANSLATE + offset), ==, 0xffffff12);

    qmi_begin_command(0x06);
    writel(QMI_CSR, QMI_EN);
    qmi_begin_command(0x20); /* SECTOR ERASE */
    writel(QMI_TX, QMI_NOPUSH | (offset >> 16));
    writel(QMI_TX, QMI_NOPUSH);
    writel(QMI_TX, QMI_NOPUSH);
    writel(QMI_CSR, 0);
    g_assert_cmphex(readl(XIP_BASE + offset), ==, 0xffffffff);
}

static void test_trng(void)
{
    uint32_t samples[2][6];

    g_assert_cmphex(readl(TRNG_VALID), ==, 0);
    writel(TRNG_ENABLE, 1);
    g_assert_cmphex(readl(TRNG_VALID), ==, 1);
    g_assert_cmphex(readl(TRNG_ISR), ==, 1);
    writel(TRNG_ICR, 1);
    g_assert_cmphex(readl(TRNG_ISR), ==, 0);
    for (unsigned j = 0; j < ARRAY_SIZE(samples); j++) {
        for (unsigned i = 0; i < ARRAY_SIZE(samples[j]); i++) {
            samples[j][i] = readl(TRNG_EHR0 + 4 * i);
        }
    }
    g_assert_cmpint(memcmp(samples[0], samples[1], sizeof(samples[0])), !=, 0);
    g_assert_cmphex(readl(TRNG_ISR), ==, 1);
    writel(TRNG_ENABLE, 0);
    g_assert_cmphex(readl(TRNG_VALID), ==, 0);
}

static void test_sha256(void)
{
    const uint32_t expected[] = {
        0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
        0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad,
    };
    const uint32_t two_blocks[] = {
        0x248d6a61, 0xd20638b8, 0xe5c02693, 0x0c3e6039,
        0xa33ce459, 0x64ff2167, 0xf6ecedd4, 0x19db06c1,
    };
    /* FIPS test message followed by padding and its 448-bit length. */
    const uint8_t message[128] = {
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\x80"
    };

    for (unsigned swap = 0; swap < 2; swap++) {
        uint32_t first = swap ? 0x80636261 : 0x61626380;
        uint32_t last = swap ? 0x18000000 : 0x18;

        writel(SHA_CSR, 0x201 | (swap << 12));
        g_assert_cmphex(readl(SHA_CSR) & 7, ==, 6);
        writel(SHA_WDATA, first);
        g_assert_cmphex(readl(SHA_CSR) & 7, ==, 2);
        for (unsigned i = 1; i < 15; i++) {
            writel(SHA_WDATA, 0);
        }
        writel(SHA_WDATA, last);
        g_assert_cmphex(readl(SHA_CSR) & 7, ==, 6);
        for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
            g_assert_cmphex(readl(SHA_SUM0 + 4 * i), ==, expected[i]);
        }
    }

    writel(SHA_CSR, 0x1201);
    for (unsigned i = 0; i < 126; i++) {
        writeb(SHA_WDATA, message[i]);
    }
    writeb(SHA_WDATA, 1);
    writeb(SHA_WDATA, 0xc0);
    for (unsigned i = 0; i < ARRAY_SIZE(two_blocks); i++) {
        g_assert_cmphex(readl(SHA_SUM0 + 4 * i), ==, two_blocks[i]);
    }
}

static void test_otp(void)
{
    g_assert_cmphex(readl(OTP_DATA3), ==, 0);
    writel(OTP_PROGRAM_ROW, 0xc0);
    writel(OTP_PROGRAM_DATA, 0x1234);
    writel(OTP_PROGRAM_DATA, 0x4020);
    g_assert_cmphex(readl(OTP_DATA3), ==, 0x5234);

    writel(OTP_LOCK3, 1);
    bus_write(OTP_PROGRAM_DATA, ATTR_SECURE, CPU0, 0xffff, false);
    g_assert_cmphex(readl(OTP_DATA3), ==, 0x5234);
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(OTP_LOCK3), ==, 0);
    g_assert_cmphex(readl(OTP_DATA3), ==, 0x5234);

    writel(OTP_PROGRAM_ROW, 0xf81 + 2 * 3);
    writel(OTP_PROGRAM_DATA, 0x000303);
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(OTP_LOCK3), ==, 3);
    g_assert_cmphex(readl(OTP_DATA3), ==, 0xffffffff);
    bus_read(OTP_GUARDED3, ATTR_SECURE, CPU0, false);
}

static void test_timers(void)
{
    uint32_t timer0 = readl(TIMER0_RAW_LO);
    uint32_t timer1 = readl(TIMER1_RAW_LO);

    qtest_clock_step(global_qtest, 100000);
    g_assert_cmpuint(readl(TIMER0_RAW_LO) - timer0, ==, 100);
    g_assert_cmpuint(readl(TIMER1_RAW_LO) - timer1, ==, 100);
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

static void dma_start(uint32_t src, uint32_t dest, unsigned count,
                      uint32_t ctrl)
{
    writel(DMA_READ, src);
    writel(DMA_WRITE, dest);
    writel(DMA_COUNT, count);
    writel(DMA_CTRL, DMA_ENABLE | DMA_WORD | ctrl);
}

static void dma_copy(uint32_t src, uint32_t dest, unsigned count)
{
    dma_start(src, dest, count,
              DMA_INCR_READ | DMA_INCR_WRITE | DMA_TREQ_FORCE);
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

static void test_dma_pio_tx(void)
{
    const uint32_t data[] = {
        1, 0x3456, 0x89abcdef, 0x44226688, 0x8765, 7, 0xaaaa, 8, 0xbbb, 0xc,
    };

    for (unsigned sm = 0; sm < 4; sm++) {
        unsigned shift = 8 * sm;
        unsigned sm_offset = PIO_SM_STRIDE * sm;
        bool joined = sm & 1;
        unsigned depth = joined ? 8 : 4;
        unsigned words = depth + 2;

        qtest_system_reset(global_qtest);
        writel(RESETS_RESET + ALIAS_CLR, BIT(2));
        writel(DMA_IRQ_ENABLE, 1);
        for (unsigned i = 0; i < words; i++) {
            writel(SRAM9_WORD + 4 * i, data[i]);
        }
        dma_start(SRAM9_WORD, PIO_TXF0 + 4 * sm, words,
                  DMA_INCR_READ | DMA_TREQ(sm));
        qtest_clock_step(global_qtest, 100);
        g_assert_cmpuint(readl(DMA_COUNT), ==, words);

        writel(RESETS_RESET + ALIAS_CLR, PIO0_RESET);
        if (joined) {
            writel(PIO_SM0_SHIFTCTRL + sm_offset + ALIAS_SET, BIT(30));
        }
        qtest_clock_step(global_qtest, 100);
        g_assert_cmphex(readl(PIO_FLEVEL), ==, depth << shift);
        g_assert_cmpuint(readl(DMA_COUNT), ==, 2);
        g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, DMA_BUSY);
        g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 0);

        writel(PIO_SM0_PINCTRL + sm_offset, 32u << 20);
        writel(PIO_IMEM(0), 0x80a0); /* PULL BLOCK */
        writel(PIO_IMEM(1), 0x6000); /* OUT PINS, 32 */
        writel(PIO_IMEM(2), 0x0002); /* JMP 2 */
        writel(PIO_CTRL + ALIAS_SET, BIT(sm));
        qtest_clock_step(global_qtest, 50);
        writel(PIO_CTRL + ALIAS_CLR, BIT(sm));
        g_assert_cmphex(readl(PIO_DBG_PADOUT), ==, data[0]);
        g_assert_cmphex(readl(PIO_FLEVEL), ==, depth << shift);
        g_assert_cmpuint(readl(DMA_COUNT), ==, 1);
        g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 0);

        for (unsigned i = 1; i < words; i++) {
            writel(PIO_SM0_INSTR + sm_offset, 0x80a0);
            writel(PIO_SM0_INSTR + sm_offset, 0x6000);
            qtest_clock_step(global_qtest, 20);
            g_assert_cmphex(readl(PIO_DBG_PADOUT), ==, data[i]);
        }
        g_assert_cmphex(readl(PIO_FLEVEL), ==, 0);
        g_assert_cmphex(readl(PIO_FDEBUG), ==, 0);
        g_assert_cmphex(readl(DMA_READ), ==, SRAM9_WORD + 4 * words);
        g_assert_cmphex(readl(DMA_WRITE), ==, PIO_TXF0 + 4 * sm);
        g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, 0);
        g_assert_cmpuint(readl(DMA_COUNT), ==, 0);
        g_assert_cmphex(readl(DMA_IRQ_STATUS), ==, 1);
    }
}

static void test_dma_pio_tx_access(void)
{
    qtest_system_reset(global_qtest);
    writel(RESETS_RESET + ALIAS_CLR, BIT(2) | PIO0_RESET);
    writel(DMA_SEC_CH0, 1); /* NSP channel */
    writel(SRAM9_WORD, 0x12345678);
    dma_start(SRAM9_WORD, PIO_TXF0, 1, DMA_TREQ(0));
    qtest_clock_step(global_qtest, 100);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 1);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_AHB_ERROR, ==, 0);

    set_acl(ACL_PIO0, 0xfd); /* NSU without NSP is ineffective. */
    qtest_clock_step(global_qtest, 100);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, DMA_BUSY);
    g_assert_cmphex(readl(PIO_FLEVEL), ==, 0);

    set_acl(ACL_PIO0, 0x1a); /* NSP DREQ is visible, DMA bus port is denied. */
    qtest_clock_step(global_qtest, 100);
    g_assert_cmphex(readl(DMA_CTRL) & (DMA_AHB_ERROR | DMA_WRITE_ERROR),
                    ==, DMA_AHB_ERROR | DMA_WRITE_ERROR);
    g_assert_cmphex(readl(PIO_FLEVEL), ==, 0);

    set_acl(ACL_PIO0, 0x5a);
    writel(DMA_CTRL, DMA_WRITE_ERROR);
    writel(DMA_MPU_BAR0, PIO_TXF0 & ~31u);
    writel(DMA_MPU_LAR0, (PIO_TXF0 & ~31u) | 7); /* SP-only destination */
    dma_start(SRAM9_WORD, PIO_TXF0, 1, DMA_TREQ(0));
    qtest_clock_step(global_qtest, 100);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_WRITE_ERROR, ==, DMA_WRITE_ERROR);
    g_assert_cmphex(readl(PIO_FLEVEL), ==, 0);

    writel(DMA_CTRL, DMA_WRITE_ERROR);
    writel(DMA_MPU_LAR0, 0);
    dma_start(SRAM9_WORD, PIO_TXF0, 1, DMA_TREQ(0));
    qtest_clock_step(global_qtest, 100);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 0);
    g_assert_cmphex(readl(PIO_FLEVEL), ==, 1);
}

static void test_dma_pacing(void)
{
    const unsigned delays[] = { 27, 20, 20 }; /* X/Y = 3/10 at 150 MHz */

    for (unsigned timer = 0; timer < 4; timer++) {
        qtest_system_reset(global_qtest);
        writel(RESETS_RESET + ALIAS_CLR, BIT(2));
        writel(DMA_PACING(timer), (3u << 16) | 10);
        g_assert_cmphex(readl(DMA_PACING(timer)), ==, (3u << 16) | 10);
        for (unsigned i = 0; i < ARRAY_SIZE(delays); i++) {
            writel(SRAM9_WORD + 4 * i, i + 1);
            writel(SRAM8_WORD + 4 * i, 0);
        }
        dma_start(SRAM9_WORD, SRAM8_WORD, ARRAY_SIZE(delays),
                  DMA_INCR_READ | DMA_INCR_WRITE | DMA_TREQ_TIMER(timer));

        for (unsigned i = 0; i < ARRAY_SIZE(delays); i++) {
            qtest_clock_step(global_qtest, delays[i] - 1);
            g_assert_cmpuint(readl(DMA_COUNT), ==, ARRAY_SIZE(delays) - i);
            g_assert_cmphex(readl(SRAM8_WORD + 4 * i), ==, 0);
            qtest_clock_step(global_qtest, 1);
            g_assert_cmpuint(readl(DMA_COUNT), ==, ARRAY_SIZE(delays) - i - 1);
            g_assert_cmphex(readl(SRAM8_WORD + 4 * i), ==, i + 1);
        }
        g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, 0);
        g_assert_cmphex(readl(DMA_INTR), ==, 1);
    }
}

static void test_dma_pacing_stop(void)
{
    uint32_t ctrl = DMA_INCR_WRITE | DMA_TREQ_TIMER(0);
    uint32_t ratio = (1u << 16) | 150; /* 1 us per request */

    qtest_system_reset(global_qtest);
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    writel(SRAM9_WORD, 0x2a);
    writel(SRAM8_WORD, 0);
    writel(SRAM8_WORD + 4, 0);
    dma_start(SRAM9_WORD, SRAM8_WORD, 2, DMA_TREQ(4)); /* PIO0 RX0 */
    qtest_clock_step(global_qtest, 2000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 2);
    writel(DMA_ABORT, 1);

    dma_start(SRAM9_WORD, SRAM8_WORD, 2, ctrl);
    writel(DMA_PACING(0), 150); /* Zero X */
    qtest_clock_step(global_qtest, 2000);
    writel(DMA_PACING(0), 1u << 16); /* Zero Y */
    qtest_clock_step(global_qtest, 2000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 2);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0);

    writel(DMA_PACING(0), ratio);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 1);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x2a);
    writel(DMA_CTRL + ALIAS_CLR, DMA_ENABLE);
    qtest_clock_step(global_qtest, 3000);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, DMA_BUSY);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 1);
    writel(DMA_CTRL + ALIAS_SET, DMA_ENABLE);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmphex(readl(SRAM8_WORD + 4), ==, 0x2a);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 0);

    writel(DMA_INTR, 1);
    writel(SRAM8_WORD, 0);
    dma_start(SRAM9_WORD, SRAM8_WORD, 1, ctrl);
    qtest_clock_step(global_qtest, 999);
    writel(DMA_PACING(0), 0);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 1);

    writel(DMA_PACING(0), ratio);
    qtest_clock_step(global_qtest, 999);
    writel(DMA_ABORT, 1);
    qtest_clock_step(global_qtest, 2000);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_BUSY, ==, 0);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0);
    g_assert_cmphex(readl(DMA_INTR), ==, 0);

    dma_start(SRAM9_WORD, SRAM8_WORD, 1, ctrl);
    writel(RESETS_RESET + ALIAS_SET, BIT(2));
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    qtest_clock_step(global_qtest, 2000);
    g_assert_cmphex(readl(DMA_PACING(0)), ==, 0);
    g_assert_cmphex(readl(DMA_CTRL), ==, 0);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0);
    g_assert_cmphex(readl(DMA_INTR), ==, 0);

    /* A beat that asserts DMA reset must leave the channel cleared. */
    writel(DMA_PACING(0), ratio);
    writel(SRAM9_WORD, BIT(2));
    dma_start(SRAM9_WORD, RESETS_RESET + ALIAS_SET, 1, ctrl);
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmphex(readl(RESETS_RESET) & BIT(2), ==, BIT(2));
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    g_assert_cmphex(readl(DMA_READ), ==, 0);
    g_assert_cmphex(readl(DMA_WRITE), ==, 0);
    g_assert_cmphex(readl(DMA_COUNT), ==, 0);
    g_assert_cmphex(readl(DMA_INTR), ==, 0);
}

static void test_dma_pacing_chain(void)
{
    qtest_system_reset(global_qtest);
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    writel(SRAM9_WORD, 0x12);
    writel(SRAM9_WORD + 4, 0x34);
    writel(SRAM8_WORD + 4, 0);
    writel(DMA_READ + DMA_CH_STRIDE, SRAM9_WORD + 4);
    writel(DMA_WRITE + DMA_CH_STRIDE, SRAM8_WORD + 4);
    writel(DMA_COUNT + DMA_CH_STRIDE, 1);
    writel(DMA_AL1_CTRL + DMA_CH_STRIDE,
           DMA_ENABLE | DMA_WORD | DMA_CHAIN_TO(1) | DMA_TREQ_FORCE);

    writel(DMA_PACING(0), (2u << 16) | 1); /* Clamp at one request per cycle. */
    dma_start(SRAM9_WORD, SRAM8_WORD, 1, DMA_TREQ_TIMER(0) | DMA_CHAIN_TO(1));
    qtest_clock_step(global_qtest, 6);
    g_assert_cmphex(readl(DMA_INTR), ==, 0);
    g_assert_cmphex(readl(SRAM8_WORD + 4), ==, 0);
    qtest_clock_step(global_qtest, 10);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x12);
    g_assert_cmphex(readl(SRAM8_WORD + 4), ==, 0x34);
    g_assert_cmphex(readl(DMA_INTR), ==, 3);
}

static void test_dma_pacing_access(void)
{
    uint32_t ratio = (1u << 16) | 150;

    qtest_system_reset(global_qtest);
    writel(RESETS_RESET + ALIAS_CLR, BIT(2));
    set_acl(ACL_DMA, 0xff);
    writel(DMA_SEC_CH0, 1);
    writel(DMA_PACING(3), ratio);
    bus_read(DMA_PACING(3), 0, CPU0, false);
    bus_write(DMA_PACING(3) + ALIAS_CLR, 0, CPU0, ratio, false);
    g_assert_cmphex(readl(DMA_PACING(3)), ==, ratio);
    writel(SRAM9_WORD, 0x5a);
    writel(SRAM8_WORD, 0);
    dma_start(SRAM9_WORD, SRAM8_WORD, 1, DMA_TREQ_TIMER(3));
    qtest_clock_step(global_qtest, 2000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 1);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0);

    writel(DMA_SEC_MISC + ALIAS_CLR, BIT(9)); /* TIMER3 is NSP. */
    g_assert_cmphex(bus_read(DMA_PACING(3), 0, CPU0, true), ==, ratio);
    bus_write(DMA_PACING(3), 0, CPU0, ratio, true);
    bus_read(DMA_PACING(3), ATTR_USER, CPU0, false);
    writel(DMA_MPU_BAR0, SRAM9_WORD);
    writel(DMA_MPU_LAR0, SRAM9_WORD | 7); /* SP-only source */
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmphex(readl(DMA_CTRL) & DMA_READ_ERROR, ==, DMA_READ_ERROR);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0);

    writel(DMA_MPU_LAR0, 0);
    writel(DMA_CTRL, DMA_READ_ERROR);
    dma_start(SRAM9_WORD, SRAM8_WORD, 1, DMA_TREQ_TIMER(3));
    qtest_clock_step(global_qtest, 1000);
    g_assert_cmpuint(readl(DMA_COUNT), ==, 0);
    g_assert_cmphex(readl(SRAM8_WORD), ==, 0x5a);
}

static void test_pio_tx_gpio(void)
{
    QTestState *s = qtest_init("-machine raspi-pico2");

    qtest_irq_intercept_out_named(s, "/machine/soc/regs[2]", "pio-out");
    qtest_writel(s, RESETS_RESET + ALIAS_CLR, PIO0_RESET);
    qtest_writel(s, LED_CTRL, 6); /* PIO0 */
    qtest_writel(s, PIO_SM0_PINCTRL, (1u << 26) | (LED_PIN << 5));
    qtest_writel(s, PIO_SM0_INSTR, 0xe081); /* SET PINDIRS, 1 while disabled */
    qtest_writel(s, PIO_SM0_PINCTRL, (1u << 20) | LED_PIN);
    g_assert_cmphex(qtest_readl(s, LED_STATUS), ==, GPIO_OETOPAD);

    qtest_writel(s, PIO_IMEM(7), 0x80a0); /* PULL BLOCK */
    qtest_writel(s, PIO_IMEM(8), 0x6001); /* OUT PINS, 1 */
    qtest_writel(s, PIO_IMEM(9), 0x0007); /* JMP 7 */
    qtest_writel(s, PIO_SM0_INSTR, 0x0007); /* JMP 7 while disabled */
    qtest_writel(s, PIO_CTRL + ALIAS_SET, 1);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, PIO_SM0_ADDR), ==, 7);
    g_assert_cmphex(qtest_readl(s, PIO_FSTAT) & PIO_TX0_EMPTY, ==,
                    PIO_TX0_EMPTY);
    g_assert_cmphex(qtest_readl(s, PIO_FDEBUG), ==, PIO_TX0_STALL);
    g_assert_false(qtest_get_irq(s, LED_PIN));

    qtest_writel(s, PIO_FDEBUG, PIO_TX0_STALL);
    qtest_writel(s, PIO_TXF0, 3);
    g_assert_cmphex(qtest_readl(s, PIO_FLEVEL), ==, 1);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, PIO_FLEVEL), ==, 0);
    g_assert_cmphex(qtest_readl(s, PIO_SM0_ADDR), ==, 7);
    g_assert_cmphex(qtest_readl(s, PIO_FDEBUG), ==, PIO_TX0_STALL);
    g_assert_cmphex(qtest_readl(s, LED_STATUS), ==,
                    GPIO_OETOPAD | GPIO_OUTTOPAD);
    g_assert_true(qtest_get_irq(s, LED_PIN));

    qtest_writel(s, LED_PAD + ALIAS_SET, BIT(7)); /* pad output disable */
    g_assert_false(qtest_get_irq(s, LED_PIN));
    qtest_writel(s, LED_PAD + ALIAS_CLR, BIT(7));
    g_assert_true(qtest_get_irq(s, LED_PIN));
    qtest_writel(s, LED_CTRL, 5); /* disconnect PIO0 from the pad */
    g_assert_false(qtest_get_irq(s, LED_PIN));
    qtest_writel(s, LED_CTRL, 6);
    g_assert_true(qtest_get_irq(s, LED_PIN));

    qtest_writel(s, PIO_CTRL + ALIAS_CLR, 1);
    qtest_writel(s, PIO_TXF0, 2);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, PIO_FLEVEL), ==, 1);
    g_assert_true(qtest_get_irq(s, LED_PIN));
    qtest_writel(s, PIO_CTRL + ALIAS_SET, 1);
    qtest_clock_step(s, 100);
    g_assert_cmphex(qtest_readl(s, PIO_FLEVEL), ==, 0);
    g_assert_cmphex(qtest_readl(s, LED_STATUS), ==, GPIO_OETOPAD);
    g_assert_false(qtest_get_irq(s, LED_PIN));

    qtest_writel(s, PIO_CTRL + ALIAS_CLR, 1);
    qtest_writel(s, PIO_TXF0, 1);
    qtest_writel(s, PIO_SM0_SHIFTCTRL + ALIAS_XOR, BIT(31)); /* FJOIN_RX */
    g_assert_cmphex(qtest_readl(s, PIO_FLEVEL), ==, 0);
    qtest_writel(s, RESETS_RESET + ALIAS_SET, PIO0_RESET);
    qtest_writel(s, RESETS_RESET + ALIAS_CLR, PIO0_RESET);
    g_assert_cmphex(qtest_readl(s, LED_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(s, PIO_FDEBUG), ==, 0);
    g_assert_cmphex(qtest_readl(s, PIO_SM0_ADDR), ==, 0);
    qtest_quit(s);
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
    qtest_add_func("/rp2350/dma/pio-tx", test_dma_pio_tx);
    qtest_add_func("/rp2350/dma/pio-tx-access", test_dma_pio_tx_access);
    qtest_add_func("/rp2350/dma/pacing", test_dma_pacing);
    qtest_add_func("/rp2350/dma/pacing-stop", test_dma_pacing_stop);
    qtest_add_func("/rp2350/dma/pacing-chain", test_dma_pacing_chain);
    qtest_add_func("/rp2350/dma/pacing-access", test_dma_pacing_access);
    qtest_add_func("/rp2350/qmi/direct", test_qmi_direct);
    qtest_add_func("/rp2350/trng/entropy", test_trng);
    qtest_add_func("/rp2350/sha256/abc", test_sha256);
    qtest_add_func("/rp2350/otp/locks-reset", test_otp);
    qtest_add_func("/rp2350/timer/two-instances", test_timers);
    qtest_add_func("/rp2350/pio/tx-gpio", test_pio_tx_gpio);

    qtest_start("-machine raspi-pico2");
    ret = g_test_run();
    qtest_end();

    return ret;
}
