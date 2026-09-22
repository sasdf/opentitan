// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAonBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kIbexNmiStateOffset = 0x50,
};

static volatile uint32_t g_wdog_nmi_count = 0;

void ottf_external_nmi_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t intr_state =
      abs_mmio_read32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  CHECK((intr_state & (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT)) != 0u);
  // Clear wdog_timer_bark in AON_TIMER INTR_STATE (W1C) first so bark_irq
  // deasserts before clearing RV_CORE_IBEX NMI_STATE.
  abs_mmio_write32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT);
  abs_mmio_write32(kIbexCfgBase + kIbexNmiStateOffset, 0x2u);
  g_wdog_nmi_count++;
}

bool test_main(void) {
  LOG_INFO("Starting ot_aon_timer FPGA/QEMU consistency test");

  // 1. Write-only INTR_TEST readback & INTR_TEST -> INTR_STATE (without
  // setting WKUP_CAUSE).
  abs_mmio_write32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kAonBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 0x0u);

  // Reading write-only INTR_TEST (0x30) and ALERT_TEST (0x00) must return 0.
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_INTR_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_ALERT_TEST_REG_OFFSET) == 0x0u);

  // Trigger wkup_timer_expired (bit 0) via INTR_TEST and verify W1C clearing.
  abs_mmio_write32(kAonBase + AON_TIMER_INTR_TEST_REG_OFFSET,
                   1u << AON_TIMER_INTR_TEST_WKUP_TIMER_EXPIRED_BIT);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET) == 0x1u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 0x0u);
  CHECK(g_wdog_nmi_count == 0u);

  abs_mmio_write32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET) == 0x0u);

  // 2. Test WKUP_CAUSE clear by writing 0 (W0C/RW0C behavior).
  abs_mmio_write32(kAonBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 0x0u);

  // 3. Trigger wdog_timer_bark (bit 1) via INTR_TEST; ROM enables Ibex WDOG NMI
  // so ottf_external_nmi_handler verifies INTR_STATE bit 1 and clears it via
  // W1C.
  abs_mmio_write32(kAonBase + AON_TIMER_INTR_TEST_REG_OFFSET,
                   1u << AON_TIMER_INTR_TEST_WDOG_TIMER_BARK_BIT);
  CHECK(g_wdog_nmi_count == 1u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_INTR_STATE_REG_OFFSET) == 0x0u);

  // 4. Test WDOG_REGWEN RW0C lock semantics:
  // Initially WDOG_REGWEN is 1, allowing writes to WDOG_CTRL,
  // WDOG_BARK_THOLD, and WDOG_BITE_THOLD.
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_REGWEN_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                   0x87654321u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET) ==
        0x87654321u);

  // Clear WDOG_REGWEN by writing 0 (RW0C).
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_REGWEN_REG_OFFSET) == 0x0u);

  // Verify writing 1 to WDOG_REGWEN cannot re-enable it.
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_REGWEN_REG_OFFSET) == 0x0u);

  // Verify gated writes to WDOG_CTRL, WDOG_BARK_THOLD, WDOG_BITE_THOLD, and
  // WDOG_COUNT are ignored when WDOG_REGWEN == 0.
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0x3u);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                   0xcafebabeu);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0x5555aaaau);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_CTRL_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET) ==
        0x87654321u);
  // WDOG_COUNT is NOT gated by WDOG_REGWEN so software can still pet the
  // watchdog after locking WDOG_REGWEN.
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_COUNT_REG_OFFSET) ==
        0x5555aaaau);
  abs_mmio_write32(kAonBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kAonBase + AON_TIMER_WDOG_COUNT_REG_OFFSET) == 0x0u);

  LOG_INFO("ot_aon_timer FPGA/QEMU consistency test passed");
  return true;
}
