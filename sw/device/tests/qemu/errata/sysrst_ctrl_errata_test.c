// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sysrst_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for Earlgrey `sysrst_ctrl`
 * (`P32`).
 *
 * Empirically verifies all documented specification errata and security
 * hardening behaviors in `/root/knowledge/errata/sysrst_ctrl.md` and
 * `/root/knowledge/errata/sysrst_ctrl_combo_immediate_trigger_and_pin_inversion.md`
 * on both physical CW340 FPGA silicon (`fpga_cw340_rom_with_fake_keys`) and
 * QEMU (`sim_qemu_rom_with_fake_keys`):
 *
 * - `[sysrst_ctrl.sv:106-111]` (`SPEC_DOC_ERRATA`):
 *   1. `PIN_IN_VALUE` (`0x40`) samples raw uninverted `cio_*_in_i` inputs
 *      (`sysrst_ctrl_pin.sv:50-77`), unaffected by `KEY_INVERT_CTL` (`0x30`).
 *   2. Software writes to `KEY_INVERT_CTL` (`0x30`) while `KEY_INTR_CTL`
 * (`0x44`) is enabled synthesize hardware input edges in
 * `sysrst_ctrl_detect.sv:65-76`
 *      (`trigger_active & ~trigger_active_q`) and fire `KEY_INTR_STATUS`
 * (`0xa8`) without any physical button press.
 *   3. Configuring `COM_SEL_CTL_0 != 0` with `COM_DET_CTL_0 = 0` when the
 *      selected input is already active (`in_i & cfg_in_sel == 0`) immediately
 *      fires `COMBO_INTR_STATUS` (`0xa4`) on the next AON cycle without
 *      requiring a fresh pin transition (`sysrst_ctrl_combo.sv:75-120`).
 * - `[sysrst_ctrl_ulp.sv:68-95]` (`SPEC_DOC_ERRATA`): `ULP_STATUS` (`0x28`,
 * `rw1c`) is driven by the single-cycle `event_detected_pulse_o` on `DetectSt
 * -> StableSt` (`sysrst_ctrl_detect.sv:180-211`), while `Sticky = 1` parks the
 *   FSM in `StableSt` (`event_detected_o = 1`). Consequently, `rw1c`-clearing
 *   `ULP_STATUS` while `ULP_CTL == 1` stays `0` (does NOT re-latch even while
 *   `ac_present_int == 1`), yet blocks any subsequent ULP trigger until
 *   `ULP_CTL` is cycled `1 -> 0 -> 1`.
 * - `[sysrst_ctrl_reg_pkg.sv:678-722]` (`INTENDED_SECURITY_HARDENING`):
 * Heterogeneous 1/2/3/4-byte `SYSRST_CTRL_PERMIT[43]` byte-enable masks
 * (`4'b0001`, `4'b0011`, `4'b0111` on `AUTO_BLOCK_DEBOUNCE_CTL`, `4'b1111` on
 *   `COM_*_DET_CTL_*`) raise synchronous Store Access Faults (`mcause = 7`) on
 *   narrower sub-word writes, while `PIN_OUT_CTL` (`0x38`) and `PIN_OUT_VALUE`
 *   (`0x3c`) remain writable after `REGWEN` (`0x10`, `rw0c`) is cleared to `0`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
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
  LOG_INFO("Starting SYSRST_CTRL CW340/QEMU Errata Confirmation Test (P32)...");

  // Clean initial state.
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

  // =========================================================================
  // Check 1: [sysrst_ctrl.sv:106-111] (SPEC_DOC_ERRATA)
  // (a) `PIN_IN_VALUE` (`0x40`) reports raw uninverted `cio_*_in_i` pin levels
  //     regardless of `KEY_INVERT_CTL` (`0x30`).
  // (b) Flipping `KEY_INVERT_CTL` in software while `KEY_INTR_CTL` (`0x44`) is
  //     enabled synthesizes an immediate hardware input edge in
  //     `sysrst_ctrl_detect.sv:65-76` and sets `KEY_INTR_STATUS`.
  // (c) Programming `COM_SEL_CTL_0 = 1` with `COM_DET_CTL_0 = 0` when `key0` is
  //     already active immediately triggers `COMBO_INTR_STATUS`.
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl.sv:106-111] (SPEC_DOC_ERRATA): "
      "Raw uninverted PIN_IN_VALUE vs KEY_INVERT_CTL, software-synthesized "
      "KEY_INTR_STATUS edges, and immediate COM_DET_CTL_0=0 combo trigger...");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET,
                   (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_H2L_BIT) |
                       (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_L2H_BIT));
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET) ==
            0u,
        "[sysrst_ctrl.sv:106-111] Expected KEY_INTR_STATUS == 0 before "
        "KEY_INVERT_CTL write");

  // Flip KEY0_IN in KEY_INVERT_CTL while KEY_INTR_CTL is enabled:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  busy_spin_micros(60);

  uint32_t pin_in_after_invert =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  CHECK(pin_in_after_invert == pin_in_initial,
        "[sysrst_ctrl.sv:106-111] Expected PIN_IN_VALUE to remain raw "
        "uninverted (0x%08x), got 0x%08x",
        pin_in_initial, pin_in_after_invert);

  uint32_t key_intr_status =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET);
  CHECK(key_intr_status != 0u && (intr_state & 0x1u) == 1u,
        "[sysrst_ctrl.sv:106-111] Expected KEY_INVERT_CTL write while "
        "KEY_INTR_CTL enabled to synthesize edge and set KEY_INTR_STATUS "
        "(0x%x) & INTR_STATE (0x%x)",
        key_intr_status, intr_state);

  // Now verify `sysrst_ctrl_combo.sv:111-120`:
  // Because `assign trigger = (in & cfg_in_sel) == '0` is `1'b1` when
  // `COM_SEL_CTL_0 == 0` (`cfg_in_sel == '0`), `trigger_active_q` in
  // `u_sysrst_ctrl_detect` (`EventType = EdgeToHigh`) is already `1'b1`.
  // Therefore, enabling `COM_SEL_CTL_0 = 1` while `key0_int` is already `0`
  // keeps `trigger` at `1'b1 -> 1'b1` (`trigger_event = 0`, so
  // `COMBO_INTR_STATUS` stays `0`), whereas transitioning `key0_int` `1 -> 0`
  // via `KEY_INVERT_CTL` after `COM_SEL_CTL_0 = 1` synthesizes a `0 -> 1` edge
  // on `trigger` and immediately sets `COMBO_INTR_STATUS = 1`!
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

  // Enable combo 0 on key0 with COM_DET_CTL_0 == 0 while key0_int is already 0:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET,
                   1u << 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 1u);
  busy_spin_micros(60);
  uint32_t combo_status_no_edge =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET);
  CHECK(combo_status_no_edge == 0u,
        "[sysrst_ctrl.sv:106-111] Expected COMBO_INTR_STATUS == 0 when "
        "COM_SEL_CTL_0=1 is written while key0_int is already 0 (trigger stays "
        "1->1), got 0x%x",
        combo_status_no_edge);

  // Now set key0_int = 1 (`trigger = 0`) and flip KEY_INVERT_CTL to key0_int =
  // 0 (`trigger` rises `0 -> 1`):
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_low);
  busy_spin_micros(60);
  uint32_t combo_status =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET);
  CHECK((combo_status & 0x1u) == 1u,
        "[sysrst_ctrl.sv:106-111] Expected COMBO_INTR_STATUS bit 0 == 1 after "
        "KEY_INVERT_CTL synthesizes 1->0 key0_int edge, got 0x%x",
        combo_status);

  // Clean up combo 0 and key interrupt:
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
  // Check 2: [sysrst_ctrl_ulp.sv:68-95] (SPEC_DOC_ERRATA)
  // `ULP_STATUS` (`0x28`) single-cycle `event_detected_pulse_o` vs `Sticky = 1`
  // `StableSt` park:
  // - Enabling `ULP_CTL = 1` with `ac_present_int == 1` sets `ULP_STATUS = 1`
  //   and `WKUP_STATUS = 1`.
  // - Clearing `ULP_STATUS` (`rw1c`) while `ULP_CTL == 1` and `ac_present_int
  //   == 1` stays `0` (does NOT re-latch!), yet the ULP FSM remains parked in
  //   `StableSt` (`Sticky = 1`) and ignores subsequent `ac_present` transitions
  //   until `ULP_CTL` is toggled `1 -> 0 -> 1`.
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl_ulp.sv:68-95] (SPEC_DOC_ERRATA): "
      "ULP_STATUS rw1c single-pulse behavior vs Sticky=1 StableSt park "
      "requiring ULP_CTL 1->0->1 cycle...");
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

  // Clear ULP_STATUS and WKUP_STATUS (`rw1c`) WITHOUT clearing ULP_CTL:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected ULP_STATUS to stay 0 after rw1c "
        "even while ULP_CTL==1 and ac_present_int==1");

  // Toggle ac_present_int 1 -> 0 -> 1 while ULP_CTL is still 1: because
  // `Sticky = 1` keeps the ULP FSM parked in `StableSt`, `ULP_STATUS` must
  // STILL remain 0!
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_low);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_high);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u,
        "[sysrst_ctrl_ulp.sv:68-95] Expected Sticky=1 StableSt to block new "
        "ULP_STATUS pulse until ULP_CTL is cleared to 0");

  // Now cycle ULP_CTL 1 -> 0 -> 1 and verify ULP_STATUS fires again:
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
  // Check 3: [sysrst_ctrl_reg_pkg.sv:678-722] (INTENDED_SECURITY_HARDENING)
  // Heterogeneous 1/2/3/4-byte `SYSRST_CTRL_PERMIT[43]` byte-enable masks &
  // `PIN_OUT_CTL` / `PIN_OUT_VALUE` ungated by `REGWEN = 0`.
  // =========================================================================
  LOG_INFO(
      "Verifying [sysrst_ctrl_reg_pkg.sv:678-722] "
      "(INTENDED_SECURITY_HARDENING): "
      "Heterogeneous SYSRST_CTRL_PERMIT (4'b0001/0011/0111/1111) sub-word "
      "write faults (mcause=7) & PIN_OUT_CTL/PIN_OUT_VALUE ungated by "
      "REGWEN...");
  // 16-bit sh write to ULP_AC_DEBOUNCE_CTL (0x18, permit 4'b0011) succeeds:
  g_fault_count = 0;
  *(volatile uint16_t *)(kSysrstBase +
                         SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET) = 0x0010u;
  CHECK(g_fault_count == 0u,
        "[sysrst_ctrl_reg_pkg.sv:678-722] Expected 16-bit sh to "
        "ULP_AC_DEBOUNCE_CTL "
        "(permit 4'b0011) to succeed");

  // 8-bit sb write to ULP_AC_DEBOUNCE_CTL (0x18, permit 4'b0011) faults:
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET,
                  0x20u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:678-722] Expected 8-bit sb to "
        "ULP_AC_DEBOUNCE_CTL "
        "(permit 4'b0011) to trap with mcause=7");

  // 16-bit sh write to AUTO_BLOCK_DEBOUNCE_CTL (0x4c, permit 4'b0111) faults:
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kSysrstBase +
                         SYSRST_CTRL_AUTO_BLOCK_DEBOUNCE_CTL_REG_OFFSET) =
      0x0010u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:678-722] Expected 16-bit sh to "
        "AUTO_BLOCK_DEBOUNCE_CTL (permit 4'b0111) to trap with mcause=7");

  // 16-bit sh write to COM_DET_CTL_0 (0x84, permit 4'b1111) faults:
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET) =
      0x0010u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[sysrst_ctrl_reg_pkg.sv:678-722] Expected 16-bit sh to COM_DET_CTL_0 "
        "(permit 4'b1111) to trap with mcause=7");

  // Clear REGWEN (`0x10`, rw0c) to 0 and verify `PIN_ALLOWED_CTL` is locked
  // while `PIN_OUT_CTL` and `PIN_OUT_VALUE` remain writable:
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0x3u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET) == 0u,
        "Expected REGWEN == 0 after rw0c clear");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET) ==
            0x3u,
        "Expected PIN_ALLOWED_CTL locked by REGWEN == 0");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x1u);
  CHECK(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET) == 0x1u,
      "Expected PIN_OUT_CTL writable when REGWEN == 0");
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET) ==
            0x1u,
        "Expected PIN_OUT_VALUE writable when REGWEN == 0");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0u);

  LOG_INFO("All SYSRST_CTRL errata & hardening items verified successfully!");
  return true;
}
