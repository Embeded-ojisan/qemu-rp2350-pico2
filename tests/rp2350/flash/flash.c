/*
 * Run the flash ROM APIs, then verify persistence in a second VM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#define XIP_OFFSET 0x3f0000u
#define TEST_ADDRESS (XIP_BASE + XIP_OFFSET)
/* Volatile accesses preserve the ordering of the SCB operations. */
#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define SCB_VTOR REG32(0xe000ed08u)
#define SCB_SHCSR REG32(0xe000ed24u)
#define SCB_CFSR REG32(0xe000ed28u)
#define SCB_BFAR REG32(0xe000ed38u)
#define EXPECTED_FAULT ((1u << 9) | (1u << 15))

static uint32_t vectors[16 + NUM_IRQS] __attribute__((aligned(512)));
static uint32_t page[FLASH_PAGE_SIZE / sizeof(uint32_t)];
/* Written asynchronously by the BusFault handler. */
static volatile uint32_t fault_status, fault_address;

void __not_in_flash_func(observe_fault)(uint32_t *frame)
{
    fault_status = SCB_CFSR;
    fault_address = SCB_BFAR;
    SCB_CFSR = fault_status;
    frame[6] += 2; /* Skip the single 16-bit LDR in probe_xip(). */
}

static void __attribute__((naked)) __not_in_flash_func(bus_fault)(void)
{
    __asm__ volatile("mrs r0, msp\n"
                     "b observe_fault");
}

static void __attribute__((naked)) __not_in_flash_func(probe_xip)(
    uint32_t address)
{
    __asm__ volatile("ldr r0, [r0]\n"
                     "bx lr");
}

static void __no_inline_not_in_flash_func(probe_direct_mode)(void)
{
    rom_flash_exit_xip_fn exit_xip =
        (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_enter_cmd_xip_fn enter_xip =
        (rom_flash_enter_cmd_xip_fn)
        rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP);

    exit_xip();
    probe_xip(TEST_ADDRESS);
    enter_xip();
}

static void __attribute__((noreturn)) pass(const char *message)
{
    puts(message);
    for (;;) {
        __wfi();
    }
}

int main(void)
{
    int (*execute_xip)(void) =
        (int (*)(void))(uintptr_t)(TEST_ADDRESS | 1u);
    uint32_t saved_vtor = SCB_VTOR;
    uint32_t irq_state;
    int result;

    stdio_init_all();
    result = execute_xip();
    if (result == 2) {
        pass("PASS flash contents survived QEMU process restart");
    }
    if (result != 1) {
        printf("FAIL initial XIP function returned %d\n", result);
        return 1;
    }

    for (unsigned i = 0; i < count_of(vectors); i++) {
        vectors[i] = REG32(saved_vtor + 4 * i);
    }
    vectors[5] = (uintptr_t)bus_fault;
    SCB_VTOR = (uintptr_t)vectors;
    SCB_SHCSR |= 1u << 17;
    __dsb();
    __isb();
    probe_direct_mode();
    SCB_VTOR = saved_vtor;
    if ((fault_status & EXPECTED_FAULT) != EXPECTED_FAULT ||
        fault_address != TEST_ADDRESS) {
        printf("FAIL direct-mode fault CFSR=%08lx BFAR=%08lx\n",
               (unsigned long)fault_status, (unsigned long)fault_address);
        return 1;
    }
    puts("PASS XIP direct-mode precise BusFault");

    if (execute_xip() != 1) {
        puts("FAIL original XIP code after leaving direct mode");
        return 1;
    }
    for (unsigned i = 0; i < count_of(page); i++) {
        page[i] = UINT32_MAX;
    }
    page[0] = 0x47702002u; /* movs r0, #2; bx lr */
    irq_state = save_and_disable_interrupts();
    flash_range_erase(XIP_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(XIP_OFFSET, (const uint8_t *)page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);

    result = execute_xip();
    if (result != 2) {
        printf("FAIL XIP function returned %d after programming\n", result);
        return 1;
    }
    pass("PASS XIP executed updated code after erase and program");
}
