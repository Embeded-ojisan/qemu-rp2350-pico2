#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: smoke-test.sh [QEMU] [FIRMWARE_ELF]

set -eu

QEMU=${1:-build/qemu-system-arm}
ELF=${2:-build/rp2350-trustzone/rp2350_trustzone.elf}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rp2350-trustzone.XXXXXX")
echo "work directory: $WORK"

status=0
timeout "${RUN_SECONDS:-3}" "$QEMU" -M raspi-pico2 -nographic \
    -kernel "$ELF" -d int -D "$WORK/interrupts.log" \
    </dev/null >"$WORK/uart.log" 2>&1 || status=$?
cat "$WORK/uart.log"

# The firmware waits in its BusFault handler after printing its result.
test "$status" -eq 124
grep -Fq 'PASS: Secure to Non-secure SIO/GPIO isolation' "$WORK/uart.log"
grep -Fq 'PASS: Non-secure SIO mirror BusFault' "$WORK/uart.log"
! grep -Fq 'FAIL:' "$WORK/uart.log"
