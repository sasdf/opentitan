// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "ast_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sensor_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0u;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

enum {
  kAstBase = TOP_EARLGREY_AST_BASE_ADDR,
  kSensorCtrlBase = TOP_EARLGREY_SENSOR_CTRL_AON_BASE_ADDR,
};

bool test_main(void) {
  LOG_INFO(
      "1. Testing AST RO registers (REGA0, REGA1, REGA28) and WO REGAL...");

  // In ast_reg_top.sv:
  // - REGA0 (0x00) has SwAccessRO and RESVAL = 0x00.
  // - REGA1 (0x04) has SwAccessRO and RESVAL = 0x01.
  // - REGA28 (0x70) has SwAccessRO and RESVAL = 0x1c.
  // - REGAL (0x98) has SwAccessWO and always reads back as 0x00000000.
  CHECK(abs_mmio_read32(kAstBase + AST_REGA0_REG_OFFSET) == 0x00u);
  CHECK(abs_mmio_read32(kAstBase + AST_REGA1_REG_OFFSET) == 0x01u);
  CHECK(abs_mmio_read32(kAstBase + AST_REGA28_REG_OFFSET) == 0x1cu);
  CHECK(abs_mmio_read32(kAstBase + AST_REGAL_REG_OFFSET) == 0x00u);

  // Attempting to write to RO registers (REGA0, REGA1, REGA28) must be ignored.
  abs_mmio_write32(kAstBase + AST_REGA0_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32(kAstBase + AST_REGA1_REG_OFFSET, 0xcafebabeu);
  abs_mmio_write32(kAstBase + AST_REGA28_REG_OFFSET, 0x12345678u);

  CHECK(abs_mmio_read32(kAstBase + AST_REGA0_REG_OFFSET) == 0x00u);
  CHECK(abs_mmio_read32(kAstBase + AST_REGA1_REG_OFFSET) == 0x01u);
  CHECK(abs_mmio_read32(kAstBase + AST_REGA28_REG_OFFSET) == 0x1cu);

  // Writing to REGAL (SwAccessWO) sets ast_init_done_o = MuBi4True (visible in
  // sensor_ctrl.STATUS.AST_INIT_DONE), while reading REGAL still returns 0.
  abs_mmio_write32(kAstBase + AST_REGAL_REG_OFFSET, 0xa5a55a5au);
  CHECK(abs_mmio_read32(kAstBase + AST_REGAL_REG_OFFSET) == 0x00u);
  uint32_t sensor_status =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(sensor_status,
                            SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT) == 1);

  LOG_INFO(
      "2. Testing AST 32-bit RW registers (REGA2..REGA27, REGA29..REGA37, "
      "REGB0..REGB4)...");

  // Save and test full 32-bit writes on REGA2..REGA37 (skipping RO REGA28).
  for (uint32_t idx = 2; idx <= 37; ++idx) {
    if (idx == 28) {
      continue;
    }
    uint32_t offset = idx * sizeof(uint32_t);
    uint32_t saved = abs_mmio_read32(kAstBase + offset);
    uint32_t pattern = 0xa5a55a00u | idx;
    abs_mmio_write32(kAstBase + offset, pattern);
    CHECK(abs_mmio_read32(kAstBase + offset) == pattern);
    abs_mmio_write32(kAstBase + offset, saved);
  }

  // Test REGB0..REGB4 (0x200..0x210) reset defaults (0) and 32-bit RW access.
  for (uint32_t idx = 0; idx < 5; ++idx) {
    uint32_t offset = AST_REGB_0_REG_OFFSET + idx * sizeof(uint32_t);
    CHECK(abs_mmio_read32(kAstBase + offset) == 0x00u);
    uint32_t pattern = 0x5a5aa500u | idx;
    abs_mmio_write32(kAstBase + offset, pattern);
    CHECK(abs_mmio_read32(kAstBase + offset) == pattern);
    abs_mmio_write32(kAstBase + offset, 0x00u);
  }

  LOG_INFO(
      "3. Testing AST_PERMIT sub-word wr_err and addrmiss TL-UL faults...");
  g_fault_seen = false;
  abs_mmio_write8(kAstBase + AST_REGB_0_REG_OFFSET, 0x55u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kAstBase + AST_REGB_0_REG_OFFSET) == 0x00u);

  g_fault_seen = false;
  (void)abs_mmio_read32(kAstBase + 0x9cu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  abs_mmio_write32(kAstBase + 0x9cu, 0x12345678u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  (void)abs_mmio_read32(kAstBase + 0x214u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  return true;
}
