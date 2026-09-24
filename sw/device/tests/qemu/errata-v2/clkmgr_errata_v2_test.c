// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Clock Manager (CLKMGR) Errata Verification Test on Earlgrey v2 (CW340 FPGA).
 *
 * Exercises and verifies:
 *   1. Asymmetric low-speed external clock division in AST bypass mode
 *      (`EXTCLK_CTRL.HI_SPEED_SEL = MuBi4False` divides `clk_io` by 2 while
 *      leaving `clk_main` un-divided).
 *   2. Deep-sleep (`LOW_POWER_EXIT`) `DomainAonSel` CSR retention
 *      (`JITTER_ENABLE`, `JITTER_REGWEN`, `*_MEAS_CTRL_SHADOWED`) vs
 *      AST `calib_rdy` drop unlocking `MEASURE_CTRL_REGWEN` (`1`) and resetting
 *      `*_MEAS_CTRL_EN` to `MuBi4False` (`0x9`).
 *   3. Earlgrey v2 fix for `JITTER_ENABLE` write-data wiring and
 *      `JITTER_REGWEN` write-enable gating (`dif_clkmgr_jitter_set_enabled`
 *      can both enable `0x6` and disable `0x9` while unlocked, and is blocked
 *      once `JITTER_REGWEN` is cleared to `0`).
 *   4. Loose multi-bit boolean evaluation (`mubi4_test_true_loose`, `!= 0x9`)
 *      on `*_MEAS_CTRL_EN` and continuous hardware re-assertion of
 *      `RECOV_ERR_CODE` (`rw1c`) while an invalid measurement remain active.
 *   5. Sub-word (`sh`) store fault (`mcause = 7`) on 3-byte-permitted
 *      `IO_MEAS_CTRL_SHADOWED` (`CLKMGR_PERMIT = 4'b0111`) vs non-faulting
 *      halfword access on 2-byte-permitted `IO_DIV4_MEAS_CTRL_SHADOWED`
 *      (`CLKMGR_PERMIT = 4'b0011`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_aon_timer.h"
#include "sw/device/lib/dif/dif_clkmgr.h"
#include "sw/device/lib/dif/dif_pwrmgr.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/aon_timer_testutils.h"
#include "sw/device/lib/testing/pwrmgr_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kClkmgrBase = 0x40420000u,

  kExpectedIoNominalCount = 480,
  kExpectedIoLowSpeedCount = 240,
  kExpectedMainNominalCount = 500,
  kMeasVariability = 20,

  kCustomIoMinPostSleep = 211,
  kCustomIoMaxPostSleep = 259,
  kCustomMainMinPostSleep = 471,
  kCustomMainMaxPostSleep = 529,
};

#include "sw/device/lib/base/csr.h"

static volatile bool g_store_fault_seen = false;
static volatile uint32_t g_store_fault_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  if (mcause == 7u) {
    g_store_fault_seen = true;
    g_store_fault_mcause = mcause;
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

static void shadow_write32(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

static void shadow_write16(uint32_t addr, uint16_t val) {
  asm volatile("sh %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
  asm volatile("sh %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
}

static bool probe_meas_window(const dif_clkmgr_t *clkmgr,
                              dif_clkmgr_measure_clock_t clock,
                              dif_clkmgr_recov_err_type_t err_mask,
                              uint32_t min_thresh, uint32_t max_thresh) {
  CHECK_DIF_OK(dif_clkmgr_disable_measure_counts(clkmgr, clock));
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(clkmgr, ~0u));
  CHECK_DIF_OK(
      dif_clkmgr_enable_measure_counts(clkmgr, clock, min_thresh, max_thresh));
  busy_spin_micros(250);
  dif_clkmgr_recov_err_codes_t codes = 0;
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(clkmgr, &codes));
  CHECK_DIF_OK(dif_clkmgr_disable_measure_counts(clkmgr, clock));
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(clkmgr, ~0u));
  return (codes & err_mask) != 0;
}

static void run_post_wakeup_checks(const dif_clkmgr_t *clkmgr,
                                   dif_clkmgr_measure_clock_t io_clk,
                                   dif_clkmgr_measure_clock_t main_clk) {
  LOG_INFO("Phase 2: Verifying post-deep-sleep CLKMGR state and errata");

  // -------------------------------------------------------------------------
  // Check 3: Post-deep-sleep state divergence between AON CSRs and calib_rdy
  // -------------------------------------------------------------------------
  uint32_t jitter_raw =
      abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET);
  CHECK(jitter_raw == kMultiBitBool4True,
        "Expected JITTER_ENABLE (DomainAonSel) preserved across deep sleep "
        "(0x%x), got 0x%x",
        kMultiBitBool4True, jitter_raw);

  bool jitter_locked = false;
  CHECK_DIF_OK(dif_clkmgr_jitter_enable_is_locked(clkmgr, &jitter_locked));
  CHECK(jitter_locked,
        "Expected JITTER_REGWEN (DomainAonSel) to stay locked (0) across deep "
        "sleep");

  uint32_t io_min = 0, io_max = 0;
  uint32_t main_min = 0, main_max = 0;
  CHECK_DIF_OK(dif_clkmgr_measure_counts_get_thresholds(clkmgr, io_clk, &io_min,
                                                        &io_max));
  CHECK_DIF_OK(dif_clkmgr_measure_counts_get_thresholds(clkmgr, main_clk,
                                                        &main_min, &main_max));
  CHECK(io_min == kCustomIoMinPostSleep && io_max == kCustomIoMaxPostSleep,
        "Expected IO_MEAS_CTRL_SHADOWED preserved (%u..%u), got (%u..%u)",
        kCustomIoMinPostSleep, kCustomIoMaxPostSleep, io_min, io_max);
  CHECK(main_min == kCustomMainMinPostSleep &&
            main_max == kCustomMainMaxPostSleep,
        "Expected MAIN_MEAS_CTRL_SHADOWED preserved (%u..%u), got (%u..%u)",
        kCustomMainMinPostSleep, kCustomMainMaxPostSleep, main_min, main_max);

  dif_toggle_t meas_ctrl_regwen = kDifToggleDisabled;
  CHECK_DIF_OK(dif_clkmgr_measure_ctrl_get_enable(clkmgr, &meas_ctrl_regwen));
  CHECK(meas_ctrl_regwen == kDifToggleEnabled,
        "Expected MEASURE_CTRL_REGWEN unlocked (1) after deep sleep due to AST "
        "dropping calib_rdy");

  dif_toggle_t io_en = kDifToggleEnabled;
  dif_toggle_t main_en = kDifToggleEnabled;
  CHECK_DIF_OK(dif_clkmgr_measure_counts_get_enable(clkmgr, io_clk, &io_en));
  CHECK_DIF_OK(
      dif_clkmgr_measure_counts_get_enable(clkmgr, main_clk, &main_en));
  CHECK(io_en == kDifToggleDisabled && main_en == kDifToggleDisabled,
        "Expected *_MEAS_CTRL_EN reset to MuBi4False (0x9) after deep sleep");

  // -------------------------------------------------------------------------
  // Check 4: Verify JITTER_REGWEN lock still blocks JITTER_ENABLE writes
  // -------------------------------------------------------------------------
  dif_result_t res = dif_clkmgr_jitter_set_enabled(clkmgr, kDifToggleDisabled);
  CHECK(res == kDifLocked,
        "Expected dif_clkmgr_jitter_set_enabled to return kDifLocked (%d), "
        "got %d",
        kDifLocked, res);
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected raw MMIO write to JITTER_ENABLE to be blocked by "
        "JITTER_REGWEN=0");

  // -------------------------------------------------------------------------
  // Check 5: Loose MuBi4 evaluation on *_MEAS_CTRL_EN & continuous RECOV_ERR
  // -------------------------------------------------------------------------
  LOG_INFO("Check 5: Testing loose MuBi4 *_MEAS_CTRL_EN & RECOV_ERR_CODE");
  CHECK_DIF_OK(dif_clkmgr_disable_measure_counts(clkmgr, io_clk));
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(clkmgr, ~0u));

  // Set out-of-range thresholds (10..20 vs actual ~480 cycles) while disabled.
  uint32_t bad_shadow =
      bitfield_field32_write(0, CLKMGR_IO_MEAS_CTRL_SHADOWED_LO_FIELD, 10);
  bad_shadow = bitfield_field32_write(
      bad_shadow, CLKMGR_IO_MEAS_CTRL_SHADOWED_HI_FIELD, 20);
  shadow_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET,
                 bad_shadow);

  // Write non-canonical value 0x0 (neither MuBi4True 0x6 nor MuBi4False 0x9).
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_EN_REG_OFFSET, 0x0);
  busy_spin_micros(250);

  dif_clkmgr_recov_err_codes_t err_codes = 0;
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(clkmgr, &err_codes));
  CHECK((err_codes & kDifClkmgrRecovErrTypeIoMeas) != 0,
        "Expected non-canonical IO_MEAS_CTRL_EN=0x0 to enable measurement via "
        "mubi4_test_true_loose and fire IO_MEASURE_ERR (got 0x%x)",
        err_codes);

  // Clear RECOV_ERR_CODE while measurement remains enabled -> hardware
  // re-asserts on the next AON cycle.
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(
      clkmgr, kDifClkmgrRecovErrTypeIoMeas));
  busy_spin_micros(250);
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(clkmgr, &err_codes));
  CHECK((err_codes & kDifClkmgrRecovErrTypeIoMeas) != 0,
        "Expected RECOV_ERR_CODE.IO_MEASURE_ERR to immediately re-assert while "
        "IO_MEAS_CTRL_EN != 0x9");

  // Disable via canonical MuBi4False (0x9) and verify error clears cleanly.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4False);
  busy_spin_micros(250);
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_clear_codes(
      clkmgr, kDifClkmgrRecovErrTypeIoMeas));
  busy_spin_micros(250);
  CHECK_DIF_OK(dif_clkmgr_recov_err_code_get_codes(clkmgr, &err_codes));
  CHECK((err_codes & kDifClkmgrRecovErrTypeIoMeas) == 0,
        "Expected IO_MEASURE_ERR to stay cleared after writing MuBi4False "
        "(0x9), got 0x%x",
        err_codes);

  // -------------------------------------------------------------------------
  // Check 6: Asymmetric CLKMGR_PERMIT sub-word access fault (4'b0111 vs 0011)
  // -------------------------------------------------------------------------
  LOG_INFO("Check 6: Testing sub-word store on IO vs IO_DIV4 shadow CSRs");

  // 16-bit halfword store to IO_DIV4_MEAS_CTRL_SHADOWED (PERMIT = 4'b0011)
  // succeeds without triggering a store access fault.
  g_store_fault_seen = false;
  uint16_t div4_val = (uint16_t)((100u & 0xffu) | ((140u & 0xffu) << 8));
  shadow_write16(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                 div4_val);
  CHECK(!g_store_fault_seen,
        "Expected 16-bit sh to IO_DIV4_MEAS_CTRL_SHADOWED (PERMIT=4'b0011) to "
        "succeed without store fault");

  // 16-bit halfword store to IO_MEAS_CTRL_SHADOWED (PERMIT = 4'b0111)
  // triggers a synchronous TL-UL bus error -> Store Access Fault (mcause = 7).
  g_store_fault_seen = false;
  g_store_fault_mcause = 0;
  asm volatile("sh %1, 0(%0)"
               :
               : "r"(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET),
                 "r"(div4_val)
               : "memory");
  CHECK(g_store_fault_seen,
        "Expected 16-bit sh to IO_MEAS_CTRL_SHADOWED (PERMIT=4'b0111) to raise "
        "Store Access Fault");
  CHECK(g_store_fault_mcause == 7, "Expected mcause == 7, got %u",
        g_store_fault_mcause);
}

bool test_main(void) {
  dif_clkmgr_t clkmgr;
  dif_pwrmgr_t pwrmgr;
  dif_rstmgr_t rstmgr;
  dif_aon_timer_t aon_timer;

  CHECK_DIF_OK(dif_clkmgr_init_from_dt((dt_clkmgr_t)0, &clkmgr));
  CHECK_DIF_OK(dif_pwrmgr_init_from_dt((dt_pwrmgr_t)0, &pwrmgr));
  CHECK_DIF_OK(dif_rstmgr_init_from_dt((dt_rstmgr_t)0, &rstmgr));
  CHECK_DIF_OK(dif_aon_timer_init_from_dt((dt_aon_timer_t)0, &aon_timer));

  dif_pwrmgr_request_sources_t wakeup_sources;
  CHECK_DIF_OK(dif_pwrmgr_find_request_source(
      &pwrmgr, kDifPwrmgrReqTypeWakeup,
      dt_aon_timer_instance_id((dt_aon_timer_t)0), kDtAonTimerWakeupWkupReq,
      &wakeup_sources));

  dif_clkmgr_measure_clock_t io_clk;
  dif_clkmgr_measure_clock_t main_clk;
  CHECK_DIF_OK(dif_clkmgr_find_measure_clock(&clkmgr, kDtClockIo, &io_clk));
  CHECK_DIF_OK(dif_clkmgr_find_measure_clock(&clkmgr, kDtClockMain, &main_clk));

  if (UNWRAP(pwrmgr_testutils_is_wakeup_reason(&pwrmgr, wakeup_sources))) {
    CHECK_STATUS_OK(aon_timer_testutils_shutdown(&aon_timer));
    run_post_wakeup_checks(&clkmgr, io_clk, main_clk);
    LOG_INFO("CLKMGR errata v2 test passed all checks on CW340 FPGA");
    return true;
  }

  CHECK(
      UNWRAP(rstmgr_testutils_reset_info_any(&rstmgr, kDifRstmgrResetInfoPor)),
      "Expected POR reset on initial entry");

  LOG_INFO("Phase 1: Testing Earlgrey v2 JITTER_ENABLE & JITTER_REGWEN fix");
  bool jitter_locked = true;
  CHECK_DIF_OK(dif_clkmgr_jitter_enable_is_locked(&clkmgr, &jitter_locked));
  CHECK(!jitter_locked, "Expected JITTER_REGWEN=1 after POR");

  CHECK_DIF_OK(dif_clkmgr_jitter_set_enabled(&clkmgr, kDifToggleEnabled));
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected JITTER_ENABLE == 0x6 after enabling");

  // In Earlgrey v2, disabling JITTER_ENABLE while JITTER_REGWEN=1 succeeds!
  // Keep JITTER_ENABLE disabled (0x9) during Check 1 frequency measurements so
  // clock jitter does not widen cycle counts beyond kMeasVariability.
  CHECK_DIF_OK(dif_clkmgr_jitter_set_enabled(&clkmgr, kDifToggleDisabled));
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
            kMultiBitBool4False,
        "Expected JITTER_ENABLE == 0x9 after disabling in v2");

  uint32_t aon_hz = (uint32_t)dt_clock_frequency(kDtClockAon);
  uint32_t exp_io_nom = (uint32_t)dt_clock_frequency(kDtClockIo) / aon_hz;
  uint32_t exp_io_half = exp_io_nom / 2u;
  uint32_t exp_main_nom = (uint32_t)dt_clock_frequency(kDtClockMain) / aon_hz;
  uint32_t exp_main_half = exp_main_nom / 2u;
  uint32_t var = 10u;

  LOG_INFO(
      "Phase 1: Testing asymmetric low-speed external clock division "
      "(io_nom=%u, io_half=%u, main_nom=%u)",
      exp_io_nom, exp_io_half, exp_main_nom);

  // -------------------------------------------------------------------------
  // Check 1: Asymmetric low-speed external clock division (clk_io vs clk_main)
  // -------------------------------------------------------------------------
  CHECK_DIF_OK(dif_clkmgr_external_clock_set_enabled(&clkmgr,
                                                     /*is_low_speed=*/true));
  CHECK_DIF_OK(dif_clkmgr_wait_for_ext_clk_switch(&clkmgr));
  busy_spin_micros(200);

  bool io_low_speed_err =
      probe_meas_window(&clkmgr, io_clk, kDifClkmgrRecovErrTypeIoMeas,
                        exp_io_half - var, exp_io_half + var);
  bool io_nom_err =
      probe_meas_window(&clkmgr, io_clk, kDifClkmgrRecovErrTypeIoMeas,
                        exp_io_nom - var, exp_io_nom + var);
  bool main_half_err =
      probe_meas_window(&clkmgr, main_clk, kDifClkmgrRecovErrTypeMainMeas,
                        exp_main_half - var, exp_main_half + var);
  bool main_nom_err =
      probe_meas_window(&clkmgr, main_clk, kDifClkmgrRecovErrTypeMainMeas,
                        exp_main_nom - var, exp_main_nom + var);

  // Restore internal clock before checking/logging over UART.
  CHECK_DIF_OK(dif_clkmgr_external_clock_set_disabled(&clkmgr));
  busy_spin_micros(200);

  CHECK(!io_low_speed_err,
        "Expected clk_io in low-speed external clock mode to fall inside "
        "halved window (%u+-%u cycles)",
        exp_io_half, var);
  CHECK(io_nom_err,
        "Expected clk_io in low-speed external clock mode to fail nominal "
        "window (%u+-%u cycles)",
        exp_io_nom, var);
  CHECK(main_half_err,
        "Expected clk_main in low-speed external clock mode to fail halved "
        "window (%u+-%u cycles) because u_clk_src_sys_sel has no divide-by-2",
        exp_main_half, var);
  CHECK(!main_nom_err,
        "Expected clk_main in low-speed external clock mode to remain at "
        "nominal window (%u+-%u cycles)",
        exp_main_nom, var);

  // -------------------------------------------------------------------------
  // Check 2: Prepare state before deep sleep to test AON vs calib_rdy reset
  // -------------------------------------------------------------------------
  LOG_INFO("Phase 1: Locking JITTER_REGWEN, MEASURE_CTRL_REGWEN & sleeping");
  CHECK_DIF_OK(dif_clkmgr_jitter_set_enabled(&clkmgr, kDifToggleEnabled));
  CHECK_DIF_OK(dif_clkmgr_lock_jitter_enable(&clkmgr));
  CHECK_DIF_OK(dif_clkmgr_jitter_enable_is_locked(&clkmgr, &jitter_locked));
  CHECK(jitter_locked, "Expected JITTER_REGWEN=0 after locking");
  CHECK(
      dif_clkmgr_jitter_set_enabled(&clkmgr, kDifToggleDisabled) == kDifLocked,
      "Expected dif_clkmgr_jitter_set_enabled to return kDifLocked once "
      "JITTER_REGWEN=0");
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, io_clk, kCustomIoMinPostSleep, kCustomIoMaxPostSleep));
  CHECK_DIF_OK(dif_clkmgr_enable_measure_counts(
      &clkmgr, main_clk, kCustomMainMinPostSleep, kCustomMainMaxPostSleep));

  CHECK_DIF_OK(dif_clkmgr_measure_ctrl_disable(&clkmgr));
  dif_toggle_t meas_ctrl_regwen = kDifToggleEnabled;
  CHECK_DIF_OK(dif_clkmgr_measure_ctrl_get_enable(&clkmgr, &meas_ctrl_regwen));
  CHECK(meas_ctrl_regwen == kDifToggleDisabled,
        "Expected MEASURE_CTRL_REGWEN == 0 before deep sleep");

  CHECK_STATUS_OK(rstmgr_testutils_pre_reset(&rstmgr));
  uint32_t wakeup_ticks = 0;
  CHECK_STATUS_OK(
      aon_timer_testutils_get_aon_cycles_32_from_us(200, &wakeup_ticks));
  CHECK_STATUS_OK(aon_timer_testutils_wakeup_config(&aon_timer, wakeup_ticks));
  CHECK_STATUS_OK(
      pwrmgr_testutils_enable_low_power(&pwrmgr, wakeup_sources, 0));
  wait_for_interrupt();

  return false;
}
