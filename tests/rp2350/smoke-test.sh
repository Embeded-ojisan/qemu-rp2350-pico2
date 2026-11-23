#!/bin/sh
# Smoke test for the raspi-pico2 board with pico-examples firmware.
#
# Runs hello_world (serial) and blink and checks:
#   - the stub ROM enters the image (rp2350_stub_rom_boot trace event)
#   - hello_world prints "Hello, world!" on UART0, booted with -kernel
#     and again from a flash image given with -drive if=mtd
#   - blink drives GPIO25 high and low through SIO (rp2350_sio_gpio_out)
#
# Usage: tests/rp2350/smoke-test.sh [QEMU] [HELLO_ELF] [BLINK_ELF]
#   QEMU defaults to build/qemu-system-arm. Without the two ELFs the
#   script builds them from $PICO_EXAMPLES_PATH with $PICO_SDK_PATH
#   (PICO_BOARD=pico2), adding $PICO_EXTRA_CFLAGS to CFLAGS.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

QEMU=${1:-build/qemu-system-arm}
HELLO_ELF=${2:-}
BLINK_ELF=${3:-}
RUN_SECONDS=${RUN_SECONDS:-3}
FLASH_SIZE=4194304

WORK=$(mktemp -d "${TMPDIR:-/tmp}/rp2350-smoke.XXXXXX")
echo "work directory: $WORK"

if [ -z "$HELLO_ELF" ] || [ -z "$BLINK_ELF" ]; then
    : "${PICO_SDK_PATH:?set PICO_SDK_PATH or pass the ELF files}"
    : "${PICO_EXAMPLES_PATH:?set PICO_EXAMPLES_PATH or pass the ELF files}"
    CFLAGS="${PICO_EXTRA_CFLAGS:-}" cmake -S "$PICO_EXAMPLES_PATH" \
        -B "$WORK/fw" -G Ninja -DPICO_BOARD=pico2 \
        -DPICO_PLATFORM=rp2350-arm-s > "$WORK/cmake.log"
    ninja -C "$WORK/fw" hello_serial blink > "$WORK/ninja.log"
    HELLO_ELF=$WORK/fw/hello_world/serial/hello_serial.elf
    BLINK_ELF=$WORK/fw/blink/blink.elf
fi

# run NAME QEMU-ARGS...: UART0 goes to NAME.uart, trace events to NAME.log
run() {
    name=$1
    shift
    timeout "$RUN_SECONDS" "$QEMU" -M raspi-pico2 -nographic \
        -D "$WORK/$name.log" -trace 'rp2350_stub_rom_*' \
        -trace rp2350_sio_gpio_out "$@" \
        < /dev/null > "$WORK/$name.uart" 2>&1 || true
}

fail=0
check() {
    if grep -q "$2" "$WORK/$1"; then
        echo "PASS: $1: $2"
    else
        echo "FAIL: $1: $2"
        fail=1
    fi
}

run hello-kernel -kernel "$HELLO_ELF"
check hello-kernel.log 'rp2350_stub_rom_boot entering image'
check hello-kernel.uart 'Hello, world!'

# Flash image: erased (0xff) 4 MB with the binary at offset 0
arm-none-eabi-objcopy -O binary "$HELLO_ELF" "$WORK/hello.bin"
head -c "$FLASH_SIZE" /dev/zero | tr '\0' '\377' > "$WORK/flash.bin"
dd if="$WORK/hello.bin" of="$WORK/flash.bin" conv=notrunc 2> /dev/null
run hello-flash -drive "if=mtd,format=raw,file=$WORK/flash.bin"
check hello-flash.log 'rp2350_stub_rom_boot entering image'
check hello-flash.uart 'Hello, world!'

run blink -kernel "$BLINK_ELF"
check blink.log 'rp2350_sio_gpio_out GPIO25 out 1'
check blink.log 'rp2350_sio_gpio_out GPIO25 out 0'

if [ "$fail" -ne 0 ]; then
    echo "smoke test FAILED (logs in $WORK)"
    exit 1
fi
echo "smoke test passed (logs in $WORK)"
