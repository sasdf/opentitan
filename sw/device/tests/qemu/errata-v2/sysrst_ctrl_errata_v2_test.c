// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sysrst_ctrl_errata_v2_test.c
 * @brief CW340 FPGA Errata Verification & Discovery Test for Earlgrey v2
 * `sysrst_ctrl` (`trunk-v2`).
 *
 * Empirically verifies on physical CW340 FPGA silicon (`trunk-v2`):
 *
 * 1. `[sysrst_ctrl.sv:106-111, sysrst_ctrl_pin.sv:50-77,
 *    sysrst_ctrl_detect.sv:65-76, sysrst_ctrl_combo.sv:75-120]`
 *    (`SPEC_DOC_ERRATA`):
 *    - `PIN_IN_VALUE` (`0x40`) samples raw uninverted `cio_*_in_i` inputs,
 *      remaining unchanged when `KEY_INVERT_CTL` (`0x30`) is toggled.
 *    - Software writes to `KEY_INVERT_CTL` (`0x30`) while `KEY_INTR_CTL`
 *      (`0x44`) is enabled synthesize hardware input transitions in
 *      `sysrst_ctrl_detect.sv:65-76` (`trigger_active & ~trigger_active_q`) and
 *      assert `KEY_INTR_STATUS` (`0xa8`).
 *    - Programming `COM_SEL_CTL_0 != 0` with `COM_DET_CTL_0 = 0` when the
 *      selected input is already active immediately triggers
 *      `COMBO_INTR_STATUS` (`0xa4`) on the next AON cycle.
 * 2. `[sysrst_ctrl_ulp.sv:68-95, sysrst_ctrl_detect.sv:180-211]`
 *    (`SPEC_DOC_ERRATA`):
 *    - `ULP_STATUS` (`0x28`, `rw1c`) is driven by the single-cycle
 *      `event_detected_pulse_o` on `DetectSt -> StableSt`, while `Sticky = 1`
 *      parks the FSM in `StableSt` (`event_detected_o = 1`).
 *    - Clearing `ULP_STATUS` via `rw1c` while `ULP_CTL == 1` keeps
 *      `ULP_STATUS == 0` even while `ac_present_int == 1`, and blocks any
 *      subsequent ULP trigger on new `ac_present` transitions until `ULP_CTL`
 *      is cycled `1 -> 0 -> 1`.
 * 3. `[sysrst_ctrl.hjson:155-165, sysrst_ctrl_pin.sv:98-99,142-145]`
 *    (`SPEC_DOC_ERRATA` — v2 Discovery):
 *    - `PIN_ALLOWED_CTL` (`0x34`) resets to `0x00000082` (`EC_RST_L_0 = 1`,
 *      `FLASH_WP_L_0 = 1`; `EC_RST_L_1 = 0`, `FLASH_WP_L_1 = 0`), contrary to
 *      `sysrst_ctrl.hjson:156` / `theory_of_operation.md:154`.
 *    - While setting `PIN_OUT_CTL.EC_RST_L = 0` releases `ec_rst_l_o` high
 *      (`PIN_IN_VALUE.EC_RST_L == 1`) via `inputs[1] = aon_ec_rst_l_hw_i = 1`,
 *      setting `PIN_OUT_CTL.FLASH_WP_L = 0` as instructed by
 *      `sysrst_ctrl.hjson:164` leaves `flash_wp_l_o` stuck low
 *      (`PIN_IN_VALUE.FLASH_WP_L == 0`) because `sysrst_ctrl_pin.sv:99`
 *      hardwires `inputs[0] = 1'b0`. Releasing `flash_wp_l_o` high
 *      (`PIN_IN_VALUE.FLASH_WP_L == 1`) requires setting all three of
 *      `PIN_ALLOWED_CTL.FLASH_WP_L_1 = 1`, `PIN_OUT_CTL.FLASH_WP_L = 1`, and
 *      `PIN_OUT_VALUE.FLASH_WP_L = 1`.
 * 4. `[dif_sysrst_ctrl.c:127-135, 782-804, sysrst_ctrl_intr.sv:41-49]`
 *    (`DIF_API_BUG` — v2 Discovery):
 *    - `dif_sysrst_ctrl_ulp_wakeup_get_status()` and
 *      `dif_sysrst_ctrl_ulp_wakeup_clear_status()` access
 *      `SYSRST_CTRL_WKUP_STATUS_REG_OFFSET` (`0x2c`) instead of
 *      `SYSRST_CTRL_ULP_STATUS_REG_OFFSET` (`0x28`), causing non-ULP key/combo
 *      events to report false-positive ULP wakeups (`wakeup_detected == true`
 *      when `ULP_STATUS == 0`) and leaving `ULP_STATUS == 1` un-cleared after a
 *      real ULP wakeup.
 *    - `dif_sysrst_ctrl_output_pin_override_configure()` enforces
 *      `(override_value && !allow_one) || (!override_value && !allow_zero)`
 *      even when `config.enabled == kDifToggleDisabled`, rejecting the hardware
 *      reset configuration (`enabled = kDifToggleDisabled, allow_zero = false,
 *      allow_one = false`) with `kDifBadArg`.
 * 5. `[sysrst_ctrl_reg_pkg.sv:522-566]` (`INTENDED_SECURITY_HARDENING`):
 *    - Heterogeneous 1/2/3/4-byte `SYSRST_CTRL_PERMIT[43]` masks (`4'b0001`,
 *      `4'b0011`, `4'b0111`, `4'b1111`) raise synchronous Store Access Faults
 *      (`mcause = 7`) on narrower sub-word stores, while `PIN_OUT_CTL` (`0x38`)
 *      and `PIN_OUT_VALUE` (`0x3c`) remain writable after `REGWEN` (`0x10`) is
 *      locked to `0`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_base.h"
#include "sw/device/lib/dif/dif_sysrst_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/sysrst_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
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

bool test_main(void) {
  LOG_INFO(
      "Starting SYSRST_CTRL Earlgrey v2 CW340 FPGA Errata Test "
      "(trunk-v2)...");

  dif_sysrst_ctrl_t sysrst_ctrl;
  CHECK_DIF_OK(
      dif_sysrst_ctrl_init(mmio_region_from_addr(kSysrstBase), &sysrst_ctrl));

  // =========================================================================
  // Check 1: [sysrst_ctrl.hjson:155-165, sysrst_ctrl_pin.sv:98-99,142-145]
  // (`SPEC_DOC_ERRATA` — v2 Discovery)
  // Verify power-on reset values of `PIN_ALLOWED_CTL` (`0x82`), `PIN_OUT_CTL`
  // (`0x82`), and `PIN_OUT_VALUE` (`0x00`), and compare releasing `ec_rst_l`
  // vs `flash_wp_l`:
  // - Setting `PIN_OUT_CTL.EC_RST_L = 0` releases `ec_rst_l_o` high
  //   (`PIN_IN_VALUE.EC_RST_L == 1`) because `inputs[1] = aon_ec_rst_l_hw_i =
  //   1`.
  // - Setting `PIN_OUT_CTL.FLASH_WP_L = 0` (as claimed by
  // `sysrst_ctrl.hjson:164`)
  //   leaves `flash_wp_l_o` stuck low (`PIN_IN_VALUE.FLASH_WP_L == 0`) because
  //   `sysrst_ctrl_pin.sv:99` hardwires `inputs[0] = 1'b0`.
  // - Setting `PIN_OUT_CTL.FLASH_WP_L = 1` and `PIN_OUT_VALUE.FLASH_WP_L = 1`
  //   without setting `PIN_ALLOWED_CTL.FLASH_WP_L_1 = 1` ALSO leaves
  //   `flash_wp_l_o` low (`PIN_IN_VALUE.FLASH_WP_L == 0`) because
  //   `PIN_ALLOWED_CTL.FLASH_WP_L_1` (bit 15) resets to `0`.
  // - Only when `PIN_ALLOWED_CTL.FLASH_WP_L_1 = 1`, `PIN_OUT_CTL.FLASH_WP_L =
  // 1`,
  //   and `PIN_OUT_VALUE.FLASH_WP_L = 1` are all set does `flash_wp_l_o`
  //   release high (`PIN_IN_VALUE.FLASH_WP_L == 1`).
  // =========================================================================
  uint32_t pin_allowed_init =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET);
  uint32_t pin_out_ctl_init =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET);
  uint32_t pin_out_val_init =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET);
  LOG_INFO(
      "Reset CSRs: PIN_ALLOWED_CTL=0x%08x PIN_OUT_CTL=0x%08x "
      "PIN_OUT_VALUE=0x%08x",
      pin_allowed_init, pin_out_ctl_init, pin_out_val_init);
  CHECK(pin_allowed_init == 0x82u,
        "[sysrst_ctrl.hjson:396-470] Expected PIN_ALLOWED_CTL reset value "
        "0x82 (EC_RST_L_0=1, FLASH_WP_L_0=1; EC_RST_L_1=0, FLASH_WP_L_1=0)");
  CHECK((pin_allowed_init &
         (1u << SYSRST_CTRL_PIN_ALLOWED_CTL_EC_RST_L_1_BIT)) == 0u,
        "[sysrst_ctrl.hjson:156] Expected EC_RST_L_1 to reset to 0");

  // Ensure initial default override holds both ec_rst_l and flash_wp_l low (0).
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0x82u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x00u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x82u);
  busy_spin_micros(60);
  uint32_t pin_in_reset =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in_reset >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 0u,
        "Expected PIN_IN_VALUE.EC_RST_L == 0 under default low override");
  CHECK(((pin_in_reset >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 0u,
        "Expected PIN_IN_VALUE.FLASH_WP_L == 0 under default low override");

  // Clear PIN_OUT_CTL (`0x00`) to disable override on both EC_RST_L and
  // FLASH_WP_L:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x00u);
  busy_spin_micros(60);
  uint32_t pin_in_no_ovr =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in_no_ovr >> SYSRST_CTRL_PIN_IN_VALUE_EC_RST_L_BIT) & 1u) == 1u,
        "[sysrst_ctrl_pin.sv:98] Expected EC_RST_L to release high (1) when "
        "PIN_OUT_CTL.EC_RST_L=0");
  CHECK(((pin_in_no_ovr >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 0u,
        "[sysrst_ctrl_pin.sv:99] Expected FLASH_WP_L to remain stuck low (0) "
        "when PIN_OUT_CTL.FLASH_WP_L=0 due to hardwired inputs[0]=1'b0");

  // Attempt to drive FLASH_WP_L high with PIN_OUT_CTL.FLASH_WP_L=1 and
  // PIN_OUT_VALUE.FLASH_WP_L=1 while PIN_ALLOWED_CTL is still at reset (0x82):
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET,
                   1u << SYSRST_CTRL_PIN_OUT_VALUE_FLASH_WP_L_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET,
                   1u << SYSRST_CTRL_PIN_OUT_CTL_FLASH_WP_L_BIT);
  busy_spin_micros(60);
  uint32_t pin_in_disallowed =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in_disallowed >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) ==
            0u,
        "[sysrst_ctrl_pin.sv:143-144] Expected FLASH_WP_L to remain 0 when "
        "PIN_ALLOWED_CTL.FLASH_WP_L_1==0");

  // Now enable PIN_ALLOWED_CTL.FLASH_WP_L_1 (bit 15) and verify FLASH_WP_L
  // rises to 1:
  abs_mmio_write32(
      kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET,
      0x82u | (1u << SYSRST_CTRL_PIN_ALLOWED_CTL_FLASH_WP_L_1_BIT));
  busy_spin_micros(60);
  uint32_t pin_in_allowed =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(
      ((pin_in_allowed >> SYSRST_CTRL_PIN_IN_VALUE_FLASH_WP_L_BIT) & 1u) == 1u,
      "[sysrst_ctrl_pin.sv:143-144] Expected FLASH_WP_L to release high (1) "
      "once PIN_ALLOWED_CTL.FLASH_WP_L_1=1, PIN_OUT_CTL.FLASH_WP_L=1, and "
      "PIN_OUT_VALUE.FLASH_WP_L=1");

  // Restore PIN_ALLOWED_CTL, PIN_OUT_CTL, PIN_OUT_VALUE to clean state.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0x82u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x00u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x82u);
  busy_spin_micros(60);

  // =========================================================================
  // Check 2: [dif_sysrst_ctrl.c:127-135, 782-804] (`DIF_API_BUG` — v2
  // Discovery) (a) `dif_sysrst_ctrl_output_pin_override_configure()`
  // unconditionally checks
  //     `(override_value && !allow_one) || (!override_value && !allow_zero)`
  //     even when `config.enabled == kDifToggleDisabled`, rejecting the
  //     hardware reset state (`enabled = kDifToggleDisabled, allow_zero =
  //     false, allow_one = false`) with `kDifBadArg`.
  // (b) `dif_sysrst_ctrl_ulp_wakeup_get_status()` and
  //     `dif_sysrst_ctrl_ulp_wakeup_clear_status()` read/write `WKUP_STATUS`
  //     (`0x2c`) instead of `ULP_STATUS` (`0x28`).
  // =========================================================================
  LOG_INFO(
      "Verifying [dif_sysrst_ctrl.c:127-135] (DIF_API_BUG): "
      "dif_sysrst_ctrl_output_pin_override_configure rejecting "
      "kDifToggleDisabled with allow_zero=false, allow_one=false...");
  dif_sysrst_ctrl_pin_config_t disable_cfg_f = {
      .enabled = kDifToggleDisabled,
      .allow_zero = false,
      .allow_one = false,
      .override_value = false,
  };
  dif_sysrst_ctrl_pin_config_t disable_cfg_t = {
      .enabled = kDifToggleDisabled,
      .allow_zero = false,
      .allow_one = false,
      .override_value = true,
  };
  CHECK(
      dif_sysrst_ctrl_output_pin_override_configure(
          &sysrst_ctrl, kDifSysrstCtrlPinKey0Out, disable_cfg_f) == kDifBadArg,
      "[dif_sysrst_ctrl.c:131-132] Expected override_configure to reject "
      "kDifToggleDisabled when allow_zero=false, allow_one=false, "
      "override_value=false");
  CHECK(
      dif_sysrst_ctrl_output_pin_override_configure(
          &sysrst_ctrl, kDifSysrstCtrlPinKey0Out, disable_cfg_t) == kDifBadArg,
      "[dif_sysrst_ctrl.c:131-132] Expected override_configure to reject "
      "kDifToggleDisabled when allow_zero=false, allow_one=false, "
      "override_value=true");

  // =========================================================================
  // Check 3: [sysrst_ctrl.sv:106-111] (`SPEC_DOC_ERRATA`) &
  //          [dif_sysrst_ctrl.c:782-804] (`DIF_API_BUG` false-positive ULP
  //          wakeup on non-ULP key edge)
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl.sv:106-111] & [dif_sysrst_ctrl.c:782-804]: "
      "Raw PIN_IN_VALUE vs KEY_INVERT_CTL, software-synthesized "
      "KEY_INTR_STATUS edges, false-positive "
      "dif_sysrst_ctrl_ulp_wakeup_get_status, "
      "and immediate COM_DET_CTL_0=0 combo trigger...");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(60);

  uint32_t pin_in_initial =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  uint32_t raw_ac =
      (pin_in_initial >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u;
  uint32_t raw_key0 =
      (pin_in_initial >> SYSRST_CTRL_PIN_IN_VALUE_KEY0_IN_BIT) & 1u;

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET,
                   (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_H2L_BIT) |
                       (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_L2H_BIT));
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET, 1u);

  // Invert key0_in via KEY_INVERT_CTL while ULP_CTL == 0:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  busy_spin_micros(60);

  uint32_t pin_in_after_invert =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(((pin_in_after_invert >> SYSRST_CTRL_PIN_IN_VALUE_KEY0_IN_BIT) & 1u) ==
            raw_key0,
        "[sysrst_ctrl_pin.sv:50-77] Expected PIN_IN_VALUE.key0_in to remain "
        "uninverted");

  uint32_t key_intr_status =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET);
  uint32_t expected_edge_bit =
      raw_key0 ? (1u << SYSRST_CTRL_KEY_INTR_STATUS_KEY0_IN_H2L_BIT)
               : (1u << SYSRST_CTRL_KEY_INTR_STATUS_KEY0_IN_L2H_BIT);
  CHECK((key_intr_status & expected_edge_bit) != 0u,
        "[sysrst_ctrl.sv:106-111] Expected software write to KEY_INVERT_CTL to "
        "synthesize KEY_INTR_STATUS edge");

  // Verify [dif_sysrst_ctrl.c:788-789]: even though ULP_CTL == 0 and
  // ULP_STATUS == 0, `dif_sysrst_ctrl_ulp_wakeup_get_status()` falsely reports
  // `ulp_detected == true` because the key edge latched `WKUP_STATUS` (`0x2c`)!
  bool ulp_detected = false;
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u,
        "Expected hardware ULP_STATUS == 0 when ULP_CTL == 0");
  CHECK_DIF_OK(
      dif_sysrst_ctrl_ulp_wakeup_get_status(&sysrst_ctrl, &ulp_detected));
  CHECK(
      ulp_detected == true,
      "[dif_sysrst_ctrl.c:788] Expected dif_sysrst_ctrl_ulp_wakeup_get_status "
      "to falsely return true on non-ULP key wakeup due to reading "
      "WKUP_STATUS");

  // Verify `sysrst_ctrl_combo.sv:111-120`:
  // Because `assign trigger = (in & cfg_in_sel) == '0` is `1'b1` when
  // `COM_SEL_CTL_0 == 0` (`cfg_in_sel == '0`), `trigger_active_q` in
  // `u_sysrst_ctrl_detect` (`EventType = EdgeToHigh`) is already `1'b1`.
  // Therefore, enabling `COM_SEL_CTL_0 = 1` while `key0_int == 0` keeps
  // `trigger` at `1'b1 -> 1'b1` (`COMBO_INTR_STATUS == 0`), whereas
  // transitioning `key0_int` `1 -> 0` via `KEY_INVERT_CTL` synthesizes a
  // `0 -> 1` edge on `trigger` and immediately sets `COMBO_INTR_STATUS = 1`.
  uint32_t inv_key0_low =
      raw_key0 ? (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT) : 0u;
  uint32_t inv_key0_high =
      inv_key0_low ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_low);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  busy_spin_micros(60);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET,
                   1u << SYSRST_CTRL_COM_OUT_CTL_0_INTERRUPT_0_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET,
                   1u << SYSRST_CTRL_COM_SEL_CTL_0_KEY0_IN_SEL_0_BIT);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase +
                        SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) == 0u,
        "[sysrst_ctrl_combo.sv:111-120] Expected COMBO_INTR_STATUS == 0 when "
        "COM_SEL_CTL_0=1 is written while key0_int==0 (trigger stays 1->1)");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_low);
  busy_spin_micros(60);
  CHECK(
      (abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) &
       1u) == 1u,
      "[sysrst_ctrl_combo.sv:111-120] Expected COMBO_INTR_STATUS bit 0 == 1 "
      "after KEY_INVERT_CTL synthesizes 1->0 key0_int edge");

  // Clean up combo & key interrupt state.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(60);

  // =========================================================================
  // Check 4: [sysrst_ctrl_ulp.sv:68-95] (`SPEC_DOC_ERRATA`) &
  //          [dif_sysrst_ctrl.c:794-804] (`DIF_API_BUG`)
  // - Enabling `ULP_CTL = 1` with `ac_present_int == 1` sets `ULP_STATUS = 1`
  //   and `WKUP_STATUS = 1`.
  // - Calling `dif_sysrst_ctrl_ulp_wakeup_clear_status()` clears `WKUP_STATUS`
  //   (`0x2c`) to `0`, leaving `ULP_STATUS` (`0x28`) stuck at `1`!
  // - Clearing `ULP_STATUS` (`0x28`) via `rw1c` while `ULP_CTL == 1` keeps
  //   `ULP_STATUS == 0`, yet `Sticky = 1` blocks any new ULP pulse until
  //   `ULP_CTL` is cycled `1 -> 0 -> 1`.
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl_ulp.sv:68-95] & [dif_sysrst_ctrl.c:794-804]: "
      "dif_sysrst_ctrl_ulp_wakeup_clear_status leaving ULP_STATUS=1, and "
      "ULP_STATUS rw1c vs Sticky=1 StableSt park...");
  uint32_t inv_ac_high =
      raw_ac ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_AC_PRESENT_BIT);
  uint32_t inv_ac_low =
      inv_ac_high ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_AC_PRESENT_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 1u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected ULP_STATUS == 1 on initial ULP "
        "detection");
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) == 1u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected WKUP_STATUS == 1 on initial ULP "
        "detection");

  // Call DIF `dif_sysrst_ctrl_ulp_wakeup_clear_status()` and verify it clears
  // `WKUP_STATUS` (`0x2c`) while leaving `ULP_STATUS` (`0x28`) stuck at `1`:
  CHECK_DIF_OK(dif_sysrst_ctrl_ulp_wakeup_clear_status(&sysrst_ctrl));
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) == 0u,
        "Expected WKUP_STATUS == 0 after "
        "dif_sysrst_ctrl_ulp_wakeup_clear_status");
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 1u,
        "[dif_sysrst_ctrl.c:800] Expected ULP_STATUS to remain stuck at 1 "
        "after dif_sysrst_ctrl_ulp_wakeup_clear_status");

  // Now clear ULP_STATUS (`rw1c`) directly without clearing ULP_CTL:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected ULP_STATUS to stay 0 after rw1c "
        "even while ULP_CTL==1 and ac_present_int==1");

  // Toggle ac_present_int 1 -> 0 -> 1 while ULP_CTL is still 1: `Sticky = 1`
  // keeps ULP FSM parked in `StableSt`, so `ULP_STATUS` stays 0!
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_low);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_high);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected Sticky=1 StableSt to block new "
        "ULP_STATUS pulse until ULP_CTL is cleared to 0");

  // Cycle ULP_CTL 1 -> 0 -> 1 and verify ULP_STATUS fires again:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 1u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected ULP_STATUS == 1 after cycling "
        "ULP_CTL 1 -> 0 -> 1");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);

  // =========================================================================
  // Check 5: [sysrst_ctrl_reg_pkg.sv:522-566] (`INTENDED_SECURITY_HARDENING`)
  // Heterogeneous 1/2/3/4-byte `SYSRST_CTRL_PERMIT[43]` byte-enable masks &
  // `PIN_OUT_CTL` / `PIN_OUT_VALUE` ungated by `REGWEN = 0`.
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl_reg_pkg.sv:522-566] "
      "(INTENDED_SECURITY_HARDENING): "
      "Heterogeneous SYSRST_CTRL_PERMIT (4'b0001/0011/0111/1111) sub-word "
      "write faults (mcause=7) & PIN_OUT_CTL/PIN_OUT_VALUE ungated by "
      "REGWEN...");
  g_fault_count = 0;
  *(volatile uint16_t *)(kSysrstBase +
                         SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET) = 0x0010u;
  CHECK(g_fault_count == 0u,
        "[sysrst_ctrl_reg_pkg.sv:522-566] Expected 16-bit sh to "
        "ULP_AC_DEBOUNCE_CTL (permit 4'b0011) to succeed");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET,
                  0x20u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:522-566] Expected 8-bit sb to "
        "ULP_AC_DEBOUNCE_CTL (permit 4'b0011) to trap with mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kSysrstBase +
                         SYSRST_CTRL_AUTO_BLOCK_DEBOUNCE_CTL_REG_OFFSET) =
      0x0010u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:522-566] Expected 16-bit sh to "
        "AUTO_BLOCK_DEBOUNCE_CTL (permit 4'b0111) to trap with mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET) =
      0x0010u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:522-566] Expected 16-bit sh to COM_DET_CTL_0 "
        "(permit 4'b1111) to trap with mcause=7");

  // Clear REGWEN (`0x10`, rw0c) to 0 and verify `PIN_ALLOWED_CTL` is locked
  // while `PIN_OUT_CTL` and `PIN_OUT_VALUE` remain writable:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0x82u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET) == 0u,
        "Expected REGWEN == 0 after rw0c clear");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET) ==
            0x82u,
        "Expected PIN_ALLOWED_CTL locked by REGWEN == 0");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x1u);
  CHECK(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET) == 0x1u,
      "Expected PIN_OUT_CTL writable when REGWEN == 0");
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET) ==
            0x1u,
        "Expected PIN_OUT_VALUE writable when REGWEN == 0");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x82u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0u);

  LOG_INFO(
      "All SYSRST_CTRL Earlgrey v2 errata & hardening items verified on "
      "CW340 FPGA!");
  return true;
}
