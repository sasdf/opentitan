// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file otp_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `otp_ctrl` (P24).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [otp_ctrl_part_unbuf.sv:248-259] (SPEC_DOC_ERRATA):
 *   When an unbuffered software partition's `*_READ_LOCK` is `0`
 *   (`VENDOR_TEST_READ_LOCK == 0`), reading its digest offset via either
 *   `SW_CFG_WINDOW` or `DAI` (`DIRECT_ACCESS_CMD.RD`) fails with `AccessError`
 *   (because `otp_ctrl_part_unbuf.sv:248` has no digest exemption and
 *   `otp_ctrl_dai.sv:315` only exempts `PartInfo[part_idx].hw_digest`), while
 *   the core `VENDOR_TEST_DIGEST_0/1` CSRs remain readable.
 * - [otp_ctrl.hjson:2133] (INTENDED_SECURITY_HARDENING):
 *   Reading a read-locked unbuffered partition in `SW_CFG_WINDOW`
 * simultaneously raises a synchronous TL-UL Load Access Fault (`d_error = 1`,
 * `mcause = 5`) and latches `ERR_CODE_0 = AccessError`,
 * `STATUS.VENDOR_TEST_ERROR = 1`, and `INTR_STATE.OTP_ERROR = 1`, whereas
 * writing to `SW_CFG_WINDOW` raises a synchronous Store Access Fault
 * (`ErrOnWrite = 1` in `u_tlul_adapter_sram`) without setting `ERR_CODE_0` or
 * `INTR_STATE.OTP_ERROR`.
 * - [otp_ctrl_dai.sv:184] (INTENDED_SECURITY_HARDENING):
 *   `INTR_STATE.otp_operation_done` pulses even when a DAI command aborts with
 *   `AccessError`, and `DIRECT_ACCESS_RDATA_0/1` is cleared to `0`.
 * - [otp_ctrl.hjson:1829] (INTENDED_SECURITY_HARDENING):
 *   `otp_ctrl.prim` (`0x40138000`) is gated by `u_tlul_lc_gate`
 * (`lc_dft_en[0]`), and `OTP_CTRL_CORE_PERMIT` / `OTP_CTRL_PRIM_PERMIT` enforce
 * sub-word write faults (`wr_err`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "lc_ctrl_regs.h"
#include "otp_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kOtpCoreBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
  kOtpPrimBase = TOP_EARLGREY_OTP_CTRL_PRIM_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
  kOtpErrNoError = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_NO_ERROR,
  kOtpErrAccessError = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
};

static volatile uint32_t load_access_fault_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  ++load_access_fault_count;
}

static void wait_for_dai_idle(void) {
  while (!bitfield_bit32_read(
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET),
      OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
  }
}

bool test_main(void) {
  wait_for_dai_idle();

  // 1. Verify [otp_ctrl.hjson:2133] (INTENDED_SECURITY_HARDENING):
  //    Read-locked SW_CFG_WINDOW read triggers BOTH synchronous TL-UL d_error=1
  //    AND asynchronous ERR_CODE_0 = AccessError / INTR_STATE.OTP_ERROR,
  //    whereas SW_CFG_WINDOW write triggers ONLY d_error=1 (ErrOnWrite=1).
  LOG_INFO(
      "Verifying [otp_ctrl.hjson:2133] (INTENDED_SECURITY_HARDENING): "
      "SW_CFG_WINDOW read-locked dual d_error=1 + ERR_CODE_0 vs ErrOnWrite...");

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) == 0x1u);
  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET);
  CHECK(load_access_fault_count == 0u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrNoError);

  // Write to SW_CFG_WINDOW while VENDOR_TEST is unlocked -> d_error=1 only,
  // ERR_CODE_0 stays NoError and INTR_STATE stays 0.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET,
                   0xdeadbeefu);
  CHECK(load_access_fault_count == 1u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrNoError);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  // Lock VENDOR_TEST_READ_LOCK (rw0c).
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET,
                   0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) == 0x0u);

  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET);
  CHECK(load_access_fault_count == 2u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrAccessError);
  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_VENDOR_TEST_ERROR_BIT));
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET),
      OTP_CTRL_INTR_STATE_OTP_ERROR_BIT));
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 2. Verify [otp_ctrl_dai.sv:184] (INTENDED_SECURITY_HARDENING):
  //    DAI read populates DIRECT_ACCESS_RDATA_0 on unlocked partition, then on
  //    a locked partition DAI read sets BOTH otp_operation_done and otp_error
  //    while clearing DIRECT_ACCESS_RDATA_0/1 to 0.
  LOG_INFO(
      "Verifying [otp_ctrl_dai.sv:184] (INTENDED_SECURITY_HARDENING): DAI "
      "otp_operation_done on AccessError & DIRECT_ACCESS_RDATA_0/1 "
      "clearing...");

  bool creator_sw_cfg_unlocked =
      abs_mmio_read32(kOtpCoreBase +
                      OTP_CTRL_CREATOR_SW_CFG_READ_LOCK_REG_OFFSET) == 1u;
  uint32_t unlocked_part_offset =
      creator_sw_cfg_unlocked ? OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET
                              : OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_OFFSET;
  uint32_t unlocked_part_size =
      creator_sw_cfg_unlocked ? OTP_CTRL_PARAM_CREATOR_SW_CFG_SIZE
                              : OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_SIZE;

  uint32_t expected_creator_word = abs_mmio_read32(
      kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + unlocked_part_offset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   unlocked_part_offset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) ==
        expected_creator_word);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_VENDOR_TEST_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrAccessError);
  CHECK((abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) &
         0x3u) == 0x3u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 3. Verify [otp_ctrl_part_unbuf.sv:248-259] (SPEC_DOC_ERRATA):
  //    Reading VENDOR_TEST_DIGEST_OFFSET (56) via SW_CFG_WINDOW and DAI while
  //    VENDOR_TEST_READ_LOCK == 0 fails with AccessError (whereas core CSRs
  //    VENDOR_TEST_DIGEST_0/1 succeed), and unaligned byte address in the last
  //    word of an unlocked partition succeeds.
  LOG_INFO(
      "Verifying [otp_ctrl_part_unbuf.sv:248-259] (SPEC_DOC_ERRATA): "
      "Unbuffered SW digest "
      "read-lock AccessError in SW_CFG_WINDOW & DAI...");

  (void)abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  (void)abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_DIGEST_1_REG_OFFSET);
  CHECK(load_access_fault_count == 2u);

  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                        OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET);
  CHECK(load_access_fault_count == 3u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrAccessError);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrAccessError);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   unlocked_part_offset + unlocked_part_size - 1u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrNoError);

  // 4. Verify [otp_ctrl.hjson:1829] (INTENDED_SECURITY_HARDENING):
  //    otp_ctrl.prim (0x40138000) u_tlul_lc_gate lc_dft_en gating and
  //    OTP_CTRL_CORE_PERMIT / OTP_CTRL_PRIM_PERMIT sub-word write faults.
  LOG_INFO(
      "Verifying [otp_ctrl.hjson:1829] (INTENDED_SECURITY_HARDENING): "
      "otp_ctrl.prim 0x40138000 lc_dft_en gate & PERMIT sub-word faults...");

  uint32_t lc_state =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_STATE_REG_OFFSET);
  bool lc_dft_en = (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED0) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED1) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED2) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED3) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED4) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED5) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED6) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED7) ||
                   (lc_state == LC_CTRL_LC_STATE_STATE_VALUE_RMA);

  uint32_t faults_before_prim = load_access_fault_count;
  if (lc_dft_en) {
    abs_mmio_write32(kOtpPrimBase + 0x00u, 0xffffffffu);
    CHECK(abs_mmio_read32(kOtpPrimBase + 0x00u) == 0x07ff3ff7u);
    abs_mmio_write32(kOtpPrimBase + 0x00u, 0x0u);
    CHECK(load_access_fault_count == faults_before_prim);
  } else {
    (void)abs_mmio_read32(kOtpPrimBase + 0x00u);
    CHECK(load_access_fault_count == faults_before_prim + 1u);
  }

  uint32_t faults_before_subword = load_access_fault_count;
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write8(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET,
                  0x99u);
  CHECK(load_access_fault_count == faults_before_subword + 1u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET) ==
        0x11223344u);

  LOG_INFO("All [otp_ctrl_part_unbuf.sv:248-259..004] checks confirmed!");
  return true;
}
