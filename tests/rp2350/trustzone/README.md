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

The integrated run passed all ten qtest cases, both hello UART
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

ACCESSCTRL and SIO share GPIO_NSMASK0/1. IO_BANK0, PADS_BANK0 and the
GPIO coprocessor use that assignment. The default smoke test opens
the IO and pad ports while leaving the GPIO assignment in its Secure
reset state. The opt-in GPIO variant assigns GPIO1 and GPIO33 to NS.

The Cortex-M33 and DMA share an identity-translating IOMMU region as
their bus filter. Normal RAM and ROM accesses can still use QEMU's
direct RAM path, and ACL changes invalidate the CPU TLB. When a
striped SRAM page contains banks with different permissions, each word
is checked as MMIO. The filter routes denied transactions to a region
that returns `MEMTX_ACCESS_ERROR`. Allowed accesses keep their
`MemTxAttrs` through the APB and AHB register aliases. ACCESSCTRL has
its own write checks. Boot RAM has a fixed Secure-only rule and SIO
handles its bank selection and mirror denial internally. DMA cannot
reach SIO. RESETS also masks individual peripheral controls according
to their NS availability.

The test firmware sets the Cortex-M33 SAU for the peripheral windows
because this board model lacks the RP2350 IDAU peripheral exemptions.
This lets the NS probes reach the bus so a denied access can be
distinguished from a processor attribution fault.

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
missing or the firmware prints a failure.

| Case | Allowed control | Denial check | Observed on the integrated model |
| --- | --- | --- | --- |
| `gpio` | NS can access GPIO1/33 through the real masks and IO/pad permissions | GPIO0/32 stay Secure and NS cannot access the SIO mirror | Passed |
| `filter` | Secure sees full RESETS.RESET_DONE and granted NSP sees the UART0 and SYSINFO bits through the same SET alias | Revoke RESETS NSP access, preserve SP access, then require a precise BusFault at `0x40022008` for the next NSP read | Passed |
| `bootram_read` | Secure direct read/write and a SET-alias write reach the same word | NS read at `0x400e03fc` takes a precise BusFault and leaves the fixture intact | Passed |
| `bootram_write` | Same Secure control as the read case | NS SET-alias write at `0x400e23fc` takes a precise BusFault and leaves the fixture intact | Passed |
| `dma` | NSP DMA channel 0 copies one word from SRAM8 and then SRAM9 to SRAM8 while both banks allow NSP | Revoke SRAM9 NSP/NSU access and check `AHB_ERROR`, `READ_ERROR`, no `WRITE_ERROR`, and an unchanged destination for the same SRAM9 read | Passed |

All five opt-in ELFs built with pico-sdk 2.3.1 and returned success
from the smoke runner. The summary above includes the existing tests.
The Boot RAM firmware verifies the fixed endpoint rejection through
real CPU transactions, in addition to the qtest checks.
Use these Boot RAM fixtures with the board model because boot firmware
can own the RAM contents on silicon.

The DMA test uses synthetic words, the same NSP channel for all three
transfers, and the DMA MPU's permissive reset configuration. This checks
the downstream SRAM9 permission independently of DMA MPU regions.
`arm-none-eabi-nm -n "$fw/rp2350_dma.elf"` placed the NS source and
destination in SRAM8 and the Secure fixture in SRAM9. A separate DMA
qtest covers a two-word transfer, channel locking and reset, masked
MPU register access, region priority, read and write errors, completion
and error IRQ status and line assertion, and recovery after clearing
the error.

To observe the RESETS fault in GDB, boot
`rp2350_filter.elf` with `-S -gdb tcp:127.0.0.1:1234`. Connect an Arm
GDB and stop on the firmware's fault handler.

```gdb
target remote 127.0.0.1:1234
hbreak isr_busfault
continue
p/x *(unsigned int *)0xe000ed28
p/x *(unsigned int *)0xe000ed38
```

GDB stopped at `isr_busfault` with CFSR `0x8200` (`PRECISERR` and
`BFARVALID`) and BFAR `0x40022008`. The guest CPU performs the
faulting access. The QEMU debugger bypasses the bus ACL under ADR 0002,
which differs from a hardware debugger's `DBG` permission.

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

The qtest drives all 16 permission nibbles, all four states, and
the four enable combinations of Core 0 and DMA on both UART0 and
SRAM9. Each read/write pair checks the transaction result and whether
the write reached the target. Additional cases check all three UART0
atomic aliases, mixed permissions on adjacent SRAM stripes, the SIO
and Boot RAM exceptions, and RESETS field visibility.

The qtest-only `memtxl` command carries `MemTxAttrs.secure`, `user`,
`debug` and `requester_id`, and reports the actual `MemTxResult`.
The model uses QEMU's default requester ID 0 for Core 0 and ID 2 for
DMA. `attrs.debug` supplies the bypass used to seed and check target
state even on deny-all ports. The opt-in firmware also exercises
actual CPU and DMA transactions through the same filter.

### Model limits

DMA models permanent-request transfers and raises the four modeled
IRQ lines from channel status. Peripheral and timer DREQ pacing,
sniffing, cycle arbitration, and debug counters are outside this
milestone. The model runs transfers as virtual timer events. Live
migration, physical silicon, and second-core behavior were untested.

GPIO function routing and IRQ delivery, QSPI pad control, and PIO
observation require models for those paths. Pad access uses logical GPIO
numbering for silicon with the RP2350-E3 erratum fixed. The tests use
the rules in RP-008373 §§2.1.2, 3.1.1, 3.6.1, 4.3, 10.2.2, 10.6 and
10.7, including channel assignment and error reporting in §§12.6.6–7.
[RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf),
[pico-sdk 2.3.1 register headers](https://github.com/raspberrypi/pico-sdk/tree/2.3.1/src/rp2350/hardware_regs/include/hardware/regs),
[QEMU memory API](../../../docs/devel/memory.rst),
[ADR 0002](https://github.com/Embeded-ojisan/rpi_pico2_qemu_inner/blob/main/docs/adr/0002-own-bus-security-filter.md).
