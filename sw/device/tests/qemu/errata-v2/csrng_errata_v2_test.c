// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file csrng_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2`) CSRNG
 *        confirmed v1 errata (`ERRATA-CSRNG-001..004`) and newly discovered
 *        v2 hardware errata (`ERRATA-CSRNG-V2-001`, `ERRATA-CSRNG-V2-002`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/alert_handler_regs.h"
#include "hw/top/csrng_regs.h"
#include "hw/top/edn_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kCsrngRecovAlertId = kTopEarlgreyAlertIdCsrngRecovAlert,
  kNumIntStateWords = 14,
};

static uint32_t make_ctrl(uint32_t enable, uint32_t sw_app_enable,
                          uint32_t read_int_state, uint32_t fips_force_enable) {
  return ((enable & 0xfu) << CSRNG_CTRL_ENABLE_OFFSET) |
         ((sw_app_enable & 0xfu) << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
         ((read_int_state & 0xfu) << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
         ((fips_force_enable & 0xfu) << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
}

static void csrng_disable_and_clear(void) {
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   (kMultiBitBool4False << EDN_CTRL_EDN_ENABLE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_BOOT_REQ_MODE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_AUTO_REQ_MODE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_CMD_FIFO_RST_OFFSET));
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4False, kMultiBitBool4False,
                             kMultiBitBool4False, kMultiBitBool4False));
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                       kCsrngRecovAlertId * 4u,
                   1u);
}

static void csrng_wait_cmd_ack(void) {
  for (int i = 0; i < 100000; ++i) {
    uint32_t sts = abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET);
    if (bitfield_bit32_read(sts, CSRNG_SW_CMD_STS_CMD_ACK_BIT)) {
      return;
    }
  }
  CHECK(false, "Timed out waiting for CSRNG_SW_CMD_STS.CMD_ACK");
}

static void csrng_send_sw_cmd(uint32_t acmd, uint32_t clen, uint32_t flag0,
                              uint32_t glen, const uint32_t *seed) {
  uint32_t hdr = (acmd & 0x7u) | ((clen & 0xfu) << 4) | ((flag0 & 0xfu) << 8) |
                 ((glen & 0xfffu) << 12);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, hdr);
  for (uint32_t i = 0; i < clen; ++i) {
    abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, seed[i]);
  }
  csrng_wait_cmd_ack();
}

/**
 * [ERRATA-CSRNG-001] (CONFIRMED_PRESENT_ON_V2):
 * RECOV_ALERT_STS.FIPS_FORCE_ENABLE_FIELD_ALERT (bit 3) is omitted from
 * recov_alert_event in csrng_core.sv:391-398, so recov_alert_o never fires.
 */
static void test_v1_001_fips_force_enable_pfa_missing_from_recov_alert(void) {
  LOG_INFO("Testing [ERRATA-CSRNG-001] on trunk-v2...");
  csrng_disable_and_clear();

  // Enable alert_handler capture for CSRNG recoverable alert (ID 26).
  uint32_t en_reg = kAlertBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                    kCsrngRecovAlertId * 4u;
  abs_mmio_write32_shadowed(en_reg, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                       kCsrngRecovAlertId * 4u,
                   1u);

  // Write invalid MUBI4 (0x5) to CTRL.FIPS_FORCE_ENABLE (bits [15:12]).
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4False, 0x5u));

  uint32_t recov_sts =
      abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  uint32_t alert_cause =
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                      kCsrngRecovAlertId * 4u);
  LOG_INFO(
      "  FIPS_FORCE_ENABLE=0x5 -> RECOV_ALERT_STS=0x%x, ALERT_CAUSE[26]=%u",
      recov_sts, alert_cause);

  CHECK(bitfield_bit32_read(
            recov_sts, CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT),
        "Expected RECOV_ALERT_STS bit 3 == 1");
  CHECK(alert_cause == 0u,
        "Expected ALERT_CAUSE[26] == 0 due to missing fips_force_enable_pfa in "
        "recov_alert_event");

  // Contrast with invalid MUBI4 (0x5) on CTRL.READ_INT_STATE (bits [11:8]),
  // which IS included in recov_alert_event and immediately sets
  // ALERT_CAUSE[26].
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True, 0x5u,
                             kMultiBitBool4False));
  alert_cause =
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                      kCsrngRecovAlertId * 4u);
  CHECK(alert_cause == 1u,
        "Expected ALERT_CAUSE[26] == 1 when READ_INT_STATE has invalid MUBI4");

  csrng_disable_and_clear();
}

/**
 * [ERRATA-CSRNG-002] (CONFIRMED_PRESENT_ON_V2):
 * HW_EXC_STS (rw0c) overwrites itself with 0x0000 after 1 clock cycle because
 * hw2reg.hw_exc_sts.de = cs_enable_fo[50] (1'b1) in csrng_core.sv:1002.
 */
static void test_v1_002_hw_exc_sts_rw0c_single_cycle_overwrite(void) {
  LOG_INFO("Testing [ERRATA-CSRNG-002] on trunk-v2...");
  csrng_disable_and_clear();

  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4False, kMultiBitBool4False));

  // Issue an invalid sequence command (RES=2 before INS=1) from EDN0 (HW
  // instance 0) via EDN0 SW_CMD_REQ.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET,
                   (kMultiBitBool4True << EDN_CTRL_EDN_ENABLE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_BOOT_REQ_MODE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_AUTO_REQ_MODE_OFFSET) |
                       (kMultiBitBool4False << EDN_CTRL_CMD_FIFO_RST_OFFSET));
  // Send RESEED (acmd=2, clen=0, flag0=kMultiBitBool4True=0x6) on
  // uninstantiated HW0.
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET,
                   0x2u | (kMultiBitBool4True << 8));

  for (int i = 0; i < 10000; ++i) {
    uint32_t recov =
        abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
    if (bitfield_bit32_read(
            recov, CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT)) {
      break;
    }
  }

  uint32_t recov_sts =
      abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  uint32_t hw_exc_sts =
      abs_mmio_read32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET);
  LOG_INFO("  EDN0 invalid cmd seq -> RECOV_ALERT_STS=0x%x, HW_EXC_STS=0x%x",
           recov_sts, hw_exc_sts);

  CHECK(
      bitfield_bit32_read(
          recov_sts, CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT),
      "Expected CMD_STAGE_INVALID_CMD_SEQ_ALERT == 1");
  CHECK(hw_exc_sts == 0u,
        "Expected HW_EXC_STS == 0 due to continuous hw2reg.hw_exc_sts.de=1 "
        "overwrite");

  csrng_disable_and_clear();
}

/**
 * [ERRATA-CSRNG-003] (CONFIRMED_PRESENT_ON_V2 + expanded dead bits in v2):
 * 1. In trunk-v2, ERR_CODE_TEST values 2..19, 23..24, 27 are dead no-ops
 *    (assigned to unused_err_code_test_bit in csrng_core.sv:1014-1015).
 * 2. When CTRL.ENABLE == kMultiBitBool4False, writing ERR_CODE_TEST = 20..22,
 *    25..26 still asserts INTR_STATE.cs_fatal_err = 1 while ERR_CODE remains 0!
 */
static void test_v1_003_err_code_test_disabled_gating_and_v2_dead_bits(void) {
  LOG_INFO("Testing [ERRATA-CSRNG-003] on trunk-v2...");
  csrng_disable_and_clear();

  // 1. Even when CSRNG is ENABLED, ERR_CODE_TEST = 23 and 24 (which were
  //    DRBG_UPDBE_SM_ERR and DRBG_UPDOB_SM_ERR in v1) and 2..19, 27 are dead in
  //    v2!
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4False, kMultiBitBool4False));
  const uint32_t dead_bits[] = {2u, 15u, 19u, 23u, 24u, 27u};
  for (size_t i = 0; i < sizeof(dead_bits) / sizeof(dead_bits[0]); ++i) {
    abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, dead_bits[i]);
    uint32_t err_code = abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET);
    uint32_t intr = abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET);
    CHECK(err_code == 0u &&
              !bitfield_bit32_read(intr, CSRNG_INTR_STATE_CS_FATAL_ERR_BIT),
          "Expected ERR_CODE_TEST=%u to be a dead no-op in trunk-v2",
          dead_bits[i]);
  }

  // 2. With CTRL.ENABLE = False, ERR_CODE_TEST = 20 (CMD_STAGE_SM_ERR) and
  //    22 (CTR_DRBG_SM_ERR) fire INTR_STATE.cs_fatal_err = 1 while ERR_CODE
  //    stays 0!
  csrng_disable_and_clear();
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 22u);
  uint32_t err_code = abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET);
  uint32_t intr = abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET);
  LOG_INFO("  Disabled ERR_CODE_TEST=22 -> ERR_CODE=0x%x, INTR_STATE=0x%x",
           err_code, intr);
  CHECK(err_code == 0u, "Expected ERR_CODE == 0 when CTRL.ENABLE == False");
  CHECK(bitfield_bit32_read(intr, CSRNG_INTR_STATE_CS_FATAL_ERR_BIT),
        "Expected INTR_STATE.cs_fatal_err == 1 when ERR_CODE_TEST=22 while "
        "disabled");

  csrng_disable_and_clear();
}

/**
 * [ERRATA-CSRNG-004] (CONFIRMED_PRESENT_ON_V2):
 * Reading GENBITS when !CTRL.SW_APP_ENABLE returns 0 yet destructively pops
 * u_prim_packer_fifo_sw_genbits, and GENBITS_VLD remains ungated.
 */
static void test_v1_004_genbits_destructive_pop_when_sw_app_disabled(void) {
  LOG_INFO("Testing [ERRATA-CSRNG-004] on trunk-v2...");
  csrng_disable_and_clear();

  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4False, kMultiBitBool4False));

  // Instantiate (acmd=1, clen=0, flag0=kMultiBitBool4True=0x6, glen=0)
  csrng_send_sw_cmd(1u, 0u, kMultiBitBool4True, 0u, NULL);
  // Generate 1 block = 4 words (acmd=3, clen=0, flag0=0x9, glen=1)
  csrng_send_sw_cmd(3u, 0u, kMultiBitBool4False, 1u, NULL);

  for (int i = 0; i < 10000; ++i) {
    uint32_t vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
    if (bitfield_bit32_read(vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT)) {
      break;
    }
  }
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET),
            CSRNG_GENBITS_VLD_GENBITS_VLD_BIT),
        "Expected GENBITS_VLD == 1 after generate");

  // Disable SW_APP_ENABLE (keep ENABLE = True).
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4False,
                             kMultiBitBool4False, kMultiBitBool4False));

  // GENBITS_VLD is still 1 (ungated), and 4 reads of GENBITS return 0 while
  // destructively popping all 4 words from u_prim_packer_fifo_sw_genbits.
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET),
            CSRNG_GENBITS_VLD_GENBITS_VLD_BIT),
        "Expected GENBITS_VLD == 1 even when SW_APP_ENABLE == False");
  for (int w = 0; w < 4; ++w) {
    uint32_t val = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
    CHECK(val == 0u, "Expected GENBITS read %d to return 0 when disabled", w);
  }
  uint32_t vld_after =
      abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  LOG_INFO("  After 4 disabled GENBITS reads -> GENBITS_VLD=0x%x", vld_after);
  CHECK(!bitfield_bit32_read(vld_after, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT),
        "Expected GENBITS_VLD == 0 after 4 destructive disabled reads");

  csrng_disable_and_clear();
}

/**
 * [ERRATA-CSRNG-V2-001] & [ERRATA-CSRNG-V2-002] (NEW_IN_V2):
 * 1. [ERRATA-CSRNG-V2-001]: In trunk-v2 (commit add768a0), INT_STATE_NUM (0x54)
 *    was converted from hwext:true to an unconditional reggen flip-flop
 *    (hwaccess:hro), whereas csrng_state_db.reg_rd_id_q is cleared to 0
 * whenever !enable_i. Writing INT_STATE_NUM = 2 while CTRL.ENABLE == False
 * makes INT_STATE_NUM read back 2 (passing the HJSON readback check), while
 *    csrng_state_db.reg_rd_id_q remains 0!
 * 2. [ERRATA-CSRNG-V2-002]: In trunk-v2 (commit add768a0,
 * csrng_state_db.sv:95-96), when !reg_rd_otp_en_i (!CTRL.READ_INT_STATE),
 * reg_rd_ptr_d is forced to '1 (4'hF = 15) instead of '0 (0). Consequently, if
 * software writes INT_STATE_NUM = 2 while CTRL.ENABLE == True and
 * CTRL.READ_INT_STATE == False and then sets CTRL.READ_INT_STATE = True,
 * reg_rd_id_q latches 2, but reg_rd_ptr_q stays 15 (4'hF). Reading 14 words
 * from INT_STATE_VAL returns 0x00000000 on word 0 (since 15 >= NumRegState=14),
 * wraps reg_rd_ptr_q to 0, and shifts words 0..12 into readout positions 1..13
 * (dropping word 13 {fips, inst_state})!
 */
static void test_v2_new_001_002_int_state_num_desync_and_ptr_15_shift(void) {
  LOG_INFO(
      "Testing [ERRATA-CSRNG-V2-001] & [ERRATA-CSRNG-V2-002] on trunk-v2...");
  csrng_disable_and_clear();

  // --- Subtest A: [ERRATA-CSRNG-V2-001] ---
  // Write INT_STATE_NUM = 2 while CTRL.ENABLE == False.
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  CHECK(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET) == 2u,
        "Expected INT_STATE_NUM register flip-flop to read back 2");

  // Now enable CTRL (ENABLE=True, SW_APP_ENABLE=True, READ_INT_STATE=True)
  // without re-writing INT_STATE_NUM, and instantiate SW instance 2.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4True, kMultiBitBool4False));
  const uint32_t seed[1] = {0xdeadbeefu};
  csrng_send_sw_cmd(1u, 1u, kMultiBitBool4True, 0u, seed);

  // INT_STATE_NUM still reads back 2, but reg_rd_id_q in csrng_state_db is 0
  // (uninstantiated HW0 instance, all zeros) AND reg_rd_ptr_q started at 15!
  CHECK(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET) == 2u,
        "Expected INT_STATE_NUM to still read 2");
  uint32_t any_nonzero = 0u;
  for (int i = 0; i < kNumIntStateWords; ++i) {
    any_nonzero |= abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  }
  LOG_INFO(
      "  [ERRATA-CSRNG-V2-001] INT_STATE_NUM=2 readback, OR of 14 "
      "INT_STATE_VAL words=0x%x",
      any_nonzero);
  CHECK(any_nonzero == 0u,
        "Expected all 14 INT_STATE_VAL reads to be 0 because reg_rd_id_q was "
        "held at 0 while CTRL.ENABLE was False");

  // --- Subtest B: [ERRATA-CSRNG-V2-002] ---
  // Now keep CTRL.ENABLE=True, SW_APP_ENABLE=True, set READ_INT_STATE=False.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4False, kMultiBitBool4False));
  // Generate 1 block on SW instance 2 (acmd=3, clen=0, flag0=False, glen=1) so
  // rs_ctr increments from 0 to 1.
  csrng_send_sw_cmd(3u, 0u, kMultiBitBool4False, 1u, NULL);

  // Write INT_STATE_NUM = 2 while CTRL.ENABLE == True and READ_INT_STATE ==
  // False. reg_rd_id_q latches 2, and INT_STATE_NUM reads back 2, but
  // reg_rd_ptr_q stays 15!
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  CHECK(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET) == 2u,
        "Expected INT_STATE_NUM == 2");

  // Enable CTRL.READ_INT_STATE = True and read 14 words (w_shifted[0..13]).
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET,
                   make_ctrl(kMultiBitBool4True, kMultiBitBool4True,
                             kMultiBitBool4True, kMultiBitBool4False));
  uint32_t w_shifted[kNumIntStateWords];
  for (int i = 0; i < kNumIntStateWords; ++i) {
    w_shifted[i] = abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  }

  // Now write INT_STATE_NUM = 2 while READ_INT_STATE == True to reset
  // reg_rd_ptr_q = 0 and read the true 14 words (w_true[0..13]).
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  uint32_t w_true[kNumIntStateWords];
  for (int i = 0; i < kNumIntStateWords; ++i) {
    w_true[i] = abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  }

  LOG_INFO(
      "  [ERRATA-CSRNG-V2-002] w_shifted[0]=0x%x, w_shifted[1]=0x%x vs "
      "w_true[0]=0x%x (rs_ctr), w_true[13]=0x%x (flags)",
      w_shifted[0], w_shifted[1], w_true[0], w_true[13]);

  CHECK(w_true[0] == 1u, "Expected true word 0 (rs_ctr) == 1");
  CHECK(w_true[13] == 1u, "Expected true word 13 (inst_state=1, fips=0) == 1");
  CHECK(w_shifted[0] == 0u,
        "Expected w_shifted[0] == 0 due to reg_rd_ptr_q == 15 (>= "
        "NumRegState=14)");
  for (int i = 1; i < kNumIntStateWords; ++i) {
    CHECK(w_shifted[i] == w_true[i - 1],
          "Expected w_shifted[%d] (0x%x) == w_true[%d] (0x%x) due to 1-word "
          "pointer misalignment",
          i, w_shifted[i], i - 1, w_true[i - 1]);
  }

  csrng_disable_and_clear();
}

bool test_main(void) {
  LOG_INFO("=== CSRNG Earlgrey v2 (trunk-v2) Errata Verification Test ===");
  test_v1_001_fips_force_enable_pfa_missing_from_recov_alert();
  test_v1_002_hw_exc_sts_rw0c_single_cycle_overwrite();
  test_v1_004_genbits_destructive_pop_when_sw_app_disabled();
  test_v2_new_001_002_int_state_num_desync_and_ptr_15_shift();
  // Run test_v1_003 last because ERR_CODE_TEST=22 triggers fatal_loc_events,
  // which permanently locks u_csrng_main_sm into MainSmError (0x29) until
  // reset.
  test_v1_003_err_code_test_disabled_gating_and_v2_dead_bits();
  LOG_INFO("=== ALL CSRNG Earlgrey v2 Errata Checks PASSED on CW340 FPGA! ===");
  return true;
}
