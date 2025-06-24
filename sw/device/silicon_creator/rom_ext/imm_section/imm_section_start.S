// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey_memory.h"
#include "sw/device/coverage/runtime_asm.h"
#include "sw/device/lib/base/hardened_asm.h"

  /**
   * NOTE: The "ax" flag below is necessary to ensure that this section
   * is allocated executable space in ROM by the linker.
   */
  .section .crt, "ax"

  /**
   * Entry point.
   *
   * This symbol is jumped to from `rom_boot` using the `entry_point` field
   * of the manifest.
   */
  .balign 4
  .global _imm_section_start_boot
  .type _imm_section_start_boot, @function
_imm_section_start_boot:

  .option push
  .option norelax

  /**
   * Set up the global pointer `gp`.
   */
  la gp, __global_pointer$

  .option pop

  /**
   * Save registers.
   */
  addi sp, sp, -8
  sw ra, 4(sp)

#ifdef OT_COVERAGE_INSTRUMENTED
  addi sp, sp, -8
  sw s10, 0(sp)
  sw s11, 4(sp)

  COVERAGE_ASM_COUNTER_INIT()
#endif

  // PRAGMA_COVERAGE: start autogen with register bits

  /**
   * Disable Interrupts.
   *
   * We cannot disable exceptions, or Ibex's non-maskable interrupts (interrupt
   * 31), so we still need to be careful.
   */

  // Clear `MIE` field of `mstatus` (disable interrupts globally).
  csrci mstatus, 0x8

  /**
   * Clear all the machine-defined interrupts, `MEIE`, `MTIE`, and `MSIE` fields
   * of `mie`.
   */
  li   t0, 0xFFFF0888
  csrc mie, t0

  /**
   * Setup C Runtime
   */
  /**
   * Initialize the `.data` section in RAM from ROM.
   */
  la   a0, _data_start
  la   a1, _data_end
  la   a2, _data_init_start
  call crt_section_copy

  /**
   * Initialize the `.bss` section.
   *
   * We do this despite zeroing all of SRAM above, so that we still zero `.bss`
   * once we've enabled SRAM scrambling.
   */
  la   a0, _bss_start
  la   a1, _bss_end
  call crt_section_clear

#ifdef OT_COVERAGE_INSTRUMENTED
  call coverage_init

  // Store the maunal counters to the prf_cnts region.
  mv a0, s10
  mv a1, s11
  call coverage_save_asm_counters

  lw s10, 0(sp)
  lw s11, 4(sp)
  addi sp, sp, 8
#endif

  /**
   * Restore registers.
   */
  lw ra, 4(sp)
  addi sp, sp, 8

  /**
   * Jump to IMM_SECTION.
   */
  tail imm_section_main

  .size _imm_section_start_boot, .-_imm_section_start_boot
