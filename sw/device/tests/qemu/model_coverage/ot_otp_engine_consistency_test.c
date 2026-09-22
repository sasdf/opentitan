// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "otp_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kOtpBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
  kOtpDaiCmdRead = 0x1u,
  kOtpDaiCmdWrite = 0x2u,
  kOtpDaiCmdDigest = 0x4u,
  kOtpErrAccessError = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
  kVendorTestAddr = OTP_CTRL_PARAM_VENDOR_TEST_OFFSET,
  kCreatorSwCfgAddr = OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
  kHwCfg0Addr = OTP_CTRL_PARAM_HW_CFG0_OFFSET,
  kLifeCycleAddr = OTP_CTRL_PARAM_LIFE_CYCLE_OFFSET,
};

static void wait_dai_idle(void) {
  while ((abs_mmio_read32(kOtpBase + OTP_CTRL_STATUS_REG_OFFSET) &
          (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) == 0u) {
  }
}

static void check_dai_access_error_and_recover(uint32_t addr, uint32_t cmd) {
  wait_dai_idle();

  // Clear INTR_STATE (rw1c).
  abs_mmio_write32(kOtpBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  // Issue the access-violating DAI command.
  abs_mmio_write32(kOtpBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  abs_mmio_write32(kOtpBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET, cmd);
  wait_dai_idle();

  // Verify STATUS.DAI_ERROR == 1, ERR_CODE_11 (DAI error) == 0x5 (AccessError),
  // and INTR_STATE.OTP_ERROR == 1.
  uint32_t status = abs_mmio_read32(kOtpBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK((status & (1u << OTP_CTRL_STATUS_DAI_ERROR_BIT)) != 0u);
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrAccessError);
  uint32_t intr_state =
      abs_mmio_read32(kOtpBase + OTP_CTRL_INTR_STATE_REG_OFFSET);
  CHECK((intr_state & (1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT)) != 0u);

  // Recover DAI error state via a valid DAI RD command on CREATOR_SW_CFG
  // (0x040), which remains read-unlocked throughout the test.
  abs_mmio_write32(kOtpBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kOtpBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kCreatorSwCfgAddr);
  abs_mmio_write32(kOtpBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   kOtpDaiCmdRead);
  wait_dai_idle();

  status = abs_mmio_read32(kOtpBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK((status & (1u << OTP_CTRL_STATUS_DAI_ERROR_BIT)) == 0u);
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) == 0x0u);
}

bool test_main(void) {
  LOG_INFO("Starting ot_otp_engine FPGA/QEMU consistency test");

  wait_dai_idle();

  // 1. DAI Read from LIFE_CYCLE partition (0x7a8) is rejected with AccessError.
  check_dai_access_error_and_recover(kLifeCycleAddr, kOtpDaiCmdRead);

  // 2. DAI Write to LIFE_CYCLE partition (0x7a8) is rejected with AccessError.
  check_dai_access_error_and_recover(kLifeCycleAddr, kOtpDaiCmdWrite);

  // 3. DAI Digest on LIFE_CYCLE partition (0x7a8) is rejected with AccessError.
  check_dai_access_error_and_recover(kLifeCycleAddr, kOtpDaiCmdDigest);

  // 4. DAI Digest on SW-digest partition VENDOR_TEST (0x000, hw_digest ==
  // false) is rejected with AccessError.
  check_dai_access_error_and_recover(kVendorTestAddr, kOtpDaiCmdDigest);

  // 5. DAI Write and DAI Digest on already-locked HW-digest partition HW_CFG0
  // (0x678) are both rejected with AccessError.
  uint32_t hw_cfg0_d0 =
      abs_mmio_read32(kOtpBase + OTP_CTRL_HW_CFG0_DIGEST_0_REG_OFFSET);
  uint32_t hw_cfg0_d1 =
      abs_mmio_read32(kOtpBase + OTP_CTRL_HW_CFG0_DIGEST_1_REG_OFFSET);
  CHECK((hw_cfg0_d0 | hw_cfg0_d1) != 0u);
  check_dai_access_error_and_recover(kHwCfg0Addr, kOtpDaiCmdWrite);
  check_dai_access_error_and_recover(kHwCfg0Addr, kOtpDaiCmdDigest);

  // 6. Lock VENDOR_TEST_READ_LOCK (rw0c write 0) and verify DAI Read from
  // VENDOR_TEST (0x000) is rejected with AccessError.
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) ==
        0x1u);
  abs_mmio_write32(kOtpBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) ==
        0x0u);
  // Writing 1 to RW0C VENDOR_TEST_READ_LOCK must not re-enable it.
  abs_mmio_write32(kOtpBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kOtpBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) ==
        0x0u);

  check_dai_access_error_and_recover(kVendorTestAddr, kOtpDaiCmdRead);

  LOG_INFO("ot_otp_engine FPGA/QEMU consistency test passed");
  return true;
}
