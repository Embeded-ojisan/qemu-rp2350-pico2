<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# RP2350 PIO0 smoke test

The Arm `raspi-pico2` milestone is met when Raspberry Pi's `hello_pio`
prints `Using gpio 25` on UART0 and PIO0 drives GPIO25 through at least
two high/low cycles. The RP2350 qtest also checks that PULL waits for a
TX word, that OUT drives the word's low bit through IO_BANK0, and that
disable, re-enable, FIFO flush and peripheral reset behave as expected.

Use pico-sdk 2.3.1 (`079c6f39023649b154152db30f1d781e884879bc`) and
pico-examples `0d62f75bafc2c8120d3276c3343d1a9195e909e9`. The example
allocator tries PIO2 before PIO1 and PIO0. The smoke build reserves the
PIO1 and PIO2 state machines with the SDK's software claim API before
the sample runs. It builds the example's original C and PIO sources.
[Raspberry Pi hello_pio source](https://github.com/raspberrypi/pico-examples/tree/0d62f75bafc2c8120d3276c3343d1a9195e909e9/pio/hello_pio)

From the QEMU checkout, with the two Raspberry Pi repositories and the
Cortex-M33 GNU Arm toolchain installed:

```sh
mkdir -p build
(cd build && ../configure --target-list=arm-softmmu --disable-docs)
ninja -C build qemu-system-arm tests/qtest/rp2350-atomic-test
(cd build && QTEST_QEMU_BINARY=./qemu-system-arm ./tests/qtest/rp2350-atomic-test)

cmake -S tests/rp2350/pio -B build/hello-pio -G Ninja \
    -DPICO_SDK_PATH=/absolute/path/to/pico-sdk \
    -DPICO_EXAMPLES_PATH=/absolute/path/to/pico-examples \
    -DPICO_BOARD=pico2 -DPICO_NO_PICOTOOL=1
cmake --build build/hello-pio --target hello_pio

timeout 4s build/qemu-system-arm -M raspi-pico2 -display none -monitor none \
    -serial file:build/hello-pio.uart.log \
    -kernel build/hello-pio/hello_pio/hello_pio.elf \
    -trace rp2350_stub_rom_boot -trace rp2350_pio_gpio_pad \
    -trace rp2350_atomic_write -D build/hello-pio.trace.log \
    || test "$?" -eq 124
grep -F 'Using gpio 25' build/hello-pio.uart.log
grep -E 'rp2350_pio_gpio_pad GPIO25 out [01] oe 1$' build/hello-pio.trace.log
```

The trace starts with GPIO25 driven low when the sample sets PINDIRS.
Expect at least two alternating `out 1 oe 1` and `out 0 oe 1` pairs
after that. The `rp2350-pio` writes at offset `0x10` are to SM0's TX
FIFO. The pad trace records the signal after function selection, output
enable and pad gating.

PIO0 models shared instruction memory, per-SM TX FIFOs and a subset of
the instruction set. This includes unconditional JMP, explicit PULL,
OUT to PINS/PINDIRS/NULL, SET to PINS/PINDIRS/X and NOP. The RP2350
datasheet chapter 11 and SDK register definitions describe the hardware.
[RP2350 datasheet](https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-4-rp2350-datasheet.pdf),
[SDK register definitions](https://github.com/raspberrypi/pico-sdk/blob/2.3.1/src/rp2350/hardware_regs/include/hardware/regs/pio.h),
[SDK hardware_pio](https://github.com/raspberrypi/pico-sdk/tree/2.3.1/src/rp2_common/hardware_pio)

RX data paths, PIO IRQs, side-set, autopull, conditional jumps,
GPIO inputs and PIO1/PIO2 execution are outside this model.
The timers approximate the clock divider in nanoseconds. Simultaneous
writes by multiple state machines to the same pin and exact clock
phase are outside the smoke test's timing model.

The DMA qtest exercises PIO0 TX0–3 with normal and joined FIFOs.
The DMA rechecks FIFO space before each beat and resumes after a PULL
makes room. See the [DMA model limits](../trustzone/README.md#model-limits)
for request visibility and timing.
