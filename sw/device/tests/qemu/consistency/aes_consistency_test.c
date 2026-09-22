// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aes_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAesBase = TOP_EARLGREY_AES_BASE_ADDR,
};

static volatile bool access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  access_fault_seen = true;
}

static void wait_for_idle(void) {
  while (!bitfield_bit32_read(abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET),
                              AES_STATUS_IDLE_BIT)) {
  }
}

static void write_ctrl_shadowed(uint32_t val) {
  wait_for_idle();
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, val);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, val);
}

static void write_dummy_key(uint32_t seed) {
  wait_for_idle();
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4,
                     seed ^ (i * 0x11111111u));
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4, 0u);
  }
  wait_for_idle();
}

static void write_dummy_iv(uint32_t seed) {
  wait_for_idle();
  for (uint32_t i = 0; i < 4; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4, seed + i);
  }
}

static void write_dummy_data_in(uint32_t seed) {
  for (uint32_t i = 0; i < 4; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4, seed + i);
  }
}

static void read_all_data_out(void) {
  for (uint32_t i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4);
  }
}

bool test_main(void) {
  wait_for_idle();

  // 1. Verify CTRL_SHADOWED sparse field sanitization
  // (aes_ctrl_reg_shadowed.sv:75-120): Writing 0x0 twice maps
  // OPERATION=0->AES_ENC(0x1), MODE=0->AES_NONE(0x20), KEY_LEN=0->AES_256(0x4),
  // PRNG_RESEED_RATE=0->PER_1(0x1) => 0x00001481.
  write_ctrl_shadowed(0x0u);
  uint32_t ctrl_val = abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET);
  CHECK(ctrl_val == 0x00001481u,
        "Expected sanitized CTRL_SHADOWED=0x00001481, got 0x%08x", ctrl_val);

  // 2. Verify TRIGGER.START auto-clear when MODE=AES_NONE or MANUAL_OPERATION=0
  // (aes_control_fsm.sv:328):
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  uint32_t status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_IDLE_BIT),
        "Expected STATUS.IDLE=1 after ignored TRIGGER.START, got status=0x%08x",
        status);

  // 3. Verify STATUS.ALERT_RECOV_CTRL_UPDATE_ERR set on shadow mismatch and
  // cleared on subsequent CTRL_SHADOWED write (aes_core.sv:915-916):
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdAesRecovCtrlUpdateErr));
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x00001105u);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x00001109u);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdAesRecovCtrlUpdateErr));
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT),
        "Expected STATUS.ALERT_RECOV_CTRL_UPDATE_ERR=1 after mismatch");

  // Rewriting CTRL_SHADOWED clears STATUS.ALERT_RECOV_CTRL_UPDATE_ERR:
  uint32_t ecb_manual_ctrl =
      (AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC
       << AES_CTRL_SHADOWED_OPERATION_OFFSET) |
      (AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB << AES_CTRL_SHADOWED_MODE_OFFSET) |
      (AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128
       << AES_CTRL_SHADOWED_KEY_LEN_OFFSET) |
      (AES_CTRL_SHADOWED_PRNG_RESEED_RATE_VALUE_PER_1
       << AES_CTRL_SHADOWED_PRNG_RESEED_RATE_OFFSET) |
      (1u << AES_CTRL_SHADOWED_MANUAL_OPERATION_BIT);
  write_ctrl_shadowed(ecb_manual_ctrl);
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(
      !bitfield_bit32_read(status, AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT),
      "Expected STATUS.ALERT_RECOV_CTRL_UPDATE_ERR=0 after CTRL_SHADOWED "
      "write");

  // 4. Verify STATUS.OUTPUT_LOST in manual mode when overwriting unread
  // DATA_OUT, and clear on CTRL_SHADOWED write (aes_control_fsm.sv:757-763):
  write_dummy_key(0x12345678u);
  write_dummy_data_in(0x100u);
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=1 after first manual block");
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_LOST_BIT),
        "Expected STATUS.OUTPUT_LOST=0 after first manual block");

  // Trigger second manual block without reading all 4 DATA_OUT registers:
  write_dummy_data_in(0x200u);
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_LOST_BIT),
        "Expected STATUS.OUTPUT_LOST=1 after overwriting unread DATA_OUT");

  // 5. Verify TRIGGER.DATA_OUT_CLEAR clears STATUS.OUTPUT_VALID
  // (aes_control_fsm.sv:745-747):
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_DATA_OUT_CLEAR_BIT);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=0 after TRIGGER.DATA_OUT_CLEAR");

  // 6. Verify writing CTRL_SHADOWED clears STATUS.OUTPUT_LOST and
  // Verify partial IV update in CBC automatic mode blocks encryption until all
  // 4 IV words are written (aes_reg_status.sv:31-78):
  uint32_t cbc_auto_ctrl =
      (AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC
       << AES_CTRL_SHADOWED_OPERATION_OFFSET) |
      (AES_CTRL_SHADOWED_MODE_VALUE_AES_CBC << AES_CTRL_SHADOWED_MODE_OFFSET) |
      (AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128
       << AES_CTRL_SHADOWED_KEY_LEN_OFFSET) |
      (AES_CTRL_SHADOWED_PRNG_RESEED_RATE_VALUE_PER_1
       << AES_CTRL_SHADOWED_PRNG_RESEED_RATE_OFFSET);
  write_ctrl_shadowed(cbc_auto_ctrl);
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_LOST_BIT),
        "Expected STATUS.OUTPUT_LOST=0 after CTRL_SHADOWED write");

  write_dummy_key(0xabcdef01u);
  write_dummy_iv(0x10u);
  write_dummy_data_in(0x300u);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=1 after full CBC block");
  read_all_data_out();

  // Partial IV update: write only IV_0, then write all 4 DATA_IN words.
  // Encryption must NOT start until IV_1..3 are also written.
  abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET, 0x99u);
  write_dummy_data_in(0x400u);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=0 when IV is only partially updated");

  // Finish writing IV_1..3; now automatic encryption must start and complete:
  for (uint32_t i = 1; i < 4; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4, 0x99u + i);
  }
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=1 after completing IV update");
  read_all_data_out();

  // 7. Verify CTRL_AUX_REGWEN is rw0c (aes.hjson:808-823):
  CHECK(abs_mmio_read32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET) == 1u,
        "Expected initial CTRL_AUX_REGWEN=1");
  abs_mmio_write32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET) == 0u,
        "Expected CTRL_AUX_REGWEN=0 after writing 0");
  abs_mmio_write32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET) == 0u,
        "Expected CTRL_AUX_REGWEN to remain 0 after writing 1 (rw0c)");

  // 8. Verify TRIGGER.DATA_OUT_CLEAR when STATUS.OUTPUT_VALID=1 sets
  // STATUS.OUTPUT_LOST=1 (aes_control_fsm.sv:601, 760-762):
  write_ctrl_shadowed(ecb_manual_ctrl);
  write_dummy_key(0x55aa55aau);
  write_dummy_data_in(0x500u);
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=1 before DATA_OUT_CLEAR");
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_LOST_BIT),
        "Expected STATUS.OUTPUT_LOST=0 before DATA_OUT_CLEAR");
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_DATA_OUT_CLEAR_BIT);
  wait_for_idle();
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, AES_STATUS_OUTPUT_VALID_BIT),
        "Expected STATUS.OUTPUT_VALID=0 after DATA_OUT_CLEAR");
  CHECK(bitfield_bit32_read(status, AES_STATUS_OUTPUT_LOST_BIT),
        "Expected STATUS.OUTPUT_LOST=1 when DATA_OUT_CLEAR overwrites unread "
        "DATA_OUT");

  // 9. Verify first (staged) write to CTRL_SHADOWED clears data_in_new_q and
  // sets STATUS.INPUT_READY=1 (aes_control_fsm.sv:705, 711-713, 738-739):
  write_dummy_data_in(0x600u);
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, AES_STATUS_INPUT_READY_BIT),
        "Expected STATUS.INPUT_READY=0 after writing all 4 DATA_IN words in "
        "manual mode");
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ecb_manual_ctrl);
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, AES_STATUS_INPUT_READY_BIT),
        "Expected STATUS.INPUT_READY=1 after first (staged) CTRL_SHADOWED "
        "write");
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ecb_manual_ctrl);

  // 10. Verify overwriting a single DATA_IN word before cipher load updates the
  // block encrypted on TRIGGER.START (aes_core.sv:280-285, 429):
  write_dummy_key(0xdeadbeefu);
  write_dummy_data_in(0x700u);
  // Overwrite only DATA_IN_0 before starting manual encryption:
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, 0x77777777u);
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  wait_for_idle();
  uint32_t out_overwritten[4];
  for (uint32_t i = 0; i < 4; ++i) {
    out_overwritten[i] =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4);
  }

  // Encrypt the same 4 words written directly without overwrite:
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, 0x77777777u);
  for (uint32_t i = 1; i < 4; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4, 0x700u + i);
  }
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  wait_for_idle();
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t out_direct =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4);
    CHECK(out_overwritten[i] == out_direct,
          "DATA_OUT_%u mismatch after single-word DATA_IN_0 overwrite: "
          "0x%08x vs 0x%08x",
          i, out_overwritten[i], out_direct);
  }

  // 11. Verify AES_PERMIT sub-word write wr_err (aes_reg_pkg.sv:377-412,
  // aes_reg_top.sv:1500-1630):
  // - CTRL_SHADOWED (0x74) has PERMIT = 4'b0011:
  //   8-bit (sb) write fails with StoreAccessFault (and does NOT trigger shadow
  //   update error), while 16-bit (sh) write at offset +0 succeeds without
  //   fault.
  // - KEY_SHARE0_0 (0x04), IV_0 (0x44), DATA_IN_0 (0x54), DATA_OUT_0 (0x64)
  // have
  //   PERMIT = 4'b1111: 8-bit (sb) and 16-bit (sh) writes fail with
  //   StoreAccessFault.
  // - TRIGGER (0x80) and ALERT_TEST (0x00) have PERMIT = 4'b0001:
  //   8-bit write at offset +0 succeeds, while 8-bit write at offset +1 fails.
  access_fault_seen = false;
  abs_mmio_write8(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x01u);
  CHECK(access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to CTRL_SHADOWED "
        "(PERMIT=4'b0011)");
  status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(
      !bitfield_bit32_read(status, AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT),
      "Blocked sub-word write to CTRL_SHADOWED must not set "
      "ALERT_RECOV_CTRL_UPDATE_ERR");

  access_fault_seen = false;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)ecb_manual_ctrl;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)ecb_manual_ctrl;
  CHECK(!access_fault_seen,
        "Expected no StoreAccessFault on 16-bit write to CTRL_SHADOWED+0");

  access_fault_seen = false;
  *(volatile uint16_t *)(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET) = 0x1234u;
  CHECK(access_fault_seen,
        "Expected StoreAccessFault on 16-bit write to KEY_SHARE0_0 "
        "(PERMIT=4'b1111)");

  access_fault_seen = false;
  abs_mmio_write8(kAesBase + AES_IV_0_REG_OFFSET, 0x56u);
  CHECK(access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to IV_0 (PERMIT=4'b1111)");

  access_fault_seen = false;
  abs_mmio_write8(kAesBase + AES_DATA_IN_0_REG_OFFSET, 0x78u);
  CHECK(
      access_fault_seen,
      "Expected StoreAccessFault on 8-bit write to DATA_IN_0 (PERMIT=4'b1111)");

  access_fault_seen = false;
  abs_mmio_write8(kAesBase + AES_TRIGGER_REG_OFFSET, 0u);
  CHECK(!access_fault_seen,
        "Expected no StoreAccessFault on 8-bit write to TRIGGER+0 "
        "(PERMIT=4'b0001)");

  access_fault_seen = false;
  abs_mmio_write8(kAesBase + AES_TRIGGER_REG_OFFSET + 1u, 0u);
  CHECK(
      access_fault_seen,
      "Expected StoreAccessFault on 8-bit write to TRIGGER+1 (PERMIT=4'b0001)");

  return true;
}
