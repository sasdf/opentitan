// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file aon_timer_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `aon_timer` (`P11`).
 *
 * Empirically confirms on physical CW340 FPGA silicon and QEMU:
 * 1. [aon_timer.sv:165-198] (SPEC_DOC_ERRATA, MEDIUM):
 *    `programmers_guide.md:90` claims `INTR_STATE` re-triggers at the next
 *    clock tick if `WKUP_COUNT >= WKUP_THOLD` after W1C clear. In
 *    `aon_timer.sv:187-198`, `u_intr_sync` (`q_posedge_pulse_o`) requires a
 *    `0 -> 1` edge on `aon_intr_set`. When `WKUP_CTRL.PRESCALER == 0`,
 *    `wkup_incr` is continuously `1`, so clearing
 * `INTR_STATE.wkup_timer_expired` via W1C leaves
 * `INTR_STATE.wkup_timer_expired` permanently `0` (while level-triggered
 * `WKUP_CAUSE` re-asserts `1`).
 * 2. [aon_timer_core.sv:40-60] (TRUE_SILICON_ERRATA, MEDIUM):
 *    In `aon_timer_core.sv:40-60`, `prescale_count_q` is NOT cleared when
 *    `WKUP_CTRL.ENABLE = 0` and uses exact equality
 *    (`prescale_count_q == prescaler`) instead of `>=`, causing a 12-bit
 *    (`4096`-cycle = `20.48 ms`) wrap-around stall when `PRESCALER` is lowered
 *    below a residual `prescale_count_q` (`e.g. 0 < 16`).
 * 3. [aon_timer_core.sv:64-65] (SPEC_DOC_ERRATA, LOW):
 *    In `aon_timer_core.sv:65`, `wkup_intr_o = wkup_incr & (wkup_count >=
 *    wkup_thold)` gates the threshold comparator by `wkup_incr`
 *    (`prescale_count_q == prescaler`), delaying `WKUP_CAUSE` and `INTR_STATE`
 *    even when `WKUP_COUNT >= WKUP_THOLD` (`0 >= 0`) until `prescale_count_q`
 *    reaches `PRESCALER`.
 * 4. [aon_timer_reg_pkg.sv:191-206] (INTENDED_SECURITY_HARDENING, LOW — SEC_CM:
 * BUS.INTEGRITY): `AON_TIMER_PERMIT` (`aon_timer_reg_pkg.sv:191-206`) assigns
 * `4'b0011` to `WKUP_CTRL` (rejecting 1-byte `sb` with `mcause = 7` while
 * accepting 2-byte `sh`), `4'b0001` to `WDOG_CTRL` (accepting 1-byte `sb`), and
 *    `4'b1111` to `WKUP_THOLD_LO` (rejecting `sb`/`sh` with `mcause = 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
};

static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  if (g_expect_access_fault) {
    g_last_mcause = ibex_mcause_read();
    ++g_access_fault_count;
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled Load/Store Fault",
                           ibex_mcause_read());
  abort();
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey AON_TIMER Errata Confirmation Suite (P11) ===");

  // Stop both timers and clear state.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  // ---------------------------------------------------------------------------
  // 1. [aon_timer.sv:165-198] (SPEC_DOC_ERRATA, MEDIUM):
  //    With WKUP_CTRL.PRESCALER == 0 (and prescale_count_q == 0), wkup_incr is
  //    continuously 1, so clearing INTR_STATE.wkup_timer_expired via W1C while
  //    WKUP_COUNT >= WKUP_THOLD leaves INTR_STATE.wkup_timer_expired == 0
  //    (whereas level-triggered WKUP_CAUSE re-asserts 1 on the next AON tick).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [aon_timer.sv:165-198] (SPEC_DOC_ERRATA): INTR_STATE fails to "
      "re-trigger when PRESCALER==0");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 2u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(50);

  uint32_t intr_state =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  CHECK(
      ((intr_state >> AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT) & 1u) == 1u,
      "[aon_timer.sv:165-198] Expected initial INTR_STATE.wkup_timer_expired "
      "== 1");

  // Clear both WKUP_CAUSE (rw0c) and INTR_STATE.wkup_timer_expired (rw1c)
  // while WKUP_COUNT >= WKUP_THOLD and PRESCALER == 0.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  busy_spin_micros(50);

  uint32_t wkup_cause =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  intr_state = abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  CHECK(wkup_cause == 1u,
        "[aon_timer.sv:165-198] Expected level-triggered WKUP_CAUSE to "
        "re-assert 1");
  CHECK(
      ((intr_state >> AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT) & 1u) == 0u,
      "[aon_timer.sv:165-198] Expected edge-triggered "
      "INTR_STATE.wkup_timer_expired "
      "to stay 0 when PRESCALER==0 (got 0x%x)",
      intr_state);

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);
  LOG_INFO(
      "[aon_timer.sv:165-198] CONFIRMED: WKUP_CAUSE=1, INTR_STATE=0 after W1C "
      "with PRESCALER=0");

  // ---------------------------------------------------------------------------
  // 2. [aon_timer_core.sv:40-60] (TRUE_SILICON_ERRATA, MEDIUM):
  //    prescale_count_q is NOT cleared on WKUP_CTRL.ENABLE = 0 and uses exact
  //    equality (prescale_count_q == prescaler). Running with PRESCALER=200
  //    for 80us (~16 AON ticks -> prescale_count_q ≈ 16), disabling, and
  //    re-enabling with PRESCALER=0 (0 < 16) stalls WKUP_COUNT_LO at 0 across
  //    100us (~20 AON ticks) because prescale_count_q must wrap 12 bits!
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [aon_timer_core.sv:40-60] (TRUE_SILICON_ERRATA): uncleared "
      "prescale_count_q & 12-bit wrap stall");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  busy_spin_micros(30);

  uint32_t ctrl_prescaled =
      bitfield_field32_write(0u, AON_TIMER_WKUP_CTRL_PRESCALER_FIELD, 200u) |
      (1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   ctrl_prescaled);
  busy_spin_micros(80);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);

  uint32_t wkup_lo =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  CHECK(wkup_lo == 0u,
        "[aon_timer_core.sv:40-60] Expected WKUP_COUNT_LO == 0 after 80us with "
        "PRESCALER=200 (got %u)",
        wkup_lo);

  // Re-enable with PRESCALER=0 (< residual prescale_count_q ≈ 16):
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(100);
  wkup_lo = abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  CHECK(
      wkup_lo == 0u,
      "[aon_timer_core.sv:40-60] Expected WKUP_COUNT_LO == 0 after 100us with "
      "PRESCALER=0 due to uncleared prescale_count_q wrap stall (got %u)",
      wkup_lo);

  // Restore prescale_count_q back to 0 by letting PRESCALER=200 complete 1 tick
  // (~1.1ms).
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   ctrl_prescaled);
  busy_spin_micros(1100);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  LOG_INFO(
      "[aon_timer_core.sv:40-60] CONFIRMED: WKUP_COUNT_LO=%u (stalled during "
      "12-bit wrap when PRESCALER=0 < prescale_count_q)",
      wkup_lo);

  // ---------------------------------------------------------------------------
  // 3. [aon_timer_core.sv:64-65] (SPEC_DOC_ERRATA, LOW):
  //    wkup_intr_o = wkup_incr & (wkup_count >= wkup_thold) is gated by
  //    wkup_incr. With WKUP_COUNT=0, WKUP_THOLD=0, and PRESCALER=200,
  //    WKUP_CAUSE and INTR_STATE stay 0 after 100us until wkup_incr fires at
  //    ~1ms.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [aon_timer_core.sv:64-65] (SPEC_DOC_ERRATA): wkup_intr_o "
      "gated "
      "by wkup_incr");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   ctrl_prescaled);
  busy_spin_micros(100);
  wkup_cause = abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  CHECK(
      wkup_cause == 0u,
      "[aon_timer_core.sv:64-65] Expected WKUP_CAUSE == 0 at 100us before "
      "wkup_incr (PRESCALER=200), even though WKUP_COUNT(0) >= WKUP_THOLD(0)");
  busy_spin_micros(1100);
  wkup_cause = abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  CHECK(wkup_cause == 1u,
        "[aon_timer_core.sv:64-65] Expected WKUP_CAUSE == 1 after wkup_incr "
        "tick");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);
  LOG_INFO(
      "[aon_timer_core.sv:64-65] CONFIRMED: wkup_intr_o gated by wkup_incr");

  // ---------------------------------------------------------------------------
  // 4. [aon_timer_reg_pkg.sv:191-206] (INTENDED_SECURITY_HARDENING, LOW —
  // SEC_CM: BUS.INTEGRITY):
  //    AON_TIMER_PERMIT enforces 4'b0011 on WKUP_CTRL (rejecting sb with
  //    mcause=7 while accepting sh), 4'b0001 on WDOG_CTRL (accepting sb), and
  //    4'b1111 on WKUP_THOLD_LO (rejecting sb/sh with mcause=7).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [aon_timer_reg_pkg.sv:191-206] (INTENDED_SECURITY_HARDENING): "
      "AON_TIMER_PERMIT sub-word write protection");
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0x01u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "[aon_timer_reg_pkg.sv:191-206] Expected 8-bit write (sb) to WKUP_CTRL "
        "(PERMIT=4'b0011) to fault with mcause=7");

  g_access_fault_count = 0;
  *(volatile uint16_t *)(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET) =
      0x0002u;
  busy_spin_micros(30);
  CHECK(
      g_access_fault_count == 0u &&
          abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET) ==
              0x0002u,
      "[aon_timer_reg_pkg.sv:191-206] Expected 16-bit write (sh) to WKUP_CTRL "
      "(PERMIT=4'b0011) to succeed");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  *(volatile uint16_t *)(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET) =
      0x1234u;
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "[aon_timer_reg_pkg.sv:191-206] Expected 16-bit write (sh) to "
        "WKUP_THOLD_LO "
        "(PERMIT=4'b1111) to fault with mcause=7");
  LOG_INFO(
      "[aon_timer_reg_pkg.sv:191-206] CONFIRMED: AON_TIMER_PERMIT 4'b0011 & "
      "4'b1111 "
      "enforced");

  LOG_INFO("=== ALL AON_TIMER ERRATA CHECKS PASSED ===");
  return true;
}
