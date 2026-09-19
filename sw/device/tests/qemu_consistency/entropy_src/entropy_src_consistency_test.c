// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEsBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
};

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // Verify MODULE_ENABLE is False, MAIN_SM_STATE is Idle (0xf5), and REGWEN is 1.
  uint32_t regwen = abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET);
  CHECK(regwen == 1u, "Expected REGWEN == 1 when disabled, got 0x%x", regwen);
  uint32_t sm_state =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  CHECK(sm_state == 0xf5u, "Expected MAIN_SM_STATE == 0xf5 (Idle), got 0x%x",
        sm_state);

  // 1. Verify FW_OV_WR_FIFO_FULL == 0 when fw_ov_mode_entropy_insert is disabled
  // (entropy_src_core.sv:1042: fw_ov_wr_fifo_full = fw_ov_mode_entropy_insert && ...).
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
  CHECK(sha3_start == kMultiBitBool4True,
        "Expected FW_OV_SHA3_START == 0x6 when written while disabled, got 0x%x",
        sha3_start);

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET, 0x5u);
  uint32_t recov_sts =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(
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
      (conf_default & ~(ENTROPY_SRC_CONF_RNG_FIPS_MASK
                        << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET)) |
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
  CHECK(regwen == 1u,
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

  return true;
}
