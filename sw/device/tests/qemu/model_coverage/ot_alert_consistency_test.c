// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kUart1AlertId = kTopEarlgreyAlertIdUart1FatalFault,
};

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

bool test_main(void) {
  // 1. Sub-Word Byte Reads on alert_handler Registers (ot_alert.c:979-981).
  CHECK(abs_mmio_read8(kAlertBase + ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read8(kAlertBase + ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET +
                       1u) == 0u);
  CHECK(abs_mmio_read8(kAlertBase +
                       ALERT_HANDLER_CLASSA_CLR_REGWEN_REG_OFFSET) == 1u);

  // 2. Lowering CLASSB_TIMEOUT_CYC_SHADOWED to 0 while in STATE_TIMEOUT (0x1)
  //    immediately transitions to STATE_PHASE0 (0x4) (ot_alert.c:399-404).
  //    Set EN=1, LOCK=0, EN_E2=1 (mapped to Phase2 -> 0x3911) and long
  //    PHASE0_CYC_SHADOWED so class_en is true while Phase0 asserts zero
  //    escalation signals.
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_ACCUM_THRESH_SHADOWED_REG_OFFSET, 10u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_PHASE0_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_PHASE1_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_PHASE2_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_PHASE3_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSB_CTRL_SHADOWED_REG_OFFSET,
                 0x3911u);

  uint32_t intr_en_orig =
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSB_BIT));
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_TEST_CLASSB_BIT);

  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_STATE_REG_OFFSET) ==
        0x1u);

  // While Class B is in STATE_TIMEOUT (0x1), lower CLASSB_TIMEOUT_CYC_SHADOWED
  // to 0 -> must immediately transition to STATE_PHASE0 (0x4).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSB_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_STATE_REG_OFFSET) ==
        0x4u);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSB_CLR_REGWEN_REG_OFFSET) == 1u);

  // Clearing INTR_STATE while in STATE_PHASE0 must NOT stop escalation, then
  // writing CLASSB_CLR_SHADOWED=1 returns CLASSB_STATE to STATE_IDLE (0x0).
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_STATE_REG_OFFSET) ==
        0x4u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSB_CLR_SHADOWED_REG_OFFSET, 1u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSB_CLR_SHADOWED_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_STATE_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSB_BIT));

  // 3. Accumulation Threshold Trigger (accu_trig) During STATE_TIMEOUT Cancels
  //    Timeout & Enters STATE_PHASE0 + CTRL.LOCK = 1 Auto-Clears
  //    CLASSA_CLR_REGWEN (ot_alert.c:795-798, 833-839).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 1u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET,
      0x100000u);
  // EN=1, LOCK=1, EN_E2=1 (mapped to Phase2 -> 0x3913): class_en=1, LOCK=1,
  // and Phase0 asserts zero escalation signals.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x3913u);

  uint32_t alert_en_addr = kAlertBase +
                           ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                           4u * kUart1AlertId;
  uint32_t alert_class_addr = kAlertBase +
                              ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                              4u * kUart1AlertId;
  uint32_t alert_cause_addr =
      kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4u * kUart1AlertId;

  write_shadowed(alert_class_addr, 0u);  // Class A
  write_shadowed(alert_en_addr, 1u);     // Enabled
  abs_mmio_write32(alert_cause_addr, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));

  // Trigger 1st alert on UART1 -> ACCUM_CNT == 1 (<= THRESH 1), enters
  // STATE_TIMEOUT (0x1), and CLASSA_CLR_REGWEN remains 1.
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(alert_cause_addr) == 1u);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) ==
        0x1u);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSA_CLR_REGWEN_REG_OFFSET) == 1u);

  // Clear ALERT_CAUSE[1] and trigger 2nd alert on UART1 while Class A is in
  // STATE_TIMEOUT (0x1) -> ACCUM_CNT == 2 (> THRESH 1), accu_trig cancels
  // timeout and enters STATE_PHASE0 (0x4), and LOCK=1 auto-clears
  // CLASSA_CLR_REGWEN to 0.
  abs_mmio_write32(alert_cause_addr, 1u);
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 2u);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) ==
        0x4u);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSA_CLR_REGWEN_REG_OFFSET) == 0u);

  // Attempt to clear Class A via CLASSA_CLR_SHADOWED while CLASSA_CLR_REGWEN=0
  // -> must be ignored, keeping ACCUM_CNT == 2 and STATE == 0x4.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kAlertBase +
                        ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 2u);
  CHECK(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) ==
        0x4u);

  return true;
}
