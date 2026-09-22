// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_aon_timer.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kRvCoreIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kRvCoreIbexNmiStateRegOffset = 0x50,
  kRvCoreIbexNmiStateWdogBit = 1,
};

static volatile uint32_t wdog_nmi_fired = 0;
static volatile uint32_t g_fault_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_count++;
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  wdog_nmi_fired++;
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT);
  abs_mmio_write32(kRvCoreIbexBase + kRvCoreIbexNmiStateRegOffset,
                   1u << kRvCoreIbexNmiStateWdogBit);
}

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

bool test_main(void) {
  bool all_ok = true;

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

  // 1. Writing WKUP_COUNT_HI while WKUP_CTRL.enable == 1 (with prescaler=0 so
  // prescale_count_q stays 0) must NOT lose elapsed ticks in WKUP_COUNT_LO.
  // In QEMU, writing WKUP_COUNT_HI resets wkup_origin_ns = now without
  // snapshotting WKUP_COUNT_LO first, discarding elapsed ticks.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(100);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 1u);
  busy_spin_micros(30);
  uint32_t wkup_lo =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  EXPECT_RTL(wkup_lo >= 15u,
             "Writing WKUP_COUNT_HI while enabled lost elapsed WKUP_COUNT_LO "
             "ticks (expected >= 15, got %u)",
             wkup_lo);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);

  // 1b. In RTL (aon_timer.sv:177-198 & aon_timer_core.sv:57-65), wkup_intr_o =
  // wkup_incr & (wkup_count >= wkup_thold) feeds u_aon_intr_flop ->
  // u_intr_sync (prim_edge_detector q_posedge_pulse_o).
  // When WKUP_CTRL.PRESCALER == 0 (and prescale_count_q == 0), wkup_incr is 1
  // on every single clk_aon_i cycle, so once wkup_count >= wkup_thold,
  // wkup_intr_o remains continuously 1 and u_intr_sync sees no further rising
  // edge after W1C clear of INTR_STATE.wkup_timer_expired until wkup_count <
  // wkup_thold (or wkup_ctrl.enable is toggled).
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 2u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(50);
  uint32_t intr_prescale0 =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_prescale0 &
              (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) != 0u,
             "WKUP_TIMER_EXPIRED did not set initially with PRESCALER=0");

  // Clear INTR_STATE.wkup_timer_expired via W1C while PRESCALER=0 and
  // WKUP_COUNT >= WKUP_THOLD: must NOT re-latch because wkup_intr_o stays 1.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  busy_spin_micros(50);
  intr_prescale0 =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_prescale0 &
              (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u,
             "INTR_STATE.wkup_timer_expired re-latched while PRESCALER=0 and "
             "wkup_intr_o stayed continuously 1 (got 0x%08x)",
             intr_prescale0);

  // Toggling WKUP_CTRL.ENABLE (0 -> 1) drops wkup_intr_o to 0 and back to 1,
  // producing a new rising edge at u_intr_sync while keeping
  // prescale_count_q=0.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(30);
  intr_prescale0 =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((intr_prescale0 &
              (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) != 0u,
             "INTR_STATE.wkup_timer_expired did not re-latch after toggling "
             "WKUP_CTRL.ENABLE");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  // 2. Disabling WKUP_CTRL with PRESCALER=0 after running with PRESCALER=200:
  // With prescaler=200 (~1 ms per wakeup tick at 200 kHz), spinning for 80 us
  // (~16 AON clocks) must NOT increment WKUP_COUNT_LO (expected 0).
  // In QEMU, writing WKUP_CTRL=0 overwrites PRESCALER=0 before computing
  // elapsed ticks, falsely inflating WKUP_COUNT_LO to ~20.
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
  wkup_lo = abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  EXPECT_RTL(wkup_lo == 0u,
             "Disabling WKUP_CTRL with prescaler=0 after 80us (prescaler=200) "
             "must keep WKUP_COUNT_LO == 0, got %u",
             wkup_lo);

  // 3. In RTL (aon_timer_core.sv), prescale_count_q is ONLY updated when
  // prescale_en is high and is NOT cleared when WKUP_CTRL.ENABLE is cleared.
  // Because prescale_count_q is ~16 from the previous 80us run, re-enabling
  // WKUP_CTRL with prescaler=0 means (prescale_count_q == prescaler) is false
  // until the 12-bit prescale_count_q wraps around (4080 AON clocks = ~20.4ms).
  // Thus after 100us, WKUP_COUNT_LO must still be 0!
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  busy_spin_micros(100);
  wkup_lo = abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  EXPECT_RTL(
      wkup_lo == 0u,
      "prescale_count_q must not reset on WKUP_CTRL disable; re-enabling "
      "with prescaler=0<prescale_count_q must not tick before 12-bit "
      "wraparound (expected 0, got %u)",
      wkup_lo);
  // Reset prescale_count_q back to 0 by setting prescaler=200 and waiting 1.1ms
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   ctrl_prescaled);
  busy_spin_micros(1100);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);

  // 4. Threshold comparison (`wkup_intr_o = wkup_incr & (wkup_count >=
  // wkup_thold)`) is gated by `wkup_incr` (when `prescale_count_q ==
  // prescaler`). When WKUP_COUNT=0 and WKUP_THOLD=0 with prescaler=200,
  // WKUP_CAUSE and INTR_STATE must NOT assert immediately upon enable before
  // the prescaler period (~1ms) elapses.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   ctrl_prescaled);
  busy_spin_micros(50);
  uint32_t intr_early =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      (intr_early & (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u,
      "WKUP_TIMER_EXPIRED fired before prescaler period elapsed "
      "(intr_state=0x%08x)",
      intr_early);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);

  // 5. ALERT_TEST is a pulse (`alert_test.q & alert_test.qe`), so writing 1
  // to ALERT_TEST a second time after clearing ALERT_CAUSE must re-trigger
  // alert_handler.
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault,
      kDifAlertHandlerClassD, kDifToggleEnabled, kDifToggleDisabled));

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault));
  abs_mmio_write32(kAonTimerBase + AON_TIMER_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(10);
  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault, &is_cause));
  EXPECT_RTL(is_cause, "First write to ALERT_TEST did not set alert cause");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault));
  busy_spin_micros(10);
  is_cause = true;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault, &is_cause));
  EXPECT_RTL(!is_cause,
             "ALERT_CAUSE remained set after acknowledge (ALERT_TEST latched "
             "high instead of pulsing)");
  abs_mmio_write32(kAonTimerBase + AON_TIMER_ALERT_TEST_REG_OFFSET, 1u);
  busy_spin_micros(10);
  is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault, &is_cause));
  EXPECT_RTL(is_cause,
             "Second write to ALERT_TEST did not set alert cause (ALERT_TEST "
             "latched high instead of pulsing)");
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdAonTimerAonFatalFault));

  // 6. Watchdog bark sets WKUP_CAUSE (`wkup_cause_d = wkup_intr_o | wdog_intr_o
  // | wkup_cause_q`) and `INTR_STATE.wdog_timer_bark` uses level-type edge
  // detection on `wdog_intr_o = wdog_incr & (wdog_count >= wdog_bark_thold)`:
  // clearing `INTR_STATE` while `WDOG_COUNT >= WDOG_BARK_THOLD` does not re-set
  // `INTR_STATE.wdog_timer_bark` until `WDOG_COUNT` is cleared below
  // `WDOG_BARK_THOLD`.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 2u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  busy_spin_micros(30);
  wdog_nmi_fired = 0;
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
  busy_spin_micros(50);
  uint32_t wkup_cause =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  EXPECT_RTL((wkup_cause & 1u) == 1u,
             "Watchdog bark did not set WKUP_CAUSE (got 0x%08x)", wkup_cause);
  EXPECT_RTL(wdog_nmi_fired == 1u,
             "Expected watchdog bark NMI to fire exactly once (got %u)",
             wdog_nmi_fired);
  EXPECT_RTL(
      (intr_state & (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT)) == 0u,
      "INTR_STATE.wdog_timer_bark re-latched while WDOG_COUNT stayed >= "
      "WDOG_BARK_THOLD (got 0x%08x)",
      intr_state);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);

  // 7. Wave 5: Verify AON_TIMER_PERMIT sub-word write checks
  // (aon_timer_reg_pkg.sv:191-206). WKUP_THOLD_LO (0x0c) has AON_TIMER_PERMIT =
  // 4'b1111: an 8-bit write (reg_be = 4'b0001) must assert wr_err = 1 (mcause =
  // 7) and preserve the register.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET,
                   0x12345678u);
  g_fault_count = 0;
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0xaau);
  EXPECT_RTL(g_fault_count == 1u,
             "Expected 8-bit write to WKUP_THOLD_LO (PERMIT=4'b1111) to fault "
             "(faults=%u)",
             g_fault_count);
  EXPECT_RTL(abs_mmio_read32(kAonTimerBase +
                             AON_TIMER_WKUP_THOLD_LO_REG_OFFSET) == 0x12345678u,
             "Sub-word write with wr_err=1 must not modify WKUP_THOLD_LO");

  // WKUP_CTRL (0x04) has AON_TIMER_PERMIT = 4'b0011: an 8-bit write (reg_be =
  // 4'b0001) must fault with wr_err = 1, whereas a 16-bit write at byte 0
  // (reg_be = 4'b0011) is permitted (wr_err = 0).
  g_fault_count = 0;
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  EXPECT_RTL(g_fault_count == 1u,
             "Expected 8-bit write to WKUP_CTRL (PERMIT=4'b0011, "
             "reg_be=4'b0001) to fault (faults=%u)",
             g_fault_count);
  g_fault_count = 0;
  *(volatile uint16_t *)(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET) = 0u;
  EXPECT_RTL(g_fault_count == 0u,
             "16-bit write to WKUP_CTRL (PERMIT=4'b0011, reg_be=4'b0011) must "
             "succeed (faults=%u)",
             g_fault_count);

  return all_ok;
}
