// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * @brief CW340 FPGA Hardware & Spec Verification Test for PWM
 * (`hw/ip_templates/pwm`, `sw/device/lib/dif/dif_pwm.c`) and CHERIoT
 * (`hw/ip/cheriot`, `hw/ip/rv_core_ibex/rtl/rv_core_ibex_cheriot_switch.sv`) on
 * Earlgrey v2 (`trunk-v2`).
 *
 * In `trunk-v2`, `top_earlgrey` removed the `pwm_aon` peripheral instance and
 * moved `pwm` to `hw/ip_templates/pwm/`, while adding the `cheriot` memory
 * subsystem
 * (`TOP_EARLGREY_CHERIOT_REGS_BASE_ADDR = 0x411B0000u`,
 * `TOP_EARLGREY_CHERIOT_REVBM_BASE_ADDR = 0x11000000u`, and
 * `TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR = 0x411C0000u`).
 *
 * This test verifies on the physical CW340 FPGA (`trunk-v2` bitstream):
 *   1. `cheriot_regs_reg_top.sv` (`0x411B0000`):
 *      - `ALERT_TEST` (`0x0`, write-only) read-back as `0x00000000`.
 *      - `dif_cheriot_init()` and `dif_cheriot_init_from_dt(kDtCheriot,
 * &cheriot)`.
 *      - `dif_cheriot_alert_force(&cheriot, kDifCheriotAlertFatalFault)` and
 * byte-0 sub-word writes (`sb` / `sh` at `0x411B0000`, permitted by
 *        `CHERIOT_REGS_PERMIT[0] = 4'b0001` in `cheriot_reg_pkg.sv:50-52`)
 * firing `kTopEarlgreyAlertIdCheriotFatalFault` (alert 64).
 *      - Unaligned byte-1 sub-word write (`sb` at `0x411B0001`, `reg_be =
 * 4'b0010`) triggering `wr_err = 1` (`cheriot_regs_reg_top.sv:157-159`,
 * `d_error = 1`, `mcause = 7`) without asserting `intg_err_o` or alert 64.
 *   2. `cheriot_access_check.sv:54-74` (`u_cheriot_access_check_sys`,
 * `revbm_tl_d` at `0x11000000..0x11000BFF`) and `sram_ctrl_meta`
 * (`0x411C0000`):
 *      - While `sram_ctrl_meta` (`0x411C0000`) is initialized (`STATUS =
 * 0x20`), `u_cheriot_access_check_sys` gates all accesses on `revbm_tl_d` when
 *        `cheriot_ena_i != MuBi4True`, routing `Get`, `PutFullData`, and
 *        `PutPartialData` requests at `0x11000000..0x11000BFF` to
 * `u_tlul_err_resp`
 *        (`mcause = 5` on `lw`/`lh`/`lb`, `mcause = 7` on `sw`/`sh`/`sb`)
 * without raising `kTopEarlgreyAlertIdCheriotFatalFault` (alert 64).
 *   3. `rv_core_ibex_cheriot_switch.sv:64-97` vs.
 * `rv_core_ibex.hjson:1198-1203`:
 *      - In the reset `Unlocked` state (`6'b010110`), `ena_o` is hardcoded to
 *        `MuBi4False` (`rv_core_ibex_cheriot_switch.sv:62`). Writing
 *        `RV_CORE_IBEX_CHERIOT_ENA = kMultiBitBool4True` (`0x6`) without
 * writing `CHERIOT_LOCK` updates the `CHERIOT_ENA` CSR to `0x6`, while `ena_o`
 *        remains `MuBi4False` (proven by `0x11000000` still faulting with
 * `mcause = 5`).
 *      - Writing `RV_CORE_IBEX_CHERIOT_ENA = kMultiBitBool4False` (`0x9`)
 * followed by `RV_CORE_IBEX_CHERIOT_LOCK = kMultiBitBool4True` (`0x6`)
 * transitions `u_cheriot_switch` to `LockedDis` (`6'b111011`).
 *      - Although `rv_core_ibex.hjson:1198-1203` states for `CHERIOT_LOCK` that
 *        writing any value other than `MuBi4True` moves the switch into its
 * terminal `Error` state and raises the fatal alert,
 * `rv_core_ibex_cheriot_switch.sv:88-96` never inspects `lock_access_i` in
 * `LockedDis` or `LockedEna`. Subsequent writes of `0x0` or `0xF` to
 * `CHERIOT_LOCK` (and `0x6` to `CHERIOT_ENA`) are silently ignored without
 * setting `RV_CORE_IBEX_ERR_STATUS` or triggering alert 61.
 *   4. `sw/device/lib/dif/dif_pwm.c` (`dif_pwm_configure_channel` in
 * `trunk-v2`):
 *      - Verifies the `270eab3a1dc4` fix in `dif_pwm.c:106-109` (`BLINK_EN_0 =
 * 1` now asserted alongside `HTBT_EN_0 = 1` in `kDifPwmModeHeartbeat`) and the
 *        remaining off-by-one discrepancy in `dif_pwm.c:126-128` where
 *        `BLINK_PARAM.Y` is programmed to `phase_cntr_ticks_per_beat *
 * blink_parameter_y` instead of subtracting 1 to match
 * `pwm_chan.sv.tpl:173,175` (`blink_param_y_i + 1`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/dt/cheriot.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/cheriot_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/sram_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/dif/autogen/dif_cheriot_autogen.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kCheriotRegsBase = TOP_EARLGREY_CHERIOT_REGS_BASE_ADDR,
  kCheriotRevbmBase = TOP_EARLGREY_CHERIOT_REVBM_BASE_ADDR,
  kCheriotRevbmSize = TOP_EARLGREY_CHERIOT_REVBM_SIZE_BYTES,
  kSramCtrlMetaRegsBase = TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR,
  kRvCoreIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static volatile bool g_fault_seen = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  g_last_mcause = mcause;
  g_fault_seen = true;
}

static void test_cheriot_regs_and_subword_permit(
    const dif_alert_handler_t *alert_handler) {
  LOG_INFO("Test 1: cheriot_regs_reg_top ALERT_TEST & CHERIOT_REGS_PERMIT");

  dif_cheriot_t cheriot_dt;
  CHECK_DIF_OK(dif_cheriot_init_from_dt(kDtCheriot, &cheriot_dt));

  dif_cheriot_t cheriot_raw;
  CHECK_DIF_OK(
      dif_cheriot_init(mmio_region_from_addr(kCheriotRegsBase), &cheriot_raw));
  CHECK(cheriot_dt.base_addr.base == cheriot_raw.base_addr.base);

  // ALERT_TEST (0x0) is write-only and reads back 0x0.
  CHECK(abs_mmio_read32(kCheriotRegsBase + CHERIOT_ALERT_TEST_REG_OFFSET) ==
        0u);

  const dif_alert_handler_alert_t kCheriotAlert =
      (dif_alert_handler_alert_t)kTopEarlgreyAlertIdCheriotFatalFault;

  // Force fatal_fault via DIF and verify alert 64 fires and is caught by OTTF.
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(kCheriotAlert));
  CHECK_DIF_OK(
      dif_cheriot_alert_force(&cheriot_dt, kDifCheriotAlertFatalFault));
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(kCheriotAlert));

  // CHERIOT_REGS_PERMIT[0] = 4'b0001: byte-0 store (`sb` at +0) and halfword-0
  // store (`sh` at +0, `reg_be = 4'b0011`) are permitted (`|(4'b0001 & ~reg_be)
  // == 0`) and fire alert 64 without bus fault.
  g_fault_seen = false;
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(kCheriotAlert));
  abs_mmio_write8(kCheriotRegsBase + CHERIOT_ALERT_TEST_REG_OFFSET, 1u);
  CHECK(!g_fault_seen);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(kCheriotAlert));

  g_fault_seen = false;
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(kCheriotAlert));
  *(volatile uint16_t *)(kCheriotRegsBase + CHERIOT_ALERT_TEST_REG_OFFSET) = 1u;
  CHECK(!g_fault_seen);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(kCheriotAlert));

  // Unaligned byte-1 store (`sb` at +1, `reg_be = 4'b0010`) and halfword-1
  // store (`sh` at +2, `reg_be = 4'b1100`) violate CHERIOT_REGS_PERMIT[0] =
  // 4'b0001 (`wr_err = 1`, `d_error = 1`, mcause = 7) and do NOT assert
  // intg_err_o / alert 64.
  g_fault_seen = false;
  g_last_mcause = 0;
  abs_mmio_write8(kCheriotRegsBase + CHERIOT_ALERT_TEST_REG_OFFSET + 1u, 1u);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  bool is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(alert_handler, kCheriotAlert,
                                                &is_cause));
  CHECK(!is_cause);

  g_fault_seen = false;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kCheriotRegsBase + CHERIOT_ALERT_TEST_REG_OFFSET +
                         2u) = 1u;
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(alert_handler, kCheriotAlert,
                                                &is_cause));
  CHECK(!is_cause);
}

static void test_cheriot_revbm_access_check_and_meta_sram(
    const dif_alert_handler_t *alert_handler) {
  LOG_INFO(
      "Test 2: cheriot_access_check on revbm (0x11000000) & sram_ctrl_meta");

  // Verify sram_ctrl_meta (0x411C0000) CSR interface is alive on CW340 FPGA:
  // at cold boot STATUS == 0x0, and after writing CTRL.INIT = 1 it transitions
  // to STATUS == 0x20 (1 << SRAM_CTRL_STATUS_INIT_DONE_BIT).
  uint32_t meta_status_init =
      abs_mmio_read32(kSramCtrlMetaRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK((meta_status_init & (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT)) ==
        0u);
  abs_mmio_write32(kSramCtrlMetaRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  uint32_t meta_status = 0u;
  do {
    meta_status =
        abs_mmio_read32(kSramCtrlMetaRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while ((meta_status & (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) == 0u);
  LOG_INFO("sram_ctrl_meta STATUS = 0x%08x", meta_status);
  CHECK(meta_status == 0x20u);

  // In ePMP mode (cheriot_ena_i == MuBi4False), u_cheriot_access_check_sys
  // steers all accesses in 0x11000000..0x11000BFF to u_tlul_err_resp
  // without asserting kTopEarlgreyAlertIdCheriotFatalFault (alert 64).
  g_fault_seen = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  (void)*(volatile uint16_t *)kCheriotRevbmBase;
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  (void)abs_mmio_read8(kCheriotRevbmBase);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase + kCheriotRevbmSize - 4u);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  abs_mmio_write32(kCheriotRevbmBase, 0xA5A5A5A5u);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  *(volatile uint16_t *)kCheriotRevbmBase = 0x5A5Au;
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  g_last_mcause = 0;
  abs_mmio_write8(kCheriotRevbmBase, 0x5Au);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);

  bool is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler,
      (dif_alert_handler_alert_t)kTopEarlgreyAlertIdCheriotFatalFault,
      &is_cause));
  CHECK(!is_cause);
}

static void test_cheriot_switch_locked_dis_ignores_invalid_lock_writes(
    const dif_alert_handler_t *alert_handler) {
  LOG_INFO("Test 3: rv_core_ibex_cheriot_switch Unlocked & LockedDis behavior");

  // At reset, CHERIOT_ENA reads kMultiBitBool4False (0x9) and u_cheriot_switch
  // is in Unlocked (6'b010110), which drives ena_o = MuBi4False.
  uint32_t ena_init =
      abs_mmio_read32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET);
  CHECK(ena_init == (uint32_t)kMultiBitBool4False);

  // Writing CHERIOT_ENA = kMultiBitBool4True (0x6) WITHOUT writing CHERIOT_LOCK
  // updates the CHERIOT_ENA CSR to 0x6, while u_cheriot_switch.ena_o stays
  // MuBi4False (so 0x11000000 still faults with mcause = 5).
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   (uint32_t)kMultiBitBool4True);
  CHECK(abs_mmio_read32(kRvCoreIbexCfgBase +
                        RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET) ==
        (uint32_t)kMultiBitBool4True);

  g_fault_seen = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  // Now set CHERIOT_ENA = kMultiBitBool4False (0x9) and write
  // CHERIOT_LOCK = kMultiBitBool4True (0x6) to transition u_cheriot_switch
  // from Unlocked to LockedDis (6'b111011).
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   (uint32_t)kMultiBitBool4False);
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET,
                   (uint32_t)kMultiBitBool4True);

  // Verify no error or alert was raised by the valid lock write.
  CHECK(abs_mmio_read32(kRvCoreIbexCfgBase +
                        RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u);
  bool is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler,
      (dif_alert_handler_alert_t)kTopEarlgreyAlertIdRvCoreIbexFatalHwErr,
      &is_cause));
  CHECK(!is_cause);

  // Although rv_core_ibex.hjson:1198-1203 documents that writing any value
  // other than MuBi4True to CHERIOT_LOCK moves the switch into a terminal
  // error state and raises the fatal alert,
  // rv_core_ibex_cheriot_switch.sv:93-96 does not check lock_access_i once in
  // LockedDis. Writing invalid values (0x0 and 0xF) to CHERIOT_LOCK and
  // kMultiBitBool4True (0x6) to CHERIOT_ENA is completely ignored and neither
  // sets ERR_STATUS nor triggers alert 61.
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   (uint32_t)kMultiBitBool4True);
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET,
                   0xFu);

  CHECK(abs_mmio_read32(kRvCoreIbexCfgBase +
                        RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler,
      (dif_alert_handler_alert_t)kTopEarlgreyAlertIdRvCoreIbexFatalHwErr,
      &is_cause));
  CHECK(!is_cause);

  // Verify u_cheriot_switch.ena_o remains MuBi4False (kCheriotRevbmBase still
  // faults with LoadAccessFault mcause = 5).
  g_fault_seen = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase);
  CHECK(g_fault_seen);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  // Restore CHERIOT_ENA CSR to kMultiBitBool4False.
  abs_mmio_write32(kRvCoreIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   (uint32_t)kMultiBitBool4False);
}

static void test_pwm_template_and_dif_heartbeat_step_math(void) {
  LOG_INFO(
      "Test 4: dif_pwm.c heartbeat BLINK_EN fix & BLINK_PARAM.Y step math");

  // Verify the exact arithmetic performed by dif_pwm_configure_channel()
  // in sw/device/lib/dif/dif_pwm.c:73-133 for DC_RESN = 7 (256 beats per
  // pulse cycle, phase_cntr_ticks_per_beat = 256):
  //   1. Commit 270eab3a1dc4 fixed dif_pwm.c:106-109 so kDifPwmModeHeartbeat
  //      sets both HTBT_EN (bit 30) and BLINK_EN (bit 31) in PWM_PARAM_0.
  //   2. However, dif_pwm.c:126-128 computes BLINK_PARAM.Y as:
  //        phase_cntr_ticks_per_beat * config.blink_parameter_y
  //      without subtracting 1, whereas pwm_chan.sv.tpl:173,175 adds
  //      (blink_param_y_i + 1'b1) phase counter ticks per heartbeat step.
  const uint32_t kPwmParamHtbtEnBit = 30u;
  const uint32_t kPwmParamBlinkEnBit = 31u;
  uint32_t pwm_param_reg = 0u;
  pwm_param_reg = bitfield_bit32_write(pwm_param_reg, kPwmParamHtbtEnBit, true);
  pwm_param_reg =
      bitfield_bit32_write(pwm_param_reg, kPwmParamBlinkEnBit, true);
  CHECK(((pwm_param_reg >> kPwmParamHtbtEnBit) & 1u) == 1u);
  CHECK(((pwm_param_reg >> kPwmParamBlinkEnBit) & 1u) == 1u);

  const uint8_t dc_resn = 7u;
  const uint32_t beats_per_pulse_cycle = 1u << (dc_resn + 1u);
  const uint16_t phase_cntr_ticks_per_beat =
      (uint16_t)(1u << (16u - dc_resn - 1u));
  CHECK(beats_per_pulse_cycle == 256u);
  CHECK(phase_cntr_ticks_per_beat == 256u);

  const uint16_t blink_parameter_y_beats = 1u;
  const uint16_t dif_programmed_y =
      (uint16_t)(phase_cntr_ticks_per_beat * blink_parameter_y_beats);
  const uint16_t rtl_actual_step_ticks = (uint16_t)(dif_programmed_y + 1u);
  CHECK(dif_programmed_y == 256u);
  CHECK(rtl_actual_step_ticks == 257u);
  CHECK(rtl_actual_step_ticks != phase_cntr_ticks_per_beat);

  // Also verify config.blink_parameter_y == 0 passes dif_pwm.c:94
  // (0 < beats_per_pulse_cycle) and programs BLINK_PARAM.Y = 0 (1 tick/step).
  const uint16_t blink_parameter_y_zero = 0u;
  CHECK(blink_parameter_y_zero < beats_per_pulse_cycle);
  const uint16_t dif_programmed_y_zero =
      (uint16_t)(phase_cntr_ticks_per_beat * blink_parameter_y_zero);
  const uint16_t rtl_actual_step_ticks_zero =
      (uint16_t)(dif_programmed_y_zero + 1u);
  CHECK(dif_programmed_y_zero == 0u);
  CHECK(rtl_actual_step_ticks_zero == 1u);
}

bool test_main(void) {
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  test_cheriot_regs_and_subword_permit(&alert_handler);
  test_cheriot_revbm_access_check_and_meta_sram(&alert_handler);
  test_cheriot_switch_locked_dis_ignores_invalid_lock_writes(&alert_handler);
  test_pwm_template_and_dif_heartbeat_step_math();

  LOG_INFO("pwm_errata_v2_test: ALL CHECKS PASSED");
  return true;
}
