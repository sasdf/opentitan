// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "pwm_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPwmBase = TOP_EARLGREY_PWM_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

static volatile uint32_t kFaultCount = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mepc;
  __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
  uint16_t insn16 = *(const uint16_t *)mepc;
  uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
  __asm__ volatile("csrw mepc, %0" : : "r"(mepc + step));
  kFaultCount++;
}

bool test_main(void) {
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(
      &pinmux, kTopEarlgreyPinmuxMioOutIoa2, kTopEarlgreyPinmuxOutselPwmAonPwm0));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInGpioGpio0,
      kTopEarlgreyPinmuxInselIoa2));

  // 1. Cover Chunk 1 (`blink_en && !htbt_en` in `ot_pwm_get_channel_level`):
  // (a) `blink_ctr <= blink_x` -> selects `duty_a` (`0x0000` -> masked 0 -> GPIO 0).
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0xffff0000u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x03e803e8u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   1u << PWM_PWM_PARAM_0_BLINK_EN_0_BIT);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 1u << PWM_CFG_CNTR_EN_BIT);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 1u);
  busy_spin_micros(20);
  uint32_t gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  CHECK(gpio_in == 0u, "Blink duty_a=0 expected GPIO0=0, got %u", gpio_in);

  // (b) `blink_ctr > blink_x` -> with `blink_x = 0, blink_y = 0x03e8`, after 1+
  // cycles (`60 us`), `blink_ctr > 0` selects `duty_b` (`0x0000` with INVERT=1 -> GPIO 1).
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 1u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x0000ffffu);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x03e80000u);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 1u);
  busy_spin_micros(60);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  CHECK(gpio_in == 1u, "Blink duty_b=0 with INVERT=1 expected GPIO0=1, got %u",
        gpio_in);

  // 2. Cover Chunk 2 (`blink_en && htbt_en && duty_a != duty_b`):
  // (a) `pos_htbt == true` (`duty_a = 0x1000 < duty_b = 0x2000`, `blink_x = 0`,
  // `blink_y = 0x0fff` -> step = `0x1000` per cycle). Over 60 us (>4 cycles),
  // `dc_htbt` steps `0x1000 -> 0x2000 -> 0x1000 -> 0x2000`, exercising both
  // `dc_htbt >= duty_b` (`htbt_dir = true`) and `dc_htbt == duty_a` (`htbt_dir = false`).
  // With `DC_RESN = 0` (`dc_mask = 0x7fff`), `dc_htbt & ~dc_mask == 0`, so output is 0.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x20001000u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x0fff0000u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   (1u << PWM_PWM_PARAM_0_BLINK_EN_0_BIT) |
                       (1u << PWM_PWM_PARAM_0_HTBT_EN_0_BIT));
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 1u);
  busy_spin_micros(60);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  CHECK(gpio_in == 0u, "Positive heartbeat masked output expected 0, got %u",
        gpio_in);

  // (b) `pos_htbt == false` (`duty_a = 0x2000 > duty_b = 0x1000`, `blink_x = 0`,
  // `blink_y = 0x0fff` -> step = `0x1000` per cycle). Over 60 us (>4 cycles),
  // `dc_htbt` steps `0x2000 -> 0x1000 -> 0x2000 -> 0x1000`, exercising both
  // `dc_htbt <= duty_b` (`htbt_dir = false`) and `dc_htbt == duty_a` (`htbt_dir = true`).
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 1u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x10002000u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x0fff0000u);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 1u);
  busy_spin_micros(60);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  CHECK(gpio_in == 1u,
        "Negative heartbeat masked output with INVERT=1 expected 1, got %u",
        gpio_in);

  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);

  // 3. Cover Chunks 3 & 4 (`reg >= REGS_COUNT` decode error in `ot_pwm_read/write_with_attrs`):
  // Offset `0x5c` (`R_BLINK_PARAM_5 + 4`) is within the 128-byte (`0x80`) `pwm_reg_top.sv`
  // (`BlockAw = 7`) TL-UL window (`addr_miss = 1` -> `d_error = 1`).
  kFaultCount = 0;
  uint32_t bad_read = abs_mmio_read32(kPwmBase + 0x5cu);
  (void)bad_read;
  CHECK(kFaultCount == 1u,
        "Read at unmapped PWM offset 0x5c must raise access fault (got %u)",
        kFaultCount);

  kFaultCount = 0;
  abs_mmio_write32(kPwmBase + 0x5cu, 0xdeadbeefu);
  CHECK(kFaultCount == 1u,
        "Write at unmapped PWM offset 0x5c must raise access fault (got %u)",
        kFaultCount);

  return true;
}
