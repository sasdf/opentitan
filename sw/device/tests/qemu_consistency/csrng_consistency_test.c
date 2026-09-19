// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/csrng_testutils.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "csrng_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
};

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // 1. Verify INT_STATE_NUM is writable and readable even when
  // CTRL.READ_INT_STATE is kMultiBitBool4False (0x9).
  uint32_t ctrl_read_int_off =
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  uint32_t int_state_num =
      abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET);
  CHECK(int_state_num == 2u,
        "Expected INT_STATE_NUM == 2 when READ_INT_STATE is False, got 0x%x",
        int_state_num);

  // 2a. Verify invalid MuBi4 in CTRL.FIPS_FORCE_ENABLE sets
  // RECOV_ALERT_STS.FIPS_FORCE_ENABLE_FIELD_ALERT (bit 3) without firing
  // recov_alert_o (per csrng_core.sv:754-761, 844-846).
  uint32_t ctrl_bad_fips_mubi =
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (0x0u << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_bad_fips_mubi);
  uint32_t recov_sts =
      abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(recov_sts ==
            (1u << CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT),
        "Expected RECOV_ALERT_STS == 0x%x (FIPS_FORCE_ENABLE_FIELD_ALERT), got 0x%x",
        (1u << CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT),
        recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);

  // 2b. Verify invalid MuBi4 in CTRL.READ_INT_STATE sets
  // RECOV_ALERT_STS.READ_INT_STATE_FIELD_ALERT (bit 2) AND fires recov_alert_o.
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  uint32_t ctrl_bad_read_int_mubi =
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (0x0u << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_bad_read_int_mubi);
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(recov_sts ==
            (1u << CSRNG_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_BIT),
        "Expected RECOV_ALERT_STS == 0x%x (READ_INT_STATE_FIELD_ALERT), got 0x%x",
        (1u << CSRNG_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_BIT),
        recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));

  // 3. Verify out-of-sequence RESEED command on uninstantiated SW instance
  // raises RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT (bit 14), sets
  // SW_CMD_STS.CMD_ACK=1 and SW_CMD_STS.CMD_STS=3 (INVALID_CMD_SEQ), and
  // does NOT enter CSRNG_ERROR (MAIN_SM_STATE remains 0x4e Idle, ERR_CODE == 0).
  dif_csrng_t csrng;
  CHECK_DIF_OK(
      dif_csrng_init(mmio_region_from_addr(kCsrngBase), &csrng));
  CHECK_DIF_OK(dif_csrng_uninstantiate(&csrng));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  // Issue RESEED (acmd = 2, clen = 0, flag0 = kMultiBitBool4True) while
  // uninstantiated.
  uint32_t reseed_cmd_hdr =
      2u | (0u << 4) | (kMultiBitBool4True << 8) | (0u << 12);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, reseed_cmd_hdr);

  uint32_t sw_cmd_sts = 0;
  do {
    sw_cmd_sts = abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET);
  } while (!bitfield_bit32_read(sw_cmd_sts, CSRNG_SW_CMD_STS_CMD_ACK_BIT));

  uint32_t cmd_sts =
      bitfield_field32_read(sw_cmd_sts, CSRNG_SW_CMD_STS_CMD_STS_FIELD);
  CHECK(cmd_sts == CSRNG_SW_CMD_STS_CMD_STS_VALUE_INVALID_CMD_SEQ,
        "Expected SW_CMD_STS.CMD_STS == %u (INVALID_CMD_SEQ), got %u (raw 0x%x)",
        CSRNG_SW_CMD_STS_CMD_STS_VALUE_INVALID_CMD_SEQ, cmd_sts, sw_cmd_sts);

  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(
            recov_sts, CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT),
        "Expected CMD_STAGE_INVALID_CMD_SEQ_ALERT set in RECOV_ALERT_STS, got 0x%x",
        recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));

  uint32_t err_code = abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET);
  uint32_t sm_state =
      abs_mmio_read32(kCsrngBase + CSRNG_MAIN_SM_STATE_REG_OFFSET);
  CHECK(err_code == 0u, "Expected ERR_CODE == 0 after INVALID_CMD_SEQ, got 0x%x",
        err_code);
  CHECK(sm_state == 0x4eu,
        "Expected MAIN_SM_STATE == 0x4e (Idle) after INVALID_CMD_SEQ, got 0x%x",
        sm_state);

  // 4. Verify FIPS_FORCE requires CTRL.FIPS_FORCE_ENABLE == kMultiBitBool4True.
  abs_mmio_write32(kCsrngBase + CSRNG_FIPS_FORCE_REG_OFFSET, 0x7u);
  const dif_csrng_seed_material_t kSeed = {
      .seed_material = {0x73bec010, 0x9262474c, 0x16a30f76, 0x531b51de,
                        0x2ee494e5, 0xdfec9db3, 0xcb7a879d, 0x5600419c,
                        0xca79b0b0, 0xdda33b5c, 0xa468649e, 0xdf5d73fa},
      .seed_material_len = 12,
  };
  CHECK_DIF_OK(dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable,
                                     &kSeed));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  uint32_t genbits_vld = 0;
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (!bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));
  CHECK(!bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_FIPS_BIT),
        "Expected GENBITS_FIPS == 0 when CTRL.FIPS_FORCE_ENABLE is False, got 0x%x",
        genbits_vld);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }

  return true;
}
