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

The pre-integration run passed all six qtest cases, both hello UART
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

Before ACCESSCTRL integration the default smoke test exercises the
all-Secure GPIO reset assignment. The test firmware sets the Cortex-M33
SAU for the peripheral windows because this board model lacks the RP2350
IDAU peripheral exemptions. The Secure CPU controls access to RESETS
and Boot RAM before enabling the SAU regions for the Non-secure probes.
This lets the probes reach the target as Non-secure bus transactions so
a denied access can be distinguished from a processor attribution fault.

### Opt-in firmware

Build the GPIO assignment variant and the four integration cases with
the SDK and QEMU build from above.

```sh
fw=build/rp2350-trustzone-b-integration
qemu=build/qemu-system-arm
runner=tests/rp2350/trustzone/smoke-test.sh

cmake -S tests/rp2350/trustzone -B "$fw" -G Ninja \
    -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DPICO_NO_PICOTOOL=1 \
    -DRP2350_TEST_ACCESSCTRL=ON -DRP2350_TEST_INTEGRATION=ON
ninja -C "$fw"
"$runner" "$qemu" "$fw/rp2350_trustzone.elf" gpio
"$runner" "$qemu" "$fw/rp2350_filter.elf" filter
"$runner" "$qemu" "$fw/rp2350_bootram_read.elf" bootram_read
"$runner" "$qemu" "$fw/rp2350_bootram_write.elf" bootram_write
"$runner" "$qemu" "$fw/rp2350_dma.elf" dma
```

Each launch checks all of its UART pass markers and fails if any marker is
missing or the firmware prints a failure. Run these cases independently
before integration so one expected failure does not hide the others.

| Case | Allowed control | Denial check | Observed on the pre-integration model |
| --- | --- | --- | --- |
| `gpio` | NS can access GPIO1/33 through the real masks and IO/pad permissions | GPIO0/32 stay Secure and NS cannot access the SIO mirror | Mask write readback failed |
| `filter` | Secure and granted NSP reads of the same RESETS.RESET_DONE SET-alias word agree | Revoke NSP access, preserve SP access, then require a precise BusFault at `0x40022008` for the next NSP read | Secure control passed, ACCESSCTRL permission readback failed |
| `bootram_read` | Secure direct read/write and a SET-alias write reach the same word | NS read at `0x400e03fc` must take a precise BusFault and leave the fixture intact | Secure control passed, NS read returned |
| `bootram_write` | Same Secure control as the read case | NS SET-alias write at `0x400e23fc` must take a precise BusFault and leave the fixture intact | Secure control passed, NS write changed the fixture |
| `dma` | NSP DMA channel 0 copies one word from SRAM8 and then SRAM9 to SRAM8 while both banks allow NSP | Revoke SRAM9 NSP/NSU access, then require `AHB_ERROR` and `READ_ERROR`, no `WRITE_ERROR`, and an unchanged destination for the same SRAM9 read | ACCESSCTRL DMA permission readback failed before starting a transfer |

All five ELFs built with pico-sdk 2.3.1. The six-case RP2350 qtest,
the existing hello/blink smoke, and the default SIO/GPIO smoke passed.
Each opt-in launch exited with status 1 on the pre-integration model.
The Boot RAM qtest checks the storage and alias control through ordinary
qtest MMIO. The opt-in firmware checks NS read and write denial using
real CPU transactions. The datasheet specifies a Secure-only endpoint.
The tests expect BusFault for that endpoint rejection under ADR 0002.
Use these Boot RAM fixtures with the board model because boot firmware
can own the RAM contents on silicon.

The DMA test uses synthetic words, the same NSP channel for all three
transfers, and the DMA MPU's permissive reset configuration. This checks
the downstream SRAM9 permission independently of DMA MPU regions.
`arm-none-eabi-nm -n "$fw/rp2350_dma.elf"` placed the NS source and
destination in SRAM8 and the Secure fixture in SRAM9. The successful
copies, DMA status bits and Secure SRAM denial await the real models.

To observe the RESETS fault in GDB after integration, boot
`rp2350_filter.elf` with `-S -gdb tcp:127.0.0.1:1234`. Connect an Arm
GDB and stop on the firmware's fault handler.

```gdb
target remote 127.0.0.1:1234
hbreak isr_busfault
continue
p/x *(unsigned int *)0xe000ed28
p/x *(unsigned int *)0xe000ed38
```

CFSR should have `PRECISERR` and `BFARVALID` set (mask `0x8200`).
BFAR should read `0x40022008`. The guest CPU performs the faulting access.
ADR 0002 specifies a debugger bypass for QEMU filter accesses, so a
debugger MMIO read of RESETS would test a different path.

### Permission matrix for the integrated qtest

Use this table independently for each selected manager and each
filtered port. The permission bits are listed as a low nibble of that
port's ACCESSCTRL register, before adding the manager bit.

| Port permission bits | SP | SU | NSP | NSU |
| --- | --- | --- | --- | --- |
| none (`0x0`) | deny | deny | deny | deny |
| SP (`0x8`) | allow | deny | deny | deny |
| SU (`0x4`) | deny | deny | deny | deny |
| SP + SU (`0xc`) | allow | allow | deny | deny |
| NSP (`0x2`) | deny | deny | allow | deny |
| NSU (`0x1`) | deny | deny | deny | deny |
| NSP + NSU (`0x3`) | deny | deny | allow | allow |
| all (`0xf`) | allow | allow | allow | allow |

For each row, set one manager bit at a time (`CORE0=0x10`,
`CORE1=0x20`, `DMA=0x40`, hardware `DBG=0x80`) and exercise each
manager. An unset manager bit always denies an ordinary bus transaction,
even when all four state bits are set. Also check the no-manager row
against all four states. If the integrated model represents a hardware
debug manager, apply the matrix to its ordinary transactions. In QEMU
check `attrs.debug` separately with a deny-all port to verify the
GDB bypass required by the ADR.

Run the matrix on APB and SRAM ports such as RESETS and SRAM9. On APB
repeat through the canonical and SET/CLR/XOR windows. Use an observable
writable word and read it back from a permitted context after each write.
Check that allowed reads return the seeded value, allowed writes change
only the selected port, and denied reads and writes return a failed
transaction with the word unchanged. Restore an allowed configuration
for the readback when a case denies every ordinary manager. Configure
the other port to allow the same transaction and verify it remains
accessible. Preserve and restore the register values around each case.

Wire these rows into the owner's filter test driver where it can send
`MemTxAttrs.secure`, `user`, and the manager's `requester_id` with the
transaction and assert its `MemTxResult`. Ordinary qtest MMIO commands
use `MEMTXATTRS_UNSPECIFIED` and do not expose the result, so they can
seed and check registers but cannot drive this matrix. Use the integrated
owner mapping for core 1 and DMA IDs. The independent firmware covers
real core 0 SP/NSP and NSP DMA paths once the models are connected.

GPIO function routing and IRQ delivery, QSPI pad control, and PIO
observation require models for those paths. Pad access uses logical GPIO
numbering for silicon with the RP2350-E3 erratum fixed. The tests use
the rules in RP-008373 §§2.1.2, 3.1.1, 3.6.1, 4.3, 10.2.2, 10.6 and
10.7, including channel assignment and error reporting in §§12.6.6–7.
[RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf),
[pico-sdk 2.3.1 register headers](https://github.com/raspberrypi/pico-sdk/tree/2.3.1/src/rp2350/hardware_regs/include/hardware/regs),
[ADR 0002](https://github.com/Embeded-ojisan/rpi_pico2_qemu_inner/blob/main/docs/adr/0002-own-bus-security-filter.md).
