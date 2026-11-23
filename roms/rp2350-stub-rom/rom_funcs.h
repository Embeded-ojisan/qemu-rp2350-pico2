/*
 * ROM function codes known to pico-sdk 2.3.1 for RP2350
 * (src/rp2_common/boot_bootrom_headers/include/boot/bootrom_constants.h).
 * Generated from that header; ROM_DATA_* codes are left out.
 *
 * ROM_FUNC(name, code): code is ROM_TABLE_CODE(c1, c2) = c1 | (c2 << 8).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

ROM_FUNC(flash_enter_cmd_xip, 0x5843) /* 'CX' */
ROM_FUNC(flash_exit_xip, 0x5845) /* 'EX' */
ROM_FUNC(flash_flush_cache, 0x4346) /* 'FC' */
ROM_FUNC(connect_internal_flash, 0x4649) /* 'IF' */
ROM_FUNC(flash_range_erase, 0x4552) /* 'RE' */
ROM_FUNC(flash_range_program, 0x5052) /* 'RP' */
ROM_FUNC(pick_ab_partition, 0x4241) /* 'AB' */
ROM_FUNC(chain_image, 0x4943) /* 'CI' */
ROM_FUNC(explicit_buy, 0x4245) /* 'EB' */
ROM_FUNC(flash_runtime_to_storage_addr, 0x4146) /* 'FA' */
ROM_FUNC(flash_op, 0x4f46) /* 'FO' */
ROM_FUNC(get_b_partition, 0x4247) /* 'GB' */
ROM_FUNC(get_partition_table_info, 0x5047) /* 'GP' */
ROM_FUNC(get_sys_info, 0x5347) /* 'GS' */
ROM_FUNC(get_uf2_target_partition, 0x5547) /* 'GU' */
ROM_FUNC(load_partition_table, 0x504c) /* 'LP' */
ROM_FUNC(otp_access, 0x414f) /* 'OA' */
ROM_FUNC(flash_reset_address_trans, 0x4152) /* 'RA' */
ROM_FUNC(reboot, 0x4252) /* 'RB' */
ROM_FUNC(set_rom_callback, 0x4352) /* 'RC' */
ROM_FUNC(secure_call, 0x4353) /* 'SC' */
ROM_FUNC(set_ns_api_permission, 0x5053) /* 'SP' */
ROM_FUNC(bootrom_state_reset, 0x5253) /* 'SR' */
ROM_FUNC(set_bootrom_stack, 0x5353) /* 'SS' */
ROM_FUNC(flash_select_xip_read_mode, 0x4d58) /* 'XM' */
ROM_FUNC(validate_ns_buffer, 0x4256) /* 'VB' */
