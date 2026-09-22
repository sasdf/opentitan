// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEsBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
};

static volatile bool kLoadStoreFault = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  kLoadStoreFault = true;
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst = *(volatile uint16_t *)mepc;
  uint32_t step = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // Verify MODULE_ENABLE is False, MAIN_SM_STATE is Idle (0xf5), and REGWEN
  // is 1.
  uint32_t regwen = abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET);
  CHECK(regwen == 1u, "Expected REGWEN == 1 when disabled, got 0x%x", regwen);
  uint32_t sm_state =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  CHECK(sm_state == 0xf5u, "Expected MAIN_SM_STATE == 0xf5 (Idle), got 0x%x",
        sm_state);

  // 1. Verify FW_OV_WR_FIFO_FULL == 0 when fw_ov_mode_entropy_insert is
  // disabled (entropy_src_core.sv:1042: fw_ov_wr_fifo_full =
  // fw_ov_mode_entropy_insert && ...).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET, 0x99u);
  uint32_t wr_fifo_full =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET);
  CHECK(wr_fifo_full == 0u,
        "Expected FW_OV_WR_FIFO_FULL == 0 when FW_OV_CONTROL is 0x99, got 0x%x",
        wr_fifo_full);

  // 2. Verify FW_OV_SHA3_START is writable when MODULE_ENABLE == False and
  // validates mubi4_t (entropy_src_core.sv:762-770).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  uint32_t sha3_start =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET);
  CHECK(
      sha3_start == kMultiBitBool4True,
      "Expected FW_OV_SHA3_START == 0x6 when written while disabled, got 0x%x",
      sha3_start);

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET, 0x5u);
  uint32_t recov_sts =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(
          recov_sts,
          ENTROPY_SRC_RECOV_ALERT_STS_FW_OV_SHA3_START_FIELD_ALERT_BIT),
      "Expected FW_OV_SHA3_START_FIELD_ALERT set in RECOV_ALERT_STS, got 0x%x",
      recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 3. Verify CONF.FIPS_FLAG and CONF.RNG_FIPS invalid mubi4_t values trigger
  // FIPS_FLAG_FIELD_ALERT (bit 17) and RNG_FIPS_FIELD_ALERT (bit 18)
  // (entropy_src_core.sv:667-701).
  uint32_t conf_default = ENTROPY_SRC_CONF_REG_RESVAL;
  uint32_t conf_bad_fips_flag =
      (conf_default & ~(ENTROPY_SRC_CONF_FIPS_FLAG_MASK
                        << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET)) |
      (0x5u << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_bad_fips_flag);
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(
            recov_sts, ENTROPY_SRC_RECOV_ALERT_STS_FIPS_FLAG_FIELD_ALERT_BIT),
        "Expected FIPS_FLAG_FIELD_ALERT set in RECOV_ALERT_STS, got 0x%x",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_default);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  uint32_t conf_bad_rng_fips =
      (conf_default &
       ~(ENTROPY_SRC_CONF_RNG_FIPS_MASK << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET)) |
      (0x5u << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_bad_rng_fips);
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(
            recov_sts, ENTROPY_SRC_RECOV_ALERT_STS_RNG_FIPS_FIELD_ALERT_BIT),
        "Expected RNG_FIPS_FIELD_ALERT set in RECOV_ALERT_STS, got 0x%x",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_default);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 4. Verify ALERT_THRESHOLD updates even when halves are non-complementary,
  // and ES_THRESH_CFG_ALERT remains set while ALERT_THRESHOLD is invalid
  // (entropy_src_core.sv:2178-2180, 2226-2227).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0x12345678u);
  uint32_t alert_thresh =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET);
  CHECK(alert_thresh == 0x12345678u,
        "Expected ALERT_THRESHOLD == 0x12345678, got 0x%x", alert_thresh);
  // Attempting to clear RECOV_ALERT_STS while ALERT_THRESHOLD is still invalid
  // must leave ES_THRESH_CFG_ALERT set.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(
            recov_sts, ENTROPY_SRC_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_BIT),
        "Expected ES_THRESH_CFG_ALERT to remain set while ALERT_THRESHOLD is "
        "invalid, got 0x%x",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   ENTROPY_SRC_ALERT_THRESHOLD_REG_RESVAL);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 5. Verify MODULE_ENABLE with invalid mubi4_t (0x5) keeps REGWEN == 1 via
  // mubi4_test_false_loose (entropy_src_core.sv:557-559).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET, 0x5u);
  regwen = abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET);
  CHECK(
      regwen == 1u,
      "Expected REGWEN == 1 when MODULE_ENABLE is 0x5 (loose false), got 0x%x",
      regwen);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 6. Verify DEBUG_STATUS.ENTROPY_FIFO_DEPTH reports seed count (not word
  // count & 3) when a seed is ready in bypass SW-route mode.
  uint32_t conf_sw_bypass =
      (kMultiBitBool4False << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_sw_bypass);
  uint32_t es_ctrl_sw_bypass =
      (kMultiBitBool4True << ENTROPY_SRC_ENTROPY_CONTROL_ES_ROUTE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_ENTROPY_CONTROL_ES_TYPE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   es_ctrl_sw_bypass);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);

  uint32_t intr_state = 0;
  do {
    intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  } while (!bitfield_bit32_read(intr_state,
                                ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT));

  uint32_t debug_status =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  uint32_t fifo_depth = bitfield_field32_read(
      debug_status, ENTROPY_SRC_DEBUG_STATUS_ENTROPY_FIFO_DEPTH_FIELD);
  CHECK(fifo_depth >= 1u,
        "Expected DEBUG_STATUS.ENTROPY_FIFO_DEPTH >= 1 when ES_ENTROPY_VALID "
        "is asserted, got %u (DEBUG_STATUS=0x%x)",
        fifo_depth, debug_status);

  // 7. Disable MODULE_ENABLE and verify ERR_CODE_TEST latches ERR_CODE bit 0
  // (SFIFO_ESRNG_ERR) without triggering a fatal alert when disabled
  // (entropy_src_core.sv:907-918, 962-963).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  while (!abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET)) {
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ERR_CODE_TEST_REG_OFFSET,
                   ENTROPY_SRC_ERR_CODE_SFIFO_ESRNG_ERR_BIT);
  uint32_t err_code =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  CHECK(bitfield_bit32_read(err_code, ENTROPY_SRC_ERR_CODE_SFIFO_ESRNG_ERR_BIT),
        "Expected ERR_CODE.SFIFO_ESRNG_ERR latched after ERR_CODE_TEST=0, got "
        "0x%x",
        err_code);

  // 8. Wave 2: Verify FW_OV_SHA3_START FSM transitions, post-hash FWInsertStart
  // state (0x0c3) with MAIN_SM_IDLE == 0, and Check 9: ES_BUS_CMP_ALERT when
  // two consecutive seeds have identical lower 64 bits
  // (entropy_src_main_sm.sv:58-68, 217-259; entropy_src_core.sv:2998-3020).
  uint32_t conf_fw_ov_sha3 =
      (kMultiBitBool4True << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_fw_ov_sha3);
  uint32_t es_ctrl_sw_cond =
      (kMultiBitBool4True << ENTROPY_SRC_ENTROPY_CONTROL_ES_ROUTE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_ENTROPY_CONTROL_ES_TYPE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   es_ctrl_sw_cond);
  uint32_t fw_ov_insert =
      (kMultiBitBool4True << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_MODE_OFFSET) |
      (kMultiBitBool4True
       << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_ENTROPY_INSERT_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   fw_ov_insert);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);

  sm_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  CHECK(sm_state == 0x0c3u,
        "Expected MAIN_SM_STATE == 0x0c3 (FWInsertStart), got 0x%x", sm_state);
  debug_status = abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(debug_status,
                             ENTROPY_SRC_DEBUG_STATUS_MAIN_SM_IDLE_BIT),
        "Expected DEBUG_STATUS.MAIN_SM_IDLE == 0 in FWInsertStart, got 0x%x",
        debug_status);

  // Writing FW_OV_SHA3_START = False while in FWInsertStart must NOT trigger a
  // hash or leave FWInsertStart.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  sm_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  debug_status = abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  CHECK(sm_state == 0x0c3u &&
            bitfield_field32_read(
                debug_status,
                ENTROPY_SRC_DEBUG_STATUS_ENTROPY_FIFO_DEPTH_FIELD) == 0u,
        "Expected FWInsertStart (0x0c3) and depth 0 after redundant False "
        "write, got state=0x%x debug=0x%x",
        sm_state, debug_status);

  // Flush any residual ROM entropy from the SHA3 sponge first
  // (entropy_src_core.sv:2917-2924: sha3_start_mask_q).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  sm_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  CHECK(sm_state == 0x059u,
        "Expected MAIN_SM_STATE == 0x059 (FWInsertMsg), got 0x%x", sm_state);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  do {
    intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  } while (!bitfield_bit32_read(intr_state,
                                ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT));
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // Start SHA3 message insertion for Seed 1, push 2 words, and stop
  // conditioner.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x55667788u);
  while (abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET)) {
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  do {
    intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  } while (!bitfield_bit32_read(intr_state,
                                ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT));

  // After Sha3MsgDone -> Idle, FSM must immediately transition back to
  // FWInsertStart (0x0c3) with MAIN_SM_IDLE == 0.
  sm_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  debug_status = abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  CHECK(sm_state == 0x0c3u,
        "Expected MAIN_SM_STATE == 0x0c3 (FWInsertStart) after FW_OV hash, got "
        "0x%x",
        sm_state);
  CHECK(!bitfield_bit32_read(debug_status,
                             ENTROPY_SRC_DEBUG_STATUS_MAIN_SM_IDLE_BIT),
        "Expected DEBUG_STATUS.MAIN_SM_IDLE == 0 after FW_OV hash, got 0x%x",
        debug_status);

  // Drain all 12 words of Seed 1.
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // 9. Wave 2: Produce a second identical seed (0x11223344, 0x55667788) and
  // verify popping it triggers ES_BUS_CMP_ALERT (bit 13).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x55667788u);
  while (abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET)) {
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  do {
    intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  } while (!bitfield_bit32_read(intr_state,
                                ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            ENTROPY_SRC_RECOV_ALERT_STS_ES_BUS_CMP_ALERT_BIT),
        "Expected ES_BUS_CMP_ALERT set after popping identical seed, got 0x%x",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 10. Wave 2: Verify ALERT_THRESHOLD ~alert_threshold_inv comparison in
  // alert_threshold_fail (entropy_src_core.sv:2182-2184): setting
  // alert_threshold = 2 and alert_threshold_inv = 0xfffe (~inv == 1) with
  // ADAPTP_HI_THRESHOLDS = 0x00010001 (1 fail per window) triggers AlertHang
  // (0x1fb) on the 1st window (summary_fails == 1).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  while (!abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET)) {
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET, 0x99u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_sw_bypass);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   es_ctrl_sw_bypass);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ADAPTP_HI_THRESHOLDS_REG_OFFSET,
                   0x00010001u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0xfffe0002u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);
  do {
    intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  } while (!bitfield_bit32_read(
      intr_state, ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  uint32_t summary_fails = abs_mmio_read32(
      kEsBase + ENTROPY_SRC_ALERT_SUMMARY_FAIL_COUNTS_REG_OFFSET);
  CHECK(summary_fails == 1u,
        "Expected ALERT_SUMMARY_FAIL_COUNTS == 1 when ~alert_threshold_inv == "
        "1 (with alert_threshold == 2), got fails=%u",
        summary_fails);
  while (!abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET)) {
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   ENTROPY_SRC_ALERT_THRESHOLD_REG_RESVAL);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  // 11. Wave 2: Verify reading ENTROPY_DATA 12 times when esfinal FIFO is empty
  // triggers SFIFO_ESFINAL_ERR | FIFO_READ_ERR on the 12th read
  // (entropy_src_core.sv:2967, 2976, 3043).
  // Ignore the fatal alert in alert_handler since prim_alert_sender(IsFatal=1)
  // latches fatal_alert until reset.
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdEntropySrcFatalAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   fw_ov_insert);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);
  for (int i = 0; i < 11; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  err_code = abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  CHECK(!bitfield_bit32_read(err_code,
                             ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT),
        "Expected SFIFO_ESFINAL_ERR == 0 after 11 empty reads, got 0x%x",
        err_code);

  (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  err_code = abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  intr_state = abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(
      bitfield_bit32_read(err_code,
                          ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT) &&
          bitfield_bit32_read(err_code, ENTROPY_SRC_ERR_CODE_FIFO_READ_ERR_BIT),
      "Expected SFIFO_ESFINAL_ERR and FIFO_READ_ERR set on 12th empty read, "
      "got 0x%x",
      err_code);
  CHECK(
      bitfield_bit32_read(intr_state, ENTROPY_SRC_INTR_STATE_ES_FATAL_ERR_BIT),
      "Expected INTR_STATE.ES_FATAL_ERR set on 12th empty read, got 0x%x",
      intr_state);

  // 12. Wave 5: Verify ENTROPY_SRC_PERMIT sub-word write error (wr_err) and
  // addrmiss decode error (entropy_src_reg_pkg.sv:980-1038,
  // entropy_src_reg_top.sv:3466-3523).
  uint32_t conf_before = abs_mmio_read32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET);
  kLoadStoreFault = false;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, 0x66u);
  CHECK(kLoadStoreFault,
        "Expected Store Access Fault on 8-bit write to ENTROPY_SRC_CONF "
        "(PERMIT=4'b1111)");
  CHECK(abs_mmio_read32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET) == conf_before,
        "Expected ENTROPY_SRC_CONF unchanged after rejected sub-word write");

  kLoadStoreFault = false;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET + 1u, 0x09u);
  CHECK(kLoadStoreFault,
        "Expected Store Access Fault on 8-bit write to byte 1 of "
        "ENTROPY_SRC_MODULE_ENABLE (PERMIT=4'b0001)");

  kLoadStoreFault = false;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                  kMultiBitBool4False);
  CHECK(!kLoadStoreFault,
        "Expected 8-bit write to byte 0 of ENTROPY_SRC_MODULE_ENABLE "
        "(PERMIT=4'b0001) to succeed");

  kLoadStoreFault = false;
  (void)abs_mmio_read32(kEsBase + 0xe4u);
  CHECK(kLoadStoreFault,
        "Expected Load Access Fault on unmapped offset 0xe4 (addrmiss)");

  kLoadStoreFault = false;
  abs_mmio_write32(kEsBase + 0xe4u, 0u);
  CHECK(kLoadStoreFault,
        "Expected Store Access Fault on unmapped offset 0xe4 (addrmiss)");

  return true;
}
