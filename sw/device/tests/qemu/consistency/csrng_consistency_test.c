// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/csrng_testutils.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "csrng_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

static bool expect_load_fault_32(mmio_region_t base, uint32_t offset) {
  uint32_t before = fault_count;
  last_mcause = 0;
  (void)mmio_region_read32(base, (ptrdiff_t)offset);
  return (fault_count == before + 1u) && (last_mcause == 5u);
}

static bool expect_store_fault_32(mmio_region_t base, uint32_t offset,
                                  uint32_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write32(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

static bool expect_store_fault_8(mmio_region_t base, uint32_t offset,
                                 uint8_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write8(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

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
        "Expected RECOV_ALERT_STS == 0x%x (FIPS_FORCE_ENABLE_FIELD_ALERT), got "
        "0x%x",
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
  CHECK(
      recov_sts == (1u << CSRNG_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_BIT),
      "Expected RECOV_ALERT_STS == 0x%x (READ_INT_STATE_FIELD_ALERT), got 0x%x",
      (1u << CSRNG_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_BIT), recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));

  // 3. Verify out-of-sequence RESEED command on uninstantiated SW instance
  // raises RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT (bit 14), sets
  // SW_CMD_STS.CMD_ACK=1 and SW_CMD_STS.CMD_STS=3 (INVALID_CMD_SEQ), and
  // does NOT enter CSRNG_ERROR (MAIN_SM_STATE remains 0x4e Idle, ERR_CODE ==
  // 0).
  dif_csrng_t csrng;
  CHECK_DIF_OK(dif_csrng_init(mmio_region_from_addr(kCsrngBase), &csrng));
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
  CHECK(
      cmd_sts == CSRNG_SW_CMD_STS_CMD_STS_VALUE_INVALID_CMD_SEQ,
      "Expected SW_CMD_STS.CMD_STS == %u (INVALID_CMD_SEQ), got %u (raw 0x%x)",
      CSRNG_SW_CMD_STS_CMD_STS_VALUE_INVALID_CMD_SEQ, cmd_sts, sw_cmd_sts);

  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(
          recov_sts, CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT),
      "Expected CMD_STAGE_INVALID_CMD_SEQ_ALERT set in RECOV_ALERT_STS, got "
      "0x%x",
      recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));

  uint32_t err_code = abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET);
  uint32_t sm_state =
      abs_mmio_read32(kCsrngBase + CSRNG_MAIN_SM_STATE_REG_OFFSET);
  CHECK(err_code == 0u,
        "Expected ERR_CODE == 0 after INVALID_CMD_SEQ, got 0x%x", err_code);
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
  CHECK_DIF_OK(
      dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable, &kSeed));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  uint32_t genbits_vld = 0;
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (
      !bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));
  CHECK(!bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_FIPS_BIT),
        "Expected GENBITS_FIPS == 0 when CTRL.FIPS_FORCE_ENABLE is False, got "
        "0x%x",
        genbits_vld);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));

  // 5. Verify UNINSTANTIATE zeroizes RESEED_COUNTER_2 and all 14 words of
  // INT_STATE_VAL (including Key[0..7] and Status/FIPS word 13)
  // (csrng_core.sv:1765-1771, csrng_state_db.sv:178, 187-201).
  uint32_t rc2_before =
      abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET);
  CHECK(rc2_before == 1u,
        "Expected RESEED_COUNTER_2 == 1 after 1 GENERATE cmd, got %u",
        rc2_before);

  CHECK_DIF_OK(dif_csrng_uninstantiate(&csrng));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));

  uint32_t rc2_after =
      abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET);
  CHECK(rc2_after == 0u,
        "Expected RESEED_COUNTER_2 == 0 after UNINSTANTIATE, got %u",
        rc2_after);

  uint32_t ctrl_read_int_on =
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_on);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  for (uint32_t i = 0; i < 14; ++i) {
    uint32_t st_word =
        abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
    CHECK(st_word == 0u,
          "Expected INT_STATE_VAL word %u == 0 after UNINSTANTIATE, got 0x%08x",
          i, st_word);
  }

  // 6. Verify GENBITS_VLD is NOT gated by CTRL.SW_APP_ENABLE, and reading
  // GENBITS while CTRL.SW_APP_ENABLE == kMultiBitBool4False returns 0 while
  // still popping the 32-bit packer FIFO (csrng_core.sv:948-952, 968-983).
  const dif_csrng_seed_material_t kSeed2 = {
      .seed_material = {0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
                        0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321,
                        0xa5a5a5a5, 0x5a5a5a5a, 0xdeadbeef, 0xcafebabe},
      .seed_material_len = 12,
  };
  CHECK_DIF_OK(
      dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable, &kSeed2));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (
      !bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));

  uint32_t ctrl_sw_app_off =
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_sw_app_off);

  genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  CHECK(bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT),
        "Expected GENBITS_VLD == 1 even when CTRL.SW_APP_ENABLE is False");

  for (int i = 0; i < 4; ++i) {
    uint32_t gb = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
    CHECK(gb == 0u,
          "Expected GENBITS read %d == 0 when SW_APP_ENABLE is False, got "
          "0x%08x",
          i, gb);
  }
  genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  CHECK(!bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT),
        "Expected GENBITS_VLD == 0 after 4 reads of GENBITS even when "
        "SW_APP_ENABLE is False");

  // 7. Verify disabling CTRL.ENABLE clears cs_rdata_capt_vld_q so generating
  // from the same deterministic seed kSeed2 after re-enabling does NOT raise
  // CS_BUS_CMP_ALERT (csrng_core.sv:1008-1015).
  uint32_t ctrl_disabled =
      (kMultiBitBool4False << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_disabled);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  CHECK_DIF_OK(
      dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable, &kSeed2));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (
      !bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(recov_sts,
                             CSRNG_RECOV_ALERT_STS_CS_BUS_CMP_ALERT_BIT),
        "Expected CS_BUS_CMP_ALERT == 0 after CTRL.ENABLE toggle, got 0x%x",
        recov_sts);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }

  // 8a. Verify UNINSTANTIATE does NOT clear cs_rdata_capt_vld_q
  // (csrng_core.sv:1008-1015): re-instantiating with identical seed kSeed2 and
  // generating 1 block triggers RECOV_ALERT_STS.CS_BUS_CMP_ALERT (bit 12).
  CHECK_DIF_OK(dif_csrng_uninstantiate(&csrng));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(
      dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable, &kSeed2));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (
      !bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            CSRNG_RECOV_ALERT_STS_CS_BUS_CMP_ALERT_BIT),
        "Expected CS_BUS_CMP_ALERT after UNINSTANTIATE + re-instantiate with "
        "identical seed, got 0x%x",
        recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }

  // 8b. Verify any CTRL.ENABLE != kMultiBitBool4True (e.g. invalid MuBi4 0x0)
  // disables CSRNG (SW_CMD_STS.CMD_RDY == 0) AND clears cs_rdata_capt_vld_q
  // (csrng_core.sv:787-789, 1008-1011).
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdCsrngRecovAlert));
  uint32_t ctrl_bad_en_mubi =
      (0x0u << CSRNG_CTRL_ENABLE_OFFSET) |
      (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
      (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_bad_en_mubi);
  sw_cmd_sts = abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(sw_cmd_sts, CSRNG_SW_CMD_STS_CMD_RDY_BIT),
        "Expected SW_CMD_STS.CMD_RDY == 0 when CTRL.ENABLE is 0x0, got 0x%x",
        sw_cmd_sts);
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            CSRNG_RECOV_ALERT_STS_ENABLE_FIELD_ALERT_BIT),
        "Expected ENABLE_FIELD_ALERT when CTRL.ENABLE is 0x0, got 0x%x",
        recov_sts);
  // Writing 0 (rw0c) while CTRL still holds invalid mubi4 0x0 must preserve bit
  // 0!
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            CSRNG_RECOV_ALERT_STS_ENABLE_FIELD_ALERT_BIT),
        "Expected ENABLE_FIELD_ALERT preserved across rw0c while CTRL.ENABLE "
        "is 0x0, got 0x%x",
        recov_sts);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_off);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET) == 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdCsrngRecovAlert));

  // Re-instantiate with identical seed kSeed2 and generate 1 block: since
  // disabling CSRNG cleared cs_rdata_capt_vld_q, CS_BUS_CMP_ALERT must remain
  // 0.
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(
      dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleDisable, &kSeed2));
  CHECK_STATUS_OK(csrng_testutils_cmd_ready_wait(&csrng));
  CHECK_DIF_OK(dif_csrng_generate_start(&csrng, 4));
  do {
    genbits_vld = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET);
  } while (
      !bitfield_bit32_read(genbits_vld, CSRNG_GENBITS_VLD_GENBITS_VLD_BIT));
  recov_sts = abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(recov_sts == 0u,
        "Expected RECOV_ALERT_STS == 0 after CTRL.ENABLE disable cleared "
        "cs_rdata_capt_vld_q, got 0x%x",
        recov_sts);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }

  // 9. Verify csrng_state_db.sv reg_rd_ptr_q == 4'hf when INT_STATE_NUM is
  // written while CTRL.READ_INT_STATE == False (state_db_is_dump_en_i == 0):
  // writing INT_STATE_NUM = 2 latches int_st_dump_id_q = 2 while keeping
  // reg_rd_ptr_q = 4'hf, so after enabling CTRL.READ_INT_STATE = True without
  // re-writing INT_STATE_NUM, the 1st read of INT_STATE_VAL (ptr == 4'hf)
  // returns 0 and advances ptr to 4'h0, and the 2nd read (ptr == 4'h0,
  // reseed_counter of instance 2) returns 1.
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, ctrl_read_int_on);
  uint32_t st_ptr_f =
      abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  uint32_t st_ptr_0 =
      abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  CHECK(st_ptr_f == 0u,
        "Expected 1st INT_STATE_VAL read (reg_rd_ptr_q == 4'hf) == 0, got "
        "0x%08x",
        st_ptr_f);
  CHECK(st_ptr_0 == 1u,
        "Expected 2nd INT_STATE_VAL read (reg_rd_ptr_q == 4'h0, rc) == 1, got "
        "0x%08x",
        st_ptr_0);

  // 10. Wave 5 Convergence Checks:
  // 10a. Unmapped offset 0x60..0x7c (within AW = 7 128-byte aperture) returns
  // addrmiss = 1'b1 (d_error = 1: Load Access Fault mcause = 5 on read, Store
  // Access Fault mcause = 7 on write).
  mmio_region_t csrng_mmio = mmio_region_from_addr(kCsrngBase);
  CHECK(expect_load_fault_32(csrng_mmio, 0x60u));
  CHECK(expect_store_fault_32(csrng_mmio, 0x60u, 0xdeadbeefu));

  // 10b. Sub-word write permit checks (CSRNG_PERMIT / wr_err):
  // - CTRL (PERMIT = 4'b0011) rejects 1-byte write (reg_be = 4'b0001) without
  //   modifying CTRL.
  // - RESEED_INTERVAL (PERMIT = 4'b1111) rejects 1-byte write (reg_be =
  // 4'b0001).
  // - REGWEN (PERMIT = 4'b0001) rejects byte write to offset +1 (reg_be =
  // 4'b0010).
  CHECK(expect_store_fault_8(csrng_mmio, CSRNG_CTRL_REG_OFFSET, 0x99u));
  CHECK(abs_mmio_read32(kCsrngBase + CSRNG_CTRL_REG_OFFSET) ==
        ctrl_read_int_on);
  CHECK(expect_store_fault_8(csrng_mmio, CSRNG_RESEED_INTERVAL_REG_OFFSET,
                             0x12u));
  CHECK(expect_store_fault_8(csrng_mmio, CSRNG_REGWEN_REG_OFFSET + 1u, 0x00u));

  return true;
}
