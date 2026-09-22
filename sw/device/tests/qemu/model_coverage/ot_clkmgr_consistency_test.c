// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/math.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_base.h"
#include "sw/device/lib/dif/dif_clkmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/aon_timer_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kClkmgrBase = TOP_EARLGREY_CLKMGR_AON_BASE_ADDR,
  kSettleDelayMicros = 200,
  kMeasurementsPerRound = 100,
  kVariabilityPercentage = 5,
};

static bool did_extclk_settle(const dif_clkmgr_t *clkmgr) {
  bool status = false;
  CHECK_DIF_OK(dif_clkmgr_external_clock_is_settled(clkmgr, &status));
  return status;
}

static uint32_t get_variability(uint32_t cycles) {
  return ((cycles * kVariabilityPercentage) + 99u) / 100u + 1u;
}

bool test_main(void) {
  dif_clkmgr_t clkmgr;
  CHECK_DIF_OK(dif_clkmgr_init(mmio_region_from_addr(kClkmgrBase), &clkmgr));

  // 1. Verify W/O ALERT_TEST readback returns 0 and R/O CLK_HINTS_STATUS /
  // FATAL_ERR_CODE 32-bit writes complete without bus fault while preserving
  // R/O state.
  uint32_t alert_test_rd =
      abs_mmio_read32(kClkmgrBase + CLKMGR_ALERT_TEST_REG_OFFSET);
  CHECK(alert_test_rd == 0u, "Expected ALERT_TEST readback == 0, got 0x%x",
        alert_test_rd);

  uint32_t hints_status_before =
      abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_HINTS_STATUS_REG_OFFSET);
  abs_mmio_write32(kClkmgrBase + CLKMGR_CLK_HINTS_STATUS_REG_OFFSET,
                   0xffffffffu);
  uint32_t hints_status_after =
      abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_HINTS_STATUS_REG_OFFSET);
  CHECK(hints_status_after == hints_status_before,
        "Expected R/O CLK_HINTS_STATUS unchanged (0x%x), got 0x%x",
        hints_status_before, hints_status_after);

  uint32_t fatal_err_before =
      abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET);
  CHECK(fatal_err_before == 0u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET, 0xffffffffu);
  uint32_t fatal_err_after =
      abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET);
  CHECK(fatal_err_after == 0u,
        "Expected R/O FATAL_ERR_CODE unchanged (0), got 0x%x", fatal_err_after);

  // 2. Verify low-speed EXTCLK_CTRL (SEL = kMuBi4True, HI_SPEED_SEL =
  // kMuBi4False) clock measurement consistency between CW340 FPGA
  // (ast_clks_byp.sv under AST_BYPASS_CLK) and QEMU:
  // - clk_io is divided by 2 (runs at io_div2 rate),
  // - clk_io_div2 and clk_io_div4 step down their divisors (remain at io_div2 /
  // io_div4 rate),
  // - clk_main remains undivided at kDeviceCpuCount rate.
  uint32_t delay_micros = 0;
  CHECK_STATUS_OK(aon_timer_testutils_get_us_from_aon_cycles(
      kMeasurementsPerRound, &delay_micros));

  const uint32_t cpu_count =
      (uint32_t)udiv64_slow(kClockFreqCpuHz, kClockFreqAonHz, /*rem_out=*/NULL);
  const uint32_t io_count =
      (uint32_t)udiv64_slow(kClockFreqPeripheralHz, kClockFreqAonHz,
                            /*rem_out=*/NULL) *
      4u;
  const uint32_t io_div2_count =
      (uint32_t)udiv64_slow(kClockFreqPeripheralHz, kClockFreqAonHz,
                            /*rem_out=*/NULL) *
      2u;
  const uint32_t io_div4_count = (uint32_t)udiv64_slow(
      kClockFreqPeripheralHz, kClockFreqAonHz, /*rem_out=*/NULL);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(&clkmgr, UINT32_MAX));
  dif_clkmgr_recov_err_codes_t err_codes = 0;
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK(err_codes == 0u);

  CHECK_DIF_OK(
      dif_clkmgr_external_clock_set_enabled(&clkmgr, /*is_low_speed=*/true));
  IBEX_SPIN_FOR(did_extclk_settle(&clkmgr), kSettleDelayMicros);

  const uint32_t io_div2_var = get_variability(io_div2_count);
  const uint32_t io_div4_var = get_variability(io_div4_count);
  const uint32_t cpu_var = get_variability(cpu_count);

  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockIo, (io_div2_count - 1u) - io_div2_var,
      (io_div2_count - 1u) + io_div2_var));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockIoDiv2, (io_div2_count - 1u) - io_div2_var,
      (io_div2_count - 1u) + io_div2_var));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockIoDiv4, (io_div4_count - 1u) - io_div4_var,
      (io_div4_count - 1u) + io_div4_var));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockMain, (cpu_count - 1u) - cpu_var,
      (cpu_count - 1u) + cpu_var));

  busy_spin_micros(delay_micros);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK(err_codes == 0u,
        "Expected 0 recov_err_code in low-speed extclk mode, got 0x%x",
        err_codes);

  // Confirm that measuring io against nominal 96 MHz (io_count) while in
  // low-speed extclk mode triggers both kDifClkmgrRecovErrTypeIoMeas and
  // kTopEarlgreyAlertIdClkmgrAonRecovFault (Alert 25).
  // Temporarily mask global IRQs while RECOV_ERR_CODE is held active so
  // ottf_alert_isr is invoked once after the fault source is cleared.
  const uint32_t io_var = get_variability(io_count);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdClkmgrAonRecovFault));
  irq_global_ctrl(false);
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockIo));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockIo, (io_count - 1u) - io_var,
      (io_count - 1u) + io_var));
  busy_spin_micros(delay_micros);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK((err_codes & kDifClkmgrRecovErrTypeIoMeas) != 0u,
        "Expected kDifClkmgrRecovErrTypeIoMeas when measuring io at full rate "
        "in low-speed extclk mode, got 0x%x",
        err_codes);

  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockIo));
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockIoDiv2));
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockIoDiv4));
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockMain));
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(&clkmgr, UINT32_MAX));
  irq_global_ctrl(true);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdClkmgrAonRecovFault));

  CHECK_DIF_OK(dif_clkmgr_external_clock_set_disabled(&clkmgr));
  IBEX_SPIN_FOR(!did_extclk_settle(&clkmgr), kSettleDelayMicros);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK(err_codes == 0u);

  return true;
}
