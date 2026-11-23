/*
 * RP2350 bus isolation tests using the Cortex-M33 and RP2350 registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include "test.h"
#include "hardware/regs/accessctrl.h"
#include "hardware/regs/dma.h"
#include "hardware/regs/resets.h"
#include "hardware/resets.h"

#define ACCESSCTRL(offset) REG(ACCESSCTRL_BASE, offset)
#define ACCESSCTRL_KEY 0xacce0000u
#define DMA(offset) REG(DMA_BASE, offset)

static void require(bool condition, const char *check)
{
    if (!condition) {
        printf("FAIL: %s\n", check);
        wait_for_stop();
    }
}

#if defined(TEST_FILTER) || defined(TEST_DMA)
static bool set_permission(unsigned offset, uint32_t permission)
{
    ACCESSCTRL(offset) = ACCESSCTRL_KEY | permission;
    return ACCESSCTRL(offset) == permission;
}
#endif

#if defined(TEST_FILTER)
#define FILTER_WORD (RESETS_BASE + RESETS_RESET_DONE_OFFSET)
#define FILTER_ALIAS (FILTER_WORD + REG_ALIAS_SET_BITS)
/* The BusFault handler observes this flag at the final access. */
static volatile bool expect_filter_fault;

static unsigned __scratch_x("ns_code") ns_read_filter_port(void)
{
    return REG(FILTER_ALIAS, 0);
}

static void run_case(void)
{
    const uint32_t secure = ACCESSCTRL_RESETS_CORE0_BITS |
                            ACCESSCTRL_RESETS_SP_BITS;
    uint32_t reset_done = REG(FILTER_WORD, 0);
    ns_fn *read_port = (ns_fn *)cmse_nsfptr_create(&ns_read_filter_port);

    require(reset_done & RESETS_RESET_DONE_UART0_BITS,
            "Secure RESETS.RESET_DONE UART0 bit clear");
    require(REG(FILTER_ALIAS, 0) == reset_done, "Secure RESETS alias mismatch");
    puts("PASS: Secure RESETS control");

    require(set_permission(ACCESSCTRL_RESETS_OFFSET,
                           secure | ACCESSCTRL_RESETS_NSP_BITS),
            "ACCESSCTRL RESETS allow configuration did not take effect");
    sau_region(1, RESETS_BASE, RESETS_BASE + 0x3fff);
    sau_enable(true);
    require(read_port() == reset_done, "Non-secure RESETS allow readback");
    sau_enable(false);
    puts("PASS: Non-secure RESETS access granted");

    require(set_permission(ACCESSCTRL_RESETS_OFFSET, secure),
            "ACCESSCTRL RESETS deny configuration did not take effect");
    require(REG(FILTER_ALIAS, 0) == reset_done, "Secure RESETS lost access");
    expect_filter_fault = true;
    sau_enable(true);
    read_port();
    require(false, "Non-secure RESETS access returned after revocation");
}
#elif defined(TEST_BOOTRAM_READ) || defined(TEST_BOOTRAM_WRITE)
#define BOOTRAM_WORD (BOOTRAM_BASE + 0x3fc)
#define BOOTRAM_ALIAS (BOOTRAM_WORD + REG_ALIAS_SET_BITS)
#define BOOTRAM_FIXTURE 0x317f5402u
#define BOOTRAM_SET_BIT 1u
#if defined(TEST_BOOTRAM_WRITE)
#define BOOTRAM_DENIED BOOTRAM_ALIAS
#define BOOTRAM_OPERATION "SET alias write"
#else
#define BOOTRAM_DENIED BOOTRAM_WORD
#define BOOTRAM_OPERATION "read"
#endif
/* The BusFault handler observes this flag at the final access. */
static volatile bool expect_bootram_fault;

static unsigned __scratch_x("ns_code") ns_access_bootram(void)
{
#if defined(TEST_BOOTRAM_WRITE)
    REG(BOOTRAM_DENIED, 0) = BOOTRAM_SET_BIT;
    return 0;
#else
    return REG(BOOTRAM_DENIED, 0);
#endif
}

static void run_case(void)
{
    ns_fn *access = (ns_fn *)cmse_nsfptr_create(&ns_access_bootram);

    REG(BOOTRAM_WORD, 0) = BOOTRAM_FIXTURE;
    require(REG(BOOTRAM_WORD, 0) == BOOTRAM_FIXTURE,
            "Secure Boot RAM readback");
    REG(BOOTRAM_ALIAS, 0) = BOOTRAM_SET_BIT;
    require(REG(BOOTRAM_WORD, 0) == (BOOTRAM_FIXTURE | BOOTRAM_SET_BIT),
            "Secure Boot RAM SET alias readback");
    REG(BOOTRAM_WORD, 0) = BOOTRAM_FIXTURE;
    puts("PASS: Secure Boot RAM control");

    sau_region(1, BOOTRAM_BASE, BOOTRAM_BASE + 0x3fff);
    expect_bootram_fault = true;
    sau_enable(true);
    access();
    sau_enable(false);
    require(REG(BOOTRAM_WORD, 0) == BOOTRAM_FIXTURE,
            "Non-secure Boot RAM write changed the fixture");
    require(false, "Non-secure Boot RAM " BOOTRAM_OPERATION " returned");
}
#elif defined(TEST_DMA)
#define DMA_ERRORS (DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS | \
                    DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | \
                    DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS)
#define DMA_READ_DENIED (DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS | \
                         DMA_CH0_CTRL_TRIG_READ_ERROR_BITS)
#define COPY_WORD 0x581ae976u
#define DEST_WORD 0x3d786412u
#define SECURE_WORD 0x4ef07132u

static io_rw_32 __scratch_x("ns_data") ns_source;
static io_rw_32 __scratch_x("ns_data") ns_destination;
static io_rw_32 __scratch_y("secure_data") secure_source;

typedef uint32_t __attribute__((cmse_nonsecure_call))
    ns_dma_fn(const io_rw_32 *source);

static uint32_t __scratch_x("ns_code") ns_dma_copy_word(const io_rw_32 *source)
{
    DMA(DMA_CH0_READ_ADDR_OFFSET) = (uintptr_t)source;
    DMA(DMA_CH0_WRITE_ADDR_OFFSET) = (uintptr_t)&ns_destination;
    DMA(DMA_CH0_TRANS_COUNT_OFFSET) = 1;
    DMA(DMA_CH0_CTRL_TRIG_OFFSET) = DMA_CH0_CTRL_TRIG_EN_BITS |
        (DMA_CH0_CTRL_TRIG_DATA_SIZE_VALUE_SIZE_WORD <<
         DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB) |
        (DMA_CH0_CTRL_TRIG_TREQ_SEL_VALUE_PERMANENT <<
         DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB);

    for (unsigned i = 0; i < 100000; i++) {
        uint32_t status = DMA(DMA_CH0_CTRL_TRIG_OFFSET);

        if (!(status & DMA_CH0_CTRL_TRIG_BUSY_BITS)) {
            return status & DMA_ERRORS;
        }
    }
    return UINT32_MAX;
}

static void run_case(void)
{
    const uint32_t sram9 = ACCESSCTRL_SRAM9_RESET &
        ~(ACCESSCTRL_SRAM9_NSP_BITS | ACCESSCTRL_SRAM9_NSU_BITS);
    ns_dma_fn *transfer = (ns_dma_fn *)cmse_nsfptr_create(&ns_dma_copy_word);

    require(set_permission(ACCESSCTRL_DMA_OFFSET,
                           ACCESSCTRL_DMA_RESET | ACCESSCTRL_DMA_NSP_BITS),
            "ACCESSCTRL DMA configuration did not take effect");
    require(set_permission(ACCESSCTRL_SRAM8_OFFSET, ACCESSCTRL_SRAM8_RESET),
            "ACCESSCTRL SRAM8 configuration did not take effect");
    require(set_permission(ACCESSCTRL_SRAM9_OFFSET, ACCESSCTRL_SRAM9_RESET),
            "ACCESSCTRL SRAM9 allow configuration did not take effect");
    unreset_block_wait(RESETS_RESET_DMA_BITS);

    /* Keep the MPU at its permissive reset setting to exercise ACCESSCTRL. */
    require(DMA(DMA_MPU_CTRL_OFFSET) == DMA_MPU_CTRL_RESET,
            "DMA MPU CTRL did not have its reset value");
    require(DMA(DMA_SECCFG_CH0_OFFSET) == DMA_SECCFG_CH0_RESET,
            "DMA channel 0 did not have its reset security level");
    DMA(DMA_SECCFG_CH0_OFFSET) = DMA_SECCFG_CH0_P_BITS;
    require(DMA(DMA_SECCFG_CH0_OFFSET) == DMA_SECCFG_CH0_P_BITS,
            "DMA channel 0 did not become Non-secure privileged");

    ns_source = COPY_WORD;
    ns_destination = DEST_WORD;
    secure_source = SECURE_WORD;
    sau_region(1, DMA_BASE, DMA_BASE + 0x3fff);
    sau_enable(true);
    require(!transfer(&ns_source) && ns_destination == COPY_WORD,
            "Non-secure DMA SRAM8 control copy");
    puts("PASS: Non-secure DMA SRAM8 control copy");
    require(!transfer(&secure_source) && ns_destination == SECURE_WORD,
            "Non-secure DMA SRAM9 copy with access granted");
    puts("PASS: Non-secure DMA SRAM9 access granted");

    require(set_permission(ACCESSCTRL_SRAM9_OFFSET, sram9),
            "ACCESSCTRL SRAM9 deny configuration did not take effect");
    ns_destination = DEST_WORD;
    __dsb();
    require(transfer(&secure_source) == DMA_READ_DENIED &&
            ns_destination == DEST_WORD, "Non-secure DMA SRAM9 read denial");
    require(secure_source == SECURE_WORD, "Secure SRAM fixture changed");
    puts("PASS: Non-secure DMA Secure SRAM read denied");
}
#endif

void isr_busfault(void)
{
#if defined(TEST_FILTER)
    require(expect_filter_fault && precise_busfault_at(FILTER_ALIAS),
            "unexpected filter BusFault");
    puts("PASS: Non-secure RESETS filter BusFault");
#elif defined(TEST_BOOTRAM_READ) || defined(TEST_BOOTRAM_WRITE)
    require(expect_bootram_fault && precise_busfault_at(BOOTRAM_DENIED),
            "unexpected Boot RAM BusFault");
    sau_enable(false);
    require(REG(BOOTRAM_WORD, 0) == BOOTRAM_FIXTURE,
            "Boot RAM fixture changed");
    puts("PASS: Non-secure Boot RAM " BOOTRAM_OPERATION " BusFault");
#elif defined(TEST_DMA)
    require(false, "unexpected CPU BusFault during DMA test");
#endif
    wait_for_stop();
}

int main(void)
{
    stdio_uart_init();
    prepare_ns();
    run_case();
    wait_for_stop();
}
