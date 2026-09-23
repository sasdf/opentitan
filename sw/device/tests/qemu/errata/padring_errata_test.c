// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `padring` (`P36`).
 *
 * Empirically verifies all documented hardware quirks and implementation
 * details in `/root/knowledge/errata/padring.md` across both the physical CW340
 * FPGA (golden Earlgrey RTL) and QEMU (`ot_pinmux_eg` / `ot_gpio_eg` /
 * `ot_sysrst_ctrl`):
 *
 * - PADRING-E2-01 (Universal 47-MIO & 16-DIO `prim_pad_wrapper` Bidirectional
 *   IOBUF Loopback, Double-Inversion Cancellation, and `input_disable`
 *   Pre-Invert Clamping):
 *   1. In `hw/top_earlgrey/rtl/padring.sv:145-165, 252-272` and
 *      `hw/ip/prim_xilinx_ultrascale/rtl/prim_xilinx_ultrascale_pad_wrapper.sv:49,
 * 76-97`, bidirectional pads apply `attr_i.invert` on BOTH the output path
 *      (`out = out_i ^ attr_i.invert`) and the input path
 *      (`in_o = attr_i.invert ^ in_raw_o`). When a pad's output driver is
 *      enabled (`oe_i = 1`, e.g. `MIO_OUTSEL` set to `ConstantZero` (`0`) or
 *      `ConstantOne` (`1`)) and `input_disable == 0`, the IOBUF loops `out`
 *      back into `in_raw_o`, so the two XORs cancel out (`in_o = out_i`).
 *      Verified across both GPIO-mapped MIO pads (`IOA2` -> `GPIO.DATA_IN[0]`)
 *      and non-GPIO MIO pads (`IOC10` -> `SYSRST_CTRL.PIN_IN_VALUE.PWRB_IN`).
 *   2. When `attr_i.input_disable == 1` (`0x2`), `ie = ie_i & ~input_disable`
 *      clamps `in_raw_o = 1'b0` BEFORE the input XOR (`in_o = attr_i.invert`),
 *      regardless of whether `oe_i == 1`. Verified on `IOA2`, `IOC10`, and
 *      dedicated bidirectional DIO pads `DIO[10]` (`ec_rst_l`) and `DIO[11]`
 *      (`flash_wp_l`) via `SYSRST_CTRL.PIN_IN_VALUE`.
 * - [padring.sv:145-165,252-272] (BENIGN_RTL_IMPL_DETAIL):
 *   `prim_xilinx_ultrascale_pad_wrapper.sv:87-97` WARL attribute masks retain
 *   `0x83` (`invert`, `input_disable`, `virtual_od_en`) on `BidirStd` /
 *   `BidirOd` pads (`MIO[0..46]`, `DIO[10..11]`) and `0x81` (`invert`,
 *   `virtual_od_en`) on `InputStd` pads (`DIO[12]` `spi_device_sck`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
};

static void verify_padring_e2_01_mio_gpio_and_sysrst_loopback(void) {
  LOG_INFO(
      "Verifying PADRING-E2-01: prim_pad_wrapper double-inversion cancellation "
      "and input_disable pre-invert clamping on IOA2 (GPIO[0]) and IOC10 "
      "(SYSRST_CTRL.PIN_IN_VALUE.PWRB_IN)...");

  /* 1. Test on IOA2 (MIO[2]) routed to GPIO[0]. */
  const uint32_t kIoa2 = kTopEarlgreyPinmuxMioOutIoa2;
  const uint32_t kGpio0InselOff =
      PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
      kTopEarlgreyPinmuxPeripheralInGpioGpio0 * sizeof(uint32_t);
  const uint32_t kIoa2OutselOff =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + kIoa2 * sizeof(uint32_t);
  const uint32_t kIoa2AttrOff =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + kIoa2 * sizeof(uint32_t);

  uint32_t orig_gpio0_insel = abs_mmio_read32(kPinmuxBase + kGpio0InselOff);
  uint32_t orig_ioa2_outsel = abs_mmio_read32(kPinmuxBase + kIoa2OutselOff);
  uint32_t orig_ioa2_attr = abs_mmio_read32(kPinmuxBase + kIoa2AttrOff);

  abs_mmio_write32(kPinmuxBase + kGpio0InselOff, kTopEarlgreyPinmuxInselIoa2);

  /* Drive ConstantOne (1) on IOA2. */
  abs_mmio_write32(kPinmuxBase + kIoa2OutselOff,
                   kTopEarlgreyPinmuxOutselConstantOne);
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, 0x0u);
  busy_spin_micros(20);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u) == 1u);

  /* Set invert=1, input_disable=0 (0x1): double-inversion cancels out
   * (out = 1^1 = 0, in_raw = 0, in_o = 0^1 = 1), so GPIO.DATA_IN[0] stays 1! */
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, 0x1u);
  busy_spin_micros(20);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u) == 1u);

  /* Set invert=0, input_disable=1 (0x80): ie=0 clamps in_raw_o=0 before XOR
   * (in_o = 0^0 = 0), overriding driven ConstantOne! */
  const uint32_t kAttrInv = 1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT;
  const uint32_t kAttrInDis = 1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT;

  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, kAttrInDis);
  busy_spin_micros(20);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u) == 0u);

  /* Set invert=1, input_disable=1 (0x81): ie=0 clamps in_raw_o=0 before XOR
   * (in_o = 0^1 = 1), so GPIO.DATA_IN[0] becomes 1! */
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, kAttrInDis | kAttrInv);
  busy_spin_micros(20);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u) == 1u);

  /* Drive ConstantZero (0) on IOA2 with invert=1, input_disable=0 (0x1):
   * double-inversion cancels out (out = 0^1 = 1, in_raw = 1, in_o = 1^1 = 0).
   */
  abs_mmio_write32(kPinmuxBase + kIoa2OutselOff,
                   kTopEarlgreyPinmuxOutselConstantZero);
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, kAttrInv);
  busy_spin_micros(20);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u) == 0u);

  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, orig_ioa2_attr);
  abs_mmio_write32(kPinmuxBase + kIoa2OutselOff, orig_ioa2_outsel);
  abs_mmio_write32(kPinmuxBase + kGpio0InselOff, orig_gpio0_insel);

  /* 2. Test on non-GPIO MIO pad IOC10 (MIO[32]) routed to
   *    SYSRST_CTRL.PIN_IN_VALUE.PWRB_IN (bit 0). */
  const uint32_t kIoc10 = kTopEarlgreyPinmuxMioOutIoc10;
  const uint32_t kPwrbInselOff =
      PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET +
      kTopEarlgreyPinmuxPeripheralInSysrstCtrlAonPwrbIn * sizeof(uint32_t);
  const uint32_t kIoc10OutselOff =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + kIoc10 * sizeof(uint32_t);
  const uint32_t kIoc10AttrOff =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + kIoc10 * sizeof(uint32_t);

  uint32_t orig_pwrb_insel = abs_mmio_read32(kPinmuxBase + kPwrbInselOff);
  uint32_t orig_ioc10_outsel = abs_mmio_read32(kPinmuxBase + kIoc10OutselOff);
  uint32_t orig_ioc10_attr = abs_mmio_read32(kPinmuxBase + kIoc10AttrOff);

  abs_mmio_write32(kPinmuxBase + kIoc10OutselOff,
                   kTopEarlgreyPinmuxOutselConstantOne);
  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, 0x0u);
  abs_mmio_write32(kPinmuxBase + kPwrbInselOff, kTopEarlgreyPinmuxInselIoc10);
  busy_spin_micros(25);
  CHECK((abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
         (1u << SYSRST_CTRL_PIN_IN_VALUE_PWRB_IN_BIT)) != 0u);

  /* Double-inversion cancellation on IOC10 with invert=1, input_disable=0. */
  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, kAttrInv);
  abs_mmio_write32(kPinmuxBase + kPwrbInselOff, kTopEarlgreyPinmuxInselIoc10);
  busy_spin_micros(25);
  CHECK((abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
         (1u << SYSRST_CTRL_PIN_IN_VALUE_PWRB_IN_BIT)) != 0u);

  /* input_disable=1, invert=0 (0x80) forces PWRB_IN = 0. */
  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, kAttrInDis);
  abs_mmio_write32(kPinmuxBase + kPwrbInselOff, kTopEarlgreyPinmuxInselIoc10);
  busy_spin_micros(25);
  CHECK((abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
         (1u << SYSRST_CTRL_PIN_IN_VALUE_PWRB_IN_BIT)) == 0u);

  /* input_disable=1, invert=1 (0x81) forces PWRB_IN = 1. */
  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, kAttrInDis | kAttrInv);
  abs_mmio_write32(kPinmuxBase + kPwrbInselOff, kTopEarlgreyPinmuxInselIoc10);
  busy_spin_micros(25);
  CHECK((abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET) &
         (1u << SYSRST_CTRL_PIN_IN_VALUE_PWRB_IN_BIT)) != 0u);

  abs_mmio_write32(kPinmuxBase + kIoc10AttrOff, orig_ioc10_attr);
  abs_mmio_write32(kPinmuxBase + kIoc10OutselOff, orig_ioc10_outsel);
  abs_mmio_write32(kPinmuxBase + kPwrbInselOff, orig_pwrb_insel);
  busy_spin_micros(20);

  /* 3. Test on dedicated DIO pads DIO[10] (ec_rst_l) and DIO[11] (flash_wp_l)
   *    via SYSRST_CTRL.PIN_IN_VALUE bits 6 and 7. */
  const uint32_t kEcDioOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSysrstCtrlAonEcRstL * sizeof(uint32_t);
  const uint32_t kWpDioOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSysrstCtrlAonFlashWpL * sizeof(uint32_t);

  uint32_t orig_ec_attr = abs_mmio_read32(kPinmuxBase + kEcDioOff);
  uint32_t orig_wp_attr = abs_mmio_read32(kPinmuxBase + kWpDioOff);

  abs_mmio_write32(kPinmuxBase + kEcDioOff, kAttrInDis);
  abs_mmio_write32(kPinmuxBase + kWpDioOff, kAttrInDis);
  busy_spin_micros(25);
  uint32_t pin_in =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 0u);
  CHECK(((pin_in >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 0u);

  abs_mmio_write32(kPinmuxBase + kEcDioOff, kAttrInDis | kAttrInv);
  abs_mmio_write32(kPinmuxBase + kWpDioOff, kAttrInDis | kAttrInv);
  busy_spin_micros(25);
  pin_in = abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 1u);
  CHECK(((pin_in >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 1u);

  abs_mmio_write32(kPinmuxBase + kEcDioOff, orig_ec_attr);
  abs_mmio_write32(kPinmuxBase + kWpDioOff, orig_wp_attr);
  busy_spin_micros(20);

  LOG_INFO("PADRING-E2-01 confirmed.");
}

static void verify_padring_pad_type_warl_masks(void) {
  LOG_INFO(
      "Verifying [padring.sv:145-165,252-272] (BENIGN_RTL_IMPL_DETAIL): "
      "prim_xilinx_ultrascale_pad_wrapper.sv WARL masks (0x83 for BidirStd / "
      "BidirOd vs 0x81 for InputStd)...");

  /* 1. BidirStd MIO pad (IOA2) and BidirOd DIO pad (ec_rst_l = DIO[10]):
   *    support invert (bit 0 = 0x01), virtual_od_en (bit 1 = 0x02), and
   *    input_disable (bit 7 = 0x80) -> 0x83. */
  const uint32_t kIoa2AttrOff = PINMUX_MIO_PAD_ATTR_0_REG_OFFSET +
                                kTopEarlgreyPinmuxMioOutIoa2 * sizeof(uint32_t);
  uint32_t orig_ioa2 = abs_mmio_read32(kPinmuxBase + kIoa2AttrOff);
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, 0xffffffffu);
  uint32_t ioa2_rb = abs_mmio_read32(kPinmuxBase + kIoa2AttrOff);
  abs_mmio_write32(kPinmuxBase + kIoa2AttrOff, orig_ioa2);
  CHECK((ioa2_rb & 0x00000083u) == 0x00000083u &&
        (ioa2_rb & ~0x0010008fu) == 0u);

  const uint32_t kEcDioOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSysrstCtrlAonEcRstL * sizeof(uint32_t);
  uint32_t orig_ec = abs_mmio_read32(kPinmuxBase + kEcDioOff);
  abs_mmio_write32(kPinmuxBase + kEcDioOff, 0xffffffffu);
  uint32_t ec_rb = abs_mmio_read32(kPinmuxBase + kEcDioOff);
  abs_mmio_write32(kPinmuxBase + kEcDioOff, orig_ec);
  CHECK((ec_rb & 0x00000083u) == 0x00000083u && (ec_rb & ~0x0010008fu) == 0u);

  /* 2. InputStd DIO pad (spi_device_sck = DIO[12]):
   *    supports invert (bit 0 = 0x01) and input_disable (bit 7 = 0x80) -> 0x81,
   *    while virtual_od_en (bit 1 = 0x02) is masked off (0). */
  const uint32_t kSckDioOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSpiDeviceSck * sizeof(uint32_t);
  uint32_t orig_sck = abs_mmio_read32(kPinmuxBase + kSckDioOff);
  abs_mmio_write32(kPinmuxBase + kSckDioOff, 0xffffffffu);
  uint32_t sck_rb = abs_mmio_read32(kPinmuxBase + kSckDioOff);
  abs_mmio_write32(kPinmuxBase + kSckDioOff, orig_sck);
  CHECK((sck_rb & 0x00000081u) == 0x00000081u && (sck_rb & 0x00000002u) == 0u);

  LOG_INFO("[padring.sv:145-165,252-272] confirmed.");
}

bool test_main(void) {
  LOG_INFO("Starting PADRING Errata Confirmation Test on %s...",
           kDeviceType == kDeviceFpgaCw340 ? "CW340_FPGA" : "QEMU");

  verify_padring_e2_01_mio_gpio_and_sysrst_loopback();
  verify_padring_pad_type_warl_masks();

  LOG_INFO("All PADRING errata & implementation checks PASSED!");
  return true;
}
