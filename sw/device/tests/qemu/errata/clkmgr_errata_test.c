// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file clkmgr_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for Earlgrey `clkmgr`
 * (`P26`).
 *
 * Empirically verifies all 5 documented specification errata and security
 * hardening behaviors in `/root/knowledge/errata/clkmgr.md` and
 * `/root/knowledge/errata/clkmgr_low_power_exit_and_meas_chk.md` on both
 * physical CW340 FPGA silicon (`fpga_cw340_rom_with_fake_keys`) and QEMU
 * (`sim_qemu_rom_with_fake_keys`):
 *
 * - `[ast_clks_byp.sv:584-668]` (`SPEC_DOC_ERRATA`): `clk_main` (`clk_src_sys`)
 *   remains at full nominal speed (`100 MHz` / `500` cycles per AON tick) in
 *   low-speed external clock mode (`EXTCLK_CTRL.SEL = MuBi4True`,
 *   `HI_SPEED_SEL = MuBi4False`), whereas `clk_io` is divided by 2 (`48 MHz` /
 *   `240` cycles) and `clk_io_div2` / `clk_io_div4` step down their divisors
 *   (`ast_clks_byp.sv:584-657`, `clkmgr.sv:182-216`).
 * - `[top_earlgrey.sv:1829-1842]` (`SPEC_DOC_ERRATA`): Deep-sleep
 * (`LOW_POWER_EXIT`) preserves `DomainAonSel` CSRs (`JITTER_ENABLE`,
 * `*_MEAS_CTRL_SHADOWED`), EXCEPT AST dropping `calib_rdy` to `MuBi4False`
 * during deep sleep unlocks `MEASURE_CTRL_REGWEN` (`rw0c`) back to `1`
 * (`clkmgr.sv:555-558`) and resets all `*_MEAS_CTRL_EN` registers to `0x9`
 * (`MuBi4False`, `clkmgr_meas_chk.sv:81-85`).
 * - `[clkmgr_reg_top.sv:866-890]` (`INTENDED_SECURITY_HARDENING`):
 * `JITTER_ENABLE`
 *   (`0x14`) ties `.wd` to `prim_mubi_pkg::MuBi4True` (`0x6`) and bypasses
 *   `JITTER_REGWEN` (`clkmgr_reg_top.sv:866-890`), so writing any value (even
 *   `0x9` or `0x0` with `JITTER_REGWEN = 0`) permanently latches
 *   `JITTER_ENABLE = 0x6` until reset.
 * - `[clkmgr.sv:572]` (`INTENDED_SECURITY_HARDENING`): `*_MEAS_CTRL_EN`
 *   evaluates `mubi4_test_true_loose` (`!= 0x9`, `clkmgr.sv:572`), so writing
 *   `0x0` enables measurement, and clearing `RECOV_ERR_CODE` (`rw1c`) while an
 *   out-of-bounds threshold remains enabled immediately re-asserts the fault on
 *   the next AON tick (`clkmgr_meas_chk.sv:43-61, 91-112`).
 * - `[clkmgr_reg_pkg.sv:365-388]` (`INTENDED_SECURITY_HARDENING`): Asymmetric
 *   `CLKMGR_PERMIT` byte-enable masks (`4'b0111` on 18/20-bit
 *   `IO/IO_DIV2/MAIN/USB_MEAS_CTRL_SHADOWED` vs `4'b0011` on 16-bit
 *   `IO_DIV4_MEAS_CTRL_SHADOWED`) cause 16-bit halfword stores (`sh`) to fault
 *   (`mcause = 7`) on `IO_MEAS_CTRL_SHADOWED` while succeeding on
 *   `IO_DIV4_MEAS_CTRL_SHADOWED`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/math.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_aon_timer.h"
#include "sw/device/lib/dif/dif_clkmgr.h"
#include "sw/device/lib/dif/dif_pwrmgr.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/aon_timer_testutils.h"
#include "sw/device/lib/testing/pwrmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rstmgr_regs.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kClkmgrBase = TOP_EARLGREY_CLKMGR_AON_BASE_ADDR,
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kSettleDelayMicros = 200,
  kMeasurementsPerRound = 20,
  kVariabilityPercentage = 5,
  kCustomIoDiv4MeasShadowed = 0x00007050u,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  g_fault_count++;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst16 = *(const volatile uint16_t *)mepc;
  ibex_mepc_write(mepc + (((inst16 & 0x3u) == 0x3u) ? 4u : 2u));
}

void ottf_internal_isr(uint32_t *exc_info) { ottf_exception_handler(exc_info); }

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
  dif_pwrmgr_t pwrmgr;
  dif_aon_timer_t aon_timer;
  CHECK_DIF_OK(dif_clkmgr_init(mmio_region_from_addr(kClkmgrBase), &clkmgr));
  CHECK_DIF_OK(dif_pwrmgr_init(mmio_region_from_addr(kPwrmgrBase), &pwrmgr));
  CHECK_DIF_OK(
      dif_aon_timer_init(mmio_region_from_addr(kAonTimerBase), &aon_timer));

  bool is_low_power_exit = UNWRAP(pwrmgr_testutils_is_wakeup_reason(
      &pwrmgr, kDifPwrmgrWakeupRequestSourceFive));

  if (is_low_power_exit) {
    // =========================================================================
    // Phase 2 (Post-Deep-Sleep Wakeup):
    // Verify [top_earlgrey.sv:1829-1842] (SPEC_DOC_ERRATA):
    // Deep sleep preserves `DomainAonSel` CSRs (`JITTER_ENABLE = 0x6`,
    // `IO_DIV4_MEAS_CTRL_SHADOWED = kCustomIoDiv4MeasShadowed`), EXCEPT AST
    // dropping `calib_rdy` during deep sleep forces `MEASURE_CTRL_REGWEN`
    // (`rw0c`) back to `1` and resets `IO_DIV4_MEAS_CTRL_EN` to `0x9`
    // (`MuBi4False`).
    // =========================================================================
    LOG_INFO(
        "Verifying [top_earlgrey.sv:1829-1842] (SPEC_DOC_ERRATA) after "
        "LOW_POWER_EXIT: "
        "DomainAonSel retention of JITTER_ENABLE & IO_DIV4_MEAS_CTRL_SHADOWED "
        "vs calib_rdy unlocking MEASURE_CTRL_REGWEN (rw0c -> 1) and clearing "
        "IO_DIV4_MEAS_CTRL_EN -> 0x9...");
    CHECK_DIF_OK(dif_aon_timer_wakeup_stop(&aon_timer));
    CHECK_DIF_OK(dif_aon_timer_irq_acknowledge_all(&aon_timer));
    CHECK_DIF_OK(dif_pwrmgr_wakeup_reason_clear(&pwrmgr));

    uint32_t jitter_post_sleep =
        abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET);
    uint32_t io_div4_shadow_post_sleep = abs_mmio_read32(
        kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET);
    uint32_t meas_regwen_post_sleep =
        abs_mmio_read32(kClkmgrBase + CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET);
    uint32_t io_div4_en_post_sleep =
        abs_mmio_read32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET);

    CHECK(jitter_post_sleep == kMultiBitBool4True,
          "[top_earlgrey.sv:1829-1842] Expected JITTER_ENABLE retained at 0x6 "
          "across "
          "deep sleep, got 0x%x",
          jitter_post_sleep);
    CHECK(io_div4_shadow_post_sleep == kCustomIoDiv4MeasShadowed,
          "[top_earlgrey.sv:1829-1842] Expected IO_DIV4_MEAS_CTRL_SHADOWED "
          "retained at "
          "0x%08x across deep sleep, got 0x%08x",
          kCustomIoDiv4MeasShadowed, io_div4_shadow_post_sleep);
    CHECK((meas_regwen_post_sleep & 0x1u) == 1u,
          "[top_earlgrey.sv:1829-1842] Expected calib_rdy=MuBi4False during "
          "deep sleep "
          "to unlock rw0c MEASURE_CTRL_REGWEN back to 1, got 0x%x",
          meas_regwen_post_sleep);
    CHECK(io_div4_en_post_sleep == kMultiBitBool4False,
          "[top_earlgrey.sv:1829-1842] Expected calib_rdy=MuBi4False during "
          "deep sleep "
          "to reset IO_DIV4_MEAS_CTRL_EN to 0x9 (MuBi4False), got 0x%x",
          io_div4_en_post_sleep);

    LOG_INFO("All 5 CLKMGR errata & hardening items verified successfully!");
    return true;
  }

  LOG_INFO("Starting CLKMGR CW340/QEMU Errata Confirmation Test (P26)...");

  // =========================================================================
  // Check 1: [ast_clks_byp.sv:584-668] (SPEC_DOC_ERRATA)
  // `clk_main` (`clk_src_sys`) remains at full nominal speed (`500` cycles per
  // AON period) in low-speed external clock mode (`EXTCLK_CTRL.SEL =
  // MuBi4True`, `HI_SPEED_SEL = MuBi4False`), whereas `clk_io` is divided by 2
  // (`240` cycles).
  // =========================================================================
  LOG_INFO(
      "Verifying [ast_clks_byp.sv:584-668] (SPEC_DOC_ERRATA): "
      "clk_main remains un-halved (500 cycles) while clk_io is halved (240 "
      "cycles) in low-speed external clock mode (HI_SPEED_SEL=MuBi4False)...");
  uint32_t delay_micros = 0;
  CHECK_STATUS_OK(aon_timer_testutils_get_us_from_aon_cycles(
      kMeasurementsPerRound, &delay_micros));

  const uint32_t cpu_count =
      (uint32_t)udiv64_slow(kClockFreqCpuHz, kClockFreqAonHz, /*rem_out=*/NULL);
  const uint32_t io_div2_count =
      (uint32_t)udiv64_slow(kClockFreqPeripheralHz, kClockFreqAonHz,
                            /*rem_out=*/NULL) *
      2u;
  const uint32_t io_div2_var = get_variability(io_div2_count);
  const uint32_t cpu_var = get_variability(cpu_count);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(&clkmgr, UINT32_MAX));
  CHECK_DIF_OK(
      dif_clkmgr_external_clock_set_enabled(&clkmgr, /*is_low_speed=*/true));
  IBEX_SPIN_FOR(did_extclk_settle(&clkmgr), kSettleDelayMicros);

  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockIo, (io_div2_count - 1u) - io_div2_var,
      (io_div2_count - 1u) + io_div2_var));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockMain, (cpu_count - 1u) - cpu_var,
      (cpu_count - 1u) + cpu_var));
  busy_spin_micros(delay_micros);

  dif_clkmgr_recov_err_codes_t err_codes = 0;
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK(err_codes == 0u,
        "[ast_clks_byp.sv:584-668] Expected 0 recov_err_code when measuring "
        "clk_io "
        "at half rate (%u) and clk_main at full rate (%u), got 0x%x",
        io_div2_count, cpu_count, err_codes);

  // Now test the spec-predicted half-speed threshold (`cpu_count / 2`) for
  // `clk_main` while in low-speed external clock mode; verify it immediately
  // faults with `MAIN_MEASURE_ERR` (`kDifClkmgrRecovErrTypeMainMeas`) because
  // `clk_main` is un-halved!
  const uint32_t cpu_half = cpu_count / 2u;
  const uint32_t cpu_half_var = get_variability(cpu_half);
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockIo));
  CHECK_DIF_OK(
      dif_clkmgr_disable_measure_counts(&clkmgr, kDifClkmgrMeasureClockMain));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, kDifClkmgrMeasureClockMain, (cpu_half - 1u) - cpu_half_var,
      (cpu_half - 1u) + cpu_half_var));
  busy_spin_micros(delay_micros);

  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(&clkmgr, &err_codes));
  CHECK((err_codes & kDifClkmgrRecovErrTypeMainMeas) != 0u,
        "[ast_clks_byp.sv:584-668] Expected MAIN_MEASURE_ERR when measuring "
        "clk_main "
        "at half speed (%u) in low-speed extclk mode, got 0x%x",
        cpu_half, err_codes);

  CHECK_DIF_OK(dif_clkmgr_external_clock_set_disabled(&clkmgr));
  IBEX_SPIN_FOR(!did_extclk_settle(&clkmgr), kSettleDelayMicros);

  // =========================================================================
  // Check 2: [clkmgr.sv:572] (INTENDED_SECURITY_HARDENING)
  // `*_MEAS_CTRL_EN` evaluates `mubi4_test_true_loose` (`!= 0x9`), so writing
  // `0x0` keeps measurement enabled, and clearing `RECOV_ERR_CODE` (`rw1c`)
  // while the out-of-bounds threshold remains enabled immediately re-asserts
  // `MAIN_MEASURE_ERR` on the next AON tick!
  // =========================================================================
  LOG_INFO(
      "Verifying [clkmgr.sv:572] (INTENDED_SECURITY_HARDENING): "
      "Loose mubi4_test_true_loose (!= 0x9) on MAIN_MEAS_CTRL_EN (0x0 keeps "
      "measurement enabled) & continuous RECOV_ERR_CODE re-assertion...");
  abs_mmio_write32(kClkmgrBase + CLKMGR_MAIN_MEAS_CTRL_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, UINT32_MAX);
  busy_spin_micros(delay_micros);
  uint32_t recov_after_zero_en =
      abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK((recov_after_zero_en &
         (1u << CLKMGR_RECOV_ERR_CODE_MAIN_MEASURE_ERR_BIT)) != 0u,
        "[clkmgr.sv:572] Expected MAIN_MEASURE_ERR to re-assert when "
        "MAIN_MEAS_CTRL_EN == 0x0 (!= 0x9), got 0x%x",
        recov_after_zero_en);

  // Write strict `kMultiBitBool4False` (`0x9`) to disable `MAIN_MEAS_CTRL_EN`,
  // clear `RECOV_ERR_CODE`, wait `delay_micros`, and verify it stays 0:
  abs_mmio_write32(kClkmgrBase + CLKMGR_MAIN_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4False);
  busy_spin_micros(20);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, UINT32_MAX);
  busy_spin_micros(delay_micros);
  uint32_t recov_after_nine_en =
      abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK(recov_after_nine_en == 0u,
        "[clkmgr.sv:572] Expected RECOV_ERR_CODE == 0 after setting "
        "MAIN_MEAS_CTRL_EN = 0x9 (MuBi4False), got 0x%x",
        recov_after_nine_en);

  // =========================================================================
  // Check 3: [clkmgr_reg_pkg.sv:365-388] (INTENDED_SECURITY_HARDENING)
  // Asymmetric `CLKMGR_PERMIT` byte-enable masks (`4'b0111` on 20-bit
  // `IO_MEAS_CTRL_SHADOWED` vs `4'b0011` on 16-bit
  // `IO_DIV4_MEAS_CTRL_SHADOWED`):
  // 16-bit halfword store (`sh`) faults (`mcause = 7`) on
  // `IO_MEAS_CTRL_SHADOWED` (`0x28`) while succeeding on
  // `IO_DIV4_MEAS_CTRL_SHADOWED` (`0x38`).
  // =========================================================================
  LOG_INFO(
      "Verifying [clkmgr_reg_pkg.sv:365-388] (INTENDED_SECURITY_HARDENING): "
      "Asymmetric CLKMGR_PERMIT (4'b0111 vs 4'b0011) sub-word write fault "
      "(mcause=7) on IO_MEAS_CTRL_SHADOWED vs IO_DIV4_MEAS_CTRL_SHADOWED...");
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kClkmgrBase +
                         CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET) = 0x59eau;
  CHECK(
      g_fault_count == 1u,
      "[clkmgr_reg_pkg.sv:365-388] Expected 16-bit sh to IO_MEAS_CTRL_SHADOWED "
      "(permit 4'b0111) to fault");
  CHECK(g_last_mcause == 7u,
        "[clkmgr_reg_pkg.sv:365-388] Expected Store Access Fault (mcause=7), "
        "got 0x%x",
        g_last_mcause);

  g_fault_count = 0;
  *(volatile uint16_t *)(kClkmgrBase +
                         CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)kCustomIoDiv4MeasShadowed;
  *(volatile uint16_t *)(kClkmgrBase +
                         CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)kCustomIoDiv4MeasShadowed;
  CHECK(g_fault_count == 0u,
        "[clkmgr_reg_pkg.sv:365-388] Expected 16-bit sh to "
        "IO_DIV4_MEAS_CTRL_SHADOWED "
        "(permit 4'b0011) to succeed without fault");
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
            kCustomIoDiv4MeasShadowed,
        "[clkmgr_reg_pkg.sv:365-388] Expected 16-bit sh shadow write to commit "
        "0x%08x",
        kCustomIoDiv4MeasShadowed);

  // =========================================================================
  // Check 4: [clkmgr_reg_top.sv:866-890] (INTENDED_SECURITY_HARDENING)
  // `JITTER_ENABLE` (`0x14`) ties `.wd` to `prim_mubi_pkg::MuBi4True` (`0x6`)
  // and bypasses `JITTER_REGWEN` (`0x10`), so writing `kMultiBitBool4False`
  // (`0x9`) or `0x0` even with `JITTER_REGWEN == 0` permanently latches
  // `JITTER_ENABLE = 0x6` (`MuBi4True`)!
  // =========================================================================
  LOG_INFO(
      "Verifying [clkmgr_reg_top.sv:866-890] (INTENDED_SECURITY_HARDENING): "
      "JITTER_ENABLE hardwired .wd(MuBi4True=0x6) ignoring write data (0x9) "
      "and bypassing JITTER_REGWEN=0...");
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET) == 0u,
        "[clkmgr_reg_top.sv:866-890] Expected JITTER_REGWEN == 0 after rw0c "
        "clear");
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
            kMultiBitBool4False,
        "[clkmgr_reg_top.sv:866-890] Expected initial JITTER_ENABLE == 0x9");

  // Write `kMultiBitBool4False` (`0x9`) while `JITTER_REGWEN == 0`:
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  uint32_t jitter_after_false_wr =
      abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET);
  CHECK(jitter_after_false_wr == kMultiBitBool4True,
        "[clkmgr_reg_top.sv:866-890] Expected writing 0x9 (MuBi4False) with "
        "JITTER_REGWEN==0 to latch JITTER_ENABLE = 0x6 (MuBi4True), got 0x%x",
        jitter_after_false_wr);

  // =========================================================================
  // Check 5 Setup: Prepare for [top_earlgrey.sv:1829-1842] Deep-Sleep
  // Transition Enable `IO_DIV4_MEAS_CTRL_EN = 0x6`, lock `MEASURE_CTRL_REGWEN =
  // 0`
  // (`rw0c`), and enter deep sleep (`domain_config = 0`) with an `aon_timer`
  // wakeup so Phase 2 can verify `DomainAonSel` retention vs `calib_rdy`
  // unlocking `MEASURE_CTRL_REGWEN` back to `1` and resetting
  // `IO_DIV4_MEAS_CTRL_EN` to `0x9`.
  // =========================================================================
  LOG_INFO(
      "Preparing [top_earlgrey.sv:1829-1842]: Locking MEASURE_CTRL_REGWEN=0 "
      "(rw0c), "
      "enabling IO_DIV4_MEAS_CTRL_EN=0x6, and entering deep sleep...");
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kClkmgrBase + CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET) ==
            0u,
        "[top_earlgrey.sv:1829-1842] Expected MEASURE_CTRL_REGWEN == 0 before "
        "sleep");
  CHECK(
      abs_mmio_read32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET) ==
          kMultiBitBool4True,
      "[top_earlgrey.sv:1829-1842] Expected IO_DIV4_MEAS_CTRL_EN == 0x6 before "
      "sleep");

  CHECK_STATUS_OK(aon_timer_testutils_wakeup_config(&aon_timer, 10u));
  CHECK_STATUS_OK(pwrmgr_testutils_enable_low_power(
      &pwrmgr, /*wakeups=*/kDifPwrmgrWakeupRequestSourceFive,
      /*domain_config=*/0));
  wait_for_interrupt();

  return false;
}
