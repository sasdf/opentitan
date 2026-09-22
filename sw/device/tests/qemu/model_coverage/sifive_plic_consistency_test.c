// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_plic_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
};

static volatile bool store_access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

bool test_main(void) {
  irq_global_ctrl(false);
  irq_external_ctrl(false);

  // 1. Verify THRESHOLD0 read/write and 2-bit WARL masking.
  for (uint32_t v = 0; v <= 3u; ++v) {
    abs_mmio_write32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET, v);
    CHECK(abs_mmio_read32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET) == v);
  }
  abs_mmio_write32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_THRESHOLD0_REG_OFFSET) == 0x0u);

  // 2. Verify PRIO1 read/write and 2-bit WARL masking.
  abs_mmio_write32(kBase + RV_PLIC_PRIO1_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_PRIO1_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kBase + RV_PLIC_PRIO1_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_PRIO1_REG_OFFSET) == 0x0u);

  // 3. Verify 32-bit write to read-only IP_0 is ignored without bus fault.
  uint32_t ip0_before = abs_mmio_read32(kBase + RV_PLIC_IP_0_REG_OFFSET);
  store_access_fault_seen = false;
  abs_mmio_write32(kBase + RV_PLIC_IP_0_REG_OFFSET, 0xffffffffu);
  CHECK(!store_access_fault_seen);
  CHECK(abs_mmio_read32(kBase + RV_PLIC_IP_0_REG_OFFSET) == ip0_before);

  // 4. Verify sub-word (8-bit) write to IP_0 (RV_PLIC_PERMIT[186] == 4'b1111)
  // triggers StoreAccessFault (mcause == 7).
  store_access_fault_seen = false;
  abs_mmio_write8(kBase + RV_PLIC_IP_0_REG_OFFSET, 0xffu);
  CHECK(store_access_fault_seen);

  LOG_INFO("sifive_plic_consistency_test passed");
  return true;
}
