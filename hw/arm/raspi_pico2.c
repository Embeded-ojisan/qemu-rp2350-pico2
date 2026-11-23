/*
 * Raspberry Pi Pico 2 board: RP2350 with a 4 MB QSPI flash on QMI CS0
 *
 * The flash (Winbond W25Q32RV on the board) is modelled by the m25p80
 * "w25q32" part. Its contents come from -drive if=mtd,format=raw,file=...;
 * an ELF given with -kernel is written over the flash contents at reset
 * (in memory only). The ROM holds the stub ROM, which -bios replaces.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/rp2350_soc.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "system/blockdev.h"
#include "system/block-backend.h"

#define PICO2_FLASH_PART    "w25q32"
#define PICO2_FLASH_SIZE    (4 * MiB)

static const char pico2_default_bios[] = "rp2350-stub-rom.bin";

static void raspi_pico2_load_rom(MachineState *machine, RP2350State *soc)
{
    const char *bios_name = machine->firmware ?: pico2_default_bios;
    g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);

    if (!filename) {
        error_report("Could not find ROM image '%s'", bios_name);
        exit(1);
    }
    /*
     * Load by address rather than into soc->rom: the M-profile CPU reset
     * reads the initial SP and PC through rom_ptr_for_as(), which ignores
     * images registered against a MemoryRegion.
     */
    if (load_image_targphys(filename, RP2350_ROM_BASE, RP2350_ROM_SIZE,
                            NULL) < 0) {
        error_report("Failed to load ROM image '%s'", filename);
        exit(1);
    }
}

static void raspi_pico2_init(MachineState *machine)
{
    DeviceState *soc_dev = qdev_new(TYPE_RP2350);
    RP2350State *soc = RP2350(soc_dev);
    DeviceState *flash;
    DriveInfo *dinfo;

    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc_dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc_dev), &error_fatal);

    flash = qdev_new(PICO2_FLASH_PART);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(flash, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
    ssi_realize_and_unref(flash, soc->qmi.ssi, &error_fatal);
    qdev_connect_gpio_out_named(DEVICE(&soc->qmi), "cs", 0,
                                qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0));
    rp2350_qmi_attach_xip(&soc->qmi, 0, flash);

    raspi_pico2_load_rom(machine, soc);
    armv7m_load_kernel(soc->armv7m.cpu, machine->kernel_filename,
                       RP2350_XIP_BASE, PICO2_FLASH_SIZE);
}

static void raspi_pico2_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"),
        NULL
    };

    mc->desc = "Raspberry Pi Pico 2 (RP2350, Cortex-M33 core 0)";
    mc->init = raspi_pico2_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
}

DEFINE_MACHINE("raspi-pico2", raspi_pico2_machine_init)
