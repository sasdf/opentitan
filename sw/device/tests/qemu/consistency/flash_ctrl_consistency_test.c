// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_flash_ctrl.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "flash_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile uint32_t g_fault_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_count++;
}

// In flash_ctrl.sv, u_to_prog_fifo (0x1b0), u_to_rd_fifo (0x1b4), and
// u_tl_adapter_eflash (0x20000000) instantiate tlul_adapter_sram with
// .EnableDataIntgGen(0) and .EnableDataIntgPt(1), returning
// d_data = 0xffffffff and data_intg = SecdedInv3932ZeroEcc (0x39) on OpWrite.
// On RTL/FPGA, ibex_load_store_unit asserts store_resp_intg_err_o = 1, which
// raises kIbexInternalIrqLoadInteg (mcause = 0xffffffe0) and Alert 60
// (kTopEarlgreyAlertIdRvCoreIbexFatalHwErr) alongside StoreAccessFault.
void ottf_load_integrity_error_handler(uint32_t *exc_info) { (void)exc_info; }

enum {
  kFlashCoreBase = TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR,
  kFlashPrimBase = TOP_EARLGREY_FLASH_CTRL_PRIM_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

bool test_main(void) {
  bool all_ok = true;

  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));

  // 1. WO registers (INTR_TEST, ALERT_TEST) must read back as 0.
  // Note: PROG_FIFO (0x1b0) is a tlul_adapter_sram window with ErrOnRead=1
  // in flash_ctrl.sv, which raises a Load Access Fault on RTL/FPGA when read.
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_INTR_TEST_REG_OFFSET) == 0u,
      "INTR_TEST is WO and must read back as 0");
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ALERT_TEST_REG_OFFSET) == 0u,
      "ALERT_TEST is WO and must read back as 0");

  // 2. INIT (0x18) is rw1s: once set to 1, writing 0 must keep INIT == 1.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INIT_REG_OFFSET, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INIT_REG_OFFSET, 0u);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_INIT_REG_OFFSET) == 1u,
             "INIT is rw1s and must remain 1 after writing 0");

  // 3. RO registers (STATUS, PHY_STATUS, CTRL_REGWEN) ignore software writes.
  uint32_t status_before =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET, 0xffffffffu);
  uint32_t status_after =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  EXPECT_RTL(status_after == status_before,
             "STATUS is RO; before=0x%08x, after=0x%08x", status_before,
             status_after);

  uint32_t phy_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PHY_STATUS_REG_OFFSET);
  EXPECT_RTL(
      (phy_status & 0x6u) == 0x6u,
      "PHY_STATUS.prog_normal_avail and prog_repair_avail must be 1 (0x%08x)",
      phy_status);

  uint32_t ctrl_regwen =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET);
  EXPECT_RTL(ctrl_regwen == 1u,
             "CTRL_REGWEN should be 1 when idle (got 0x%08x)", ctrl_regwen);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET, 0u);
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET) == 1u,
      "CTRL_REGWEN is RO (HW-managed) and must ignore software writes");

  // 4. SCRATCH (0x1a0) full 32-bit RW and FIFO_LVL (0x1a4) reserved bit masking
  // (0x1f1f).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET, 0xdeadbeefu);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET) ==
                 0xdeadbeefu,
             "SCRATCH readback mismatch");

  uint32_t fifo_lvl_orig =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET,
                   0xffffffffu);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET) ==
                 0x00001f1fu,
             "FIFO_LVL reserved bits must read as 0 (expected 0x1f1f)");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET,
                   fifo_lvl_orig);

  // 5. PROG_TYPE_EN (0x28) is rw0c: clearing bit 1 (REPAIR) to 0 cannot be set
  // back to 1.
  uint32_t prog_type_before =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET);
  if ((prog_type_before & (1u << FLASH_CTRL_PROG_TYPE_EN_REPAIR_BIT)) != 0u) {
    uint32_t cleared =
        prog_type_before & ~(1u << FLASH_CTRL_PROG_TYPE_EN_REPAIR_BIT);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET,
                     cleared);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET, 0x3u);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET) == cleared,
               "PROG_TYPE_EN is rw0c and must not allow setting cleared bits "
               "back to 1");
  }

  // 6. Region configuration & REGION_CFG_REGWEN_7 W0C lock test (using region
  // 7).
  uint32_t regwen7 = abs_mmio_read32(kFlashCoreBase +
                                     FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET);
  if (regwen7 == 1u) {
    uint32_t test_cfg =
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_RD_EN_7_FIELD,
                               kMultiBitBool4True) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_PROG_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ERASE_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u,
                               FLASH_CTRL_MP_REGION_CFG_7_SCRAMBLE_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ECC_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_HE_EN_7_FIELD,
                               kMultiBitBool4False);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                     0xf0000000u | test_cfg);
    EXPECT_RTL(
        abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET) == test_cfg,
        "MP_REGION_CFG_7 upper bits [31:28] must be masked to 0");

    // Lock REGION_CFG_REGWEN_7 via W0C and verify it cannot be re-enabled.
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET,
                     0u);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET,
                     1u);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET) == 0u,
               "REGION_CFG_REGWEN_7 is W0C and must remain 0 once cleared");

    // Attempt to modify MP_REGION_CFG_7 while locked -> must be ignored.
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                     0x9999999u);
    EXPECT_RTL(
        abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET) == test_cfg,
        "MP_REGION_CFG_7 write must be ignored when REGION_CFG_REGWEN_7 == 0");
  }

  // 7. MP_BANK_CFG_SHADOWED (0x16c) shadow semantics and BANK_CFG_REGWEN
  // (0x168) gating.
  uint32_t bank_cfg_regwen =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_BANK_CFG_REGWEN_REG_OFFSET);
  if (bank_cfg_regwen == 1u) {
    uint32_t orig_bank_cfg = abs_mmio_read32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET);
    uint32_t target_bank_cfg = orig_bank_cfg ^ 0x3u;

    // Single write stages value without committing (and reading resets shadow
    // phase).
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        target_bank_cfg);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET) ==
                   orig_bank_cfg,
               "MP_BANK_CFG_SHADOWED must not update on single write");

    // Two consecutive matching writes commit value.
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        target_bank_cfg);
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        target_bank_cfg);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET) ==
                   target_bank_cfg,
               "MP_BANK_CFG_SHADOWED must commit on second matching write");

    // Mismatched two-step write sets ERR_CODE.UPDATE_ERR and triggers recov_err
    // alert.
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET, 0x1u);
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET, 0x2u);
    uint32_t update_err_code =
        abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET);
    EXPECT_RTL(
        (update_err_code & (1u << FLASH_CTRL_ERR_CODE_UPDATE_ERR_BIT)) != 0u,
        "Mismatched MP_BANK_CFG_SHADOWED write must set ERR_CODE.UPDATE_ERR "
        "(got 0x%08x)",
        update_err_code);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET,
                     update_err_code);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

    // Restore original value via two matching writes.
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        orig_bank_cfg);
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        orig_bank_cfg);

    // Lock BANK_CFG_REGWEN (rw0c) and verify shadowed writes are ignored.
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK_CFG_REGWEN_REG_OFFSET,
                     0u);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK_CFG_REGWEN_REG_OFFSET,
                     1u);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_BANK_CFG_REGWEN_REG_OFFSET) == 0u,
               "BANK_CFG_REGWEN is rw0c and must remain 0 once cleared");
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        target_bank_cfg);
    abs_mmio_write32(
        kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
        target_bank_cfg);
    EXPECT_RTL(
        abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET) ==
            orig_bank_cfg,
        "MP_BANK_CFG_SHADOWED must ignore writes when BANK_CFG_REGWEN == 0");
  }

  // 8. Invalid CONTROL.OP (0x3) sets OP_STATUS.ERR, OP_STATUS.DONE,
  // ERR_CODE.OP_ERR, clears CONTROL.START, does NOT set INTR_STATE.CORR_ERR,
  // and pulses recov_err alert.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INTR_STATE_REG_OFFSET,
                   (1u << FLASH_CTRL_INTR_STATE_OP_DONE_BIT) |
                       (1u << FLASH_CTRL_INTR_STATE_CORR_ERR_BIT));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  uint32_t bad_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD, 0x3u) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, bad_ctrl);

  uint32_t ctrl_after =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(ctrl_after, FLASH_CTRL_CONTROL_START_BIT) == false,
      "CONTROL.START must be cleared by hardware upon op completion (got "
      "0x%08x)",
      ctrl_after);

  uint32_t intr_state =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_state & (1u << FLASH_CTRL_INTR_STATE_CORR_ERR_BIT)) == 0u,
             "OP_ERR must NOT set INTR_STATE.CORR_ERR (got 0x%08x)",
             intr_state);

  uint32_t op_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET);
  uint32_t err_code =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(op_status == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)),
             "Invalid CONTROL.OP (0x3) must set OP_STATUS.DONE | OP_STATUS.ERR "
             "(got 0x%08x)",
             op_status);
  EXPECT_RTL((err_code & (1u << FLASH_CTRL_ERR_CODE_OP_ERR_BIT)) != 0u,
             "Invalid CONTROL.OP (0x3) must set ERR_CODE.OP_ERR (got 0x%08x)",
             err_code);

  // Clear ERR_CODE (W1C) and OP_STATUS (RW), and verify alert was caught.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, err_code);
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET) == 0u,
      "ERR_CODE must clear to 0 on W1C write");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET) == 0u,
      "OP_STATUS must clear to 0 on RW write");

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // 9. Prim CSRs: CSR0_REGWEN (0x00) is rw0c; CSR2 (0x08) and CSR20 (0x50) are
  // ungated by CSR0_REGWEN.
  abs_mmio_write32(kFlashPrimBase + 0x00u, 0u);
  abs_mmio_write32(kFlashPrimBase + 0x00u, 1u);
  EXPECT_RTL(abs_mmio_read32(kFlashPrimBase + 0x00u) == 0u,
             "CSR0_REGWEN is rw0c and must remain 0 once cleared");

  // CSR2 (0x08) bits 3 and 7 (0x88) are rw and ungated by CSR0_REGWEN; bits
  // [2:0,6:4] (0x77) are rw1c.
  abs_mmio_write32(kFlashPrimBase + 0x08u, 0xffu);
  EXPECT_RTL(abs_mmio_read32(kFlashPrimBase + 0x08u) == 0x88u,
             "CSR2 rw bits (0x88) must be writable when CSR0_REGWEN == 0, "
             "while rw1c bits (0x77) stay 0");
  abs_mmio_write32(kFlashPrimBase + 0x08u, 0x00u);

  // CSR20 (0x50) bits [1:0] are rw1c and bit 2 is ro -> writing 1s must not set
  // bits.
  abs_mmio_write32(kFlashPrimBase + 0x50u, 0x07u);
  EXPECT_RTL(abs_mmio_read32(kFlashPrimBase + 0x50u) == 0x00u,
             "CSR20 rw1c/ro bits must not be set by software writes");

  // 10. Wave 2: ERR_ADDR latching semantics (flash_ctrl.sv:1083-1086) and
  // erase MP_ERR page/bank alignment (flash_ctrl_erase.sv:58-62).
  // Preserve RD_EN, SCRAMBLE_EN, ECC_EN, and HE_EN in DEFAULT_REGION because
  // under sival_rom_ext the owner test stage executes from 0x20010480 (outside
  // ROM_EXT MP_REGION_CFG_0/1) using DEFAULT_REGION.
  uint32_t orig_default_region =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_DEFAULT_REGION_REG_OFFSET);
  uint32_t dis_default_region = orig_default_region;
  dis_default_region = bitfield_field32_write(
      dis_default_region, FLASH_CTRL_DEFAULT_REGION_PROG_EN_FIELD,
      kMultiBitBool4False);
  dis_default_region = bitfield_field32_write(
      dis_default_region, FLASH_CTRL_DEFAULT_REGION_ERASE_EN_FIELD,
      kMultiBitBool4False);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_DEFAULT_REGION_REG_OFFSET,
                   dis_default_region);

  // 10a. Page Erase MP_ERR at unaligned intra-page address 0x81234 (Info
  // Partition 2, which is disabled under both ROM and ROM_EXT) must latch
  // page-aligned ERR_ADDR 0x81000 (op_addr_i & PageAddrMask in
  // flash_ctrl_erase.sv).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x81234u);
  uint32_t pg_erase_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_ERASE) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, 2u) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_ERASE_SEL_BIT, false) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   pg_erase_ctrl);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
                 0x81000u,
             "Page erase MP_ERR at 0x81234 must latch page-aligned ERR_ADDR "
             "0x81000 (got 0x%08x)",
             abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // 10b. Bank Erase MP_ERR at unaligned address 0x81234 must latch
  // bank-aligned ERR_ADDR 0x80000 (op_addr_i & BankAddrMask in
  // flash_ctrl_erase.sv).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x81234u);
  uint32_t bk_erase_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_ERASE) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_ERASE_SEL_BIT, true) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   bk_erase_ctrl);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
                 0x80000u,
             "Bank erase MP_ERR at 0x81234 must latch bank-aligned ERR_ADDR "
             "0x80000 (got 0x%08x)",
             abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // 10c. Read MP_ERR (with NUM=1 so u_flash_ctrl_rd enters StErr) at 0x81234
  // (Info Partition 2) must latch word-aligned ERR_ADDR 0x81234.
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x81234u);
  uint32_t rd_err_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, 2u) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, rd_err_ctrl);
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
                 0x81234u,
             "Read MP_ERR at 0x81234 must latch ERR_ADDR 0x81234 (got 0x%08x)",
             abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_DEFAULT_REGION_REG_OFFSET,
                   orig_default_region);

  // 10d. OP_ERR must NOT overwrite ERR_ADDR (hw2reg.err_addr.de is only
  // mp_err | rd_err | prog_err in flash_ctrl.sv:1084-1086).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x00100u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, bad_ctrl);
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
          0x81234u,
      "OP_ERR must not overwrite ERR_ADDR (expected 0x81234, got 0x%08x)",
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // 11. Wave 2: PROG_WIN_ERR & PROG_TYPE_ERR simultaneous reporting,
  // ERR_ADDR preservation, and StErr PROG_FIFO draining
  // (flash_ctrl_prog.sv:164-178).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x8003cu);
  uint32_t win_and_type_err_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_PROG) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_PROG_SEL_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   win_and_type_err_ctrl);

  uint32_t pre_drain_err_code =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET);
  uint32_t expected_win_type_bits =
      (1u << FLASH_CTRL_ERR_CODE_PROG_WIN_ERR_BIT) |
      (1u << FLASH_CTRL_ERR_CODE_PROG_TYPE_ERR_BIT);
  EXPECT_RTL(
      (pre_drain_err_code & expected_win_type_bits) == expected_win_type_bits,
      "PROG with both window crossing and disabled PROG_SEL must set both "
      "PROG_WIN_ERR and PROG_TYPE_ERR (got 0x%08x)",
      pre_drain_err_code);

  uint32_t pre_drain_op_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET);
  EXPECT_RTL(pre_drain_op_status == (1u << FLASH_CTRL_OP_STATUS_ERR_BIT),
             "Before PROG_FIFO drains NUM+1 words in StErr, OP_STATUS must be "
             "ERR=1, DONE=0 (got 0x%08x)",
             pre_drain_op_status);
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET) == 0u,
      "CTRL_REGWEN must be 0 while StErr is waiting to drain PROG_FIFO");

  // Push NUM+1 (2) words into PROG_FIFO to complete StErr draining.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0xffffffffu);

  uint32_t post_drain_op_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET);
  EXPECT_RTL(post_drain_op_status == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                                      (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)),
             "After draining PROG_FIFO in StErr, OP_STATUS must be DONE|ERR "
             "(got 0x%08x)",
             post_drain_op_status);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
                 0x81234u,
             "PROG_WIN_ERR/PROG_TYPE_ERR must not overwrite ERR_ADDR (expected "
             "0x81234, got 0x%08x)",
             abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // 12. Wave 4: PROG_FIFO (0x1b0) ErrOnRead=1 (flash_ctrl.sv:528) and
  // RD_FIFO (0x1b4) ErrOnWrite=1 (flash_ctrl.sv:644) must return TL-UL
  // d_error=1 (Ibex Load/Store Access Fault).
  g_fault_count = 0;
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET);
  EXPECT_RTL(g_fault_count == 1u,
             "Reading PROG_FIFO (ErrOnRead=1) must raise Load Access Fault "
             "(faults=%u)",
             g_fault_count);

  g_fault_count = 0;
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET, 0xdeadbeefu);
  EXPECT_RTL(g_fault_count == 1u,
             "Writing RD_FIFO (ErrOnWrite=1) must raise Store Access Fault "
             "(faults=%u)",
             g_fault_count);

  g_fault_count = 0;
  (void)abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  EXPECT_RTL(g_fault_count == 1u,
             "Reading empty RD_FIFO with no active SW read op (rd_no_op_d=1) "
             "must raise Load Access Fault (faults=%u)",
             g_fault_count);

  // 20. FLASH_CTRL_CORE_PERMIT, PROG_FIFO .ByteAccess(0), and
  //     FLASH_CTRL_PRIM_PERMIT sub-word write (wr_err) verification.
  //     In flash_ctrl_core_reg_top.sv and flash_ctrl_prim_reg_top.sv:
  //     wr_err = reg_we & |(PERMIT[i] & ~reg_be).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET, 0xa55a1234u);
  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET, 0xffu);
  EXPECT_RTL(
      g_fault_count == 1u,
      "8-bit write to SCRATCH (PERMIT=0xf) must raise Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET) ==
                 0xa55a1234u,
             "Rejected 8-bit write must not modify SCRATCH");

  g_fault_count = 0;
  *(volatile uint16_t *)(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET + 2u) =
      0xffffu;
  EXPECT_RTL(
      g_fault_count == 1u,
      "16-bit write to SCRATCH+2 (PERMIT=0xf) must raise Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET) ==
                 0xa55a1234u,
             "Rejected 16-bit write must not modify SCRATCH");

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET,
                   0x00001203u);
  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET, 0x1fu);
  EXPECT_RTL(
      g_fault_count == 1u,
      "8-bit write to FIFO_LVL (PERMIT=0x3) must raise Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET) ==
                 0x00001203u,
             "Rejected 8-bit write must not modify FIFO_LVL");

  g_fault_count = 0;
  *(volatile uint16_t *)(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET) =
      0x0506u;
  EXPECT_RTL(
      g_fault_count == 0u,
      "16-bit write to FIFO_LVL+0 (be=0x3 covers PERMIT=0x3) must succeed");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET) ==
                 0x00000506u,
             "16-bit write to FIFO_LVL+0 must update FIFO_LVL to 0x0506");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET,
                   fifo_lvl_orig);

  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_INTR_ENABLE_REG_OFFSET, 0x15u);
  EXPECT_RTL(
      g_fault_count == 0u,
      "8-bit write to INTR_ENABLE+0 (be=0x1 covers PERMIT=0x1) must succeed");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                             FLASH_CTRL_INTR_ENABLE_REG_OFFSET) == 0x15u,
             "8-bit write to INTR_ENABLE+0 must update INTR_ENABLE to 0x15");

  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_INTR_ENABLE_REG_OFFSET + 1u,
                  0x1fu);
  EXPECT_RTL(g_fault_count == 1u,
             "8-bit write to INTR_ENABLE+1 (be=0x2 != PERMIT=0x1) must raise "
             "Store Access Fault");
  EXPECT_RTL(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_INTR_ENABLE_REG_OFFSET) ==
          0x15u,
      "Rejected 8-bit write to INTR_ENABLE+1 must not modify INTR_ENABLE");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INTR_ENABLE_REG_OFFSET, 0u);

  // PROG_FIFO (0x1b0) uses tlul_adapter_sram with .ByteAccess(0) -> sub-word
  // writes must raise Store Access Fault.
  g_fault_count = 0;
  abs_mmio_write8(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET, 0xaau);
  EXPECT_RTL(g_fault_count == 1u,
             "8-bit write to PROG_FIFO (.ByteAccess(0)) must raise Store "
             "Access Fault");

  g_fault_count = 0;
  *(volatile uint16_t *)(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET) =
      0x55aau;
  EXPECT_RTL(g_fault_count == 1u,
             "16-bit write to PROG_FIFO (.ByteAccess(0)) must raise Store "
             "Access Fault");

  // FLASH_CTRL_PRIM_PERMIT verification on CSR1 (0x04, PERMIT=0x3) and
  // CSR2 (0x08, PERMIT=0x1, RW bits 0x88 ungated by CSR0_REGWEN).
  uint32_t csr1_orig = abs_mmio_read32(kFlashPrimBase + 0x04u);
  g_fault_count = 0;
  abs_mmio_write8(kFlashPrimBase + 0x04u, 0xffu);
  EXPECT_RTL(
      g_fault_count == 1u,
      "8-bit write to PRIM CSR1 (PERMIT=0x3) must raise Store Access Fault");

  g_fault_count = 0;
  *(volatile uint16_t *)(kFlashPrimBase + 0x04u) = (uint16_t)csr1_orig;
  EXPECT_RTL(
      g_fault_count == 0u,
      "16-bit write to PRIM CSR1+0 (be=0x3 covers PERMIT=0x3) must succeed");

  g_fault_count = 0;
  abs_mmio_write8(kFlashPrimBase + 0x08u, 0x88u);
  EXPECT_RTL(
      g_fault_count == 0u,
      "8-bit write to PRIM CSR2+0 (be=0x1 covers PERMIT=0x1) must succeed");
  EXPECT_RTL(abs_mmio_read32(kFlashPrimBase + 0x08u) == 0x88u,
             "8-bit write to PRIM CSR2+0 must update ungated RW bits to 0x88");

  g_fault_count = 0;
  abs_mmio_write8(kFlashPrimBase + 0x08u + 1u, 0x00u);
  EXPECT_RTL(g_fault_count == 1u,
             "8-bit write to PRIM CSR2+1 (be=0x2 != PERMIT=0x1) must raise "
             "Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(kFlashPrimBase + 0x08u) == 0x88u,
             "Rejected 8-bit write to PRIM CSR2+1 must not modify PRIM CSR2");
  abs_mmio_write32(kFlashPrimBase + 0x08u, 0x00u);

  // 21. Wave 7: Direct CPU writes (sw/sb) to the eFlash memory window
  // (u_tl_adapter_eflash with ErrOnWrite = 1 in flash_ctrl.sv:1302-1315) must
  // raise a Store Access Fault (mcause = 7) and leave eFlash contents
  // unchanged.
  uint32_t eflash_test_addr = (uint32_t)((uintptr_t)&test_main) & ~0x3u;
  uint32_t eflash_orig_word = abs_mmio_read32(eflash_test_addr);
  g_fault_count = 0;
  abs_mmio_write32(eflash_test_addr, eflash_orig_word ^ 0xffffffffu);
  EXPECT_RTL(g_fault_count == 1u,
             "32-bit direct write to eFlash window (ErrOnWrite=1) must raise "
             "Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(eflash_test_addr) == eflash_orig_word,
             "Rejected 32-bit direct write to eFlash window must not modify "
             "eFlash data");

  g_fault_count = 0;
  abs_mmio_write8(eflash_test_addr, (uint8_t)(eflash_orig_word ^ 0xffu));
  EXPECT_RTL(g_fault_count == 1u,
             "8-bit direct write to eFlash window (ErrOnWrite=1) must raise "
             "Store Access Fault");
  EXPECT_RTL(abs_mmio_read32(eflash_test_addr) == eflash_orig_word,
             "Rejected 8-bit direct write to eFlash window must not modify "
             "eFlash data");

  return all_ok;
}
