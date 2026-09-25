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

bool test_main(void) {
  uint32_t kBase = TOP_EARLGREY_KEYMGR_DPE_BASE_ADDR;

  // 1. Check working state after ROM boot
  uint32_t working_state =
      abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET);
  CHECK(working_state == KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE,
        "Expected Available(1) working state after ROM");

  // 2. Shadow Register Mismatch Error & Clear on Read (trigers Recov alert)
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x111);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x222);

  uint32_t err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK((err_code & (1u << KEYMGR_DPE_ERR_CODE_INVALID_SHADOW_UPDATE_BIT)) !=
        0);

  // Allow time for alert propagation to the core
  busy_spin_micros(100);

  // Check alert
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  abs_mmio_write32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);

  // Clear the error state by reading the register
  (void)abs_mmio_read32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET);

  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x333);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x333);
  err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == 0, "Expected successful shadow commit");

  // 4. SIDELOAD_CLEAR Priority
  // Write '7' to clear AES, KMAC, OTBN sideload sink states
  abs_mmio_write32(kBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET, 7);

  // 5. Operation Boundary / KDF packing errors
  // Trigger an INVALID_OP by using an uninitialized slot (Slot 7)
  // Setup CONTROL_SHADOWED Operation=GenerateSwOutput (3<<4 = 0x30) |
  // SLOT_SRC_SEL=7 (7<<14 = 0x1C000)
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x1C030);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x1C030);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  // Trigger operation
  abs_mmio_write32(kBase + KEYMGR_DPE_START_REG_OFFSET, 1);

  busy_spin_micros(100);

  err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  LOG_INFO("err_code after op: 0x%08x", err_code);
  CHECK((err_code & (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT)) != 0,
        "Expected INVALID_OP");

  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrDpeRecovOperationErr));

  // 3. Subword Byte-Enable Permits (triggers store access fault)
  expected_exception_caught = false;
  *(volatile uint16_t *)(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET) = 0x1234;

  CHECK(expected_exception_caught,
        "Expected a store access fault on 16-bit write");

  LOG_INFO("keymgr_dpe consistency verified");
  return true;
}
