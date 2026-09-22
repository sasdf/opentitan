// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "otp_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
};

bool test_main(void) {
  // 1. Verify write-only ALERT_TEST and INTR_TEST registers read back as 0.
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_ALERT_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_INTR_TEST_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + OTP_CTRL_ALERT_TEST_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_ALERT_TEST_REG_OFFSET) == 0x0u);

  // 2. Verify read-only digest registers ignore writes and preserve readback.
  uint32_t vtest_d0 =
      abs_mmio_read32(kBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  uint32_t sec2_d1 =
      abs_mmio_read32(kBase + OTP_CTRL_SECRET2_DIGEST_1_REG_OFFSET);

  abs_mmio_write32(kBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET, ~vtest_d0);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET) ==
        vtest_d0);

  abs_mmio_write32(kBase + OTP_CTRL_SECRET2_DIGEST_1_REG_OFFSET, ~sec2_d1);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_SECRET2_DIGEST_1_REG_OFFSET) ==
        sec2_d1);

  // 3. Verify CHECK_TRIGGER and CHECK_TRIGGER_REGWEN RW0C locking.
  abs_mmio_write32(kBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_CHECK_TRIGGER_REGWEN_REG_OFFSET) ==
        0x1u);

  abs_mmio_write32(kBase + OTP_CTRL_CHECK_TRIGGER_REG_OFFSET, 0x3u);
  for (uint32_t i = 0; i < 1000u; ++i) {
    uint32_t st = abs_mmio_read32(kBase + OTP_CTRL_STATUS_REG_OFFSET);
    if ((st & (1u << OTP_CTRL_STATUS_CHECK_PENDING_BIT)) == 0u) {
      break;
    }
    busy_spin_micros(5);
  }
  CHECK((abs_mmio_read32(kBase + OTP_CTRL_STATUS_REG_OFFSET) &
         (1u << OTP_CTRL_STATUS_CHECK_PENDING_BIT)) == 0u);
  for (uint32_t i = 0; i <= 12u; ++i) {
    CHECK(abs_mmio_read32(kBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET + i * 4u) ==
          0x0u);
  }

  abs_mmio_write32(kBase + OTP_CTRL_CHECK_TRIGGER_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_CHECK_TRIGGER_REGWEN_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + OTP_CTRL_CHECK_TRIGGER_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + OTP_CTRL_CHECK_TRIGGER_REGWEN_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + OTP_CTRL_CHECK_TRIGGER_REG_OFFSET, 0x3u);
  CHECK((abs_mmio_read32(kBase + OTP_CTRL_STATUS_REG_OFFSET) &
         (1u << OTP_CTRL_STATUS_CHECK_PENDING_BIT)) == 0u);

  // 4. Verify unbuffered ROT_CREATOR_AUTH digest window reads match digest
  // CSRs.
  uint32_t codesign_d0_csr = abs_mmio_read32(
      kBase + OTP_CTRL_ROT_CREATOR_AUTH_CODESIGN_DIGEST_0_REG_OFFSET);
  uint32_t codesign_d0_win =
      abs_mmio_read32(kBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                      OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_DIGEST_OFFSET);
  CHECK(codesign_d0_win == codesign_d0_csr);

  uint32_t state_d0_csr = abs_mmio_read32(
      kBase + OTP_CTRL_ROT_CREATOR_AUTH_STATE_DIGEST_0_REG_OFFSET);
  uint32_t state_d0_win =
      abs_mmio_read32(kBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                      OTP_CTRL_PARAM_ROT_CREATOR_AUTH_STATE_DIGEST_OFFSET);
  CHECK(state_d0_win == state_d0_csr);

  LOG_INFO("ot_otp_eg_consistency_test passed");
  return true;
}
