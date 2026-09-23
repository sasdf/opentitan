// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwrmgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kAonTimerWkupBit = 1u << PWRMGR_PARAM_AON_TIMER_AON_WKUP_REQ_IDX,
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

/**
 * [pwrmgr.sv:502,512-515] (BENIGN_RTL_IMPL_DETAIL):
 * Verify that `WAKE_STATUS` (`0x24`) is masked by `slow_wakeup_en`
 * (`pwrmgr.sv:502, 512-515`) and remains `0` even when `aon_timer` `wkup_req`
 * is actively asserted unless `WAKEUP_EN` (`0x20`) is set and synchronized
 * across the slow AON clock domain via `CFG_CDC_SYNC` (`0x18`).
 */
static void test_pwrmgr_wake_status_cdc_masking(void) {
  LOG_INFO(
      "Verifying [pwrmgr.sv:502,512-515] (BENIGN_RTL_IMPL_DETAIL): "
      "WAKE_STATUS masking by slow_wakeup_en via CFG_CDC_SYNC");

  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();

  /* Assert AON timer wakeup request while WAKEUP_EN == 0. */
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
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

  uint32_t wake_status =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == 0u,
        "[pwrmgr.sv:502,512-515] WAKE_STATUS must be 0 when slow_wakeup_en==0, "
        "got "
        "0x%08x",
        wake_status);

  /* Enable AON timer bit in WAKEUP_EN and sync via CFG_CDC_SYNC. */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);
  pwrmgr_cdc_sync();
  busy_spin_micros(30);
  wake_status = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == kAonTimerWkupBit,
        "[pwrmgr.sv:502,512-515] WAKE_STATUS must equal 0x%08x after "
        "CFG_CDC_SYNC, "
        "got 0x%08x",
        kAonTimerWkupBit, wake_status);

  /* Clean up AON timer wakeup request. */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  busy_spin_micros(30);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) == 0u);
}

/**
 * [pwrmgr_wake_info.sv:36-65] (SPEC_DOC_ERRATA) & [pwrmgr.sv:278-368]:
 * Verify that `WAKE_INFO` (`0x3c`) `record_en` (`pwrmgr_wake_info.sv:36-65`)
 * remains armed (`record_en == 1`) after low-power wakeup while
 * `WAKE_INFO_CAPTURE_DIS == 0` (`0x38`), immediately overriding `rw1c` clears
 * (`0xff`) on the next clock cycle while `wakeups_i` is still asserted, and
 * stopping capture only after `WAKE_INFO_CAPTURE_DIS = 1` is written.
 * Also verify `CONTROL[8:4]` retention (`0x180`) and `LOW_POWER_HINT`
 * auto-clear upon low-power exit.
 */
static void test_pwrmgr_wake_info_relatch_and_retention(void) {
  LOG_INFO(
      "Verifying [pwrmgr_wake_info.sv:36-65] (SPEC_DOC_ERRATA) & "
      "[pwrmgr.sv:278-368]: "
      "WAKE_INFO rw1c immediate re-latch while WAKE_INFO_CAPTURE_DIS==0 and "
      "CONTROL retention across LowPowerExit");

  const uint32_t kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR;
  const uint32_t kPwrmgrIrq = kTopEarlgreyPlicIrqIdPwrmgrAonWakeup;

  irq_global_ctrl(false);
  irq_external_ctrl(true);
  abs_mmio_write32(kPlicBase + 0x0u + (kPwrmgrIrq * 4u), 1u);
  uint32_t ie_off = 0x2000u + ((kPwrmgrIrq / 32u) * 4u);
  uint32_t ie_val = abs_mmio_read32(kPlicBase + ie_off);
  abs_mmio_write32(kPlicBase + ie_off, ie_val | (1u << (kPwrmgrIrq % 32u)));
  abs_mmio_write32(kPlicBase + 0x200000u, 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);

  uint32_t retained_ctrl_bits = (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT) |
                                (1u << PWRMGR_CONTROL_USB_CLK_EN_ACTIVE_BIT);
  abs_mmio_write32(
      kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET,
      retained_ctrl_bits | (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT));
  pwrmgr_cdc_sync();

  /* Schedule AON timer wakeup in ~50 us (10 ticks) and execute WFI. */
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 10u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  wait_for_interrupt();

  /* [pwrmgr.sv:278-368]: Verify CONTROL[8:4] retained (0x180), LOW_POWER_HINT
   * cleared (0), and CTRL_CFG_REGWEN unlocked (1). */
  uint32_t ctrl_after =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
  CHECK(ctrl_after == retained_ctrl_bits,
        "[pwrmgr.sv:278-368] Expected CONTROL == 0x%08x after wakeup, got "
        "0x%08x",
        retained_ctrl_bits, ctrl_after);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) == 1u,
        "Expected CTRL_CFG_REGWEN == 1 after wakeup");
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x1u,
        "Expected INTR_STATE.WAKEUP == 1 after wakeup");

  uint32_t wake_info =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK((wake_info & kAonTimerWkupBit) != 0u,
        "[pwrmgr_wake_info.sv:36-65] Expected WAKE_INFO to capture "
        "kAonTimerWkupBit, "
        "got 0x%08x",
        wake_info);

  /* [pwrmgr_wake_info.sv:36-65]: Because `record_en == 1`
   * (`WAKE_INFO_CAPTURE_DIS == 0`) and `aon_timer` wakeup is still asserted
   * (`wakeups_i != 0`), writing `0xff`
   * (`rw1c`) to `WAKE_INFO` clears `info` on the write cycle and immediately
   * re-latches `kAonTimerWkupBit` on the very next clock cycle! */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  wake_info = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK(
      (wake_info & kAonTimerWkupBit) != 0u,
      "[pwrmgr_wake_info.sv:36-65] Expected WAKE_INFO to immediately re-latch "
      "active wakeup after rw1c while WAKE_INFO_CAPTURE_DIS==0, got 0x%08x",
      wake_info);

  /* Once `WAKE_INFO_CAPTURE_DIS = 1` (`0x38`) is written (`record_en <= 0`),
   * `rw1c` clearing `WAKE_INFO` (`0xff`) keeps `WAKE_INFO == 0` even while
   * `aon_timer` `wkup_req` remains actively asserted! */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
  wake_info = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
  CHECK(wake_info == 0u,
        "[pwrmgr_wake_info.sv:36-65] Expected WAKE_INFO == 0 after "
        "WAKE_INFO_CAPTURE_DIS=1 + rw1c, got 0x%08x",
        wake_info);

  /* Clean up AON timer, PLIC, and pwrmgr interrupt state. */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPlicBase + ie_off, ie_val);
  irq_external_ctrl(true);
  irq_global_ctrl(true);
}

/**
 * [pwrmgr_reg_pkg.sv:259-277] (INTENDED_SECURITY_HARDENING):
 * Verify `PWRMGR_PERMIT[17]` (`pwrmgr_reg_pkg.sv:259-277`) sub-word write
 * byte-enable enforcement (`4'b0011` on `CONTROL` `0x14` rejecting 8-bit `sb`
 * with Store Access Fault `mcause = 7` while allowing 16-bit `sh`, `4'b0001` on
 * `INTR_ENABLE` `0x04` allowing 8-bit `sb` at `+0` and faulting at `+1`), and
 * `addrmiss` synchronous bus faults (`mcause = 5 / 7`) at offset `0x44`.
 */
static void test_pwrmgr_permit_and_addrmiss_faults(void) {
  LOG_INFO(
      "Verifying [pwrmgr_reg_pkg.sv:259-277] (INTENDED_SECURITY_HARDENING): "
      "PWRMGR_PERMIT[5]=4'b0011 sub-word write fault on CONTROL and addrmiss");

  uint32_t orig_ctrl = abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);

  /* 8-bit store (`sb`) to `CONTROL` (`PWRMGR_PERMIT[5] = 4'b0011`) -> fault! */
  expect_store8_fault(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, 0x00u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == orig_ctrl,
        "Blocked 8-bit write to CONTROL must not modify CONTROL");

  /* 16-bit store (`sh`) to `CONTROL` (`reg_be = 4'b0011`) -> succeeds! */
  *(volatile uint16_t *)(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) =
      (uint16_t)orig_ctrl;
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == orig_ctrl);

  /* 8-bit store (`sb`) to `INTR_ENABLE + 0` (`PWRMGR_PERMIT[1] = 4'b0001`) ->
   * succeeds, while `INTR_ENABLE + 1` -> fault! */
  abs_mmio_write8(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x01u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET) == 0x01u);
  expect_store8_fault(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET + 1u, 0x00u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x00u);

  /* Unmapped offset `0x44` (`addrmiss`) -> Load/Store Access Fault! */
  expect_load_fault(kPwrmgrBase + kPwrmgrUnmappedOffset);
  expect_store32_fault(kPwrmgrBase + kPwrmgrUnmappedOffset, 0xdeadbeefu);
}

/**
 * [pwrmgr_reg_top.sv:432-652] (BENIGN_RTL_IMPL_DETAIL):
 * Verify 1-bit `INTR_*` mask (`0x1` rather than `0x3f`), `ALERT_TEST` (`0x0c`)
 * 1-cycle transient pulse (`q & qe`), `CTRL_CFG_REGWEN` (`0x10`, `ro`/`hwo`)
 * ignoring software writes of `0`, and `WAKEUP_EN_REGWEN` (`0x1c`) /
 * `RESET_EN_REGWEN` (`0x28`) `rw0c` permanent locking.
 */
static void test_pwrmgr_intr_alert_and_regwen_locks(void) {
  LOG_INFO(
      "Verifying [pwrmgr_reg_top.sv:432-652] (BENIGN_RTL_IMPL_DETAIL): "
      "1-bit INTR mask, ALERT_TEST pulse, CTRL_CFG_REGWEN ro, and rw0c locks");

  /* 1. Verify `ALERT_TEST` 1-cycle transient pulse on repeated writes. */
  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdPwrmgrAonFatalFault));
    abs_mmio_write32(kPwrmgrBase + PWRMGR_ALERT_TEST_REG_OFFSET,
                     1u << PWRMGR_ALERT_TEST_FATAL_FAULT_BIT);
    CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
        kTopEarlgreyAlertIdPwrmgrAonFatalFault));
  }

  /* 2. Verify `INTR_ENABLE`, `INTR_STATE`, and `INTR_TEST` 1-bit mask (`0x1`).
   */
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

  /* 3. Verify `CTRL_CFG_REGWEN` (`0x10`) is `ro` (`hwaccess: hwo`) and ignores
   * software writes of `0`. */
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) == 1u,
        "CTRL_CFG_REGWEN (ro/hwo) must ignore software write of 0");

  /* 4. Verify `WAKEUP_EN_REGWEN` (`0x1c`) and `RESET_EN_REGWEN` (`0x28`) `rw0c`
   * permanent lock semantics. */
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
  LOG_INFO("Starting pwrmgr_errata_test (P29) on CW340 FPGA / QEMU...");
  test_pwrmgr_wake_status_cdc_masking();
  test_pwrmgr_wake_info_relatch_and_retention();
  test_pwrmgr_permit_and_addrmiss_faults();
  test_pwrmgr_intr_alert_and_regwen_locks();
  LOG_INFO("All pwrmgr_errata_test checks PASSED!");
  return true;
}
