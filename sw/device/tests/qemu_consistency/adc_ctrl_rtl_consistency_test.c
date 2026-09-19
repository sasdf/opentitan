// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "adc_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAdcBase = TOP_EARLGREY_ADC_CTRL_AON_BASE_ADDR,
};

static void aon_cdc_wait(void) {
  // 200kHz AON clock period is 5us; 8 AON cycles (~40us) guarantees CDC sync.
  busy_spin_micros(40);
}

bool test_main(void) {
  // 1. Check reset defaults and reserved bitmasks on ADC_PD_CTL and
  // ADC_CHN0/1_FILTER_CTL_0.
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) == 0x64070u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET) ==
        0x4u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET) ==
        0x9bu);

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, 0xffffffffu);
  aon_cdc_wait();
  uint32_t pd_ctl = abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET);
  CHECK(pd_ctl == 0xfffffff1u,
        "ADC_PD_CTL must mask reserved bits [3:1] (got 0x%08x)", pd_ctl);

  // Set fast pwrup_time = 1, wakeup_time = 1, lp_mode = 0.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, 0x00000110u);
  aon_cdc_wait();

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   0xffffffffu);
  aon_cdc_wait();
  uint32_t f0_ctl =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET);
  uint32_t f1_ctl =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET);
  CHECK(f0_ctl == 0x8ffc1ffcu,
        "ADC_CHN0_FILTER_CTL_0 must mask reserved bits to 0x8ffc1ffc (got "
        "0x%08x)",
        f0_ctl);
  CHECK(f1_ctl == 0x8ffc1ffcu,
        "ADC_CHN1_FILTER_CTL_0 must mask reserved bits to 0x8ffc1ffc (got "
        "0x%08x)",
        f1_ctl);

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET, 0u);
  aon_cdc_wait();

  // 2. Verify Status-type INTR_STATE and INTR_TEST behavior, plus ALERT_TEST.
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_TEST_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) == 0x1u);
  // INTR_STATE is SwAccessRO for Status-type interrupts; writing 1 to
  // INTR_STATE while INTR_TEST=1 must not clear INTR_STATE.
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_TEST_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdAdcCtrlAonFatalFault));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ALERT_TEST_REG_OFFSET, 0x1u);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdAdcCtrlAonFatalFault));

  // 3. Verify ADC_FSM_RST gating and release behavior.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0x3ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0x1u);
  aon_cdc_wait();

  // Enable ADC_ENABLE | ONESHOT_MODE while ADC_FSM_RST == 1.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);
  busy_spin_micros(500);
  uint32_t intr_status_during_rst =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK(intr_status_during_rst == 0u,
        "ADC_INTR_STATUS must stay 0 while ADC_FSM_RST=1 (got 0x%08x)",
        intr_status_during_rst);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) == 0u);

  // Release ADC_FSM_RST = 0 while ADC_EN_CTL == 0x3; trigger_l2h must now fire
  // and complete the oneshot conversion.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0x0u);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
       (1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT)) != 0u,
      2000);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) == 0x1u);

  // 4. Verify oneshot edge-detection (trigger_l2h): re-writing ADC_EN_CTL=0x3
  // while ADC_ENABLE is already 1 must NOT re-trigger ONESHOT.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) == 0u);

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);
  busy_spin_micros(500);
  uint32_t retrigger_status =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK(retrigger_status == 0u,
        "Re-writing ADC_EN_CTL=0x3 while already enabled must not re-trigger "
        "oneshot (got 0x%08x)",
        retrigger_status);

  // 5. Verify active normal-power filter evaluation (ONESHOT_MODE=0, LP_MODE=0,
  // NP_SAMPLE_CNT=1) with an all-inclusive filter [0, 0x3ff] on both channels.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x0u);
  aon_cdc_wait();
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   (1u << 31) | (0x3ffu << 18) | (0u << 2));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   (1u << 31) | (0x3ffu << 18) | (0u << 2));
  abs_mmio_write32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  aon_cdc_wait();

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x1u);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET) & 0x1u) !=
          0u,
      2000);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
       0x1u) != 0u,
      2000);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) != 0u,
        "ADC_FSM_STATE must not remain PWRDN (0) while active in NP mode");

  // Clean up ADC_CTRL state.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x0u);
  aon_cdc_wait();
  abs_mmio_write32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);

  return true;
}
