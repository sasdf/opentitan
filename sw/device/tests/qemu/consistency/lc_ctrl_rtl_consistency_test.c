// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "lc_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
  kLcAlertProgErr = kTopEarlgreyAlertIdLcCtrlFatalProgError,
};

static volatile bool g_fault_seen;
static volatile uint32_t g_fault_mcause;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

bool test_main(void) {
  uint32_t mismatches = 0;

  // 1. Verify initial STATUS, LC_STATE, LC_TRANSITION_CNT, HW_REVISION0/1.
  uint32_t status = abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET);
  uint32_t expected_status =
      (1u << LC_CTRL_STATUS_INITIALIZED_BIT) | (1u << LC_CTRL_STATUS_READY_BIT);
  if (status != expected_status) {
    LOG_ERROR("RTL_MISMATCH: initial STATUS=0x%08x (expected 0x%08x)", status,
              expected_status);
    mismatches++;
  }

  uint32_t lc_state =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_STATE_REG_OFFSET);
  uint32_t lc_cnt =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_TRANSITION_CNT_REG_OFFSET);
  uint32_t hw_rev0 =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_HW_REVISION0_REG_OFFSET);
  uint32_t hw_rev1 =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_HW_REVISION1_REG_OFFSET);
  LOG_INFO("LC_STATE=0x%08x LC_CNT=%u HW_REV0=0x%08x HW_REV1=0x%08x", lc_state,
           lc_cnt, hw_rev0, hw_rev1);
  CHECK(hw_rev0 != 0 && hw_rev1 != 0);

  // 2. Verify ALERT_TEST is a transient pulse (lc_ctrl.sv:573-606), so repeated
  // writes of the same bit to ALERT_TEST trigger the alert each time.
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(kLcAlertProgErr));
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << LC_CTRL_ALERT_TEST_FATAL_PROG_ERROR_BIT);
  status_t st1 = ottf_alerts_expect_alert_finish(kLcAlertProgErr);

  // Write the same bit to ALERT_TEST a second time without writing 0 in between
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(kLcAlertProgErr));
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << LC_CTRL_ALERT_TEST_FATAL_PROG_ERROR_BIT);
  status_t st2 = ottf_alerts_expect_alert_finish(kLcAlertProgErr);

  if (!status_ok(st1) || !status_ok(st2)) {
    LOG_ERROR(
        "RTL_MISMATCH: ALERT_TEST repeated write ok1=%u ok2=%u "
        "(expected 1, 1)",
        status_ok(st1), status_ok(st2));
    mismatches++;
  }

  // 3. Verify CLAIM_TRANSITION_IF initial state and arbitrary 8-bit mubi8_t
  // storage/readback (lc_ctrl.sv:345, 386).
  uint32_t claim_regwen = abs_mmio_read32(
      kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET);
  uint32_t claim_init =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET);
  uint32_t trans_regwen_init =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET);
  if (claim_regwen != 1u || claim_init != kMultiBitBool8False ||
      trans_regwen_init != 0u) {
    LOG_ERROR("RTL_MISMATCH: init claim_regwen=%u claim=0x%02x trans_regwen=%u",
              claim_regwen, claim_init, trans_regwen_init);
    mismatches++;
  }

  // Write non-MuBi8True/False values (0x00, 0x5a, 0xff) to CLAIM_TRANSITION_IF.
  // In RTL (lc_ctrl.sv:345, 386), sw_claim_transition_if_q stores any 8-bit
  // value written and returns it on readback, while TRANSITION_REGWEN is 1 only
  // when mubi8_test_true_strict(sw_claim_transition_if_q) (0x69) is true.
  const uint32_t test_claims[] = {0x00u, 0x5au, 0xffu};
  for (size_t i = 0; i < sizeof(test_claims) / sizeof(test_claims[0]); ++i) {
    abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                     test_claims[i]);
    uint32_t rd_claim =
        abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET);
    uint32_t rd_tregwen =
        abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET);
    if (rd_claim != test_claims[i] || rd_tregwen != 0u) {
      LOG_ERROR(
          "RTL_MISMATCH: CLAIM_TRANSITION_IF write 0x%02x read back 0x%02x "
          "(expected 0x%02x), TRANSITION_REGWEN=%u (expected 0)",
          test_claims[i], rd_claim, test_claims[i], rd_tregwen);
      mismatches++;
    }
  }

  // 4. Claim the mutex strictly with kMultiBitBool8True (0x69).
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8True);
  if (abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) !=
          kMultiBitBool8True ||
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) !=
          1u) {
    LOG_ERROR("RTL_MISMATCH: failed to claim mutex with MuBi8True (0x69)");
    mismatches++;
  }

  // 5. Test TRANSITION_TARGET arbitrary 30-bit storage and readback while
  // holding the mutex (lc_ctrl.sv:359, 451-456).
  // In RTL, transition_target_q stores any 30-bit value written when
  // TRANSITION_REGWEN == 1; validity is only checked if TRANSITION_CMD is set.
  const uint32_t target_patterns[] = {
      0x12345678u,  // arbitrary non-replicated 30-bit value
      0x3fffffffu,  // all 1s in 30-bit field
      0x2b39ce52u,  // LC_ENC_STATE_POST_TRANSITION
      0x2739ce73u,  // LC_ENC_STATE_RMA (valid state)
      0x00000000u,  // LC_ENC_STATE_RAW (valid state)
  };
  for (size_t i = 0; i < sizeof(target_patterns) / sizeof(target_patterns[0]);
       ++i) {
    abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                     target_patterns[i]);
    uint32_t rd_target =
        abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET);
    if (rd_target != target_patterns[i]) {
      LOG_ERROR(
          "RTL_MISMATCH: TRANSITION_TARGET write 0x%08x read back 0x%08x "
          "(expected 0x%08x)",
          target_patterns[i], rd_target, target_patterns[i]);
      mismatches++;
    }
  }

  // 6. Test TRANSITION_TOKEN_0..3, TRANSITION_TARGET, and OTP_VENDOR_TEST_CTRL
  // readback gating when mutex is released vs re-claimed (lc_ctrl.sv:355-364).
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET,
                   0xcafebabeu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET,
                   0xa5a5a5a5u);

  // Release mutex by writing kMultiBitBool8False (0x96).
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8False);
  if (abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) !=
          0u ||
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) !=
          0u ||
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) !=
          0u) {
    LOG_ERROR(
        "RTL_MISMATCH: mutex-gated CSRs not masked to 0 when mutex is "
        "released");
    mismatches++;
  }

  // Re-claim mutex and verify underlying flops preserved their values.
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8True);
  uint32_t tok0_re =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET);
  uint32_t tgt_re =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET);
  uint32_t vctrl_re =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET);
  if (tok0_re != 0xdeadbeefu || tgt_re != 0x12345678u ||
      vctrl_re != 0xa5a5a5a5u) {
    LOG_ERROR(
        "RTL_MISMATCH: re-claimed CSRs tok0=0x%08x tgt=0x%08x vctrl=0x%08x "
        "(expected 0xdeadbeef, 0x12345678, 0xa5a5a5a5)",
        tok0_re, tgt_re, vctrl_re);
    mismatches++;
  }

  // 6b. Wave 5 checks: unmapped offset 0x8c (addrmiss = 1 -> Load/Store Access
  // Fault) and LC_CTRL_PERMIT sub-word writes (wr_err = 1 -> Store Access
  // Fault, preserving registers).
  g_fault_seen = false;
  (void)abs_mmio_read32(kLcCtrlBase + 0x8cu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  abs_mmio_write32(kLcCtrlBase + 0x8cu, 0x12345678u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  abs_mmio_write8(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET, 0x11u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
        0xdeadbeefu);

  g_fault_seen = false;
  *(volatile uint16_t *)(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) =
      0x2222u;
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
        0x12345678u);

  // 8-bit store to CLAIM_TRANSITION_IF (LC_CTRL_PERMIT[3] = 4'b0001) must
  // succeed.
  g_fault_seen = false;
  abs_mmio_write8(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                  kMultiBitBool8True);
  CHECK(!g_fault_seen);

  // On Earlgrey (top_pkg::SecVolatileRawUnlockEn = 0, lc_ctrl.sv:534),
  // volatile_raw_unlock_q is tied to 1'b0, so writing VOLATILE_RAW_UNLOCK=1
  // to TRANSITION_CTRL must read back 0.
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET,
                   1u << LC_CTRL_TRANSITION_CTRL_VOLATILE_RAW_UNLOCK_BIT);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET) ==
        0u);

  // Restore clean state before locking CLAIM_TRANSITION_IF_REGWEN.
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET, 0u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET, 0u);

  // 7. Lock CLAIM_TRANSITION_IF_REGWEN (rw0c) and verify CLAIM_TRANSITION_IF
  // cannot be modified afterwards.
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET,
                   0u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8False);
  if (abs_mmio_read32(kLcCtrlBase +
                      LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET) != 0u ||
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) !=
          kMultiBitBool8True) {
    LOG_ERROR(
        "RTL_MISMATCH: CLAIM_TRANSITION_IF modified when "
        "CLAIM_TRANSITION_IF_REGWEN=0");
    mismatches++;
  }

  CHECK(mismatches == 0,
        "lc_ctrl RTL consistency test failed with %u mismatches", mismatches);
  return true;
}
