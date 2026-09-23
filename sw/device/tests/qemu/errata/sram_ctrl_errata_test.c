// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sram_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Suite for `sram_ctrl` (P25).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * 1. [sram_ctrl_regs_reg_top.sv:555-579] (SPEC_DOC_ERRATA, LOW):
 *    `SCR_KEY_ROTATED` (`0x18`) is a `MuBi4` `SwAccessW1C` register updated on
 *    every write via `mubi4_and_hi(q, ~wd)`:
 *    - Writing `0x9` (`kMultiBitBool4False`) preserves `0x6`
 * (`kMultiBitBool4True`) unchanged (true no-op).
 *    - Writing `0x0` (standard `rw1c` no-op idiom) corrupts `0x6` into `0xf`.
 *    - Writing `0xf` (standard `rw1c` all-ones clear idiom) corrupts `0x6`
 *      into `0x0`.
 *    - Writing `0x6` (`kMultiBitBool4True`) cleanly clears `0x6` to `0x9`
 *      (`kMultiBitBool4False`).
 * 2. [sram_ctrl_reg_pkg.sv:152-162] (INTENDED_SECURITY_HARDENING, INFO —
 * SEC_CM: BUS.INTEGRITY): All 9 `sram_ctrl` CSRs (`0x00..0x20`) have
 * `SRAM_CTRL_REGS_PERMIT = 4'b0001`:
 *    - An 8-bit `sb` write to byte lane `0` (`EXEC_REGWEN + 0`) succeeds,
 *      whereas an 8-bit `sb` write to byte lane `1` (`EXEC_REGWEN + 1`,
 *      `reg_be[0] == 0`) faults synchronously with `mcause = 7`.
 *    - Reading an unmapped CSR offset (`0x24`) faults with `mcause = 5`,
 *      while reading `CTRL` (`0x14`, `wo`) returns `0x0`.
 * 3. [sram_ctrl.sv:272-305] (INTENDED_SECURITY_HARDENING, INFO — SEC_CM:
 * MEM.SCRAMBLE):
 *    - Rotating the scrambling key (`CTRL.RENEW_SCR_KEY = 1`) without
 *      `CTRL.INIT = 1` keeps `STATUS.INIT_DONE == 1`, and running
 *      `CTRL.INIT = 1` keeps `STATUS.SCR_KEY_VALID == 1`.
 *    - Reading an SRAM word written under the previous PRINCE `(key, nonce)`
 *      after `CTRL.RENEW_SCR_KEY = 1` decrypts with an invalid `SecdedInv3932`
 *      syndrome and triggers an Ibex Load Integrity Error NMI
 *      (`mcause = 0xffffffe0`).
 *    - Re-initializing `sram_ctrl_ret_aon` via `CTRL.INIT = 1` restores valid
 *      39-bit SECDED ECC across all words.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sram_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_RAM_BASE_ADDR,
  kIbexLoadIntegrityNmiMcause = 0xffffffe0u,
};

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;
static volatile uint32_t g_nmi_load_integ_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_load_store_fault = true;
  g_last_mcause = ibex_mcause_read();
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexLoadIntegrityNmiMcause) {
    g_nmi_load_integ_count++;
  }
}

static void sram_ret_renew_key(void) {
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET),
      SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)) {
  }
}

static void sram_ret_init_mem(void) {
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET),
      SRAM_CTRL_STATUS_INIT_DONE_BIT)) {
  }
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey SRAM_CTRL Errata Confirmation Suite (P25) ===");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdSramCtrlRetAonFatalError));

  uintptr_t ret_buf = kSramRetRamBase + offsetof(retention_sram_t, owner);

  // ---------------------------------------------------------------------------
  // 1. [sram_ctrl_regs_reg_top.sv:555-579] (SPEC_DOC_ERRATA, LOW):
  //    SCR_KEY_ROTATED (0x18) MuBi4 SwAccessW1C bitwise mubi4_and_hi(q, ~wd)
  //    behavior on 0x9 (no-op), 0x0 (corrupts 0x6 -> 0xf), 0xf (corrupts
  //    0x6 -> 0x0), and 0x6 (clears 0x6 -> 0x9).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [sram_ctrl_regs_reg_top.sv:555-579] (SPEC_DOC_ERRATA): "
      "SCR_KEY_ROTATED "
      "mubi4_and_hi(q, ~wd) W1C semantics");
  sram_ret_renew_key();
  uint32_t rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == kMultiBitBool4True,
        "Expected SCR_KEY_ROTATED == 0x6 after RENEW_SCR_KEY, got 0x%x", rot);

  // Writing 0x9 (kMultiBitBool4False) is the true no-op: preserves 0x6.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4False);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == kMultiBitBool4True,
        "[sram_ctrl_regs_reg_top.sv:555-579] Expected writing 0x9 to preserve "
        "SCR_KEY_ROTATED == 0x6, got 0x%x",
        rot);

  // Writing 0x0 (standard rw1c no-op) corrupts 0x6 into 0xf!
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   0x0u);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == 0xfu,
        "[sram_ctrl_regs_reg_top.sv:555-579] Expected writing 0x0 to corrupt "
        "SCR_KEY_ROTATED from 0x6 to 0xf, got 0x%x",
        rot);

  // Rotate again (0x6) and write 0xf (standard rw1c clear) -> corrupts to 0x0!
  sram_ret_renew_key();
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   0xfu);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == 0x0u,
        "[sram_ctrl_regs_reg_top.sv:555-579] Expected writing 0xf to corrupt "
        "SCR_KEY_ROTATED from 0x6 to 0x0, got 0x%x",
        rot);

  // Rotate again (0x6) and write 0x6 (kMultiBitBool4True) -> clears cleanly to
  // 0x9!
  sram_ret_renew_key();
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == kMultiBitBool4False,
        "[sram_ctrl_regs_reg_top.sv:555-579] Expected writing 0x6 to clear "
        "SCR_KEY_ROTATED to 0x9, got 0x%x",
        rot);
  LOG_INFO(
      "[sram_ctrl_regs_reg_top.sv:555-579] CONFIRMED: "
      "SCR_KEY_ROTATED(0x9->0x6, 0x0->0xf, "
      "0xf->0x0, 0x6->0x9)");

  // ---------------------------------------------------------------------------
  // 2. [sram_ctrl_reg_pkg.sv:152-162] (INTENDED_SECURITY_HARDENING — SEC_CM:
  // BUS.INTEGRITY):
  //    SRAM_CTRL_REGS_PERMIT = 4'b0001 accepts sb to +0, faults sb to +1
  //    (mcause=7), and faults unmapped read at 0x24 (mcause=5).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [sram_ctrl_reg_pkg.sv:152-162] (INTENDED_SECURITY_HARDENING): "
      "SRAM_CTRL_REGS_PERMIT = 4'b0001 sub-word CSR access");
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
        "Expected WO register CTRL (0x14) to read back 0");

  g_load_store_fault = false;
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET, 1u);
  CHECK(!g_load_store_fault,
        "[sram_ctrl_reg_pkg.sv:152-162] Expected sb to EXEC_REGWEN+0 "
        "(PERMIT=4'b0001) "
        "to succeed");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET + 1u, 1u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[sram_ctrl_reg_pkg.sv:152-162] Expected sb to EXEC_REGWEN+1 "
        "(reg_be=4'b0010) "
        "to fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSramRetRegsBase + 0x24u);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[sram_ctrl_reg_pkg.sv:152-162] Expected unmapped CSR read at 0x24 to "
        "fault "
        "with mcause=5");
  LOG_INFO("[sram_ctrl_reg_pkg.sv:152-162] CONFIRMED: PERMIT=4'b0001 enforced");

  // ---------------------------------------------------------------------------
  // 3. [sram_ctrl.sv:272-305] (INTENDED_SECURITY_HARDENING — SEC_CM:
  // MEM.SCRAMBLE):
  //    - CTRL.INIT = 1 sets STATUS.INIT_DONE = 1 while keeping SCR_KEY_VALID
  //    = 1.
  //    - Sub-word sb/sh writes on SRAM memory window succeed via 3-cycle RMW.
  //    - CTRL.RENEW_SCR_KEY = 1 without CTRL.INIT = 1 keeps STATUS.INIT_DONE =
  //    1,
  //      and reading prior SRAM words triggers Ibex Load Integrity NMI
  //      (mcause = 0xffffffe0).
  //    - Subsequent CTRL.INIT = 1 restores valid 39-bit SECDED ECC.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [sram_ctrl.sv:272-305] (INTENDED_SECURITY_HARDENING): "
      "RENEW_SCR_KEY status independence & stale-key ECC NMI");
  sram_ret_init_mem();
  uint32_t status =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT) &&
            bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT),
        "Expected INIT_DONE=1 and SCR_KEY_VALID=1 after CTRL.INIT, got 0x%x",
        status);

  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(ret_buf + i * 4u, 0xa5a50000u + i);
  }
  abs_mmio_write8(ret_buf + 1u, 0x5au);
  abs_mmio_write8(ret_buf + 2u, 0x34u);
  abs_mmio_write8(ret_buf + 3u, 0x12u);
  CHECK(abs_mmio_read32(ret_buf) == 0x12345a00u,
        "Expected sub-word RMW on SRAM data window to produce 0x12345a00");

  // Rotate scrambling key WITHOUT CTRL.INIT:
  g_nmi_load_integ_count = 0;
  sram_ret_renew_key();
  status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT),
        "[sram_ctrl.sv:272-305] Expected STATUS.INIT_DONE to remain 1 after "
        "CTRL.RENEW_SCR_KEY=1 without CTRL.INIT (status=0x%x)",
        status);

  for (uint32_t i = 0; i < 8 && g_nmi_load_integ_count == 0; ++i) {
    (void)abs_mmio_read32(ret_buf + i * 4u);
  }
  CHECK(g_nmi_load_integ_count >= 1u,
        "[sram_ctrl.sv:272-305] Expected reading stale SRAM words after "
        "CTRL.RENEW_SCR_KEY=1 to trigger Ibex Load Integrity NMI "
        "(mcause=0xffffffe0), got nmi_count=%u",
        g_nmi_load_integ_count);

  // Re-initialize retention SRAM via CTRL.INIT = 1 to restore clean ECC:
  sram_ret_init_mem();
  uint32_t nmi_before = g_nmi_load_integ_count;
  (void)abs_mmio_read32(ret_buf);
  CHECK(
      g_nmi_load_integ_count == nmi_before,
      "Expected read after CTRL.INIT=1 to succeed without Load Integrity NMI");
  LOG_INFO(
      "[sram_ctrl.sv:272-305] CONFIRMED: INIT_DONE remained 1 across "
      "RENEW_SCR_KEY, stale read fired Load Integrity NMI (count=%u), "
      "CTRL.INIT restored clean ECC",
      g_nmi_load_integ_count);

  LOG_INFO("=== ALL SRAM_CTRL ERRATA CHECKS PASSED ===");
  return true;
}
