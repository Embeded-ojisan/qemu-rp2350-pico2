#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: smoke-test.sh [QEMU] [FIRMWARE_ELF] [CASE]

set -eu

QEMU=${1:-build/qemu-system-arm}
ELF=${2:-build/rp2350-trustzone/rp2350_trustzone.elf}
case ${3:-gpio} in
    gpio)
        set -- 'Secure to Non-secure SIO/GPIO isolation' \
            'Non-secure SIO mirror BusFault'
        ;;
    filter)
        set -- 'Secure RESETS control' 'Non-secure RESETS access granted' \
            'Non-secure RESETS filter BusFault'
        ;;
    bootram_read)
        set -- 'Secure Boot RAM control' 'Non-secure Boot RAM read BusFault'
        ;;
    bootram_write)
        set -- 'Secure Boot RAM control' \
            'Non-secure Boot RAM SET alias write BusFault'
        ;;
    dma)
        set -- 'Non-secure DMA SRAM8 control copy' \
            'Non-secure DMA SRAM9 access granted' \
            'Non-secure DMA Secure SRAM read denied'
        ;;
    *)
        echo "Unknown test case: $3" >&2
        exit 2
        ;;
esac
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rp2350-trustzone.XXXXXX")
echo "work directory: $WORK"

status=0
timeout "${RUN_SECONDS:-3}" "$QEMU" -M raspi-pico2 -nographic \
    -kernel "$ELF" -d int -D "$WORK/interrupts.log" \
    </dev/null >"$WORK/uart.log" 2>&1 || status=$?
cat "$WORK/uart.log"

# The firmware waits after reporting the result.
test "$status" -eq 124
cr=$(printf '\r')
for expected do
    grep -Fxq -e "PASS: $expected" -e "PASS: $expected$cr" "$WORK/uart.log"
done
! grep -Fq 'FAIL:' "$WORK/uart.log"
