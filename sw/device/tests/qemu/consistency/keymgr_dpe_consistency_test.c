// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/keymgr_dpe_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

// Global flag to track if we caught the expected exception.
static volatile bool expected_exception_caught = false;

// Override the default exception handler.
void ottf_exception_handler(void) {
  // Acknowledge the fault status so we can check it
  expected_exception_caught = true;

  // Advance MEPC by 4 bytes (standard RISC-V instruction size) to skip the
  // faulting instruction
  uint32_t mepc;
  asm volatile("csrr %0, mepc" : "=r"(mepc));
  mepc += 4;
  asm volatile("csrw mepc, %0" : : "r"(mepc));
}

static void run_op_expect_success(uint32_t base) {
  abs_mmio_write32(base + KEYMGR_DPE_START_REG_OFFSET, 1);
  uint32_t op_status;
  do {
    op_status = abs_mmio_read32(base + KEYMGR_DPE_OP_STATUS_REG_OFFSET);
  } while (op_status == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP);
  abs_mmio_write32(base + KEYMGR_DPE_OP_STATUS_REG_OFFSET, op_status);
  CHECK(op_status == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
        "Expected DONE_SUCCESS, got 0x%x", op_status);
  CHECK(abs_mmio_read32(base + KEYMGR_DPE_ERR_CODE_REG_OFFSET) == 0,
        "Expected ERR_CODE == 0");
  CHECK(abs_mmio_read32(base + KEYMGR_DPE_DEBUG_REG_OFFSET) == 0,
        "Expected DEBUG == 0");
}

static void run_op_expect_error(uint32_t base, uint32_t expected_err_code,
                                uint32_t expected_debug) {
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  abs_mmio_write32(base + KEYMGR_DPE_START_REG_OFFSET, 1);

  uint32_t op_status;
  do {
    op_status = abs_mmio_read32(base + KEYMGR_DPE_OP_STATUS_REG_OFFSET);
  } while (op_status == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP);
  abs_mmio_write32(base + KEYMGR_DPE_OP_STATUS_REG_OFFSET, op_status);
  CHECK(op_status == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR,
        "Expected DONE_ERROR, got 0x%x", op_status);

  uint32_t err_code = abs_mmio_read32(base + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == expected_err_code, "Expected ERR_CODE 0x%x, got 0x%x",
        expected_err_code, err_code);

  uint32_t debug = abs_mmio_read32(base + KEYMGR_DPE_DEBUG_REG_OFFSET);
  CHECK(debug == expected_debug, "Expected DEBUG 0x%x, got 0x%x",
        expected_debug, debug);

  busy_spin_micros(50);
  abs_mmio_write32(base + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);
  abs_mmio_write32(base + KEYMGR_DPE_DEBUG_REG_OFFSET, 0);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));
}

static uint32_t make_ctrl(uint32_t op, uint32_t src_slot, uint32_t dst_slot,
                          uint32_t dest_sel, bool sw_binding_only) {
  uint32_t ctrl = 0;
  ctrl = bitfield_field32_write(
      ctrl, KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_FIELD, op);
  ctrl = bitfield_field32_write(
      ctrl, KEYMGR_DPE_CONTROL_SHADOWED_SLOT_SRC_SEL_FIELD, src_slot);
  ctrl = bitfield_field32_write(
      ctrl, KEYMGR_DPE_CONTROL_SHADOWED_SLOT_DST_SEL_FIELD, dst_slot);
  ctrl = bitfield_field32_write(
      ctrl, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_FIELD, dest_sel);
  ctrl = bitfield_bit32_write(
      ctrl, KEYMGR_DPE_CONTROL_SHADOWED_SW_BINDING_ONLY_BIT, sw_binding_only);
  return ctrl;
}

static void read_sw_key(uint32_t base, uint32_t key_out[8]) {
  uint32_t nonzero = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t s0 = abs_mmio_read32(
        base + KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET + i * 4);
    uint32_t s1 = abs_mmio_read32(
        base + KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET + i * 4);
    key_out[i] = s0 ^ s1;
    nonzero |= key_out[i];
  }
  CHECK(nonzero != 0, "Expected non-zero generated SW key");
}

bool test_main(void) {
  uint32_t kBase = TOP_EARLGREY_KEYMGR_DPE_BASE_ADDR;

  // 1. Check working state after ROM boot
  uint32_t working_state =
      abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET);
  CHECK(working_state == KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE,
        "Expected Available(1) working state after ROM");

  // 2. Shadow Register Mismatch Error & Clear on Read (CONTROL,
  // RESEED_INTERVAL, MAX_KEY_VER) + W/O & R/O & REGWEN checks
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x111);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x222);
  uint32_t err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == (1u << KEYMGR_DPE_ERR_CODE_INVALID_SHADOW_UPDATE_BIT));
  busy_spin_micros(50);
  abs_mmio_write32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);
  (void)abs_mmio_read32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET,
                   0x1234);
  abs_mmio_write32(kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET,
                   0x5678);
  err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == (1u << KEYMGR_DPE_ERR_CODE_INVALID_SHADOW_UPDATE_BIT));
  busy_spin_micros(50);
  abs_mmio_write32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);
  (void)abs_mmio_read32(kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  // Lock RESEED_INTERVAL_REGWEN and verify writes to RESEED_INTERVAL_SHADOWED
  // are ignored
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET, 0x4321);
  CHECK(abs_mmio_read32(
            kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET) == 0x4321);
  abs_mmio_write32(kBase + KEYMGR_DPE_RESEED_INTERVAL_REGWEN_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_RESEED_INTERVAL_REGWEN_REG_OFFSET) ==
        0);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET, 0x9999);
  CHECK(abs_mmio_read32(
            kBase + KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET) == 0x4321);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 0x10);
  abs_mmio_write32(kBase + KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 0x20);
  err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == (1u << KEYMGR_DPE_ERR_CODE_INVALID_SHADOW_UPDATE_BIT));
  busy_spin_micros(50);
  abs_mmio_write32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);
  (void)abs_mmio_read32(kBase + KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  // W/O registers read back as 0; R/O registers ignore writes
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_INTR_TEST_REG_OFFSET) == 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_ALERT_TEST_REG_OFFSET) == 0);
  abs_mmio_write32(kBase + KEYMGR_DPE_CFG_REGWEN_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_CFG_REGWEN_REG_OFFSET) == 1);
  abs_mmio_write32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE);
  abs_mmio_write32(kBase + KEYMGR_DPE_FAULT_STATUS_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_FAULT_STATUS_REG_OFFSET) == 0);

  // 3. Test ERASE_SLOT, LOAD_ROOT_KEY, LOAD_KEY_LOCK, and uninitialized vs
  // erased slot key_vld checks
  // 3a. ERASE_SLOT on empty Slot 2 -> INVALID_OP (0x1)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase, 1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT, 0);

  // 3b. LOAD_ROOT_KEY on already-valid Slot 0 -> INVALID_OP (0x1)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY, 0, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase, 1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT, 0);

  // 3c. GENERATE_SW_OUTPUT on uninitialized Slot 3 (valid=0, key=0) ->
  // INVALID_OP | INVALID_KMAC_INPUT (0x3) and DEBUG.INVALID_KEY
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                3, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      1u << KEYMGR_DPE_DEBUG_INVALID_KEY_BIT);

  // 3d. LOAD_ROOT_KEY on empty Slot 2 -> DONE_SUCCESS, then ERASE_SLOT on
  // Slot 2 -> DONE_SUCCESS (randomizes Slot 2 key shares)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  // 3e. GENERATE_SW_OUTPUT on erased Slot 2 (valid=0, key!=0) -> INVALID_OP
  // (0x1) only, DEBUG == 0
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                2, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase, 1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT, 0);

  // 3f. Reload UDS into Slot 2, then lock LOAD_KEY_LOCK (RW1S) and verify
  // LOAD_ROOT_KEY on empty Slot 3 is rejected
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET) == 0);
  abs_mmio_write32(kBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET, 1);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET) == 1);
  abs_mmio_write32(kBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET) == 1);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY, 0, 3,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase, 1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT, 0);

  // 4. Verify policy checks & 3-stage (Creator -> OwnerInt -> Owner -> Runtime)
  // KDF behavior on Earlgrey
  // Slot 2 has UDS (boot_stage=0, retain_parent=0, allow_child=1).
  // 4a. Test invalid_retain_parent when retain_parent=0 (src != dst)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 3,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      0);

  // 4b. Advance Slot 2 in-place (0 -> 1 = BootStageOwnerInt) with
  // retain_parent=1, allow_child=1, max_key_ver=2
  abs_mmio_write32_shadowed(kBase + KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET,
                            2);
  abs_mmio_write32(kBase + KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET, 0);
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + i * 4,
                     0x10 + i);
    CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + i * 4) ==
          0x10 + i);
  }
  abs_mmio_write32(kBase + KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET, 0);
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET,
                   (1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT) |
                       (1u << KEYMGR_DPE_SLOT_POLICY_RETAIN_PARENT_BIT));
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET, 0);

  // Verify writes to locked SW_BINDING / SLOT_POLICY / MAX_KEY_VER are ignored
  abs_mmio_write32(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET, 0xdeadbeef);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET) == 0x10);
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET) ==
        ((1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT) |
         (1u << KEYMGR_DPE_SLOT_POLICY_RETAIN_PARENT_BIT)));

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  // 4c. Test invalid_retain_parent when retain_parent=1 (src == dst, or dst
  // already occupied)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      0);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      0);

  // 4d. Erase Slot 0 and Slot 1, then advance Slot 2 (boot_stage=1 =
  // BootStageOwnerInt) into both Slot 0 and Slot 1 (1 -> 2 = BootStageOwner,
  // consuming creator_seed) with identical inputs
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  abs_mmio_write32_shadowed(kBase + KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET,
                            2);
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET,
                   1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 2, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  // 4e. Advance Slot 0 (2 -> 3 = BootStageRuntime) with sw_binding_only=false
  // (consumes owner_seed) and Slot 1 (2 -> 3 = BootStageRuntime) with
  // sw_binding_only=true (skips owner_seed). Verify their derived SW keys
  // differ!
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET,
                   (1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT) |
                       (1u << KEYMGR_DPE_SLOT_POLICY_RETAIN_PARENT_BIT));
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 0, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  // Set allow_child=0 on Slot 1 so we can also test invalid_allow_child
  abs_mmio_write32(kBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 0);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 1, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, true));
  run_op_expect_success(kBase);

  // Test invalid_allow_child on Slot 1
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 1, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      0);

  // Program SALT and verify readback
  abs_mmio_write32(kBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 1);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET) == 1);
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kBase + KEYMGR_DPE_SALT_0_REG_OFFSET + i * 4, 0x100 + i);
    CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_SALT_0_REG_OFFSET + i * 4) ==
          0x100 + i);
  }

  uint32_t key_slot0[8], key_slot1[8];
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                0, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  read_sw_key(kBase, key_slot0);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                1, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  read_sw_key(kBase, key_slot1);

  bool owner_seed_mixed = false;
  for (uint32_t i = 0; i < 8; ++i) {
    if (key_slot0[i] != key_slot1[i]) {
      owner_seed_mixed = true;
    }
  }
  CHECK(owner_seed_mixed,
        "Expected BootStageOwner (stage 2) advance to mix owner_seed when "
        "sw_binding_only=false");

  // 4f. Test key_version > max_key_version error on Slot 0 (max_key_ver=2)
  abs_mmio_write32(kBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 3);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                0, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase,
                      (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                          (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT),
                      1u << KEYMGR_DPE_DEBUG_INVALID_KEY_VERSION_BIT);
  abs_mmio_write32(kBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 0);

  // 4g. Verify BootStageRuntime (3 -> 3) advance produces identical keys
  // whether sw_binding_only is false or true
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 0, 1,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE, 0, 2,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, true));
  run_op_expect_success(kBase);

  uint32_t rt_key1[8], rt_key2[8];
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                1, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  read_sw_key(kBase, rt_key1);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                2, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  read_sw_key(kBase, rt_key2);

  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(rt_key1[i] == rt_key2[i],
          "Expected BootStageRuntime (stage 3) keys to match at word %u", i);
  }

  // 5. Test GENERATE_HW_OUTPUT across DEST_SEL values and SIDELOAD_CLEAR
  const uint32_t kDestSels[] = {
      KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE,
      KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_AES,
      KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_KMAC,
      KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_OTBN,
  };
  for (size_t i = 0; i < ARRAYSIZE(kDestSels); ++i) {
    abs_mmio_write32_shadowed(
        kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
        make_ctrl(
            KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_HW_OUTPUT, 0,
            0, kDestSels[i], false));
    run_op_expect_success(kBase);
  }

  abs_mmio_write32(kBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_DPE_SIDELOAD_CLEAR_VAL_VALUE_AES);
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_HW_OUTPUT,
                0, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_AES, false));
  run_op_expect_success(kBase);

  abs_mmio_write32(kBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET, 7);
  abs_mmio_write32(kBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_DPE_SIDELOAD_CLEAR_VAL_VALUE_NONE);

  // 6. Test OP_DISABLE -> WORKING_STATE == DISABLED, and verify operations in
  // DISABLED state fail with INVALID_OP (0x1)
  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_DISABLE, 0, 0,
                KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_success(kBase);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_DPE_WORKING_STATE_STATE_VALUE_DISABLED);

  abs_mmio_write32_shadowed(
      kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
                0, 0, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, false));
  run_op_expect_error(kBase, 1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT, 0);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_DPE_WORKING_STATE_STATE_VALUE_DISABLED);

  // 7. Subword Byte-Enable Permits (triggers store access fault)
  expected_exception_caught = false;
  *(volatile uint16_t *)(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET) = 0x1234;

  CHECK(expected_exception_caught,
        "Expected a store access fault on 16-bit write");

  LOG_INFO("keymgr_dpe consistency verified");
  return true;
}
