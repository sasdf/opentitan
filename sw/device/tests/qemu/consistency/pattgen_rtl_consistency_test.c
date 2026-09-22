// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pattgen_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPattgenBase = TOP_EARLGREY_PATTGEN_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)                 \
  do {                                        \
    if (!(cond)) {                            \
      LOG_INFO("RTL_MISMATCH: " __VA_ARGS__); \
      failures++;                             \
    }                                         \
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
  uint32_t failures = 0;

  // 1. Verify CTRL register reserved bits [31:8] are masked to 0 on write.
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0xfffffffcu);
  uint32_t ctrl_val = abs_mmio_read32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET);
  EXPECT_RTL(ctrl_val == 0x000000fcu,
             "[1] CTRL readback = 0x%08x, expected 0x000000fc", ctrl_val);
  if (ctrl_val == 0x000000fcu) {
    LOG_INFO("OK [1]: CTRL reserved bits [31:8] masked to 0x%08x", ctrl_val);
  }
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);

  // 2. Verify ALERT_TEST triggers a one-shot alert pulse and does not latch.
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault,
      kDifAlertHandlerClassD, kDifToggleEnabled, kDifToggleDisabled));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault));

  abs_mmio_write32(kPattgenBase + PATTGEN_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(50);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault, &is_cause));
  EXPECT_RTL(is_cause, "[2a] ALERT_TEST did not set ALERT_CAUSE");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault));
  busy_spin_micros(50);

  // Second write to ALERT_TEST must trigger a fresh alert edge (fails if QEMU
  // left s->alert latched at 1 instead of pulsing 1 -> 0).
  abs_mmio_write32(kPattgenBase + PATTGEN_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(50);
  is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault, &is_cause));
  EXPECT_RTL(is_cause,
             "[2b] Second ALERT_TEST write did not set ALERT_CAUSE (s->alert "
             "stayed latched at 1 instead of pulsing 1->0)");
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPattgenFatalFault));
  if (is_cause) {
    LOG_INFO("OK [2]: ALERT_TEST pulsed 1->0 and re-triggered on second write");
  }

  // 3. Verify Pinmux -> GPIO loopback for pattgen pda0_tx (outsel 49) &
  // pcl0_tx (outsel 50).
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa2,
                                        kTopEarlgreyPinmuxOutselPattgenPda0Tx));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa3,
                                        kTopEarlgreyPinmuxOutselPattgenPcl0Tx));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselIoa2));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio3,
                                       kTopEarlgreyPinmuxInselIoa3));

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(10);
  uint32_t gpio_low =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(gpio_low == 0x0u,
             "[3a] GPIO[3:2] = 0x%x when INACTIVE_LEVEL=0, expected 0x0",
             gpio_low);

  // Set INACTIVE_LEVEL_PCL_CH0 (bit 4) and INACTIVE_LEVEL_PDA_CH0 (bit 5) while
  // disabled.
  uint32_t ctrl_inactive_hi = (1u << PATTGEN_CTRL_INACTIVE_LEVEL_PCL_CH0_BIT) |
                              (1u << PATTGEN_CTRL_INACTIVE_LEVEL_PDA_CH0_BIT);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, ctrl_inactive_hi);
  busy_spin_micros(10);
  uint32_t gpio_hi =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(gpio_hi == 0x3u,
             "[3b] GPIO[3:2] = 0x%x when INACTIVE_LEVEL=1, expected 0x3",
             gpio_hi);
  if (gpio_hi == 0x3u) {
    LOG_INFO("OK [3]: Pinmux -> GPIO loopback reflects INACTIVE_LEVEL (0x%x)",
             gpio_hi);
  }
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(10);

  // 4. Verify clock-driven PREDIV_CH0 timing vs hardcoded 2 ms timer.
  // 4a: Fast pattern (prediv=0, len=3 -> 4 bits, reps=0 -> 8 cycles < 1 us).
  // Should complete well within 100 us (0.1 ms), NOT 2 ms!
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0xau);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   1u << PATTGEN_CTRL_ENABLE_CH0_BIT);
  busy_spin_micros(100);
  uint32_t intr_fast =
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_fast & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) != 0u,
             "[4a] Fast pattern (prediv=0, 4 bits) did not set DONE_CH0 after "
             "100 us (INTR_STATE=0x%x)",
             intr_fast);
  if ((intr_fast & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) != 0u) {
    LOG_INFO("OK [4a]: Fast pattern set DONE_CH0 within 100 us");
  }

  // 5. Verify channel configuration registers (INACTIVE_LEVEL_*) are frozen
  // while ENABLE_CH0 == 1 (`pattgen_chan.sv`: `enable ? inactive_level_q :
  // ctrl_i`).
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   (1u << PATTGEN_CTRL_ENABLE_CH0_BIT) | ctrl_inactive_hi);
  busy_spin_micros(10);
  uint32_t gpio_while_enabled =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(gpio_while_enabled == 0x0u,
             "[5a] INACTIVE_LEVEL updated output (0x%x) while ENABLE_CH0=1, "
             "expected frozen 0x0",
             gpio_while_enabled);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, ctrl_inactive_hi);
  busy_spin_micros(10);
  uint32_t gpio_after_disable =
      (abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u;
  EXPECT_RTL(gpio_after_disable == 0x3u,
             "[5b] INACTIVE_LEVEL did not update output after ENABLE_CH0=0 "
             "(got 0x%x, expected 0x3)",
             gpio_after_disable);
  if (gpio_while_enabled == 0x0u && gpio_after_disable == 0x3u) {
    LOG_INFO(
        "OK [5]: Channel config stays frozen while ENABLE_CH0=1 and updates "
        "when ENABLE_CH0=0");
  }
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(10);

  // 4b: Slow pattern: 8 * (prediv + 1) cycles = kClockFreqPeripheralHz / 200
  // cycles = 5.0 ms (5000 us) exact duration.
  // Must NOT complete after 3 ms (3000 us), and MUST complete by 7 ms!
  uint32_t prediv_5ms = ((uint32_t)kClockFreqPeripheralHz / 1600u) - 1u;
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, prediv_5ms);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   1u << PATTGEN_CTRL_ENABLE_CH0_BIT);
  busy_spin_micros(3000);
  uint32_t intr_early =
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_early & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) == 0u,
             "[4b] Slow pattern (5 ms duration) prematurely set DONE_CH0 at "
             "3 ms (INTR_STATE=0x%x)",
             intr_early);
  busy_spin_micros(4000);
  uint32_t intr_late =
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_late & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) != 0u,
             "[4c] Slow pattern (5 ms duration) did not set DONE_CH0 after "
             "7 ms (INTR_STATE=0x%x)",
             intr_late);
  if ((intr_early & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) == 0u &&
      (intr_late & (1u << PATTGEN_INTR_STATE_DONE_CH0_BIT)) != 0u) {
    LOG_INFO(
        "OK [4b]: Slow pattern (5 ms) did not fire at 3 ms and completed by "
        "7 ms");
  }
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);

  // 6. Verify INTR_ENABLE mask (0x3), INTR_TEST (WO) triggering INTR_STATE,
  // INTR_STATE RW1C clearing, and WO zero readback for INTR_TEST / ALERT_TEST.
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_ENABLE_REG_OFFSET, 0xffffffffu);
  uint32_t intr_en =
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_ENABLE_REG_OFFSET);
  EXPECT_RTL(intr_en == 0x3u,
             "[6a] INTR_ENABLE readback = 0x%08x, expected 0x3", intr_en);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_ENABLE_REG_OFFSET, 0u);

  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_TEST_REG_OFFSET, 0x3u);
  EXPECT_RTL(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_TEST_REG_OFFSET) == 0u,
             "[6b] INTR_TEST (WO) readback must be 0");
  EXPECT_RTL(
      abs_mmio_read32(kPattgenBase + PATTGEN_ALERT_TEST_REG_OFFSET) == 0u,
      "[6c] ALERT_TEST (WO) readback must be 0");
  EXPECT_RTL(
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0x3u,
      "[6d] INTR_STATE after INTR_TEST=0x3 must be 0x3");
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  EXPECT_RTL(
      abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0u,
      "[6e] INTR_STATE after RW1C write 0x3 must be 0");

  // 7. Verify PATTGEN_PERMIT sub-word write checks (pattgen_reg_top.sv wr_err):
  // - Sub-word write to DATA_CH0_0 (PATTGEN_PERMIT = 4'b1111) faults and blocks
  // write.
  // - Sub-word write to CTRL+1 (PATTGEN_PERMIT = 4'b0001, reg_be = 4'b0010)
  // faults.
  // - Byte-0 sub-word write to CTRL (PATTGEN_PERMIT = 4'b0001, reg_be =
  // 4'b0001) succeeds.
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0x12345678u);
  kFaultCount = 0;
  *((volatile uint8_t *)(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET)) = 0x99u;
  EXPECT_RTL(
      kFaultCount == 1u &&
          abs_mmio_read32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET) ==
              0x12345678u,
      "[7a] Sub-word write to DATA_CH0_0 must fault and not modify register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kPattgenBase + PATTGEN_CTRL_REG_OFFSET + 1u)) = 0x11u;
  EXPECT_RTL(
      kFaultCount == 1u &&
          abs_mmio_read32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET) == 0u,
      "[7b] Sub-word write to CTRL+1 must fault and not modify register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kPattgenBase + PATTGEN_CTRL_REG_OFFSET)) = 0x0cu;
  EXPECT_RTL(
      kFaultCount == 0u &&
          abs_mmio_read32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET) == 0x0cu,
      "[7c] Byte-0 sub-word write to CTRL (PATTGEN_PERMIT=4'b0001) must "
      "succeed");
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);

  return failures == 0;
}
