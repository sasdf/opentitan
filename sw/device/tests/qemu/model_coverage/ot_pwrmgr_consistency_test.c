// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwrmgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
};

static volatile bool store_fault_seen = false;
static volatile uint32_t last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  OT_DISCARD(exc_info);
  uint32_t mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  last_mcause = mcause;
  if (mcause == kIbexExcStoreAccessFault) {
    store_fault_seen = true;
    uint16_t insn16 = *(const uint16_t *)mepc;
    uint32_t insn_len = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    ibex_mepc_write(mepc + insn_len);
    return;
  }
  LOG_ERROR("Unexpected exception mcause=0x%x mepc=0x%x", mcause, mepc);
  CHECK(false);
}

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while ((abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) &
          (1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT)) != 0u) {
  }
}

bool test_main(void) {
  mmio_region_t pwrmgr_region = mmio_region_from_addr(kPwrmgrBase);

  // 1. Write-Only register readbacks (INTR_TEST, ALERT_TEST) and INTR_TEST ->
  // INTR_STATE -> RW1C clear parity.
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_ALERT_TEST_REG_OFFSET) == 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_TEST_REG_OFFSET) == 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) == 0u);

  // 2. Read-Only register write ignored semantics (CTRL_CFG_REGWEN,
  // WAKE_STATUS, RESET_STATUS, ESCALATE_RESET_STATUS, FAULT_STATUS).
  uint32_t ctrl_cfg_regwen_init =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET);
  uint32_t wake_status_init =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET);
  uint32_t reset_status_init =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_STATUS_REG_OFFSET);
  uint32_t esc_reset_status_init =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_ESCALATE_RESET_STATUS_REG_OFFSET);
  uint32_t fault_status_init =
      abs_mmio_read32(kPwrmgrBase + PWRMGR_FAULT_STATUS_REG_OFFSET);

  CHECK(ctrl_cfg_regwen_init == 1u);
  CHECK(esc_reset_status_init == 0u);
  CHECK(fault_status_init == 0u);

  abs_mmio_write32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_STATUS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_ESCALATE_RESET_STATUS_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_FAULT_STATUS_REG_OFFSET, 0xffffffffu);

  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET) ==
        ctrl_cfg_regwen_init);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_STATUS_REG_OFFSET) ==
        wake_status_init);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_STATUS_REG_OFFSET) ==
        reset_status_init);
  CHECK(
      abs_mmio_read32(kPwrmgrBase + PWRMGR_ESCALATE_RESET_STATUS_REG_OFFSET) ==
      esc_reset_status_init);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_FAULT_STATUS_REG_OFFSET) ==
        fault_status_init);

  // 3. Sub-word byte reads and sub-word write permit enforcement on CONTROL.
  uint32_t ctrl_orig = abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
  // Program CONTROL with 0x190 (MAIN_PD_N = 1, CORE_CLK_EN = 1, IO_CLK_EN = 1,
  // LOW_POWER_HINT = 0).
  const uint32_t ctrl_test_val = 0x00000190u;
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, ctrl_test_val);
  pwrmgr_cdc_sync();
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) ==
        ctrl_test_val);

  CHECK(mmio_region_read8(pwrmgr_region, PWRMGR_CONTROL_REG_OFFSET + 0u) ==
        0x90u);
  CHECK(mmio_region_read8(pwrmgr_region, PWRMGR_CONTROL_REG_OFFSET + 1u) ==
        0x01u);
  CHECK(mmio_region_read8(pwrmgr_region, PWRMGR_CONTROL_REG_OFFSET + 2u) ==
        0x00u);
  CHECK(mmio_region_read8(pwrmgr_region,
                          PWRMGR_CTRL_CFG_REGWEN_REG_OFFSET + 0u) == 0x01u);

  // CONTROL has PWRMGR_PERMIT = 4'b0011 (0x3), so an 8-bit write (be = 0x1)
  // triggers a transactional TL-UL bus error (mcause == 7) without modifying
  // CONTROL or raising FAULT_STATUS.
  store_fault_seen = false;
  last_mcause = 0;
  mmio_region_write8(pwrmgr_region, PWRMGR_CONTROL_REG_OFFSET + 0u, 0x00u);
  CHECK(store_fault_seen, "Expected Store Access Fault on 8-bit CONTROL write");
  CHECK(last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET) ==
        ctrl_test_val);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_FAULT_STATUS_REG_OFFSET) == 0u);

  // Restore original CONTROL value.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, ctrl_orig);
  pwrmgr_cdc_sync();

  // 4. Peripheral HW reset masking when PWRMGR.RESET_EN == 0.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET, 0u);
  pwrmgr_cdc_sync();
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_EN_REG_OFFSET) == 0u);

  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 1000u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, 1u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);

  busy_spin_micros(50);

  uint32_t wdog_count =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET);
  CHECK(wdog_count >= 2u, "Expected WDOG_COUNT >= 2 after 50us, got %u",
        wdog_count);
  CHECK(abs_mmio_read32(kPwrmgrBase + PWRMGR_RESET_STATUS_REG_OFFSET) ==
        reset_status_init);

  // Clean up aon_timer watchdog.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT) |
                       (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT));

  return true;
}
