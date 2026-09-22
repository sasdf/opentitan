// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
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

void ottf_load_integrity_error_handler(uint32_t *exc_info) { (void)exc_info; }

enum {
  kFlashCoreBase = TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR,
  kFlashPrimBase = TOP_EARLGREY_FLASH_CTRL_PRIM_BASE_ADDR,
  kFlashMemBase = TOP_EARLGREY_EFLASH_BASE_ADDR,
};

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

static void write_mp_bank_cfg_shadowed(uint32_t val) {
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
                   val);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_BANK_CFG_SHADOWED_REG_OFFSET,
                   val);
}

static void verify_info_page_erase_prog_read(uint32_t byte_addr,
                                             uint32_t info_sel, uint32_t word0,
                                             uint32_t word1) {
  // 1. Page Erase on INFO partition (PARTITION_SEL = 1, INFO_SEL = info_sel).
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, byte_addr);
  uint32_t erase_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  erase_ctrl = bitfield_field32_write(erase_ctrl, FLASH_CTRL_CONTROL_OP_FIELD,
                                      FLASH_CTRL_CONTROL_OP_VALUE_ERASE);
  erase_ctrl = bitfield_bit32_write(erase_ctrl,
                                    FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true);
  erase_ctrl = bitfield_field32_write(
      erase_ctrl, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, info_sel);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, erase_ctrl);
  uint32_t op_status = wait_op_done();
  CHECK(op_status == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "INFO%u @ 0x%x erase failed: op_status=0x%x", info_sel, byte_addr,
        op_status);

  // 2. Program 2 words (NUM = 1) into INFO partition.
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, byte_addr);
  uint32_t prog_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  prog_ctrl = bitfield_field32_write(prog_ctrl, FLASH_CTRL_CONTROL_OP_FIELD,
                                     FLASH_CTRL_CONTROL_OP_VALUE_PROG);
  prog_ctrl = bitfield_bit32_write(prog_ctrl,
                                   FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true);
  prog_ctrl = bitfield_field32_write(
      prog_ctrl, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, info_sel);
  prog_ctrl =
      bitfield_field32_write(prog_ctrl, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, prog_ctrl);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET, word0);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET, word1);
  op_status = wait_op_done();
  CHECK(op_status == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "INFO%u @ 0x%x prog failed: op_status=0x%x", info_sel, byte_addr,
        op_status);

  // 3. Read 2 words (NUM = 1) from INFO partition and verify data.
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, byte_addr);
  uint32_t rd_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  rd_ctrl = bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_OP_FIELD,
                                   FLASH_CTRL_CONTROL_OP_VALUE_READ);
  rd_ctrl =
      bitfield_bit32_write(rd_ctrl, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true);
  rd_ctrl = bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_INFO_SEL_FIELD,
                                   info_sel);
  rd_ctrl = bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, rd_ctrl);
  op_status = wait_op_done();
  CHECK(op_status == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "INFO%u @ 0x%x read failed: op_status=0x%x", info_sel, byte_addr,
        op_status);

  uint32_t r0 = abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  uint32_t r1 = abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET);
  CHECK(r0 == word0, "INFO%u @ 0x%x word0 mismatch: got 0x%x, expected 0x%x",
        info_sel, byte_addr, r0, word0);
  CHECK(r1 == word1, "INFO%u @ 0x%x word1 mismatch: got 0x%x, expected 0x%x",
        info_sel, byte_addr, r1, word1);
  clear_flash_status();
}

bool test_main(void) {
  mmio_region_t core_region = mmio_region_from_addr(kFlashCoreBase);

  // ---------------------------------------------------------------------------
  // 1. Sub-word (8-bit and 16-bit) MMIO reads and auxiliary registers
  //    (HW_INFO_CFG_OVERRIDE, ECC_SINGLE_ERR_CNT, PHY_ALERT_CFG).
  // ---------------------------------------------------------------------------
  uint32_t status32 =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  uint8_t status_b0 =
      mmio_region_read8(core_region, FLASH_CTRL_STATUS_REG_OFFSET);
  uint16_t status_h0 = *(
      volatile const uint16_t *)(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  CHECK(status_b0 == (uint8_t)(status32 & 0xffu),
        "8-bit STATUS read mismatch: got 0x%x, expected 0x%x", status_b0,
        status32 & 0xffu);
  CHECK(status_h0 == (uint16_t)(status32 & 0xffffu),
        "16-bit STATUS read mismatch: got 0x%x, expected 0x%x", status_h0,
        status32 & 0xffffu);

  uint32_t orig_hw_override = abs_mmio_read32(
      kFlashCoreBase + FLASH_CTRL_HW_INFO_CFG_OVERRIDE_REG_OFFSET);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_HW_INFO_CFG_OVERRIDE_REG_OFFSET,
                   0x66u);
  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_HW_INFO_CFG_OVERRIDE_REG_OFFSET) == 0x66u,
        "HW_INFO_CFG_OVERRIDE readback mismatch");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_HW_INFO_CFG_OVERRIDE_REG_OFFSET,
                   orig_hw_override);

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ECC_SINGLE_ERR_CNT_REG_OFFSET,
                   0xa55au);
  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_ECC_SINGLE_ERR_CNT_REG_OFFSET) == 0xa55au,
        "ECC_SINGLE_ERR_CNT readback mismatch");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ECC_SINGLE_ERR_CNT_REG_OFFSET,
                   0u);

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PHY_ALERT_CFG_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PHY_ALERT_CFG_REG_OFFSET) ==
            0x1u,
        "PHY_ALERT_CFG readback mismatch");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PHY_ALERT_CFG_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 2. Configure and exercise INFO1 (INFO_SEL = 1) and INFO2 (INFO_SEL = 2)
  //    pages on both Bank 0 and Bank 1.
  // ---------------------------------------------------------------------------
  const uint32_t kInfoCfgAllowUnscrambled =
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_RD_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_PROG_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ERASE_EN_7_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_SCRAMBLE_EN_7_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ECC_EN_7_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_HE_EN_7_FIELD,
                             kMultiBitBool4False);

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK0_INFO1_PAGE_CFG_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);
  abs_mmio_write32(
      kFlashCoreBase + FLASH_CTRL_BANK0_INFO2_PAGE_CFG_0_REG_OFFSET,
      kInfoCfgAllowUnscrambled);
  abs_mmio_write32(
      kFlashCoreBase + FLASH_CTRL_BANK0_INFO2_PAGE_CFG_1_REG_OFFSET,
      kInfoCfgAllowUnscrambled);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK1_INFO1_PAGE_CFG_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);
  abs_mmio_write32(
      kFlashCoreBase + FLASH_CTRL_BANK1_INFO2_PAGE_CFG_0_REG_OFFSET,
      kInfoCfgAllowUnscrambled);
  abs_mmio_write32(
      kFlashCoreBase + FLASH_CTRL_BANK1_INFO2_PAGE_CFG_1_REG_OFFSET,
      kInfoCfgAllowUnscrambled);

  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_BANK0_INFO1_PAGE_CFG_REG_OFFSET) ==
            kInfoCfgAllowUnscrambled,
        "BANK0_INFO1_PAGE_CFG mismatch");
  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_BANK0_INFO2_PAGE_CFG_0_REG_OFFSET) ==
            kInfoCfgAllowUnscrambled,
        "BANK0_INFO2_PAGE_CFG_0 mismatch");
  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_BANK1_INFO1_PAGE_CFG_REG_OFFSET) ==
            kInfoCfgAllowUnscrambled,
        "BANK1_INFO1_PAGE_CFG mismatch");
  CHECK(abs_mmio_read32(kFlashCoreBase +
                        FLASH_CTRL_BANK1_INFO2_PAGE_CFG_0_REG_OFFSET) ==
            kInfoCfgAllowUnscrambled,
        "BANK1_INFO2_PAGE_CFG_0 mismatch");

  verify_info_page_erase_prog_read(0x0u, /*info_sel=*/1u, 0x11223344u,
                                   0x55667788u);
  verify_info_page_erase_prog_read(0x0u, /*info_sel=*/2u, 0x99aabbccu,
                                   0xddeeff00u);
  verify_info_page_erase_prog_read(0x80000u, /*info_sel=*/1u, 0xdeadbeefu,
                                   0xcafebabeu);
  verify_info_page_erase_prog_read(0x80000u, /*info_sel=*/2u, 0x01234567u,
                                   0x89abcdefu);

  // ---------------------------------------------------------------------------
  // 3. CURR_FIFO_LVL & FIFO_RST: Read 2 words into RD_FIFO without draining,
  //    verify CURR_FIFO_LVL.RD == 2, then assert FIFO_RST = 1 and verify
  //    CURR_FIFO_LVL drops to 0.
  // ---------------------------------------------------------------------------
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x0u);
  uint32_t rd_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  rd_ctrl = bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_OP_FIELD,
                                   FLASH_CTRL_CONTROL_OP_VALUE_READ);
  rd_ctrl =
      bitfield_bit32_write(rd_ctrl, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT, true);
  rd_ctrl =
      bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, 1u);
  rd_ctrl = bitfield_field32_write(rd_ctrl, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "RD_FIFO fill read failed");

  uint32_t curr_lvl =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CURR_FIFO_LVL_REG_OFFSET);
  CHECK(
      bitfield_field32_read(curr_lvl, FLASH_CTRL_CURR_FIFO_LVL_RD_FIELD) == 2u,
      "Expected CURR_FIFO_LVL.RD == 2 before FIFO_RST, got 0x%x", curr_lvl);

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 1u);
  curr_lvl =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CURR_FIFO_LVL_REG_OFFSET);
  CHECK(curr_lvl == 0u,
        "Expected CURR_FIFO_LVL == 0 while FIFO_RST == 1, got 0x%x", curr_lvl);

  // While FIFO_RST == 1, execute an OP_READ and verify it completes with DONE
  // while RD_FIFO remains empty (CURR_FIFO_LVL.RD == 0, STATUS.RD_EMPTY == 1).
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "OP_READ while FIFO_RST == 1 failed");
  curr_lvl =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CURR_FIFO_LVL_REG_OFFSET);
  CHECK(
      curr_lvl == 0u,
      "Expected CURR_FIFO_LVL == 0 after OP_READ with FIFO_RST == 1, got 0x%x",
      curr_lvl);
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET),
            FLASH_CTRL_STATUS_RD_EMPTY_BIT),
        "Expected STATUS.RD_EMPTY == 1 after OP_READ with FIFO_RST == 1");

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 0u);
  clear_flash_status();

  // ---------------------------------------------------------------------------
  // 4. Bank 1 Bank Erase (ERASE_SEL = 1):
  //    (a) With MP_BANK_CFG_SHADOWED == 0, verify bank erase fails with MP_ERR.
  //    (b) With MP_BANK_CFG_SHADOWED == 0x2 (ERASE_EN_1 = 1), initiate Bank 1
  //        data bank erase, verify CTRL_REGWEN == 0 locks CONTROL/ADDR/
  //        PROG_TYPE_EN while active, and exercise ERASE_SUSPEND = 1.
  // ---------------------------------------------------------------------------
  write_mp_bank_cfg_shadowed(0u);
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t bk_erase_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  bk_erase_ctrl =
      bitfield_field32_write(bk_erase_ctrl, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_ERASE);
  bk_erase_ctrl = bitfield_bit32_write(bk_erase_ctrl,
                                       FLASH_CTRL_CONTROL_ERASE_SEL_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   bk_erase_ctrl);
  uint32_t bk_status = wait_op_done();
  CHECK(bk_status == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                      (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)),
        "Disabled Bank 1 erase must fail with DONE|ERR, got 0x%x", bk_status);
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET),
            FLASH_CTRL_ERR_CODE_MP_ERR_BIT),
        "Disabled Bank 1 erase must set ERR_CODE.MP_ERR");
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // Enable Bank 1 bank erase and start Bank 1 data bank erase (takes ~1ms).
  write_mp_bank_cfg_shadowed(1u
                             << FLASH_CTRL_MP_BANK_CFG_SHADOWED_ERASE_EN_1_BIT);
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   bk_erase_ctrl);

  // Immediately verify CTRL_REGWEN == 0 protects ADDR, PROG_TYPE_EN, CONTROL.
  CHECK(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET) == 0u,
      "Expected CTRL_REGWEN == 0 during ongoing Bank 1 erase");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x1234u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, 0u);
  CHECK(
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET) == 0x80000u,
      "ADDR must remain 0x80000 while CTRL_REGWEN == 0");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PROG_TYPE_EN_REG_OFFSET) ==
            0x3u,
        "PROG_TYPE_EN must remain 0x3 while CTRL_REGWEN == 0");

  // Request ERASE_SUSPEND = 1 and wait for completion.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERASE_SUSPEND_REG_OFFSET, 1u);
  bk_status = wait_op_done();
  CHECK(bk_status == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Bank 1 erase with suspend failed: op_status=0x%x", bk_status);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERASE_SUSPEND_REG_OFFSET) ==
            0u,
        "ERASE_SUSPEND must auto-clear to 0 once erase completes/suspends");
  clear_flash_status();

  // (c) Info-Selected Bank Erase (ERASE_SEL = 1, PARTITION_SEL = 1):
  //     Configure MP_REGION_7 for Bank 1 page 0 (base = 256, size = 1),
  //     program non-0xFFFFFFFF words into both Bank 1 Data (0x80000) and
  //     Bank 1 INFO1 (0x80000), set BANK1_INFO1_PAGE_CFG.ERASE_EN = False,
  //     and execute Bank Erase with PARTITION_SEL = 1. Verify BOTH Info and
  //     Data partitions of Bank 1 are erased to 0xFFFFFFFF.
  uint32_t mp_reg7 =
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_7_BASE_7_FIELD, 256u) |
      bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_7_SIZE_7_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_7_REG_OFFSET, mp_reg7);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_7_REG_OFFSET, mp_reg7);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);

  // Program 2 words into Bank 1 Data page 0 (0x80000, PARTITION_SEL = 0).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t data_prog_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_PROG) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   data_prog_ctrl);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0xa5a55a5au);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0x5a5aa5a5u);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Bank 1 Data prog failed");
  clear_flash_status();

  // Program 2 words into Bank 1 INFO1 page 0 (0x80000, PARTITION_SEL = 1).
  verify_info_page_erase_prog_read(0x80000u, /*info_sel=*/1u, 0x13572468u,
                                   0x24681357u);

  // Disable ERASE_EN on BANK1_INFO1_PAGE_CFG to verify bank erase ignores
  // per-page ERASE_EN when MP_BANK_CFG_SHADOWED.ERASE_EN_1 == 1.
  uint32_t info_cfg_no_erase = bitfield_field32_write(
      kInfoCfgAllowUnscrambled, FLASH_CTRL_MP_REGION_CFG_7_ERASE_EN_7_FIELD,
      kMultiBitBool4False);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_BANK1_INFO1_PAGE_CFG_REG_OFFSET,
                   info_cfg_no_erase);

  // Execute Info-Selected Bank Erase on Bank 1 (ERASE_SEL = 1, PARTITION_SEL =
  // 1).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t info_bk_erase_ctrl =
      bitfield_bit32_write(bk_erase_ctrl, FLASH_CTRL_CONTROL_PARTITION_SEL_BIT,
                           true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_INFO_SEL_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   info_bk_erase_ctrl);
  bk_status = wait_op_done();
  CHECK(bk_status == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Bank 1 Info-selected bank erase failed: op_status=0x%x", bk_status);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET) == 0u,
        "Expected ERR_CODE == 0 after Info-selected bank erase");
  clear_flash_status();

  // Verify Bank 1 INFO1 (0x80000) was erased to 0xFFFFFFFF.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Bank 1 INFO1 read after bank erase failed");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Bank 1 INFO1 word0 not erased by Info-selected bank erase");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Bank 1 INFO1 word1 not erased by Info-selected bank erase");
  clear_flash_status();

  // Verify Bank 1 Data (0x80000) was ALSO erased to 0xFFFFFFFF.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t data_rd_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   data_rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Bank 1 Data read after Info-selected bank erase failed");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Bank 1 Data word0 not erased by Info-selected bank erase");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Bank 1 Data word1 not erased by Info-selected bank erase");
  clear_flash_status();
  write_mp_bank_cfg_shadowed(0u);

  // ---------------------------------------------------------------------------
  // 5. FLASH_CTRL_PRIM CSRs (CSR1..CSR19 masking, 16-bit read, and CSR0_REGWEN
  //    RW0C locking).
  // ---------------------------------------------------------------------------
  CHECK(abs_mmio_read32(kFlashPrimBase + 0x00u) == 1u,
        "Expected CSR0_REGWEN == 1 initially");

  struct {
    uint32_t offset;
    uint32_t expected_mask;
  } const kPrimCsrs[] = {
      {0x04u, 0x1fffu},      // CSR1
      {0x0cu, 0xfffffffu},   // CSR3
      {0x10u, 0xfffu},       // CSR4
      {0x14u, 0x7fffffu},    // CSR5
      {0x18u, 0x1ffffffu},   // CSR6
      {0x1cu, 0x1ffffu},     // CSR7
      {0x20u, 0xffffffffu},  // CSR8
      {0x24u, 0xffffffffu},  // CSR9
      {0x28u, 0xffffffffu},  // CSR10
      {0x2cu, 0xffffffffu},  // CSR11
      {0x30u, 0x3ffu},       // CSR12
      {0x34u, 0x1fffffu},    // CSR13
      {0x38u, 0x1ffu},       // CSR14
      {0x3cu, 0x1ffu},       // CSR15
      {0x40u, 0x1ffu},       // CSR16
      {0x44u, 0x1ffu},       // CSR17
      {0x48u, 0x1u},         // CSR18
      {0x4cu, 0x1u},         // CSR19
  };

  for (size_t i = 0; i < ARRAYSIZE(kPrimCsrs); ++i) {
    abs_mmio_write32(kFlashPrimBase + kPrimCsrs[i].offset, 0xffffffffu);
    uint32_t got = abs_mmio_read32(kFlashPrimBase + kPrimCsrs[i].offset);
    CHECK(got == kPrimCsrs[i].expected_mask,
          "FLASH_CTRL_PRIM CSR @ 0x%x mismatch: got 0x%x, expected 0x%x",
          kPrimCsrs[i].offset, got, kPrimCsrs[i].expected_mask);
  }

  uint16_t csr3_h0 = *(volatile const uint16_t *)(kFlashPrimBase + 0x0cu);
  CHECK(csr3_h0 == 0xffffu, "16-bit CSR3 read mismatch: got 0x%x", csr3_h0);

  // Clear CSR0_REGWEN (RW0C) and verify CSR3..CSR19 ignore subsequent writes.
  abs_mmio_write32(kFlashPrimBase + 0x00u, 0u);
  CHECK(abs_mmio_read32(kFlashPrimBase + 0x00u) == 0u,
        "Expected CSR0_REGWEN == 0 after RW0C clear");
  for (size_t i = 1; i < ARRAYSIZE(kPrimCsrs); ++i) {
    abs_mmio_write32(kFlashPrimBase + kPrimCsrs[i].offset, 0u);
    uint32_t got = abs_mmio_read32(kFlashPrimBase + kPrimCsrs[i].offset);
    CHECK(got == kPrimCsrs[i].expected_mask,
          "Locked FLASH_CTRL_PRIM CSR @ 0x%x modified when CSR0_REGWEN == 0",
          kPrimCsrs[i].offset);
  }

  // ---------------------------------------------------------------------------
  // 6. Double-Programmed Data Word ECC Fault (`OtFlashEccFault`):
  //    - Idle PROG_FIFO write + FIFO_RST recovery (Chunk 39)
  //    - Double-programming 2x 64-bit words at 0x80000..0x8000f with
  //    ECC_EN=True
  //      and SCRAMBLE_EN=False (`0x55555555` -> `0x55555554`), exercising
  //      `ot_flash_add_ecc_fault` deduplication on word 1/3 (Chunk 14)
  //    - Direct CPU store to `0x20080000`
  //    (`ot_flash_ecc_fault_write_with_attrs`,
  //      Chunk 13)
  //    - Direct CPU load from `0x20080000` with `ECC_EN = False`
  //      (`ot_flash_ecc_fault_read_with_attrs` bypass, Chunk 12)
  //    - Controller `OP_READ` from `0x80008` with `ECC_EN = True`
  //      (`ot_flash_is_ecc_corrupted` Chunk 11 + `ot_flash_op_read`
  //      `PHY_RELBL_ERR` & `RD_ERR` Chunk 24)
  //    - `FAULT_STATUS` write with non-`rw0c` bits (`0xffffffff`, Chunk 41)
  //      followed by `rw0c` clear (`0x0`)
  //    - Page Erase at `0x80000` (`ot_flash_clear_ecc_faults_range`, Chunk 15)
  // ---------------------------------------------------------------------------
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));

  // (a) Write PROG_FIFO while no software OP_PROG is active, then reset FIFOs.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 1u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_RST_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CURR_FIFO_LVL_REG_OFFSET) ==
            0u,
        "Expected CURR_FIFO_LVL == 0 after FIFO_RST");

  // (b) Configure MP_REGION_7 (Bank 1 Page 0: base=256, size=1) with
  //     ECC_EN = True, SCRAMBLE_EN = False, RD_EN = True, PROG_EN = True,
  //     ERASE_EN = True.
  const uint32_t kDataCfgEccNoScramble = bitfield_field32_write(
      kInfoCfgAllowUnscrambled, FLASH_CTRL_MP_REGION_CFG_7_ECC_EN_7_FIELD,
      kMultiBitBool4True);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kDataCfgEccNoScramble);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kDataCfgEccNoScramble);

  // First program 4 words (NUM = 3) at 0x80000..0x8000f with 0x55555555.
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t prog4_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_PROG) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_NUM_FIELD, 3u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, prog4_ctrl);
  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                     0x55555555u);
  }
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "First 4-word program at 0x80000 failed");

  // Second program 4 words (NUM = 3) at 0x80000..0x8000f with 0x55555554
  // without erasing -> creates reliability ECC faults at 0x80000 and 0x80008.
  clear_flash_status();
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, prog4_ctrl);
  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET,
                     0x55555554u);
  }
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Second 4-word double-program at 0x80000 failed");

  // (c) Direct CPU store to the faulted 8-byte window (0x20080000) must raise
  //     Store Access Fault (`ot_flash_ecc_fault_write_with_attrs`).
  g_fault_count = 0;
  abs_mmio_write32(kFlashMemBase + 0x80000u, 0xdeadbeefu);
  CHECK(g_fault_count == 1u,
        "Direct CPU store to ECC-faulted eFlash word must raise Store Access "
        "Fault");

  // (d) Temporarily disable ECC_EN on MP_REGION_7 and verify direct CPU read
  //     from 0x20080000 succeeds (`ot_flash_ecc_fault_read_with_attrs` bypass)
  //     and returns the raw bitwise-ANDed word 0x55555554.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kInfoCfgAllowUnscrambled);
  g_fault_count = 0;
  uint32_t raw_word = abs_mmio_read32(kFlashMemBase + 0x80000u);
  CHECK(g_fault_count == 0u,
        "Direct CPU read with ECC_EN=False must not raise Load Access Fault");
  CHECK(raw_word == 0x55555554u,
        "Direct CPU read with ECC_EN=False mismatch: got 0x%x, expected "
        "0x55555554",
        raw_word);

  // (e) Re-enable ECC_EN = True on MP_REGION_7 and execute controller OP_READ
  //     at 0x80008 (NUM = 1). Verify OP_STATUS == DONE|ERR, ERR_CODE.RD_ERR,
  //     FAULT_STATUS.PHY_RELBL_ERR, ERR_ADDR == 0x80008, and RD_FIFO ==
  //     0xffffffff.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kDataCfgEccNoScramble);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                   kDataCfgEccNoScramble);
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80008u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   data_rd_ctrl);
  uint32_t ecc_rd_status = wait_op_done();
  CHECK(ecc_rd_status == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                          (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)),
        "OP_READ on ECC-corrupted word must complete with DONE|ERR, got 0x%x",
        ecc_rd_status);
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET),
            FLASH_CTRL_ERR_CODE_RD_ERR_BIT),
        "OP_READ on ECC-corrupted word must set ERR_CODE.RD_ERR");
  CHECK(bitfield_bit32_read(abs_mmio_read32(kFlashCoreBase +
                                            FLASH_CTRL_FAULT_STATUS_REG_OFFSET),
                            FLASH_CTRL_FAULT_STATUS_PHY_RELBL_ERR_BIT),
        "OP_READ on ECC-corrupted word must set FAULT_STATUS.PHY_RELBL_ERR");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_ADDR_REG_OFFSET) ==
            0x80008u,
        "OP_READ on ECC-corrupted word at 0x80008 must latch ERR_ADDR 0x80008");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "OP_READ on ECC-corrupted word must return 0xffffffff in RD_FIFO");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0x55555554u,
        "OP_READ second word in StErr after RD_ERR must return latched "
        "flash_data_i (0x55555554)");

  // (f) Write 0xffffffff to FAULT_STATUS (exercising non-rw0c bit guard and
  //     verifying PHY_RELBL_ERR remains 1), then write 0x0 to clear
  //     PHY_RELBL_ERR.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FAULT_STATUS_REG_OFFSET,
                   0xffffffffu);
  CHECK(bitfield_bit32_read(abs_mmio_read32(kFlashCoreBase +
                                            FLASH_CTRL_FAULT_STATUS_REG_OFFSET),
                            FLASH_CTRL_FAULT_STATUS_PHY_RELBL_ERR_BIT),
        "Writing 1s to RW0C FAULT_STATUS must not clear PHY_RELBL_ERR");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FAULT_STATUS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FAULT_STATUS_REG_OFFSET) ==
            0u,
        "Writing 0 to RW0C FAULT_STATUS must clear PHY_RELBL_ERR");
  clear_flash_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  // (g) Erase Bank 1 Page 0 (0x80000) to clear ECC faults
  //     (`ot_flash_clear_ecc_faults_range`) and verify OP_READ at 0x80008
  //     succeeds with 0xffffffff and ERR_CODE == 0.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80000u);
  uint32_t data_pg_erase_ctrl =
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD,
                             FLASH_CTRL_CONTROL_OP_VALUE_ERASE);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   data_pg_erase_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "Page erase of 0x80000 after ECC fault failed");
  clear_flash_status();

  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ADDR_REG_OFFSET, 0x80008u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET,
                   data_rd_ctrl);
  CHECK(wait_op_done() == (1u << FLASH_CTRL_OP_STATUS_DONE_BIT),
        "OP_READ at 0x80008 after page erase failed");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET) == 0u,
        "ERR_CODE must be 0 on OP_READ after page erase cleared ECC fault");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Word 0 at 0x80008 must be 0xffffffff after page erase");
  CHECK(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_RD_FIFO_REG_OFFSET) ==
            0xffffffffu,
        "Word 1 at 0x80008 must be 0xffffffff after page erase");
  clear_flash_status();

  return true;
}
