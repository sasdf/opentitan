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

  // 4. Test DAI Read of an unlocked SW_CFG partition -> sets otp_operation_done
  // and populates DIRECT_ACCESS_RDATA_0 matching SW_CFG_WINDOW.
  // Note: ROM_EXT locks CREATOR_SW_CFG_READ_LOCK, while
  // ROT_CREATOR_AUTH_CODESIGN remains unlocked (READ_LOCK == 1) in both ROM and
  // ROM_EXT.
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
                   unlocked_part_offset);
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
  (void)abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  (void)abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_VENDOR_TEST_DIGEST_1_REG_OFFSET);
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

  // 8. Test DAI Read with unaligned byte address in the last word of the
  // unlocked partition (e.g. offset + size - 1). In RTL
  // (otp_ctrl_dai.sv:721-722, 754-769), part_sel_oh checks
  // dai_addr_i < PartEndInt and masks the lower 2/3 address bits, so the read
  // succeeds with ERR_CODE_11 == NoError.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   unlocked_part_offset + unlocked_part_size - 1u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
        kOtpErrNoError);

  // 9. Test `otp_ctrl.prim` (`TOP_EARLGREY_OTP_CTRL_PRIM_BASE_ADDR` =
  // 0x40138000) `u_tlul_lc_gate` (`lc_dft_en`) qualification
  // (`otp_ctrl.sv:778-792`), CSR0..CSR7 read/write field masks
  // (`otp_ctrl_prim_reg_top.sv` / `prim_generic_otp.sv`), and verify unmapped
  // `0x40132000` raises a Load Access Fault.
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
    for (uint32_t off = 0; off < 0x20u; off += 4u) {
      CHECK(abs_mmio_read32(kOtpPrimBase + off) == 0x0u);
    }
    CHECK(load_access_fault_count == faults_before_prim);

    static const uint32_t kPrimExpectedMask[8] = {
        0x07ff3ff7u,  // CSR0: field0[0], field1[1], field2[2], field3[13:4],
                      // field4[26:16]
        0xffffffffu,  // CSR1: field0..4[31:0]
        0x00000001u,  // CSR2: field0[0]
        0x00000000u,  // CSR3: field0..2 W1C (0), field3..8 RO (0)
        0x000073ffu,  // CSR4: field0[9:0], field1[12], field2[13], field3[14]
        0xffff00ffu,  // CSR5: field0[5:0], field1[7:6], field6[31:16] RW;
                      // field2..5 RO (0)
        0xffff1bffu,  // CSR6: field0[9:0], field1[11], field2[12],
                      // field3[31:16]
        0x00000000u,  // CSR7: field0..3 RO (0)
    };
    for (uint32_t i = 0; i < 8u; ++i) {
      abs_mmio_write32(kOtpPrimBase + i * 4u, 0xffffffffu);
      CHECK(abs_mmio_read32(kOtpPrimBase + i * 4u) == kPrimExpectedMask[i]);
      abs_mmio_write32(kOtpPrimBase + i * 4u, 0x0u);
      CHECK(abs_mmio_read32(kOtpPrimBase + i * 4u) == 0x0u);
    }
    CHECK(load_access_fault_count == faults_before_prim);
  } else {
    // When lc_dft_en == Off (e.g. PROD / DEV in sival_rom_ext), u_tlul_lc_gate
    // blocks 0x40138000..0x4013801f and returns d_error = 1 (Load Access
    // Fault).
    (void)abs_mmio_read32(kOtpPrimBase + 0x00u);
    CHECK(load_access_fault_count == faults_before_prim + 1u);
    (void)abs_mmio_read32(kOtpPrimBase + 0x1cu);
    CHECK(load_access_fault_count == faults_before_prim + 2u);
    faults_before_prim = load_access_fault_count;
  }

  // Reading unmapped 0x40132000 (outside 0x40130000..0x40131fff core and
  // 0x40138000..0x4013801f prim) must raise a Load Access Fault.
  (void)abs_mmio_read32(0x40132000u);
  CHECK(load_access_fault_count == faults_before_prim + 1u);

  // 10. Test `OTP_CTRL_CORE_PERMIT` (`otp_ctrl_core_reg_top.sv:2049-2106`) and
  // `OTP_CTRL_PRIM_PERMIT` (`otp_ctrl_prim_reg_top.sv:1282-1292`) sub-word
  // write `wr_err` enforcement.
  uint32_t faults_before_subword = load_access_fault_count;
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write8(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET,
                  0x99u);
  CHECK(load_access_fault_count == faults_before_subword + 1u);
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET) ==
        0x11223344u);

  // Byte write to offset + 1 of INTR_ENABLE (PERMIT = 4'b0001) must fault,
  // while byte write to offset + 0 (reg_be = 4'b0001) is permitted.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET, 0x0u);
  abs_mmio_write8(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET + 1u, 0x3u);
  CHECK(load_access_fault_count == faults_before_subword + 2u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET) ==
        0x0u);
  abs_mmio_write8(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET, 0x2u);
  CHECK(load_access_fault_count == faults_before_subword + 2u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET) ==
        0x2u);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_ENABLE_REG_OFFSET, 0x0u);

  if (lc_dft_en) {
    uint32_t faults_before_prim_subword = load_access_fault_count;
    abs_mmio_write32(kOtpPrimBase + 0x00u, 0x07ff3ff7u);
    abs_mmio_write8(kOtpPrimBase + 0x00u, 0x00u);
    CHECK(load_access_fault_count == faults_before_prim_subword + 1u);
    CHECK(abs_mmio_read32(kOtpPrimBase + 0x00u) == 0x07ff3ff7u);
    abs_mmio_write32(kOtpPrimBase + 0x00u, 0x0u);

    // CSR2 has OTP_CTRL_PRIM_PERMIT[2] = 4'b0001: byte 0 write succeeds, byte 1
    // write faults.
    abs_mmio_write8(kOtpPrimBase + 0x08u, 0x1u);
    CHECK(load_access_fault_count == faults_before_prim_subword + 1u);
    CHECK(abs_mmio_read32(kOtpPrimBase + 0x08u) == 0x1u);
    abs_mmio_write8(kOtpPrimBase + 0x08u + 1u, 0x0u);
    CHECK(load_access_fault_count == faults_before_prim_subword + 2u);
    CHECK(abs_mmio_read32(kOtpPrimBase + 0x08u) == 0x1u);
    abs_mmio_write32(kOtpPrimBase + 0x08u, 0x0u);
  }

  // 11. Test SW_CFG_WINDOW sub-word reads (`lb`) and sub-word writes (`sb`):
  // - `lb` at byte offsets 0..3 of an unlocked SW_CFG partition returns the
  //   exact bytes of `expected_creator_word` without faulting.
  // - `sb` and `sw` to a read-locked SW_CFG partition (`VENDOR_TEST`) must
  //   raise a Store Access Fault (`ErrOnWrite = 1` in `u_tlul_adapter_sram`)
  //   WITHOUT triggering `part_tlul_req` or setting `INTR_STATE.OTP_ERROR`.
  uint32_t faults_before_swcfg_subword = load_access_fault_count;
  uint32_t swcfg_word_addr =
      kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + unlocked_part_offset;
  uint32_t reconstructed_word =
      ((uint32_t)abs_mmio_read8(swcfg_word_addr + 0u) << 0) |
      ((uint32_t)abs_mmio_read8(swcfg_word_addr + 1u) << 8) |
      ((uint32_t)abs_mmio_read8(swcfg_word_addr + 2u) << 16) |
      ((uint32_t)abs_mmio_read8(swcfg_word_addr + 3u) << 24);
  CHECK(load_access_fault_count == faults_before_swcfg_subword);
  CHECK(reconstructed_word == expected_creator_word);

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);
  abs_mmio_write8(kOtpCoreBase + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                      OTP_CTRL_PARAM_VENDOR_TEST_OFFSET,
                  0x5au);
  CHECK(load_access_fault_count == faults_before_swcfg_subword + 1u);
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  return true;
}
