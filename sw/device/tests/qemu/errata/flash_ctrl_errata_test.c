// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file flash_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Empirical Errata Confirmation Test for FLASH_CTRL.
 *
 * Empirically confirms all software-visible hardware errata, specification
 * discrepancies, and security hardening behaviors documented in
 * `/root/knowledge/errata/flash_ctrl.md`:
 * - [flash_ctrl.sv:1084-1086] (TRUE_SILICON_ERRATA): `ERR_ADDR` (`0x188`) is
 *   omitted from `hw2reg.err_addr.de` on `PROG_WIN_ERR` and `PROG_TYPE_ERR`
 *   (`flash_ctrl.sv:1083-1086`), retaining the previous error address despite
 *   `flash_ctrl.hjson` and `programmers_guide.md` instructing software to read
 *   `ERR_ADDR`.
 * - [flash_ctrl_rd.sv:96-102] (TRUE_SILICON_ERRATA): Single-word controller
 * read
 *   (`CONTROL.NUM = 0`) `op_err_addr_o` 1-cycle pipeline hazard
 *   (`flash_ctrl_rd.sv:96-102, 135-147`): when a 1-word read faults with
 *   `MP_ERR`, `ERR_ADDR` latches the *previous* read error address while
 *   `op_err_addr_o` updates for the next read failure.
 * - [flash_ctrl_erase.sv:58-62] & [flash_ctrl.sv:1159-1160] (SPEC_DOC_ERRATA):
 *   `FlashOpErase` masks `ERR_ADDR` to 2 KiB page (`~0x7ff`) or 512 KiB bank
 *   (`~0x7ffff`) boundaries (`flash_ctrl_erase.sv:58-62`), and
 *   `INTR_STATE.CORR_ERR` (`bit 5`) never asserts on `ERR_CODE` controller
 *   errors (`flash_ctrl.sv:1159-1160`).
 * - [flash_ctrl_prog.sv:128-130] (SPEC_DOC_ERRATA): `PROG_WIN_ERR` and
 *   `PROG_TYPE_ERR` assert simultaneously and hold `flash_ctrl_prog` in
 *   `StErr` (`OP_STATUS.DONE = 0`, `CONTROL.START = 1`, `CTRL_REGWEN = 0`)
 *   until `CONTROL.NUM + 1` dummy words are pushed to `PROG_FIFO`.
 * - [flash_ctrl.sv:621-671] (INTENDED_SECURITY_HARDENING) &
 *   [tlul_adapter_sram.sv:340-350] (TRUE_SILICON_ERRATA):
 *   Reading `RD_FIFO` (`0x1b4`) when empty/no-op outputs `39'h0` (`data_intg =
 *   7'h0 != 7'h39`), triggering both `LoadAccessFault` (`mcause = 5`) and an
 *   Ibex `load_resp_intg_err_o` NMI (`mcause = 0xffffffe0`); similarly, any
 *   CPU store to the eFlash window (`0x20000000`) returns `d_data = 0xffffffff`
 *   with `data_intg = 0x39` (`SecdedInv3932ZeroEcc` instead of `0x55` in
 *   `tlul_adapter_sram.sv:340-350`), triggering both `StoreAccessFault`
 *   (`mcause = 7`) and an Ibex `store_resp_intg_err_o` NMI (`mcause =
 *   0xffffffe0`) + `RV_CORE_IBEX.ERR_STATUS.FATAL_INTG_ERR`.
 * - [flash_ctrl.sv:524-552..010] (INTENDED_SECURITY_HARDENING): `PROG_FIFO`
 *   (`ErrOnRead=1`, `.ByteAccess(0)`), `RD_FIFO` (`ErrOnWrite=1` without NMI),
 *   and CSR `PERMIT` sub-word write faults (`mcause = 5 / 7`).
 * - [flash_ctrl_prim_reg_top.sv:355-570] (SPEC_DOC_ERRATA): `flash_ctrl_prim`
 * `CSR2`
 * (`0x08`) bypasses `CSR0_REGWEN` (`0x00`) after `CSR0_REGWEN` is locked to
 * `0`.
 * - [flash_ctrl.sv:560] (SPEC_DOC_ERRATA): Level-sensitive `FIFO_RST`
 *   holds `RD_FIFO` empty (`CURR_FIFO_LVL.RD == 0`) during `OP_READ`.
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "flash_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kFlashCoreBase = TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR,
  kFlashPrimBase = TOP_EARLGREY_FLASH_CTRL_PRIM_BASE_ADDR,
  kFlashMemBase = TOP_EARLGREY_EFLASH_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_fault_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_count++;
  g_last_fault_mcause = ibex_mcause_read();
}

void ottf_load_integrity_error_handler(uint32_t *exc_info) {
  (void)exc_info;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                   1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT);
}

static void clear_flash_status(void) {
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INTR_STATE_REG_OFFSET, 0x3fu);
}

static uint32_t wait_op_done(void) {
  uint32_t op_status;
  do {
    op_status =
        abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(op_status, FLASH_CTRL_OP_STATUS_DONE_BIT));
  return op_status;
}

static void trigger_mp_err_read(uint32_t byte_addr, uint32_t num_field) {
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, byte_addr);
  uint32_t ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, num_field);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, ctrl);
  uint32_t st = wait_op_done();
  CHECK(st == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
               (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET),
      FLASH_CTRL_ERR_CODE_MP_ERR_BIT));
  // Drain RD_FIFO via FIFO_RST so subsequent reads start with an empty RD_FIFO.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey FLASH_CTRL Errata Confirmation Suite (P09) ===");

  // ---------------------------------------------------------------------------
  // 1. [flash_ctrl_rd.sv:96-102] (TRUE_SILICON_ERRATA): Single-word read
  //    (`CONTROL.NUM = 0`) `op_err_addr_o` 1-cycle pipeline hazard.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl_rd.sv:96-102] (TRUE_SILICON_ERRATA): NUM=0 1-word "
      "read ERR_ADDR pipeline hazard");
  // Ensure Bank 1 Data region (`0x80000..0xfffff`) is disabled (`MP_ERR`).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET, 0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET, 0u);

  // Step 1a: Multi-word read (`NUM = 1`, 2 words) at `0x81234`: enters `StErr`
  // for 1 extra cycle, so `op_err_addr_o` and `ERR_ADDR` both latch `0x81234`.
  trigger_mp_err_read(0x81234u, /*num_field=*/1u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x81234u);

  // Step 1b: Single-word read (`NUM = 0`, 1 word) at `0x82348`: `cnt_hit == 1`
  // on word 0 asserts `op_done_o` and `hw2reg.err_addr.de` BEFORE
  // `op_err_addr_o` updates at the posedge, so `ERR_ADDR` latches the STALE
  // `0x81234` while `op_err_addr_o` updates to `0x82348`!
  trigger_mp_err_read(0x82348u, /*num_field=*/0u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x81234u);

  // Step 1c: Second single-word read (`NUM = 0`, 1 word) at `0x8345c`: now
  // `ERR_ADDR` latches `0x82348` (from Step 1b!) while `op_err_addr_o` updates
  // to `0x8345c`!
  trigger_mp_err_read(0x8345cu, /*num_field=*/0u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x82348u);

  // Step 1d: Third single-word read (`NUM = 0`, 1 word) at `0x84560`: now
  // `ERR_ADDR` latches `0x8345c` (from Step 1c!).
  trigger_mp_err_read(0x84560u, /*num_field=*/0u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x8345cu);

  // ---------------------------------------------------------------------------
  // 2. [flash_ctrl.sv:1084-1086] (TRUE_SILICON_ERRATA) &
  //    [flash_ctrl_prog.sv:128-130] (SPEC_DOC_ERRATA):
  //    `PROG_WIN_ERR` and `PROG_TYPE_ERR` assert simultaneously, hold
  //    `flash_ctrl_prog` in `StErr` until `NUM + 1` dummy words are pushed to
  //    `PROG_FIFO`, and NEVER update `ERR_ADDR` (`0x188`).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl.sv:1084-1086] & [flash_ctrl_prog.sv:128-130]: "
      "PROG_WIN_ERR + PROG_TYPE_ERR StErr drain & ERR_ADDR omission");
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // Clear PROG_TYPE_EN.NORMAL_EN (RW0C) so `prog_type_err` asserts alongside
  // `prog_win_err` (`ADDR = 0x80ffc` with `NUM = 1` crosses 64B window
  // `0x80fc0..0x80fff`).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80ffcu);
  uint32_t prog_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_PROG) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, prog_ctrl);

  // Verify [flash_ctrl_prog.sv:128-130]: Both PROG_WIN_ERR and PROG_TYPE_ERR
  // assert immediately, and `flash_ctrl_prog` stays in `StErr` (`OP_STATUS.DONE
  // == 0`, `CTRL_REGWEN == 0`) until `NUM + 1 = 2` dummy words are pushed to
  // PROG_FIFO.
  uint32_t err_code =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET);
  CHECK(bitfield_bit32_read(err_code, FLASH_CTRL_ERR_CODE_PROG_WIN_ERR_BIT));
  CHECK(bitfield_bit32_read(err_code, FLASH_CTRL_ERR_CODE_PROG_TYPE_ERR_BIT));
  CHECK(!bitfield_bit32_read(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET),
      FLASH_CTRL_OP_STATUS_DONE_BIT));
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        0u);

  // Push 2 dummy words into PROG_FIFO to drain `StErr` and complete the op.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0x11111111u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0x22222222u);
  uint32_t prog_st = wait_op_done();
  CHECK(prog_st == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                    (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)));

  // Verify [flash_ctrl.sv:1084-1086]: `ERR_ADDR` was NOT updated to `0x80ffc`
  // and still holds `0x8345c` from Step 1d!
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x8345cu);
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 3. [flash_ctrl_erase.sv:58-62] & [flash_ctrl.sv:1159-1160]
  // (SPEC_DOC_ERRATA):
  //    Page/Bank Erase masks `ERR_ADDR` to `~0x7ff` / `~0x7ffff`, and
  //    `INTR_STATE.CORR_ERR` never asserts on `ERR_CODE` errors.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl_erase.sv:58-62] & [flash_ctrl.sv:1159-1160]: "
      "Erase "
      "ERR_ADDR masking & CORR_ERR exclusion");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x85678u);
  uint32_t pg_erase_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_ERASE);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   pg_erase_ctrl);
  CHECK(wait_op_done() == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x85000u);
  CHECK(!bitfield_bit32_read(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_INTR_STATE_REG_OFFSET),
      FLASH_CTRL_INTR_STATE_CORR_ERR_BIT));
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x85678u);
  uint32_t bk_erase_ctrl = bitfield_bit32_write(
      pg_erase_ctrl, FLASH_CTRL_CONTROL_ERASE_SEL_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   bk_erase_ctrl);
  CHECK(wait_op_done() == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x80000u);
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 4. [flash_ctrl.sv:621-671] (INTENDED_SECURITY_HARDENING) &
  //    [tlul_adapter_sram.sv:340-350] (TRUE_SILICON_ERRATA):
  //    - Empty/No-Op read of `RD_FIFO` (`0x1b4`) outputs `39'h0` (`data_intg =
  //    0`)
  //      -> triggers `LoadAccessFault` (`mcause = 5`) + Load Integrity NMI
  //      (`mcause = 0xffffffe0`).
  //    - CPU store to eFlash (`0x20080000`) returns `d_data = 0xffffffff` with
  //      `data_intg = 0x39` -> triggers `StoreAccessFault` (`mcause = 7`) +
  //      Store Integrity NMI (`mcause = 0xffffffe0`).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl.sv:621-671] & [tlul_adapter_sram.sv:340-350]: "
      "RD_FIFO "
      "empty read & eFlash store Bus Integrity NMI");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));

  g_fault_count = 0;
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_count = 0;
  abs_mmio_write32(kFlashMemBase + 0x80000u, 0xdeadbeefu);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  // ---------------------------------------------------------------------------
  // 5. [flash_ctrl.sv:524-552..010] (INTENDED_SECURITY_HARDENING):
  //    - Write to `RD_FIFO` (`0x1b4`) -> `StoreAccessFault` (`mcause = 7`).
  //    - Read from `PROG_FIFO` (`0x1b0`) -> `LoadAccessFault` (`mcause = 5`).
  //    - Sub-word write (`sb`) to `PROG_FIFO` (`0x1b0`) and `CONTROL` (`0x20`)
  //      -> `StoreAccessFault` (`mcause = 7`).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl.sv:524-552..010]: PROG_FIFO/RD_FIFO & CSR PERMIT "
      "TL-UL access faults");
  g_fault_count = 0;
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET, 0x12345678u);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET, 0x11u);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, 0x01u);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  // ---------------------------------------------------------------------------
  // 6. [flash_ctrl_prim_reg_top.sv:355-570] (SPEC_DOC_ERRATA):
  // `flash_ctrl_prim` `CSR2`
  //    (`0x08`) bypasses `CSR0_REGWEN` (`0x00`) after `CSR0_REGWEN` is locked.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl_prim_reg_top.sv:355-570]: flash_ctrl_prim CSR2 "
      "bypasses "
      "CSR0_REGWEN");
  abs_mmio_write32(kFlashPrimBase + 0x04u, 0x123u);  // CSR1
  CHECK(abs_mmio_read32(kFlashPrimBase + 0x04u) == 0x123u);
  abs_mmio_write32(kFlashPrimBase + 0x00u, 0u);  // Lock CSR0_REGWEN = 0
  CHECK(abs_mmio_read32(kFlashPrimBase + 0x00u) == 0u);
  abs_mmio_write32(kFlashPrimBase + 0x04u, 0x456u);  // CSR1 write ignored
  CHECK(abs_mmio_read32(kFlashPrimBase + 0x04u) == 0x123u);
  abs_mmio_write32(kFlashPrimBase + 0x08u,
                   0x88u);  // CSR2 rw bits [7,3] bypass lock!
  CHECK((abs_mmio_read32(kFlashPrimBase + 0x08u) & 0x88u) == 0x88u);
  abs_mmio_write32(kFlashPrimBase + 0x08u, 0x00u);
  CHECK((abs_mmio_read32(kFlashPrimBase + 0x08u) & 0x88u) == 0x00u);

  // ---------------------------------------------------------------------------
  // 7. [flash_ctrl.sv:560] (SPEC_DOC_ERRATA): Level-sensitive `FIFO_RST`
  //    holds `RD_FIFO` empty (`CURR_FIFO_LVL.RD == 0`) during `OP_READ`.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [flash_ctrl.sv:560]: Level-sensitive FIFO_RST holds "
      "RD_FIFO empty during OP_READ");
  const uint32_t kInfoCfgAllowRead =
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_RD_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_SCRAMBLE_EN_7_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ECC_EN_7_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_HE_EN_7_FIELD,
                             kMultiBitBool4False);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK0_INFO1_PAGE_CFG_REG_OFFSET,
                   kInfoCfgAllowRead);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 1u);
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x0u);
  uint32_t info_rd_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, 1u) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   info_rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT));
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CURR_FIFO_LVL_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 0u);

  LOG_INFO("=== ALL FLASH_CTRL ERRATA CHECKS PASSED ===");
  return true;
}
