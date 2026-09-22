// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  if (g_expect_access_fault && (mcause == 5u || mcause == 7u)) {
    g_access_fault_count++;
    g_last_mcause = mcause;
    uint32_t mepc = 0;
    CSR_READ(CSR_REG_MEPC, &mepc);
    uint16_t inst16 = *(const uint16_t *)mepc;
    uint32_t step = ((inst16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "FAULT", mcause);
  abort();
}

#define EXPECT_RTL(cond, ...)                 \
  do {                                        \
    if (!(cond)) {                            \
      LOG_INFO("RTL_MISMATCH: " __VA_ARGS__); \
      failures++;                             \
    }                                         \
  } while (0)

bool test_main(void) {
  uint32_t failures = 0;

  // 1. Verify GPIO.ALERT_TEST is a one-shot 1-cycle pulse (`alert_test.q &
  // alert_test.qe` in gpio.sv:149-152) rather than latching s->alert high.
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdGpioFatalFault, kDifAlertHandlerClassD,
      kDifToggleEnabled, kDifToggleDisabled));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdGpioFatalFault));

  abs_mmio_write32(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(20);
  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdGpioFatalFault, &is_cause));
  EXPECT_RTL(is_cause, "[1a] GPIO ALERT_TEST did not set ALERT_CAUSE");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdGpioFatalFault));
  busy_spin_micros(20);
  is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdGpioFatalFault, &is_cause));
  EXPECT_RTL(!is_cause,
             "[1b] GPIO ALERT_CAUSE remained latched after ACK (s->alert was "
             "not pulsed 1->0)");
  if (!is_cause) {
    LOG_INFO("OK [1]: GPIO ALERT_TEST generated one-shot pulse");
  }

  // 2. Verify level interrupts (`INTR_CTRL_EN_LVLLOW` / `INTR_CTRL_EN_LVLHIGH`)
  // continuously assert `event_intr_actlow` / `event_intr_acthigh` so writing 1
  // to `INTR_STATE` (RW1C) while the level condition remains true immediately
  // re-asserts `INTR_STATE` (`gpio.sv:139-145`).
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio0,
                                       kTopEarlgreyPinmuxInselConstantZero));
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_LVLLOW_REG_OFFSET, 1u);
  busy_spin_micros(10);

  uint32_t intr_before =
      abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) & 1u;
  EXPECT_RTL(intr_before == 1u,
             "[2a] INTR_STATE[0] = %u when LVLLOW=1 and input=0, expected 1",
             intr_before);

  // Write 1 to INTR_STATE[0] (RW1C) without reading DATA_IN first; because
  // `~data_in_d & intr_ctrl_en_lvllow.q` is still 1 on every cycle, INTR_STATE
  // must read back 1 immediately.
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(5);
  uint32_t intr_after_rw1c =
      abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) & 1u;
  EXPECT_RTL(intr_after_rw1c == 1u,
             "[2b] INTR_STATE[0] cleared to %u on RW1C while LVLLOW=1 & "
             "input=0, expected 1",
             intr_after_rw1c);
  if (intr_before == 1u && intr_after_rw1c == 1u) {
    LOG_INFO("OK [2]: Level interrupt INTR_STATE[0] remains asserted on RW1C");
  }
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_LVLLOW_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);

  // 3. Verify MIO_OUTSEL ConstantOne and cross-GPIO (`GPIO[2] -> Ioa2 ->
  // GPIO[3]`) loopback into `GPIO.DATA_IN`.
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa2,
                                        kTopEarlgreyPinmuxOutselConstantOne));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselIoa2));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio3,
                                       kTopEarlgreyPinmuxInselIoa2));
  busy_spin_micros(10);
  uint32_t data_const_one =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(data_const_one == 0x3u,
             "[3a] GPIO[3:2] = 0x%x when Ioa2 MIO_OUTSEL=ConstantOne, expected "
             "0x3",
             data_const_one);

  // Route GPIO[2] output to Ioa2 (`MIO_OUTSEL[Ioa2] = GpioGpio2`), drive
  // GPIO[2]=1 (`DIRECT_OE = 1<<2, DIRECT_OUT = 1<<2`), and verify BOTH
  // GPIO[2] and GPIO[3] (`PERIPH_INSEL = Ioa2`) read 1 (`0x3`).
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa2,
                                        kTopEarlgreyPinmuxOutselGpioGpio2));
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);
  uint32_t data_cross_gpio =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(data_cross_gpio == 0x3u,
             "[3b] GPIO[3:2] = 0x%x when GPIO[2]=1 drives Ioa2 (cross-pin to "
             "GPIO[3]), expected 0x3",
             data_cross_gpio);
  if (data_const_one == 0x3u && data_cross_gpio == 0x3u) {
    LOG_INFO(
        "OK [3]: MIO_OUTSEL ConstantOne and cross-GPIO (GPIO2->Ioa2->GPIO3) "
        "loopback match RTL");
  }

  // 4. Verify MIO_PAD_ATTR.INVERT (bit 0) behavior:
  // 4a: In self-loopback (`GPIO[2]` driving `Ioa2` with `DIRECT_OUT[2]=0` and
  // reading `Ioa2` with `MIO_PAD_ATTR[Ioa2].INVERT=1`), output inversion
  // (`0 ^ 1 = 1` on pad) and input inversion (`1 ^ 1 = 0` from pad) cancel out,
  // so `GPIO.DATA_IN[2]` MUST read 0!
  uint32_t ioa2_attr_off =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET +
      (uint32_t)kTopEarlgreyPinmuxMioOutIoa2 * sizeof(uint32_t);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT);
  busy_spin_micros(10);
  uint32_t data_self_inv0 =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u;
  EXPECT_RTL(data_self_inv0 == 0u,
             "[4a] Self-loopback GPIO[2]=0 with MIO_PAD_ATTR[Ioa2].INVERT=1 "
             "read %u, expected 0 (out_inv ^ in_inv == 0)",
             data_self_inv0);

  // 4b: When `PERIPH_INSEL[2] = ConstantZero` (`insel = 0`), `mio_mux[0]` in
  // `pinmux.sv:470` selects `1'b0` directly inside pinmux without passing
  // through `Ioa2`'s pad wrapper (`mio_attr[Ioa2].invert`), so
  // `GPIO.DATA_IN[2]` MUST read 0 even while `MIO_OUTSEL[Ioa2] = GpioGpio2` and
  // `MIO_PAD_ATTR[Ioa2].INVERT = 1`.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselConstantZero));
  busy_spin_micros(10);
  uint32_t data_const0_with_pad_inv =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u;
  EXPECT_RTL(data_const0_with_pad_inv == 0u,
             "[4b] PERIPH_INSEL[2]=ConstantZero read %u when "
             "MIO_PAD_ATTR[Ioa2].INVERT=1, expected 0",
             data_const0_with_pad_inv);
  if (data_self_inv0 == 0u && data_const0_with_pad_inv == 0u) {
    LOG_INFO("OK [4]: MIO_PAD_ATTR.INVERT pad vs mux semantics match RTL");
  }

  // 5. Verify MIO_PAD_ATTR.INPUT_DISABLE (bit 7 = 0x80) WARL readback and pad
  // input disable behavior (`ie = 0 -> in_raw_o = 0` in
  // `prim_xilinx_ultrascale_pad_wrapper.sv:49, 94` even while pad drives 1).
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselIoa2));
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT);
  busy_spin_micros(10);
  uint32_t attr_rb = abs_mmio_read32(kPinmuxBase + ioa2_attr_off);
  uint32_t data_in_dis =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u;
  EXPECT_RTL(attr_rb == (1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT),
             "[5a] MIO_PAD_ATTR[Ioa2] write INPUT_DISABLE (0x80) read back "
             "0x%08x, expected 0x00000080",
             attr_rb);
  EXPECT_RTL(
      data_in_dis == 0u,
      "[5b] GPIO.DATA_IN[2] = %u when MIO_PAD_ATTR[Ioa2].INPUT_DISABLE=1 "
      "(driving 1), expected 0",
      data_in_dis);
  if (attr_rb == (1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT) &&
      data_in_dis == 0u) {
    LOG_INFO(
        "OK [5]: MIO_PAD_ATTR.INPUT_DISABLE (0x80) WARL and DATA_IN=0 match "
        "RTL");
  }

  // 6. Verify MASKED_OUT_{LOWER,UPPER} and MASKED_OE_{LOWER,UPPER} mask bits
  // [31:16] read back 0 (`hw2reg.masked_*.mask.d = 16'h0` in `gpio.sv:70-74,
  // 93-96`) and alias to DIRECT_OUT / DIRECT_OE.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kGpioBase + GPIO_MASKED_OUT_LOWER_REG_OFFSET, 0x00ff00a5u);
  abs_mmio_write32(kGpioBase + GPIO_MASKED_OUT_UPPER_REG_OFFSET, 0xff005a00u);
  uint32_t dir_out = abs_mmio_read32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET);
  uint32_t m_lower =
      abs_mmio_read32(kGpioBase + GPIO_MASKED_OUT_LOWER_REG_OFFSET);
  uint32_t m_upper =
      abs_mmio_read32(kGpioBase + GPIO_MASKED_OUT_UPPER_REG_OFFSET);
  EXPECT_RTL(
      dir_out == 0x5a3456a5u,
      "[6a] DIRECT_OUT after masked writes = 0x%08x, expected 0x5a3456a5",
      dir_out);
  EXPECT_RTL(m_lower == 0x000056a5u && m_upper == 0x00005a34u,
             "[6b] MASKED_OUT_{LOWER,UPPER} readback = (0x%08x, 0x%08x), "
             "expected (0x000056a5, 0x00005a34) with mask[31:16]=0",
             m_lower, m_upper);

  // 7. Verify INTR_CTRL_EN_RISING / INTR_CTRL_EN_FALLING edge interrupts latch
  // on 0->1 and 1->0 transitions and clear to 0 on INTR_STATE RW1C.
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_RISING_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_FALLING_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);
  uint32_t intr_rise =
      (abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) >> 2) & 1u;
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 1u << 2);
  uint32_t intr_rise_cleared =
      (abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) >> 2) & 1u;
  EXPECT_RTL(
      intr_rise == 1u && intr_rise_cleared == 0u,
      "[7] Edge interrupt INTR_STATE[2] rise=%u cleared=%u, expected 1,0",
      intr_rise, intr_rise_cleared);

  // 8. Wave 5: GPIO_PERMIT sub-word write wr_err and addrmiss checks.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0xa5a55a5au);
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0x11u);
  g_expect_access_fault = false;
  uint32_t out_after_sb =
      abs_mmio_read32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET);
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 7u &&
                 out_after_sb == 0xa5a55a5au,
             "[8a] 8-bit write to DIRECT_OUT (PERMIT=4'b1111) must raise Store "
             "Access Fault (mcause=7) and preserve 0xa5a55a5a (got count=%u "
             "mcause=%u val=0x%08x)",
             g_access_fault_count, g_last_mcause, out_after_sb);

  g_access_fault_count = 0;
  abs_mmio_write8(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 0u);
  EXPECT_RTL(g_access_fault_count == 0u,
             "[8b] 8-bit write to ALERT_TEST+0 (PERMIT=4'b0001) must succeed");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET + 1u, 0u);
  g_expect_access_fault = false;
  EXPECT_RTL(
      g_access_fault_count == 1u && g_last_mcause == 7u,
      "[8c] 8-bit write to ALERT_TEST+1 (PERMIT=4'b0001, reg_be=4'b0010) "
      "must raise Store Access Fault (mcause=7)");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kGpioBase + 0x40u);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 5u,
             "[8d] 32-bit read at unmapped offset 0x40 (addrmiss) must raise "
             "Load Access Fault (mcause=5)");

  // Cleanup
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_RISING_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_FALLING_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);

  return failures == 0;
}
