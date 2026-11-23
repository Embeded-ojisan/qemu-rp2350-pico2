#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

qemu=${1:-build/qemu-system-arm}
firmware=${2:-build/rp2350-flash/rp2350_flash.elf}
work=$(mktemp -d "${TMPDIR:-/tmp}/rp2350-flash.XXXXXX")
echo "work directory: $work"

python3 -c 'from pathlib import Path; import sys
image = bytearray(b"\xff" * 0x400000)
image[0x3f0000:0x3f0004] = bytes.fromhex("01207047")
Path(sys.argv[1]).write_bytes(image)' "$work/flash.bin"

run()
{
    timeout 3 "$qemu" -M raspi-pico2 -display none -monitor none \
        -serial stdio -kernel "$firmware" \
        -drive "if=mtd,format=raw,file=$work/flash.bin" \
        -trace 'rp2350_stub_rom_*' -d int -D "$work/$1.trace" \
        < /dev/null > "$work/$1.uart" 2>&1 || test "$?" -eq 124
    cat "$work/$1.uart"
    if grep -q 'FAIL' "$work/$1.uart"; then
        return 1
    fi
}

run program
grep -Fq 'PASS XIP direct-mode precise BusFault' "$work/program.uart"
grep -Fq 'PASS XIP executed updated code after erase and program' \
    "$work/program.uart"
run reopen
grep -Fq 'PASS flash contents survived QEMU process restart' "$work/reopen.uart"
