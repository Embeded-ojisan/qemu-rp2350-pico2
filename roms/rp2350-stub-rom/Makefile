# RP2350 stub ROM for QEMU's raspi-pico2 board.
# SPDX-License-Identifier: GPL-2.0-or-later

CROSS ?= arm-none-eabi-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy

CFLAGS = -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib \
         -Wall -Wextra -Werror -fno-common
OUT ?= ../../pc-bios/rp2350-stub-rom.bin

all: $(OUT)

rp2350-stub-rom.elf: start.S stub.c stub.h rom_table.h rom_funcs.h link.ld
	$(CC) $(CFLAGS) -T link.ld -o $@ start.S stub.c

$(OUT): rp2350-stub-rom.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f rp2350-stub-rom.elf

.PHONY: all clean
