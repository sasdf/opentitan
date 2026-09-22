// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_plic_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  kMipMsipBit = 3u,
};

bool test_main(void) {
  irq_global_ctrl(false);
  irq_software_ctrl(false);

  // 1. Verify write-only ALERT_TEST register readback returns 0 and writing 0
  // succeeds without firing an alert.
  CHECK(abs_mmio_read32(kBase + RV_PLIC_ALERT_TEST_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + RV_PLIC_ALERT_TEST_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_ALERT_TEST_REG_OFFSET) == 0x0u);

  // 2. Verify MSIP0 initial state, 1-bit masking, sub-word readback, and
  // CSR MIP.MSIP propagation.
  CHECK(abs_mmio_read32(kBase + RV_PLIC_MSIP0_REG_OFFSET) == 0x0u);

  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK(((mip >> kMipMsipBit) & 0x1u) == 0x0u);

  abs_mmio_write32(kBase + RV_PLIC_MSIP0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_MSIP0_REG_OFFSET) == 0x1u);
  CHECK(abs_mmio_read8(kBase + RV_PLIC_MSIP0_REG_OFFSET) == 0x1u);
  CHECK(abs_mmio_read8(kBase + RV_PLIC_MSIP0_REG_OFFSET + 1u) == 0x0u);

  CSR_READ(CSR_REG_MIP, &mip);
  CHECK(((mip >> kMipMsipBit) & 0x1u) == 0x1u);

  // 3. Clear MSIP0 via byte write and verify MIP.MSIP clears.
  abs_mmio_write8(kBase + RV_PLIC_MSIP0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_MSIP0_REG_OFFSET) == 0x0u);

  CSR_READ(CSR_REG_MIP, &mip);
  CHECK(((mip >> kMipMsipBit) & 0x1u) == 0x0u);

  LOG_INFO("ot_plic_ext_consistency_test passed");
  return true;
}
