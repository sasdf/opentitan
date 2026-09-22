// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "adc_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAdcBase = TOP_EARLGREY_ADC_CTRL_AON_BASE_ADDR,
};

static void aon_cdc_wait(void) {
  // 200kHz AON clock period is 5us; 8 AON cycles (~40us) guarantees CDC sync.
  busy_spin_micros(40);
}

static volatile uint32_t kFaultCount = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mepc;
  __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
  uint16_t insn16 = *(const uint16_t *)mepc;
  uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
  __asm__ volatile("csrw mepc, %0" : : "r"(mepc + step));
  kFaultCount++;
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

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdAdcCtrlAonFatalFault));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ALERT_TEST_REG_OFFSET, 0x1u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdAdcCtrlAonFatalFault));

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
  IBEX_SPIN_FOR((abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET) &
                 0x1u) != 0u,
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
  // 5. Wave 2 Check A: Oneshot mode with ADC_INTR_CTL == 0 must NOT set
  // ADC_INTR_STATUS.ONESHOT, and FSM must return to PWRDN (0) when done.
  mmio_region_t adc = mmio_region_from_addr(kAdcBase);
  uint32_t fsm_state;
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);
  mmio_region_write32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0u);
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                          (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT));
  busy_spin_micros(600);
  fsm_state = mmio_region_read32(adc, ADC_CTRL_ADC_FSM_STATE_REG_OFFSET);
  CHECK(fsm_state == 0u,
        "Expected FSM state == PWRDN (0) after oneshot completion, got %u",
        fsm_state);
  uint32_t intr_status =
      mmio_region_read32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK(intr_status == 0u,
        "Expected ADC_INTR_STATUS == 0 when ADC_INTR_CTL == 0 after oneshot, "
        "got 0x%x",
        intr_status);

  // 6. Wave 2 Check B: Oneshot mode with ADC_INTR_CTL.ONESHOT_EN == 1 sets
  // ADC_INTR_STATUS.ONESHOT and captures matching ADC_CHN_VALUE &
  // ADC_CHN_VALUE_INTR.
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                      1u << ADC_CTRL_ADC_INTR_CTL_ONESHOT_EN_BIT);
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                          (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT));
  busy_spin_micros(600);
  intr_status = mmio_region_read32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK((intr_status & (1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT)) != 0u,
        "Expected ADC_INTR_STATUS.ONESHOT set when ONESHOT_EN == 1, got 0x%x",
        intr_status);
  uint32_t chn0_reg =
      mmio_region_read32(adc, ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn0_val =
      (chn0_reg >> ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_OFFSET) &
      ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_MASK;
  uint32_t chn0_val_intr =
      (chn0_reg >> ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_OFFSET) &
      ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_MASK;
  CHECK(chn0_val == chn0_val_intr,
        "Expected CHN0 value (0x%x) == CHN0 intr value (0x%x) after oneshot",
        chn0_val, chn0_val_intr);

  // 7. Wave 2 Check C: Pulsing ADC_FSM_RST (1 -> 0) while ADC_ENABLE == 1
  // re-triggers the FSM (trigger_l2h) and runs another oneshot conversion.
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  CHECK(mmio_region_read32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
        "Expected ADC_INTR_STATUS cleared before ADC_FSM_RST re-trigger");
  mmio_region_write32(adc, ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(50);
  mmio_region_write32(adc, ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  busy_spin_micros(600);
  intr_status = mmio_region_read32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK((intr_status & (1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT)) != 0u,
        "Expected releasing ADC_FSM_RST while ADC_ENABLE==1 to re-trigger "
        "oneshot, got ADC_INTR_STATUS=0x%x",
        intr_status);

  // 8. Wave 2 Check D: Low-Power mode (LP_MODE = 1) matching filter sets
  // FILTER_STATUS.TRANS (bit 8) and FILTER_STATUS.MATCH_0 (bit 0) while
  // ADC_INTR_CTL == 0 keeps ADC_INTR_STATUS == 0.
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);
  mmio_region_write32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0u);
  uint32_t match_all_filter =
      (0x3ffu << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                      match_all_filter);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                      match_all_filter);
  mmio_region_write32(adc, ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 2u);
  mmio_region_write32(adc, ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 2u);
  mmio_region_write32(adc, ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                      (1u << ADC_CTRL_ADC_PD_CTL_LP_MODE_BIT) |
                          (1u << ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_OFFSET) |
                          (2u << ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_OFFSET));
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  busy_spin_micros(2000);
  uint32_t filter_status =
      mmio_region_read32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((filter_status & 0x101u) == 0x101u,
        "Expected FILTER_STATUS.TRANS (bit 8) and MATCH_0 (bit 0) set in "
        "LP_MODE, got 0x%x",
        filter_status);
  intr_status = mmio_region_read32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK(intr_status == 0u,
        "Expected ADC_INTR_STATUS == 0 when ADC_INTR_CTL == 0 in LP_MODE, got "
        "0x%x",
        intr_status);

  // 9. Wave 2 Check E: Unsatisfiable filters (COND=1 [0, 0x3ff] and
  // COND=0 [0x300, 0x100]) must NOT set FILTER_STATUS, and mutually exclusive
  // filters ([0, 0x1ff] vs [0x200, 0x3ff]) on CH0 must match only one branch.
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);
  for (int k = 0; k < 8; ++k) {
    mmio_region_write32(
        adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET + (k * 4), 0u);
    mmio_region_write32(
        adc, ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET + (k * 4), 0u);
  }
  mmio_region_write32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  mmio_region_write32(adc, ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);
  mmio_region_write32(adc, ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                      (1u << ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_OFFSET) |
                          (2u << ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_OFFSET));
  // Filter 0: COND=1 (out-of-range) with [0x000, 0x3ff] -> impossible for 10b
  uint32_t impossible_oor =
      (0u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MIN_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_COND_0_BIT) |
      (0x3ffu << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT);
  // Filter 1: COND=0 (in-range) with min_v=0x300 > max_v=0x100 -> impossible
  uint32_t impossible_ir =
      (0x300u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MIN_V_0_OFFSET) |
      (0x100u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                      impossible_oor);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET,
                      impossible_ir);
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  busy_spin_micros(600);
  filter_status = mmio_region_read32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((filter_status & 0x3u) == 0u,
        "Unsatisfiable filters must not match, got FILTER_STATUS=0x%x",
        filter_status);

  // Now configure mutually exclusive CH0 filters: Filter 0 = [0, 0x1ff],
  // Filter 1 = [0x200, 0x3ff]. Exactly one must match.
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);
  mmio_region_write32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  uint32_t low_half =
      (0u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MIN_V_0_OFFSET) |
      (0x1ffu << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT);
  uint32_t high_half =
      (0x200u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MIN_V_0_OFFSET) |
      (0x3ffu << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_OFFSET) |
      (1u << ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET, low_half);
  mmio_region_write32(adc, ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET,
                      high_half);
  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  busy_spin_micros(600);
  filter_status = mmio_region_read32(adc, ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK(((filter_status & 0x3u) == 0x1u) || ((filter_status & 0x3u) == 0x2u),
        "Mutually exclusive CH0 filters [0,0x1ff] and [0x200,0x3ff] cannot "
        "both match simultaneously, got FILTER_STATUS=0x%x",
        filter_status);

  mmio_region_write32(adc, ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  aon_cdc_wait();

  // 10. Verify ADC_CTRL_PERMIT sub-word write checks (adc_ctrl_reg_top.sv
  // wr_err):
  // - Sub-word write to ADC_PD_CTL (ADC_CTRL_PERMIT = 4'b1111) faults and
  // blocks write.
  // - Sub-word write to ADC_LP_SAMPLE_CTL+1 (ADC_CTRL_PERMIT = 4'b0001, reg_be
  // = 4'b0010) faults.
  // - Byte-0 sub-word write to ADC_LP_SAMPLE_CTL (ADC_CTRL_PERMIT = 4'b0001,
  // reg_be = 4'b0001) succeeds.
  uint32_t orig_pd = abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET);
  kFaultCount = 0;
  *((volatile uint8_t *)(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET)) = 0x55u;
  CHECK(
      kFaultCount == 1u &&
          abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) == orig_pd,
      "Sub-word write to ADC_PD_CTL must fault and not modify register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET +
                         1u)) = 0x11u;
  CHECK(kFaultCount == 1u &&
            abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET) ==
                2u,
        "Sub-word write to ADC_LP_SAMPLE_CTL+1 must fault and not modify "
        "register");

  kFaultCount = 0;
  *((volatile uint8_t *)(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET)) =
      0x09u;
  aon_cdc_wait();
  CHECK(kFaultCount == 0u &&
            abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET) ==
                0x09u,
        "Byte-0 sub-word write to ADC_LP_SAMPLE_CTL (ADC_CTRL_PERMIT=4'b0001) "
        "must succeed");

  return true;
}
