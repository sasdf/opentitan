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

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kSysrstCtrlBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
};

#define EXPECT_CHECK(cond, fmt, ...)              \
  do {                                            \
    if (!(cond)) {                                \
      LOG_ERROR("MISMATCH: " fmt, ##__VA_ARGS__); \
      failures++;                                 \
    }                                             \
  } while (0)

bool test_main(void) {
  uint32_t failures = 0;

  const uint32_t kMioPadIoa2 = kTopEarlgreyPinmuxMioOutIoa2;
  const uint32_t kMioOutselOff =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + kMioPadIoa2 * sizeof(uint32_t);
  const uint32_t kMioAttrOff =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + kMioPadIoa2 * sizeof(uint32_t);
  const uint32_t kInselGpio2Off =
      PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
      kTopEarlgreyPinmuxPeripheralInGpioGpio2 * sizeof(uint32_t);
  const uint32_t kInselGpio3Off =
      PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
      kTopEarlgreyPinmuxPeripheralInGpioGpio3 * sizeof(uint32_t);
  const uint32_t kInselAcPresentOff =
      PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
      kTopEarlgreyPinmuxPeripheralInSysrstCtrlAonAcPresent * sizeof(uint32_t);

  // Route IOA2 input to GPIO[2], GPIO[3], and SYSRST_CTRL.AC_PRESENT
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + kInselGpio2Off, kTopEarlgreyPinmuxInselIoa2);
  abs_mmio_write32(kPinmuxBase + kInselGpio3Off, kTopEarlgreyPinmuxInselIoa2);
  abs_mmio_write32(kPinmuxBase + kInselAcPresentOff,
                   kTopEarlgreyPinmuxInselIoa2);

  // ---------------------------------------------------------------------------
  // 1. Padring u_mio_pad (prim_pad_wrapper) input_disable (0x80) + invert
  // (0x01)
  //    With MIO_OUTSEL[Ioa2] = HighZ (2):
  //    - attr = 0x80 (input_disable=1, invert=0) -> in_raw_o = 0, in_o = 0
  //    - attr = 0x81 (input_disable=1, invert=1) -> in_raw_o = 0, in_o = 1
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + kMioOutselOff,
                   kTopEarlgreyPinmuxOutselConstantHighZ);
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0x80u);
  busy_spin_micros(10);
  uint32_t gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  uint32_t sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(
      ((gpio_in >> 2) & 1u) == 0u &&
          ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u) == 0u,
      "Padring IOA2 attr=0x80 (input_disable=1, invert=0): "
      "expected GPIO[2]=0, AC_PRESENT=0; got GPIO=0x%08x, SYSRST=0x%02x",
      gpio_in, sysrst_in);

  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0x81u);
  busy_spin_micros(10);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(((gpio_in >> 2) & 1u) == 1u,
               "Padring IOA2 attr=0x81 (input_disable=1, invert=1) -> "
               "GPIO.DATA_IN[2] expected 1, got 0 (GPIO=0x%08x)",
               gpio_in);
  EXPECT_CHECK(
      ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u) == 1u,
      "Padring IOA2 attr=0x81 (input_disable=1, invert=1) -> "
      "SYSRST_CTRL.AC_PRESENT expected 1, got 0 (SYSRST=0x%02x)",
      sysrst_in);

  // ---------------------------------------------------------------------------
  // 2. Padring u_mio_pad double-inversion cancellation (out_i ^ inv ^ inv ==
  // out_i)
  //    when MIO_OUTSEL[Ioa2] drives ConstantOne (1) and ConstantZero (0)
  //    with MIO_PAD_ATTR[Ioa2] = 0x01 (invert=1, input_disable=0)
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0x01u);
  abs_mmio_write32(kPinmuxBase + kMioOutselOff,
                   kTopEarlgreyPinmuxOutselConstantOne);
  busy_spin_micros(10);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  EXPECT_CHECK(((gpio_in >> 2) & 1u) == 1u,
               "Padring IOA2 ConstantOne with invert=1: expected "
               "GPIO.DATA_IN[2]=1 (double-inversion cancellation), got 0");

  abs_mmio_write32(kPinmuxBase + kMioOutselOff,
                   kTopEarlgreyPinmuxOutselConstantZero);
  busy_spin_micros(10);
  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  EXPECT_CHECK(((gpio_in >> 2) & 1u) == 0u,
               "Padring IOA2 ConstantZero with invert=1: expected "
               "GPIO.DATA_IN[2]=0 (double-inversion cancellation), got 1");

  // ---------------------------------------------------------------------------
  // 3. Padring u_mio_pad cross-peripheral / multi-input pad sharing:
  //    GPIO[2] driving IOA2 must be observed on GPIO.DATA_IN[3] and
  //    SYSRST_CTRL.PIN_IN_VALUE.AC_PRESENT
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0x00u);
  abs_mmio_write32(kPinmuxBase + kMioOutselOff,
                   kTopEarlgreyPinmuxOutselGpioGpio2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);

  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(
      ((gpio_in >> 3) & 1u) == 1u,
      "Padring IOA2 driven high by GPIO[2]: expected GPIO.DATA_IN[3]=1 "
      "(shared pad loopback), got 0 (GPIO=0x%08x)",
      gpio_in);
  EXPECT_CHECK(
      ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u) == 1u,
      "Padring IOA2 driven high by GPIO[2]: expected "
      "SYSRST_CTRL.AC_PRESENT=1, got 0 (SYSRST=0x%02x)",
      sysrst_in);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 4. Padring u_dio_pad DIO_PAD_ATTR (input_disable=0x80, invert=0x01)
  //    on DioSysrstCtrlAonEcRstL and DioSysrstCtrlAonFlashWpL
  // ---------------------------------------------------------------------------
  const uint32_t kDioEcRstAttrOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSysrstCtrlAonEcRstL * sizeof(uint32_t);
  const uint32_t kDioFlashWpAttrOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSysrstCtrlAonFlashWpL * sizeof(uint32_t);

  abs_mmio_write32(kPinmuxBase + kDioEcRstAttrOff, 0x80u);
  abs_mmio_write32(kPinmuxBase + kDioFlashWpAttrOff, 0x80u);
  busy_spin_micros(10);
  sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(
      ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 0u &&
          ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 0u,
      "Padring DIO_PAD_ATTR[EcRstL/FlashWpL]=0x80 (input_disable=1): "
      "expected EC_RST_L=0, FLASH_WP_L=0; got SYSRST=0x%02x",
      sysrst_in);

  abs_mmio_write32(kPinmuxBase + kDioEcRstAttrOff, 0x81u);
  busy_spin_micros(10);
  sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(
      ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 1u,
      "Padring DIO_PAD_ATTR[EcRstL]=0x81 (input_disable=1, invert=1): "
      "expected EC_RST_L=1; got SYSRST=0x%02x",
      sysrst_in);

  abs_mmio_write32(kPinmuxBase + kDioEcRstAttrOff, 0x00u);
  abs_mmio_write32(kPinmuxBase + kDioFlashWpAttrOff, 0x00u);

  // ---------------------------------------------------------------------------
  // 5. Padring u_mio_pad[Ioc10] (MIO pad 32, outside host_pin 0..31 mapping):
  //    GPIO[2] driving IOC10 must loop back through u_mio_pad[32] to
  //    GPIO.DATA_IN[3] and SYSRST_CTRL.PIN_IN_VALUE.AC_PRESENT
  // ---------------------------------------------------------------------------
  const uint32_t kMioPadIoc10 = kTopEarlgreyPinmuxMioOutIoc10;
  const uint32_t kIoc10OutselOff =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + kMioPadIoc10 * sizeof(uint32_t);
  const uint32_t kIoc10AttrOff =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + kMioPadIoc10 * sizeof(uint32_t);

  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, 0x00u);
  abs_mmio_write32(kPinmuxBase + kIoc10OutselOff,
                   kTopEarlgreyPinmuxOutselGpioGpio2);
  abs_mmio_write32(kPinmuxBase + kInselGpio3Off, kTopEarlgreyPinmuxInselIoc10);
  abs_mmio_write32(kPinmuxBase + kInselAcPresentOff,
                   kTopEarlgreyPinmuxInselIoc10);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);

  gpio_in = abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET);
  sysrst_in =
      abs_mmio_read32(kSysrstCtrlBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  EXPECT_CHECK(((gpio_in >> 3) & 1u) == 1u,
               "Padring IOC10 (pad 32) driven high by GPIO[2]: expected "
               "GPIO.DATA_IN[3]=1, got 0 (GPIO=0x%08x)",
               gpio_in);
  EXPECT_CHECK(
      ((sysrst_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u) == 1u,
      "Padring IOC10 (pad 32) driven high by GPIO[2]: expected "
      "SYSRST_CTRL.AC_PRESENT=1, got 0 (SYSRST=0x%02x)",
      sysrst_in);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + kIoc10OutselOff,
                   kTopEarlgreyPinmuxOutselConstantHighZ);

  LOG_INFO("padring_rtl_consistency_test completed with %u failures", failures);
  return failures == 0;
}
