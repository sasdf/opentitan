// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_dm_regs.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRvDmRegsBase = TOP_EARLGREY_RV_DM_REGS_BASE_ADDR,
};

bool test_main(void) {
  // 1. Verify ALERT_TEST (0x00) is WO (reads back 0) and generates a transient
  // alert pulse on every write of 1 (testing two consecutive pulses).
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET) == 0u);

  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRvDmFatalFault));
    abs_mmio_write32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET,
                     1u << RV_DM_ALERT_TEST_FATAL_FAULT_BIT);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET) == 0u);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRvDmFatalFault));
    busy_spin_micros(10);
  }

  // 2. Verify LATE_DEBUG_ENABLE_REGWEN (0x04, rw0c) and LATE_DEBUG_ENABLE (0x08).
  uint32_t regwen = abs_mmio_read32(
      kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET);
  CHECK((regwen & ~1u) == 0u);

  if (regwen == 1u) {
    uint32_t orig_val =
        abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
    CHECK(orig_val == kMultiBitBool32False || orig_val == kMultiBitBool32True);

    // Writing 1 to rw0c LATE_DEBUG_ENABLE_REGWEN must keep it 1, and upper bits
    // [31:1] must be masked to 0.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     0xffffffffu);
    CHECK(abs_mmio_read32(kRvDmRegsBase +
                          RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 1u);

    // Toggle LATE_DEBUG_ENABLE while unlocked.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     kMultiBitBool32True);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
          kMultiBitBool32True);

    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     orig_val);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
          orig_val);

    // Lock LATE_DEBUG_ENABLE_REGWEN (rw0c) by writing 0 and verify it cannot be
    // re-enabled by writing 1.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     0u);
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     1u);
    CHECK(abs_mmio_read32(kRvDmRegsBase +
                          RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 0u);
  }

  // Once LATE_DEBUG_ENABLE_REGWEN == 0, writes to LATE_DEBUG_ENABLE must be
  // ignored.
  uint32_t locked_val =
      abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
  abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                   locked_val ^ 0xffffffffu);
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        locked_val);

  return true;
}
