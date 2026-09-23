// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "lc_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kLcCtrlBase = 0x40140000u,
  kLcCtrlUnmappedOffset = 0x8cu,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_bus_fault_seen = false;
static volatile uint32_t g_bus_fault_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  CHECK(g_expect_bus_fault, "Unexpected load/store fault: mcause=0x%x", mcause);
  g_bus_fault_seen = true;
  g_bus_fault_mcause = mcause;
}

static void expect_load_fault(uint32_t addr) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  (void)abs_mmio_read32(addr);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Load Access Fault (mcause=5) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcLoadAccessFault,
        "Expected mcause=5, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store32_fault(uint32_t addr, uint32_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  abs_mmio_write32(addr, val);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Store Access Fault (mcause=7) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store16_fault(uint32_t addr, uint16_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  asm volatile("sh %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen,
        "Expected Store Access Fault (mcause=7) on 16-bit write at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store8_fault(uint32_t addr, uint8_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  asm volatile("sb %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen,
        "Expected Store Access Fault (mcause=7) on 8-bit write at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

/**
 * [lc_ctrl.sv:140] (SPEC_DOC_ERRATA + INTENDED_SECURITY_HARDENING):
 * Verify that `CLAIM_TRANSITION_IF` resets to `0x69` (`kMultiBitBool8False`,
 * `lc_ctrl.sv:474-475`) rather than `0` (`programmers_guide.md:12`), stores
 * arbitrary 8-bit values when `tap_claim_transition_if_q == MuBi8False` while
 * keeping `TRANSITION_REGWEN == 0`, activates `TRANSITION_REGWEN == 1` strictly
 * on `kMultiBitBool8True` (`0x96`), and masks `TRANSITION_TOKEN_*`,
 * `TRANSITION_TARGET`, and `OTP_VENDOR_TEST_CTRL` readbacks to `0` when
 * unclaimed without destroying the underlying flip-flops.
 */
static void test_lc_ctrl_claim_mutex_and_masking(void) {
  LOG_INFO(
      "Verifying [lc_ctrl.sv:140] (SPEC_DOC_ERRATA): "
      "CLAIM_TRANSITION_IF reset 0x69 (MuBi8False) and unclaimed read masking");

  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET) == 1u,
        "CLAIM_TRANSITION_IF_REGWEN must reset to 1");
  uint32_t reset_claim =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET);
  CHECK(reset_claim == (uint32_t)kMultiBitBool8False,
        "[lc_ctrl.sv:140] Expected CLAIM_TRANSITION_IF reset=0x69 "
        "(MuBi8False), got 0x%x",
        reset_claim);
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) == 0u,
      "TRANSITION_REGWEN must be 0 when CLAIM_TRANSITION_IF == 0x69");

  /* When unclaimed by TAP
   * (`mubi8_test_false_loose(tap_claim_transition_if_q)`),
   * `sw_claim_transition_if_q` accepts any 8-bit value (`0x00`, `0x5a`,
   * `0xff`), returns it verbatim on readback, and keeps `TRANSITION_REGWEN ==
   * 0`. */
  const uint8_t kNonTrueVals[] = {0x00u, 0x5au, 0xffu};
  for (size_t i = 0; i < ARRAYSIZE(kNonTrueVals); ++i) {
    abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                     kNonTrueVals[i]);
    uint32_t rb =
        abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET);
    CHECK(rb == kNonTrueVals[i],
          "[lc_ctrl.sv:140] Expected CLAIM_TRANSITION_IF readback 0x%02x, "
          "got 0x%02x",
          kNonTrueVals[i], rb);
    CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) ==
              0u,
          "TRANSITION_REGWEN must remain 0 for non-MuBi8True value 0x%02x",
          kNonTrueVals[i]);
  }

  /* Claim mutex with `kMultiBitBool8True` (`0x96`) -> `TRANSITION_REGWEN == 1`.
   */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   (uint32_t)kMultiBitBool8True);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) ==
            (uint32_t)kMultiBitBool8True,
        "Expected CLAIM_TRANSITION_IF == 0x96 (MuBi8True)");
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) == 1u,
      "TRANSITION_REGWEN must be 1 when CLAIM_TRANSITION_IF == 0x96");

  /* Program token, target, and OTP vendor test control while claimed. */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET,
                   0x55667788u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_2_REG_OFFSET,
                   0x99aabbccu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_3_REG_OFFSET,
                   0xddeeff00u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                   0x15a5a5a5u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET,
                   0xa5a55a5au);

  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
        0x11223344u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET) ==
        0x55667788u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_2_REG_OFFSET) ==
        0x99aabbccu);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_3_REG_OFFSET) ==
        0xddeeff00u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
        0x15a5a5a5u);
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) ==
      0xa5a55a5au);

  /* Release mutex (`0x69`): `TRANSITION_REGWEN == 0` and readbacks are masked
   * to `0`, while writes are ignored without corrupting the stored flip-flops.
   */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   (uint32_t)kMultiBitBool8False);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_2_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_3_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) == 0u);

  /* Attempt writes while unclaimed (`TRANSITION_REGWEN == 0`). */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                   0x3fffffffu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET,
                   0xffffffffu);

  /* Re-claim (`0x96`) and confirm underlying flip-flops were preserved! */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   (uint32_t)kMultiBitBool8True);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
        0x11223344u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
        0x15a5a5a5u);
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) ==
      0xa5a55a5au);
}

/**
 * [lc_ctrl_state_decode.sv:50-66] (INTENDED_SECURITY_HARDENING):
 * Verify operational state decoding (`STATUS`, `LC_STATE`, `LC_TRANSITION_CNT`,
 * `LC_ID_STATE` 32-bit 8x4-bit nibbles `0x00000000` `BLANK` or `0x55555555`
 * `PERSONALIZED`), `SecVolatileRawUnlockEn = 0` (`VOLATILE_RAW_UNLOCK` bit
 * hardwired to `0`), and `EXT_CLOCK_EN` (`rw1s`) sticky behavior.
 */
static void test_lc_ctrl_state_decode_and_transition_ctrl(void) {
  LOG_INFO(
      "Verifying [lc_ctrl_state_decode.sv:50-66] "
      "(INTENDED_SECURITY_HARDENING): "
      "LC_STATE/LC_ID_STATE decoding and SecVolatileRawUnlockEn=0");

  uint32_t status = abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET);
  uint32_t expected_ready =
      (1u << LC_CTRL_STATUS_INITIALIZED_BIT) | (1u << LC_CTRL_STATUS_READY_BIT);
  CHECK((status & expected_ready) == expected_ready,
        "Expected STATUS.INITIALIZED and STATUS.READY set (0x3), got 0x%08x",
        status);

  uint32_t lc_state =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_STATE_REG_OFFSET);
  CHECK((lc_state & ~0x3fffffffu) == 0u,
        "LC_STATE top 2 bits must be 0, got 0x%08x", lc_state);
  uint32_t group0 = lc_state & 0x1fu;
  for (int g = 1; g < 6; ++g) {
    uint32_t group_g = (lc_state >> (g * 5)) & 0x1fu;
    CHECK(group_g == group0,
          "LC_STATE 5-bit group %d (0x%02x) must equal group 0 (0x%02x) in "
          "0x%08x",
          g, group_g, group0, lc_state);
  }

  uint32_t lc_cnt =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_TRANSITION_CNT_REG_OFFSET);
  CHECK(lc_cnt < 31u, "Expected LC_TRANSITION_CNT < 31, got %u", lc_cnt);

  uint32_t lc_id_state =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_ID_STATE_REG_OFFSET);
  CHECK(lc_id_state == 0x00000000u || lc_id_state == 0x55555555u,
        "Expected LC_ID_STATE to be BLANK (0x0) or PERSONALIZED (0x55555555), "
        "got 0x%08x",
        lc_id_state);

  /* Verify `SecVolatileRawUnlockEn = 0`: `VOLATILE_RAW_UNLOCK` (bit 1) is
   * permanently tied to `0`, while `EXT_CLOCK_EN` (bit 0) is `rw1s`. */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET,
                   (1u << LC_CTRL_TRANSITION_CTRL_VOLATILE_RAW_UNLOCK_BIT));
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET) == 0u,
      "[lc_ctrl_state_decode.sv:50-66] Expected VOLATILE_RAW_UNLOCK tied to 0 "
      "(SecVolatileRawUnlockEn=0)");

  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET,
                   (1u << LC_CTRL_TRANSITION_CTRL_EXT_CLOCK_EN_BIT) |
                       (1u << LC_CTRL_TRANSITION_CTRL_VOLATILE_RAW_UNLOCK_BIT));
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET) ==
            (1u << LC_CTRL_TRANSITION_CTRL_EXT_CLOCK_EN_BIT),
        "Expected EXT_CLOCK_EN=1 and VOLATILE_RAW_UNLOCK=0");
  /* Verify `rw1s` cannot be cleared by writing 0. */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET) ==
            (1u << LC_CTRL_TRANSITION_CTRL_EXT_CLOCK_EN_BIT),
        "Expected EXT_CLOCK_EN (rw1s) to remain sticky at 1");
}

/**
 * [lc_ctrl.sv:359] (INTENDED_SECURITY_HARDENING):
 * Verify that `TRANSITION_TARGET` (`lc_ctrl.sv:359, 451-456`) stores any raw
 * 30-bit value unconditionally when `TRANSITION_REGWEN == 1` and returns it
 * verbatim (`val & 0x3fffffff`), deferring sparse replication validation until
 * `TRANSITION_CMD.START = 1`.
 */
static void test_lc_ctrl_unfiltered_transition_target(void) {
  LOG_INFO(
      "Verifying [lc_ctrl.sv:359] (INTENDED_SECURITY_HARDENING): "
      "unfiltered 30-bit TRANSITION_TARGET storage & readback");

  const uint32_t kPatterns[] = {
      0x12345678u, 0x3fffffffu, 0x2b39ce52u, 0x2739ce73u, 0x00000000u,
  };
  for (size_t i = 0; i < ARRAYSIZE(kPatterns); ++i) {
    abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                     kPatterns[i]);
    uint32_t rb =
        abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET);
    CHECK(rb == (kPatterns[i] & 0x3fffffffu),
          "[lc_ctrl.sv:359] Expected TRANSITION_TARGET readback 0x%08x, "
          "got 0x%08x",
          kPatterns[i] & 0x3fffffffu, rb);
  }
}

/**
 * [lc_ctrl_reg_pkg.sv:341-377] (INTENDED_SECURITY_HARDENING):
 * Verify `LC_CTRL_PERMIT[35]` (`lc_ctrl_reg_pkg.sv:341-377`) sub-word write
 * byte-enable enforcement (`mcause = 7` on 1-byte `sb` to `TRANSITION_TOKEN_0`
 * or 2-byte `sh` to `TRANSITION_TARGET` where `permit = 4'b1111`, while
 * permitting 1-byte `sb` to `CLAIM_TRANSITION_IF` where `permit = 4'b0001`)
 * and `addrmiss` synchronous bus faults (`mcause = 5 / 7`) at offset `0x8c`.
 */
static void test_lc_ctrl_permit_and_addrmiss_faults(void) {
  LOG_INFO(
      "Verifying [lc_ctrl_reg_pkg.sv:341-377] (INTENDED_SECURITY_HARDENING): "
      "LC_CTRL_PERMIT[35] sub-word write faults and addrmiss at 0x8c");

  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                   0xcafebabeu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                   0x12345678u);

  /* 1-byte store (`sb`) to `TRANSITION_TOKEN_0` (`permit = 4'b1111`) -> fault!
   */
  expect_store8_fault(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                      0xaa);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
            0xcafebabeu,
        "TRANSITION_TOKEN_0 must remain unmodified after faulted sb");

  /* 2-byte store (`sh`) to `TRANSITION_TARGET` (`permit = 4'b1111`) -> fault!
   */
  expect_store16_fault(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                       0x5555);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
            0x12345678u,
        "TRANSITION_TARGET must remain unmodified after faulted sh");

  /* 1-byte store (`sb`) to `CLAIM_TRANSITION_IF` (`permit = 4'b0001`) -> OK! */
  asm volatile("sb %1, 0(%0)"
               :
               : "r"(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET),
                 "r"((uint8_t)kMultiBitBool8False)
               : "memory");
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) ==
        (uint32_t)kMultiBitBool8False);

  /* Unmapped offset `0x8c` (`addrmiss`) -> Load/Store Access Fault! */
  expect_load_fault(kLcCtrlBase + kLcCtrlUnmappedOffset);
  expect_store32_fault(kLcCtrlBase + kLcCtrlUnmappedOffset, 0xdeadbeefu);
}

/**
 * [lc_ctrl.sv:573-606] (BENIGN_RTL_IMPL_DETAIL):
 * Verify that `ALERT_TEST` (`lc_ctrl.sv:573-606`) generates a 1-cycle transient
 * pulse (`q & qe`) so repeated writes to `ALERT_TEST` fire the alert each time
 * without sticking high, and verify `CLAIM_TRANSITION_IF_REGWEN` (`rw0c`)
 * locks `CLAIM_TRANSITION_IF` permanently.
 */
static void test_lc_ctrl_alert_test_pulse_and_regwen_lock(void) {
  LOG_INFO(
      "Verifying [lc_ctrl.sv:573-606] (BENIGN_RTL_IMPL_DETAIL): "
      "ALERT_TEST 1-cycle transient pulse and CLAIM_TRANSITION_IF_REGWEN lock");

  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
        kTopEarlgreyAlertIdLcCtrlFatalProgError));
    abs_mmio_write32(kLcCtrlBase + LC_CTRL_ALERT_TEST_REG_OFFSET,
                     (1u << LC_CTRL_ALERT_TEST_FATAL_PROG_ERROR_BIT));
    CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
        kTopEarlgreyAlertIdLcCtrlFatalProgError));
  }

  /* Lock `CLAIM_TRANSITION_IF_REGWEN` (`rw0c` write 0) and confirm
   * `CLAIM_TRANSITION_IF` (`0x69`) can no longer be changed to `0x96`. */
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET,
                   0u);
  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET) == 0u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   (uint32_t)kMultiBitBool8True);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) ==
            (uint32_t)kMultiBitBool8False,
        "CLAIM_TRANSITION_IF must ignore writes after REGWEN=0");
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) ==
        0u);
}

bool test_main(void) {
  LOG_INFO("Starting lc_ctrl_errata_test (P23) on CW340 FPGA / QEMU...");
  test_lc_ctrl_claim_mutex_and_masking();
  test_lc_ctrl_state_decode_and_transition_ctrl();
  test_lc_ctrl_unfiltered_transition_target();
  test_lc_ctrl_permit_and_addrmiss_faults();
  test_lc_ctrl_alert_test_pulse_and_regwen_lock();
  LOG_INFO("All lc_ctrl_errata_test checks PASSED!");
  return true;
}
