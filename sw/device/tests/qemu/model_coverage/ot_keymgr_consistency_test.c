// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_keymgr.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/keymgr_testutils.h"
#include "sw/device/lib/testing/kmac_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "keymgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_KEYMGR_BASE_ADDR,
};

static void verify_shadow_mismatch(uint32_t reg_offset, uint32_t v1,
                                   uint32_t v2) {
  uint32_t orig = abs_mmio_read32(kBase + reg_offset);
  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  abs_mmio_write32(kBase + reg_offset, v1);
  abs_mmio_write32(kBase + reg_offset, v2);

  uint32_t err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  CHECK((err & (1u << KEYMGR_ERR_CODE_INVALID_SHADOW_UPDATE_BIT)) != 0u);
  CHECK(abs_mmio_read32(kBase + reg_offset) == orig);

  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
}

static uint32_t wait_for_op_done(void) {
  for (uint32_t i = 0; i < 2000u; ++i) {
    uint32_t st = abs_mmio_read32(kBase + KEYMGR_OP_STATUS_REG_OFFSET) & 0x3u;
    if (st != KEYMGR_OP_STATUS_STATUS_VALUE_WIP &&
        st != KEYMGR_OP_STATUS_STATUS_VALUE_IDLE) {
      return st;
    }
    busy_spin_micros(5);
  }
  return abs_mmio_read32(kBase + KEYMGR_OP_STATUS_REG_OFFSET) & 0x3u;
}

bool test_main(void) {
  CHECK_STATUS_OK(keymgr_testutils_init_nvm_then_reset());

  // 1. Verify write-only INTR_TEST and ALERT_TEST readback as 0, and read-only
  // WORKING_STATE and FAULT_STATUS ignore writes.
  CHECK(abs_mmio_read32(kBase + KEYMGR_INTR_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ALERT_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_RESET);
  CHECK(abs_mmio_read32(kBase + KEYMGR_FAULT_STATUS_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_RESET);

  abs_mmio_write32(kBase + KEYMGR_FAULT_STATUS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + KEYMGR_FAULT_STATUS_REG_OFFSET) == 0x0u);

  // 2. Verify mismatched shadow register updates set
  // ERR_CODE.INVALID_SHADOW_UPDATE and trigger recov_operation_err alert across
  // the unlocked shadowed registers, and verify MAX_CREATOR_KEY_VER_REGWEN
  // (locked by ROM) blocks writes to MAX_CREATOR_KEY_VER_SHADOWED.
  verify_shadow_mismatch(KEYMGR_CONTROL_SHADOWED_REG_OFFSET, 0x10u, 0x20u);
  verify_shadow_mismatch(KEYMGR_RESEED_INTERVAL_SHADOWED_REG_OFFSET, 0x1111u,
                         0x2222u);
  verify_shadow_mismatch(KEYMGR_MAX_OWNER_INT_KEY_VER_SHADOWED_REG_OFFSET,
                         0x33u, 0x44u);
  verify_shadow_mismatch(KEYMGR_MAX_OWNER_KEY_VER_SHADOWED_REG_OFFSET, 0x55u,
                         0x66u);

  CHECK(abs_mmio_read32(kBase + KEYMGR_MAX_CREATOR_KEY_VER_REGWEN_REG_OFFSET) ==
        0x0u);
  uint32_t creator_ver_init =
      abs_mmio_read32(kBase + KEYMGR_MAX_CREATOR_KEY_VER_SHADOWED_REG_OFFSET);
  abs_mmio_write32(kBase + KEYMGR_MAX_CREATOR_KEY_VER_SHADOWED_REG_OFFSET,
                   ~creator_ver_init);
  abs_mmio_write32(kBase + KEYMGR_MAX_CREATOR_KEY_VER_SHADOWED_REG_OFFSET,
                   ~creator_ver_init);
  CHECK(
      abs_mmio_read32(kBase + KEYMGR_MAX_CREATOR_KEY_VER_SHADOWED_REG_OFFSET) ==
      creator_ver_init);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);

  // Verify RESEED_INTERVAL_REGWEN RW0C locking.
  uint32_t reseed_init =
      abs_mmio_read32(kBase + KEYMGR_RESEED_INTERVAL_SHADOWED_REG_OFFSET);
  CHECK(abs_mmio_read32(kBase + KEYMGR_RESEED_INTERVAL_REGWEN_REG_OFFSET) ==
        0x1u);
  abs_mmio_write32(kBase + KEYMGR_RESEED_INTERVAL_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_RESEED_INTERVAL_REGWEN_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + KEYMGR_RESEED_INTERVAL_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_RESEED_INTERVAL_REGWEN_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + KEYMGR_RESEED_INTERVAL_SHADOWED_REG_OFFSET, 0x1234u);
  abs_mmio_write32(kBase + KEYMGR_RESEED_INTERVAL_SHADOWED_REG_OFFSET, 0x1234u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_RESEED_INTERVAL_SHADOWED_REG_OFFSET) ==
        reseed_init);

  // 3. Initialize entropy complex & KMAC and advance keymgr to INIT (1).
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  dif_kmac_t kmac;
  dif_keymgr_t keymgr;
  CHECK_DIF_OK(
      dif_kmac_init(mmio_region_from_addr(TOP_EARLGREY_KMAC_BASE_ADDR), &kmac));
  CHECK_STATUS_OK(kmac_testutils_config(&kmac, /*sideload=*/true));
  CHECK_DIF_OK(dif_keymgr_init(mmio_region_from_addr(kBase), &keymgr));

  CHECK_STATUS_OK(keymgr_testutils_advance_state(&keymgr, NULL));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_INIT);

  // 4. Trigger invalid operation (OP_GENERATE_SW_OUTPUT) in INIT state.
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  uint32_t gen_sw_ctrl =
      KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT
      << KEYMGR_CONTROL_SHADOWED_OPERATION_OFFSET;
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, gen_sw_ctrl);
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, gen_sw_ctrl);
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);

  uint32_t st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  uint32_t err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  CHECK((err & (1u << KEYMGR_ERR_CODE_INVALID_OP_BIT)) != 0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_INIT);
  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));

  // 5. Advance to CREATOR_ROOT_KEY (2) and test HW sideload generation +
  // SIDELOAD_CLEAR (1=AES, 2=KMAC, 3=OTBN, 4=all slots).
  CHECK_DIF_OK(dif_keymgr_advance_state_raw(&keymgr));
  CHECK_STATUS_OK(keymgr_testutils_wait_for_operation_done(&keymgr));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY);

  dif_keymgr_versioned_key_params_t sideload_params = kKeyVersionedParams;
  sideload_params.dest = kDifKeymgrVersionedKeyDestAes;
  sideload_params.version = 0;
  CHECK_STATUS_OK(
      keymgr_testutils_generate_versioned_key(&keymgr, sideload_params));

  const dif_keymgr_versioned_key_dest_t kClearDests[4] = {
      kDifKeymgrVersionedKeyDestAes,   // clr_val = 1 (AES)
      kDifKeymgrVersionedKeyDestKmac,  // clr_val = 2 (KMAC)
      kDifKeymgrVersionedKeyDestOtbn,  // clr_val = 3 (OTBN)
      kDifKeymgrVersionedKeyDestKmac,  // clr_val = 4 (All slots)
  };
  for (uint32_t clr_val = 1u; clr_val <= 4u; ++clr_val) {
    abs_mmio_write32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET, clr_val);
    CHECK((abs_mmio_read32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET) & 0x7u) ==
          clr_val);
    sideload_params.dest = kClearDests[clr_val - 1u];
    CHECK_STATUS_OK(
        keymgr_testutils_generate_versioned_key(&keymgr, sideload_params));
    CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);
  }
  abs_mmio_write32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET, 0x0u);
  CHECK((abs_mmio_read32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET) & 0x7u) ==
        0x0u);

  // 6. Trigger non-enumerated OPERATION = 5 (behaves the same as OP_DISABLE per
  // keymgr_ctrl.sv:154) and verify transition to DISABLED (7), then verify
  // START in DISABLED triggers INVALID_OP error and recov_operation_err alert.
  uint32_t non_enum_ctrl = 5u << KEYMGR_CONTROL_SHADOWED_OPERATION_OFFSET;
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, non_enum_ctrl);
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, non_enum_ctrl);
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);
  st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_DISABLED);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);
  st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  CHECK((err & (1u << KEYMGR_ERR_CODE_INVALID_OP_BIT)) != 0u);
  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));

  LOG_INFO("ot_keymgr_consistency_test passed");
  return true;
}
