// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/ip/rv_timer/data/rv_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_plic_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kRvTimerBase = TOP_EARLGREY_RV_TIMER_BASE_ADDR,
  kRvPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kRvTimerPlicIrqId = kTopEarlgreyPlicIrqIdRvTimerTimerExpiredHart0Timer0,
  kRvTimerPlicIpRegOffset =
      RV_PLIC_IP_0_REG_OFFSET + ((kRvTimerPlicIrqId / 32) * 4),
  kRvTimerPlicIpBitMask = 1u << (kRvTimerPlicIrqId % 32),
  kMipMtipMask = 1u << 7,
  kRvTimerAlertId = kTopEarlgreyAlertIdRvTimerFatalFault,
};

static void alert_handler_enable_alert(uint32_t alert_id) {
  uint32_t en_offset =
      ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + alert_id * 4;
  abs_mmio_write32_shadowed(kAlertHandlerBase + en_offset, 1u);
}

static bool alert_handler_is_cause_set(uint32_t alert_id) {
  uint32_t cause_offset = ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + alert_id * 4;
  return (abs_mmio_read32(kAlertHandlerBase + cause_offset) & 1u) != 0;
}

static void alert_handler_clear_cause(uint32_t alert_id) {
  uint32_t cause_offset = ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + alert_id * 4;
  abs_mmio_write32(kAlertHandlerBase + cause_offset, 1u);
}

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

bool test_main(void) {
  irq_global_ctrl(false);
  irq_timer_ctrl(false);

  uint32_t orig_ctrl = abs_mmio_read32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET);
  uint32_t orig_cfg0 = abs_mmio_read32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET);

  // 1. Verify untouched reset defaults and W/O register readbacks.
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET) == 0u,
        "INTR_ENABLE0 reset default mismatch");
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u,
        "INTR_STATE0 reset default mismatch");
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET) ==
            0xffffffffu,
        "COMPARE_LOWER0_0 reset default mismatch");
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET) ==
            0xffffffffu,
        "COMPARE_UPPER0_0 reset default mismatch");
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_ALERT_TEST_REG_OFFSET) == 0u,
        "ALERT_TEST W/O readback mismatch");
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_TEST0_REG_OFFSET) == 0u,
        "INTR_TEST0 W/O readback mismatch");

  // 2. Verify CFG0 is writable even while CTRL.ACTIVE0 == 1 (RTL has no !active
  //    write gating on u_cfg0_prescale / u_cfg0_step).
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET) == 1u,
        "CTRL.ACTIVE0 should be 1");
  uint32_t active_cfg = (5u << RV_TIMER_CFG0_STEP_OFFSET) | 0x123u;
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   0xffffffffu);  // Test bitmask while active
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET) == 0x00ff0fffu,
        "CFG0 mask or write-while-active failed: got 0x%08x",
        abs_mmio_read32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET));
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET, active_cfg);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET) == active_cfg,
        "CFG0 write while CTRL.ACTIVE0==1 failed: got 0x%08x",
        abs_mmio_read32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET));

  // 3. Verify partial 32-bit write to TIMER_V_UPPER0 while CTRL.ACTIVE0 == 1
  //    does not rewind TIMER_V_LOWER0 back to the value at origin_ns.
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   (1u << RV_TIMER_CFG0_STEP_OFFSET) | 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  for (volatile int i = 0; i < 200; ++i) {
  }
  uint32_t lower_before =
      abs_mmio_read32(kRvTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(lower_before > 0u, "TIMER_V_LOWER0 did not advance while active");
  abs_mmio_write32(kRvTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET,
                   0x12345678u);
  uint32_t lower_after =
      abs_mmio_read32(kRvTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  uint32_t upper_after =
      abs_mmio_read32(kRvTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  CHECK(upper_after == 0x12345678u, "TIMER_V_UPPER0 write mismatch: got 0x%08x",
        upper_after);
  CHECK(lower_after >= lower_before,
        "Writing TIMER_V_UPPER0 while active rewound TIMER_V_LOWER0 from %u to "
        "%u",
        lower_before, lower_after);

  // 4. Verify CFG0.STEP == 0 still asserts INTR_STATE0 when CTRL.ACTIVE0 == 1
  //    and mtime >= mtimecmp (timer_core.sv: assign intr[t] = active & (mtime
  //    >= mtimecmp[t])).
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET, 0u);  // STEP = 0
  abs_mmio_write32(kRvTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 100u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 50u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u,
        "INTR_STATE0 should be 0 while CTRL.ACTIVE0 == 0");
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u,
        "INTR_STATE0 should assert when active and mtime >= mtimecmp even with "
        "STEP==0");
  // While CTRL.ACTIVE0 == 1 and mtime (100) >= mtimecmp (50), intr[0] is
  // continuously 1 on every clk_i cycle, so a W1C write to INTR_STATE0 is
  // immediately re-latched to 1 before the next MMIO read completes.
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u,
        "INTR_STATE0 should remain 1 after W1C while active and mtime >= "
        "mtimecmp");
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u,
        "INTR_STATE0 W1C clear failed");

  // 5. Verify writing COMPARE_LOWER0_0 / COMPARE_UPPER0_0 while CTRL.ACTIVE0 ==
  // 0
  //    clears INTR_STATE0 AND deasserts the external IRQ lines (mip.MTIP and
  //    PLIC IP).
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1u);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_TEST0_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u,
        "INTR_TEST0 failed to set INTR_STATE0");
  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK((mip & kMipMtipMask) != 0u, "mip.MTIP should be 1 when IRQ is pending");
  CHECK((abs_mmio_read32(kRvPlicBase + kRvTimerPlicIpRegOffset) &
         kRvTimerPlicIpBitMask) != 0u,
        "RV_PLIC IP for rv_timer should be 1 when IRQ is pending");

  // Write COMPARE_LOWER0_0 while CTRL.ACTIVE0 == 0: must clear INTR_STATE0 and
  // deassert mip.MTIP (and allow RV_PLIC gateway to remain 0 after
  // claim/complete).
  abs_mmio_write32(kRvTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u,
        "COMPARE write should clear INTR_STATE0");
  CSR_READ(CSR_REG_MIP, &mip);
  CHECK((mip & kMipMtipMask) == 0u,
        "mip.MTIP stayed stuck at 1 after COMPARE write cleared INTR_STATE0");
  abs_mmio_write32(kRvTimerBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0u);

  // 6. Verify ALERT_TEST generates a single pulse so ALERT_HANDLER.ALERT_CAUSE
  //    can be cleared via W1C.
  alert_handler_enable_alert(kRvTimerAlertId);
  alert_handler_clear_cause(kRvTimerAlertId);
  CHECK(!alert_handler_is_cause_set(kRvTimerAlertId),
        "rv_timer alert cause should be 0 before ALERT_TEST");
  abs_mmio_write32(kRvTimerBase + RV_TIMER_ALERT_TEST_REG_OFFSET, 1u);
  for (volatile int i = 0; i < 100; ++i) {
  }
  CHECK(alert_handler_is_cause_set(kRvTimerAlertId),
        "rv_timer ALERT_TEST did not set alert cause");
  alert_handler_clear_cause(kRvTimerAlertId);
  CHECK(!alert_handler_is_cause_set(kRvTimerAlertId),
        "rv_timer alert cause remained stuck at 1 after W1C clear");

  // 7. Verify unmapped hole 0x008..0x0fc (between CTRL at 0x004 and
  //    INTR_ENABLE0 at 0x100) raises TL-UL bus error (addrmiss=1 -> d_error=1).
  uint32_t before_fault = fault_count;
  last_mcause = 0;
  (void)abs_mmio_read32(kRvTimerBase + 0x008u);
  CHECK(fault_count == before_fault + 1u && last_mcause == 5u,
        "Expected read at RV_TIMER + 0x008 (addrmiss hole) to fault with "
        "mcause=5, got count=%u mcause=0x%x",
        fault_count - before_fault, last_mcause);

  before_fault = fault_count;
  last_mcause = 0;
  abs_mmio_write32(kRvTimerBase + 0x0fcu, 0xdeadbeefu);
  CHECK(fault_count == before_fault + 1u && last_mcause == 7u,
        "Expected write at RV_TIMER + 0x0fc (addrmiss hole) to fault with "
        "mcause=7, got count=%u mcause=0x%x",
        fault_count - before_fault, last_mcause);

  // 8. Wave 5: Verify RV_TIMER_PERMIT sub-word write checks
  // (rv_timer_reg_pkg.sv:138-149). COMPARE_LOWER0_0 (0x118) has RV_TIMER_PERMIT
  // = 4'b1111: an 8-bit write (reg_be = 4'b0001) must assert wr_err = 1 (mcause
  // = 7) and preserve the register.
  abs_mmio_write32(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
                   0x12345678u);
  before_fault = fault_count;
  last_mcause = 0;
  abs_mmio_write8(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 0xaau);
  CHECK(fault_count == before_fault + 1u && last_mcause == 7u,
        "Expected 8-bit write to COMPARE_LOWER0_0 (PERMIT=4'b1111) to fault "
        "with mcause=7, got count=%u mcause=0x%x",
        fault_count - before_fault, last_mcause);
  CHECK(abs_mmio_read32(kRvTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET) ==
            0x12345678u,
        "Sub-word write with wr_err=1 must not modify COMPARE_LOWER0_0");

  // CTRL (0x004) has RV_TIMER_PERMIT = 4'b0001: an 8-bit write at byte 0
  // (reg_be = 4'b0001) is permitted (wr_err = 0), whereas byte 1 (reg_be =
  // 4'b0010) must fault with wr_err = 1 (mcause = 7).
  before_fault = fault_count;
  abs_mmio_write8(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  CHECK(
      fault_count == before_fault,
      "8-bit write to CTRL byte 0 (PERMIT=4'b0001) must succeed without fault");
  before_fault = fault_count;
  last_mcause = 0;
  abs_mmio_write8(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET + 1u, 0x1u);
  CHECK(fault_count == before_fault + 1u && last_mcause == 7u,
        "Expected 8-bit write to CTRL+1 (PERMIT=4'b0001, reg_be=4'b0010) to "
        "fault with mcause=7");

  // Restore original rv_timer state for OTTF.
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CFG0_REG_OFFSET, orig_cfg0);
  abs_mmio_write32(kRvTimerBase + RV_TIMER_CTRL_REG_OFFSET, orig_ctrl);

  LOG_INFO("rv_timer_rtl_consistency_test passed!");
  return true;
}
