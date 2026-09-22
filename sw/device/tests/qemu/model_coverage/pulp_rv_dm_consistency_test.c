// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_dm_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRvDmRegsBase = TOP_EARLGREY_RV_DM_REGS_BASE_ADDR,
  kRvDmMemBase = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR,
};

static volatile bool load_fault_seen = false;
static volatile bool store_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_seen = true;
  } else if (mcause == kIbexExcStoreAccessFault) {
    store_fault_seen = true;
  } else {
    CHECK(false, "Unexpected exception mcause=0x%x", mcause);
  }
}

static bool expect_load_fault_32(uint32_t addr) {
  load_fault_seen = false;
  (void)abs_mmio_read32(addr);
  return load_fault_seen;
}

static bool expect_load_fault_8(uint32_t addr) {
  load_fault_seen = false;
  (void)abs_mmio_read8(addr);
  return load_fault_seen;
}

static bool expect_store_fault_32(uint32_t addr, uint32_t val) {
  store_fault_seen = false;
  abs_mmio_write32(addr, val);
  return store_fault_seen;
}

bool test_main(void) {
  // 1. RV_DM_REGS (0x41200000) reset values and out-of-range decode fault.
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kRvDmRegsBase +
                        RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 1u);
  uint32_t orig_late_dbg =
      abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
  CHECK(orig_late_dbg == kMultiBitBool32False ||
        orig_late_dbg == kMultiBitBool32True);
  CHECK(expect_load_fault_32(kRvDmRegsBase + 0x0cu));
  CHECK(expect_store_fault_32(kRvDmRegsBase + 0x0cu, 0u));

  // Enable LATE_DEBUG_ENABLE so RV_DM.MEM is accessible in DEV/TEST_UNLOCKED.
  abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                   kMultiBitBool32True);

  // 2. RV_DM_MEM (0x00010000) unmapped offset 0x20 triggers Load/Store fault.
  CHECK(expect_load_fault_32(kRvDmMemBase + 0x20u));
  CHECK(expect_store_fault_32(kRvDmMemBase + 0x20u, 0u));

  /*
   * 3. Sub-word read at mapped dmact register (HaltedAddr + 1 = 0x101):
   *    In rv_dm.sv + dm_mem.sv, tlul_adapter_reg word-aligns addr_o and
   *    gen_wr_err returns 0 when we_i == 0 (read), so sub-word reads at mapped
   *    dmact addresses succeed and return 0.
   *    Conversely, sub-word read at an unmapped dmact hole (0x105) triggers
   *    LoadAccessFault (mcause = 5).
   */
  load_fault_seen = false;
  uint8_t val8 = abs_mmio_read8(kRvDmMemBase + 0x101u);
  CHECK(!load_fault_seen);
  CHECK(val8 == 0u);
  CHECK(expect_load_fault_8(kRvDmMemBase + 0x105u));
  LOG_INFO("pulp_rv_dm_consistency_test: dmact sub-word read verified");

  // 4. dmact unmapped hole (0x104) write triggers StoreAccessFault
  // (hw/misc/pulp_rv_dm.c:462-466).
  CHECK(expect_store_fault_32(kRvDmMemBase + 0x104u, 0u));

  // 5. dmact R_EXCEPTION (0x118) write succeeds without fault
  // (hw/misc/pulp_rv_dm.c:532-535).
  store_fault_seen = false;
  abs_mmio_write32(kRvDmMemBase + 0x118u, 0u);
  CHECK(!store_fault_seen);

  // 6. Abstract Command ROM (0x35c = ebreak 0x00100073) and Program Buffer
  // (0x360 = 0x00000000) readbacks.
  CHECK(abs_mmio_read32(kRvDmMemBase + 0x35cu) == 0x00100073u);
  CHECK(abs_mmio_read32(kRvDmMemBase + 0x360u) == 0x00000000u);

  // 7. dmflag (0x400) hart write is ignored as R/O without faulting
  // (hw/misc/pulp_rv_dm.c:638-642).
  store_fault_seen = false;
  abs_mmio_write32(kRvDmMemBase + 0x400u, 0xffffffffu);
  CHECK(!store_fault_seen);
  CHECK(abs_mmio_read32(kRvDmMemBase + 0x400u) == 0u);

  abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                   orig_late_dbg);
  return true;
}
