// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_timer_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_RV_TIMER_BASE_ADDR,
};

bool test_main(void) {
  irq_timer_ctrl(false);

  // Save initial RV_TIMER state.
  uint32_t saved_ctrl = abs_mmio_read32(kBase + RV_TIMER_CTRL_REG_OFFSET);
  uint32_t saved_cfg0 = abs_mmio_read32(kBase + RV_TIMER_CFG0_REG_OFFSET);
  uint32_t saved_ie0 =
      abs_mmio_read32(kBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET);
  uint32_t saved_cmp_hi =
      abs_mmio_read32(kBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET);
  uint32_t saved_cmp_lo =
      abs_mmio_read32(kBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET);

  // 1. Stop timer and configure initial 64-bit value and compare far in future.
  abs_mmio_write32(kBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0u);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u);

  abs_mmio_write32(kBase + RV_TIMER_CFG0_REG_OFFSET,
                   (1u << RV_TIMER_CFG0_STEP_OFFSET) |
                       (0u << RV_TIMER_CFG0_PRESCALE_OFFSET));
  abs_mmio_write32(kBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0x00000100u);
  CHECK(abs_mmio_read32(kBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) ==
        0x00000100u);

  // 2. Start timer (`CTRL.ACTIVE0 = 1`) and write `TIMER_V_LOWER0` in-flight.
  abs_mmio_write32(kBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  abs_mmio_write32(kBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0x00002000u);

  uint32_t upper_after_lo_write =
      abs_mmio_read32(kBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET);
  uint32_t lower_after_lo_write =
      abs_mmio_read32(kBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(upper_after_lo_write == 0x12345678u);
  CHECK(lower_after_lo_write >= 0x00002000u &&
        lower_after_lo_write < 0x00100000u);

  // 3. Write `TIMER_V_UPPER0` and `CFG0` in-flight while `CTRL.ACTIVE0 == 1`.
  abs_mmio_write32(kBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0x87654321u);
  abs_mmio_write32(kBase + RV_TIMER_CFG0_REG_OFFSET,
                   (2u << RV_TIMER_CFG0_STEP_OFFSET) |
                       (0u << RV_TIMER_CFG0_PRESCALE_OFFSET));
  CHECK(abs_mmio_read32(kBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET) ==
        0x87654321u);
  uint32_t lower_after_hi_write =
      abs_mmio_read32(kBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(lower_after_hi_write >= lower_after_lo_write);

  // 4. Verify compare match sets `INTR_STATE0` and updating `COMPARE_UPPER0_0`
  // clears `INTR_STATE0`.
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0x87654321u);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 0x00000000u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  abs_mmio_write32(kBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u);

  // 5. Restore RV_TIMER state.
  abs_mmio_write32(kBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kBase + RV_TIMER_CFG0_REG_OFFSET, saved_cfg0);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, saved_cmp_hi);
  abs_mmio_write32(kBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, saved_cmp_lo);
  abs_mmio_write32(kBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, saved_ie0);
  abs_mmio_write32(kBase + RV_TIMER_CTRL_REG_OFFSET, saved_ctrl);

  return true;
}
