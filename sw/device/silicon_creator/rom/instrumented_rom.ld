/* Copyright lowRISC contributors (OpenTitan project). */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */

/**
 * Linker script for an OpenTitan ROM_EXT.
 *
 * Portions of this file are Ibex-specific.
 *
 * The ROM_EXT is actually kept in flash, rather than ROM. While a ROM_EXT can
 * be loaded into either Slot A (the start of flash), or Slot B (the start of
 * the upper half of flash), this linker script only targets Slot A.
 */

INCLUDE hw/top_earlgrey/sw/autogen/top_earlgrey_memory.ld

_rom_boot_address = ORIGIN(eflash) + LENGTH(eflash) - 0x20000;

_rom_digest_size = 32;
_chip_info_start = ORIGIN(eflash) + LENGTH(eflash) - _rom_digest_size - _chip_info_size;

REGION_ALIAS("rom_region", eflash);

INCLUDE sw/device/silicon_creator/rom/rom_common.ld
