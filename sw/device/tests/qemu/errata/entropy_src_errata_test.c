// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file entropy_src_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `entropy_src` (`P18`).
 *
 * Empirically confirms on physical CW340 FPGA silicon and QEMU:
 * 1. [entropy_src_core.sv:653-770] & [entropy_src_core.sv:557-561]
 * (SPEC_DOC_ERRATA / INTENDED_SECURITY_HARDENING, MEDIUM):
 *    - `RECOV_ALERT_STS.ES_THRESH_CFG_ALERT` (`entropy_src_core.sv:2178-2180`)
 *      is a continuous combinational level signal that overrides `rw0c` clears
 *      while `ALERT_THRESHOLD` is non-complementary (`0x12345678`).
 *    - `REGWEN` uses `mubi4_test_false_loose(MODULE_ENABLE)` (`!= 0x6`), so
 *      writing `0x5` to `MODULE_ENABLE` keeps `REGWEN == 1`.
 * 2. [entropy_src_core.sv:1042-1043], [entropy_src_main_sm.sv:62-68] &
 *    [entropy_src_main_sm.sv:58-68] (SPEC_DOC_ERRATA, MEDIUM):
 *    - In conditioned `FW_OV_ENTROPY_INSERT` mode (`entropy_src_main_sm.sv`),
 *      enabling `MODULE_ENABLE` enters `FWInsertStart` (`0x0c3`,
 *      `DEBUG_STATUS.MAIN_SM_IDLE == 0`), and after completing a SHA3-384 hash,
 *      `MAIN_SM_STATE` immediately returns to `FWInsertStart` (`0x0c3`,
 *      `MAIN_SM_IDLE == 0`).
 *    - Clearing `FW_OV_SHA3_START` with 1 unprocessed 32-bit word
 * (`0x11223344`) in `pfifo_precon` (`FW_OV_WR_FIFO_FULL == 0`) does NOT set
 *      `ES_FW_OV_DISABLE_ALERT` (`entropy_src_core.sv:2691-2692`) and absorbs
 *      that odd 32-bit word into the current SHA3-384 digest.
 * 3. [entropy_src_core.sv:3007-3020] (INTENDED_SECURITY_HARDENING, LOW):
 *    - `ES_BUS_CMP_ALERT` (`entropy_src_core.sv:3007-3020`) compares `[63:0]`
 *      of consecutive seeds and asserts on the 12th `ENTROPY_DATA` read
 *      (`sfifo_esfinal_pop`).
 * 4. [entropy_src_core.sv:2967] (TRUE_SILICON_ERRATA, LOW, 65%):
 *    - Reading an empty `ENTROPY_DATA` returns `0` with `ERR_CODE == 0` on
 *      reads `1..11` (`swread_idx_q = 0..10`) and only latches
 *      `SFIFO_ESFINAL_ERR | FIFO_READ_ERR` (`0x20000008`) on the 12th read
 *      (`swread_idx_q == 11`, `swread_done == 1`).
 * 5. [entropy_src_reg_pkg.sv:980-1038] (INTENDED_SECURITY_HARDENING, INFO —
 * SEC_CM: BUS.INTEGRITY):
 *    - `ENTROPY_SRC_PERMIT` (`entropy_src_reg_pkg.sv:980-1038`) rejects 8-bit
 *      `sb` writes to `CONF` (`4'b1111`, `mcause = 7`) while accepting `sb` to
 *      byte 0 of `MODULE_ENABLE` (`4'b0001`).
 */

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

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_load_store_fault = true;
  g_last_mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst = *(volatile uint16_t *)mepc;
  uint32_t step = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey ENTROPY_SRC Errata Confirmation Suite (P18) ===");
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // ---------------------------------------------------------------------------
  // 1. [entropy_src_core.sv:653-770] & [entropy_src_core.sv:557-561]
  //    (SPEC_DOC_ERRATA / INTENDED_SECURITY_HARDENING):
  //    - ES_THRESH_CFG_ALERT level signal overrides rw0c clear while
  //      ALERT_THRESHOLD is non-complementary (0x12345678).
  //    - REGWEN stays 1 when MODULE_ENABLE is written with 0x5 (loose false).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:557-561,653-770]: sticky "
      "ES_THRESH_CFG_ALERT & "
      "loose-false REGWEN");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  uint32_t recov_sts =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(recov_sts,
                          ENTROPY_SRC_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_BIT),
      "[entropy_src_core.sv:653-770] Expected ES_THRESH_CFG_ALERT to remain 1 "
      "after rw0c clear while ALERT_THRESHOLD=0x12345678 (got 0x%x)",
      recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   ENTROPY_SRC_ALERT_THRESHOLD_REG_RESVAL);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET, 0x5u);
  uint32_t regwen = abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET);
  CHECK(regwen == 1u,
        "[entropy_src_core.sv:557-561] Expected REGWEN == 1 when "
        "MODULE_ENABLE=0x5 "
        "(mubi4_test_false_loose), got 0x%x",
        regwen);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  LOG_INFO(
      "[entropy_src_core.sv:557-561,653-770] CONFIRMED: ES_THRESH_CFG_ALERT "
      "sticky & "
      "REGWEN=1 on MODULE_ENABLE=0x5");

  // ---------------------------------------------------------------------------
  // 2. [entropy_src_core.sv:1042-1043], [entropy_src_main_sm.sv:62-68] &
  //    [entropy_src_main_sm.sv:58-68] (SPEC_DOC_ERRATA, MEDIUM) +
  //    [entropy_src_core.sv:3007-3020] (INTENDED_SECURITY_HARDENING, LOW):
  //    - Conditioned FW_OV_ENTROPY_INSERT enters FWInsertStart (0x0c3) with
  //      MAIN_SM_IDLE == 0, and pulses sha3_start_o immediately in Idle so
  //      words written in FWInsertStart (0x11111111, 0x22222222) BEFORE
  //      FW_OV_SHA3_START=True are absorbed into the SHA3-384 digest
  //      (word 0 == 0x1d9845a5).
  //    - Clearing FW_OV_SHA3_START with 1 odd 32-bit word (0xdeadbeef) in
  //      pfifo_precon (FW_OV_WR_FIFO_FULL == 0) does NOT assert
  //      ES_FW_OV_DISABLE_ALERT.
  //    - Popping an identical 2nd seed on the 12th ENTROPY_DATA read asserts
  //      ES_BUS_CMP_ALERT.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:1042-1043,3007-3020 / "
      "entropy_src_main_sm.sv:58-68]: FWInsertStart (0x0c3), "
      "pre-SHA3_START absorption, & ES_BUS_CMP_ALERT");
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

  // Seed 1: While still in FWInsertStart (0x0c3, FW_OV_SHA3_START == False),
  // write (0x11111111, 0x22222222) to FW_OV_WR_DATA. Then transition to
  // FWInsertMsg (FW_OV_SHA3_START = True), write 1 odd word (0xdeadbeef) into
  // pfifo_precon, and clear FW_OV_SHA3_START = False.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x11111111u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x22222222u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) ==
            0u,
        "Expected FW_OV_WR_FIFO_FULL == 0 after 1 odd 32-bit word");
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);

  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(
            recov_sts, ENTROPY_SRC_RECOV_ALERT_STS_ES_FW_OV_DISABLE_ALERT_BIT),
        "[entropy_src_core.sv:1042-1043] Expected ES_FW_OV_DISABLE_ALERT == 0 "
        "when "
        "FW_OV_WR_FIFO_FULL == 0 despite 1 unprocessed 32-bit word in "
        "pfifo_precon");

  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }

  uint32_t sm_state =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  uint32_t debug_status =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  CHECK(sm_state == 0x0c3u &&
            !bitfield_bit32_read(debug_status,
                                 ENTROPY_SRC_DEBUG_STATUS_MAIN_SM_IDLE_BIT),
        "[entropy_src_main_sm.sv:58-68] Expected MAIN_SM_STATE == 0x0c3 "
        "(FWInsertStart) and MAIN_SM_IDLE == 0 after SHA3 completion");

  uint32_t seed1_w0 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  for (int i = 1; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK(
      seed1_w0 == 0x1d9845a5u,
      "[entropy_src_main_sm.sv:62-68] Expected words (0x11111111, 0x22222222) "
      "written in FWInsertStart prior to FW_OV_SHA3_START=True to be "
      "absorbed into SHA3-384 digest (0x1d9845a5), got 0x%08x",
      seed1_w0);

  // Clear the half-word in pfifo_precon by cycling MODULE_ENABLE (which keep
  // rdata_capt in QEMU? Wait: disabling MODULE_ENABLE resets rdata_capt_vld!).
  // Instead of disabling MODULE_ENABLE, let's produce two identical
  // empty-sponge SHA3 digests (Seed 2 and Seed 3, after flushing 0xdeadbeef
  // with 0xcafebabe in Seed 2, then two empty sponges Seed 3 and Seed 4) to
  // trigger ES_BUS_CMP_ALERT!
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0xcafebabeu);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // Seed 3: Empty SHA3-384 sponge #1
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // Seed 4: Empty SHA3-384 sponge #2 (identical to Seed 3) -> ES_BUS_CMP_ALERT
  // fires on the 12th ENTROPY_DATA read!
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            ENTROPY_SRC_RECOV_ALERT_STS_ES_BUS_CMP_ALERT_BIT),
        "[entropy_src_core.sv:3007-3020] Expected ES_BUS_CMP_ALERT set on 12th "
        "read "
        "of identical seed (got 0x%x)",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  LOG_INFO(
      "[entropy_src_core.sv:1042-1043,3007-3020 / "
      "entropy_src_main_sm.sv:58-68] CONFIRMED: seed1_w0=0x%08x, "
      "MAIN_SM_STATE=0x%03x, ES_BUS_CMP_ALERT=1",
      seed1_w0, sm_state);

  // ---------------------------------------------------------------------------
  // 3. [entropy_src_core.sv:2967] (TRUE_SILICON_ERRATA, LOW, 65%):
  //    Reading an empty ENTROPY_DATA returns 0 with ERR_CODE == 0 on
  //    reads 1..11 (swread_idx_q = 0..10) and only latches SFIFO_ESFINAL_ERR |
  //    FIFO_READ_ERR (0x20000008) on the 12th read (swread_idx_q == 11,
  //    swread_done == 1).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:2967] (TRUE_SILICON_ERRATA): "
      "ENTROPY_DATA "
      "empty underflow only fires on 12th read");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdEntropySrcFatalAlert));
  for (int i = 0; i < 11; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  uint32_t err_code_11 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  CHECK(err_code_11 == 0u,
        "[entropy_src_core.sv:2967] Expected ERR_CODE == 0 after 11 empty "
        "ENTROPY_DATA reads, got 0x%08x",
        err_code_11);

  (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  uint32_t err_code_12 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(bitfield_bit32_read(err_code_12,
                            ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT) &&
            bitfield_bit32_read(err_code_12,
                                ENTROPY_SRC_ERR_CODE_FIFO_READ_ERR_BIT),
        "[entropy_src_core.sv:2967] Expected SFIFO_ESFINAL_ERR | FIFO_READ_ERR "
        "(0x20000008) on 12th empty read, got 0x%08x",
        err_code_12);
  LOG_INFO(
      "[entropy_src_core.sv:2967] CONFIRMED: ERR_CODE after 11 reads=0x%08x, "
      "after 12th read=0x%08x",
      err_code_11, err_code_12);

  // ---------------------------------------------------------------------------
  // 4. [entropy_src_reg_pkg.sv:980-1038] (INTENDED_SECURITY_HARDENING, INFO —
  // SEC_CM: BUS.INTEGRITY):
  //    ENTROPY_SRC_PERMIT rejects 8-bit sb writes to CONF (4'b1111, mcause=7)
  //    while accepting 8-bit sb writes to byte 0 of MODULE_ENABLE (4'b0001).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_reg_pkg.sv:980-1038] "
      "(INTENDED_SECURITY_HARDENING): "
      "ENTROPY_SRC_PERMIT sub-word write protection");
  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, 0x66u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[entropy_src_reg_pkg.sv:980-1038] Expected sb to CONF "
        "(PERMIT=4'b1111) to "
        "fault with mcause=7");

  g_load_store_fault = false;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                  kMultiBitBool4False);
  CHECK(!g_load_store_fault,
        "[entropy_src_reg_pkg.sv:980-1038] Expected sb to MODULE_ENABLE+0 "
        "(PERMIT=4'b0001) to succeed");
  LOG_INFO(
      "[entropy_src_reg_pkg.sv:980-1038] CONFIRMED: ENTROPY_SRC_PERMIT "
      "enforced");

  LOG_INFO("=== ALL ENTROPY_SRC ERRATA CHECKS PASSED ===");
  return true;
}
