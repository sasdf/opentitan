// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwrmgr_regs.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kAonTimerWkupBit = 1u << PWRMGR_PARAM_AON_TIMER_AON_WKUP_REQ_IDX,
};

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while (abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) != 0) {
  }
}

bool test_main(void) {
  // 1. Verify ALERT_TEST single-shot pulse for fatal_fault.
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdPwrmgrAonFatalFault));
  abs_mmio_write32(kPwrmgrBase + PWRMGR_ALERT_TEST_REG_OFFSET,
                   1u << PWRMGR_ALERT_TEST_FATAL_FAULT_BIT);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdPwrmgrAonFatalFault));

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

  return true;
}
