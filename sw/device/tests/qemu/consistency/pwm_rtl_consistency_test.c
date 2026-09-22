// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwm_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPwmBase = TOP_EARLGREY_PWM_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

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
  bool all_ok = true;

  // 1. Verify power-on reset values of PWM registers.
  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 1u,
             "REGWEN reset value mismatch");
  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_CFG_REG_OFFSET) == 0x38008000u,
             "CFG reset value mismatch (got 0x%08x)",
             abs_mmio_read32(kPwmBase + PWM_CFG_REG_OFFSET));
  EXPECT_RTL(
      abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) == 0x7fff7fffu,
      "DUTY_CYCLE_0 reset value mismatch");

  // 2. Verify register bitmasks on PWM_EN, INVERT, and PWM_PARAM_0..5.
  // In pwm.hjson / pwm_reg_top.sv, PWM_PARAM[i] only populates bits [31:30]
  // (BLINK_EN, HTBT_EN) and [15:0] (PHASE_DELAY); bits [29:16] are reserved 0
  // (mask 0xc000ffff).
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0xffffffffu);
  busy_spin_micros(20);
  uint32_t pwm_en = abs_mmio_read32(kPwmBase + PWM_PWM_EN_REG_OFFSET);
  EXPECT_RTL(pwm_en == 0x3fu, "PWM_EN mask mismatch: got 0x%08x, expected 0x3f",
             pwm_en);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);

  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0xffffffffu);
  busy_spin_micros(20);
  uint32_t invert = abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET);
  EXPECT_RTL(invert == 0x3fu, "INVERT mask mismatch: got 0x%08x, expected 0x3f",
             invert);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);

  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t offset = PWM_PWM_PARAM_0_REG_OFFSET + i * sizeof(uint32_t);
    abs_mmio_write32(kPwmBase + offset, 0xffffffffu);
    busy_spin_micros(20);
    uint32_t param = abs_mmio_read32(kPwmBase + offset);
    EXPECT_RTL(param == 0xc000ffffu,
               "PWM_PARAM_%u write 0xffffffff read back 0x%08x, expected "
               "0xc000ffff",
               i, param);
    abs_mmio_write32(kPwmBase + offset, 0u);
  }
  busy_spin_micros(20);

  // 3. Verify PWM_AON output loopback via PINMUX (Ioa2) -> GPIO0 and verify
  // pwm_chan.sv dc_mask rounding:
  // When dc_resn = 0, dc_mask = 0x7fff (~dc_mask = 0x8000). With duty_cycle_a =
  // 0x7fff, duty_cycle_masked = 0x7fff & 0x8000 = 0, so pwm_o must remain 0
  // even when CNTR_EN=1 and PWM_EN=1!
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa2,
                                        kTopEarlgreyPinmuxOutselPwmAonPwm0));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio0,
                                       kTopEarlgreyPinmuxInselIoa2));

  // With PWM_EN=0 and INVERT=1, cio_pwm_o[0] must be 1.
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 1u);
  busy_spin_micros(30);
  uint32_t gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  EXPECT_RTL(gpio_in == 1u,
             "PWM0 output with PWM_EN=0, INVERT=1 should read 1 via PINMUX "
             "loopback, got %u",
             gpio_in);

  // With PWM_EN=1, INVERT=0, DC_RESN=0, CLK_DIV=0, DUTY_CYCLE_0=0x7fff7fff,
  // duty_cycle_masked is (0x7fff & ~0x7fff) == 0, so cio_pwm_o[0] must stay 0.
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x7fff7fffu);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 1u << PWM_CFG_CNTR_EN_BIT);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 1u);
  busy_spin_micros(30);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;
  EXPECT_RTL(gpio_in == 0u,
             "PWM0 output with DC_RESN=0 and DUTY_CYCLE.A=0x7fff must round "
             "down to 0 via dc_mask (got %u)",
             gpio_in);

  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x38008000u);
  busy_spin_micros(20);

  // 4. Verify ALERT_TEST is a 1-cycle pulse (`alert_test.q & alert_test.qe` in
  // pwm.sv:45-48) rather than latching high forever.
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdPwmAonFatalFault,
      kDifAlertHandlerClassD, kDifToggleEnabled, kDifToggleDisabled));

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPwmAonFatalFault));
  abs_mmio_write32(kPwmBase + PWM_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(10);
  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPwmAonFatalFault, &is_cause));
  EXPECT_RTL(is_cause, "First write to PWM ALERT_TEST did not set alert cause");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPwmAonFatalFault));
  busy_spin_micros(10);
  is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPwmAonFatalFault, &is_cause));
  EXPECT_RTL(!is_cause,
             "ALERT_CAUSE remained set after acknowledge (PWM ALERT_TEST "
             "latched high instead of pulsing)");
  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_ALERT_TEST_REG_OFFSET) == 0u,
             "ALERT_TEST (WO) readback must be 0");

  // 4b. Verify PWM_PERMIT sub-word write checks (pwm_reg_top.sv wr_err):
  // - Sub-word write to DUTY_CYCLE_0 (PWM_PERMIT = 4'b1111) faults and blocks
  // write.
  // - Sub-word write to INVERT+1 (PWM_PERMIT = 4'b0001, reg_be = 4'b0010)
  // faults.
  // - Sub-word write to INVERT+0 (PWM_PERMIT = 4'b0001, reg_be = 4'b0001)
  // succeeds.
  kFaultCount = 0;
  *((volatile uint8_t *)(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET)) = 0x12u;
  EXPECT_RTL(
      kFaultCount == 1u &&
          abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) ==
              0x7fff7fffu,
      "Sub-word write to DUTY_CYCLE_0 must fault and not modify register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kPwmBase + PWM_INVERT_REG_OFFSET + 1u)) = 0x01u;
  EXPECT_RTL(kFaultCount == 1u &&
                 abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0u,
             "Sub-word write to INVERT+1 must fault and not modify register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kPwmBase + PWM_INVERT_REG_OFFSET)) = 0x15u;
  busy_spin_micros(20);
  EXPECT_RTL(
      kFaultCount == 0u &&
          abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0x15u,
      "Byte-0 sub-word write to INVERT (PWM_PERMIT=4'b0001) must succeed");
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0u);
  busy_spin_micros(20);

  // 4c. Verify unmapped offset 0x5c (reg >= REGS_COUNT within BlockAw=7 0x80
  // aperture, pwm_reg_top.sv addrmiss) raises Load/Store Access Faults.
  kFaultCount = 0;
  (void)abs_mmio_read32(kPwmBase + 0x5cu);
  EXPECT_RTL(kFaultCount == 1u,
             "Reading unmapped PWM offset 0x5c must raise Load Access Fault");
  kFaultCount = 0;
  abs_mmio_write32(kPwmBase + 0x5cu, 0xdeadbeefu);
  EXPECT_RTL(kFaultCount == 1u,
             "Writing unmapped PWM offset 0x5c must raise Store Access Fault");

  // 5. Verify REGWEN (rw0c) locks CFG, PWM_EN, INVERT, PWM_PARAM_0,
  // DUTY_CYCLE_0, and BLINK_PARAM_0 while leaving ALERT_TEST functional.
  abs_mmio_write32(kPwmBase + PWM_REGWEN_REG_OFFSET, 0u);
  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 0u,
             "REGWEN did not clear on write 0");
  abs_mmio_write32(kPwmBase + PWM_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_REGWEN_REG_OFFSET) == 0u,
             "REGWEN (rw0c) must not set back to 1 on write 1");

  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0xc000ffffu);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x12345678u);
  busy_spin_micros(20);

  EXPECT_RTL(abs_mmio_read32(kPwmBase + PWM_CFG_REG_OFFSET) == 0x38008000u &&
                 abs_mmio_read32(kPwmBase + PWM_PWM_EN_REG_OFFSET) == 0u &&
                 abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0u &&
                 abs_mmio_read32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET) == 0u &&
                 abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) ==
                     0x7fff7fffu &&
                 abs_mmio_read32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET) == 0u,
             "PWM configuration registers modified while REGWEN == 0");

  return all_ok;
}
