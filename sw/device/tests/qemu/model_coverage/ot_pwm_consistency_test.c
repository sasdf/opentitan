// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"
#include "pwm_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPwmBase = TOP_EARLGREY_PWM_AON_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
};

static inline uint32_t sample_pwm0(void) {
  return abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u;
}

static void observe_transitions(uint32_t *seen_high, uint32_t *seen_low,
                                uint32_t *edges) {
  *seen_high = 0;
  *seen_low = 0;
  *edges = 0;
  uint32_t prev = sample_pwm0();
  if (prev) {
    (*seen_high)++;
  } else {
    (*seen_low)++;
  }
  for (uint32_t i = 0; i < 120u; ++i) {
    busy_spin_micros(2);
    uint32_t cur = sample_pwm0();
    if (cur) {
      (*seen_high)++;
    } else {
      (*seen_low)++;
    }
    if (cur != prev) {
      (*edges)++;
      prev = cur;
    }
    if (*seen_high > 0 && *seen_low > 0 && *edges >= 2u) {
      break;
    }
  }
}

bool test_main(void) {
  // Route PWM0 output to MIO pad IOA0 and IOA0 input to GPIO0.
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET,
                   kTopEarlgreyPinmuxOutselPwmAonPwm0);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET,
                   kTopEarlgreyPinmuxInselIoa0);

  // 1. Verify write-only ALERT_TEST register reads back as 0.
  CHECK(abs_mmio_read32(kPwmBase + PWM_ALERT_TEST_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kPwmBase + PWM_ALERT_TEST_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_ALERT_TEST_REG_OFFSET) == 0x0u);

  // 2. Standard Blink Mode (blink_en = 1, htbt_en = 0):
  // duty_a = 0xffff (100% high), duty_b = 0x0000 (0% low), blink_x = 1,
  // blink_y = 1, dc_resn = 0, clk_div = 0.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x0000ffffu);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x00010001u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 1u << 31);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 1u << 31);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x1u);

  uint32_t seen_high = 0, seen_low = 0, edges = 0;
  observe_transitions(&seen_high, &seen_low, &edges);
  CHECK(seen_high > 0u);
  CHECK(seen_low > 0u);
  CHECK(edges >= 2u);

  // 3. Positive Heartbeat Mode (blink_en = 1, htbt_en = 1, duty_a < duty_b):
  // Latch duty_a = 0x0000 while htbt_en = 0 across >= 2 AON clock ticks.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x80000000u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x7fff0000u);
  busy_spin_micros(20);

  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   (1u << 31) | (1u << 30));
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 1u << 31);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x1u);

  observe_transitions(&seen_high, &seen_low, &edges);
  CHECK(seen_high > 0u);
  CHECK(seen_low > 0u);
  CHECK(edges >= 2u);

  // 4. Negative Heartbeat Mode (blink_en = 1, htbt_en = 1, duty_a > duty_b):
  // Latch duty_a = 0xffff while htbt_en = 0 across >= 2 AON clock ticks.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x7fffffffu);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x7fff0000u);
  busy_spin_micros(20);

  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   (1u << 31) | (1u << 30));
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 1u << 31);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x1u);

  observe_transitions(&seen_high, &seen_low, &edges);
  CHECK(seen_high > 0u);
  CHECK(seen_low > 0u);
  CHECK(edges >= 2u);

  // 5. Verify REGWEN RW0C lock prevents further register modifications.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kPwmBase + PWM_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kPwmBase + PWM_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x3fu);
  CHECK(abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0x0u);

  LOG_INFO("ot_pwm_consistency_test passed");
  return true;
}
