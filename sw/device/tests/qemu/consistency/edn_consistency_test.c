// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_edn.h"
#include "sw/device/lib/dif/dif_entropy_src.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "csrng_regs.h"
#include "edn_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kEdnStateIdle = 0x0c1u,
  kEdnStateSwPortMode = 0x095u,
  kEdnStateRejectCsrngEntropy = 0x018u,
  kEdnStateError = 0x17eu,
};

static uint32_t make_edn_ctrl(multi_bit_bool_t edn_en,
                              multi_bit_bool_t boot_req,
                              multi_bit_bool_t auto_req,
                              multi_bit_bool_t fifo_rst) {
  return ((uint32_t)(edn_en & 0xfu) << EDN_CTRL_EDN_ENABLE_OFFSET) |
         ((uint32_t)(boot_req & 0xfu) << EDN_CTRL_BOOT_REQ_MODE_OFFSET) |
         ((uint32_t)(auto_req & 0xfu) << EDN_CTRL_AUTO_REQ_MODE_OFFSET) |
         ((uint32_t)(fifo_rst & 0xfu) << EDN_CTRL_CMD_FIFO_RST_OFFSET);
}

static void test_reset_and_mubi_alerts(void) {
  LOG_INFO("1. Testing EDN0 reset defaults and MuBi4 recoverable alerts...");

  // Stop all entropy complex blocks so EDN0 is in its disabled/idle state.
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  CHECK(abs_mmio_read32(kEdn0Base + EDN_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET) == 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET) == 0x901u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET) == 0xfff003u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateIdle);

  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0u);

  // 1a. Invalid EDN_ENABLE (0x5) -> bit 0 + recov_alert
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl((multi_bit_bool_t)0x5, kMultiBitBool4False,
                                 kMultiBitBool4False, kMultiBitBool4False));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        (1u << EDN_RECOV_ALERT_STS_EDN_ENABLE_FIELD_ALERT_BIT));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 1b. Invalid BOOT_REQ_MODE (0x3) -> bit 1 + recov_alert
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4False, (multi_bit_bool_t)0x3,
                                 kMultiBitBool4False, kMultiBitBool4False));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        ((1u << EDN_RECOV_ALERT_STS_EDN_ENABLE_FIELD_ALERT_BIT) |
         (1u << EDN_RECOV_ALERT_STS_BOOT_REQ_MODE_FIELD_ALERT_BIT)));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 1c. Invalid AUTO_REQ_MODE (0xc) -> bit 2 + recov_alert
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4False, kMultiBitBool4False,
                                 (multi_bit_bool_t)0xc, kMultiBitBool4False));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        ((1u << EDN_RECOV_ALERT_STS_EDN_ENABLE_FIELD_ALERT_BIT) |
         (1u << EDN_RECOV_ALERT_STS_BOOT_REQ_MODE_FIELD_ALERT_BIT) |
         (1u << EDN_RECOV_ALERT_STS_AUTO_REQ_MODE_FIELD_ALERT_BIT)));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 1d. Invalid CMD_FIFO_RST (0x7) -> bit 3 + recov_alert
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4False, kMultiBitBool4False,
                                 kMultiBitBool4False, (multi_bit_bool_t)0x7));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0xfu);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // Clear RECOV_ALERT_STS (RW0C)
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0u);
}

static void test_sw_port_mode_and_csrng_ack_err(void) {
  LOG_INFO("2. Testing EDN0 SWPortMode, SW_CMD_STS, and CSRNG_ACK_ERR...");

  // Enable entropy_src and csrng so CSRNG can process EDN0 commands.
  dif_entropy_src_t entropy_src;
  CHECK_DIF_OK(dif_entropy_src_init(
      mmio_region_from_addr(TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR), &entropy_src));
  CHECK_DIF_OK(dif_entropy_src_configure(
      &entropy_src, entropy_testutils_config_default(), kDifToggleEnabled));

  abs_mmio_write32(
      kCsrngBase + CSRNG_CTRL_REG_OFFSET,
      ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
          ((uint32_t)kMultiBitBool4False
           << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET));

  // Enable EDN0 in SWPortMode (EDN_ENABLE = True, others False).
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4True, kMultiBitBool4False,
                                 kMultiBitBool4False, kMultiBitBool4False));

  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateSwPortMode);
  uint32_t sw_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_REG_RDY_BIT) == 1);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_RDY_BIT) == 1);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_ACK_BIT) == 0);

  // Send INSTANTIATE with flag0 = kMultiBitBool4True (0x6), clen = 0 (0x601).
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR(bitfield_bit32_read(
                    abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET),
                    EDN_INTR_STATE_EDN_CMD_REQ_DONE_BIT),
                1000);
  sw_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_REG_RDY_BIT) == 1);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_RDY_BIT) == 1);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_ACK_BIT) == 1);
  CHECK(bitfield_field32_read(sw_sts, EDN_SW_CMD_STS_CMD_STS_FIELD) == 0u);
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);

  // Send duplicate INSTANTIATE -> CSRNG returns INVALID_CMD_SEQ (3).
  // Both CSRNG and EDN0 will raise recoverable alerts!
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));

  IBEX_SPIN_FOR(bitfield_bit32_read(
                    abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET),
                    EDN_INTR_STATE_EDN_CMD_REQ_DONE_BIT),
                1000);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateRejectCsrngEntropy);
  sw_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_REG_RDY_BIT) == 0);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_RDY_BIT) == 0);
  CHECK(bitfield_bit32_read(sw_sts, EDN_SW_CMD_STS_CMD_ACK_BIT) == 1);
  CHECK(bitfield_field32_read(sw_sts, EDN_SW_CMD_STS_CMD_STS_FIELD) == 3u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        (1u << EDN_RECOV_ALERT_STS_CSRNG_ACK_ERR_BIT));

  // Verify writing 0xffffffff to RECOV_ALERT_STS (RW0C) preserves
  // CSRNG_ACK_ERR!
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        (1u << EDN_RECOV_ALERT_STS_CSRNG_ACK_ERR_BIT));

  // Clear CSRNG_ACK_ERR and disable EDN0 + CSRNG.
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateIdle);
}

static void test_boot_and_auto_mode_hw_cmd_sts_and_max_reqs_cnt(void) {
  LOG_INFO(
      "2b. Testing EDN0 HW_CMD_STS BOOT_MODE/AUTO_MODE latching and "
      "max_reqs_cnt restore...");

  // Reset CSRNG state by disabling and re-enabling CSRNG.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(
      kCsrngBase + CSRNG_CTRL_REG_OFFSET,
      ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
          ((uint32_t)kMultiBitBool4False
           << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET));

  // Part A: Boot Mode failure -> HW_CMD_STS.BOOT_MODE must remain 1 in
  // RejectCsrngEntropy. Set BOOT_INS_CMD = 0x00000601 (flag0=True so CSRNG does
  // not block on entropy_src) and BOOT_GEN_CMD = 0x00000601 (duplicate
  // INSTANTIATE instead of GENERATE).
  abs_mmio_write32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET, 0x00000601u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET, 0x00000601u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                                 kMultiBitBool4False, kMultiBitBool4False));
  IBEX_SPIN_FOR(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
                    kEdnStateRejectCsrngEntropy,
                1000);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));

  uint32_t hw_sts = abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_BOOT_MODE_BIT) == 1);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_AUTO_MODE_BIT) == 0);
  CHECK(bitfield_field32_read(hw_sts, EDN_HW_CMD_STS_CMD_TYPE_FIELD) == 1u);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_CMD_ACK_BIT) == 1);
  CHECK(bitfield_field32_read(hw_sts, EDN_HW_CMD_STS_CMD_STS_FIELD) == 3u);

  // Restore BOOT_INS_CMD / BOOT_GEN_CMD and disable EDN0; verify HW_CMD_STS
  // clears to 0 on Idle.
  abs_mmio_write32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET, 0x901u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET, 0xfff003u);
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateIdle);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET) == 0u);

  // Part B: Auto Mode max_reqs_cnt restore on disable + HW_CMD_STS.AUTO_MODE
  // latch. Reset CSRNG state and configure MAX_NUM_REQS_BETWEEN_RESEEDS = 0
  // prior to disable, then disable EDN0 so main_sm_done_pulse restores
  // max_reqs_cnt = 0.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(
      kCsrngBase + CSRNG_CTRL_REG_OFFSET,
      ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
          ((uint32_t)kMultiBitBool4True << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
          ((uint32_t)kMultiBitBool4False
           << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET));

  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4True, kMultiBitBool4False,
                                 kMultiBitBool4True, kMultiBitBool4False));
  // Load RESEED_CMD with duplicate INSTANTIATE (0x00000601) so that when
  // AutoDispatch immediately runs AutoCaptReseedCnt -> AutoSendReseedCmd,
  // CSRNG rejects it with CMD_STS_INVALID_CMD_SEQ (3).
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000601u);
  abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x00001003u);

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEdn0RecovAlert));
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
                    kEdnStateRejectCsrngEntropy,
                1000);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEdn0RecovAlert));
  hw_sts = abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_BOOT_MODE_BIT) == 0);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_AUTO_MODE_BIT) == 1);
  CHECK(bitfield_field32_read(hw_sts, EDN_HW_CMD_STS_CMD_TYPE_FIELD) == 1u);
  CHECK(bitfield_bit32_read(hw_sts, EDN_HW_CMD_STS_CMD_ACK_BIT) == 1);
  CHECK(bitfield_field32_read(hw_sts, EDN_HW_CMD_STS_CMD_STS_FIELD) == 3u);

  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateIdle);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET) == 0u);
}

static void test_fifo_depth_and_err_code_test(void) {
  LOG_INFO(
      "3. Testing EDN0 13-word FIFO depth, overflow, and ERR_CODE_TEST...");

  // Disable external IRQs before triggering EDN0 fatal alert because
  // prim_alert_sender #(.IsFatal(1)) latches alert_set_q = 1 until hardware
  // reset, which would otherwise re-trigger ottf_alert_isr continuously.
  irq_external_ctrl(false);
  uint32_t edn0_recov_cause_reg =
      kAlertHandlerBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
      4u * (uint32_t)kTopEarlgreyAlertIdEdn0RecovAlert;
  uint32_t edn0_fatal_cause_reg =
      kAlertHandlerBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
      4u * (uint32_t)kTopEarlgreyAlertIdEdn0FatalAlert;
  abs_mmio_write32(edn0_recov_cause_reg, 1u);
  abs_mmio_write32(edn0_fatal_cause_reg, 1u);

  // Verify continuous RECOV_ALERT_STS MuBi4 field alert re-assertion across
  // rw0c while CTRL holds an invalid mubi4 value (edn_core.sv:473-475).
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4False, kMultiBitBool4False,
                                 kMultiBitBool4False, (multi_bit_bool_t)0x0));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        (1u << EDN_RECOV_ALERT_STS_CMD_FIFO_RST_FIELD_ALERT_BIT));
  // Writing 0 (rw0c) while CTRL still has an invalid mubi4 must keep bit 3 set!
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) ==
        (1u << EDN_RECOV_ALERT_STS_CMD_FIFO_RST_FIELD_ALERT_BIT));
  // Restoring CTRL to valid mubi4 (0x9999) allows rw0c to clear
  // RECOV_ALERT_STS.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0u);
  abs_mmio_write32(edn0_recov_cause_reg, 1u);

  // Enable EDN0 with CMD_FIFO_RST = kMultiBitBool4True (level reset active).
  // In edn_core.sv:702, 745 (sfifo_rescmd_clr / sfifo_gencmd_clr), while
  // CTRL.CMD_FIFO_RST == kMultiBitBool4True, both replay FIFOs are held in
  // synchronous clear, so writing >13 words to RESEED_CMD / GENERATE_CMD must
  // not fill or overflow either FIFO.
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4True, kMultiBitBool4False,
                                 kMultiBitBool4False, kMultiBitBool4True));
  for (uint32_t i = 0; i < 14; ++i) {
    abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x2000u + i);
    abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x3000u + i);
  }
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(edn0_fatal_cause_reg) == 0u);

  // Deassert CMD_FIFO_RST (set to kMultiBitBool4False) in SWPortMode.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   make_edn_ctrl(kMultiBitBool4True, kMultiBitBool4False,
                                 kMultiBitBool4False, kMultiBitBool4False));

  // Push 13 words to RESEED_CMD -> FIFO depth is 13, so no error may occur!
  for (uint32_t i = 0; i < 13; ++i) {
    abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x1000u + i);
  }
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(edn0_fatal_cause_reg) == 0u);

  // Pushing a 14th word overflows RESEED_CMD FIFO -> sets SFIFO_RESCMD_ERR (0)
  // and FIFO_WRITE_ERR (28), sets INTR_STATE.EDN_FATAL_ERR, and fires
  // fatal_alert!
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0xdeadbeefu);
  IBEX_SPIN_FOR(abs_mmio_read32(edn0_fatal_cause_reg) == 1u, 100);

  uint32_t expected_err = (1u << EDN_ERR_CODE_SFIFO_RESCMD_ERR_BIT) |
                          (1u << EDN_ERR_CODE_FIFO_WRITE_ERR_BIT);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == expected_err);
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET),
            EDN_INTR_STATE_EDN_FATAL_ERR_BIT) == 1);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) ==
        kEdnStateSwPortMode);

  // Clear INTR_STATE and force bit 1 (SFIFO_GENCMD_ERR) via ERR_CODE_TEST.
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_SFIFO_GENCMD_ERR_BIT);

  expected_err |= (1u << EDN_ERR_CODE_SFIFO_GENCMD_ERR_BIT);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET) ==
        EDN_ERR_CODE_SFIFO_GENCMD_ERR_BIT);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == expected_err);
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET),
            EDN_INTR_STATE_EDN_FATAL_ERR_BIT) == 1);
}

static volatile bool g_fault_seen;
static volatile uint32_t g_fault_mcause;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

static void test_subword_wr_err_and_addrmiss(void) {
  LOG_INFO("4. Testing EDN0 addrmiss and EDN_PERMIT sub-word wr_err...");

  CHECK_STATUS_OK(entropy_testutils_stop_all());
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET) == 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET) == 0x901u);

  // 1. Unmapped offset 0x48 (EDN_MAIN_SM_STATE_REG_OFFSET + 4): addrmiss = 1 ->
  // Load/Store Access Fault.
  g_fault_seen = false;
  (void)abs_mmio_read32(kEdn0Base + 0x48u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  abs_mmio_write32(kEdn0Base + 0x48u, 0x12345678u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  // 2. Sub-word 8-bit store to EDN_CTRL (EDN_PERMIT[5] = 4'b0011): wr_err = 1
  // -> Store Access Fault, write blocked, and no spurious recoverable alert
  // asserted.
  g_fault_seen = false;
  abs_mmio_write8(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x99u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET) == 0x9999u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0u);

  // 3. Sub-word 16-bit store to EDN_CTRL (EDN_PERMIT[5] = 4'b0011): reg_be =
  // 4'b0011 -> succeeds!
  g_fault_seen = false;
  *(volatile uint16_t *)(kEdn0Base + EDN_CTRL_REG_OFFSET) = 0x9999u;
  CHECK(!g_fault_seen);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET) == 0x9999u);

  // 4. Sub-word 16-bit store to EDN_BOOT_INS_CMD (EDN_PERMIT[6] = 4'b1111):
  // wr_err = 1 -> Store Access Fault, register value preserved.
  g_fault_seen = false;
  *(volatile uint16_t *)(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET) = 0xdead;
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET) == 0x901u);

  // 5. Sub-word 8-bit store to EDN_INTR_ENABLE (EDN_PERMIT[1] = 4'b0001):
  // succeeds!
  g_fault_seen = false;
  abs_mmio_write8(kEdn0Base + EDN_INTR_ENABLE_REG_OFFSET, 0x3u);
  CHECK(!g_fault_seen);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_INTR_ENABLE_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kEdn0Base + EDN_INTR_ENABLE_REG_OFFSET, 0x0u);
}

bool test_main(void) {
  test_reset_and_mubi_alerts();
  test_sw_port_mode_and_csrng_ack_err();
  test_boot_and_auto_mode_hw_cmd_sts_and_max_reqs_cnt();
  test_subword_wr_err_and_addrmiss();
  test_fifo_depth_and_err_code_test();
  return true;
}
