// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file pattgen_consistency_test.c
 * @brief Physical CW340 FPGA and QEMU verification of Earlgrey v2 (`trunk-v2`)
 *        `pattgen` exclusion (`0x400e0000` unmapped in `xbar_peri`,
 *        `v2_devs[OT_EG_SOC_DEV_PATTGEN].type = NULL` in `ot_earlgrey.c:2378`),
 *        PLIC IRQ / Pinmux outsel compaction, and `tlul_err_resp` bus fault
 *        semantics across all 12 v1 `PATTGEN` CSR offsets (`0x00..0x2c`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kV1PattgenBase = 0x400e0000u,
  kV1PattgenNumCsrs = 12u,
};

static volatile uint32_t g_load_faults = 0;
static volatile uint32_t g_store_faults = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcLoadAccessFault) {
    g_load_faults++;
  } else if (mcause == kIbexExcStoreAccessFault) {
    g_store_faults++;
  }
  uint32_t mepc = ibex_mepc_read();
  uint16_t insn16 = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

bool test_main(void) {
  g_load_faults = 0;
  g_store_faults = 0;
  LOG_INFO(
      "=== Earlgrey v2 PATTGEN Exclusion & xbar_peri Consistency Test ===");

  // 1. Verify all 12 v1 PATTGEN CSR offsets (0x00..0x2c) at 0x400e0000 are
  // unmapped in Earlgrey v2 xbar_peri (`tl_peri_pkg.sv` /
  // `ot_earlgrey.c:2378`), faulting with Load Access Fault (mcause = 5) on
  // 32-bit loads and Store Access Fault (mcause = 7) on 32-bit/8-bit stores.
  for (uint32_t i = 0; i < kV1PattgenNumCsrs; ++i) {
    uint32_t addr = kV1PattgenBase + (i * 4u);
    uint32_t prev_ld = g_load_faults;
    g_last_mcause = 0;
    (void)abs_mmio_read32(addr);
    CHECK(g_load_faults == prev_ld + 1u &&
              g_last_mcause == (uint32_t)kIbexExcLoadAccessFault,
          "Expected Load Access Fault (mcause=5) at unmapped v2 PATTGEN offset "
          "0x%02x",
          i * 4u);

    uint32_t prev_st = g_store_faults;
    g_last_mcause = 0;
    abs_mmio_write32(addr, 0xffffffffu);
    CHECK(g_store_faults == prev_st + 1u &&
              g_last_mcause == (uint32_t)kIbexExcStoreAccessFault,
          "Expected Store Access Fault (mcause=7) on 32-bit write at unmapped "
          "v2 PATTGEN offset 0x%02x",
          i * 4u);

    prev_st = g_store_faults;
    g_last_mcause = 0;
    abs_mmio_write8(addr, 0xffu);
    CHECK(g_store_faults == prev_st + 1u &&
              g_last_mcause == (uint32_t)kIbexExcStoreAccessFault,
          "Expected Store Access Fault (mcause=7) on 8-bit write at unmapped "
          "v2 PATTGEN offset 0x%02x",
          i * 4u);
  }
  CHECK(g_load_faults == kV1PattgenNumCsrs);
  CHECK(g_store_faults == kV1PattgenNumCsrs * 2u);

  // 2. Verify Earlgrey v2 top-level PLIC Peripheral, PLIC IRQ, and Pinmux
  // Outsel compaction resulting from PATTGEN removal:
  // - 1 PLIC peripheral removed (30 -> 29)
  // - 2 PLIC IRQs removed (185 -> 183)
  // - 4 Pinmux outsel signals removed (pda0_tx/pcl0_tx/pda1_tx/pcl1_tx: 70 ->
  // 66)
  CHECK((uint32_t)kTopEarlgreyPlicPeripheralLast == 29u,
        "Expected kTopEarlgreyPlicPeripheralLast == 29 in Earlgrey v2, got %u",
        (uint32_t)kTopEarlgreyPlicPeripheralLast);
  CHECK((uint32_t)kTopEarlgreyPlicIrqIdLast == 183u,
        "Expected kTopEarlgreyPlicIrqIdLast == 183 in Earlgrey v2, got %u",
        (uint32_t)kTopEarlgreyPlicIrqIdLast);
  CHECK((uint32_t)kTopEarlgreyPinmuxOutselLast == 66u,
        "Expected kTopEarlgreyPinmuxOutselLast == 66 in Earlgrey v2, got %u",
        (uint32_t)kTopEarlgreyPinmuxOutselLast);

  LOG_INFO("=== ALL PATTGEN V2 CONSISTENCY CHECKS OK ===");
  return true;
}
