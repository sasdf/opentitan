// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kSysrstCtrlBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
};

bool test_main(void) {
  // 1. W/O INTR_TEST (0x08) and ALERT_TEST (0x0c) read back as 0.
  CHECK(abs_mmio_read32(kGpioBase + GPIO_INTR_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET) == 0u);

  // 2. Masked & Direct OE read/write consistency (L702-705, L712-717,
  // L813-821).
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_MASKED_OE_LOWER_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_MASKED_OE_UPPER_REG_OFFSET) == 0u);

  const uint32_t kLowerMask = 0x55aau;
  const uint32_t kLowerVal = 0x1234u;
  const uint32_t kExpectedLower = kLowerVal & kLowerMask;
  abs_mmio_write32(kGpioBase + GPIO_MASKED_OE_LOWER_REG_OFFSET,
                   (kLowerMask << 16) | kLowerVal);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_MASKED_OE_LOWER_REG_OFFSET) ==
        kExpectedLower);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET) ==
        kExpectedLower);

  const uint32_t kUpperMask = 0xa55au;
  const uint32_t kUpperVal = 0xabcdu;
  const uint32_t kExpectedUpper = kUpperVal & kUpperMask;
  abs_mmio_write32(kGpioBase + GPIO_MASKED_OE_UPPER_REG_OFFSET,
                   (kUpperMask << 16) | kUpperVal);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_MASKED_OE_UPPER_REG_OFFSET) ==
        kExpectedUpper);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET) ==
        ((kExpectedUpper << 16) | kExpectedLower));

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);

  // 3. Route GPIO[0] to ConstantOne (1) and GPIO[1..31] to ConstantZero (0),
  // verify DATA_IN == 0x1, and verify R/O DATA_IN ignores software writes.
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET,
                   kTopEarlgreyPinmuxInselConstantOne);
  for (uint32_t g = 1; g < 32u; ++g) {
    abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
                         g * sizeof(uint32_t),
                     kTopEarlgreyPinmuxInselConstantZero);
  }
  // Trigger data_in refresh via DIRECT_OUT write.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) == 0x1u);

  // Writing 0xffffffff to R/O DATA_IN must be ignored.
  abs_mmio_write32(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) == 0x1u);

  // 4. MIO pad loopback from sysrst_ctrl (outsel 35..42), pattgen (outsel
  // 49..52), and pwm (outsel 65..70).
  const uint32_t kPadSysrst = kTopEarlgreyPinmuxMioOutIor10;
  const uint32_t kPadPattgen = kTopEarlgreyPinmuxMioOutIor11;
  const uint32_t kPadPwm = kTopEarlgreyPinmuxMioOutIor12;

  // Configure sysrst_ctrl bat_disable (bit 0) override to drive 1 on
  // kPadSysrst.
  abs_mmio_write32(kSysrstCtrlBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET,
                   0xffffu);
  abs_mmio_write32(kSysrstCtrlBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kSysrstCtrlBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET,
                   0x1u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET +
                       kPadSysrst * sizeof(uint32_t),
                   kTopEarlgreyPinmuxOutselSysrstCtrlAonBatDisable);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
                       1u * sizeof(uint32_t),
                   kTopEarlgreyPinmuxInselIor10);

  // Configure kPadPattgen driven by pattgen and kPadPwm driven by pwm, and
  // route GPIO[2..3] plus sysrst_ctrl inputs ac_present and key0_in to those
  // pads so both ot_gpio_eg_update_data_in and ot_gpio_eg_get_mio_pad_in query
  // pattgen and pwm.
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET +
                       kPadPattgen * sizeof(uint32_t),
                   kTopEarlgreyPinmuxOutselPattgenPda0Tx);
  abs_mmio_write32(
      kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + kPadPwm * sizeof(uint32_t),
      kTopEarlgreyPinmuxOutselPwmAonPwm0);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
                       2u * sizeof(uint32_t),
                   kTopEarlgreyPinmuxInselIor11);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
                       3u * sizeof(uint32_t),
                   kTopEarlgreyPinmuxInselIor12);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
                       kTopEarlgreyPinmuxPeripheralInSysrstCtrlAonAcPresent *
                           sizeof(uint32_t),
                   kTopEarlgreyPinmuxInselIor11);
  abs_mmio_write32(
      kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
          kTopEarlgreyPinmuxPeripheralInSysrstCtrlAonKey0In * sizeof(uint32_t),
      kTopEarlgreyPinmuxInselIor12);
  abs_mmio_write32(
      kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
          kTopEarlgreyPinmuxPeripheralInSysrstCtrlAonKey1In * sizeof(uint32_t),
      kTopEarlgreyPinmuxInselIor10);

  // Trigger GPIO update_data_out + update_data_in after clk_aon_i CDC sync.
  busy_spin_micros(100);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) == 0x3u);
  CHECK(
      (abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
       (1u << 2)) == (1u << 2));

  // Drive sysrst_ctrl bat_disable = 0 -> GPIO[1] and sysrst_ctrl key1_in read
  // 0.
  abs_mmio_write32(kSysrstCtrlBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET,
                   0x0u);
  busy_spin_micros(100);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) == 0x1u);
  CHECK(
      (abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
       (1u << 2)) == 0u);

  return true;
}
