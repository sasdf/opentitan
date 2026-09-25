import re

file_path = "sw/device/tests/consistency/keymgr_dpe_consistency_test.c"
with open(file_path, "r") as f:
    data = f.read()

# I will replace the abs_mmio_write16 section so it doesn't cause a fatal exception,
# or catch the exception, but in OpenTitan usually tests do EXPECTED_FATAL_ERROR
# wait, if I write16, it throws a STORE_ACCESS exception instantly ending the test!
# And it causes MCAUSE 7 (Store/AMO access fault).
# To handle an expected exception we use `EXPECT_EXCEPTION();` and then an exception handler.

new_data = """// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "keymgr_dpe_regs.h"

OTTF_DEFINE_TEST_CONFIG();

// Global flag to track if we caught the expected exception.
static volatile bool expected_exception_caught = false;

// Override the default exception handler.
void ottf_exception_handler(void) {
  // Acknowledge the fault status so we can check it
  expected_exception_caught = true;
  
  // Advance MEPC by 4 bytes (standard RISC-V instruction size) to skip the faulting instruction
  uint32_t mepc;
  asm volatile("csrr %0, mepc" : "=r"(mepc));
  mepc += 4;
  asm volatile("csrw mepc, %0" : : "r"(mepc));
}

bool test_main(void) {
  uint32_t device = kDeviceType;
  uint32_t kBase = TOP_EARLGREY_KEYMGR_DPE_CORE_BASE_ADDR;
  
  // 1. Initial Reset state check
  uint32_t working_state = abs_mmio_read32(kBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET);
  CHECK(working_state == KEYMGR_DPE_WORKING_STATE_STATE_VALUE_RESET, "Expected Reset(0) working state");

  uint32_t ctrl = abs_mmio_read32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET);
  CHECK(ctrl == 0, "Expected CONTROL_SHADOWED to be 0 at reset");
  
  // 2. Shadow Register Mismatch Error & Clear on Read
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x111);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x222);
  
  uint32_t err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK((err_code & (1u << KEYMGR_DPE_ERR_CODE_INVALID_SHADOW_UPDATE_BIT)) != 0);

  abs_mmio_write32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err_code);

  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x333);
  uint32_t readback = abs_mmio_read32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET);
  CHECK(readback == 0x111, "Read should return Stage A value");
  
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x444);
  abs_mmio_write32(kBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, 0x444); 
  err_code = abs_mmio_read32(kBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  CHECK(err_code == 0, "Expected successful shadow commit");
  
  // 3. Subword Byte-Enable Permits
  expected_exception_caught = false;
  abs_mmio_write16(kBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET, 0x1234);
  
  CHECK(expected_exception_caught, "Expected a store access fault on 16-bit write");

  uint32_t fault_status = abs_mmio_read32(kBase + KEYMGR_DPE_FAULT_STATUS_REG_OFFSET);
  CHECK((fault_status & (1u << KEYMGR_DPE_FAULT_STATUS_REGFILE_INTG_BIT)) != 0, "Expected REGFILE_INTG fault");

  LOG_INFO("keymgr_dpe consistency verified");
  return true;
}
"""

with open(file_path, "w") as f:
    f.write(new_data)
