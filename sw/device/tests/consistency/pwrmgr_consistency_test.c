// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_base.h"
#include "sw/device/lib/dif/dif_pwrmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/aon_timer_regs.h"
#include "hw/top/otp_ctrl_regs.h"
#include "hw/top/pinmux_regs.h"
#include "hw/top/pwrmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

static const dt_pwrmgr_t kPwrmgrDt = (dt_pwrmgr_t)0;

enum {
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
  kOtpCtrlBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
  kAonTimerWkupBit = 1u << PWRMGR_PARAM_AON_TIMER_WKUP_REQ_IDX,
  kPwrmgrUnmappedOffset = 0x44u,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_bus_fault_seen = false;
static volatile uint32_t g_bus_fault_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  CHECK(g_expect_bus_fault, "Unexpected load/store fault: mcause=0x%x", mcause);
  g_bus_fault_seen = true;
  g_bus_fault_mcause = mcause;
}

static void expect_load_fault(uint32_t addr) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  (void)abs_mmio_read32(addr);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Load Access Fault (mcause=5) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcLoadAccessFault,
        "Expected mcause=5, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store32_fault(uint32_t addr, uint32_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  abs_mmio_write32(addr, val);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Store Access Fault (mcause=7) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store8_fault(uint32_t addr, uint8_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  abs_mmio_write8(addr, val);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen,
        "Expected Store Access Fault (mcause=7) on 8-bit write at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while (abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) != 0u) {
  }
}

static void aon_timer_clear_wakeup(void) {
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  busy_spin_micros(30);
}

/**
 * Verify pwrmgr.sv:525,535-538:
 * WAKE_STATUS (0x24) reflects peri_reqs_masked.wakeups
 * (slow_peri_reqs.wakeups & slow_wakeup_en) and remains 0 even when
 * aon_timer wkup_req is actively asserted unless WAKEUP_EN (0x20) is set
 * and synchronized across the slow AON clock domain via CFG_CDC_SYNC (0x18).
 */
static void test_pwrmgr_wake_status_cdc_masking(void) {
  LOG_INFO(
      "Verifying [pwrmgr.sv:525,535-538]: WAKE_STATUS masking by "
      "slow_wakeup_en via CFG_CDC_SYNC");

  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  aon_timer_clear_wakeup();

  // Assert AON timer wakeup request while WAKEUP_EN == 0.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 1u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  while ((abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET) &
          (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u) {
  }
  busy_spin_micros(30);

  uint32_t wake_status =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == 0u,
        "[pwrmgr.sv:525,535-538] WAKE_STATUS must be 0 when slow_wakeup_en==0, "
        "got 0x%08x",
        wake_status);

  // Enable AON timer bit in WAKEUP_EN: before CFG_CDC_SYNC, slow_wakeup_en is
  // still 0 even after waiting 30 us of slow clock cycles, so WAKE_STATUS stays
  // 0 until CFG_CDC_SYNC is pulsed.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);
  busy_spin_micros(30);
  wake_status = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == 0u,
        "[pwrmgr.sv:525,535-538] WAKE_STATUS must remain 0 after writing "
        "WAKEUP_EN before CFG_CDC_SYNC, got 0x%08x",
        wake_status);
  pwrmgr_cdc_sync();
  busy_spin_micros(30);
  wake_status = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == kAonTimerWkupBit,
        "[pwrmgr.sv:525,535-538] WAKE_STATUS must equal 0x%08x after "
        "CFG_CDC_SYNC, got 0x%08x",
        kAonTimerWkupBit, wake_status);

  // Clean up AON timer wakeup request.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  aon_timer_clear_wakeup();
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) == 0u);
}

/**
 * Verify dif_pwrmgr.c:113-182, 322-392, 451-485 & dif_pwrmgr_autogen.c:59-96:
 * 1. Initializing dif_pwrmgr_t via dif_pwrmgr_init(base_addr, &pwrmgr_no_dt)
 *    returns kDifOk and sets pwrmgr_no_dt.dt = kDtPwrmgrCount (1).
 * 2. Although dif_pwrmgr.h:196, 211 specifies @return kDifError if the DIF was
 *    not initialized by DT, both dif_pwrmgr_find_request_source() and
 *    dif_pwrmgr_get_all_request_sources() return kDifBadArg (2) instead of
 *    kDifError (1).
 * 3. Furthermore, dif_pwrmgr_set_request_sources(),
 *    dif_pwrmgr_get_request_sources(),
 *    dif_pwrmgr_get_current_request_sources(), and
 *    dif_pwrmgr_wakeup_reason_get() all fail with kDifBadArg (2) on a
 *    dif_pwrmgr_init()-constructed handle (even when WAKE_INFO has ABORT or
 *    FALL_THROUGH bits that do not depend on DT), whereas a handle initialized
 *    via dif_pwrmgr_init_from_dt(kPwrmgrDt, &pwrmgr_dt) succeeds with kDifOk.
 */
static void test_pwrmgr_dif_dt_init_regression_and_return_code(void) {
  LOG_INFO(
      "Verifying [dif_pwrmgr.c:113-182,322-392,451-485]: non-DT "
      "dif_pwrmgr_init() kDifBadArg regression and kDifError doc mismatch");

  dif_pwrmgr_t pwrmgr_no_dt;
  dif_pwrmgr_t pwrmgr_dt;
  CHECK_DIF_OK(
      dif_pwrmgr_init(mmio_region_from_addr(kPwrmgrBase), &pwrmgr_no_dt));
  CHECK_DIF_OK(dif_pwrmgr_init_from_dt(kPwrmgrDt, &pwrmgr_dt));
  CHECK(pwrmgr_no_dt.dt == kDtPwrmgrCount,
        "Expected dif_pwrmgr_init() to set dt == kDtPwrmgrCount (%d), got %d",
        (int)kDtPwrmgrCount, (int)pwrmgr_no_dt.dt);

  // Domain config and lock query functions still work on pwrmgr_no_dt.
  dif_pwrmgr_domain_config_t domain_cfg = 0;
  CHECK_DIF_OK(dif_pwrmgr_get_domain_config(&pwrmgr_no_dt, &domain_cfg));

  // dif_pwrmgr.h:196, 211 documents kDifError when not initialized by DT,
  // but dif_pwrmgr_get_dt() returns kDifBadArg.
  dif_pwrmgr_request_sources_t sources = 0;
  dif_result_t res_all = dif_pwrmgr_get_all_request_sources(
      &pwrmgr_no_dt, kDifPwrmgrReqTypeWakeup, &sources);
  CHECK(res_all == kDifBadArg,
        "[dif_pwrmgr.c:155,dif_pwrmgr.h:211] Expected kDifBadArg (%d) vs "
        "documented kDifError (%d), got %d",
        (int)kDifBadArg, (int)kDifError, (int)res_all);

  dif_result_t res_find = dif_pwrmgr_find_request_source(
      &pwrmgr_no_dt, kDifPwrmgrReqTypeWakeup, dt_pwrmgr_instance_id(kPwrmgrDt),
      0, &sources);
  CHECK(res_find == kDifBadArg,
        "[dif_pwrmgr.c:121,dif_pwrmgr.h:196] Expected kDifBadArg (%d) vs "
        "documented kDifError (%d), got %d",
        (int)kDifBadArg, (int)kDifError, (int)res_find);

  // All 4 request-source / wakeup-reason DIFs fail with kDifBadArg on
  // pwrmgr_no_dt while succeeding with kDifOk on pwrmgr_dt.
  CHECK(dif_pwrmgr_set_request_sources(&pwrmgr_no_dt, kDifPwrmgrReqTypeWakeup,
                                       0u, kDifToggleDisabled) == kDifBadArg);
  CHECK(dif_pwrmgr_get_request_sources(&pwrmgr_no_dt, kDifPwrmgrReqTypeWakeup,
                                       &sources) == kDifBadArg);
  CHECK(dif_pwrmgr_get_current_request_sources(
            &pwrmgr_no_dt, kDifPwrmgrReqTypeWakeup, &sources) == kDifBadArg);

  dif_pwrmgr_wakeup_reason_t reason = {0};
  CHECK(dif_pwrmgr_wakeup_reason_get(&pwrmgr_no_dt, &reason) == kDifBadArg);

  CHECK_DIF_OK(dif_pwrmgr_get_all_request_sources(
      &pwrmgr_dt, kDifPwrmgrReqTypeWakeup, &sources));
  CHECK(sources == 0x3fu, "Expected 6 wakeup sources (0x3f), got 0x%x",
        sources);
  CHECK_DIF_OK(dif_pwrmgr_wakeup_reason_get(&pwrmgr_dt, &reason));
}

/**
 * Verify pwrmgr_fsm.sv:378,382,401-411, pwrmgr_wake_info.sv:36-65, and
 * pinmux.sv:436,495-520:
 * 1. When WFI is executed with CONTROL.LOW_POWER_HINT = 1 while OTP DAI is
 *    busy (otp_idle_i == 0), pwrmgr_fsm aborts low-power entry in
 *    FastPwrStateNvmIdleChk (setting WAKE_INFO.ABORT = 1 and INTR_STATE.WAKEUP
 *    = 1) without ever entering FastPwrStateLowPower.
 * 2. Even though low-power entry was aborted, pwrmgr_fsm.sv:378,382 already
 *    asserted low_power_o = 1 in FastPwrStateDisClks, which:
 *    (a) Pulses pinmux.sleep_en_i = 1 (earlgrey_pd_main.sv:1861), latching
 *        PINMUX_MIO_PAD_SLEEP_STATUS_0 (0x4046053c) to 1 and locking MIO pads
 *        into sleep retention despite the aborted sleep entry!
 *    (b) Arms pwrmgr_wake_info record_en = 1 (pwrmgr_wake_info.sv:41-44), so
 *        even after WAKE_INFO is cleared to 0, subsequent wakeup requests in
 *        normal Active mode are recorded into WAKE_INFO and immediately
 *        override rw1c clears until WAKE_INFO_CAPTURE_DIS = 1 (0x38) is set.
 * 3. Also verify normal sleep wakeup retention of CONTROL[8:4] across
 *    LowPowerExit (pwrmgr.sv:340-354).
 */
static void test_pwrmgr_aborted_sleep_pinmux_latch_and_wake_info_relatch(void) {
  LOG_INFO(
      "Verifying [pwrmgr_fsm.sv:378,382,401-411 & pinmux.sv:436,495-520]: "
      "Aborted low-power entry latches pinmux MIO_PAD_SLEEP_STATUS and arms "
      "WAKE_INFO record_en in Active mode");

  const uint32_t kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR;
  const uint32_t kPwrmgrIrq = kTopEarlgreyPlicIrqIdPwrmgrWakeup;

  irq_global_ctrl(false);
  irq_external_ctrl(true);
  abs_mmio_write32(kPlicBase + 0x0u + (kPwrmgrIrq * 4u), 1u);
  uint32_t ie_off = 0x2000u + ((kPwrmgrIrq / 32u) * 4u);
  uint32_t ie_val = abs_mmio_read32(kPlicBase + ie_off);
  abs_mmio_write32(kPlicBase + ie_off, ie_val | (1u << (kPwrmgrIrq % 32u)));
  abs_mmio_write32(kPlicBase + 0x200000u, 0u);

  // Configure MIO pad 0 (an unused MIO pad) with MIO_PAD_SLEEP_MODE_0 = 2
  // (High-Z) and MIO_PAD_SLEEP_EN_0 = 1, and clear MIO_PAD_SLEEP_STATUS_0
  // (rw0c: write 0 to clear bit 0).
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_MODE_0_REG_OFFSET, 2u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_EN_0_REG_OFFSET, 1u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET, ~1u);
  CHECK(
      (abs_mmio_read32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET) &
       1u) == 0u,
      "Expected MIO_PAD_SLEEP_STATUS_0 bit 0 == 0 before sleep attempt");

  // Prepare pwrmgr for low-power entry with WAKEUP_EN == 0 (no peripheral
  // wakeup enabled; only the ABORT wakeup interrupt will wake Ibex from WFI).
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET) == 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);

  uint32_t retained_ctrl_bits = (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT) |
                                (1u << PWRMGR_CONTROL_USB_CLK_EN_ACTIVE_BIT);
  abs_mmio_write32(
      kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET,
      retained_ctrl_bits | (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT));
  pwrmgr_cdc_sync();

  // Issue an OTP DAI write of 0x0 to VENDOR_TEST (offset 0x0) so otp_idle_i
  // is 0 (dai_prog_idle_o == 0) when WFI enters FastPwrStateNvmIdleChk.
  while ((abs_mmio_read32(kOtpCtrlBase + OTP_CTRL_STATUS_REG_OFFSET) &
          (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) == 0u) {
  }
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT);
  wait_for_interrupt();

  // Wait for OTP DAI to return to idle and clear any recoverable error if set.
  while ((abs_mmio_read32(kOtpCtrlBase + OTP_CTRL_STATUS_REG_OFFSET) &
          (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) == 0u) {
  }
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_INTR_STATE_REG_OFFSET, 0x3u);

  // 1. Verify low-power entry ABORTED in FastPwrStateNvmIdleChk (WAKE_INFO ==
  // 0x80, ABORT bit 7 set, no peripheral wakeup bits set).
  uint32_t wake_info_abort =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK(wake_info_abort == (1u << PWRMGR_WAKE_INFO_ABORT_BIT),
        "[pwrmgr_fsm.sv:401-411] Expected WAKE_INFO == 0x80 (ABORT) on aborted "
        "low-power entry, got 0x%08x",
        wake_info_abort);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x1u,
        "Expected INTR_STATE.WAKEUP == 1 after aborted low-power entry");
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  uint32_t plic_claim = abs_mmio_read32(kPlicBase + 0x200004u);
  if (plic_claim != 0u) {
    abs_mmio_write32(kPlicBase + 0x200004u, plic_claim);
  }

  // 2. Verify that despite aborting low-power entry before
  // FastPwrStateLowPower, pwrmgr.low_power_o pulsed 1 in FastPwrStateDisClks
  // and latched pinmux MIO_PAD_SLEEP_STATUS_0 bit 0 to 1!
  uint32_t mio_sleep_status =
      abs_mmio_read32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET);
  CHECK((mio_sleep_status & 1u) == 1u,
        "[pwrmgr_fsm.sv:378,382 & pinmux.sv:436,518] Expected aborted sleep "
        "entry to prematurely latch MIO_PAD_SLEEP_STATUS_0 bit 0 == 1, got "
        "0x%08x",
        mio_sleep_status);

  // Clean up MIO_PAD_SLEEP_EN_0 and MIO_PAD_SLEEP_STATUS_0 (rw0c).
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_EN_0_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET, ~1u);

  // 3. Verify that even though low-power entry was ABORTED and WAKE_INFO is now
  // cleared via rw1c (0xff -> WAKE_INFO == 0), record_en remains armed (1) in
  // Active mode, so an AON timer wakeup asserted in Active mode is immediately
  // recorded into WAKE_INFO and overrides subsequent rw1c clears!
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET) == 0u,
        "Expected WAKE_INFO == 0 after clearing ABORT bit");

  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);
  pwrmgr_cdc_sync();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 1u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  while ((abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET) &
          (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u) {
  }
  busy_spin_micros(30);

  uint32_t wake_info_active =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK((wake_info_active & kAonTimerWkupBit) != 0u,
        "[pwrmgr_wake_info.sv:36-65] Expected WAKE_INFO to capture "
        "kAonTimerWkupBit in Active mode after aborted sleep entry, got 0x%08x",
        wake_info_active);

  // Writing 0xff (rw1c) to WAKE_INFO while WAKE_INFO_CAPTURE_DIS == 0 and
  // aon_timer wkup_req is still asserted immediately re-latches
  // kAonTimerWkupBit on the next cycle.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  wake_info_active = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK((wake_info_active & kAonTimerWkupBit) != 0u,
        "[pwrmgr_wake_info.sv:36-65] Expected WAKE_INFO to immediately "
        "re-latch active wakeup after rw1c while WAKE_INFO_CAPTURE_DIS==0, "
        "got 0x%08x",
        wake_info_active);

  // Disabling capture (WAKE_INFO_CAPTURE_DIS = 1) disarms record_en (0),
  // allowing rw1c (0xff) to keep WAKE_INFO == 0 even while wkup_req is active.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET) == 0u,
        "Expected WAKE_INFO == 0 after WAKE_INFO_CAPTURE_DIS=1 + rw1c");

  // Clean up AON timer wakeup before testing normal sleep wakeup retention.
  aon_timer_clear_wakeup();
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) == 0u,
        "Expected WAKE_STATUS == 0 after aon_timer_clear_wakeup()");

  // 4. Verify normal sleep wakeup (LowPowerExit) retains CONTROL[8:4], clears
  // LOW_POWER_HINT, and unlocks CTRL_CFG_REGWEN = 1.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  abs_mmio_write32(
      kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET,
      retained_ctrl_bits | (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT));
  pwrmgr_cdc_sync();

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 10u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  wait_for_interrupt();

  uint32_t ctrl_after =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
  CHECK(ctrl_after == retained_ctrl_bits,
        "[pwrmgr.sv:340-354] Expected CONTROL == 0x%08x after LowPowerExit, "
        "got 0x%08x",
        retained_ctrl_bits, ctrl_after);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) ==
            kAonTimerWkupBit,
        "Expected WAKEUP_EN retained across LowPowerExit");
  CHECK((abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET) &
         kAonTimerWkupBit) != 0u,
        "Expected WAKE_INFO to retain wakeup bit across LowPowerExit");
  CHECK((abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) &
         kAonTimerWkupBit) != 0u,
        "Expected WAKE_STATUS to retain wakeup status across LowPowerExit");

  // Clean up AON timer, PLIC, and pwrmgr interrupt state.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  aon_timer_clear_wakeup();
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  plic_claim = abs_mmio_read32(kPlicBase + 0x200004u);
  if (plic_claim != 0u) {
    abs_mmio_write32(kPlicBase + 0x200004u, plic_claim);
  }
  abs_mmio_write32(kPlicBase + ie_off, ie_val);
  irq_external_ctrl(true);
  irq_global_ctrl(true);
}

/**
 * Verify pwrmgr_reg_pkg.sv:273-291 & pwrmgr_reg_top.sv:1245-1267:
 * PWRMGR_PERMIT[5] = 4'b0011 on CONTROL (0x14) rejects 8-bit sb stores with
 * synchronous Store Access Fault (mcause = 7) while allowing 16-bit sh stores,
 * PWRMGR_PERMIT[1] = 4'b0001 on INTR_ENABLE (0x04) allows 8-bit sb at +0 and
 * faults at +1, and addrmiss (>= 0x44) raises Load/Store Access Faults
 * (mcause = 5 / 7).
 */
static void test_pwrmgr_permit_and_addrmiss_faults(void) {
  LOG_INFO(
      "Verifying [pwrmgr_reg_pkg.sv:273-291]: PWRMGR_PERMIT[5]=4'b0011 "
      "sub-word write fault on CONTROL and addrmiss");

  uint32_t orig_ctrl = abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);

  // 8-bit store (sb) to CONTROL (PWRMGR_PERMIT[5] = 4'b0011) -> fault!
  expect_store8_fault(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, 0x00u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == orig_ctrl,
        "Blocked 8-bit write to CONTROL must not modify CONTROL");

  // 16-bit store (sh) to CONTROL (reg_be = 4'b0011) -> succeeds!
  *(volatile uint16_t *)(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) =
      (uint16_t)orig_ctrl;
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == orig_ctrl);

  // 8-bit store (sb) to INTR_ENABLE + 0 (PWRMGR_PERMIT[1] = 4'b0001) ->
  // succeeds, while INTR_ENABLE + 1 -> fault!
  abs_mmio_write8(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x01u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET) == 0x01u);
  expect_store8_fault(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET + 1u, 0x00u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x00u);

  // Unmapped offset 0x44 (addrmiss) -> Load/Store Access Fault!
  expect_load_fault(kPwrmgrBase + kPwrmgrUnmappedOffset);
  expect_store32_fault(kPwrmgrBase + kPwrmgrUnmappedOffset, 0xdeadbeefu);
}

/**
 * Verify pwrmgr_reg_top.sv:215-260,317-320,525-549,678-702:
 * 1-bit INTR_* mask (0x1), ALERT_TEST (0x0c) 1-cycle transient pulse,
 * CTRL_CFG_REGWEN (0x10, ro/hwo) ignoring software writes of 0, and
 * WAKEUP_EN_REGWEN (0x1c) / RESET_EN_REGWEN (0x28) rw0c permanent locking.
 */
static void test_pwrmgr_intr_alert_and_regwen_locks(void) {
  LOG_INFO(
      "Verifying [pwrmgr_reg_top.sv:215-702]: 1-bit INTR mask, ALERT_TEST "
      "pulse, CTRL_CFG_REGWEN ro, and rw0c locks");

  // 1. Verify ALERT_TEST 1-cycle transient pulse on repeated writes.
  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdPwrmgrFatalFault));
    abs_mmio_write32(kPwrmgrBase + PWRMGR_ALERT_TEST_REG_OFFSET,
                     1u << PWRMGR_ALERT_TEST_FATAL_FAULT_BIT);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdPwrmgrFatalFault));
  }

  // 2. Verify INTR_ENABLE, INTR_STATE, and INTR_TEST 1-bit mask (0x1).
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET, 0x3eu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0u);

  // 3. Verify CTRL_CFG_REGWEN (0x10) is ro (hwaccess: hwo) and ignores
  // software writes of 0.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) == 1u,
        "CTRL_CFG_REGWEN (ro/hwo) must ignore software write of 0");

  // 4. Verify WAKEUP_EN_REGWEN (0x1c) and RESET_EN_REGWEN (0x28) rw0c
  // permanent lock semantics.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0x15u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0x15u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0x15u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0x2u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x2u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET) == 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET) == 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x2u);
}

bool test_main(void) {
  LOG_INFO("Starting pwrmgr_errata_v2_test on CW340 FPGA (trunk-v2)...");
  test_pwrmgr_wake_status_cdc_masking();
  test_pwrmgr_dif_dt_init_regression_and_return_code();
  test_pwrmgr_aborted_sleep_pinmux_latch_and_wake_info_relatch();
  test_pwrmgr_permit_and_addrmiss_faults();
  test_pwrmgr_intr_alert_and_regwen_locks();
  LOG_INFO("All pwrmgr_errata_v2_test checks PASSED!");
  return true;
}
