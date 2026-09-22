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
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "adc_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwrmgr_regs.h"
#include "rv_plic_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAdcBase = TOP_EARLGREY_ADC_CTRL_AON_BASE_ADDR,
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  // In chip_earlgrey_cw340.sv, AST inputs adc_a0_ai and adc_a1_ai are tied to
  // '0, so adc_ana.sv outputs adc_d_ch0_o = 10'h000 and adc_d_ch1_o = 10'h000.
  kExpectedChn0Val = 0x000u,
  kExpectedChn1Val = 0x000u,
};

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while ((abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) &
          (1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT)) != 0u) {
  }
}

static uint32_t make_filter_ctl(uint32_t min_v, uint32_t max_v, bool out_range,
                                bool en) {
  return ((min_v & 0x3ffu) << 2u) | ((out_range ? 1u : 0u) << 12u) |
         ((max_v & 0x3ffu) << 18u) | ((en ? 1u : 0u) << 31u);
}

bool test_main(void) {
  // 1. Reset ADC FSM and wait 5+ AON clock cycles (50 us) for CDC sync.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(50);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  busy_spin_micros(50);
  abs_mmio_write32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET, 0x1u);

  // 2. Verify Write-Only registers (INTR_TEST, ALERT_TEST) read back as 0.
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ALERT_TEST_REG_OFFSET) == 0u);

  // 3. Configure Filter 0 (in-range [0x000, 0x100] -> matches 0x000) and
  //    Filter 2 (in-range [0x100, 0x200] -> must NOT match 0x000), and enable
  //    Low-Power -> Normal-Power transition (LP_MODE=1) with ADC_INTR_CTL.TRANS
  //    (bit 8), MATCH_0 (bit 0), and MATCH_2 (bit 2).
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, /*out_range=*/false,
                                   /*en=*/true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, /*out_range=*/false,
                                   /*en=*/true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_2_REG_OFFSET,
                   make_filter_ctl(0x100u, 0x200u, /*out_range=*/false,
                                   /*en=*/true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_2_REG_OFFSET,
                   make_filter_ctl(0x100u, 0x200u, /*out_range=*/false,
                                   /*en=*/true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   (1u << 0u) | (1u << 2u) | (1u << 8u));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_WAKEUP_CTL_REG_OFFSET,
                   (1u << 0u) | (1u << 2u) | (1u << 8u));
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_ENABLE_REG_OFFSET, 0x1u);

  // Fast LP/NP sample counts so conversion completes within a few AON cycles.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);
  // LP_MODE=1, PWRUP_TIME=1, WAKEUP_TIME=2.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                   0x1u | (1u << 4u) | (2u << 8u));
  busy_spin_micros(50);

  // Enable continuous ADC conversion (ADC_ENABLE=1, ONESHOT_MODE=0).
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x1u);

  // Poll until FILTER_STATUS indicates both Filter 0 match and LP->NP TRANS.
  uint32_t filter_status = 0u;
  for (int i = 0; i < 5000; ++i) {
    filter_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
    if ((filter_status & 0x101u) == 0x101u) {
      break;
    }
    busy_spin_micros(10);
  }
  // Wait 6 AON cycles (30 us) for adc_ctrl_intr.sv's 2-stage AON staging/hold
  // pipeline (aon_staging_reqs_q -> aon_req_hold_q -> u_match_sync) to update
  // ADC_INTR_STATUS in the core clock domain.
  busy_spin_micros(30);
  filter_status = abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  // Filter 0 (bit 0) and TRANS (bit 8) must be set; Filter 2 (bit 2) must be 0.
  CHECK((filter_status & 0x105u) == 0x101u);

  uint32_t intr_status =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK((intr_status & 0x105u) == 0x101u);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET) & 0x1u) ==
        0x1u);

  // Verify captured channel values match AST (0x000, 0x000) in both VALUE and
  // INTR_VALUE fields.
  uint32_t chn0_val =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn1_val =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(((chn0_val >> 2u) & 0x3ffu) == kExpectedChn0Val);
  CHECK(((chn0_val >> 18u) & 0x3ffu) == kExpectedChn0Val);
  CHECK(((chn1_val >> 2u) & 0x3ffu) == kExpectedChn1Val);
  CHECK(((chn1_val >> 18u) & 0x3ffu) == kExpectedChn1Val);

  // 4. Verify writes to Read-Only registers (ADC_CHN_VAL_0, ADC_CHN_VAL_1,
  //    ADC_FSM_STATE) are ignored and preserve the captured ADC values.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET) ==
        chn0_val);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET) ==
        chn1_val);

  // 5. While continuous ADC conversion is active (ADC_ENABLE=1,
  // ONESHOT_MODE=0),
  //    dynamically update ADC_PD_CTL and enable Filter 1 (out-of-range COND=1
  //    [0x100, 0x200] matching 0x000) and verify Filter 1 also matches while
  //    Filter 2 ([0x100, 0x200] COND=0) remains 0.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 8u));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                   0x0u | (1u << 4u) | (2u << 8u));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET,
                   make_filter_ctl(0x100u, 0x200u, /*out_range=*/true,
                                   /*en=*/true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_1_REG_OFFSET,
                   make_filter_ctl(0x100u, 0x200u, /*out_range=*/true,
                                   /*en=*/true));

  for (int i = 0; i < 5000; ++i) {
    filter_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
    if ((filter_status & 0x3u) == 0x3u) {
      break;
    }
    busy_spin_micros(10);
  }
  busy_spin_micros(30);
  filter_status = abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((filter_status & 0x7u) == 0x3u);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
         0x7u) == 0x3u);

  // 6. Normal-sleep (MAIN_PD_N = 1) wakeup via ADC_CTRL Filter 0 matching
  //    (0x000, 0x000).
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(50);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  busy_spin_micros(50);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_1_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_2_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_2_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_WAKEUP_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  // LP_SAMPLE_CTL = 4, WAKEUP_TIME = 100 (~1.7 ms total LP window) so the CPU
  // enters WFI and pwrmgr transitions to low power before LP sampling finishes.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 4u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                   0x1u | (7u << 4u) | (100u << 8u));
  busy_spin_micros(50);

  // Enable pwrmgr_aon_wakeup in RV_PLIC (with global MIE=0 and MEIE=1 so WFI
  // resumes inline without jumping to OTTF ISR).
  const uint32_t kPwrmgrWakeupIrq = kTopEarlgreyPlicIrqIdPwrmgrAonWakeup;
  abs_mmio_write32(kPlicBase + RV_PLIC_PRIO0_REG_OFFSET + 4u * kPwrmgrWakeupIrq,
                   1u);
  uint32_t ie_reg =
      kPlicBase + RV_PLIC_IE0_0_REG_OFFSET + 4u * (kPwrmgrWakeupIrq / 32u);
  abs_mmio_write32(ie_reg,
                   abs_mmio_read32(ie_reg) | (1u << (kPwrmgrWakeupIrq % 32u)));
  abs_mmio_write32(kPlicBase + RV_PLIC_THRESHOLD0_REG_OFFSET, 0u);
  irq_global_ctrl(false);
  irq_external_ctrl(true);

  // Arm pwrmgr for normal sleep (MAIN_PD_N=1, CORE_CLK_EN=1, IO_CLK_EN=1,
  // USB_CLK_EN_ACTIVE=1, LOW_POWER_HINT=1) with ADC_CTRL wakeup (bit 1).
  uint32_t ctrl_orig = abs_mmio_read32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_CAPTURE_DIS_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, (1u << 1u));
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, 0x000001b1u);
  pwrmgr_cdc_sync();

  // Start ADC conversion and immediately enter WFI to sleep until ADC wakeup.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x1u);
  wait_for_interrupt();

  irq_external_ctrl(false);
  CHECK((abs_mmio_read32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET) & 1u) ==
        1u);
  CHECK((abs_mmio_read32(kPwrmgrBase + PWRMGR_WAKE_INFO_REG_OFFSET) &
         (1u << 1u)) != 0u);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET) &
         0x1u) == 0x1u);

  // Restore pwrmgr and disable ADC.
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET, ctrl_orig);
  pwrmgr_cdc_sync();

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET, 0u);
  return true;
}
