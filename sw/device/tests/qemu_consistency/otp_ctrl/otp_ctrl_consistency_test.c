// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "otp_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kOtpCoreBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
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

  // 1. Test INTR_TEST bitwise-OR (`|=`) and INTR_STATE W1C behavior.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_TEST_REG_OFFSET,
                   1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x1u);

  // Writing bit 1 to INTR_TEST must bitwise-OR into INTR_STATE without clearing
  // bit 0.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_TEST_REG_OFFSET,
                   1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x3u);

  // W1C individual bits of INTR_STATE.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x2u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x2u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  // 2. Test CHECK_TIMEOUT, INTEGRITY_CHECK_PERIOD, CONSISTENCY_CHECK_PERIOD RW
  // storage and CHECK_REGWEN (rw0c) write-locking.
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_CHECK_REGWEN_REG_OFFSET) ==
        0x1u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTEGRITY_CHECK_PERIOD_REG_OFFSET,
                   0x0abcdef0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CONSISTENCY_CHECK_PERIOD_REG_OFFSET,
                   0x07654320u);

  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_INTEGRITY_CHECK_PERIOD_REG_OFFSET) ==
        0x0abcdef0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_CONSISTENCY_CHECK_PERIOD_REG_OFFSET) ==
        0x07654320u);

  // Disable periodic check timers before locking CHECK_REGWEN.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTEGRITY_CHECK_PERIOD_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CONSISTENCY_CHECK_PERIOD_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CHECK_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_CHECK_REGWEN_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CHECK_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_CHECK_REGWEN_REG_OFFSET) ==
        0x0u);

  // Writes to CHECK_TIMEOUT / *_PERIOD must be ignored when CHECK_REGWEN == 0.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTEGRITY_CHECK_PERIOD_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_CONSISTENCY_CHECK_PERIOD_REG_OFFSET,
                   0xdeadbeefu);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_CHECK_TIMEOUT_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_INTEGRITY_CHECK_PERIOD_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_CONSISTENCY_CHECK_PERIOD_REG_OFFSET) == 0x0u);

  // 3. Test VENDOR_TEST_READ_LOCK (rw0c), SW_CFG_WINDOW access error + TL-UL
  // bus error (Load Access Fault), ERR_CODE_0, and STATUS.VENDOR_TEST_ERROR
  // (without false STATUS.DAI_ERROR).
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) == 0x1u);
  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET);
  CHECK(load_access_fault_count == 0u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrNoError);

  // Lock VENDOR_TEST_READ_LOCK (rw0c).
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET,
                   0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET,
                   0x1u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET) == 0x0u);

  // Reading VENDOR_TEST in SW_CFG_WINDOW while locked must raise a TL-UL bus
  // error (Load Access Fault), set ERR_CODE_0 = AccessError (2),
  // STATUS.VENDOR_TEST_ERROR = 1, STATUS.DAI_ERROR = 0, and
  // INTR_STATE.OTP_ERROR = 1.
  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET);
  CHECK(load_access_fault_count == 1u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
        kOtpErrAccessError);
  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_VENDOR_TEST_ERROR_BIT));
  CHECK(!bitfield_bit32_read(status, OTP_CTRL_STATUS_DAI_ERROR_BIT));
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET),
      OTP_CTRL_INTR_STATE_OTP_ERROR_BIT));
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 4. Test DAI Read of CREATOR_SW_CFG (unlocked) -> sets otp_operation_done
  // and populates DIRECT_ACCESS_RDATA_0 matching SW_CFG_WINDOW.
  uint32_t expected_creator_word =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                      OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();

  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET),
      OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT));
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrNoError);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) ==
        expected_creator_word);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 5. Test DAI Read of locked VENDOR_TEST (address 0x0) -> sets ERR_CODE_11 =
  // AccessError (2), STATUS.DAI_ERROR = 1, INTR_STATE = 0x3
  // (otp_operation_done | otp_error), and clears DIRECT_ACCESS_RDATA_0/1 to 0.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_VENDOR_TEST_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();

  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrAccessError);
  status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_DAI_ERROR_BIT));
  CHECK((abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) &
         0x3u) == 0x3u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 6. Verify subsequent valid DAI Read clears ERR_CODE_11 and STATUS.DAI_ERROR
  // while STATUS.VENDOR_TEST_ERROR remains set.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();

  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrNoError);
  status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, OTP_CTRL_STATUS_DAI_ERROR_BIT));
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_VENDOR_TEST_ERROR_BIT));
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 7. Test reading VENDOR_TEST_DIGEST_OFFSET (56) via SW_CFG_WINDOW and via
  // DAI while VENDOR_TEST_READ_LOCK == 0:
  // - Core CSRs VENDOR_TEST_DIGEST_0/1 remain readable without fault.
  // - SW_CFG_WINDOW read at VENDOR_TEST_DIGEST_OFFSET must still raise a TL-UL
  //   bus error (Load Access Fault) and set ERR_CODE_0 = AccessError
  //   (otp_ctrl_part_unbuf.sv:248 has no digest exception).
  // - DAI Read at VENDOR_TEST_DIGEST_OFFSET must set ERR_CODE_11 = AccessError
  //   and STATUS.DAI_ERROR = 1 because VENDOR_TEST has sw_digest=1, hw_digest=0
  //   and otp_ctrl_dai.sv:315 only exempts PartInfo[part_idx].hw_digest.
  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_1_REG_OFFSET);
  CHECK(load_access_fault_count == 1u);

  (void)abs_mmio_read32(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                        OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET);
  CHECK(load_access_fault_count == 2u);
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
  status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_DAI_ERROR_BIT));
  CHECK((abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) &
         0x3u) == 0x3u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 8. Test DAI Read with unaligned byte address in the last word of unlocked
  // CREATOR_SW_CFG (e.g. offset + size - 1 = 431). In RTL
  // (otp_ctrl_dai.sv:721-722, 754-769), part_sel_oh checks
  // dai_addr_i < PartEndInt and masks the lower 2/3 address bits, so the read
  // succeeds with ERR_CODE_11 == NoError.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET +
                       OTP_CTRL_PARAM_CREATOR_SW_CFG_SIZE - 1u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrNoError);

  return true;
}
