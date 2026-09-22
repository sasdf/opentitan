// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
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
};

static volatile bool access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  access_fault_seen = true;
}

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while (abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) != 0) {
  }
}

bool test_main(void) {
  // 1. Verify ALERT_TEST single-shot pulse for fatal_fault.
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdPwrmgrAonFatalFault));
  abs_mmio_write32(kPwrmgrBase + PWRMGR_ALERT_TEST_REG_OFFSET,
                   1u << PWRMGR_ALERT_TEST_FATAL_FAULT_BIT);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdPwrmgrAonFatalFault));

  // 2. Verify reset defaults of pwrmgr CSRs.
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == 0x180u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x2u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase +
                        PWRMGR_ESCALATE_RESET_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase +
                        PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_FAULT_STATUS_REG_OFFSET) == 0u);

  // 3. Verify INTR_ENABLE, INTR_STATE, and INTR_TEST are 1-bit wide (0x1 mask,
  // NOT 6-bit WAKEUP_MASK 0x3f).
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0xffffffffu);
  uint32_t intr_en =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET);
  CHECK(intr_en == 0x1u,
        "INTR_ENABLE bitmask mismatch: got 0x%08x, expected 0x1", intr_en);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);

  // Writing bits [5:1] (0x3e) to INTR_TEST must NOT set any bits in INTR_STATE.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET, 0x3eu);
  uint32_t intr_state =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0u,
        "INTR_TEST upper bits leaked into INTR_STATE: got 0x%08x, expected 0",
        intr_state);

  // Writing bit 0 (0x1) to INTR_TEST sets INTR_STATE.WAKEUP, and W1C clears it.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x0u);

  // 4. Verify WAKE_STATUS reflects post-enable-mask wakeups (wakeups_i &
  // slow_wakeup_en) synchronized via CFG_CDC_SYNC.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();

  // Trigger AON timer wakeup request while WAKEUP_EN == 0.
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

  // Because WAKEUP_EN is 0, WAKE_STATUS must read 0 even though aon_timer
  // wkup_req is asserted.
  uint32_t wake_status =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == 0u,
        "WAKE_STATUS should be 0 when WAKEUP_EN=0, got 0x%08x", wake_status);

  // Enable AON timer wakeup in WAKEUP_EN and sync via CFG_CDC_SYNC:
  // WAKE_STATUS must now show bit 4 (kAonTimerWkupBit).
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);
  pwrmgr_cdc_sync();
  busy_spin_micros(30);
  wake_status = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  CHECK(wake_status == kAonTimerWkupBit,
        "WAKE_STATUS should be 0x%08x after enabling WAKEUP_EN + CFG_CDC_SYNC, "
        "got 0x%08x",
        kAonTimerWkupBit, wake_status);

  // Disable AON timer wakeup and clean up AON timer.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  busy_spin_micros(30);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) == 0u);

  // 4b. Wave 2 Check: Shallow sleep (MAIN_PD_N=1, LOW_POWER_HINT=1) wakeup via
  // AON timer auto-clears CONTROL.LOW_POWER_HINT, sets INTR_STATE.WAKEUP, and
  // keeps WAKE_INFO recording active (record_en=1) after wakeup until
  // WAKE_INFO_CAPTURE_DIS is written 1.
  {
    const uint32_t kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR;
    const uint32_t kPwrmgrIrq = kTopEarlgreyPlicIrqIdPwrmgrAonWakeup;
    // Enable kTopEarlgreyPlicIrqIdPwrmgrAonWakeup in RV_PLIC with priority 1,
    // keeping mstatus.MIE=0 and mie.MEIE=1 so WFI resumes inline without ISR.
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
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET,
                     kAonTimerWkupBit);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET,
                     (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT) |
                         (1u << PWRMGR_CONTROL_USB_CLK_EN_ACTIVE_BIT) |
                         (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT));
    pwrmgr_cdc_sync();

    // Start AON timer to fire wakeup in ~50us (10 AON ticks) and enter WFI.
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 10u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                     1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
    wait_for_interrupt();

    // Verify LOW_POWER_HINT auto-cleared, INTR_STATE.WAKEUP set, and WAKE_INFO
    // recorded kAonTimerWkupBit.
    uint32_t ctrl_after =
        abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
    CHECK((ctrl_after & (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT)) == 0u,
          "Expected CONTROL.LOW_POWER_HINT cleared after shallow sleep wakeup, "
          "got 0x%08x",
          ctrl_after);
    CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0x1u,
          "Expected INTR_STATE.WAKEUP == 1 after shallow sleep wakeup");
    uint32_t wake_info =
        abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
    CHECK((wake_info & kAonTimerWkupBit) != 0u,
          "Expected WAKE_INFO to capture AON_TIMER wakeup, got 0x%08x",
          wake_info);

    // While WAKE_INFO_CAPTURE_DIS == 0 (record_en == 1) and AON_TIMER wkup_req
    // is still asserted, W1C clearing WAKE_INFO must immediately re-latch
    // kAonTimerWkupBit on the next clock cycle.
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
    wake_info = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
    CHECK((wake_info & kAonTimerWkupBit) != 0u,
          "Expected WAKE_INFO to re-latch active wakeup while record_en=1 "
          "(WAKE_INFO_CAPTURE_DIS=0), got 0x%08x",
          wake_info);

    // Once WAKE_INFO_CAPTURE_DIS is set to 1 (record_en=0), W1C clearing
    // WAKE_INFO must stay 0 even while AON_TIMER wkup_req is still asserted.
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 1u);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffu);
    wake_info = abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET);
    CHECK(wake_info == 0u,
          "Expected WAKE_INFO == 0 after WAKE_INFO_CAPTURE_DIS=1 + W1C, got "
          "0x%08x",
          wake_info);

    // Clean up AON timer, PLIC, and pwrmgr interrupt state.
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
    pwrmgr_cdc_sync();
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                     1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 0x1u);
    abs_mmio_write32(kPlicBase + ie_off, ie_val);
    irq_external_ctrl(false);
  }

  // 5. Verify WAKEUP_EN_REGWEN and RESET_EN_REGWEN are RW0C (Write-0-to-Clear)
  // and cannot be re-enabled by writing 1 once cleared.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0x3fu);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0x3fu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0x15u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0x15u);

  // Clear WAKEUP_EN_REGWEN by writing 0, then try writing 1 (must stay 0).
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REGWEN_REG_OFFSET) ==
        0u);

  // Writes to WAKEUP_EN must now be blocked.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET) == 0x15u);

  // Verify RESET_EN and RESET_EN_REGWEN RW0C behavior.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET) == 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REGWEN_REG_OFFSET) == 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0x0u);

  // 6. Verify PWRMGR_PERMIT sub-word write wr_err (pwrmgr_reg_pkg.sv:259-277):
  // - CONTROL (0x14) has PERMIT = 4'b0011:
  //   8-bit (sb) write fails with StoreAccessFault without modifying CONTROL,
  //   while 16-bit (sh) write at offset +0 succeeds without fault.
  // - INTR_ENABLE (0x04) and WAKEUP_EN (0x20) have PERMIT = 4'b0001:
  //   8-bit write at offset +0 succeeds, while 8-bit write at offset +1 fails.
  uint32_t orig_ctrl = abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
  access_fault_seen = false;
  abs_mmio_write8(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, 0x00u);
  CHECK(access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to CONTROL (PERMIT=4'b0011)");
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) == orig_ctrl,
        "Blocked 8-bit write to CONTROL must not modify CONTROL");

  access_fault_seen = false;
  *(volatile uint16_t *)(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) =
      (uint16_t)orig_ctrl;
  CHECK(!access_fault_seen,
        "Expected no StoreAccessFault on 16-bit write to CONTROL+0");

  access_fault_seen = false;
  abs_mmio_write8(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x01u);
  CHECK(!access_fault_seen,
        "Expected no StoreAccessFault on 8-bit write to INTR_ENABLE+0 "
        "(PERMIT=4'b0001)");
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET) == 0x01u);

  access_fault_seen = false;
  abs_mmio_write8(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET + 1u, 0x00u);
  CHECK(access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to INTR_ENABLE+1 "
        "(PERMIT=4'b0001)");
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0x00u);

  return true;
}
