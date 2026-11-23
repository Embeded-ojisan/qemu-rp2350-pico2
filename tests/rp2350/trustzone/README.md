<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Pico 2 TrustZone smoke test

The test runs a Non-secure routine from scratch X SRAM using the Cortex-M33
CMSE call mechanism. It checks the SIO spinlock banks and GPIO views through
MMIO and coprocessor 0, plus per-pin IO and pad registers. The Secure
BusFault handler checks that a Non-secure read of the SIO mirror reported
`PRECISERR`, `BFARVALID`, and the address `0xd0020100`.

Build with Arm GNU Toolchain, CMake, Ninja, and pico-sdk 2.3.1. Run these
commands from the QEMU checkout.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_EXAMPLES_PATH=/path/to/pico-examples

mkdir -p build
(cd build && ../configure --target-list=arm-softmmu --disable-docs --disable-werror)
ninja -C build qemu-system-arm tests/qtest/rp2350-atomic-test
build/run meson test -C build --print-errorlogs qtest-arm/rp2350-atomic-test
tests/rp2350/smoke-test.sh build/qemu-system-arm

cmake -S tests/rp2350/trustzone -B build/rp2350-trustzone -G Ninja \
    -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DPICO_NO_PICOTOOL=1
ninja -C build/rp2350-trustzone
tests/rp2350/trustzone/smoke-test.sh
```

The pre-integration run passed all five qtest cases, both hello UART
boot paths (`-kernel` and `-drive if=mtd`), and the two blink transitions
on GPIO25. The existing smoke runner builds hello and blink with
pico-sdk 2.3.1. The TrustZone smoke runner captures
UART and the QEMU exception trace in its printed work directory. The
UART output was:

```text
Secure: preparing SIO and GPIO
PASS: Secure to Non-secure SIO/GPIO isolation
PASS: Non-secure SIO mirror BusFault
```

Running the same ELF on the approved milestone (a′) build failed at the
Non-secure spinlock check.

## Integration

ACCESSCTRL should use `sio.gpio_nsmask[0/1]` for GPIO_NSMASK reads and call
`rp2350_sio_gpio_set_nsmask()` for permitted writes. Apply the relevant
reset or configuration-reset semantics through the same interface and
keep one copy of the mask state. IO_BANK0 and PADS_BANK0 use those masks
per pin. The bus filter must preserve `MemTxAttrs` through the normal
and atomic aliases. SIO handles its bank selection and mirror denial
internally. DMA exclusion from SIO needs the DMA requester integration.

Before ACCESSCTRL integration the smoke test exercises the all-Secure
GPIO reset assignment. The demo explicitly configures the Cortex-M33 SAU
for the peripheral windows because this board model lacks the RP2350
IDAU peripheral exemptions. Build the assignment variant after connecting
the real ACCESSCTRL and filter models:

```sh
cmake -S tests/rp2350/trustzone -B build/rp2350-trustzone-integrated -G Ninja \
    -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DPICO_NO_PICOTOOL=1 \
    -DRP2350_TEST_ACCESSCTRL=ON
ninja -C build/rp2350-trustzone-integrated
tests/rp2350/trustzone/smoke-test.sh build/qemu-system-arm \
    build/rp2350-trustzone-integrated/rp2350_trustzone.elf
```

This variant grants NS access to GPIO1 and GPIO33 through the real masks and
to the IO and pad peripherals through ACCESSCTRL. The standalone build of
this variant fails on mask readback until ACCESSCTRL is wired. Integration
also needs the filter permission matrix, an NS BusFault on a filter-denied
peripheral, the Boot RAM Secure-only exception, and denial of an NS DMA
transfer from Secure SRAM. The SIO test exercises the currently modeled GPIO
and spinlock state. GPIO function routing and IRQ delivery, QSPI pad control,
and PIO observation require models for those paths. Pad access uses logical
GPIO numbering for silicon with the RP2350-E3 erratum fixed.

References are the [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
§§2.1.2, 3.1.1, 3.6.1, 10.2.2, 10.6.1, and the
[pico-sdk 2.3.1 register headers](https://github.com/raspberrypi/pico-sdk/tree/2.3.1/src/rp2350/hardware_regs/include/hardware/regs).
