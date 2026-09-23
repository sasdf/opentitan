// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"
#include "rv_plic_regs.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kTestIrqId = kTopEarlgreyPlicIrqIdUart1TxWatermark,  // ID 10
  kMipMeipBit = 11u,
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
  }
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
}

bool test_main(void) {
  irq_global_ctrl(false);
  irq_external_ctrl(false);

  // Clear all IE0_0..5 words first.
  for (uint32_t i = 0; i < 6u; ++i) {
    abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);

  // =========================================================================
  // 1. [rv_plic_reg_top.sv:1260-1285] (SPEC_DOC_ERRATA):
  //    PRIO0 (0x48000000) is instantiated as a 2-bit RW flip-flop despite
  //    interrupt source 0 being reserved ("No Interrupt") in the RISC-V PLIC
  //    specification.
  // =========================================================================
  LOG_INFO(
      "Verifying [rv_plic_reg_top.sv:1260-1285] (SPEC_DOC_ERRATA): PRIO0 2-bit "
      "RW "
      "flip-flop...");
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO0_REG_OFFSET, 0xffffffffu);
  CHECK_EQ(
      abs_mmio_read32(kPlicBase + RV_PLIC_PRIO0_REG_OFFSET), 0x3u,
      "[rv_plic_reg_top.sv:1260-1285] Expected PRIO0 to latch 2-bit value 0x3");
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO0_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kPlicBase + RV_PLIC_PRIO0_REG_OFFSET), 0u,
           "[rv_plic_reg_top.sv:1260-1285] Expected PRIO0 to clear to 0");

  // =========================================================================
  // 2. [rv_plic_target.sv:42-60] (TRUE_SILICON_ERRATA) &
  //    [rv_plic_target.sv:42-60] (INTENDED_SECURITY_HARDENING):
  //    u_prim_max_tree in rv_plic_target.sv:42-60 feeds valid_i = ip_i & ie_i
  //    directly into irq_id_o (CC0) without checking prio_i > 0 or
  //    prio_i > threshold_i. Reading CC0 claims and clears IP even when
  //    PRIO[s] == 0 or PRIO[s] <= THRESHOLD0, while MIP.MEIP stays 0.
  //    Also verify CC0 8-bit truncation (0xdeadba00 | irq_id) and immediate
  //    rv_plic_gateway IP re-latching on complete while src_i == 1.
  // =========================================================================
  LOG_INFO(
      "Verifying [rv_plic_target.sv:42-60] (TRUE_SILICON_ERRATA): CC0 claims "
      "interrupts when PRIO==0 or PRIO<=THRESHOLD0...");
  uint32_t prio_reg = kPlicBase + RV_PLIC_PRIO0_REG_OFFSET + kTestIrqId * 4u;
  uint32_t irq_bit = 1u << kTestIrqId;

  // Drive UART1 tx_watermark interrupt line high (src_i[10] = 1).
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 1u);
  abs_mmio_write32(kUart1Base + UART_INTR_TEST_REG_OFFSET, 1u);
  CHECK((abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) != 0u,
        "Expected IP_0 bit 10 to latch 1 from UART1");

  // Configure PRIO10 = 0 (disabled per RISC-V PLIC spec), THRESHOLD0 = 0, and
  // IE0_0 = (1 << 10).
  abs_mmio_write32(prio_reg, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, irq_bit);

  // Verify MIP.MEIP (bit 11) is 0 because max_value (0) > threshold (0) is
  // false.
  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK_EQ((mip >> kMipMeipBit) & 1u, 0u,
           "[rv_plic_target.sv:42-60] Expected MIP.MEIP == 0 when PRIO10 == 0");

  // Read CC0: verify it claims source 10 despite PRIO10 == 0, and clears IP_0!
  uint32_t claimed_zero_prio =
      abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET);
  CHECK_EQ(claimed_zero_prio, (uint32_t)kTestIrqId,
           "[rv_plic_target.sv:42-60] Expected CC0 to claim IRQ %u even when "
           "PRIO == 0, got %u",
           kTestIrqId, claimed_zero_prio);
  CHECK_EQ(
      abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit, 0u,
      "[rv_plic_target.sv:42-60] Expected IP_0 bit 10 cleared on CC0 claim");

  // [rv_plic_target.sv:42-60]: Complete via 32-bit value 0xdeadba00 |
  // kTestIrqId while UART1 interrupt line (src_i[10]) is still high (1). Verify
  // CC0 truncates to 8 bits, completes IRQ 10, and rv_plic_gateway immediately
  // re-latches IP_0 bit 10!
  abs_mmio_write32(kPlicBase + RV_PLIC_CC0_REG_OFFSET,
                   0xdeadba00u | (uint32_t)kTestIrqId);
  CHECK(
      (abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit) != 0u,
      "[rv_plic_target.sv:42-60] Expected CC0 8-bit truncation to complete IRQ "
      "10 and gateway to immediately re-latch IP_0 bit 10 while src_i==1");

  // Now test PRIO10 = 1 with THRESHOLD0 = 2 (PRIO10 <= THRESHOLD0), and
  // deassert UART1 src_i[10] while IP_0 bit 10 remains latched.
  abs_mmio_write32(prio_reg, 1u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 2u);
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, UINT32_MAX);
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK_EQ(
      (mip >> kMipMeipBit) & 1u, 0u,
      "[rv_plic_target.sv:42-60] Expected MIP.MEIP == 0 when PRIO10 (1) <= "
      "THRESHOLD0 (2)");

  uint32_t claimed_below_thresh =
      abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET);
  CHECK_EQ(
      claimed_below_thresh, (uint32_t)kTestIrqId,
      "[rv_plic_target.sv:42-60] Expected CC0 to claim IRQ %u when PRIO10 (1) "
      "<= THRESHOLD0 (2)",
      kTestIrqId);
  abs_mmio_write32(kPlicBase + RV_PLIC_CC0_REG_OFFSET, (uint32_t)kTestIrqId);
  CHECK_EQ(abs_mmio_read32(kPlicBase + RV_PLIC_IP_0_REG_OFFSET) & irq_bit, 0u,
           "Expected IP_0 bit 10 to stay 0 after completing with src_i==0");
  CHECK_EQ(abs_mmio_read32(kPlicBase + RV_PLIC_CC0_REG_OFFSET), 0u,
           "Expected CC0 == 0 when no interrupts pending");

  // Clean up IE0_0, PRIO10, THRESHOLD0.
  abs_mmio_write32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, 0u);
  abs_mmio_write32(prio_reg, 0u);
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);

  // =========================================================================
  // 3. [rv_plic_reg_top.sv:7579 / rv_plic_reg_pkg.sv:1087-1290]
  // (INTENDED_SECURITY_HARDENING):
  //    Unmapped PLIC offsets (PRIO186 at 0x2e8, IE0_6 at 0x2018) assert
  //    addrmiss = 1 (mcause = 5/7), and RV_PLIC_PERMIT allows byte-0 sb to
  //    PRIO1 (4'b0001) while faulting byte-1 sb to PRIO1 and any sb to IE0_0
  //    (4'b1111).
  // =========================================================================
  LOG_INFO(
      "Verifying [rv_plic_reg_top.sv:7579 / rv_plic_reg_pkg.sv:1087-1290] "
      "(INTENDED_SECURITY_HARDENING): "
      "addrmiss bus faults & asymmetric RV_PLIC_PERMIT sub-word rules...");
  load_fault_count = 0;
  (void)abs_mmio_read32(kPlicBase + 0x0002e8u);  // PRIO186 (unmapped)
  CHECK_EQ(load_fault_count, 1u,
           "[rv_plic_reg_top.sv:7579] Expected Load Access Fault at PRIO186 "
           "(0x2e8)");

  load_fault_count = 0;
  (void)abs_mmio_read32(kPlicBase + 0x002018u);  // IE0_6 (unmapped)
  CHECK_EQ(load_fault_count, 1u,
           "[rv_plic_reg_top.sv:7579] Expected Load Access Fault at IE0_6 "
           "(0x2018)");

  // Byte-0 write (sb) to PRIO1 (RV_PLIC_PERMIT = 4'b0001) succeeds.
  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_PRIO1_REG_OFFSET, 0x2u);
  CHECK_EQ(
      store_fault_count, 0u,
      "[rv_plic_reg_pkg.sv:1087-1290] Expected byte-0 sb to PRIO1 to succeed");
  CHECK_EQ(
      abs_mmio_read32(kPlicBase + RV_PLIC_PRIO1_REG_OFFSET), 0x2u,
      "[rv_plic_reg_pkg.sv:1087-1290] Expected PRIO1 == 0x2 after byte-0 sb");

  // Byte-1 write (sb) to PRIO1 + 1 (reg_be = 4'b0010) faults!
  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_PRIO1_REG_OFFSET + 1u, 0x3u);
  CHECK_EQ(
      store_fault_count, 1u,
      "[rv_plic_reg_pkg.sv:1087-1290] Expected byte-1 sb to PRIO1+1 to fault");
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO1_REG_OFFSET, 0u);

  // Byte-0 write (sb) to IE0_0 (RV_PLIC_PERMIT = 4'b1111) faults and drops
  // write!
  store_fault_count = 0;
  abs_mmio_write8(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET, 0xffu);
  CHECK_EQ(
      store_fault_count, 1u,
      "[rv_plic_reg_pkg.sv:1087-1290] Expected byte-0 sb to IE0_0 to fault");
  CHECK_EQ(
      abs_mmio_read32(kPlicBase + RV_PLIC_IE0_0_REG_OFFSET), 0u,
      "[rv_plic_reg_pkg.sv:1087-1290] Expected sub-word write to IE0_0 to be "
      "dropped");

  LOG_INFO("All RV_PLIC errata & security hardening checks passed!");
  return true;
}
