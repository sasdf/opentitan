// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rv_plic_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2`) hardware,
 *        specification, and DIF discrepancies in `rv_plic`
 *        (`hw/top_earlgrey/ip_autogen/rv_plic`, `hw/ip_templates/rv_plic`, and
 *        `sw/device/lib/dif/dif_rv_plic.c`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_rv_plic.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/rv_plic_regs.h"
#include "hw/top/uart_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kTestIrqId = kTopEarlgreyPlicIrqIdUart1TxWatermark,  // 10
  kMipMeipBit = 11,
};

static volatile uint32_t load_fault_count = 0;
static volatile uint32_t store_fault_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_count++;
  } else if (mcause == kIbexExcStoreAccessFault) {
    store_fault_count++;
  } else {
    ottf_generic_fault_print(exc_info, "Load/Store Fault", mcause);
    abort();
  }
}

bool test_main(void) {
  LOG_INFO("Starting rv_plic Earlgrey v2 FPGA verification test...");

  // Ensure CPU M-mode external interrupts (MIE.MEIE) are disabled so we can
  // inspect MIP.MEIP and CC0 directly without jumping into an external ISR.
  CSR_CLEAR_BITS(CSR_REG_MIE, 1u << kMipMeipBit);

  // Clear all PLIC IE0 words and threshold.
  for (uint32_t i = 0; i < RV_PLIC_IE0_MULTIREG_COUNT; ++i) {
    abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);

  // 1. Verify PRIO_0 (0x48000000) and IE0_0[0] (0x48002000 bit 0) are
  //    instantiated as RW flip-flops despite ID 0 being reserved ("No
  //    Interrupt") in the RISC-V PLIC spec (rv_plic_reg_top.sv).
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET) == 0u);
  LOG_INFO("Confirmed PRIO_0 2-bit RW flip-flop.");

  // 2. Verify CC0 (0x48200004) claims and clears IP for enabled pending
  //    interrupts even when PRIO[s] == 0 or PRIO[s] <= THRESHOLD0, while
  //    MIP.MEIP stays 0 (rv_plic_target.sv:42-60). Also verify 8-bit truncation
  //    on CC0 completion writes (0xdeadba00 | irq_id) and immediate gateway
  //    IP re-latching while src_i == 1 (rv_plic_gateway.sv:82-106).
  uint32_t prio_reg = kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET + kTestIrqId * 4u;
  uint32_t irq_bit = 1u << kTestIrqId;

  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 1u);
  abs_mmio_write32(kUart1Base + UART_INTR_TEST_REG_OFFSET, 1u);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) != 0u);

  abs_mmio_write32(prio_reg, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, irq_bit);

  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK(((mip >> kMipMeipBit) & 1u) == 0u);

  uint32_t claimed_zero_prio =
      abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET);
  CHECK(claimed_zero_prio == (uint32_t)kTestIrqId);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) == 0u);

  // Complete via 32-bit value 0xdeadba00 | kTestIrqId while src_i[10] == 1.
  abs_mmio_write32(kPlicBase + RV_PLIC_CC0_REG_OFFSET,
                   0xdeadba00u | (uint32_t)kTestIrqId);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) != 0u);

  // Now test PRIO_10 = 1 <= THRESHOLD0 = 2 with src_i[10] cleared to 0.
  abs_mmio_write32(prio_reg, 1u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 2u);
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, UINT32_MAX);
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK(((mip >> kMipMeipBit) & 1u) == 0u);

  uint32_t claimed_below_thresh =
      abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET);
  CHECK(claimed_below_thresh == (uint32_t)kTestIrqId);
  abs_mmio_write32(kPlicBase + RV_PLIC_CC0_REG_OFFSET, (uint32_t)kTestIrqId);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) == 0u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET) == 0u);

  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, 0u);
  abs_mmio_write32(prio_reg, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);
  LOG_INFO("Confirmed CC0 claim with PRIO==0 / PRIO<=THRESHOLD0 & 8-bit CC0.");

  // 3. Verify asymmetric RV_PLIC_PERMIT sub-word write faults and unmapped
  //    aperture addrmiss faults (rv_plic_reg_pkg.sv:1415-1609).
  load_fault_count = 0;
  (void)abs_mmio_read32(kPlicBase + 0x002018u);  // IE0_6 (unmapped)
  CHECK(load_fault_count == 1u);

  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_PRIO_1_REG_OFFSET, 0x2u);
  CHECK(store_fault_count == 0u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO_1_REG_OFFSET) == 0x2u);

  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_PRIO_1_REG_OFFSET + 1u, 0x3u);
  CHECK(store_fault_count == 1u);
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO_1_REG_OFFSET, 0u);

  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, 0xffu);
  CHECK(store_fault_count == 1u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET) == 0u);
  LOG_INFO("Confirmed RV_PLIC_PERMIT sub-word rules and IE0_6 addrmiss.");

  // 4. Verify v2 NumSrc = 184 shrinkage (from 186 in v1):
  //    - PRIO_183 (0x2dc) is the highest valid PRIO register (latches 0x3),
  //      whereas PRIO_184 (0x2e0) and PRIO_185 (0x2e4) now fault with
  //      Load/Store Access Fault (addrmiss = 1, mcause = 5/7).
  //    - IE0_5 (0x2014) masks bits [31:24] to 0 (0x00ffffff vs 0x03ffffff in
  //      v1) and has RV_PLIC_PERMIT[195] = 4'b0111 (rejecting both sb 4'b0001
  //      and sh 4'b0011 sub-word writes with mcause = 7, while accepting 4-byte
  //      sw 4'b1111).
  CHECK(RV_PLIC_PARAM_NUM_SRC == 184u);
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO_183_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO_183_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO_183_REG_OFFSET, 0u);

  load_fault_count = 0;
  (void)abs_mmio_read32(kPlicBase + 0x0002e0u);  // PRIO_184 (unmapped in v2)
  CHECK(load_fault_count == 1u);
  load_fault_count = 0;
  (void)abs_mmio_read32(kPlicBase + 0x0002e4u);  // PRIO_185 (unmapped in v2)
  CHECK(load_fault_count == 1u);

  store_fault_count = 0;
  *((volatile uint16_t *)(kPlicBase + RV_PLIC_IE0_5_REG_OFFSET)) = 0xffffu;
  CHECK(store_fault_count == 1u);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_IE0_5_REG_OFFSET) == 0u);

  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_5_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_IE0_5_REG_OFFSET) == 0x00ffffffu);
  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_5_REG_OFFSET, 0u);
  LOG_INFO("Confirmed v2 NumSrc=184 PRIO_184/185 addrmiss & IE0_5 0x00ffffff.");

  // 5. Verify dif_rv_plic API acceptance of reserved irq=0 ("No Interrupt")
  //    and unvalidated 32-bit complete_data in dif_rv_plic_irq_complete()
  //    (sw/device/lib/dif/dif_rv_plic.c:137-197, 256-269).
  dif_rv_plic_t plic;
  CHECK_DIF_OK(dif_rv_plic_init(mmio_region_from_addr(kPlicBase), &plic));

  CHECK_DIF_OK(dif_rv_plic_irq_set_priority(&plic, 0, 2u));
  CHECK(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO_0_REG_OFFSET) == 2u);

  CHECK_DIF_OK(dif_rv_plic_irq_set_enabled(&plic, 0, 0, kDifToggleEnabled));
  dif_toggle_t irq0_enabled = kDifToggleDisabled;
  CHECK_DIF_OK(dif_rv_plic_irq_get_enabled(&plic, 0, 0, &irq0_enabled));
  CHECK(irq0_enabled == kDifToggleEnabled);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET) & 1u) == 1u);

  CHECK_DIF_OK(dif_rv_plic_irq_set_enabled(&plic, 0, 0, kDifToggleDisabled));
  CHECK_DIF_OK(dif_rv_plic_irq_set_priority(&plic, 0, 0u));

  // Verify dif_rv_plic_irq_complete() accepts out-of-range complete_data >= 184
  // (0xdeadba00 | kTestIrqId) and returns kDifOk.
  CHECK_DIF_OK(dif_rv_plic_irq_complete(
      &plic, 0, (dif_rv_plic_irq_id_t)(0xdeadba00u | (uint32_t)kTestIrqId)));
  LOG_INFO("Confirmed dif_rv_plic irq=0 and out-of-range complete_data.");

  LOG_INFO("All rv_plic Earlgrey v2 FPGA checks PASSED!");
  return true;
}
